// PROBE name=fminus.schematic timeout=300
//
// fminus.schematic: the schematic's model (fminus/CirDoc.h, fminus/CirLayout.h): a .cir netlist's drawing kept in `*@`
// comment lines, which FMinus never reads. On the library's netlists (Library.h's .cir files, the ones the examples
// #include) and on planted ones:
//   FORMAT     write(read(x)) is a fixed point (the block, and the text written with it); the block's lines shuffled,
//              spaced oddly, its keywords upper-case, the text in CRLF (written in LF, the same netlist), the block
//              split around the file or after .end: all write the same bytes; unknown *@ lines are kept; a `*@ schematic 2` block is read only (never
//              rewritten, every op refused); 120 mutated texts (lines deleted, swapped, a character changed, nodes,
//              parts and *@ lines planted, two elements of one name, parts and stray routes far out, CRs): every write
//              a fixed point that changes no netlist line, and the edits of an op list applied to one make the text
//              apply() says
//   INVISIBLE  for every library netlist, generate() with and without its block gives the same source, errors,
//              warnings and notes byte for byte and the same selfTestKey; a + continuation across a *@ line joins
//   LAYOUT     two runs give the same bytes; no two bodies, flags or terminals overlap; every wired net's route reaches
//              all its pins and touches no other net (no pin of another, no shared edge, a crossing only straight
//              through); ground pins carry flags; diodetone.cir's block is the pinned text; screamer.cir's feedback
//              (R6, Pdrive, C4, D1, D2) sits above XU1's body and its R4-C3 leg below n1; otalp.cir has no net it
//              cannot route
//   STABLE     a shunt, a series part and a feedback part added by text, then written: every earlier part line and
//              every untouched net's line byte for byte as it was
//   EDITS      every op of the spec's §6.3 gives the exact expected text (merge naming: ground, then a directive's
//              node, then a user's name over n<k>, then the earlier mention; a cut: the anchor's side keeps the name,
//              the input's when it is the net's first pin and the output is on it too); label's tags, connect's drawn
//              path and a cut's split wires as the exact net lines, on netlists drawn by hand; the edits are sorted,
//              apart, and make the text the result says (on a text of 2,500 lines too); a failing op in a list changes
//              nothing and names that op; parseOp reads edit_circuit's grammar
//   PINS       placing each of the 13 kinds writes the spec's §5 line; `connect R4.2 XU1.inn` rewrites XU1's token 2;
//              for every library netlist each drawn pin's token is the one FMinus's Netlist.cpp reads for it (this
//              probe's own reading of its tokens and table); screamer's feedback is found on inn-out (inn's end left)
//   RECONCILE  a part renamed in the text keeps its place; renames of two kinds at once both keep theirs; a renamed
//              node keeps its route; two renames of one kind are placed again, with a note; a deleted part loses its
//              line and its stubs, nothing else; session pins hold an unplaced part where the view drew it; a part line
//              past reach (-9979..9979) is placed again, with a note
//   TEXT       stripLayout, sameNetlist, lineMap and sameProgram on planted cases
//   BUDGET     the A* pops of each library netlist's layout, printed, below the caps; the times of a reconcile and a
//              full layout at 60 parts printed (never asserted); the router's grid bounded: a part moved 9,000 units
//              away routes nothing (every wired net name tags, one note, the write a fixed point), an op that would do
//              so is refused, as is a point past reach (place, add, paste); a stray route far out is dropped and the
//              net routed again; a drawing at the edge of reach places a new part within it and writes only what
//              reads back
//   CLIP       copied(): parts as text (their lines, no terminal, part lines from the selection's top-left), pasted at a
//              point (new names, fresh nodes, the top-left there), text no netlist's refused; e12Stepped's steps and
//              decades in the suffix's style; milliWarning, FMinus's words
#include "Probe.h"

#include "Library.h"
#include "Product.h"

#include "dsp/Sources.h"
#include "fminus/CirDoc.h"

#include "fminus/Circuit.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace pg = playground;
namespace cir = playground::cir;

namespace
{
    std::vector<pg::SourceFile> netlists()
    {
        std::vector<pg::SourceFile> out;
        for (const pg::LibraryFile& f : pg::kLibrary)
            if (pg::isCircuitFile(f.name))
                out.push_back({ f.name, f.source });
        return out;
    }

    std::string libraryText(std::string_view name)
    {
        const pg::LibraryFile* f = pg::findLibraryFile(name);
        return f != nullptr ? f->source : "";
    }

    std::vector<std::string> linesOf(const std::string& text)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= text.size())
        {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            out.push_back(text.substr(start, end - start));
            if (end == text.size())
                break;
            start = end + 1;
        }
        return out;
    }

    std::string joined(const std::vector<std::string>& lines)
    {
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i)
            out += (i == 0 ? "" : "\n") + lines[i];
        return out;
    }

    std::vector<std::string> layoutLines(const std::string& text)
    {
        std::vector<std::string> out;
        for (const std::string& l : linesOf(text))
            if (cir::isLayoutLine(l))
                out.push_back(l);
        return out;
    }

    std::string replaced(std::string s, const std::string& from, const std::string& to)
    {
        const std::size_t at = s.find(from);
        return at == std::string::npos ? s : s.replace(at, from.size(), to);
    }

    std::string replacedAll(std::string s, const std::string& from, const std::string& to)
    {
        for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
            s.replace(at, from.size(), to);
        return s;
    }

    // A spec row: `got` is `want`; the two printed when not.
    bool same(funkgui::test::Probe& P, const std::string& key, const std::string& got, const std::string& want)
    {
        const bool ok = got == want;
        if (!ok)
            std::printf("%s: got\n%s\n--- wanted\n%s\n---\n", key.c_str(), got.c_str(), want.c_str());
        P.eq(key, ok ? 1 : 0, 1);
        return ok;
    }

    std::string blockOf(const std::string& text)
    {
        const cir::Netlist nl = cir::read(text);
        return cir::writeBlock(nl, cir::reconcile(text));
    }

    // `ops` (edit_circuit lines) applied to `text`; the error in `why`.
    cir::Applied run(const std::string& text, const std::vector<std::string>& lines, std::string* why = nullptr)
    {
        std::vector<cir::Op> ops;
        for (const std::string& l : lines)
        {
            std::string w;
            const std::optional<cir::Op> op = cir::parseOp(l, w);
            if (!op)
            {
                cir::Applied a;
                a.error = "parse '" + l + "': " + w;
                if (why != nullptr)
                    *why = a.error;
                return a;
            }
            ops.push_back(*op);
        }
        cir::Applied a = cir::apply(text, cir::reconcile(text), ops);
        if (why != nullptr)
            *why = a.error;
        if (!a.error.empty())
            std::printf("apply: %s\n", a.error.c_str());
        return a;
    }

    // The edits made on `text`, as the editor's applyEdits makes them (from the last, so the positions hold).
    std::string edited(const std::string& text, const std::vector<cir::Edit>& edits, bool& apart)
    {
        std::vector<std::string> lines = linesOf(text);
        apart = true;
        for (std::size_t i = 1; i < edits.size(); ++i)
            apart = apart && edits[i - 1].to < edits[i].from;
        std::string whole = text;
        // positions to offsets
        std::vector<std::size_t> start(lines.size() + 1, 0);
        for (std::size_t i = 0; i < lines.size(); ++i)
            start[i + 1] = start[i] + lines[i].size() + 1;
        for (std::size_t k = edits.size(); k-- > 0;)
        {
            const cir::Edit& e = edits[k];
            const std::size_t a = start[static_cast<std::size_t>(e.from.line)] + static_cast<std::size_t>(e.from.col);
            const std::size_t b = start[static_cast<std::size_t>(e.to.line)] + static_cast<std::size_t>(e.to.col);
            whole.replace(a, b - a, e.text);
        }
        return whole;
    }

    std::string ptText(cir::Pt p) { return std::to_string(p.x) + "," + std::to_string(p.y); }

    std::string partLine(const std::string& text, const std::string& name)
    {
        for (const std::string& l : layoutLines(text))
            if (l.rfind("*@ part " + name + " ", 0) == 0)
                return l;
        return "(none)";
    }

    std::string netLine(const std::string& text, const std::string& name)
    {
        for (const std::string& l : layoutLines(text))
            if (l.rfind("*@ net " + name + " ", 0) == 0 || l == "*@ net " + name)
                return l;
        return "(none)";
    }

    // Whether every wired net's route reaches its pins and touches no other net.
    bool routesClean(const cir::Drawing& d, std::string& why)
    {
        std::map<std::pair<int, int>, std::vector<std::pair<int, bool>>> at;   // point -> (net, horizontal edge)
        std::map<std::pair<int, int>, int> pinNet;
        for (const cir::Part& p : d.parts)
            for (const cir::PinAt& pin : p.pins)
                pinNet[{ pin.at.x, pin.at.y }] = pin.net;
        std::set<std::tuple<int, int, bool>> seenEdge;
        for (std::size_t n = 0; n < d.nets.size(); ++n)
            for (const cir::UnitEdge& e : d.nets[n].edges)
            {
                if (!seenEdge.insert({ e.a.x, e.a.y, e.h }).second)
                {
                    why = d.nets[n].name + " shares an edge at " + std::to_string(e.a.x) + "," + std::to_string(e.a.y);
                    return false;
                }
                const cir::Pt b = e.a + (e.h ? cir::Pt{ 1, 0 } : cir::Pt{ 0, 1 });
                at[{ e.a.x, e.a.y }].push_back({ static_cast<int>(n), e.h });
                at[{ b.x, b.y }].push_back({ static_cast<int>(n), e.h });
            }
        for (const auto& [p, list] : at)
        {
            std::set<int> nets;
            for (const auto& [n, h] : list)
                nets.insert(n);
            if (const auto pin = pinNet.find(p); pin != pinNet.end() && nets.count(pin->second) == 0)
            {
                why = "a wire touches the pin of " + d.nets[static_cast<std::size_t>(pin->second)].name + " at " + std::to_string(p.first) + ","
                    + std::to_string(p.second);
                return false;
            }
            if (nets.size() > 2)
            {
                why = "three nets meet at " + std::to_string(p.first) + "," + std::to_string(p.second);
                return false;
            }
            if (nets.size() == 2)
            {
                // a crossing: each net straight through, one horizontal and one vertical
                std::map<int, std::pair<int, int>> axes;   // net -> (horizontal edges, vertical edges)
                for (const auto& [n, h] : list)
                    (h ? axes[n].first : axes[n].second) += 1;
                for (const auto& [n, c] : axes)
                    if (!((c.first == 2 && c.second == 0) || (c.first == 0 && c.second == 2)))
                    {
                        why = d.nets[static_cast<std::size_t>(n)].name + " touches another net at " + std::to_string(p.first) + ","
                            + std::to_string(p.second);
                        return false;
                    }
            }
        }
        // each wired net: one piece holding all its pins; nothing inside a body but its own pins
        for (const cir::Net& n : d.nets)
        {
            if (n.style != cir::Style::wired || n.unroutable || n.pins.size() < 2)
                continue;
            std::map<std::pair<int, int>, std::pair<int, int>> parent;
            const auto find = [&](std::pair<int, int> x) {
                if (parent.find(x) == parent.end())
                    parent[x] = x;
                while (parent[x] != x)
                    x = parent[x] = parent[parent[x]];
                return x;
            };
            for (const cir::UnitEdge& e : n.edges)
            {
                const cir::Pt b = e.a + (e.h ? cir::Pt{ 1, 0 } : cir::Pt{ 0, 1 });
                const auto ra = find({ e.a.x, e.a.y }), rb = find({ b.x, b.y });
                if (ra != rb)
                    parent[std::max(ra, rb)] = std::min(ra, rb);
            }
            std::set<std::pair<int, int>> roots;
            for (const cir::PinRef& r : n.pins)
            {
                const cir::Pt p = d.parts[static_cast<std::size_t>(r.part)].pins[static_cast<std::size_t>(r.pin)].at;
                roots.insert(find({ p.x, p.y }));
            }
            if (roots.size() != 1)
            {
                why = n.name + "'s route does not reach all its pins";
                return false;
            }
            for (const cir::UnitEdge& e : n.edges)
                for (const cir::Pt q : { e.a, e.a + (e.h ? cir::Pt{ 1, 0 } : cir::Pt{ 0, 1 }) })
                {
                    const auto pin = pinNet.find({ q.x, q.y });
                    const bool own = pin != pinNet.end() && d.nets[static_cast<std::size_t>(pin->second)].key == n.key;
                    for (const cir::Part& p : d.parts)
                        if (p.body.contains(q) && !own)
                        {
                            why = n.name + "'s route crosses " + p.name;
                            return false;
                        }
                    for (const cir::Flag& f : d.flags)
                        if (f.box.contains(q))
                        {
                            why = n.name + "'s route crosses a flag";
                            return false;
                        }
                }
        }
        return true;
    }

    const cir::Part* partOf(const cir::Drawing& d, std::string_view name)
    {
        const int p = d.part(name);
        return p < 0 ? nullptr : &d.parts[static_cast<std::size_t>(p)];
    }

    // FMinus's tokens of a logical line (its Netlist.cpp's scan: blanks split, `{...}` one token), this probe's own.
    std::vector<std::string> fminusTokens(const std::string& line)
    {
        std::vector<std::string> out;
        std::string raw = line.substr(0, line.find(';'));
        std::size_t i = 0;
        while (i < raw.size())
        {
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'))
                ++i;
            if (i == raw.size())
                break;
            const std::size_t first = i;
            while (i < raw.size() && raw[i] != ' ' && raw[i] != '\t')
            {
                if (raw[i] == '{')
                {
                    const std::size_t close = raw.find('}', i);
                    i = close == std::string::npos ? raw.size() : close + 1;
                }
                else
                    ++i;
            }
            out.push_back(raw.substr(first, i - first));
        }
        return out;
    }

    // Netlist.cpp's node tokens, by kind: R C L D V I S 1 2; E and linear G 1-4; G ... OTA 1 2 4 5; X 1-3; P with its
    // first { at token 5: 1-3, at 4: 1 2.
    std::vector<int> fminusNodeTokens(const std::vector<std::string>& t)
    {
        const char k = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0][0])));
        switch (k)
        {
            case 'r':
            case 'c':
            case 'l':
            case 'd':
            case 'v':
            case 'i':
            case 's': return { 1, 2 };
            case 'e': return { 1, 2, 3, 4 };
            case 'g':
            {
                std::string w = t.size() > 3 ? t[3] : "";
                for (char& c : w)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return w == "ota" ? std::vector<int>{ 1, 2, 4, 5 } : std::vector<int>{ 1, 2, 3, 4 };
            }
            case 'x': return { 1, 2, 3 };
            case 'p':
                for (std::size_t i = 1; i < t.size(); ++i)
                    if (t[i].front() == '{')
                        return i == 5 ? std::vector<int>{ 1, 2, 3 } : std::vector<int>{ 1, 2 };
                return {};
            default: return {};
        }
    }
}

PROBE(fminus, schematic)
{
    const std::vector<pg::SourceFile> library = netlists();
    P.ge("library.netlists", static_cast<double>(library.size()), 3);

    // ---- FORMAT -----------------------------------------------------------------------------------------------------
    {
        for (const pg::SourceFile& f : library)
        {
            const std::string t1 = cir::written(f.text);
            P.eq("format." + f.name + ".fixed_point", cir::written(t1) == t1 ? 1 : 0, 1);
            P.eq("format." + f.name + ".block_fixed", blockOf(t1) == joined(layoutLines(t1)) + "\n" ? 1 : 0, 1);
            P.eq("format." + f.name + ".netlist_kept", cir::stripLayout(t1) == cir::stripLayout(f.text) ? 1 : 0, 1);
        }
        const std::string base = cir::written(libraryText("screamer.cir"));
        const std::vector<std::string> block = layoutLines(base);
        std::vector<std::string> lines = linesOf(base);
        std::vector<std::string> netlistLines;
        for (const std::string& l : lines)
            if (!cir::isLayoutLine(l))
                netlistLines.push_back(l);
        const auto endAt = [&] {
            for (std::size_t i = 0; i < netlistLines.size(); ++i)
                if (netlistLines[i] == ".end")
                    return i;
            return netlistLines.size();
        };
        // the block's lines in another order (the header kept first)
        {
            std::vector<std::string> shuffled = block;
            std::reverse(shuffled.begin() + 1, shuffled.end());
            std::rotate(shuffled.begin() + 1, shuffled.begin() + 4, shuffled.end());
            std::vector<std::string> t = netlistLines;
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(endAt()), shuffled.begin(), shuffled.end());
            same(P, "format.shuffled", cir::written(joined(t)), base);
        }
        // odd spacing, upper-case keywords, CRLF
        {
            std::vector<std::string> odd;
            for (const std::string& l : block)
                odd.push_back("  " + replacedAll(replacedAll(l, " ", "   \t"), "*@", "*@ "));
            std::vector<std::string> t = netlistLines;
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(endAt()), odd.begin(), odd.end());
            same(P, "format.odd_spacing", cir::written(joined(t)), base);
            std::vector<std::string> upper;
            for (const std::string& l : block)
                upper.push_back(replaced(replaced(replaced(replaced(replaced(replaced(l, "*@ part", "*@ PART"), "*@ net", "*@ Net"), "*@ input", "*@ INPUT"),
                                                           "*@ schematic", "*@ SCHEMATIC"), " r90", " R90"), " r180", " R180"));
            t = netlistLines;
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(endAt()), upper.begin(), upper.end());
            same(P, "format.upper_case", cir::written(joined(t)), base);
            const std::string crlf = replacedAll(base, "\n", "\r\n");
            same(P, "format.crlf_block", blockOf(crlf), blockOf(base));
            same(P, "format.crlf_text", cir::written(crlf), base);   // in LF, as the editor keeps a text
            P.eq("format.crlf_same_netlist", cir::sameNetlist(crlf, base) && cir::lineMap(crlf, base).size() == linesOf(crlf).size() ? 1 : 0, 1);
        }
        // split around the file; after .end
        {
            std::vector<std::string> t = netlistLines;
            const std::size_t half = block.size() / 2;
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(endAt()), block.begin() + static_cast<std::ptrdiff_t>(half), block.end());
            t.insert(t.begin() + 3, block.begin(), block.begin() + static_cast<std::ptrdiff_t>(half));
            same(P, "format.split", cir::written(joined(t)), base);
            std::vector<std::string> after = netlistLines;
            if (!after.empty() && after.back().empty())
                after.pop_back();
            after.insert(after.end(), block.begin(), block.end());
            same(P, "format.after_end", cir::written(joined(after) + "\n"), base);
        }
        // mutated texts (lines deleted and swapped, a character changed, a node, a part and *@ lines planted, two
        // elements of one name, a part anywhere a line can say, a stray route far out, CRs at the lines' ends): every
        // write a fixed point that changes no netlist line, and an op list applied
        {
            uint32_t seed = 12345;
            const auto next = [&seed] {
                seed = seed * 1664525u + 1013904223u;
                return seed >> 8;
            };
            const std::vector<std::string> nodes{ "a", "b", "n1", "0", "in", "out", "v2", "zz", "gnd" };
            int fixed = 0, kept = 0, runs = 0;
            for (const pg::SourceFile& f : library)
            {
                const std::vector<std::string> start = linesOf(cir::written(f.text));
                for (int it = 0; it < 40; ++it)
                {
                    std::vector<std::string> l = start;
                    const uint32_t edits = 1 + next() % 4;
                    for (uint32_t k = 0; k < edits && !l.empty(); ++k)
                    {
                        const std::size_t i = next() % l.size();
                        switch (next() % 10)
                        {
                            case 0: l.erase(l.begin() + static_cast<std::ptrdiff_t>(i)); break;
                            case 1: std::swap(l[i], l[next() % l.size()]); break;
                            case 2:
                                if (!l[i].empty())
                                    l[i][next() % l[i].size()] = " ,|-0123x"[next() % 9];
                                break;
                            case 3:
                                if (const std::size_t sp = l[i].find(' '); sp != std::string::npos)
                                    l[i].insert(sp + 1, nodes[next() % nodes.size()] + " ");
                                break;
                            case 4:
                                l.insert(l.begin() + static_cast<std::ptrdiff_t>(i), "R" + std::to_string(50 + next() % 9) + " " + nodes[next() % nodes.size()] + " "
                                                                                      + nodes[next() % nodes.size()] + " 1k");
                                break;
                            case 5:
                                l.insert(l.begin() + static_cast<std::ptrdiff_t>(i), "*@ part R" + std::to_string(next() % 12) + " "
                                                                                      + std::to_string(static_cast<int>(next() % 80) - 20) + ","
                                                                                      + std::to_string(static_cast<int>(next() % 40) - 20));
                                break;
                            case 6:
                                l.insert(l.begin() + static_cast<std::ptrdiff_t>(i), "*@ net " + nodes[next() % nodes.size()] + " 0,0 " + std::to_string(next() % 30) + ",0");
                                break;
                            case 7:   // a part anywhere a line can say, past reach too
                                l.insert(l.begin() + static_cast<std::ptrdiff_t>(i), "*@ part R" + std::to_string(next() % 12) + " "
                                                                                      + std::to_string(static_cast<int>(next() % 19999) - 9999) + ","
                                                                                      + std::to_string(static_cast<int>(next() % 19999) - 9999));
                                break;
                            case 8:   // a stray route far out
                                l.insert(l.begin() + static_cast<std::ptrdiff_t>(i), "*@ net " + nodes[next() % nodes.size()] + " "
                                                                                      + std::to_string(static_cast<int>(next() % 19999) - 9999) + ",5 9999,5 9999,-9999");
                                break;
                            default:  // a CR at every line's end (twice: two)
                                for (std::string& x : l)
                                    x += "\r";
                                break;
                        }
                    }
                    const std::string t = joined(l) + "\n";
                    const std::string w = cir::written(t);
                    ++runs;
                    fixed += cir::written(w) == w ? 1 : 0;
                    kept += cir::sameNetlist(t, w) ? 1 : 0;
                    std::string why;
                    std::vector<cir::Op> ops;
                    for (const char* o : { "place R1 3,-7 r90", "connect R1.2 0", "rename R1 R77", "place C1 9000,40", "delete C1", "layout" })
                        if (const std::optional<cir::Op> op = cir::parseOp(o, why))
                            ops.push_back(*op);
                    const cir::Applied a = cir::apply(w, cir::reconcile(w), std::span<const cir::Op>(ops.data(), 1 + next() % ops.size()));
                    bool apart = false;
                    if (a.error.empty() && edited(w, a.edits, apart) != a.text)
                        --kept;
                }
            }
            P.eq("format.mutations_fixed", fixed, runs);
            P.eq("format.mutations_netlist_kept", kept, runs);
        }

        // unknown lines kept; a newer version read only
        {
            const std::string unknown = replaced(base, ".end", "*@ probe n1 1\n*@   Future  x   y\n.end");
            const std::string w = cir::written(unknown);
            const std::vector<std::string> l = layoutLines(w);
            P.eq("format.unknown_kept", l.size() == block.size() + 2 && l[l.size() - 2] == "*@ probe n1 1" && l.back() == "*@ Future x y" ? 1 : 0, 1);
            const std::string v2 = replaced(base, "*@ schematic 1", "*@ schematic 2");
            const cir::Drawing d = cir::reconcile(v2);
            P.eq("format.v2_read_only", d.readOnly && d.version == 2 ? 1 : 0, 1);
            P.eq("format.v2_not_rewritten", cir::written(v2) == v2 ? 1 : 0, 1);
            const cir::Applied a = run(v2, { "place R1 40,40" });
            P.eq("format.v2_refuses_ops", !a.error.empty() && a.edits.empty() ? 1 : 0, 1);
        }
    }

    // ---- INVISIBLE ----------------------------------------------------------------------------------------------------
    {
        for (const pg::SourceFile& f : library)
        {
            const std::string drawn = cir::written(f.text);
            const fminus::circuit::Result a = fminus::circuit::generate(f.text, f.name);
            const fminus::circuit::Result b = fminus::circuit::generate(drawn, f.name);
            P.eq("invisible." + f.name + ".source", !a.source.empty() && a.source == b.source ? 1 : 0, 1);
            P.eq("invisible." + f.name + ".messages", a.errors == b.errors && a.warnings == b.warnings && a.notes == b.notes ? 1 : 0, 1);
            P.eq("invisible." + f.name + ".key", fminus::circuit::selfTestKey(f.text, f.name, {}) == fminus::circuit::selfTestKey(drawn, f.name, {}) ? 1 : 0, 1);
        }
        // a + continuation across a *@ line still joins
        const std::string text = libraryText("diodetone.cir");
        const std::string split = replaced(text, "R1     in a  2.2k\n", "R1     in a\n*@ part R1 6,0\n+ 2.2k\n");
        const fminus::circuit::Result a = fminus::circuit::generate(text, "diodetone.cir");
        const fminus::circuit::Result b = fminus::circuit::generate(split, "diodetone.cir");
        P.eq("invisible.continuation_joins", split != text && b.errors.empty() && a.source == b.source ? 1 : 0, 1);
        const cir::Netlist nl = cir::read(split);
        const int r1 = nl.element("R1");
        P.eq("invisible.continuation_read", r1 >= 0 && nl.elements[static_cast<std::size_t>(r1)].tokens.size() == 4 ? 1 : 0, 1);
    }

    // ---- LAYOUT -------------------------------------------------------------------------------------------------------
    {
        for (const pg::SourceFile& f : library)
        {
            const cir::Drawing d = cir::reconcile(f.text);
            P.eq("layout." + f.name + ".deterministic", cir::written(f.text) == cir::written(f.text) && blockOf(f.text) == blockOf(f.text) ? 1 : 0, 1);
            for (const auto& [a, b] : d.overlaps)
                std::printf("layout: %s overlaps %s: %s and %s\n", f.name.c_str(), f.name.c_str(), a.c_str(), b.c_str());
            P.eq("layout." + f.name + ".no_overlap", d.overlaps.empty() ? 1 : 0, 1);
            std::string why;
            const bool clean = routesClean(d, why);
            if (!clean)
                std::printf("layout: %s: %s\n", f.name.c_str(), why.c_str());
            P.eq("layout." + f.name + ".routes", clean ? 1 : 0, 1);
            bool flags = true;
            for (const cir::Net& n : d.nets)
                if (n.style == cir::Style::ground)
                    for (const cir::PinRef& r : n.pins)
                        flags = flags && std::any_of(d.flags.begin(), d.flags.end(), [&](const cir::Flag& x) { return x.pin == r; });
            P.eq("layout." + f.name + ".ground_flags", flags ? 1 : 0, 1);
            P.eq("layout." + f.name + ".all_placed", std::all_of(d.parts.begin(), d.parts.end(), [](const cir::Part& p) { return p.placed; }) ? 1 : 0, 1);
            P.eq("layout." + f.name + ".routable", std::none_of(d.nets.begin(), d.nets.end(), [](const cir::Net& n) { return n.unroutable; }) ? 1 : 0, 1);
            std::printf("layout: %s\n%s", f.name.c_str(), blockOf(f.text).c_str());
        }
        // diodetone: the spec's §4.5 shape, pinned (R2 and Ptone are one chain, its net c a 2-unit wire)
        same(P, "layout.diodetone_pinned", blockOf(libraryText("diodetone.cir")),
             "*@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)\n"
             "*@ input u 0,0\n"
             "*@ output vout 40,0\n"
             "*@ part C1 10,5 r90\n"
             "*@ part C2 35,5 r90\n"
             "*@ part D1 14,5 r90\n"
             "*@ part D2 18,5 r270\n"
             "*@ part Ptone 31,0\n"
             "*@ part R1 6,0\n"
             "*@ part R2 25,0\n"
             "*@ net a 8,0 23,0 | 10,0 10,3 | 14,0 14,3 | 18,0 18,3\n"
             "*@ net b 33,0 40,0 | 35,0 35,3\n"
             "*@ net c 27,0 29,0\n"
             "*@ net in 0,0 4,0\n");
        // screamer: the feedback above XU1, the R4-C3 leg below n1
        {
            const cir::Drawing d = cir::reconcile(libraryText("screamer.cir"));
            const cir::Part* xu1 = partOf(d, "XU1");
            bool above = xu1 != nullptr;
            for (const char* name : { "R6", "Pdrive", "C4", "D1", "D2" })
            {
                const cir::Part* p = partOf(d, name);
                above = above && p != nullptr && p->body.y1 < xu1->body.y0;
            }
            P.eq("layout.screamer_feedback_above", above ? 1 : 0, 1);
            const cir::Part* r4 = partOf(d, "R4");
            const cir::Part* c3 = partOf(d, "C3");
            const int n1y = xu1 != nullptr ? xu1->pins[1].at.y : 0;   // XU1.inn, on n1
            P.eq("layout.screamer_leg_below", r4 != nullptr && c3 != nullptr && r4->body.y0 > n1y && c3->body.y0 > r4->body.y1 ? 1 : 0, 1);
        }
        {
            const cir::Drawing d = cir::reconcile(libraryText("otalp.cir"));
            P.eq("layout.otalp_routable", std::none_of(d.nets.begin(), d.nets.end(), [](const cir::Net& n) { return n.unroutable; }) && d.notes.empty() ? 1 : 0, 1);
        }
    }

    // ---- STABLE -------------------------------------------------------------------------------------------------------
    {
        const auto check = [&](const std::string& key, const std::string& base, const std::string& changedText, const std::set<std::string>& touchedNets) {
            const std::string w = cir::written(changedText);
            const std::vector<std::string> before = layoutLines(base), after = layoutLines(w);
            bool kept = true;
            for (const std::string& l : before)
            {
                const bool part = l.rfind("*@ part ", 0) == 0;
                bool touched = false;
                if (l.rfind("*@ net ", 0) == 0)
                {
                    const std::string name = l.substr(7, l.find(' ', 7) - 7);
                    touched = touchedNets.count(name) != 0;
                }
                if ((part || (l.rfind("*@ net ", 0) == 0 && !touched)) && std::find(after.begin(), after.end(), l) == after.end())
                {
                    std::printf("%s: changed: %s\n", key.c_str(), l.c_str());
                    kept = false;
                }
            }
            P.eq(key, kept && after.size() > before.size() ? 1 : 0, 1);
        };
        const std::string tone = cir::written(libraryText("diodetone.cir"));
        check("stable.shunt", tone, replaced(tone, "C2     b  0  22n\n", "C2     b  0  22n\nC9     a  0  1n\n"), { "a" });
        check("stable.series", tone, replaced(tone, "R2     a  c  470\n", "R2     a  m  470\nR9     m  c  1k\n"), { "c", "m" });
        const std::string pedal = cir::written(libraryText("screamer.cir"));
        check("stable.feedback", pedal, replaced(pedal, "D2     v2  n1  1N914\n", "D2     v2  n1  1N914\nC9     n1  v2  100p\n"), { "n1", "v2" });
    }

    // ---- EDITS --------------------------------------------------------------------------------------------------------
    {
        // A small netlist, drawn once: the input u on in, R1 in-a, C1 a-0, R2 a-n3 (a generated name), the output
        // vout on out, and a pot.
        const std::string text = "* small.cir: a planted netlist\n"
                                 ".input  u in\n"
                                 ".output vout out\n"
                                 ".pot    tone 0.5 lin\n"
                                 "R1 in a 1k\n"
                                 "C1 a 0 10n                ; a comment kept\n"
                                 "R2 a n3 2.2k\n"
                                 "Ptone n3 out 25k {1 - tone}\n"
                                 "R3 out x 10k\n"
                                 "R4 y 0 10k\n"
                                 ".end\n";
        const std::string drawn = cir::written(text);
        const auto netlistOf = [](const cir::Applied& a) { return cir::stripLayout(a.text); };
        const auto check = [&](const std::string& key, const std::vector<std::string>& ops, const std::string& want) {
            const cir::Applied a = run(drawn, ops);
            bool apart = false;
            const std::string made = edited(drawn, a.edits, apart);
            P.eq(key + ".edits", a.error.empty() && apart && made == a.text ? 1 : 0, 1);
            same(P, key, netlistOf(a), want);
            return a;
        };
        const auto with = [&](std::string s, const std::string& from, const std::string& to) { return replaced(std::move(s), from, to); };
        const std::string base = cir::stripLayout(drawn);
        // add, with a place; place; the .pot an added pot needs, just before it
        {
            const cir::Applied a = check("edits.add", { "add R5 a 0 1k at 30,10 r90" }, with(base, "R4 y 0 10k\n", "R4 y 0 10k\nR5 a 0 1k\n"));
            same(P, "edits.add_part_line", partLine(a.text, "R5"), "*@ part R5 30,10 r90");
            cir::Op pot;
            pot.type = cir::Op::Type::add;
            std::string name;
            pot.lines = cir::placementLines(cir::read(drawn), cir::Kind::Pot, &name);
            const cir::Applied b = cir::apply(drawn, cir::reconcile(drawn), std::span<const cir::Op>(&pot, 1));
            same(P, "edits.add_pot", cir::stripLayout(b.text), with(base, "R4 y 0 10k\n", "R4 y 0 10k\n.pot pot1 0.5 lin\nP1 n1 n2 n4 100k {pot1}\n"));
            check("edits.add_directive", { "add .pot drive 0.5 audio" }, with(base, ".pot    tone 0.5 lin\n", ".pot    tone 0.5 lin\n.pot drive 0.5 audio\n"));
            const cir::Applied c = check("edits.place", { "place R1 2,-8 r90" }, base);
            same(P, "edits.place_line", partLine(c.text, "R1"), "*@ part R1 2,-8 r90");
        }
        // connect: the kept name
        check("edits.connect_ground", { "connect R4.1 C1.2" }, with(base, "R4 y 0 10k", "R4 0 0 10k"));
        check("edits.connect_directive", { "connect R3.2 R1.1" }, with(base, "R3 out x 10k", "R3 out in 10k"));
        {
            // two directive nodes: the input's is kept
            const std::string t = cir::written(with(text, "R3 out x 10k", "R3 out in 10k"));
            const cir::Applied a = run(t, { "connect R3.2 R3.1" });
            same(P, "edits.connect_input_first", cir::stripLayout(a.text),
                 with(with(with(cir::stripLayout(t), ".output vout out", ".output vout in"), "Ptone n3 out", "Ptone n3 in"), "R3 out in 10k", "R3 in in 10k"));
        }
        check("edits.connect_user_name", { "connect Ptone.a R4.1" }, with(with(base, "R2 a n3 2.2k", "R2 a y 2.2k"), "Ptone n3 out", "Ptone y out"));
        check("edits.connect_earlier", { "connect R4.1 R3.2" }, with(base, "R4 y 0 10k", "R4 x 0 10k"));
        check("edits.connect_node_name", { "connect R4.1 gnd" }, with(base, "R4 y 0 10k", "R4 0 0 10k"));
        // disconnect: that pin alone on a fresh node
        check("edits.disconnect", { "disconnect R2.1" }, with(base, "R2 a n3 2.2k", "R2 n1 n3 2.2k"));
        // cut: the wires to C1 taken out of net a: R1's side (the anchor: the first pin) keeps a
        {
            const cir::Drawing d = cir::reconcile(drawn);
            const cir::Part* c1 = partOf(d, "C1");
            cir::Op cut;
            cut.type = cir::Op::Type::cut;
            cut.name = "a";
            const cir::Net& a = d.nets[static_cast<std::size_t>(d.net("a"))];
            // the wire's unit edges that end at C1's pin on a, back to the first junction
            const cir::Pt pin = c1->pins[0].at;
            cir::Pt at = pin;
            cut.path.push_back(at);
            std::set<std::pair<int, int>> visited{ { at.x, at.y } };
            for (bool more = true; more;)
            {
                more = false;
                int degree = 0;
                cir::Pt next{};
                for (const cir::UnitEdge& e : a.edges)
                {
                    const cir::Pt b = e.a + (e.h ? cir::Pt{ 1, 0 } : cir::Pt{ 0, 1 });
                    if (e.a == at || b == at)
                    {
                        ++degree;
                        const cir::Pt o = e.a == at ? b : e.a;
                        if (visited.count({ o.x, o.y }) == 0)
                            next = o;
                    }
                }
                if ((at == pin || degree == 2) && visited.count({ next.x, next.y }) == 0)
                {
                    cut.path.push_back(next);
                    visited.insert({ next.x, next.y });
                    at = next;
                    more = true;
                }
            }
            // a path of unit steps as a polyline
            const cir::Applied r = cir::apply(drawn, d, std::span<const cir::Op>(&cut, 1));
            same(P, "edits.cut", cir::stripLayout(r.text), with(base, "C1 a 0 10n", "C1 n1 0 10n"));
            // the output's side keeps out: R3's wire cut
            const cir::Net& out = d.nets[static_cast<std::size_t>(d.net("out"))];
            const cir::Part* r3 = partOf(d, "R3");
            cir::Op cut2;
            cut2.type = cir::Op::Type::cut;
            cut2.name = "out";
            for (const cir::UnitEdge& e : out.edges)
            {
                const cir::Pt b = e.a + (e.h ? cir::Pt{ 1, 0 } : cir::Pt{ 0, 1 });
                if (e.a == r3->pins[0].at || b == r3->pins[0].at)
                    cut2.path = { e.a, b };
            }
            const cir::Applied r2 = cir::apply(drawn, d, std::span<const cir::Op>(&cut2, 1));
            same(P, "edits.cut_anchor_side", cir::stripLayout(r2.text), with(base, "R3 out x 10k", "R3 n1 x 10k"));
        }
        // rename a part, a node (and a node onto another: they merge), a terminal
        {
            const cir::Applied a = check("edits.rename_part", { "rename R1 R10" }, with(base, "R1 in a 1k", "R10 in a 1k"));
            P.eq("edits.rename_part_line", partLine(a.text, "R10") != "(none)" && partLine(a.text, "R1") == "(none)" ? 1 : 0, 1);
            const cir::Applied b = check("edits.rename_node", { "rename a mid" },
                                         with(with(with(base, "R1 in a 1k", "R1 in mid 1k"), "C1 a 0", "C1 mid 0"), "R2 a n3", "R2 mid n3"));
            P.eq("edits.rename_node_line", netLine(b.text, "mid") != "(none)" && netLine(b.text, "a") == "(none)" ? 1 : 0, 1);
            check("edits.rename_merges", { "rename y x" }, with(base, "R4 y 0 10k", "R4 x 0 10k"));
            check("edits.rename_terminal", { "rename u sig" }, with(base, ".input  u in", ".input  sig in"));
        }
        // set: a value, an expression, the keys
        {
            check("edits.set_value", { "set R1 4.7k" }, with(base, "R1 in a 1k", "R1 in a 4.7k"));
            check("edits.set_expr", { "set Ptone {tone}" }, with(base, "{1 - tone}", "{tone}"));
            check("edits.set_pot_value", { "set Ptone 50k" }, with(base, "Ptone n3 out 25k", "Ptone n3 out 50k"));
            check("edits.set_taper", { "set Ptone taper=audio" }, with(base, ".pot    tone 0.5 lin", ".pot    tone 0.5 audio"));
            check("edits.set_pot_rename", { "set Ptone pot=bright" }, with(with(base, ".pot    tone", ".pot    bright"), "{1 - tone}", "{1 - bright}"));
            check("edits.set_scale", { "set vout 2" }, with(base, ".output vout out", ".output vout out scale=2"));
            const std::string sw = cir::written(".input u in\n.output y out\n.switch sel 3 1\nS1 in out sel 1\nD1 out 0 1N914\nE1 out 0 in 0 10\n.end\n");
            const cir::Applied a = run(sw, { "set S1 positions=0,2", "set D1 model=1N4148", "set E1 gain=20", "set S1 switch=mode" });
            same(P, "edits.set_keys", cir::stripLayout(a.text), ".input u in\n.output y out\n.switch mode 3 1\nS1 in out mode 0,2\nD1 out 0 1N4148\nE1 out 0 in 0 20\n.end\n");
        }
        // delete: the element and its part line, and a .pot nothing else uses
        {
            const cir::Applied a = check("edits.delete", { "delete Ptone R4" }, with(with(with(base, ".pot    tone 0.5 lin\n", ""), "Ptone n3 out 25k {1 - tone}\n", ""), "R4 y 0 10k\n", ""));
            P.eq("edits.delete_part_line", partLine(a.text, "Ptone") == "(none)" && partLine(a.text, "R1") == partLine(drawn, "R1") ? 1 : 0, 1);
        }
        // label and style: the net line only
        {
            cir::Op style;
            style.type = cir::Op::Type::style;
            style.name = "a";
            style.flag = true;
            const cir::Applied a = cir::apply(drawn, cir::reconcile(drawn), std::span<const cir::Op>(&style, 1));
            P.eq("edits.style", cir::stripLayout(a.text) == base && netLine(a.text, "a") == "*@ net a flag" ? 1 : 0, 1);
            cir::Op label;
            label.type = cir::Op::Type::label;
            label.name = "c";
            const cir::Applied b = cir::apply(drawn, cir::reconcile(drawn), std::span<const cir::Op>(&label, 1));
            P.eq("edits.label_none", b.error.find("no net c") != std::string::npos ? 1 : 0, 1);
            // two tags on net a's route (given in any order): written sorted, before the route, in its line alone
            const cir::Drawing d = cir::reconcile(drawn);
            const cir::Net& na = d.nets[static_cast<std::size_t>(d.net("a"))];
            label.name = "a";
            label.points = { na.edges.back().a, na.edges.front().a };
            std::vector<cir::Pt> tags = label.points;
            std::sort(tags.begin(), tags.end());
            const cir::Applied c = cir::apply(drawn, d, std::span<const cir::Op>(&label, 1));
            const std::string was = netLine(drawn, "a");
            const std::string want = "*@ net a label=" + ptText(tags[0]) + " label=" + ptText(tags[1]) + was.substr(std::string("*@ net a").size());
            same(P, "edits.label", netLine(c.text, "a"), want);
            P.eq("edits.label_line_only", cir::stripLayout(c.text) == base && joined(layoutLines(c.text)) == replaced(joined(layoutLines(drawn)), was, want) ? 1 : 0, 1);
        }
        // a drawn netlist (its block written by hand, canonical): one net of four pins, m
        {
            const std::string ladder = "* ladder.cir: a planted netlist, drawn\n"
                                       ".input u in\n"
                                       ".output y o\n"
                                       "R1 in m 1k\n"
                                       "C1 m 0 10n\n"
                                       "C2 m 0 22n\n"
                                       "R2 m o 1k\n"
                                       "*@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)\n"
                                       "*@ input u 0,0\n"
                                       "*@ output y 27,0\n"
                                       "*@ part C1 10,5 r90\n"
                                       "*@ part C2 14,5 r90\n"
                                       "*@ part R1 6,0\n"
                                       "*@ part R2 21,0\n"
                                       "*@ net in 0,0 4,0\n"
                                       "*@ net m 8,0 19,0 | 10,0 10,3 | 14,0 14,3\n"
                                       "*@ net o 23,0 27,0\n"
                                       ".end\n";
            P.eq("edits.planted_drawn", cir::written(ladder) == ladder ? 1 : 0, 1);
            const std::string plain = cir::stripLayout(ladder);
            // connect with a drawn path: o into in (the input's node), the path joining in's route as drawn
            cir::Op con;
            con.type = cir::Op::Type::connect;
            con.a = { "R2", "2", "" };
            con.b = { "R1", "1", "" };
            con.path = { { 23, 0 }, { 24, 0 }, { 24, -4 }, { 3, -4 }, { 3, 0 }, { 4, 0 } };
            const cir::Applied a = cir::apply(ladder, cir::reconcile(ladder), std::span<const cir::Op>(&con, 1));
            same(P, "edits.connect_path", cir::stripLayout(a.text), with(with(plain, ".output y o", ".output y in"), "R2 m o 1k", "R2 m in 1k"));
            same(P, "edits.connect_path_route", netLine(a.text, "in") + " / " + netLine(a.text, "o"),
                 "*@ net in 0,0 4,0 | 3,0 3,-4 24,-4 24,0 | 23,0 27,0 / (none)");
            // cut between C1's and C2's drops: R1's side (m's first pin) keeps m, the other side's wires go to n1
            cir::Op cut;
            cut.type = cir::Op::Type::cut;
            cut.name = "m";
            cut.path = { { 10, 0 }, { 14, 0 } };
            const cir::Applied r = cir::apply(ladder, cir::reconcile(ladder), std::span<const cir::Op>(&cut, 1));
            same(P, "edits.cut_split", cir::stripLayout(r.text), with(with(plain, "C2 m 0 22n", "C2 n1 0 22n"), "R2 m o 1k", "R2 n1 o 1k"));
            same(P, "edits.cut_split_routes", netLine(r.text, "m") + " / " + netLine(r.text, "n1"), "*@ net m 8,0 10,0 10,3 / *@ net n1 14,3 14,0 19,0");
        }
        // cut: the input's side keeps the name when the input is the net's first pin and the output is on it too
        {
            const std::string chain = "* chain.cir: a planted netlist, drawn\n"
                                      ".input  u x\n"
                                      ".output y x\n"
                                      "R1 x 0 1k\n"
                                      "C1 x 0 10n\n"
                                      "*@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)\n"
                                      "*@ input u 0,0\n"
                                      "*@ output y 11,0\n"
                                      "*@ part C1 2,5 r90\n"
                                      "*@ part R1 6,5 r90\n"
                                      "*@ net x 0,0 11,0 | 2,0 2,3 | 6,0 6,3\n"
                                      ".end\n";
            P.eq("edits.planted_chain", cir::written(chain) == chain ? 1 : 0, 1);
            cir::Op cut;
            cut.type = cir::Op::Type::cut;
            cut.name = "x";
            cut.path = { { 6, 0 }, { 11, 0 } };
            const cir::Applied r = cir::apply(chain, cir::reconcile(chain), std::span<const cir::Op>(&cut, 1));
            same(P, "edits.cut_input_first", cir::stripLayout(r.text), with(cir::stripLayout(chain), ".output y x", ".output y n1"));
        }
        // a long text (2,500 comment lines between R1 and the block, past what the line diff matches line by line):
        // the edits still make the text, the unchanged lines untouched
        {
            std::string t = "* long.cir\n.input u in\n.output y out\nR1 in out 1k\n";
            for (int k = 0; k < 2500; ++k)
                t += "* filler " + std::to_string(k) + "\n";
            t += ".end\n";
            const std::string w = cir::written(t);
            const cir::Applied a = run(w, { "rename R1 R2" });
            bool apart = false;
            P.eq("edits.long_text", a.error.empty() && edited(w, a.edits, apart) == a.text && apart && a.edits.size() == 2 && partLine(a.text, "R2") != "(none)" ? 1 : 0, 1);
            const cir::Applied b = run(w, { "add R5 in 0 1k" });
            P.eq("edits.long_text_add", b.error.empty() && edited(w, b.edits, apart) == b.text && apart && partLine(b.text, "R5") != "(none)" ? 1 : 0, 1);
        }
        // paste: the names that collide take the next free, every node a fresh one but ground, positions offset
        {
            cir::Op paste;
            paste.type = cir::Op::Type::paste;
            paste.lines = { "R1 p q 1k", "C1 q 0 1n", "*@ part R1 0,0", "*@ part C1 4,2 r90", "*@ net q 2,0 4,0" };
            paste.at = cir::Pt{ 40, 20 };
            const cir::Applied a = cir::apply(drawn, cir::reconcile(drawn), std::span<const cir::Op>(&paste, 1));
            same(P, "edits.paste", cir::stripLayout(a.text), with(base, "R4 y 0 10k\n", "R4 y 0 10k\nR5 n1 n2 1k\nC2 n2 0 1n\n"));
            same(P, "edits.paste_places", partLine(a.text, "R5") + " / " + partLine(a.text, "C2"), "*@ part R5 40,20 / *@ part C2 44,22 r90");
        }
        // layout: one part placed again; everything (Tidy) as from nothing
        {
            const cir::Applied a = run(cir::written(text), { "place R4 60,60", "layout R4" });
            P.eq("edits.layout_one", a.error.empty() && partLine(a.text, "R4") != "*@ part R4 60,60" && cir::stripLayout(a.text) == base ? 1 : 0, 1);
            const cir::Applied b = run(drawn, { "place R1 50,50", "place R2 70,70", "layout" });
            same(P, "edits.tidy", b.text, cir::written(text));
        }
        // a failing op: nothing changes, and it is named
        {
            std::string why;
            const cir::Applied a = run(drawn, { "place R1 3,9", "connect R9.1 a", "rename R2 R20" }, &why);
            P.eq("edits.failing_names_op", a.edits.empty() && a.text == drawn && why.rfind("op 2 (connect R9.1 a): there is no part R9", 0) == 0 ? 1 : 0, 1);
            const cir::Applied b = run(drawn, { "rename R1 C7" });
            P.eq("edits.rename_keeps_letter", !b.error.empty() && b.edits.empty() ? 1 : 0, 1);
            const cir::Applied c = run(drawn, { "place R1 6,0", "place R2 6,0" });
            P.eq("edits.overlap_refused", c.error.find("overlap") != std::string::npos && c.edits.empty() ? 1 : 0, 1);
        }
        // parseOp: the grammar
        {
            std::string why;
            const std::optional<cir::Op> add = cir::parseOp("add P1 a w b 100k {drive} taper=audio at 12,4 r90", why);
            P.eq("edits.parse_add", add && add->type == cir::Op::Type::add && add->lines.front() == "P1 a w b 100k {drive} taper=audio" && add->at
                                       && add->at->x == 12 && add->at->y == 4 && add->orient == cir::Orient::r90 ? 1 : 0, 1);
            const std::optional<cir::Op> set = cir::parseOp("set Ptone {1 - tone}", why);
            P.eq("edits.parse_set_expr", set && set->value == "{1 - tone}" && set->key.empty() ? 1 : 0, 1);
            const std::optional<cir::Op> key = cir::parseOp("set D1 model=1N4148", why);
            P.eq("edits.parse_set_key", key && key->key == "model" && key->value == "1N4148" ? 1 : 0, 1);
            const std::optional<cir::Op> con = cir::parseOp("connect R4.2 XU1.inn", why);
            P.eq("edits.parse_connect", con && con->a.part == "R4" && con->a.pin == "2" && con->b.part == "XU1" && con->b.pin == "inn" ? 1 : 0, 1);
            P.eq("edits.parse_unknown", !cir::parseOp("wire a b", why) && why.find("unknown command") != std::string::npos ? 1 : 0, 1);
            P.eq("edits.parse_place_bad", !cir::parseOp("place R1 x,4", why) && why.find("not a position") != std::string::npos ? 1 : 0, 1);
        }
    }

    // ---- PINS ---------------------------------------------------------------------------------------------------------
    {
        const cir::Netlist empty = cir::read("");
        const std::vector<std::pair<cir::Kind, std::string>> want = {
            { cir::Kind::R, "R1 n1 n2 10k" },
            { cir::Kind::C, "C1 n1 n2 100n" },
            { cir::Kind::L, "L1 n1 n2 10m" },
            { cir::Kind::D, "D1 n1 n2 1N4148" },
            { cir::Kind::Pot, ".pot pot1 0.5 lin|P1 n1 n2 n3 100k {pot1}" },
            { cir::Kind::Rheostat, ".pot pot1 0.5 lin|P1 n1 n2 100k {pot1}" },
            { cir::Kind::OpAmp, "XU1 n1 n2 n3 opamp" },
            { cir::Kind::Vcvs, "E1 n1 n2 n3 n4 10" },
            { cir::Kind::Vccs, "G1 n1 n2 n3 n4 1m" },
            { cir::Kind::Ota, "G1 n1 n2 OTA n3 n4 {100u}" },
            { cir::Kind::V, "V1 n1 n2 DC 9" },
            { cir::Kind::I, "I1 n1 n2 DC 1m" },
            { cir::Kind::S, ".switch sw1 2 0|S1 n1 n2 sw1 1" },
        };
        P.eq("pins.kinds", static_cast<int64_t>(want.size()), cir::kPlaceableKinds);
        for (const auto& [kind, line] : want)
        {
            std::string name;
            const std::vector<std::string> lines = cir::placementLines(empty, kind, &name);
            std::string got;
            for (const std::string& l : lines)
                got += (got.empty() ? "" : "|") + l;
            std::string key = std::string("pins.place_") + cir::symbol(kind).word;
            for (char& c : key)
                c = c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            same(P, key, got, line);
            // and read back as that kind, drawn, its pins its symbol's
            std::string text = ".input u in\n.output vout out\n";
            for (const std::string& l : lines)
                text += l + "\n";
            text += ".end\n";
            const cir::Drawing d = cir::reconcile(text);
            const cir::Part* p = partOf(d, name);
            P.eq(key + "_drawn", p != nullptr && p->kind == kind && static_cast<int>(p->pins.size()) == cir::symbol(kind).pinCount ? 1 : 0, 1);
        }
        // connect R4.2 XU1.inn: XU1's token 2 rewritten (R4's user name kept over n<k>)
        {
            const std::string t = cir::written(".input u in\n.output vout out\nR4 in fb 10k\nXU1 in n2 out opamp\nR5 n2 0 1k\n.end\n");
            const cir::Applied a = run(t, { "connect R4.2 XU1.inn" });
            same(P, "pins.connect_inn", cir::stripLayout(a.text), ".input u in\n.output vout out\nR4 in fb 10k\nXU1 in fb out opamp\nR5 fb 0 1k\n.end\n");
            const cir::Applied b = run(t, { "connect R4.2 XU1.2" });
            P.eq("pins.connect_token_number", cir::stripLayout(b.text) == cir::stripLayout(a.text) ? 1 : 0, 1);
        }
        // each drawn pin's token is FMinus's
        for (const pg::SourceFile& f : library)
        {
            const cir::Netlist nl = cir::read(f.text);
            const cir::Drawing d = cir::reconcile(f.text);
            bool ok = true;
            for (const cir::Part& p : d.parts)
            {
                if (p.element < 0)
                    continue;
                const cir::Element& e = nl.elements[static_cast<std::size_t>(p.element)];
                // the element's line as FMinus reads it, its continuations joined
                std::string logical;
                for (int l = e.firstLine; l <= e.lastLine; ++l)
                {
                    std::string line = nl.lines[static_cast<std::size_t>(l)];
                    const std::size_t at = line.find_first_not_of(" \t");
                    if (l > e.firstLine && (at == std::string::npos || line[at] != '+'))
                        continue;
                    if (l > e.firstLine)
                        line = line.substr(at + 1);
                    logical += " " + line.substr(0, line.find(';'));
                }
                const std::vector<std::string> t = fminusTokens(logical);
                const std::vector<int> nodes = fminusNodeTokens(t);
                ok = ok && nodes.size() == p.pins.size();
                for (std::size_t k = 0; ok && k < p.pins.size(); ++k)
                {
                    const int tok = nodes[k];
                    ok = p.pins[k].token == tok && e.tokenOf[k] == tok && static_cast<std::size_t>(tok) < t.size() && t[static_cast<std::size_t>(tok)] == e.nodes[k].text
                      && p.pins[k].net == nl.node(t[static_cast<std::size_t>(tok)]);
                }
                if (!ok)
                {
                    std::printf("pins: %s: %s's pins are not FMinus's tokens\n", f.name.c_str(), p.name.c_str());
                    break;
                }
            }
            P.eq("pins.tokens." + f.name, ok ? 1 : 0, 1);
        }
        // screamer's feedback on inn-out: each row's inn end on the left
        {
            const cir::Drawing d = cir::reconcile(libraryText("screamer.cir"));
            const int n1 = d.net("n1"), v2 = d.net("v2");
            bool left = true;
            for (const char* name : { "C4", "D1", "D2" })
            {
                const cir::Part* p = partOf(d, name);
                int xn = 0, xv = 0;
                for (const cir::PinAt& pin : p->pins)
                    (pin.net == n1 ? xn : xv) = pin.at.x;
                left = left && p->pins[0].net != p->pins[1].net && (p->pins[0].net == n1 || p->pins[0].net == v2) && xn < xv;
            }
            P.eq("pins.screamer_feedback_inn_out", left ? 1 : 0, 1);
        }
    }

    // ---- RECONCILE ----------------------------------------------------------------------------------------------------
    {
        const std::string tone = cir::written(libraryText("diodetone.cir"));
        const cir::Drawing before = cir::reconcile(tone);
        const auto at = [](const cir::Drawing& d, const char* name) {
            const cir::Part* p = partOf(d, name);
            return p == nullptr ? std::string("(none)") : std::to_string(p->origin.x) + "," + std::to_string(p->origin.y) + (p->source == cir::Source::stored ? " stored" : p->source == cir::Source::session ? " session" : " placed");
        };
        const std::string r1 = at(before, "R1"), c1 = at(before, "C1"), r2 = at(before, "R2");
        {
            const std::string t = replaced(tone, "R1     in a  2.2k", "R7     in a  2.2k");
            same(P, "reconcile.rename_keeps_place", at(cir::reconcile(t), "R7"), r1);
            P.eq("reconcile.rename_written", partLine(cir::written(t), "R7") == replaced(partLine(tone, "R1"), "R1", "R7") ? 1 : 0, 1);
        }
        {
            const std::string t = replaced(replaced(tone, "R1     in a  2.2k", "R7     in a  2.2k"), "C1     a  0  10n", "C7     a  0  10n");
            const cir::Drawing d = cir::reconcile(t);
            P.eq("reconcile.two_kinds", at(d, "R7") == r1 && at(d, "C7") == c1 ? 1 : 0, 1);
        }
        {
            const std::string t = replaced(replaced(tone, "R2     a  c  470", "R2     a  mid  470"), "Ptone  c  b", "Ptone  mid  b");
            const cir::Drawing d = cir::reconcile(t);
            const int mid = d.net("mid");
            const int c = before.net("c");
            P.eq("reconcile.node_route", mid >= 0 && d.nets[static_cast<std::size_t>(mid)].edges == before.nets[static_cast<std::size_t>(c)].edges ? 1 : 0, 1);
        }
        {
            const std::string t = replaced(replaced(tone, "R1     in a  2.2k", "R7     in a  2.2k"), "R2     a  c  470", "R8     a  c  470");
            const cir::Drawing d = cir::reconcile(t);
            const bool placed = partOf(d, "R7") != nullptr && partOf(d, "R7")->source == cir::Source::placed && partOf(d, "R8")->source == cir::Source::placed;
            const bool noted = std::any_of(d.notes.begin(), d.notes.end(), [](const std::string& n) { return n.find("placed again") != std::string::npos; });
            for (const std::string& n : d.notes)
                std::printf("reconcile: note: %s\n", n.c_str());
            P.eq("reconcile.two_renames_placed", placed && noted ? 1 : 0, 1);
        }
        {
            const std::string t = replaced(tone, "D2     0  a  1N914\n", "");
            const std::string w = cir::written(t);
            std::vector<std::string> want;
            for (const std::string& l : layoutLines(tone))
            {
                if (l.rfind("*@ part D2 ", 0) == 0)
                    continue;
                want.push_back(l == "*@ net a 8,0 23,0 | 10,0 10,3 | 14,0 14,3 | 18,0 18,3" ? "*@ net a 8,0 23,0 | 10,0 10,3 | 14,0 14,3" : l);
            }
            same(P, "reconcile.delete_stubs", joined(layoutLines(w)), joined(want));
        }
        {
            const std::string t = replaced(tone, "C2     b  0  22n\n", "C2     b  0  22n\nC9     a  0  1n\n");
            cir::SessionPins pins;
            pins.parts.push_back({ "C9", { 2, 12 }, cir::Orient::r90, {}, -1 });
            const cir::Drawing d = cir::reconcile(t, pins);
            same(P, "reconcile.session_pins", at(d, "C9"), "2,12 session");
            P.eq("reconcile.session_written", partLine(cir::written(t, pins), "C9") == "*@ part C9 2,12 r90" ? 1 : 0, 1);
            P.eq("reconcile.others_still", at(d, "R2") == r2 ? 1 : 0, 1);
        }
        {
            // a part line past reach (its pins' wires could not be written): placed again, with a note; the write of
            // that a fixed point
            const std::string t = replaced(tone, partLine(tone, "R1"), "*@ part R1 9990,0");
            const cir::Drawing d = cir::reconcile(t);
            const cir::Part* p = partOf(d, "R1");
            const bool noted = std::any_of(d.notes.begin(), d.notes.end(), [](const std::string& n) {
                return n.find("*@ part R1: 9990,0 is too far out") != std::string::npos && n.find("placed again") != std::string::npos;
            });
            P.eq("reconcile.far_part_placed", p != nullptr && p->source == cir::Source::placed && cir::withinReach(p->origin) && noted ? 1 : 0, 1);
            const std::string w = cir::written(t);
            P.eq("reconcile.far_part_fixed", cir::written(w) == w ? 1 : 0, 1);
        }
    }

    // ---- TEXT ---------------------------------------------------------------------------------------------------------
    {
        same(P, "text.strip", cir::stripLayout("a\n*@ part R1 0,0\nb\n  *@ net a 0,0 1,0\n"), "a\nb\n");
        same(P, "text.strip_end", cir::stripLayout("a\n*@ x"), "a\n");
        same(P, "text.strip_only", cir::stripLayout("*@ x\n"), "");
        same(P, "text.strip_crlf", cir::stripLayout("a\r\n*@ x\r\nb\r\n"), "a\nb\n");
        P.eq("text.not_layout", cir::isLayoutLine("* @ x") || cir::isLayoutLine("R1 a b 1k ;*@") || !cir::isLayoutLine("\t*@x") ? 0 : 1, 1);
        P.eq("text.same_netlist", cir::sameNetlist("R1 a b 1k\n*@ part R1 0,0\n.end\n", "*@ net a 1,0 2,0\nR1 a b 1k\n.end") ? 1 : 0, 1);
        P.eq("text.other_netlist", cir::sameNetlist("R1 a b 1k\n", "R1 a b 2k\n") ? 0 : 1, 1);
        P.eq("text.comment_counts", cir::sameNetlist("R1 a b 1k\n* x\n", "R1 a b 1k\n") ? 0 : 1, 1);
        {
            const std::vector<int> m = cir::lineMap("A\n*@ x\nB\nC\n", "A\nB\n*@ y\n*@ z\nC\n");
            P.eq("text.line_map", m == std::vector<int>{ 0, -1, 1, 4, 5 } ? 1 : 0, 1);
            P.eq("text.line_map_other", cir::lineMap("A\nB\n", "A\nC\n").empty() ? 1 : 0, 1);
        }
        {
            const pg::Sources a{ { "program.fm", "#include \"p.cir\"\n" }, { "p.cir", "R1 a b 1k\n.end\n" } };
            pg::Sources b = a;
            b[1].text = "R1 a b 1k\n*@ part R1 0,0\n.end\n";
            P.eq("text.same_program", cir::sameProgram(a, b) ? 1 : 0, 1);
            pg::Sources c = a;
            c[0].text = "#include \"p.cir\"\n*@ part R1 0,0\n";
            P.eq("text.fm_bytes", cir::sameProgram(a, c) ? 0 : 1, 1);
            pg::Sources d = a;
            d[1].text = "R1 a b 2k\n.end\n";
            P.eq("text.netlist_change", cir::sameProgram(a, d) ? 0 : 1, 1);
            pg::Sources e{ a[1], a[0] };
            P.eq("text.order", cir::sameProgram(a, e) ? 0 : 1, 1);
            pg::Sources f = a;
            f[0].text = "#include \"p.cir\"\r\n";
            P.eq("text.sanitised", cir::sameProgram(a, f) ? 1 : 0, 1);
            pg::Sources g = a;
            g[1].name = "p2.cir";
            P.eq("text.names", cir::sameProgram(a, g) ? 0 : 1, 1);
            same(P, "text.sanitise", cir::sanitised("a\r\nb\rc\td\xc3\xa9" "e\x01"), "a\nb\nc\td?e?");
        }
    }

    // ---- BUDGET -------------------------------------------------------------------------------------------------------
    {
        for (const pg::SourceFile& f : library)
        {
            const cir::Drawing d = cir::reconcile(f.text);
            std::printf("budget: %s: %ld A* pops, at most %ld a connection, a grid of %ld points\n", f.name.c_str(), d.budget.pops, d.budget.most,
                        d.budget.cells);
            P.le("budget." + f.name + ".connection", static_cast<double>(d.budget.most), static_cast<double>(cir::kPopsPerConnection));
            P.le("budget." + f.name + ".layout", static_cast<double>(d.budget.pops), static_cast<double>(cir::kPopsPerLayout));
        }
        // 60 parts: a ladder of 20 RC sections with a diode across each (printed only)
        std::string big = ".input u n0\n.output y n20\n";
        for (int k = 1; k <= 20; ++k)
        {
            const std::string a = "n" + std::to_string(k - 1), b = "n" + std::to_string(k);
            big += "R" + std::to_string(k) + " " + a + " " + b + " 1k\nC" + std::to_string(k) + " " + b + " 0 10n\nD" + std::to_string(k) + " " + b + " 0 1N914\n";
        }
        big += ".end\n";
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        const std::string drawn = cir::written(big);
        const auto t1 = Clock::now();
        const cir::Drawing d = cir::reconcile(drawn);
        const auto t2 = Clock::now();
        std::printf("budget: 60 parts: the full layout %.2f ms, a reconcile of its drawing %.2f ms, %ld pops\n",
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(), d.budget.pops);
        P.eq("budget.sixty_drawn", d.parts.size() == 62 && d.overlaps.empty() ? 1 : 0, 1);
    }
    // The router's grid stays bounded (kRouteCells), and everything a write says reads back (kPartLimit).
    {
        const std::string tone = cir::written(libraryText("diodetone.cir"));
        // a part moved thousands of units away by hand: nothing routed, every wired net name tags, one note
        const std::string far = replaced(tone, partLine(tone, "C2"), "*@ part C2 9000,9000 r90");
        const cir::Drawing d = cir::reconcile(far);
        int wired = 0, tags = 0;
        for (const cir::Net& n : d.nets)
            if (n.style == cir::Style::wired && n.pins.size() >= 2)
            {
                ++wired;
                tags += n.unroutable ? 1 : 0;
            }
        const bool noted = std::any_of(d.notes.begin(), d.notes.end(), [](const std::string& n) { return n.find("past what the router wires") != std::string::npos; });
        std::printf("budget: C2 at 9000,9000: %s, %d of %d wired nets name tags, %zu note(s)\n", d.budget.wide ? "too wide to route" : "routed", tags, wired,
                    d.notes.size());
        P.eq("budget.wide_tags", d.budget.wide && d.budget.cells == 0 && d.budget.pops == 0 && wired > 0 && tags == wired && noted ? 1 : 0, 1);
        const std::string w = cir::written(far);
        P.eq("budget.wide_fixed_point", cir::written(w) == w && cir::sameNetlist(w, tone) ? 1 : 0, 1);
        // an op that would spread the parts so is refused, and so is a point past reach
        std::string why;
        run(tone, { "place C2 9000,9000" }, &why);
        P.eq("budget.wide_refused", why.rfind("op 1 (place C2 9000,9000): the parts would span", 0) == 0 && why.find("past what the router wires") != std::string::npos ? 1 : 0, 1);
        run(tone, { "place C2 9999,0" }, &why);
        P.eq("budget.far_place_refused", why.rfind("op 1 (place C2 9999,0): 9999,0 is too far out", 0) == 0 ? 1 : 0, 1);
        run(tone, { "add C9 b 0 1n at 0,-9990" }, &why);
        P.eq("budget.far_add_refused", why.find("0,-9990 is too far out") != std::string::npos ? 1 : 0, 1);
        cir::Op paste;
        paste.type = cir::Op::Type::paste;
        paste.lines = { "R1 p q 1k", "C1 q 0 1n", "*@ part R1 0,0", "*@ part C1 4,2 r90", "*@ net q 2,0 4,0" };
        paste.at = cir::Pt{ 9978, 0 };
        const cir::Applied pasted = cir::apply(tone, cir::reconcile(tone), std::span<const cir::Op>(&paste, 1));
        P.eq("budget.far_paste_refused", pasted.edits.empty() && pasted.error.find("C1: 9982,2 is too far out") != std::string::npos ? 1 : 0, 1);
        // a stray route far out: dropped (the grid holds the parts' box), the net routed again within it
        const std::string stray = replaced(tone, netLine(tone, "a"), "*@ net a -9999,-9999 -9999,9999 9999,9999");
        const cir::Drawing s = cir::reconcile(stray);
        std::string bad;
        const bool clean = routesClean(s, bad);
        std::printf("budget: a stray route: a grid of %ld points%s%s\n", s.budget.cells, clean ? "" : ", ", bad.c_str());
        P.eq("budget.stray_route", !s.budget.wide && s.budget.cells < 20000 && clean && !s.nets[static_cast<std::size_t>(s.net("a"))].unroutable ? 1 : 0, 1);
        const std::string sw = cir::written(stray);
        P.eq("budget.stray_fixed_point", cir::written(sw) == sw ? 1 : 0, 1);
        // at the edge of reach (the drawing moved right, its right end at the limit), a shunt added by text: placed
        // within reach, every point written within what a line says, the write a fixed point
        std::vector<std::string> shifted;
        for (const std::string& l : linesOf(tone))
        {
            if (!cir::isLayoutLine(l))
            {
                shifted.push_back(l);
                continue;
            }
            std::string moved;
            std::size_t i = 0;
            while (i < l.size())
            {
                const std::size_t e = std::min(l.find(' ', i), l.size());
                std::string tok = l.substr(i, e - i);
                const std::size_t comma = tok.find(',');
                if (comma != std::string::npos && comma > 0 && (std::isdigit(static_cast<unsigned char>(tok[0])) != 0 || tok[0] == '-'))
                    tok = std::to_string(std::stoi(tok.substr(0, comma)) + 9936) + tok.substr(comma);
                moved += (moved.empty() ? "" : " ") + tok;
                i = e + 1;
            }
            shifted.push_back(moved);
        }
        const std::string edge = joined(shifted);
        const std::string grown = replaced(edge, "C2     b  0  22n\n", "C2     b  0  22n\nC9     b  0  1n\n");
        const std::string ew = cir::written(grown);
        bool readable = true;
        for (const std::string& l : layoutLines(ew))
        {
            std::size_t i = 0;
            while ((i = l.find_first_of("-0123456789", i)) != std::string::npos)
            {
                std::size_t e = i + 1;
                while (e < l.size() && std::isdigit(static_cast<unsigned char>(l[e])) != 0)
                    ++e;
                if (e - i > 1 || l[i] != '-')
                    readable = readable && std::abs(std::stoi(l.substr(i, e - i))) <= cir::kCoordLimit;
                i = e;
            }
        }
        const cir::Drawing ed = cir::reconcile(ew);
        const cir::Part* c9 = partOf(ed, "C9");
        std::printf("budget: at the edge of reach: %s\n", partLine(ew, "C9").c_str());
        P.eq("budget.edge_drawn", cir::reconcile(edge).notes.empty() && cir::stripLayout(edge) == cir::stripLayout(tone) ? 1 : 0, 1);
        P.eq("budget.edge_placed", c9 != nullptr && cir::withinReach(c9->origin) && readable && cir::written(ew) == ew ? 1 : 0, 1);
    }

    // ---- CLIP: the view's clipboard and its value fields' helpers -----------------------------------------------------
    {
        const std::string tone = cir::written(libraryText("diodetone.cir"));
        const cir::Drawing d = cir::reconcile(tone);
        cir::Pt origin;
        const std::string clip = cir::copied(tone, d, { "R1", "D1", "u" }, &origin);
        std::printf("clip: copied R1, D1 and the input:\n%s", clip.c_str());
        cir::Box box;
        for (const char* n : { "R1", "D1" })
            if (const cir::Part* q = partOf(d, n))
                box = box.joined(q->body);
        const cir::Layout lay = cir::readLayout(clip);
        const cir::Netlist nl = cir::read(clip);
        bool relative = lay.parts.size() == 2;
        for (const cir::PartLine& l : lay.parts)
            if (const cir::Part* q = partOf(d, l.name.c_str()))
                relative = relative && l.at == q->origin - cir::Pt{ box.x0, box.y0 } && l.orient == q->orient;
        P.eq("clip.lines", nl.elements.size() == 2 && !nl.input && origin == cir::Pt{ box.x0, box.y0 } ? 1 : 0, 1);
        P.eq("clip.relative", relative ? 1 : 0, 1);
        // a paste of it at a point: new names, every node fresh but ground, the selection's top-left there
        cir::Op paste;
        paste.type = cir::Op::Type::paste;
        paste.at = cir::Pt{ 40, 20 };
        for (const std::string& l : linesOf(clip))
            paste.lines.push_back(l);
        const cir::Applied a = cir::apply(tone, d, std::span<const cir::Op>(&paste, 1));
        const cir::Drawing after = cir::reconcile(a.text);
        cir::Box pasted;
        bool fresh = a.error.empty() && a.touched.size() == 2;
        for (const std::string& n : a.touched)
            if (const cir::Part* q = partOf(after, n.c_str()))
            {
                pasted = pasted.joined(q->body);
                fresh = fresh && n != "R1" && n != "D1";
            }
        std::printf("clip: pasted %s at 40,20: %s\n", a.error.empty() ? "" : a.error.c_str(), a.said.empty() ? "" : a.said.front().c_str());
        P.eq("clip.paste_fresh", fresh && pasted.x0 == 40 && pasted.y0 == 20 ? 1 : 0, 1);
        cir::Op junk = paste;
        junk.lines = { "hello world", "" };
        P.eq("clip.paste_refused", !cir::apply(tone, d, std::span<const cir::Op>(&junk, 1)).error.empty() ? 1 : 0, 1);
        // E12 steps, keeping the suffix's style; FMinus's milliohm warning
        const auto step = [](const char* v, int dir, bool decade) { return cir::e12Stepped(v, dir, decade).value_or("-"); };
        std::printf("clip: e12: 820+ %s, 4.7k+ %s, 1meg- %s, 100n+ %s, 5k+ %s, 5k- %s, 820 decade+ %s, 1K+ %s, {x} %s\n",
                    step("820", 1, false).c_str(), step("4.7k", 1, false).c_str(), step("1meg", -1, false).c_str(),
                    step("100n", 1, false).c_str(), step("5k", 1, false).c_str(), step("5k", -1, false).c_str(),
                    step("820", 1, true).c_str(), step("1K", 1, false).c_str(), step("{x}", 1, false).c_str());
        P.eq("clip.e12", step("820", 1, false) == "1k" && step("4.7k", 1, false) == "5.6k" && step("1meg", -1, false) == "820k"
                             && step("100n", 1, false) == "120n" && step("5k", 1, false) == "5.6k" && step("5k", -1, false) == "4.7k"
                             && step("820", 1, true) == "8.2k" && step("1K", 1, false) == "1.2K" && step("{x}", 1, false) == "-" ? 1 : 0, 1);
        std::string meg;
        P.eq("clip.milli", cir::milliWarning("1M", &meg) == "'1M' is 1 milliohm in SPICE; write 1meg for a megohm" && meg == "1meg"
                               && cir::milliWarning("4.7m").size() > 0 && cir::milliWarning("1meg").empty() && cir::milliWarning("10k").empty()
                               && cir::milliWarning("1000m").empty() ? 1 : 0, 1);
    }
    return P.finish();
}
