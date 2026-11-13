// Source/editor/Picture.cpp: picturePanel (plugin/EditorPort.h), the chat's view_panel tool's picture: a Panel of its
// own, headless, at the editor's size, its frame drawn on the GPU (FunkGui's renderGpuPicture: shaders as on screen),
// else with FunkGui's SoftRaster (picture::draw, PictureDraw.cpp), and written as a PNG; pictureSchematic, its view
// "schematic" (a SchematicPicture: a netlist's drawing alone, no editor Panel); and the GPU context they hold
// (picturesIdle).
#include "editor/Picture.h"

#include "editor/CodeAssist.h"
#include "editor/Columns.h"
#include "editor/DisplayItem.h"
#include "editor/GuiLayout.h"
#include "editor/Items.h"
#include "editor/Panel.h"
#include "editor/SchematicDraw.h"
#include "editor/Theme.h"
#include "fminus/CirDoc.h"
#include "plugin/EditorPort.h"

#include "Product.h"

#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#if defined(FUNKGUI_HAS_BGFX) && FUNKGUI_HAS_BGFX
#include <funkgui/gpu/GpuPicture.h>
#endif

#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace playground
{
    namespace
    {
        static_assert(PictureState::kScopeSamples == ui::DisplayItem::kScopeSamples);
        constexpr float kDpi = 1.5f;             // the picture's pixels a logical px: the panel's text stays legible

        // The meters are measured only while someone reads them (EditorPort::addMeterReader). When the picture's Panel
        // is the only reader (no editor shown), wait for two blocks measured (the one under way may have begun before
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

        // A dial's port in a picture whose state sets it: the user's in all but its value, which the knob and a
        // shader's input of it read (a picture's Panel writes nothing).
        class ShownPort final : public funkgui::ParamPort
        {
        public:
            ShownPort(funkgui::ParamPort& real, float value01) : real_(real), value_(value01) {}
            float value01() const override { return value_; }
            float default01() const override { return real_.default01(); }
            int numSteps() const override { return real_.numSteps(); }
            void beginGesture() override {}
            void setValue01(float) override {}
            void endGesture() override {}
            const char* id() const override { return real_.id(); }
            void* native() const override { return real_.native(); }

        private:
            funkgui::ParamPort& real_;
            float value_;
        };

        std::string lowered(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string numberText(float v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.4g", static_cast<double>(v));
            return buf;
        }

        // A value's plain number for dial `c` (its options by name, from the range's low end; on and off), false when
        // it names none.
        bool plainOfValue(const Control& c, const std::vector<std::string>& options, const PictureValue& v, float& plain)
        {
            if (!v.isText)
            {
                plain = v.number;
                return std::isfinite(plain);
            }
            const std::string t = lowered(v.text);
            for (std::size_t k = 0; k < options.size(); ++k)
                if (!options[k].empty() && lowered(options[k]) == t)
                {
                    plain = (c.type == ControlType::toggle ? 0.0f : c.min) + static_cast<float>(k);
                    return true;
                }
            if (t == "on" || t == "true" || t == "off" || t == "false")
            {
                plain = t == "on" || t == "true" ? 1.0f : 0.0f;
                return true;
            }
            char* end = nullptr;
            const double x = std::strtod(t.c_str(), &end);
            plain = static_cast<float>(x);
            return end != t.c_str() && std::isfinite(x);
        }

        // What a state asks of the picture, against the program the panel shows: the dials (normalised, by slot), the
        // taps, the levels, and in words what was drawn and what names nothing.
        struct Resolved
        {
            std::array<float, kMaxControls> dial01{};
            std::array<bool, kMaxControls> dialSet{};
            ui::Panel::ShownState shown;
            std::string drawn;                   // "gear 12, drive TRUCK, out -6 dBFS"
            std::string refused;                 // "drive takes a number or one of its names. "
            std::string unknown;                 // "speed, x"
        };

        Resolved resolve(const PictureState& state, const Controls& cs, const PanelSpec& spec)
        {
            Resolved r;
            const auto slotNamed = [&](const std::string& name) {
                for (int c = 0; c < kMaxControls; ++c)
                    if (cs[static_cast<std::size_t>(c)].used && name == cs[static_cast<std::size_t>(c)].name)
                        return c;
                return -1;
            };
            const auto setDial = [&](int slot, float plain) {
                const Control& c = cs[static_cast<std::size_t>(slot)];
                r.dial01[static_cast<std::size_t>(slot)] = normOf(c, plain);
                r.dialSet[static_cast<std::size_t>(slot)] = true;
            };
            for (const auto& [name, plain] : state.dials)  // at's: the dials as the render had them then
                if (const int slot = slotNamed(name); slot >= 0)
                    setDial(slot, plain);
            if (state.rendered())
            {
                r.shown.frames = state.frames;
                r.shown.scopeIn = state.scopeIn;
                r.shown.scopeOut = state.scopeOut;
            }
            for (const PictureValue& v : state.values)
            {
                const auto said = [&](const std::string& what) { r.drawn += (r.drawn.empty() ? "" : ", ") + v.name + " " + what; };
                if (const int w = pointerInput(v.name); w >= 0)
                {
                    // The pointer over every shader that reads it (pointer.x ... 0 to 1; down and hover 0 or 1).
                    if (v.isText || !std::isfinite(v.number))
                    {
                        r.refused += v.name + " takes a number. ";
                        continue;
                    }
                    const bool read = std::any_of(spec.gui.nodes.begin(), spec.gui.nodes.end(), [&](const GuiNode& n) {
                        return n.kind == GuiKind::shader && std::find(n.inputs.begin(), n.inputs.end(), v.name) != n.inputs.end();
                    });
                    r.shown.pointer[static_cast<std::size_t>(w)] = std::clamp(v.number, 0.0f, 1.0f);
                    said(numberText(std::clamp(v.number, 0.0f, 1.0f)) + (read ? "" : " (no shader reads it)"));
                    continue;
                }
                if (const int slot = slotNamed(v.name); slot >= 0)
                {
                    const Control& c = cs[static_cast<std::size_t>(slot)];
                    const std::vector<std::string>& options = spec.options[static_cast<std::size_t>(slot)];
                    float plain = 0.0f;
                    if (!plainOfValue(c, options, v, plain))
                    {
                        r.refused += v.name + " takes a number" + (options.empty() ? std::string() : " or one of its names") + ". ";
                        continue;
                    }
                    setDial(slot, plain);
                    said(dialText(c, plainOf(c, normOf(c, plain)), options));
                    continue;
                }
                const bool level = v.name == "out" || (v.name == "in" && !product::kIsInstrument);
                if (level && !v.isText)
                {
                    const float lin = v.number > -240.0f ? std::pow(10.0f, v.number / 20.0f) : 0.0f;
                    (v.name == "in" ? r.shown.inLevel : r.shown.outLevel) = std::isfinite(lin) ? lin : 0.0f;
                    said(numberText(v.number) + " dBFS");
                    continue;
                }
                bool found = false;
                for (const PanelDisplay& d : spec.displays)
                    if (!d.audio() && d.tap >= 0 && (d.source == v.name || d.name == v.name))
                    {
                        if (!v.isText)
                            r.shown.taps.emplace_back(d.tap, v.number);
                        found = true;
                    }
                if (found && !v.isText)
                    said(numberText(v.number));
                else if (found || level)
                    r.refused += v.name + " takes a number. ";
                else
                    r.unknown += (r.unknown.empty() ? "" : ", ") + v.name;
            }
            return r;
        }

        // The fields the panel reads, each once (a display's, a live= mark's, a shader's input), for the words.
        std::string fieldsOf(const PanelSpec& spec)
        {
            std::string out;
            std::vector<std::string> seen;
            for (const PanelDisplay& d : spec.displays)
                if (!d.audio() && std::find(seen.begin(), seen.end(), d.source) == seen.end())
                {
                    seen.push_back(d.source);
                    out += (out.empty() ? "" : ", ") + d.source;
                }
            return out;
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

    // ---- the panel's fit (EditorPort.h) -----------------------------------------------------------------------------
    PanelFitReady panelFitReady()
    {
        PanelFitReady out;
        out.fonts = funkgui::FontService::get().atlas().baked();   // made at the first call (the message thread's)
        out.chatWidth = funkgui::UiPreferences::get().getInt(ui::columns::kChatWidthKey, 0, 0,
                                                             static_cast<int>(ui::columns::kChatMaxW));
        out.explorerWidth = funkgui::UiPreferences::get().getInt(ui::columns::kExplorerWidthKey, 0, 0,
                                                                 static_cast<int>(ui::columns::kExplorerMaxW));
        return out;
    }

    std::string panelFitLine(const PanelSpec& spec, const Controls& controls, int width, int height, bool chatOpen,
                             int chatWidth, bool explorerOpen, int explorerWidth)
    {
        if (spec.gui.empty())
            return {};
        const bool sized = width > 0 && height > 0;
        const float w = static_cast<float>(std::max(sized ? width : ui::kDefaultWidth, ui::kMinWidth));
        const float h = static_cast<float>(std::max(sized ? height : ui::kDefaultHeight, ui::kMinHeight));
        const funkgui::Rect area = ui::panelViewArea(w, h, chatOpen, static_cast<float>(std::max(0, chatWidth)),
                                                     explorerOpen, static_cast<float>(std::max(0, explorerWidth)));
        const ui::GuiMeasure measure = [](const std::string& text, const funkgui::TextStyle& st) {
            return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(), st);
        };
        const ui::GuiLayout gl = ui::layoutGui(spec, controls, measure, {}, area, 0.0f, 0.0f);
        return ui::guiFitLine(spec, controls, gl, std::string("the PANEL view's area at ")
                                                      + (sized ? "the editor's size" : "the editor's default size")
                                                      + (chatOpen ? ", the chat open" : ""));
    }

    PanelPicture picturePanel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, int width, int height,
                              bool window, const std::string& page, const PictureState& state)
    {
        PanelPicture out;
        // A state to draw (view_panel's values and at): resolved against the program the panel shows (as the Panel's
        // refresh takes it), its dials through ports of their own.
        const bool stated = !state.empty();
        const CompileStatus status = port.status();
        // at's render is of the files that played when it began: drawn on another program's panel, its taps (by
        // place) and dials (by name) would land on displays and dials that are not theirs.
        if (state.rendered() && !cir::sameProgram(state.sources, status.ok ? status.sources : port.running()))
        {
            out.why = "the program that plays changed while at rendered it; look again";
            return out;
        }
        const Controls cs = status.ok ? status.controls : port.controls();
        const PanelSpec spec = status.ok ? status.panel : port.panel();
        Resolved asked = stated ? resolve(state, cs, spec) : Resolved{};
        std::vector<std::unique_ptr<ShownPort>> shownPorts;
        std::vector<funkgui::ParamPort*> drawnPorts(ports.begin(), ports.end());
        for (std::size_t c = 0; c < drawnPorts.size() && c < static_cast<std::size_t>(kMaxControls); ++c)
            if (asked.dialSet[c] && drawnPorts[c] != nullptr)
            {
                shownPorts.push_back(std::make_unique<ShownPort>(*drawnPorts[c], asked.dial01[c]));
                drawnPorts[c] = shownPorts.back().get();
            }
        const bool rendered = state.rendered();
        // What the user has, given back after: the view, the chat's fold, the pages, the netlists' views (a picture's
        // Panel writes none of them on a gesture, as it has none, but what it lays out is never the session's).
        const bool view = port.panelView();
        const bool chatOpen = port.chatOpen();
        const std::vector<std::string> pages = port.guiPages();
        const std::vector<FileView> fileViews = port.fileViews();
        {
            ui::Panel panel(drawnPorts, port, true);  // a picture's: no probe and no session pin of its own, the
                                                      // user's stay as they are
            if (stated)
                panel.showState(asked.shown);
            if (!rendered)
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
            if (rendered)
            {
                // at's frames, one a frame up to its moment, which the picture shows (a field scope's last seconds, a
                // VU's swing and the peaks as the editor would have them then).
                panel.playShown();
                for (int k = 0; k < 400 && !panel.shownDone(); ++k)
                    host.tick(1);
                host.tick(1);                        // the last frame's readouts
            }
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
                out.about = std::string(window ? (panel.editorOut() ? "The whole editor, as the user has it: the panel full "
                                                                      "size, the code editor in a window of its own (not shown)"
                                                                    : "The whole editor, as the user has it")
                                               : "The program's panel, in the PANEL view")
                          + ", " + std::to_string(lw) + " x " + std::to_string(lh) + " logical px (the picture "
                          + std::to_string(w) + " x " + std::to_string(h) + " pixels)"
                          + (window ? "" : (panel.hasGui() ? ", from panel.gui" : ", the dials (no .gui file)"))
                          + (allPages.empty() ? std::string() : ". Its pages: " + allPages)
                          + (shownPage.empty() ? (page.empty() ? std::string() : ". There is no page " + page + "; the first pages are shown")
                                               : ". Shown: page " + shownPage)
                          + (drawn.gpu ? std::string(". Drawn by the GPU as the user's screen draws it, shaders included")
                                       : ". Drawn without the GPU (" + drawn.why + "): each shader shows as a striped "
                                         "placeholder here, though the user's screen draws it");
                if (!stated)
                    out.about += "; the displays show the sound playing now.";
                else
                {
                    // What was drawn in place of what plays, and what the displays read then.
                    out.about += rendered ? "; the displays and the dials show the state " + state.about
                                          : std::string("; the displays show the sound playing now");
                    if (!asked.drawn.empty())
                        out.about += (rendered ? ", and " : ", but ") + asked.drawn + " (as asked)";
                    out.about += ".";
                    if (!asked.refused.empty())
                        out.about += " Not drawn: " + asked.refused.substr(0, asked.refused.size() - 1);
                    if (!asked.unknown.empty())
                    {
                        const std::string fields = fieldsOf(spec);
                        out.about += " No dial or field of the panel is named " + asked.unknown
                                   + (fields.empty() ? std::string(" (it reads no field)") : " (its fields: " + fields + ")") + ".";
                    }
                    // The displays shown, as they read (their labels as drawn), and with at the fields no display
                    // shown reads (a shader's input, a live= mark's, another page's), their values then.
                    std::string reads, others;
                    std::vector<std::string> named;
                    const PanelSpec& shownSpec = panel.panelSpec();
                    for (std::size_t d = 0; d < shownSpec.displays.size() && d < static_cast<std::size_t>(kMaxDisplays); ++d)
                    {
                        const PanelDisplay& x = shownSpec.displays[d];
                        const ui::DisplayItem& item = panel.displayItem(static_cast<int>(d));
                        if (!x.hidden && item.shown() && !item.readout().empty())
                        {
                            const PanelDisplay& shownAs = item.display();
                            reads += (reads.empty() ? "" : ", ") + ui::upper(ui::asciiText(shownAs.label.empty() ? shownAs.name : shownAs.label))
                                   + " " + item.readout();
                            named.push_back(x.source);
                        }
                    }
                    if (rendered && !state.frames.empty())
                    {
                        Meters last = state.frames.back();
                        for (const auto& [tap, v] : asked.shown.taps)
                            if (tap >= 0 && tap < kMaxDisplays)
                                last.taps[tap] = v;
                        for (const PanelDisplay& x : shownSpec.displays)
                            if (!x.audio() && x.tap >= 0 && x.tap < kMaxDisplays
                                && std::find(named.begin(), named.end(), x.source) == named.end())
                            {
                                named.push_back(x.source);
                                others += (others.empty() ? "" : ", ") + x.source + " " + numberText(last.taps[x.tap]);
                            }
                    }
                    if (!reads.empty())
                        out.about += " The displays shown read: " + reads + ".";
                    if (!others.empty())
                        out.about += " The fields no display shown reads, then: " + others + ".";
                }
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
