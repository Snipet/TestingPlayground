// Source/editor/Picture.cpp: picturePanel (plugin/EditorPort.h), the chat's view_panel tool's picture: a Panel of its
// own, headless, at the editor's size, its frame drawn on the GPU (FunkGui's renderGpuPicture: shaders as on screen),
// else with FunkGui's SoftRaster (picture::draw, PictureDraw.cpp), and written as a PNG; pictureSchematic, its view
// "schematic" (a SchematicPicture: a netlist's drawing alone, no editor Panel); and the GPU context they hold
// (picturesIdle).
#include "editor/Picture.h"

#include "editor/CodeAssist.h"
#include "editor/Panel.h"
#include "editor/SchematicDraw.h"
#include "editor/Theme.h"
#include "fminus/CirDoc.h"
#include "plugin/EditorPort.h"

#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#if defined(FUNKGUI_HAS_BGFX) && FUNKGUI_HAS_BGFX
#include <funkgui/gpu/GpuPicture.h>
#endif

#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace playground
{
    namespace
    {
        constexpr float kDpi = 1.5f;             // the picture's pixels a logical px: the panel's text stays legible

        // The meters are measured only while someone reads them (EditorPort::addMeterReader). When the picture's Panel
        // is the only reader (no editor open), wait for two blocks measured (the one under way may have begun before
        // it), so its displays show the sound playing now: 100 ms at most, for an audio thread that is not running.
        void waitForMeters(EditorPort& port)
        {
            if (port.meterReaders() > 1)
                return;
            Meters m;
            for (int k = 0; k < 10 && !port.meters(m); ++k)
                std::this_thread::yield();
            const uint32_t from = m.frame;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (std::chrono::steady_clock::now() < until)
            {
                if (port.meters(m) && m.frame - from >= 2)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    // The hold (EditorPort.h): let go `seconds` after the last picture. Releasing it shuts a headless bgfx down (an
    // editor's goes on: nothing restarts while one is open); the next picture starts it again, its shaders' programs
    // and the font's texture built again at their first draw. Keeping it would leave a GPU device, bgfx's buffers and
    // one of BgfxContext's window slots alive in a host whose user closed the editor long ago; letting go after each
    // picture would start bgfx for each of a burst (Claude Desktop looking at a page, then the next).
    void picturesIdle(double seconds)
    {
#if defined(FUNKGUI_HAS_BGFX) && FUNKGUI_HAS_BGFX
        if (!funkgui::gpuPictureContextHeld())
            return;
        if (const int64_t since = picture::msSinceGpuPicture(); since >= 0 && static_cast<double>(since) < seconds * 1000.0)
            return;
        funkgui::releaseGpuPictureContext();
#else
        static_cast<void>(seconds);
#endif
    }

    PanelPicture picturePanel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, int width, int height,
                              bool window, const std::string& page)
    {
        PanelPicture out;
        // What the user has, given back after: the view, the chat's fold, the pages, the netlists' views (a picture's
        // Panel writes none of them on a gesture, as it has none, but what it lays out is never the session's).
        const bool view = port.panelView();
        const bool chatOpen = port.chatOpen();
        const std::vector<std::string> pages = port.guiPages();
        const std::vector<FileView> fileViews = port.fileViews();
        {
            ui::Panel panel(ports, port);
            waitForMeters(port);
            funkgui::HeadlessHost host(panel, funkgui::UiPreferences::get().theme(), kDpi);
            host.resize(std::max(width > 0 ? width : ui::kDefaultWidth, ui::kMinWidth),
                        std::max(height > 0 ? height : ui::kDefaultHeight, ui::kMinHeight));
            host.settle(120);
            if (!window)
                panel.setPanelView(true);
            // The page asked for: the first tabs element with a page of that title.
            std::string shownPage;
            const std::vector<GuiNode>& nodes = panel.panelSpec().gui.nodes;
            std::string allPages;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (nodes[i].kind != GuiKind::tabs)
                    continue;
                std::string these;
                for (std::size_t k = 0; k < nodes[i].children.size(); ++k)
                {
                    const std::string& title = nodes[static_cast<std::size_t>(nodes[i].children[k])].text;
                    these += (these.empty() ? "" : " / ") + title;
                    if (!page.empty() && shownPage.empty() && title == page)
                    {
                        panel.chooseGuiPage(static_cast<int>(i), static_cast<int>(k));
                        shownPage = title;
                    }
                }
                allPages += (allPages.empty() ? "" : "; ") + these;
            }
            host.settle(120);
            const picture::Drawn drawn = picture::draw(host.draw());
            const funkgui::Image& img = drawn.image;
            out.gpu = drawn.gpu;
            if (img.w <= 0 || img.h <= 0)
                out.why = "the editor drew nothing";
            else
            {
                // The panel's own area (a .gui file's interface, else the program's card) or the whole editor.
                funkgui::Rect crop{ 0.0f, 0.0f, static_cast<float>(img.w) / kDpi, static_cast<float>(img.h) / kDpi };
                if (!window)
                    crop = panel.hasGui() ? panel.guiLayout().area : panel.programCard();
                const int x0 = std::clamp(static_cast<int>(std::floor(crop.x * kDpi)), 0, img.w - 1);
                const int y0 = std::clamp(static_cast<int>(std::floor(crop.y * kDpi)), 0, img.h - 1);
                const int w = std::clamp(static_cast<int>(std::ceil(crop.w * kDpi)), 1, img.w - x0);
                const int h = std::clamp(static_cast<int>(std::ceil(crop.h * kDpi)), 1, img.h - y0);
                out.png = picture::png(img, x0, y0, w, h);
                out.width = w;
                out.height = h;
                if (out.png.empty())
                    out.why = "the picture could not be written";
                const int lw = static_cast<int>(std::lround(crop.w)), lh = static_cast<int>(std::lround(crop.h));
                out.about = std::string(window ? "The whole editor, as the user has it" : "The program's panel, in the PANEL view")
                          + ", " + std::to_string(lw) + " x " + std::to_string(lh) + " logical px (the picture "
                          + std::to_string(w) + " x " + std::to_string(h) + " pixels)"
                          + (window ? "" : (panel.hasGui() ? ", from panel.gui" : ", the dials (no .gui file)"))
                          + (allPages.empty() ? std::string() : ". Its pages: " + allPages)
                          + (shownPage.empty() ? (page.empty() ? std::string() : ". There is no page " + page + "; the first pages are shown")
                                               : ". Shown: page " + shownPage)
                          + (drawn.gpu ? std::string(". Drawn by the GPU as the user's screen draws it, shaders included")
                                       : ". Drawn without the GPU (" + drawn.why + "): each shader shows as a striped "
                                         "placeholder here, though the user's screen draws it")
                          + "; the displays show the sound playing now.";
            }
        }
        port.setPanelView(view);
        port.setChatOpen(chatOpen);
        port.setGuiPages(pages);
        port.setFileViews(fileViews);
        return out;
    }

    // ---- view_panel's schematic ----------------------------------------------------------------------------------------
    namespace
    {
        constexpr float kSchematicW = 1400.0f, kSchematicH = 900.0f;   // a schematic picture's largest, logical px
        constexpr int kSchematicMargin = 5;              // grid units around the drawing: the labels above and beside

        // A netlist's drawing, alone: SchematicDraw's still frame on the code editor's ground, nothing of an editor (no
        // ui::Panel: no probe owned, no file view read or written, no meter reader).
        class SchematicPicture final : public funkgui::Panel
        {
        public:
            SchematicPicture(const cir::Netlist& nl, const cir::Drawing& d, std::vector<ui::sch::PartMarks> marks, cir::Box box,
                             float scale, int w, int h)
                : nl_(nl), d_(d), marks_(std::move(marks)), box_(box), scale_(scale), w_(w), h_(h) {}

            void attach(funkgui::HostServices&) override {}
            int width() const override { return w_; }
            int height() const override { return h_; }
            void tick(float) override {}
            bool wantsFullRate() const override { return false; }
            void accessibility(std::vector<funkgui::A11yItem>&) const override {}
            uint32_t a11yRevision() const override { return 0; }
            void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
            void closeGestures() override {}

            void draw(funkgui::Canvas& c, const funkgui::Theme& hostTheme) override
            {
                const funkgui::Theme th = ui::productTheme(hostTheme);
                const funkgui::TextEditorOptions o;      // the tab editor's own marks: its DecorationInks
                ui::sch::Inks k = ui::sch::inksFor(th, o.inks.error.resolve(th), o.inks.warning.resolve(th), ui::codeSecond(th), false);
                k.ground = ui::codeGround(th);
                const float w = static_cast<float>(w_), h = static_cast<float>(h_);
                c.rrect(0.0f, 0.0f, w, h, 0.0f, k.ground);
                ui::sch::View v;
                v.scale = scale_;
                v.ox = (static_cast<float>(kSchematicMargin - box_.x0)) * scale_;
                v.oy = (static_cast<float>(kSchematicMargin - box_.y0)) * scale_;
                const funkgui::Canvas::Scope s(c, ui::sch::kPartTag, false);
                labelled_ = ui::sch::still(c, v, nl_, d_, marks_, k, { 0.0f, 0.0f, w, h }, ui::sch::labelPx(v, true));
            }
            int labelled() const noexcept { return labelled_; }   // the parts whose names the last frame drew

        private:
            const cir::Netlist& nl_;
            const cir::Drawing& d_;
            std::vector<ui::sch::PartMarks> marks_;
            cir::Box box_;
            float scale_;
            int w_, h_;
            int labelled_ = 0;
        };

        std::string pt(cir::Pt p) { return std::to_string(p.x) + "," + std::to_string(p.y); }
    }

    PanelPicture pictureSchematic(EditorPort& port, const std::string& file)
    {
        PanelPicture out;
        const Draft draft = port.draft();
        const SourceFile* f = nullptr;
        for (const SourceFile& x : draft.files)
            if (x.name == file)
                f = &x;
        if (f == nullptr)
        {
            out.why = "there is no file " + file;
            return out;
        }
        if (!isCircuitFile(file))
        {
            out.why = file + " is no .cir netlist: the schematic is a netlist's";
            return out;
        }
        const std::string text = cir::sanitised(f->text);
        const cir::Netlist nl = cir::read(text);
        const cir::Drawing d = cir::reconcile(text, cir::readLayout(port.schematicPins(file)));
        // The last compile's messages on this netlist, when it compiled this netlist (its *@ lines aside), at the lines
        // they are on now.
        std::vector<ui::Diagnostic> problems;
        const CompileStatus status = port.status();
        for (const SourceFile& s : status.sources)
            if (s.name == file && cir::sameNetlist(s.text, text))
            {
                const std::vector<int> map = cir::lineMap(s.text, text);
                for (const ui::Problem& p : ui::parseProblems(status.errors + status.notes))
                {
                    if (p.file != file)
                        continue;
                    ui::Diagnostic g;
                    g.file = p.file;
                    g.line = p.line;
                    if (p.line > 0 && p.line - 1 < static_cast<int>(map.size()) && map[static_cast<std::size_t>(p.line - 1)] >= 0)
                        g.line = map[static_cast<std::size_t>(p.line - 1)] + 1;
                    g.column = p.column;
                    g.severity = p.error ? ui::Diagnostic::Severity::error
                               : p.warning ? ui::Diagnostic::Severity::warning
                                           : ui::Diagnostic::Severity::info;
                    g.text = p.text;
                    problems.push_back(std::move(g));
                }
            }
        const std::vector<ui::sch::Mark> marks = ui::sch::mapMarks(nl, d, problems);
        // At 100 % (10 px a grid unit), smaller only to fit the largest picture.
        cir::Box box = d.bounds();
        if (box.empty())
            box = { 0, 0, 20, 10 };
        const float units = static_cast<float>(box.x1 - box.x0 + 2 * kSchematicMargin);
        const float unitsY = static_cast<float>(box.y1 - box.y0 + 2 * kSchematicMargin);
        const float scale = std::min({ ui::sch::kUnitPx, kSchematicW / units, kSchematicH / unitsY });
        const int w = std::max(120, static_cast<int>(std::ceil(units * scale)));
        const int h = std::max(80, static_cast<int>(std::ceil(unitsY * scale)));
        {
            SchematicPicture panel(nl, d, ui::sch::partMarksOf(d, marks), box, scale, w, h);
            funkgui::HeadlessHost host(panel, funkgui::UiPreferences::get().theme(), kDpi);
            host.settle(4);
            const picture::Drawn drawn = picture::draw(host.draw());
            const funkgui::Image& img = drawn.image;
            out.gpu = drawn.gpu;
            if (img.w <= 0 || img.h <= 0)
            {
                out.why = "the schematic drew nothing";
                return out;
            }
            out.png = picture::png(img, 0, 0, img.w, img.h);
            out.width = img.w;
            out.height = img.h;
            if (out.png.empty())
                out.why = "the picture could not be written";
            const int percent = static_cast<int>(std::lround(scale / ui::sch::kUnitPx * 100.0f));
            // What it shows, in words: the parts with their places and pins, the nets, the problems, what is not drawn.
            int nets = 0;
            for (const cir::Net& n : d.nets)
                nets += n.pins.empty() ? 0 : 1;
            std::string a = "The schematic of " + file + ", as the user's Schematic view draws it, at " + std::to_string(percent)
                          + " % (" + (percent == 100 ? std::string("10 px a grid unit") : std::string("made smaller to fit"))
                          + "), " + std::to_string(w) + " x " + std::to_string(h) + " logical px (the picture "
                          + std::to_string(img.w) + " x " + std::to_string(img.h) + " pixels), every name and value shown ("
                          + std::to_string(panel.labelled()) + " parts labelled): " + std::to_string(d.parts.size()) + " parts, "
                          + std::to_string(nets) + " nets. Places are grid "
                          "units, x to the right and y down (its *@ lines'); a part's place is its symbol's origin.\nParts "
                          "(name at place and orientation: kind and value; each pin at its point, on its net):\n";
            for (const cir::Part& p : d.parts)
            {
                const std::string value = ui::sch::valueText(nl, p);
                a += "- " + p.name + " at " + pt(p.origin) + " " + cir::orientName(p.orient) + ": "
                   + (p.element < 0 ? std::string(p.kind == cir::Kind::Input ? "the input" : "the output") : std::string(cir::symbol(p.kind).word))
                   + (value.empty() ? "" : " " + value) + ";";
                for (std::size_t k = 0; k < p.pins.size(); ++k)
                {
                    const cir::PinAt& pin = p.pins[k];
                    const std::string net = pin.net >= 0 ? d.nets[static_cast<std::size_t>(pin.net)].name : std::string("nothing");
                    a += std::string(k == 0 ? " " : ", ") + (pin.name.empty() ? std::string("pin") : pin.name) + " " + pt(pin.at) + " (" + net + ")";
                }
                if (p.source != cir::Source::stored)
                    a += p.source == cir::Source::session ? " [no part line: placed where the user's view drew it]"
                                                          : " [no part line: placed by the layout]";
                a += "\n";
            }
            a += "Nets (their pins; how each is drawn):\n";
            for (const cir::Net& n : d.nets)
            {
                if (n.pins.empty())
                    continue;
                a += "- " + n.name + ":";
                for (std::size_t k = 0; k < n.pins.size(); ++k)
                {
                    const cir::Part& p = d.parts[static_cast<std::size_t>(n.pins[k].part)];
                    a += std::string(k == 0 ? " " : ", ")
                       + (p.element < 0 ? p.name : p.name + "." + p.pins[static_cast<std::size_t>(n.pins[k].pin)].name);
                }
                a += n.style == cir::Style::ground ? " (ground: a flag at each pin)"
                   : n.style == cir::Style::rail   ? " (a rail: a flag at each pin)"
                   : n.style == cir::Style::flag || n.unroutable ? " (name tags at its pins)"
                                                                 : " (wired)";
                a += "\n";
            }
            a += "Problems (the last run's, on this netlist):";
            if (marks.empty())
                a += " none\n";
            else
            {
                a += "\n";
                for (const ui::sch::Mark& m : marks)
                {
                    a += "- " + file + ":" + std::to_string(m.line + 1) + ":" + std::to_string(m.col + 1) + ": "
                       + (m.severity == 0 ? "error" : m.severity == 1 ? "warning" : "note") + ": " + m.text;
                    std::string on;
                    for (const int p : m.parts)
                        on += (on.empty() ? "" : ", ") + d.parts[static_cast<std::size_t>(p)].name;
                    a += (on.empty() ? std::string() : " (marks " + on + ")") + "\n";
                }
            }
            for (const auto& [x, y] : d.overlaps)
                a += "- " + x + " overlaps " + y + " (drawn outlined: move one)\n";
            for (const std::string& n : d.notes)
                a += "- " + n + "\n";
            if (!d.notDrawn.empty())
            {
                a += "Not drawn (listed only):";
                for (std::size_t k = 0; k < d.notDrawn.size(); ++k)
                    a += (k == 0 ? " " : ", ") + d.notDrawn[k];
                a += "\n";
            }
            a += drawn.gpu ? std::string("Drawn by the GPU.") : "Drawn without the GPU (" + drawn.why + ").";
            out.about = std::move(a);
        }
        return out;
    }
}
