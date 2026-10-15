// PROBE name=gui.dump timeout=180
//
// gui.dump: the editor's Panel, headless (funkgui::HeadlessHost: no window, no GPU), over a real Processor's
// parameters. It writes the settled frame, rasterised on the CPU by FunkGui's SoftRaster at 2x, to
//   <build>/probe-out/gui.dump.png            GRAPHITE (theme 0), at the defaults
//   <build>/probe-out/gui.dump.paper.png      PAPER (theme 1), at the defaults
//   <build>/probe-out/gui.dump.envelope.png   GRAPHITE, ATTACK 40 ms, DECAY 400 ms, SUSTAIN 50 %, RELEASE 300 ms,
//                                             the pointer on DECAY
// with the ZOOM cells shown as the plugin's editor shows them, and checks that the frame settles, that every glyph is
// in the font, that every parameter with a control has its accessibility item, and that the controls write their
// parameters: a drag up on the first dial or slider raises it, a double-click returns it to its default. The envelope
// view: it is shown when the table has its four rows, follows the parameters written through their ports (SUSTAIN
// 50 % puts the hold halfway up the plot; ATTACK from 5 to 40 ms widens the rise by sqrt(8)), highlights the segment
// of the control under the pointer (one primitive more), and has its accessibility item, which reads the values out.
#include "Probe.h"

#include "editor/Panel.h"
#include "plugin/Processor.h"

#include "dsp/Params.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace
{
    std::size_t tagged(const funkgui::PrimList& frame, funkgui::Tag tag)
    {
        std::size_t n = 0;
        for (const funkgui::Prim& p : frame.prims)
            n += p.tag == tag ? 1 : 0;
        return n;
    }

    std::optional<std::size_t> indexOf(const char* id)
    {
        for (std::size_t i = 0; i < testingplaygroundsynth::kParams.size(); ++i)
            if (std::strcmp(testingplaygroundsynth::kParams[i].id, id) == 0)
                return i;
        return std::nullopt;
    }

    void write(funkgui::ParamPort& port, float plain, std::size_t param)
    {
        port.beginGesture();
        port.setValue01(testingplaygroundsynth::toNormalised(testingplaygroundsynth::kParams[param], plain));
        port.endGesture();
    }
}

namespace tps = testingplaygroundsynth;

PROBE(gui, dump)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's GUI side: the font atlas bakes through it
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    tps::Processor processor;

    std::size_t controls = 0;
    for (const tps::ParamSpec& s : tps::kParams)
        if (s.control != tps::Control::hidden)
            ++controls;

    for (const int theme : { 0, 1 })
    {
        const std::string tag = theme == 0 ? "graphite" : "paper";
        funkgui::UiPreferences::get().setTheme(theme);          // the THEME cells show the theme drawn
        tps::ui::Panel panel(processor.ports());
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
    tps::ui::Panel panel(processor.ports());
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    for (std::size_t i = 0; i < tps::kParams.size(); ++i)
    {
        const tps::Control c = tps::kParams[i].control;
        if (c != tps::Control::dial && c != tps::Control::slider)
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

    // The envelope view: shown exactly when the table has its four rows (found by ID, so this probe builds whatever
    // the table holds).
    const auto attack = indexOf("attack"), decay = indexOf("decay"), sustain = indexOf("sustain");
    const auto release = indexOf("release");
    const bool rows = attack && decay && sustain && release;
    const tps::ui::EnvelopeView* env = panel.envelope();
    P.eq("envelope.shown_with_its_four_rows", env != nullptr ? 1 : 0, rows ? 1 : 0);
    if (env != nullptr && rows)
    {
        const auto& ports = processor.ports();
        host.move(1.0f, 1.0f);                                  // the pointer over no control
        host.settle();
        const std::size_t plain = tagged(host.draw(), tps::ui::kEnvelopeTag);
        P.eq("envelope.primitives", static_cast<int64_t>(plain), 5);  // the baseline, an AREA column per segment

        write(*ports[*sustain], 50.0f, *sustain);
        host.settle();
        auto c = env->corners();
        P.near("envelope.sustain50.hold_halfway_y", c[2].y, 0.5 * (c[1].y + c[0].y), 0.01);
        P.near("envelope.sustain50.hold_flat", c[3].y - c[2].y, 0.0, 0.0);

        // The rise is drawn on a square-root time scale.
        const double attackBefore = tps::fromNormalised(tps::kParams[*attack], ports[*attack]->value01());
        const float riseBefore = c[1].x - c[0].x;
        write(*ports[*attack], 40.0f, *attack);
        write(*ports[*decay], 400.0f, *decay);
        write(*ports[*release], 300.0f, *release);
        host.settle();
        c = env->corners();
        P.near("envelope.attack40.rise_rel_before", (c[1].x - c[0].x) / riseBefore, std::sqrt(40.0 / attackBefore),
               0.01);

        const auto at = panel.centreOf(*decay);
        P.eq("envelope.decay_control_found", at.has_value() ? 1 : 0, 1);
        if (at)
        {
            host.move(at->x, at->y);
            host.settle();
            P.eq("envelope.hover_decay.highlighted_param", panel.highlightedParam(), static_cast<int64_t>(*decay));
            P.eq("envelope.hover_decay.extra_primitives",
                 static_cast<int64_t>(tagged(host.draw(), tps::ui::kEnvelopeTag)) - static_cast<int64_t>(plain), 1);
            const std::string path = probe::outPath("gui.dump.envelope.png");
            P.eq("envelope.png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
            std::printf("wrote %s\n", path.c_str());
        }

        std::string value;
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.title == "Envelope")
                value = item.value;
        std::printf("envelope a11y: %s\n", value.c_str());
        P.eq("envelope.a11y.reads_sustain", value.find("Sustain 50") != std::string::npos ? 1 : 0, 1);
    }
    return P.finish();
}
