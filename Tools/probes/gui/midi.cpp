// PROBE name=gui.midi timeout=240
//
// gui.midi: MIDI learn on the panel's dials (plugin/MidiLearn.h; editor/Panel.cpp's dial menu, Looks.h's midiMark),
// headless over real Processors, the MIDI in host blocks, to <build>/probe-out/:
//   gui.midi.menu.png and gui.midi.menu.paper.png     a dial's menu over its dial (FunkGui's MenuLook, as gui.menus)
//   gui.midi.mapped.menu.png                          ... once the dial has learned a control
//   gui.midi.learning.png                             the dial learning: its ring, the footer's line under the pointer
//   gui.midi.mark.png and gui.midi.mark.paper.png     the dial a control moves: its dot, the footer naming the control
//   gui.midi.gui.png                                  a .gui file's knob a control moves, and one learning
// and checks:
//   MENU    a right-click (a ctrl-click) on a dial opens the dial's menu at the pointer and writes nothing: its title
//           the dial's name, Learn MIDI CC, Forget MIDI CC (off while it has none), Forget every MIDI CC (off while there
//           is none) and the host's own menu (a host parameter's); a toggle's right-click writes nothing either
//   LEARN   Learn MIDI CC: the dial learns (its ring, the footer's LEARNING line, Stop learning in its menu); CC 74 on
//           channel 2 is learned: the dot, the footer "MIDI CC 74 ON CHANNEL 2", and the menu's Forget names it
//   HOST    the host's menu item opens the host's own menu for that parameter
//   FORGET  Forget CC 74 on channel 2: no dot, no MIDI in the footer
//   GUI     a .gui file's knob: its right-click opens the same menu; learned, its dot and its footer line; learning, its
//           ring; its switch: a right-click and a screen reader's menu action both open the menu, writing nothing, its
//           press still flipping it
//   EFFECT  the effect's dial: a right-click opens the host's menu, as before (the effect learns no MIDI)
#include "Probe.h"

#include "editor/DialLayout.h"
#include "editor/GuiItems.h"
#include "editor/Looks.h"
#include "editor/Panel.h"
#include "editor/Theme.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "Product.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/juce/MenuLook.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    const char* const kThemeName[2] = { "", ".paper" };

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    std::string program(bool gui)
    {
        const std::string params = "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float resonance [[fminus::range(0, 1)]] [[fminus::default(0.2)]];\n"
                                   "    param bool sync [[fminus::default(0)]];\n";
        const std::string panel = gui ? "" : "#pragma panel style sync switch\n";
        if constexpr (kInstrument)
            return "// MIDI: the probe's.\n" + panel + "module Voice\n{\n" + params
                 + "    void start(float note, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// MIDI: the probe's.\n" + panel + "module Effect\n{\n" + params
                 + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    const char* const kGui = R"GUI(// The probe's interface.
panel {
    row gap=10 {
        knob cutoff size=large
        knob resonance
        switch sync
        spacer
    }
}
)GUI";

    void run(pg::Processor& p, const pg::Sources& files)
    {
        p.editorPort().setDraft({ files, 0 });
        p.editorPort().run(files);
        p.settle();
        p.setRateAndBufferSizeDetails(kFs, kBlock);
        p.prepareToPlay(kFs, kBlock);
    }

    void settle(pg::Processor& p, funkgui::HeadlessHost& host)
    {
        p.settle();
        host.tick(2);
        host.settle();
    }

    // One host block with these control changes (channel, number, value), then MIDI learn's tick at `ms`.
    void controls(pg::Processor& p, double ms, std::initializer_list<std::tuple<int, int, int>> ccs)
    {
        juce::AudioBuffer<float> buffer(2, kBlock);
        buffer.clear();
        juce::MidiBuffer midi;
        for (const auto& [ch, cc, v] : ccs)
            midi.addEvent(juce::MidiMessage::controllerEvent(ch, cc, v), 0);
        p.processBlock(buffer, midi);
        p.pollMidi(ms);
    }

    // The items of a menu by label (nullptr when it has none).
    const funkgui::MenuItem* itemOf(const funkgui::MenuRequest& m, const std::string& label)
    {
        for (const funkgui::MenuItem& it : m.items)
            if (it.label == label)
                return &it;
        return nullptr;
    }

    // ---- the pictures (as gui.menus draws its own) ----------------------------------------------------------------------
    struct Backdrop final : juce::Component
    {
        juce::Image frame;
        juce::Rectangle<float> window;
        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff5a5d63));
            g.drawImage(frame, window);
        }
    };

    struct Pictures
    {
        std::vector<std::unique_ptr<Backdrop>> backdrops;
        std::vector<std::unique_ptr<funkgui::MenuLook>> looks;
    };

    juce::Image toJuce(const funkgui::Image& img)
    {
        juce::Image out(juce::Image::ARGB, std::max(1, img.w), std::max(1, img.h), true);
        juce::Image::BitmapData d(out, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
            {
                const uint8_t* q = &img.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(img.w) + static_cast<std::size_t>(x)) * 4u];
                d.setPixelColour(x, y, juce::Colour(q[0], q[1], q[2], q[3]));
            }
        return out;
    }

    // The menu `request` opens over the Panel's frame, cut to the menu and `keep` (the dial) with a margin.
    void menuPicture(funkgui::test::Probe& P, Pictures& pics, funkgui::HeadlessHost& host, const pg::ui::Panel& panel,
                     const funkgui::MenuRequest& request, const funkgui::Rect& keep, const std::string& file)
    {
        auto backdrop = std::make_unique<Backdrop>();
        backdrop->frame = toJuce(funkgui::rasterise(host.draw(), funkgui::FontService::get().atlas(), 2));
        const juce::Point<int> at = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                                        ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea.getTopLeft()
                                        : juce::Point<int>{};
        backdrop->window = { 0.0f, 0.0f, static_cast<float>(panel.width()), static_cast<float>(panel.height()) };
        backdrop->setBounds(at.x, at.y, panel.width(), panel.height() + 720);
        auto look = std::make_unique<funkgui::MenuLook>(request.theme);
        juce::PopupMenu menu = funkgui::MenuLook::popupMenuFor(request);
        menu.setLookAndFeel(look.get());
        const funkgui::Rect& a = request.anchor;
        const juce::Rectangle<int> anchor = juce::Rectangle<float>(a.x, a.y, a.w, a.h).toNearestInt();
        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withParentComponent(backdrop.get())
                               .withTargetScreenArea(look->targetAreaFor(anchor + at))
                               .withMaximumNumColumns(1),
                           [](int) {});
        juce::Component* window = nullptr;
        for (int i = backdrop->getNumChildComponents(); --i >= 0;)
            if (backdrop->getChildComponent(i)->getName() == "menu")
                window = backdrop->getChildComponent(i);
        bool written = false;
        if (window != nullptr)
        {
            const juce::Rectangle<int> dial = juce::Rectangle<float>(keep.x, keep.y, keep.w, keep.h).toNearestInt();
            const juce::Rectangle<int> crop =
                window->getBounds().getUnion(anchor).getUnion(dial).expanded(16).getIntersection(backdrop->getLocalBounds());
            const juce::Image shot = backdrop->createComponentSnapshot(crop, true, 2.0f);
            const std::string path = probe::outPath(file);
            const juce::File f(juce::String::fromUTF8(path.c_str()));
            f.deleteFile();
            juce::FileOutputStream out(f);
            juce::PNGImageFormat png;
            written = out.openedOk() && png.writeImageToStream(shot, out);
            if (written)
                std::printf("wrote %s\n", path.c_str());
        }
        juce::PopupMenu::dismissAllActiveMenus();
        P.eq("png." + file, written ? 1 : 0, 1);
        pics.backdrops.push_back(std::move(backdrop));
        pics.looks.push_back(std::move(look));
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const std::string& name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq("png." + name, host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // No glyph drawn over the mark's place at `at` (a label's, a value's).
    bool clearOfText(const funkgui::PrimList& f, funkgui::Point at)
    {
        const float h = pg::ui::looks::kMidiMarkR + 1.0f;
        for (const funkgui::Prim& p : f.prims)
            if (static_cast<int>(p.d2[2] + 0.5f) == static_cast<int>(funkgui::PrimKind::text))
            {
                const float pad = 1.0f;          // a glyph's quad has an apron
                if (p.x0 + pad < at.x + h && p.x1 - pad > at.x - h && p.y0 + pad < at.y + h && p.y1 - pad > at.y - h)
                    return false;
            }
        return true;
    }

    funkgui::Rect cellOf(const pg::ui::Panel& panel, int slot)
    {
        const pg::ui::ItemPlace& c = panel.dialsLayout().dials[static_cast<std::size_t>(slot)];
        return { c.x, c.top, c.w, c.h };
    }

    // A primitive drawn as the mark: a disc (a small rounded square) centred at `at`.
    bool markDrawn(const funkgui::PrimList& f, funkgui::Point at)
    {
        const float x = at.x, y = at.y;
        for (const funkgui::Prim& p : f.prims)
            if (static_cast<int>(p.d2[2] + 0.5f) == static_cast<int>(funkgui::PrimKind::rrect) && p.x1 - p.x0 < 20.0f
                && std::abs(0.5f * (p.x0 + p.x1) - x) < 0.6f && std::abs(0.5f * (p.y0 + p.y1) - y) < 0.6f)
                return true;
        return false;
    }
}

PROBE(gui, midi)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File(juce::String(probe::outPath("gui-midi-stores")));
    folder.deleteRecursively();
    pg::projectstore::setDirectoryForTests(folder.getChildFile("projects"));
    Pictures pics;

    if constexpr (!kInstrument)
    {
        // ---- EFFECT ----------------------------------------------------------------------------------------------------
        pg::Processor processor;
        run(processor, pg::singleFile(program(false)));
        pg::ui::Panel panel(processor.ports(), processor.editorPort());
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        settle(processor, host);
        const auto at = panel.centreOfDial(0);
        P.eq("effect.dial", at ? 1 : 0, 1);
        const int menus = host.log.menus;
        const float before = processor.ports()[0]->value01();
        if (at)
            host.click(at->x, at->y, funkgui::Mods{ false, false, false, true });
        host.tick(1);
        P.eq("effect.host_menu", host.log.menus - menus, 1);
        P.eq("effect.no_menu_of_ours", host.pendingMenu() == nullptr ? 1 : 0, 1);
        P.eq("effect.no_write", processor.ports()[0]->value01() == before ? 1 : 0, 1);
        pg::projectstore::setDirectoryForTests({});
        return P.finish();
    }

    double now = 1000.0;
    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    run(processor, pg::singleFile(program(false)));

    for (int t = 0; t < 2; ++t)
    {
        const bool rows = t == 0;                        // the rows once; the pictures in both themes
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, t, 2.0f);
        settle(processor, host);
        const auto at = panel.centreOfDial(0);
        const funkgui::Rect cell = cellOf(panel, 0);
        if (!at)
        {
            P.eq("menu.dial", 0, 1);
            break;
        }

        // ---- MENU --------------------------------------------------------------------------------------------------
        const float before = processor.ports()[0]->value01();
        host.click(at->x, at->y, funkgui::Mods{ false, false, false, true });
        host.tick(1);
        const funkgui::MenuRequest* m = host.pendingMenu();
        if (rows)
        {
            P.eq("menu.opens", m != nullptr ? 1 : 0, 1);
            P.eq("menu.no_write", processor.ports()[0]->value01() == before ? 1 : 0, 1);
        }
        if (m == nullptr)
            break;
        if (rows)
        {
            P.eq("menu.at_pointer", std::abs(m->anchor.x - at->x) < 0.5f && std::abs(m->anchor.y - at->y) < 0.5f ? 1 : 0, 1);
            P.eq("menu.title", !m->items.empty() && m->items[0].header && has(m->items[0].label, "CUTOFF") ? 1 : 0, 1);
            const funkgui::MenuItem* learn = itemOf(*m, "Learn MIDI CC");
            const funkgui::MenuItem* forget = itemOf(*m, "Forget MIDI CC");
            const funkgui::MenuItem* all = itemOf(*m, "Forget every MIDI CC");
            const funkgui::MenuItem* hostItem = itemOf(*m, "The host's menu...");
            P.eq("menu.learn", learn != nullptr && learn->enabled ? 1 : 0, 1);
            P.eq("menu.forget_off", forget != nullptr && !forget->enabled ? 1 : 0, 1);
            P.eq("menu.forget_all_off", all != nullptr && !all->enabled ? 1 : 0, 1);
            P.eq("menu.host", hostItem != nullptr && hostItem->enabled ? 1 : 0, 1);
        }
        menuPicture(P, pics, host, panel, *m, cell, "gui.midi.menu" + std::string(kThemeName[t]) + ".png");

        // ---- LEARN -------------------------------------------------------------------------------------------------
        P.eq(rows ? "learn.chosen" : "learn.chosen.paper", host.chooseMenuItem("Learn MIDI CC") ? 1 : 0, 1);
        host.move(at->x, at->y);
        settle(processor, host);
        if (rows)
        {
            P.eq("learn.port", port.midi().learning, 0);
            P.eq("learn.ring", panel.dialMidiLearning(0) && markDrawn(host.draw(), pg::ui::looks::midiMarkAt(cell)) ? 1 : 0, 1);
            P.eq("learn.footer", has(panel.dialSpecLine(0), "LEARNING: MOVE A CONTROL ON A MIDI CONTROLLER") ? 1 : 0, 1);
            writePng(P, host, "gui.midi.learning.png");
            host.click(at->x, at->y, funkgui::Mods{ false, false, false, true });
            host.tick(1);
            const funkgui::MenuRequest* again = host.pendingMenu();
            P.eq("learn.menu_stop", again != nullptr && itemOf(*again, "Stop learning") != nullptr ? 1 : 0, 1);
            host.cancelMenu();
            host.tick(1);
        }
        controls(processor, now += 10.0, { { 2, 74, 90 } });
        settle(processor, host);
        if (rows)
        {
            P.eq("learn.learned", port.midi().words[0] == "CC 74 on channel 2" ? 1 : 0, 1);
            P.eq("learn.dot", panel.dialMidiMarked(0) && !panel.dialMidiLearning(0) && markDrawn(host.draw(), pg::ui::looks::midiMarkAt(cell)) ? 1 : 0, 1);
            P.eq("learn.footer_cc", has(panel.dialSpecLine(0), "MIDI CC 74 ON CHANNEL 2") ? 1 : 0, 1);
            P.eq("learn.no_dot_elsewhere", panel.dialMidiMarked(1) ? 0 : 1, 1);
        }
        controls(processor, now += 10.0, { { 2, 74, 127 } });
        settle(processor, host);
        if (rows)
            P.near("learn.moves_the_dial", static_cast<double>(processor.ports()[0]->value01()), 1.0, 1e-7);
        processor.pollMidi(now += 400.0);                // the touch ends
        // resonance (a label as wide as its cell) learns CC 75: its dot too, clear of its label as cutoff's is.
        (void)port.learnMidi(1);
        controls(processor, now += 10.0, { { 2, 75, 20 } });
        settle(processor, host);
        if (rows)
        {
            P.eq("learn.second_dot", panel.dialMidiMarked(1) && markDrawn(host.draw(), pg::ui::looks::midiMarkAt(cellOf(panel, 1))) ? 1 : 0, 1);
            P.eq("learn.clear_of_text", clearOfText(host.draw(), pg::ui::looks::midiMarkAt(cell))
                                            && clearOfText(host.draw(), pg::ui::looks::midiMarkAt(cellOf(panel, 1))) ? 1 : 0, 1);
        }
        host.move(at->x, at->y);
        host.tick(1);
        writePng(P, host, "gui.midi.mark" + std::string(kThemeName[t]) + ".png");

        // A toggle (sync, a switch): its right-click writes nothing either.
        if (rows)
        {
            const auto sw = panel.centreOfDial(2);
            const float was = processor.ports()[2]->value01();
            if (sw)
                host.click(sw->x, sw->y, funkgui::Mods{ false, false, false, true });
            host.tick(1);
            P.eq("menu.toggle_no_write", sw && processor.ports()[2]->value01() == was && host.pendingMenu() != nullptr ? 1 : 0, 1);
            host.cancelMenu();
            host.tick(1);
        }

        // ---- HOST, FORGET ------------------------------------------------------------------------------------------
        host.click(at->x, at->y, funkgui::Mods{ false, false, false, true });
        host.tick(1);
        m = host.pendingMenu();
        if (m == nullptr)
            break;
        if (rows)
        {
            P.eq("forget.named", itemOf(*m, "Forget CC 74 on channel 2") != nullptr && itemOf(*m, "Forget CC 74 on channel 2")->enabled ? 1 : 0, 1);
            P.eq("forget.learn_another", itemOf(*m, "Learn another MIDI CC") != nullptr ? 1 : 0, 1);
            P.eq("forget.all_on", itemOf(*m, "Forget every MIDI CC (2)") != nullptr && itemOf(*m, "Forget every MIDI CC (2)")->enabled ? 1 : 0, 1);
            menuPicture(P, pics, host, panel, *m, cell, "gui.midi.mapped.menu.png");
            const int menus = host.log.menus;
            P.eq("host.chosen", host.chooseMenuItem("The host's menu...") ? 1 : 0, 1);
            host.tick(1);
            P.eq("host.opened", host.log.menus - menus, 1);
            host.click(at->x, at->y, funkgui::Mods{ false, false, false, true });
            host.tick(1);
            m = host.pendingMenu();
            if (m == nullptr)
                break;
        }
        P.eq(rows ? "forget.chosen" : "forget.chosen.paper", host.chooseMenuItem("Forget CC 74 on channel 2") ? 1 : 0, 1);
        settle(processor, host);
        if (rows)
        {
            P.eq("forget.gone", port.midi().words[0].empty() && !panel.dialMidiMarked(0) ? 1 : 0, 1);
            P.eq("forget.footer", has(panel.dialSpecLine(0), "MIDI") ? 0 : 1, 1);
            P.eq("forget.no_mark", markDrawn(host.draw(), pg::ui::looks::midiMarkAt(cell)) ? 0 : 1, 1);
            P.eq("forget.others_kept", panel.dialMidiMarked(1) ? 1 : 0, 1);
        }
        (void)port.forgetAllMidi();
    }

    // ---- GUI ---------------------------------------------------------------------------------------------------------
    {
        run(processor, { { pg::kProgramName, program(true) }, { "panel.gui", kGui } });
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        settle(processor, host);
        P.eq("gui.has_gui", panel.hasGui() ? 1 : 0, 1);
        const pg::ui::GuiControlItem& knob = panel.guiControl(0);
        const pg::ui::GuiControlItem& reso = panel.guiControl(1);
        const funkgui::Rect cell = knob.cell();
        const funkgui::Point at{ cell.centreX(), cell.centreY() };
        const float before = processor.ports()[0]->value01();
        host.click(at.x, at.y, funkgui::Mods{ false, false, false, true });
        host.tick(1);
        const funkgui::MenuRequest* m = host.pendingMenu();
        P.eq("gui.menu", m != nullptr && itemOf(*m, "Learn MIDI CC") != nullptr ? 1 : 0, 1);
        P.eq("gui.no_write", processor.ports()[0]->value01() == before ? 1 : 0, 1);
        P.eq("gui.learn", m != nullptr && host.chooseMenuItem("Learn MIDI CC") ? 1 : 0, 1);
        settle(processor, host);
        controls(processor, now += 10.0, { { 1, 71, 30 } });
        settle(processor, host);
        P.eq("gui.dot", knob.midiMarked() && markDrawn(host.draw(), knob.midiMarkAt()) ? 1 : 0, 1);
        char line[256] = {};
        knob.specLine(line, sizeof line);
        P.eq("gui.footer", has(line, "MIDI CC 71 ON CHANNEL 1") ? 1 : 0, 1);
        controls(processor, now += 10.0, { { 1, 71, 127 } });
        settle(processor, host);
        P.near("gui.moves", static_cast<double>(processor.ports()[0]->value01()), 1.0, 1e-7);
        processor.pollMidi(now += 400.0);
        // The second knob learning: its ring.
        const funkgui::Point at2{ reso.cell().centreX(), reso.cell().centreY() };
        host.click(at2.x, at2.y, funkgui::Mods{ false, false, false, true });
        host.tick(1);
        P.eq("gui.learn_second", host.chooseMenuItem("Learn MIDI CC") ? 1 : 0, 1);
        host.move(at.x, at.y);
        settle(processor, host);
        P.eq("gui.ring", reso.midiLearning() && markDrawn(host.draw(), reso.midiMarkAt()) ? 1 : 0, 1);
        P.eq("gui.clear_of_text", clearOfText(host.draw(), knob.midiMarkAt()) && clearOfText(host.draw(), reso.midiMarkAt()) ? 1 : 0, 1);
        writePng(P, host, "gui.midi.gui.png");
        port.cancelMidiLearn();
        // The switch (sync): a right-click and a screen reader's menu action alike open the dial's menu, and neither
        // writes; its press still flips it.
        {
            const pg::ui::GuiControlItem& sw = panel.guiControl(2);
            const funkgui::Point c{ sw.cell().centreX(), sw.cell().centreY() };
            const float was = processor.ports()[2]->value01();
            host.click(c.x, c.y, funkgui::Mods{ false, false, false, true });
            host.tick(1);
            const funkgui::MenuRequest* r = host.pendingMenu();
            P.eq("gui.switch_menu", r != nullptr && itemOf(*r, "Learn MIDI CC") != nullptr && processor.ports()[2]->value01() == was ? 1 : 0, 1);
            host.cancelMenu();
            host.tick(1);
            std::optional<funkgui::A11yItem> item;
            for (const funkgui::A11yItem& a : host.accessibility())
                if (a.visible && a.bounds.contains(c) && a.role == funkgui::A11yRole::toggleButton)
                    item = a;
            if (item)
                panel.a11yAction(item->id, funkgui::A11yAction::showMenu);
            host.tick(1);
            r = host.pendingMenu();
            P.eq("gui.switch_a11y_menu", item && r != nullptr && itemOf(*r, "Learn MIDI CC") != nullptr && processor.ports()[2]->value01() == was ? 1 : 0, 1);
            host.cancelMenu();
            host.tick(1);
            if (item)
                panel.a11yAction(item->id, funkgui::A11yAction::press);
            settle(processor, host);
            P.eq("gui.switch_press_flips", item && processor.ports()[2]->value01() != was && host.pendingMenu() == nullptr ? 1 : 0, 1);
        }
        (void)port.forgetAllMidi();
    }

    pg::projectstore::setDirectoryForTests({});
    return P.finish();
}
