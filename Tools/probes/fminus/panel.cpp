// PROBE name=fminus.panel timeout=120
//
// fminus.panel: the #pragma panel lines a program lays its dials out with (Source/fminus/PanelSpec.h), as
// Patch::compile reads them for this plugin's kind:
//   LINES     sections with their titles and dials in order, a row break, labels and small dials; a // comment at a
//             line's end and commas between names; spaces before the # and between # and pragma; no note
//   UNTITLED  a section without a title has the title ""
//   REST      the dials no section names come last, in slot order, untitled, with a note naming them
//   NOTES     each line the plugin cannot use is a note on its file and line, and the program still plays: an unknown
//             word, a name that is no dial (a seventeenth parameter's too), a dial in two sections (the first keeps
//             it), a text without its closing quote, a label without its text, a section without a dial (not drawn),
//             small without a name, row with words after it, a title and a label too long (cut)
//   FILES     lines in an included file count, after the program's; in a file no #include reads they do not; other
//             pragmas (#pragma once, #pragma panelx) are no panel line and no note
//   PLAIN     labels and small dials without a section line: no section (the dials in plain rows), the labels kept
#include "Probe.h"

#include "Rig.h"

#include "fminus/Patch.h"

#include "Product.h"

#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;

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
        tp::PanelSpec panel;
        std::string notes;
    };

    Compiled compile(const tp::Sources& files)
    {
        tp::Patch::Result r = tp::Patch::compile(files, 48000.0f);
        Compiled c;
        c.ok = r.patch != nullptr;
        c.notes = r.notes;
        if (r.patch)
            c.panel = r.patch->panel();
        if (!r.errors.empty())
            std::printf("errors:\n%s", r.errors.c_str());
        return c;
    }

    Compiled compile(const std::string& source) { return compile(tp::singleFile(source)); }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

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
        const std::vector<tp::PanelSection>& s = c.panel.sections;
        P.eq("lines.sections", static_cast<int64_t>(s.size()), 4);
        if (s.size() == 4)
        {
            P.eq("lines.titles", s[0].title == "OSC" && s[1].title == "FILTER" && s[2].title == "LFO" && s[3].title == "OUT" ? 1 : 0, 1);
            P.eq("lines.dials_in_order", s[0].slots == std::vector<int>{ 0, 1 } && s[1].slots == std::vector<int>{ 2, 3, 4 }
                                             && s[2].slots == std::vector<int>{ 5, 6 } && s[3].slots == std::vector<int>{ 7 }
                                             ? 1 : 0, 1);
            P.eq("lines.row_break", !s[0].newRow && !s[1].newRow && s[2].newRow && !s[3].newRow ? 1 : 0, 1);
        }
        P.eq("lines.label", c.panel.labels[4] == "ENV AMT" && c.panel.labels[0].empty() ? 1 : 0, 1);
        P.eq("lines.small", c.panel.small[1] && c.panel.small[6] && !c.panel.small[0] ? 1 : 0, 1);
    }

    // UNTITLED, REST
    {
        const Compiled c = compile(program("#pragma panel section shape detune\n#pragma panel section \"F\" cutoff\n", kDials));
        const std::vector<tp::PanelSection>& s = c.panel.sections;
        P.eq("untitled.title_empty", !s.empty() && s[0].title.empty() && s[0].slots == std::vector<int>{ 0, 1 } ? 1 : 0, 1);
        P.eq("rest.last_untitled", s.size() == 3 && s[2].title.empty() && s[2].slots == std::vector<int>{ 3, 4, 5, 6, 7 } ? 1 : 0, 1);
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
                                   "#pragma panel section \"D\" p17\n";                  // 11: a parameter past the dials
        std::vector<std::string> params = kDials;
        for (int k = 9; k <= 17; ++k)
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
        P.eq("notes.section_without_dial", noteOn(6, "has no dial, so it is not drawn") ? 1 : 0, 1);
        P.eq("notes.small_without_name", noteOn(7, "small takes the names") ? 1 : 0, 1);
        P.eq("notes.row_words", noteOn(8, "row takes nothing") ? 1 : 0, 1);
        P.eq("notes.title_cut", noteOn(9, "is cut to 24 characters") ? 1 : 0, 1);
        P.eq("notes.open_quote", noteOn(10, "without its closing") ? 1 : 0, 1);
        P.eq("notes.past_the_dials", noteOn(11, "there is no dial p17") ? 1 : 0, 1);
        const std::vector<tp::PanelSection>& s = c.panel.sections;
        P.eq("notes.first_keeps_dial", !s.empty() && s[0].title == "A" && s[0].slots == std::vector<int>{ 0 } ? 1 : 0, 1);
        P.eq("notes.label_kept_cut", c.panel.labels[1] == std::string("LONG LABEL TEXT HERE").substr(0, 14) ? 1 : 0, 1);
        bool cutTitle = false;
        for (const tp::PanelSection& x : s)
            cutTitle = cutTitle || x.title == std::string("AN EXTREMELY LONG SECTION TITLE").substr(0, 24);
        P.eq("notes.title_kept_cut", cutTitle ? 1 : 0, 1);
        bool drawnC = false;
        for (const tp::PanelSection& x : s)
            drawnC = drawnC || x.title == "C";
        P.eq("notes.empty_section_dropped", drawnC ? 0 : 1, 1);
    }

    // FILES
    {
        tp::Sources files{ { "program.fm", program("#pragma panel section \"MAIN\" shape detune\n#include \"more.fm\"\n#pragma once\n#pragma panelx bogus\n", kDials) },
                           { "more.fm", "#pragma panel section \"MORE\" cutoff resonance\nfloat twice(float x) { return 2 * x; }\n" },
                           { "unread.fm", "#pragma panel section \"UNREAD\" amount\n" } };
        const Compiled c = compile(files);
        P.eq("files.compiles", c.ok ? 1 : 0, 1);
        const std::vector<tp::PanelSection>& s = c.panel.sections;
        P.eq("files.included_counts_after", s.size() >= 2 && s[0].title == "MAIN" && s[1].title == "MORE" ? 1 : 0, 1);
        bool unread = false;
        for (const tp::PanelSection& x : s)
            unread = unread || x.title == "UNREAD";
        P.eq("files.unread_ignored", unread ? 0 : 1, 1);
        P.eq("files.other_pragmas_no_note", lines(c.notes), 1);         // only the dials in no section
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
