// Tools/probes/common/Cadence.h: an editor's frames as FunkGui runs them on screen, headless (the gui probes).
// FramePump ticks every open editor 60 times a second and runs a calm one's frame (wantsFullRate() false at its last
// frame, no input since) at every 5th tick, about 12 Hz (funkgui/gpu/FramePump.h); each frame ticks the Panel with the
// time since its last one. The probes take "calm" from what the items decide (`fullRate`, Panel::moving(): a shown item
// unsettled), without the Panel's own changes, since the top bar's CPU figure moves with real time.
#pragma once

#include <funkgui/panel/HeadlessHost.h>

namespace probe
{
    struct Cadence
    {
        int ticks = 0;                                   // the display link's, 60 a second
        int frames = 0;                                  // the editor's frames run
        double framesPerSecond() const { return ticks > 0 ? 60.0 * frames / ticks : 0.0; }
    };

    // `ticks` ticks of the link from a frame at full rate: `each(t)` before tick t (the audio and the host's automation
    // of that sixtieth of a second), and after each frame `fullRate()`, whether the editor runs at full rate from then on.
    template <class Each, class FullRate>
    Cadence runCadence(funkgui::HeadlessHost& host, int ticks, Each&& each, FullRate&& fullRate)
    {
        Cadence c;
        bool full = true;
        int since = 0;
        float dt = 0.0f;
        for (int t = 0; t < ticks; ++t)
        {
            each(t);
            dt += 1.0f / 60.0f;
            ++c.ticks;
            if (!full && ++since < 5)
                continue;
            since = 0;
            host.tick(1, dt);
            dt = 0.0f;
            ++c.frames;
            full = fullRate();
        }
        return c;
    }
}
