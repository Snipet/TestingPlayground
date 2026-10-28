// Source/fminus/PanelSpec.h: how a program asks for its dials to be laid out and what else its panel shows
// (JUCE-free), in #pragma panel lines. FMinus ignores every #pragma (as C ignores the pragmas it does not know); the
// plugin reads these from the files it compiled (the program and what it includes), in that order, and the editor
// draws them as an analog synth's panel is printed: titled sections, framed, in rows.
//
//   #pragma panel section "FILTER" cutoff resonance gr     a section: its title (may be ""), then its items in order:
//                                                          dials, and displays by their source's name
//   #pragma panel row                                      the sections after it start a new row
//   #pragma panel label envamount "AMOUNT"                 a dial's label (else its parameter's name)
//   #pragma panel small detune lforate                     small knobs: no value line under them
//   #pragma panel style attack decay fader                 how dials are drawn (the style last): knob (the default),
//                                                          vintage, fader, switch (a bool, or an int of two values),
//                                                          buttons (an int or a bool, a lit button a value)
//   #pragma panel options wave "SAW" "SQUARE" "TRI"        the names of an int's values from its range's low end (a
//                                                          bool's: off, on), on its buttons, its dial and to the host
//   #pragma panel color "FILTER" orange                    a section's colour (by its title) or an item's (by name)
//
// Displays show what the program does, read on the audio thread each block (Engine.h):
//   #pragma panel meter out "OUTPUT"                       LED bars
//   #pragma panel vu out                                   an analog VU meter, a needle
//   #pragma panel led gate "GATE"                          a light
//   #pragma panel number cutoffHz "HZ"                     a readout
//   #pragma panel scope out                                an oscilloscope
// Their source: `in` or `out`, the plugin's audio (an effect's input, the output; left and right, in dBFS), or a
// float, int or bool field of the main module that the program writes (`gr`, `env`), shown between a range given
// after it (default 0 1: #pragma panel meter gr 0 24 "GR"); an instrument's from its newest sounding voice. A
// field's scope is its trace over the last seconds. A section names a display by its name, the source's unless
// `as` gives another, which a second display of one source needs: #pragma panel scope out as wave "SCOPE".
//
// The sections go left to right in the order of their lines, and to another row when one is full. Dials and
// displays no section names come last, in a section of their own without a title. A line, or a part of one, the
// plugin cannot use is a note (its file and line); the rest stands.
//
// A program with a .gui file (GuiSpec.h) has its interface from that file instead: PanelSpec::gui holds its tree,
// and the labels, options, styles, colours and displays below are what the tree's controls and displays ask for.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace playground
{
    inline constexpr std::size_t kMaxSectionTitle = 24;   // characters; longer ones are cut
    inline constexpr std::size_t kMaxDialLabel = 14;
    inline constexpr std::size_t kMaxOptionLabel = 10;
    inline constexpr int kMaxDisplays = 16;               // displays (and so field taps) a program may have
    inline constexpr int kMaxOptions = 8;                 // values a switch or buttons may name

    enum class DisplayKind : uint8_t { meter, vu, led, number, scope, spectrum };   // spectrum: a .gui file's
    enum class DialStyle : uint8_t { knob, vintage, fader, toggle, buttons };   // toggle: #pragma panel style x switch

    struct PanelDisplay
    {
        DisplayKind kind = DisplayKind::meter;
        std::string name;                                   // what a section line calls it: the source's, or `as`'s
        std::string source;                                 // "in", "out", or a field of the main module
        std::string label;                                  // "": the source's name
        float lo = 0.0f, hi = 1.0f;                         // a field's range
        int tap = -1;                                       // a field's tap (Patch::taps), in display order; -1: audio
        std::string color;                                  // a colour's name (kPanelColors), "" for the default
        bool hidden = false;                                // a tap only, for a control's live= (never drawn)
        bool audio() const noexcept { return source == "in" || source == "out"; }
        bool operator==(const PanelDisplay&) const = default;
    };

    struct PanelItem
    {
        bool display = false;                               // a display (PanelSpec::displays), else a dial
        int index = 0;                                      // the dial's slot, or the display's index
        bool operator==(const PanelItem&) const = default;
    };

    struct PanelSection
    {
        std::string title;                                  // as written; "" for none
        std::vector<PanelItem> items;                       // its dials and displays, in order
        bool newRow = false;                                // a row starts with it
        std::string color;
        bool operator==(const PanelSection&) const = default;
    };

    // ---- a .gui file's interface (GuiSpec.h) ----------------------------------------------------------------------
    enum class GuiKind : uint8_t
    {
        row, column, grid, section, box, tabs, page, spacer, divider, label,     // layout and decoration
        knob, slider, toggle, buttons, menu, xy, envelope,                       // controls (a parameter's or more)
        display,                                                                 // a display (PanelSpec::displays)
        keyboard,                                                                // an instrument's keys, played
        shader                                                                   // a program's own fragment shader
    };
    enum class GuiAlign : uint8_t { stretch, start, center, end };              // across a container's direction
    enum class GuiJustify : uint8_t { start, center, end, between, around };    // along it, when there is room
    enum class GuiSize : uint8_t { small, medium, large };
    enum class GuiText : uint8_t { small, normal, large, title };               // a label's size

    // How an element is drawn, past its kind's defaults: what a knob, a slider, a container or tabs may say (GuiSpec.h
    // lists which takes what). "" and -1 (or 0 for a length) leave the kind's own.
    struct GuiStyle
    {
        std::string look;               // a knob: modern, vintage, flat, minimal, ring, led, chicken, soft, dot; a
                                        // section or box: frame, card, inset, plate; tabs: tabs, bar, pills, side
        std::string arc, track, cap, pointerColor, ticks, label, value;   // a knob's or a slider's colours ("none")
        std::string fill, border, title;                                  // a container's colours ("none" for none)
        int pointer = -1;               // 0 line, 1 dot, 2 triangle, 3 none
        float px = 0.0f;                // a knob's face, its diameter in px (else its size=)
        float thickness = 0.0f;         // a knob's arc, px
        float sweep = 0.0f;             // a knob's travel, degrees (270)
        int tickCount = -1;             // a knob's scale, its ticks (0 none)
        bool bipolar = false;           // a knob's or a slider's arc from its middle
        int labelPos = -1;              // 0 top, 1 bottom, 2 none
        int valueShow = -1;             // 0 always, 1 while hovered or dragged, 2 never
        int shadow = -1;                // 0 none, 1 a soft shadow under it
        float radius = -1.0f;           // a container's corners, px
        float borderWidth = -1.0f;      // px
        int titleAlign = -1;            // 0 left, 1 centre, 2 right
        int titleSize = -1;             // a GuiText
        bool operator==(const GuiStyle&) const = default;
    };

    struct GuiNode
    {
        GuiKind kind = GuiKind::column;
        int parent = -1;
        std::vector<int> children;                          // in order
        int line = 0;                                       // its line in the .gui file
        std::string text;                                   // a section's or a page's title, a label's text
        int slot = -1, slot2 = -1;                          // a control's parameter (an xy pad's x, then y)
        std::array<int, 4> env{ -1, -1, -1, -1 };           // an envelope's attack, decay, sustain, release
        int display = -1;                                   // a display's index in PanelSpec::displays
        bool vertical = true;                               // a container's direction: column (and a page, a box or a
                                                            // section that says layout=column), a slider's, buttons'
        float gap = 8.0f, pad = 0.0f, width = 0.0f, height = 0.0f, grow = 0.0f;   // px; 0 width/height: its own
                                                            // (an xy pad's size= sets both)
        int cols = 0;                                       // a grid's
        GuiAlign align = GuiAlign::stretch;
        GuiJustify justify = GuiJustify::start;
        GuiSize size = GuiSize::medium;                     // a knob's
        bool vintage = false;                               // a knob's style
        GuiText textSize = GuiText::normal;
        float textPx = 0.0f;                                // a label's size in px, when it gives a number
        int textAlign = -1;                                 // a label's: 0 left, 1 centre, 2 right
        std::string color;                                  // a name (kPanelColors) or #rrggbb, "" for the default
        GuiStyle style;
        // Flex, past gap, pad, align, justify, grow: wrap a row's or a column's children onto more lines; the
        // element's own alignment across its container (-1: the container's align=); room around it; its bounds.
        bool wrap = false;
        int self = -1;                                      // a GuiAlign
        float margin = 0.0f;
        float minW = 0.0f, maxW = 0.0f, minH = 0.0f, maxH = 0.0f;   // px; 0: none
        int live = -1;                                      // a control's live= field: its tap's display (hidden)
        int from = 48, keys = 25;                           // a keyboard's lowest note and its keys
        bool wheels = false;                                // and its pitch and mod wheels, at its left
        std::string file;                                   // a shader's source file
        std::array<std::string, 8> inputs{};                // a shader's inputs: parameters, fields, in, out
        int shader = -1;                                    // a shader's program (PanelSpec::shaders)
        bool operator==(const GuiNode&) const = default;
    };

    struct GuiTree
    {
        std::vector<GuiNode> nodes;                         // nodes[0] the root, a column; empty without a .gui file
        std::string file;                                   // the .gui file's name
        std::string background, accent;                     // the panel's own colours ("" for the theme's)
        std::string gradient;                               // the background's colour at its bottom (a vertical blend)
        bool empty() const noexcept { return nodes.empty(); }
        bool operator==(const GuiTree&) const = default;
    };

    // A .gui file's shader element's program (Shaders.h): its file, its inputs' names, and what the compiler built.
    struct PanelShader
    {
        std::string file;
        std::vector<std::string> inputs;                    // as inputs= names them (the shader's in_<name>)
        std::vector<uint8_t> binary;                        // shaderc's output; empty: not built (a note says why)
        uint32_t id = 0;                                    // shaderId(binary), FunkGui's ShaderLibrary's
        bool operator==(const PanelShader&) const = default;
    };

    struct PanelSpec
    {
        GuiTree gui;                                        // a .gui file's interface; empty: the #pragma panel lines'
        std::vector<PanelShader> shaders;                   // its shaders' programs, by GuiNode::shader
        std::vector<PanelSection> sections;                 // empty: the dials in plain rows (no section, no display)
        std::vector<PanelDisplay> displays;
        std::array<std::string, kMaxControls> labels{};     // "": the parameter's name
        std::array<bool, kMaxControls> small{};
        std::array<DialStyle, kMaxControls> styles{};
        std::array<std::vector<std::string>, kMaxControls> options{};   // an int's or a bool's values' names
        std::array<std::string, kMaxControls> colors{};
        bool operator==(const PanelSpec&) const = default;
    };

    // A field of the main module, as FMinus describes it ("float", "int", "bool", "float[16]", "param float", ...).
    struct PanelField
    {
        std::string name, type;
    };

    // The colours a panel line may name.
    inline constexpr const char* kPanelColors[] = { "red",  "orange", "yellow", "green", "teal",
                                                    "blue", "violet", "pink",   "cream", "grey" };

    // The panel `files` ask for (the files compiled, the program first) for `controls`, the program's dials, and
    // `fields`, its main module's (an instrument has no `in`). Notes, a line each, are appended to `notes`:
    // "program.fm:12:1: note: #pragma panel: ...".
    PanelSpec readPanelSpec(const std::vector<const SourceFile*>& files, const Controls& controls,
                            const std::vector<PanelField>& fields, bool instrument, std::string& notes);
}
