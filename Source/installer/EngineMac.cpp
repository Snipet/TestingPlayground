// Source/installer/EngineMac.cpp: the engine's macOS half (Engine.h, EnginePlatform.h): what is at a path (lstat), the
// renames (renamex_np), versions (Info.plist) and the code signature's CDHash (Security), this Mac's places and folders,
// and "All users": the shell script osascript runs as root, its status file read as it runs.
#include "installer/Engine.h"
#include "installer/EnginePlatform.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <pwd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playground::installer
{
    namespace pf = platform;
    using pf::actionWords;
    using pf::fileText;
    using pf::plainName;
    using pf::trimmed;

    namespace
    {
        bool lstatOf(const fs::path& p, struct stat& st)
        {
            return ::lstat(p.c_str(), &st) == 0;
        }

        bool writable(const fs::path& p)
        {
            return ::access(p.c_str(), W_OK) == 0;
        }

        std::string ownerOf(const struct stat& st)
        {
            if (const passwd* pw = ::getpwuid(st.st_uid); pw != nullptr && pw->pw_name != nullptr)
                return pw->pw_name;
            return "user " + std::to_string(st.st_uid);
        }

        std::string cfString(CFStringRef s)
        {
            if (s == nullptr)
                return {};
            char buf[256];
            if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8))
                return buf;
            return {};
        }

        // The CDHash of a signed bundle or binary (its code directory's hash: the main executable's code and, through
        // the resource seal's hash, every sealed file), in hex; "" when it has no signature.
        std::string codeHash(const fs::path& p)
        {
            const std::string s = p.string();
            CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(s.data()),
                                                                   static_cast<CFIndex>(s.size()), fs::is_directory(p));
            if (url == nullptr)
                return {};
            std::string out;
            SecStaticCodeRef code = nullptr;
            if (SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess && code != nullptr)
            {
                CFDictionaryRef info = nullptr;
                if (SecCodeCopySigningInformation(code, kSecCSDefaultFlags, &info) == errSecSuccess && info != nullptr)
                {
                    const void* v = CFDictionaryGetValue(info, kSecCodeInfoUnique);
                    if (v != nullptr && CFGetTypeID(v) == CFDataGetTypeID())
                    {
                        const auto data = static_cast<CFDataRef>(v);
                        const UInt8* bytes = CFDataGetBytePtr(data);
                        const CFIndex n = CFDataGetLength(data);
                        char hex[3];
                        for (CFIndex i = 0; i < n; ++i)
                        {
                            std::snprintf(hex, sizeof hex, "%02x", static_cast<unsigned>(bytes[i]));
                            out += hex;
                        }
                    }
                    CFRelease(info);
                }
                CFRelease(code);
            }
            CFRelease(url);
            return out;
        }

        // The known hosts, by their app bundle's name (its start): a running one is told to restart.
        constexpr const char* kHosts[] = {
            "Logic Pro", "MainStage", "GarageBand", "Ableton Live", "Live", "REAPER", "Bitwig Studio", "Cubase", "Nuendo",
            "Studio One", "FL Studio", "Reason", "Digital Performer", "Ardour", "Waveform", "Renoise", "Mixbus",
            "Audacity", "Final Cut Pro", "AudioPluginHost", "Audio Plugin Host", "Luna", "Tracktion",
        };

        bool isHost(const std::string& bundleName)
        {
            for (const char* h : kHosts)
            {
                const std::string_view host(h);
                if (bundleName.size() >= host.size() && bundleName.compare(0, host.size(), host) == 0
                    && (bundleName.size() == host.size() || bundleName[host.size()] == ' '
                        || bundleName[host.size()] == '.'))
                    return true;
            }
            return false;
        }
    }

    // ---- EnginePlatform.h --------------------------------------------------------------------------------------------

    pf::Node pf::nodeOf(const fs::path& p)
    {
        Node n;
        struct stat st{};
        if (!lstatOf(p, st))
        {
            n.error = errno;
            n.missing = n.error == ENOENT;
            return n;
        }
        n.exists = true;
        n.link = S_ISLNK(st.st_mode);
        n.dir = S_ISDIR(st.st_mode);
        n.file = S_ISREG(st.st_mode);
        n.mine = st.st_uid == ::getuid();
        if (!n.mine)
            n.owner = ownerOf(st);
        return n;
    }

    bool pf::writable(const fs::path& dir)
    {
        return ::access(dir.c_str(), W_OK) == 0;
    }

    bool pf::mayMove(const fs::path& path)
    {
        struct stat st{};
        if (!lstatOf(path, st))
            return false;
        if (!writable(path.parent_path()))
            return false;
        return !S_ISDIR(st.st_mode) || (st.st_uid == ::getuid() && writable(path));
    }

    std::string pf::linkTarget(const fs::path& link)
    {
        std::error_code ec;
        const fs::path t = fs::read_symlink(link, ec);
        return ec ? std::string("somewhere it cannot read") : t.string();
    }

    std::string pf::errorText(int error)
    {
        return std::strerror(error);
    }

    bool pf::renameItem(const fs::path& from, const fs::path& to, int& error)
    {
        if (::rename(from.c_str(), to.c_str()) == 0)
            return true;
        error = errno;
        return false;
    }

    bool pf::renameNew(const fs::path& from, const fs::path& to, int& error)
    {
        if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0)
            return true;
        error = errno;
        return false;
    }

    bool pf::swapItems(const fs::path& a, const fs::path& b, int& error)
    {
        if (::renamex_np(a.c_str(), b.c_str(), RENAME_SWAP) == 0)
            return true;
        error = errno;
        return false;
    }

    bool pf::removeEmptyFolder(const fs::path& p)
    {
        return ::rmdir(p.c_str()) == 0;
    }

    fs::path pf::makeStage(const fs::path& folder, int& error)
    {
        std::string tmpl = (folder / ".installing-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (::mkdtemp(buf.data()) == nullptr)
        {
            error = errno;
            return {};
        }
        return fs::path(buf.data());
    }

    std::string pf::cdhash(const fs::path& p)
    {
        return codeHash(p);
    }

    std::string pf::now()
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        ::localtime_r(&t, &tm);
        char b[32];
        std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
        return b;
    }

    std::string pf::appFile(const std::string& name)
    {
        return name + ".app";
    }

    bool pf::sameName(const fs::path& a, const fs::path& b)
    {
        return a == b;
    }

    std::string pf::hostName(const fs::path& app)
    {
        const std::string name = app.stem().string();
        return isHost(name) ? name : std::string();
    }

    // ---- places, folders and versions --------------------------------------------------------------------------------

    Places Places::thisMachine()
    {
        Places p;
        if (const char* home = std::getenv("HOME"); home != nullptr && *home == '/')
            p.home = home;
        else if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_dir != nullptr)
            p.home = pw->pw_dir;
        p.root = "/";
        p.scratch = "/private/tmp";
        return p;
    }

    Places Places::thisMachine(const std::string& logName)
    {
        Places p = thisMachine();
        if (!p.home.empty() && plainName(logName))
            p.log = p.home / "Library" / "Logs" / (logName + ".log");
        return p;
    }

    std::string Engine::bundleVersion(const fs::path& bundle)
    {
        const std::string text = fileText(bundle / "Contents" / "Info.plist");
        if (text.empty())
            return {};
        CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(text.data()), static_cast<CFIndex>(text.size()));
        if (data == nullptr)
            return {};
        std::string version;
        CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
        if (plist != nullptr)
        {
            if (CFGetTypeID(plist) == CFDictionaryGetTypeID())
            {
                const auto dict = static_cast<CFDictionaryRef>(plist);
                for (CFStringRef key : { CFSTR("CFBundleShortVersionString"), CFSTR("CFBundleVersion") })
                {
                    const void* v = CFDictionaryGetValue(dict, key);
                    if (v != nullptr && CFGetTypeID(v) == CFStringGetTypeID())
                    {
                        version = cfString(static_cast<CFStringRef>(v));
                        if (!version.empty())
                            break;
                    }
                }
            }
            CFRelease(plist);
        }
        CFRelease(data);
        return version;
    }

    fs::path Engine::folder(const Product& product, Part part, Scope scope) const
    {
        const fs::path& base = scope == Scope::user ? places_.home : places_.root;
        switch (part)
        {
            case Part::au:   return base / "Library" / "Audio" / "Plug-Ins" / "Components";
            case Part::vst3: return base / "Library" / "Audio" / "Plug-Ins" / "VST3";
            case Part::app:
                if (scope == Scope::system || writable(places_.root / "Applications"))
                    return places_.root / "Applications";
                return places_.home / "Applications";
            case Part::extension: return base / "Library" / "Application Support" / product.company / product.name;
        }
        return base;
    }

    std::vector<std::pair<fs::path, Scope>> Engine::knownFolders(const Product& product, Part part) const
    {
        if (part == Part::app)
            return { { places_.home / "Applications", Scope::user }, { places_.root / "Applications", Scope::system } };
        return { { folder(product, part, Scope::user), Scope::user }, { folder(product, part, Scope::system), Scope::system } };
    }

    std::string Engine::shown(const fs::path& p) const
    {
        const fs::path n = p.lexically_normal();
        const auto below = [&](const fs::path& base, fs::path& rel) {
            rel = n.lexically_relative(base.lexically_normal());
            return !rel.empty() && *rel.begin() != "..";
        };
        fs::path rel;
        if (!places_.home.empty() && below(places_.home, rel))
            return rel == "." ? std::string("~") : "~/" + rel.string();
        if (!places_.root.empty() && places_.root != "/" && below(places_.root, rel))
            return rel == "." ? std::string("/") : "/" + rel.string();
        return n.string();
    }

    fs::path Engine::trashFolder() const
    {
        return places_.trash.empty() ? places_.home / ".Trash" : places_.trash;
    }

    // ---- "All users": the script, run as root ------------------------------------------------------------------------

    namespace
    {
        // ASCII letters lowered: `pwd -P` gives a folder's name as the disk has it, and the disks are mostly
        // case-insensitive, so a pinned folder is compared with its expected path in lower case (LC_ALL=C tr).
        std::string lowered(std::string s)
        {
            for (char& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        }
    }

    fs::path Engine::realOf(const fs::path& p) const
    {
        const fs::path n = p.lexically_normal();
        for (const auto& [base, real] : { std::pair<const fs::path&, const fs::path&>{ places_.root, realRoot_ },
                                          std::pair<const fs::path&, const fs::path&>{ places_.home, realHome_ } })
        {
            if (base.empty())
                continue;
            const fs::path rel = n.lexically_relative(base.lexically_normal());
            if (!rel.empty() && *rel.begin() != "..")
                return rel == "." ? real : (real / rel).lexically_normal();
        }
        std::error_code ec;
        const fs::path parent = fs::weakly_canonical(n.parent_path(), ec);
        return ec ? n : parent / n.filename();
    }

    std::string Engine::scriptHead(const std::string& what, const fs::path& statusDir) const
    {
        const fs::path trash = trashFolder();
        std::string s;
        s += "#!/bin/sh\n";
        s += "# " + what + " for all users: generated for this one run, then run once as root through macOS's\n";
        s += "# administrator prompt, its text passed to it (never a file). Every path is quoted. Each step reports to\n";
        s += "# the status file, in a folder the script makes itself; the first failure ends the script. Nothing is\n";
        s += "# deleted: what is replaced or removed goes to the Trash.\n";
        s += "umask 022\n";
        s += "if [ \"$(id -u)\" -eq 0 ]; then ROOT=1; else ROOT=0; fi\n";
        // The status folder: new (mkdir fails on anything there, a link too), in a folder only root may change, and
        // given back to the user as the script ends, whatever ends it.
        s += "SDIR=" + shellQuote(statusDir.string()) + "\n";
        s += "mkdir -m 755 \"$SDIR\" || exit 1\n";
        s += "if [ \"$ROOT\" -eq 1 ]; then trap 'chown -R " + std::to_string(::getuid()) + " \"$SDIR\"' EXIT; fi\n";
        s += "STATUS=\"$SDIR\"/" + std::string(kStatusFile) + "\n";
        s += "say() { printf '%s\\n' \"$*\" >> \"$STATUS\"; }\n";
        s += "fail() { say \"STEP $1 fail $2\"; exit 1; }\n";
        // here <folder> <its real path, lower case>: in the folder, which is the real one (no link on the way to it).
        s += "here() { cd -P \"$1\" 2>/dev/null && [ \"$(pwd -P | LC_ALL=C tr 'A-Z' 'a-z')\" = \"$2\" ]; }\n";
        // discard <item, from here> <stem> <extension>: to the user's real Trash under a name not taken there; a link
        // there is never followed (mv -h).
        s += "TRASH=" + shellQuote(trash.string()) + "\n";
        s += "TRASHREAL=" + shellQuote(lowered(realOf(trash).string())) + "\n";
        s += "discard() {\n";
        s += "  S=\"$(pwd -P)/$1\"\n";
        s += "  ( here \"$TRASH\" \"$TRASHREAL\" || exit 1\n";
        s += "    D=\"./$2$3\"; n=2\n";
        s += "    while [ -e \"$D\" ] || [ -L \"$D\" ]; do D=\"./$2 $n$3\"; n=$((n + 1)); done\n";
        s += "    mv -h \"$S\" \"$D\" )\n";
        s += "}\n";
        return s;
    }

    std::string Engine::walkChecks(const fs::path& folder, const std::string& k) const
    {
        const auto fail = [&](const std::string& why) { return "fail " + k + " " + shellQuote(why); };
        std::string s;
        // Every folder from the root (or home) down to this one is a real folder...
        const fs::path base = folder.lexically_normal();
        fs::path anchor = places_.root, rel = base.lexically_relative(places_.root.lexically_normal());
        if (rel.empty() || *rel.begin() == "..")
        {
            anchor = places_.home;
            rel = base.lexically_relative(places_.home.lexically_normal());
        }
        if (!rel.empty() && *rel.begin() != "..")
        {
            fs::path walk = anchor;
            for (const fs::path& part : rel)
            {
                if (part == ".")
                    continue;
                walk /= part;
                s += "if [ -L " + shellQuote(walk.string()) + " ]; then "
                     + fail(shown(walk) + " is a link: this installer writes only into real folders") + "; fi\n";
            }
        }
        // ... and the step works in it, from here on, by names relative to it: whatever is renamed on the way later.
        s += "here " + shellQuote(folder.string()) + " " + shellQuote(lowered(realOf(folder).string())) + " || "
             + fail(shown(folder) + " is not a real folder (a link on the way to it): left as it is") + "\n";
        return s;
    }

    std::string Engine::adminScript(const Plan& plan, const fs::path& statusDir) const
    {
        std::string s = scriptHead("Installing", statusDir);
        for (std::size_t i = 0; i < plan.steps.size(); ++i)
        {
            const Step& st = plan.steps[i];
            if (!st.changes())
                continue;
            const std::string k = std::to_string(i);
            const std::string name = shellQuote(st.target.filename().string());
            const std::string here = shellQuote("./" + st.target.filename().string());   // the target, from its folder
            const std::string stem = shellQuote(st.target.stem().string()), ext = shellQuote(st.target.extension().string());
            const std::string folder = shellQuote(st.folder.string());
            const auto fail = [&](const std::string& why) { return "fail " + k + " " + shellQuote(why); };
            const std::string drop = "discard \"$T\"/" + name + " " + stem + " " + ext;
            s += "\n# " + k + ": " + st.product + ", " + partName(st.part) + " (" + actionWords(st.action) + ")\n";
            s += "say \"STEP " + k + " start\"\n";
            s += "mkdir -p " + folder + " || " + fail("it could not make the folder " + shown(st.folder)) + "\n";
            s += walkChecks(st.folder, k);
            s += "if [ -L " + here + " ]; then " + fail(shown(st.target) + " is a link: left as it is") + "; fi\n";
            s += "T=$(mktemp -d ./.installing-XXXXXX) || " + fail("it may not write in " + shown(st.folder)) + "\n";
            s += "if ! ditto --noqtn " + shellQuote(st.source.string()) + " \"$T\"/" + name + "; then\n";
            s += "  if [ -e \"$T\"/" + name + " ]; then " + drop + "; fi\n";
            s += "  rmdir \"$T\"; " + fail("it could not copy it into " + shown(st.folder)) + "\n";
            s += "fi\n";
            s += "if [ \"$ROOT\" -eq 1 ] && ! chown -R root:wheel \"$T\"/" + name + "; then\n";
            s += "  " + drop + "; rmdir \"$T\"; " + fail("it could not give the copy to root") + "\n";
            s += "fi\n";
            s += "if [ -e " + here + " ]; then\n";
            s += "  if ! mv -h " + here + " \"$T/old\"; then\n";
            s += "    " + drop + "; rmdir \"$T\"; " + fail("it could not move the old copy aside") + "\n";
            s += "  fi\n";
            s += "  if ! mv -h \"$T\"/" + name + " " + here + "; then\n";
            s += "    mv -h \"$T/old\" " + here + "; " + drop + "; rmdir \"$T\"; "
                 + fail("it could not put the new copy in place") + "\n";
            s += "  fi\n";
            s += "  if discard \"$T/old\" " + stem + " " + ext + "; then say \"STEP " + k
                 + " note the old copy is in the Trash\"; else say \"STEP " + k + " note the old copy stays in \""
                 + shellQuote(shown(st.folder)) + "\"/${T#./}\"; fi\n";
            s += "elif ! mv -h \"$T\"/" + name + " " + here + "; then\n";
            s += "  " + drop + "; rmdir \"$T\"; " + fail("it could not put it in " + shown(st.folder)) + "\n";
            s += "fi\n";
            s += "rmdir \"$T\" 2>/dev/null\n";
            s += "cd /\n";
            s += "say \"STEP " + k + " done\"\n";
        }
        return s;
    }

    std::string Engine::adminUninstallScript(const std::vector<Copy>& copies, const fs::path& statusDir,
                                             const std::vector<int>& only) const
    {
        std::string s = scriptHead("Uninstalling", statusDir);
        for (std::size_t i = 0; i < copies.size(); ++i)
        {
            const Copy& c = copies[i];
            if (c.link || (!only.empty() && std::find(only.begin(), only.end(), static_cast<int>(i)) == only.end()))
                continue;                                // links stay; the copies not asked for are not its
            const std::string k = std::to_string(i), here = shellQuote("./" + c.path.filename().string());
            const auto fail = [&](const std::string& why) { return "fail " + k + " " + shellQuote(why); };
            s += "\n# " + k + ": " + c.path.filename().string() + "\n";
            s += "say \"STEP " + k + " start\"\n";
            s += walkChecks(c.path.parent_path(), k);
            s += "if [ -L " + here + " ]; then " + fail(shown(c.path) + " is a link: left as it is") + "; fi\n";
            s += "if [ -e " + here + " ]; then\n";
            s += "  discard " + here + " " + shellQuote(c.path.stem().string()) + " "
                 + shellQuote(c.path.extension().string()) + " || " + fail("it could not move it to the Trash") + "\n";
            s += "fi\n";
            s += "cd /\n";
            s += "say \"STEP " + k + " done\"\n";
        }
        return s;
    }

    Report Engine::runAdmin(const std::function<std::string(const fs::path&)>& makeScript, int steps,
                            const std::vector<int>& indices, const ProgressFn& progress, Report report,
                            const std::string& what, const std::vector<fs::path>& /*targets: Windows' alone*/)
    {
        Report r = std::move(report);
        const auto setState = [&](int i, State s, const std::string& line) {
            if (i < 0 || i >= steps)
                return;
            r.states[static_cast<std::size_t>(i)] = s;
            r.lines[static_cast<std::size_t>(i)] = line;
            if (progress)
                progress(i, s, line);
        };

        // The status folder, which the script makes as root (it must not be there yet), in a folder only root may
        // change; its name is new each run. The script itself goes to the prompt as text: no file of it is run.
        std::error_code ec;
        const fs::path scratch = places_.scratch.empty() ? fs::path("/private/tmp") : places_.scratch;
        fs::create_directories(scratch, ec);             // /private/tmp is there already; a probe's is made
        unsigned char rnd[8];
        ::arc4random_buf(rnd, sizeof rnd);
        std::string id;
        char hex[3];
        for (const unsigned char b : rnd)
        {
            std::snprintf(hex, sizeof hex, "%02x", static_cast<unsigned>(b));
            id += hex;
        }
        const fs::path statusDir = scratch / ("installer-" + id);
        const fs::path status = statusDir / kStatusFile;
        const std::string script = makeScript(statusDir);
        fs::create_directories(trashFolder(), ec);       // as this user: the script only moves into it
        for (const int i : indices)
            setState(i, State::waiting, "waiting for the administrator's password");

        // The status file, read as the script writes it.
        std::size_t seen = 0;
        std::vector<std::string> failures;
        const auto poll = [&]() {
            const std::string all = fileText(status);
            while (seen < all.size())
            {
                const std::size_t end = all.find('\n', seen);
                if (end == std::string::npos)
                    break;
                const std::string line = all.substr(seen, end - seen);
                seen = end + 1;
                std::istringstream in(line);
                std::string word, verb;
                int i = -1;
                in >> word >> i >> verb;
                std::string rest;
                std::getline(in, rest);
                rest = trimmed(rest);
                if (word != "STEP" || i < 0 || i >= steps)
                    continue;
                if (verb == "start")
                    setState(i, State::working, what == "install" ? "copying" : "moving to the Trash");
                else if (verb == "done")
                {
                    std::string l = r.lines[static_cast<std::size_t>(i)];
                    setState(i, State::done, l.rfind("note:", 0) == 0 ? l.substr(5) : std::string());
                }
                else if (verb == "note")
                    r.lines[static_cast<std::size_t>(i)] = "note:" + rest;
                else if (verb == "fail")
                {
                    setState(i, State::failed, rest);
                    failures.push_back(rest);
                }
            }
        };
        std::mutex m;
        std::atomic<bool> running{ true };
        std::thread poller([&]() {
            while (running.load())
            {
                {
                    const std::lock_guard<std::mutex> lock(m);
                    poll();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        std::string error;
        int exitStatus = -1;                             // macOS reads the status file instead
        const System::Admin result = system_.runAsAdministrator(
            script, payload_.project + " Installer wants to " + (what == "install" ? "install" : "uninstall")
                        + " plugins for all users.",
            error, exitStatus);
        running.store(false);
        poller.join();
        poll();
        // The script gave its folder back to this user as it ended: the status file and the folder go (they are the
        // installer's own, never the user's: a folder that is not this user's, or a link, is left).
        if (struct stat st{}; lstatOf(statusDir, st) && S_ISDIR(st.st_mode) && st.st_uid == ::getuid())
        {
            ::unlink(status.c_str());
            ::rmdir(statusDir.c_str());
        }

        if (result == System::Admin::cancelled)
        {
            r.cancelled = true;
            for (const int i : indices)
                setState(i, State::notRun, "not changed: the administrator's password was not given");
            log(r, "the administrator prompt was cancelled: nothing ran");
            return r;
        }
        if (failures.empty() && result == System::Admin::failed)
            r.error = "macOS could not run the " + what + " as an administrator (" + trimmed(error) + ").";
        for (const int i : indices)
            if (r.states[static_cast<std::size_t>(i)] == State::waiting
                || r.states[static_cast<std::size_t>(i)] == State::working)
                setState(i, State::notRun, "not changed");
        for (const int i : indices)
            log(r, "step " + std::to_string(i) + ": "
                   + (r.states[static_cast<std::size_t>(i)] == State::done ? "done" : "not done") + " "
                   + r.lines[static_cast<std::size_t>(i)]);
        return r;
    }
}
