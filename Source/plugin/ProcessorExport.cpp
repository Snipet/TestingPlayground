// Source/plugin/ProcessorExport.cpp: the Processor's export off the message thread (EditorPort::startExport,
// exporting(), cancelExport()): one project's export, or several as a bundle (ProjectExport.h), each project's file
// read (projectstore::load) and written on a thread of its own, never the message thread.
#include "plugin/Processor.h"

#include "plugin/ProjectExport.h"

#include <juce_core/juce_core.h>

namespace playground
{
    namespace
    {
        std::string methodOf(bool package, projectexport::Method m)
        {
            return !package ? "zip" : m == projectexport::Method::xz ? "xz" : m == projectexport::Method::zlib ? "zlib" : "stored";
        }

        std::string gigabytes(int64_t bytes)
        {
            return juce::String(static_cast<double>(bytes) / static_cast<double>(int64_t{ 1 } << 30), 2).toStdString() + " GB";
        }
    }

    bool Processor::startExport(const std::vector<std::string>& projects, bool package, bool allVersions, const std::string& path,
                                std::string& why)
    {
        {
            const std::lock_guard<std::mutex> lock(exportMutex_);
            if (exportState_.busy)
            {
                why = "an export is under way";
                return false;
            }
        }
        if (projects.empty())
        {
            why = "there is no project to export";
            return false;
        }
        if (static_cast<int>(projects.size()) > projectexport::kMaxBundleProjects)
        {
            why = std::to_string(projects.size()) + " projects are more than one export takes ("
                + std::to_string(projectexport::kMaxBundleProjects) + ")";
            return false;
        }
        const juce::File to(juce::String::fromUTF8(path.c_str()));
        if (path.empty() || !juce::File::isAbsolutePath(juce::String::fromUTF8(path.c_str())))
        {
            why = "the file needs a full path";
            return false;
        }
        if (to.isDirectory())
        {
            why = path + " is a folder";
            return false;
        }
        if (to.exists())
        {
            why = path + " is there already: an export never replaces a file (choose another name)";
            return false;
        }
        if (exporter_.joinable())
            exporter_.join();                                    // the last one, ended (busy is false)
        ExportJob taken{ projects, package, allVersions, path };
        {
            const std::lock_guard<std::mutex> lock(exportMutex_);
            const uint64_t ended = exportState_.ended;
            exportState_ = {};
            exportState_.busy = true;
            exportState_.ended = ended;
            exportState_.total = static_cast<int>(projects.size());
            exportState_.path = path;
        }
        exportCancel_.store(false);
        exporter_ = std::thread([this, job = std::move(taken)] { runExport(job); });
        return true;
    }

    ExportState Processor::exporting() const
    {
        const std::lock_guard<std::mutex> lock(exportMutex_);
        return exportState_;
    }

    void Processor::cancelExport()
    {
        exportCancel_.store(true);
        {
            const std::lock_guard<std::mutex> lock(exportMutex_);   // the gate's waiter sees it (setExportGate)
        }
        exportGateOpen_.notify_all();
    }

    void Processor::setExportGate(bool shut)
    {
        {
            const std::lock_guard<std::mutex> lock(exportMutex_);
            exportGateShut_ = shut;
        }
        exportGateOpen_.notify_all();
    }

    void Processor::endExport(bool ok, const Exported& out, const std::string& why, bool cancelled)
    {
        const std::lock_guard<std::mutex> lock(exportMutex_);
        exportState_.ok = ok;
        exportState_.out = ok ? out : Exported{};
        exportState_.why = ok ? std::string() : why;
        exportState_.cancelled = cancelled;
        exportState_.busy = false;
        exportState_.finished = true;
        ++exportState_.ended;
    }

    // The exporter's thread: nothing here touches the processor but exportState_ (under exportMutex_) and the
    // store's own functions.
    void Processor::runExport(const ExportJob& job)
    {
        {
            std::unique_lock<std::mutex> lock(exportMutex_);
            exportGateOpen_.wait(lock, [this] { return !exportGateShut_ || exportCancel_.load(); });
        }
        const auto cancelled = [this] { return exportCancel_.load(); };
        if (cancelled())
        {
            endExport(false, {}, "cancelled", true);
            return;
        }
        // The limit on what is read, from a look at each file (no read), before anything is written.
        int64_t total = 0;
        for (const std::string& name : job.names)
            total += projectstore::fileBytes(name).value_or(0);
        if (total > projectexport::kMaxBundleBytes)
        {
            endExport(false, {}, "the projects' files are " + gigabytes(total) + " in all, more than an export takes ("
                                     + std::to_string(projectexport::kMaxBundleBytes >> 30) + " GB)", false);
            return;
        }
        const juce::File to(juce::String::fromUTF8(job.path.c_str()));
        Exported out;
        std::string why;
        if (job.names.size() == 1)
        {
            // One project: its own export, the bytes exportProject writes.
            std::optional<projectstore::Project> p = projectstore::load(job.names[0], &why);
            if (p && cancelled())
            {
                endExport(false, {}, "cancelled", true);
                return;
            }
            projectexport::Result r;
            bool ok = false;
            if (p)
            {
                p->name = ownName(p->name);                      // a factory project's: its own name
                ok = job.package ? projectexport::writePackage(*p, job.allVersions, to, r, &why, false)
                                 : projectexport::writeSource(*p, job.allVersions, to, r, &why, false);
            }
            {
                const std::lock_guard<std::mutex> lock(exportMutex_);
                exportState_.done = 1;
                if (!p)
                {
                    exportState_.failed = 1;
                    exportState_.failedNames.push_back(ownName(job.names[0]) + ": " + why);
                }
            }
            if (ok)
                out = { r.bytes, r.raw, r.versions, methodOf(job.package, r.method) };
            endExport(ok, out, why, false);
            return;
        }
        projectexport::BundleProgress progress;
        progress.cancelled = cancelled;
        progress.step = [this](const std::string& name, bool ok, const std::string& whyNot) {
            const std::lock_guard<std::mutex> lock(exportMutex_);
            ++exportState_.done;
            if (!ok)
            {
                ++exportState_.failed;
                exportState_.failedNames.push_back(ownName(name) + ": " + whyNot);
            }
        };
        projectexport::BundleResult r;
        const bool ok = projectexport::writeBundle(
            job.names, [](const std::string& name, std::string* whyNot) { return projectstore::load(name, whyNot); }, job.package,
            job.allVersions, to, false, progress, r, &why);
        if (ok)
            out = { r.file.bytes, r.file.raw, r.file.versions, "bundle" };
        endExport(ok, out, why, r.cancelled);
    }

    // poll()'s: an export that ended has its thread joined.
    void Processor::pollExport()
    {
        if (!exporter_.joinable())
            return;
        {
            const std::lock_guard<std::mutex> lock(exportMutex_);
            if (exportState_.busy)
                return;
        }
        exporter_.join();
    }

    void Processor::finishExport()
    {
        if (!exporter_.joinable())
            return;
        cancelExport();
        exporter_.join();
    }
}
