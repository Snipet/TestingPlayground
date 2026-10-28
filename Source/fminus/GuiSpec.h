// Source/fminus/GuiSpec.h: a program's interface from a .gui file (JUCE-free), richer than #pragma panel lines
// (PanelSpec.h): containers that lay their children out as CSS's flexbox does, pages behind tabs, controls of every
// kind with their own labels, units and tapers, displays, and text. A program has one: the first of its files whose
// name ends in .gui (panel.gui). FMinus never reads it; with one, the program's #pragma panel lines are left out.
//
// The language, a line an element (or several, between ;), a block of children between { and }:
//
//   element argument argument key=value key="value" {
//       child ...
//   }
//
// An argument or a value is a name (cutoff, log), a "text", a number (12, -0.5) or a colour (#e07b39). // and /* */
// are comments. The file's elements stack top to bottom, as in a column; `panel` may hold them instead, with the
// panel's own colours (background=#1d1b19 accent=orange).
//
// Containers (props: gap= between children, pad= inside, align= across: stretch (the default), start, center, end;
// justify= along: start, center, end, between, around; grow= its share of the room left in its parent; width= and
// height= at least that many px; wrap: a row's children onto more lines when it is too narrow):
//   row { }                 its children left to right
//   column { }              top to bottom
//   grid cols=4 { }         in rows of cols, each column as wide as its widest child
//   section "FILTER" { }    a frame, its title cut into the top edge (color=, layout=row (the default) or column)
//   box { }                 a filled frame without a title (color=, layout=column (the default) or row)
//   tabs { page "OSC" { } page "FILTER" { } }   pages, one shown at a time, chosen by the tabs over them
//   spacer                  room (width=, height=; without them it grows)
//   divider                 a line across a row or down a column (color=, thickness=)
//   label "TEXT"            text (size=small, normal, large, title or px 10 to 44; align=left, center, right; color=)
// Any element: margin= (room around it), self= (its own align=), minwidth= maxwidth= minheight= maxheight=.
//
// Looks. section and box: look=frame (a section's default), card, inset or plate; radius=, fill=, border= (a colour or
// none), borderwidth=, shadow=; a section's titlecolor=, titlealign=, titlesize=. row and column: fill=, border=,
// radius=. tabs: look=tabs (the default), bar (a header bar), pills or side (a list at the left). panel: background=,
// gradient= (the ground's colour at the bottom), accent=.
//
// Controls, each a parameter's (one control a parameter), with label="..." (else the parameter's name) and color=:
//   knob cutoff             size=small, medium, large or a face's px (24 to 240); look= (style=) modern (the default),
//                           vintage, flat, minimal, ring, led, chicken, soft or dot; arc=, track=, cap=, pointer=line,
//                           dot, triangle or none, pointercolor=, thickness=, sweep= (degrees), ticks=, tickcolor=,
//                           bipolar, labelpos=top, bottom or none, value=always, hover or never, valuecolor=,
//                           labelcolor=, shadow=; for a float also unit="Hz" decimals=1 scale=100 (shows 0..1 as
//                           0..100) taper=log (a frequency's or a time's: every octave the same turn; its range must be
//                           above 0), which the host's text and its automation follow too
//   slider level            vertical, or `slider pan horizontal`; length= px; unit=, decimals=, scale=, taper=; arc=,
//                           track=, cap=, thickness=, bipolar, labelpos=, value=
//   switch sync "OFF" "ON"  a bool, or an int of two values; the texts name them
//   buttons wave "SAW" "SQUARE" "TRI"   a lit button a value (2 to 8), in a column, or a row: buttons wave horizontal
//   menu mode "A" "B" "C"   a drop-down of an int's values (or a bool's), the texts their names
//   xy cutoff resonance     a pad: x the first parameter, y the second; size= px (120)
//   envelope attack decay sustain release   the ADSR shape its four parameters make, drawn (width=, height=)
//   keyboard from=48 keys=25   an instrument's keys, which play notes (octaves= instead of keys=; color=)
//   shader "glow.sc" inputs="cutoff env out"   a fragment shader of the program's own files, drawn by the GPU over
//                           its cell (width=, height=; it stretches as a scope does), four at most: its inputs (up to
//                           eight: parameters, fields, in, out) are its in0..in7 (fminus/Shaders.h); the compiler
//                           builds it, and a shader that does not compile is a note on its own line
// A knob's or a slider's live=field marks where a field of the program has the parameter now (a hidden display's tap).
//
// Classes and defaults, anywhere in the file (read first): `class "big" size=72 arc=yellow` gives its properties to an
// element with class="big" (several: class="big warm", in order); `defaults knob look=ring` gives them to every knob.
// An element's own properties come last, so they win; a class's or the defaults' property an element does not take is
// left out without a note.
//
// Displays, of `in` or `out` (the plugin's audio: an effect's input, the output) or a float, int or bool field of the
// main module the program writes (an instrument's from its newest sounding voice), with label=, color=, and min= max=
// for a field (0 and 1 when unsaid):
//   meter out   vu out   led gate   number cutoffHz   scope out   spectrum out
// scope and spectrum (an analyser of in or out) take width= and height=.
//
// A parameter that no control names comes last, in a section without a title. What the plugin cannot use is a note,
// "panel.gui:12:5: note: ...", and the rest stands.
#pragma once

#include "fminus/PanelSpec.h"

#include <string>
#include <string_view>
#include <vector>

namespace playground
{
    inline constexpr int kMaxGuiNodes = 400;             // elements a .gui file may have
    inline constexpr int kMaxGuiDepth = 12;              // containers in containers
    inline constexpr int kMaxMenuOptions = 32;
    inline constexpr std::size_t kMaxGuiShaders = 4;     // shader elements a panel draws
    inline constexpr std::size_t kMaxGuiText = 48;       // a label's characters

    // Whether a file's name makes it a .gui file.
    bool isGuiFile(std::string_view name);

    // The interface `gui` asks for, for the program's `controls` (the taper, unit, decimals and scale it gives a dial
    // go into them), `fields` (its main module's; an instrument has no `in`) and `fileNames` (the program's files, which
    // a shader names). Notes, a line each, are appended.
    PanelSpec readGuiSpec(const SourceFile& gui, Controls& controls, const std::vector<PanelField>& fields,
                          bool instrument, const std::vector<std::string>& fileNames, std::string& notes);
}
