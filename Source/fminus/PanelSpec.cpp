// Source/fminus/PanelSpec.cpp: the #pragma panel lines read (PanelSpec.h). Two passes over the lines: the displays
// first, so that a section may name a display whose line comes after it; then everything else.
#include "fminus/PanelSpec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace testingplayground
{
    namespace
    {
        struct Token
        {
            enum class Kind : uint8_t { name, text, number };
            Kind kind = Kind::name;
            std::string value;
            double number = 0.0;
            bool isName() const noexcept { return kind == Kind::name; }
            bool isText() const noexcept { return kind == Kind::text; }
            bool isNumber() const noexcept { return kind == Kind::number; }
        };

        struct Line
        {
            const SourceFile* file = nullptr;
            int no = 0;
            std::vector<Token> words;
        };

        bool nameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
        bool nameChar(char c) { return nameStart(c) || (c >= '0' && c <= '9'); }
        bool digit(char c) { return c >= '0' && c <= '9'; }
        bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

        // The rest of the line after "#pragma panel", when the line is one; else false.
        bool panelLine(std::string_view line, std::string_view& rest)
        {
            std::size_t i = 0;
            const auto skip = [&] {
                while (i < line.size() && space(line[i]))
                    ++i;
            };
            const auto word = [&](const char* w) {
                const std::size_t n = std::strlen(w);
                if (line.substr(i, n) != w || (i + n < line.size() && nameChar(line[i + n])))
                    return false;
                i += n;
                return true;
            };
            skip();
            if (i >= line.size() || line[i] != '#')
                return false;
            ++i;
            skip();
            if (!word("pragma"))
                return false;
            skip();
            if (!word("panel"))
                return false;
            rest = line.substr(i);
            return true;
        }

        // Names, "texts" and numbers (-24, 0.5); commas count as spaces and // ends the line. False, with why, for
        // what is none of them.
        bool tokens(std::string_view s, std::vector<Token>& out, std::string& why)
        {
            std::size_t i = 0;
            while (i < s.size())
            {
                const char c = s[i];
                if (space(c) || c == ',')
                    ++i;
                else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
                    break;
                else if (c == '"')
                {
                    const std::size_t close = s.find('"', i + 1);
                    if (close == std::string_view::npos)
                    {
                        why = "a text without its closing \"";
                        return false;
                    }
                    out.push_back({ Token::Kind::text, std::string(s.substr(i + 1, close - i - 1)), 0.0 });
                    i = close + 1;
                }
                else if (nameStart(c))
                {
                    const std::size_t start = i;
                    while (i < s.size() && nameChar(s[i]))
                        ++i;
                    out.push_back({ Token::Kind::name, std::string(s.substr(start, i - start)), 0.0 });
                }
                else if (digit(c) || ((c == '-' || c == '.') && i + 1 < s.size() && (digit(s[i + 1]) || s[i + 1] == '.')))
                {
                    const std::size_t start = i;
                    ++i;
                    while (i < s.size() && (digit(s[i]) || s[i] == '.'))
                        ++i;
                    const std::string text(s.substr(start, i - start));
                    char* end = nullptr;
                    const double v = std::strtod(text.c_str(), &end);
                    if (end == nullptr || *end != '\0' || !std::isfinite(v))
                    {
                        why = "'" + text + "' is not a number";
                        return false;
                    }
                    out.push_back({ Token::Kind::number, text, v });
                }
                else
                {
                    why = std::string("'") + c + "' is neither a name, a \"text\" nor a number";
                    return false;
                }
            }
            return true;
        }

        std::string dialNames(const Controls& controls)
        {
            std::string out;
            for (const Control& c : controls)
                if (c.used)
                    out += (out.empty() ? "" : ", ") + std::string(c.name);
            return out.empty() ? "none" : out;
        }

        bool isColor(const std::string& name)
        {
            return std::any_of(std::begin(kPanelColors), std::end(kPanelColors), [&](const char* c) { return name == c; });
        }

        std::string colorNames()
        {
            std::string out;
            for (const char* c : kPanelColors)
                out += (out.empty() ? "" : ", ") + std::string(c);
            return out;
        }

        // How many values an int or a bool dial takes (0 for a real one).
        int valuesOf(const Control& c)
        {
            if (c.type == ControlType::toggle)
                return 2;
            if (c.type == ControlType::integer)
                return static_cast<int>(std::lround(c.max) - std::lround(c.min)) + 1;
            return 0;
        }

        bool isDisplayWord(const std::string& w)
        {
            return w == "meter" || w == "vu" || w == "led" || w == "number" || w == "scope";
        }

        DisplayKind displayKind(const std::string& w)
        {
            if (w == "vu")
                return DisplayKind::vu;
            if (w == "led")
                return DisplayKind::led;
            if (w == "number")
                return DisplayKind::number;
            if (w == "scope")
                return DisplayKind::scope;
            return DisplayKind::meter;
        }

        constexpr const char* kWords = "section, row, label, small, style, options, color, meter, vu, led, number or scope";
    }

    PanelSpec readPanelSpec(const std::vector<const SourceFile*>& files, const Controls& controls,
                            const std::vector<PanelField>& fields, bool instrument, std::string& notes)
    {
        PanelSpec spec;
        const auto noteAt = [&](const Line& l, const std::string& what) {
            notes += l.file->name + ":" + std::to_string(l.no) + ":1: note: #pragma panel: " + what + "\n";
        };

        // The panel lines, each in words.
        std::vector<Line> lines;
        for (const SourceFile* f : files)
        {
            if (f == nullptr)
                continue;
            std::string_view text = f->text;
            int lineNo = 0;
            while (!text.empty())
            {
                const std::size_t end = text.find('\n');
                const std::string_view line = text.substr(0, end);
                text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
                ++lineNo;
                std::string_view rest;
                if (!panelLine(line, rest))
                    continue;
                Line l{ f, lineNo, {} };
                std::string why;
                if (!tokens(rest, l.words, why))
                    noteAt(l, why);
                else if (l.words.empty() || !l.words[0].isName())
                    noteAt(l, std::string("a panel line is ") + kWords + ", then what it takes");
                else
                    lines.push_back(std::move(l));
            }
        }

        // A dial's slot by its parameter's name, else -1 (and a note).
        const auto slotOf = [&](const Line& l, const std::string& name) {
            for (int c = 0; c < kMaxControls; ++c)
                if (controls[static_cast<std::size_t>(c)].used && name == controls[static_cast<std::size_t>(c)].name)
                    return c;
            noteAt(l, "there is no dial " + name + " (the dials: " + dialNames(controls) + ")");
            return -1;
        };
        const auto displayOf = [&](const std::string& name) {
            for (std::size_t d = 0; d < spec.displays.size(); ++d)
                if (spec.displays[d].name == name)
                    return static_cast<int>(d);
            return -1;
        };
        const auto isDial = [&](const std::string& name) {
            for (const Control& c : controls)
                if (c.used && name == c.name)
                    return true;
            return false;
        };

        // ---- pass 1: the displays ----------------------------------------------------------------------------------
        int taps = 0;
        for (const Line& l : lines)
        {
            const std::string& what = l.words[0].value;
            if (!isDisplayWord(what))
                continue;
            if (l.words.size() < 2 || !l.words[1].isName())
            {
                noteAt(l, what + " takes a source: in, out, or a field of the main module");
                continue;
            }
            PanelDisplay d;
            d.kind = displayKind(what);
            d.source = l.words[1].value;
            d.name = d.source;
            // After the source, in any order: a range (two numbers), `as` and a name, a "label".
            bool ranged = false, named = false, labelled = false, extra = false;
            for (std::size_t k = 2; k < l.words.size(); ++k)
            {
                const Token& t = l.words[k];
                if (t.isNumber() && !ranged && k + 1 < l.words.size() && l.words[k + 1].isNumber())
                {
                    ranged = true;
                    d.lo = static_cast<float>(t.number);
                    d.hi = static_cast<float>(l.words[++k].number);
                    if (!(d.hi > d.lo))
                    {
                        noteAt(l, "a range goes from low to high: " + d.source + " shows 0 to 1");
                        d.lo = 0.0f;
                        d.hi = 1.0f;
                    }
                }
                else if (t.isName() && t.value == "as" && !named && k + 1 < l.words.size() && l.words[k + 1].isName())
                {
                    named = true;
                    d.name = l.words[++k].value;
                }
                else if (t.isText() && !labelled)
                {
                    labelled = true;
                    d.label = t.value;
                    if (d.label.size() > kMaxDialLabel)
                    {
                        noteAt(l, "the label \"" + d.label + "\" is cut to " + std::to_string(kMaxDialLabel) + " characters");
                        d.label.resize(kMaxDialLabel);
                    }
                }
                else if (!extra)
                {
                    extra = true;
                    noteAt(l, what + " takes a source, then a range (two numbers), as and a name, and a \"label\", each "
                                  "optional: \"" + t.value + "\" is left out");
                }
            }
            if (d.source == "in" && instrument)
            {
                noteAt(l, "an instrument has no input: in shows nothing");
                continue;
            }
            if (!d.audio())
            {
                const auto f = std::find_if(fields.begin(), fields.end(), [&](const PanelField& x) { return x.name == d.source; });
                if (f == fields.end())
                {
                    std::string have;
                    for (const PanelField& x : fields)
                        if (x.type == "float" || x.type == "int" || x.type == "bool")
                            have += (have.empty() ? "" : ", ") + x.name;
                    noteAt(l, "there is no field " + d.source + " in the main module (a display shows in, out or a float, "
                                  "int or bool field" + (have.empty() ? std::string(")") : ": " + have + ")"));
                    continue;
                }
                if (f->type.rfind("param", 0) == 0)
                {
                    noteAt(l, d.source + " is a dial; a display shows a field the program writes, or in or out");
                    continue;
                }
                if (f->type != "float" && f->type != "int" && f->type != "bool")
                {
                    noteAt(l, d.source + " is a " + f->type + "; a display shows a float, int or bool field");
                    continue;
                }
            }
            if (displayOf(d.name) >= 0)
            {
                noteAt(l, "a display is named " + d.name + " already; this one is left out (name it with as)");
                continue;
            }
            if (isDial(d.name))
            {
                noteAt(l, d.name + " is a dial's name; this display is left out (name it with as)");
                continue;
            }
            if (static_cast<int>(spec.displays.size()) >= kMaxDisplays)
            {
                noteAt(l, "a panel shows " + std::to_string(kMaxDisplays) + " displays at most; " + d.source + " is left out");
                continue;
            }
            if (!d.audio())
                d.tap = taps++;
            spec.displays.push_back(std::move(d));
        }

        // ---- pass 2: the sections, and what dials look like ----------------------------------------------------------
        std::array<bool, kMaxControls> placed{};
        std::vector<bool> shown(spec.displays.size(), false);
        bool rowNext = false, sectionLines = false;
        struct Paint
        {
            const Line* line;
            std::string title, color;
        };
        std::vector<Paint> titled;                           // colours of sections named by their titles, for later
        for (const Line& l : lines)
        {
            const std::string& what = l.words[0].value;
            const std::vector<Token>& w = l.words;
            if (isDisplayWord(what))
                continue;
            if (what == "section")
            {
                sectionLines = true;
                PanelSection s;
                std::size_t k = 1;
                if (k < w.size() && w[k].isText())
                {
                    s.title = w[k++].value;
                    if (s.title.size() > kMaxSectionTitle)
                    {
                        noteAt(l, "the title \"" + s.title + "\" is cut to " + std::to_string(kMaxSectionTitle) + " characters");
                        s.title.resize(kMaxSectionTitle);
                    }
                }
                for (; k < w.size(); ++k)
                {
                    if (!w[k].isName())
                    {
                        noteAt(l, "a section takes one title, first, then the names of its dials and displays: \""
                                      + w[k].value + "\" is left out");
                        continue;
                    }
                    if (const int d = displayOf(w[k].value); d >= 0)
                    {
                        if (shown[static_cast<std::size_t>(d)])
                        {
                            noteAt(l, "the display " + w[k].value + " is in an earlier section; it stays there");
                            continue;
                        }
                        shown[static_cast<std::size_t>(d)] = true;
                        s.items.push_back({ true, d });
                        continue;
                    }
                    const int c = slotOf(l, w[k].value);
                    if (c < 0)
                        continue;
                    if (placed[static_cast<std::size_t>(c)])
                    {
                        noteAt(l, w[k].value + " is in an earlier section; it stays there");
                        continue;
                    }
                    placed[static_cast<std::size_t>(c)] = true;
                    s.items.push_back({ false, c });
                }
                if (s.items.empty())
                {
                    noteAt(l, "the section " + (s.title.empty() ? std::string("without a title") : "\"" + s.title + "\"")
                                  + " has no dial or display, so it is not drawn");
                    continue;
                }
                s.newRow = rowNext && !spec.sections.empty();
                rowNext = false;
                spec.sections.push_back(std::move(s));
            }
            else if (what == "row")
            {
                if (w.size() > 1)
                    noteAt(l, "row takes nothing after it");
                rowNext = true;
            }
            else if (what == "label")
            {
                if (w.size() != 3 || !w[1].isName() || !w[2].isText())
                {
                    noteAt(l, "label takes a dial's name and its \"label\"");
                    continue;
                }
                const int c = slotOf(l, w[1].value);
                if (c < 0)
                    continue;
                std::string label = w[2].value;
                if (label.size() > kMaxDialLabel)
                {
                    noteAt(l, "the label \"" + label + "\" is cut to " + std::to_string(kMaxDialLabel) + " characters");
                    label.resize(kMaxDialLabel);
                }
                spec.labels[static_cast<std::size_t>(c)] = label;
            }
            else if (what == "small")
            {
                if (w.size() < 2)
                    noteAt(l, "small takes the names of the dials to draw small");
                for (std::size_t k = 1; k < w.size(); ++k)
                {
                    if (!w[k].isName())
                    {
                        noteAt(l, "small takes dials' names, not \"" + w[k].value + "\"");
                        continue;
                    }
                    if (const int c = slotOf(l, w[k].value); c >= 0)
                        spec.small[static_cast<std::size_t>(c)] = true;
                }
            }
            else if (what == "style")
            {
                // Dials' names, then the style they take: a dial it does not suit is a note, and keeps its own.
                bool names = w.size() >= 3;
                for (std::size_t k = 1; k < w.size(); ++k)
                    names = names && w[k].isName();
                if (!names)
                {
                    noteAt(l, "style takes dials' names, then a style: knob, vintage, fader, switch or buttons");
                    continue;
                }
                const std::string& st = w.back().value;
                DialStyle style = DialStyle::knob;
                if (st == "knob")
                    style = DialStyle::knob;
                else if (st == "vintage")
                    style = DialStyle::vintage;
                else if (st == "fader")
                    style = DialStyle::fader;
                else if (st == "switch")
                    style = DialStyle::toggle;
                else if (st == "buttons")
                    style = DialStyle::buttons;
                else
                {
                    noteAt(l, "unknown style '" + st + "': knob, vintage, fader, switch or buttons");
                    continue;
                }
                for (std::size_t n = 1; n + 1 < w.size(); ++n)
                {
                    const std::string& name = w[n].value;
                    const int c = slotOf(l, name);
                    if (c < 0)
                        continue;
                    const Control& k = controls[static_cast<std::size_t>(c)];
                    const int values = valuesOf(k);
                    if (style == DialStyle::fader && k.type == ControlType::toggle)
                    {
                        noteAt(l, name + " is a bool: a switch or buttons suit it, not a fader");
                        continue;
                    }
                    if (style == DialStyle::toggle && values != 2)
                    {
                        noteAt(l, "a switch takes a bool, or an int of two values: " + name + " is neither");
                        continue;
                    }
                    if (style == DialStyle::buttons && (values < 2 || values > kMaxOptions))
                    {
                        noteAt(l, "buttons take a bool, or an int of 2 to " + std::to_string(kMaxOptions) + " values: "
                                      + name + " is neither");
                        continue;
                    }
                    spec.styles[static_cast<std::size_t>(c)] = style;
                }
            }
            else if (what == "options")
            {
                if (w.size() < 3 || !w[1].isName())
                {
                    noteAt(l, "options takes an int's or a bool's name and a \"name\" for each of its values");
                    continue;
                }
                const int c = slotOf(l, w[1].value);
                if (c < 0)
                    continue;
                const int values = valuesOf(controls[static_cast<std::size_t>(c)]);
                if (values == 0)
                {
                    noteAt(l, w[1].value + " is a float: options name an int's or a bool's values");
                    continue;
                }
                std::vector<std::string> names;
                for (std::size_t k = 2; k < w.size(); ++k)
                {
                    if (!w[k].isText())
                    {
                        noteAt(l, "options are \"texts\": " + w[k].value + " is left out");
                        continue;
                    }
                    std::string n = w[k].value;
                    if (n.size() > kMaxOptionLabel)
                    {
                        noteAt(l, "the option \"" + n + "\" is cut to " + std::to_string(kMaxOptionLabel) + " characters");
                        n.resize(kMaxOptionLabel);
                    }
                    names.push_back(std::move(n));
                }
                if (static_cast<int>(names.size()) != values)
                {
                    noteAt(l, w[1].value + " takes " + std::to_string(values) + " values, and the line names "
                                  + std::to_string(names.size()) + (static_cast<int>(names.size()) > values
                                                                        ? ": the extra ones are left out"
                                                                        : ": the others keep their numbers"));
                    names.resize(static_cast<std::size_t>(std::min<int>(values, kMaxOptions * 2)));
                }
                spec.options[static_cast<std::size_t>(c)] = std::move(names);
            }
            else if (what == "color")
            {
                if (w.size() != 3 || w[1].isNumber() || !w[2].isName())
                {
                    noteAt(l, "color takes a section's \"title\" or an item's name, then a colour: " + colorNames());
                    continue;
                }
                if (!isColor(w[2].value))
                {
                    noteAt(l, "unknown colour '" + w[2].value + "': " + colorNames());
                    continue;
                }
                if (w[1].isText())
                    titled.push_back({ &l, w[1].value, w[2].value });
                else if (const int d = displayOf(w[1].value); d >= 0)
                    spec.displays[static_cast<std::size_t>(d)].color = w[2].value;
                else if (const int c = slotOf(l, w[1].value); c >= 0)
                    spec.colors[static_cast<std::size_t>(c)] = w[2].value;
            }
            else
                noteAt(l, "unknown '" + what + "': a panel line is " + kWords);
        }

        // The dials and displays no section names, after the sections.
        if (!spec.sections.empty() || !spec.displays.empty())
        {
            PanelSection rest;
            std::string names;
            for (int c = 0; c < kMaxControls; ++c)
                if (controls[static_cast<std::size_t>(c)].used && !placed[static_cast<std::size_t>(c)])
                {
                    rest.items.push_back({ false, c });
                    names += (names.empty() ? "" : ", ") + std::string(controls[static_cast<std::size_t>(c)].name);
                }
            for (std::size_t d = 0; d < spec.displays.size(); ++d)
                if (!shown[d])
                {
                    rest.items.push_back({ true, static_cast<int>(d) });
                    names += (names.empty() ? "" : ", ") + spec.displays[d].name;
                }
            if (!rest.items.empty())
            {
                if (sectionLines)
                    notes += (files.empty() || files.front() == nullptr ? std::string(kProgramName) : files.front()->name)
                             + ": note: #pragma panel: " + names + (rest.items.size() == 1 ? " is" : " are")
                             + " in no section; they come last, untitled\n";
                rest.newRow = false;
                spec.sections.push_back(std::move(rest));
            }
        }

        // Colours for sections named by their titles.
        for (const Paint& p : titled)
        {
            bool found = false;
            for (PanelSection& s : spec.sections)
                if (s.title == p.title)
                {
                    s.color = p.color;
                    found = true;
                }
            if (!found)
                noteAt(*p.line, "there is no section \"" + p.title + "\" to colour");
        }
        return spec;
    }
}
