// Source/editor/Looks.h: how the panel's dials of every style and its displays are drawn (DialLayout.h's cells), with
// FunkGui's Canvas: flat and square like the rest of the panel, the hardware a few touches of colour and shade.
//
//   vintage   a skirted black knob with a white pointer over a printed scale of eleven ticks (0 to 10)
//   fader     a slider's slot, ticks either side and a ridged cap
//   switch    a toggle's plate and its lever, the state's name under it
//   buttons   a light and a name a value, the chosen one lit
//   meter     LED bars: green, then yellow, then red near the top, a peak held a moment
//   vu        an analog VU meter: a cream face with its printed scale (-20 to +3, the red zone from 0), the needle
//   led       a lens that glows with the value
//   number    a dark readout, its digits lit
//   scope     a dark screen with a graticule and the trace
// The colours a program names (PanelSpec's kPanelColors) are tuned for each theme: panelColor().
#pragma once

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>

#include <string>
#include <vector>

namespace playground::ui::looks
{
    // A colour by its name, for this theme; `fallback` for "" or a name it does not know.
    funkgui::Col panelColor(const std::string& name, const funkgui::Theme& th, funkgui::Col fallback);
    bool dark(const funkgui::Theme& th);

    // The angle (screen radians, clockwise from 3:00) of a knob's position t (0..1): 7:30 to 4:30 through 12:00.
    float knobAngle(float t);

    struct Knob
    {
        funkgui::Rect cell;                  // the dial's cell
        bool small = false;
        float t = 0.0f;                      // its position, 0..1
        const char* label = "";
        const char* value = "";
        bool atDefault = false;              // its value drawn dim, as FunkGui's Dial does
        bool hover = false, focus = false;
        funkgui::Col color{};                // its cap's colour (`tinted`)
        bool tinted = false;
    };
    void vintage(funkgui::Canvas& c, const funkgui::Theme& th, const Knob& k);

    // A fader, a switch and buttons take the same description: t the value 0..1 (a switch: >= 0.5 is on; buttons:
    // `chosen` of `options`).
    void fader(funkgui::Canvas& c, const funkgui::Theme& th, const Knob& k);
    void toggle(funkgui::Canvas& c, const funkgui::Theme& th, const Knob& k, const char* state);
    void buttons(funkgui::Canvas& c, const funkgui::Theme& th, const Knob& k, const std::vector<std::string>& options,
                 int chosen, int hoverRow);

    // A focus ring around a cell, the panel's (square).
    void focusRing(funkgui::Canvas& c, const funkgui::Theme& th, const funkgui::Rect& cell);

    struct Display
    {
        funkgui::Rect cell;
        const char* label = "";
        funkgui::Col color{};
        bool tinted = false;
    };
    // meter: `levels` 0..1 (a bar's fill), one or two; `holds` their held peaks; `readout` under them.
    void meter(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, int count, const float* levels,
               const float* holds, const char* readout);
    // vu: the needle's position 0..1 along the scale; `audio`: the VU scale printed, else the range's ends.
    void vu(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, float needle, bool audio, const char* lo,
            const char* hi);
    void led(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, float brightness);
    void number(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, const char* text);
    // scope: `ys` in -1..1 (the screen's height), `n` of them across its width.
    void scope(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, const float* ys, int n);

    // Where on the VU scale (0..1) a level of `vu` VU sits: the needle moves with the amplitude, so -20 is near the
    // left end and 0 at about 71 %.
    float vuPosition(float vu);
}
