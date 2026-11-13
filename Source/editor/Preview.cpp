// Source/editor/Preview.cpp: see Preview.h.
#include "editor/Preview.h"

#include "editor/PanelView.h"
#include "editor/Picture.h"
#include "editor/ProgramDials.h"
#include "fminus/GuiSpec.h"
#include "fminus/Shaders.h"
#include "sandbox/SandboxProtocol.h"

#include <funkgui/panel/HeadlessHost.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace playground::preview
{
    namespace
    {
        bool sameName(const std::string& a, const std::string& b)
        {
            return a.size() == b.size()
                && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        }

        // `img` scaled to tw x th by the mean of the pixels each covers (the sandbox's way of making a picture smaller).
        funkgui::Image scaled(const funkgui::Image& img, int tw, int th)
        {
            funkgui::Image out;
            out.w = tw;
            out.h = th;
            out.rgba.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4, 0);
            const double sx = static_cast<double>(img.w) / tw, sy = static_cast<double>(img.h) / th;
            for (int y = 0; y < th; ++y)
            {
                const int y0 = static_cast<int>(std::floor(y * sy));
                const int y1 = std::max(y0 + 1, std::min(img.h, static_cast<int>(std::floor((y + 1) * sy))));
                for (int x = 0; x < tw; ++x)
                {
                    const int x0 = static_cast<int>(std::floor(x * sx));
                    const int x1 = std::max(x0 + 1, std::min(img.w, static_cast<int>(std::floor((x + 1) * sx))));
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

        // The version's values by name (an int's or a bool's by its value's name when the panel has it), the rest at
        // their defaults.
        void setValues(Prepared& out, const Request& r)
        {
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = out.controls[static_cast<std::size_t>(c)];
                float plain = k.def;
                if (k.used)
                    for (const Dial& d : r.dials)
                    {
                        if (!sameName(d.name, k.name))
                            continue;
                        plain = d.value;
                        const std::vector<std::string>& opts = out.spec.options[static_cast<std::size_t>(c)];
                        if (!d.label.empty())
                            for (std::size_t o = 0; o < opts.size(); ++o)
                                if (sameName(opts[o], d.label))
                                    plain = k.min + static_cast<float>(o);
                        break;
                    }
                out.values01[static_cast<std::size_t>(c)] = normOf(k, plain);
            }
        }
    }

    Prepared prepare(const Request& r, const ShaderTools& tools)
    {
        Prepared out;
        out.instrument = r.instrument;
        out.files = r.files;
        int gui = -1;
        for (std::size_t k = 0; k < out.files.size() && gui < 0; ++k)
            if (isGuiFile(out.files[k].name))
                gui = static_cast<int>(k);
        if (out.files.empty())
        {
            out.why = "the version has no files";
            return out;
        }
        if (gui < 0)
        {
            // No .gui: the dials in rows, or in the sections and displays its #pragma panel lines ask for, read from
            // the program's FMinus files as the compile reads them.
            const sandbox::Declared declared = sandbox::readProgram(out.files, r.instrument);
            out.controls = sandbox::controlsOf(declared.dials);
            std::vector<const SourceFile*> fm;
            for (const SourceFile& f : out.files)
                if (f.name.size() > 3 && f.name.compare(f.name.size() - 3, 3, ".fm") == 0)
                    fm.push_back(&f);
            out.spec = readPanelSpec(fm, out.controls, declared.fields, r.instrument, out.notes);
            out.rows = true;
            setValues(out, r);
            out.ok = true;
            return out;
        }
        // The dials as the GUI sandbox reads them (never compiled), and the fields the displays may read: the program's,
        // else those the .gui names, mocked as floats (Sandbox::rebuild's way).
        const sandbox::Declared declared = sandbox::readProgram(out.files, r.instrument);
        std::vector<PanelField> fields;
        if (!declared.module.empty())
            fields = declared.fields;
        else
            for (const std::string& f : sandbox::fieldsNamed(out.files[static_cast<std::size_t>(gui)].text))
            {
                const std::size_t open = f.find('[');
                const std::string name = f.substr(0, open);
                std::string type = "float";
                for (std::size_t k = open; k != std::string::npos; k = f.find('[', k + 1))
                    type += "[?]";
                if (std::none_of(fields.begin(), fields.end(), [&](const PanelField& x) { return x.name == name; }))
                    fields.push_back({ name, type });
            }
        out.controls = sandbox::controlsOf(declared.dials);
        std::vector<std::string> names;
        for (const SourceFile& f : out.files)
            names.push_back(f.name);
        out.spec = readGuiSpec(out.files[static_cast<std::size_t>(gui)], out.controls, fields, r.instrument, names, out.notes);
        if (out.spec.gui.nodes.size() <= 1)
        {
            // Nothing of it reads (no element under its root): its first note says why, as the problems list would.
            const std::string first = out.notes.substr(0, out.notes.find('\n'));
            const std::size_t at = first.find(": note: ");
            out.why = out.files[static_cast<std::size_t>(gui)].name + " does not read"
                    + (at == std::string::npos ? std::string() : ": " + first.substr(at + 8));
            return out;
        }
        setValues(out, r);
        // The shaders, built here (the process's cache, in memory and on disk), as the sandbox builds them.
        std::vector<ShaderJob> jobs;
        std::vector<PanelShader*> built;
        const Sources headers = shaderHeaders(out.files);
        for (PanelShader& sh : out.spec.shaders)
        {
            const auto f = std::find_if(out.files.begin(), out.files.end(), [&](const SourceFile& s) { return s.name == sh.file; });
            if (f == out.files.end())
                continue;
            if (!tools.usable())
            {
                out.notes += sh.file + ": note: not built: no shader compiler found; it shows as a placeholder\n";
                continue;
            }
            jobs.push_back({ sh.file, f->text, sh.inputs, headers });
            built.push_back(&sh);
        }
        const std::vector<ShaderBuild> builds = shaderCache().buildAll(jobs, tools);
        for (std::size_t k = 0; k < builds.size() && k < built.size(); ++k)
        {
            const ShaderBuild& b = builds[k];
            if (b.ok())
            {
                built[k]->binary = b.binary;
                built[k]->id = shaderId(b.binary);
            }
            else if (out.notes.find(b.error) == std::string::npos)
                out.notes += b.error;
        }
        out.ok = true;
        return out;
    }

    funkgui::Image areaOf(const funkgui::Image& frame, int w, int h, int maxWidth)
    {
        funkgui::Image area;
        area.w = std::min(w, frame.w);
        area.h = std::min(h, frame.h);
        area.rgba.resize(static_cast<std::size_t>(area.w) * static_cast<std::size_t>(area.h) * 4);
        for (int y = 0; y < area.h; ++y)
            std::copy_n(frame.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.w) * 4,
                        static_cast<std::size_t>(area.w) * 4, area.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(area.w) * 4);
        if (maxWidth > 0 && area.w > maxWidth)
        {
            const int th = std::max(1, static_cast<int>(std::lround(static_cast<double>(area.h) * maxWidth / area.w)));
            return scaled(area, maxWidth, th);
        }
        return area;
    }

    Picture draw(const Prepared& p, int theme, int maxWidth)
    {
        Picture out;
        if (!p.ok)
        {
            out.why = p.why;
            return out;
        }
        if (p.rows)
            return drawRows(p, theme, maxWidth);
        sandbox::View view(p.spec, p.controls, p.values01, p.files, p.notes, kAreaWidth, kAreaHeight, "", p.instrument);
        funkgui::HeadlessHost host(view, theme == 1 ? 1 : 0, sandbox::kDpi);
        host.settle(120);
        const picture::Drawn drawn = picture::draw(host.draw());
        out.gpu = drawn.gpu;
        out.why = drawn.why;
        if (drawn.image.w <= 0 || drawn.image.h <= 0)
        {
            out.why = "nothing was drawn";
            return out;
        }
        // The area's pixels (the frame may be one wider or taller: View::width()).
        out.image = areaOf(drawn.image, view.pictureWidth(), view.pictureHeight(), maxWidth);
        return out;
    }
}
