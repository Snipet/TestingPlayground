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

    // MIDI learn's mark on a dial (the plugin's editor: plugin/MidiLearn.h), at `at`: a dot while a MIDI control moves
    // the dial, a ring while it learns one; nothing else. A dial's cell (DialLayout.h) has it at its right edge, just
    // under its label's line (midiMarkAt), clear of a knob's marks, a fader's ticks and a switch's plate in every style
    // and size, and of the label however long; a .gui file's control places its own (GuiControlItem::midiMarkAt).
    inline constexpr float kMidiMarkR = 3.0f;          // the dot's radius (the ring's 1 px more)
    inline constexpr float kMidiMarkInset = 6.0f;      // its centre from the cell's right edge
    inline constexpr float kMidiMarkTop = 17.0f;       // ... and from its top
    inline funkgui::Point midiMarkAt(const funkgui::Rect& cell)
    {
        return { cell.right() - kMidiMarkInset, cell.y + kMidiMarkTop };
    }
    void midiMark(funkgui::Canvas& c, const funkgui::Theme& th, funkgui::Point at, bool mapped, bool learning);

    struct Display
    {
        funkgui::Rect cell;
        const char* label = "";
        funkgui::Col color{};
        bool tinted = false;
    };
    // meter: `levels` 0..1 (a bar's fill), one or two; `holds` their held peaks; `readout` under them. Laid out in its
    // cell, whatever its size: the label at its top, the readout at its bottom (smaller when it would not fit the
    // cell's width, half size at least, and past that from its left, cut at its right), the bars between them and no
    // wider than the cell; a cell too short for bars of kMeterLeast px and the readout drops the readout. In a cell
    // 80 px high (the #pragma panel's, a .gui file's own size) the bars are 46 px, as they always were.
    inline constexpr float kMeterLeast = 15.0f;
    void meter(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, int count, const float* levels,
               const float* holds, const char* readout);
    // vu: the needle's position 0..1 along the scale; `audio`: the VU scale printed, else the range's ends.
    void vu(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, float needle, bool audio, const char* lo,
            const char* hi);
    void led(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, float brightness);
    // number: `text` in its box (the cell's width less 4), smaller when it would not fit (half size at least) and cut
    // at the box past that, so a readout never spills onto its neighbours. kReadout is its digits' style, which a
    // .gui file's layout measures a number's cell with.
    inline constexpr funkgui::TextStyle kReadout{ 15.0f, 0.6f, 0.14f, true };
    void number(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, const char* text);
    // A formatted number's widest usual readout, as formatValue writes it (DisplayItem): a sign and four digits, its
    // decimals (0 to 4), its unit (Hz's as kHz): what a layout measures its cell with in kReadout (the box keeps 3 px
    // each side, and the cell 2 more).
    std::string readoutSample(const std::string& unit, int decimals);
    // scope: `ys` in -1..1 (the screen's height), `n` of them across its width.
    void scope(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, const float* ys, int n);
    // The px a scope in `cell` draws a value of 1 from its middle (a change of a value by 0.5 / scopeScale is half a
    // px of its trace: DisplayItem's tolerance).
    float scopeScale(const funkgui::Rect& cell);

    // Where on the VU scale (0..1) a level of `vu` VU sits: the needle moves with the amplitude, so -20 is near the
    // left end and 0 at about 71 %.
    float vuPosition(float vu);
}
