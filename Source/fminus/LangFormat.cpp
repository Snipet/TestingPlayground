// Source/fminus/LangFormat.cpp: lang::format() and lang::mapFormatted() (Lang.h): the house style of the examples and
// the library.
//
// One pass over the lines, keeping a stack of the brackets open (a block, an initialiser list, a parenthesis, an index,
// an attribute's [[ ]]), the if/for/while/else heads whose single statement is still to come (a level deeper; an else
// takes the indent of the if it closes, as C binds it: the nearest), and whether a statement goes on over the line's
// end (a continuation line: a level deeper, or under its open parenthesis). Each line is cut where a `{` that opens a
// block ends it after code, and after a `}` that more code follows (`} else {`); then indented and spaced. Lines inside
// a block comment are kept as they are; a directive starts its line. The result is checked against the text: the same
// tokens, in order, with the same spelling (comments and directives the same text), or the text is returned as it
// was.
#include "fminus/Lang.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace playground::lang
{
    namespace
    {
        bool blank(std::string_view s)
        {
            for (const char c : s)
                if (c != ' ' && c != '\t' && c != '\r' && c != '\f' && c != '\v')
                    return false;
            return true;
        }

        std::string rtrim(std::string_view s)
        {
            std::size_t b = s.size();
            while (b > 0 && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\f' || s[b - 1] == '\v'))
                --b;
            return std::string(s.substr(0, b));
        }

        int leadingBlanks(std::string_view s, int tabWidth = 4)
        {
            int col = 0;
            for (const char c : s)
            {
                if (c == ' ')
                    ++col;
                else if (c == '\t')
                    col = (col / tabWidth + 1) * tabWidth;
                else
                    break;
            }
            return col;
        }

        bool isCode(const Token& t) { return t.kind != Tok::comment && t.kind != Tok::directive; }

        // Two characters that would lex as one token (or open a comment) if nothing stood between them.
        bool glue(char a, char b)
        {
            if (isIdentChar(a) && isIdentChar(b))
                return true;
            static constexpr const char* kPairs[] = { "++", "--", "+=", "-=", "*=", "/=", "%=", "->", "<=", ">=", "==",
                                                      "!=", "&&", "||", "::", "//", "/*", "*/" };
            for (const char* p : kPairs)
                if (p[0] == a && p[1] == b)
                    return true;
            return false;
        }

        struct Frame
        {
            char kind = '{';                     // '{' a block, 'i' a list, '(' a parenthesis, '[' an index, 'a' [[ ]]
            int indent = 0;                      // a block's or a list's line indent (its contents a level deeper)
            int align = 0;                       // a bracket's: where its continuation lines start
            int openIndent = 0;                  // the indent of the line it opened on
            bool head = false;                   // the parenthesis of an if, for or while
            bool isIf = false;
            bool castable = false;               // may be a cast's: not after a name or sizeof
            bool types = true;                   // only type words inside so far
            int count = 0;                       // tokens inside
            bool ownsHead = false;               // a block that is a pending head's statement
        };

        struct Head
        {
            int indent = 0;                      // the head's line indent (its statement a level deeper)
            std::size_t depth = 0;               // the brackets open around it
            bool isIf = false;
        };

        class Formatter
        {
        public:
            explicit Formatter(const std::vector<std::string>& types) : types_(types) {}

            void line(std::string_view text, bool startsInComment, const std::vector<Token>& toks)
            {
                if (startsInComment)
                {
                    // Inside a block comment: as it is. Its code after the comment's end still counts.
                    flushBlank();
                    out_.push_back(rtrim(text));
                    std::vector<Token> code;
                    for (const Token& t : toks)
                        if (isCode(t))
                            code.push_back(t);
                    if (!code.empty())
                    {
                        std::string scratch;
                        segment(code, text, &scratch);
                    }
                    return;
                }
                if (blank(text) || toks.empty())
                {
                    blankPending_ = !out_.empty();
                    return;
                }
                flushBlank();
                if (toks.front().kind == Tok::directive)
                {
                    out_.push_back(std::string(toks.front().text));
                    return;
                }
                for (const std::vector<Token>& seg : split(toks))
                {
                    std::string l;
                    segment(seg, text, &l);
                    out_.push_back(std::move(l));
                }
            }

            std::string finish() const
            {
                std::string s;
                for (const std::string& l : out_)
                {
                    s += l;
                    s += '\n';
                }
                return s;
            }

        private:
            void flushBlank()
            {
                if (blankPending_ && !out_.empty())
                    out_.emplace_back();
                blankPending_ = false;
            }

            // ---- what a token is ------------------------------------------------------------------------------------
            bool isType(const Token& t) const
            {
                return t.kind == Tok::type || (t.kind == Tok::identifier && std::binary_search(types_.begin(), types_.end(), std::string(t.text)));
            }

            // Whether a `{` after these tokens (the line's code before it, and the code before the line) opens a list.
            bool initBrace(const std::vector<Token>& before) const
            {
                const Token* p = before.empty() ? (hasPrev_ ? &prev_ : nullptr) : &before.back();
                if (p == nullptr)
                    return false;
                const std::string_view x = p->text;
                if (x == "=" || x == "return" || x == "," || x == "(")
                    return true;
                if (!frames_.empty() && frames_.back().kind == 'i' && (before.empty() || x == "{"))
                    return true;
                if (x == ")" && !before.empty())
                {
                    // A vector literal's cast: ( type words ) {
                    int depth = 0;
                    for (int k = static_cast<int>(before.size()) - 1; k >= 0; --k)
                    {
                        const std::string_view y = before[static_cast<std::size_t>(k)].text;
                        if (y == ")")
                            ++depth;
                        else if (y == "(" && --depth == 0)
                        {
                            bool types = k + 1 < static_cast<int>(before.size()) - 1;
                            for (int j = k + 1; j < static_cast<int>(before.size()) - 1; ++j)
                                types = types && before[static_cast<std::size_t>(j)].kind == Tok::type;
                            const bool afterName = k > 0 && (before[static_cast<std::size_t>(k - 1)].kind == Tok::identifier
                                                             || before[static_cast<std::size_t>(k - 1)].text == "sizeof");
                            return types && !afterName;
                        }
                    }
                }
                return false;
            }

            // The line's tokens cut into the lines they become: before a `{` that opens a block and ends the line after
            // code, and after a `}` that begins it with more code after it (not `};` or `},`).
            std::vector<std::vector<Token>> split(const std::vector<Token>& toks) const
            {
                std::vector<std::vector<Token>> out;
                std::vector<Token> cur;
                std::vector<Token> codeSoFar;            // the current piece's code
                int lastCode = -1;
                for (int i = 0; i < static_cast<int>(toks.size()); ++i)
                    if (isCode(toks[static_cast<std::size_t>(i)]))
                        lastCode = i;
                int balance = 0;                         // the line's own braces open before this token
                int outerCloses = 0;                     // its closers of braces opened before the line
                for (int i = 0; i < static_cast<int>(toks.size()); ++i)
                {
                    const Token& t = toks[static_cast<std::size_t>(i)];
                    if (isCode(t))
                    {
                        if (t.text == "}" && balance == 0 && !codeSoFar.empty())
                        {
                            // A closer of a block opened on a line before, after code: on a line of its own.
                            int k = outerCloses;
                            const Frame* closes = nullptr;
                            for (auto it = frames_.rbegin(); it != frames_.rend() && closes == nullptr; ++it)
                                if (it->kind == '{' || it->kind == 'i')
                                {
                                    if (k == 0)
                                        closes = &*it;
                                    else
                                        --k;
                                }
                            if (closes != nullptr && closes->kind == '{')
                            {
                                out.push_back(std::move(cur));
                                cur.clear();
                                codeSoFar.clear();
                            }
                        }
                        if (t.text == "{")
                            ++balance;
                        else if (t.text == "}")
                        {
                            if (balance > 0)
                                --balance;
                            else
                                ++outerCloses;
                        }
                        if (t.text == "{" && i == lastCode && !codeSoFar.empty() && !initBrace(codeSoFar))
                        {
                            out.push_back(std::move(cur));
                            cur.clear();
                            codeSoFar.clear();
                        }
                        cur.push_back(t);
                        codeSoFar.push_back(t);
                        if (t.text == "}" && codeSoFar.size() == 1)
                        {
                            // More code after it on the line, other than ; or , ?
                            int next = -1;
                            for (int k = i + 1; k < static_cast<int>(toks.size()) && next < 0; ++k)
                                if (isCode(toks[static_cast<std::size_t>(k)]))
                                    next = k;
                            if (next >= 0)
                            {
                                const std::string_view x = toks[static_cast<std::size_t>(next)].text;
                                if (x != ";" && x != "," && x != ")")
                                {
                                    // A comment between stays with the brace.
                                    for (int k = i + 1; k < next; ++k)
                                        cur.push_back(toks[static_cast<std::size_t>(k)]);
                                    out.push_back(std::move(cur));
                                    cur.clear();
                                    codeSoFar.clear();
                                    i = next - 1;
                                }
                            }
                        }
                        continue;
                    }
                    cur.push_back(t);
                }
                if (!cur.empty())
                    out.push_back(std::move(cur));
                return out;
            }

            // ---- indentation ---------------------------------------------------------------------------------------
            int blockIndent() const
            {
                for (auto it = frames_.rbegin(); it != frames_.rend(); ++it)
                    if (it->kind == '{' || it->kind == 'i')
                        return it->indent + 4;
                return 0;
            }

            int baseIndent() const
            {
                if (!frames_.empty())
                {
                    const Frame& top = frames_.back();
                    if (top.kind == '(' || top.kind == '[' || top.kind == 'a')
                        return top.align;
                }
                if (stmtOpen_ && stmtDepth_ == frames_.size())
                    return stmtIndent_ + 4;
                if (!heads_.empty() && heads_.back().depth == frames_.size())
                    return heads_.back().indent + 4;
                return blockIndent();
            }

            // An else at a line's start: the if it closes (the innermost still open) gives its indent; the heads that
            // were waiting around that if wait again.
            int bindElse()
            {
                for (int k = static_cast<int>(popped_.size()) - 1; k >= 0; --k)
                    if (popped_[static_cast<std::size_t>(k)].isIf)
                    {
                        const int indent = popped_[static_cast<std::size_t>(k)].indent;
                        heads_.insert(heads_.end(), popped_.begin(), popped_.begin() + k);
                        popped_.clear();
                        return indent;
                    }
                popped_.clear();
                return baseIndent();
            }

            // The heads waiting at this depth have their statement: done (kept for an else).
            void completeHeads()
            {
                popped_.clear();
                while (!heads_.empty() && heads_.back().depth == frames_.size())
                {
                    popped_.insert(popped_.begin(), heads_.back());
                    heads_.pop_back();
                }
                if (inlineIf_ && inlineIf_->depth == frames_.size())
                {
                    popped_.push_back(*inlineIf_);
                    inlineIf_.reset();
                }
            }

            // ---- one line ------------------------------------------------------------------------------------------
            void segment(const std::vector<Token>& seg, std::string_view original, std::string* out)
            {
                int firstCode = -1, lastCode = -1;
                for (int i = 0; i < static_cast<int>(seg.size()); ++i)
                    if (isCode(seg[static_cast<std::size_t>(i)]))
                    {
                        if (firstCode < 0)
                            firstCode = i;
                        lastCode = i;
                    }
                int indent = 0;
                if (firstCode < 0)
                    indent = baseIndent();
                else
                {
                    const Token& f = seg[static_cast<std::size_t>(firstCode)];
                    if (f.kind == Tok::keyword && f.text == "else")
                        indent = bindElse();
                    else
                    {
                        popped_.clear();
                        if (f.text == "}")
                            indent = blockIndent() - 4 < 0 ? 0 : blockIndent() - 4;
                        else if (f.text == "{")
                        {
                            if (!heads_.empty() && heads_.back().depth == frames_.size() && !initBrace({}))
                                indent = heads_.back().indent;
                            else if (stmtOpen_ && stmtDepth_ == frames_.size())
                                indent = stmtIndent_;
                            else
                                indent = baseIndent();
                        }
                        else if ((f.text == ")" || f.text == "]") && !frames_.empty()
                                 && (frames_.back().kind == '(' || frames_.back().kind == '[' || frames_.back().kind == 'a'))
                            indent = frames_.back().openIndent;
                        else
                            indent = baseIndent();
                    }
                }
                indent = std::max(0, indent);
                std::string& l = *out;
                l.assign(static_cast<std::size_t>(indent), ' ');
                const int origIndent = leadingBlanks(original);
                bool onLine = false;                     // a token is on the line already
                int lastEnd = -1;                        // the original column after the token before
                for (int i = 0; i < static_cast<int>(seg.size()); ++i)
                {
                    const Token& t = seg[static_cast<std::size_t>(i)];
                    if (!isCode(t))
                    {
                        if (i > lastCode && lastCode >= 0)
                        {
                            // A comment after the code: at its column, moved with the line's indent, where it fits.
                            const int want = t.col + (indent - origIndent);
                            const int end = static_cast<int>(l.size());
                            const int gap = lastEnd >= 0 ? std::max(1, t.col - lastEnd) : 1;
                            const int col = want >= end + 1 ? want : end + gap;
                            l.append(static_cast<std::size_t>(col - end), ' ');
                        }
                        else if (onLine)
                            l += ' ';
                        l += rtrim(t.text);
                        onLine = true;
                        lastEnd = t.col + t.len;
                        continue;
                    }
                    const Token* next = nullptr;
                    for (int k = i + 1; k < static_cast<int>(seg.size()) && next == nullptr; ++k)
                        if (isCode(seg[static_cast<std::size_t>(k)]))
                            next = &seg[static_cast<std::size_t>(k)];
                    i = code(seg, i, next, i == lastCode || (next != nullptr && next == &seg[static_cast<std::size_t>(lastCode)]
                                                             && isAttrPair(t, *next)),
                             indent, onLine, l);
                    onLine = true;
                    lastEnd = seg[static_cast<std::size_t>(i)].col + seg[static_cast<std::size_t>(i)].len;
                }
            }

            static bool isAttrPair(const Token& a, const Token& b)
            {
                return (a.text == "[" && b.text == "[") || (a.text == "]" && b.text == "]");
            }

            // One code token (two for [[ and ]]) onto the line; returns the index of the last token it used.
            int code(const std::vector<Token>& seg, int i, const Token* next, bool last, int indent, bool onLine,
                     std::string& l)
            {
                const Token& t = seg[static_cast<std::size_t>(i)];
                const std::string_view x = t.text;
                // What it is here.
                const bool attrOpen = x == "[" && next != nullptr && next->text == "[" && (frames_.empty() || frames_.back().kind != 'a');
                const bool attrClose = x == "]" && next != nullptr && next->text == "]" && !frames_.empty() && frames_.back().kind == 'a';
                bool unary = false, postfix = false, pointer = false;
                if (t.kind == Tok::op)
                {
                    if (x == "!")
                        unary = true;
                    else if (x == "++" || x == "--")
                        (prevOperand_ ? postfix : unary) = true;
                    else if (x == "*" && hasPrev_ && (isType(prev_) || (prev_.kind == Tok::identifier && maybeTypeName(next))))
                        pointer = true;
                    else if ((x == "-" || x == "+" || x == "*" || x == "&") && !prevOperand_)
                        unary = true;
                }
                // The space before it.
                std::string sep;
                if (onLine)
                {
                    const std::string_view p = hasPrev_ ? prev_.text : std::string_view{};
                    bool tight = false;
                    if (prevUnary_ || p == "(" || p == "[" || prevAttrOpen_ || p == "." || p == "->" || p == "::")
                        tight = true;
                    else if (prevPointer_)
                        tight = !(t.kind == Tok::identifier || t.kind == Tok::keyword || t.kind == Tok::type);
                    else if (x == ")" || x == "]" || x == "," || x == ";" || attrClose || x == "." || x == "->" || x == "::"
                             || postfix || pointer)
                        tight = !attrOpen;
                    else if (x == "(")
                        tight = prev_.kind == Tok::identifier || p == "sizeof" || prev_.kind == Tok::type
                             || (p == ")" && prevCastClose_);
                    else if (x == "[")
                        tight = !attrOpen;
                    else if (p == ")" && prevCastClose_)
                        tight = true;
                    else if (p == "{" && x == "}")
                        tight = true;
                    if (attrOpen)
                        tight = p == "(" || p == "[";
                    if (!tight || (!l.empty() && glue(l.back(), x.front())))
                        sep = " ";
                }
                l += sep;
                const std::size_t before = frames_.size();
                // A statement begins with the first token after the last one ended.
                if (!stmtOpen_ && !(x == ";" || x == "{" || x == "}"))
                {
                    stmtOpen_ = true;
                    stmtIndent_ = indent;
                    stmtDepth_ = before;
                }
                if (!frames_.empty() && frames_.back().kind == '(' && x != ")")
                {
                    Frame& fr = frames_.back();
                    ++fr.count;
                    if (!(t.kind == Tok::type || x == "const"))
                        fr.types = false;
                }
                int used = i;
                bool castClose = false;
                if (attrOpen)
                {
                    l += "[[";
                    used = i + 1;
                    Frame fr;
                    fr.kind = 'a';
                    fr.align = static_cast<int>(l.size());
                    fr.openIndent = indent;
                    frames_.push_back(fr);
                }
                else if (attrClose)
                {
                    l += "]]";
                    used = i + 1;
                    frames_.pop_back();
                }
                else
                {
                    l += x;
                    if (t.kind == Tok::punct)
                    {
                        if (x == "(")
                        {
                            Frame fr;
                            fr.kind = '(';
                            fr.align = last ? indent + 4 : static_cast<int>(l.size());
                            fr.openIndent = indent;
                            fr.head = hasPrev_ && prev_.kind == Tok::keyword && (prev_.text == "if" || prev_.text == "for" || prev_.text == "while");
                            fr.isIf = fr.head && prev_.text == "if";
                            fr.castable = !(hasPrev_ && (prev_.kind == Tok::identifier || prev_.text == "sizeof" || prev_.kind == Tok::type
                                                         || prev_.text == ")" || prev_.text == "]"));
                            frames_.push_back(fr);
                        }
                        else if (x == ")")
                        {
                            while (!frames_.empty() && frames_.back().kind != '(' && frames_.back().kind != '{' && frames_.back().kind != 'i')
                                frames_.pop_back();
                            if (!frames_.empty() && frames_.back().kind == '(')
                            {
                                const Frame fr = frames_.back();
                                frames_.pop_back();
                                castClose = fr.castable && fr.types && fr.count > 0;
                                if (fr.head)
                                {
                                    if (last)
                                    {
                                        heads_.push_back({ indent, frames_.size(), fr.isIf });
                                        stmtOpen_ = false;
                                    }
                                    else if (fr.isIf)
                                        inlineIf_ = Head{ indent, frames_.size(), true };
                                }
                            }
                        }
                        else if (x == "[")
                        {
                            Frame fr;
                            fr.kind = '[';
                            fr.align = last ? indent + 4 : static_cast<int>(l.size());
                            fr.openIndent = indent;
                            frames_.push_back(fr);
                        }
                        else if (x == "]")
                        {
                            if (!frames_.empty() && frames_.back().kind == '[')
                                frames_.pop_back();
                        }
                        else if (x == "{")
                        {
                            Frame fr;
                            std::vector<Token> lineBefore;
                            for (int k = 0; k < i; ++k)
                                if (isCode(seg[static_cast<std::size_t>(k)]))
                                    lineBefore.push_back(seg[static_cast<std::size_t>(k)]);
                            fr.kind = initBrace(lineBefore) ? 'i' : '{';
                            fr.indent = indent;
                            fr.openIndent = indent;
                            if (fr.kind == '{')
                            {
                                fr.ownsHead = !heads_.empty() && heads_.back().depth == frames_.size();
                                stmtOpen_ = false;
                            }
                            frames_.push_back(fr);
                        }
                        else if (x == "}")
                        {
                            Frame closed;
                            bool found = false;
                            while (!frames_.empty() && !found)
                            {
                                closed = frames_.back();
                                frames_.pop_back();
                                found = closed.kind == '{' || closed.kind == 'i';
                            }
                            if (!found || closed.kind == '{')
                            {
                                stmtOpen_ = false;
                                if (closed.ownsHead)
                                    completeHeads();
                                else if (inlineIf_ && inlineIf_->depth == frames_.size())
                                {
                                    popped_.clear();
                                    popped_.push_back(*inlineIf_);
                                    inlineIf_.reset();
                                }
                            }
                        }
                        else if (x == ";")
                        {
                            if (frames_.empty() || frames_.back().kind == '{')
                            {
                                stmtOpen_ = false;
                                completeHeads();
                            }
                        }
                    }
                    else if (t.kind == Tok::keyword && (x == "else" || x == "parallel") && last)
                    {
                        heads_.push_back({ indent, frames_.size(), false });
                        stmtOpen_ = false;
                    }
                }
                // What the next token sees.
                prevPrev_ = prev_;
                hasPrevPrev_ = hasPrev_;
                prev_ = seg[static_cast<std::size_t>(used)];
                hasPrev_ = true;
                prevUnary_ = unary;
                prevPointer_ = pointer;
                prevAttrOpen_ = attrOpen;
                prevCastClose_ = castClose;
                if (attrClose)
                    prevOperand_ = false;
                else if (t.kind == Tok::identifier || t.kind == Tok::number)
                    prevOperand_ = true;
                else if (t.kind == Tok::keyword)
                    prevOperand_ = x == "this" || x == "true" || x == "false";
                else if (x == ")")
                    prevOperand_ = !castClose;
                else if (x == "]")
                    prevOperand_ = true;
                else if (x == "++" || x == "--")
                    prevOperand_ = postfix;
                else
                    prevOperand_ = false;
                return used;
            }

            // A name before `*` that is a type's though no module of that name is known: capitalised, at a statement's
            // or a parameter's start, and a name after the `*` (`Svf* f;`, `(Svf* p, int n)`).
            bool maybeTypeName(const Token* next) const
            {
                if (prev_.text.empty() || !(prev_.text[0] >= 'A' && prev_.text[0] <= 'Z'))
                    return false;
                const std::string_view b = hasPrevPrev_ ? prevPrev_.text : std::string_view{};
                const bool start = !hasPrevPrev_ || b == ";" || b == "{" || b == "}" || b == "(" || b == "," || b == "const";
                return start && next != nullptr && (next->kind == Tok::identifier || next->text == "const" || next->text == "restrict");
            }

            const std::vector<std::string>& types_;
            std::vector<std::string> out_;
            bool blankPending_ = false;
            std::vector<Frame> frames_;
            std::vector<Head> heads_, popped_;
            std::optional<Head> inlineIf_;
            bool stmtOpen_ = false;
            int stmtIndent_ = 0;
            std::size_t stmtDepth_ = 0;
            Token prev_{}, prevPrev_{};
            bool hasPrev_ = false, hasPrevPrev_ = false, prevOperand_ = false, prevUnary_ = false, prevPointer_ = false,
                 prevAttrOpen_ = false, prevCastClose_ = false;
        };

        // A text's tokens as format() compares them: kind and spelling, comments without trailing blanks.
        std::vector<std::pair<Tok, std::string>> signatureOf(std::string_view text)
        {
            std::vector<std::pair<Tok, std::string>> out;
            for (const Token& t : lex(text))
                out.push_back({ t.kind, t.kind == Tok::comment ? rtrim(t.text) : std::string(t.text) });
            return out;
        }
    }

    Formatted format(std::string_view text, const std::vector<std::string>& typeNames)
    {
        Formatted r;
        r.text = std::string(text);
        const std::vector<std::string_view> lines = splitLines(text);
        // The lines' tokens, and the block-comment state each starts in.
        std::vector<std::vector<Token>> toks(lines.size());
        std::vector<bool> inComment(lines.size(), false);
        bool state = false;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            inComment[i] = state;
            lexLine(lines[i], static_cast<int>(i), state, toks[i]);
            for (const Token& t : toks[i])
                if (t.kind == Tok::unknown)
                {
                    r.ok = false;
                    r.why = "line " + std::to_string(i + 1) + " has '" + std::string(t.text) + "', which FMinus does not read";
                    return r;
                }
        }
        // The module names: the program's, and this text's own.
        std::vector<std::string> types = typeNames;
        for (const std::vector<Token>& l : toks)
            for (std::size_t k = 0; k + 1 < l.size(); ++k)
                if (l[k].kind == Tok::keyword && l[k].text == "module" && l[k + 1].kind == Tok::identifier)
                    types.emplace_back(l[k + 1].text);
        std::sort(types.begin(), types.end());
        types.erase(std::unique(types.begin(), types.end()), types.end());

        Formatter f(types);
        for (std::size_t i = 0; i < lines.size(); ++i)
            f.line(lines[i], inComment[i], toks[i]);
        std::string out = f.finish();
        if (signatureOf(out) != signatureOf(text))
        {
            r.ok = false;
            r.why = "formatting would have changed the program's tokens (a formatter bug): the text is left as it was";
            return r;
        }
        r.changed = out != text;
        r.text = std::move(out);
        return r;
    }

    Pos mapFormatted(std::string_view before, std::string_view after, Pos p)
    {
        // The token the place is in or after, by its index (format() keeps the tokens), and the offset into it.
        const std::vector<Token> a = lex(before), b = lex(after);
        if (a.size() != b.size())
            return p;
        int k = -1;
        for (std::size_t i = 0; i < a.size() && Pos{ a[i].line, a[i].col } <= p; ++i)
            k = static_cast<int>(i);
        if (k < 0)
            return b.empty() ? Pos{} : Pos{ b.front().line, 0 };
        const Token& from = a[static_cast<std::size_t>(k)];
        const Token& to = b[static_cast<std::size_t>(k)];
        const int offset = p.line == from.line ? std::clamp(p.col - from.col, 0, from.len) : from.len;
        return { to.line, to.col + std::min(offset, to.len) };
    }
}
