// PROBE name=gui.panel timeout=240
//
// gui.panel: a program's interface from a .gui file (fminus/GuiSpec.h) in the editor's Panel, headless, over a real
// Processor, written to <build>/probe-out/:
//   gui.panel.code.png    the CODE view: the interface under the editor
//   gui.panel.png         the PANEL view: the interface across the left column; .paper.png in PAPER
//   gui.panel.meters.png  its METERS page, audio flowing (meter, VU, light, number, scope, spectrum)
//   gui.panel.small.png   the least size, CODE view, scrolled to its end
//   gui.panel.readouts.png  READOUTS' panel: number displays of Shared's fields and of the main module's, formatted
//   gui.panel.state.png     STATE's picture Panel at its frames' last; .picture.png view_panel's picture with values
// and checks:
//   CODE      the program runs with no note; the dials' own items give way to the interface's; every item it shows is
//             inside its area, in the left column, none overlapping another; the area under the problems list
//   VIEW      the toolbar's PANEL cell shows the PANEL view: the tabs, the editor and the problems go, the interface
//             starts under the toolbar and fits; the processor keeps the choice, and its state carries it to another
//             processor; CODE brings the editor back
//   CONTROLS  a drag up on the large knob raises its parameter by a quarter for 50 px, and a double-click returns it to
//             its default; the knob's log taper puts 632 Hz at its middle (the host's text says so); a click on a
//             button of a row of buttons chooses its value, a click on the switch flips it, a click on the menu opens
//             the host's menu of its values (the chosen one ticked) and choosing one sets it; a slider's drag up
//             raises it; a drag on the pad moves both of its parameters; one host gesture each
//   ENVELOPE  the envelope draws its four parameters: its corners in order left to right, the peak at the top, the
//             sustain at its level
//   TABS      the first page shows (the pad), the second's items do not; a click on the second tab shows it (its
//             displays) and hides the first's; accessibility has the tabs as a radio group
//   AUDIO     with audio through the processor the meter reads the output's peak, the light and the number the
//             fields, and the spectrum's strongest point is at the sine's frequency and level (a point either way,
//             3 dB)
//   SCROLL    at the least size the CODE view's interface is taller than its strip: the wheel over its ground
//             scrolls it (its items move up, none drawn outside its area), and over a knob it turns the knob instead
//   PRAGMAS   a program without a .gui file in the PANEL view: its #pragma panel dials from the column's top
//   READOUTS  a program whose Shared's init sets its fields, with no note played: number displays of shared->level
//             (0.750), of an int, shared->notes (3, not 3.000), of shared->speed with unit="rpm" decimals=0 scale=60
//             (1250 rpm, as a knob reads), and a meter of shared->level at three quarters, all whether a voice sounds
//             or not; the main module's int field a whole number too; a readout with a unit (-1200.0 cents) and one of
//             twelve digits each drawn whole inside its box
//   STATE     a picture's state (Panel::showState, view_panel's values and at): a value for a knob's live= field drawn
//             as its mark before the audio thread plays the program (none without it); frames shown at the first until
//             they play, then one a frame to the last (a number's readout, a field scope's points one a frame), a tap and
//             the output's level over each; picturePanel with values: Shared's field, a dial and the output's level drawn
//             as asked (the text says so, and what the displays read), a name that is none listed with the fields, the
//             dial moving the picture and not the user's dial; at's dials drawn as the render left them, and its state
//             refused once the program that plays is another (gui.panel.state.png, gui.panel.state.picture.png)
#include "Probe.h"

#include "editor/DisplayItem.h"
#include "editor/GuiItems.h"
#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    enum Slot : int { cutoff, resonance, attack, decay, sustain, release, wave, syncSlot, mode, voices, level, pan };

    std::string program()
    {
        const std::string body = "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                                 "    param float resonance [[fminus::range(0, 1)]] [[fminus::default(0.2)]];\n"
                                 "    param float attack [[fminus::range(0.001, 2)]] [[fminus::default(0.1)]];\n"
                                 "    param float decay [[fminus::range(0.001, 2)]] [[fminus::default(0.3)]];\n"
                                 "    param float sustain [[fminus::range(0, 1)]] [[fminus::default(0.6)]];\n"
                                 "    param float release [[fminus::range(0.001, 4)]] [[fminus::default(0.5)]];\n"
                                 "    param int wave [[fminus::range(0, 2)]] [[fminus::default(0)]];\n"
                                 "    param bool sync [[fminus::default(0)]];\n"
                                 "    param int mode [[fminus::range(0, 1)]] [[fminus::default(0)]];\n"
                                 "    param int voices [[fminus::range(1, 16)]] [[fminus::default(4)]];\n"
                                 "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                 "    param float pan [[fminus::range(-1, 1)]] [[fminus::default(0)]];\n"
                                 "    float env;\n    bool gate;\n    int count;\n    float ph;\n    float step;\n";
        const std::string fields = "        env = 0.5f;\n        gate = true;\n        count = 7;\n";
        if constexpr (kInstrument)
            return "module Voice\n{\n" + body + "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            const float s = 0.25f * sinf(6.28318531f * ph);\n"
                   "            ph += step;\n            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n        }\n"
                   + fields + "        return true;\n    }\n};\n";
        else
            return "module Effect\n{\n" + body + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   + fields + "    }\n};\n";
    }

    const char* const kGui = R"GUI(// The probe's interface.
panel accent=orange {
    row {
        label "PANEL PROBE" size=title
        spacer
        label "EVERY ELEMENT" size=small
    }
    row gap=10 {
        section "FILTER" layout=column {
            knob cutoff size=large taper=log unit="Hz"
            knob resonance style=vintage
        }
        section "ENVELOPE" {
            slider attack
            slider decay
            slider sustain scale=100 unit="%" decimals=0
            slider release
            envelope attack decay sustain release label="AMP"
        }
        box {
            buttons wave "SAW" "SQR" "TRI" horizontal
            switch sync
            menu mode "POLY" "MONO"
        }
    }
    tabs {
        page "MOD" {
            row {
                xy level pan label="LEVEL / PAN"
                divider
                knob voices size=small
            }
        }
        page "METERS" layout=row {
            meter out
            vu out
            led gate label="GATE"
            number count min=0 max=16
            scope env
            spectrum out grow=1
        }
    }
}
)GUI";

    // The editor's files as a session restores them (the editor shows them: draftRevision bumps), then run.
    void load(pg::Processor& p, const pg::Sources& files)
    {
        p.editorPort().setDraft({ files, 0 });
        juce::MemoryBlock state;
        p.getStateInformation(state);
        p.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        p.editorPort().run(files);
    }

    void settleRun(pg::Processor& p, funkgui::HeadlessHost& host)
    {
        p.settle();
        host.tick(2);
        host.settle();
    }

    bool writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name, const std::string& key)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        const bool ok = host.writePng(path.c_str(), 2);
        P.eq(key + ".png_written", ok ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
        return ok;
    }

    bool inside(const funkgui::Rect& in, const funkgui::Rect& r)
    {
        return r.x >= in.x - 0.5f && r.y >= in.y - 0.5f && r.right() <= in.right() + 0.5f && r.bottom() <= in.bottom() + 0.5f;
    }

    bool overlap(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x < b.right() - 0.5f && b.x < a.right() - 0.5f && a.y < b.bottom() - 0.5f && b.y < a.bottom() - 0.5f;
    }

    // The interface's items shown, with their places: the controls', the pads', the envelopes', the displays'.
    std::vector<funkgui::Rect> shownCells(const pg::ui::Panel& panel)
    {
        std::vector<funkgui::Rect> out;
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (panel.guiControl(c).shown())
                out.push_back(panel.guiControl(c).cell());
        for (int k = 0; k < 8; ++k)
        {
            const auto& pad = static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, k));
            if (pad.shown())
                out.push_back(pad.pad());
        }
        for (int d = 0; d < pg::kMaxDisplays; ++d)
            if (panel.displayItem(d).shown())
                out.push_back(panel.displayItem(d).cell());
        return out;
    }

    float hzOfPoint(int k)
    {
        return 20.0f * std::pow(1000.0f, static_cast<float>(k) / static_cast<float>(pg::ui::DisplayItem::kSpectrumPoints - 1));
    }
}

PROBE(gui, panel)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    funkgui::UiPreferences::get().setTheme(0);
    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();

    // ---- CODE ----------------------------------------------------------------------------------------------------------
    load(processor, { { pg::kProgramName, program() }, { "panel.gui", kGui } });
    settleRun(processor, host);
    P.eq("code.compiles", port.status().ok ? 1 : 0, 1);
    if (!port.status().notes.empty() || !port.status().errors.empty())
        std::printf("notes:\n%s%s", port.status().errors.c_str(), port.status().notes.c_str());
    P.eq("code.no_notes", port.status().notes.empty() ? 1 : 0, 1);
    P.eq("code.has_gui", panel.hasGui() ? 1 : 0, 1);
    P.eq("code.dials_give_way", panel.centreOfDial(cutoff).has_value() ? 0 : 1, 1);
    const pg::ui::GuiLayout* L = &panel.guiLayout();
    const funkgui::Rect chat = panel.chatCard();
    P.eq("code.area_left_of_chat", L->area.right() <= chat.x ? 1 : 0, 1);
    P.ge("code.area_under_editor", static_cast<double>(L->area.y), 300.0);
    {
        const std::vector<funkgui::Rect> cells = shownCells(panel);
        bool in = true, apart = true;
        for (std::size_t a = 0; a < cells.size(); ++a)
        {
            in = in && inside(L->area, cells[a]);
            for (std::size_t b = a + 1; b < cells.size(); ++b)
                apart = apart && !overlap(cells[a], cells[b]);
        }
        std::printf("code view: %zu items in %g x %g (content %g x %g)\n", cells.size(), static_cast<double>(L->area.w),
                    static_cast<double>(L->area.h), static_cast<double>(L->contentW), static_cast<double>(L->contentH));
        P.ge("code.items", static_cast<double>(cells.size()), 6.0);   // the rest scrolled out of its strip
        P.eq("code.inside", in || L->maxScrollY > 0.0f ? 1 : 0, 1);
        P.eq("code.apart", apart ? 1 : 0, 1);
    }
    writePng(P, host, "gui.panel.code.png", "code");

    // ---- VIEW ----------------------------------------------------------------------------------------------------------
    {
        const funkgui::Point at = panel.centreOfView(true);
        host.click(at.x, at.y);
        host.settle();
        L = &panel.guiLayout();
        P.eq("view.panel", panel.panelView() && port.panelView() ? 1 : 0, 1);
        P.le("view.from_toolbar", static_cast<double>(L->area.y), 100.0);
        P.eq("view.fits", L->maxScrollY == 0.0f && L->maxScrollX == 0.0f ? 1 : 0, 1);
        bool editorGone = true;                  // no text field in the left column (the chat's message box stays)
        for (const funkgui::A11yItem& a : host.accessibility())
            editorGone = editorGone && !(a.role == funkgui::a11yrole::editableText && a.bounds.x < panel.chatCard().x);
        P.eq("view.editor_gone", editorGone ? 1 : 0, 1);
        const std::vector<funkgui::Rect> cells = shownCells(panel);
        bool in = true, apart = true;
        for (std::size_t a = 0; a < cells.size(); ++a)
        {
            in = in && inside(L->area, cells[a]);
            for (std::size_t b = a + 1; b < cells.size(); ++b)
                apart = apart && !overlap(cells[a], cells[b]);
        }
        P.eq("view.inside", in ? 1 : 0, 1);
        P.eq("view.apart", apart ? 1 : 0, 1);
        P.eq("view.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
        writePng(P, host, "gui.panel.png", "view");
        {
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            P.eq("view.kept_by_processor", paper.panelView() ? 1 : 0, 1);
            writePng(P, paperHost, "gui.panel.paper.png", "view.paper");
            funkgui::UiPreferences::get().setTheme(0);
        }
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        pg::Processor second;
        second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("view.in_session", second.editorPort().panelView() ? 1 : 0, 1);
    }

    // ---- CONTROLS ------------------------------------------------------------------------------------------------------
    {
        funkgui::ParamPort& cut = *processor.ports()[cutoff];
        const pg::ui::GuiControlItem& knob = panel.guiControl(cutoff);
        P.eq("controls.knob_large", knob.shown() && knob.cell().w >= 86.0f && knob.cell().h >= 106.0f ? 1 : 0, 1);   // 86 x 106
        const float before = cut.value01();      // 1000 Hz: 0.57 on its log taper
        const funkgui::Point k = knob.centre();
        host.drag(k.x, k.y, k.x, k.y - 50.0f, 10);
        host.settle();
        P.near("controls.knob_drag", static_cast<double>(cut.value01() - before), 0.25, 0.02);
        host.doubleClick(k.x, k.y);
        host.settle();
        P.near("controls.knob_default", static_cast<double>(pg::plainOf(processor.control(cutoff), cut.value01())), 1000.0, 1.0);
        processor.getParameters()[cutoff]->setValueNotifyingHost(0.5f);
        host.settle();
        const std::string middle = processor.getParameters()[cutoff]->getCurrentValueAsText().toStdString();
        std::printf("the cutoff knob's middle: %s\n", middle.c_str());
        P.eq("controls.log_middle_text", middle == "632 Hz" ? 1 : 0, 1);

        // A row of buttons: the third chooses TRI.
        const pg::ui::GuiControlItem& buttons = panel.guiControl(wave);
        const funkgui::Rect b = buttons.cell();
        host.click(b.x + b.w * 5.0f / 6.0f, b.y + 26.0f);
        host.settle();
        P.near("controls.buttons", static_cast<double>(pg::plainOf(processor.control(wave), processor.ports()[wave]->value01())), 2.0, 1e-4);
        P.eq("controls.buttons_host_name", processor.getParameters()[wave]->getCurrentValueAsText() == "TRI" ? 1 : 0, 1);
        const funkgui::Point sw = panel.guiControl(syncSlot).centre();
        host.click(sw.x, sw.y);
        host.settle();
        P.near("controls.switch", static_cast<double>(processor.ports()[syncSlot]->value01()), 1.0, 1e-4);
        const funkgui::Point menu = panel.guiControl(mode).centre();
        host.click(menu.x, menu.y + 8.0f);
        const funkgui::MenuRequest* m = host.pendingMenu();
        P.eq("controls.menu_opens", m != nullptr && m->items.size() == 2 && m->items[0].label == "POLY" && m->items[0].checked ? 1 : 0, 1);
        host.chooseMenuItem("MONO");
        host.settle();
        P.near("controls.menu_chooses", static_cast<double>(processor.ports()[mode]->value01()), 1.0, 1e-4);
        // A slider: a drag up from its cap raises it.
        funkgui::ParamPort& dec = *processor.ports()[decay];
        const pg::ui::GuiControlItem& slider = panel.guiControl(decay);
        const funkgui::Rect tr = slider.track();
        const float y0 = tr.bottom() - tr.h * dec.value01();
        const float d0 = dec.value01();
        host.drag(tr.centreX(), y0, tr.centreX(), y0 - 0.25f * tr.h, 8);
        host.settle();
        P.near("controls.slider_drag", static_cast<double>(dec.value01() - d0), 0.25, 0.03);
        // The pad, on the first page: a drag to its top right corner's quarter.
        const auto& pad = static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, 0));
        P.eq("controls.pad_shown", pad.shown() ? 1 : 0, 1);
        const funkgui::Rect r = pad.pad();
        host.drag(r.x + 0.5f * r.w, r.y + 0.5f * r.h, r.x + 0.75f * r.w, r.y + 0.25f * r.h, 6);
        host.settle();
        P.near("controls.pad_x", static_cast<double>(processor.ports()[level]->value01()), 0.75, 0.02);
        P.near("controls.pad_y", static_cast<double>(processor.ports()[pan]->value01()), 0.75, 0.02);
    }

    // ---- ENVELOPE ------------------------------------------------------------------------------------------------------
    {
        const auto& env = static_cast<const pg::ui::GuiEnvelopeItem&>(panel.guiItem(1, 0));
        P.eq("envelope.shown", env.shown() ? 1 : 0, 1);
        const std::array<funkgui::Point, 5> p = env.corners();
        bool ordered = true;
        for (std::size_t k = 0; k + 1 < p.size(); ++k)
            ordered = ordered && p[k + 1].x >= p[k].x;
        P.eq("envelope.ordered", ordered ? 1 : 0, 1);
        P.eq("envelope.peak_top", p[1].y < p[2].y && p[0].y == p[4].y ? 1 : 0, 1);
        const float s = processor.control(sustain).def;
        P.near("envelope.sustain_level", static_cast<double>((p[0].y - p[2].y) / (p[0].y - p[1].y)), static_cast<double>(s), 0.02);
    }

    // ---- TABS ----------------------------------------------------------------------------------------------------------
    {
        const auto& tabs = static_cast<const pg::ui::GuiTabsItem&>(panel.guiItem(2, 0));
        P.eq("tabs.two", tabs.shown() && tabs.tabs().size() == 2 ? 1 : 0, 1);
        int displaysShown = 0;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
            displaysShown += panel.displayItem(d).shown() ? 1 : 0;
        P.eq("tabs.first_page", displaysShown == 0 && static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, 0)).shown() ? 1 : 0, 1);
        uint32_t group = 0;
        int buttons = 0;
        const std::vector<funkgui::A11yItem> a11y = host.accessibility();
        for (const funkgui::A11yItem& a : a11y)
            if (a.role == funkgui::A11yRole::radioGroup && a.title == "Pages")
                group = a.id;
        for (const funkgui::A11yItem& a : a11y)
            buttons += group != 0 && a.parent == group && a.role == funkgui::A11yRole::radioButton ? 1 : 0;
        P.eq("tabs.a11y_group", group != 0 && buttons == 2 ? 1 : 0, 1);
        if (tabs.tabs().size() == 2)
        {
            const funkgui::Rect t = tabs.tabs()[1];
            host.click(t.centreX(), t.centreY());
            host.settle();
        }
        displaysShown = 0;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
            displaysShown += panel.displayItem(d).shown() ? 1 : 0;
        P.eq("tabs.second_page", displaysShown == 6 && !static_cast<const pg::ui::GuiXyItem&>(panel.guiItem(0, 0)).shown() ? 1 : 0, 1);
    }

    // ---- AUDIO ---------------------------------------------------------------------------------------------------------
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;
        processor.prepareToPlay(kRate, kBlock);
        juce::AudioBuffer<float> buffer(2, kBlock);
        double phase = 0.0;
        const auto push = [&](int blocks, bool noteOn) {
            for (int b = 0; b < blocks; ++b)
            {
                juce::MidiBuffer midi;
                if (noteOn && b == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
                for (int i = 0; i < kBlock; ++i)
                {
                    const float v = kInstrument ? 0.0f : static_cast<float>(0.5 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 600.0 / kRate;
                    buffer.setSample(0, i, v);
                    buffer.setSample(1, i, v);
                }
                processor.processBlock(buffer, midi);
            }
        };
        push(1, true);
        for (int k = 0; k < 120; ++k)
        {
            push(1, false);
            host.tick(1);
        }
        const pg::ui::DisplayItem* spectrum = nullptr;
        const pg::ui::DisplayItem* meter = nullptr;
        const pg::ui::DisplayItem* number = nullptr;
        const pg::ui::DisplayItem* light = nullptr;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
        {
            const pg::ui::DisplayItem& item = panel.displayItem(d);
            if (!item.shown())
                continue;
            if (item.display().kind == pg::DisplayKind::spectrum)
                spectrum = &item;
            if (item.display().kind == pg::DisplayKind::meter)
                meter = &item;
            if (item.display().kind == pg::DisplayKind::number)
                number = &item;
            if (item.display().kind == pg::DisplayKind::led)
                light = &item;
        }
        const double outDb = kInstrument ? -12.04 : -6.02;
        const float hz = kInstrument ? 440.0f : 600.0f;
        if (meter != nullptr)
            P.near("audio.meter", std::atof(meter->readout().c_str()), outDb, 0.4);
        if (number != nullptr)
            P.eq("audio.number", number->readout() == "7" ? 1 : 0, 1);   // count, an int: a whole number
        if (light != nullptr)
            P.near("audio.light", static_cast<double>(light->level()), 1.0, 0.01);
        P.eq("audio.spectrum_shown", spectrum != nullptr && !spectrum->spectrum().empty() ? 1 : 0, 1);
        if (spectrum != nullptr && !spectrum->spectrum().empty())
        {
            const std::vector<float>& s = spectrum->spectrum();
            const auto top = std::max_element(s.begin(), s.end());
            const int k = static_cast<int>(top - s.begin());
            std::printf("spectrum: strongest %.1f dB at %.0f Hz\n", static_cast<double>(*top), static_cast<double>(hzOfPoint(k)));
            P.in("audio.spectrum_hz", static_cast<double>(hzOfPoint(k)), static_cast<double>(hz) / 1.08, static_cast<double>(hz) * 1.08);
            P.near("audio.spectrum_db", static_cast<double>(*top), outDb, 3.0);
        }
        writePng(P, host, "gui.panel.meters.png", "audio");
    }

    // ---- SCROLL --------------------------------------------------------------------------------------------------------
    {
        const funkgui::Point code = panel.centreOfView(false);
        host.click(code.x, code.y);
        host.settle();
        P.eq("scroll.code_view", !panel.panelView() ? 1 : 0, 1);
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        host.settle();
        L = &panel.guiLayout();
        std::printf("least size: the interface's area %g tall, content %g\n", static_cast<double>(L->area.h), static_cast<double>(L->contentH));
        P.eq("scroll.taller", L->maxScrollY > 0.0f ? 1 : 0, 1);
        // Over the ground (the area's right edge, past the items): the interface scrolls.
        const float y0 = panel.guiControl(cutoff).cell().y;
        host.wheel(L->area.right() - 6.0f, L->area.y + 10.0f, -0.5f);
        host.settle();
        L = &panel.guiLayout();
        P.eq("scroll.moved_up", panel.guiControl(cutoff).cell().y < y0 ? 1 : 0, 1);
        // Over a knob it turns the knob.
        funkgui::ParamPort& res = *processor.ports()[resonance];
        const float r0 = res.value01();
        if (panel.guiControl(resonance).shown())
        {
            const funkgui::Point c = panel.guiControl(resonance).centre();
            if (L->area.contains(c))
            {
                const float scrolled = L->maxScrollY;
                host.wheel(c.x, c.y, 0.2f);
                host.settle();
                P.eq("scroll.knob_takes_wheel", res.value01() != r0 && panel.guiLayout().maxScrollY == scrolled ? 1 : 0, 1);
            }
        }
        host.wheel(L->area.right() - 6.0f, L->area.y + 10.0f, -5.0f);
        host.settle();
        writePng(P, host, "gui.panel.small.png", "scroll");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
    }

    // ---- PRAGMAS -------------------------------------------------------------------------------------------------------
    {
        const std::string pragmas = "#pragma panel section \"ONE\" cutoff resonance\n" + program();
        load(processor, { { pg::kProgramName, pragmas } });
        settleRun(processor, host);
        P.eq("pragmas.no_gui", !panel.hasGui() ? 1 : 0, 1);
        const funkgui::Point at = panel.centreOfView(true);
        host.click(at.x, at.y);
        host.settle();
        P.le("pragmas.panel_view_top", static_cast<double>(panel.dialsLayout().top), 100.0);
        P.eq("pragmas.dials_shown", panel.centreOfDial(cutoff).has_value() ? 1 : 0, 1);
        const funkgui::Point code = panel.centreOfView(false);
        host.click(code.x, code.y);
        host.settle();
    }

    // ---- READOUTS ------------------------------------------------------------------------------------------------------
    {
        const std::string shared = "module Shared\n{\n    float level, speed, detune, big;\n    int notes;\n"
                                   "    void init(float rate) { level = 0.75f; notes = 3; speed = 20.8333333f; detune = -1200; big = 123456789012.0f; }\n"
                                   "};\n\n";
        const std::string main = kInstrument
                                     ? "module Voice\n{\n    Shared* shared;\n    param float gain [[fminus::default(0.5)]];\n    int count;\n"
                                       "    void start(float n, float v) { count = 3; }\n"
                                       "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                                     : "module Effect\n{\n    Shared* shared;\n    param float gain [[fminus::default(0.5)]];\n    int count;\n"
                                       "    void process(float* restrict left, float* restrict right, int n) { count = 3; }\n};\n";
        const std::string readoutGui = "row gap=10 {\n"
                                "    section \"SHARED\" {\n"
                                "        number shared->level label=\"LEVEL\"\n"
                                "        number shared->notes label=\"NOTES\"\n"
                                "        number shared->speed label=\"SPEED\" unit=\"rpm\" decimals=0 scale=60\n"
                                "        meter shared->level label=\"METER\"\n"
                                "        number shared->detune label=\"DETUNE\" unit=\"cents\" decimals=1\n"
                                "        number shared->big label=\"BIG\"\n"
                                "    }\n"
                                "    section \"VOICE\" {\n"
                                "        knob gain\n"
                                "        number count label=\"COUNT\"\n"
                                "    }\n"
                                "}\n";
        load(processor, { { pg::kProgramName, shared + main }, { "panel.gui", readoutGui } });
        settleRun(processor, host);
        const pg::CompileStatus st = port.status();
        P.eq("readouts.runs_with_no_note", st.ok && st.notes.empty() ? 1 : 0, 1);
        if (!st.notes.empty())
            std::printf("%s", st.notes.c_str());
        const funkgui::Point view = panel.centreOfView(true);
        host.click(view.x, view.y);
        host.settle();
        processor.prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> buffer(2, 256);
        for (int k = 0; k < 60; ++k)                     // no note: the synth's voices are silent
        {
            buffer.clear();
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            host.tick(1);
        }
        host.settle();
        std::string level, notes, speed, count, detune;
        float meterLevel = -1.0f;
        // A readout's glyphs whole in its box (looks::number's: the cell less 2 each side, from 18 down, 30 high): as
        // many text quads centred in it as it has characters but spaces.
        const funkgui::PrimList& frame = host.draw();
        const auto inBox = [&](const pg::ui::DisplayItem& item) {
            const funkgui::Rect& r = item.cell();
            const funkgui::Rect box{ r.x + 2.0f, r.y + 18.0f, r.w - 4.0f, 30.0f };
            int glyphs = 0;
            for (const funkgui::Prim& q : frame.prims)
                if (static_cast<int>(q.d2[2] + 0.5f) == static_cast<int>(funkgui::PrimKind::text)
                    && box.contains({ 0.5f * (q.x0 + q.x1), 0.5f * (q.y0 + q.y1) }))
                    ++glyphs;
            const auto chars = std::count_if(item.readout().begin(), item.readout().end(), [](char ch) { return ch != ' '; });
            std::printf("readout %s: %d glyphs in its box of %.0f px, %d characters\n", item.readout().c_str(), glyphs,
                        static_cast<double>(box.w), static_cast<int>(chars));
            return glyphs == static_cast<int>(chars);
        };
        bool detuneFits = false, bigFits = false;
        for (int d = 0; d < pg::kMaxDisplays; ++d)
        {
            const pg::ui::DisplayItem& item = panel.displayItem(d);
            if (!item.shown())
                continue;
            const pg::PanelDisplay& x = item.display();
            std::printf("readout %s %s: %s\n", x.source.c_str(), x.label.c_str(), item.readout().c_str());
            if (x.kind == pg::DisplayKind::meter)
                meterLevel = item.level();
            else if (x.label == "LEVEL")
                level = item.readout();
            else if (x.label == "NOTES")
                notes = item.readout();
            else if (x.label == "SPEED")
                speed = item.readout();
            else if (x.label == "COUNT")
                count = item.readout();
            else if (x.label == "DETUNE")
            {
                detune = item.readout();
                detuneFits = inBox(item);
            }
            else if (x.label == "BIG")
                bigFits = inBox(item);
        }
        P.eq("readouts.shared_float", level == "0.750" ? 1 : 0, 1);
        P.eq("readouts.shared_int_whole", notes == "3" ? 1 : 0, 1);
        P.eq("readouts.formatted_as_a_knob", speed == "1250 rpm" ? 1 : 0, 1);
        // A unit's readout has a cell wide enough; a field's huge value is drawn smaller: neither spills out of its box.
        P.eq("readouts.unit_formatted", detune == "-1200.0 cents" ? 1 : 0, 1);
        P.eq("readouts.unit_in_its_box", detuneFits ? 1 : 0, 1);
        P.eq("readouts.huge_in_its_box", bigFits ? 1 : 0, 1);
        P.near("readouts.shared_meter", static_cast<double>(meterLevel), 0.75, 0.01);
        // The main module's int: the effect's process writes 3; the synth's voice sounds none, so it reads 0.
        P.eq("readouts.main_int_whole", count == (kInstrument ? "0" : "3") ? 1 : 0, 1);
        writePng(P, host, "gui.panel.readouts.png", "readouts");
    }

    // ---- STATE ---------------------------------------------------------------------------------------------------------
    {
        const std::string shared = "module Shared\n{\n    float level;\n    int notes;\n"
                                   "    void init(float rate) { level = 0.75f; notes = 3; }\n};\n\n";
        const std::string main = kInstrument
                                     ? "module Voice\n{\n    Shared* shared;\n    param float gain [[fminus::default(0.5)]];\n"
                                       "    void start(float n, float v) {}\n"
                                       "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                                     : "module Effect\n{\n    Shared* shared;\n    param float gain [[fminus::default(0.5)]];\n"
                                       "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const std::string stateGui = "row gap=10 {\n"
                                     "    section \"STATE\" {\n"
                                     "        knob gain live=shared->level\n"
                                     "        number shared->level label=\"LEVEL\"\n"
                                     "        number shared->notes label=\"NOTES\"\n"
                                     "        scope shared->level label=\"TRACE\"\n"
                                     "        meter out label=\"OUT\"\n"
                                     "    }\n"
                                     "}\n";
        load(processor, { { pg::kProgramName, shared + main }, { "panel.gui", stateGui } });
        settleRun(processor, host);
        const pg::PanelSpec spec = port.status().panel;
        int levelTap = -1, traceTap = -1, notesTap = -1, liveTap = -1, levelD = -1, notesD = -1, traceD = -1, outD = -1;
        for (std::size_t d = 0; d < spec.displays.size(); ++d)
        {
            const pg::PanelDisplay& x = spec.displays[d];
            const int i = static_cast<int>(d);
            if (x.hidden)
                liveTap = x.source == "shared->level" ? x.tap : liveTap;   // the knob's live= mark's
            else if (x.source == "shared->level" && x.kind == pg::DisplayKind::number)
                levelTap = x.tap, levelD = i;
            else if (x.source == "shared->level" && x.kind == pg::DisplayKind::scope)
                traceTap = x.tap, traceD = i;
            else if (x.source == "shared->notes")
                notesTap = x.tap, notesD = i;
            else if (x.source == "out")
                outD = i;
        }
        // values alone (view_panel's), before the audio thread has played this program: a live= mark shows no value of
        // its own then, but the one asked for.
        {
            const auto liveMark = [&](const pg::ui::Panel::ShownState* st) {
                pg::ui::Panel early(processor.ports(), port, true);
                if (st != nullptr)
                    early.showState(*st);
                funkgui::HeadlessHost h(early, 0, 1.5f);
                h.settle(120);
                early.setPanelView(true);
                h.settle(120);
                return early.hasGui() ? early.guiControl(0).live01() : -2.0f;
            };
            pg::ui::Panel::ShownState asked;
            asked.taps.emplace_back(liveTap, 0.6f);
            const float none = liveMark(nullptr), drawn = liveMark(&asked);
            std::printf("state, a live= mark before the program plays: %.3f, asked 0.6: %.3f\n", static_cast<double>(none),
                        static_cast<double>(drawn));
            P.eq("state.live_mark_unplayed", liveTap >= 0 && none < 0.0f ? 1 : 0, 1);
            P.eq("state.live_mark_asked", std::fabs(drawn - 0.6f) < 1.0e-4f ? 1 : 0, 1);
        }
        juce::AudioBuffer<float> buffer(2, 256);
        for (int k = 0; k < 8; ++k)                      // its fields published (Shared's init set them)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
        }
        P.eq("state.displays", port.status().ok && levelTap >= 0 && notesTap >= 0 && traceD >= 0 && outD >= 0 ? 1 : 0, 1);
        // A picture's Panel (Panel::showState, view_panel's at): 40 frames, the field rising; held at the first until it
        // plays, then one a frame, the last kept; a tap and the output's level over every frame.
        pg::ui::Panel pic(processor.ports(), port, true);
        pg::ui::Panel::ShownState st;
        for (int k = 0; k < 40; ++k)
        {
            pg::Meters m;
            for (const int t : { levelTap, traceTap })      // each display of the field has a tap of its own
                if (t >= 0)
                    m.taps[t] = 0.1f + 0.02f * static_cast<float>(k);
            m.frame = static_cast<uint32_t>(k + 1);
            st.frames.push_back(m);
        }
        if (notesTap >= 0)
            st.taps.emplace_back(notesTap, 9.0f);
        st.outLevel = 0.5f;
        pic.showState(st);
        funkgui::HeadlessHost picHost(pic, 0, 1.5f);
        picHost.settle(120);
        pic.setPanelView(true);
        picHost.settle(120);
        const auto readout = [&](int d) { return d >= 0 ? pic.displayItem(d).readout() : std::string(); };
        const std::string first = readout(levelD);
        pic.playShown();
        int played = 0;
        for (; played < 200 && !pic.shownDone(); ++played)
            picHost.tick(1);
        picHost.tick(1);
        std::printf("state: LEVEL %s at the first frame, %s after %d frames; NOTES %s, OUT %s, TRACE %d points\n", first.c_str(),
                    readout(levelD).c_str(), played, readout(notesD).c_str(), readout(outD).c_str(),
                    traceD >= 0 ? pic.displayItem(traceD).traceSize() : -1);
        P.eq("state.held_at_first", first == "0.100" ? 1 : 0, 1);
        P.eq("state.played_to_last", played == 39 && readout(levelD) == "0.880" ? 1 : 0, 1);
        P.eq("state.tap_over_frames", readout(notesD) == "9" ? 1 : 0, 1);
        P.eq("state.level_over_frames", readout(outD) == "-6.0" ? 1 : 0, 1);
        P.eq("state.trace_of_frames", traceD >= 0 ? pic.displayItem(traceD).traceSize() : -1, 40);
        writePng(P, picHost, "gui.panel.state.png", "state");
        // picturePanel with values: Shared's field, a dial, the output's level, a name that is none; the dial's value
        // moves the picture.
        pg::PictureState ps;
        const auto value = [](const char* name, float v) {
            pg::PictureValue x;
            x.name = name;
            x.number = v;
            return x;
        };
        ps.values = { value("shared->level", 0.25f), value("gain", 0.9f), value("out", -12.0f), value("zzz", 1.0f) };
        const pg::PanelPicture a = pg::picturePanel(processor.ports(), port, 0, 0, false, "", ps);
        ps.values[1].number = 0.1f;
        const pg::PanelPicture b = pg::picturePanel(processor.ports(), port, 0, 0, false, "", ps);
        std::printf("state, a picture: %s\n", a.about.c_str());
        const auto in = [](const std::string& t, const char* what) { return t.find(what) != std::string::npos; };
        P.eq("state.picture_said", in(a.about, "but shared->level 0.25, gain 0.90, out -12 dBFS (as asked).") && in(a.about, "LEVEL 0.250")
                                       && in(a.about, "NOTES 3") && in(a.about, "OUT -12.0") ? 1 : 0, 1);
        P.eq("state.picture_unknown", in(a.about, "No dial or field of the panel is named zzz (its fields: shared->level, shared->notes).") ? 1 : 0, 1);
        P.eq("state.picture_dial_drawn", !a.png.empty() && !b.png.empty() && a.png != b.png ? 1 : 0, 1);
        P.eq("state.picture_user_dial", processor.getParameters()[0]->getValue() == 0.5f ? 1 : 0, 1);
        // at's state: the dials as the render left them (PictureState::dials) drawn, the user's unmoved; and refused
        // when the program that plays is no longer the one rendered.
        pg::PictureState at;
        at.frames.resize(1);
        at.sources = port.status().sources;
        at.about = "0.50 s into a render";
        at.dials = { { "gain", 0.9f } };
        const pg::PanelPicture high = pg::picturePanel(processor.ports(), port, 0, 0, false, "", at);
        at.dials = { { "gain", 0.1f } };
        const pg::PanelPicture low = pg::picturePanel(processor.ports(), port, 0, 0, false, "", at);
        P.eq("state.at_dials_drawn", !high.png.empty() && !low.png.empty() && high.png != low.png
                                         && high.about.find("the displays and the dials show the state 0.50 s into a render") != std::string::npos
                                         ? 1 : 0, 1);
        P.eq("state.at_user_dial", processor.getParameters()[0]->getValue() == 0.5f ? 1 : 0, 1);
        if (!at.sources.empty())
            at.sources.front().text += "\n// changed\n";
        const pg::PanelPicture other = pg::picturePanel(processor.ports(), port, 0, 0, false, "", at);
        P.eq("state.at_other_program", other.png.empty() && other.why.find("changed while at rendered") != std::string::npos ? 1 : 0, 1);
        if (!a.png.empty())
            if (std::FILE* f = std::fopen(probe::outPath("gui.panel.state.picture.png").c_str(), "wb"))
            {
                std::fwrite(a.png.data(), 1, a.png.size(), f);
                std::fclose(f);
            }
    }
    return P.finish();
}
