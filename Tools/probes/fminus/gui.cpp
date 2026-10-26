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
//   REST      a parameter no control names comes last, in a section without a title, with a note naming it
//   NOTES     what the plugin cannot use is a note on its line and column, and the rest stands: an unknown element,
//             property or value, a parameter that is none or has a control already, a switch, buttons or a menu on a
//             parameter it does not suit, a pad on an int, an envelope without four parameters, a { without its },
//             a } that closes nothing, a "text" without its ", a colour that is none, a number with a unit, tabs
//             holding no page, a page outside tabs, a label without text, a log taper from 0, decimals out of range;
//             a second .gui file, and #pragma panel lines beside a .gui file, are notes too
//   PRAGMAS   without a .gui file the #pragma panel lines still make the panel (no tree)
//   STYLES    classes and defaults reach their elements (an element's own property beats its class's, which beats
//             the defaults'); every look and style property lands in its node (tabs, sections, boxes, rows that wrap,
//             knobs, sliders, labels, dividers, the panel's gradient, an instrument's keyboard); live= taps its field
//             once for every control that names it; a mistake in them is a note on its line and column
#include "Probe.h"

#include "fminus/GuiSpec.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;

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
        tp::PanelSpec panel;
        tp::Controls controls{};
        std::string notes;
    };

    Compiled compile(const tp::Sources& files)
    {
        tp::Patch::Result r = tp::Patch::compile(files, 48000.0f);
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
        return compile({ { tp::kProgramName, program(before) }, { "panel.gui", gui } });
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
    int find(const tp::GuiTree& t, tp::GuiKind kind, const std::string& text = "", int slot = -1)
    {
        for (std::size_t i = 0; i < t.nodes.size(); ++i)
        {
            const tp::GuiNode& n = t.nodes[i];
            if (n.kind == kind && (text.empty() || n.text == text) && (slot < 0 || n.slot == slot))
                return static_cast<int>(i);
        }
        return -1;
    }

    const tp::GuiNode& at(const tp::GuiTree& t, int i)
    {
        static const tp::GuiNode none{};
        return i >= 0 && static_cast<std::size_t>(i) < t.nodes.size() ? t.nodes[static_cast<std::size_t>(i)] : none;
    }

    std::string text(const tp::Control& c, float plain)
    {
        char buf[32];
        tp::formatValue(c, plain, buf, sizeof buf);
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
        const tp::GuiTree& t = c.panel.gui;
        P.eq("tree.file", t.file == "panel.gui" ? 1 : 0, 1);
        P.eq("tree.root_column", !t.nodes.empty() && t.nodes[0].kind == tp::GuiKind::column ? 1 : 0, 1);
        P.eq("tree.panel_colours", t.background == "#1d1b19" && t.accent == "orange" ? 1 : 0, 1);
        // The root's children, in order: the label, the row, the tabs, the spacer, the knob.
        std::vector<tp::GuiKind> top;
        for (const int ch : at(t, 0).children)
            top.push_back(at(t, ch).kind);
        P.eq("tree.top_order", top == std::vector<tp::GuiKind>{ tp::GuiKind::label, tp::GuiKind::row, tp::GuiKind::tabs,
                                                                tp::GuiKind::spacer, tp::GuiKind::knob } ? 1 : 0, 1);
        const tp::GuiNode& title = at(t, find(t, tp::GuiKind::label));
        P.eq("tree.label", title.text == "TEST SYNTH" && title.textSize == tp::GuiText::title && title.color == "cream" ? 1 : 0, 1);
        const tp::GuiNode& row = at(t, find(t, tp::GuiKind::row));
        P.eq("tree.row_props", row.gap == 12.0f && row.pad == 4.0f && row.align == tp::GuiAlign::center
                                   && row.justify == tp::GuiJustify::between && !row.vertical && row.children.size() == 4 ? 1 : 0, 1);
        const tp::GuiNode& filter = at(t, find(t, tp::GuiKind::section, "FILTER"));
        P.eq("tree.section", filter.color == "#e07b39" && filter.vertical && filter.children.size() == 2 ? 1 : 0, 1);
        const tp::GuiNode& env = at(t, find(t, tp::GuiKind::section, "ENVELOPE"));
        P.eq("tree.section_row", !env.vertical && env.children.size() == 5 ? 1 : 0, 1);
        const tp::GuiNode& big = at(t, find(t, tp::GuiKind::knob, "", cutoff));
        P.eq("tree.knob_large", big.size == tp::GuiSize::large && !big.vintage && big.parent == find(t, tp::GuiKind::section, "FILTER") ? 1 : 0, 1);
        const tp::GuiNode& res = at(t, find(t, tp::GuiKind::knob, "", resonance));
        P.eq("tree.knob_vintage_small", res.vintage && res.size == tp::GuiSize::small ? 1 : 0, 1);
        const tp::GuiNode& att = at(t, find(t, tp::GuiKind::slider, "", attack));
        P.eq("tree.slider_length", att.vertical && att.height == 120.0f ? 1 : 0, 1);
        const tp::GuiNode& adsr = at(t, find(t, tp::GuiKind::envelope));
        P.eq("tree.envelope", adsr.env == std::array<int, 4>{ attack, decay, sustain, release } && adsr.text == "AMP"
                                  && adsr.width == 160.0f && adsr.height == 90.0f ? 1 : 0, 1);
        P.eq("tree.divider", at(t, find(t, tp::GuiKind::divider)).color == "grey" ? 1 : 0, 1);
        const tp::GuiNode& box = at(t, find(t, tp::GuiKind::box));
        P.eq("tree.box", box.color == "teal" && !box.vertical && box.children.size() == 3 && box.pad == 8.0f ? 1 : 0, 1);
        const tp::GuiNode& buttons = at(t, find(t, tp::GuiKind::buttons, "", wave));
        P.eq("tree.buttons_row", !buttons.vertical ? 1 : 0, 1);
        P.eq("tree.options", c.panel.options[wave] == std::vector<std::string>{ "SAW", "SQUARE", "TRI" }
                                 && c.panel.options[syncSlot] == std::vector<std::string>{ "FREE", "SYNC" }
                                 && c.panel.options[mode] == std::vector<std::string>{ "POLY", "MONO" } ? 1 : 0, 1);
        P.eq("tree.menu", find(t, tp::GuiKind::menu, "", mode) >= 0 && at(t, find(t, tp::GuiKind::menu, "", mode)).width == 90.0f ? 1 : 0, 1);
        P.eq("tree.switch", find(t, tp::GuiKind::toggle, "", syncSlot) >= 0 ? 1 : 0, 1);
        const int tabs = find(t, tp::GuiKind::tabs);
        P.eq("tree.tabs", tabs >= 0 && at(t, tabs).children.size() == 2 && at(t, tabs).color == "blue"
                              && at(t, at(t, tabs).children[0]).text == "MOD" && at(t, at(t, tabs).children[1]).text == "METERS"
                              && !at(t, at(t, tabs).children[1]).vertical ? 1 : 0, 1);
        const tp::GuiNode& pad = at(t, find(t, tp::GuiKind::xy));
        P.eq("tree.xy", pad.slot == level && pad.slot2 == pan && pad.width == 140.0f && pad.text == "LEVEL / PAN" ? 1 : 0, 1);
        const tp::GuiNode& grid = at(t, find(t, tp::GuiKind::grid));
        P.eq("tree.grid", grid.cols == 3 && grid.gap == 6.0f && grid.children.size() == 6 ? 1 : 0, 1);
        P.eq("tree.labels", c.panel.labels[cutoff] == "CUTOFF" ? 1 : 0, 1);
        const tp::GuiNode& spacer = at(t, find(t, tp::GuiKind::spacer));
        P.eq("tree.spacer", spacer.height == 12.0f && spacer.grow == 0.0f ? 1 : 0, 1);
        P.eq("tree.grow", at(t, find(t, tp::GuiKind::knob, "", voices)).grow == 1.0f ? 1 : 0, 1);
        P.eq("tree.no_rest", find(t, tp::GuiKind::section, "") < 0 || at(t, find(t, tp::GuiKind::section, "")).text != "" ? 1 : 0, 1);

        // ---- DISPLAYS ----------------------------------------------------------------------------------------------
        const std::vector<tp::PanelDisplay>& d = c.panel.displays;
        P.eq("displays.count", static_cast<int64_t>(d.size()), 6);
        if (d.size() == 6)
        {
            P.eq("displays.kinds", d[0].kind == tp::DisplayKind::meter && d[1].kind == tp::DisplayKind::vu
                                       && d[2].kind == tp::DisplayKind::led && d[3].kind == tp::DisplayKind::number
                                       && d[4].kind == tp::DisplayKind::scope && d[5].kind == tp::DisplayKind::spectrum ? 1 : 0, 1);
            P.eq("displays.audio_no_tap", d[0].tap == -1 && d[1].tap == -1 && d[5].tap == -1 ? 1 : 0, 1);
            P.eq("displays.field_taps", d[2].tap == 0 && d[3].tap == 1 && d[4].tap == 2 ? 1 : 0, 1);
            P.eq("displays.two_of_out", d[0].name == "out" && d[1].name == "out2" && d[5].name == "out3" ? 1 : 0, 1);
            P.eq("displays.range", d[3].lo == 0.0f && d[3].hi == 16.0f ? 1 : 0, 1);
            P.eq("displays.label", d[0].label == "OUT" ? 1 : 0, 1);
            const tp::GuiNode& scope = at(t, find(t, tp::GuiKind::display) + 4);
            P.eq("displays.scope_size", scope.display == 4 && scope.width == 200.0f && scope.height == 70.0f ? 1 : 0, 1);
        }

        // ---- CONTROLS ----------------------------------------------------------------------------------------------
        const tp::Control& f = c.controls[cutoff];
        P.eq("controls.taper", f.taper == tp::ControlTaper::log ? 1 : 0, 1);
        P.near("controls.log_middle", static_cast<double>(tp::plainOf(f, 0.5f)), std::sqrt(20.0 * 20000.0), 0.5);
        P.near("controls.log_back", static_cast<double>(tp::normOf(f, 632.455f)), 0.5, 1e-4);
        P.near("controls.log_ends", static_cast<double>(tp::plainOf(f, 0.0f)) + static_cast<double>(tp::plainOf(f, 1.0f)), 20020.0, 0.1);
        P.eq("controls.hz", text(f, 440.0f) == "440 Hz" ? 1 : 0, 1);
        P.eq("controls.khz", text(f, 2500.0f) == "2.50 kHz" ? 1 : 0, 1);
        P.near("controls.parse_k", static_cast<double>(tp::parseValue(f, "2.5k")), 2500.0, 1e-3);
        const tp::Control& s = c.controls[sustain];
        P.eq("controls.percent", text(s, 0.6f) == "60 %" ? 1 : 0, 1);
        P.near("controls.parse_scaled", static_cast<double>(tp::parseValue(s, "60")), 0.6, 1e-5);
        P.eq("controls.decimals", text(c.controls[resonance], 0.25f) == "0.25" ? 1 : 0, 1);
        P.eq("controls.linear_others", c.controls[attack].taper == tp::ControlTaper::linear ? 1 : 0, 1);
    }

    // ---- LEX -----------------------------------------------------------------------------------------------------------
    {
        const std::string gui = "/* a block\n   comment */ row // a row\n{\n  knob cutoff; knob resonance, size=small\n"
                                "  label \"A \\\"QUOTE\\\"\" color=#e73\n}\nknob attack\n";
        const Compiled c = withGui(gui);
        const tp::GuiTree& t = c.panel.gui;
        const int row = find(t, tp::GuiKind::row);
        P.eq("lex.brace_next_line", row >= 0 && at(t, row).children.size() == 3 ? 1 : 0, 1);
        P.eq("lex.semicolon_comma", find(t, tp::GuiKind::knob, "", resonance) >= 0
                                        && at(t, find(t, tp::GuiKind::knob, "", resonance)).size == tp::GuiSize::small ? 1 : 0, 1);
        const tp::GuiNode& l = at(t, find(t, tp::GuiKind::label));
        P.eq("lex.escaped_quote", l.text == "A \"QUOTE\"" ? 1 : 0, 1);
        P.eq("lex.short_hex", l.color == "#ee7733" ? 1 : 0, 1);
        P.eq("lex.after_block", find(t, tp::GuiKind::knob, "", attack) >= 0 && at(t, find(t, tp::GuiKind::knob, "", attack)).parent == 0 ? 1 : 0, 1);
        // The rest (9 parameters) in a section without a title, with a note naming them: REST.
        const int rest = find(t, tp::GuiKind::section);
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
        P.eq("notes.page_children_kept", find(c.panel.gui, tp::GuiKind::knob, "", resonance) >= 0 ? 1 : 0, 1);
        P.eq("notes.colour", has(c.notes, "panel.gui:13:13: note: 'puce' is not a colour") ? 1 : 0, 1);
        P.eq("notes.unit", has(c.notes, "note: a number takes no unit after it: 'px'") ? 1 : 0, 1);
        P.eq("notes.unit_value_kept", at(c.panel.gui, find(c.panel.gui, tp::GuiKind::row)).gap == 12.0f ? 1 : 0, 1);
        P.eq("notes.log_from_zero", has(c.notes, "panel.gui:15:14: note: a log taper needs a range above 0") ? 1 : 0, 1);
        P.eq("notes.log_stays_linear", c.controls[sustain].taper == tp::ControlTaper::linear ? 1 : 0, 1);
        P.eq("notes.decimals_range", has(c.notes, "panel.gui:16:14: note: decimals=9 is outside 0 to 4: 4 instead") ? 1 : 0, 1);
        P.eq("notes.no_field", has(c.notes, "panel.gui:17:7: note: there is no field nofield") ? 1 : 0, 1);
        P.eq("notes.open_quote", has(c.notes, "panel.gui:18:7: note: a \"text\" without its closing \"") ? 1 : 0, 1);
        P.eq("notes.stray_close", has(c.notes, "panel.gui:19:1: note: a } that closes nothing") ? 1 : 0, 1);
        P.eq("notes.unclosed", has(c.notes, "panel.gui:20:5: note: this { has no }") ? 1 : 0, 1);
        P.eq("notes.rest_stands", find(c.panel.gui, tp::GuiKind::knob, "", level) >= 0 && find(c.panel.gui, tp::GuiKind::knob, "", cutoff) >= 0 ? 1 : 0, 1);
    }

    // ---- two .gui files, pragmas beside one, and PRAGMAS alone -------------------------------------------------------
    {
        const std::string pragmas = "#pragma panel section \"F\" cutoff resonance\n";
        const Compiled both = compile({ { tp::kProgramName, program(pragmas) }, { "panel.gui", "knob cutoff\n" },
                                        { "other.gui", "knob level\n" } });
        P.eq("files.second_gui", has(both.notes, "other.gui: note: the interface is panel.gui's") ? 1 : 0, 1);
        P.eq("files.pragmas_left_out", has(both.notes, "the #pragma panel lines are left out") ? 1 : 0, 1);
        P.eq("files.gui_wins", !both.panel.gui.empty() && both.panel.sections.empty() ? 1 : 0, 1);
        const Compiled alone = compile({ { tp::kProgramName, program(pragmas) } });
        P.eq("pragmas.no_tree", alone.panel.gui.empty() && !alone.panel.sections.empty() ? 1 : 0, 1);
        P.eq("pragmas.no_note_about_gui", has(alone.notes, ".gui") ? 0 : 1, 1);
        P.eq("pragmas.is_gui_file", tp::isGuiFile("panel.gui") && !tp::isGuiFile("gui") && !tp::isGuiFile("panel.fm") ? 1 : 0, 1);
    }

    // ---- STYLES --------------------------------------------------------------------------------------------------------
    {
        const std::string keys = kInstrument ? "  keyboard from=36 octaves=3 height=80\n" : "  spacer\n";
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
        const tp::GuiTree& t = c.panel.gui;
        P.eq("styles.gradient", t.gradient == "#101010" && t.background == "#202020" ? 1 : 0, 1);
        const tp::GuiNode& tabs = at(t, find(t, tp::GuiKind::tabs));
        P.eq("styles.tabs_bar", tabs.style.look == "bar" ? 1 : 0, 1);
        const tp::GuiNode& sec = at(t, find(t, tp::GuiKind::section, "S"));
        P.eq("styles.section_own_look", sec.style.look == "plate" ? 1 : 0, 1);              // its own beats the defaults'
        P.eq("styles.section_defaults_radius", sec.style.radius == 8.0f ? 1 : 0, 1);         // the defaults' still apply
        P.eq("styles.section_title", sec.style.titleAlign == 0 && sec.style.titleSize == 2 && sec.style.shadow == 0 ? 1 : 0, 1);
        const tp::GuiNode& row = at(t, find(t, tp::GuiKind::row));
        P.eq("styles.row_wrap", row.wrap && row.gap == 6.0f && row.margin == 4.0f ? 1 : 0, 1);
        const tp::GuiNode& big = at(t, find(t, tp::GuiKind::knob, "", cutoff));
        P.eq("styles.class", big.style.px == 72.0f && big.style.arc == "#ffaa00" && big.style.pointer == 1 ? 1 : 0, 1);
        P.eq("styles.own_beats_class", big.style.look == "soft" ? 1 : 0, 1);
        P.eq("styles.defaults_ticks", big.style.tickCount == 11 ? 1 : 0, 1);
        P.eq("styles.live", big.live >= 0 && big.live < static_cast<int>(c.panel.displays.size())
                                && c.panel.displays[static_cast<std::size_t>(big.live)].hidden
                                && c.panel.displays[static_cast<std::size_t>(big.live)].source == "env" ? 1 : 0, 1);
        const tp::GuiNode& slider = at(t, find(t, tp::GuiKind::slider, "", attack));
        P.eq("styles.live_shared_tap", slider.live == big.live ? 1 : 0, 1);                   // one tap for one field
        P.eq("styles.slider_colours", slider.style.track == "none" && slider.style.cap == "#ffffff" ? 1 : 0, 1);
        const tp::GuiNode& reso = at(t, find(t, tp::GuiKind::knob, "", resonance));
        P.eq("styles.knob_props", reso.style.bipolar && reso.style.labelPos == 1 && reso.style.valueShow == 2
                                      && reso.style.sweep == 300.0f && reso.style.thickness == 4.0f && reso.style.look == "ring" ? 1 : 0, 1);
        const tp::GuiNode& box = at(t, find(t, tp::GuiKind::box));
        P.eq("styles.box", box.style.look == "inset" && box.style.fill == "none" && box.style.borderWidth == 2.0f
                               && !box.style.border.empty() ? 1 : 0, 1);
        const tp::GuiNode& lvl = at(t, find(t, tp::GuiKind::knob, "", level));
        P.eq("styles.self_min", lvl.self == 3 && lvl.minW == 80.0f ? 1 : 0, 1);
        const tp::GuiNode& label = at(t, find(t, tp::GuiKind::label, "BIG"));
        P.eq("styles.label_px", label.textPx == 24.0f && label.textAlign == 2 ? 1 : 0, 1);
        P.eq("styles.divider", at(t, find(t, tp::GuiKind::divider)).style.thickness == 3.0f ? 1 : 0, 1);
        if constexpr (kInstrument)
        {
            const tp::GuiNode& kb = at(t, find(t, tp::GuiKind::keyboard));
            P.eq("styles.keyboard", kb.from == 36 && kb.keys == 37 && kb.height == 80.0f ? 1 : 0, 1);
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
        P.eq("styles.mistakes_rest_stands", find(m.panel.gui, tp::GuiKind::knob, "", cutoff) >= 0
                                                && find(m.panel.gui, tp::GuiKind::knob, "", resonance) >= 0 ? 1 : 0, 1);
    }

    // ---- an instrument has no input ------------------------------------------------------------------------------------
    {
        const Compiled c = withGui("meter in\nknob cutoff\n");
        P.eq("in.instrument_note", has(c.notes, "an instrument has no input") ? (kInstrument ? 1 : 0) : (kInstrument ? 0 : 1), 1);
    }
    return P.finish();
}
