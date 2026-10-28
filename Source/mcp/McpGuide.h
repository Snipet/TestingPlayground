// Source/mcp/McpGuide.h: the guide in parts (playground_guide's `part`). The guide is what the plugin's own chat works
// from, for an assistant driving the plugin from outside (ChatAgent::mcpGuide: a word on working from outside, the
// chat's system prompt, then what the MCP tools' short descriptions leave out); it is cut at its headings here, the
// same way in the plugin (McpLink, from the guide it makes) and in the server (mcp/Main.cpp, from the guide.md an
// instance left, when none runs), so the parts stay in step with the prompt (plugin/ChatPrompt.cpp).
//
//   start                 what to read before any work: from the top to THREADS (working from outside, the contract,
//                         the host, FMINUS, DSP PRACTICE, CHECKING YOUR WORK), the tools' notes for it, and a line for
//                         each other part saying when to read it
//   large-program         BUILDING A LARGE PROGRAM, THREADS (FMinus's parallel for and parallel { }), and A PROGRAM
//                         BUILT FROM THE LIBRARY (the example and its panel.gui)
//   interface             THE INTERFACE: panel.gui, and the notes of the tools it uses
//   library               THE LIBRARY's introduction and an index, one line a file, and the notes of the tools it uses
//   library:<file>        that file's header (how to use it), as THE LIBRARY quotes it
//   library:<file>:code   the whole file (the caller's library gives it): nothing is added to the user's editor
//   examples              THIS PLUGIN'S EXAMPLES
//   projects              RANDOM AND PROJECTS
//   all                   all of it
// The tools' notes are the guide's last section: one <tool name=".." part="..">..</tool> a tool, each in the part it
// names. std only: the server includes it as well as the plugin.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace playground::mcp::guide
{
    // The headings the guide is cut at (ChatPrompt.cpp writes them; plugin.mcp checks that every part is found).
    inline constexpr const char* kOutside = "WORKING FROM OUTSIDE THE PLUGIN";
    inline constexpr const char* kThreads = "THREADS: parallel for AND parallel { }";
    inline constexpr const char* kLarge = "BUILDING A LARGE PROGRAM";
    inline constexpr const char* kInterface = "THE INTERFACE: panel.gui";
    inline constexpr const char* kLibrary = "THE LIBRARY";
    inline constexpr const char* kBuilt = "A PROGRAM BUILT FROM THE LIBRARY";
    inline constexpr const char* kExamples = "THIS PLUGIN'S EXAMPLES";
    inline constexpr const char* kProjects = "RANDOM AND PROJECTS";
    inline constexpr const char* kNotes = "THE TOOLS: WHAT THEIR SHORT DESCRIPTIONS LEAVE OUT";

    // The parts a caller may ask for (library:<file> and library:<file>:code aside).
    inline constexpr const char* kParts[] = { "start", "large-program", "interface", "library", "examples", "projects", "all" };

    namespace detail
    {
        // Where heading `h` starts a line of `g` (followed by the line's end or " ("), from `from`; npos when it does not.
        inline std::size_t heading(const std::string& g, const char* h, std::size_t from = 0)
        {
            const std::string want = std::string("\n") + h;
            for (std::size_t p = g.find(want, from); p != std::string::npos; p = g.find(want, p + 1))
            {
                const std::size_t end = p + want.size();
                if (end == g.size() || g[end] == '\n' || g.compare(end, 2, " (") == 0)
                    return p + 1;
            }
            return std::string::npos;
        }

        // The value of attribute `name` in the tag that starts at `at` ("" when it has none).
        inline std::string attribute(const std::string& g, std::size_t at, const char* name)
        {
            const std::size_t close = g.find('>', at);
            const std::string key = std::string(" ") + name + "=\"";
            const std::size_t p = g.find(key, at);
            if (p == std::string::npos || p > close)
                return {};
            const std::size_t q = g.find('"', p + key.size());
            return q == std::string::npos ? std::string() : g.substr(p + key.size(), q - p - key.size());
        }

        inline std::string trimmed(std::string s)
        {
            while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
                s.pop_back();
            std::size_t b = 0;
            while (b < s.size() && (s[b] == '\n' || s[b] == ' '))
                ++b;
            return s.substr(b);
        }

        struct Cuts
        {
            std::size_t threads, large, interface, library, built, examples, projects, notes;
            bool ok = false;
        };

        inline Cuts cuts(const std::string& g)
        {
            Cuts c{};
            c.threads = heading(g, kThreads);
            c.large = heading(g, kLarge, c.threads == std::string::npos ? 0 : c.threads);
            c.interface = heading(g, kInterface, c.large == std::string::npos ? 0 : c.large);
            c.library = heading(g, kLibrary, c.interface == std::string::npos ? 0 : c.interface);
            c.built = heading(g, kBuilt, c.library == std::string::npos ? 0 : c.library);
            c.examples = heading(g, kExamples, c.built == std::string::npos ? 0 : c.built);
            c.projects = heading(g, kProjects, c.examples == std::string::npos ? 0 : c.examples);
            c.notes = heading(g, kNotes, c.projects == std::string::npos ? 0 : c.projects);
            if (c.notes == std::string::npos)
                c.notes = g.size();                      // a guide from before the notes (an older plugin's guide.md)
            c.ok = c.threads != std::string::npos && c.large != std::string::npos && c.interface != std::string::npos && c.library != std::string::npos
                && c.built != std::string::npos && c.examples != std::string::npos && c.projects != std::string::npos
                && c.threads < c.large && c.large < c.interface && c.interface < c.library && c.library < c.built && c.built < c.examples
                && c.examples < c.projects && c.projects <= c.notes;
            return c;
        }

        // The tools' notes for `part`, as the guide's last section has them: "" when there are none.
        inline std::string notes(const std::string& g, std::size_t from, const std::string& part)
        {
            std::string out;
            for (std::size_t p = g.find("<tool name=\"", from); p != std::string::npos; p = g.find("<tool name=\"", p + 1))
            {
                const std::size_t end = g.find("</tool>", p);
                if (end == std::string::npos)
                    break;
                if (attribute(g, p, "part") == part)
                    out += g.substr(p, end + 7 - p) + "\n";
            }
            return out.empty() ? out : "\n" + std::string(kNotes) + "\n\n" + out;
        }

        // The library files THE LIBRARY quotes: each one's name, category and header (its leading comment lines).
        struct Block
        {
            std::string name, category, header;
        };

        inline std::vector<Block> blocks(const std::string& g, std::size_t from, std::size_t to)
        {
            std::vector<Block> out;
            for (std::size_t p = g.find("<library_file name=\"", from); p != std::string::npos && p < to;
                 p = g.find("<library_file name=\"", p + 1))
            {
                const std::size_t open = g.find(">\n", p);
                const std::size_t end = g.find("</library_file>", p);
                if (open == std::string::npos || end == std::string::npos || end > to)
                    break;
                out.push_back({ attribute(g, p, "name"), attribute(g, p, "category"), g.substr(open + 2, end - open - 2) });
            }
            return out;
        }

        // A header's first line's description ("// svf.fm: a state-variable filter" -> "a state-variable filter").
        inline std::string summary(const Block& b)
        {
            const std::string first = b.header.substr(0, b.header.find('\n'));
            const std::size_t colon = first.find(": ");
            return colon == std::string::npos ? trimmed(first) : trimmed(first.substr(colon + 2));
        }
    }

    // Part `name` of the whole guide `g`; found false (and the text says why) when there is no such part. `code` gives a
    // library file's whole text by its name ("" when there is none), for library:<file>:code; it may be empty.
    inline std::string part(const std::string& g, const std::string& name, const std::function<std::string(const std::string&)>& code,
                            bool& found)
    {
        using namespace detail;
        found = true;
        const std::string n = name.empty() ? std::string("start") : name;
        if (n == "all")
            return g;
        const Cuts c = cuts(g);
        if (!c.ok)
        {
            found = false;
            return "This guide cannot be cut into parts (its headings are not all there): read part \"all\".";
        }
        const std::vector<Block> files = blocks(g, c.library, c.built);
        if (n == "start")
        {
            std::string categories;
            for (const Block& b : files)
                if (categories.find(b.category) == std::string::npos)
                    categories += (categories.empty() ? "" : ", ") + b.category;
            return g.substr(0, c.threads) + notes(g, c.notes, "start")
                 + "\nTHE OTHER PARTS OF THIS GUIDE\n\n"
                   "Read one with playground_guide's part when the work needs it:\n"
                   "- \"large-program\": how to build a large program (its files, modules and blocks, its modulation, its "
                   "dials), FMinus's threads for heavy work (parallel for, parallel { }), and a program built from the library "
                   "with its panel.gui. Read it before a program of more than one file, or one that is heavy.\n"
                   "- \"interface\": the panel.gui language (sections, controls and their looks, displays, pages) and looking at "
                   "the panel. Read it before you write or change panel.gui.\n"
                   "- \"library\": the ready-made building blocks (" + categories + "), a line each. Read it before you "
                   "build, and use them rather than writing your own; then \"library:<file>\" gives a block's usage (its "
                   "header), \"library:<file>:code\" its whole code without adding anything to the user's editor.\n"
                   "- \"examples\": this plugin's examples (its EXAMPLES menu), whole.\n"
                   "- \"projects\": RANDOM, and the user's projects and presets.\n"
                   "- \"all\": all of it at once (about " + std::to_string((g.size() / 4 + 500) / 1000) + "K tokens).\n";
        }
        if (n == "large-program")
            return g.substr(c.large, c.interface - c.large) + "\n" + g.substr(c.threads, c.large - c.threads) + "\n"
                 + g.substr(c.built, c.examples - c.built);
        if (n == "interface")
            return g.substr(c.interface, c.library - c.interface) + notes(g, c.notes, "interface");
        if (n == "library")
        {
            const std::size_t first = g.find("<library_file name=\"", c.library);
            std::string out = trimmed(g.substr(c.library, (first == std::string::npos || first > c.built ? c.built : first) - c.library));
            out += "\n\n(Here, a line each. A file's header, which says how to use it: part \"library:<file>\", such as "
                   "\"library:svf.fm\". Its whole code: part \"library:<file>:code\", which reads it without adding a tab to the "
                   "user's editor.)\n\nTHE FILES\n";
            for (const Block& b : files)
                out += "- " + b.name + " (" + b.category + "): " + summary(b) + "\n";
            return out + notes(g, c.notes, "library");
        }
        if (n == "examples")
            return g.substr(c.examples, c.projects - c.examples);
        if (n == "projects")
            return trimmed(g.substr(c.projects, c.notes - c.projects)) + "\n";
        if (n.rfind("library:", 0) == 0)
        {
            std::string file = n.substr(8);
            const bool whole = file.size() > 5 && file.compare(file.size() - 5, 5, ":code") == 0;
            if (whole)
                file.resize(file.size() - 5);
            for (const Block& b : files)
                if (b.name == file)
                {
                    if (!whole)
                        return "<library_file name=\"" + b.name + "\" category=\"" + b.category + "\">\n" + b.header + "</library_file>\n";
                    const std::string text = code ? code(file) : std::string();
                    if (!text.empty())
                        return "<library_file name=\"" + b.name + "\" category=\"" + b.category + "\" whole=\"true\">\n" + text
                             + (text.back() == '\n' ? "" : "\n") + "</library_file>\n";
                    found = false;
                    return "The code of " + file + " comes from the plugin: ask the user to open it, then read this again.";
                }
            std::string have;
            for (const Block& b : files)
                have += (have.empty() ? "" : ", ") + b.name;
            found = false;
            return "The library has no file " + file + ". Its files: " + have + ".";
        }
        std::string have;
        for (const char* p : kParts)
            have += (have.empty() ? "" : ", ") + ("\"" + std::string(p) + "\"");
        found = false;
        return "The guide has no part \"" + n + "\". Its parts: " + have + ", \"library:<file>\" and \"library:<file>:code\".";
    }
}
