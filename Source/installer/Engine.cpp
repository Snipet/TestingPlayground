// Source/installer/Engine.cpp: see Engine.h. What is the same on both platforms (the payload, the plan, the runs, the
// notes); each platform's own ways are EnginePlatform.h's functions and the members EngineMac.cpp and EngineWin.cpp
// define (the places, the folders, versions, and "All users").
#include "installer/Engine.h"
#include "installer/EnginePlatform.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>

namespace playground::installer
{
    namespace pf = platform;

    namespace
    {
        // A path as JUCE takes it.
        juce::File juceFileOf(const fs::path& p)
        {
#if defined(_WIN32)
            return juce::File(juce::String(p.wstring().c_str()));
#else
            return juce::File(juce::String::fromUTF8(p.c_str()));
#endif
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
                const pf::Node n = pf::nodeOf(it->path());
                const char kind = n.link ? 'l' : n.dir ? 'd' : 'f';
                if (n.link)
                    it.disable_recursion_pending();      // a junction too: listed, not looked into
                out.emplace_back(fs::relative(it->path(), dir, ec).string(), kind);
            }
            std::sort(out.begin(), out.end());
            return out;
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
    }

    // ---- shared helpers (EnginePlatform.h) ---------------------------------------------------------------------------

    std::string pf::trimmed(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        std::size_t a = 0;
        while (a < s.size() && (s[a] == '\n' || s[a] == ' '))
            ++a;
        return s.substr(a);
    }

    // A name the payload gives that becomes part of a path: one plain component, never "..", no slash.
    bool pf::plainName(const std::string& s)
    {
        if (s.empty() || s == "." || s == ".." || s.size() > 128)
            return false;
        return std::all_of(s.begin(), s.end(), [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return std::isalnum(u) != 0 || c == ' ' || c == '-' || c == '_' || c == '.';
        });
    }

    std::string pf::fileText(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    const char* pf::actionWords(Action a)
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
#if defined(_WIN32)
            case Part::app:       return "";             // the Standalone's folder: <Name>\<Name>.exe and its licences
#else
            case Part::app:       return ".app";
#endif
            case Part::extension: return ".mcpb";
        }
        return "";
    }

    const char* trashWords()
    {
#if defined(_WIN32)
        return "the Recycle Bin";
#else
        return "the Trash";
#endif
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

    std::string psQuote(const std::string& s)
    {
        // PowerShell ends a single-quoted string at ' and at its typographic kin (U+2018, U+2019, U+201A, U+201B), each
        // written twice to stand for itself.
        std::string q = "'";
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(s[i]);
            if (c == '\'')
                q += "''";
            else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80
                     && static_cast<unsigned char>(s[i + 2]) >= 0x98 && static_cast<unsigned char>(s[i + 2]) <= 0x9B)
            {
                q += s.substr(i, 3);
                q += s.substr(i, 3);
                i += 2;
            }
            else
                q += s[i];
        }
        return q + "'";
    }

    // ---- places ------------------------------------------------------------------------------------------------------

    Places Places::under(const fs::path& testRoot)
    {
        Places p;
        p.home = testRoot / "home";
        p.root = testRoot / "system";
        p.trash = testRoot / "Trash";
        p.log = testRoot / "install.log";
        p.scratch = testRoot / "tmp";
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
        const juce::var v = juce::JSON::parse(juce::String::fromUTF8(pf::fileText(manifest).c_str()));
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
            if (!pf::plainName(pr.key) || !pf::plainName(pr.name) || !pf::plainName(pr.company))
            {
                p.error = "This installer's payload names a product it cannot use (" + manifest.string() + ").";
                p.products.clear();
                return p;
            }
            for (const Part part : kAllParts)
            {
                Offer off;
                off.part = part;
                off.source = folder / (pr.name + partExtension(part));
                const pf::Node n = pf::nodeOf(off.source);
                off.present = (part == Part::extension ? n.file : n.dir)
                              && std::find(std::begin(kParts), std::end(kParts), part) != std::end(kParts);
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

    std::string Engine::extensionVersion(const fs::path& mcpb)
    {
        juce::ZipFile zip(juceFileOf(mcpb));
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
        const pf::Node na = pf::nodeOf(a), nb = pf::nodeOf(b);
        if (!na.exists || !nb.exists || na.link || nb.link)
            return false;
        if (na.file && nb.file)
            return sameFile(a, b);
        if (!na.dir || !nb.dir)
            return false;
        if (const std::string ha = pf::cdhash(a), hb = pf::cdhash(b); !ha.empty() && !hb.empty())
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
            const pf::Node node = pf::nodeOf(real);
            if (!node.exists)
            {
                if (node.missing)
                    return {};                           // the rest is made when it is installed
                return "it cannot look into " + shown(said) + " (" + pf::errorText(node.error) + ")";
            }
            if (node.link)
                return shown(said) + " is a link to " + pf::linkTarget(real)
                       + ": this installer writes only into real folders, and left it as it is";
            if (!node.dir)
                return shown(said) + " is a file where a folder should be";
        }
        return {};
    }

    bool Engine::mayMove(const fs::path& path) const
    {
        return pf::mayMove(path);
    }

    std::vector<Copy> Engine::installed(const Product& product, Part part) const
    {
        std::vector<Copy> out;
        for (const auto& [dir, scope] : knownFolders(product, part))
        {
            if (!unsafe(dir).empty())
                continue;                                // a linked folder is never looked into
            const fs::path p = dir / itemName(product, part);
            const pf::Node node = pf::nodeOf(p);
            if (!node.exists)
                continue;
            Copy c;
            c.key = product.key;
            c.part = part;
            c.scope = scope;
            c.path = p;
            c.link = node.link;
            c.mine = node.mine;
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
                const pf::Node target = pf::nodeOf(s.target);
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
                else if (scope == Scope::user && pf::nodeOf(s.folder).exists && !pf::writable(s.folder))
                {
                    s.action = Action::blocked;
                    s.why = shown(s.folder) + " may not be written to (its permissions): choose All users, or change them";
                }
                else if (target.exists)
                {
                    const bool kindOk = part == Part::extension ? target.file : target.dir;
                    if (target.link)
                    {
                        s.action = Action::blocked;
                        s.why = shown(s.target) + " is a link to " + pf::linkTarget(s.target) + ": left as it is";
                    }
                    else if (!kindOk)
                    {
                        s.action = Action::blocked;
                        s.why = shown(s.target) + " is not " + (part == Part::extension ? "a file" : pf::kFolderKind)
                                + ": left as it is";
                    }
                    else if (scope == Scope::user && !mayMove(s.target))
                    {
                        s.action = Action::blocked;
                        s.from = part == Part::extension ? extensionVersion(s.target) : bundleVersion(s.target);
                        s.why = !target.mine
                                    ? shown(s.target) + " belongs to " + target.owner
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

        // The notes: what runs that uses what changes, and copies the other scope has too (one note a product).
        std::vector<std::string> apps;
        bool plugins = false;
        for (const Step& s : plan.steps)
            if (s.changes())
            {
                plugins = plugins || s.part == Part::au || s.part == Part::vst3;
                if (s.part == Part::app)
                    apps.push_back(s.product);
            }
        plan.notes = runningNotes(apps, plugins, false);
        for (const Product& pr : payload_.products)
        {
            std::vector<std::string> mine, theirs;       // the parts with a copy in this scope's folders, the other's
            for (const Step& s : plan.steps)
            {
                if (s.key != pr.key || s.part == Part::extension)
                    continue;
                for (const Copy& c : installed(pr, s.part))
                    if (c.path.parent_path() != s.folder && !c.link)
                    {
                        (c.scope == Scope::system ? theirs : mine).push_back(partName(s.part));
                        break;
                    }
            }
            const auto note = [&](const std::vector<std::string>& parts, const char* where) {
                if (!parts.empty())
                    plan.notes.push_back(pr.name + " is also installed " + where + " (" + joined(parts)
                                         + "): uninstall one of the two.");
            };
            note(theirs, "for all users");
            note(mine, "just for you");
        }
        return plan;
    }

    std::vector<std::string> Engine::runningNotes(const std::vector<std::string>& apps, bool plugins, bool removing) const
    {
        std::vector<std::string> notes;
        const std::vector<fs::path> running = system_.runningApps();
        for (const std::string& app : apps)
            for (const fs::path& a : running)
                if (pf::sameName(a.filename(), pf::appFile(app)))
                {
                    notes.push_back(app + (removing ? pf::kOpenWhileRemoved
                                                    : " is open: quit it first, or it plays the old version until it "
                                                      "opens again."));
                    break;
                }
        std::vector<std::string> hosts;
        for (const fs::path& a : running)
        {
            const std::string name = pf::hostName(a);
            if (!name.empty() && std::find(hosts.begin(), hosts.end(), name) == hosts.end())
                hosts.push_back(name);
        }
        if (plugins && !hosts.empty())
            notes.push_back(joined(hosts) + (hosts.size() == 1 ? " is" : " are")
                            + (removing ? " open: restart your DAW, and it lets go of the plugins."
                                        : " open: restart your DAW after installing, and it loads the new plugins."));
        return notes;
    }

    std::vector<std::string> Engine::uninstallNotes(const std::vector<Copy>& copies) const
    {
        std::vector<std::string> apps;
        bool plugins = false;
        for (const Copy& c : copies)
        {
            if (c.link)
                continue;
            plugins = plugins || c.part == Part::au || c.part == Part::vst3;
            if (const Product* pr = payload_.find(c.key); pr != nullptr && c.part == Part::app
                && std::find(apps.begin(), apps.end(), pr->name) == apps.end())
                apps.push_back(pr->name);
        }
        return runningNotes(apps, plugins, true);
    }

    // ---- the trash and the log ---------------------------------------------------------------------------------------

    fs::path Engine::trashName(const fs::path& item) const
    {
        const fs::path dir = places_.trash;
        const std::string stem = stemOf(item), ext = item.extension().string();
        fs::path p = dir / item.filename();
        for (int n = 2; pf::nodeOf(p).exists; ++n)
            p = dir / (stem + " " + std::to_string(n) + ext);
        return p;
    }

    bool Engine::trash(const fs::path& path, std::string& where, std::string& error)
    {
        if (places_.trash.empty())
        {
            where = trashWords();
            return system_.moveToTrash(path, error);
        }
        std::error_code ec;
        fs::create_directories(places_.trash, ec);
        const fs::path dest = trashName(path);
        int err = 0;
        if (!pf::renameItem(path, dest, err))
        {
            error = pf::errorText(err);
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
        out << pf::now() << "  " << line << "\n";
    }

    // ---- installing --------------------------------------------------------------------------------------------------

    std::string Engine::sweep(const fs::path& folder, const std::string& name)
    {
        // A staging folder is .installing-XXXXXX (six letters and digits); it is an earlier run's only when it holds
        // nothing but this part (the copy being made) and `old` (the copy it replaced), neither a link, and it is this
        // user's.
        constexpr std::size_t kStageName = std::char_traits<char>::length(".installing-XXXXXX");
        std::vector<std::pair<fs::path, std::vector<fs::path>>> stale;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::path stage = it->path();
            const std::string n = stage.filename().string();
            if (n.size() != kStageName || n.rfind(".installing-", 0) != 0)
                continue;
            const pf::Node node = pf::nodeOf(stage);
            if (!node.dir || !node.mine)
                continue;
            std::vector<fs::path> inside;
            bool ours = true;
            std::error_code ei;
            for (fs::directory_iterator jt(stage, ei), jend; !ei && jt != jend; jt.increment(ei))
            {
                const std::string e = jt->path().filename().string();
                ours = ours && (e == name || e == "old") && !pf::nodeOf(jt->path()).link;
                inside.push_back(jt->path());
            }
            if (ours && !ei)
                stale.emplace_back(stage, std::move(inside));
        }
        int moved = 0;
        for (auto& [stage, inside] : stale)
        {
            for (fs::path item : inside)
            {
                if (item.filename() == "old")
                {
                    // Named as what it is, in the Trash: "<name> (old)".
                    const fs::path named = stage / (fs::path(name).stem().string() + " (old)" + fs::path(name).extension().string());
                    int err = 0;
                    if (pf::renameNew(item, named, err))
                        item = named;
                }
                std::string where, error;
                moved += trash(item, where, error) ? 1 : 0;
            }
            pf::removeEmptyFolder(stage);
        }
        if (moved == 0)
            return {};
        return std::string(moved == 1 ? "a copy" : "copies") + " an earlier run left in " + shown(folder)
               + (moved == 1 ? " is" : " are") + " in " + trashWords();
    }

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
        const pf::Node target = pf::nodeOf(step.target);
        const bool replacing = target.exists;
        if (replacing && target.link)
        {
            error = shown(step.target) + " is a link to " + pf::linkTarget(step.target) + ": left as it is";
            return false;
        }
        const std::string swept = sweep(step.folder, name);

        // The staging folder beside the target: the copy is made there, then renamed into place.
        int err = 0;
        const fs::path stage = pf::makeStage(step.folder, err);
        if (stage.empty())
        {
            error = "it may not write in " + shown(step.folder) + " (" + pf::errorText(err) + ")";
            return false;
        }
        const fs::path staged = stage / name;
        // A copy that will not be used: to the Trash (never deleted), then the staging folder, empty, removed. What
        // could not go is said.
        const auto discardStage = [&]() -> std::string {
            std::string where, terr;
            if (pf::nodeOf(staged).exists)
                trash(staged, where, terr);
            if (pf::removeEmptyFolder(stage) || !pf::nodeOf(stage).exists)
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
            // macOS: one atomic exchange; Windows: three renames, the old copy put back if the new one cannot go in
            // (EnginePlatform.h). Either way a failure leaves the old copy whole where it was.
            if (!pf::swapItems(staged, step.target, err))
            {
                error = "it could not put the new copy in place of " + shown(step.target) + " (" + pf::errorText(err) + ")";
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
        else if (!pf::renameNew(staged, step.target, err))
        {
            error = "it could not put it in " + shown(step.folder) + " (" + pf::errorText(err) + ")";
            error += discardStage();
            return false;
        }
        pf::removeEmptyFolder(stage);                    // empty unless the old copy could not go to the Trash
        if (!swept.empty())
            line += (line.empty() ? "" : "; ") + swept;
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
            std::vector<fs::path> targets;
            for (const Step& s : plan.steps)
                targets.push_back(s.target);
            r = runAdmin([&](const fs::path& status) { return adminScript(plan, status); }, n, todo, progress,
                         std::move(r), "install", targets);
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
                    log(r, s.product + " " + partName(s.part) + " (" + pf::actionWords(s.action) + "): " + line);
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
                r.error = std::string("Nothing was installed: ") + pf::kNoAdmin + ", so nothing was changed.";
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
            const pf::Node node = pf::nodeOf(c.path);
            if (!why.empty())
                set(i, State::skipped, "left as it was: " + why);
            else if (!node.exists)
                set(i, State::skipped, "already gone");
            else if (node.link)
                set(i, State::skipped, "left as it was: " + shown(c.path) + " is a link to " + pf::linkTarget(c.path));
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
            set(i, State::working, std::string("moving to ") + trashWords());
            std::string where, error;
            if (trash(c.path, where, error))
            {
                set(i, State::done, shown(c.path) + " is in " + where);
                log(r, "moved " + shown(c.path) + " to " + where);
                if (const std::string swept = sweep(c.path.parent_path(), c.path.filename().string()); !swept.empty())
                    log(r, swept);
            }
            else
            {
                set(i, State::failed, "it could not move " + shown(c.path) + " to " + trashWords() + " (" + error + ")");
                r.error = "It could not move " + shown(c.path) + " to " + trashWords() + " (" + error + ").";
                log(r, r.error);
                break;
            }
        }
        if (r.error.empty() && !r.cancelled && !admin.empty())
        {
            // The script numbers its steps by the copies' indices, and holds the copies this user may not move.
            std::vector<fs::path> targets;
            for (const Copy& c : copies)
                targets.push_back(c.path);
            r = runAdmin([&](const fs::path& status) { return adminUninstallScript(copies, status, admin); }, n, admin,
                         progress, std::move(r), "uninstall", targets);
            for (const int i : admin)
                if (r.states[static_cast<std::size_t>(i)] == State::failed && r.error.empty())
                    r.error = shown(copies[static_cast<std::size_t>(i)].path) + " was not moved to " + trashWords() + ": "
                              + r.lines[static_cast<std::size_t>(i)] + ".";
        }
        for (int i = 0; i < n; ++i)
            if (r.states[static_cast<std::size_t>(i)] == State::waiting)
                set(i, State::notRun, "not moved");
        if (r.cancelled && r.error.empty())
            r.error = r.count(State::done) == 0 ? std::string("Nothing was moved to ") + trashWords() + "."
                                                : std::string("Stopped before the next copy.");
        bool auGone = false;
        for (int i = 0; i < n; ++i)
            auGone = auGone || (copies[static_cast<std::size_t>(i)].part == Part::au
                                && r.states[static_cast<std::size_t>(i)] == State::done);
        if (auGone)
            r.registrarRestarted = system_.restartAuRegistrar();
        log(r, r.ok() ? "done" : "stopped: " + r.error);
        return r;
    }
}
