// Source/plugin/ChatFiles.cpp: read_file and edit_file (ChatAgent.h's readFileText and editFileText), the chat's and
// MCP's alike.
//
// read_file gives a file whole, or a range of its lines (from, lines) or the lines that hold a text (find, two lines
// around each), numbered; find with regex is a pattern (dsp/Regex.h: linear time, its steps bounded, kSearchSteps), and
// find with no name searches every file (Finder, findInEvery); a part read leaves the chat's record of what its Claude
// saw (markSeen) alone, and an edit marks the file seen only when its Claude had seen the text it edited (carrySeen).
// edit_file replaces one passage
// (old_text, new_text) or a list of them (edits), in order, each occurring exactly once in the text the ones before it
// left (try_edits' rule), all or none: one write, one step the user can undo. A passage that does not occur is answered
// with the closest one in the file, its lines numbered, and what differs. The write goes through chatSetDraftIf: only
// while the file is still the text the tool read, so what the user types meanwhile is never overwritten (the edits are
// tried once more on the new text). With `expected`, the version Claude read (every read and write says the file's
// version, its text's hash), it writes only while the file is still that version: otherwise it writes nothing, says
// whose change it was (Claude's own when a call of Claude's gave the text there is now after that version, else the
// user's: an undo back to a text Claude saw before is the user's), and shows the lines that changed since (the newest
// kFileVersions versions given are kept for that).
// With `files` it does all of that for several files at once: a list of each file's name, edits and expected, or a
// unified diff (each hunk's old lines found where a line starts, the occurrence nearest its line when there are
// several; a line that could be part of the change is never passed over, parseDiff); all or none across them, in one
// write of the draft (chatSetDraftIf's, of every file it read), which the editor merges as one undo step across their
// tabs (Panel.cpp's FilesItem::merge).
#include "plugin/ChatAgent.h"

#include "dsp/Regex.h"
#include "dsp/TextDiff.h"
#include "fminus/Lang.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace playground
{
    namespace
    {
        constexpr std::size_t kMaxOutput = 12000;          // a numbered read's characters at most
        constexpr std::size_t kMaxFound = 40;              // find: the lines that hold it, shown at most
        constexpr int kContext = 2;                        // find: lines around each
        constexpr int kMaxEdits = 32;                      // edit_file's edits a call
        constexpr std::size_t kMaxHunks = 256;             // a diff's hunks a file (each one scan of the file's text)
        constexpr std::size_t kMaxPassage = 30;            // a closest passage's lines shown at most
        constexpr int kChangedLines = 40;                  // a refused write's diff, lines at most

        bool isNumber(const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }

        // The text's lines without their ends ('\n', and a '\r' before it); a last line without one counts.
        std::vector<std::string_view> linesOf(std::string_view t)
        {
            std::vector<std::string_view> out = textdiff::lines(t);
            for (std::string_view& l : out)
                if (!l.empty() && l.back() == '\r')
                    l.remove_suffix(1);
            return out;
        }

        // The 1-based line that byte `at` of `t` is on.
        int lineAt(std::string_view t, std::size_t at)
        {
            return 1 + static_cast<int>(std::count(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(std::min(at, t.size())), '\n'));
        }

        std::string numberedLine(int n, std::string_view text) { return std::to_string(n) + "\t" + std::string(text) + "\n"; }

        // A line too long for what a part read has room for (`room` characters, at least half of kMaxOutput): its head, cut
        // where a character starts, then its length and the way to it all (a part read would show the same head again).
        std::string cutLongLine(int n, std::string_view text, std::size_t room)
        {
            const std::string note = "(line " + std::to_string(n) + " is " + std::to_string(text.size())
                                   + " characters long: cut here; read_file with no from, lines or find gives the whole file)\n";
            const std::size_t head = std::to_string(n).size() + 2 + note.size();
            const std::string_view shown = lang::headOf(text, room > head ? room - head : 0);
            return std::to_string(n) + "\t" + std::string(shown) + "\n" + note;
        }

        // "a", "a and b", "a, b and c".
        std::string joinedAnd(const std::vector<std::string>& parts)
        {
            std::string out;
            for (std::size_t i = 0; i < parts.size(); ++i)
                out += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
            return out;
        }

        // A line for the words: its indentation off, cut past 120 characters.
        std::string shown(std::string_view l)
        {
            while (!l.empty() && (l.front() == ' ' || l.front() == '\t'))
                l.remove_prefix(1);
            return l.size() > 120 ? std::string(l.substr(0, 117)) + "..." : std::string(l);
        }

        // ---- the closest passage ---------------------------------------------------------------------------------------
        // A line without its spaces and tabs, and its character pairs, sorted: two lines that differ only in whitespace
        // squash alike, and the pairs they share say how alike two others are.
        struct Squashed
        {
            std::string text;
            std::vector<uint16_t> pairs;
        };

        Squashed squash(std::string_view l)
        {
            Squashed s;
            for (const char c : l)
                if (c != ' ' && c != '\t')
                    s.text += c;
            for (std::size_t i = 0; i + 1 < s.text.size(); ++i)
                s.pairs.push_back(static_cast<uint16_t>((static_cast<unsigned char>(s.text[i]) << 8) | static_cast<unsigned char>(s.text[i + 1])));
            std::sort(s.pairs.begin(), s.pairs.end());
            return s;
        }

        // The character pairs two squashed lines share (each pair as often as both have it).
        std::size_t sharedPairs(const Squashed& a, const Squashed& b)
        {
            std::size_t i = 0, j = 0, shared = 0;
            while (i < a.pairs.size() && j < b.pairs.size())
            {
                if (a.pairs[i] == b.pairs[j])
                {
                    ++shared;
                    ++i;
                    ++j;
                }
                else if (a.pairs[i] < b.pairs[j])
                    ++i;
                else
                    ++j;
            }
            return shared;
        }

        // How alike two squashed lines are: their shared pairs over all of them (Dice's coefficient), 0 to 1.
        double dice(const Squashed& a, const Squashed& b)
        {
            if (a.pairs.empty() || b.pairs.empty())
                return a.text == b.text ? 1.0 : 0.0;
            return 2.0 * static_cast<double>(sharedPairs(a, b)) / static_cast<double>(a.pairs.size() + b.pairs.size());
        }

        // How much of `part` a line holds: the pairs they share over part's, 0 to 1. A fragment of a long line scores
        // on its own characters, which Dice's coefficient drowns in the line's others.
        double held(const Squashed& line, const Squashed& part)
        {
            if (part.pairs.empty())
                return 0.0;                                      // a character or none: dice's (the same or not)
            return static_cast<double>(sharedPairs(line, part)) / static_cast<double>(part.pairs.size());
        }

        enum class Edge : uint8_t { whole, first, last, only };   // which of old_text's lines (its first may be the end of
                                                                  // a line, its last the start of one)

        // A file's line against one of old_text's: 1 the same (or, at an edge, the part it would be), 0.9 the same but
        // for whitespace, else up to 0.85 as alike as their characters are (at an edge, where old_text's line may be
        // part of the file's, as much of it as the file's holds, if that is more).
        double alike(std::string_view file, const Squashed& fs, std::string_view old, const Squashed& os, Edge edge)
        {
            if (file == old)
                return 1.0;
            const bool ends = file.size() >= old.size() && file.compare(file.size() - old.size(), old.size(), old) == 0;
            const bool starts = file.compare(0, old.size(), old) == 0;
            if (!old.empty() && ((edge == Edge::first && ends) || (edge == Edge::last && starts)
                                 || (edge == Edge::only && file.find(old) != std::string_view::npos)))
                return 1.0;
            if (fs.text == os.text)
                return 0.9;
            return 0.85 * (edge == Edge::whole ? dice(fs, os) : std::max(dice(fs, os), held(fs, os)));
        }

        // What differs between a file's line and one of old_text's, in words: only the whitespace, or the part that is
        // not the same (their common start and end set aside).
        std::string difference(int line, std::string_view file, std::string_view old, bool sameButSpace)
        {
            if (sameButSpace)
                return "line " + std::to_string(line) + " differs only in its spaces or tabs (copy them exactly)";
            std::size_t a = 0;
            while (a < file.size() && a < old.size() && file[a] == old[a])
                ++a;
            std::size_t b = 0;
            while (b < file.size() - a && b < old.size() - a && file[file.size() - 1 - b] == old[old.size() - 1 - b])
                ++b;
            const std::string_view f = file.substr(a, file.size() - a - b), o = old.substr(a, old.size() - a - b);
            std::string out = "line " + std::to_string(line) + " is `" + shown(file) + "` where old_text has `" + shown(old) + "`";
            if (a + b > 0 && f.size() <= 40 && o.size() <= 40)
                out += " (`" + std::string(f) + "` against `" + std::string(o) + "`)";
            return out;
        }

        // The passage of `text` most like `oldText`, its lines numbered and what differs in words ("" when nothing in
        // the file is near it).
        std::string closest(std::string_view text, std::string_view oldText)
        {
            const std::vector<std::string_view> fl = linesOf(text), ol = linesOf(oldText);
            if (fl.empty() || ol.empty())
                return {};
            const std::size_t k = std::min(ol.size(), fl.size());
            std::vector<Squashed> fs, os;
            fs.reserve(fl.size());
            for (const std::string_view l : fl)
                fs.push_back(squash(l));
            for (const std::string_view l : ol)
                os.push_back(squash(l));
            const auto edgeOf = [&](std::size_t j) {
                return ol.size() == 1 ? Edge::only : j == 0 ? Edge::first : j + 1 == ol.size() ? Edge::last : Edge::whole;
            };
            // A long passage in a long file is scored on at most 16 of its lines, spread over it.
            std::vector<std::size_t> scored;
            const std::size_t most = fl.size() * k > 400000 ? 16 : k;
            for (std::size_t m = 0; m < std::min(most, k); ++m)
                scored.push_back(most >= k ? m : m * (k - 1) / (most - 1));
            double best = -1.0;
            std::size_t at = 0;
            for (std::size_t s = 0; s + k <= fl.size(); ++s)
            {
                double score = 0.0;
                for (const std::size_t j : scored)
                    score += alike(fl[s + j], fs[s + j], ol[j], os[j], edgeOf(j));
                if (score > best)
                {
                    best = score;
                    at = s;
                }
            }
            if (best / static_cast<double>(scored.size()) < 0.3)
                return {};
            std::string out = "The closest passage, " + (k == 1 ? "line " + std::to_string(at + 1) : "lines " + std::to_string(at + 1) + " to "
                                                                                                         + std::to_string(at + k))
                            + ":\n";
            for (std::size_t j = 0; j < k && j < kMaxPassage; ++j)
                out += numberedLine(static_cast<int>(at + j + 1), fl[at + j]);
            if (k > kMaxPassage)
                out += "(" + std::to_string(k - kMaxPassage) + " more lines)\n";
            std::vector<std::string> diffs;
            std::size_t differing = 0;
            for (std::size_t j = 0; j < k; ++j)
            {
                const double a = alike(fl[at + j], fs[at + j], ol[j], os[j], edgeOf(j));
                if (a >= 1.0)
                    continue;
                ++differing;
                if (diffs.size() < 4)
                    diffs.push_back(difference(static_cast<int>(at + j + 1), fl[at + j], ol[j], fs[at + j].text == os[j].text));
            }
            if (ol.size() > fl.size())
                diffs.push_back("old_text has " + std::to_string(ol.size()) + " lines, the file " + std::to_string(fl.size()));
            out += diffs.empty() ? std::string("What differs: the line ends (old_text's run on past a line, or start within one).\n")
                                 : "What differs: " + joinedAnd(diffs) + (differing > 4 ? " (and " + std::to_string(differing - 4) + " more lines)" : "")
                                       + ".\n";
            return out;
        }

        // A text's version as the tools say it: the top half of its 64-bit FNV-1a hash, 8 hex digits.
        std::string versionOf(const std::string& text)
        {
            uint64_t h = 1469598103934665603ull;
            for (const char c : text)
            {
                h ^= static_cast<uint8_t>(c);
                h *= 1099511628211ull;
            }
            char buf[16];
            std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(h >> 32));
            return buf;
        }

        // Where `oldText` occurs in `text`: the first place (npos: none), and how often.
        std::size_t occurs(const std::string& text, const std::string& oldText, std::size_t& count, std::vector<std::string>& lines)
        {
            count = 0;
            std::size_t first = std::string::npos;
            for (std::size_t p = text.find(oldText); p != std::string::npos; p = text.find(oldText, p + 1))
            {
                if (count == 0)
                    first = p;
                if (lines.size() < 8)
                    lines.push_back(std::to_string(lineAt(text, p)));
                ++count;
            }
            return first;
        }

        // ---- the edits -------------------------------------------------------------------------------------------------
        // One replacement: old_text by new_text, occurring exactly once in what the edits before it left; or a unified
        // diff's hunk (`hunk`): its old lines (the context and - lines) found where a line starts, the occurrence
        // nearest `line` (its old start, moved by what the hunks before it in the file added and removed) when they
        // occur more than once, and with no old lines its new ones inserted after line `line` (0: at the start).
        struct Edit
        {
            std::string oldText, newText;
            int line = 0;
            bool hunk = false;
        };

        // A file's edits, and the version it must still be ("": any).
        struct FileEdits
        {
            std::string name;
            std::vector<Edit> edits;
            std::string expected;
        };

        // The edits a holder gives (edit_file's input, or one of its files): old_text and new_text, or edits. "" when they
        // read, else why not (`missing`: when it gives neither).
        std::string editsOf(const juce::var& holder, std::vector<Edit>& out, const std::string& missing)
        {
            const juce::var list = holder.getProperty("edits", juce::var());
            const bool single = holder.hasProperty("old_text") || holder.hasProperty("new_text");
            if (!list.isVoid() && !list.isUndefined())
            {
                if (single)
                    return "Give old_text and new_text, or edits, not both";
                const juce::Array<juce::var>* arr = list.getArray();
                if (arr == nullptr || arr->isEmpty() || arr->size() > kMaxEdits)
                    return "edits is a list of 1 to " + std::to_string(kMaxEdits) + " edits, [{old_text, new_text}]";
                for (int i = 0; i < arr->size(); ++i)
                {
                    const juce::var& e = (*arr)[i];
                    const juce::var o = e.getProperty("old_text", juce::var()), n = e.getProperty("new_text", juce::var());
                    if (!o.isString() || !n.isString())
                        return "Edit " + std::to_string(i + 1) + " needs old_text and new_text. Nothing was changed.";
                    out.push_back({ o.toString().toStdString(), n.toString().toStdString() });
                }
                return {};
            }
            const juce::var o = holder.getProperty("old_text", juce::var()), n = holder.getProperty("new_text", juce::var());
            if (!o.isString() || !n.isString())
                return missing;
            out.push_back({ o.toString().toStdString(), n.toString().toStdString() });
            return {};
        }

        // ---- a unified diff --------------------------------------------------------------------------------------------
        // A --- or +++ line's path: a timestamp after a tab and git's a/ or b/ off, quotes taken away.
        std::string diffPath(std::string_view s)
        {
            if (const std::size_t tab = s.find('\t'); tab != std::string_view::npos)
                s = s.substr(0, tab);
            while (!s.empty() && s.back() == ' ')
                s.remove_suffix(1);
            while (!s.empty() && s.front() == ' ')
                s.remove_prefix(1);
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                s = s.substr(1, s.size() - 2);
            if (s.size() > 2 && (s.substr(0, 2) == "a/" || s.substr(0, 2) == "b/"))
                s.remove_prefix(2);
            return std::string(s);
        }

        // A file's header: a --- line and the +++ line after it.
        bool fileHeader(const std::vector<std::string_view>& lines, std::size_t i)
        {
            return i + 1 < lines.size() && lines[i].substr(0, 4) == "--- " && lines[i + 1].substr(0, 4) == "+++ ";
        }

        // "@@ -12,3 +12,4 @@ words": the old side's first line (a count after it is not needed: the hunk's lines say).
        bool hunkHeader(std::string_view l, int& oldStart)
        {
            if (l.substr(0, 4) != "@@ -" || l.find(" @@", 4) == std::string_view::npos)
                return false;
            std::size_t p = 4;
            long v = 0;
            if (p >= l.size() || std::isdigit(static_cast<unsigned char>(l[p])) == 0)
                return false;
            while (p < l.size() && std::isdigit(static_cast<unsigned char>(l[p])) != 0 && v < 100000000)
                v = v * 10 + (l[p++] - '0');
            oldStart = static_cast<int>(v);
            return true;
        }

        // A line for the words, in backticks: cut past 60 characters, a tab shown as \t.
        std::string quoted(std::string_view l)
        {
            std::string out = "`";
            for (const char c : l.substr(0, 60))
                out += c == '\t' ? std::string("\\t") : std::string(1, c);
            return out + (l.size() > 60 ? "...`" : "`");
        }

        // A unified diff of the program's files (git diff's, diff -u's: each file's --- and +++ lines, then its @@ hunks;
        // what comes between them, git's diff and index lines or words, is passed over) as each file's edits, a hunk an
        // edit. The counts in a hunk's header are not needed (its lines say), a blank line in a hunk is a blank line of
        // context whose space was lost, and blank lines at a hunk's end are dropped (context on both sides alike, so the
        // change is the same). Nothing that could be part of a change is passed over: once a file's header is read, a
        // context, - or + line outside a hunk is refused (naming the line that ended the hunk before it, such as a context
        // line that lost its space or was indented with a tab), so is an @@ line that is no hunk's header, and so are git's
        // lines that make, delete, rename or copy a file. "" when it reads, else why not.
        std::string parseDiff(const std::string& diff, std::vector<FileEdits>& out)
        {
            const std::vector<std::string_view> lines = linesOf(diff);
            const auto starts = [](std::string_view l, std::string_view s) { return l.substr(0, s.size()) == s; };
            std::string git;                                     // the file the last "diff --git" line names (b/'s)
            std::string ended;                                   // why the last hunk ended where it did, when no header ended it
            std::size_t i = 0;
            // " to <path>" when the line after this one is `s` and a path.
            const auto then = [&](std::string_view s) {
                return i + 1 < lines.size() && starts(lines[i + 1], s) ? " to " + diffPath(lines[i + 1].substr(s.size())) : std::string();
            };
            while (i < lines.size())
            {
                const std::string_view l = lines[i];
                if (fileHeader(lines, i))
                {
                    const std::string from = diffPath(l.substr(4)), to = diffPath(lines[i + 1].substr(4));
                    if (to == "/dev/null")
                        return "The diff deletes " + from + ": delete_file does that";
                    if (from == "/dev/null")
                        return "The diff makes a new file, " + to + ": write_file does that";
                    if (from != to)
                        return "The diff renames " + from + " to " + to + ": rename_file does that";
                    if (std::any_of(out.begin(), out.end(), [&](const FileEdits& f) { return f.name == to; }))
                        return "The diff names " + to + " twice: give its hunks under one --- and +++";
                    if (out.size() >= kMaxFiles)
                        return "The diff names more than " + std::to_string(kMaxFiles) + " files";
                    out.push_back({ to, {}, {} });
                    ended.clear();
                    i += 2;
                    continue;
                }
                int oldStart = 0;
                if (starts(l, "@@") && !hunkHeader(l, oldStart))
                    return (out.empty() ? std::string() : out.back().name + ": ") + quoted(l)
                         + " is not a hunk's header (@@ -<line>,<count> +<line>,<count> @@)";
                if (!starts(l, "@@"))
                {
                    // Between the files: git's diff, index and mode lines, or words; never a line of a change, nor git's
                    // header-only blocks (a file made or deleted empty, renamed or copied whole: no --- and +++ lines).
                    if (starts(l, "diff --git "))
                    {
                        const std::size_t b = l.rfind(" b/");
                        git = b == std::string_view::npos ? std::string() : diffPath(l.substr(b + 1));
                        ended.clear();
                    }
                    else if (starts(l, "new file mode "))
                        return "The diff makes a new file" + (git.empty() ? std::string() : ", " + git) + ": write_file does that";
                    else if (starts(l, "deleted file mode "))
                        return "The diff deletes " + (git.empty() ? std::string("a file") : git) + ": delete_file does that";
                    else if (starts(l, "rename from "))
                        return "The diff renames " + diffPath(l.substr(12)) + then("rename to ") + ": rename_file does that";
                    else if (starts(l, "copy from "))
                        return "The diff copies " + diffPath(l.substr(10)) + then("copy to ") + ": write_file makes the copy";
                    else if (!out.empty() && !l.empty() && (l.front() == ' ' || l.front() == '-' || l.front() == '+'))
                        return !ended.empty() ? ended
                                              : out.back().name + ": " + quoted(l) + " comes before the file's first @@ line";
                    ++i;
                    continue;
                }
                if (out.empty())
                    return "The diff's first hunk comes before a file's --- and +++ lines";
                FileEdits& f = out.back();
                if (f.edits.size() >= kMaxHunks)
                    return "The diff has more than " + std::to_string(kMaxHunks) + " hunks for " + f.name;
                std::vector<std::pair<char, std::string_view>> body;   // ' ', '-' or '+', and the line
                bool oldEnds = true, newEnds = true;                    // each side's last line ends with a line end
                for (++i; i < lines.size() && !fileHeader(lines, i); ++i)
                {
                    const std::string_view h = lines[i];
                    if (h.empty())
                        body.push_back({ '\0', {} });               // a blank line of context, its space lost
                    else if (h.front() == ' ' || h.front() == '-' || h.front() == '+')
                        body.push_back({ h.front(), h.substr(1) });
                    else if (h.front() == '\\')                     // "\ No newline at end of file": the line before
                    {
                        const char was = body.empty() ? '+' : body.back().first;
                        oldEnds = oldEnds && was == '+';
                        newEnds = newEnds && was == '-';
                    }
                    else
                        break;
                }
                // Ended by a line that is no header (the next hunk's, a file's, git's): a change after it is refused.
                ended = i < lines.size() && !fileHeader(lines, i) && !starts(lines[i], "@@")
                            ? f.name + " (@@ -" + std::to_string(oldStart) + "): " + quoted(lines[i])
                                  + " is not a context, - or + line (a context line starts with a space)"
                            : std::string();
                while (!body.empty() && body.back().first == '\0')
                    body.pop_back();
                Edit e;
                e.hunk = true;
                e.line = oldStart;
                std::size_t olds = 0, news = 0;
                for (const auto& [kind, text] : body)
                {
                    if (kind != '+')
                    {
                        e.oldText.append(text).push_back('\n');
                        ++olds;
                    }
                    if (kind != '-')
                    {
                        e.newText.append(text).push_back('\n');
                        ++news;
                    }
                }
                if (!oldEnds && olds > 0)
                    e.oldText.pop_back();
                if (!newEnds && news > 0)
                    e.newText.pop_back();
                if (olds + news > 0)
                    f.edits.push_back(std::move(e));
            }
            if (out.empty())
                return "The diff names no file: each file's hunks come after its --- a/<name> and +++ b/<name> lines";
            for (const FileEdits& f : out)
                if (f.edits.empty())
                    return "The diff has no hunk for " + f.name;
            return {};
        }

        // The lines a text holds (a last one without its line end counts).
        int lineCount(std::string_view t)
        {
            return static_cast<int>(std::count(t.begin(), t.end(), '\n')) + (!t.empty() && t.back() != '\n' ? 1 : 0);
        }

        // Where a hunk's old lines are in `t`: at a line's start (and at the text's end when their last line has no line
        // end), the occurrence nearest line `near` (1-based); npos when they do not occur.
        std::size_t hunkAt(const std::string& t, const std::string& old, int near)
        {
            const bool toEnd = !old.empty() && old.back() != '\n';
            std::size_t best = std::string::npos, scanned = 0;
            long bestDistance = 0;
            int line = 1;                                        // the line `scanned` is on
            for (std::size_t p = t.find(old); p != std::string::npos; p = t.find(old, p + 1))
            {
                line += static_cast<int>(std::count(t.begin() + static_cast<std::ptrdiff_t>(scanned), t.begin() + static_cast<std::ptrdiff_t>(p), '\n'));
                scanned = p;
                if ((p > 0 && t[p - 1] != '\n') || (toEnd && p + old.size() != t.size()))
                    continue;
                const long distance = std::labs(static_cast<long>(line) - static_cast<long>(near));
                if (best == std::string::npos || distance < bestDistance)
                {
                    best = p;
                    bestDistance = distance;
                }
            }
            return best;
        }

        // Where line `after` ends in `t` (0: the text's start; past its last line: its end).
        std::size_t afterLine(const std::string& t, int after)
        {
            std::size_t at = 0;
            for (int n = 0; n < after && at < t.size(); ++n)
            {
                const std::size_t end = t.find('\n', at);
                at = end == std::string::npos ? t.size() : end + 1;
            }
            return at;
        }
    }

    std::string ChatAgent::keepVersion(const std::string& name, const std::string& text, bool refused)
    {
        const std::string version = versionOf(text);
        FileVersion kept{ name, version, std::make_shared<const std::string>(text), ++versionsGiven_, !refused };
        const auto same = [&](const FileVersion& v) { return v.name == name && v.version == version; };
        if (const auto was = std::find_if(fileVersions_.begin(), fileVersions_.end(), same); refused && was != fileVersions_.end())
        {
            kept.given = was->given;                             // newest again (so kept longest), given as it was
            kept.asked = was->asked;
        }
        fileVersions_.erase(std::remove_if(fileVersions_.begin(), fileVersions_.end(), same), fileVersions_.end());
        fileVersions_.push_back(std::move(kept));
        while (fileVersions_.size() > kFileVersions)
            fileVersions_.pop_front();
        return version;
    }

    namespace
    {
        // ---- find: a text or a pattern, in one file or in every file ---------------------------------------------------
        constexpr std::uint64_t kSearchSteps = 20'000'000;   // a pattern's search's steps at most, every file's (~0.1 s)

        // The lines that hold find's text (or match its pattern, dsp/Regex.h), as read_file says them: one file's, in a
        // range of its lines, or every file's. Literal text is matched in its case first, then in any; a pattern too,
        // compiled as it is and with ignoreCase. A pattern's steps are counted across every line searched (kSearchSteps):
        // past them the search stops (`stopped`, at the line it reached).
        struct Finder
        {
            std::string want;
            bool pattern = false;
            regex::Regex exact, anyCase;
            std::uint64_t steps = kSearchSteps;
            bool stopped = false;

            // Whether line `l` holds it; false (and stopped) once the steps are spent.
            bool holds(std::string_view l, bool inAnyCase)
            {
                if (pattern)
                {
                    const regex::Found f = (inAnyCase ? anyCase : exact).search(l, steps);
                    stopped = stopped || f == regex::Found::spent;
                    return f == regex::Found::yes;
                }
                if (!inAnyCase)
                    return l.find(want) != std::string_view::npos;
                const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
                std::string a(l), b(want);
                std::transform(a.begin(), a.end(), a.begin(), lower);
                std::transform(b.begin(), b.end(), b.begin(), lower);
                return a.find(b) != std::string::npos;
            }

            // The lines from..to (1-based) that hold it, onto `found`; the line it stopped at, or 0.
            int scan(const std::vector<std::string_view>& lines, int from, int to, bool inAnyCase, std::vector<int>& found)
            {
                for (int n = from; n <= to; ++n)
                {
                    if (holds(lines[static_cast<std::size_t>(n - 1)], inAnyCase))
                        found.push_back(n);
                    if (stopped)
                        return n;
                }
                return 0;
            }

            std::string quoted() const { return "\"" + want + "\""; }
            std::string verb(std::size_t n) const { return pattern ? (n == 1 ? "matches" : "match") : (n == 1 ? "holds" : "hold"); }
            std::string narrower() const { return pattern ? "a narrower pattern" : "a longer text"; }
            // Where the steps ran out, and what searches the rest (`named`: the search was of one file).
            std::string stoppedNote(const std::string& where, bool named) const
            {
                return "the search stopped at " + where + ": this pattern takes more than " + std::to_string(kSearchSteps / 1000000)
                     + " million steps over so much text; a simpler one, or " + (named ? "" : "a file's name with ")
                     + "from and lines, searches the rest";
            }
        };

        // Up to 12 of the line numbers `found` holds, of its first `shown`; ", ..." after them when there are more.
        std::string someNumbers(const std::vector<int>& found, std::size_t shown)
        {
            std::string numbers;
            for (std::size_t i = 0; i < shown && i < 12; ++i)
                numbers += (i == 0 ? "" : ", ") + std::to_string(found[i]);
            if (shown > 12)
                numbers += ", ...";
            return numbers;
        }

        // The first `shown` of the lines `found` holds (1-based, ascending), kContext lines around each, numbered, groups
        // apart ("--"), onto `out`; false when kMaxOutput cut it. A line past the room while under half of it is used
        // is shown cut (cutLongLine), `longCut` set: its note says how to read it all.
        bool aroundEach(const std::vector<std::string_view>& lines, const std::vector<int>& found, std::size_t shown, std::string& out,
                        bool& longCut)
        {
            const int count = static_cast<int>(lines.size());
            int last = 0;                                        // the last line written
            for (std::size_t i = 0; i < shown; ++i)
            {
                const int a = std::max(1, found[i] - kContext), b = std::min(count, found[i] + kContext);
                if (last > 0 && a > last + 1)
                    out += "--\n";
                for (int n = std::max(a, last + 1); n <= b; ++n)
                {
                    const std::string l = numberedLine(n, lines[static_cast<std::size_t>(n - 1)]);
                    if (out.size() + l.size() > kMaxOutput)
                    {
                        if (out.size() < kMaxOutput / 2)
                        {
                            out += cutLongLine(n, lines[static_cast<std::size_t>(n - 1)], kMaxOutput - out.size());
                            longCut = true;
                        }
                        return false;
                    }
                    out += l;
                }
                last = std::max(last, b);
            }
            return true;
        }

        // find with no name: every file's lines that hold it, file by file in the tabs' order (in its case in every file
        // first, then in any).
        struct FileHits
        {
            const SourceFile* file = nullptr;
            std::vector<std::string_view> lines;
            std::vector<int> found;
        };
        struct Everywhere
        {
            std::vector<FileHits> hits;                          // the files with a line that holds it
            bool anyCase = false;
            std::size_t without = 0;                             // the files searched to their end with no line that does
            std::string stoppedAt;                               // where the steps ran out ("engine.fm's line 1200"), or ""
            std::string unsearched;                              // then what it never searched ("engine.fm from line 1200, x.fm")
            std::string unreached;                               // and the files after the one it stopped in ("x.fm"), or ""
        };

        Everywhere findInEvery(const Draft& d, Finder& finder)
        {
            Everywhere e;
            for (const bool c : { false, true })
            {
                e.hits.clear();
                e.without = 0;
                for (std::size_t i = 0; i < d.files.size(); ++i)
                {
                    const SourceFile& f = d.files[i];
                    FileHits h{ &f, linesOf(f.text), {} };
                    const int at = finder.scan(h.lines, 1, static_cast<int>(h.lines.size()), c, h.found);
                    if (at == 0 && h.found.empty())
                        ++e.without;
                    if (!h.found.empty())
                        e.hits.push_back(std::move(h));
                    if (at > 0)
                    {
                        // Its line `at` and the rest of the file unsearched, and every file after it.
                        e.stoppedAt = f.name + "'s line " + std::to_string(at);
                        for (std::size_t k = i + 1; k < d.files.size(); ++k)
                            e.unreached += (e.unreached.empty() ? "" : ", ") + d.files[k].name;
                        e.unsearched = f.name + " from line " + std::to_string(at) + (e.unreached.empty() ? "" : ", " + e.unreached);
                        break;
                    }
                }
                if (!e.hits.empty() || !e.stoppedAt.empty())
                {
                    e.anyCase = c;
                    break;
                }
            }
            return e;
        }

        // Its words: how many lines in each file and in how many others none (when the steps ran out: of the files it
        // searched to their end, and what it never searched), then each file's (its line count and version, `versions`,
        // as a read says them) with kContext lines around each, at most kMaxFound lines shown in all.
        std::string everywhereText(const Draft& d, const Finder& finder, const Everywhere& e, const std::vector<std::string>& versions)
        {
            const std::size_t fileCount = d.files.size();
            const bool cut = !e.stoppedAt.empty();
            const std::string stopped = cut ? finder.stoppedNote(e.stoppedAt, false) : std::string();
            if (e.hits.empty())
                return (cut ? "No line found yet: "
                                  + finder.stoppedNote(e.stoppedAt + (e.unreached.empty() ? "" : " (" + e.unreached + " not searched"
                                                                      + (e.anyCase ? " in another case)" : ")")), false)
                            : "No line of " + (fileCount == 1 ? d.files.front().name : "the " + std::to_string(fileCount) + " files")
                                  + " " + finder.verb(1) + " " + finder.quoted())
                     + ".\n";
            std::size_t total = 0;
            std::string each;
            for (const FileHits& h : e.hits)
            {
                total += h.found.size();
                each += (each.empty() ? "" : ", ") + h.file->name + " " + std::to_string(h.found.size());
            }
            if (e.without > 0)
                each += "; none in the other " + (e.without == 1 ? std::string("file") : std::to_string(e.without) + " files")
                      + (cut ? " searched" : "");
            if (cut)
                each += std::string("; not searched") + (e.anyCase ? " in another case: " : ": ") + e.unsearched;
            std::string out = std::to_string(total) + (total == 1 ? " line " : " lines ") + finder.verb(total) + " " + finder.quoted()
                            + (e.anyCase ? " (in another case)" : "") + " (" + each + "), with " + std::to_string(kContext)
                            + " lines around each:\n";
            std::size_t room = kMaxFound;                        // lines still to show
            for (std::size_t k = 0; k < e.hits.size() && room > 0; ++k)
            {
                const FileHits& h = e.hits[k];
                const std::size_t shown = std::min(h.found.size(), room);
                const int count = static_cast<int>(h.lines.size());
                out += h.file->name + ": " + std::to_string(count) + (count == 1 ? " line" : " lines") + ", version " + versions[k]
                     + (h.found.size() == 1 ? "; line " : "; lines ") + someNumbers(h.found, shown) + ":\n";
                bool longCut = false;
                if (out.size() > kMaxOutput || !aroundEach(h.lines, h.found, shown, out, longCut))
                    return longCut ? out : out + "(cut here: give find " + finder.narrower() + ", or a file's name with from and lines)\n";
                room -= shown;
            }
            if (total > kMaxFound)
                out += "(and " + std::to_string(total - kMaxFound) + " more lines " + finder.verb(2) + " it: give find "
                     + finder.narrower() + ", or a file's name)\n";
            if (!stopped.empty())
                out += "(" + stopped + ")\n";
            return out;
        }
    }

    std::string ChatAgent::readFileText(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        const juce::var nameVar = input.getProperty("name", juce::var()), fromVar = input.getProperty("from", juce::var()),
                        linesVar = input.getProperty("lines", juce::var()), findVar = input.getProperty("find", juce::var());
        const bool finding = findVar.isString() && findVar.toString().isNotEmpty();
        // find with no name: every file (a name of "" is none).
        const bool everyFile = finding && (nameVar.isVoid() || nameVar.isUndefined() || (nameVar.isString() && nameVar.toString().isEmpty()));
        if (!nameVar.isString() && !everyFile)
            return fail("read_file needs a name (or find alone, to search every file)");
        Finder finder;
        if (finding)
        {
            finder.want = findVar.toString().toStdString();
            finder.pattern = static_cast<bool>(input.getProperty("regex", false));
            if (finder.pattern)
            {
                finder.exact = regex::Regex::compile(finder.want);
                if (!finder.exact.ok())
                    return fail("find " + finder.quoted() + " is not a regex this search takes: " + finder.exact.error()
                                + ". (Without regex, find looks for its text as it is.)");
                finder.anyCase = regex::Regex::compile(finder.want, true);
            }
        }
        const Draft d = host_.chatDraft();
        if (everyFile)
        {
            if (isNumber(fromVar) || isNumber(linesVar))
                return fail("from and lines read one file: give its name too");
            setActivity("Searching the files");
            const Everywhere e = findInEvery(d, finder);
            std::vector<std::string> versions;                   // each file's with a line found, kept (edit_file's expected)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                for (const FileHits& h : e.hits)
                    versions.push_back(keepVersion(h.file->name, h.file->text));
            }
            return everywhereText(d, finder, e, versions);       // a part read: nothing marked seen
        }

        const std::string file = nameVar.toString().toStdString();
        const auto f = std::find_if(d.files.begin(), d.files.end(), [&](const SourceFile& s) { return s.name == file; });
        if (f == d.files.end())
        {
            std::string names;
            for (const SourceFile& s : d.files)
                names += (names.empty() ? "" : ", ") + s.name;
            return fail("There is no file " + file + ". The files: " + names);
        }
        setActivity("Reading " + file);
        const std::string& t = f->text;
        const std::vector<std::string_view> lines = linesOf(t);
        const int count = static_cast<int>(lines.size());
        const bool part = isNumber(fromVar) || isNumber(linesVar) || finding;
        std::string version;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            version = keepVersion(file, t);
            if (!part)
                markSeen(file, t);                               // a part read is not the file seen
        }
        const std::string head = file + ": " + std::to_string(count) + (count == 1 ? " line" : " lines") + ", version " + version;
        if (!part)
            return head + ".\n" + t;
        if (count == 0)
            return head + ": it is empty.\n";

        // The range (all of it unless from or lines say), then the lines in it that hold `find`. The numbers are
        // rounded and checked as doubles, so one past an int's range is refused or clamped, never cast.
        int from = 1, to = count;
        if (isNumber(fromVar))
        {
            const double first = std::round(static_cast<double>(fromVar));
            if (!(first >= 1.0 && first <= static_cast<double>(count)))
                return fail(file + " has " + std::to_string(count) + (count == 1 ? " line" : " lines") + ": from is 1 to "
                            + std::to_string(count));
            from = static_cast<int>(first);
        }
        if (isNumber(linesVar))
        {
            const double n = std::round(static_cast<double>(linesVar));
            if (!(n >= 1.0))
                return fail("lines is how many lines to read, 1 or more");
            to = static_cast<int>(std::min(static_cast<double>(count), static_cast<double>(from) + n - 1.0));
        }
        if (!finding)
        {
            std::string out = head + "; lines " + std::to_string(from) + " to " + std::to_string(to) + ":\n";
            for (int n = from; n <= to; ++n)
            {
                const std::string l = numberedLine(n, lines[static_cast<std::size_t>(n - 1)]);
                if (out.size() + l.size() > kMaxOutput)
                    return out.size() < kMaxOutput / 2
                               ? out + cutLongLine(n, lines[static_cast<std::size_t>(n - 1)], kMaxOutput - out.size())
                                     + (n < to ? "(read from " + std::to_string(n + 1) + " for the lines after it)\n" : "")
                               : out + "(cut here: read from " + std::to_string(n) + " for the rest)\n";
                out += l;
            }
            return out;
        }
        std::vector<int> found;
        bool anyCase = false;
        int stoppedAt = 0;
        for (const bool c : { false, true })
        {
            stoppedAt = finder.scan(lines, from, to, c, found);
            if (!found.empty() || stoppedAt > 0)
            {
                anyCase = c;
                break;
            }
        }
        const std::string range = from == 1 && to == count ? std::string() : " in lines " + std::to_string(from) + " to " + std::to_string(to);
        const std::string stopped = stoppedAt > 0 ? finder.stoppedNote("line " + std::to_string(stoppedAt), true) : std::string();
        if (found.empty())
            return head + "; " + (stopped.empty() ? "no line" + range + " " + finder.verb(1) + " " + finder.quoted() : stopped) + ".\n";
        const std::size_t shownCount = std::min(found.size(), kMaxFound);
        std::string out = head + "; " + std::to_string(found.size()) + (found.size() == 1 ? " line" : " lines") + range + " "
                        + finder.verb(found.size()) + " " + finder.quoted() + (anyCase ? " (in another case)" : "") + " ("
                        + someNumbers(found, shownCount) + "), with " + std::to_string(kContext) + " lines around each:\n";
        bool longCut = false;
        if (!aroundEach(lines, found, shownCount, out, longCut))
            return longCut ? out : out + "(cut here: give find " + finder.narrower() + ", or from and lines)\n";
        if (found.size() > shownCount)
            out += "(and " + std::to_string(found.size() - shownCount) + " more lines " + finder.verb(2) + " it: give find "
                 + finder.narrower() + ")\n";
        if (!stopped.empty())
            out += "(" + stopped + ")\n";
        return out;
    }

    std::string ChatAgent::editFileText(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        // The files and their edits: one file (name, and old_text and new_text or edits, and expected), or several
        // (files: a list of those, or a unified diff), all or none.
        std::vector<FileEdits> files;
        juce::var filesVar = input.getProperty("files", juce::var());
        const bool many = !filesVar.isVoid() && !filesVar.isUndefined();
        const auto expectedOf = [](const juce::var& holder, std::string& out) {
            const juce::var v = holder.getProperty("expected", juce::var());
            out = v.isString() ? v.toString().trim().toStdString() : std::string();
            return v.isVoid() || v.isUndefined() || !out.empty();
        };
        if (many)
        {
            for (const char* k : { "name", "old_text", "new_text", "edits", "expected" })
                if (input.hasProperty(k))
                    return fail("Give files, or name and its edits, not both. Nothing was changed.");
            // The list written as a string, its JSON (an input streamed eagerly is not checked against the schema): read
            // as the list. A diff never starts with [.
            if (filesVar.isString() && filesVar.toString().trimStart().startsWithChar('['))
            {
                const juce::var list = juce::JSON::parse(filesVar.toString());
                if (!list.isArray())
                    return fail("files is a list of files or a unified diff: this string starts with [ but is no JSON list. Nothing "
                                "was changed.");
                filesVar = list;
            }
            if (filesVar.isString())
            {
                if (const std::string why = parseDiff(filesVar.toString().toStdString(), files); !why.empty())
                    return fail(why + ". Nothing was changed.");
            }
            else
            {
                const juce::Array<juce::var>* arr = filesVar.getArray();
                if (arr == nullptr || arr->isEmpty() || arr->size() > static_cast<int>(kMaxFiles))
                    return fail("files is a list of 1 to " + std::to_string(kMaxFiles) + " files, [{name, edits, expected}], or a unified "
                                "diff. Nothing was changed.");
                for (int i = 0; i < arr->size(); ++i)
                {
                    const juce::var& e = (*arr)[i];
                    const juce::var n = e.getProperty("name", juce::var());
                    if (!n.isString() || n.toString().isEmpty())
                        return fail("File " + std::to_string(i + 1) + " of files needs a name. Nothing was changed.");
                    FileEdits f{ n.toString().toStdString(), {}, {} };
                    if (std::any_of(files.begin(), files.end(), [&](const FileEdits& g) { return g.name == f.name; }))
                        return fail(f.name + " is named twice in files: give its edits in one entry. Nothing was changed.");
                    if (const std::string why = editsOf(e, f.edits, "needs old_text and new_text, or edits"); !why.empty())
                        return fail(f.name + ": " + why + (why.find("Nothing was changed") == std::string::npos ? ". Nothing was changed." : ""));
                    if (!expectedOf(e, f.expected))
                        return fail(f.name + ": expected is the version read_file gave (8 hex digits). Nothing was changed.");
                    files.push_back(std::move(f));
                }
            }
        }
        else
        {
            const juce::var nameVar = input.getProperty("name", juce::var());
            if (!nameVar.isString())
                return fail("edit_file needs name, and old_text and new_text (or edits), or files");
            FileEdits f{ nameVar.toString().toStdString(), {}, {} };
            if (const std::string why = editsOf(input, f.edits, "edit_file needs name, and old_text and new_text (or edits)"); !why.empty())
                return fail(why);
            if (!expectedOf(input, f.expected))
                return fail("expected is the version read_file gave (8 hex digits)");
            files.push_back(std::move(f));
        }
        // What an edit's error starts with: nothing for one edit of one file, "Edit 2 of 3: " for one file's list; with
        // files, the file's name, and a hunk's line.
        const auto label = [&](const FileEdits& f, std::size_t i) {
            const Edit& e = f.edits[i];
            const std::string what = e.hunk ? "hunk" : "edit";
            const std::string place = e.hunk ? " (@@ -" + std::to_string(e.line) + ")" : std::string();
            if (!many)
                return f.edits.size() == 1 ? std::string() : "Edit " + std::to_string(i + 1) + " of " + std::to_string(f.edits.size()) + ": ";
            return f.name + (f.edits.size() == 1 ? place + ": "
                                                 : ", " + what + " " + std::to_string(i + 1) + " of " + std::to_string(f.edits.size()) + place + ": ");
        };

        // The text changed since `version` (the file's lines that changed, against the kept text), refused: the
        // user's edit, unless the text now is a version one of Claude's calls gave (a read, a write, edit_circuit) after
        // it gave `version`. A text Claude saw before `version`, which the user's undo brought back, is the user's; so
        // is one only a refusal showed (this one keeps it, as given as it was: a second refusal says the same).
        const auto changedSince = [&](const std::string& file, const std::string& version, const std::string& now) {
            std::shared_ptr<const std::string> then;
            std::string nowVersion;
            bool known = false;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                const std::string current = versionOf(now);
                uint64_t thenGiven = 0;
                const FileVersion* is = nullptr;
                for (const FileVersion& v : fileVersions_)
                {
                    if (v.name == file && v.version == version)
                    {
                        then = v.text;
                        thenGiven = v.given;
                    }
                    if (v.name == file && v.version == current)
                        is = &v;
                }
                known = is != nullptr && is->asked && is->given > thenGiven;
                nowVersion = keepVersion(file, now, true);
            }
            std::string out = file + " changed since version " + version
                            + (known ? " (now version " + nowVersion + ", which one of your own calls gave)" : " (the user's edit, which is kept)")
                            + ": nothing was written" + (many ? " to any file. " : ". ");
            if (!then)
                return fail(out + "That version's text is no longer kept, so read_file shows the file now (version " + nowVersion + ").");
            const textdiff::Unified u = textdiff::unified(*then, now, 2, kChangedLines);
            return fail(out + "Its lines changed since (+" + std::to_string(u.added) + " -" + std::to_string(u.removed) + "):\n" + u.text
                        + (u.cut ? "(cut here: read_file shows the rest)\n" : "") + "Its version now: " + nowVersion
                        + ". Edit again from this text.");
        };

        // One file's edits on its text, in order, each on the text the ones before it left: "" and the text changed, or
        // why not.
        const auto apply = [&](const FileEdits& f, std::string& t) -> std::string {
            int moved = 0;                                       // the lines the hunks before this one added, less removed
            for (std::size_t i = 0; i < f.edits.size(); ++i)
            {
                const Edit& e = f.edits[i];
                if (e.hunk)
                {
                    const int near = e.line + moved;
                    std::size_t at = 0;
                    if (e.oldText.empty())
                    {
                        at = afterLine(t, near);
                        if (at == t.size() && !t.empty() && t.back() != '\n')
                        {
                            t.push_back('\n');                   // after a last line with no line end
                            at = t.size();
                        }
                    }
                    else if ((at = hunkAt(t, e.oldText, near)) == std::string::npos)
                    {
                        const std::string closer = closest(t, e.oldText);
                        return label(f, i) + "its old lines (context and -) do not occur in " + f.name
                             + (i > 0 ? " (as the hunks before it leave it)" : "") + ". "
                             + (closer.empty() ? std::string("Nothing in it is close: read_file shows its text.\n") : closer)
                             + "Nothing was changed.";
                    }
                    t.replace(at, e.oldText.size(), e.newText);
                    moved += lineCount(e.newText) - lineCount(e.oldText);
                    continue;
                }
                if (e.oldText.empty())
                    return label(f, i) + "old_text is empty: give the exact passage to replace. Nothing was changed.";
                std::size_t count = 0;
                std::vector<std::string> where;
                const std::size_t at = occurs(t, e.oldText, count, where);
                if (count == 0)
                {
                    const std::string near = closest(t, e.oldText);
                    return label(f, i) + "old_text does not occur in " + f.name + (i > 0 ? " (as the edits before it leave it)" : "") + ". "
                         + (near.empty() ? std::string("Nothing in it is close: read_file shows its text.\n") : near) + "Nothing was changed.";
                }
                if (count > 1)
                    return label(f, i) + "old_text occurs " + std::to_string(count) + " times in " + f.name + " (lines " + joinedAnd(where)
                         + (count > where.size() ? ", ..." : "") + "): include more of the surrounding lines so it occurs once. "
                           "Nothing was changed.";
                t.replace(at, e.oldText.size(), e.newText);
            }
            return {};
        };

        std::vector<std::string> names;
        std::size_t total = 0;
        for (const FileEdits& f : files)
        {
            names.push_back(f.name);
            total += f.edits.size();
        }
        setActivity("Editing " + (files.size() <= 2 ? joinedAnd(names) : std::to_string(files.size()) + " files"));
        const auto fileNames = [](const Draft& d) {
            std::string out;
            for (const SourceFile& s : d.files)
                out += (out.empty() ? "" : ", ") + s.name;
            return out;
        };
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            Draft d = host_.chatDraft();
            Sources read;                                        // each file as the edits found it
            for (const FileEdits& f : files)
            {
                const SourceFile* s = findFile(d.files, f.name);
                if (s == nullptr)
                    return fail("There is no file " + f.name + ". The files: " + fileNames(d) + (many ? ". Nothing was changed." : ""));
                read.push_back(*s);
            }
            // Compared, not kept: keeping it first could push out the very version it was given (changedSince looks
            // that up, then keeps the text now).
            for (std::size_t k = 0; k < files.size(); ++k)
                if (!files[k].expected.empty() && versionOf(read[k].text) != files[k].expected)
                    return changedSince(files[k].name, files[k].expected, read[k].text);
            // All or none: every file's edits on a copy first.
            std::vector<std::string> after, held;
            for (std::size_t k = 0; k < files.size(); ++k)
            {
                after.push_back(read[k].text);
                if (const std::string why = apply(files[k], after.back()); !why.empty())
                    return fail(why);
                held.push_back(asEditorHolds(after.back()));     // as the editor holds it (ChatAgent.h)
            }
            for (SourceFile& s : d.files)
                for (std::size_t k = 0; k < files.size(); ++k)
                    if (s.name == files[k].name)
                        s.text = after[k];
            d.active = static_cast<int>(findFile(d.files, files.front().name) - d.files.data());
            if (fileWriteHook_)
                fileWriteHook_(attempt);
            if (!host_.chatSetDraftIf(d, read))
            {
                // The user typed in one of them between the read and the write: their text stays. A file with
                // expected says so at once; else the edits are tried once more on the new text.
                const Draft now = host_.chatDraft();
                std::vector<std::string> moved;
                for (std::size_t k = 0; k < files.size(); ++k)
                {
                    const SourceFile* g = findFile(now.files, files[k].name);
                    if (g != nullptr && g->text == read[k].text)
                        continue;
                    if (!files[k].expected.empty() && g != nullptr)
                        return changedSince(files[k].name, files[k].expected, g->text);
                    moved.push_back(files[k].name);
                }
                if (attempt == 1)
                    return fail(joinedAnd(moved.empty() ? names : moved) + " changed while editing (the user's typing, which is kept): try "
                                "again." + (many ? " Nothing was changed." : ""));
                continue;
            }
            std::vector<std::string> versions;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                for (std::size_t k = 0; k < files.size(); ++k)
                {
                    carrySeen(files[k].name, read[k].text, after[k]);   // seen only if what it edited was
                    versions.push_back(keepVersion(files[k].name, after[k]));
                }
                addEntry(ChatEntry::Kind::tool, "edited " + joinedAnd(names)
                                                    + (total > 1 ? " (" + std::to_string(total) + " changes)" : ""));
            }
            const auto lines = [&](std::size_t k) {
                const std::size_t n = linesOf(after[k]).size();
                return std::to_string(n) + (n == 1 ? " line" : " lines");
            };
            if (!many)
                return "Edited " + names[0] + (total > 1 ? ": " + std::to_string(total) + " edits, one step the user can undo" : "") + "; now "
                     + lines(0) + ", version " + versions[0] + (held[0].empty() ? "" : "; " + held[0]) + ".";
            std::string out = "Edited " + std::to_string(files.size()) + (files.size() == 1 ? " file" : " files")
                            + ", one step the user can undo:\n";
            for (std::size_t k = 0; k < files.size(); ++k)
            {
                const std::size_t n = files[k].edits.size();
                const std::string what = files[k].edits.front().hunk ? (n == 1 ? " hunk" : " hunks") : (n == 1 ? " edit" : " edits");
                out += "- " + files[k].name + ": " + std::to_string(n) + what + "; now " + lines(k) + ", version " + versions[k]
                     + (held[k].empty() ? "" : "; " + held[k]) + "\n";
            }
            return out;
        }
        return fail(joinedAnd(names) + " changed while editing (the user's typing, which is kept): try again.");
    }
}
