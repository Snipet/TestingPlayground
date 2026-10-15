// PROBE name=gui.dump timeout=180
//
// gui.dump: the editor's Panel, headless (funkgui::HeadlessHost: no window, no GPU), over a real Processor's
// parameters. It writes the settled frame, rasterised on the CPU by FunkGui's SoftRaster at 2x, to
//   <build>/probe-out/gui.dump.png          GRAPHITE (theme 0)
//   <build>/probe-out/gui.dump.paper.png    PAPER (theme 1)
// with the ZOOM cells shown as the plugin's editor shows them, and checks that the frame settles, that every glyph is
// in the font, that every parameter with a control has its accessibility item, and that the controls write their
// parameters: a drag up on the first dial or slider raises it, a double-click returns it to its default.
#include "Probe.h"

#include "editor/Panel.h"
#include "plugin/Processor.h"

#include "dsp/Params.h"

#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <cstdio>
#include <string>
#include <vector>

namespace fpt = testingplayground;

PROBE(gui, dump)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's GUI side: the font atlas bakes through it
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    fpt::Processor processor;

    std::size_t controls = 0;
    for (const fpt::ParamSpec& s : fpt::kParams)
        if (s.control != fpt::Control::hidden)
            ++controls;

    for (const int theme : { 0, 1 })
    {
        const std::string tag = theme == 0 ? "graphite" : "paper";
        funkgui::UiPreferences::get().setTheme(theme);          // the THEME cells show the theme drawn
        fpt::ui::Panel panel(processor.ports());
        funkgui::HeadlessHost host(panel, theme, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);               // as the plugin's editor (CreateEditorGpu.cpp)
        const int frames = host.settle();
        P.le(tag + ".settle_frames", frames, 599.0);
        const funkgui::PrimList& frame = host.draw();
        P.eq(tag + ".missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
        P.ge(tag + ".primitives", static_cast<double>(frame.prims.size()), 20.0);

        std::size_t visible = 0;
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.visible && item.parent == 0)
                ++visible;
        P.ge(tag + ".a11y_items", static_cast<double>(visible), static_cast<double>(controls));

        const std::string path = probe::outPath(theme == 0 ? "gui.dump.png" : "gui.dump.paper.png");
        P.eq(tag + ".png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s (%d x %d logical px)\n", path.c_str(), panel.width(), panel.height());
    }

    // The controls write their parameters: the first dial or slider.
    funkgui::UiPreferences::get().setTheme(0);
    fpt::ui::Panel panel(processor.ports());
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    for (std::size_t i = 0; i < fpt::kParams.size(); ++i)
    {
        const fpt::Control c = fpt::kParams[i].control;
        if (c != fpt::Control::dial && c != fpt::Control::slider)
            continue;
        funkgui::ParamPort& port = *processor.ports()[i];
        const auto at = panel.centreOf(i);
        P.eq("drag.control_found", at.has_value() ? 1 : 0, 1);
        if (!at)
            break;
        port.beginGesture();
        port.setValue01(0.25f);                                 // room to move up, whatever the default
        port.endGesture();
        host.settle();
        host.drag(at->x, at->y, at->x, at->y - 60.0f);           // 60 px up: a quarter of the track
        host.settle();
        P.near("drag.up_60px.delta", port.value01() - 0.25f, 0.25, 0.02);
        host.doubleClick(at->x, at->y);
        host.settle();
        P.near("doubleclick.restores_default", port.value01(), port.default01(), 1e-6);
        break;
    }
    return P.finish();
}
