// PROBE name=gui.diff timeout=240
//
// gui.diff: COMPARE's card (editor/DiffCard.h, its view editor/DiffView.h) headless over a real Processor whose
// projects are in a temporary folder of the probe's own: a project "Diff Pad" of two versions (version 2: gain.fm's K
// changed, long.fm changed in six places, pedal.cir's drawing alone changed, notes.fm gone, lib.fm new, the
// level dial moved, a sample added) with two presets, and the editor's files after it (K again, the sample replaced), to
// <build>/probe-out/:
//   gui.diff.png           v1 against v2, gain.fm's rows
//   gui.diff.dials.png     the DIALS pill
//   gui.diff.samples.png   the SAMPLES pill
//   gui.diff.paper.png     the card in PAPER
//   gui.diff.reading.png   "Reading version 1..." while the store's Reader reads the file
//   gui.diff.small.png     the least window
//   gui.diff.wide.png      a wide window
// and checks:
//   OPEN     PROJECTS' Compare... opens it (the project open and edited: the version open against the editor), the
//            versions' menu (a right-click on a row: with the version before, with the editor), the top bar's pill's
//            menu (a right-click: Compare with the saved version, only while the editor has changes), Escape and a
//            click outside close it (a double-click's second press reaching nothing under it)
//   PICK     the left dropdown's menu lists Editor and every version, the current one dotted; Editor chosen; the swap
//            exchanges the sides
//   DIFF     v1 to v2: a pill a file (gain.fm +1 -1, notes.fm gone, lib.fm new), DIALS and SAMPLES after them, the
//            line "5 files, 1 dial, 1 sample"; gain.fm's rows: the K taken away and added, numbered, the lines between
//            said in one
//   DRAWING  pedal.cir's drawing lines alone changed: its pill "pedal.cir drawing", one folded row and no line added or
//            taken away
//   DIALS    a row for the level dial, its value each side as the panel says it
//   SAMPLES  kick.wav added from v1 to v2, replaced from v2 to the editor; the same samples: no pill
//   NEXT     ] in long.fm goes to its second change (its first shows already), [ back to the first; ] through its six
//            changes on into the next pill, [ back to its last; the top bar's presets do not move while the card has
//            the keys; the change buttons do the same
//   SCROLL   the wheel, End and Home scroll the rows as the code does (Scroll.h)
//   KEYS     Tab the next pill
//   A11Y     the sides as combo boxes, the swap, the change buttons, the pills; its ids in a range of their own
//   OPENS    Open v1 over the editor's changes asks first (Open anyway), the second click opens it and closes the card
//   AB       A/B... closes it and starts A/B with these two sides
//   READING  a file another rewrote: "Reading version 1..." in the view's place, nothing read whole on this thread,
//            then the diff once the Reader has read it
//   SPEED    the diff of two files of 4,000 lines with changes spread through them, made on this thread: its time
#include "Probe.h"

#include "Audio.h"

#include "editor/AbCard.h"
#include "editor/DiffCard.h"
#include "editor/DiffView.h"
#include "editor/ExportCard.h"
#include "editor/Panel.h"
#include "editor/PresetPicker.h"
#include "editor/ProjectBrowser.h"
#include "editor/SettingsCard.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
using DC = pg::ui::DiffCard;
using DV = pg::ui::DiffView;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    std::string programText()
    {
        const std::string body = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                 "    param float tone [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        return kInstrument
            ? "// Diff: a quiet voice\n#include \"gain.fm\"\nmodule Voice\n{\n" + body
                  + "    bool held;\n    void start(float note, float v) { held = true; }\n    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] += 0.0f * gainK() * level[i]; }\n        return held;\n    }\n};\n"
            : "// Diff: a level\n#include \"gain.fm\"\nmodule Effect\n{\n" + body
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] *= gainK() * level[i]; right[i] *= gainK() * level[i]; }\n    }\n};\n";
    }

    std::string gainText(const std::string& k)
    {
        return "// gain.fm: the level's scale\n\n// K, the program's loudness\nfloat gainK()\n{\n    return " + k
             + "f;\n}\n\n// half of it\nfloat halfK()\n{\n    return 0.5f * gainK();\n}\n\n// a quarter\nfloat quarterK()\n{\n"
               "    return 0.25f * gainK();\n}\n";
    }

    // 120 lines of comment; with `changed`, lines 10, 30, ..., 110 saying so (six changes, more rows than a view holds).
    std::string longText(bool changed)
    {
        std::string s = "// long.fm: notes\n";
        for (int i = 1; i <= 120; ++i)
            s += "// " + std::to_string(i) + (changed && i % 20 == 10 ? ". changed here\n" : ". a line of notes\n");
        return s;
    }

    std::string notesText()
    {
        std::string s = "// notes.fm: what is left to do (no code)\n";
        for (int i = 1; i <= 80; ++i)
            s += "// " + std::to_string(i) + ". a line of notes\n";
        return s;
    }

    std::string pedalText(int x)
    {
        return "* pedal.cir: a divider\nR1 in out 10k\nR2 out 0 10k\n.end\n*@ R1 " + std::to_string(x) + " 40 0\n*@ R2 "
             + std::to_string(x) + " 80 1\n";
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // The processor's timer and the Panel's frames, until `done` or `seconds`.
    bool pump(pg::Processor& p, funkgui::HeadlessHost& host, double seconds, const std::function<bool()>& done)
    {
        const auto from = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count() < seconds)
        {
            p.poll();
            host.tick(1);
            if (done())
            {
                host.settle();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        host.settle();
        return done();
    }

    void clickAt(funkgui::HeadlessHost& host, funkgui::Point p, funkgui::Mods m = {})
    {
        host.click(p.x, p.y, m);
        host.tick(2);
        host.settle();
    }

    funkgui::Point mid(const funkgui::Rect& r) { return { r.centreX(), r.centreY() }; }

    // That pill shown, by Tab (a pill past the row's end comes into it so), as a user would reach it.
    bool toPill(funkgui::HeadlessHost& host, const DV& v, int i)
    {
        for (std::size_t k = 0; k <= v.files().size() && v.file() != i; ++k)
        {
            host.keys("tab");
            host.settle();
        }
        return v.file() == i;
    }

    funkgui::Mods ctrl()                                 // a ctrl-click: a context menu's (a right-click)
    {
        funkgui::Mods m;
        m.ctrl = true;
        return m;
    }

    int pillOf(const DV& v, const std::string& label)
    {
        for (std::size_t i = 0; i < v.files().size(); ++i)
            if (DV::tabLabel(v.files()[i]) == label)
                return static_cast<int>(i);
        return -1;
    }

    const funkgui::MenuItem* labelled(const std::vector<funkgui::MenuItem>& items, const std::string& label)
    {
        for (const funkgui::MenuItem& it : items)
            if (it.label == label)
                return &it;
        return nullptr;
    }

    bool sound(const std::string& path, float freq)
    {
        probe::Stereo s(4800);
        for (std::size_t i = 0; i < s.size(); ++i)
            s.l[i] = s.r[i] = 0.25f * std::sin(6.2831853f * freq * static_cast<float>(i) / 48000.0f);
        return probe::writeWav(path, s, 48000);
    }
}

PROBE(gui, diff)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-diff-" + juce::String(juce::Time::currentTimeMillis()));
    temp.deleteRecursively();
    temp.createDirectory();
    pg::projectstore::setDirectoryForTests(temp.getChildFile("projects"));
    const std::string kick1 = temp.getChildFile("kick.wav").getFullPathName().toStdString();
    const std::string kick2 = temp.getChildFile("again").getChildFile("kick.wav").getFullPathName().toStdString();
    temp.getChildFile("again").createDirectory();

    pg::Processor processor;
    processor.compiler().setFreezing(false);
    pg::EditorPort& port = processor.editorPort();
    const std::string project = "Diff Pad";
    const pg::Sources v1{ { "program.fm", programText() }, { "gain.fm", gainText("1.0") }, { "long.fm", longText(false) },
                          { "pedal.cir", pedalText(10) }, { "notes.fm", notesText() } };
    const pg::Sources v2{ { "program.fm", programText() }, { "gain.fm", gainText("0.5") }, { "long.fm", longText(true) },
                          { "pedal.cir", pedalText(30) }, { "lib.fm", "// lib.fm: more\nfloat twice(float x) { return 2.0f * x; }\n" } };
    pg::Sources current = v2;
    current[1].text = gainText("0.25");
    std::string why;
    pg::SavedVersion saved;
    const auto load = [&](const pg::Sources& files) {
        pg::Draft d;
        d.files = files;
        port.setDraft(d);
        port.run(d.files);
        processor.settle();
    };
    load(v1);
    P.eq("setup.v1", port.saveVersion(project, saved, why) ? 1 : 0, 1);
    load(v2);
    processor.getParameters()[0]->setValueNotifyingHost(pg::normOf(processor.control(0), 0.8f));
    P.eq("setup.sample", sound(kick1, 220.0f) && port.addAssets({ kick1 }, why) == 1 ? 1 : 0, 1);
    P.eq("setup.v2", port.saveVersion(project, saved, why) && saved.number == 2 ? 1 : 0, 1);
    P.eq("setup.presets", port.savePreset("Bright", why) && port.savePreset("Dark", why) && port.applyPreset(project, "Bright", why) ? 1 : 0, 1);
    processor.settle();
    load(current);
    P.eq("setup.sample_replaced", sound(kick2, 330.0f) && port.addAssets({ kick2 }, why) == 1 ? 1 : 0, 1);

    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    processor.settle();
    pump(processor, host, 5.0, [&] { return port.project().edited; });
    float W = static_cast<float>(pg::ui::kDefaultWidth), H = static_cast<float>(pg::ui::kDefaultHeight);
    DC& card = panel.compareCard();
    pg::ui::ProjectBrowser& browser = panel.projectBrowser();
    const auto readBoth = [&] { return pump(processor, host, 20.0, [&] { return !card.reading(); }); };
    const auto view = [&] { return card.viewArea(W, H); };

    // ---- OPEN ----------------------------------------------------------------------------------------------------------
    P.eq("open.edited", port.project().name == project && port.project().version == 2 && port.project().edited ? 1 : 0, 1);
    panel.openProjects();
    host.settle();
    browser.chooseProjectNamed(project, W, H);
    host.settle();
    clickAt(host, mid(browser.places(W, H).compare));
    P.eq("open.button", card.isOpen() && !browser.isOpen() && card.project() == project ? 1 : 0, 1);
    P.eq("open.button_sides", card.side(0) == 2 && card.side(1) == DC::kEditor ? 1 : 0, 1);
    host.keys("escape");
    host.settle();
    P.eq("open.escape_closes", !card.isOpen() ? 1 : 0, 1);
    // The versions' menu: v1's row (the second, the newest first).
    panel.openProjects();
    host.settle();
    browser.chooseProjectNamed(project, W, H);
    host.settle();
    clickAt(host, browser.centreOfRow(pg::ui::ProjectBrowser::List::versions, 1, W, H), ctrl());
    {
        const funkgui::MenuRequest* m = host.pendingMenu();
        P.eq("open.version_menu", m != nullptr && labelled(m->items, "Compare with the editor") != nullptr
                                      && labelled(m->items, "Compare with the version before") != nullptr
                                      && !labelled(m->items, "Compare with the version before")->enabled ? 1 : 0, 1);
        P.eq("open.version_menu_chosen", host.chooseMenuItem("Compare with the editor") ? 1 : 0, 1);
        host.tick(2);
        host.settle();
    }
    P.eq("open.version_menu_editor", card.isOpen() && card.side(0) == 1 && card.side(1) == DC::kEditor ? 1 : 0, 1);
    // A double-click outside, over the PROJECTS pill: the first press closes the card (Panel::pressCard), the second
    // and the double-click reach nothing (the pill would open PROJECTS).
    {
        const funkgui::Point pill = panel.centreOfProjects();
        host.doubleClick(pill.x, pill.y);
        host.settle();
    }
    P.eq("open.click_outside_closes", !card.isOpen() ? 1 : 0, 1);
    P.eq("open.outside_double_click_swallowed", !card.isOpen() && !browser.isOpen() ? 1 : 0, 1);
    panel.openProjects();
    host.settle();
    browser.chooseProjectNamed(project, W, H);
    host.settle();
    clickAt(host, browser.centreOfRow(pg::ui::ProjectBrowser::List::versions, 0, W, H), ctrl());
    P.eq("open.version_before", host.chooseMenuItem("Compare with the version before") ? 1 : 0, 1);
    host.tick(2);
    host.settle();
    P.eq("open.version_before_sides", card.isOpen() && card.side(0) == 1 && card.side(1) == 2 ? 1 : 0, 1);
    host.keys("escape");
    host.settle();
    // The top bar's pill: a right-click, its menu.
    clickAt(host, panel.centreOfProjects(), ctrl());
    {
        const funkgui::MenuRequest* m = host.pendingMenu();
        const funkgui::MenuItem* it = m != nullptr ? labelled(m->items, "Compare with the saved version") : nullptr;
        P.eq("open.pill_menu", it != nullptr && it->enabled && labelled(m->items, "Projects...") != nullptr ? 1 : 0, 1);
        P.eq("open.pill_chosen", host.chooseMenuItem("Compare with the saved version") ? 1 : 0, 1);
        host.tick(2);
        host.settle();
    }
    P.eq("open.pill_sides", card.isOpen() && card.side(0) == 2 && card.side(1) == DC::kEditor ? 1 : 0, 1);
    P.eq("open.pill_read", readBoth() ? 1 : 0, 1);
    P.eq("open.pill_one_file", card.summary() == "1 file, 1 sample" ? 1 : 0, 1);

    // ---- SAMPLES (v2 against the editor: kick.wav replaced) -------------------------------------------------------------
    {
        const int s = pillOf(card.view(), "SAMPLES 1");
        P.eq("samples.replaced", s >= 0 && card.view().files()[static_cast<std::size_t>(s)].lines.size() == 1
                                     && card.view().files()[static_cast<std::size_t>(s)].lines[0].text == "kick.wav  replaced" ? 1 : 0, 1);
        P.eq("samples.no_dials_pill", pillOf(card.view(), "DIALS 1") < 0 ? 1 : 0, 1);
    }
    host.keys("escape");
    host.settle();

    // ---- PICK, DIFF ------------------------------------------------------------------------------------------------------
    panel.openCompare(project, 2, 2);
    P.eq("pick.read", readBoth() ? 1 : 0, 1);
    P.eq("pick.same", card.summary() == "The same" ? 1 : 0, 1);
    clickAt(host, mid(card.places(W, H).left));
    {
        const funkgui::MenuRequest* m = host.pendingMenu();
        bool rows = m != nullptr && m->items.size() == 4 && m->items[0].label == "Editor" && m->items[1].header
                 && m->items[2].label.rfind("v2  ", 0) == 0 && m->items[2].checked && m->items[3].label.rfind("v1  ", 0) == 0;
        P.eq("pick.menu", rows ? 1 : 0, 1);
        P.eq("pick.menu_editor", host.chooseMenuItem("Editor") ? 1 : 0, 1);
        host.tick(2);
        host.settle();
    }
    P.eq("pick.editor", card.side(0) == DC::kEditor && card.side(1) == 2 ? 1 : 0, 1);
    clickAt(host, mid(card.places(W, H).swap));
    P.eq("pick.swap", card.side(0) == 2 && card.side(1) == DC::kEditor ? 1 : 0, 1);
    card.setSide(0, 1);
    card.setSide(1, 2);
    host.settle();
    P.eq("diff.read", readBoth() ? 1 : 0, 1);
    const DV& v = card.view();
    P.eq("diff.summary", card.summary() == "5 files, 1 dial, 1 sample" ? 1 : 0, 1);
    std::printf("summary: %s\n", card.summary().c_str());
    for (const DV::FileDiff& f : v.files())
        std::printf("pill: %s\n", DV::tabLabel(f).c_str());
    P.eq("diff.pills", pillOf(v, "program.fm") == 0 && pillOf(v, "gain.fm +1 -1") == 1 && pillOf(v, "notes.fm gone") >= 0
                           && pillOf(v, "lib.fm new") >= 0 && pillOf(v, "DIALS 1") == static_cast<int>(v.files().size()) - 2
                           && pillOf(v, "SAMPLES 1") == static_cast<int>(v.files().size()) - 1 ? 1 : 0, 1);
    {
        const DV::FileDiff& g = v.files()[1];
        bool removed = false, added = false, gap = false;
        for (const DV::Line& l : g.lines)
        {
            removed = removed || (l.kind == DV::Line::Kind::removed && l.text.find("return 1.0f;") != std::string::npos && l.a == 6);
            added = added || (l.kind == DV::Line::Kind::added && l.text.find("return 0.5f;") != std::string::npos && l.b == 6);
            gap = gap || (l.kind == DV::Line::Kind::gap && l.text.find("lines the same") != std::string::npos);
        }
        P.eq("diff.rows", removed && added && gap ? 1 : 0, 1);
        P.eq("diff.first_changed_shown", v.file() == 1 ? 1 : 0, 1);
    }
    writePng(P, host, "gui.diff.png");

    // ---- DRAWING -------------------------------------------------------------------------------------------------------
    {
        const int i = pillOf(v, "pedal.cir drawing");
        int drawing = 0, plus = 0;
        if (i >= 0)
            for (const DV::Line& l : v.files()[static_cast<std::size_t>(i)].lines)
            {
                drawing += l.kind == DV::Line::Kind::drawing ? 1 : 0;
                plus += l.kind == DV::Line::Kind::added || l.kind == DV::Line::Kind::removed ? 1 : 0;
            }
        P.eq("drawing.folded", i >= 0 && drawing == 1 && plus == 0 ? 1 : 0, 1);
    }

    // ---- DIALS, SAMPLES ------------------------------------------------------------------------------------------------
    {
        const int d = pillOf(v, "DIALS 1");
        (void)toPill(host, v, d);
        const DV::FileDiff& f = v.files()[static_cast<std::size_t>(d)];
        P.eq("dials.row", v.file() == d && f.lines.size() == 1 && f.lines[0].kind == DV::Line::Kind::changed
                              && f.lines[0].text.rfind("level:", 0) == 0 && f.lines[0].text.find("->") != std::string::npos ? 1 : 0, 1);
        std::printf("dial row: %s\n", f.lines.empty() ? "" : f.lines[0].text.c_str());
        writePng(P, host, "gui.diff.dials.png");
        const int s = pillOf(v, "SAMPLES 1");
        (void)toPill(host, v, s);
        const DV::FileDiff& g = v.files()[static_cast<std::size_t>(s)];
        P.eq("samples.added", v.file() == s && g.lines.size() == 1 && g.lines[0].kind == DV::Line::Kind::added
                                  && g.lines[0].text == "kick.wav  added" ? 1 : 0, 1);
        writePng(P, host, "gui.diff.samples.png");
        const DV::FileDiff pure = DV::samplesDiff({ { "a.wav", "1" }, { "b.wav", "2" } }, { { "b.wav", "3" }, { "c.wav", "4" } });
        P.eq("samples.pure", pure.lines.size() == 3 && pure.lines[0].text == "a.wav  removed" && pure.lines[1].text == "b.wav  replaced"
                                 && pure.lines[2].text == "c.wav  added" ? 1 : 0, 1);
        const DV::FileDiff dials = DV::dialsDiff({ { "cut", 1.0f, "", "1 kHz" }, { "q", 0.5f, "", "0.5" } },
                                                 { { "cut", 1.0f, "", "1 kHz" }, { "drive", 2.0f, "", "2" } }, "v1", "v2");
        P.eq("dials.pure", dials.lines.size() == 2 && dials.lines[0].text == "q:  0.5  (only in v1)"
                               && dials.lines[1].text == "drive:  2  (only in v2)" ? 1 : 0, 1);
    }

    // ---- NEXT ----------------------------------------------------------------------------------------------------------
    {
        const int lng = pillOf(v, "long.fm +6 -6");
        (void)toPill(host, v, lng);
        host.keys("home");
        host.settle();
        const std::string preset = port.project().preset;
        P.eq("next.long_shown", lng >= 0 && v.file() == lng && v.scroll() == 0 ? 1 : 0, 1);
        host.keys("]");
        host.settle();
        const int second = v.scroll();
        P.ge("next.second_change", second, 10);           // the first change shows at the top already
        host.keys("[");
        host.settle();
        P.eq("next.back_to_first", v.file() == lng && v.scroll() == 3 ? 1 : 0, 1);
        int presses = 0;
        while (v.file() == lng && presses < 10)
        {
            host.keys("]");
            host.settle();
            ++presses;
        }
        P.eq("next.on_to_next_pill", v.file() > lng && presses == 6 ? 1 : 0, 1);
        host.keys("[");
        host.settle();
        const int last = v.scroll();
        P.eq("next.back_to_last", v.file() == lng && last > second ? 1 : 0, 1);
        P.eq("next.presets_untouched", card.isOpen() && port.project().preset == preset && !preset.empty() ? 1 : 0, 1);
        // The last changes share the last screen (the rows clamp there): three steps back leave it, three come again.
        for (int k = 0; k < 3; ++k)
            clickAt(host, mid(card.view().places(view()).previous));
        P.eq("next.button_previous", v.file() == lng && v.scroll() < last ? 1 : 0, 1);
        for (int k = 0; k < 3; ++k)
            clickAt(host, mid(card.view().places(view()).next));
        P.eq("next.button_next", v.file() == lng && v.scroll() == last ? 1 : 0, 1);
    }

    // ---- SCROLL, KEYS --------------------------------------------------------------------------------------------------
    {
        (void)toPill(host, v, pillOf(v, "notes.fm gone"));
        P.eq("scroll.notes", DV::tabLabel(v.files()[static_cast<std::size_t>(v.file())]) == "notes.fm gone" ? 1 : 0, 1);
        host.keys("home");
        host.settle();
        P.eq("scroll.home", v.scroll() == 0 ? 1 : 0, 1);
        const funkgui::Rect d = card.view().places(view()).diff;
        host.wheel(d.centreX(), d.centreY(), -funkgui::scroll::kWheelNotch);
        host.settle();
        P.eq("scroll.notch_three_rows", v.scroll() == 3 ? 1 : 0, 1);
        host.keys("end");
        host.settle();
        P.ge("scroll.end", v.scroll(), 40);
        const int was = v.file();
        host.keys("tab");
        host.settle();
        P.eq("keys.tab", v.file() == (was + 1) % static_cast<int>(v.files().size()) ? 1 : 0, 1);
        (void)toPill(host, v, 1);
        P.eq("keys.pill_click", (clickAt(host, card.view().centreOfTab(2, view())), v.file() == 2 && card.isOpen()) ? 1 : 0, 1);
        (void)toPill(host, v, 1);
    }

    // ---- A11Y ----------------------------------------------------------------------------------------------------------
    {
        bool left = false, right = false, swap = false, next = false, pill = false, own = true;
        const std::vector<uint32_t> bases{ DC::kA11yBase, pg::ui::AbCard::kA11yBase, pg::ui::ProjectBrowser::kA11yBase,
                                           pg::ui::ExportCard::kA11yBase, pg::ui::SettingsCard::kA11yBase,
                                           pg::ui::PresetPicker::kA11yBase };
        bool apart = true;
        for (std::size_t i = 1; i < bases.size(); ++i)
            apart = apart && (bases[0] > bases[i] ? bases[0] - bases[i] : bases[i] - bases[0]) >= 0x10000u;
        for (const funkgui::A11yItem& it : host.accessibility())
        {
            left = left || (it.role == funkgui::A11yRole::comboBox && it.title == "Left side" && it.value == "v1");
            right = right || (it.role == funkgui::A11yRole::comboBox && it.title == "Right side" && it.value == "v2");
            swap = swap || (it.role == funkgui::A11yRole::button && it.title == "Swap the sides");
            next = next || (it.role == funkgui::A11yRole::button && it.title == "Next change");
            pill = pill || (it.role == funkgui::A11yRole::toggleButton && it.title == "gain.fm +1 -1" && it.checked);
            if (it.title == "Swap the sides" || it.title == "Next change" || it.title == "gain.fm +1 -1")
                own = own && it.id >= DC::kA11yBase && it.id - DC::kA11yBase < 0x10000u;
        }
        P.eq("a11y.items", left && right && swap && next && pill ? 1 : 0, 1);
        P.eq("a11y.ids_apart", apart && own ? 1 : 0, 1);
    }

    // ---- PAPER, sizes ----------------------------------------------------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::ui::Panel paper(processor.ports(), port);
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        paperHost.settle();
        paper.openCompare(project, 1, 2);
        pump(processor, paperHost, 20.0, [&] { return !paper.compareCard().reading(); });
        paperHost.settle();
        writePng(P, paperHost, "gui.diff.paper.png");
        funkgui::UiPreferences::get().setTheme(0);
    }
    {
        const bool least = host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        host.settle();
        W = static_cast<float>(pg::ui::kMinWidth);
        H = static_cast<float>(pg::ui::kMinHeight);
        const DC::Places at = card.places(W, H);
        P.eq("small.fits", least && at.box.x >= 0.0f && at.box.right() <= W && at.box.bottom() <= H
                               && card.view().places(at.view).lines >= 10 && at.openRight.right() < at.ab.x
                               && at.right.right() <= at.box.right() ? 1 : 0, 1);
        writePng(P, host, "gui.diff.small.png");
        const bool wide = host.resize(1900, 1000);
        host.settle();
        P.eq("wide.resized", wide ? 1 : 0, 1);
        W = 1900.0f;
        H = 1000.0f;
        writePng(P, host, "gui.diff.wide.png");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
        W = static_cast<float>(pg::ui::kDefaultWidth);
        H = static_cast<float>(pg::ui::kDefaultHeight);
    }
    // The Projects card's Compare... at the least window: clear of Close.
    {
        host.keys("escape");
        host.settle();
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        host.settle();
        panel.openProjects();
        host.settle();
        const pg::ui::ProjectBrowser::Places at = browser.places(static_cast<float>(pg::ui::kMinWidth), static_cast<float>(pg::ui::kMinHeight));
        P.eq("small.projects_button_clear", at.compare.right() < at.close.x && at.compare.x > at.ab.right() ? 1 : 0, 1);
        writePng(P, host, "gui.diff.projects_small.png");
        host.keys("escape");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
    }

    // ---- OPENS ---------------------------------------------------------------------------------------------------------
    {
        panel.openCompare(project, 1, 2);
        readBoth();
        clickAt(host, mid(card.places(W, H).openLeft));
        P.eq("opens.asks", card.isOpen() && card.why().find("Open anyway") != std::string::npos ? 1 : 0, 1);
        clickAt(host, mid(card.places(W, H).openLeft));
        processor.settle();
        host.tick(2);
        host.settle();
        P.eq("opens.opened", !card.isOpen() && port.project().version == 1 && port.draft().files == v1 ? 1 : 0, 1);
    }

    // ---- AB ------------------------------------------------------------------------------------------------------------
    {
        panel.openCompare(project, 1, 2);
        readBoth();
        // A double-click on A/B...: the first press opens the A/B card (COMPARE gone), the second is swallowed
        // (Panel::pressCard), never a press on the A/B card that took its place.
        {
            const funkgui::Point at = mid(card.places(W, H).ab);
            host.doubleClick(at.x, at.y);
        }
        processor.settle();
        host.settle();
        const pg::AbState st = panel.abCard().state();
        P.eq("ab.started", !card.isOpen() && panel.abCard().isOpen() && st.on && st.sides[0].version == 1 && st.sides[1].version == 2 ? 1 : 0, 1);
        host.keys("escape");
        processor.settle();
        host.settle();
        P.eq("ab.closed", !panel.abCard().isOpen() && !port.ab().on ? 1 : 0, 1);
    }

    // ---- READING -------------------------------------------------------------------------------------------------------
    {
        const juce::File f = temp.getChildFile("projects").getChildFile(juce::String(project) + ".fmproject");
        juce::var root = juce::JSON::parse(f.loadFileAsString());
        if (auto* first = root["versions"][0].getDynamicObject())
            first->setProperty("note", "rewritten by another instance, a note longer than it was");
        f.replaceWithText(juce::JSON::toString(root));
        const int parsed0 = pg::projectstore::parses(), reader0 = pg::projectstore::readerParses();
        panel.openCompare(project, 1, 2);
        // At once: the version table lacks the file as it is now (the Reader may read it before the next frame).
        P.eq("reading.shown", card.isOpen() && card.reading() ? 1 : 0, 1);
        host.settle();
        writePng(P, host, "gui.diff.reading.png");
        P.eq("reading.read", readBoth() ? 1 : 0, 1);
        P.eq("reading.diff", card.summary() == "5 files, 1 dial, 1 sample" ? 1 : 0, 1);
        P.eq("reading.none_here", (pg::projectstore::parses() - parsed0) - (pg::projectstore::readerParses() - reader0), 0);
        host.keys("escape");
        host.settle();
    }

    // ---- SPEED: a long diff made on this thread ------------------------------------------------------------------------
    {
        std::string a, b;
        for (int i = 0; i < 4000; ++i)
        {
            a += "    x" + std::to_string(i) + " = y * " + std::to_string(i) + ";\n";
            b += (i % 97 == 0 ? "    z" : "    x") + std::to_string(i) + " = y * " + std::to_string(i) + ";\n";
        }
        const double from = probe::threadMs();
        const std::vector<DV::FileDiff> d = DV::diffOf({ { "big.fm", a } }, { { "big.fm", b } });
        const double ms = probe::threadMs() - from;
        std::printf("diff of 4,000 lines, 42 changes: %.2f ms\n", ms);
        P.eq("speed.diffed", d.size() == 1 && d[0].added == 42 && d[0].removed == 42 ? 1 : 0, 1);
        P.le("speed.ms", ms, 50.0);
    }

    pg::projectstore::setDirectoryForTests({});
    temp.deleteRecursively();
    return P.finish();
}
