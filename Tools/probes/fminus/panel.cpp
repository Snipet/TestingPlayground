// PROBE name=fminus.panel timeout=120
//
// fminus.panel: the #pragma panel lines a program lays its dials out with (Source/fminus/PanelSpec.h), as
// Patch::compile reads them for this plugin's kind:
//   LINES     sections with their titles and dials in order, a row break, labels and small dials; a // comment at a
//             line's end and commas between names; spaces before the # and between # and pragma; no note
//   UNTITLED  a section without a title has the title ""
//   REST      the dials no section names come last, in slot order, untitled, with a note naming them
//   NOTES     each line the plugin cannot use is a note on its file and line, and the program still plays: an unknown
//             word, a name that is no dial (a parameter's past the 32 dials too), a dial in two sections (the first keeps
//             it), a text without its closing quote, a label without its text, a section without a dial (not drawn),
//             small without a name, row with words after it, a title and a label too long (cut)
//   FILES     lines in an included file count, after the program's; in a file no #include reads they do not; other
//             pragmas (#pragma once, #pragma panelx) are no panel line and no note
//   PLAIN     labels and small dials without a section line: no section (the dials in plain rows), the labels kept
//   DISPLAYS  meter, vu, led, number and scope of in, out and the main module's float, int and bool fields: each with
//             its kind, source, name (as gives a second one of a source), label and range, a field's tap in order;
//             a section takes displays by name; a display no section names comes last, untitled. Notes on their lines:
//             a field that is not there, an array field, a dial as a source, a second display under one name, a name
//             that is a dial's, a range from high to low, an instrument's in
//   STYLES    vintage, fader, switch and buttons where they suit the dial, several dials a line; each misfit a note,
//             the dial keeping its style and the line's other dials taking theirs: a bool fader, a switch of three
//             values, buttons of twenty-one; an unknown style
//   OPTIONS   an int's values' names; a float's options and a wrong count are notes (the extra left out)
//   COLOURS   a section's by its title, a dial's, a display's; an unknown colour and a title no section has are notes
//   ONLY      displays without a section line: one section, untitled, of every dial and display, with no note
//   ELEMENTS  a double field, an array's element (bands[2]) and a 2-D array's (grid[1][2]) as displays, each a tap that
//             reads what the program wrote there (the double rounded to a float); a field's attributes as what the line
//             leaves out (ui::label, fminus::range; a number's ui::unit and ui::decimals), the line's own beating them
//             (an `as` name is no label); a label and a unit too long for a display cut where a character starts, with
//             a note, the field keeping them whole; notes for an array named without its indexes, an index outside its
//             extent and one index too many
#include "Probe.h"

#include "Rig.h"

#include "fminus/Patch.h"

#include "Product.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // A program of this plugin's kind: `before` (the panel's lines, an #include), then a main module with these float
    // parameters.
    std::string program(const std::string& before, const std::vector<std::string>& params)
    {
        std::string body;
        for (const std::string& p : params)
            body += "    param float " + p + " [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        if constexpr (kInstrument)
            return before + "module Voice\n{\n" + body + "    float note;\n    void start(float n, float v) { note = n; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return before + "module Effect\n{\n" + body + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    const std::vector<std::string> kDials{ "shape", "detune", "cutoff", "resonance", "amount", "rate", "depth", "level" };

    struct Compiled
    {
        bool ok = false;
        pg::PanelSpec panel;
        std::string notes;
    };

    Compiled compile(const pg::Sources& files)
    {
        pg::Patch::Result r = pg::Patch::compile(files, 48000.0f);
        Compiled c;
        c.ok = r.patch != nullptr;
        c.notes = r.notes;
        if (r.patch)
            c.panel = r.patch->panel();
        if (!r.errors.empty())
            std::printf("errors:\n%s", r.errors.c_str());
        return c;
    }

    Compiled compile(const std::string& source) { return compile(pg::singleFile(source)); }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // A section's dials' slots, in order.
    std::vector<int> dialsOf(const pg::PanelSection& s)
    {
        std::vector<int> out;
        for (const pg::PanelItem& it : s.items)
            if (!it.display)
                out.push_back(it.index);
        return out;
    }

    int lines(const std::string& s)
    {
        int n = 0;
        for (const char c : s)
            n += c == '\n' ? 1 : 0;
        return n;
    }
}

PROBE(fminus, panel)
{
    // LINES
    {
        const Compiled c = compile(program("#pragma panel section \"OSC\" shape, detune   // the sources\n"
                                           "  #  pragma panel section \"FILTER\" cutoff resonance amount\n"
                                           "#pragma panel row\n"
                                           "#pragma panel section \"LFO\" rate depth\n"
                                           "#pragma panel section \"OUT\" level\n"
                                           "#pragma panel label amount \"ENV AMT\"\n"
                                           "#pragma panel small detune depth\n",
                                           kDials));
        P.eq("lines.compiles", c.ok ? 1 : 0, 1);
        P.eq("lines.no_note", c.notes.empty() ? 1 : 0, 1);
        if (!c.notes.empty())
            std::printf("%s", c.notes.c_str());
        const std::vector<pg::PanelSection>& s = c.panel.sections;
        P.eq("lines.sections", static_cast<int64_t>(s.size()), 4);
        if (s.size() == 4)
        {
            P.eq("lines.titles", s[0].title == "OSC" && s[1].title == "FILTER" && s[2].title == "LFO" && s[3].title == "OUT" ? 1 : 0, 1);
            P.eq("lines.dials_in_order", dialsOf(s[0]) == std::vector<int>{ 0, 1 } && dialsOf(s[1]) == std::vector<int>{ 2, 3, 4 }
                                             && dialsOf(s[2]) == std::vector<int>{ 5, 6 } && dialsOf(s[3]) == std::vector<int>{ 7 }
                                             ? 1 : 0, 1);
            P.eq("lines.row_break", !s[0].newRow && !s[1].newRow && s[2].newRow && !s[3].newRow ? 1 : 0, 1);
        }
        P.eq("lines.label", c.panel.labels[4] == "ENV AMT" && c.panel.labels[0].empty() ? 1 : 0, 1);
        P.eq("lines.small", c.panel.small[1] && c.panel.small[6] && !c.panel.small[0] ? 1 : 0, 1);
    }

    // UNTITLED, REST
    {
        const Compiled c = compile(program("#pragma panel section shape detune\n#pragma panel section \"F\" cutoff\n", kDials));
        const std::vector<pg::PanelSection>& s = c.panel.sections;
        P.eq("untitled.title_empty", !s.empty() && s[0].title.empty() && dialsOf(s[0]) == std::vector<int>{ 0, 1 } ? 1 : 0, 1);
        P.eq("rest.last_untitled", s.size() == 3 && s[2].title.empty() && dialsOf(s[2]) == std::vector<int>{ 3, 4, 5, 6, 7 } ? 1 : 0, 1);
        P.eq("rest.noted", has(c.notes, "resonance, amount, rate, depth, level are in no section") ? 1 : 0, 1);
    }

    // NOTES: each on its line.
    {
        const std::string before = "#pragma panel section \"A\" shape cutof\n"           // 1: no dial cutof
                                   "#pragma panel section \"B\" shape detune\n"          // 2: shape twice
                                   "#pragma panel bogus shape\n"                         // 3: an unknown word
                                   "#pragma panel label detune \"LONG LABEL TEXT HERE\"\n"   // 4: cut
                                   "#pragma panel label detune\n"                        // 5: no text
                                   "#pragma panel section \"C\" nothing\n"               // 6: no dial (and not drawn)
                                   "#pragma panel small\n"                               // 7: no name
                                   "#pragma panel row now\n"                             // 8: words after row
                                   "#pragma panel section \"AN EXTREMELY LONG SECTION TITLE\" cutoff\n"   // 9: cut
                                   "#pragma panel section \"open cutoff\n"               // 10: no closing quote
                                   "#pragma panel section \"D\" p" + std::to_string(pg::kMaxControls + 1) + "\n";   // 11: past the dials
        std::vector<std::string> params = kDials;
        for (int k = 9; k <= pg::kMaxControls + 1; ++k)
            params.push_back("p" + std::to_string(k));
        const Compiled c = compile(program(before, params));
        P.eq("notes.still_plays", c.ok ? 1 : 0, 1);
        std::printf("%s", c.notes.c_str());
        const auto noteOn = [&](int line, const char* what) {
            return has(c.notes, "program.fm:" + std::to_string(line) + ":1: note: #pragma panel: ") && has(c.notes, what);
        };
        P.eq("notes.no_such_dial", noteOn(1, "there is no dial cutof") ? 1 : 0, 1);
        P.eq("notes.dial_twice", noteOn(2, "shape is in an earlier section") ? 1 : 0, 1);
        P.eq("notes.unknown_word", noteOn(3, "unknown 'bogus'") ? 1 : 0, 1);
        P.eq("notes.label_cut", noteOn(4, "is cut to 14 characters") ? 1 : 0, 1);
        P.eq("notes.label_text", noteOn(5, "label takes a dial's name and its") ? 1 : 0, 1);
        P.eq("notes.section_without_dial", noteOn(6, "has no dial or display, so it is not drawn") ? 1 : 0, 1);
        P.eq("notes.small_without_name", noteOn(7, "small takes the names") ? 1 : 0, 1);
        P.eq("notes.row_words", noteOn(8, "row takes nothing") ? 1 : 0, 1);
        P.eq("notes.title_cut", noteOn(9, "is cut to 24 characters") ? 1 : 0, 1);
        P.eq("notes.open_quote", noteOn(10, "without its closing") ? 1 : 0, 1);
        P.eq("notes.past_the_dials", noteOn(11, ("there is no dial p" + std::to_string(pg::kMaxControls + 1)).c_str()) ? 1 : 0, 1);
        const std::vector<pg::PanelSection>& s = c.panel.sections;
        P.eq("notes.first_keeps_dial", !s.empty() && s[0].title == "A" && dialsOf(s[0]) == std::vector<int>{ 0 } ? 1 : 0, 1);
        P.eq("notes.label_kept_cut", c.panel.labels[1] == std::string("LONG LABEL TEXT HERE").substr(0, 14) ? 1 : 0, 1);
        bool cutTitle = false;
        for (const pg::PanelSection& x : s)
            cutTitle = cutTitle || x.title == std::string("AN EXTREMELY LONG SECTION TITLE").substr(0, 24);
        P.eq("notes.title_kept_cut", cutTitle ? 1 : 0, 1);
        bool drawnC = false;
        for (const pg::PanelSection& x : s)
            drawnC = drawnC || x.title == "C";
        P.eq("notes.empty_section_dropped", drawnC ? 0 : 1, 1);
    }

    // FILES
    {
        pg::Sources files{ { "program.fm", program("#pragma panel section \"MAIN\" shape detune\n#include \"more.fm\"\n#pragma once\n#pragma panelx bogus\n", kDials) },
                           { "more.fm", "#pragma panel section \"MORE\" cutoff resonance\nfloat twice(float x) { return 2 * x; }\n" },
                           { "unread.fm", "#pragma panel section \"UNREAD\" amount\n" } };
        const Compiled c = compile(files);
        P.eq("files.compiles", c.ok ? 1 : 0, 1);
        const std::vector<pg::PanelSection>& s = c.panel.sections;
        P.eq("files.included_counts_after", s.size() >= 2 && s[0].title == "MAIN" && s[1].title == "MORE" ? 1 : 0, 1);
        bool unread = false;
        for (const pg::PanelSection& x : s)
            unread = unread || x.title == "UNREAD";
        P.eq("files.unread_ignored", unread ? 0 : 1, 1);
        P.eq("files.other_pragmas_no_note", lines(c.notes), 1);         // only the dials in no section
    }

    // DISPLAYS, STYLES, OPTIONS, COLOURS
    {
        const std::string before =
            "#pragma panel meter out\n"                                        // 1
            "#pragma panel vu out\n"                                           // 2: a second display named out
            "#pragma panel vu out as needle \"OUTPUT\"\n"                      // 3
            "#pragma panel meter gr 0 24 \"GR\"\n"                             // 4: tap 0
            "#pragma panel led gate\n"                                         // 5: tap 1, a bool
            "#pragma panel number steps\n"                                     // 6: tap 2, an int
            "#pragma panel scope nosuch\n"                                     // 7: no field
            "#pragma panel meter buf\n"                                        // 8: an array
            "#pragma panel meter cutoff\n"                                     // 9: a dial
            "#pragma panel meter gr 5 1 as grbad\n"                            // 10: high to low, kept 0..1 (tap 3)
            + std::string(kInstrument ? "#pragma panel led in\n" : "#pragma panel led in \"CLIP\"\n")   // 11
            + "#pragma panel scope out as cutoff\n"                            // 12: a dial's name
              "#pragma panel section \"S\" needle gr gate cutoff\n"            // 13
              "#pragma panel style wide vintage\n"                            // 14
              "#pragma panel style on cutoff fader\n"                         // 15: a bool fader, then cutoff
              "#pragma panel style mode switch\n"                              // 16: three values
              "#pragma panel style pair switch\n"                              // 17
              "#pragma panel style wide buttons\n"                             // 18: twenty-one values
              "#pragma panel style mode buttons\n"                             // 19
              "#pragma panel style on weird\n"                                 // 20: no such style
              "#pragma panel options mode \"A\" \"B\" \"C\"\n"                 // 21
              "#pragma panel options cutoff \"X\"\n"                           // 22: a float
              "#pragma panel options pair \"ONE\" \"TWO\" \"THREE\"\n"         // 23: one too many
              "#pragma panel color \"S\" orange\n"                             // 24
              "#pragma panel color \"NOPE\" red\n"                             // 25: no such section
              "#pragma panel color cutoff purple\n"                            // 26: no such colour
              "#pragma panel color needle blue\n"                              // 27
              "#pragma panel color mode red\n";                                // 28
        // The dials (and the fields the program writes, which only an effect's process writes here).
        const std::string params = std::string("    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                               "    param int mode [[fminus::range(0, 2)]] [[fminus::default(0)]];\n"
                                               "    param bool on [[fminus::default(0)]];\n"
                                               "    param int pair [[fminus::range(0, 1)]] [[fminus::default(0)]];\n"
                                               "    param int wide [[fminus::range(0, 20)]] [[fminus::default(0)]];\n"
                                               "    float gr;\n    int steps;\n    bool gate;\n    float buf[4];\n");
        const std::string source = kInstrument
            ? before + "module Voice\n{\n" + params + "    float note;\n    void start(float n, float v) { note = n; }\n"
                       "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
            : before + "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const Compiled c = compile(source);
        P.eq("displays.compiles", c.ok ? 1 : 0, 1);
        std::printf("%s", c.notes.c_str());
        const auto noteOn = [&](int line, const char* what) {
            return has(c.notes, "program.fm:" + std::to_string(line) + ":1: note: #pragma panel: ") && has(c.notes, what);
        };
        const std::vector<pg::PanelDisplay>& d = c.panel.displays;
        P.eq("displays.count", static_cast<int64_t>(d.size()), kInstrument ? 6 : 7);
        const auto find = [&](const std::string& name) -> const pg::PanelDisplay* {
            for (const pg::PanelDisplay& x : d)
                if (x.name == name)
                    return &x;
            return nullptr;
        };
        const pg::PanelDisplay* out = find("out");
        P.eq("displays.audio_meter", out != nullptr && out->kind == pg::DisplayKind::meter && out->audio() && out->tap < 0 ? 1 : 0, 1);
        const pg::PanelDisplay* vu = find("needle");
        P.eq("displays.named_vu", vu != nullptr && vu->kind == pg::DisplayKind::vu && vu->source == "out" && vu->label == "OUTPUT" ? 1 : 0, 1);
        const pg::PanelDisplay* gr = find("gr");
        P.eq("displays.field_range", gr != nullptr && gr->lo == 0.0f && gr->hi == 24.0f && gr->tap == 0 && gr->label == "GR" ? 1 : 0, 1);
        const pg::PanelDisplay* gate = find("gate");
        const pg::PanelDisplay* steps = find("steps");
        P.eq("displays.taps_in_order", gate != nullptr && gate->tap == 1 && gate->kind == pg::DisplayKind::led && steps != nullptr
                                           && steps->tap == 2 && steps->kind == pg::DisplayKind::number ? 1 : 0, 1);
        const pg::PanelDisplay* bad = find("grbad");
        P.eq("displays.range_reset", bad != nullptr && bad->lo == 0.0f && bad->hi == 1.0f && bad->tap == 3 ? 1 : 0, 1);
        P.eq("notes.second_display_named", noteOn(2, "a display is named out already") ? 1 : 0, 1);
        P.eq("notes.no_field", noteOn(7, "there is no field nosuch") ? 1 : 0, 1);
        P.eq("notes.array_field", noteOn(8, "buf is a float[4]") ? 1 : 0, 1);
        P.eq("notes.dial_source", noteOn(9, "cutoff is a dial; a display shows a field") ? 1 : 0, 1);
        P.eq("notes.range_order", noteOn(10, "a range goes from low to high") ? 1 : 0, 1);
        if constexpr (kInstrument)
            P.eq("notes.instrument_input", noteOn(11, "an instrument has no input") ? 1 : 0, 1);
        else
            P.eq("displays.effect_input", find("in") != nullptr && find("in")->label == "CLIP" ? 1 : 0, 1);
        P.eq("notes.dial_name", noteOn(12, "cutoff is a dial's name") ? 1 : 0, 1);
        // The section takes displays by name; the rest come last.
        const std::vector<pg::PanelSection>& s = c.panel.sections;
        bool mixed = false;
        if (!s.empty() && s[0].title == "S" && s[0].items.size() == 4)
            mixed = s[0].items[0].display && d[static_cast<std::size_t>(s[0].items[0].index)].name == "needle"
                 && s[0].items[1].display && s[0].items[2].display && !s[0].items[3].display && s[0].items[3].index == 0;
        P.eq("displays.in_section", mixed ? 1 : 0, 1);
        int leftover = 0;
        if (s.size() == 2 && s[1].title.empty())
            for (const pg::PanelItem& it : s[1].items)
                leftover += it.display ? 1 : 0;
        P.eq("displays.rest_last", leftover, kInstrument ? 3 : 4);          // out, steps, grbad (and in)
        // Styles.
        P.eq("styles.fader_after_a_misfit", c.panel.styles[0] == pg::DialStyle::fader ? 1 : 0, 1);
        P.eq("styles.vintage", c.panel.styles[4] == pg::DialStyle::vintage ? 1 : 0, 1);
        P.eq("styles.switch_pair", c.panel.styles[3] == pg::DialStyle::toggle ? 1 : 0, 1);
        P.eq("styles.buttons_mode", c.panel.styles[1] == pg::DialStyle::buttons ? 1 : 0, 1);
        P.eq("styles.misfits_keep_theirs", c.panel.styles[2] == pg::DialStyle::knob && c.panel.styles[4] == pg::DialStyle::vintage ? 1 : 0, 1);
        P.eq("notes.bool_fader", noteOn(15, "on is a bool") ? 1 : 0, 1);
        P.eq("notes.switch_three", noteOn(16, "a switch takes a bool, or an int of two values") ? 1 : 0, 1);
        P.eq("notes.buttons_many", noteOn(18, "buttons take a bool, or an int of 2 to 8 values") ? 1 : 0, 1);
        P.eq("notes.unknown_style", noteOn(20, "unknown style 'weird'") ? 1 : 0, 1);
        // Options.
        P.eq("options.mode", c.panel.options[1] == std::vector<std::string>{ "A", "B", "C" } ? 1 : 0, 1);
        P.eq("notes.float_options", noteOn(22, "cutoff is a float") ? 1 : 0, 1);
        P.eq("notes.options_count", noteOn(23, "pair takes 2 values") && c.panel.options[3].size() == 2 ? 1 : 0, 1);
        // Colours.
        P.eq("colours.section", !s.empty() && s[0].color == "orange" ? 1 : 0, 1);
        P.eq("colours.display", vu != nullptr && vu->color == "blue" ? 1 : 0, 1);
        P.eq("colours.dial", c.panel.colors[1] == "red" ? 1 : 0, 1);
        P.eq("notes.no_section_to_colour", noteOn(25, "there is no section \"NOPE\"") ? 1 : 0, 1);
        P.eq("notes.unknown_colour", noteOn(26, "unknown colour 'purple'") ? 1 : 0, 1);
    }

    // ONLY: displays without a section line.
    {
        const Compiled c = compile(program("#pragma panel meter out\n", kDials));
        P.eq("only.one_section", c.panel.sections.size() == 1 && c.panel.sections[0].title.empty()
                                     && c.panel.sections[0].items.size() == kDials.size() + 1 ? 1 : 0, 1);
        P.eq("only.no_note", c.notes.empty() ? 1 : 0, 1);
    }

    // ELEMENTS: a double, an array's element, a 2-D array's, a field's attributes
    {
        // 15 bytes, an A and seven two-byte characters: cut to 14 bytes it would split the last, so it loses it whole.
        const std::string kLongLabel = "A\xc3\x89\xc3\x89\xc3\x89\xc3\x89\xc3\x89\xc3\x89\xc3\x89";
        const std::string before =
            "#pragma panel meter temp\n"                                      // 1: a double, tap 0
            "#pragma panel meter bands[2]\n"                                  // 2: tap 1
            "#pragma panel number grid[1][2]\n"                               // 3: tap 2
            "#pragma panel meter gr\n"                                        // 4: tap 3, its look
            "#pragma panel number gr as grnum\n"                              // 5: tap 4, unit and decimals
            "#pragma panel meter gr 0 12 \"MINE\" as grown\n"                 // 6: tap 5, the line's own
            "#pragma panel meter grid\n"                                      // 7: no indexes
            "#pragma panel meter grid[2][0]\n"                                // 8: outside
            "#pragma panel meter bands[1][1]\n"                               // 9: one too many
            "#pragma panel number bend\n";                                   // 10: tap 6, a look too long
        const std::string fields = "    double temp;\n    float bands[4];\n    float grid[2][3];\n"
                                   "    float gr [[ui::label(\"GR\")]] [[fminus::range(0, 24)]] [[ui::unit(\"dB\")]] [[ui::decimals(1)]];\n"
                                   "    float bend [[ui::unit(\"semitones\")]] [[ui::label(\"" + kLongLabel + "\")]];\n";
        const std::string writes = "        temp = 90.5;\n        for (int k = 0; k < 4; ++k)\n            bands[k] = k + 0.5f;\n"
                                   "        for (int r = 0; r < 2; ++r)\n            for (int c = 0; c < 3; ++c)\n"
                                   "                grid[r][c] = 10 * r + c;\n        gr = 6;\n        bend = 2;\n";
        const std::string source = kInstrument
            ? before + "module Voice\n{\n" + fields + "    void start(float n, float v) {}\n"
                       "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + writes + "        return true;\n    }\n};\n"
            : before + "module Effect\n{\n" + fields + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                       + writes + "    }\n};\n";
        pg::Patch::Result r = pg::Patch::compile(pg::singleFile(source), 48000.0f);
        P.eq("elements.compiles", r.patch != nullptr ? 1 : 0, 1);
        if (!r.errors.empty())
            std::printf("errors:\n%s", r.errors.c_str());
        std::printf("%s", r.notes.c_str());
        if (r.patch != nullptr)
        {
            const std::vector<pg::PanelDisplay>& d = r.patch->panel().displays;
            const auto find = [&](const std::string& name) -> const pg::PanelDisplay* {
                for (const pg::PanelDisplay& x : d)
                    if (x.name == name)
                        return &x;
                return nullptr;
            };
            const pg::PanelDisplay* temp = find("temp");
            const pg::PanelDisplay* band = find("bands[2]");
            const pg::PanelDisplay* cell = find("grid[1][2]");
            const pg::PanelDisplay* gr = find("gr");
            const pg::PanelDisplay* grnum = find("grnum");
            const pg::PanelDisplay* grown = find("grown");
            const pg::PanelDisplay* bend = find("bend");
            P.eq("elements.count", static_cast<int64_t>(d.size()), 7);
            P.eq("elements.taps", r.patch->taps(), 7);
            P.eq("elements.double", temp != nullptr && temp->tap == 0 && !temp->integer ? 1 : 0, 1);
            P.eq("elements.element", band != nullptr && band->tap == 1 && band->source == "bands[2]" ? 1 : 0, 1);
            P.eq("elements.element_2d", cell != nullptr && cell->tap == 2 && cell->kind == pg::DisplayKind::number ? 1 : 0, 1);
            P.eq("elements.look", gr != nullptr && gr->label == "GR" && gr->lo == 0.0f && gr->hi == 24.0f ? 1 : 0, 1);
            P.eq("elements.number_look", grnum != nullptr && grnum->unit == "dB" && grnum->decimals == 1 && grnum->hi == 24.0f ? 1 : 0, 1);
            P.eq("elements.as_keeps_look_label", grnum != nullptr && grnum->label == "GR" ? 1 : 0, 1);   // `as` names it, no more
            P.eq("elements.line_beats_look", grown != nullptr && grown->label == "MINE" && grown->lo == 0.0f && grown->hi == 12.0f
                                                 && grown->unit.empty() ? 1 : 0, 1);
            const auto noteOn = [&](int line, const std::string& what) {
                return has(r.notes, "program.fm:" + std::to_string(line) + ":1: note: #pragma panel: ") && has(r.notes, what);
            };
            P.eq("elements.note_no_index", noteOn(7, "grid is a float[2][3]: an element shows, an index a dimension (grid[1][3])") ? 1 : 0, 1);
            P.eq("elements.note_outside", noteOn(8, "grid[2][0] is outside grid, a float[2][3]") ? 1 : 0, 1);
            P.eq("elements.note_too_many", noteOn(9, "bands is a float[4]: an element shows") ? 1 : 0, 1);
            // A look too long for the display: cut where a character starts, with a note; the field keeps it whole.
            P.eq("elements.long_label_cut", bend != nullptr && bend->label == kLongLabel.substr(0, 13) ? 1 : 0, 1);
            P.eq("elements.long_unit_cut", bend != nullptr && bend->unit == "semiton" ? 1 : 0, 1);
            P.eq("elements.long_look_notes", noteOn(10, "the label \"" + kLongLabel + "\" (bend's ui::label) is cut to 14 characters")
                                                  && noteOn(10, "the unit \"semitones\" (bend's ui::unit) is cut to 7 characters") ? 1 : 0, 1);
            bool whole = false;
            for (const pg::Patch::Field& f : r.patch->fieldMap())
                whole = whole || (f.path == "bend" && f.look.label == kLongLabel && f.look.unit == "semitones");
            P.eq("elements.field_keeps_look_whole", whole ? 1 : 0, 1);
            // What the taps read once the program ran: its values, where FMinus put them.
            std::vector<float> l(16, 0.0f), rr(16, 0.0f);
            if constexpr (kInstrument)
            {
                r.patch->start(0, 60.0f, 0.8f);
                r.patch->render(0, l.data(), rr.data(), 16);
            }
            else
                r.patch->process(l.data(), rr.data(), 16);
            std::printf("elements: taps %g %g %g %g\n", static_cast<double>(r.patch->tap(0, 0)), static_cast<double>(r.patch->tap(1, 0)),
                        static_cast<double>(r.patch->tap(2, 0)), static_cast<double>(r.patch->tap(3, 0)));
            P.near("elements.tap_double", r.patch->tap(0, 0), 90.5, 0.0);
            P.near("elements.tap_element", r.patch->tap(1, 0), 2.5, 0.0);
            P.near("elements.tap_element_2d", r.patch->tap(2, 0), 12.0, 0.0);
            P.near("elements.tap_field", r.patch->tap(3, 0), 6.0, 0.0);
        }
    }

    // PLAIN
    {
        const Compiled c = compile(program("#pragma panel label rate \"RATE\"\n#pragma panel small depth\n", kDials));
        P.eq("plain.no_section", c.panel.sections.empty() ? 1 : 0, 1);
        P.eq("plain.label_kept", c.panel.labels[5] == "RATE" && c.panel.small[6] ? 1 : 0, 1);
        P.eq("plain.no_note", c.notes.empty() ? 1 : 0, 1);
    }
    return P.finish();
}
