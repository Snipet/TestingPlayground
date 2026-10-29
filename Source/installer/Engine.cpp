// Source/installer/Engine.cpp: see Engine.h.
#include "installer/Engine.h"

#include <juce_core/juce_core.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <pwd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playground::installer
{
    namespace
    {
        bool lstatOf(const fs::path& p, struct stat& st)
        {
            return ::lstat(p.c_str(), &st) == 0;
        }

        bool isLink(const fs::path& p)
        {
            struct stat st{};
            return lstatOf(p, st) && S_ISLNK(st.st_mode);
        }

        bool writable(const fs::path& p)
        {
            return ::access(p.c_str(), W_OK) == 0;
        }

        std::string linkTarget(const fs::path& p)
        {
            std::error_code ec;
            const fs::path t = fs::read_symlink(p, ec);
            return ec ? std::string("somewhere it cannot read") : t.string();
        }

        std::string ownerOf(const struct stat& st)
        {
            if (const passwd* pw = ::getpwuid(st.st_uid); pw != nullptr && pw->pw_name != nullptr)
                return pw->pw_name;
            return "user " + std::to_string(st.st_uid);
        }

        std::string trimmed(std::string s)
        {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            std::size_t a = 0;
            while (a < s.size() && (s[a] == '\n' || s[a] == ' '))
                ++a;
            return s.substr(a);
        }

        // A name the payload gives that becomes part of a path: one plain component, never "..", no slash.
        bool plainName(const std::string& s)
        {
            if (s.empty() || s == "." || s == ".." || s.size() > 128)
                return false;
            return std::all_of(s.begin(), s.end(), [](char c) {
                const auto u = static_cast<unsigned char>(c);
                return std::isalnum(u) != 0 || c == ' ' || c == '-' || c == '_' || c == '.';
            });
        }

        std::string fileText(const fs::path& p)
        {
            std::ifstream in(p, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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
        std::string cdhash(const fs::path& p)
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

        bool sameFile(const fs::path& a, const fs::path& b)
        {
            std::error_code ea, eb;
            if (fs::file_size(a, ea) != fs::file_size(b, eb) || ea || eb)
                return false;
            std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
            char ba[65536], bb[65536];
            while (fa && fb)
            {
                fa.read(ba, sizeof ba);
                fb.read(bb, sizeof bb);
                if (fa.gcount() != fb.gcount()
                    || std::memcmp(ba, bb, static_cast<std::size_t>(fa.gcount())) != 0)
                    return false;
            }
            return fa.eof() && fb.eof();
        }

        // Every entry under a folder, relative, with its kind ('d', 'f', 'l'): links are listed, never followed.
        std::vector<std::pair<std::string, char>> listing(const fs::path& dir)
        {
            std::vector<std::pair<std::string, char>> out;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end; !ec && it != end;
                 it.increment(ec))
            {
                const fs::file_status st = it->symlink_status(ec);
                const char kind = fs::is_symlink(st) ? 'l' : fs::is_directory(st) ? 'd' : 'f';
                out.emplace_back(fs::relative(it->path(), dir, ec).string(), kind);
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        std::string now()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
            ::localtime_r(&t, &tm);
            char b[32];
            std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
            return b;
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

        std::string joined(const std::vector<std::string>& v)
        {
            std::string s;
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                if (i > 0)
                    s += i + 1 == v.size() ? " and " : ", ";
                s += v[i];
            }
            return s;
        }

        std::string stemOf(const fs::path& item)
        {
            return item.stem().string();
        }

        const char* actionWords(Action a)
        {
            switch (a)
            {
                case Action::install:   return "install";
                case Action::upgrade:   return "upgrade";
                case Action::downgrade: return "downgrade";
                case Action::rebuild:   return "replace (another build)";
                case Action::replace:   return "replace";
                case Action::same:      return "already installed";
                case Action::blocked:   return "blocked";
            }
            return "";
        }
    }

    // ---- names -------------------------------------------------------------------------------------------------------

    const char* partName(Part part)
    {
        switch (part)
        {
            case Part::au:        return "Audio Unit";
            case Part::vst3:      return "VST3";
            case Part::app:       return "App";
            case Part::extension: return "Claude Desktop extension";
        }
        return "";
    }

    const char* partExtension(Part part)
    {
        switch (part)
        {
            case Part::au:        return ".component";
            case Part::vst3:      return ".vst3";
            case Part::app:       return ".app";
            case Part::extension: return ".mcpb";
        }
        return "";
    }

    std::string shellQuote(const std::string& s)
    {
        std::string q = "'";
        for (const char c : s)
        {
            if (c == '\'')
                q += "'\\''";
            else
                q += c;
        }
        return q + "'";
    }

    // ---- places ------------------------------------------------------------------------------------------------------

    Places Places::thisMac()
    {
        Places p;
        if (const char* home = std::getenv("HOME"); home != nullptr && *home == '/')
            p.home = home;
        else if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_dir != nullptr)
            p.home = pw->pw_dir;
        p.root = "/";
        return p;
    }

    Places Places::thisMac(const std::string& logName)
    {
        Places p = thisMac();
        if (!p.home.empty() && plainName(logName))
            p.log = p.home / "Library" / "Logs" / (logName + ".log");
        return p;
    }

    Places Places::under(const fs::path& testRoot)
    {
        Places p;
        p.home = testRoot / "home";
        p.root = testRoot / "system";
        p.trash = testRoot / "Trash";
        p.log = testRoot / "install.log";
        return p;
    }

    // ---- the payload -------------------------------------------------------------------------------------------------

    Payload Payload::read(const fs::path& folder)
    {
        Payload p;
        p.folder = folder;
        const fs::path manifest = folder / "payload.json";
        std::error_code ec;
        if (!fs::is_regular_file(manifest, ec))
        {
            p.error = "This installer holds nothing to install: its payload (" + manifest.string() + ") is missing.";
            return p;
        }
        const juce::var v = juce::JSON::parse(juce::String::fromUTF8(fileText(manifest).c_str()));
        const juce::var list = v.getProperty("products", {});
        p.project = v.getProperty("project", {}).toString().toStdString();
        if (!list.isArray() || list.size() == 0)
        {
            p.error = "This installer's payload names no product (" + manifest.string() + ").";
            return p;
        }
        for (int i = 0; i < list.size(); ++i)
        {
            const juce::var& o = list[i];
            Product pr;
            pr.key = o.getProperty("key", {}).toString().toStdString();
            pr.name = o.getProperty("name", {}).toString().toStdString();
            pr.kind = o.getProperty("kind", {}).toString().toStdString();
            pr.company = o.getProperty("company", {}).toString().toStdString();
            pr.bundleId = o.getProperty("bundleId", {}).toString().toStdString();
            if (!plainName(pr.key) || !plainName(pr.name) || !plainName(pr.company))
            {
                p.error = "This installer's payload names a product it cannot use (" + manifest.string() + ").";
                p.products.clear();
                return p;
            }
            for (const Part part : kParts)
            {
                Offer off;
                off.part = part;
                off.source = folder / (pr.name + partExtension(part));
                struct stat st{};
                off.present = lstatOf(off.source, st) && (part == Part::extension ? S_ISREG(st.st_mode) : S_ISDIR(st.st_mode));
                if (off.present)
                    off.version = part == Part::extension ? Engine::extensionVersion(off.source) : Engine::bundleVersion(off.source);
                pr.offers.push_back(std::move(off));
            }
            pr.version = pr.offer(Part::app).version;
            for (const Part part : { Part::au, Part::vst3 })
                if (pr.version.empty())
                    pr.version = pr.offer(part).version;
            p.products.push_back(std::move(pr));
        }
        return p;
    }

    const Product* Payload::find(const std::string& key) const
    {
        for (const Product& pr : products)
            if (pr.key == key)
                return &pr;
        return nullptr;
    }

    int Plan::changing() const
    {
        return static_cast<int>(std::count_if(steps.begin(), steps.end(), [](const Step& s) { return s.changes(); }));
    }

    int Plan::blocked() const
    {
        return static_cast<int>(std::count_if(steps.begin(), steps.end(),
                                              [](const Step& s) { return s.action == Action::blocked; }));
    }

    int Report::count(State s) const
    {
        return static_cast<int>(std::count(states.begin(), states.end(), s));
    }

    // ---- versions and contents ----------------------------------------------------------------------------------------

    int Engine::compareVersions(const std::string& a, const std::string& b)
    {
        const auto parts = [](const std::string& s) {
            std::vector<std::string> v;
            std::stringstream in(s);
            std::string part;
            while (std::getline(in, part, '.'))
                v.push_back(part);
            return v;
        };
        const std::vector<std::string> pa = parts(a), pb = parts(b);
        for (std::size_t i = 0; i < std::max(pa.size(), pb.size()); ++i)
        {
            const std::string x = i < pa.size() ? pa[i] : "0", y = i < pb.size() ? pb[i] : "0";
            const bool nx = !x.empty() && std::all_of(x.begin(), x.end(), [](char c) { return c >= '0' && c <= '9'; });
            const bool ny = !y.empty() && std::all_of(y.begin(), y.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (nx && ny)
            {
                const unsigned long long vx = std::strtoull(x.c_str(), nullptr, 10), vy = std::strtoull(y.c_str(), nullptr, 10);
                if (vx != vy)
                    return vx < vy ? -1 : 1;
            }
            else if (x != y)
                return x < y ? -1 : 1;
        }
        return 0;
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

    std::string Engine::extensionVersion(const fs::path& mcpb)
    {
        juce::ZipFile zip(juce::File(juce::String::fromUTF8(mcpb.c_str())));
        const int index = zip.getIndexOfFileName("manifest.json");
        if (index < 0)
            return {};
        std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(index));
        if (in == nullptr)
            return {};
        const juce::var v = juce::JSON::parse(in->readEntireStreamAsString());
        return v.getProperty("version", {}).toString().toStdString();
    }

    bool Engine::sameContents(const fs::path& a, const fs::path& b)
    {
        struct stat sa{}, sb{};
        if (!lstatOf(a, sa) || !lstatOf(b, sb) || S_ISLNK(sa.st_mode) || S_ISLNK(sb.st_mode))
            return false;
        if (S_ISREG(sa.st_mode) && S_ISREG(sb.st_mode))
            return sameFile(a, b);
        if (!S_ISDIR(sa.st_mode) || !S_ISDIR(sb.st_mode))
            return false;
        if (const std::string ha = cdhash(a), hb = cdhash(b); !ha.empty() && !hb.empty())
            return ha == hb;
        const auto la = listing(a), lb = listing(b);
        if (la != lb)
            return false;
        for (const auto& [rel, kind] : la)
        {
            if (kind == 'f' && !sameFile(a / rel, b / rel))
                return false;
            if (kind == 'l')
            {
                std::error_code e1, e2;
                if (fs::read_symlink(a / rel, e1) != fs::read_symlink(b / rel, e2) || e1 || e2)
                    return false;
            }
        }
        return true;
    }

    // ---- the engine --------------------------------------------------------------------------------------------------

    Engine::Engine(Payload payload, Places places, System& system)
        : payload_(std::move(payload)), places_(std::move(places)), system_(system)
    {
        std::error_code ec;
        realHome_ = fs::weakly_canonical(places_.home, ec);
        if (ec)
            realHome_ = places_.home;
        realRoot_ = fs::weakly_canonical(places_.root, ec);
        if (ec)
            realRoot_ = places_.root;
    }

    std::string Engine::itemName(const Product& product, Part part)
    {
        return product.name + partExtension(part);
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

    std::string Engine::unsafe(const fs::path& folder) const
    {
        const fs::path n = folder.lexically_normal();
        const auto below = [&](const fs::path& base, fs::path& rel) {
            rel = n.lexically_relative(base.lexically_normal());
            return !rel.empty() && *rel.begin() != "..";
        };
        fs::path rel, real, said;
        if (!places_.home.empty() && below(places_.home, rel))
        {
            real = realHome_;
            said = places_.home;
        }
        else if (!places_.root.empty() && below(places_.root, rel))
        {
            real = realRoot_;
            said = places_.root;
        }
        else
            return shown(folder) + " is outside the folders this installer writes to";
        for (const fs::path& part : rel)
        {
            if (part == ".")
                continue;
            if (part == "..")
                return shown(folder) + " is outside the folders this installer writes to";
            real /= part;
            said /= part;
            struct stat st{};
            if (!lstatOf(real, st))
            {
                if (errno == ENOENT)
                    return {};                           // the rest is made when it is installed
                return "it cannot look into " + shown(said) + " (" + std::strerror(errno) + ")";
            }
            if (S_ISLNK(st.st_mode))
                return shown(said) + " is a link to " + linkTarget(real)
                       + ": this installer writes only into real folders, and left it as it is";
            if (!S_ISDIR(st.st_mode))
                return shown(said) + " is a file where a folder should be";
        }
        return {};
    }

    bool Engine::mayMove(const fs::path& path) const
    {
        struct stat st{};
        if (!lstatOf(path, st))
            return false;
        if (!writable(path.parent_path()))
            return false;
        return !S_ISDIR(st.st_mode) || (st.st_uid == ::getuid() && writable(path));
    }

    std::vector<Copy> Engine::installed(const Product& product, Part part) const
    {
        std::vector<Copy> out;
        for (const auto& [dir, scope] : knownFolders(product, part))
        {
            if (!unsafe(dir).empty())
                continue;                                // a linked folder is never looked into
            const fs::path p = dir / itemName(product, part);
            struct stat st{};
            if (!lstatOf(p, st))
                continue;
            Copy c;
            c.key = product.key;
            c.part = part;
            c.scope = scope;
            c.path = p;
            c.link = S_ISLNK(st.st_mode);
            c.mine = st.st_uid == ::getuid();
            if (!c.link)
                c.version = part == Part::extension ? extensionVersion(p) : bundleVersion(p);
            c.trashable = !c.link && mayMove(p);
            out.push_back(std::move(c));
        }
        return out;
    }

    std::vector<Copy> Engine::installed() const
    {
        std::vector<Copy> out;
        for (const Product& pr : payload_.products)
            for (const Part part : kParts)
                for (Copy& c : installed(pr, part))
                    out.push_back(std::move(c));
        return out;
    }

    Plan Engine::plan(const std::vector<Selection>& chosen, Scope scope) const
    {
        Plan plan;
        plan.scope = scope;
        for (const Product& pr : payload_.products)
            for (const Part part : kParts)
            {
                if (std::none_of(chosen.begin(), chosen.end(),
                                 [&](const Selection& s) { return s.key == pr.key && s.part == part; }))
                    continue;
                Step s;
                s.key = pr.key;
                s.product = pr.name;
                s.part = part;
                const Offer& o = pr.offer(part);
                s.source = o.source;
                s.to = o.version;
                s.folder = folder(pr, part, scope);
                s.target = s.folder / itemName(pr, part);
                struct stat st{};
                if (!o.present)
                {
                    s.action = Action::blocked;
                    s.why = "this installer does not hold it";
                }
                else if (const std::string why = unsafe(s.folder); !why.empty())
                {
                    s.action = Action::blocked;
                    s.why = why;
                }
                else if (scope == Scope::user && ::access(s.folder.c_str(), F_OK) == 0 && !writable(s.folder))
                {
                    s.action = Action::blocked;
                    s.why = shown(s.folder) + " may not be written to (its permissions): choose All users, or change them";
                }
                else if (lstatOf(s.target, st))
                {
                    const bool kindOk = part == Part::extension ? S_ISREG(st.st_mode) : S_ISDIR(st.st_mode);
                    if (S_ISLNK(st.st_mode))
                    {
                        s.action = Action::blocked;
                        s.why = shown(s.target) + " is a link to " + linkTarget(s.target) + ": left as it is";
                    }
                    else if (!kindOk)
                    {
                        s.action = Action::blocked;
                        s.why = shown(s.target) + " is not " + (part == Part::extension ? "a file" : "a bundle")
                                + ": left as it is";
                    }
                    else if (scope == Scope::user && !mayMove(s.target))
                    {
                        s.action = Action::blocked;
                        s.from = part == Part::extension ? extensionVersion(s.target) : bundleVersion(s.target);
                        s.why = st.st_uid != ::getuid()
                                    ? shown(s.target) + " belongs to " + ownerOf(st)
                                          + " (installed for all users): choose All users to replace it"
                                    : shown(s.target) + " may not be moved (its permissions): choose All users, or change them";
                    }
                    else
                    {
                        s.from = part == Part::extension ? extensionVersion(s.target) : bundleVersion(s.target);
                        if (s.from.empty() || s.to.empty())
                            s.action = Action::replace;
                        else if (const int c = compareVersions(s.from, s.to); c < 0)
                            s.action = Action::upgrade;
                        else if (c > 0)
                            s.action = Action::downgrade;
                        else
                            s.action = sameContents(s.target, s.source) ? Action::same : Action::rebuild;
                    }
                }
                else
                    s.action = Action::install;
                plan.steps.push_back(std::move(s));
            }

        // The notes: what runs that uses what changes, and copies the other scope has too.
        const std::vector<fs::path> apps = system_.runningApps();
        std::vector<std::string> hosts;
        for (const fs::path& a : apps)
        {
            const std::string name = a.stem().string();
            if (isHost(name) && std::find(hosts.begin(), hosts.end(), name) == hosts.end())
                hosts.push_back(name);
        }
        bool plugins = false;
        for (const Step& s : plan.steps)
        {
            if (!s.changes())
                continue;
            plugins = plugins || s.part == Part::au || s.part == Part::vst3;
            if (s.part == Part::app)
                for (const fs::path& a : apps)
                    if (a.filename() == s.product + ".app")
                    {
                        plan.notes.push_back(s.product + " is open: quit it before installing, or it plays the old "
                                             "version until it opens again.");
                        break;
                    }
        }
        if (plugins && !hosts.empty())
            plan.notes.push_back(joined(hosts) + (hosts.size() == 1 ? " is" : " are")
                                 + " open: restart your DAW after installing, and it loads the new plugins.");
        for (const Step& s : plan.steps)
        {
            const Product* pr = payload_.find(s.key);
            if (pr == nullptr || s.part == Part::extension)
                continue;
            for (const Copy& c : installed(*pr, s.part))
                if (c.path.parent_path() != s.folder && !c.link)
                    plan.notes.push_back(s.product + "'s " + partName(s.part) + " is also in " + shown(c.path.parent_path())
                                         + (s.part == Part::app ? ": two copies of the app."
                                                                : ": hosts list both; uninstall one."));
        }
        return plan;
    }

    // ---- the trash and the log ---------------------------------------------------------------------------------------

    fs::path Engine::trashFolder() const
    {
        return places_.trash.empty() ? places_.home / ".Trash" : places_.trash;
    }

    fs::path Engine::trashName(const fs::path& item) const
    {
        const fs::path dir = trashFolder();
        const std::string stem = stemOf(item), ext = item.extension().string();
        fs::path p = dir / item.filename();
        for (int n = 2; ::access(p.c_str(), F_OK) == 0 || isLink(p); ++n)
            p = dir / (stem + " " + std::to_string(n) + ext);
        return p;
    }

    bool Engine::trash(const fs::path& path, std::string& where, std::string& error)
    {
        if (places_.trash.empty())
        {
            where = "the Trash";
            return system_.moveToTrash(path, error);
        }
        std::error_code ec;
        fs::create_directories(places_.trash, ec);
        const fs::path dest = trashName(path);
        if (::rename(path.c_str(), dest.c_str()) != 0)
        {
            error = std::strerror(errno);
            return false;
        }
        where = shown(dest);
        return true;
    }

    void Engine::log(Report& report, const std::string& line)
    {
        report.log.push_back(line);
        if (places_.log.empty())
            return;
        std::error_code ec;
        fs::create_directories(places_.log.parent_path(), ec);
        std::ofstream out(places_.log, std::ios::app);
        out << now() << "  " << line << "\n";
    }

    // ---- installing --------------------------------------------------------------------------------------------------

    bool Engine::installStep(const Step& step, std::string& line, std::string& error)
    {
        const std::string name = step.target.filename().string();
        std::error_code ec;
        fs::create_directories(step.folder, ec);
        if (ec)
        {
            error = "it could not make the folder " + shown(step.folder) + " (" + ec.message() + ")";
            return false;
        }
        if (const std::string why = unsafe(step.folder); !why.empty())
        {
            error = why;
            return false;
        }
        struct stat st{};
        const bool replacing = lstatOf(step.target, st);
        if (replacing && S_ISLNK(st.st_mode))
        {
            error = shown(step.target) + " is a link to " + linkTarget(step.target) + ": left as it is";
            return false;
        }

        // The staging folder beside the target: the copy is made there, then renamed into place.
        std::string tmpl = (step.folder / ".installing-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (::mkdtemp(buf.data()) == nullptr)
        {
            error = "it may not write in " + shown(step.folder) + " (" + std::strerror(errno) + ")";
            return false;
        }
        const fs::path stage(buf.data());
        const fs::path staged = stage / name;
        // A copy that will not be used: to the Trash (never deleted), then the staging folder, empty, removed. What
        // could not go is said.
        const auto discardStage = [&]() -> std::string {
            std::string where, terr;
            if (::access(staged.c_str(), F_OK) == 0 || isLink(staged))
                trash(staged, where, terr);
            if (::rmdir(stage.c_str()) == 0 || errno == ENOENT)
                return {};
            return "; the copy it made stays in " + shown(stage);
        };
        std::string copyError;
        if (!system_.copy(step.source, staged, copyError))
        {
            error = "it could not copy it into " + shown(step.folder) + " (" + copyError + ")";
            error += discardStage();
            return false;
        }
        if (replacing)
        {
            if (::renamex_np(staged.c_str(), step.target.c_str(), RENAME_SWAP) != 0)
            {
                error = "it could not put the new copy in place of " + shown(step.target) + " (" + std::strerror(errno) + ")";
                error += discardStage();
                return false;
            }
            // The old copy is where the new one was: to the Trash.
            std::string where, terr;
            if (trash(staged, where, terr))
                line = "the old copy" + (step.from.empty() ? std::string() : " (" + step.from + ")") + " is in " + where;
            else
                line = "the old copy" + (step.from.empty() ? std::string() : " (" + step.from + ")") + " stays at "
                       + shown(staged) + " (" + terr + ")";
        }
        else if (::renamex_np(staged.c_str(), step.target.c_str(), RENAME_EXCL) != 0)
        {
            error = "it could not put it in " + shown(step.folder) + " (" + std::strerror(errno) + ")";
            error += discardStage();
            return false;
        }
        ::rmdir(stage.c_str());                          // empty unless the old copy could not go to the Trash
        return true;
    }

    Report Engine::install(const Plan& plan, const ProgressFn& progress, const std::atomic<bool>* cancel)
    {
        const int n = static_cast<int>(plan.steps.size());
        Report r;
        r.states.assign(static_cast<std::size_t>(n), State::waiting);
        r.lines.assign(static_cast<std::size_t>(n), std::string());
        const auto set = [&](int i, State s, const std::string& line) {
            r.states[static_cast<std::size_t>(i)] = s;
            r.lines[static_cast<std::size_t>(i)] = line;
            if (progress)
                progress(i, s, line);
        };
        log(r, "install " + std::string(plan.scope == Scope::user ? "just for me" : "for all users") + " from "
               + payload_.folder.string());
        for (int i = 0; i < n; ++i)
        {
            const Step& s = plan.steps[static_cast<std::size_t>(i)];
            if (s.action == Action::same)
                set(i, State::skipped, "already installed: " + shown(s.target) + " (" + s.to + ")");
            else if (s.action == Action::blocked)
                set(i, State::skipped, "left as it was: " + s.why);
        }
        for (int i = 0; i < n; ++i)
        {
            const Step& s = plan.steps[static_cast<std::size_t>(i)];
            if (!s.changes())
                log(r, s.product + " " + partName(s.part) + ": " + r.lines[static_cast<std::size_t>(i)]);
        }

        std::vector<int> todo;
        for (int i = 0; i < n; ++i)
            if (plan.steps[static_cast<std::size_t>(i)].changes())
                todo.push_back(i);

        if (plan.admin())
        {
            r = runAdmin([&](const fs::path& status) { return adminScript(plan, status); }, n, todo, progress,
                         std::move(r), "install");
            for (const int i : todo)
                if (r.states[static_cast<std::size_t>(i)] == State::failed && r.error.empty())
                {
                    const Step& s = plan.steps[static_cast<std::size_t>(i)];
                    r.error = s.product + "'s " + partName(s.part) + " was not installed: "
                              + r.lines[static_cast<std::size_t>(i)] + ".";
                }
        }
        else
        {
            for (const int i : todo)
            {
                const Step& s = plan.steps[static_cast<std::size_t>(i)];
                if (cancel != nullptr && cancel->load())
                {
                    r.cancelled = true;
                    break;
                }
                set(i, State::working, std::string("copying to ") + shown(s.folder));
                std::string note, error;
                if (installStep(s, note, error))
                {
                    std::string line = shown(s.target) + " (" + s.to + ")";
                    if (!note.empty())
                        line += "; " + note;
                    set(i, State::done, line);
                    log(r, s.product + " " + partName(s.part) + " (" + actionWords(s.action) + "): " + line);
                }
                else
                {
                    set(i, State::failed, error);
                    log(r, s.product + " " + partName(s.part) + " failed: " + error);
                    r.error = s.product + "'s " + partName(s.part) + " was not installed: " + error + ".";
                    break;
                }
            }
            for (const int i : todo)
                if (r.states[static_cast<std::size_t>(i)] == State::waiting)
                    set(i, State::notRun, "not changed");
        }

        // Say what was and was not changed.
        std::vector<std::string> done, kept;
        for (int i = 0; i < n; ++i)
        {
            const Step& s = plan.steps[static_cast<std::size_t>(i)];
            const std::string what = s.product + "'s " + partName(s.part);
            if (r.states[static_cast<std::size_t>(i)] == State::done)
                done.push_back(what);
            else if (s.changes() && r.states[static_cast<std::size_t>(i)] != State::failed)
                kept.push_back(what);
        }
        if (!r.error.empty() || r.cancelled)
        {
            if (r.error.empty() && plan.admin() && done.empty())
                r.error = "Nothing was installed: the administrator's password was not given, so nothing was changed.";
            else
            {
                if (r.error.empty())
                    r.error = "Stopped before the next step.";
                if (done.empty())
                    r.error += " Nothing was changed.";
                else
                {
                    r.error += " Installed: " + joined(done) + ".";
                    if (!kept.empty())
                        r.error += " Left as they were: " + joined(kept) + ".";
                }
            }
        }
        bool auChanged = false;
        for (int i = 0; i < n; ++i)
            auChanged = auChanged || (plan.steps[static_cast<std::size_t>(i)].part == Part::au
                                      && r.states[static_cast<std::size_t>(i)] == State::done);
        if (auChanged)
        {
            r.registrarRestarted = system_.restartAuRegistrar();
            log(r, r.registrarRestarted ? "restarted the AU registrar (hosts see the new Audio Units)"
                                        : "could not restart the AU registrar: hosts see the new Audio Units after a restart");
        }
        log(r, r.ok() ? "done" : "stopped: " + r.error);
        return r;
    }

    // ---- uninstalling ------------------------------------------------------------------------------------------------

    Report Engine::uninstall(const std::vector<Copy>& copies, const ProgressFn& progress, const std::atomic<bool>* cancel)
    {
        const int n = static_cast<int>(copies.size());
        Report r;
        r.states.assign(static_cast<std::size_t>(n), State::waiting);
        r.lines.assign(static_cast<std::size_t>(n), std::string());
        const auto set = [&](int i, State s, const std::string& line) {
            r.states[static_cast<std::size_t>(i)] = s;
            r.lines[static_cast<std::size_t>(i)] = line;
            if (progress)
                progress(i, s, line);
        };
        log(r, "uninstall");

        // Each copy must be one this engine found: a part's own name in one of its known folders, a real folder.
        std::vector<int> mine, admin;
        for (int i = 0; i < n; ++i)
        {
            const Copy& c = copies[static_cast<std::size_t>(i)];
            const Product* pr = payload_.find(c.key);
            bool known = false;
            if (pr != nullptr && c.path.filename() == itemName(*pr, c.part))
                for (const auto& [dir, scope] : knownFolders(*pr, c.part))
                    known = known || c.path.parent_path().lexically_normal() == dir.lexically_normal();
            const std::string why = known ? unsafe(c.path.parent_path()) : shown(c.path) + " is not a part this installer knows";
            struct stat st{};
            if (!why.empty())
                set(i, State::skipped, "left as it was: " + why);
            else if (!lstatOf(c.path, st))
                set(i, State::skipped, "already gone");
            else if (S_ISLNK(st.st_mode))
                set(i, State::skipped, "left as it was: " + shown(c.path) + " is a link to " + linkTarget(c.path));
            else if (mayMove(c.path))
                mine.push_back(i);
            else
                admin.push_back(i);
        }
        for (const int i : mine)
        {
            const Copy& c = copies[static_cast<std::size_t>(i)];
            if (cancel != nullptr && cancel->load())
            {
                r.cancelled = true;
                break;
            }
            set(i, State::working, "moving to the Trash");
            std::string where, error;
            if (trash(c.path, where, error))
            {
                set(i, State::done, shown(c.path) + " is in " + where);
                log(r, "moved " + shown(c.path) + " to " + where);
            }
            else
            {
                set(i, State::failed, "it could not move " + shown(c.path) + " to the Trash (" + error + ")");
                r.error = "It could not move " + shown(c.path) + " to the Trash (" + error + ").";
                log(r, r.error);
                break;
            }
        }
        if (r.error.empty() && !r.cancelled && !admin.empty())
        {
            // The script numbers its steps by the copies' indices, and holds the copies this user may not move.
            r = runAdmin([&](const fs::path& status) { return adminUninstallScript(copies, status, admin); }, n, admin,
                         progress, std::move(r), "uninstall");
            for (const int i : admin)
                if (r.states[static_cast<std::size_t>(i)] == State::failed && r.error.empty())
                    r.error = shown(copies[static_cast<std::size_t>(i)].path) + " was not moved to the Trash: "
                              + r.lines[static_cast<std::size_t>(i)] + ".";
        }
        for (int i = 0; i < n; ++i)
            if (r.states[static_cast<std::size_t>(i)] == State::waiting)
                set(i, State::notRun, "not moved");
        if (r.cancelled && r.error.empty())
            r.error = r.count(State::done) == 0 ? "Nothing was moved to the Trash." : "Stopped before the next copy.";
        bool auGone = false;
        for (int i = 0; i < n; ++i)
            auGone = auGone || (copies[static_cast<std::size_t>(i)].part == Part::au
                                && r.states[static_cast<std::size_t>(i)] == State::done);
        if (auGone)
            r.registrarRestarted = system_.restartAuRegistrar();
        log(r, r.ok() ? "done" : "stopped: " + r.error);
        return r;
    }

    // ---- "All users": the script, run as root ------------------------------------------------------------------------

    namespace
    {
        // The script's head: the status file, its helpers.
        std::string scriptHead(const std::string& what, const std::string& status)
        {
            std::string s;
            s += "#!/bin/sh\n";
            s += "# " + what + " for all users: generated for this one run, then run once as root through macOS's\n";
            s += "# administrator prompt. Every path is quoted. Each step reports to the status file; the first failure\n";
            s += "# ends the script. Nothing is deleted: what is replaced or removed goes to the Trash.\n";
            s += "umask 022\n";
            s += "STATUS=" + shellQuote(status) + "\n";
            s += "say() { printf '%s\\n' \"$*\" >> \"$STATUS\"; }\n";
            s += "fail() { say \"STEP $1 fail $2\"; exit 1; }\n";
            // discard <item> <trash folder> <stem> <extension>: to the Trash under a name not taken there.
            s += "discard() {\n";
            s += "  D=\"$2/$3$4\"; n=2\n";
            s += "  while [ -e \"$D\" ] || [ -L \"$D\" ]; do D=\"$2/$3 $n$4\"; n=$((n + 1)); done\n";
            s += "  mv \"$1\" \"$D\"\n";
            s += "}\n";
            s += "if [ \"$(id -u)\" -eq 0 ]; then ROOT=1; else ROOT=0; fi\n";
            return s;
        }
    }

    std::string Engine::adminScript(const Plan& plan, const fs::path& status) const
    {
        std::string s = scriptHead("Installing", status.string());
        const std::string trashDir = shellQuote(trashFolder().string());
        for (std::size_t i = 0; i < plan.steps.size(); ++i)
        {
            const Step& st = plan.steps[i];
            if (!st.changes())
                continue;
            const std::string k = std::to_string(i);
            const std::string name = shellQuote(st.target.filename().string());
            const std::string stem = shellQuote(st.target.stem().string()), ext = shellQuote(st.target.extension().string());
            const std::string folder = shellQuote(st.folder.string()), target = shellQuote(st.target.string());
            const auto fail = [&](const std::string& why) { return "fail " + k + " " + shellQuote(why); };
            s += "\n# " + k + ": " + st.product + ", " + partName(st.part) + " (" + actionWords(st.action) + ")\n";
            s += "say \"STEP " + k + " start\"\n";
            s += "mkdir -p " + folder + " || " + fail("it could not make the folder " + shown(st.folder)) + "\n";
            // Every folder from the root down to the target's is a real folder, and the target no link.
            const fs::path base = st.folder.lexically_normal();
            fs::path anchor = places_.root, rel = base.lexically_relative(places_.root.lexically_normal());
            if (rel.empty() || *rel.begin() == "..")
            {
                anchor = places_.home;
                rel = base.lexically_relative(places_.home.lexically_normal());
            }
            fs::path walk = anchor;
            for (const fs::path& part : rel)
            {
                if (part == ".")
                    continue;
                walk /= part;
                s += "if [ -L " + shellQuote(walk.string()) + " ]; then "
                     + fail(shown(walk) + " is a link: this installer writes only into real folders") + "; fi\n";
            }
            s += "if [ -L " + target + " ]; then " + fail(shown(st.target) + " is a link: left as it is") + "; fi\n";
            s += "T=$(mktemp -d " + shellQuote((st.folder / ".installing-XXXXXX").string()) + ") || "
                 + fail("it may not write in " + shown(st.folder)) + "\n";
            s += "if ! ditto --noqtn " + shellQuote(st.source.string()) + " \"$T\"/" + name + "; then\n";
            s += "  if [ -e \"$T\"/" + name + " ]; then discard \"$T\"/" + name + " " + trashDir + " " + stem + " " + ext + "; fi\n";
            s += "  rmdir \"$T\"; " + fail("it could not copy it into " + shown(st.folder)) + "\n";
            s += "fi\n";
            s += "if [ \"$ROOT\" -eq 1 ] && ! chown -R root:wheel \"$T\"/" + name + "; then\n";
            s += "  discard \"$T\"/" + name + " " + trashDir + " " + stem + " " + ext + "; rmdir \"$T\"; "
                 + fail("it could not give the copy to root") + "\n";
            s += "fi\n";
            s += "if [ -e " + target + " ]; then\n";
            s += "  if ! mv " + target + " \"$T/old\"; then\n";
            s += "    discard \"$T\"/" + name + " " + trashDir + " " + stem + " " + ext + "; rmdir \"$T\"; "
                 + fail("it could not move the old copy aside") + "\n";
            s += "  fi\n";
            s += "  if ! mv \"$T\"/" + name + " " + target + "; then\n";
            s += "    mv \"$T/old\" " + target + "; discard \"$T\"/" + name + " " + trashDir + " " + stem + " " + ext
                 + "; rmdir \"$T\"; " + fail("it could not put the new copy in place") + "\n";
            s += "  fi\n";
            s += "  if discard \"$T/old\" " + trashDir + " " + stem + " " + ext + "; then say \"STEP " + k
                 + " note the old copy is in the Trash\"; else say \"STEP " + k + " note the old copy stays in $T\"; fi\n";
            s += "elif ! mv \"$T\"/" + name + " " + target + "; then\n";
            s += "  discard \"$T\"/" + name + " " + trashDir + " " + stem + " " + ext + "; rmdir \"$T\"; "
                 + fail("it could not put it in " + shown(st.folder)) + "\n";
            s += "fi\n";
            s += "rmdir \"$T\" 2>/dev/null\n";
            s += "say \"STEP " + k + " done\"\n";
        }
        return s;
    }

    std::string Engine::adminUninstallScript(const std::vector<Copy>& copies, const fs::path& status,
                                             const std::vector<int>& only) const
    {
        std::string s = scriptHead("Uninstalling", status.string());
        const std::string trashDir = shellQuote(trashFolder().string());
        for (std::size_t i = 0; i < copies.size(); ++i)
        {
            const Copy& c = copies[i];
            if (c.link || (!only.empty() && std::find(only.begin(), only.end(), static_cast<int>(i)) == only.end()))
                continue;                                // links stay; the copies not asked for are not its
            const std::string k = std::to_string(i), path = shellQuote(c.path.string());
            const auto fail = [&](const std::string& why) { return "fail " + k + " " + shellQuote(why); };
            s += "\n# " + k + ": " + c.path.filename().string() + "\n";
            s += "say \"STEP " + k + " start\"\n";
            s += "if [ -L " + path + " ]; then " + fail(shown(c.path) + " is a link: left as it is") + "; fi\n";
            s += "if [ -e " + path + " ]; then\n";
            s += "  discard " + path + " " + trashDir + " " + shellQuote(c.path.stem().string()) + " "
                 + shellQuote(c.path.extension().string()) + " || " + fail("it could not move it to the Trash") + "\n";
            s += "fi\n";
            s += "say \"STEP " + k + " done\"\n";
        }
        return s;
    }

    Report Engine::runAdmin(const std::function<std::string(const fs::path&)>& makeScript, int steps,
                            const std::vector<int>& indices, const ProgressFn& progress, Report report,
                            const std::string& what)
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

        // The script and its status file, in this user's temporary folder (0600).
        const fs::path tmp = fs::temp_directory_path();
        const auto makeFile = [&](const char* suffix) -> fs::path {
            std::string t = (tmp / ("installer-XXXXXX" + std::string(suffix))).string();
            std::vector<char> b(t.begin(), t.end());
            b.push_back('\0');
            const int fd = ::mkstemps(b.data(), static_cast<int>(std::strlen(suffix)));
            if (fd < 0)
                return {};
            ::close(fd);
            return fs::path(b.data());
        };
        const fs::path script = makeFile(".sh"), status = makeFile(".status");
        if (script.empty() || status.empty())
        {
            r.error = "It could not write its script for the administrator prompt (" + std::string(std::strerror(errno)) + ").";
            return r;
        }
        {
            std::ofstream out(script, std::ios::binary | std::ios::trunc);
            out << makeScript(status);
        }
        std::error_code ec;
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
        const System::Admin result = system_.runAsAdministrator(
            script, payload_.project + " Installer wants to " + (what == "install" ? "install" : "uninstall")
                        + " plugins for all users.",
            error);
        running.store(false);
        poller.join();
        poll();
        ::unlink(script.c_str());
        ::unlink(status.c_str());

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
