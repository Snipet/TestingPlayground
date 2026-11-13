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
// read_file's find shows `context` lines around each (0 to 10, 2 unless said), or with `count` only how many, and with
// `skip` passes over that many, its last line saying what is left and the skip that reads on. edit_file's find
// (replaceText) replaces every match of a text or a pattern (Regex::find: where, never a group) by new_text as it is,
// line by line in find's own case, never a netlist's *@ line, in one file or every file: all or none, at most
// kMaxReplaced, in one chatSetDraftIf of every file it read.
#include "plugin/ChatAgent.h"

#include "dsp/Regex.h"
#include "dsp/TextDiff.h"
#include "fminus/CirDoc.h"

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
        constexpr int kContext = 2;                        // find: lines around each, unless context says (0 to kMaxContext)
        constexpr int kMaxContext = 10;
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
        std::string closest(std::string_view text, std::string_view oldText, double* scoreOut = nullptr)
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
            if (scoreOut != nullptr)
                *scoreOut = best / static_cast<double>(scored.size());
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

        // Up to 12 of the line numbers `found` holds, of the `shown` from its `first`; ", ..." after them when there are
        // more.
        std::string someNumbers(const std::vector<int>& found, std::size_t first, std::size_t shown)
        {
            std::string numbers;
            for (std::size_t i = 0; i < shown && i < 12; ++i)
                numbers += (i == 0 ? "" : ", ") + std::to_string(found[first + i]);
            if (shown > 12)
                numbers += ", ...";
            return numbers;
        }

        // ", with 2 lines around each" (", the lines alone" with none), as a find's head says it.
        std::string aroundWords(int context)
        {
            return context == 0 ? std::string(", the lines alone") : ", with " + std::to_string(context) + (context == 1 ? " line" : " lines") + " around each";
        }

        // The `shown` of the lines `found` holds (1-based, ascending) from its `first`, `context` lines around each,
        // numbered, groups apart ("--"), onto `out`: how many of them it wrote whole (`shown` unless kMaxOutput cut it).
        std::size_t aroundEach(const std::vector<std::string_view>& lines, const std::vector<int>& found, std::size_t first, std::size_t shown,
                               int context, std::string& out)
        {
            const int count = static_cast<int>(lines.size());
            int last = 0;                                        // the last line written
            for (std::size_t i = 0; i < shown; ++i)
            {
                const int hit = found[first + i];
                const int a = std::max(1, hit - context), b = std::min(count, hit + context);
                if (last > 0 && a > last + 1)
                    out += "--\n";
                for (int n = std::max(a, last + 1); n <= b; ++n)
                {
                    const std::string l = numberedLine(n, lines[static_cast<std::size_t>(n - 1)]);
                    if (out.size() + l.size() > kMaxOutput)
                        return i;
                    out += l;
                }
                last = std::max(last, b);
            }
            return shown;
        }

        // How a find's reading goes on: its context, whether it counts alone, and the hits it passes over first.
        struct Reading
        {
            int context = kContext;
            bool count = false;
            std::size_t skip = 0;
        };

        // The last line of a find that showed hits up to `next` (their index) of `total`: what is left, and how to read on.
        std::string readOn(std::size_t next, std::size_t total, const std::string& narrower)
        {
            return "(" + std::to_string(total - next) + " more: call again with skip=" + std::to_string(next) + ", or give find " + narrower + ")\n";
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
        std::string everywhereText(const Draft& d, const Finder& finder, const Everywhere& e, const std::vector<std::string>& versions,
                                   const Reading& r)
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
                            + (e.anyCase ? " (in another case)" : "") + " (" + each + ")";
            if (r.count)
                return out + ".\n" + (stopped.empty() ? "" : "(" + stopped + ")\n");
            if (r.skip >= total)
                return out + ": skip=" + std::to_string(r.skip) + " passes over " + (total == 1 ? "it" : "all of them") + ".\n"
                     + (stopped.empty() ? "" : "(" + stopped + ")\n");
            out += aroundWords(r.context) + (r.skip > 0 ? ", after the first " + std::to_string(r.skip) : std::string()) + ":\n";
            std::size_t passed = r.skip;                         // hits still to pass over
            std::size_t room = kMaxFound;                        // lines still to show
            std::size_t next = 0;                                // the index of the hit after the last one shown
            for (std::size_t k = 0; k < e.hits.size() && room > 0; ++k)
            {
                const FileHits& h = e.hits[k];
                if (passed >= h.found.size())
                {
                    passed -= h.found.size();
                    next += h.found.size();
                    continue;
                }
                const std::size_t first = passed, shown = std::min(h.found.size() - first, room);
                passed = 0;
                next += first;
                const int count = static_cast<int>(h.lines.size());
                out += h.file->name + ": " + std::to_string(count) + (count == 1 ? " line" : " lines") + ", version " + versions[k]
                     + (shown == 1 ? "; line " : "; lines ") + someNumbers(h.found, first, shown) + ":\n";
                const std::size_t written = out.size() > kMaxOutput ? 0 : aroundEach(h.lines, h.found, first, shown, r.context, out);
                next += written;
                if (written < shown)
                    return out + "(cut here: call again with skip=" + std::to_string(next) + ", or give find " + finder.narrower()
                         + ", or a file's name with from and lines)\n";
                room -= shown;
            }
            if (total > next)
                out += readOn(next, total, finder.narrower() + ", or a file's name");
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
        // context, count and skip: how a find reads (each checked as a double, so one past an int's range is refused).
        Reading reading;
        {
            const juce::var contextVar = input.getProperty("context", juce::var()), countVar = input.getProperty("count", juce::var()),
                            skipVar = input.getProperty("skip", juce::var());
            const auto given = [](const juce::var& v) { return !v.isVoid() && !v.isUndefined(); };
            if (!finding && (given(contextVar) || given(countVar) || given(skipVar)))
                return fail("context, count and skip go with find");
            if (given(contextVar))
            {
                const double c = isNumber(contextVar) ? static_cast<double>(contextVar) : -1.0;
                if (!(c >= 0.0 && c <= kMaxContext && c == std::floor(c)))
                    return fail("context is how many lines around each, 0 to " + std::to_string(kMaxContext));
                reading.context = static_cast<int>(c);
            }
            if (given(countVar))
            {
                if (!countVar.isBool())
                    return fail("count is true or false");
                reading.count = static_cast<bool>(countVar);
            }
            if (given(skipVar))
            {
                const double k = isNumber(skipVar) ? static_cast<double>(skipVar) : -1.0;
                if (!(k >= 0.0 && k <= 1.0e9 && k == std::floor(k)))
                    return fail("skip is how many of the lines found to pass over, 0 or more");
                reading.skip = static_cast<std::size_t>(k);
            }
        }
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
            return everywhereText(d, finder, e, versions, reading);   // a part read: nothing marked seen
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
                    return out + "(cut here: read from " + std::to_string(n) + " for the rest)\n";
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
        std::string out = head + "; " + std::to_string(found.size()) + (found.size() == 1 ? " line" : " lines") + range + " "
                        + finder.verb(found.size()) + " " + finder.quoted() + (anyCase ? " (in another case)" : "");
        const auto withStop = [&](std::string text) { return stopped.empty() ? text : text + "(" + stopped + ")\n"; };
        if (reading.count)
            return withStop(out + ".\n");
        if (reading.skip >= found.size())
            return withStop(out + ": skip=" + std::to_string(reading.skip) + " passes over " + (found.size() == 1 ? "it" : "all of them") + ".\n");
        const std::size_t first = reading.skip, shownCount = std::min(found.size() - first, kMaxFound);
        out += " (" + someNumbers(found, first, shownCount) + ")" + aroundWords(reading.context)
             + (first > 0 ? ", after the first " + std::to_string(first) : std::string()) + ":\n";
        if (const std::size_t written = aroundEach(lines, found, first, shownCount, reading.context, out); written < shownCount)
            return out + "(cut here: call again with skip=" + std::to_string(first + written) + ", or give find " + finder.narrower()
                 + ", or from and lines)\n";
        if (found.size() > first + shownCount)
            out += readOn(first + shownCount, found.size(), finder.narrower());
        if (!stopped.empty())
            out += "(" + stopped + ")\n";
        return out;
    }

    // The text changed since `version` (the file's lines that changed, against the kept text), refused: the user's edit,
    // unless the text now is a version one of Claude's calls gave (a read, a write, edit_circuit) after it gave `version`.
    // A text Claude saw before `version`, which the user's undo brought back, is the user's; so is one only a refusal
    // showed (this one keeps it, as given as it was: a second refusal says the same).
    std::string ChatAgent::changedSinceText(const std::string& file, const std::string& version, const std::string& now, bool many)
    {
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
            return out + "That version's text is no longer kept, so read_file shows the file now (version " + nowVersion + ").";
        const textdiff::Unified u = textdiff::unified(*then, now, 2, kChangedLines);
        return out + "Its lines changed since (+" + std::to_string(u.added) + " -" + std::to_string(u.removed) + "):\n" + u.text
             + (u.cut ? "(cut here: read_file shows the rest)\n" : "") + "Its version now: " + nowVersion + ". Edit again from this text.";
    }

    std::string ChatAgent::editFileText(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        if (input.hasProperty("find"))
            return replaceText(input, isError);
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

        const auto changedSince = [&](const std::string& file, const std::string& version, const std::string& now) {
            return fail(changedSinceText(file, version, now, many));
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

    namespace
    {
        // ---- find in edit_file: every match replaced -------------------------------------------------------------------
        constexpr std::size_t kMaxReplaced = 2000;           // matches replaced a call at most
        constexpr std::size_t kChangedShown = 20;            // the result's changed lines shown at most

        // One file's text with every match replaced: the text after, how many, and where (each changed line's first line
        // in the new text and how many lines it became: new_text may hold line ends).
        struct Replacing
        {
            std::string text;
            std::size_t count = 0;
            std::vector<std::pair<int, int>> changed;
            int stoppedAt = 0;                               // the line where a pattern's steps ran out (0: they did not)
        };

        // Every match of `want` (or of the pattern `re`), line by line in its own case, replaced by `with` (as it is), the
        // matches apart and left to right; a match of nothing passed over; a netlist's *@ lines (`circuit`) left as they
        // are. It stops once the count passes `limit`, or the steps run out (stoppedAt).
        Replacing replaceIn(const std::string& t, bool circuit, const std::string& want, regex::Regex* re, const std::string& with,
                            std::uint64_t& steps, std::size_t limit)
        {
            Replacing r;
            int withLines = 0;                                   // its line breaks as the editor reads them: CR LF, CR or LF
            for (std::size_t k = 0; k < with.size(); ++k)
                if (with[k] == '\n' || (with[k] == '\r' && (k + 1 == with.size() || with[k + 1] != '\n')))
                    ++withLines;
            int line = 0, newLine = 1;                           // this line's number, before and after
            for (std::size_t at = 0; at < t.size();)
            {
                ++line;
                const std::size_t nl = t.find('\n', at);
                const std::size_t endAll = nl == std::string::npos ? t.size() : nl + 1;
                std::size_t endText = nl == std::string::npos ? t.size() : nl;
                if (endText > at && t[endText - 1] == '\r')
                    --endText;
                const std::string_view l(t.data() + at, endText - at);
                std::string out;
                std::size_t n = 0;
                if (!(circuit && cir::isLayoutLine(l)))
                {
                    std::size_t p = 0, copied = 0;
                    while (p <= l.size())
                    {
                        std::size_t b = 0, e = 0;
                        if (re != nullptr)
                        {
                            const regex::Found f = re->find(l, p, steps, b, e);
                            if (f == regex::Found::spent)
                            {
                                r.stoppedAt = line;
                                return r;
                            }
                            if (f == regex::Found::no)
                                break;
                        }
                        else
                        {
                            b = l.find(want, p);
                            if (b == std::string_view::npos)
                                break;
                            e = b + want.size();
                        }
                        if (e == b)
                        {
                            // A match of nothing: on past the character there.
                            p = b + 1;
                            while (p < l.size() && (static_cast<unsigned char>(l[p]) & 0xC0) == 0x80)
                                ++p;
                            continue;
                        }
                        out.append(l.substr(copied, b - copied)).append(with);
                        copied = p = e;
                        if (r.count + ++n > limit)
                        {
                            r.count += n;
                            return r;
                        }
                    }
                    if (n > 0)
                        out.append(l.substr(copied));
                }
                if (n > 0)
                {
                    r.text += out;
                    r.text.append(t, endText, endAll - endText);
                    r.count += n;
                    r.changed.push_back({ newLine, 1 + static_cast<int>(n) * withLines });
                }
                else
                    r.text.append(t, at, endAll - at);
                newLine += 1 + static_cast<int>(n) * withLines;
                at = endAll;
            }
            return r;
        }
    }

    std::string ChatAgent::replaceText(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        for (const char* k : { "old_text", "edits", "files" })
            if (input.hasProperty(k))
                return fail(std::string("Give find and new_text (every match replaced), or old_text and new_text, edits or files: not find with ") + k
                            + ". Nothing was changed.");
        const juce::var findVar = input.getProperty("find", juce::var()), newVar = input.getProperty("new_text", juce::var()),
                        nameVar = input.getProperty("name", juce::var()), expectedVar = input.getProperty("expected", juce::var());
        if (!findVar.isString() || findVar.toString().isEmpty())
            return fail("find is the text (with regex, the pattern) whose every match new_text replaces");
        if (!newVar.isString())
            return fail("find needs new_text: what replaces each match, taken as it is (no $1: nothing is captured)");
        Finder finder;                                       // its words, and a match in another case for a miss's note
        finder.want = findVar.toString().toStdString();
        finder.pattern = static_cast<bool>(input.getProperty("regex", false));
        const std::string with = newVar.toString().toStdString();
        regex::Regex re;
        if (finder.pattern)
        {
            re = regex::Regex::compile(finder.want);
            if (!re.ok())
                return fail("find " + finder.quoted() + " is not a regex this search takes: " + re.error()
                            + ". (Without regex, find looks for its text as it is.) Nothing was changed.");
            finder.exact = re;
            finder.anyCase = regex::Regex::compile(finder.want, true);
        }
        else if (finder.want.find('\n') != std::string::npos)
            return fail("find matches within a line: give one line's text, or old_text for a passage of several lines. Nothing was changed.");
        const bool named = nameVar.isString() && nameVar.toString().isNotEmpty();
        if (!named && !nameVar.isVoid() && !nameVar.isUndefined() && !(nameVar.isString()))
            return fail("name is a file's name (or none, to replace in every file)");
        std::string expected = expectedVar.isString() ? expectedVar.toString().trim().toStdString() : std::string();
        if (!expectedVar.isVoid() && !expectedVar.isUndefined() && expected.empty())
            return fail("expected is the version read_file gave (8 hex digits)");
        if (!expected.empty() && !named)
            return fail("expected holds for a named file: give its name too, or leave expected out. Nothing was changed.");
        const std::string name = named ? nameVar.toString().toStdString() : std::string();
        setActivity(named ? "Replacing in " + name : std::string("Replacing in the files"));

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            Draft d = host_.chatDraft();
            Sources read;                                    // every file it reads, as it read them
            if (named)
            {
                const SourceFile* f = findFile(d.files, name);
                if (f == nullptr)
                {
                    std::string names;
                    for (const SourceFile& g : d.files)
                        names += (names.empty() ? "" : ", ") + g.name;
                    return fail("There is no file " + name + ". The files: " + names);
                }
                read.push_back(*f);
                if (!expected.empty() && versionOf(f->text) != expected)
                    return fail(changedSinceText(name, expected, f->text, false));
            }
            else
                read = d.files;
            if (read.empty())
                return fail("There is no file to replace in");

            std::uint64_t steps = kSearchSteps;
            std::size_t total = 0;
            std::vector<Replacing> done;
            for (const SourceFile& f : read)
            {
                done.push_back(replaceIn(f.text, isCircuitFile(f.name), finder.want, finder.pattern ? &re : nullptr, with, steps,
                                         kMaxReplaced - total));
                if (done.back().stoppedAt > 0)
                    return fail("Nothing was changed: " + finder.stoppedNote(f.name + "'s line " + std::to_string(done.back().stoppedAt), named)
                                + " (read_file with this pattern counts its matches).");
                total += done.back().count;
                if (total > kMaxReplaced)
                    return fail("find " + finder.quoted() + " matches more than " + std::to_string(kMaxReplaced) + " times"
                                + (named ? " in " + name : std::string()) + ": give " + finder.narrower()
                                + (named ? std::string() : ", or a file's name") + ". Nothing was changed.");
            }
            if (total == 0)
            {
                // None in its own case: the lines that hold it in another, and for a text the passage closest to it.
                std::string out = (named ? "No line of " + name : "No line of the " + std::to_string(read.size()) + (read.size() == 1 ? " file" : " files"))
                                + " " + finder.verb(1) + " " + finder.quoted() + " (find replaces in its own case).";
                std::string other;
                for (const SourceFile& f : read)
                {
                    std::vector<int> found;
                    const std::vector<std::string_view> lines = linesOf(f.text);
                    for (int n = 1; n <= static_cast<int>(lines.size()) && !finder.stopped; ++n)
                        if (!(isCircuitFile(f.name) && cir::isLayoutLine(lines[static_cast<std::size_t>(n - 1)]))
                            && finder.holds(lines[static_cast<std::size_t>(n - 1)], true))
                            found.push_back(n);
                    if (!found.empty())
                        other += (other.empty() ? "" : "; ") + f.name + (found.size() == 1 ? " line " : " lines ")
                               + someNumbers(found, 0, found.size());
                }
                if (!other.empty())
                    out += " In another case: " + other + " (give find as they write it).";
                if (!finder.pattern)
                {
                    double best = -1.0;
                    std::string near, in;
                    for (const SourceFile& f : read)
                    {
                        double score = 0.0;
                        const std::string c = closest(f.text, finder.want, &score);
                        if (!c.empty() && score > best)
                        {
                            best = score;
                            near = c;
                            in = f.name;
                        }
                    }
                    // closest() speaks of old_text: here it is find.
                    for (std::size_t p = near.find("old_text"); p != std::string::npos; p = near.find("old_text", p + 4))
                        near.replace(p, 8, "find");
                    if (!near.empty())
                        out += "\n" + (named ? std::string() : in + ": ") + near;
                    else
                        out += other.empty() ? " Nothing is close: read_file shows the text.\n" : "\n";
                }
                else
                    out += "\n";
                return fail(out + "Nothing was changed.");
            }

            // One write of every file it read (those with no match as they were), each changed one as the editor
            // holds it (ChatAgent.h).
            std::vector<std::string> held(read.size());
            for (std::size_t k = 0; k < read.size(); ++k)
                if (done[k].count > 0)
                    held[k] = asEditorHolds(done[k].text);
            int active = -1;
            for (std::size_t k = 0; k < read.size(); ++k)
                if (done[k].count > 0)
                    for (std::size_t i = 0; i < d.files.size(); ++i)
                        if (d.files[i].name == read[k].name)
                        {
                            d.files[i].text = done[k].text;
                            if (active < 0)
                                active = static_cast<int>(i);
                        }
            d.active = active;
            if (fileWriteHook_)
                fileWriteHook_(attempt);
            if (!host_.chatSetDraftIf(d, read))
            {
                // The user typed meanwhile: their text stays. With expected that is said at once; else the replacement
                // runs once more on the new text.
                const Draft now = host_.chatDraft();
                if (named && !expected.empty())
                    if (const SourceFile* g = findFile(now.files, name); g != nullptr && g->text != read.front().text)
                        return fail(changedSinceText(name, expected, g->text, false));
                if (attempt == 1)
                    return fail((named ? name : std::string("The files")) + " changed while replacing (the user's typing, which is kept): try again. "
                                "Nothing was changed.");
                continue;
            }

            std::vector<std::string> names, versions;
            std::vector<std::size_t> changedFiles;
            for (std::size_t k = 0; k < read.size(); ++k)
                if (done[k].count > 0)
                {
                    changedFiles.push_back(k);
                    names.push_back(read[k].name);
                }
            const auto matches = [](std::size_t n) { return std::to_string(n) + (n == 1 ? " match" : " matches"); };
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                for (const std::size_t k : changedFiles)
                {
                    carrySeen(read[k].name, read[k].text, done[k].text);   // seen only if what it edited was
                    versions.push_back(keepVersion(read[k].name, done[k].text));
                }
                addEntry(ChatEntry::Kind::tool, "replaced " + matches(total) + " of " + finder.quoted() + " in " + joinedAnd(names));
            }
            const auto lineList = [&](std::size_t k) {
                std::vector<int> firsts;
                for (const auto& [first, span] : done[k].changed)
                    firsts.push_back(first);
                return (firsts.size() == 1 ? "line " : "lines ") + someNumbers(firsts, 0, firsts.size());
            };
            const auto linesNow = [&](std::size_t k) {
                const std::size_t n = linesOf(done[k].text).size();
                return std::to_string(n) + (n == 1 ? " line" : " lines");
            };
            std::string out;
            if (named)
                out = "Replaced " + matches(total) + " of " + finder.quoted() + " in " + name + " (" + lineList(0)
                    + "), one step the user can undo; now " + linesNow(0) + ", version " + versions[0]
                    + (held[0].empty() ? "" : "; " + held[0]) + ".\n";
            else
            {
                out = "Replaced " + matches(total) + " of " + finder.quoted() + " in " + std::to_string(changedFiles.size())
                    + (changedFiles.size() == 1 ? " file" : " files") + ", one step the user can undo:\n";
                for (std::size_t j = 0; j < changedFiles.size(); ++j)
                {
                    const std::size_t k = changedFiles[j];
                    out += "- " + read[k].name + ": " + std::to_string(done[k].count) + " (" + lineList(k) + "); now " + linesNow(k)
                         + ", version " + versions[j] + (held[k].empty() ? "" : "; " + held[k]) + "\n";
                }
            }
            // The lines changed, as they are now: the first kChangedShown of them, numbered.
            std::size_t all = 0;
            for (const std::size_t k : changedFiles)
                for (const auto& [first, span] : done[k].changed)
                    all += static_cast<std::size_t>(span);
            out += all > kChangedShown ? "The first " + std::to_string(kChangedShown) + " of the " + std::to_string(all) + " lines changed:\n"
                                       : std::string(all == 1 ? "The line changed:\n" : "The lines changed:\n");
            std::size_t shownLines = 0;
            for (const std::size_t k : changedFiles)
            {
                if (shownLines >= kChangedShown)
                    break;
                if (!named)
                    out += read[k].name + ":\n";
                const std::vector<std::string_view> lines = linesOf(done[k].text);
                int last = 0;
                for (const auto& [first, span] : done[k].changed)
                {
                    if (shownLines >= kChangedShown)
                        break;
                    if (last > 0 && first > last + 1)
                        out += "--\n";
                    for (int n = first; n < first + span && shownLines < kChangedShown && n <= static_cast<int>(lines.size()); ++n, ++shownLines)
                    {
                        const std::string l = numberedLine(n, lines[static_cast<std::size_t>(n - 1)]);
                        if (out.size() + l.size() > kMaxOutput)
                            return out + "(cut here: read_file shows the rest)\n";
                        out += l;
                        last = n;
                    }
                }
            }
            return out;
        }
        return fail("The files changed while replacing (the user's typing, which is kept): try again. Nothing was changed.");
    }
}

