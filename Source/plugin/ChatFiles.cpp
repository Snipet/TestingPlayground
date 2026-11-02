// Source/plugin/ChatFiles.cpp: read_file and edit_file (ChatAgent.h's readFileText and editFileText), the chat's and
// MCP's alike.
//
// read_file gives a file whole, or a range of its lines (from, lines) or the lines that hold a text (find, two lines
// around each), numbered; a part read leaves the chat's record of what its Claude saw (markSeen) alone, and an edit
// marks the file seen only when its Claude had seen the text it edited (carrySeen). edit_file replaces one passage
// (old_text, new_text) or a list of them (edits), in order, each occurring exactly once in the text the ones before it
// left (try_edits' rule), all or none: one write, one step the user can undo. A passage that does not occur is answered
// with the closest one in the file, its lines numbered, and what differs. The write goes through chatSetDraftIf: only
// while the file is still the text the tool read, so what the user types meanwhile is never overwritten (the edits are
// tried once more on the new text). With `expected`, the version Claude read (every read and write says the file's
// version, its text's hash), it writes only while the file is still that version: otherwise it writes nothing, says the
// user changed it, and shows the lines that changed since (the newest kFileVersions versions given are kept for that).
#include "plugin/ChatAgent.h"

#include "dsp/TextDiff.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace playground
{
    namespace
    {
        constexpr std::size_t kMaxOutput = 12000;          // a numbered read's characters at most
        constexpr std::size_t kMaxFound = 40;              // find: the lines that hold it, shown at most
        constexpr int kContext = 2;                        // find: lines around each
        constexpr int kMaxEdits = 32;                      // edit_file's edits a call
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
    }

    std::string ChatAgent::keepVersion(const std::string& name, const std::string& text)
    {
        const std::string version = versionOf(text);
        const auto same = [&](const FileVersion& v) { return v.name == name && v.version == version; };
        fileVersions_.erase(std::remove_if(fileVersions_.begin(), fileVersions_.end(), same), fileVersions_.end());
        fileVersions_.push_back({ name, version, std::make_shared<const std::string>(text) });
        while (fileVersions_.size() > kFileVersions)
            fileVersions_.pop_front();
        return version;
    }

    std::string ChatAgent::readFileText(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        const juce::var nameVar = input.getProperty("name", juce::var());
        if (!nameVar.isString())
            return fail("read_file needs a name");
        const std::string file = nameVar.toString().toStdString();
        const Draft d = host_.chatDraft();
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
        const juce::var fromVar = input.getProperty("from", juce::var()), linesVar = input.getProperty("lines", juce::var()),
                        findVar = input.getProperty("find", juce::var());
        const bool part = isNumber(fromVar) || isNumber(linesVar) || (findVar.isString() && findVar.toString().isNotEmpty());
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
        if (!(findVar.isString() && findVar.toString().isNotEmpty()))
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
        const std::string want = findVar.toString().toStdString();
        const auto holds = [&](std::string_view l, bool anyCase) {
            if (!anyCase)
                return l.find(want) != std::string_view::npos;
            const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
            std::string a(l), b(want);
            std::transform(a.begin(), a.end(), a.begin(), lower);
            std::transform(b.begin(), b.end(), b.begin(), lower);
            return a.find(b) != std::string::npos;
        };
        std::vector<int> found;
        bool anyCase = false;
        for (const bool c : { false, true })
        {
            for (int n = from; n <= to; ++n)
                if (holds(lines[static_cast<std::size_t>(n - 1)], c))
                    found.push_back(n);
            if (!found.empty())
            {
                anyCase = c;
                break;
            }
        }
        const std::string range = from == 1 && to == count ? std::string() : " in lines " + std::to_string(from) + " to " + std::to_string(to);
        if (found.empty())
            return head + "; no line" + range + " holds \"" + want + "\".\n";
        const std::size_t shownCount = std::min(found.size(), kMaxFound);
        std::string numbers;
        for (std::size_t i = 0; i < shownCount && i < 12; ++i)
            numbers += (i == 0 ? "" : ", ") + std::to_string(found[i]);
        if (shownCount > 12)
            numbers += ", ...";
        std::string out = head + "; " + std::to_string(found.size()) + (found.size() == 1 ? " line" : " lines") + range + " hold"
                        + (found.size() == 1 ? "s" : "") + " \"" + want + "\"" + (anyCase ? " (in another case)" : "") + " (" + numbers
                        + "), with " + std::to_string(kContext) + " lines around each:\n";
        int last = 0;                                            // the last line written
        for (std::size_t i = 0; i < shownCount; ++i)
        {
            const int a = std::max(1, found[i] - kContext), b = std::min(count, found[i] + kContext);
            if (last > 0 && a > last + 1)
                out += "--\n";
            for (int n = std::max(a, last + 1); n <= b; ++n)
            {
                const std::string l = numberedLine(n, lines[static_cast<std::size_t>(n - 1)]);
                if (out.size() + l.size() > kMaxOutput)
                    return out + "(cut here: give find a longer text, or from and lines)\n";
                out += l;
            }
            last = std::max(last, b);
        }
        if (found.size() > shownCount)
            out += "(and " + std::to_string(found.size() - shownCount) + " more lines hold it: give find a longer text)\n";
        return out;
    }

    std::string ChatAgent::editFileText(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        const juce::var nameVar = input.getProperty("name", juce::var());
        if (!nameVar.isString())
            return fail("edit_file needs name, and old_text and new_text (or edits)");
        const std::string file = nameVar.toString().toStdString();
        // The edits: one passage, or a list of them.
        struct Edit
        {
            std::string oldText, newText;
        };
        std::vector<Edit> edits;
        const juce::var list = input.getProperty("edits", juce::var());
        const bool single = input.hasProperty("old_text") || input.hasProperty("new_text");
        if (!list.isVoid() && !list.isUndefined())
        {
            if (single)
                return fail("Give old_text and new_text, or edits, not both");
            const juce::Array<juce::var>* arr = list.getArray();
            if (arr == nullptr || arr->isEmpty() || arr->size() > kMaxEdits)
                return fail("edits is a list of 1 to " + std::to_string(kMaxEdits) + " edits, [{old_text, new_text}]");
            for (int i = 0; i < arr->size(); ++i)
            {
                const juce::var& e = (*arr)[i];
                const juce::var o = e.getProperty("old_text", juce::var()), n = e.getProperty("new_text", juce::var());
                if (!o.isString() || !n.isString())
                    return fail("Edit " + std::to_string(i + 1) + " needs old_text and new_text. Nothing was changed.");
                edits.push_back({ o.toString().toStdString(), n.toString().toStdString() });
            }
        }
        else
        {
            const juce::var o = input.getProperty("old_text", juce::var()), n = input.getProperty("new_text", juce::var());
            if (!o.isString() || !n.isString())
                return fail("edit_file needs name, and old_text and new_text (or edits)");
            edits.push_back({ o.toString().toStdString(), n.toString().toStdString() });
        }
        const juce::var expectedVar = input.getProperty("expected", juce::var());
        const std::string expected = expectedVar.isString() ? expectedVar.toString().trim().toStdString() : std::string();
        if (!expectedVar.isVoid() && !expectedVar.isUndefined() && expected.empty())
            return fail("expected is the version read_file gave (8 hex digits)");
        const auto label = [&](std::size_t i) {
            return edits.size() == 1 ? std::string() : "Edit " + std::to_string(i + 1) + " of " + std::to_string(edits.size()) + ": ";
        };

        // The text changed since `version` (the file's lines that changed, against the kept text), refused: the
        // user's edit, unless the text now is a version one of Claude's calls gave (a read, a write, edit_circuit).
        const auto changedSince = [&](const std::string& version, const std::string& now) {
            std::shared_ptr<const std::string> then;
            std::string nowVersion;
            bool known = false;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                const std::string current = versionOf(now);
                for (const FileVersion& v : fileVersions_)
                {
                    if (v.name == file && v.version == version)
                        then = v.text;
                    known = known || (v.name == file && v.version == current);
                }
                nowVersion = keepVersion(file, now);
            }
            std::string out = file + " changed since version " + version
                            + (known ? " (now version " + nowVersion + ", which one of your own calls gave)" : " (the user's edit, which is kept)")
                            + ": nothing was written. ";
            if (!then)
                return fail(out + "That version's text is no longer kept, so read_file shows the file now (version " + nowVersion + ").");
            const textdiff::Unified u = textdiff::unified(*then, now, 2, kChangedLines);
            return fail(out + "Its lines changed since (+" + std::to_string(u.added) + " -" + std::to_string(u.removed) + "):\n" + u.text
                        + (u.cut ? "(cut here: read_file shows the rest)\n" : "") + "Its version now: " + nowVersion
                        + ". Edit again from this text.");
        };

        setActivity("Editing " + file);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            Draft d = host_.chatDraft();
            const auto f = std::find_if(d.files.begin(), d.files.end(), [&](const SourceFile& s) { return s.name == file; });
            if (f == d.files.end())
            {
                std::string names;
                for (const SourceFile& s : d.files)
                    names += (names.empty() ? "" : ", ") + s.name;
                return fail("There is no file " + file + ". The files: " + names);
            }
            const std::string before = f->text;
            // Compared, not kept: keeping it first could push out the very version it was given (changedSince looks
            // that up, then keeps the text now).
            if (!expected.empty() && versionOf(before) != expected)
                return changedSince(expected, before);
            // All or none, in order, each on the text the ones before it left.
            std::string t = before;
            for (std::size_t i = 0; i < edits.size(); ++i)
            {
                const Edit& e = edits[i];
                if (e.oldText.empty())
                    return fail(label(i) + "old_text is empty: give the exact passage to replace. Nothing was changed.");
                std::size_t count = 0;
                std::vector<std::string> where;
                const std::size_t at = occurs(t, e.oldText, count, where);
                if (count == 0)
                {
                    const std::string near = closest(t, e.oldText);
                    return fail(label(i) + "old_text does not occur in " + file + (i > 0 ? " (as the edits before it leave it)" : "")
                                + ". " + (near.empty() ? std::string("Nothing in it is close: read_file shows its text.\n") : near)
                                + "Nothing was changed.");
                }
                if (count > 1)
                    return fail(label(i) + "old_text occurs " + std::to_string(count) + " times in " + file + " (lines " + joinedAnd(where)
                                + (count > where.size() ? ", ..." : "") + "): include more of the surrounding lines so it occurs once. "
                                  "Nothing was changed.");
                t.replace(at, e.oldText.size(), e.newText);
            }
            f->text = t;
            d.active = static_cast<int>(f - d.files.begin());
            if (!host_.chatSetDraftIf(d, file, before))
            {
                // The user typed in it between the read and the write: their text stays. With expected, that is the
                // answer; without, the edits are tried once more on the new text.
                if (!expected.empty() || attempt == 1)
                {
                    const Draft now = host_.chatDraft();
                    const auto g = std::find_if(now.files.begin(), now.files.end(), [&](const SourceFile& s) { return s.name == file; });
                    if (!expected.empty() && g != now.files.end())
                        return changedSince(expected, g->text);
                    return fail(file + " changed while editing (the user's typing, which is kept): try again.");
                }
                continue;
            }
            std::string version;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                carrySeen(file, before, t);                      // seen only if what it edited was
                version = keepVersion(file, t);
                addEntry(ChatEntry::Kind::tool, "edited " + file + (edits.size() > 1 ? " (" + std::to_string(edits.size()) + " changes)" : ""));
            }
            const std::size_t lines = linesOf(t).size();
            return "Edited " + file + (edits.size() > 1 ? ": " + std::to_string(edits.size()) + " edits, one step the user can undo" : "")
                 + "; now " + std::to_string(lines) + (lines == 1 ? " line" : " lines") + ", version " + version + ".";
        }
        return fail(file + " changed while editing (the user's typing, which is kept): try again.");
    }
}
