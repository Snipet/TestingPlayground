// PROBE name=fminus.gui timeout=120
//
// fminus.gui: a program's interface from a .gui file (Source/fminus/GuiSpec.h), as Patch::compile reads it for this
// plugin's kind:
//   TREE      every element of a rich file (panel's colours, rows, columns, a grid, sections, a box, tabs and pages,
//             labels, a divider, a spacer, every control and display) in a tree, in order, with what each says: its
//             title or text, its parameter's slot, gap, pad, align, justify, grow, width, height, cols, layout, size,
//             style, direction, colours named and #hex; options and labels for the host; no note
//   LEX       // and /* */ comments, ; between elements, a { on the next line, #rgb as #rrggbb, commas as spaces
//   CONTROLS  taper=log maps the knob geometrically (its middle at the range's geometric mean) and back; unit=,
//             decimals= and scale= make the value's text (Hz from 1000 up as kHz), and a typed text maps back
//   DISPLAYS  meter, vu, led, number, scope and spectrum of out and of the main module's fields, a field's tap in
//             order, two of one source; min= and max=
//   SHARED    a display of a field of the module Shared through the main module's pointer (number shared->level,
//             meter shared->notes, led shared->on; live=shared->level), tapped in order like the main module's; an
//             int's and a bool's display whole numbers; a number's unit=, decimals= and scale= kept; a field Shared
//             lacks, an array of it, a pointer that is not the Shared* one, and Shared's fields in a program without
//             one are notes as the main module's are; number out with unit= is a note (dBFS); the #pragma panel lines
//             take shared->level too; a display given no label is labelled by Shared's field's own name ("level")
//   REST      a parameter no control names comes last, in a section without a title, with a note naming it
//   NOTES     what the plugin cannot use is a note on its line and column, and the rest stands: an unknown element,
//             property or value, a parameter that is none or has a control already, a switch, buttons or a menu on a
//             parameter it does not suit, a pad on an int, an envelope without four parameters, a { without its },
//             a } that closes nothing, a "text" without its ", a colour that is none, a number with a unit, tabs
//             holding no page, a page outside tabs, a label without text, a log taper from 0, decimals out of range;
//             a second .gui file, and #pragma panel lines beside a .gui file, are notes too
//   STRAY     a { where an element's name should be (the whole file {, {{{, not a panel {{{, a { inside a row) is read
//             as if its braces were not there: one note at it, its } taken with it, its elements its holder's; one
//             the file ends inside is "this { has no }"; every such file compiles and plays, and the reader returns
//             on every file of up to six tokens of row, knob, cutoff, {, }, =, a line end and a "text", with notes a
//             few a token; a file of thousands of broken tokens says at most a page of notes and a line that the rest
//             were left out; containers nested 100,000 deep return, with one note at the first past the limit
//   PRAGMAS   without a .gui file the #pragma panel lines still make the panel (no tree)
//   LIMITS    each at its edge and one past it: 64 dials (a knob of the 33rd and the 64th, the panel's own dials:
//             isPanelDial, at their slots; the 65th parameter none, a note), 32 displays (number f1 to f32, each its
//             tap in order; the 33rd a note, left out), a shader of 16 inputs (inputs= of 17 names: the first 16
//             kept in order, the 17th a note)
//   STYLES    classes and defaults reach their elements (an element's own property beats its class's, which beats
//             the defaults'); every look and style property lands in its node (tabs, sections, boxes, rows that wrap,
//             knobs, sliders, labels, dividers, the panel's gradient, an instrument's keyboard); live= taps its field
//             once for every control that names it; a mistake in them is a note on its line and column
//   POINTER   area and the pointer's properties (x=, y=, down=, wheel=, hover=, drag=, release=, cursor=) on an
//             area, a box, a label, a display and a shader, each its slot; the pointer's words among a shader's inputs
//             (and read so before the compile); a param a pointer writes may have a knob too, and one with none comes in
//             no section of the rest; the notes (an int under x=, no such param, a host parameter under hover=, a knob
//             that takes presses, a bool under wheel=, drag= without x= or y=, an empty area, a cursor alone, a word
//             drag= does not take, a text), each on its line, the property left out; the 17th pointer element a note;
//             the pointer's words counted among a shader's 16 inputs
//   ELEMENTS  a double field's display, an array's element's (number bands[1], live=bands[2]), and a field's attributes as
//             what a display leaves out: ui::label and fminus::range (a meter's label, min and max), ui::unit and
//             ui::decimals (a number's), each beaten by the display's own (unit="" says none); no note; a label and a
//             unit too long for a display, and an option's name too long for its button, cut where a character starts,
//             with a note on its line; the Patch's taps (one a
//             display, in order) reading what the program wrote in a double, an element and a 2-D array's (live=), and
//             again after a panel's edit (Patch::readPanel over the recipe) that moves them about and names others
#include "Probe.h"

#include "fminus/GuiSpec.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // A program of this plugin's kind with these parameters and fields (and `before`: pragma lines).
    std::string program(const std::string& before = "")
    {
        const std::string body = "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                                 "    param float resonance [[fminus::range(0, 1)]] [[fminus::default(0.2)]];\n"
                                 "    param float attack [[fminus::range(0.001, 2)]] [[fminus::default(0.01)]];\n"
                                 "    param float decay [[fminus::range(0.001, 2)]] [[fminus::default(0.3)]];\n"
                                 "    param float sustain [[fminus::range(0, 1)]] [[fminus::default(0.6)]];\n"
                                 "    param float release [[fminus::range(0.001, 4)]] [[fminus::default(0.5)]];\n"
                                 "    param int wave [[fminus::range(0, 2)]] [[fminus::default(0)]];\n"
                                 "    param bool sync [[fminus::default(0)]];\n"
                                 "    param int mode [[fminus::range(0, 1)]] [[fminus::default(0)]];\n"
                                 "    param int voices [[fminus::range(1, 16)]] [[fminus::default(4)]];\n"
                                 "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                 "    param float pan [[fminus::range(-1, 1)]] [[fminus::default(0)]];\n"
                                 "    float env;\n    bool gate;\n    int count;\n";
        if constexpr (kInstrument)
            return before + "module Voice\n{\n" + body + "    void start(float n, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return before + "module Effect\n{\n" + body + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    // The slots of the parameters, in declaration order.
    enum Slot : int { cutoff, resonance, attack, decay, sustain, release, wave, syncSlot, mode, voices, level, pan };

    struct Compiled
    {
        bool ok = false;
        pg::PanelSpec panel;
        pg::Controls controls{};
        std::string notes;
    };

    Compiled compile(const pg::Sources& files)
    {
        pg::Patch::Result r = pg::Patch::compile(files, 48000.0f);
        Compiled c;
        c.ok = r.patch != nullptr;
        c.notes = r.notes;
        if (r.patch)
        {
            c.panel = r.patch->panel();
            c.controls = r.patch->controls();
        }
        if (!r.errors.empty())
            std::printf("errors:\n%s", r.errors.c_str());
        return c;
    }

    Compiled withGui(const std::string& gui, const std::string& before = "")
    {
        return compile({ { pg::kProgramName, program(before) }, { "panel.gui", gui } });
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    int countLines(const std::string& s)
    {
        int n = 0;
        for (const char c : s)
            n += c == '\n' ? 1 : 0;
        return n;
    }

    // The first node of this kind (and, when given, this text or slot), else -1.
    int find(const pg::GuiTree& t, pg::GuiKind kind, const std::string& text = "", int slot = -1)
    {
        for (std::size_t i = 0; i < t.nodes.size(); ++i)
        {
            const pg::GuiNode& n = t.nodes[i];
            if (n.kind == kind && (text.empty() || n.text == text) && (slot < 0 || n.slot == slot))
                return static_cast<int>(i);
        }
        return -1;
    }

    const pg::GuiNode& at(const pg::GuiTree& t, int i)
    {
        static const pg::GuiNode none{};
        return i >= 0 && static_cast<std::size_t>(i) < t.nodes.size() ? t.nodes[static_cast<std::size_t>(i)] : none;
    }

    std::string text(const pg::Control& c, float plain)
    {
        char buf[32];
        pg::formatValue(c, plain, buf, sizeof buf);
        return buf;
    }
}

PROBE(fminus, gui)
{
    // ---- TREE ----------------------------------------------------------------------------------------------------------
    {
        const std::string gui = R"GUI(// The interface of a test program.
panel background=#1d1b19 accent=orange {
    label "TEST SYNTH" size=title color=cream
    row gap=12 pad=4 align=center justify=between {
        section "FILTER" color=#e07b39 layout=column {
            knob cutoff size=large taper=log unit="Hz" label="CUTOFF"
            knob resonance style=vintage size=small decimals=2
        }
        section "ENVELOPE" {
            slider attack length=120
            slider decay
            slider sustain scale=100 unit="%" decimals=0
            slider release
            envelope attack decay sustain release label="AMP" width=160 height=90
        }
        divider color=grey
        box color=teal layout=row {
            buttons wave "SAW" "SQUARE" "TRI" horizontal
            switch sync "FREE" "SYNC"
            menu mode "POLY" "MONO" width=90
        }
    }
    tabs color=blue {
        page "MOD" {
            xy level pan size=140 label="LEVEL / PAN"
        }
        page "METERS" layout=row {
            grid cols=3 gap=6 {
                meter out label="OUT"
                vu out
                led gate
                number count min=0 max=16
                scope env min=0 max=1 width=200 height=70
                spectrum out height=100
            }
        }
    }
    spacer height=12
    knob voices grow=1
}
)GUI";
        const Compiled c = withGui(gui);
        if (!c.notes.empty())
            std::printf("notes:\n%s", c.notes.c_str());
        P.eq("tree.compiles", c.ok ? 1 : 0, 1);
        P.eq("tree.no_notes", c.notes.empty() ? 1 : 0, 1);
        const pg::GuiTree& t = c.panel.gui;
        P.eq("tree.file", t.file == "panel.gui" ? 1 : 0, 1);
        P.eq("tree.root_column", !t.nodes.empty() && t.nodes[0].kind == pg::GuiKind::column ? 1 : 0, 1);
        P.eq("tree.panel_colours", t.background == "#1d1b19" && t.accent == "orange" ? 1 : 0, 1);
        // The root's children, in order: the label, the row, the tabs, the spacer, the knob.
        std::vector<pg::GuiKind> top;
        for (const int ch : at(t, 0).children)
            top.push_back(at(t, ch).kind);
        P.eq("tree.top_order", top == std::vector<pg::GuiKind>{ pg::GuiKind::label, pg::GuiKind::row, pg::GuiKind::tabs,
                                                                pg::GuiKind::spacer, pg::GuiKind::knob } ? 1 : 0, 1);
        const pg::GuiNode& title = at(t, find(t, pg::GuiKind::label));
        P.eq("tree.label", title.text == "TEST SYNTH" && title.textSize == pg::GuiText::title && title.color == "cream" ? 1 : 0, 1);
        const pg::GuiNode& row = at(t, find(t, pg::GuiKind::row));
        P.eq("tree.row_props", row.gap == 12.0f && row.pad == 4.0f && row.align == pg::GuiAlign::center
                                   && row.justify == pg::GuiJustify::between && !row.vertical && row.children.size() == 4 ? 1 : 0, 1);
        const pg::GuiNode& filter = at(t, find(t, pg::GuiKind::section, "FILTER"));
        P.eq("tree.section", filter.color == "#e07b39" && filter.vertical && filter.children.size() == 2 ? 1 : 0, 1);
        const pg::GuiNode& env = at(t, find(t, pg::GuiKind::section, "ENVELOPE"));
        P.eq("tree.section_row", !env.vertical && env.children.size() == 5 ? 1 : 0, 1);
        const pg::GuiNode& big = at(t, find(t, pg::GuiKind::knob, "", cutoff));
        P.eq("tree.knob_large", big.size == pg::GuiSize::large && !big.vintage && big.parent == find(t, pg::GuiKind::section, "FILTER") ? 1 : 0, 1);
        const pg::GuiNode& res = at(t, find(t, pg::GuiKind::knob, "", resonance));
        P.eq("tree.knob_vintage_small", res.vintage && res.size == pg::GuiSize::small ? 1 : 0, 1);
        const pg::GuiNode& att = at(t, find(t, pg::GuiKind::slider, "", attack));
        P.eq("tree.slider_length", att.vertical && att.height == 120.0f ? 1 : 0, 1);
        const pg::GuiNode& adsr = at(t, find(t, pg::GuiKind::envelope));
        P.eq("tree.envelope", adsr.env == std::array<int, 4>{ attack, decay, sustain, release } && adsr.text == "AMP"
                                  && adsr.width == 160.0f && adsr.height == 90.0f ? 1 : 0, 1);
        P.eq("tree.divider", at(t, find(t, pg::GuiKind::divider)).color == "grey" ? 1 : 0, 1);
        const pg::GuiNode& box = at(t, find(t, pg::GuiKind::box));
        P.eq("tree.box", box.color == "teal" && !box.vertical && box.children.size() == 3 && box.pad == 8.0f ? 1 : 0, 1);
        const pg::GuiNode& buttons = at(t, find(t, pg::GuiKind::buttons, "", wave));
        P.eq("tree.buttons_row", !buttons.vertical ? 1 : 0, 1);
        P.eq("tree.options", c.panel.options[wave] == std::vector<std::string>{ "SAW", "SQUARE", "TRI" }
                                 && c.panel.options[syncSlot] == std::vector<std::string>{ "FREE", "SYNC" }
                                 && c.panel.options[mode] == std::vector<std::string>{ "POLY", "MONO" } ? 1 : 0, 1);
        P.eq("tree.menu", find(t, pg::GuiKind::menu, "", mode) >= 0 && at(t, find(t, pg::GuiKind::menu, "", mode)).width == 90.0f ? 1 : 0, 1);
        P.eq("tree.switch", find(t, pg::GuiKind::toggle, "", syncSlot) >= 0 ? 1 : 0, 1);
        const int tabs = find(t, pg::GuiKind::tabs);
        P.eq("tree.tabs", tabs >= 0 && at(t, tabs).children.size() == 2 && at(t, tabs).color == "blue"
                              && at(t, at(t, tabs).children[0]).text == "MOD" && at(t, at(t, tabs).children[1]).text == "METERS"
                              && !at(t, at(t, tabs).children[1]).vertical ? 1 : 0, 1);
        const pg::GuiNode& pad = at(t, find(t, pg::GuiKind::xy));
        P.eq("tree.xy", pad.slot == level && pad.slot2 == pan && pad.width == 140.0f && pad.text == "LEVEL / PAN" ? 1 : 0, 1);
        const pg::GuiNode& grid = at(t, find(t, pg::GuiKind::grid));
        P.eq("tree.grid", grid.cols == 3 && grid.gap == 6.0f && grid.children.size() == 6 ? 1 : 0, 1);
        P.eq("tree.labels", c.panel.labels[cutoff] == "CUTOFF" ? 1 : 0, 1);
        const pg::GuiNode& spacer = at(t, find(t, pg::GuiKind::spacer));
        P.eq("tree.spacer", spacer.height == 12.0f && spacer.grow == 0.0f ? 1 : 0, 1);
        P.eq("tree.grow", at(t, find(t, pg::GuiKind::knob, "", voices)).grow == 1.0f ? 1 : 0, 1);
        P.eq("tree.no_rest", find(t, pg::GuiKind::section, "") < 0 || at(t, find(t, pg::GuiKind::section, "")).text != "" ? 1 : 0, 1);

        // ---- DISPLAYS ----------------------------------------------------------------------------------------------
        const std::vector<pg::PanelDisplay>& d = c.panel.displays;
        P.eq("displays.count", static_cast<int64_t>(d.size()), 6);
        if (d.size() == 6)
        {
            P.eq("displays.kinds", d[0].kind == pg::DisplayKind::meter && d[1].kind == pg::DisplayKind::vu
                                       && d[2].kind == pg::DisplayKind::led && d[3].kind == pg::DisplayKind::number
                                       && d[4].kind == pg::DisplayKind::scope && d[5].kind == pg::DisplayKind::spectrum ? 1 : 0, 1);
            P.eq("displays.audio_no_tap", d[0].tap == -1 && d[1].tap == -1 && d[5].tap == -1 ? 1 : 0, 1);
            P.eq("displays.field_taps", d[2].tap == 0 && d[3].tap == 1 && d[4].tap == 2 ? 1 : 0, 1);
            P.eq("displays.two_of_out", d[0].name == "out" && d[1].name == "out2" && d[5].name == "out3" ? 1 : 0, 1);
            P.eq("displays.range", d[3].lo == 0.0f && d[3].hi == 16.0f ? 1 : 0, 1);
            P.eq("displays.label", d[0].label == "OUT" ? 1 : 0, 1);
            const pg::GuiNode& scope = at(t, find(t, pg::GuiKind::display) + 4);
            P.eq("displays.scope_size", scope.display == 4 && scope.width == 200.0f && scope.height == 70.0f ? 1 : 0, 1);
        }

        // ---- CONTROLS ----------------------------------------------------------------------------------------------
        const pg::Control& f = c.controls[cutoff];
        P.eq("controls.taper", f.taper == pg::ControlTaper::log ? 1 : 0, 1);
        P.near("controls.log_middle", static_cast<double>(pg::plainOf(f, 0.5f)), std::sqrt(20.0 * 20000.0), 0.5);
        P.near("controls.log_back", static_cast<double>(pg::normOf(f, 632.455f)), 0.5, 1e-4);
        P.near("controls.log_ends", static_cast<double>(pg::plainOf(f, 0.0f)) + static_cast<double>(pg::plainOf(f, 1.0f)), 20020.0, 0.1);
        P.eq("controls.hz", text(f, 440.0f) == "440 Hz" ? 1 : 0, 1);
        P.eq("controls.khz", text(f, 2500.0f) == "2.50 kHz" ? 1 : 0, 1);
        P.near("controls.parse_k", static_cast<double>(pg::parseValue(f, "2.5k")), 2500.0, 1e-3);
        const pg::Control& s = c.controls[sustain];
        P.eq("controls.percent", text(s, 0.6f) == "60 %" ? 1 : 0, 1);
        P.near("controls.parse_scaled", static_cast<double>(pg::parseValue(s, "60")), 0.6, 1e-5);
        P.eq("controls.decimals", text(c.controls[resonance], 0.25f) == "0.25" ? 1 : 0, 1);
        P.eq("controls.linear_others", c.controls[attack].taper == pg::ControlTaper::linear ? 1 : 0, 1);
    }

    // ---- LEX -----------------------------------------------------------------------------------------------------------
    {
        const std::string gui = "/* a block\n   comment */ row // a row\n{\n  knob cutoff; knob resonance, size=small\n"
                                "  label \"A \\\"QUOTE\\\"\" color=#e73\n}\nknob attack\n";
        const Compiled c = withGui(gui);
        const pg::GuiTree& t = c.panel.gui;
        const int row = find(t, pg::GuiKind::row);
        P.eq("lex.brace_next_line", row >= 0 && at(t, row).children.size() == 3 ? 1 : 0, 1);
        P.eq("lex.semicolon_comma", find(t, pg::GuiKind::knob, "", resonance) >= 0
                                        && at(t, find(t, pg::GuiKind::knob, "", resonance)).size == pg::GuiSize::small ? 1 : 0, 1);
        const pg::GuiNode& l = at(t, find(t, pg::GuiKind::label));
        P.eq("lex.escaped_quote", l.text == "A \"QUOTE\"" ? 1 : 0, 1);
        P.eq("lex.short_hex", l.color == "#ee7733" ? 1 : 0, 1);
        P.eq("lex.after_block", find(t, pg::GuiKind::knob, "", attack) >= 0 && at(t, find(t, pg::GuiKind::knob, "", attack)).parent == 0 ? 1 : 0, 1);
        // The rest (9 parameters) in a section without a title, with a note naming them: REST.
        const int rest = find(t, pg::GuiKind::section);
        P.eq("rest.section", rest >= 0 && at(t, rest).text.empty() && at(t, rest).parent == 0 && at(t, rest).children.size() == 9 ? 1 : 0, 1);
        P.eq("rest.note", has(c.notes, "decay, sustain, release, wave, sync, mode, voices, level, pan have no control") ? 1 : 0, 1);
        P.eq("rest.only_note", countLines(c.notes), 1);
    }

    // ---- NOTES ---------------------------------------------------------------------------------------------------------
    {
        const std::string gui = "knob cutof\n"                                   // 1: no parameter
                                "frob cutoff\n"                                  // 2: unknown element
                                "knob cutoff size=huge wobble=3\n"               // 3: bad value, unknown property
                                "slider cutoff\n"                                // 4: a control already
                                "switch wave\n"                                  // 5: three values
                                "buttons voices\n"                               // 6: sixteen values
                                "menu level\n"                                   // 7: a float
                                "xy wave pan\n"                                  // 8: an int
                                "envelope attack decay sustain\n"                // 9: three
                                "label color=red\n"                              // 10: no text
                                "tabs { knob sync }\n"                           // 11: not a page
                                "page \"LOST\" { knob resonance }\n"             // 12: page outside tabs
                                "knob attack color=puce\n"                       // 13: no colour
                                "row gap=12px { knob decay }\n"                  // 14: a unit
                                "knob sustain taper=log\n"                       // 15: range from 0
                                "knob release decimals=9\n"                      // 16: out of range
                                "meter nofield\n"                                // 17: no field
                                "label \"OPEN\n"                                 // 18: no closing quote
                                "}\n"                                            // 19: closes nothing
                                "row {\n"                                        // 20: never closed
                                "  knob level\n";
        const Compiled c = withGui(gui);
        std::printf("notes:\n%s", c.notes.c_str());
        P.eq("notes.compiles", c.ok ? 1 : 0, 1);
        P.eq("notes.no_param", has(c.notes, "panel.gui:1:6: note: there is no parameter cutof") ? 1 : 0, 1);
        P.eq("notes.unknown_element", has(c.notes, "panel.gui:2:1: note: unknown element 'frob'") ? 1 : 0, 1);
        P.eq("notes.bad_value", has(c.notes, "panel.gui:3:13: note: size= takes small, medium or large, not 'huge'") ? 1 : 0, 1);
        P.eq("notes.unknown_property", has(c.notes, "panel.gui:3:23: note: knob takes") && has(c.notes, "wobble= is left out") ? 1 : 0, 1);
        P.eq("notes.twice", has(c.notes, "panel.gui:4:8: note: cutoff has a control already") ? 1 : 0, 1);
        P.eq("notes.switch_misfit", has(c.notes, "panel.gui:5:8: note: a switch takes a bool, or an int of two values: wave is an int of 3 values") ? 1 : 0, 1);
        P.eq("notes.buttons_misfit", has(c.notes, "panel.gui:6:9: note: buttons take") && has(c.notes, "voices is an int of 16 values") ? 1 : 0, 1);
        P.eq("notes.menu_misfit", has(c.notes, "panel.gui:7:6: note: a menu takes") ? 1 : 0, 1);
        P.eq("notes.xy_int", has(c.notes, "panel.gui:8:1: note: an xy pad moves floats: wave is an int") ? 1 : 0, 1);
        P.eq("notes.envelope_three", has(c.notes, "panel.gui:9:1: note: envelope takes four parameters") ? 1 : 0, 1);
        P.eq("notes.label_text", has(c.notes, "panel.gui:10:1: note: label takes a \"text\"") ? 1 : 0, 1);
        P.eq("notes.tabs_page", has(c.notes, "panel.gui:11:8: note: tabs hold pages") ? 1 : 0, 1);
        P.eq("notes.page_outside", has(c.notes, "panel.gui:12:1: note: a page goes in tabs") ? 1 : 0, 1);
        P.eq("notes.page_children_kept", find(c.panel.gui, pg::GuiKind::knob, "", resonance) >= 0 ? 1 : 0, 1);
        P.eq("notes.colour", has(c.notes, "panel.gui:13:13: note: 'puce' is not a colour") ? 1 : 0, 1);
        P.eq("notes.unit", has(c.notes, "note: a number takes no unit after it: 'px'") ? 1 : 0, 1);
        P.eq("notes.unit_value_kept", at(c.panel.gui, find(c.panel.gui, pg::GuiKind::row)).gap == 12.0f ? 1 : 0, 1);
        P.eq("notes.log_from_zero", has(c.notes, "panel.gui:15:14: note: a log taper needs a range above 0") ? 1 : 0, 1);
        P.eq("notes.log_stays_linear", c.controls[sustain].taper == pg::ControlTaper::linear ? 1 : 0, 1);
        P.eq("notes.decimals_range", has(c.notes, "panel.gui:16:14: note: decimals=9 is outside 0 to 4: 4 instead") ? 1 : 0, 1);
        P.eq("notes.no_field", has(c.notes, "panel.gui:17:7: note: there is no field nofield") ? 1 : 0, 1);
        P.eq("notes.open_quote", has(c.notes, "panel.gui:18:7: note: a \"text\" without its closing \"") ? 1 : 0, 1);
        P.eq("notes.stray_close", has(c.notes, "panel.gui:19:1: note: a } that closes nothing") ? 1 : 0, 1);
        P.eq("notes.unclosed", has(c.notes, "panel.gui:20:5: note: this { has no }") ? 1 : 0, 1);
        P.eq("notes.rest_stands", find(c.panel.gui, pg::GuiKind::knob, "", level) >= 0 && find(c.panel.gui, pg::GuiKind::knob, "", cutoff) >= 0 ? 1 : 0, 1);
    }

    // ---- STRAY ---------------------------------------------------------------------------------------------------------
    {
        // Each of these returns (the reader moved on), with its notes and its tree.
        {
            const Compiled c = withGui("{");
            std::printf("STRAY {:\n%s", c.notes.c_str());
            P.eq("stray.brace_compiles", c.ok ? 1 : 0, 1);
            P.eq("stray.brace_noted", has(c.notes, "panel.gui:1:1: note: a { with no element before it: what it holds is read as if "
                                                   "the braces were not there") ? 1 : 0, 1);
            P.eq("stray.brace_unclosed", has(c.notes, "panel.gui:1:1: note: this { has no } (the file ends inside it)") ? 1 : 0, 1);
            P.eq("stray.brace_rest", find(c.panel.gui, pg::GuiKind::knob, "", cutoff) >= 0 ? 1 : 0, 1);
            P.le("stray.brace_notes", countLines(c.notes), 3);
        }
        {
            const Compiled c = withGui("{{{");
            P.eq("stray.three_compiles", c.ok ? 1 : 0, 1);
            P.eq("stray.three_noted", has(c.notes, "panel.gui:1:1: note: a { with no element") && has(c.notes, "panel.gui:1:2: note: a { with no element")
                                          && has(c.notes, "panel.gui:1:3: note: a { with no element") ? 1 : 0, 1);
            P.eq("stray.three_unclosed", has(c.notes, "panel.gui:1:3: note: this { has no }") ? 1 : 0, 1);
            P.le("stray.three_notes", countLines(c.notes), 7);
        }
        {
            const Compiled c = withGui("not a panel {{{");
            std::printf("STRAY not a panel {{{:\n%s", c.notes.c_str());
            P.eq("stray.after_name_compiles", c.ok ? 1 : 0, 1);
            P.eq("stray.after_name_unknown", has(c.notes, "panel.gui:1:1: note: unknown element 'not'") ? 1 : 0, 1);
            P.eq("stray.after_name_noted", has(c.notes, "panel.gui:1:14: note: a { with no element") && has(c.notes, "panel.gui:1:15: note: a { with no element")
                                               && has(c.notes, "panel.gui:1:13: note: this { has no }") ? 1 : 0, 1);
            P.le("stray.after_name_notes", countLines(c.notes), 8);
        }
        {
            const Compiled c = withGui("row {\n    {\n        knob cutoff\n    }\n}\nknob resonance\n");
            std::printf("STRAY in a row:\n%s", c.notes.c_str());
            const pg::GuiTree& t = c.panel.gui;
            const int row = find(t, pg::GuiKind::row), knob = find(t, pg::GuiKind::knob, "", cutoff);
            P.eq("stray.inner_compiles", c.ok ? 1 : 0, 1);
            P.eq("stray.inner_noted", has(c.notes, "panel.gui:2:5: note: a { with no element before it") ? 1 : 0, 1);
            P.eq("stray.inner_child_kept", row >= 0 && knob >= 0 && at(t, knob).parent == row && at(t, row).children.size() == 1 ? 1 : 0, 1);
            P.eq("stray.inner_after", find(t, pg::GuiKind::knob, "", resonance) >= 0
                                          && at(t, find(t, pg::GuiKind::knob, "", resonance)).parent == at(t, row).parent ? 1 : 0, 1);
            P.eq("stray.inner_closed", has(c.notes, "closes nothing") || has(c.notes, "has no }") ? 0 : 1, 1);
            P.eq("stray.inner_one_note", has(c.notes, "a { with no element") && countLines(c.notes) == 2 ? 1 : 0, 1);   // and REST's
        }

        // The reader alone (readGuiSpec, as compile() calls it) over every file of up to six of these tokens.
        const Compiled base = withGui("knob cutoff\n");
        const std::vector<pg::PanelField> noFields;
        const std::vector<std::string> names{ pg::kProgramName, "panel.gui" };
        const auto read = [&](const std::string& text) {
            pg::Controls controls = base.controls;
            std::string notes;
            (void) pg::readGuiSpec({ "panel.gui", text }, controls, noFields, kInstrument, names, notes);
            return notes;
        };
        const char* const words[] = { "row", "knob", "cutoff", "{", "}", "=", "\n", "\"T\"" };
        constexpr int kWords = 8;
        const double start = probe::threadMs();
        long files = 0;
        int worst = 0;                                   // the most note lines, less 3 a token
        std::string worstFile;
        for (int n = 1; n <= 6; ++n)
        {
            std::vector<int> pick(static_cast<std::size_t>(n), 0);
            for (;;)
            {
                std::string text;
                for (const int w : pick)
                    text += std::string(words[w]) + " ";
                const int over = countLines(read(text)) - 3 * n;
                if (over > worst)
                {
                    worst = over;
                    worstFile = text;
                }
                ++files;
                std::size_t k = 0;
                while (k < pick.size() && ++pick[k] == kWords)
                    pick[k++] = 0;
                if (k == pick.size())
                    break;
            }
        }
        const double ms = probe::threadMs() - start;
        std::printf("STRAY: %ld files read in %.0f ms (thread), the most notes past 3 a token %d: %s\n", files, ms, worst, worstFile.c_str());
        P.eq("stray.every_file_read", files, 8L + 64 + 512 + 4096 + 32768 + 262144);
        P.le("stray.notes_few_a_token", worst, 0);   // at most 3 note lines a token
        P.le("stray.every_file_ms", ms, 120000.0);

        // However broken the file, a page of notes: each part says 100 at most, then that the rest were left out.
        std::string broken;
        for (int k = 0; k < 20000; ++k)
            broken += k % 2 == 0 ? "{ @ " : "= \"T\" } ";
        const std::string many = read(broken);
        std::printf("STRAY broken: %zu bytes of notes, %d lines\n", many.size(), countLines(many));
        P.le("stray.capped_lines", countLines(many), 3 * 101 + 1);
        P.eq("stray.capped_said", has(many, "panel.gui: note: more than 100 notes: the rest are left out") ? 1 : 0, 1);

        // Nested far past the limit: one note there, and no stack spent on what is left out.
        std::string deep;
        for (int k = 0; k < 100000; ++k)
            deep += "row {";
        const std::string deepNotes = read(deep);
        std::printf("STRAY deep: %d lines\n", countLines(deepNotes));
        P.eq("stray.deep_noted", has(deepNotes, "panel.gui:1:" + std::to_string(5 * pg::kMaxGuiDepth + 5) + ": note: containers nest") ? 1 : 0, 1);
        P.le("stray.deep_lines", countLines(deepNotes), 3 * 101 + 1);
    }

    // ---- two .gui files, pragmas beside one, and PRAGMAS alone -------------------------------------------------------
    {
        const std::string pragmas = "#pragma panel section \"F\" cutoff resonance\n";
        const Compiled both = compile({ { pg::kProgramName, program(pragmas) }, { "panel.gui", "knob cutoff\n" },
                                        { "other.gui", "knob level\n" } });
        P.eq("files.second_gui", has(both.notes, "other.gui: note: the interface is panel.gui's") ? 1 : 0, 1);
        P.eq("files.pragmas_left_out", has(both.notes, "the #pragma panel lines are left out") ? 1 : 0, 1);
        P.eq("files.gui_wins", !both.panel.gui.empty() && both.panel.sections.empty() ? 1 : 0, 1);
        const Compiled alone = compile({ { pg::kProgramName, program(pragmas) } });
        P.eq("pragmas.no_tree", alone.panel.gui.empty() && !alone.panel.sections.empty() ? 1 : 0, 1);
        P.eq("pragmas.no_note_about_gui", has(alone.notes, ".gui") ? 0 : 1, 1);
        P.eq("pragmas.is_gui_file", pg::isGuiFile("panel.gui") && !pg::isGuiFile("gui") && !pg::isGuiFile("panel.fm") ? 1 : 0, 1);
    }

    // ---- STYLES --------------------------------------------------------------------------------------------------------
    {
        const std::string keys = kInstrument ? "  keyboard from=36 octaves=3 height=80 wheels\n" : "  spacer\n";
        const Compiled c = withGui("class \"big\" size=72 arc=#ffaa00 pointer=dot\n"                                  // 1
                                   "defaults knob look=ring ticks=11\n"                                                // 2
                                   "defaults section look=card radius=8\n"                                             // 3
                                   "panel background=#202020 gradient=#101010 {\n"                                     // 4
                                   "  tabs look=bar {\n"                                                               // 5
                                   "    page \"A\" {\n"                                                                // 6
                                   "      section \"S\" look=plate titlealign=left titlesize=large shadow=false {\n"   // 7
                                   "        row wrap gap=6 margin=4 {\n"                                               // 8
                                   "          knob cutoff class=\"big\" look=soft live=env\n"                          // 9
                                   "          knob resonance bipolar labelpos=bottom value=never sweep=300 thickness=4\n" // 10
                                   "          slider attack track=none cap=#ffffff live=env\n"                         // 11
                                   "        }\n"                                                                       // 12
                                   "      }\n"                                                                         // 13
                                   "    }\n"                                                                           // 14
                                   "    page \"B\" { box look=inset fill=none border=red borderwidth=2 { knob level self=end minwidth=80 } }\n"
                                   "  }\n"                                                                             // 16
                                   "  label \"BIG\" size=24 align=right\n"                                             // 17
                                   "  divider thickness=3\n"                                                           // 18
                                   + keys +                                                                            // 19
                                   "}\n");
        int lineNotes = 0;
        for (std::size_t at = c.notes.find("panel.gui:"); at != std::string::npos; at = c.notes.find("panel.gui:", at + 1))
            lineNotes += at + 10 < c.notes.size() && std::isdigit(static_cast<unsigned char>(c.notes[at + 10])) ? 1 : 0;
        if (lineNotes > 0)
            std::printf("styles notes:\n%s", c.notes.c_str());
        P.eq("styles.no_line_notes", lineNotes, 0);
        const pg::GuiTree& t = c.panel.gui;
        P.eq("styles.gradient", t.gradient == "#101010" && t.background == "#202020" ? 1 : 0, 1);
        const pg::GuiNode& tabs = at(t, find(t, pg::GuiKind::tabs));
        P.eq("styles.tabs_bar", tabs.style.look == "bar" ? 1 : 0, 1);
        const pg::GuiNode& sec = at(t, find(t, pg::GuiKind::section, "S"));
        P.eq("styles.section_own_look", sec.style.look == "plate" ? 1 : 0, 1);              // its own beats the defaults'
        P.eq("styles.section_defaults_radius", sec.style.radius == 8.0f ? 1 : 0, 1);         // the defaults' still apply
        P.eq("styles.section_title", sec.style.titleAlign == 0 && sec.style.titleSize == 2 && sec.style.shadow == 0 ? 1 : 0, 1);
        const pg::GuiNode& row = at(t, find(t, pg::GuiKind::row));
        P.eq("styles.row_wrap", row.wrap && row.gap == 6.0f && row.margin == 4.0f ? 1 : 0, 1);
        const pg::GuiNode& big = at(t, find(t, pg::GuiKind::knob, "", cutoff));
        P.eq("styles.class", big.style.px == 72.0f && big.style.arc == "#ffaa00" && big.style.pointer == 1 ? 1 : 0, 1);
        P.eq("styles.own_beats_class", big.style.look == "soft" ? 1 : 0, 1);
        P.eq("styles.defaults_ticks", big.style.tickCount == 11 ? 1 : 0, 1);
        P.eq("styles.live", big.live >= 0 && big.live < static_cast<int>(c.panel.displays.size())
                                && c.panel.displays[static_cast<std::size_t>(big.live)].hidden
                                && c.panel.displays[static_cast<std::size_t>(big.live)].source == "env" ? 1 : 0, 1);
        const pg::GuiNode& slider = at(t, find(t, pg::GuiKind::slider, "", attack));
        P.eq("styles.live_shared_tap", slider.live == big.live ? 1 : 0, 1);                   // one tap for one field
        P.eq("styles.slider_colours", slider.style.track == "none" && slider.style.cap == "#ffffff" ? 1 : 0, 1);
        const pg::GuiNode& reso = at(t, find(t, pg::GuiKind::knob, "", resonance));
        P.eq("styles.knob_props", reso.style.bipolar && reso.style.labelPos == 1 && reso.style.valueShow == 2
                                      && reso.style.sweep == 300.0f && reso.style.thickness == 4.0f && reso.style.look == "ring" ? 1 : 0, 1);
        const pg::GuiNode& box = at(t, find(t, pg::GuiKind::box));
        P.eq("styles.box", box.style.look == "inset" && box.style.fill == "none" && box.style.borderWidth == 2.0f
                               && !box.style.border.empty() ? 1 : 0, 1);
        const pg::GuiNode& lvl = at(t, find(t, pg::GuiKind::knob, "", level));
        P.eq("styles.self_min", lvl.self == 3 && lvl.minW == 80.0f ? 1 : 0, 1);
        const pg::GuiNode& label = at(t, find(t, pg::GuiKind::label, "BIG"));
        P.eq("styles.label_px", label.textPx == 24.0f && label.textAlign == 2 ? 1 : 0, 1);
        P.eq("styles.divider", at(t, find(t, pg::GuiKind::divider)).style.thickness == 3.0f ? 1 : 0, 1);
        if constexpr (kInstrument)
        {
            const pg::GuiNode& kb = at(t, find(t, pg::GuiKind::keyboard));
            P.eq("styles.keyboard", kb.from == 36 && kb.keys == 37 && kb.height == 80.0f && kb.wheels ? 1 : 0, 1);
        }

        // Mistakes: an unknown class, a look a knob has not, a keyboard on an effect, defaults of no element, a class
        // without a name; the rest stands.
        const Compiled m = withGui("defaults widget look=ring\n"            // 1
                                   "class size=10\n"                        // 2
                                   "knob cutoff class=\"nope\"\n"           // 3
                                   "knob resonance look=shiny\n"            // 4
                                   "tabs look=folder { page \"P\" { knob level } }\n"   // 5
                                   "keyboard\n");                           // 6
        P.eq("styles.note_defaults_word", has(m.notes, "panel.gui:1:10: note: defaults takes an element's name") ? 1 : 0, 1);
        P.eq("styles.note_class_name", has(m.notes, "panel.gui:2:1: note: class takes a \"name\"") ? 1 : 0, 1);
        P.eq("styles.note_no_class", has(m.notes, "panel.gui:3:13: note: there is no class \"nope\"") ? 1 : 0, 1);
        P.eq("styles.note_look", has(m.notes, "panel.gui:4:") && has(m.notes, "shiny") ? 1 : 0, 1);
        P.eq("styles.note_tabs_look", has(m.notes, "panel.gui:5:") && has(m.notes, "folder") ? 1 : 0, 1);
        P.eq("styles.note_keyboard", has(m.notes, "panel.gui:6:1: note: an effect plays no notes") ? (kInstrument ? 0 : 1) : (kInstrument ? 1 : 0), 1);
        P.eq("styles.mistakes_rest_stands", find(m.panel.gui, pg::GuiKind::knob, "", cutoff) >= 0
                                                && find(m.panel.gui, pg::GuiKind::knob, "", resonance) >= 0 ? 1 : 0, 1);
    }

    // ---- SHARED --------------------------------------------------------------------------------------------------------
    {
        const std::string shared = "module Shared\n{\n    float level;\n    int notes;\n    bool on;\n    float table[8];\n};\n\n";
        const std::string main = program();
        const std::size_t open = main.find("{\n") + 2;
        const std::string withShared = shared + main.substr(0, open) + "    Shared* shared;\n    float speed;\n" + main.substr(open);
        const std::string gui = "row {\n"                                                 // line 1
                                "    number shared->level\n"                              // 2: tap 0
                                "    meter shared->notes min=0 max=16\n"                  // 3: tap 1, an int
                                "    led shared->on\n"                                    // 4: tap 2, a bool
                                "    number speed unit=\"rpm\" decimals=0 scale=60\n"   // 5: tap 3
                                "    number count\n"                                      // 6: tap 4, an int
                                "    number shared->nope\n"                               // 7: a note
                                "    number shared->table\n"                              // 8: a note
                                "    number host->bpm\n"                                  // 9: a note
                                "    number out unit=\"dB\"\n"                          // 10: a note, then dBFS
                                "    knob cutoff live=shared->level\n"                    // 11
                                "}\n";
        const Compiled c = compile({ { pg::kProgramName, withShared }, { "panel.gui", gui } });
        P.eq("shared.compiles", c.ok ? 1 : 0, 1);
        const std::vector<pg::PanelDisplay>& d = c.panel.displays;
        std::printf("shared: %zu displays; notes:\n%s", d.size(), c.notes.c_str());
        P.eq("shared.displays", static_cast<int64_t>(d.size()), 7);   // six drawn, the live= tap hidden
        if (d.size() == 7)
        {
            P.eq("shared.sources", d[0].source == "shared->level" && d[1].source == "shared->notes" && d[2].source == "shared->on"
                                       && d[3].source == "speed" && d[4].source == "count" && d[5].source == "out" ? 1 : 0, 1);
            P.eq("shared.taps", d[0].tap == 0 && d[1].tap == 1 && d[2].tap == 2 && d[3].tap == 3 && d[4].tap == 4 && d[5].tap == -1
                                    && d[6].tap == 5 ? 1 : 0, 1);
            P.eq("shared.whole", !d[0].integer && d[1].integer && d[2].integer && !d[3].integer && d[4].integer ? 1 : 0, 1);
            P.eq("shared.number_format", d[3].unit == "rpm" && d[3].decimals == 0 && d[3].scale == 60.0f && d[3].formatted()
                                             && !d[0].formatted() ? 1 : 0, 1);
            P.eq("shared.audio_unformatted", !d[5].formatted() ? 1 : 0, 1);
            P.eq("shared.live", d[6].hidden && d[6].source == "shared->level" ? 1 : 0, 1);
            P.eq("shared.label_its_own_name", d[0].label == "level" && d[1].label == "notes" && d[3].label == "speed" ? 1 : 0, 1);
        }
        P.eq("shared.note_nope", has(c.notes, "panel.gui:7:12: note: there is no field shared->nope (") ? 1 : 0, 1);
        P.eq("shared.note_array", has(c.notes, "panel.gui:8:12: note: shared->table is a float[8]") ? 1 : 0, 1);
        P.eq("shared.note_other_pointer", has(c.notes, "panel.gui:9:12: note: there is no field host->bpm (") ? 1 : 0, 1);
        P.eq("shared.note_audio_format", has(c.notes, "panel.gui:10:16: note: number out shows dBFS") ? 1 : 0, 1);
        P.eq("shared.no_note_on_the_rest", !has(c.notes, "panel.gui:2:") && !has(c.notes, "panel.gui:3:") && !has(c.notes, "panel.gui:4:")
                                               && !has(c.notes, "panel.gui:5:") && !has(c.notes, "panel.gui:6:")
                                               && !has(c.notes, "panel.gui:11:") ? 1 : 0, 1);
        // No Shared: its fields are none.
        const Compiled none = withGui("number shared->level\nknob cutoff\n");
        P.eq("shared.none_without_shared", none.panel.displays.empty() && has(none.notes, "there is no field shared->level") ? 1 : 0, 1);
        // The #pragma panel lines name one too.
        const Compiled pragma = compile({ { pg::kProgramName, "#pragma panel number shared->notes \"N\"\n#pragma panel meter shared->level\n"
                                                              "#pragma panel led shared->on as lit\n" + withShared } });
        const std::vector<pg::PanelDisplay>& pd = pragma.panel.displays;
        P.eq("shared.pragma", pd.size() == 3 && pd[0].source == "shared->notes" && pd[0].integer && pd[0].tap == 0 ? 1 : 0, 1);
        // Labelled as said, else by Shared's field's own name (not "SHARED->LEVEL"), else by `as`'s name.
        P.eq("shared.pragma_labels", pd.size() == 3 && pd[0].label == "N" && pd[1].label == "level" && pd[1].name == "shared->level"
                                         && pd[2].label.empty() && pd[2].name == "lit" ? 1 : 0, 1);
    }

    // ---- ELEMENTS: a double, an array's element, a field's attributes as a display's defaults ---------------------------
    {
        const std::string body = "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                 "    double temp [[ui::label(\"TEMP\")]] [[fminus::range(80, 100)]];\n"
                                 "    float bands [[ui::unit(\"dB\")]][4];\n"
                                 "    float gr [[ui::unit(\"dB\")]] [[ui::decimals(2)]];\n";
        const std::string program = kInstrument
            ? "module Voice\n{\n" + body + "    void start(float n, float v) {}\n"
              "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
            : "module Effect\n{\n" + body + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const Compiled c = compile({ { pg::kProgramName, program },
                                     { "panel.gui", "meter temp\nnumber bands[1]\nnumber gr unit=\"\" decimals=0\n"
                                                    "number gr label=\"MINE\"\nknob cutoff live=bands[2]\n" } });
        std::printf("elements: %s", c.notes.c_str());
        const std::vector<pg::PanelDisplay>& d = c.panel.displays;
        P.eq("elements.compiles", c.ok ? 1 : 0, 1);
        P.eq("elements.count", static_cast<int64_t>(d.size()), 5);
        if (d.size() == 5)
        {
            P.eq("elements.double_look", d[0].source == "temp" && d[0].label == "TEMP" && d[0].lo == 80.0f && d[0].hi == 100.0f
                                             && !d[0].integer && d[0].tap == 0 ? 1 : 0, 1);
            P.eq("elements.element_unit", d[1].source == "bands[1]" && d[1].unit == "dB" && d[1].tap == 1 ? 1 : 0, 1);
            P.eq("elements.own_beats_look", d[2].unit.empty() && d[2].decimals == 0 ? 1 : 0, 1);
            P.eq("elements.number_look", d[3].label == "MINE" && d[3].unit == "dB" && d[3].decimals == 2 ? 1 : 0, 1);
            P.eq("elements.live_element", d[4].hidden && d[4].source == "bands[2]" ? 1 : 0, 1);
        }
        P.eq("elements.no_note", !has(c.notes, "panel.gui:") ? 1 : 0, 1);

        // A field's label and unit too long for a display: cut where a character starts (the last two-byte one left
        // out whole), with a note on the display's line.
        const std::string longLabel = "A\xc3\x89\xc3\x89\xc3\x89\xc3\x89\xc3\x89\xc3\x89\xc3\x89";   // 15 bytes
        const std::string longBody = "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                     "    float bend [[ui::label(\"" + longLabel + "\")]] [[ui::unit(\"semitones\")]];\n";
        const std::string longProgram = kInstrument
            ? "module Voice\n{\n" + longBody + "    void start(float n, float v) {}\n"
              "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
            : "module Effect\n{\n" + longBody + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const Compiled cut = compile({ { pg::kProgramName, longProgram }, { "panel.gui", "knob cutoff\nnumber bend\n" } });
        std::printf("elements cut: %s", cut.notes.c_str());
        P.eq("elements.long_look_cut", cut.panel.displays.size() == 1 && cut.panel.displays[0].label == longLabel.substr(0, 13)
                                           && cut.panel.displays[0].unit == "semiton" ? 1 : 0, 1);
        P.eq("elements.long_look_notes", has(cut.notes, "panel.gui:2:") && has(cut.notes, "(bend's ui::label) is cut to 14 characters")
                                             && has(cut.notes, "the unit \"semitones\" (bend's ui::unit) is cut to 7 characters") ? 1 : 0, 1);
        // An option's name too long for its button: cut where a character starts, as a label is.
        const std::string longOption = std::string(9, 'O') + "\xc3\x89";                   // 11 bytes
        const std::string optBody = "    param int mode [[fminus::range(0, 1)]] [[fminus::default(0)]];\n";
        const std::string optProgram = kInstrument
            ? "module Voice\n{\n" + optBody + "    void start(float n, float v) {}\n"
              "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
            : "module Effect\n{\n" + optBody + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const Compiled opt = compile({ { pg::kProgramName, optProgram }, { "panel.gui", "buttons mode \"" + longOption + "\" \"B\"\n" } });
        std::printf("elements option cut: %s", opt.notes.c_str());
        P.eq("elements.long_option_cut", opt.panel.options[0] == std::vector<std::string>{ std::string(9, 'O'), "B" }
                                             && has(opt.notes, "is cut to 10 characters") ? 1 : 0, 1);

        // The Patch's taps through a .gui file (Patch::readPanel): one a display, in its order, each reading what the
        // program wrote where FMinus put it (a double rounded to a float, an array's element, a 2-D array's by live=);
        // and again after a panel's edit (a RUN of the .gui alone: readPanel over the compile's recipe, the Engine's
        // taps then, Engine::setTaps) that moves the displays about and names other elements.
        const std::string tapFields = "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                      "    double temp;\n    float bands[4];\n    float grid[2][3];\n    float gr;\n";
        const std::string tapWrites = "        temp = 90.5;\n        for (int k = 0; k < 4; ++k)\n            bands[k] = k + 0.5f;\n"
                                      "        for (int r = 0; r < 2; ++r)\n            for (int c = 0; c < 3; ++c)\n"
                                      "                grid[r][c] = 10 * r + c;\n        gr = 6;\n";
        const std::string tapProgram = kInstrument
            ? "module Voice\n{\n" + tapFields + "    void start(float n, float v) {}\n"
              "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + tapWrites + "        return true;\n    }\n};\n"
            : "module Effect\n{\n" + tapFields + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
              + tapWrites + "    }\n};\n";
        const pg::Sources tapFiles = { { pg::kProgramName, tapProgram },
                                       { "panel.gui", "meter temp\nnumber bands[1]\nnumber gr\nknob cutoff live=grid[1][2]\n" } };
        pg::Patch::Result tr = pg::Patch::compile(tapFiles, 48000.0f);
        P.eq("elements.taps_compile", tr.patch != nullptr ? 1 : 0, 1);
        if (!tr.errors.empty())
            std::printf("errors:\n%s", tr.errors.c_str());
        if (tr.patch != nullptr)
        {
            std::vector<float> l(16, 0.0f), rr(16, 0.0f);
            if constexpr (kInstrument)
            {
                tr.patch->start(0, 60.0f, 0.8f);
                tr.patch->render(0, l.data(), rr.data(), 16);
            }
            else
                tr.patch->process(l.data(), rr.data(), 16);
            const auto tapOf = [&](const std::string& source) {
                for (const pg::PanelDisplay& x : tr.patch->panel().displays)
                    if (x.source == source && x.tap >= 0)
                        return static_cast<double>(tr.patch->tap(x.tap, 0));
                return -1.0;
            };
            std::printf("elements: %d taps: temp %g, bands[1] %g, gr %g, grid[1][2] %g\n", tr.patch->taps(), tapOf("temp"),
                        tapOf("bands[1]"), tapOf("gr"), tapOf("grid[1][2]"));
            P.eq("elements.patch_taps", tr.patch->taps(), 4);
            P.near("elements.tap_double", tapOf("temp"), 90.5, 0.0);
            P.near("elements.tap_element", tapOf("bands[1]"), 1.5, 0.0);
            P.near("elements.tap_field", tapOf("gr"), 6.0, 0.0);
            P.near("elements.tap_live_element_2d", tapOf("grid[1][2]"), 12.0, 0.0);

            const pg::Patch::Recipe recipe = tr.patch->recipe();
            pg::Sources edited = tapFiles;
            edited[1].text = "knob cutoff\nnumber gr\nnumber grid[0][2]\nmeter temp\nnumber bands[3]\n";
            pg::Controls controls{};
            std::vector<pg::Patch::Tap> taps;
            std::string notes;
            const pg::PanelSpec spec = pg::Patch::readPanel(recipe.panel, recipe.controls, edited, controls, taps, notes);
            const auto editedTap = [&](const std::string& source) {
                for (const pg::PanelDisplay& x : spec.displays)
                    if (x.source == source && x.tap >= 0 && x.tap < static_cast<int>(taps.size()))
                        return static_cast<double>(tr.patch->tapOf(taps[static_cast<std::size_t>(x.tap)], 0));
                return -1.0;
            };
            std::printf("elements edited: %zu taps: gr %g, grid[0][2] %g, temp %g, bands[3] %g%s%s\n", taps.size(), editedTap("gr"),
                        editedTap("grid[0][2]"), editedTap("temp"), editedTap("bands[3]"), notes.empty() ? "" : "; notes: ", notes.c_str());
            P.eq("elements.edit_taps", static_cast<int64_t>(taps.size()), 4);
            P.near("elements.edit_tap_field", editedTap("gr"), 6.0, 0.0);
            P.near("elements.edit_tap_element_2d", editedTap("grid[0][2]"), 2.0, 0.0);
            P.near("elements.edit_tap_double", editedTap("temp"), 90.5, 0.0);
            P.near("elements.edit_tap_element", editedTap("bands[3]"), 3.5, 0.0);
        }
    }

    // ---- LIMITS --------------------------------------------------------------------------------------------------------
    {
        // 65 parameters p1 to p65 and 33 fields f1 to f33.
        std::string params, fields;
        for (int k = 1; k <= pg::kMaxControls + 1; ++k)
            params += "    param float p" + std::to_string(k) + " [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        for (int k = 1; k <= pg::kMaxDisplays + 1; ++k)
            fields += "    float f" + std::to_string(k) + ";\n";
        const std::string body = params + fields;
        const std::string source = kInstrument
            ? "module Voice\n{\n" + body + "    void start(float n, float v) {}\n"
                  "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
            : "module Effect\n{\n" + body + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        std::string gui = "knob p33\nknob p64\nknob p65\n";
        for (int k = 1; k <= pg::kMaxDisplays + 1; ++k)
            gui += "number f" + std::to_string(k) + "\n";
        std::string inputs;
        for (int k = 1; k <= pg::kMaxShaderInputs + 1; ++k)
            inputs += (k == 1 ? "" : " ") + std::string("p") + std::to_string(k);
        gui += "shader \"many.sc\" inputs=\"" + inputs + "\"\n";
        const Compiled c = compile({ { pg::kProgramName, source }, { "panel.gui", gui }, { "many.sc", "void main() { gl_FragColor = vec4(in15); }\n" } });
        std::printf("LIMITS notes:\n%s", c.notes.c_str());
        P.eq("limits.compiles", c.ok ? 1 : 0, 1);
        const pg::GuiTree& t = c.panel.gui;
        const int first = pg::kHostControls, last = pg::kMaxControls - 1;
        P.eq("limits.panel_dial_knobs", find(t, pg::GuiKind::knob, "", first) >= 0 && find(t, pg::GuiKind::knob, "", last) >= 0
                                            && c.controls[static_cast<std::size_t>(first)].used && c.controls[static_cast<std::size_t>(last)].used
                                            && std::string(c.controls[static_cast<std::size_t>(last)].name) == "p64" ? 1 : 0, 1);
        P.eq("limits.past_dials_noted", has(c.notes, "p65 has no dial") && has(c.notes, "panel.gui:3:6: note:") ? 1 : 0, 1);
        bool inOrder = static_cast<int>(c.panel.displays.size()) == pg::kMaxDisplays;
        for (int k = 0; inOrder && k < pg::kMaxDisplays; ++k)
            inOrder = c.panel.displays[static_cast<std::size_t>(k)].source == "f" + std::to_string(k + 1)
                   && c.panel.displays[static_cast<std::size_t>(k)].tap == k;
        P.eq("limits.displays_most", inOrder ? 1 : 0, 1);
        P.eq("limits.display_past_noted", has(c.notes, "a panel shows " + std::to_string(pg::kMaxDisplays) + " displays at most")
                                              && has(c.notes, "panel.gui:" + std::to_string(4 + pg::kMaxDisplays) + ":") ? 1 : 0, 1);
        const int sh = find(t, pg::GuiKind::shader);
        bool sixteen = sh >= 0 && c.panel.shaders.size() == 1 && static_cast<int>(c.panel.shaders[0].inputs.size()) == pg::kMaxShaderInputs;
        for (int k = 0; sixteen && k < pg::kMaxShaderInputs; ++k)
            sixteen = t.nodes[static_cast<std::size_t>(sh)].inputs[static_cast<std::size_t>(k)] == "param:" + std::to_string(k)
                   && c.panel.shaders[0].inputs[static_cast<std::size_t>(k)] == "p" + std::to_string(k + 1);
        P.eq("limits.shader_inputs_most", sixteen ? 1 : 0, 1);
        P.eq("limits.shader_input_past_noted", has(c.notes, "a shader takes " + std::to_string(pg::kMaxShaderInputs) + " inputs at most: p"
                                                                + std::to_string(pg::kMaxShaderInputs + 1) + " is left out") ? 1 : 0, 1);
    }

    // ---- POINTER -------------------------------------------------------------------------------------------------------
    {
        // px, py (floats), gate (a bool), steps (an int), then floats to the 32nd; lit (a bool) and glow (a float) after
        // it, the panel's own; a field env.
        std::string params = "    param float px [[fminus::range(0, 1)]] [[fminus::default(0.25)]];\n"
                             "    param float py [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                             "    param bool gate [[fminus::default(0)]];\n"
                             "    param int steps [[fminus::range(0, 7)]] [[fminus::default(0)]];\n";
        for (int k = 5; k <= pg::kHostControls; ++k)
            params += "    param float f" + std::to_string(k) + " [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        params += "    param bool lit [[fminus::default(0)]];\n"
                  "    param float glow [[fminus::range(0, 1)]] [[fminus::default(0)]];\n"
                  "    float env;\n";
        const std::string source = kInstrument
            ? "module Voice\n{\n" + params + "    void start(float n, float v) {}\n"
                  "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
            : "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const int px = 0, py = 1, gate = 2, steps = 3, lit = pg::kHostControls, glow = pg::kHostControls + 1;
        const std::string shaderCode = "void main() { gl_FragColor = vec4(in_pointer_x, in_pointer_y, in_pointer_down, in_pointer_hover); }\n";
        const auto build = [&](const std::string& gui) {
            return compile({ { pg::kProgramName, source }, { "panel.gui", gui }, { "d.sc", shaderCode } });
        };
        const std::string gui = "area x=px y=py down=gate wheel=steps hover=lit drag=relative release=spring cursor=cross width=200 height=100\n"
                                "box down=gate {\n"
                                "    knob px\n"
                                "}\n"
                                "label \"HI\" hover=glow\n"
                                "meter env x=px\n"
                                "shader \"d.sc\" inputs=\"px pointer.x pointer.y pointer.down pointer.hover\" y=py cursor=updown\n";
        const Compiled c = build(gui);
        std::printf("POINTER notes:\n%s", c.notes.c_str());
        P.eq("pointer.compiles", c.ok ? 1 : 0, 1);
        P.eq("pointer.no_line_notes", has(c.notes, "panel.gui:1:") || has(c.notes, "panel.gui:2:") || has(c.notes, "panel.gui:5:")
                                              || has(c.notes, "panel.gui:6:") || has(c.notes, "panel.gui:7:") ? 0 : 1, 1);
        const pg::GuiTree& t = c.panel.gui;
        const pg::GuiNode& area = at(t, find(t, pg::GuiKind::area));
        P.eq("pointer.area", area.kind == pg::GuiKind::area ? 1 : 0, 1);
        P.eq("pointer.area_slots", area.pointer.x == px && area.pointer.y == py && area.pointer.down == gate
                                       && area.pointer.wheel == steps && area.pointer.hover == lit ? 1 : 0, 1);
        P.eq("pointer.area_shape", area.pointer.drag == pg::PanelPointer::Drag::relative
                                       && area.pointer.release == pg::PanelPointer::Release::spring
                                       && area.pointer.cursor == pg::PanelPointer::Cursor::cross ? 1 : 0, 1);
        P.eq("pointer.area_size", area.width == 200.0f && area.height == 100.0f ? 1 : 0, 1);
        const pg::GuiNode& box = at(t, find(t, pg::GuiKind::box));
        P.eq("pointer.box", box.pointer.down == gate && box.pointer.x < 0 && box.pointer.drag == pg::PanelPointer::Drag::absolute ? 1 : 0, 1);
        P.eq("pointer.knob_too", find(t, pg::GuiKind::knob, "", px) >= 0 ? 1 : 0, 1);   // a param a pointer writes takes a control too
        P.eq("pointer.label_hover", at(t, find(t, pg::GuiKind::label, "HI")).pointer.hover == glow ? 1 : 0, 1);
        P.eq("pointer.display", at(t, find(t, pg::GuiKind::display)).pointer.x == px ? 1 : 0, 1);
        const int sh = find(t, pg::GuiKind::shader);
        const pg::GuiNode& shader = at(t, sh);
        P.eq("pointer.shader_inputs", shader.inputs[0] == "param:" + std::to_string(px) && shader.inputs[1] == "pointer.x"
                                          && shader.inputs[2] == "pointer.y" && shader.inputs[3] == "pointer.down"
                                          && shader.inputs[4] == "pointer.hover" && shader.inputs[5].empty() ? 1 : 0, 1);
        P.eq("pointer.shader_names", c.panel.shaders.size() == 1 && c.panel.shaders[0].inputs.size() == 5
                                         && c.panel.shaders[0].inputs[1] == "pointer.x" && c.panel.shaders[0].inputs[4] == "pointer.hover" ? 1 : 0, 1);
        P.eq("pointer.shader_props", shader.pointer.y == py && shader.pointer.cursor == pg::PanelPointer::Cursor::updown ? 1 : 0, 1);
        P.eq("pointer.words", pg::pointerInput("pointer.x") == 0 && pg::pointerInput("pointer.hover") == 3
                                  && pg::pointerInput("pointer.z") < 0 ? 1 : 0, 1);
        // Read before the program compiles (readGuiShaders): the pointer's words as they are.
        const std::vector<pg::PanelShader> early = pg::readGuiShaders({ "panel.gui", gui }, kInstrument);
        P.eq("pointer.early_shaders", early.size() == 1 && early[0].inputs.size() == 5 && early[0].inputs[2] == "pointer.y" ? 1 : 0, 1);
        // A parameter only a pointer writes comes in no section of the rest (gate, steps, lit, glow, py); f5 does.
        P.eq("pointer.rest", has(c.notes, "f5") && !has(c.notes, "gate") && !has(c.notes, "steps") && !has(c.notes, "lit,")
                                 && !has(c.notes, "glow") ? 1 : 0, 1);

        // The notes, each on its line, the property left out.
        const std::string bad = "area x=steps\n"                       // 1 an int
                                "area x=nope\n"                        // 2 none
                                "area hover=gate\n"                    // 3 a host parameter
                                "knob px x=py\n"                       // 4 a control that takes presses
                                "area wheel=gate\n"                    // 5 a bool
                                "area drag=relative down=gate\n"       // 6 drag= without x= or y=
                                "area\n"                               // 7 nothing to write
                                "label \"A\" cursor=hand\n"            // 8 a cursor alone
                                "area x=px drag=sideways\n"            // 9 a word drag= does not take
                                "area down=\"gate\"\n";                // 10 a text, not a name
        const Compiled n = build(bad);
        std::printf("POINTER bad notes:\n%s", n.notes.c_str());
        const auto noted = [&](int line, const std::string& words) {
            return has(n.notes, "panel.gui:" + std::to_string(line) + ":") && has(n.notes, words);
        };
        P.eq("pointer.note_int", noted(1, "x= moves a float: steps is an int") ? 1 : 0, 1);
        P.eq("pointer.note_none", noted(2, "there is no parameter nope") ? 1 : 0, 1);
        P.eq("pointer.note_hover_host", noted(3, "hover= needs a panel-only param: one after the program's 32nd") ? 1 : 0, 1);
        P.eq("pointer.note_control", noted(4, "knob takes presses itself") ? 1 : 0, 1);
        P.eq("pointer.note_wheel_bool", noted(5, "wheel= turns a float or an int: gate is a bool") ? 1 : 0, 1);
        P.eq("pointer.note_shaping", noted(6, "drag= and release= shape x= and y=") ? 1 : 0, 1);
        P.eq("pointer.note_empty", noted(7, "area writes what the pointer does there") ? 1 : 0, 1);
        P.eq("pointer.note_cursor", noted(8, "cursor= is the pointer's shape") ? 1 : 0, 1);
        P.eq("pointer.note_word", noted(9, "drag= takes absolute or relative") ? 1 : 0, 1);
        P.eq("pointer.note_text", noted(10, "down= takes a parameter's name") ? 1 : 0, 1);
        int areas = 0, refused = 0;
        for (const pg::GuiNode& x : n.panel.gui.nodes)
            if (x.kind == pg::GuiKind::area)
            {
                ++areas;
                refused += x.pointer.any() ? 0 : 1;
            }
        P.eq("pointer.refused_left_out", areas == 8 && refused == 8 - 2 ? 1 : 0, 1);   // all but 6's down= and 9's x=
        P.eq("pointer.control_unchanged", at(n.panel.gui, find(n.panel.gui, pg::GuiKind::knob, "", px)).pointer.any() ? 0 : 1, 1);

        // The 17th element with pointer properties is a note, left out; the 16 before it kept.
        std::string many;
        for (int k = 0; k <= pg::kMaxPointers; ++k)
            many += "area down=gate\n";
        const Compiled m = build(many);
        int with = 0;
        for (const pg::GuiNode& x : m.panel.gui.nodes)
            with += x.pointer.any() ? 1 : 0;
        P.eq("pointer.most", with, pg::kMaxPointers);
        P.eq("pointer.past_noted", has(m.notes, "panel.gui:" + std::to_string(pg::kMaxPointers + 1) + ":")
                                       && has(m.notes, "a panel has " + std::to_string(pg::kMaxPointers) + " elements with pointer properties at most") ? 1 : 0, 1);

        // The pointer's words count among a shader's 16 inputs: the 17th name is a note.
        std::string inputs = "pointer.x pointer.y pointer.down pointer.hover";
        for (int k = 5; k <= pg::kMaxShaderInputs + 1; ++k)
            inputs += " f" + std::to_string(k);
        const Compiled s = build("shader \"d.sc\" inputs=\"" + inputs + "\"\n");
        const pg::GuiNode& full = at(s.panel.gui, find(s.panel.gui, pg::GuiKind::shader));
        P.eq("pointer.inputs_cap", full.inputs[0] == "pointer.x" && full.inputs[3] == "pointer.hover"
                                       && full.inputs[15] == "param:" + std::to_string(pg::kMaxShaderInputs - 1) ? 1 : 0, 1);
        P.eq("pointer.inputs_past_noted", has(s.notes, "inputs at most: f" + std::to_string(pg::kMaxShaderInputs + 1) + " is left out") ? 1 : 0, 1);
    }

    // ---- an instrument has no input ------------------------------------------------------------------------------------
    {
        const Compiled c = withGui("meter in\nknob cutoff\n");
        P.eq("in.instrument_note", has(c.notes, "an instrument has no input") ? (kInstrument ? 1 : 0) : (kInstrument ? 0 : 1), 1);
    }
    return P.finish();
}
