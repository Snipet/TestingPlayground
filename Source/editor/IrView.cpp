// Source/editor/IrView.cpp: see IrView.h.
#include "editor/IrView.h"

#include "editor/CodeAssist.h"                       // the code's styles and palette

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

namespace playground::ui::irview
{
    namespace
    {
        template <std::size_t N>
        constexpr std::array<std::string_view, N> sorted(std::array<std::string_view, N> words)
        {
            std::sort(words.begin(), words.end());
            return words;
        }

        // LLVM's instructions (LangRef), and the words that start one (tail, musttail, notail).
        constexpr auto kInstructions = sorted(std::to_array<std::string_view>({
            "add", "addrspacecast", "alloca", "and", "ashr", "atomicrmw", "bitcast", "br", "call", "callbr", "catchpad",
            "catchret", "catchswitch", "cleanuppad", "cleanupret", "cmpxchg", "extractelement", "extractvalue", "fadd",
            "fcmp", "fdiv", "fence", "fmul", "fneg", "fpext", "fptosi", "fptoui", "fptrunc", "freeze", "frem", "fsub",
            "getelementptr", "icmp", "indirectbr", "insertelement", "insertvalue", "inttoptr", "invoke", "landingpad",
            "load", "lshr", "mul", "musttail", "notail", "or", "phi", "ptrtoint", "resume", "ret", "sdiv", "select",
            "sext", "shl", "shufflevector", "sitofp", "srem", "store", "sub", "switch", "tail", "trunc", "udiv",
            "uitofp", "unreachable", "urem", "va_arg", "xor", "zext",
        }));

        // The types but the integers (i1, i32, ...: any `i` and digits), and the `x` of a vector's or an array's.
        constexpr auto kTypes = sorted(std::to_array<std::string_view>({
            "bfloat", "double", "float", "fp128", "half", "label", "metadata", "opaque", "ppc_fp128", "ptr", "token",
            "void", "x", "x86_amx", "x86_fp80",
        }));

        // The module's own words: what a definition, a declaration or the module's header starts with.
        constexpr auto kDirectives = sorted(std::to_array<std::string_view>({
            "alias", "asm", "attributes", "comdat", "constant", "datalayout", "declare", "define", "global", "ifunc",
            "module", "section", "source_filename", "target", "triple", "type", "uselistorder",
        }));

        // Constants.
        constexpr auto kConstants = sorted(std::to_array<std::string_view>({
            "false", "none", "null", "poison", "true", "undef", "zeroinitializer",
        }));

        // A comparison's predicate and a cast's `to`: part of the instruction, drawn as an operator.
        constexpr auto kOperators = sorted(std::to_array<std::string_view>({
            "eq", "ne", "oeq", "oge", "ogt", "ole", "olt", "one", "ord", "sge", "sgt", "sle", "slt", "to", "ueq", "uge",
            "ugt", "ule", "ult", "une", "uno",
        }));

        // Linkage, flags, attributes, calling conventions, orderings: drawn faint, as the code's attributes are.
        constexpr auto kAttributes = sorted(std::to_array<std::string_view>({
            "acq_rel", "acquire", "afn", "align", "allockind", "allocsize", "alwaysinline", "appending", "arcp",
            "argmem", "atomic", "available_externally", "byval", "captures", "ccc", "cold", "coldcc", "common",
            "contract", "convergent", "default", "dereferenceable", "dereferenceable_or_null", "disjoint", "dso_local",
            "dso_preemptable", "errnomem", "exact", "extern_weak", "external", "fast", "fastcc", "hidden", "hot",
            "immarg", "inaccessiblemem", "inbounds", "inrange", "inreg", "internal", "linkonce", "linkonce_odr",
            "local_unnamed_addr", "memory", "minsize", "monotonic", "mustprogress", "nnan", "nneg", "noalias",
            "nocallback", "nocapture", "nofree", "noinline", "nonlazybind", "nonnull", "norecurse", "noreturn", "nounwind",
            "nosync", "nsw", "nsz", "noundef", "nusw", "nuw", "optsize", "private", "protected", "range", "read",
            "readnone", "readonly", "readwrite", "reassoc", "release", "returned", "samesign", "seq_cst", "signext",
            "speculatable", "sret", "syncscope", "unnamed_addr", "uwtable", "volatile", "weak", "weak_odr",
            "willreturn", "write", "writeonly", "zeroext",
        }));

        template <std::size_t N>
        bool in(const std::array<std::string_view, N>& words, std::string_view w)
        {
            return std::binary_search(words.begin(), words.end(), w);
        }

        bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
        bool digit(char c) { return c >= '0' && c <= '9'; }
        bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
        // A name's characters after % or @, and a label's: [-a-zA-Z$._0-9].
        bool nameChar(char c) { return alpha(c) || digit(c) || c == '-' || c == '$' || c == '.' || c == '_'; }
        // A bare word's: [a-zA-Z_][a-zA-Z_0-9.]*.
        bool wordChar(char c) { return alpha(c) || digit(c) || c == '_' || c == '.'; }

        // The end of a string starting at `at` (its opening quote): after its closing quote, or the line's end.
        int stringEnd(std::string_view l, int at)
        {
            const std::size_t e = l.find('"', static_cast<std::size_t>(at) + 1);
            return e == std::string_view::npos ? static_cast<int>(l.size()) : static_cast<int>(e) + 1;
        }

        // `n` with a comma every three digits: 123,456.
        std::string counted(std::size_t n)
        {
            std::string s = std::to_string(n);
            for (int k = static_cast<int>(s.size()) - 3; k > 0; k -= 3)
                s.insert(static_cast<std::size_t>(k), ",");
            return s;
        }

        std::string sizeOf(std::size_t bytes)
        {
            char buf[32];
            if (bytes >= 1000000)
                std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / 1.0e6);
            else
                std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(bytes) / 1.0e3);
            return buf;
        }

        // `text` as comment lines of at most `width` columns, broken at spaces.
        void comment(std::string& out, const std::string& text, std::size_t width = 80)
        {
            std::size_t at = 0;
            while (at < text.size())
            {
                std::size_t end = text.size();
                if (end - at > width - 2)
                {
                    end = text.rfind(' ', at + width - 2);
                    if (end == std::string::npos || end <= at)
                        end = at + width - 2;
                }
                out += "; " + text.substr(at, end - at) + "\n";
                at = end;
                while (at < text.size() && text[at] == ' ')
                    ++at;
            }
        }

        int errorsIn(const std::string& messages)
        {
            int n = 0;
            for (std::size_t at = messages.find(": error: "); at != std::string::npos; at = messages.find(": error: ", at + 1))
                ++n;
            return n;
        }
    }

    std::string name(View v, std::string_view programFile)
    {
        std::string stem(programFile);
        if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0)
            stem.resize(dot);
        if (stem.empty())
            stem = "program";
        switch (v)
        {
            case View::lowered: return stem + ".lowered.ll";
            case View::frozen:  return stem + ".frozen.ll";
            case View::optimised: break;
        }
        return stem + ".ll";
    }

    const char* word(View v)
    {
        switch (v)
        {
            case View::lowered: return "lowered";
            case View::frozen:  return "frozen";
            case View::optimised: break;
        }
        return "optimised";
    }

    std::string text(View v, std::string_view programFile, const Source& from)
    {
        const IrStatus none;
        const CompileStatus unknown;
        const IrStatus& ir = from.ir != nullptr ? *from.ir : none;
        const CompileStatus& st = from.status != nullptr ? *from.status : unknown;
        const std::shared_ptr<const IrText>& t = v == View::lowered ? ir.lowered : v == View::frozen ? ir.frozen : ir.optimised;
        std::string out;
        // What it is.
        switch (v)
        {
            case View::optimised:
                comment(out, name(v, programFile) + ": the program as FMinus compiled it, in LLVM IR, optimised (FMinus's -O2 "
                             "pipeline, its own passes among them): the text the JIT made the program's machine code from. "
                             "Read only, and not one of the program's files.");
                break;
            case View::lowered:
                comment(out, name(v, programFile) + ": the program in LLVM IR as FMinus's code generator wrote it for this "
                             "machine, before any pass ran: what the optimiser started from (" + name(View::optimised, programFile)
                             + " is what it made). Read only, and not one of the program's files.");
                break;
            case View::frozen:
                comment(out, name(v, programFile) + ": the frozen copy in LLVM IR, optimised: the program compiled again "
                             "with its dials as constants, in fast math, once they held still for 3 s. Read only, and not "
                             "one of the program's files.");
                break;
        }
        // How the program stands.
        const bool playing = ir.program != 0 || ir.making || st.ok;
        if (st.revision > 0 && !st.ok)
        {
            const int errors = errorsIn(st.errors);
            const std::string count = std::to_string(errors) + (errors == 1 ? " error" : " errors");
            comment(out, playing ? "The last RUN did not compile (" + count + ", listed under the text): the program before it "
                                   "plays on, and this is its IR."
                                 : "The program did not compile (" + count + ", listed under the text), and none compiled "
                                   "before it: there is no IR to show.");
        }
        else if (from.edited && playing)
            comment(out, "The editor's files have changed since the program that plays was compiled: RUN (Cmd-Return) "
                         "compiles them, and this view follows.");
        if (v == View::frozen)
        {
            if (!t)
                comment(out, from.frozenPlays ? std::string("Compiling the frozen copy's IR...")
                                              : std::string("No frozen copy plays now: once the dials hold still for 3 s, "
                                                            "the program is compiled again with them as constants, and "
                                                            "its IR shows here."));
            else
            {
                std::string dials;
                if (from.controls != nullptr)
                    for (int k = 0; k < kMaxControls; ++k)
                    {
                        const Control& c = (*from.controls)[static_cast<std::size_t>(k)];
                        if (!c.used)
                            continue;
                        char value[48];
                        std::snprintf(value, sizeof value, "%g", static_cast<double>(ir.frozenValues[static_cast<std::size_t>(k)]));
                        dials += (dials.empty() ? "" : ", ") + std::string(c.name) + " " + value;
                    }
                if (!dials.empty())
                    comment(out, "Its dials, as constants: " + dials + ".");
                comment(out, from.frozenPlays ? std::string("It plays now.")
                                              : std::string("It does not play now: a dial moved (a copy at the new values "
                                                            "comes once they hold still for 3 s)."));
            }
        }
        else if (!t)
        {
            if (ir.making)
                comment(out, "Compiling the IR of the program that plays...");
            else if (!ir.why.empty())
                comment(out, "Its IR could not be made: " + ir.why.substr(0, ir.why.find('\n')));
            else if (ir.program == 0 && st.ok)
                comment(out, "RUN (Cmd-Return) compiles the program, and its IR shows here.");
        }
        if (t && t->cut())
        {
            std::size_t shown = static_cast<std::size_t>(std::count(t->text.begin(), t->text.end(), '\n'));
            comment(out, "Cut: the IR is " + counted(t->lines) + " lines (" + sizeOf(t->bytes) + "); this view holds the first "
                         + counted(shown) + " (" + sizeOf(t->text.size()) + ").");
        }
        out += "\n";
        if (t)
        {
            out += t->text;
            if (t->cut())
            {
                const std::size_t shown = static_cast<std::size_t>(std::count(t->text.begin(), t->text.end(), '\n'));
                out += "; ... cut here: " + counted(t->lines > shown ? t->lines - shown : 0) + " more lines\n";
            }
        }
        return out;
    }

    void configure(funkgui::TextEditor& e, const funkgui::Theme& theme)
    {
        funkgui::TextEditorOptions o;
        o.style = funkgui::editorFont(funkgui::EditorFont::code);
        o.readOnly = true;                               // it moves, selects, copies; it refuses every edit
        o.softTabs = true;
        o.tabWidth = 2;                                  // LLVM indents by two
        o.lineNumbers = true;
        o.highlightLine = true;
        o.matchBrackets = true;
        o.minimap = true;
        o.minimapWidth = 72.0f;                          // as the code's (CodeAssist::configure)
        o.a11yTitle = "LLVM IR, read only";
        e.setOptions(o);
        e.setHighlighter(std::make_unique<IrHighlighter>());
        e.setPalette(codePalette(theme));
    }

    // ---- the highlighter ---------------------------------------------------------------------------------------------
    uint32_t IrHighlighter::line(std::string_view l, uint32_t, std::vector<funkgui::text::Span>& out) const
    {
        const int n = static_cast<int>(l.size());
        const auto span = [&](int a, int b, uint16_t st) {
            if (b > a)
                out.push_back({ a, b, st });
        };
        int i = 0;
        bool first = true;                               // the line's first token: a label's place
        while (i < n)
        {
            const char c = l[static_cast<std::size_t>(i)];
            if (c == ' ' || c == '\t')
            {
                ++i;
                continue;
            }
            const bool atStart = first && i == 0;
            first = false;
            if (c == ';')
            {
                span(i, n, csComment);
                break;
            }
            if (c == '"')
            {
                const int e = stringEnd(l, i);
                span(i, e, csString);
                i = e;
                continue;
            }
            if (c == '%' || c == '@')
            {
                int e = i + 1;
                if (e < n && l[static_cast<std::size_t>(e)] == '"')
                    e = stringEnd(l, e);
                else
                    while (e < n && nameChar(l[static_cast<std::size_t>(e)]))
                        ++e;
                span(i, e, c == '%' ? csMember : csFunction);
                i = e;
                continue;
            }
            if (c == '!' || c == '#')
            {
                // Metadata (!0, !tbaa, !{...}'s mark) and an attribute group (#0).
                int e = i + 1;
                while (e < n && nameChar(l[static_cast<std::size_t>(e)]))
                    ++e;
                span(i, e, csAttribute);
                i = e;
                continue;
            }
            const bool minus = c == '-' && i + 1 < n && digit(l[static_cast<std::size_t>(i + 1)]);
            if (digit(c) || minus)
            {
                int e = i + (minus ? 1 : 0);
                if (l[static_cast<std::size_t>(e)] == '0' && e + 1 < n && l[static_cast<std::size_t>(e + 1)] == 'x')
                {
                    e += 2;
                    if (e < n && (l[static_cast<std::size_t>(e)] == 'K' || l[static_cast<std::size_t>(e)] == 'L'
                                  || l[static_cast<std::size_t>(e)] == 'M' || l[static_cast<std::size_t>(e)] == 'H'
                                  || l[static_cast<std::size_t>(e)] == 'R'))
                        ++e;                             // a half's, a bfloat's, a long double's prefix
                    while (e < n && hex(l[static_cast<std::size_t>(e)]))
                        ++e;
                }
                else
                {
                    while (e < n && digit(l[static_cast<std::size_t>(e)]))
                        ++e;
                    if (e + 1 < n && l[static_cast<std::size_t>(e)] == '.' && digit(l[static_cast<std::size_t>(e + 1)]))
                    {
                        ++e;
                        while (e < n && digit(l[static_cast<std::size_t>(e)]))
                            ++e;
                    }
                    if (e < n && (l[static_cast<std::size_t>(e)] == 'e' || l[static_cast<std::size_t>(e)] == 'E'))
                    {
                        int k = e + 1;
                        if (k < n && (l[static_cast<std::size_t>(k)] == '+' || l[static_cast<std::size_t>(k)] == '-'))
                            ++k;
                        if (k < n && digit(l[static_cast<std::size_t>(k)]))
                        {
                            e = k;
                            while (e < n && digit(l[static_cast<std::size_t>(e)]))
                                ++e;
                        }
                    }
                }
                // A numbered block's label ("3:") at the line's start.
                const bool label = atStart && !minus && e < n && l[static_cast<std::size_t>(e)] == ':';
                span(i, label ? e + 1 : e, label ? csFunction : csNumber);
                i = label ? e + 1 : e;
                continue;
            }
            if (alpha(c) || c == '_' || c == '.' || c == '$')
            {
                int e = i + 1;
                while (e < n && (atStart ? nameChar(l[static_cast<std::size_t>(e)]) : wordChar(l[static_cast<std::size_t>(e)])))
                    ++e;
                const std::string_view w = l.substr(static_cast<std::size_t>(i), static_cast<std::size_t>(e - i));
                if (atStart && e < n && l[static_cast<std::size_t>(e)] == ':')
                {
                    span(i, e + 1, csFunction);          // a block's label: "entry:", "for.body:"
                    i = e + 1;
                    continue;
                }
                if (w == "c" && e < n && l[static_cast<std::size_t>(e)] == '"')
                {
                    const int s = stringEnd(l, e);       // c"...": an array of bytes
                    span(i, s, csString);
                    i = s;
                    continue;
                }
                uint16_t st = csPlain;
                if (w.size() >= 2 && w[0] == 'i' && std::all_of(w.begin() + 1, w.end(), digit))
                    st = csType;
                else if (in(kInstructions, w))
                    st = csKeyword;
                else if (in(kTypes, w))
                    st = csType;
                else if (in(kDirectives, w))
                    st = csDirective;
                else if (in(kConstants, w))
                    st = csNumber;
                else if (in(kOperators, w))
                    st = csOperator;
                else if (in(kAttributes, w))
                    st = csAttribute;
                span(i, e, st);
                i = e;
                continue;
            }
            switch (c)
            {
                case '(': case ')': case '[': case ']': case '{': case '}': case '<': case '>': case ',':
                    span(i, i + 1, csPunct);
                    break;
                case '=': case '*':
                    span(i, i + 1, csOperator);
                    break;
                default:
                    break;
            }
            ++i;
        }
        return 0;
    }
}
