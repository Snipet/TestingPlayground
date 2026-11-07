// Source/plugin/FolderSync.cpp: see FolderSync.h.
#include "plugin/FolderSync.h"

#include "plugin/FolderWatch.h"

#include "dsp/Sources.h"
#include "fminus/LibraryIncludes.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace playground
{
    namespace fs = std::filesystem;

    namespace
    {
        // Paths from UTF-8 and back, on every platform (a folder's name may be the project's).
        fs::path pathOf(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
        std::string utf8Of(const fs::path& p)
        {
            const std::u8string s = p.u8string();
            return std::string(s.begin(), s.end());
        }

        std::string lower(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // The kinds the editor knows (dsp/Sources.h's isProgramFile, any case): a new file of one in the folder is the
        // program's.
        bool knownExtension(const std::string& name) { return isProgramFile(lower(name)); }

        // A text as the editor holds it (TextEditor::sanitise), only when it is not so already.
        bool plain(std::string_view text) noexcept
        {
            for (const char c : text)
            {
                const auto u = static_cast<unsigned char>(c);
                if (!((u >= 32 && u <= 126) || u == '\t' || u == '\n'))
                    return false;
            }
            return true;
        }
        std::string editorText(std::string_view text)
        {
            return plain(text) ? std::string(text) : funkgui::TextEditor::sanitise(text);
        }
        uint64_t editorHash(std::string_view text)
        {
            return plain(text) ? FolderSync::hashOf(text) : FolderSync::hashOf(funkgui::TextEditor::sanitise(text));
        }

        std::string hex(uint64_t h)
        {
            char buf[17];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
            return buf;
        }

        bool readAll(const fs::path& p, std::string& out, std::size_t limit, bool& large)
        {
            large = false;
            std::error_code ec;
            const auto size = fs::file_size(p, ec);
            if (ec)
                return false;
            if (size > limit)
            {
                large = true;
                return false;
            }
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return !in.bad();
        }

        void writable(const fs::path& p)
        {
            std::error_code ec;
            fs::permissions(p, fs::perms::owner_write, fs::perm_options::add, ec);
        }

        std::string joined(const std::vector<std::string>& names)
        {
            std::string s;
            for (const std::string& n : names)
                s += (s.empty() ? "" : ", ") + n;
            return s;
        }
    }

    uint64_t FolderSync::hashOf(std::string_view text) noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (const char c : text)
        {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    std::string FolderSync::tidyFolderName(const std::string& name)
    {
        std::string s;
        for (const char c : name)
        {
            const auto u = static_cast<unsigned char>(c);
            const bool bad = u < 32 || u == 127 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"'
                          || c == '<' || c == '>' || c == '|';
            s += bad ? '-' : c;
        }
        const auto trim = [](char c) { return c == ' ' || c == '.' || c == '-'; };
        while (!s.empty() && trim(s.front()))
            s.erase(s.begin());
        while (!s.empty() && trim(s.back()))
            s.pop_back();
        constexpr std::size_t kMax = 60;
        if (s.size() > kMax)
        {
            std::size_t cut = kMax;
            while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)   // never inside a UTF-8 sequence
                --cut;
            s.resize(cut);
            while (!s.empty() && trim(s.back()))
                s.pop_back();
        }
        return s;
    }

    FolderSync::FolderSync(Plugin plugin) : plugin_(std::move(plugin)) {}

    FolderSync::~FolderSync() { stop(); }

    bool FolderSync::watching() const noexcept { return watch_ != nullptr && watch_->watching(); }

    // ---- the folder and its lock ------------------------------------------------------------------------------------

    bool FolderSync::lock(const fs::path& dir)
    {
        const fs::path p = dir / kLock;
#if defined(_WIN32)
        int fd = -1;
        if (_wsopen_s(&fd, p.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) != 0 || fd < 0)
            return false;                                // another instance has it open
#else
        const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
            return false;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0)         // another instance (this process's too) holds it
        {
            ::close(fd);
            return false;
        }
#endif
        lockFd_ = fd;
        return true;
    }

    void FolderSync::unlock()
    {
        if (lockFd_ < 0)
            return;
#if defined(_WIN32)
        _close(lockFd_);
#else
        ::flock(lockFd_, LOCK_UN);
        ::close(lockFd_);
#endif
        lockFd_ = -1;
    }

    bool FolderSync::start(const fs::path& root, const std::string& name, double now, std::string& why)
    {
        stop(false);
        const std::string tidy = tidyFolderName(name);
        if (tidy.empty())
        {
            why = "no name for a folder";
            error_ = why;
            bump();
            return false;
        }
        std::error_code ec;
        fs::create_directories(root, ec);
        for (int n = 1; n <= 99; ++n)
        {
            const std::string candidate = n == 1 ? tidy : tidy + " " + std::to_string(n);
            const fs::path dir = root / pathOf(candidate);
            fs::create_directories(dir, ec);
            if (!fs::is_directory(dir, ec))
            {
                why = "could not make the folder \"" + candidate + "\"";
                error_ = why;
                bump();
                return false;
            }
            if (!lock(dir))
                continue;                                // another instance syncs with it
            name_ = candidate;
            folder_ = dir;
            on_ = true;
            base_.clear();
            copies_.clear();
            seen_.clear();
            listing_.clear();
            conflicts_.clear();
            conditions_.clear();
            events_.clear();
            lookedOnce_ = false;
            listedAt_ = -1.0e9;
            nextLook_ = now;
            draftKey_ = 0;
            draftAt_ = -1.0e9;                           // what the draft holds now is written at once
            pendingSince_ = -1.0;
            loadManifest();
            writeExtras();
            watch_ = std::make_unique<FolderWatch>();
            if (!watch_->start(utf8Of(dir)))
                watch_.reset();
            error_.clear();
            bump();
            pass(now, true);
            return true;
        }
        why = "every folder named \"" + tidy + "\" is in use by another instance";
        error_ = why;
        bump();
        return false;
    }

    void FolderSync::stop(bool flush)
    {
        if (!on_)
            return;
        // The plugin's changes the folder has not had: written now, whatever the draft's rest (the folder's own
        // changes not taken by now stay in the folder, which is what it holds).
        if (flush && plugin_.draft)
        {
            const Draft d = plugin_.draft();
            listing_ = list(d);
            listedAt_ = -1.0e9;
            decide(0.0, d, true, true);
        }
        saveManifest();
        if (watch_)
            watch_->stop();
        watch_.reset();
        unlock();
        on_ = false;
        conflicts_.clear();
        conditions_.clear();
        events_.clear();
        bump();
    }

    void FolderSync::setRunOnSave(bool on)
    {
        if (runOnSave_ != on)
            bump();
        runOnSave_ = on;
    }

    // ---- the manifest, the note, VS Code's settings ------------------------------------------------------------------

    void FolderSync::loadManifest()
    {
        std::ifstream in(folder_ / kManifest, std::ios::binary);
        std::string line;
        if (!in || !std::getline(in, line) || line.rfind("fmsync 1", 0) != 0)
            return;
        while (std::getline(in, line))
        {
            // "base <16 hex> <name>" or "copy <16 hex> <name>"
            if (line.size() < 23 || line[4] != ' ' || line[21] != ' ')
                continue;
            const std::string kind = line.substr(0, 4), name = line.substr(22);
            uint64_t h = 0;
            if (std::sscanf(line.c_str() + 5, "%16llx", reinterpret_cast<unsigned long long*>(&h)) != 1 || name.empty())
                continue;
            if (kind == "base")
                base_[name] = h;
            else if (kind == "copy")
                copies_[name] = h;
        }
    }

    void FolderSync::saveManifest()
    {
        if (!manifestDirty_ || folder_.empty())
            return;
        std::string text = "fmsync 1\n";
        for (const auto& [name, h] : base_)
            text += "base " + hex(h) + " " + name + "\n";
        for (const auto& [name, h] : copies_)
            text += "copy " + hex(h) + " " + name + "\n";
        const fs::path temp = folder_ / ".fmsync.tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << text;
            if (!out)
                return;
        }
        std::error_code ec;
        fs::rename(temp, folder_ / kManifest, ec);
        if (!ec)
            manifestDirty_ = false;
    }

    void FolderSync::writeExtras()
    {
        std::error_code ec;
        const fs::path note = folder_ / kNote;
        if (!fs::exists(note, ec))
        {
            std::ofstream out(note, std::ios::binary);
            out << "# " << product::kName << ": " << name_ << "\n\n"
                << "This folder holds the program's files of one " << product::kName << " instance, synced both ways.\n\n"
                << "- A file saved here changes it in the plugin (one undo step there), and an edit in the plugin is written here.\n"
                << "- A new file here (.fm, .gui, .cir, .sc, .sh) becomes one of the program's files; a file renamed or deleted here is\n"
                << "  renamed or deleted there. The program, the first file, deleted here is written back: delete it in the plugin.\n"
                << "- When a file changed on both sides, neither is written: the plugin asks which to keep, and the other is kept\n"
                << "  in `.conflicts/`.\n"
                << "- `host.fm` is the plugin's own file, here read only so that `#include \"host.fm\"` resolves; so is a library\n"
                << "  file an `#include` names that the program does not have (LIBRARY in the plugin adds it, to compile and to edit).\n"
                << "- The plugin's editor holds printable ASCII and tabs: other characters become `?`.\n"
                << "- FMinus's VS Code extension (`editors/vscode` in FMinus's repository) highlights `.fm` files.\n\n"
                << "Files whose names start with a dot are the sync's own.\n";
        }
        const fs::path vscode = folder_ / ".vscode";
        const fs::path settings = vscode / "settings.json";
        if (!fs::exists(settings, ec))
        {
            fs::create_directories(vscode, ec);
            std::ofstream out(settings, std::ios::binary);
            out << "{\n"
                   "    \"files.readonlyFromPermissions\": true,\n"
                   "    \"files.exclude\": { \".fmsync*\": true }\n"
                   "}\n";
        }
    }

    // ---- the folder's files ------------------------------------------------------------------------------------------

    bool FolderSync::ours(const std::string& name, const Draft& d) const
    {
        if (name.empty() || name.front() == '.')
            return false;                                // the sync's own, an editor's swap file, .vscode
        return findFile(d.files, name) != nullptr || base_.count(name) > 0 || copies_.count(name) > 0
            || knownExtension(name);
    }

    std::map<std::string, FolderSync::Stat> FolderSync::list(const Draft& d) const
    {
        std::map<std::string, Stat> out;
        std::error_code ec;
        for (fs::directory_iterator it(folder_, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec))
        {
            std::error_code e2;
            if (!it->is_regular_file(e2))
                continue;
            const std::string name = utf8Of(it->path().filename());
            if (!ours(name, d))
                continue;
            Stat s;
            s.size = static_cast<int64_t>(it->file_size(e2));
            s.time = static_cast<int64_t>(it->last_write_time(e2).time_since_epoch().count());
            out[name] = s;
        }
        return out;
    }

    bool FolderSync::look(double now, const Draft& d)
    {
        std::error_code ec;
        if (!fs::is_directory(folder_, ec))
        {
            // The folder went (deleted, moved): made again, with every file the plugin's.
            fs::create_directories(folder_, ec);
            unlock();
            (void)lock(folder_);
            base_.clear();
            copies_.clear();
            seen_.clear();
            manifestDirty_ = true;
            writeExtras();
            note("the folder was gone: made again", now);
        }
        std::map<std::string, Stat> l = list(d);
        if (!lookedOnce_ || l != listing_)
        {
            listing_ = std::move(l);
            listedAt_ = now;
            lookedOnce_ = true;
            nextLook_ = now + kSettleSeconds;
            return false;
        }
        if (now - listedAt_ < kSettleSeconds)
        {
            nextLook_ = listedAt_ + kSettleSeconds;
            return false;
        }
        nextLook_ = now + (watching() ? kWatchedSeconds : kPollSeconds);
        return true;
    }

    bool FolderSync::readText(const std::string& name, std::string& text)
    {
        bool large = false;
        std::string raw;
        if (!readAll(folder_ / pathOf(name), raw, kMaxBytes, large))
            return false;
        text = editorText(raw);
        return true;
    }

    const FolderSync::Seen* FolderSync::seenOf(const std::string& name)
    {
        const auto l = listing_.find(name);
        if (l == listing_.end())
            return nullptr;
        if (const auto s = seen_.find(name); s != seen_.end() && s->second.stat == l->second)
            return &s->second;
        Seen s;
        s.stat = l->second;
        std::string raw;
        if (!readAll(folder_ / pathOf(name), raw, kMaxBytes, s.large))
        {
            if (!s.large)
                return nullptr;
        }
        else
            s.hash = editorHash(raw);
        return &(seen_[name] = s);
    }

    bool FolderSync::writeFile(const std::string& name, const std::string& text, bool readOnly)
    {
        const fs::path target = folder_ / pathOf(name);
        const fs::path temp = folder_ / pathOf(".fmsync-" + name + ".tmp");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!out)
            {
                std::error_code ec;
                fs::remove(temp, ec);
                return false;
            }
        }
        std::error_code ec;
        fs::permissions(temp,
                        readOnly ? fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read
                                 : fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
                        ec);
        if (fs::exists(target, ec))
            writable(target);                            // Windows replaces no read-only file
        fs::rename(temp, target, ec);                    // whole or not at all
        if (ec)
        {
            fs::remove(temp, ec);
            return false;
        }
        Stat s;
        s.size = static_cast<int64_t>(fs::file_size(target, ec));
        s.time = static_cast<int64_t>(fs::last_write_time(target, ec).time_since_epoch().count());
        listing_[name] = s;                              // its own write: nothing to read back
        seen_[name] = Seen{ s, editorHash(text), false };
        return true;
    }

    bool FolderSync::removeFile(const std::string& name)
    {
        const fs::path target = folder_ / pathOf(name);
        std::error_code ec;
        writable(target);
        fs::remove(target, ec);
        listing_.erase(name);
        seen_.erase(name);
        return !ec;
    }

    // The side not kept, in .conflicts/: "program.folder-20261106-140211.fm", a name not taken yet.
    fs::path FolderSync::asideFor(const std::string& name, const char* side)
    {
        std::error_code ec;
        const fs::path dir = folder_ / kConflicts;
        fs::create_directories(dir, ec);
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        const std::size_t dot = name.rfind('.');
        const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
        const std::string ext = dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
        std::string file = stem + "." + side + "-" + stamp + ext;
        for (int n = 2; fs::exists(dir / pathOf(file), ec) && n < 100; ++n)
            file = stem + "." + side + "-" + stamp + "-" + std::to_string(n) + ext;
        return dir / pathOf(file);
    }

    bool FolderSync::keepAside(const std::string& name, const std::string& text, const char* side)
    {
        std::ofstream out(asideFor(name, side), std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(out);
    }

    // The folder's file as it is, whatever its size: copied whole.
    bool FolderSync::keepFolderFileAside(const std::string& name)
    {
        std::error_code ec;
        return fs::copy_file(folder_ / pathOf(name), asideFor(name, "folder"), ec) && !ec;
    }

    void FolderSync::note(const std::string& text, double now)
    {
        events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const Event& e) { return e.text == text; }),
                      events_.end());
        events_.insert(events_.begin(), Event{ text, now });
        if (events_.size() > 4)
            events_.resize(4);
        bump();
    }

    // ---- a pass ------------------------------------------------------------------------------------------------------

    namespace
    {
        uint64_t draftKeyOf(const Draft& d)
        {
            uint64_t h = 1469598103934665603ull;
            const auto mix = [&h](std::string_view s) {
                for (const char c : s)
                {
                    h ^= static_cast<unsigned char>(c);
                    h *= 1099511628211ull;
                }
                h ^= 0xFFu;
                h *= 1099511628211ull;
            };
            for (const SourceFile& f : d.files)
            {
                mix(f.name);
                mix(f.text);
            }
            return h;
        }
    }

    void FolderSync::pass(double now, bool force)
    {
        if (!on_)
            return;
        const std::size_t events = events_.size();
        events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const Event& e) { return now - e.at > kEventSeconds; }),
                      events_.end());
        if (events_.size() != events)
            bump();
        const Draft d = plugin_.draft();
        const uint64_t key = draftKeyOf(d);
        if (key != draftKey_)
        {
            draftKey_ = key;
            draftAt_ = now;
        }
        // kMaxWaitSeconds runs from the draft's first move away from what was last decided, not from the next look
        // (kWatchedSeconds off while watched): decide() lets it go once nothing waits.
        if (key != decidedKey_ && pendingSince_ < 0.0)
            pendingSince_ = now;
        const bool told = watch_ != nullptr && watch_->takeChanged();
        const bool rested = now - draftAt_ >= kRestSeconds
                         || (pendingSince_ >= 0.0 && now - pendingSince_ >= kMaxWaitSeconds);
        // A write of the plugin's that is due looks at the folder first: a save not yet taken is never written over.
        const bool due = (key != decidedKey_ || pendingSince_ >= 0.0) && rested;
        if (!(force || told || due || now >= nextLook_))
            return;
        if (!look(now, d))
            return;                                      // not still yet: looked at again in kSettleSeconds
        if (decide(now, d, rested, false))
            decidedKey_ = key;
        saveManifest();
    }

    bool FolderSync::decide(double now, const Draft& d, bool rested, bool writeOnly)
    {
        // What the folder holds for the #includes, read only: host.fm (unless the program has its own) and each
        // library file an #include names that the program does not have, with what those include in turn.
        std::map<std::string, std::string> provided;
        if (findFile(d.files, "host.fm") == nullptr)
            provided["host.fm"] = std::string(Patch::hostSource());
        {
            const Sources all = withLibraryIncludes(d.files);
            for (std::size_t i = d.files.size(); i < all.size(); ++i)
                provided[all[i].name] = all[i].text;
        }

        std::vector<std::string> conditions;
        // A draft file whose name differs from an earlier one's by case alone: the Mac's disks tell them apart no more
        // than its project names (ProjectStore.h), so only the first is the folder's.
        std::set<std::string> skipped;
        {
            std::set<std::string> lowered;
            for (const SourceFile& f : d.files)
                if (!lowered.insert(lower(f.name)).second)
                {
                    skipped.insert(f.name);
                    conditions.push_back(f.name + ": same name but for case");
                }
        }
        std::set<std::string> names;
        for (const SourceFile& f : d.files)
            names.insert(f.name);
        for (const auto& [n, s] : listing_)
            names.insert(n);
        for (const auto& [n, h] : base_)
            names.insert(n);

        struct Adopt
        {
            std::string name;
            bool present = false;                        // in the folder (else deleted there)
            std::string text;
            uint64_t hash = 0;
        };
        std::vector<Adopt> adopt;
        std::vector<std::string> writes, removes;
        bool waiting = false;
        const auto same = [](bool ah, uint64_t a, bool bh, uint64_t b) { return ah == bh && (!ah || a == b); };

        for (const std::string& n : names)
        {
            const SourceFile* df = findFile(d.files, n);
            if (df == nullptr && (provided.count(n) > 0 || copies_.count(n) > 0))
            {
                // A read-only copy's (syncCopies), no longer a file of the draft. A provided one keeps its base until
                // syncCopies has looked: the folder's file as both last agreed on is the plugin's to write over.
                if (provided.count(n) == 0 && base_.erase(n) > 0)
                    manifestDirty_ = true;
                continue;
            }
            if (skipped.count(n) > 0)
                continue;                                // a name the disk cannot hold beside another
            const bool dh = df != nullptr;
            const uint64_t dx = dh ? editorHash(df->text) : 0;
            const bool fh = listing_.count(n) > 0;
            const Seen* s = fh ? seenOf(n) : nullptr;
            if (fh && s == nullptr)
                continue;                                // unreadable just now: next time
            if (s != nullptr && s->large)
            {
                conditions.push_back(n + ": over 1 MB, not taken");
                continue;
            }
            const uint64_t fx = s != nullptr ? s->hash : 0;
            const auto b = base_.find(n);
            bool bh = b != base_.end();
            uint64_t bx = bh ? b->second : 0;
            // A read-only copy the folder holds as written, and a file of the draft's by its name now (a project with
            // its own host.fm or library file opened, LIBRARY's file typed in before a look): the copy is what both
            // last agreed on, so the draft's text is the plugin's change, written over it (writable), never a
            // conflict. The base is kept: syncCopies forgets the copy this very pass.
            if (dh && !bh && fh)
                if (const auto c = copies_.find(n); c != copies_.end() && c->second == fx)
                {
                    base_[n] = fx;
                    manifestDirty_ = true;
                    bh = true;
                    bx = fx;
                }
            if (same(dh, dx, fh, fx))
            {
                if (dh && (!bh || bx != dx))
                {
                    base_[n] = dx;
                    manifestDirty_ = true;
                }
                else if (!dh && bh)
                {
                    base_.erase(n);
                    manifestDirty_ = true;
                }
                if (conflicts_.erase(n) > 0)
                    bump();
                if (dh && copies_.erase(n) > 0)          // a read-only copy the program now has (LIBRARY): its own
                {
                    writable(folder_ / pathOf(n));
                    manifestDirty_ = true;
                }
                continue;
            }
            if (conflicts_.count(n) > 0)
                continue;                                // held until the user chooses
            if (same(fh, fx, bh, bx))
            {
                if (!rested)
                    waiting = true;                      // the plugin's change, once the draft rests
                else
                    (dh ? writes : removes).push_back(n);
            }
            else if (same(dh, dx, bh, bx))
            {
                if (writeOnly)
                    continue;
                Adopt a;
                a.name = n;
                a.present = fh;
                if (fh)
                {
                    if (!dh && !validFileName(n))
                    {
                        conditions.push_back(n + ": not a valid name");
                        continue;
                    }
                    if (!readText(n, a.text))
                        continue;
                    a.hash = hashOf(a.text);
                }
                adopt.push_back(std::move(a));
            }
            else if (!writeOnly)
            {
                conflicts_.insert(n);           // listed as a conflict (FolderState::conflicts), not as a note
                bump();
            }
        }

        // The folder's changes, one version-guarded write of the draft.
        if (!adopt.empty())
        {
            Draft next = d;
            std::vector<bool> paired(adopt.size(), false);
            std::vector<std::string> landed;
            const auto indexOf = [&next](const std::string& name) {
                for (std::size_t i = 0; i < next.files.size(); ++i)
                    if (next.files[i].name == name)
                        return static_cast<int>(i);
                return -1;
            };
            // The tab shown stays the file it was, by its name (or its new one, below).
            std::string shown = d.active >= 0 && d.active < static_cast<int>(d.files.size())
                                    ? d.files[static_cast<std::size_t>(d.active)].name
                                    : std::string();
            for (Adopt& a : adopt)
                if (a.present && indexOf(a.name) >= 0)
                {
                    next.files[static_cast<std::size_t>(indexOf(a.name))].text = a.text;
                    landed.push_back(a.name);
                }
            for (std::size_t g = 0; g < adopt.size(); ++g)
            {
                const Adopt& gone = adopt[g];
                if (gone.present)
                    continue;
                const int at = indexOf(gone.name);
                if (at < 0)
                    continue;
                // A file deleted and one added with its text: renamed, in its place (its tab).
                const uint64_t was = editorHash(next.files[static_cast<std::size_t>(at)].text);
                bool renamed = false;
                for (std::size_t k = 0; k < adopt.size() && !renamed; ++k)
                    if (!paired[k] && adopt[k].present && indexOf(adopt[k].name) < 0 && adopt[k].hash == was)
                    {
                        next.files[static_cast<std::size_t>(at)].name = adopt[k].name;
                        if (gone.name == shown)
                            shown = adopt[k].name;
                        paired[k] = paired[g] = true;
                        landed.push_back(gone.name + " -> " + adopt[k].name);
                        renamed = true;
                    }
                if (renamed)
                    continue;
                if (at == 0)
                {
                    writes.push_back(gone.name);         // the program stays: written back
                    paired[g] = true;
                    note(gone.name + ": the program, written back", now);
                    continue;
                }
                next.files.erase(next.files.begin() + at);
                paired[g] = true;
                landed.push_back(gone.name + " deleted");
            }
            for (std::size_t k = 0; k < adopt.size(); ++k)
            {
                Adopt& a = adopt[k];
                if (paired[k] || !a.present || indexOf(a.name) >= 0)
                    continue;
                if (next.files.size() >= kMaxFiles)
                {
                    conditions.push_back(a.name + ": no room (" + std::to_string(kMaxFiles) + " files)");
                    a.name.clear();                      // not taken
                    continue;
                }
                next.files.push_back({ a.name, a.text });
                landed.push_back(a.name + " new");
            }
            next.active = 0;
            for (std::size_t i = 0; i < next.files.size(); ++i)
                if (next.files[i].name == shown)
                    next.active = static_cast<int>(i);
            if (next.files != d.files)
            {
                if (!plugin_.setDraftIf(d, next))
                    return false;                        // the draft moved meanwhile: decided again next pass
                for (const Adopt& a : adopt)
                {
                    if (a.name.empty())
                        continue;
                    if (a.present && findFile(next.files, a.name) != nullptr)
                        base_[a.name] = a.hash;
                    else if (!a.present && findFile(next.files, a.name) == nullptr)
                        base_.erase(a.name);
                }
                manifestDirty_ = true;
                landed_ = joined(landed);
                ++landedCount_;
                bump();
                if (runOnSave_ && plugin_.run)
                    plugin_.run(next.files);
            }
        }

        // The plugin's changes: deletions first (a name that differs by case alone would take the new file with it).
        for (const std::string& n : removes)
        {
            removeFile(n);
            base_.erase(n);
            manifestDirty_ = true;
        }
        for (const std::string& n : writes)
            if (const SourceFile* f = findFile(d.files, n))
            {
                if (copies_.erase(n) > 0)
                    manifestDirty_ = true;               // a read-only copy the program now has: its own, writable
                if (writeFile(n, f->text, false))
                {
                    base_[n] = editorHash(f->text);
                    manifestDirty_ = true;
                }
            }
        if (waiting && pendingSince_ < 0.0)
            pendingSince_ = now;
        else if (!waiting)
            pendingSince_ = -1.0;

        syncCopies(provided, d, now, conditions);
        std::sort(conditions.begin(), conditions.end());
        if (conditions != conditions_)
        {
            conditions_ = std::move(conditions);
            bump();
        }
        return true;
    }

    void FolderSync::syncCopies(const std::map<std::string, std::string>& provided, const Draft& d, double now,
                                std::vector<std::string>& conditions)
    {
        const auto forgetBase = [this](const std::string& n) {
            if (base_.erase(n) > 0)
                manifestDirty_ = true;
        };
        for (const auto& [n, text] : provided)
        {
            const uint64_t h = editorHash(text);
            const bool listed = listing_.count(n) > 0;
            const Seen* s = listed ? seenOf(n) : nullptr;
            if (listed && s == nullptr)
                continue;                                // unreadable just now: next time
            if (s != nullptr && !s->large && s->hash == h)
            {
                if (const auto c = copies_.find(n); c == copies_.end() || c->second != h)
                {
                    std::error_code ec;                  // a file of the program's that is now a copy: read only
                    fs::permissions(folder_ / pathOf(n), fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                                    fs::perm_options::remove, ec);
                    copies_[n] = h;
                    manifestDirty_ = true;
                }
                forgetBase(n);
                continue;
            }
            // A file of the draft's until now (decide() kept its base for this), the folder's as both last agreed on:
            // the plugin's to write over, quietly, as a file the plugin deletes is removed.
            const auto b = base_.find(n);
            const bool agreed = b != base_.end() && s != nullptr && !s->large && s->hash == b->second;
            if (s != nullptr && !agreed)
            {
                // A copy changed in the folder (made writable and saved), a file of the plugin's name the user put
                // there, or a save of a draft file's not taken before it became a copy: kept aside whole, and the copy
                // written again; never written over unkept.
                if (!keepFolderFileAside(n))
                {
                    conditions.push_back(n + ": could not be kept in " + kConflicts + ", not written");
                    continue;
                }
                note(n + (n == "host.fm" ? ": read only, written again" : ": library copy, written again"), now);
            }
            if (writeFile(n, text, true))
            {
                copies_[n] = h;
                forgetBase(n);
                manifestDirty_ = true;
            }
        }
        // Copies no #include reads any more: removed, unless changed (then the folder's own file, taken as one).
        for (auto c = copies_.begin(); c != copies_.end();)
        {
            if (provided.count(c->first) > 0)
            {
                ++c;
                continue;
            }
            if (findFile(d.files, c->first) == nullptr)
                if (const Seen* s = listing_.count(c->first) > 0 ? seenOf(c->first) : nullptr; s != nullptr && s->hash == c->second)
                    removeFile(c->first);
            c = copies_.erase(c);
            manifestDirty_ = true;
        }
    }

    // ---- a conflict settled ------------------------------------------------------------------------------------------

    bool FolderSync::resolve(const std::string& file, bool keepPlugin, double now, std::string& why)
    {
        if (!on_ || conflicts_.count(file) == 0)
        {
            why = file + " is in no conflict";
            return false;
        }
        const Draft d = plugin_.draft();
        const SourceFile* df = findFile(d.files, file);
        const fs::path path = folder_ / pathOf(file);
        std::string raw;
        bool large = false;
        const bool fh = readAll(path, raw, kMaxBytes, large);
        // A file there that cannot be read (over 1 MB, unreadable) is no deletion: never taken, never written over unkept.
        std::error_code ec;
        const bool there = fh || fs::exists(path, ec) || ec;
        if (keepPlugin)
        {
            if (there && !keepFolderFileAside(file))
            {
                why = "could not keep the folder's " + file + " in " + kConflicts;
                return false;
            }
            if (df != nullptr)
            {
                if (!writeFile(file, df->text, false))
                {
                    why = "could not write " + file;
                    return false;
                }
                base_[file] = editorHash(df->text);
            }
            else
            {
                removeFile(file);
                base_.erase(file);
            }
            note(file + ": kept the plugin's", now);
        }
        else
        {
            if (there && !fh)
            {
                why = file + (large ? " in the folder is over 1 MB: too large to take" : " in the folder could not be read");
                return false;
            }
            Draft next = d;
            const auto at = std::find_if(next.files.begin(), next.files.end(), [&](const SourceFile& f) { return f.name == file; });
            if (fh)
            {
                const std::string text = editorText(raw);
                if (at != next.files.end())
                    at->text = text;
                else if (next.files.size() >= kMaxFiles)
                {
                    why = "no room for " + file + " (" + std::to_string(kMaxFiles) + " files at most)";
                    return false;
                }
                else
                    next.files.push_back({ file, text });
            }
            else if (at != next.files.end())
            {
                if (at == next.files.begin())
                {
                    why = file + " is the program: rename or delete it in the plugin";
                    return false;
                }
                next.files.erase(at);
                next.active = std::clamp(next.active, 0, static_cast<int>(next.files.size()) - 1);
            }
            if (!plugin_.setDraftIf(d, next))
            {
                why = "the files changed meanwhile: try again";
                return false;
            }
            if (df != nullptr)
                keepAside(file, df->text, "plugin");
            if (fh)
                base_[file] = editorHash(raw);
            else
                base_.erase(file);
            landed_ = file;
            ++landedCount_;
            if (runOnSave_ && plugin_.run)
                plugin_.run(next.files);
            note(file + ": kept the folder's", now);
        }
        conflicts_.erase(file);
        manifestDirty_ = true;
        saveManifest();
        bump();
        return true;
    }

    FolderState FolderSync::state(const std::string& shownPath) const
    {
        FolderState s;
        s.on = on_;
        s.name = name_;
        s.path = shownPath;
        s.runOnSave = runOnSave_;
        s.watching = watching();
        s.conflicts.assign(conflicts_.begin(), conflicts_.end());
        for (const Event& e : events_)
            s.notes.push_back(e.text);
        for (const std::string& c : conditions_)
            s.notes.push_back(c);
        s.landed = landed_;
        s.landedCount = landedCount_;
        s.error = error_;
        return s;
    }
}
