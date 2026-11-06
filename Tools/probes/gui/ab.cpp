// PROBE name=gui.ab timeout=240
//
// gui.ab: A/B's card (editor/AbCard.h) headless over a real Processor whose projects are in a temporary folder of the
// probe's own: a project of two versions of a program of two files (a sine whose level is gain.fm's K), and the
// editor's files after it (K changed, a line added to the program, a third file of notes), to <build>/probe-out/:
//   gui.ab.projects.png   PROJECTS with the project chosen: A/B... beside Export...
//   gui.ab.png            the card: Current against version 2, B playing, matched, gain.fm's diff
//   gui.ab.paper.png      the same in PAPER
//   gui.ab.notes.png      a file only A has (gone in B), scrolled
//   gui.ab.small.png      the least window
//   gui.ab.wide.png       a wide window
// and checks:
//   OPEN     A/B... in PROJECTS opens it on the chosen project: A Current, B its newest; the rows Current, v2, v1; the
//            footer's keys; the first file that changed shown
//   DIFF     a tab a file: program.fm a line taken away, gain.fm one taken away and one added, notes.fm gone; gain.fm's
//            lines: A's K taken away, B's added, numbered; the same lines around it
//   CHOOSE   a row's A chip makes it A, a click on a row B; Up moves B, Shift-Up A; the diff follows the sides
//   PLAY     Play B: on its way, then playing (its program plays, the editor's text unchanged); A and Space switch;
//            the loudness measured and said (B turned down)
//   ATTACH   another editor made while it plays (the window opened again: here PAPER's) shows its card at once, as it
//            is, nothing started again
//   SCROLL   Tab shows the next file; the wheel and End scroll a long diff, Home back
//   A11Y     the card's items: the versions' rows and chips, the files' tabs, Play A and Play B, Keep B
//   KEEP     Keep B: the card closes, the status says so, the editor shows B's files and one Cmd-Z gives the files before
//            it back (one undo step across the tabs), the project open is that version
//   ESCAPE   opened again, Escape closes it and what played before plays again
//   ENDED    opened again, another program run: the card closes on its own, the status says why
#include "Probe.h"

#include "editor/AbCard.h"
#include "editor/Panel.h"
#include "editor/ProjectBrowser.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
using AC = pg::ui::AbCard;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    std::string programText(const std::string& extra)
    {
        const std::string body = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                 "    float phase;\n    float step;\n"
                                 "    void init(float sampleRate) { step = 1000 / sampleRate; }\n";
        const std::string loop = "        for (int i = 0; i < n; ++i)\n        {\n"
                                 "            phase += step;\n            if (phase >= 1) phase -= 1;\n"
                                 "            const float y = gainK() * level[i] * sinf(6.28318531f * phase);\n";
        return kInstrument
            ? "// AB: a sine\n#include \"gain.fm\"\n" + extra + "module Voice\n{\n" + body
                  + "    bool held;\n    void start(float note, float v) { held = true; phase = 0; }\n"
                    "    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + loop
                  + "            left[i] += y;\n            right[i] += y;\n        }\n        return held;\n    }\n};\n"
            : "// AB: a sine\n#include \"gain.fm\"\n" + extra + "module Effect\n{\n" + body
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n" + loop
                  + "            left[i] = y;\n            right[i] = y;\n        }\n    }\n};\n";
    }

    std::string gainText(const std::string& k)
    {
        return "// gain.fm: the level's scale\n\n// K, the program's loudness\nfloat gainK()\n{\n    return " + k
             + "f;\n}\n\n// half of it\nfloat halfK()\n{\n    return 0.5f * gainK();\n}\n";
    }

    std::string notesText()
    {
        std::string s = "// notes.fm: what is left to do (no code)\n";
        for (int i = 1; i <= 60; ++i)
            s += "// " + std::to_string(i) + ". a line of notes\n";
        return s;
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
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        host.settle();
        return done();
    }

    void clickAt(funkgui::HeadlessHost& host, funkgui::Point p)
    {
        host.click(p.x, p.y);
        host.tick(2);
        host.settle();
    }

    int tabOf(const AC& card, const std::string& name)
    {
        for (std::size_t i = 0; i < card.files().size(); ++i)
            if (card.files()[i].name == name)
                return static_cast<int>(i);
        return -1;
    }
}

PROBE(gui, ab)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-ab-" + juce::String(juce::Time::currentTimeMillis()));
    temp.deleteRecursively();
    pg::projectstore::setDirectoryForTests(temp);

    pg::Processor processor;
    processor.compiler().setFreezing(false);
    pg::EditorPort& port = processor.editorPort();
    const std::string project = "Sine Pad";
    const pg::Sources v1{ { "program.fm", programText("") }, { "gain.fm", gainText("1.0") } };
    const pg::Sources v2{ { "program.fm", programText("") }, { "gain.fm", gainText("0.5") } };
    const pg::Sources current{ { "program.fm", programText("// the level's K is gain.fm's\n") },
                               { "gain.fm", gainText("0.25") },
                               { "notes.fm", notesText() } };
    std::string why;
    pg::SavedVersion saved;
    for (const pg::Sources* files : { &v1, &v2 })
    {
        pg::Draft d;
        d.files = *files;
        port.setDraft(d);
        port.run(d.files);
        processor.settle();
        P.eq(files == &v1 ? "setup.v1" : "setup.v2", port.saveVersion(project, saved, why) ? 1 : 0, 1);
    }
    {
        pg::Draft d;
        d.files = current;
        port.setDraft(d);
        port.run(d.files);
        processor.settle();
    }

    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    processor.settle();
    host.tick(4);
    host.settle();
    const float W = static_cast<float>(pg::ui::kDefaultWidth), H = static_cast<float>(pg::ui::kDefaultHeight);
    AC& card = panel.abCard();
    pg::ui::ProjectBrowser& browser = panel.projectBrowser();

    // ---- OPEN ----------------------------------------------------------------------------------------------------------
    panel.openProjects();
    host.settle();
    browser.chooseProjectNamed(project, W, H);
    host.settle();
    writePng(P, host, "gui.ab.projects.png");
    {
        const funkgui::Rect r = browser.places(W, H).ab;
        clickAt(host, { r.centreX(), r.centreY() });
    }
    P.eq("open.card", card.isOpen() && !browser.isOpen() ? 1 : 0, 1);
    pg::AbState st = card.state();
    P.eq("open.sides", st.on && st.sides[0].version == 0 && st.sides[1].version == 2 ? 1 : 0, 1);
    P.eq("open.rows", card.versionList().size() == 2u && card.versionList()[0].number == 2 && card.versionList()[1].number == 1 ? 1 : 0, 1);
    {
        bool group = false;
        for (const funkgui::A11yItem& it : host.accessibility())
            group = group || (it.role == funkgui::a11yrole::group && it.title == "A/B");
        P.eq("open.a11y_group", group ? 1 : 0, 1);
    }
    P.eq("open.first_changed_shown", card.file() == tabOf(card, "program.fm") ? 1 : 0, 1);

    // ---- DIFF ----------------------------------------------------------------------------------------------------------
    {
        const int prog = tabOf(card, "program.fm"), gain = tabOf(card, "gain.fm"), notes = tabOf(card, "notes.fm");
        P.eq("diff.tabs", card.files().size() == 3u && prog == 0 && gain == 1 && notes == 2 ? 1 : 0, 1);
        P.eq("diff.labels", AC::tabLabel(card.files()[0]) == "program.fm +0 -1" && AC::tabLabel(card.files()[1]) == "gain.fm +1 -1"
                                && AC::tabLabel(card.files()[2]) == "notes.fm gone" ? 1 : 0, 1);
        const AC::FileDiff& g = card.files()[1];
        bool removedA = false, addedB = false, context = false, numbered = true;
        for (const AC::Line& l : g.lines)
        {
            removedA = removedA || (l.kind == AC::Line::Kind::removed && l.text.find("return 0.25f;") != std::string::npos && l.a == 6 && l.b == 0);
            addedB = addedB || (l.kind == AC::Line::Kind::added && l.text.find("return 0.5f;") != std::string::npos && l.b == 6 && l.a == 0);
            context = context || (l.kind == AC::Line::Kind::same && l.text == "{");
            if (l.kind == AC::Line::Kind::same)
                numbered = numbered && l.a > 0 && l.b > 0;
        }
        P.eq("diff.gain_lines", removedA && addedB ? 1 : 0, 1);
        P.eq("diff.context", context && numbered ? 1 : 0, 1);
        bool gap = false;
        for (const AC::Line& l : g.lines)
            gap = gap || (l.kind == AC::Line::Kind::gap && l.text.find("the same") != std::string::npos);
        P.eq("diff.gap", gap ? 1 : 0, 1);
        const AC::FileDiff& n = card.files()[2];
        P.eq("diff.gone", n.change == AC::Change::removed && n.removed == 61 && n.lines.size() == 61u ? 1 : 0, 1);
        clickAt(host, card.centreOfTab(gain, W, H));
        P.eq("diff.tab_click", card.file() == gain ? 1 : 0, 1);
    }

    // ---- PLAY ----------------------------------------------------------------------------------------------------------
    {
        const std::string text = panel.editor().text();
        clickAt(host, { card.places(W, H).playB.centreX(), card.places(W, H).playB.centreY() });
        const bool coming = card.state().pending == 1;
        const bool landed = pump(processor, host, 20.0, [&] { return card.state().playing == 1 && card.state().pending < 0; });
        P.eq("play.b_coming_then_plays", coming && landed ? 1 : 0, 1);
        P.eq("play.b_runs", port.running() == v2 ? 1 : 0, 1);
        P.eq("play.editor_unchanged", panel.editor().text() == text ? 1 : 0, 1);
        host.keys("a");
        pump(processor, host, 20.0, [&] { return card.state().playing == 0 && card.state().pending < 0; });
        P.eq("play.key_a", card.state().playing == 0 && port.running() == current ? 1 : 0, 1);
        host.keys("space");
        pump(processor, host, 20.0, [&] { return card.state().playing == 1 && card.state().pending < 0; });
        P.eq("play.space", card.state().playing == 1 ? 1 : 0, 1);
        const bool measured = pump(processor, host, 60.0, [&] { return !card.state().measuring; });
        st = card.state();
        P.eq("play.measured", measured && st.trimDb[1] < -5.0f && st.trimDb[0] == 0.0f ? 1 : 0, 1);
        writePng(P, host, "gui.ab.png");
    }
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::ui::Panel paper(processor.ports(), port);
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        const uint64_t revision = port.abRevision();
        paperHost.tick(2);
        paperHost.settle();
        P.eq("attach.second_editor_shows_it", paper.abCard().isOpen() && paper.abCard().state().playing == 1
                                                  && paper.abCard().state().sides[1].version == 2 && port.abRevision() == revision ? 1 : 0, 1);
        paperHost.click(paper.abCard().centreOfTab(1, W, H).x, paper.abCard().centreOfTab(1, W, H).y);
        paperHost.settle();
        writePng(P, paperHost, "gui.ab.paper.png");
        funkgui::UiPreferences::get().setTheme(0);
    }

    // ---- CHOOSE --------------------------------------------------------------------------------------------------------
    {
        clickAt(host, card.centreOfRow(2, 0, W, H));     // v1's A chip
        P.eq("choose.a_chip", card.state().sides[0].version == 1 ? 1 : 0, 1);
        P.eq("choose.diff_follows", tabOf(card, "notes.fm") < 0 && tabOf(card, "gain.fm") >= 0
                                    && AC::tabLabel(card.files()[static_cast<std::size_t>(tabOf(card, "gain.fm"))]) == "gain.fm +1 -1" ? 1 : 0, 1);
        clickAt(host, card.centreOfRow(0, -1, W, H));    // Current's row: B
        P.eq("choose.row_b", card.state().sides[1].version == 0 ? 1 : 0, 1);
        host.keys("down");
        host.settle();
        P.eq("choose.down_b", card.state().sides[1].version == 2 ? 1 : 0, 1);
        host.keys("shift+up");
        host.settle();
        P.eq("choose.shift_up_a", card.state().sides[0].version == 2 ? 1 : 0, 1);
        clickAt(host, card.centreOfRow(0, 0, W, H));     // back: A Current
        clickAt(host, card.centreOfRow(1, 1, W, H));     // B v2
        pump(processor, host, 60.0, [&] { return card.state().pending < 0 && !card.state().measuring; });
        P.eq("choose.back", card.state().sides[0].version == 0 && card.state().sides[1].version == 2 ? 1 : 0, 1);
    }

    // ---- SCROLL --------------------------------------------------------------------------------------------------------
    {
        const int notes = tabOf(card, "notes.fm");
        while (card.file() != notes && notes >= 0)
        {
            const int was = card.file();
            host.keys("tab");
            host.settle();
            if (card.file() == was)
                break;
        }
        P.eq("scroll.tab_to_notes", card.file() == notes ? 1 : 0, 1);
        const funkgui::Rect d = card.places(W, H).diff;
        host.wheel(d.centreX(), d.centreY(), -1.0f);
        host.settle();
        P.ge("scroll.wheel", card.scroll(), 1);
        writePng(P, host, "gui.ab.notes.png");
        host.keys("end");
        host.settle();
        P.eq("scroll.end", card.scroll() == static_cast<int>(card.files()[static_cast<std::size_t>(notes)].lines.size()) - card.places(W, H).lines ? 1 : 0, 1);
        host.keys("home");
        host.settle();
        P.eq("scroll.home", card.scroll() == 0 ? 1 : 0, 1);
        clickAt(host, card.centreOfTab(tabOf(card, "gain.fm"), W, H));
    }

    // ---- sizes -----------------------------------------------------------------------------------------------------------
    {
        const bool least = host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        host.settle();
        const AC::Places at = card.places(static_cast<float>(pg::ui::kMinWidth), static_cast<float>(pg::ui::kMinHeight));
        P.eq("small.fits", least && at.box.x >= 0.0f && at.box.right() <= static_cast<float>(pg::ui::kMinWidth)
                               && at.box.bottom() <= static_cast<float>(pg::ui::kMinHeight) && at.lines >= 10 && at.rows >= 3
                               && at.match.right() < at.box.right() - 150.0f && at.close.x > at.match.x - 300.0f ? 1 : 0, 1);
        writePng(P, host, "gui.ab.small.png");
        const bool wide = host.resize(1900, 1000);
        host.settle();
        P.eq("wide.resized", wide ? 1 : 0, 1);
        writePng(P, host, "gui.ab.wide.png");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
    }

    // ---- A11Y ----------------------------------------------------------------------------------------------------------
    {
        bool row = false, chip = false, tab = false, playA = false, keepB = false;
        for (const funkgui::A11yItem& it : host.accessibility())
        {
            row = row || (it.role == funkgui::A11yRole::listItem && it.title == "Version 2" && it.value.rfind("B", 0) == 0);
            chip = chip || (it.role == funkgui::A11yRole::radioButton && it.title == "Current as A" && it.checked);
            tab = tab || (it.role == funkgui::A11yRole::toggleButton && it.title == "gain.fm +1 -1" && it.checked);
            playA = playA || (it.title == "Play A, Current" && it.checkable);
            keepB = keepB || (it.role == funkgui::A11yRole::button && it.title == "Keep B");
        }
        P.eq("a11y.items", row && chip && tab && playA && keepB ? 1 : 0, 1);
    }

    // ---- KEEP ----------------------------------------------------------------------------------------------------------
    {
        const std::string before = panel.editor().text();
        clickAt(host, { card.places(W, H).keepB.centreX(), card.places(W, H).keepB.centreY() });
        processor.settle();
        host.tick(2);
        host.settle();
        P.eq("keep.closed", !card.isOpen() && !port.ab().on ? 1 : 0, 1);
        P.eq("keep.status", panel.statusText().find("KEPT B: V2") != std::string::npos ? 1 : 0, 1);
        P.eq("keep.editor_b", panel.editor().text() == v2[0].text && port.draft().files == v2 ? 1 : 0, 1);
        P.eq("keep.project", port.project().name == project && port.project().version == 2 && !port.project().edited ? 1 : 0, 1);
        panel.editor().setFocused(true);
        host.keys("cmd+z");
        host.tick(2);
        host.settle();
        P.eq("keep.one_undo_step", panel.editor().text() == before ? 1 : 0, 1);
        bool gainBack = false;
        for (const pg::SourceFile& f : port.draft().files)
            gainBack = gainBack || (f.name == "gain.fm" && f.text == current[1].text);
        P.eq("keep.undo_across_tabs", gainBack ? 1 : 0, 1);
        host.keys("cmd+shift+z");
        host.tick(2);
        host.settle();
        P.eq("keep.redo", panel.editor().text() == v2[0].text ? 1 : 0, 1);
    }

    // ---- ESCAPE --------------------------------------------------------------------------------------------------------
    {
        panel.openAb(project, 1);
        host.settle();
        P.eq("escape.open", card.isOpen() && card.state().sides[1].version == 1 ? 1 : 0, 1);
        clickAt(host, { card.places(W, H).playB.centreX(), card.places(W, H).playB.centreY() });
        pump(processor, host, 20.0, [&] { return card.state().playing == 1 && card.state().pending < 0; });
        P.eq("escape.b_plays", port.running() == v1 ? 1 : 0, 1);
        host.keys("escape");
        processor.settle();
        host.settle();
        P.eq("escape.closed", !card.isOpen() && !port.ab().on ? 1 : 0, 1);
        P.eq("escape.before_plays", port.running() == v2 ? 1 : 0, 1);
        P.eq("escape.status", panel.statusText().find("A/B CLOSED") != std::string::npos ? 1 : 0, 1);
    }

    // ---- ENDED ---------------------------------------------------------------------------------------------------------
    {
        panel.openAb(project, 1);
        host.settle();
        port.run(v1);
        const bool closed = pump(processor, host, 20.0, [&] { return !card.isOpen(); });
        P.eq("ended.closes", closed ? 1 : 0, 1);
        P.eq("ended.status", panel.statusText().find("A/B ENDED") != std::string::npos ? 1 : 0, 1);
    }

    pg::projectstore::setDirectoryForTests({});
    temp.deleteRecursively();
    return P.finish();
}
