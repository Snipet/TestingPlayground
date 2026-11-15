// Source/plugin/GitRecord.cpp: see GitRecord.h.
#include "plugin/GitRecord.h"

#include "plugin/EditorPort.h"
#include "plugin/FactoryStore.h"
#include "plugin/FileReplace.h"
#include "plugin/ProjectStore.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace playground::gitrecord
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr std::uintmax_t kMaxFile = 64 * 1024;

        // ---- files: as UpdateCheck.cpp's (private there too) -----------------------------------------------------

        std::optional<std::string> readFile(const fs::path& p)
        {
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(p, ec);
            if (ec || size > kMaxFile)
                return std::nullopt;
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return std::nullopt;
            std::string out(static_cast<std::size_t>(size), '\0');
            in.read(out.data(), static_cast<std::streamsize>(out.size()));
            if (static_cast<std::size_t>(in.gcount()) != out.size())
                return std::nullopt;
            return out;
        }

        bool writeFile(const fs::path& p, const std::string& bytes)
        {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            const std::string tag = juce::Uuid().toString().substring(0, 12).toStdString();
            const fs::path temp = p.parent_path() / files::pathOf(files::utf8Of(p.filename()) + "." + tag + ".tmp");
            bool written = false;
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.close();
                written = static_cast<bool>(out);
            }
            const bool moved = written && files::replace(temp, p, ec);   // its answer, never ec
            if (!moved)
            {
                std::error_code e;
                fs::remove(temp, e);
                return false;
            }
            return true;
        }

        // The folder's lock (.lock: UpdateCheck's, FactoryStore's and the ledger's too), tried for a few seconds at
        // most.
        class FolderLock
        {
        public:
            explicit FolderLock(const fs::path& dir)
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                const fs::path p = dir / ".lock";
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < until)
                {
#if defined(_WIN32)
                    int fd = -1;
                    if (_wsopen_s(&fd, p.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0
                        && fd >= 0)
                    {
                        fd_ = fd;
                        return;
                    }
#else
                    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
                    if (fd >= 0)
                    {
                        if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
                        {
                            fd_ = fd;
                            return;
                        }
                        ::close(fd);
                    }
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
            ~FolderLock()
            {
                if (fd_ < 0)
                    return;
#if defined(_WIN32)
                _close(fd_);
#else
                ::flock(fd_, LOCK_UN);
                ::close(fd_);
#endif
            }
            FolderLock(const FolderLock&) = delete;
            FolderLock& operator=(const FolderLock&) = delete;
            bool held() const noexcept { return fd_ >= 0; }

        private:
            int fd_ = -1;
        };

        // ---- the record ------------------------------------------------------------------------------------------

        // A file's look: its size, time and identity (Linux before 6.13 moves a file's time by the kernel's tick, so a
        // rename of a new file in is told by its identity too).
        struct Look
        {
            bool there = false;
            std::uintmax_t size = 0;
            int64_t time = 0;
            uint64_t id = 0;
            bool operator==(const Look&) const = default;
        };

        Look lookAt(const fs::path& p)
        {
            Look l;
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(p, ec);
            if (ec)
                return l;
            l.there = true;
            l.size = size;
            const auto t = fs::last_write_time(p, ec);
            l.time = ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
#if !defined(_WIN32)
            struct stat st {};
            if (::stat(p.c_str(), &st) == 0)
                l.id = static_cast<uint64_t>(st.st_ino);
#endif
            return l;
        }

        std::mutex recordMutex;                  // the cache below; one writer in this process
        std::string cachedFile;
        Look cachedLook;
        juce::var cachedRoot;                    // the file's JSON as last read or written (unknown fields kept)
        std::atomic<uint64_t> revisionCount{ 1 };

        fs::path fileNow()
        {
            return files::pathOf(factorystore::directory().getChildFile(kFile).getFullPathName().toStdString());
        }

        juce::var parse(const std::optional<std::string>& text)
        {
            juce::var v;
            if (!text || text->size() > 0x7fffffff
                || !juce::CharPointer_UTF8::isValidString(text->data(), static_cast<int>(text->size()))
                || !juce::JSON::parse(juce::String::fromUTF8(text->data(), static_cast<int>(text->size())), v).wasOk()
                || !v.isObject() || v["format"].toString() != kFormat)
                return {};
            return v;
        }

        std::string str(const juce::var& v) { return v.isString() ? v.toString().toStdString() : std::string(); }

        // The file's JSON as it is now, read again only when its look moved (under recordMutex).
        const juce::var& rootLocked()
        {
            const fs::path p = fileNow();
            const std::string name = files::utf8Of(p);
            const Look l = lookAt(p);
            if (name == cachedFile && l == cachedLook)
                return cachedRoot;
            cachedFile = name;
            cachedLook = l;
            cachedRoot = l.there ? parse(readFile(p)) : juce::var();
            revisionCount.fetch_add(1, std::memory_order_acq_rel);
            return cachedRoot;
        }

        Record recordOf(const juce::var& root, bool& hasChosen)
        {
            Record r;
            hasChosen = root.isObject() && root.hasProperty("chosen");
            if (!root.isObject())
                return r;
            r.chosen = str(root["chosen"]);
            const juce::var f = root["found"];
            if (f.isObject())
            {
                r.path = str(f["path"]);
                r.version = str(f["version"]);
                r.foundMs = static_cast<juce::int64>(f["ms"]);
            }
            const juce::var n = root["none"];
            if (n.isObject() && r.path.empty())
                r.why = str(n["why"]);
            return r;
        }

        // `change` applied to the record as the file has it now, under the folder's lock (the file read again first;
        // every field it does not set kept), with the old field taken over when the record has no choice yet; then the
        // old field dropped from this product's links file. False, with why, when it cannot be written.
        template <typename Change>
        bool write(Change change, std::string* why)
        {
            const fs::path p = fileNow();
            std::string legacy = projectstore::legacyGitPath();
            bool dropLegacy = !legacy.empty();
            {
                const std::lock_guard<std::mutex> lock(recordMutex);
                const FolderLock folderLock(p.parent_path());
                if (!folderLock.held())
                {
                    if (why != nullptr)
                        *why = "the folder " + files::utf8Of(p.parent_path()) + " is busy: try again";
                    return false;
                }
                juce::var root = parse(readFile(p));
                if (!root.isObject())
                {
                    root = juce::var(new juce::DynamicObject());
                    root.getDynamicObject()->setProperty("format", kFormat);
                    root.getDynamicObject()->setProperty("version", 1);
                }
                juce::DynamicObject* o = root.getDynamicObject();
                if (!o->hasProperty("chosen") && !legacy.empty())
                    o->setProperty("chosen", juce::String::fromUTF8(legacy.c_str()));   // taken over, once
                if (!change(*o))
                    return true;                 // nothing to change
                if (!writeFile(p, juce::JSON::toString(root).toStdString() + "\n"))
                {
                    if (why != nullptr)
                        *why = "cannot write " + files::utf8Of(p);
                    return false;
                }
                cachedFile = files::utf8Of(p);
                cachedLook = lookAt(p);
                cachedRoot = root;
                revisionCount.fetch_add(1, std::memory_order_acq_rel);
            }
            if (dropLegacy)
                projectstore::dropLegacyGitPath();
            return true;
        }
    }

    Record read()
    {
        Record r;
        bool hasChosen = false;
        {
            const std::lock_guard<std::mutex> lock(recordMutex);
            r = recordOf(rootLocked(), hasChosen);
        }
        // The old field: the choice while the record has none of its own; dropped unread once the record has one.
        const std::string legacy = projectstore::legacyGitPath();
        if (!legacy.empty())
        {
            if (hasChosen)
                projectstore::dropLegacyGitPath();
            else
                r.chosen = legacy;
        }
        return r;
    }

    uint64_t revision()
    {
        const std::lock_guard<std::mutex> lock(recordMutex);
        (void)rootLocked();
        return revisionCount.load(std::memory_order_acquire);
    }

    bool setChosen(const std::string& path, std::string* why)
    {
        return write(
            [&](juce::DynamicObject& o) {
                if (o.hasProperty("chosen") && str(o.getProperty("chosen")) == path)
                    return false;
                o.setProperty("chosen", juce::String::fromUTF8(path.c_str()));
                o.removeProperty("found");       // what was found under the choice before: the next look says
                o.removeProperty("none");
                return true;
            },
            why);
    }

    void noteFound(git::Found& found, int64_t nowMs)
    {
        const Record before = read();
        if (!found.found() && !before.path.empty() && before.chosen.empty())
            found.why = "The git found on " + std::string(kThisMachine) + " before, at " + before.path
                      + ", is no longer there: " + found.why;
        (void)write(
            [&](juce::DynamicObject& o) {
                if (found.found())
                {
                    const juce::var f = o.getProperty("found");
                    if (f.isObject() && str(f["path"]) == found.path && str(f["version"]) == found.version
                        && !o.hasProperty("none"))
                        return false;            // as remembered: nothing written
                    auto* n = new juce::DynamicObject();
                    n->setProperty("path", juce::String::fromUTF8(found.path.c_str()));
                    n->setProperty("version", juce::String::fromUTF8(found.version.c_str()));
                    n->setProperty("ms", static_cast<juce::int64>(nowMs));
                    o.setProperty("found", juce::var(n));
                    o.removeProperty("none");
                    return true;
                }
                const juce::var was = o.getProperty("none");
                if (was.isObject() && str(was["why"]) == found.why && !o.hasProperty("found"))
                    return false;
                auto* n = new juce::DynamicObject();
                n->setProperty("why", juce::String::fromUTF8(found.why.c_str()));
                n->setProperty("ms", static_cast<juce::int64>(nowMs));
                o.setProperty("none", juce::var(n));
                o.removeProperty("found");
                return true;
            },
            nullptr);
    }
}
