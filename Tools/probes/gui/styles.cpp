// PROBE name=gui.styles timeout=240
//
// gui.styles: what a .gui file can make an interface look like (fminus/GuiSpec.h), in the Panel's PANEL view over a
// real Processor, written to <build>/probe-out/:
//   gui.styles.png         its SYNTH page: a bar of pages, the knobs' nine looks in a row that wraps, sections as a
//                          card, an inset and a plate, rounded, on a ground with a gradient; an instrument's keyboard
//   gui.styles.fx.png      its FX page: a frame with its own corners, fill and border; pills of pages in it
//   gui.styles.mixer.png   its MIXER page: a list of pages at the left
//   gui.styles.narrow.png  the least size: the knobs' row wraps onto more lines
// and checks:
//   PARSE     the interface reads with no note; a defaults line and a class reach their elements (an element's own
//             property beats both)
//   KNOBS     each look draws in a cell its size gives (size=small, a size in px); labelpos=bottom puts the label under
//             the face, value=never leaves the value out; every control inside its section, none overlapping
//   SECTIONS  card, inset and plate are drawn rounded (the frame's radius=, its fill, its border)
//   WRAP      at the least size the knobs' row is more than one line, and no knob leaves its section
//   PAGES     a bar of three pages: the first shows; a click on the third shows it and hides the first's; the processor
//             keeps the page by its title, its state carries it to another processor, whose Panel opens on it; the
//             program run again with an edited interface keeps it; pills and a side list switch their pages too
//   KEYBOARD  (an instrument) a press on a key plays its note: the output sounds, at the key's pitch; letting go
//             releases it (silent); a drag slides to the next key
//   WHEELS    (an instrument) the keyboard's wheels: the pitch wheel dragged up bends +1 and reaches the Engine's Host
//             block (host.fm); let go, it springs back to 0; the mod wheel stays where it is left
//   LIVE      a knob's live= follows its field: the field's value as a share of the parameter's range
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplaygroundsynth;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;
    // The parameters, in their slots.
    enum Slot : int
    {
        drive, tone, colour, mix, width, depth, rate, glide, level,   // the nine looks
        pan, cutoff, res,                                             // SHAPES
        echo, space,                                                  // FX
        busA, busB, busC                                              // MIXER
    };

    std::string program()
    {
        std::string params;
        for (const char* name : { "drive", "tone", "colour", "mix", "width", "depth", "rate", "glide", "level" })
            params += std::string("    param float ") + name + " [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        params += "    param float pan [[fminus::range(-1, 1)]] [[fminus::default(0)]];\n"
                  "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.7)]];\n"
                  "    param float res [[fminus::range(0, 1)]] [[fminus::default(0.3)]];\n"
                  "    param float echo [[fminus::range(0, 1)]] [[fminus::default(0.4)]];\n"
                  "    param float space [[fminus::range(0, 1)]] [[fminus::default(0.6)]];\n"
                  "    param float busA [[fminus::range(0, 1)]] [[fminus::default(0.8)]];\n"
                  "    param float busB [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                  "    param float busC [[fminus::range(0, 1)]] [[fminus::default(0.2)]];\n"
                  "    float lfo;\n";
        if constexpr (kInstrument)
            return "module Voice\n{\n" + params + "    float ph, step;\n    bool held;\n"
                   "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; held = true; }\n"
                   "    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            const float s = 0.25f * sinf(6.28318531f * ph);\n"
                   "            ph += step;\n            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n        }\n"
                   "        lfo = 0.25f;\n        return held;\n    }\n};\n";
        else
            return "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        lfo = 0.25f;\n    }\n};\n";
    }

    std::string gui(const std::string& title)
    {
        const std::string keyboard = kInstrument ? "            keyboard from=48 keys=25 wheels\n" : "";
        return R"GUI(// The styles probe's interface.
class "big" size=72 arc=yellow
defaults knob look=ring
defaults section radius=10
panel background=#1b1d24 gradient=#0e0f13 accent=teal {
    row align=center {
        label ")GUI" + title + R"GUI(" size=22 color=yellow
        spacer
        label "EVERY LOOK" size=small align=right
    }
    tabs look=bar {
        page "SYNTH" {
            section "KNOBS" look=card {
                row wrap gap=8 {
                    knob drive look=modern
                    knob tone look=vintage
                    knob colour look=flat
                    knob mix look=minimal
                    knob width
                    knob depth look=led ticks=11
                    knob rate look=chicken
                    knob glide look=soft
                    knob level look=dot
                }
            }
            row gap=10 {
                section "SHAPES" look=inset grow=1 {
                    knob pan bipolar size=small labelpos=bottom value=never
                    knob cutoff class="big" live=lfo
                    knob res size=96 sweep=300 pointer=triangle thickness=6 look=modern
                }
                section "PLATE" look=plate titlealign=left grow=1 {
                    label "SCREWED ON" size=small
                }
            }
)GUI" + keyboard + R"GUI(        }
        page "FX" {
            section "FRAME" radius=14 fill=#232733 border=yellow borderwidth=2 {
                tabs look=pills {
                    page "ECHO" { knob echo size=large }
                    page "SPACE" { knob space size=large }
                }
            }
        }
        page "MIXER" {
            tabs look=side {
                page "BUS A" { slider busA }
                page "BUS B" { slider busB }
                page "BUS C" { slider busC }
            }
        }
    }
}
)GUI";
    }

    void load(tp::Processor& p, const tp::Sources& files)
    {
        p.editorPort().setDraft({ files, 0 });
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

    // The node of a section titled `title`, -1 for none.
    int sectionNode(const tp::ui::Panel& panel, const char* title)
    {
        const std::vector<tp::GuiNode>& nodes = panel.panelSpec().gui.nodes;
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].kind == tp::GuiKind::section && nodes[i].text == title)
                return static_cast<int>(i);
        return -1;
    }

    // The node of slot's control.
    int controlNode(const tp::ui::Panel& panel, int slot)
    {
        const std::vector<tp::GuiNode>& nodes = panel.panelSpec().gui.nodes;
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if ((nodes[i].kind == tp::GuiKind::knob || nodes[i].kind == tp::GuiKind::slider) && nodes[i].slot == slot)
                return static_cast<int>(i);
        return -1;
    }

    // The output's RMS over `blocks` blocks of 256 at 48 kHz, and the sine's frequency by its zero crossings.
    struct Heard
    {
        double rms = 0.0, hz = 0.0;
    };
    Heard listen(tp::Processor& p, funkgui::HeadlessHost& host, int blocks)
    {
        constexpr int kBlock = 256;
        juce::AudioBuffer<float> buffer(2, kBlock);
        double sum = 0.0;
        int crossings = 0, samples = 0;
        float last = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            p.processBlock(buffer, midi);
            for (int i = 0; i < kBlock; ++i)
            {
                const float v = buffer.getSample(0, i);
                sum += static_cast<double>(v) * v;
                crossings += (last <= 0.0f && v > 0.0f) ? 1 : 0;
                last = v;
                ++samples;
            }
            host.tick(1);
        }
        return { std::sqrt(sum / std::max(1, samples)), crossings * 48000.0 / std::max(1, samples) };
    }
}

PROBE(gui, styles)
{
    const funkgui::HeadlessGuiScope scope;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    funkgui::UiPreferences::get().setTheme(0);
    tp::Processor processor;
    tp::EditorPort& port = processor.editorPort();
    processor.prepareToPlay(48000.0, 256);
    tp::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    {
        const funkgui::Point at = panel.centreOfView(true);
        host.click(at.x, at.y);
        host.settle();
    }

    // ---- PARSE ---------------------------------------------------------------------------------------------------------
    load(processor, { { tp::kProgramName, program() }, { "panel.gui", gui("STYLES") } });
    settleRun(processor, host);
    P.eq("parse.compiles", port.status().ok ? 1 : 0, 1);
    if (!port.status().notes.empty() || !port.status().errors.empty())
        std::printf("notes:\n%s%s", port.status().errors.c_str(), port.status().notes.c_str());
    P.eq("parse.no_notes", port.status().notes.empty() ? 1 : 0, 1);
    P.eq("parse.panel_view", panel.panelView() && panel.hasGui() ? 1 : 0, 1);
    P.eq("parse.defaults", panel.guiControl(width).style().look == "ring" ? 1 : 0, 1);
    P.eq("parse.own_beats_defaults", panel.guiControl(tone).style().look == "vintage" ? 1 : 0, 1);
    P.eq("parse.class", panel.guiControl(cutoff).style().px == 72.0f && panel.guiControl(cutoff).style().arc == "yellow" ? 1 : 0, 1);
    {
        const int card = sectionNode(panel, "KNOBS");
        P.eq("parse.section_defaults", card >= 0 && panel.panelSpec().gui.nodes[static_cast<std::size_t>(card)].style.radius == 10.0f ? 1 : 0, 1);
    }

    // ---- KNOBS ---------------------------------------------------------------------------------------------------------
    {
        const tp::ui::GuiLayout& L = panel.guiLayout();
        int shown = 0;
        bool apart = true, within = true;
        const int card = sectionNode(panel, "KNOBS");
        for (int s = drive; s <= level; ++s)
        {
            const tp::ui::GuiControlItem& k = panel.guiControl(s);
            shown += k.shown() ? 1 : 0;
            if (card >= 0)
                within = within && inside(L.rects[static_cast<std::size_t>(card)], k.cell());
            for (int t = s + 1; t <= level; ++t)
                apart = apart && !overlap(k.cell(), panel.guiControl(t).cell());
        }
        P.eq("knobs.nine_shown", shown, 9);
        P.eq("knobs.apart", apart ? 1 : 0, 1);
        P.eq("knobs.in_section", within ? 1 : 0, 1);
        const tp::ui::GuiKnobGeom& small = panel.guiControl(pan).knobGeom();
        P.eq("knobs.small", small.d < 30.0f && small.labelBottom && !small.labelTop ? 1 : 0, 1);
        P.eq("knobs.value_never", small.value ? 0 : 1, 1);
        P.near("knobs.px_size", static_cast<double>(panel.guiControl(res).knobGeom().d), 96.0, 0.5);
        P.near("knobs.class_size", static_cast<double>(panel.guiControl(cutoff).knobGeom().d), 72.0, 0.5);
        P.ge("knobs.px_cell", static_cast<double>(panel.guiControl(res).cell().w), 96.0);
        P.eq("knobs.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
    }

    // ---- SECTIONS ------------------------------------------------------------------------------------------------------
    {
        const tp::GuiTree& t = panel.panelSpec().gui;
        const int inset = sectionNode(panel, "SHAPES"), plate = sectionNode(panel, "PLATE");
        P.eq("sections.looks", inset >= 0 && plate >= 0 && t.nodes[static_cast<std::size_t>(inset)].style.look == "inset"
                                   && t.nodes[static_cast<std::size_t>(plate)].style.look == "plate" ? 1 : 0, 1);
        P.eq("sections.gradient", t.gradient.empty() ? 0 : 1, 1);
        // The inset and the plate sit side by side and share the row.
        const tp::ui::GuiLayout& L = panel.guiLayout();
        if (inset >= 0 && plate >= 0)
        {
            const funkgui::Rect a = L.rects[static_cast<std::size_t>(inset)], b = L.rects[static_cast<std::size_t>(plate)];
            P.eq("sections.side_by_side", a.right() <= b.x && std::fabs(a.y - b.y) < 0.5f ? 1 : 0, 1);
            const tp::ui::GuiControlItem& big = panel.guiControl(cutoff);
            P.eq("sections.knob_inside", inside(a, big.cell()) ? 1 : 0, 1);
        }
    }
    writePng(P, host, "gui.styles.png", "synth");

    // ---- LIVE ----------------------------------------------------------------------------------------------------------
    // The field is written while the program plays (an instrument's: while a note sounds; the keyboard plays one).
    const tp::ui::GuiKeyboardItem* keys = nullptr;
    if constexpr (kInstrument)
    {
        keys = &static_cast<const tp::ui::GuiKeyboardItem&>(panel.guiItem(3, 0));
        P.eq("keyboard.shown", keys->shown() ? 1 : 0, 1);
        const Heard before = listen(processor, host, 8);
        P.le("keyboard.silent_first", before.rms, 1e-6);
        const funkgui::Point a = keys->centreOfNote(69);
        host.press(a.x, a.y);
        host.tick(1);
        P.eq("keyboard.held", keys->held(), 69);
        listen(processor, host, 4);                          // past the voice's start
        const Heard on = listen(processor, host, 40);
        P.near("keyboard.sounds", on.rms, 0.25 / std::sqrt(2.0), 0.02);
        P.near("keyboard.pitch", on.hz, 440.0, 10.0);
        host.tick(4);
        P.near("live.share", static_cast<double>(panel.guiControl(cutoff).live01()), 0.25, 0.01);
        const funkgui::Point b = keys->centreOfNote(71);
        host.moveTo(b.x, b.y, 4);
        host.tick(1);
        P.eq("keyboard.slides", keys->held(), 71);
        listen(processor, host, 4);
        P.near("keyboard.slid_pitch", listen(processor, host, 40).hz, 493.9, 12.0);
        host.release();
        host.tick(1);
        P.eq("keyboard.released", keys->held(), -1);
        listen(processor, host, 4);
        P.le("keyboard.silent_after", listen(processor, host, 8).rms, 1e-4);
        bool a11y = false;
        for (const funkgui::A11yItem& item : host.accessibility())
            a11y = a11y || item.title == "Keyboard";
        P.eq("keyboard.a11y", a11y ? 1 : 0, 1);

        // ---- WHEELS ----------------------------------------------------------------------------------------------------
        const funkgui::Rect pw = keys->pitchWheel(), mw = keys->modWheel();
        P.eq("wheels.shown", keys->wheels() && pw.w > 0.0f && mw.w > 0.0f && pw.right() <= mw.x ? 1 : 0, 1);
        P.eq("wheels.keys_after", keys->noteAt({ mw.centreX(), mw.centreY() }) < 0 ? 1 : 0, 1);
        host.press(pw.centreX(), pw.centreY());
        host.moveTo(pw.centreX(), pw.y + 1.0f, 4);
        host.tick(1);
        P.near("wheels.pitch_up", static_cast<double>(keys->pitch()), 1.0, 1e-6);
        listen(processor, host, 1);
        P.near("wheels.pitch_reaches_host", static_cast<double>(processor.engine().host().pitchbend), 1.0, 1e-6);
        host.release();
        host.tick(1);
        P.eq("wheels.pitch_springs_back", keys->pitch() == 0.0f ? 1 : 0, 1);
        listen(processor, host, 1);
        P.eq("wheels.pitch_rest_reaches_host", processor.engine().host().pitchbend == 0.0f ? 1 : 0, 1);
        host.press(mw.centreX(), mw.centreY());
        host.moveTo(mw.centreX(), mw.y + 6.0f + 0.25f * (mw.h - 12.0f), 4);
        host.release();
        host.tick(1);
        P.near("wheels.mod_stays", static_cast<double>(keys->mod()), 0.75, 1e-3);
        listen(processor, host, 1);
        P.near("wheels.mod_reaches_host", static_cast<double>(processor.engine().host().modwheel), 0.75, 1e-3);
        P.eq("wheels.settled", keys->settled() ? 1 : 0, 1);
    }
    else
    {
        listen(processor, host, 8);
        host.tick(4);
        P.near("live.share", static_cast<double>(panel.guiControl(cutoff).live01()), 0.25, 0.01);
    }

    // ---- PAGES ---------------------------------------------------------------------------------------------------------
    {
        const auto& bar = static_cast<const tp::ui::GuiTabsItem&>(panel.guiItem(2, 0));
        P.eq("pages.three", bar.shown() && bar.tabs().size() == 3 ? 1 : 0, 1);
        P.eq("pages.first", panel.guiControl(drive).shown() && !panel.guiControl(busA).shown() ? 1 : 0, 1);
        if (bar.tabs().size() == 3)
        {
            const funkgui::Rect t = bar.tabs()[2];
            host.click(t.centreX(), t.centreY());
            host.settle();
        }
        P.eq("pages.third", panel.guiControl(busA).shown() && !panel.guiControl(drive).shown() ? 1 : 0, 1);
        if (keys != nullptr)
            P.eq("pages.keyboard_hidden", keys->shown() ? 0 : 1, 1);
        const std::vector<std::string> pages = port.guiPages();
        P.eq("pages.kept", !pages.empty() && pages[0] == "MIXER" ? 1 : 0, 1);
        writePng(P, host, "gui.styles.mixer.png", "mixer");

        // The side list: its second page.
        const tp::ui::GuiTabsItem* side = nullptr;
        for (int k = 0; k < 8; ++k)
        {
            const auto& t = static_cast<const tp::ui::GuiTabsItem&>(panel.guiItem(2, k));
            if (t.shown() && t.node() != bar.node() && t.tabs().size() == 3)
                side = &t;
        }
        P.eq("pages.side_shown", side != nullptr ? 1 : 0, 1);
        if (side != nullptr)
        {
            P.eq("pages.side_left", side->tabs()[1].y > side->tabs()[0].y && std::fabs(side->tabs()[1].x - side->tabs()[0].x) < 0.5f ? 1 : 0, 1);
            const funkgui::Rect t = side->tabs()[1];
            host.click(t.centreX(), t.centreY());
            host.settle();
            P.eq("pages.side_second", panel.guiControl(busB).shown() && !panel.guiControl(busA).shown() ? 1 : 0, 1);
            P.eq("pages.side_beside", panel.guiControl(busB).cell().x >= side->tabs()[1].right() ? 1 : 0, 1);
        }

        // In the session: another processor, and a Panel on it, open on MIXER (BUS B).
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        tp::Processor second;
        second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        const std::vector<std::string> again = second.editorPort().guiPages();
        // Every tabs element's page, in the file's order: the bar's, the pills' (on FX), the side list's.
        P.eq("pages.in_session", again == std::vector<std::string>{ "MIXER", "ECHO", "BUS B" } ? 1 : 0, 1);
        {
            tp::ui::Panel other(second.ports(), second.editorPort());
            funkgui::HeadlessHost otherHost(other, 0, 2.0f);
            second.settle();
            otherHost.tick(2);
            otherHost.settle();
            P.eq("pages.reopened", other.guiControl(busB).shown() && !other.guiControl(drive).shown() ? 1 : 0, 1);
        }

        // The interface edited (its title) and run again: the same pages.
        load(processor, { { tp::kProgramName, program() }, { "panel.gui", gui("STYLES 2") } });
        settleRun(processor, host);
        P.eq("pages.edited_keeps", panel.guiControl(busB).shown() && !panel.guiControl(drive).shown() ? 1 : 0, 1);

        // FX: pills.
        if (bar.tabs().size() == 3)
        {
            const funkgui::Rect t = bar.tabs()[1];
            host.click(t.centreX(), t.centreY());
            host.settle();
        }
        P.eq("pages.fx", panel.guiControl(echo).shown() && !panel.guiControl(space).shown() ? 1 : 0, 1);
        const tp::ui::GuiTabsItem* pills = nullptr;
        for (int k = 0; k < 8; ++k)
        {
            const auto& t = static_cast<const tp::ui::GuiTabsItem&>(panel.guiItem(2, k));
            if (t.shown() && t.node() != bar.node() && t.tabs().size() == 2)
                pills = &t;
        }
        P.eq("pages.pills_shown", pills != nullptr ? 1 : 0, 1);
        if (pills != nullptr)
        {
            const funkgui::Rect t = pills->tabs()[1];
            host.click(t.centreX(), t.centreY());
            host.settle();
            P.eq("pages.pills_second", panel.guiControl(space).shown() && !panel.guiControl(echo).shown() ? 1 : 0, 1);
        }
        writePng(P, host, "gui.styles.fx.png", "fx");
        if (bar.tabs().size() == 3)
        {
            const funkgui::Rect t = bar.tabs()[0];
            host.click(t.centreX(), t.centreY());
            host.settle();
        }
    }

    // ---- WRAP ----------------------------------------------------------------------------------------------------------
    {
        host.resize(900, 640);
        host.settle();
        const tp::ui::GuiLayout& L = panel.guiLayout();
        float top = 1e9f, bottom = -1e9f;
        bool within = true;
        const int card = sectionNode(panel, "KNOBS");
        for (int s = drive; s <= level; ++s)
        {
            const funkgui::Rect c = panel.guiControl(s).cell();
            top = std::min(top, c.y);
            bottom = std::max(bottom, c.y);
            if (card >= 0)
                within = within && inside(L.rects[static_cast<std::size_t>(card)], c);
        }
        std::printf("narrow: area %g wide, knobs' rows from %g to %g\n", static_cast<double>(L.area.w), static_cast<double>(top),
                    static_cast<double>(bottom));
        P.eq("wrap.lines", bottom > top + 20.0f ? 1 : 0, 1);
        P.eq("wrap.in_section", within ? 1 : 0, 1);
        const int node = controlNode(panel, level);
        P.eq("wrap.node", node >= 0 ? 1 : 0, 1);
        writePng(P, host, "gui.styles.narrow.png", "narrow");
    }
    return P.finish();
}
