// PROBE name=gui.dump timeout=180
//
// gui.dump: the editor's Panel, headless (funkgui::HeadlessHost: no window, no GPU), over a real Processor's
// parameters and LFO shape. It writes settled frames, rasterised on the CPU by FunkGui's SoftRaster at 2x, to
//   <build>/probe-out/gui.dump.png          GRAPHITE (theme 0), the defaults
//   <build>/probe-out/gui.dump.paper.png    PAPER (theme 1), the defaults
//   <build>/probe-out/gui.dump.edited.png   GRAPHITE after the graph's edits below, a point selected
// with the ZOOM cells shown as the plugin's editor shows them, and checks:
//   FRAME   the frame settles, every glyph is in the font, every parameter with a control and the graph have their
//           accessibility items, the DELAYS line shows the Engine's lengths (787 ... 1583 at 12 m, 48 kHz)
//   CONTROL a drag up on the first dial raises its parameter by a quarter for 60 px, a double-click returns it to its
//           default
//   GRAPH   the LFO shape editor writes the processor's shape (EditorPort): a double-click on the plot adds a point
//           there; a drag moves it (60 px up: +60 / plot height in y); with Alt it snaps to the grid's eighths; a drag
//           on a segment's midpoint handle bends that segment; a drag of an edge point moves both edges; a
//           double-click on a point removes it; the right-click menu offers Delete point and Reset shape, and Reset
//           restores the default; every completed edit tells the host once that the plugin's state changed
//           (AudioProcessorListener::audioProcessorChanged with nonParameterStateChanged; a click that only selects
//           tells it nothing), and no parameter is written
//   NAN     a host writes NaN to SIZE and GAIN: the dials show the last finite values (their spoken values unchanged,
//           no "nan"), and so does the DELAYS line
#include "Probe.h"

#include "editor/Panel.h"
#include "editor/ShapeView.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "dsp/LfoShape.h"
#include "dsp/Params.h"

#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace fpt = testingplayground;

namespace
{
    // Counts what the processor tells a host about its state besides the parameters (EditorPort::endShapeEdit).
    struct HostNotes final : juce::AudioProcessorListener
    {
        int stateChanges = 0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (details.nonParameterStateChanged)
                ++stateChanges;
        }
    };

    // The spoken value of the a11y item titled `title` ("" when there is none).
    std::string a11yValue(funkgui::HeadlessHost& host, const char* title)
    {
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.title == title)
                return item.value;
        return {};
    }
}

PROBE(gui, dump)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's GUI side: the font atlas bakes through it
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    HostNotes notes;                                            // outlives the processor it listens to
    fpt::Processor processor;
    fpt::EditorPort& port = processor.editorPort();
    processor.addListener(&notes);
    int told = 0;                                               // notes.stateChanges at the last check
    const auto toldSince = [&]() {
        const int n = notes.stateChanges - told;
        told = notes.stateChanges;
        return static_cast<int64_t>(n);
    };

    std::size_t controls = 1;                                   // the graph
    for (const fpt::ParamSpec& s : fpt::kParams)
        if (s.control != fpt::Control::hidden)
            ++controls;

    for (const int theme : { 0, 1 })
    {
        const std::string tag = theme == 0 ? "graphite" : "paper";
        funkgui::UiPreferences::get().setTheme(theme);          // the THEME cells show the theme drawn
        fpt::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, theme, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);               // as the plugin's editor (CreateEditorGpu.cpp)
        const int frames = host.settle();
        P.le(tag + ".settle_frames", frames, 599.0);
        const funkgui::PrimList& frame = host.draw();
        P.eq(tag + ".missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
        P.ge(tag + ".primitives", static_cast<double>(frame.prims.size()), 100.0);

        std::size_t visible = 0;
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.visible && item.parent == 0)
                ++visible;
        P.ge(tag + ".a11y_items", static_cast<double>(visible), static_cast<double>(controls));

        const std::string path = probe::outPath(theme == 0 ? "gui.dump.png" : "gui.dump.paper.png");
        P.eq(tag + ".png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s (%d x %d logical px)\n", path.c_str(), panel.width(), panel.height());
    }

    funkgui::UiPreferences::get().setTheme(0);
    fpt::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();

    // FRAME: the DELAYS line.
    const std::string delays = panel.delaysText();
    std::printf("%s\n", delays.c_str());
    P.eq("frame.delays.starts_787", delays.rfind("DELAYS 787 ", 0) == 0 ? 1 : 0, 1);
    P.eq("frame.delays.has_1583", delays.find(" 1583 SAMPLES AT 48.0 KHZ") != std::string::npos ? 1 : 0, 1);

    // CONTROL: the first dial.
    {
        const std::size_t i = fpt::param::size;
        funkgui::ParamPort& p = *processor.ports()[i];
        const auto at = panel.centreOf(i);
        P.eq("control.found", at.has_value() ? 1 : 0, 1);
        if (at)
        {
            p.beginGesture();
            p.setValue01(0.25f);                                // room to move up, whatever the default
            p.endGesture();
            host.settle();
            host.drag(at->x, at->y, at->x, at->y - 60.0f);       // 60 px up: a quarter of the track
            host.settle();
            P.near("control.drag_up_60px.delta", p.value01() - 0.25f, 0.25, 0.02);
            host.doubleClick(at->x, at->y);
            host.settle();
            P.near("control.doubleclick.restores_default", p.value01(), p.default01(), 1e-6);
        }
    }

    // GRAPH
    const fpt::ui::ShapeView& view = panel.shapeView();
    const funkgui::Rect plot = view.plot();
    std::vector<float> before;
    for (const auto* p : processor.ports())
        before.push_back(p->value01());
    const int menusBefore = host.log.menus;
    P.eq("graph.default_points", static_cast<int64_t>(port.shape().count), 4);

    // A double-click at x 0.4, y 0.7 of the plot (on the curve, away from every point and handle) adds a point.
    const float ax = plot.x + 0.4f * plot.w, ay = plot.y + (1.0f - 0.7f) * plot.h;
    toldSince();
    host.doubleClick(ax, ay);
    host.settle();
    fpt::LfoShape s = port.shape();
    P.eq("graph.doubleclick.adds_point", static_cast<int64_t>(s.count), 5);
    P.eq("graph.doubleclick.host_told_once", toldSince(), 1);
    const int added = view.selected();
    P.eq("graph.doubleclick.selects_it", added == 2 ? 1 : 0, 1);
    if (added >= 0 && added < s.count)
    {
        const fpt::ShapePoint& p = s.points[static_cast<std::size_t>(added)];
        P.near("graph.doubleclick.point_x", p.x, 0.4, 1.0 / plot.w);
        P.near("graph.doubleclick.point_y", p.y, 0.7, 1.0 / plot.h);
    }

    // A drag moves it up by 30 px.
    {
        const funkgui::Point at = view.pointAt(2);
        host.drag(at.x, at.y, at.x, at.y - 30.0f);
        host.settle();
        s = port.shape();
        P.near("graph.drag.point_y", s.points[2].y, 0.7 + 30.0 / plot.h, 1.0 / plot.h);
        P.near("graph.drag.point_x_kept", s.points[2].x, 0.4, 1.0 / plot.w);
        P.eq("graph.drag.host_told_once", toldSince(), 1);
    }

    // With Alt it snaps to the grid's eighths.
    {
        const funkgui::Point at = view.pointAt(2);
        funkgui::Mods alt;
        alt.alt = true;
        host.drag(at.x, at.y, at.x + 0.07f * plot.w, at.y + 0.05f * plot.h, 8, alt);
        host.settle();
        s = port.shape();
        const fpt::ShapePoint& p = s.points[2];
        P.near("graph.alt_drag.x_on_grid", p.x * 8.0f - std::round(p.x * 8.0f), 0.0, 1e-6);
        P.near("graph.alt_drag.y_on_grid", p.y * 8.0f - std::round(p.y * 8.0f), 0.0, 1e-6);
        P.eq("graph.alt_drag.host_told_once", toldSince(), 1);
    }

    // A drag on the midpoint handle of segment 0 bends it.
    {
        const float curve0 = port.shape().points[0].curve;
        const funkgui::Point m = view.midpointAt(0);
        host.drag(m.x, m.y, m.x, m.y + 0.15f * plot.h);
        host.settle();
        P.eq("graph.bend.curve_changed", port.shape().points[0].curve != curve0 ? 1 : 0, 1);
        P.eq("graph.bend.host_told_once", toldSince(), 1);
    }

    // An edge point moves both edges.
    {
        const funkgui::Point at = view.pointAt(0);
        host.drag(at.x, at.y, at.x + 20.0f, at.y - 0.1f * plot.h);
        host.settle();
        s = port.shape();
        P.near("graph.edge_drag.first_x_pinned", s.points[0].x, 0.0, 0.0);
        P.near("graph.edge_drag.first_y", s.points[0].y, 0.6, 1.5 / plot.h);
        P.near("graph.edge_drag.last_y_follows", s.points[static_cast<std::size_t>(s.count - 1)].y, s.points[0].y, 0.0);
        P.eq("graph.edge_drag.host_told_once", toldSince(), 1);
    }

    // The edited frame: a point selected (a click on point 2).
    {
        const funkgui::Point at = view.pointAt(2);
        host.click(at.x, at.y);
        host.settle();
        P.eq("graph.select_click.host_told_nothing", toldSince(), 0);
        host.draw();
        const std::string path = probe::outPath("gui.dump.edited.png");
        P.eq("graph.edited_png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // A double-click on the point removes it.
    {
        const funkgui::Point at = view.pointAt(2);
        host.doubleClick(at.x, at.y);
        host.settle();
        P.eq("graph.doubleclick_point.removes_it", static_cast<int64_t>(port.shape().count), 4);
        P.eq("graph.doubleclick_point.host_told_once", toldSince(), 1);
    }

    // The menu: on a point, Delete point is offered; Reset shape restores the default.
    {
        const funkgui::Point at = view.pointAt(1);
        funkgui::Mods ctrl;
        ctrl.ctrl = true;
        host.click(at.x, at.y, ctrl);
        const funkgui::MenuRequest* menu = host.pendingMenu();
        P.eq("graph.menu.shown", menu != nullptr ? 1 : 0, 1);
        if (menu != nullptr)
        {
            P.eq("graph.menu.items", static_cast<int64_t>(menu->items.size()), 3);
            P.eq("graph.menu.delete_enabled", !menu->items.empty() && menu->items[0].enabled ? 1 : 0, 1);
            P.eq("graph.menu.reset_chosen", host.chooseMenuItem("Reset shape") ? 1 : 0, 1);
        }
        host.settle();
        P.eq("graph.menu.reset_restores_default", port.shape() == fpt::defaultShape() ? 1 : 0, 1);
        P.eq("graph.menu.reset.host_told_once", toldSince(), 1);
    }

    // No parameter moved, and the host was not asked for a parameter menu.
    bool same = true;
    for (std::size_t i = 0; i < before.size(); ++i)
        same = same && processor.ports()[i]->value01() == before[i];
    P.eq("graph.no_parameter_written", same ? 1 : 0, 1);
    P.eq("graph.no_param_menu", static_cast<int64_t>(host.log.menus - menusBefore), 0);

    // NAN: a host writes NaN to SIZE and GAIN.
    {
        const std::string sizeBefore = a11yValue(host, "SIZE"), gainBefore = a11yValue(host, "GAIN");
        const std::string delaysBefore = panel.delaysText();
        for (const std::size_t i : { fpt::param::size, fpt::param::gain })
            processor.getParameters()[static_cast<int>(i)]->setValueNotifyingHost(
                std::numeric_limits<float>::quiet_NaN());
        host.settle();
        host.draw();
        const std::string sizeAfter = a11yValue(host, "SIZE"), gainAfter = a11yValue(host, "GAIN");
        std::printf("after a host NaN: SIZE '%s' (was '%s'), GAIN '%s' (was '%s')\n", sizeAfter.c_str(),
                    sizeBefore.c_str(), gainAfter.c_str(), gainBefore.c_str());
        P.eq("nan.size.shows_last_value", !sizeBefore.empty() && sizeAfter == sizeBefore ? 1 : 0, 1);
        P.eq("nan.gain.shows_last_value", !gainBefore.empty() && gainAfter == gainBefore ? 1 : 0, 1);
        P.eq("nan.delays.unchanged", panel.delaysText() == delaysBefore ? 1 : 0, 1);
    }
    processor.removeListener(&notes);
    return P.finish();
}
