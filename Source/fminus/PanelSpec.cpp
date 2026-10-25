// Source/fminus/PanelSpec.cpp: the #pragma panel lines read (PanelSpec.h).
#include "fminus/PanelSpec.h"

#include <cstring>
#include <string_view>

namespace testingplayground
{
    namespace
    {
        struct Token
        {
            bool text = false;                 // "quoted", else a name
            std::string value;
        };

        bool nameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
        bool nameChar(char c) { return nameStart(c) || (c >= '0' && c <= '9'); }
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

        // Names and "texts"; commas count as spaces and // ends the line. False, with why, for what is neither.
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
                    out.push_back({ true, std::string(s.substr(i + 1, close - i - 1)) });
                    i = close + 1;
                }
                else if (nameStart(c))
                {
                    const std::size_t start = i;
                    while (i < s.size() && nameChar(s[i]))
                        ++i;
                    out.push_back({ false, std::string(s.substr(start, i - start)) });
                }
                else
                {
                    why = std::string("'") + c + "' is neither a name nor a \"text\"";
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
    }

    PanelSpec readPanelSpec(const std::vector<const SourceFile*>& files, const Controls& controls, std::string& notes)
    {
        PanelSpec spec;
        std::array<bool, kMaxControls> placed{};
        bool rowNext = false;
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
                const auto note = [&](const std::string& what) {
                    notes += f->name + ":" + std::to_string(lineNo) + ":1: note: #pragma panel: " + what + "\n";
                };
                // A dial's slot by its parameter's name, else -1 (and a note).
                const auto slotOf = [&](const std::string& name) {
                    for (int c = 0; c < kMaxControls; ++c)
                        if (controls[static_cast<std::size_t>(c)].used && name == controls[static_cast<std::size_t>(c)].name)
                            return c;
                    note("there is no dial " + name + " (the dials: " + dialNames(controls) + ")");
                    return -1;
                };
                std::vector<Token> words;
                std::string why;
                if (!tokens(rest, words, why))
                {
                    note(why);
                    continue;
                }
                if (words.empty() || words[0].text)
                {
                    note("a panel line is section, row, label or small, then what it takes");
                    continue;
                }
                const std::string& what = words[0].value;
                if (what == "section")
                {
                    PanelSection s;
                    std::size_t k = 1;
                    if (k < words.size() && words[k].text)
                    {
                        s.title = words[k++].value;
                        if (s.title.size() > kMaxSectionTitle)
                        {
                            note("the title \"" + s.title + "\" is cut to " + std::to_string(kMaxSectionTitle)
                                 + " characters");
                            s.title.resize(kMaxSectionTitle);
                        }
                    }
                    for (; k < words.size(); ++k)
                    {
                        if (words[k].text)
                        {
                            note("a section takes one title, first, then dials' names: \"" + words[k].value
                                 + "\" is left out");
                            continue;
                        }
                        const int c = slotOf(words[k].value);
                        if (c < 0)
                            continue;
                        if (placed[static_cast<std::size_t>(c)])
                        {
                            note(words[k].value + " is in an earlier section; it stays there");
                            continue;
                        }
                        placed[static_cast<std::size_t>(c)] = true;
                        s.slots.push_back(c);
                    }
                    if (s.slots.empty())
                    {
                        note("the section " + (s.title.empty() ? std::string("without a title") : "\"" + s.title + "\"")
                             + " has no dial, so it is not drawn");
                        continue;
                    }
                    s.newRow = rowNext && !spec.sections.empty();
                    rowNext = false;
                    spec.sections.push_back(std::move(s));
                }
                else if (what == "row")
                {
                    if (words.size() > 1)
                        note("row takes nothing after it");
                    rowNext = true;
                }
                else if (what == "label")
                {
                    if (words.size() != 3 || words[1].text || !words[2].text)
                    {
                        note("label takes a dial's name and its \"label\"");
                        continue;
                    }
                    const int c = slotOf(words[1].value);
                    if (c < 0)
                        continue;
                    std::string label = words[2].value;
                    if (label.size() > kMaxDialLabel)
                    {
                        note("the label \"" + label + "\" is cut to " + std::to_string(kMaxDialLabel) + " characters");
                        label.resize(kMaxDialLabel);
                    }
                    spec.labels[static_cast<std::size_t>(c)] = label;
                }
                else if (what == "small")
                {
                    if (words.size() < 2)
                        note("small takes the names of the dials to draw small");
                    for (std::size_t k = 1; k < words.size(); ++k)
                    {
                        if (words[k].text)
                        {
                            note("small takes dials' names, not \"" + words[k].value + "\"");
                            continue;
                        }
                        if (const int c = slotOf(words[k].value); c >= 0)
                            spec.small[static_cast<std::size_t>(c)] = true;
                    }
                }
                else
                    note("unknown '" + what + "': a panel line is section, row, label or small");
            }
        }

        // The dials no section names, after the sections.
        if (!spec.sections.empty())
        {
            PanelSection rest;
            std::string names;
            for (int c = 0; c < kMaxControls; ++c)
                if (controls[static_cast<std::size_t>(c)].used && !placed[static_cast<std::size_t>(c)])
                {
                    rest.slots.push_back(c);
                    names += (names.empty() ? "" : ", ") + std::string(controls[static_cast<std::size_t>(c)].name);
                }
            if (!rest.slots.empty())
            {
                notes += (files.empty() || files.front() == nullptr ? std::string(kProgramName) : files.front()->name)
                         + ": note: #pragma panel: " + names + (rest.slots.size() == 1 ? " is" : " are")
                         + " in no section; they come last, untitled\n";
                spec.sections.push_back(std::move(rest));
            }
        }
        return spec;
    }
}
