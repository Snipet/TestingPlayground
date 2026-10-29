// Source/sandbox/Sandbox.cpp: see Sandbox.h.
#include "sandbox/Sandbox.h"

#include "sandbox/SandboxProtocol.h"
#include "sandbox/View.h"

#include "editor/GuiLayout.h"
#include "editor/Items.h"
#include "editor/Picture.h"
#include "fminus/GuiSpec.h"

#include "Product.h"

#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playground::sandbox
{
    namespace
    {
        constexpr int kReportLines = 40;         // about as many lines as a report takes, unless detail

        juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
        {
            auto* o = new juce::DynamicObject();
            for (const auto& [k, v] : props)
                o->setProperty(k, v);
            return juce::var(o);
        }

        juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        std::string text(const juce::var& v) { return v.toString().toStdString(); }

        juce::var fail(const std::string& why) { return object({ { "ok", false }, { "text", str(why) } }); }

        bool isGui(const std::string& n) { return isGuiFile(n); }
        bool endsWith(const std::string& s, const char* e)
        {
            const std::size_t n = std::strlen(e);
            return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
        }

        int lineCount(const std::string& t)
        {
            if (t.empty())
                return 0;
            return static_cast<int>(std::count(t.begin(), t.end(), '\n')) + (t.back() == '\n' ? 0 : 1);
        }

        std::vector<std::string> linesOf(const std::string& t)
        {
            std::vector<std::string> out;
            std::istringstream in(t);
            std::string l;
            while (std::getline(in, l))
                out.push_back(l);
            return out;
        }

        // Lines added and removed between two texts, as a multiset of lines (cheap, and what a summary needs).
        std::pair<int, int> lineChanges(const std::string& before, const std::string& after)
        {
            std::map<std::string, int> count;
            for (const std::string& l : linesOf(before))
                ++count[l];
            int added = 0;
            for (const std::string& l : linesOf(after))
            {
                auto it = count.find(l);
                if (it != count.end() && it->second > 0)
                    --it->second;
                else
                    ++added;
            }
            int removed = 0;
            for (const auto& [l, n] : count)
                removed += n;
            return { added, removed };
        }

        float measure(const std::string& t, const funkgui::TextStyle& st)
        {
            return funkgui::text::width(funkgui::FontService::get().atlas(), t.c_str(), st);
        }

        std::string number(float v) { return std::to_string(static_cast<long>(std::lround(v))); }

        std::string rectText(const funkgui::Rect& r)
        {
            return number(r.x) + "," + number(r.y) + " " + number(r.w) + "x" + number(r.h);
        }

        const char* displayWord(DisplayKind k)
        {
            switch (k)
            {
                case DisplayKind::meter:    return "meter";
                case DisplayKind::vu:       return "vu";
                case DisplayKind::led:      return "led";
                case DisplayKind::number:   return "number";
                case DisplayKind::scope:    return "scope";
                case DisplayKind::spectrum: return "spectrum";
            }
            return "display";
        }

        bool isContainer(GuiKind k)
        {
            return k == GuiKind::row || k == GuiKind::column || k == GuiKind::grid || k == GuiKind::section || k == GuiKind::box
                || k == GuiKind::tabs || k == GuiKind::page;
        }

        bool isControl(GuiKind k)
        {
            return k == GuiKind::knob || k == GuiKind::slider || k == GuiKind::toggle || k == GuiKind::buttons || k == GuiKind::menu;
        }

        // An element as the report names it: its kind and what it shows.
        std::string nameOf(const GuiNode& n, const PanelSpec& spec, const Controls& controls)
        {
            const auto dial = [&](int slot) {
                return slot >= 0 && slot < kMaxControls && controls[static_cast<std::size_t>(slot)].used
                         ? std::string(controls[static_cast<std::size_t>(slot)].name)
                         : std::string("?");
            };
            switch (n.kind)
            {
                case GuiKind::row:      return n.wrap ? "row (wraps)" : "row";
                case GuiKind::column:   return "column";
                case GuiKind::grid:     return "grid of " + std::to_string(n.cols);
                case GuiKind::section:  return "section \"" + n.text + "\"";
                case GuiKind::box:      return "box";
                case GuiKind::tabs:     return "tabs";
                case GuiKind::page:     return "page \"" + n.text + "\"";
                case GuiKind::spacer:   return "spacer";
                case GuiKind::divider:  return "divider";
                case GuiKind::label:    return "label \"" + n.text + "\"";
                case GuiKind::knob:     return "knob " + dial(n.slot);
                case GuiKind::slider:   return "slider " + dial(n.slot);
                case GuiKind::toggle:   return "switch " + dial(n.slot);
                case GuiKind::buttons:  return "buttons " + dial(n.slot);
                case GuiKind::menu:     return "menu " + dial(n.slot);
                case GuiKind::xy:       return "xy " + dial(n.slot) + " " + dial(n.slot2);
                case GuiKind::envelope: return "envelope " + dial(n.env[0]) + " " + dial(n.env[1]) + " " + dial(n.env[2]) + " " + dial(n.env[3]);
                case GuiKind::display:
                    if (n.display >= 0 && static_cast<std::size_t>(n.display) < spec.displays.size())
                    {
                        const PanelDisplay& d = spec.displays[static_cast<std::size_t>(n.display)];
                        return std::string(displayWord(d.kind)) + " " + d.source;
                    }
                    return "display";
                case GuiKind::keyboard: return "keyboard from " + std::to_string(n.from) + ", " + std::to_string(n.keys) + " keys";
                case GuiKind::shader:   return "shader \"" + n.file + "\"";
            }
            return "element";
        }

        // Where the plugin keeps shaderc: <ENV_PREFIX>SHADERC (the probes'), beside this helper, else in an installed
        // plugin's bundle (Contents/Resources/shaderc: the AU, the VST3 or the app, the user's or the system's).
        std::string findShaderc()
        {
            const std::string var = std::string(product::kEnvPrefix) + "SHADERC";
            if (const char* e = std::getenv(var.c_str()); e != nullptr && *e != '\0' && access(e, X_OK) == 0)
                return e;
            std::vector<std::string> places;
            const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
            places.push_back(exe.getSiblingFile("shaderc").getFullPathName().toStdString());
            const std::string name = product::kName;
            const char* home = std::getenv("HOME");
            std::vector<std::string> roots;
            if (home != nullptr && *home != '\0')
                roots.push_back(std::string(home) + "/Library/Audio/Plug-Ins");
            roots.push_back("/Library/Audio/Plug-Ins");
            for (const std::string& r : roots)
            {
                places.push_back(r + "/Components/" + name + ".component/Contents/Resources/shaderc");
                places.push_back(r + "/VST3/" + name + ".vst3/Contents/Resources/shaderc");
            }
            places.push_back("/Applications/" + name + ".app/Contents/Resources/shaderc");
            if (home != nullptr && *home != '\0')
                places.push_back(std::string(home) + "/Applications/" + name + ".app/Contents/Resources/shaderc");
            for (const std::string& p : places)
                if (access(p.c_str(), X_OK) == 0)
                    return p;
            return {};
        }

        // `img` scaled down to tw x th, each pixel the mean of those it covers.
        funkgui::Image scaled(const funkgui::Image& img, int tw, int th)
        {
            funkgui::Image out;
            out.w = tw;
            out.h = th;
            out.rgba.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4, 0);
            const double sx = static_cast<double>(img.w) / tw, sy = static_cast<double>(img.h) / th;
            for (int y = 0; y < th; ++y)
            {
                const int y0 = static_cast<int>(std::floor(y * sy)), y1 = std::max(y0 + 1, std::min(img.h, static_cast<int>(std::floor((y + 1) * sy))));
                for (int x = 0; x < tw; ++x)
                {
                    const int x0 = static_cast<int>(std::floor(x * sx)), x1 = std::max(x0 + 1, std::min(img.w, static_cast<int>(std::floor((x + 1) * sx))));
                    double acc[4] = {};
                    for (int yy = y0; yy < y1; ++yy)
                        for (int xx = x0; xx < x1; ++xx)
                        {
                            const uint8_t* p = img.rgba.data() + (static_cast<std::size_t>(yy) * static_cast<std::size_t>(img.w) + static_cast<std::size_t>(xx)) * 4;
                            for (int k = 0; k < 4; ++k)
                                acc[k] += p[k];
                        }
                    const double n = static_cast<double>((y1 - y0) * (x1 - x0));
                    uint8_t* q = out.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(tw) + static_cast<std::size_t>(x)) * 4;
                    for (int k = 0; k < 4; ++k)
                        q[k] = static_cast<uint8_t>(std::clamp(std::lround(acc[k] / n), 0L, 255L));
                }
            }
            return out;
        }
    }

    Sandbox::Sandbox(bool instrument) : instrument_(instrument), areaW_(kDefaultWidth), areaH_(kDefaultHeight)
    {
        tools_.shaderc = findShaderc();
        tools_.prelude = funkgui::shaders::fragmentPrelude();
        tools_.varyingDef = funkgui::shaders::varyingDef();
        tools_.bgfxShaderSh = funkgui::shaders::bgfxShaderSh();
#if defined(__APPLE__)
        tools_.platform = "osx";
        tools_.profile = "metal";
#else
        tools_.platform = "linux";
        tools_.profile = "spirv";
#endif
    }

    juce::var Sandbox::handle(const juce::var& r)
    {
        const std::string op = text(r["op"]);
        // `load` first: the sandbox as the server kept it (a helper started again, or another design asked for by its
        // number), made here before the request itself; the reply then says what is open (its state), whatever came of
        // the request, so that the server's copy stays what this helper holds.
        bool loaded = false;
        if (r["load"].isObject())
        {
            juce::var l = juce::JSON::parse(juce::JSON::toString(r["load"]));
            if (juce::DynamicObject* o = l.getDynamicObject())
            {
                o->setProperty("op", "open");
                o->setProperty("from", "files");
                o->setProperty("restore", true);
            }
            const juce::var made = open(l);
            if (!static_cast<bool>(made["ok"]))
                return fail("The sandbox could not be made again: " + text(made["text"]));
            loaded = true;
        }
        juce::var out;
        if (op == "open")
            out = open(r);
        else if (op == "state")
            out = open_ ? state() : fail("Nothing is open in the sandbox.");
        else if (!open_)
            out = fail("Nothing is open in the sandbox: open it first (a blank panel, the open plugin's, a file, or a .gui "
                       "as text).");
        else if (r.hasProperty("expect") && static_cast<int>(r["expect"]) != number_)
            out = fail("Sandbox #" + std::to_string(static_cast<int>(r["expect"])) + " is not the one open here: nothing was done.");
        else if (op == "edit")
            out = edit(r);
        else if (op == "render")
            out = render(r);
        else
            out = fail("The sandbox has no request '" + op + "'.");
        if (loaded && open_)
            if (juce::DynamicObject* o = out.getDynamicObject(); o != nullptr && !o->hasProperty("state"))
                o->setProperty("state", state());
        return out;
    }

    bool Sandbox::readPath(const std::string& given, Sources& out, std::string& why) const
    {
        std::string path = juce::String(str(given)).trim().toStdString();
        if (path.rfind("~/", 0) == 0 || path == "~")
            if (const char* home = std::getenv("HOME"))
                path = std::string(home) + path.substr(1);
        if (path.empty() || path[0] != '/')
        {
            why = "path names a file the user named, whole (/Users/... or ~/...): '" + given + "' is not";
            return false;
        }
        struct stat st{};
        if (stat(path.c_str(), &st) != 0)
        {
            why = "there is no file or folder " + given;
            return false;
        }
        const auto readOne = [&](const std::string& file, const std::string& name) {
            struct stat fs{};
            if (stat(file.c_str(), &fs) != 0 || !S_ISREG(fs.st_mode))
                return false;
            if (static_cast<std::size_t>(fs.st_size) > kMaxFileBytes)
            {
                why = name + " is " + std::to_string(fs.st_size / 1024) + " KB: the sandbox reads files of " + std::to_string(kMaxFileBytes / 1024)
                    + " KB at most";
                return false;
            }
            juce::MemoryBlock mb;
            if (!juce::File(str(file)).loadFileAsData(mb))
            {
                why = "could not read " + name;
                return false;
            }
            out.push_back({ name, std::string(static_cast<const char*>(mb.getData()), mb.getSize()) });
            return true;
        };
        if (S_ISREG(st.st_mode))
        {
            if (!endsWith(path, ".gui"))
            {
                why = given + " is not a .gui file: name a .gui file or a program's folder (its .fm, .gui and .sc files)";
                return false;
            }
            return readOne(path, juce::File(str(path)).getFileName().toStdString());
        }
        if (!S_ISDIR(st.st_mode))
        {
            why = given + " is neither a file nor a folder";
            return false;
        }
        std::vector<std::string> names;
        if (DIR* d = opendir(path.c_str()))
        {
            while (const dirent* e = readdir(d))
            {
                const std::string n = e->d_name;
                if (!n.empty() && n[0] != '.' && (endsWith(n, ".fm") || endsWith(n, ".gui") || endsWith(n, ".sc")))
                    names.push_back(n);
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        if (names.empty())
        {
            why = "the folder " + given + " has no .fm, .gui or .sc file";
            return false;
        }
        if (names.size() > kMaxFiles)
        {
            why = "the folder " + given + " has " + std::to_string(names.size()) + " program files: the sandbox reads "
                + std::to_string(kMaxFiles) + " at most (a program's own folder)";
            return false;
        }
        std::size_t total = 0;
        for (const std::string& n : names)
        {
            if (!readOne(path + "/" + n, n))
                return false;
            total += out.back().text.size();
            if (total > kMaxTotalBytes)
            {
                why = "the folder's files come to more than " + std::to_string(kMaxTotalBytes / 1024) + " KB";
                return false;
            }
        }
        return true;
    }

    juce::var Sandbox::open(const juce::var& r)
    {
        std::string from = text(r["from"]);
        if (from.empty())
            from = r.hasProperty("text") ? "text" : "blank";
        std::string notes;
        const std::vector<Dial> given = parseDials(r["dials"], notes);
        Sources files;
        std::string origin = text(r["origin"]);
        if (from == "blank")
        {
            if (origin.empty())
                origin = "a blank panel";
        }
        else if (from == "text")
        {
            const std::string t = text(r["text"]);
            if (t.size() > kMaxFileBytes)
                return fail("The .gui text is " + std::to_string(t.size() / 1024) + " KB: " + std::to_string(kMaxFileBytes / 1024) + " KB at most.");
            files.push_back({ "panel.gui", t });
            if (origin.empty())
                origin = "your text";
        }
        else if (from == "file")
        {
            std::string why;
            if (!readPath(text(r["path"]), files, why))
                return fail("Could not open it: " + why + ".");
            if (origin.empty())
                origin = text(r["path"]);
        }
        else if (from == "files")
        {
            if (const juce::Array<juce::var>* a = r["files"].getArray())
                for (const juce::var& f : *a)
                    files.push_back({ text(f["name"]), text(f["text"]) });
            if (origin.empty())
                origin = "the plugin's program";
        }
        else
            return fail("from is blank, instance, file or text, not '" + from + "'.");

        // The program's declarations, then the dials given.
        const Declared declared = readProgram(files, instrument_);
        std::vector<Dial> dials = merge(declared.dials, given);
        int gui = -1;
        for (std::size_t k = 0; k < files.size() && gui < 0; ++k)
            if (isGui(files[k].name))
                gui = static_cast<int>(k);
        std::string made;
        if (gui < 0)
        {
            files.push_back({ "panel.gui", blankGui(dials, instrument_) });
            gui = static_cast<int>(files.size()) - 1;
            if (from != "blank")
                made = "It has no .gui file: panel.gui is a blank panel of its dials, to start from.";
        }
        open_ = true;
        number_ = r.hasProperty("number") ? static_cast<int>(r["number"]) : number_ + 1;
        // A sandbox made again keeps its area (its last render's); a new one starts at the PANEL view's.
        areaW_ = kDefaultWidth;
        areaH_ = kDefaultHeight;
        if (r["area"].isArray() && r["area"].size() == 2)
        {
            areaW_ = std::clamp(static_cast<int>(r["area"][0]), kMinSide, kMaxSide);
            areaH_ = std::clamp(static_cast<int>(r["area"][1]), kMinSide, kMaxSide);
        }
        origin_ = origin;
        files_ = std::move(files);
        gui_ = gui;
        declared_ = declared;
        dials_ = std::move(dials);
        dialNotes_ = notes;
        rebuild();
        if (static_cast<bool>(r["restore"]))
            return object({ { "ok", true }, { "text", "restored" }, { "state", state() } });
        const bool detail = text(r["detail"]) == "high";
        std::string out = summary() + (made.empty() ? "" : made + "\n") + problems(detail) + report(detail);
        return object({ { "ok", true }, { "text", str(out) }, { "state", state() } });
    }

    void Sandbox::rebuild()
    {
        fields_.clear();
        mockedFields_.clear();
        if (!declared_.module.empty())
            fields_ = declared_.fields;
        else
            for (const std::string& f : fieldsNamed(files_[static_cast<std::size_t>(gui_)].text))
            {
                fields_.push_back({ f, "float" });
                mockedFields_.push_back(f);
            }
        controls_ = controlsOf(dials_);
        std::vector<std::string> names;
        for (const SourceFile& f : files_)
            names.push_back(f.name);
        notes_.clear();
        spec_ = readGuiSpec(files_[static_cast<std::size_t>(gui_)], controls_, fields_, instrument_, names, notes_);
        for (PanelShader& sh : spec_.shaders)
        {
            const auto f = std::find_if(files_.begin(), files_.end(), [&](const SourceFile& s) { return s.name == sh.file; });
            if (f == files_.end())
                continue;                                // GuiSpec said so
            if (!tools_.usable())
            {
                notes_ += sh.file + ": note: not built: no shader compiler found (shaderc comes in the plugin: install the "
                                    "plugin, and the sandbox finds it there); it shows as a placeholder\n";
                continue;
            }
            const ShaderBuild b = shaders_.build(sh.file, f->text, sh.inputs, tools_);
            if (b.ok())
            {
                sh.binary = b.binary;
                sh.id = shaderId(sh.binary);
            }
            else if (notes_.find(b.error) == std::string::npos)
                notes_ += b.error;
        }
    }

    const Dial* Sandbox::dialNamed(const std::string& name, int* slot) const
    {
        for (std::size_t k = 0; k < dials_.size() && k < static_cast<std::size_t>(kMaxControls); ++k)
            if (juce::String(str(dials_[k].name)).equalsIgnoreCase(str(name)))
            {
                if (slot != nullptr)
                    *slot = static_cast<int>(k);
                return &dials_[k];
            }
        return nullptr;
    }

    std::string Sandbox::summary() const
    {
        const SourceFile& g = files_[static_cast<std::size_t>(gui_)];
        int controls = 0, displays = 0, pages = 0;
        for (const GuiNode& n : spec_.gui.nodes)
        {
            controls += isControl(n.kind) || n.kind == GuiKind::xy ? 1 : 0;
            displays += n.kind == GuiKind::display ? 1 : 0;
            pages += n.kind == GuiKind::page ? 1 : 0;
        }
        const auto count = [](std::size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
        std::string out = "Sandbox #" + std::to_string(number_) + ", from " + origin_ + ": " + g.name + ", "
                        + count(static_cast<std::size_t>(lineCount(g.text)), "line", "lines") + ", "
                        + count(spec_.gui.nodes.size() > 0 ? spec_.gui.nodes.size() - 1 : 0, "element", "elements") + " ("
                        + count(static_cast<std::size_t>(controls), "control", "controls") + ", "
                        + count(static_cast<std::size_t>(displays), "display", "displays")
                        + (pages > 0 ? ", " + count(static_cast<std::size_t>(pages), "page", "pages") : std::string()) + ").\n";
        // The dials, and where they came from.
        int fromProgram = 0;
        std::string yours;
        for (const Dial& d : dials_)
        {
            if (d.given)
                yours += (yours.empty() ? "" : ", ") + d.name;
            else
                ++fromProgram;
        }
        if (dials_.empty())
            out += "Dials: none (give dials, or open the plugin's program or a program's folder, for its own).\n";
        else
        {
            out += "Dials: " + std::to_string(dials_.size());
            if (fromProgram > 0)
                out += ", " + std::to_string(fromProgram) + " from the program's declarations (module " + declared_.module + " in "
                     + declared_.file + ")";
            if (!yours.empty())
                out += std::string(fromProgram > 0 ? "; yours: " : ", yours: ") + yours;
            out += ".";
            if (declared_.beyond > 0)
                out += " " + std::to_string(declared_.beyond) + " more parameters past " + std::to_string(kMaxControls)
                     + " (the plugin leaves them at their defaults).";
            if (dials_.size() > static_cast<std::size_t>(kMaxControls))
                out += " Only the first " + std::to_string(kMaxControls) + " are dials.";
            out += "\n";
        }
        if (!declared_.module.empty())
        {
            // Those GuiSpec takes (a float, int or bool field: fieldOk), not a module's instance, a pointer or an array.
            std::string f;
            for (const PanelField& x : declared_.fields)
                if (x.type == "float" || x.type == "int" || x.type == "bool")
                    f += (f.empty() ? "" : ", ") + x.name + " (" + x.type + ")";
            if (!f.empty())
                out += "Fields a display may show: " + f + ".\n";
        }
        else if (!mockedFields_.empty())
        {
            std::string f;
            for (const std::string& x : mockedFields_)
                f += (f.empty() ? "" : ", ") + x;
            out += "Fields the displays read, mocked as floats (no program declares them here): " + f + ".\n";
        }
        if (!spec_.shaders.empty())
        {
            int built = 0;
            for (const PanelShader& sh : spec_.shaders)
                built += sh.id != 0 ? 1 : 0;
            out += "Shaders: " + std::to_string(built) + " of " + std::to_string(spec_.shaders.size()) + " built"
                 + (built < static_cast<int>(spec_.shaders.size()) ? " (the notes say why)" : std::string()) + ".\n";
        }
        return out;
    }

    std::string Sandbox::problems(bool detail) const
    {
        std::vector<std::string> lines;
        for (const std::string& l : linesOf(notes_))
            if (!l.empty() && l.find(" no control in the file;") == std::string::npos)   // the report says that
                lines.push_back(l);
        for (const std::string& l : linesOf(dialNotes_))
            if (!l.empty())
                lines.push_back(l);
        if (lines.empty())
            return "No notes: the plugin reads every line.\n";
        std::string out = std::to_string(lines.size()) + (lines.size() == 1 ? " note" : " notes") + " (what the plugin cannot use; the rest stands):\n";
        const std::size_t cap = detail ? lines.size() : std::min<std::size_t>(lines.size(), 12);
        for (std::size_t k = 0; k < cap; ++k)
            out += lines[k] + "\n";
        if (cap < lines.size())
            out += "(" + std::to_string(lines.size() - cap) + " more: detail \"high\" lists them all)\n";
        return out;
    }

    std::string Sandbox::report(bool detail) const
    {
        std::array<float, kMaxControls> values{};
        for (int c = 0; c < kMaxControls; ++c)
            values[static_cast<std::size_t>(c)] = normOf(controls_[static_cast<std::size_t>(c)], controls_[static_cast<std::size_t>(c)].def);
        const View view(spec_, controls_, values, files_, notes_, areaW_, areaH_, "", instrument_);
        const ui::GuiLayout& gl = view.places();
        const std::vector<GuiNode>& nodes = spec_.gui.nodes;
        std::string out;
        const ui::GuiExtent natural = ui::naturalGuiSize(spec_, controls_, measure, static_cast<float>(areaW_));
        out += "Layout in the area " + std::to_string(areaW_) + " x " + std::to_string(areaH_) + " (" + (areaW_ == kDefaultWidth && areaH_ == kDefaultHeight
                   ? std::string("the PANEL view at the plugin's default size") : std::string("the last render's"))
             + "): natural size " + number(natural.w) + " x " + number(natural.h);
        if (gl.maxScrollX <= 0.0f && gl.maxScrollY <= 0.0f)
            out += ", it fits.\n";
        else
        {
            out += ";";
            if (gl.maxScrollY > 0.0f)
                out += " taller than the area by " + number(gl.maxScrollY) + " px, so it scrolls (a bar at its right)";
            if (gl.maxScrollX > 0.0f)
                out += std::string(gl.maxScrollY > 0.0f ? ";" : "") + " wider than the area by " + number(gl.maxScrollX) + " px, so it scrolls sideways";
            out += ".\n";
        }
        if (!view.allPages().empty())
            out += "Pages: " + view.allPages() + " (the first of each shown; render's page shows another).\n";

        // The dials no control names, as GuiSpec's note lists them ("a, b have no control in the file; ..."): the plugin
        // adds them last, in a section without a title, which the elements below call (added).
        std::vector<std::string> left;
        for (const std::string& l : linesOf(notes_))
        {
            const std::size_t at = l.find(": note: "), end = l.find(" no control in the file;");
            if (at == std::string::npos || end == std::string::npos || end < at + 8)
                continue;
            std::string names = l.substr(at + 8, end - at - 8);
            for (const char* verb : { " has", " have" })
                if (names.size() > std::strlen(verb) && endsWith(names, verb))
                    names.resize(names.size() - std::strlen(verb));
            for (std::size_t from = 0; from <= names.size();)
            {
                const std::size_t comma = names.find(", ", from);
                left.push_back(names.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
                if (comma == std::string::npos)
                    break;
                from = comma + 2;
            }
        }
        std::vector<bool> added(nodes.size(), false);
        if (!left.empty() && !nodes.empty() && !nodes[0].children.empty())
        {
            const int last = nodes[0].children.back();
            if (nodes[static_cast<std::size_t>(last)].kind == GuiKind::section && nodes[static_cast<std::size_t>(last)].text.empty())
                for (std::size_t i = static_cast<std::size_t>(last); i < nodes.size(); ++i)
                    added[i] = true;             // the section and what follows it: GuiSpec adds them last
        }

        // The elements on the pages shown, a line each: its line in the file, its kind and name, x,y wxh in the area.
        std::vector<std::string> elements;
        int hidden = 0;
        const auto depthOf = [&](std::size_t i) {
            int d = 0;
            for (int p = nodes[i].parent; p > 0; p = nodes[static_cast<std::size_t>(p)].parent)
                ++d;
            return d;
        };
        for (std::size_t i = 1; i < nodes.size(); ++i)
        {
            if (i >= gl.visible.size() || !gl.visible[i])
            {
                ++hidden;
                continue;
            }
            const GuiNode& n = nodes[i];
            elements.push_back(std::string(static_cast<std::size_t>(2 * depthOf(i)), ' ') + (added[i] ? std::string("(added) ") : "L" + std::to_string(n.line) + " ")
                               + nameOf(n, spec_, controls_) + " " + rectText(gl.rects[i]));
        }
        // Overlaps between the elements that draw something (containers hold theirs).
        std::vector<std::string> overlaps, outside, cut;
        const auto leaf = [&](std::size_t i) {
            return i < gl.visible.size() && gl.visible[i] && !isContainer(nodes[i].kind) && nodes[i].kind != GuiKind::spacer;
        };
        const auto where = [&](std::size_t i) {
            return nameOf(nodes[i], spec_, controls_) + (added[i] ? std::string(" (added)") : " (line " + std::to_string(nodes[i].line) + ")");
        };
        for (std::size_t i = 1; i < nodes.size(); ++i)
        {
            if (!leaf(i))
                continue;
            for (std::size_t j = i + 1; j < nodes.size(); ++j)
            {
                if (!leaf(j))
                    continue;
                const funkgui::Rect& a = gl.rects[i];
                const funkgui::Rect& b = gl.rects[j];
                const float w = std::min(a.right(), b.right()) - std::max(a.x, b.x), h = std::min(a.bottom(), b.bottom()) - std::max(a.y, b.y);
                if (w > 1.0f && h > 1.0f)
                    overlaps.push_back(where(i) + " and " + where(j) + ", by " + number(w) + " x " + number(h) + " px");
            }
        }
        // What sticks out of its container, and text cut to its room.
        for (std::size_t i = 1; i < nodes.size(); ++i)
        {
            if (i >= gl.visible.size() || !gl.visible[i])
                continue;
            const GuiNode& n = nodes[i];
            const funkgui::Rect& r = gl.rects[i];
            if (n.parent > 0)
            {
                const funkgui::Rect& p = gl.rects[static_cast<std::size_t>(n.parent)];
                std::string sides;
                const auto side = [&](float by, const char* name) {
                    if (by > 1.0f)
                        sides += (sides.empty() ? "" : ", ") + number(by) + " px at the " + name;
                };
                side(p.x - r.x, "left");
                side(r.right() - p.right(), "right");
                side(p.y - r.y, "top");
                side(r.bottom() - p.bottom(), "bottom");
                if (!sides.empty())
                    outside.push_back(where(i) + " sticks out of its " + where(static_cast<std::size_t>(n.parent)) + ": " + sides);
            }
            if (n.kind == GuiKind::label)
            {
                const float tw = measure(ui::upper(ui::asciiText(n.text)), ui::guiLabelStyle(n));
                if (tw > r.w + 1.0f)
                    cut.push_back(where(i) + ": its text, " + number(tw) + " px wide, runs over its " + number(r.w) + " px");
            }
            if (n.kind == GuiKind::section && !n.text.empty())
            {
                const float tw = measure(ui::upper(ui::asciiText(n.text)), ui::kGuiSectionTitle);
                if (tw > r.w - 16.0f + 1.0f)
                    cut.push_back(where(i) + ": its title, " + number(tw) + " px wide, is cut to its " + number(r.w - 16.0f) + " px");
            }
        }
        // Controls that name no dial (GuiSpec's notes), and dials no control names.
        std::vector<std::string> nameless;
        for (const std::string& l : linesOf(notes_))
            if (l.find("there is no parameter ") != std::string::npos || l.find("takes a parameter's name") != std::string::npos)
                nameless.push_back(l.substr(0, l.find(": note:")) + l.substr(l.find(": note:") == std::string::npos ? 0 : l.find(": note:") + 6));
        std::string missing;
        for (const std::string& name : left)
            missing += (missing.empty() ? "" : ", ") + name;

        // At most about kReportLines lines: the findings whole, the elements as far as there is room.
        std::vector<std::string> tail;
        const auto group = [&](const char* title, const std::vector<std::string>& items, std::size_t cap) {
            if (items.empty())
                return;
            tail.push_back(std::string(title) + ":");
            const std::size_t n = detail ? items.size() : std::min(items.size(), cap);
            for (std::size_t k = 0; k < n; ++k)
                tail.push_back("  " + items[k]);
            if (n < items.size())
                tail.push_back("  (" + std::to_string(items.size() - n) + " more)");
        };
        group("Overlaps", overlaps, 6);
        group("Outside its container", outside, 6);
        group("Text too wide", cut, 4);
        group("Controls that name no dial", nameless, 6);
        if (!missing.empty())
            tail.push_back("Dials no control names: " + missing + " (the plugin puts them last, in a section without a title).");
        if (overlaps.empty() && outside.empty() && cut.empty() && nameless.empty() && missing.empty())
            tail.push_back("No overlaps, nothing outside its container, no text too wide, every control on a dial and every dial on the "
                           "panel.");
        const int used = static_cast<int>(linesOf(out).size() + tail.size()) + 1;
        const std::size_t room = detail ? elements.size() : static_cast<std::size_t>(std::max(8, kReportLines - used));
        out += "Elements (line, kind and name, x,y wxh in the area" + std::string(hidden > 0 ? "; " + std::to_string(hidden) + " more on pages not shown" : "")
             + "):\n";
        for (std::size_t k = 0; k < elements.size() && k < room; ++k)
            out += elements[k] + "\n";
        if (room < elements.size())
            out += "  ... " + std::to_string(elements.size() - room) + " more (detail \"high\" lists every one)\n";
        for (const std::string& t : tail)
            out += t + "\n";
        return out;
    }

    juce::var Sandbox::edit(const juce::var& r)
    {
        std::string& gui = files_[static_cast<std::size_t>(gui_)].text;
        const std::string before = gui;
        std::string next = gui;
        if (r.hasProperty("text"))
        {
            next = text(r["text"]);
            if (next.size() > kMaxFileBytes)
                return fail("The .gui text is " + std::to_string(next.size() / 1024) + " KB: " + std::to_string(kMaxFileBytes / 1024) + " KB at most.");
        }
        else if (const juce::Array<juce::var>* edits = r["edits"].getArray())
        {
            int k = 0;
            for (const juce::var& e : *edits)
            {
                ++k;
                const std::string from = text(e["old_text"]), to = text(e["new_text"]);
                if (from.empty())
                    return fail("Edit " + std::to_string(k) + "'s old_text is empty: give the exact passage to replace. Nothing changed.");
                std::size_t count = 0, at = std::string::npos;
                for (std::size_t p = next.find(from); p != std::string::npos; p = next.find(from, p + 1))
                {
                    if (count == 0)
                        at = p;
                    ++count;
                }
                if (count != 1)
                    return fail("Edit " + std::to_string(k) + "'s old_text occurs " + std::to_string(count) + " times in "
                                + files_[static_cast<std::size_t>(gui_)].name + " (it must occur exactly once"
                                + (count == 0 ? ": check the whitespace and the text as it is now" : ": give more of it") + "). Nothing changed.");
                next.replace(at, from.size(), to);
            }
        }
        else if (!r.hasProperty("dials"))
            return fail("edit takes text (the .gui whole), edits ([{ old_text, new_text }]) or dials. Nothing changed.");
        if (r.hasProperty("dials"))
        {
            std::string notes;
            const std::vector<Dial> given = parseDials(r["dials"], notes);
            std::vector<Dial> kept;
            for (const Dial& d : dials_)
                if (d.given)
                    kept.push_back(d);
            dials_ = merge(merge(declared_.dials, kept), given);
            dialNotes_ = notes;
        }
        gui = next;
        rebuild();
        const auto [added, removed] = lineChanges(before, gui);
        const bool detail = text(r["detail"]) == "high";
        std::string out = "Sandbox #" + std::to_string(number_) + ": " + files_[static_cast<std::size_t>(gui_)].name + " "
                        + (before == gui ? std::string("unchanged") : "now " + std::to_string(lineCount(gui)) + " lines (+" + std::to_string(added) + " -" + std::to_string(removed) + ")")
                        + ".\n" + problems(detail) + report(detail);
        return object({ { "ok", true }, { "text", str(out) }, { "state", state() }, { "gui", str(gui) } });
    }

    juce::var Sandbox::render(const juce::var& r)
    {
        std::string said;
        const auto side = [&](const char* key, int fallback) {
            const juce::var v = r[key];
            if (!(v.isInt() || v.isInt64() || v.isDouble()))
                return fallback;
            const double d = static_cast<double>(v);
            const int n = std::isfinite(d) ? static_cast<int>(std::lround(d)) : fallback;
            const int c = std::clamp(n, kMinSide, kMaxSide);
            if (c != n)
                said += std::string(key) + " " + std::to_string(n) + " is outside " + std::to_string(kMinSide) + " to " + std::to_string(kMaxSide)
                      + ": " + std::to_string(c) + ". ";
            return c;
        };
        const int w = side("width", areaW_), h = side("height", areaH_);
        areaW_ = w;
        areaH_ = h;
        // The theme asked for, else the user's in the plugin (its preferences, read again: the plugin may have changed
        // them since), as view_panel draws it.
        const std::string theme = text(r["theme"]);
        int themeIndex = theme == "paper" ? 1 : 0;
        const bool users = theme != "paper" && theme != "graphite";
        if (users)
        {
            funkgui::UiPreferences::get().reload();
            themeIndex = funkgui::UiPreferences::get().theme() == 1 ? 1 : 0;
            if (!theme.empty())
                said += "theme is graphite or paper: the user's. ";
        }
        // The dials at their defaults, but those `values` names.
        std::array<float, kMaxControls> values{};
        for (int c = 0; c < kMaxControls; ++c)
            values[static_cast<std::size_t>(c)] = normOf(controls_[static_cast<std::size_t>(c)], controls_[static_cast<std::size_t>(c)].def);
        if (const juce::DynamicObject* o = r["values"].getDynamicObject())
            for (const auto& kv : o->getProperties())
            {
                int slot = -1;
                const Dial* d = dialNamed(kv.name.toString().toStdString(), &slot);
                if (d == nullptr)
                {
                    said += "There is no dial " + kv.name.toString().toStdString() + ". ";
                    continue;
                }
                Dial named = *d;                      // its values' names as the panel has them
                if (!spec_.options[static_cast<std::size_t>(slot)].empty())
                    named.options = spec_.options[static_cast<std::size_t>(slot)];
                float plain = 0.0f;
                if (!valueOf(named, kv.value, plain))
                {
                    said += d->name + " takes a number" + (named.options.empty() ? std::string() : " or one of its names") + ". ";
                    continue;
                }
                values[static_cast<std::size_t>(slot)] = normOf(controls_[static_cast<std::size_t>(slot)], plain);
            }
        const std::string page = text(r["page"]);
        View view(spec_, controls_, values, files_, notes_, w, h, page, instrument_);
        funkgui::HeadlessHost host(view, themeIndex, kDpi);
        host.settle(120);
        const picture::Drawn drawn = picture::draw(host.draw());
        if (drawn.image.w <= 0 || drawn.image.h <= 0)
            return fail("The sandbox drew nothing.");
        // The area's pixels (the frame may be one wider or taller: View::width()).
        funkgui::Image area;
        area.w = std::min(view.pictureWidth(), drawn.image.w);
        area.h = std::min(view.pictureHeight(), drawn.image.h);
        std::string png = picture::png(drawn.image, 0, 0, area.w, area.h);
        if (png.empty())
            return fail("The picture could not be written.");
        if (area.w != drawn.image.w || area.h != drawn.image.h)
        {
            area.rgba.resize(static_cast<std::size_t>(area.w) * static_cast<std::size_t>(area.h) * 4);
            for (int y = 0; y < area.h; ++y)
                std::copy_n(drawn.image.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(drawn.image.w) * 4,
                            static_cast<std::size_t>(area.w) * 4, area.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(area.w) * 4);
        }
        else
            area = drawn.image;
        // At most `cap` bytes (the MCP image cap), unless 0: the PNG scaled down until it fits.
        const double cap = r.hasProperty("cap") ? static_cast<double>(r["cap"]) : 0.0;
        int pw = area.w, ph = area.h;
        bool smaller = false;
        if (cap > 0.0 && static_cast<double>(png.size()) > cap)
        {
            double s = std::min(0.95, 0.95 * std::sqrt(cap / static_cast<double>(png.size())));
            for (int step = 0; step < 10 && static_cast<double>(png.size()) > cap; ++step)
            {
                pw = std::max(64, static_cast<int>(std::lround(area.w * s)));
                ph = std::max(64, static_cast<int>(std::lround(area.h * s)));
                png = picture::png(scaled(area, pw, ph), 0, 0, pw, ph);
                s *= 0.8;
                smaller = true;
            }
        }
        std::string line = said + std::to_string(w) + " x " + std::to_string(h) + " px, " + (themeIndex == 1 ? "paper" : "graphite")
                         + (users ? " (the user's theme in the plugin)" : "") + " (the picture "
                         + std::to_string(pw) + " x " + std::to_string(ph) + " pixels"
                         + (smaller ? ", made smaller to fit, from " + std::to_string(area.w) + " x " + std::to_string(area.h)
                                          + "; detail \"high\" sends it whole"
                                    : std::string())
                         + ")";
        if (!view.allPages().empty())
            line += (view.shownPage().empty() ? (page.empty() ? std::string("; the first pages shown") : "; there is no page " + page + ": the first pages shown")
                                              : "; page " + view.shownPage() + " shown")
                  + " (its pages: " + view.allPages() + ")";
        int unbuilt = 0;
        for (const PanelShader& sh : spec_.shaders)
            unbuilt += sh.id == 0 ? 1 : 0;
        if (!drawn.gpu)
            line += "; drawn without the GPU (" + drawn.why + ")"
                  + (spec_.shaders.empty() ? std::string() : ": each shader is a striped placeholder here, though the plugin draws it");
        else if (unbuilt > 0)
            line += "; drawn by the GPU as the plugin draws it, but " + std::to_string(unbuilt) + " of its "
                  + std::to_string(spec_.shaders.size()) + (spec_.shaders.size() == 1 ? " shader" : " shaders")
                  + " not built here, shown as placeholders (the notes say why)";
        else
            line += std::string("; drawn by the GPU as the plugin draws it") + (spec_.shaders.empty() ? "" : ", shaders included");
        line += "; the displays show demo values. Sandbox #" + std::to_string(number_) + ".";
        // The area goes back with it: the server keeps it, so a helper started again lays the panel out as it was.
        return object({ { "ok", true },
                        { "text", str(line) },
                        { "png", juce::Base64::toBase64(png.data(), png.size()) },
                        { "width", pw },
                        { "height", ph },
                        { "area", juce::Array<juce::var>{ areaW_, areaH_ } },
                        { "gpu", drawn.gpu } });
    }

    juce::var Sandbox::state() const
    {
        juce::Array<juce::var> files;
        for (const SourceFile& f : files_)
            files.add(object({ { "name", str(f.name) }, { "text", str(f.text) } }));
        std::vector<Dial> given;
        for (const Dial& d : dials_)
            if (d.given)
                given.push_back(d);
        return object({ { "files", files },
                        { "dials", toJson(given) },
                        { "origin", str(origin_) },
                        { "number", number_ },
                        { "area", juce::Array<juce::var>{ areaW_, areaH_ } },
                        { "gui", str(files_[static_cast<std::size_t>(gui_)].name) } });
    }
}
