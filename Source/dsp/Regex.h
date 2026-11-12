// Source/dsp/Regex.h: a regular expression that matches in linear time (JUCE-free), for read_file's search. RE2's
// syntax, the part of it a search of program text needs, compiled to a Thompson NFA and run over the text as a set of
// states (Pike's simulation, without captures): a search costs at most (the text's length + 1) x (the program's size)
// steps, whatever the pattern, since nothing backtracks. A pattern such as (a+)+$ that takes a backtracking engine
// (std::regex's) exponential time on a line of a's takes this one a step a state a character. Each search counts its
// steps against a budget the caller gives, and stops with `spent` when it runs out, so a search across every file has
// a bound in time too (the MCP link runs its calls on one worker). Compiling has one as well: a repetition of what
// matches the empty string alone writes nothing, and a pattern whose repetitions written out make more than kMaxProgram
// instructions, or take more than about 4 x kMaxProgram calls in all to write ((?:(?:()()()()a){1000}){4}: 4,001
// instructions, but its empty groups' calls past that), is refused as too large.
//
// The syntax: literals; . (any character but '\n', with s any); [abc], [^a-z], [[:alpha:]] (alnum, alpha, ascii, blank,
// cntrl, digit, graph, lower, print, punct, space, upper, word, xdigit; [:^alpha:] its complement); \d \D \w \W \s \S;
// ^ and $ (the text's start and end, \A and \z too) and \b \B (ASCII's word characters); (...), (?:...), (?P<name>...)
// and (?<name>...) (groups: nothing is captured); a|b; * + ? {n} {n,} {n,m} (bounds at most 1000, a lazy ? after one
// accepted: whether a text matches is the same); flags (?i) (?-i) (?i:...) (i: ASCII's letters in either case; s: .
// takes '\n' too; m and U accepted, changing nothing a search of a line can tell); escapes \t \n \r \f \v \a, \x41,
// \x{263A}, \Q...\E (literal text, a character an atom: \Qab\E* is ab*) and \ before any punctuation. Characters are
// UTF-8's code points; a byte that is no part of one is a character of its own. Refused, with words that say so:
// backreferences, lookahead and lookbehind (they need backtracking), \p's Unicode classes, a repetition of a repetition
// (a**), an escape it does not know.
//
// Deterministic, and allocation-free while it searches (compile() sizes what search() uses). One thread at a time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playground::regex
{
    inline constexpr std::size_t kMaxPattern = 1000;   // a pattern's bytes at most
    inline constexpr std::size_t kMaxProgram = 5000;   // its program's instructions at most, repetitions expanded
    inline constexpr int kMaxRepeat = 1000;            // a counted repetition's bound at most (RE2's)
    inline constexpr int kMaxDepth = 100;              // groups and repetitions nested at most

    enum class Found : uint8_t
    {
        no,
        yes,
        spent,                                        // the steps ran out before it could say
    };

    class Regex
    {
    public:
        // The pattern compiled (ignoreCase: as if it began with (?i)). A pattern it does not take gives ok() false and
        // error() why, in RE2's words where it has them ("missing ): (abc").
        static Regex compile(std::string_view pattern, bool ignoreCase = false);

        bool ok() const { return error_.empty(); }
        const std::string& error() const { return error_; }
        std::size_t size() const { return prog_.size(); }   // its program's instructions

        // Whether a part of `text` matches (RE2's PartialMatch: ^ and $ are its start and end). Each state it visits
        // and each character it reads takes one off `steps`; at 0 it stops with `spent`.
        Found search(std::string_view text, std::uint64_t& steps);

        enum class Op : uint8_t
        {
            ch,                                       // one code point, `a`, or its other case, `b`
            set,                                      // a code point in sets_[a] (its ranges), or not in it (b != 0)
            any,                                      // any code point; with b != 0, any but '\n'
            bol,                                      // assertions: the text's start, its end, a word's edge, none
            eol,
            edge,
            noEdge,
            split,                                    // on to `a` and to `b`
            jump,                                     // on to `a`
            match,
        };
        struct Inst
        {
            Op op = Op::match;
            uint32_t a = 0, b = 0;
        };
        struct Range
        {
            uint32_t lo = 0, hi = 0;
        };

    private:
        // The closure of `pc` at byte `pos` of `text` added to list `l` (search()'s step): yes when it reaches match.
        Found close(std::size_t l, uint32_t pc, std::string_view text, std::size_t pos, std::uint64_t& steps);
        void prepare();                               // the first bytes, and the states sized

        std::vector<Inst> prog_;
        std::vector<std::vector<Range>> sets_;        // each sorted, merged
        std::string error_;
        bool firstAny_ = true;                        // a match may start at any byte (else at one in first_)
        bool first_[256] = {};                        // the bytes a match may start with
        // search()'s states: two sparse sets over the program, and the closure's stack.
        std::vector<uint32_t> dense_[2], sparse_[2], stack_;
        std::size_t count_[2] = {};
    };
}
