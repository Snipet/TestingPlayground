// PROBE name=gui.panel timeout=240
//
// gui.panel: a program's interface from a .gui file (fminus/GuiSpec.h) in the editor's Panel, headless, over a real
// Processor, written to <build>/probe-out/:
//   gui.panel.code.png    the CODE view: the interface under the editor
//   gui.panel.png         the PANEL view: the interface across the left column; .paper.png in PAPER
//   gui.panel.meters.png  its METERS page, audio flowing (meter, VU, light, number, scope, spectrum)
//   gui.panel.small.png   the least size, CODE view, scrolled to its end
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
#include "Probe.h"

#include "editor/DisplayItem.h"
#include "editor/GuiItems.h"
#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplaygroundsynth;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;
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
    void load(tp::Processor& p, const tp::Sources& files)
    {
        p.editorPort().setDraft({ files, 0 });
        juce::MemoryBlock state;
        p.getStateInformation(state);
        p.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        p.editorPort().run(files);
    }

    void settleRun(tp::Processor& p, funkgui::HeadlessHost& host)
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
    std::vector<funkgui::Rect> shownCells(const tp::ui::Panel& panel)
    {
        std::vector<funkgui::Rect> out;
        for (int c = 0; c < tp::kMaxControls; ++c)
            if (panel.guiControl(c).shown())
                out.push_back(panel.guiControl(c).cell());
        for (int k = 0; k < 8; ++k)
        {
            const auto& pad = static_cast<const tp::ui::GuiXyItem&>(panel.guiItem(0, k));
            if (pad.shown())
                out.push_back(pad.pad());
        }
        for (int d = 0; d < tp::kMaxDisplays; ++d)
            if (panel.displayItem(d).shown())
                out.push_back(panel.displayItem(d).cell());
        return out;
    }

    float hzOfPoint(int k)
    {
        return 20.0f * std::pow(1000.0f, static_cast<float>(k) / static_cast<float>(tp::ui::DisplayItem::kSpectrumPoints - 1));
    }
}

PROBE(gui, panel)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    funkgui::UiPreferences::get().setTheme(0);
    tp::Processor processor;
    tp::EditorPort& port = processor.editorPort();
    tp::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();

    // ---- CODE ----------------------------------------------------------------------------------------------------------
    load(processor, { { tp::kProgramName, program() }, { "panel.gui", kGui } });
    settleRun(processor, host);
    P.eq("code.compiles", port.status().ok ? 1 : 0, 1);
    if (!port.status().notes.empty() || !port.status().errors.empty())
        std::printf("notes:\n%s%s", port.status().errors.c_str(), port.status().notes.c_str());
    P.eq("code.no_notes", port.status().notes.empty() ? 1 : 0, 1);
    P.eq("code.has_gui", panel.hasGui() ? 1 : 0, 1);
    P.eq("code.dials_give_way", panel.centreOfDial(cutoff).has_value() ? 0 : 1, 1);
    const tp::ui::GuiLayout* L = &panel.guiLayout();
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
            tp::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            P.eq("view.kept_by_processor", paper.panelView() ? 1 : 0, 1);
            writePng(P, paperHost, "gui.panel.paper.png", "view.paper");
            funkgui::UiPreferences::get().setTheme(0);
        }
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        tp::Processor second;
        second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("view.in_session", second.editorPort().panelView() ? 1 : 0, 1);
    }

    // ---- CONTROLS ------------------------------------------------------------------------------------------------------
    {
        funkgui::ParamPort& cut = *processor.ports()[cutoff];
        const tp::ui::GuiControlItem& knob = panel.guiControl(cutoff);
        P.eq("controls.knob_large", knob.shown() && knob.cell().w >= 84.0f && knob.cell().h >= 108.0f ? 1 : 0, 1);
        const float before = cut.value01();      // 1000 Hz: 0.57 on its log taper
        const funkgui::Point k = knob.centre();
        host.drag(k.x, k.y, k.x, k.y - 50.0f, 10);
        host.settle();
        P.near("controls.knob_drag", static_cast<double>(cut.value01() - before), 0.25, 0.02);
        host.doubleClick(k.x, k.y);
        host.settle();
        P.near("controls.knob_default", static_cast<double>(tp::plainOf(processor.control(cutoff), cut.value01())), 1000.0, 1.0);
        processor.getParameters()[cutoff]->setValueNotifyingHost(0.5f);
        host.settle();
        const std::string middle = processor.getParameters()[cutoff]->getCurrentValueAsText().toStdString();
        std::printf("the cutoff knob's middle: %s\n", middle.c_str());
        P.eq("controls.log_middle_text", middle == "632 Hz" ? 1 : 0, 1);

        // A row of buttons: the third chooses TRI.
        const tp::ui::GuiControlItem& buttons = panel.guiControl(wave);
        const funkgui::Rect b = buttons.cell();
        host.click(b.x + b.w * 5.0f / 6.0f, b.y + 26.0f);
        host.settle();
        P.near("controls.buttons", static_cast<double>(tp::plainOf(processor.control(wave), processor.ports()[wave]->value01())), 2.0, 1e-4);
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
        const tp::ui::GuiControlItem& slider = panel.guiControl(decay);
        const funkgui::Rect tr = slider.track();
        const float y0 = tr.bottom() - tr.h * dec.value01();
        const float d0 = dec.value01();
        host.drag(tr.centreX(), y0, tr.centreX(), y0 - 0.25f * tr.h, 8);
        host.settle();
        P.near("controls.slider_drag", static_cast<double>(dec.value01() - d0), 0.25, 0.03);
        // The pad, on the first page: a drag to its top right corner's quarter.
        const auto& pad = static_cast<const tp::ui::GuiXyItem&>(panel.guiItem(0, 0));
        P.eq("controls.pad_shown", pad.shown() ? 1 : 0, 1);
        const funkgui::Rect r = pad.pad();
        host.drag(r.x + 0.5f * r.w, r.y + 0.5f * r.h, r.x + 0.75f * r.w, r.y + 0.25f * r.h, 6);
        host.settle();
        P.near("controls.pad_x", static_cast<double>(processor.ports()[level]->value01()), 0.75, 0.02);
        P.near("controls.pad_y", static_cast<double>(processor.ports()[pan]->value01()), 0.75, 0.02);
    }

    // ---- ENVELOPE ------------------------------------------------------------------------------------------------------
    {
        const auto& env = static_cast<const tp::ui::GuiEnvelopeItem&>(panel.guiItem(1, 0));
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
        const auto& tabs = static_cast<const tp::ui::GuiTabsItem&>(panel.guiItem(2, 0));
        P.eq("tabs.two", tabs.shown() && tabs.tabs().size() == 2 ? 1 : 0, 1);
        int displaysShown = 0;
        for (int d = 0; d < tp::kMaxDisplays; ++d)
            displaysShown += panel.displayItem(d).shown() ? 1 : 0;
        P.eq("tabs.first_page", displaysShown == 0 && static_cast<const tp::ui::GuiXyItem&>(panel.guiItem(0, 0)).shown() ? 1 : 0, 1);
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
        for (int d = 0; d < tp::kMaxDisplays; ++d)
            displaysShown += panel.displayItem(d).shown() ? 1 : 0;
        P.eq("tabs.second_page", displaysShown == 6 && !static_cast<const tp::ui::GuiXyItem&>(panel.guiItem(0, 0)).shown() ? 1 : 0, 1);
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
        const tp::ui::DisplayItem* spectrum = nullptr;
        const tp::ui::DisplayItem* meter = nullptr;
        const tp::ui::DisplayItem* number = nullptr;
        const tp::ui::DisplayItem* light = nullptr;
        for (int d = 0; d < tp::kMaxDisplays; ++d)
        {
            const tp::ui::DisplayItem& item = panel.displayItem(d);
            if (!item.shown())
                continue;
            if (item.display().kind == tp::DisplayKind::spectrum)
                spectrum = &item;
            if (item.display().kind == tp::DisplayKind::meter)
                meter = &item;
            if (item.display().kind == tp::DisplayKind::number)
                number = &item;
            if (item.display().kind == tp::DisplayKind::led)
                light = &item;
        }
        const double outDb = kInstrument ? -12.04 : -6.02;
        const float hz = kInstrument ? 440.0f : 600.0f;
        if (meter != nullptr)
            P.near("audio.meter", std::atof(meter->readout().c_str()), outDb, 0.4);
        if (number != nullptr)
            P.eq("audio.number", number->readout() == "7.000" ? 1 : 0, 1);
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
        host.resize(tp::ui::kMinWidth, tp::ui::kMinHeight);
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
        host.resize(tp::ui::kDefaultWidth, tp::ui::kDefaultHeight);
        host.settle();
    }

    // ---- PRAGMAS -------------------------------------------------------------------------------------------------------
    {
        const std::string pragmas = "#pragma panel section \"ONE\" cutoff resonance\n" + program();
        load(processor, { { tp::kProgramName, pragmas } });
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
    return P.finish();
}
