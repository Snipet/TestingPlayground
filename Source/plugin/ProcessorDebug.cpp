// Source/plugin/ProcessorDebug.cpp: the processor's side of the settings card (EditorPort's "Settings"): the Debug
// page's sections (debugRows), gathered on the message thread from getters that exist and only while a Debug page is
// open (debugViewers), and a folder shown in the file manager (showPath). The one file that includes BuildCommit.h
// (cmake/BuildCommit.cmake): this build's commit, kept out of Product.h, which nearly every file includes.
//
// What a row may say: names, numbers, paths, states. Never a key, a chat's word, a file's text or the user's audio:
// the program's files are named, never shown; the environment's switches are the product's own (<prefix>...), none a
// secret.
#include "plugin/Processor.h"

#include "fminus/Patch.h"
#include "fminus/Shaders.h"
#include "plugin/ChatStore.h"
#include "plugin/FactoryStore.h"
#include "plugin/Launch.h"
#include "plugin/ProjectStore.h"

#include "BuildCommit.h"
#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
  #include <stdlib.h>                                    // _environ
#else
extern char** environ;
#endif

namespace playground
{
    namespace
    {
        std::string number(double v, int decimals)
        {
            char b[48];
            std::snprintf(b, sizeof b, "%.*f", decimals, v);
            return b;
        }

        std::string count(uint64_t n)
        {
            return std::to_string(static_cast<unsigned long long>(n));
        }

        std::string onOff(bool on) { return on ? "on" : "off"; }

        // A path as the projects folder shows it: the home folder as "~".
        std::string shown(const juce::File& f)
        {
            const juce::String path = f.getFullPathName();
            const juce::String home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
            return (path.startsWith(home + juce::File::getSeparatorString()) ? "~" + path.substring(home.length()) : path)
                .toStdString();
        }

        std::string shown(const std::filesystem::path& p)
        {
            const std::u8string u = p.u8string();
            return shown(juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(u.c_str()))));
        }

        const char* statusWords(factorystore::Status s)
        {
            switch (s)
            {
                case factorystore::Status::ready:    return "ready";
                case factorystore::Status::fetching: return "fetching";
                case factorystore::Status::off:      return "off";
                case factorystore::Status::notAsked: return "not asked";
                case factorystore::Status::declined: return "declined";
                case factorystore::Status::failed:   return "failed";
                case factorystore::Status::none:     return "none";
            }
            return "";
        }

        std::string when(int64_t ms)
        {
            if (ms <= 0)
                return "never";
            const std::time_t t = static_cast<std::time_t>(ms / 1000);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char b[40];
            std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
            return b;
        }

        // The product's own switches in the environment (<prefix>...), names and values, sorted.
        std::vector<DebugRow> environmentRows()
        {
            std::vector<DebugRow> rows;
            const std::string prefix = product::kEnvPrefix;
#if defined(_WIN32)
            char** env = _environ;
#else
            char** env = environ;
#endif
            for (char** e = env; e != nullptr && *e != nullptr; ++e)
            {
                const std::string kv = *e;
                if (kv.rfind(prefix, 0) != 0)
                    continue;
                const std::size_t eq = kv.find('=');
                rows.push_back({ kv.substr(0, eq), eq == std::string::npos ? std::string() : kv.substr(eq + 1) });
            }
            std::sort(rows.begin(), rows.end(), [](const DebugRow& a, const DebugRow& b) { return a.name < b.name; });
            if (rows.empty())
                rows.push_back({ "Switches", "none set (" + prefix + "...)" });
            return rows;
        }
    }

    std::vector<DebugSection> Processor::debugRows() const
    {
        if (debugViewers_ <= 0)
            return {};                                   // nothing gathered while no Debug page is open
        ++debugGathers_;
        std::vector<DebugSection> out;

        // ---- Versions --------------------------------------------------------------------------------------------------
        {
            const Patch::BuildInfo fm = Patch::buildInfo();
            DebugSection s{ "Versions", {} };
            s.rows.push_back({ "Product", std::string(product::kName) + " (" + product::kKind + ") " + product::kVersion });
            s.rows.push_back({ "Build", std::string("commit ") + FP_BUILD_COMMIT + ", " + product::kBuildType });
            s.rows.push_back({ "FMinus", std::string(fm.version) + ", commit " + std::string(product::kFMinusCommit).substr(0, 12)
                                             + " (" + std::string(fm.buildType) + ")" });
            s.rows.push_back({ "LLVM", std::string(fm.llvmVersion) + ", " + std::string(fm.llvmMode) + ", assertions "
                                           + onOff(fm.llvmAssertions) });
            s.rows.push_back({ "FunkGui", product::kFunkGuiVersion });
            s.rows.push_back({ "JUCE", product::kJuceVersion });
            s.rows.push_back({ "Claude Desktop extension", product::kMcpVersion });
            out.push_back(std::move(s));
        }

        // ---- Program (FMinus) ------------------------------------------------------------------------------------------
        {
            const CompileStatus st = status();
            DebugSection s{ "Program (FMinus)", {} };
            s.rows.push_back({ "Program", st.program == 0 ? std::string("none yet") : "#" + count(st.program) });
            std::string files;
            for (const SourceFile& f : st.sources)
                files += (files.empty() ? "" : ", ") + f.name;
            s.rows.push_back({ "Files", files.empty() ? std::string("none") : std::to_string(st.sources.size()) + ": " + files });
            s.rows.push_back({ "Compile", !st.ok ? (st.stopped ? std::string("stopped as it played") : std::string("failed"))
                                                 : "compiled in " + number(st.ms, 0) + " ms"
                                                       + (st.panelOnly ? " (a panel's edit)" : st.refined ? " (refined)" : "") });
            int dials = 0;
            for (const Control& c : st.controls)
                dials += c.used ? 1 : 0;
            s.rows.push_back({ "Dials", std::to_string(dials) });
            s.rows.push_back({ "Loop budget and bounds checks", "on (every compile that plays; the frozen copy without bounds checks)" });
            s.rows.push_back({ "Frozen copy", frozen() ? "playing" : "none playing" });
            s.rows.push_back({ "Circuits", std::to_string(st.circuitKeys.size()) + " netlists with a configuration to choose, "
                                               + std::to_string(st.circuitDials.size()) + " circuit fields" });
            s.rows.push_back({ "IR", irViewers_ > 0 ? "kept (an IR view is open)" : "not kept (no IR view open)" });
            out.push_back(std::move(s));
        }

        // ---- Audio -----------------------------------------------------------------------------------------------------
        {
            DebugSection s{ "Audio", {} };
            s.rows.push_back({ "Sample rate", getSampleRate() > 0.0 ? number(getSampleRate(), 0) + " Hz" : std::string("not prepared") });
            s.rows.push_back({ "Largest host block", std::to_string(largestHostBlock_.load(std::memory_order_relaxed)) + " samples" });
            s.rows.push_back({ "Load", number(100.0 * static_cast<double>(load()), 1) + " %" });
            Meters m;
            if (meters(m))
            {
                if constexpr (product::kIsInstrument)
                    s.rows.push_back({ "Voices", std::to_string(m.voices) + " sounding, " + std::to_string(m.fading) + " fading, "
                                                     + std::to_string(m.steals) + " stolen, " + std::to_string(m.dropped)
                                                     + " dropped (non-finite)" });
                s.rows.push_back({ "Samples processed", count(m.samples) });
            }
            s.rows.push_back({ "Bypass", onOff(bypassed_.load(std::memory_order_relaxed)) });
            out.push_back(std::move(s));
        }

        // ---- Shaders ---------------------------------------------------------------------------------------------------
        {
            DebugSection s{ "Shaders", {} };
            std::string shaderc = bundledShaderc(wrapperType == wrapperType_Standalone);
            if (shaderc.empty())
                if (const char* env = std::getenv((std::string(product::kEnvPrefix) + "SHADERC").c_str()); env != nullptr)
                    shaderc = env;
            s.rows.push_back({ "shaderc", shaderc.empty() ? std::string("none") : shaderc });
            s.rows.push_back({ "Runs this process", count(shadercRuns()) });
            const std::filesystem::path cache = shaderCacheFolder();
            if (cache.empty())
                s.rows.push_back({ "Cache", "none" });
            else
            {
                s.rows.push_back({ "Cache folder", shown(cache), true });
                std::error_code ec;
                std::size_t entries = 0;
                for (std::filesystem::directory_iterator it(cache, ec), end; !ec && it != end; it.increment(ec))
                    ++entries;
                s.rows.push_back({ "Cache entries", std::to_string(entries) });
            }
            s.rows.push_back({ "Cache scans", count(shaderCacheScans()) });
            out.push_back(std::move(s));
        }

        // ---- Stores ----------------------------------------------------------------------------------------------------
        {
            DebugSection s{ "Stores", {} };
            s.rows.push_back({ "Projects folder", projectFolder(), true });
            s.rows.push_back({ "Projects", std::to_string(projectsPolled().size()) + " listed, "
                                               + std::to_string(projectstore::parses()) + " read whole this process ("
                                               + std::to_string(projectstore::readerParses()) + " by the reader), "
                                               + std::to_string(projectstore::listings()) + " listings" });
            s.rows.push_back({ "Chats folder", shown(chatstore::directory()), true });
            if (factoryStore_ != nullptr)
            {
                const factorystore::State f = factoryStore_->state();
                s.rows.push_back({ "Factory projects", std::string(statusWords(f.status)) + ", " + std::to_string(f.projects)
                                                           + " in the set, last checked " + when(f.checkedMs)
                                                           + (f.reason.empty() ? "" : ": " + f.reason) });
            }
            s.rows.push_back({ "Factory folder", shown(factorystore::directory()), true });
            out.push_back(std::move(s));
        }

        // ---- Claude Desktop --------------------------------------------------------------------------------------------
        {
            DebugSection s{ "Claude Desktop", {} };
            const DesktopState d = desktop();
            if (mcp_ == nullptr || !mcp_->listening())
                s.rows.push_back({ "Link", "not listening (no socket for this instance)" });
            else
            {
                s.rows.push_back({ "Link", d.connected ? "listening, Claude Desktop connected" : "listening" });
                s.rows.push_back({ "Socket", mcp_->socketPath() });
                s.rows.push_back({ "Name", d.name + (d.number.empty() || d.number == d.name ? "" : " (" + d.number + ")") });
                s.rows.push_back({ "Target", d.target ? std::string("this instance")
                                                      : d.targetName.empty() ? std::string("none chosen") : d.targetName });
                s.rows.push_back({ "Others open", std::to_string(d.others.size()) });
            }
            out.push_back(std::move(s));
        }

        out.push_back({ "Environment", environmentRows() });
        return out;
    }

    bool Processor::showPath(const std::string& path, std::string& why)
    {
        std::string full = path;
        if (full.rfind("~", 0) == 0)
            full = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName().toStdString() + full.substr(1);
        if (folderLauncher_)
            return folderLauncher_(FolderApp::files, full, why);
        // Never from a probe (CTest's sandbox): it launches nothing on the machine that runs it.
        if (const char* sandbox = std::getenv((std::string(product::kEnvPrefix) + "PREFS_DIR").c_str());
            sandbox != nullptr && *sandbox != '\0')
        {
            why = "nothing opens under a probe";
            return false;
        }
        const juce::File f(juce::String::fromUTF8(full.c_str()));
        if (!f.isDirectory())
        {
            why = "the folder is not there yet";
            return false;
        }
#if defined(__linux__)
        // xdg-open given the path as its one argument (Launch.h): never a shell between the plugin and a path.
        std::string launchWhy;
        if (launch::detached(launch::openerArgs(full), launchWhy))
            return true;
        why = "the folder did not open: " + launchWhy;
        return false;
#else
        if (f.startAsProcess())
            return true;
        why = "the folder did not open";
        return false;
#endif
    }
}
