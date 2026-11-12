// PROBE name=gui.scroll timeout=240
//
// gui.scroll: the editor's lists scroll as its code does (editor/Scroll.h, after FunkGui's TextEditor::wheel), headless
// over a real Processor, in a folder of the probe's own with made-up projects and presets, to <build>/probe-out/:
//   gui.scroll.explorer.png   the explorer at the least size, a notch down: a row cut at its top edge
//   gui.scroll.projects.png   PROJECTS' projects a notch and a half down: a row cut at each edge, its thumb
//   gui.scroll.presets.png    the presets' list a notch down
// and checks:
//   CODE      Scroll's constants held to the TextEditor's: a code tab scrolled by one notch of a notched wheel moves
//             3 * rowHeight(), which is Scroll's rule for its row; a trackpad's 0.1 unit 51.2 px; WheelEvent::reversed
//             set, the same direction (the TextEditor never reads it); a tab that fits takes no wheel
//   SCROLL    the helper alone: a view that holds its content has no bar and takes no wheel; the offset clamped to
//             0 .. content - view; reveal the least move; a press on the thumb drags it, on the track jumps
//   EXPLORER  (the least size, the chat folded: its list taller than its card) and
//   PROJECTS  (PROJECTS' projects' list: 31 projects) and
//   PRESETS   (the top bar's presets' list: 20 presets), each: a notch moves three of its rows, a trackpad's 0.1 unit
//             51.2 px, with reversed set the code's direction; the offset stops at both ends; its thumb dragged moves
//             the content in proportion (the drag's px times the scroll's range over the track's free length); a press
//             on its track jumps the thumb under the pointer (at the track's end: the end); let go, no drag held.
//             The explorer with every section folded fits: no bar, and the Panel takes no wheel there. PROJECTS: a
//             thumb's drag chooses no project and carries none onto a group; the versions' list (one version) fits and
//             takes no wheel. The presets' list: a wheel over the search field moves nothing
// FunkGui's HeadlessHost::wheel can set neither reversed nor dx (until FunkGui's F1): those rows build the WheelEvent
// and give it to Panel::wheel, the host's own entry point, as EditorHost does.
#include "Probe.h"

#include "editor/Explorer.h"
#include "editor/Panel.h"
#include "editor/PresetPicker.h"
#include "editor/ProjectBrowser.h"
#include "editor/Scroll.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>

namespace pg = playground;
using S = pg::ui::ExplorerItem::Section;
using BL = pg::ui::ProjectBrowser::List;

namespace
{
    constexpr float kNotch = pg::ui::Scroll::kWheelNotch;       // one notch of a notched wheel, as JUCE gives it

    std::string program()
    {
        if constexpr (pg::product::kIsInstrument)
            return "// Scroll: a program\nmodule Voice\n{\n"
                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                   "    void start(float note, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// Scroll: a program\nmodule Effect\n{\n"
                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            left[i] *= level[i];\n"
                   "            right[i] *= level[i];\n        }\n    }\n};\n";
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // The WheelEvent EditorHost would give for JUCE's deltas with isReversed set (HeadlessHost cannot set it).
    funkgui::WheelEvent reversedWheel(funkgui::Point at, float dy, bool smooth = false)
    {
        funkgui::WheelEvent w;
        w.x = at.x;
        w.y = at.y;
        w.dy = dy;
        w.smooth = smooth;
        w.reversed = true;
        return w;
    }

    // A scrolling surface: its scroll as it is now, its bar's area, where the wheel goes over it.
    struct Surface
    {
        std::string key;
        std::function<pg::ui::Scroll()> scroll;
        std::function<funkgui::Rect()> area;
        funkgui::Point over;
    };

    // The rows every surface keeps (the file's header: EXPLORER, PROJECTS, PRESETS).
    void check(funkgui::test::Probe& P, funkgui::HeadlessHost& host, pg::ui::Panel& panel, const Surface& s)
    {
        const auto offset = [&] { return static_cast<double>(s.scroll().offset); };
        const auto top = [&] {
            host.wheel(s.over.x, s.over.y, 1000.0f);
            host.settle();
        };
        const pg::ui::Scroll first = s.scroll();
        const double most = first.max(), notch = 3.0 * first.row;
        std::printf("%s: content %.1f, view %.1f, row %.1f, range %.1f\n", s.key.c_str(), static_cast<double>(first.content),
                    static_cast<double>(first.view), static_cast<double>(first.row), most);
        P.eq(s.key + ".overflows", first.shown() ? 1 : 0, 1);
        P.ge(s.key + ".range_past_a_notch", most, notch + 52.0);
        top();
        P.near(s.key + ".top", offset(), 0.0, 1.0e-4);
        host.wheel(s.over.x, s.over.y, -kNotch);           // one notch down
        host.settle();
        P.near(s.key + ".notch_three_rows", offset(), notch, 0.01);
        top();
        host.wheel(s.over.x, s.over.y, -0.1f, true);       // a trackpad: 0.1 unit is 51.2 px
        host.settle();
        P.near(s.key + ".smooth_512_a_unit", offset(), 51.2, 0.01);
        top();
        panel.wheel(reversedWheel(s.over, -kNotch));       // natural scrolling on: JUCE's delta already follows it
        host.settle();
        P.near(s.key + ".reversed_as_code", offset(), notch, 0.01);
        host.wheel(s.over.x, s.over.y, -1000.0f);
        host.settle();
        P.near(s.key + ".stops_at_end", offset(), most, 0.01);
        host.wheel(s.over.x, s.over.y, -kNotch);
        host.settle();
        P.near(s.key + ".end_holds", offset(), most, 0.01);
        // The thumb dragged 20 px down from the top: the content moves 20 px times the range over the track's free length.
        top();
        const funkgui::Rect a = s.area(), t = s.scroll().thumb(a);
        const double free = static_cast<double>(a.h - t.h);
        host.drag(t.centreX(), t.centreY(), t.centreX(), t.centreY() + 20.0f, 4);
        host.settle();
        P.near(s.key + ".thumb_drags", offset(), 20.0 * most / free, 0.05);
        P.eq(s.key + ".thumb_let_go", s.scroll().dragging() ? 1 : 0, 0);
        // A press on the track's end: the thumb jumps under the pointer, so the end.
        top();
        host.click(t.centreX(), a.bottom() - 1.0f);
        host.settle();
        P.near(s.key + ".track_jumps", offset(), most, 0.01);
        top();
    }
}

PROBE(gui, scroll)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-scroll-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    pg::projectstore::setDirectoryForTests(folder);

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    pg::Draft d;
    d.files = { { "program.fm", program() } };
    port.setDraft(d);
    port.run(d.files);
    processor.settle();

    // ---- SCROLL: the helper alone --------------------------------------------------------------------------------------
    {
        pg::ui::Scroll s;
        s.content = 100.0f;
        s.view = 200.0f;
        P.eq("scroll.fits_no_bar", s.shown() ? 1 : 0, 0);
        P.eq("scroll.fits_no_wheel", s.wheel(-1.0f, false) ? 1 : 0, 0);
        s.content = 1000.0f;
        P.eq("scroll.overflows_bar", s.shown() ? 1 : 0, 1);
        P.eq("scroll.wheel_used", s.wheel(-kNotch, false) ? 1 : 0, 1);
        P.near("scroll.notch", s.offset, 3.0 * s.row, 1.0e-4);
        P.eq("scroll.edge_bump_used", s.wheel(1000.0f, false) ? 1 : 0, 1);
        P.near("scroll.clamped_top", s.offset, 0.0, 0.0);
        s.wheel(-1000.0f, false);
        P.near("scroll.clamped_end", s.offset, 800.0, 0.0);
        s.reveal(100.0f, 124.0f);
        P.near("scroll.reveal_up", s.offset, 100.0, 0.0);
        s.reveal(400.0f, 424.0f);
        P.near("scroll.reveal_down", s.offset, 224.0, 0.0);
        const funkgui::Rect a{ 0.0f, 0.0f, 100.0f, 200.0f };
        const funkgui::Rect t = s.thumb(a);
        P.near("scroll.bar_inset", static_cast<double>(a.right() - t.right()), 2.0, 0.0);
        P.near("scroll.bar_width", t.w, 4.0, 0.0);
        P.near("scroll.thumb_length", t.h, 40.0, 1.0e-4);    // the view's share of the track
        P.eq("scroll.press_beside_bar", s.press({ 80.0f, 100.0f }, a) ? 1 : 0, 0);
        P.eq("scroll.press_on_thumb", s.press({ t.centreX(), t.centreY() }, a) && s.dragging() ? 1 : 0, 1);
        s.drag({ t.centreX(), t.centreY() + 16.0f }, a);
        P.near("scroll.drag_in_proportion", s.offset, 224.0 + 16.0 * 800.0 / 160.0, 1.0e-3);
        s.release();
        P.eq("scroll.released", s.dragging() ? 1 : 0, 0);
        s.press({ 96.0f, 0.5f }, a);                        // the track's top: the thumb jumps there
        P.near("scroll.track_jump", s.offset, 0.0, 0.0);
        s.release();
        s.content = 10.0f;
        s.view = 10.0f;
        s.offset = 0.0f;
        P.near("scroll.thumb_whole_when_fits", s.thumb(a).h, 200.0, 0.0);
        s.content = 100000.0f;
        P.near("scroll.thumb_least", s.thumb(a).h, 12.0, 0.0);
    }

    // The made-up projects: thirty to scroll through, then the one open with twenty presets.
    {
        bool ok = true;
        std::string why;
        pg::SavedVersion saved;
        for (int i = 1; i <= 30; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof name, "Sketch %02d", i);
            ok = ok && port.saveVersion(name, saved, why);
        }
        ok = ok && port.saveVersion("Scroll Bed", saved, why);
        for (int i = 1; i <= 20; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof name, "Preset %02d", i);
            ok = ok && port.savePreset(name, why);
        }
        if (!why.empty())
            std::printf("setup: %s\n", why.c_str());
        P.eq("setup.saved", ok ? 1 : 0, 1);
    }

    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    processor.settle();
    host.tick(4);
    host.settle();

    // ---- CODE: the TextEditor, the reference --------------------------------------------------------------------------
    {
        funkgui::TextEditor& ed = panel.editor();
        std::string text = program();
        for (int i = 0; i < 300; ++i)
            text += "// line " + std::to_string(i) + "\n";
        ed.selectAll();
        ed.insert(text);
        host.settle();
        const funkgui::Rect b = ed.bounds();
        const funkgui::Point over{ b.centreX(), b.centreY() };
        const double rowH = ed.rowHeight();
        const auto top = [&] {
            host.wheel(over.x, over.y, 1000.0f);
            host.settle();
        };
        top();
        P.near("code.top", ed.scroll().y, 0.0, 1.0e-4);
        host.wheel(over.x, over.y, -kNotch);
        host.settle();
        P.near("code.notch_three_rows", ed.scroll().y, 3.0 * rowH, 0.01);
        P.near("code.notch_is_scrolls", ed.scroll().y, pg::ui::Scroll::pxPerUnit(false, static_cast<float>(rowH)) * kNotch, 0.01);
        top();
        host.wheel(over.x, over.y, -0.1f, true);
        host.settle();
        P.near("code.smooth_512_a_unit", ed.scroll().y, 0.1 * pg::ui::Scroll::kWheelPxPerUnit, 0.01);
        top();
        panel.wheel(reversedWheel(over, -kNotch));
        host.settle();
        P.near("code.reversed_ignored", ed.scroll().y, 3.0 * rowH, 0.01);
        ed.selectAll();
        ed.insert(program());
        host.settle();
    }

    // ---- EXPLORER ------------------------------------------------------------------------------------------------------
    {
        P.eq("explorer.least_size", host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight) ? 1 : 0, 1);
        panel.setChatOpen(false);                    // the least width has room for the explorer beside the chat's rail
        host.settle();
        pg::ui::ExplorerItem& ex = panel.explorer();
        P.eq("explorer.shown", panel.explorerShown() ? 1 : 0, 1);
        const funkgui::Rect b = ex.bounds();
        check(P, host, panel,
              { "explorer", [&] { return ex.scroller(); }, [&] { return ex.bounds(); }, { b.x + 0.5f * b.w, b.y + 0.5f * b.h } });
        host.wheel(b.centreX(), b.centreY(), -kNotch * 0.5f);   // a row and a half: one cut at the top
        host.settle();
        writePng(P, host, "gui.scroll.explorer.png");
        // Every section folded: it fits, no bar, and the Panel takes no wheel there.
        for (int s = 0; s < pg::ui::ExplorerItem::kSections; ++s)
            ex.setSectionOpen(static_cast<S>(s), false);
        host.settle();
        P.eq("explorer.folded_fits", ex.scroller().shown() ? 1 : 0, 0);
        P.eq("explorer.folded_no_wheel", panel.wheel(reversedWheel({ b.centreX(), b.y + 20.0f }, -kNotch)) ? 1 : 0, 0);
        for (int s = 0; s < pg::ui::ExplorerItem::kSections; ++s)
            ex.setSectionOpen(static_cast<S>(s), s != static_cast<int>(S::factory));
        panel.setChatOpen(true);
        P.eq("explorer.default_size", host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight) ? 1 : 0, 1);
        host.settle();
    }

    const float W = static_cast<float>(pg::ui::kDefaultWidth), H = static_cast<float>(pg::ui::kDefaultHeight);

    // ---- PROJECTS ------------------------------------------------------------------------------------------------------
    {
        panel.openProjects();
        host.settle();
        pg::ui::ProjectBrowser& b = panel.projectBrowser();
        P.eq("projects.open", b.isOpen() ? 1 : 0, 1);
        const pg::ui::ProjectBrowser::Places at = b.places(W, H);
        const std::string chosen = b.chosenProject();
        check(P, host, panel,
              { "projects", [&] { return b.scrollOf(b.places(W, H), BL::projects); }, [&] { return b.barArea(b.places(W, H), BL::projects); },
                { at.projects.centreX(), at.projects.centreY() } });
        P.eq("projects.thumb_chose_none", b.chosenProject() == chosen && b.selection().size() <= 1 ? 1 : 0, 1);
        P.eq("projects.thumb_carried_none", b.dragging() ? 1 : 0, 0);
        // The versions' list: the chosen project's one version fits, and takes no wheel.
        const pg::ui::Scroll versions = b.scrollOf(at, BL::versions);
        P.eq("projects.versions_fit", versions.shown() ? 1 : 0, 0);
        host.wheel(at.versions.centreX(), at.versions.centreY(), -kNotch);
        host.settle();
        P.near("projects.versions_no_wheel", b.scrolled(BL::versions), 0.0, 0.0);
        host.wheel(at.projects.centreX(), at.projects.centreY(), -1.5f * kNotch);   // a notch and a half: rows cut at both edges
        host.settle();
        writePng(P, host, "gui.scroll.projects.png");
        host.keys("escape");
        host.settle();
        P.eq("projects.closed", b.isOpen() ? 0 : 1, 1);
    }

    // ---- PRESETS -------------------------------------------------------------------------------------------------------
    {
        panel.openPresets();
        host.settle();
        pg::ui::PresetPicker& list = panel.presetPicker();
        P.eq("presets.open", list.isOpen() ? 1 : 0, 1);
        const pg::ui::PresetPicker::Places at = list.places(W, H);
        check(P, host, panel, { "presets", [&] { return list.scrollOf(list.places(W, H)); }, [&] { return list.places(W, H).list; },
                                { at.list.centreX(), at.list.centreY() } });
        P.eq("presets.still_open", list.isOpen() ? 1 : 0, 1);
        host.wheel(at.field.centreX(), at.field.centreY(), -kNotch);   // over the search field: nothing
        host.settle();
        P.near("presets.field_no_wheel", list.scrollOf(list.places(W, H)).offset, 0.0, 0.0);
        host.wheel(at.list.centreX(), at.list.centreY(), -kNotch * 0.5f);
        host.settle();
        writePng(P, host, "gui.scroll.presets.png");
        host.keys("escape");
        host.settle();
    }

    pg::projectstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
