// Source/dsp/TextDiff.h: a line diff of two texts (JUCE-free): Myers's O(ND) shortest edit script, after the lines the
// two share at their ends are set aside, so a small edit to a long file costs little. unified() writes it as `diff -u`
// does, hunks with a few lines of context, cut at a number of lines. get_status's `since` shows the user's edits with
// it (against the files' text at a token); a comparison of two versions of a program can use it too.
//
// Lines are the text's parts between '\n's; a last line without one counts, an empty text has none. Two lines are the
// same when their bytes are. Deterministic: the same texts give the same script.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playground::textdiff
{
    std::vector<std::string_view> lines(std::string_view text);

    enum class Op : uint8_t
    {
        same,                                         // a line of both: `a` its number in the old text, `b` in the new
        removed,                                      // a line of the old text only (`a`)
        added,                                        // a line of the new text only (`b`)
    };
    struct Edit
    {
        Op op = Op::same;
        std::size_t a = 0, b = 0;                     // 0-based line numbers
    };

    // The edit script that turns `before` into `after`, line by line, in order. A script longer than `maxEdits` (lines
    // removed and added) is not searched for: `whole` is then false, and the script removes every line that differs
    // and adds every new one (still a correct script, not the shortest).
    std::vector<Edit> diff(const std::vector<std::string_view>& before, const std::vector<std::string_view>& after,
                           bool* whole = nullptr, std::size_t maxEdits = 1000);

    struct Unified
    {
        std::string text;                             // the hunks ("@@ -12,5 +12,6 @@" and their lines), "" when the
                                                      // texts are the same
        int added = 0, removed = 0;                   // lines, over the whole diff (also what was cut)
        int lines = 0;                                // lines of `text`
        bool cut = false;                             // it went past maxLines: the rest is left out
    };

    // `before` to `after` as a unified diff: `context` lines around each change, at most `maxLines` lines in all (a
    // hunk is cut where the limit falls).
    Unified unified(std::string_view before, std::string_view after, int context = 2, int maxLines = 60);
}
