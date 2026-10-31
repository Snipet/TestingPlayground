// Source/fminus/CirLayout.cpp: the schematic's geometry and layout engine (CirLayout.h).
#include "fminus/CirLayout.h"

#include "fminus/CirDoc.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace playground::cir
{
    // ---- the grid and orientation -----------------------------------------------------------------------------------
    Box Box::joined(const Box& o) const noexcept
    {
        if (o.empty())
            return *this;
        if (empty())
            return o;
        return { std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1) };
    }

    Box Box::of(Pt a, Pt b) noexcept
    {
        return { std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y) };
    }

    namespace
    {
        constexpr const char* kOrientNames[] = { "r0", "r90", "r180", "r270", "m0", "m90", "m180", "m270" };
    }

    const char* orientName(Orient o) noexcept { return kOrientNames[static_cast<int>(o)]; }

    std::optional<Orient> orientOf(std::string_view word) noexcept
    {
        for (const Orient o : kOrients)
        {
            const std::string_view n = kOrientNames[static_cast<int>(o)];
            if (n.size() != word.size())
                continue;
            bool same = true;
            for (std::size_t i = 0; i < n.size() && same; ++i)
            {
                const char c = word[i] >= 'A' && word[i] <= 'Z' ? static_cast<char>(word[i] - 'A' + 'a') : word[i];
                same = c == n[i];
            }
            if (same)
                return o;
        }
        return std::nullopt;
    }

    Pt turned(Orient o, Pt p) noexcept
    {
        const int i = static_cast<int>(o);
        if (i >= 4)
            p.x = -p.x;
        for (int r = 0; r < i % 4; ++r)
            p = { -p.y, p.x };                            // 90 degrees clockwise, y down
        return p;
    }

    Dir turned(Orient o, Dir d) noexcept
    {
        const Pt v = turned(o, unit(d));
        return v.x > 0 ? Dir::E : v.x < 0 ? Dir::W : v.y > 0 ? Dir::S : Dir::N;
    }

    Box turned(Orient o, const Box& b) noexcept { return Box::of(turned(o, Pt{ b.x0, b.y0 }), turned(o, Pt{ b.x1, b.y1 })); }

    Orient rotatedCw(Orient o) noexcept
    {
        const int i = static_cast<int>(o);
        return static_cast<Orient>((i / 4) * 4 + (i % 4 + 1) % 4);
    }

    Orient rotatedCcw(Orient o) noexcept
    {
        const int i = static_cast<int>(o);
        return static_cast<Orient>((i / 4) * 4 + (i % 4 + 3) % 4);
    }

    Orient mirrored(Orient o) noexcept
    {
        // The screen's mirror M after R_r M^m is R_(-r) M^(m+1).
        const int i = static_cast<int>(o);
        return static_cast<Orient>((1 - i / 4) * 4 + (4 - i % 4) % 4);
    }

    // ---- the part library ---------------------------------------------------------------------------------------------
    namespace
    {
        constexpr Box kTwo{ -2, -1, 2, 1 };
        constexpr Box kAmp{ -3, -2, 3, 2 };

        const Symbol kSymbols[] = {
            { Kind::R, "resistor", "R", { { { "1", 1, { -2, 0 }, Dir::W }, { "2", 2, { 2, 0 }, Dir::E } } }, 2, kTwo },
            { Kind::C, "capacitor", "C", { { { "1", 1, { -2, 0 }, Dir::W }, { "2", 2, { 2, 0 }, Dir::E } } }, 2, kTwo },
            { Kind::L, "inductor", "L", { { { "1", 1, { -2, 0 }, Dir::W }, { "2", 2, { 2, 0 }, Dir::E } } }, 2, kTwo },
            { Kind::D, "diode", "D", { { { "a", 1, { -2, 0 }, Dir::W }, { "k", 2, { 2, 0 }, Dir::E } } }, 2, kTwo },
            { Kind::Pot, "pot", "P",
              { { { "e1", 1, { -2, 0 }, Dir::W }, { "w", 2, { 0, -2 }, Dir::N }, { "e2", 3, { 2, 0 }, Dir::E } } }, 3,
              { -2, -2, 2, 1 } },
            { Kind::Rheostat, "rheostat", "Shift-P", { { { "a", 1, { -2, 0 }, Dir::W }, { "b", 2, { 2, 0 }, Dir::E } } }, 2, kTwo },
            { Kind::OpAmp, "op amp", "X",
              { { { "inp", 1, { -3, 1 }, Dir::W }, { "inn", 2, { -3, -1 }, Dir::W }, { "out", 3, { 3, 0 }, Dir::E } } }, 3, kAmp },
            { Kind::Vcvs, "VCVS", "E",
              { { { "p", 1, { 1, -2 }, Dir::N }, { "n", 2, { 1, 2 }, Dir::S }, { "cp", 3, { -3, -1 }, Dir::W },
                  { "cn", 4, { -3, 1 }, Dir::W } } },
              4, kAmp },
            { Kind::Vccs, "VCCS", "Shift-G",
              { { { "p", 1, { 1, -2 }, Dir::N }, { "n", 2, { 1, 2 }, Dir::S }, { "cp", 3, { -3, -1 }, Dir::W },
                  { "cn", 4, { -3, 1 }, Dir::W } } },
              4, kAmp },
            { Kind::Ota, "OTA", "O",
              { { { "p", 1, { 0, 2 }, Dir::S }, { "n", 2, { 3, 0 }, Dir::E }, { "cp", 4, { -3, 1 }, Dir::W },
                  { "cn", 5, { -3, -1 }, Dir::W } } },
              4, kAmp },
            { Kind::V, "DC voltage", "V", { { { "p", 1, { -2, 0 }, Dir::W }, { "n", 2, { 2, 0 }, Dir::E } } }, 2, kTwo },
            { Kind::I, "DC current", "I", { { { "p", 1, { -2, 0 }, Dir::W }, { "n", 2, { 2, 0 }, Dir::E } } }, 2, kTwo },
            { Kind::S, "switch", "S", { { { "a", 1, { -2, 0 }, Dir::W }, { "b", 2, { 2, 0 }, Dir::E } } }, 2, kTwo },
            { Kind::Input, "input", "", { { { "", 2, { 0, 0 }, Dir::E } } }, 1, { -3, -1, 0, 1 } },
            { Kind::Output, "output", "", { { { "", 2, { 0, 0 }, Dir::W } } }, 1, { 0, -1, 3, 1 } },
            { Kind::Unknown, "part", "", {}, 0, {} },
        };
    }

    const Symbol& symbol(Kind k) noexcept { return kSymbols[static_cast<int>(k)]; }

    int pinIndex(Kind k, std::string_view name) noexcept
    {
        const Symbol& s = symbol(k);
        const bool number = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
        for (int i = 0; i < s.pinCount; ++i)
        {
            const PinDef& p = s.pins[static_cast<std::size_t>(i)];
            if (number)
            {
                if (name.size() <= 2 && std::stoi(std::string(name)) == p.token)
                    return i;
                continue;
            }
            const std::string_view n = p.name;
            if (n.size() != name.size() || n.empty())
                continue;
            bool same = true;
            for (std::size_t j = 0; j < n.size() && same; ++j)
                same = (name[j] >= 'A' && name[j] <= 'Z' ? static_cast<char>(name[j] - 'A' + 'a') : name[j]) == n[j];
            if (same)
                return i;
        }
        return -1;
    }

    char letterOf(Kind k) noexcept
    {
        switch (k)
        {
            case Kind::R: return 'R';
            case Kind::C: return 'C';
            case Kind::L: return 'L';
            case Kind::D: return 'D';
            case Kind::Pot:
            case Kind::Rheostat: return 'P';
            case Kind::OpAmp: return 'X';
            case Kind::Vcvs: return 'E';
            case Kind::Vccs:
            case Kind::Ota: return 'G';
            case Kind::V: return 'V';
            case Kind::I: return 'I';
            case Kind::S: return 'S';
            case Kind::Input:
            case Kind::Output:
            case Kind::Unknown: return 0;
        }
        return 0;
    }

    void put(Part& p, Pt origin, Orient o) noexcept
    {
        const Symbol& s = symbol(p.kind);
        p.origin = origin;
        p.orient = o;
        p.placed = true;
        p.body = turned(o, s.body).moved(origin);
        for (std::size_t k = 0; k < p.pins.size() && k < static_cast<std::size_t>(s.pinCount); ++k)
        {
            p.pins[k].at = origin + turned(o, s.pins[k].at);
            p.pins[k].out = turned(o, s.pins[k].out);
        }
    }

    Box flagBox(Pt pin, Dir out) noexcept
    {
        const Pt u = unit(out);
        const Pt side = horizontal(out) ? Pt{ 0, 1 } : Pt{ 1, 0 };
        return Box::of(pin + u - side, pin + Pt{ 2 * u.x, 2 * u.y } + side);
    }

    namespace
    {
        bool flagged(const Net& n) noexcept { return n.style == Style::ground || n.style == Style::rail; }
        bool signalNet(const Net& n) noexcept { return n.style == Style::wired || n.style == Style::flag; }
        bool tagged(const Net& n) noexcept { return !(n.style == Style::wired && !n.unroutable); }   // drawn with flags

        // A net's rank for "+ up": a positive rail above ground above a negative rail.
        int rankOf(const Net& n) noexcept
        {
            return n.style == Style::rail ? (n.positive ? 2 : 0) : 1;
        }

        int dot(Dir a, Dir b) noexcept
        {
            const Pt u = unit(a), v = unit(b);
            return u.x * v.x + u.y * v.y;
        }

        // The orientation whose pins best face where they want to (0: no wish), ties in kOrients' order.
        Orient bestOrient(Kind kind, const std::vector<std::optional<Dir>>& wanted)
        {
            const Symbol& s = symbol(kind);
            Orient best = Orient::r0;
            int bestScore = INT_MIN;
            for (const Orient o : kOrients)
            {
                int score = 0;
                for (int k = 0; k < s.pinCount && k < static_cast<int>(wanted.size()); ++k)
                    if (wanted[static_cast<std::size_t>(k)])
                        score += dot(turned(o, s.pins[static_cast<std::size_t>(k)].out), *wanted[static_cast<std::size_t>(k)]);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = o;
                }
            }
            return best;
        }

        int floorDiv(long a, long b) noexcept
        {
            long q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0)))
                --q;
            return static_cast<int>(q);
        }

        // ---- the full layout (spec 7.2-7.7) ---------------------------------------------------------------------
        class FullLayout
        {
        public:
            FullLayout(Drawing& d, std::vector<int> subset) : d_(d), subset_(std::move(subset))
            {
                in_.assign(d_.parts.size(), 0);
                for (const int p : subset_)
                    in_[static_cast<std::size_t>(p)] = 1;
                for (std::size_t k = 0; k < d_.parts.size(); ++k)
                {
                    if (d_.parts[k].kind == Kind::Input && in_[k])
                        inputPart_ = static_cast<int>(k);
                    if (d_.parts[k].kind == Kind::Output && in_[k])
                        outputPart_ = static_cast<int>(k);
                }
                inNet_ = inputPart_ >= 0 ? d_.parts[static_cast<std::size_t>(inputPart_)].pins[0].net : -1;
                outNet_ = outputPart_ >= 0 ? d_.parts[static_cast<std::size_t>(outputPart_)].pins[0].net : -1;
            }

            void run()
            {
                chains();
                classify();
                vertices();
                columns();
                orient();
                satellites();
                coordinates();
                for (const auto& [part, at, o] : world_)
                    put(d_.parts[static_cast<std::size_t>(part)], at, o);
            }

        private:
            struct Chain
            {
                std::vector<int> parts;              // from end a to end b
                std::vector<int> toward;             // each part's pin toward end a
                PinRef a, b;                         // its end pins
                int netA = -1, netB = -1;
                int first = INT_MAX;                 // its first part (line order)
            };
            enum class Role : uint8_t { core, shunt, feedback, supply };

            struct Vertex
            {
                int chain = -1;                      // a chain of two-pin parts, else
                int part = -1;                       // a part of three pins or more, or a terminal
                std::vector<PinRef> pins;            // its pins (a chain's: its ends)
                long order = 0;
                int comp = -1, depth = -1, column = 0, row = 0, discovered = 0;
                int parent = -1;
                PinRef parentPin{}, entry{};
                bool spine = false;
                std::vector<std::tuple<int, Pt, Orient>> local;   // its parts' origins in its frame, turned
                Box cell{};
                Pt at{};                             // its frame in the world
            };

            const Part& part(int p) const { return d_.parts[static_cast<std::size_t>(p)]; }
            const Net& net(int n) const { return d_.nets[static_cast<std::size_t>(n)]; }
            int netOf(PinRef r) const { return part(r.part).pins[static_cast<std::size_t>(r.pin)].net; }
            bool twoPin(int p) const { return in_[static_cast<std::size_t>(p)] && part(p).element >= 0 && symbol(part(p).kind).pinCount == 2; }
            bool isFlagged(int n) const { return n >= 0 && flagged(net(n)); }
            bool isSignal(int n) const { return n >= 0 && signalNet(net(n)); }

            bool internal(int n) const
            {
                if (!isSignal(n) || n == inNet_ || n == outNet_)
                    return false;
                const Net& x = net(n);
                if (x.totalPins != 2 || x.pins.size() != 2 || x.pins[0].part == x.pins[1].part)
                    return false;
                return twoPin(x.pins[0].part) && twoPin(x.pins[1].part);
            }

            // Chains: maximal runs of two-pin parts joined through internal nets.
            void chains()
            {
                std::vector<int> done(d_.parts.size(), 0);
                for (const int p : subset_)
                {
                    if (!twoPin(p) || done[static_cast<std::size_t>(p)])
                        continue;
                    // walk to one end: the end part reached first going through pin 0's side
                    const auto across = [&](int q, int pin) -> std::pair<int, int> {   // the part on the other side, its pin
                        const int n = part(q).pins[static_cast<std::size_t>(pin)].net;
                        if (!internal(n))
                            return { -1, -1 };
                        for (const PinRef& r : net(n).pins)
                            if (r.part != q)
                                return { r.part, r.pin };
                        return { -1, -1 };
                    };
                    // collect the run
                    std::vector<int> run{ p };
                    std::set<int> seen{ p };
                    for (int side = 0; side < 2; ++side)
                    {
                        int q = p, pin = side;
                        while (true)
                        {
                            const auto [r, rpin] = across(q, pin);
                            if (r < 0 || seen.count(r) != 0)
                                break;
                            seen.insert(r);
                            run.push_back(r);
                            q = r;
                            pin = 1 - rpin;
                        }
                    }
                    // order it from its end whose part comes first in line order
                    std::vector<int> ends;
                    for (const int q : run)
                    {
                        int links = 0;
                        for (int pin = 0; pin < 2; ++pin)
                            links += across(q, pin).first >= 0 ? 1 : 0;
                        if (links < 2)
                            ends.push_back(q);
                    }
                    Chain c;
                    const int start = ends.empty() ? *std::min_element(run.begin(), run.end()) : *std::min_element(ends.begin(), ends.end());
                    int startPin = 0;
                    if (!ends.empty())
                        startPin = across(start, 0).first >= 0 && across(start, 1).first < 0 ? 1 : 0;
                    // startPin: the end pin (its other side internal or not)
                    int q = start, toward = startPin;
                    std::set<int> walked;
                    while (q >= 0 && walked.count(q) == 0)
                    {
                        walked.insert(q);
                        c.parts.push_back(q);
                        c.toward.push_back(toward);
                        done[static_cast<std::size_t>(q)] = 1;
                        const auto [r, rpin] = across(q, 1 - toward);
                        if (r < 0 || walked.count(r) != 0)
                            break;
                        q = r;
                        toward = rpin;
                    }
                    c.a = { c.parts.front(), c.toward.front() };
                    c.b = { c.parts.back(), 1 - c.toward.back() };
                    c.netA = netOf(c.a);
                    c.netB = netOf(c.b);
                    c.first = *std::min_element(c.parts.begin(), c.parts.end());
                    chainOf_[c.parts.front()] = static_cast<int>(chains_.size());
                    for (const int x : c.parts)
                        chainOf_[x] = static_cast<int>(chains_.size());
                    chains_.push_back(std::move(c));
                }
                std::stable_sort(chains_.begin(), chains_.end(), [](const Chain& x, const Chain& y) { return x.first < y.first; });
                chainOf_.clear();
                for (std::size_t i = 0; i < chains_.size(); ++i)
                    for (const int x : chains_[i].parts)
                        chainOf_[x] = static_cast<int>(i);
            }

            // The amplifier pins of a part: its inverting input and its output (-1: not an amplifier).
            std::pair<int, int> amplifier(int p) const
            {
                const Part& x = part(p);
                switch (x.kind)
                {
                    case Kind::OpAmp: return { 1, 2 };
                    case Kind::Vcvs:
                        if (isFlagged(x.pins[1].net) && net(x.pins[1].net).style == Style::ground)
                            return { 3, 0 };
                        return { -1, -1 };
                    case Kind::Ota: return { 3, 1 };
                    case Kind::R:
                    case Kind::C:
                    case Kind::L:
                    case Kind::D:
                    case Kind::Pot:
                    case Kind::Rheostat:
                    case Kind::Vccs:
                    case Kind::V:
                    case Kind::I:
                    case Kind::S:
                    case Kind::Input:
                    case Kind::Output:
                    case Kind::Unknown: return { -1, -1 };
                }
                return { -1, -1 };
            }

            void classify()
            {
                roles_.assign(chains_.size(), Role::core);
                feedbackOf_.assign(chains_.size(), -1);
                for (std::size_t i = 0; i < chains_.size(); ++i)
                {
                    const Chain& c = chains_[i];
                    const bool fa = isFlagged(c.netA), fb = isFlagged(c.netB);
                    if (fa && fb)
                        roles_[i] = Role::supply;
                    else if (fa != fb)
                        roles_[i] = Role::shunt;
                    else
                        for (const int p : subset_)
                        {
                            const auto [inv, out] = amplifier(p);
                            if (inv < 0)
                                continue;
                            const int ni = part(p).pins[static_cast<std::size_t>(inv)].net;
                            const int no = part(p).pins[static_cast<std::size_t>(out)].net;
                            if (ni != no && ((c.netA == ni && c.netB == no) || (c.netA == no && c.netB == ni)))
                            {
                                roles_[i] = Role::feedback;
                                feedbackOf_[i] = p;
                                break;
                            }
                        }
                }
                // A part of three pins or more, all on flagged nets, is a supply too.
                for (const int p : subset_)
                {
                    if (part(p).element < 0 || symbol(part(p).kind).pinCount < 3)
                        continue;
                    const bool all = std::all_of(part(p).pins.begin(), part(p).pins.end(), [&](const PinAt& pin) { return isFlagged(pin.net); });
                    if (all)
                        supplyParts_.insert(p);
                }
                // A shunt or feedback whose net has no core pin becomes core, until nothing changes.
                for (bool changed = true; changed;)
                {
                    changed = false;
                    std::set<int> corePinNets;
                    for (const int p : subset_)
                    {
                        if (supplyParts_.count(p) != 0)
                            continue;
                        if (part(p).element < 0 || symbol(part(p).kind).pinCount >= 3)
                            for (const PinAt& pin : part(p).pins)
                                corePinNets.insert(pin.net);
                    }
                    for (std::size_t i = 0; i < chains_.size(); ++i)
                        if (roles_[i] == Role::core)
                        {
                            corePinNets.insert(chains_[i].netA);
                            corePinNets.insert(chains_[i].netB);
                        }
                    for (std::size_t i = 0; i < chains_.size(); ++i)
                    {
                        if (roles_[i] != Role::shunt)
                            continue;
                        const int s = isFlagged(chains_[i].netA) ? chains_[i].netB : chains_[i].netA;
                        if (corePinNets.count(s) == 0)
                        {
                            roles_[i] = Role::core;
                            changed = true;
                        }
                    }
                }
            }

            void vertices()
            {
                for (const int p : subset_)
                {
                    const Part& x = part(p);
                    if (x.element >= 0 && (symbol(x.kind).pinCount == 2 || supplyParts_.count(p) != 0))
                        continue;
                    Vertex v;
                    v.part = p;
                    for (std::size_t k = 0; k < x.pins.size(); ++k)
                        v.pins.push_back({ p, static_cast<int>(k) });
                    v.order = x.kind == Kind::Input ? -1 : x.kind == Kind::Output ? LONG_MAX : p;
                    verts_.push_back(std::move(v));
                }
                for (std::size_t i = 0; i < chains_.size(); ++i)
                {
                    if (roles_[i] != Role::core)
                        continue;
                    Vertex v;
                    v.chain = static_cast<int>(i);
                    v.pins = { chains_[i].a, chains_[i].b };
                    v.order = chains_[i].first;
                    verts_.push_back(std::move(v));
                }
                std::stable_sort(verts_.begin(), verts_.end(), [](const Vertex& a, const Vertex& b) { return a.order < b.order; });
                // the core pins on each signal net, in line order
                pinsOn_.assign(d_.nets.size(), {});
                std::map<std::pair<int, int>, int> vertexOfPin;
                for (std::size_t v = 0; v < verts_.size(); ++v)
                    for (const PinRef& r : verts_[v].pins)
                        vertexOfPin[{ r.part, r.pin }] = static_cast<int>(v);
                for (std::size_t n = 0; n < d_.nets.size(); ++n)
                {
                    if (!signalNet(d_.nets[n]) || internal(static_cast<int>(n)))
                        continue;
                    for (const PinRef& r : d_.nets[n].pins)
                        if (const auto it = vertexOfPin.find({ r.part, r.pin }); it != vertexOfPin.end())
                            pinsOn_[n].push_back({ it->second, r });
                }
                adj_.assign(verts_.size(), {});
                for (const auto& list : pinsOn_)
                    for (const auto& [v, r] : list)
                        for (const auto& [w, q] : list)
                            if (v != w)
                                adj_[static_cast<std::size_t>(v)].insert(w);
            }

            int vertexOf(int p) const
            {
                for (std::size_t v = 0; v < verts_.size(); ++v)
                    if (verts_[v].part == p)
                        return static_cast<int>(v);
                return -1;
            }

            void columns()
            {
                // components: the input's first, then by their first vertex
                int comps = 0;
                int discovered = 0;
                std::vector<int> roots;
                const int input = inputPart_ >= 0 ? vertexOf(inputPart_) : -1;
                const int output = outputPart_ >= 0 ? vertexOf(outputPart_) : -1;
                std::vector<int> starts;
                if (input >= 0)
                    starts.push_back(input);
                for (std::size_t v = 0; v < verts_.size(); ++v)
                    starts.push_back(static_cast<int>(v));
                for (const int root : starts)
                {
                    if (verts_[static_cast<std::size_t>(root)].comp >= 0)
                        continue;
                    const int comp = comps++;
                    std::vector<int> queue{ root };
                    Vertex& r = verts_[static_cast<std::size_t>(root)];
                    r.comp = comp;
                    r.depth = 0;
                    r.discovered = discovered++;
                    int maxDepth = 0;
                    for (std::size_t qi = 0; qi < queue.size(); ++qi)
                    {
                        const int v = queue[qi];
                        if (v == output && v != root)
                            continue;                     // the output is not expanded: it goes past the deepest
                        const Vertex& vv = verts_[static_cast<std::size_t>(v)];
                        for (const PinRef& p : vv.pins)
                        {
                            const int n = netOf(p);
                            if (n < 0 || !isSignal(n))
                                continue;
                            for (const auto& [w, q] : pinsOn_[static_cast<std::size_t>(n)])
                            {
                                Vertex& ww = verts_[static_cast<std::size_t>(w)];
                                if (w == v || ww.comp >= 0)
                                    continue;
                                ww.comp = comp;
                                ww.depth = vv.depth + 1;
                                ww.parent = v;
                                ww.parentPin = p;
                                ww.discovered = discovered++;
                                // its lowest-index pin on that net
                                ww.entry = q;
                                for (const PinRef& e : ww.pins)
                                    if (netOf(e) == n)
                                    {
                                        ww.entry = e;
                                        break;
                                    }
                                if (w != output)
                                    maxDepth = std::max(maxDepth, ww.depth);
                                queue.push_back(w);
                            }
                        }
                    }
                    if (output >= 0 && verts_[static_cast<std::size_t>(output)].comp == comp && output != root)
                        verts_[static_cast<std::size_t>(output)].depth = maxDepth + 1;
                    roots.push_back(root);
                }
                comps_ = comps;
                // the spine: on a shortest path from the input to the output
                if (input >= 0 && output >= 0 && verts_[static_cast<std::size_t>(output)].comp == verts_[static_cast<std::size_t>(input)].comp)
                {
                    std::vector<int> distIn(verts_.size(), -1), distOut(verts_.size(), -1);
                    const auto bfs = [&](int from, std::vector<int>& dist) {
                        std::vector<int> q{ from };
                        dist[static_cast<std::size_t>(from)] = 0;
                        for (std::size_t i = 0; i < q.size(); ++i)
                            for (const int w : adj_[static_cast<std::size_t>(q[i])])
                                if (dist[static_cast<std::size_t>(w)] < 0)
                                {
                                    dist[static_cast<std::size_t>(w)] = dist[static_cast<std::size_t>(q[i])] + 1;
                                    q.push_back(w);
                                }
                    };
                    bfs(input, distIn);
                    bfs(output, distOut);
                    const int total = distIn[static_cast<std::size_t>(output)];
                    for (std::size_t v = 0; v < verts_.size(); ++v)
                        verts_[v].spine = distIn[v] >= 0 && distOut[v] >= 0 && distIn[v] + distOut[v] == total;
                }
                for (Vertex& v : verts_)
                    v.column = std::max(0, v.depth);
                // order within columns: the spine first, then discovery order; then three barycentre sweeps
                cols_.assign(static_cast<std::size_t>(comps), {});
                for (std::size_t v = 0; v < verts_.size(); ++v)
                {
                    auto& cc = cols_[static_cast<std::size_t>(verts_[v].comp)];
                    if (static_cast<int>(cc.size()) <= verts_[v].column)
                        cc.resize(static_cast<std::size_t>(verts_[v].column + 1));
                    cc[static_cast<std::size_t>(verts_[v].column)].push_back(static_cast<int>(v));
                }
                for (auto& cc : cols_)
                {
                    for (auto& col : cc)
                        std::stable_sort(col.begin(), col.end(), [&](int a, int b) {
                            const Vertex& x = verts_[static_cast<std::size_t>(a)];
                            const Vertex& y = verts_[static_cast<std::size_t>(b)];
                            if (x.spine != y.spine)
                                return x.spine;
                            return x.discovered < y.discovered;
                        });
                    const auto rows = [&] {
                        for (auto& col : cc)
                            for (std::size_t i = 0; i < col.size(); ++i)
                                verts_[static_cast<std::size_t>(col[i])].row = static_cast<int>(i);
                    };
                    rows();
                    const auto sweep = [&](int c, int other) {
                        if (other < 0 || other >= static_cast<int>(cc.size()))
                            return;
                        auto& col = cc[static_cast<std::size_t>(c)];
                        struct Key
                        {
                            int64_t sum, count;
                        };
                        std::map<int, Key> key;
                        for (const int v : col)
                        {
                            Key k{ 0, 0 };
                            for (const int w : adj_[static_cast<std::size_t>(v)])
                                if (verts_[static_cast<std::size_t>(w)].column == other && verts_[static_cast<std::size_t>(w)].comp == verts_[static_cast<std::size_t>(v)].comp)
                                {
                                    k.sum += verts_[static_cast<std::size_t>(w)].row;
                                    ++k.count;
                                }
                            if (k.count == 0)
                                k = { verts_[static_cast<std::size_t>(v)].row, 1 };
                            key[v] = k;
                        }
                        std::stable_sort(col.begin(), col.end(), [&](int a, int b) {
                            const Key x = key[a], y = key[b];
                            return x.sum * y.count < y.sum * x.count;
                        });
                        for (std::size_t i = 0; i < col.size(); ++i)
                            verts_[static_cast<std::size_t>(col[i])].row = static_cast<int>(i);
                    };
                    const int n = static_cast<int>(cc.size());
                    for (int c = 1; c < n; ++c)
                        sweep(c, c - 1);
                    for (int c = n - 2; c >= 0; --c)
                        sweep(c, c + 1);
                    for (int c = 1; c < n; ++c)
                        sweep(c, c - 1);
                }
            }

            // Each pin's wish (spec 7.5) for a core vertex's part.
            std::optional<Dir> wish(int v, int n) const
            {
                if (n < 0)
                    return std::nullopt;
                if (isFlagged(n))
                    return net(n).style == Style::rail && net(n).positive ? Dir::N : Dir::S;
                const Vertex& x = verts_[static_cast<std::size_t>(v)];
                bool earlier = n == inNet_ && part(x.part).kind != Kind::Input, later = false;
                for (const auto& [w, r] : pinsOn_[static_cast<std::size_t>(n)])
                {
                    if (w == v)
                        continue;
                    const Vertex& y = verts_[static_cast<std::size_t>(w)];
                    if (y.comp != x.comp)
                        continue;
                    earlier = earlier || y.column < x.column;
                    later = later || y.column > x.column;
                }
                if (earlier && !later)
                    return Dir::W;
                if (later && !earlier)
                    return Dir::E;
                return std::nullopt;
            }

            void orient()
            {
                for (std::size_t v = 0; v < verts_.size(); ++v)
                {
                    Vertex& x = verts_[v];
                    if (x.chain >= 0)
                    {
                        Chain& c = chains_[static_cast<std::size_t>(x.chain)];
                        if (x.parent >= 0 && x.entry.part == c.b.part && x.entry.pin == c.b.pin && !(c.a.part == c.b.part && c.a.pin == c.b.pin))
                        {
                            // the entry on the left: the chain turned round
                            std::reverse(c.parts.begin(), c.parts.end());
                            std::reverse(c.toward.begin(), c.toward.end());
                            for (int& t : c.toward)
                                t = 1 - t;
                            std::swap(c.a, c.b);
                            std::swap(c.netA, c.netB);
                            x.pins = { c.a, c.b };
                        }
                        for (std::size_t i = 0; i < c.parts.size(); ++i)
                            x.local.emplace_back(c.parts[i], Pt{ 6 * static_cast<int>(i) + 2, 0 }, c.toward[i] == 0 ? Orient::r0 : Orient::r180);
                        facing_[{ c.a.part, c.a.pin }] = Dir::W;
                        facing_[{ c.b.part, c.b.pin }] = Dir::E;
                        continue;
                    }
                    const Part& p = part(x.part);
                    Orient o = Orient::r0;
                    if (p.element >= 0)
                    {
                        std::vector<std::optional<Dir>> wanted;
                        for (const PinAt& pin : p.pins)
                            wanted.push_back(wish(static_cast<int>(v), pin.net));
                        o = bestOrient(p.kind, wanted);
                    }
                    x.local.emplace_back(x.part, Pt{ 0, 0 }, o);
                    const Symbol& s = symbol(p.kind);
                    for (int k = 0; k < s.pinCount; ++k)
                        facing_[{ x.part, k }] = turned(o, s.pins[static_cast<std::size_t>(k)].out);
                }
            }

            // A part's pin in its vertex's frame.
            static Pt pinAt(Kind kind, Pt origin, Orient o, int pin)
            {
                return origin + turned(o, symbol(kind).pins[static_cast<std::size_t>(pin)].at);
            }

            void satellites()
            {
                // the anchor of each signal net: its E-facing core pin in the earliest column, else its earliest core pin
                const auto anchorOf = [&](int s) -> std::pair<int, PinRef> {
                    std::pair<int, PinRef> best{ -1, {} }, east{ -1, {} };
                    const auto earlier = [&](int v, int w) {
                        const Vertex& a = verts_[static_cast<std::size_t>(v)];
                        const Vertex& b = verts_[static_cast<std::size_t>(w)];
                        return a.comp != b.comp ? a.comp < b.comp : a.column != b.column ? a.column < b.column : a.row < b.row;
                    };
                    for (const auto& [v, r] : pinsOn_[static_cast<std::size_t>(s)])
                    {
                        if (best.first < 0 || earlier(v, best.first))
                            best = { v, r };
                        const auto f = facing_.find({ r.part, r.pin });
                        if (f != facing_.end() && f->second == Dir::E && (east.first < 0 || earlier(v, east.first)))
                            east = { v, r };
                    }
                    return east.first >= 0 ? east : best;
                };
                // shunts, by their anchor pin
                std::map<std::pair<int, int>, std::vector<int>> shunts;   // (part, pin) -> chains
                std::map<std::pair<int, int>, int> shuntVertex;
                for (std::size_t i = 0; i < chains_.size(); ++i)
                {
                    if (roles_[i] != Role::shunt)
                        continue;
                    Chain& c = chains_[i];
                    if (isFlagged(c.netA))
                    {
                        // ordered from s outward
                        std::reverse(c.parts.begin(), c.parts.end());
                        std::reverse(c.toward.begin(), c.toward.end());
                        for (int& t : c.toward)
                            t = 1 - t;
                        std::swap(c.a, c.b);
                        std::swap(c.netA, c.netB);
                    }
                    const auto [v, r] = anchorOf(c.netA);
                    if (v < 0)
                        continue;
                    shunts[{ r.part, r.pin }].push_back(static_cast<int>(i));
                    shuntVertex[{ r.part, r.pin }] = v;
                }
                for (auto& [pin, list] : shunts)
                {
                    std::stable_sort(list.begin(), list.end(), [&](int a, int b) {
                        return naturalLess(part(chains_[static_cast<std::size_t>(a)].parts.front()).name,
                                           part(chains_[static_cast<std::size_t>(b)].parts.front()).name);
                    });
                    // antiparallel twins (two diodes, the same nets, reversed) side by side
                    std::vector<int> ordered;
                    std::vector<int> left = list;
                    while (!left.empty())
                    {
                        const int c = left.front();
                        left.erase(left.begin());
                        ordered.push_back(c);
                        const Chain& x = chains_[static_cast<std::size_t>(c)];
                        if (x.parts.size() != 1 || part(x.parts[0]).kind != Kind::D)
                            continue;
                        for (std::size_t k = 0; k < left.size(); ++k)
                        {
                            const Chain& y = chains_[static_cast<std::size_t>(left[k])];
                            if (y.parts.size() == 1 && part(y.parts[0]).kind == Kind::D && y.netB == x.netB && y.toward[0] != x.toward[0])
                            {
                                ordered.push_back(left[k]);
                                left.erase(left.begin() + static_cast<std::ptrdiff_t>(k));
                                break;
                            }
                        }
                    }
                    list = ordered;
                }
                // feedback rows, by amplifier
                std::map<int, std::vector<int>> rows;
                for (std::size_t i = 0; i < chains_.size(); ++i)
                    if (roles_[i] == Role::feedback)
                        rows[feedbackOf_[i]].push_back(static_cast<int>(i));
                for (auto& [amp, list] : rows)
                {
                    const auto [inv, out] = amplifier(amp);
                    const int invNet = part(amp).pins[static_cast<std::size_t>(inv)].net;
                    for (const int i : list)
                    {
                        Chain& c = chains_[static_cast<std::size_t>(i)];
                        if (c.netA != invNet)
                        {
                            std::reverse(c.parts.begin(), c.parts.end());
                            std::reverse(c.toward.begin(), c.toward.end());
                            for (int& t : c.toward)
                                t = 1 - t;
                            std::swap(c.a, c.b);
                            std::swap(c.netA, c.netB);
                        }
                    }
                    std::stable_sort(list.begin(), list.end(), [&](int a, int b) {
                        const Chain& x = chains_[static_cast<std::size_t>(a)];
                        const Chain& y = chains_[static_cast<std::size_t>(b)];
                        return x.parts.size() != y.parts.size() ? x.parts.size() < y.parts.size() : x.first < y.first;
                    });
                }
                // each vertex's satellites in its frame, then its cell
                for (Vertex& v : verts_)
                {
                    const std::vector<std::tuple<int, Pt, Orient>> own = v.local;
                    for (const auto& [p, o, r] : own)
                    {
                        const Part& x = part(p);
                        const Box body = turned(r, symbol(x.kind).body).moved(o);
                        for (int k = 0; k < symbol(x.kind).pinCount; ++k)
                        {
                            const auto it = shunts.find({ p, k });
                            if (it == shunts.end())
                                continue;
                            const Pt at = pinAt(x.kind, o, r, k);
                            const Dir face = turned(r, symbol(x.kind).pins[static_cast<std::size_t>(k)].out);
                            for (std::size_t j = 0; j < it->second.size(); ++j)
                            {
                                const Chain& c = chains_[static_cast<std::size_t>(it->second[j])];
                                const int sx = at.x + (face == Dir::W ? -2 - 4 * static_cast<int>(j) : 2 + 4 * static_cast<int>(j));
                                const bool up = net(c.netB).style == Style::rail && net(c.netB).positive;
                                for (std::size_t i = 0; i < c.parts.size(); ++i)
                                {
                                    const int step = 6 * static_cast<int>(i);
                                    if (!up)
                                    {
                                        const int top = body.y1 + 2 + step;
                                        v.local.emplace_back(c.parts[i], Pt{ sx, top + 2 }, c.toward[i] == 0 ? Orient::r90 : Orient::r270);
                                    }
                                    else
                                    {
                                        const int bottom = body.y0 - 2 - step;
                                        v.local.emplace_back(c.parts[i], Pt{ sx, bottom - 2 }, c.toward[i] == 0 ? Orient::r270 : Orient::r90);
                                    }
                                }
                            }
                        }
                        const auto rit = rows.find(p);
                        if (rit == rows.end())
                            continue;
                        for (std::size_t k = 0; k < rit->second.size(); ++k)
                        {
                            const Chain& c = chains_[static_cast<std::size_t>(rit->second[k])];
                            const int y = body.y0 - 3 * static_cast<int>(k + 1);
                            const int left = o.x - (3 * static_cast<int>(c.parts.size()) - 1);
                            for (std::size_t i = 0; i < c.parts.size(); ++i)
                                v.local.emplace_back(c.parts[i], Pt{ left + 6 * static_cast<int>(i) + 2, y },
                                                     c.toward[i] == 0 ? Orient::r0 : Orient::r180);
                        }
                    }
                    v.cell = cellOf(v.local);
                }
            }

            Box cellOf(const std::vector<std::tuple<int, Pt, Orient>>& parts) const
            {
                Box cell;
                for (const auto& [p, o, r] : parts)
                {
                    const Part& x = part(p);
                    cell = cell.joined(turned(r, symbol(x.kind).body).moved(o));
                    for (int k = 0; k < symbol(x.kind).pinCount; ++k)
                    {
                        const int n = x.pins[static_cast<std::size_t>(k)].net;
                        if (n >= 0 && tagged(net(n)))
                            cell = cell.joined(flagBox(pinAt(x.kind, o, r, k), turned(r, symbol(x.kind).pins[static_cast<std::size_t>(k)].out)));
                    }
                }
                return cell;
            }

            void coordinates()
            {
                std::vector<Box> compBox(static_cast<std::size_t>(comps_));
                for (int comp = 0; comp < comps_; ++comp)
                {
                    const auto& cc = cols_[static_cast<std::size_t>(comp)];
                    int x = 0;
                    for (const auto& col : cc)
                    {
                        int width = 0;
                        for (const int v : col)
                            width = std::max(width, verts_[static_cast<std::size_t>(v)].cell.x1 - verts_[static_cast<std::size_t>(v)].cell.x0);
                        int prevBottom = INT_MIN;
                        for (const int vi : col)
                        {
                            Vertex& v = verts_[static_cast<std::size_t>(vi)];
                            v.at.x = x - v.cell.x0;
                            int desired = 0;
                            if (v.parent >= 0)
                            {
                                const Vertex& p = verts_[static_cast<std::size_t>(v.parent)];
                                const Pt pp = p.at + localPin(p, v.parentPin);
                                const Pt ep = localPin(v, v.entry);
                                const auto f = facing_.find({ v.entry.part, v.entry.pin });
                                const int shift = f == facing_.end() ? 0 : f->second == Dir::N ? 2 : f->second == Dir::S ? -2 : 0;
                                desired = pp.y + shift - ep.y;
                            }
                            v.at.y = prevBottom == INT_MIN ? desired : std::max(desired, prevBottom + 2 - v.cell.y0);
                            prevBottom = v.at.y + v.cell.y1;
                            compBox[static_cast<std::size_t>(comp)] = compBox[static_cast<std::size_t>(comp)].joined(v.cell.moved(v.at));
                        }
                        x += width + 4;
                    }
                }
                // components stacked 6 apart, left edges aligned
                std::vector<Pt> shift(static_cast<std::size_t>(comps_));
                Box all;
                for (int comp = 0; comp < comps_; ++comp)
                {
                    const Box b = compBox[static_cast<std::size_t>(comp)];
                    if (comp == 0 || all.empty())
                        shift[static_cast<std::size_t>(comp)] = { 0, 0 };
                    else
                        shift[static_cast<std::size_t>(comp)] = { all.x0 - b.x0, all.y1 + 6 - b.y0 };
                    all = all.joined(b.moved(shift[static_cast<std::size_t>(comp)]));
                }
                for (Vertex& v : verts_)
                {
                    v.at = v.at + shift[static_cast<std::size_t>(v.comp)];
                    for (const auto& [p, o, r] : v.local)
                        world_.emplace_back(p, v.at + o, r);
                }
                // supplies: a strip 4 above everything, in line order
                std::vector<std::pair<int, std::vector<std::tuple<int, Pt, Orient>>>> supplies;   // (line order, parts)
                for (std::size_t i = 0; i < chains_.size(); ++i)
                {
                    if (roles_[i] != Role::supply)
                        continue;
                    Chain c = chains_[i];
                    if (rankOf(net(c.netA)) < rankOf(net(c.netB)))
                    {
                        std::reverse(c.parts.begin(), c.parts.end());
                        std::reverse(c.toward.begin(), c.toward.end());
                        for (int& t : c.toward)
                            t = 1 - t;
                    }
                    std::vector<std::tuple<int, Pt, Orient>> local;
                    for (std::size_t k = 0; k < c.parts.size(); ++k)
                        local.emplace_back(c.parts[k], Pt{ 0, 6 * static_cast<int>(k) + 2 }, c.toward[k] == 0 ? Orient::r90 : Orient::r270);
                    supplies.emplace_back(c.first, std::move(local));
                }
                for (const int p : supplyParts_)
                    supplies.push_back({ p, { { p, Pt{ 0, 0 }, Orient::r0 } } });
                std::stable_sort(supplies.begin(), supplies.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                if (!supplies.empty())
                {
                    const int top = all.empty() ? 0 : all.y0;
                    int cursor = all.empty() ? 0 : all.x0;
                    for (const auto& [order, local] : supplies)
                    {
                        const Box cell = cellOf(local);
                        const Pt at{ cursor - cell.x0, top - 4 - cell.y1 };
                        for (const auto& [p, o, r] : local)
                            world_.emplace_back(p, at + o, r);
                        cursor += cell.x1 - cell.x0 + 2;
                    }
                }
                // the input's pin at 0,0
                if (inputPart_ >= 0)
                {
                    Pt pin{};
                    for (const auto& [p, o, r] : world_)
                        if (p == inputPart_)
                            pin = o;
                    for (auto& [p, o, r] : world_)
                        o = o - pin;
                }
            }

            // A vertex's pin in its frame.
            Pt localPin(const Vertex& v, PinRef r) const
            {
                for (const auto& [p, o, rr] : v.local)
                    if (p == r.part)
                        return pinAt(part(p).kind, o, rr, r.pin);
                return {};
            }

            Drawing& d_;
            std::vector<int> subset_;
            std::vector<char> in_;
            int inputPart_ = -1, outputPart_ = -1, inNet_ = -1, outNet_ = -1;
            std::vector<Chain> chains_;
            std::map<int, int> chainOf_;
            std::vector<Role> roles_;
            std::vector<int> feedbackOf_;
            std::set<int> supplyParts_;
            std::vector<Vertex> verts_;
            std::vector<std::vector<std::pair<int, PinRef>>> pinsOn_;
            std::vector<std::set<int>> adj_;
            std::map<std::pair<int, int>, Dir> facing_;   // a core pin's direction once oriented
            int comps_ = 0;
            std::vector<std::vector<std::vector<int>>> cols_;   // per component, per column, its vertices in order
            std::vector<std::tuple<int, Pt, Orient>> world_;
        };
    }

    void layoutAll(Drawing& d, const Netlist& nl)
    {
        std::vector<int> all;
        for (std::size_t k = 0; k < d.parts.size(); ++k)
        {
            d.parts[k].placed = false;
            all.push_back(static_cast<int>(k));
        }
        for (Net& n : d.nets)
        {
            n.edges.clear();
            n.labels.clear();
        }
        FullLayout(d, all).run();
        if (std::any_of(d.parts.begin(), d.parts.end(), [](const Part& p) { return !p.placed; }))
            placeRest(d, nl);                             // never expected: whatever the classes left out
        for (Part& p : d.parts)
            p.source = Source::placed;
    }

    // ---- incremental placement (spec 7.9) -------------------------------------------------------------------------
    namespace
    {
        // What a placement must not meet: bodies, flags, pins and route points.
        struct Taken
        {
            std::vector<Box> boxes;
            std::set<Pt> points;

            void add(const Drawing& d)
            {
                boxes.clear();
                points.clear();
                for (const Part& p : d.parts)
                {
                    if (!p.placed)
                        continue;
                    boxes.push_back(p.body);
                    for (const PinAt& pin : p.pins)
                    {
                        points.insert(pin.at);
                        if (pin.net >= 0 && tagged(d.nets[static_cast<std::size_t>(pin.net)]))
                            boxes.push_back(flagBox(pin.at, pin.out));
                    }
                }
                for (const Net& n : d.nets)
                    for (const UnitEdge& e : n.edges)
                    {
                        points.insert(e.a);
                        points.insert(e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 }));
                    }
            }

            bool meets(const Box& b) const
            {
                for (const Box& x : boxes)
                    if (x.meets(b))
                        return true;
                for (auto it = points.lower_bound({ b.x0, INT_MIN }); it != points.end() && it->x <= b.x1; ++it)
                    if (b.contains(*it))
                        return true;
                return false;
            }
        };
    }

    void placeRest(Drawing& d, const Netlist& nl)
    {
        (void)nl;
        std::vector<int> rest;
        for (std::size_t k = 0; k < d.parts.size(); ++k)
            if (!d.parts[k].placed)
                rest.push_back(static_cast<int>(k));
        std::stable_sort(rest.begin(), rest.end(), [&](int a, int b) { return naturalLess(d.parts[static_cast<std::size_t>(a)].name, d.parts[static_cast<std::size_t>(b)].name); });
        Taken taken;
        // A stored wire counts where the router's grid can hold it (the placed parts' box and 18 around it): one drawn
        // far out by hand neither pulls a part nor pushes an island away (the router drops it).
        Box near;
        for (const Part& p : d.parts)
            if (p.placed)
                near = near.joined(p.body);
        near = near.grown(18);
        const auto nearEdge = [&](const UnitEdge& e) { return near.contains(e.a) && near.contains(e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 })); };
        const auto anchorsOf = [&](const Part& p, int net) {
            std::vector<Pt> out;
            const Net& n = d.nets[static_cast<std::size_t>(net)];
            for (const PinRef& r : n.pins)
            {
                const Part& q = d.parts[static_cast<std::size_t>(r.part)];
                if (q.placed && &q != &p)
                    out.push_back(q.pins[static_cast<std::size_t>(r.pin)].at);
            }
            for (const UnitEdge& e : n.edges)
                if (nearEdge(e))
                {
                    out.push_back(e.a);
                    out.push_back(e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 }));
                }
            return out;
        };
        while (!rest.empty())
        {
            bool placedOne = false;
            for (std::size_t i = 0; i < rest.size(); ++i)
            {
                Part& p = d.parts[static_cast<std::size_t>(rest[i])];
                std::vector<Pt> all;
                std::vector<std::vector<Pt>> perPin(p.pins.size());
                bool ground = false, positive = false;
                for (std::size_t k = 0; k < p.pins.size(); ++k)
                {
                    const int n = p.pins[k].net;
                    if (n < 0)
                        continue;
                    const Net& net = d.nets[static_cast<std::size_t>(n)];
                    if (flagged(net))
                    {
                        ground = ground || !(net.style == Style::rail && net.positive);
                        positive = positive || (net.style == Style::rail && net.positive);
                        continue;
                    }
                    perPin[k] = anchorsOf(p, n);
                    all.insert(all.end(), perPin[k].begin(), perPin[k].end());
                }
                if (all.empty())
                    continue;
                long sx = 0, sy = 0;
                for (const Pt a : all)
                {
                    sx += a.x;
                    sy += a.y;
                }
                const Pt centre{ floorDiv(sx, static_cast<long>(all.size())), floorDiv(sy, static_cast<long>(all.size())) };
                const Pt target = centre + Pt{ 0, ground && !positive ? 4 : positive && !ground ? -4 : 0 };
                Orient o = Orient::r0;
                if (p.element >= 0)
                {
                    std::vector<std::optional<Dir>> wanted(p.pins.size());
                    for (std::size_t k = 0; k < p.pins.size(); ++k)
                    {
                        const int n = p.pins[k].net;
                        if (n < 0)
                            continue;
                        const Net& net = d.nets[static_cast<std::size_t>(n)];
                        if (flagged(net))
                        {
                            wanted[k] = net.style == Style::rail && net.positive ? Dir::N : Dir::S;
                            continue;
                        }
                        if (perPin[k].empty())
                            continue;
                        long cx = 0, cy = 0;
                        for (const Pt a : perPin[k])
                        {
                            cx += a.x;
                            cy += a.y;
                        }
                        const long dx = cx - static_cast<long>(target.x) * static_cast<long>(perPin[k].size());
                        const long dy = cy - static_cast<long>(target.y) * static_cast<long>(perPin[k].size());
                        if (dx == 0 && dy == 0)
                            continue;
                        wanted[k] = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy) ? (dx >= 0 ? Dir::E : Dir::W) : (dy >= 0 ? Dir::S : Dir::N);
                    }
                    o = bestOrient(p.kind, wanted);
                }
                taken.add(d);
                Part trial = p;
                bool found = false;
                for (int r = 0; r <= 40 && !found; ++r)
                    for (int dy = -r; dy <= r && !found; ++dy)
                    {
                        const int ax = r - (dy < 0 ? -dy : dy);
                        for (const int dx : { -ax, ax })
                        {
                            put(trial, target + Pt{ dx, dy }, o);
                            bool clear = withinReach(trial.origin) && !taken.meets(trial.body.grown(1));
                            for (const PinAt& pin : trial.pins)
                                if (clear && pin.net >= 0 && tagged(d.nets[static_cast<std::size_t>(pin.net)]))
                                    clear = !taken.meets(flagBox(pin.at, pin.out).grown(1));
                            if (clear)
                            {
                                found = true;
                                break;
                            }
                            if (ax == 0)
                                break;
                        }
                    }
                if (!found)
                    put(trial, target, o);                // nowhere clear within 40: drawn, and its overlap flagged
                p = trial;
                p.source = Source::placed;
                rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(i));
                placedOne = true;
                break;
            }
            if (placedOne)
                continue;
            // Islands: the rest, grouped by the signal nets they share, each laid out on its own, 6 right of the
            // drawing, top-aligned.
            std::vector<int> group{ rest.front() };
            for (std::size_t g = 0; g < group.size(); ++g)
                for (const PinAt& pin : d.parts[static_cast<std::size_t>(group[g])].pins)
                {
                    if (pin.net < 0 || !signalNet(d.nets[static_cast<std::size_t>(pin.net)]))
                        continue;
                    for (const PinRef& r : d.nets[static_cast<std::size_t>(pin.net)].pins)
                        if (!d.parts[static_cast<std::size_t>(r.part)].placed && std::find(group.begin(), group.end(), r.part) == group.end())
                            group.push_back(r.part);
                }
            std::sort(group.begin(), group.end());
            Box before;
            for (const Part& p : d.parts)
                if (p.placed)
                    before = before.joined(p.body);
            for (const Net& n : d.nets)
                for (const UnitEdge& e : n.edges)
                    if (nearEdge(e))
                        before = before.joined(Box::of(e.a, e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 })));
            FullLayout(d, group).run();
            Box island;
            for (const int k : group)
                island = island.joined(d.parts[static_cast<std::size_t>(k)].body);
            const Pt shift = before.empty() ? Pt{ 0, 0 } : Pt{ before.x1 + 6 - island.x0, before.y0 - island.y0 };
            for (const int k : group)
            {
                Part& p = d.parts[static_cast<std::size_t>(k)];
                put(p, p.origin + shift, p.orient);
                p.source = Source::placed;
                rest.erase(std::find(rest.begin(), rest.end(), k));
            }
        }
    }

    // ---- routing (spec 7.8) -----------------------------------------------------------------------------------------
    namespace
    {
        class Router
        {
        public:
            Router(Drawing& d, Budget& budget) : d_(d), budget_(budget) {}

            void run()
            {
                // A net that cannot be routed is drawn as name tags, and their flags are walls: the pass runs again
                // from what it reached (each time one net more is tags, so it ends), so that no wire accepted runs
                // where a flag came later, and a drawing read back routes the same.
                for (bool again = true; again;)
                {
                    order_.clear();
                    order();
                    grid();
                    if (wide_)
                    {
                        // the parts span more than the grid holds: no net is routed, every wired one is name tags
                        budget_.wide = true;
                        for (Net& net : d_.nets)
                            if (net.style == Style::wired && !net.unroutable)
                            {
                                net.edges.clear();
                                net.unroutable = net.pins.size() >= 2;
                            }
                        return;
                    }
                    // 1. drop what is no longer valid, each net against those before it
                    for (const int n : order_)
                        validate(n);
                    // 2. prune dangling ends and pieces without a pin
                    for (const int n : order_)
                        prune(n);
                    // 3. connect
                    again = false;
                    for (const int n : order_)
                    {
                        const bool was = d_.nets[static_cast<std::size_t>(n)].unroutable;
                        connect(n);
                        again = again || (!was && d_.nets[static_cast<std::size_t>(n)].unroutable);
                    }
                    for (const int n : order_)
                    {
                        Net& net = d_.nets[static_cast<std::size_t>(n)];
                        net.edges.assign(edges_[static_cast<std::size_t>(n)].begin(), edges_[static_cast<std::size_t>(n)].end());
                    }
                }
            }

        private:
            static constexpr int kNone = 4;           // no heading

            // ---- the grid ------------------------------------------------------------------------------------------
            bool inside(Pt p) const noexcept { return p.x >= area_.x0 && p.x <= area_.x1 && p.y >= area_.y0 && p.y <= area_.y1; }
            std::size_t at(Pt p) const noexcept
            {
                return static_cast<std::size_t>(p.y - area_.y0) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(p.x - area_.x0);
            }

            void order()
            {
                struct Key
                {
                    long half;
                    std::size_t pins;
                    int net;
                };
                std::vector<Key> keys;
                for (std::size_t n = 0; n < d_.nets.size(); ++n)
                {
                    Net& net = d_.nets[n];
                    if (net.style != Style::wired || net.unroutable)
                    {
                        net.edges.clear();
                        continue;
                    }
                    Box b;
                    for (const PinRef& r : net.pins)
                        b = b.joined(Box::of(pinOf(r).at, pinOf(r).at));
                    keys.push_back({ b.empty() ? 0L : static_cast<long>(b.x1 - b.x0) + (b.y1 - b.y0), net.pins.size(), static_cast<int>(n) });
                }
                std::stable_sort(keys.begin(), keys.end(), [&](const Key& a, const Key& b) {
                    if (a.half != b.half)
                        return a.half < b.half;
                    if (a.pins != b.pins)
                        return a.pins < b.pins;
                    return naturalLess(d_.nets[static_cast<std::size_t>(a.net)].name, d_.nets[static_cast<std::size_t>(b.net)].name);
                });
                for (const Key& k : keys)
                    order_.push_back(k.net);
            }

            const PinAt& pinOf(PinRef r) const
            {
                return d_.parts[static_cast<std::size_t>(r.part)].pins[static_cast<std::size_t>(r.pin)];
            }

            // The grid: the drawing's box (its bodies and stored wires) and 18 around it, within what a line can say
            // (kCoordLimit). Past kRouteCells points, the parts' box alone (a stored wire outside its 18 is dropped by
            // validation); past them still, no grid at all (wide_: nothing is routed).
            void grid()
            {
                const auto areaOf = [](Box b) {
                    if (b.empty())
                        b = { 0, 0, 0, 0 };
                    const Box g = b.grown(18);
                    return Box{ std::max(g.x0, -kCoordLimit), std::max(g.y0, -kCoordLimit), std::min(g.x1, kCoordLimit),
                                std::min(g.y1, kCoordLimit) };
                };
                const auto pointsOf = [](const Box& b) { return static_cast<long>(b.x1 - b.x0 + 1) * static_cast<long>(b.y1 - b.y0 + 1); };
                Box parts;
                for (const Part& p : d_.parts)
                    if (p.placed)
                        parts = parts.joined(p.body);
                Box b = parts;
                for (const Net& n : d_.nets)
                    for (const UnitEdge& e : n.edges)
                        b = b.joined(Box::of(e.a, e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 })));
                if (pointsOf(areaOf(b)) > kRouteCells)
                    b = parts;
                if (b.empty())
                    b = { 0, 0, 0, 0 };
                bounds_ = b;
                area_ = areaOf(b);
                wide_ = pointsOf(area_) > kRouteCells;  // the parts' box alone decides it: the first pass's
                budget_.cells = wide_ ? 0 : pointsOf(area_);
                if (wide_)
                    return;
                w_ = area_.x1 - area_.x0 + 1;
                h_ = area_.y1 - area_.y0 + 1;
                const std::size_t cells = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
                wall_.assign(cells, 0);
                pinNet_.assign(cells, -1);
                pinOut_.assign(cells, 0);
                hEdge_.assign(cells, -1);
                vEdge_.assign(cells, -1);
                const auto fill = [&](const Box& box) {
                    for (int y = std::max(box.y0, area_.y0); y <= std::min(box.y1, area_.y1); ++y)
                        for (int x = std::max(box.x0, area_.x0); x <= std::min(box.x1, area_.x1); ++x)
                            wall_[at({ x, y })] = 1;
                };
                for (const Part& p : d_.parts)
                {
                    if (!p.placed)
                        continue;
                    fill(p.body);
                    for (const PinAt& pin : p.pins)
                    {
                        if (inside(pin.at))
                        {
                            pinNet_[at(pin.at)] = pin.net;
                            pinOut_[at(pin.at)] = static_cast<int8_t>(pin.out);
                        }
                        if (pin.net >= 0 && tagged(d_.nets[static_cast<std::size_t>(pin.net)]))
                            fill(flagBox(pin.at, pin.out));
                    }
                }
                edges_.assign(d_.nets.size(), {});
                for (std::size_t n = 0; n < d_.nets.size(); ++n)
                    if (d_.nets[n].style == Style::wired && !d_.nets[n].unroutable)
                        edges_[n].insert(d_.nets[n].edges.begin(), d_.nets[n].edges.end());
            }

            // the net owning edge (p, p + d)
            int edgeNet(Pt p, Dir dir) const noexcept
            {
                const Pt q = p + unit(dir);
                if (!inside(p) || !inside(q))
                    return -1;
                switch (dir)
                {
                    case Dir::E: return hEdge_[at(p)];
                    case Dir::W: return hEdge_[at(q)];
                    case Dir::S: return vEdge_[at(p)];
                    case Dir::N: return vEdge_[at(q)];
                }
                return -1;
            }

            void setEdge(UnitEdge e, int net) noexcept
            {
                if (!inside(e.a) || !inside(e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 })))
                    return;
                (e.h ? hEdge_ : vEdge_)[at(e.a)] = net;
            }

            // How net `n` meets point p: its edges there (a bit a direction), and the other nets'.
            int ownBits(Pt p, int n) const noexcept
            {
                int bits = 0;
                for (int k = 0; k < 4; ++k)
                    bits |= edgeNet(p, static_cast<Dir>(k)) == n ? 1 << k : 0;
                return bits;
            }

            int otherBits(Pt p, int n) const noexcept
            {
                int bits = 0;
                for (int k = 0; k < 4; ++k)
                {
                    const int e = edgeNet(p, static_cast<Dir>(k));
                    bits |= e >= 0 && e != n ? 1 << k : 0;
                }
                return bits;
            }

            static int degree(int bits) noexcept { return ((bits >> 0) & 1) + ((bits >> 1) & 1) + ((bits >> 2) & 1) + ((bits >> 3) & 1); }
            static constexpr int kH = (1 << 0) | (1 << 2);   // E and W
            static constexpr int kV = (1 << 1) | (1 << 3);   // S and N

            // ---- 1. validation ---------------------------------------------------------------------------------------
            void validate(int n)
            {
                std::set<UnitEdge>& mine = edges_[static_cast<std::size_t>(n)];
                if (mine.empty())
                    return;
                struct Seg
                {
                    Pt a, b;
                    bool h;
                };
                std::vector<Seg> segs;
                {
                    std::vector<UnitEdge> hs, vs;
                    for (const UnitEdge& e : mine)
                        (e.h ? hs : vs).push_back(e);
                    std::sort(hs.begin(), hs.end(), [](UnitEdge x, UnitEdge y) { return x.a.y < y.a.y || (x.a.y == y.a.y && x.a.x < y.a.x); });
                    std::sort(vs.begin(), vs.end(), [](UnitEdge x, UnitEdge y) { return x.a.x < y.a.x || (x.a.x == y.a.x && x.a.y < y.a.y); });
                    for (std::size_t i = 0; i < hs.size();)
                    {
                        std::size_t j = i;
                        while (j + 1 < hs.size() && hs[j + 1].a.y == hs[i].a.y && hs[j + 1].a.x == hs[j].a.x + 1)
                            ++j;
                        segs.push_back({ hs[i].a, { hs[j].a.x + 1, hs[i].a.y }, true });
                        i = j + 1;
                    }
                    for (std::size_t i = 0; i < vs.size();)
                    {
                        std::size_t j = i;
                        while (j + 1 < vs.size() && vs[j + 1].a.x == vs[i].a.x && vs[j + 1].a.y == vs[j].a.y + 1)
                            ++j;
                        segs.push_back({ vs[i].a, { vs[i].a.x, vs[j].a.y + 1 }, false });
                        i = j + 1;
                    }
                }
                std::set<UnitEdge> kept;
                for (const Seg& s : segs)
                {
                    bool ok = true;
                    const Pt step = s.h ? Pt{ 1, 0 } : Pt{ 0, 1 };
                    const int len = s.h ? s.b.x - s.a.x : s.b.y - s.a.y;
                    for (int k = 0; k <= len && ok; ++k)
                    {
                        const Pt q{ s.a.x + step.x * k, s.a.y + step.y * k };
                        const bool end = k == 0 || k == len;
                        if (!inside(q))
                        {
                            ok = false;
                            break;
                        }
                        const int pn = pinNet_[at(q)];
                        if (wall_[at(q)] != 0 && !(pn == n && end))
                            ok = false;
                        else if (pn >= 0 && pn != n)
                            ok = false;
                        else if (pn == n && !end)
                            ok = false;
                        else
                        {
                            const int other = otherBits(q, n);
                            if (other != 0)
                            {
                                const int along = s.h ? kH : kV, across = s.h ? kV : kH;
                                // an end on another net's wire, a run along it, or a touch of its end, corner or
                                // junction: only a crossing straight through a straight point is allowed
                                if (end || (other & along) != 0 || other != across)
                                    ok = false;
                            }
                        }
                    }
                    if (!ok)
                        continue;
                    for (int k = 0; k < len; ++k)
                        kept.insert({ { s.a.x + step.x * k, s.a.y + step.y * k }, s.h });
                }
                mine = kept;
                for (const UnitEdge& e : mine)
                    setEdge(e, n);
            }

            // ---- 2. pruning ------------------------------------------------------------------------------------------
            void prune(int n)
            {
                std::set<UnitEdge>& mine = edges_[static_cast<std::size_t>(n)];
                for (bool again = true; again;)
                {
                    again = false;
                    std::map<Pt, int> deg;
                    for (const UnitEdge& e : mine)
                    {
                        ++deg[e.a];
                        ++deg[e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 })];
                    }
                    for (auto it = mine.begin(); it != mine.end();)
                    {
                        const Pt b = it->a + (it->h ? Pt{ 1, 0 } : Pt{ 0, 1 });
                        const auto dangling = [&](Pt p) { return deg[p] == 1 && pinNet_[at(p)] != n; };
                        if (dangling(it->a) || dangling(b))
                        {
                            setEdge(*it, -1);
                            it = mine.erase(it);
                            again = true;
                        }
                        else
                            ++it;
                    }
                }
                // pieces with no pin of the net
                const std::vector<std::set<Pt>> pieces = componentsOf(n);
                for (const std::set<Pt>& piece : pieces)
                {
                    const bool pinned = std::any_of(piece.begin(), piece.end(), [&](Pt p) { return pinNet_[at(p)] == n; });
                    if (pinned)
                        continue;
                    for (auto it = mine.begin(); it != mine.end();)
                    {
                        if (piece.count(it->a) != 0)
                        {
                            setEdge(*it, -1);
                            it = mine.erase(it);
                        }
                        else
                            ++it;
                    }
                }
            }

            // The connected pieces of a net's wires (their points).
            std::vector<std::set<Pt>> componentsOf(int n) const
            {
                const std::set<UnitEdge>& mine = edges_[static_cast<std::size_t>(n)];
                std::map<Pt, Pt> parent;
                const auto find = [&](Pt p) {
                    while (!(parent[p] == p))
                        p = parent[p] = parent[parent[p]];
                    return p;
                };
                for (const UnitEdge& e : mine)
                {
                    const Pt b = e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 });
                    if (parent.find(e.a) == parent.end())
                        parent[e.a] = e.a;
                    if (parent.find(b) == parent.end())
                        parent[b] = b;
                    const Pt ra = find(e.a), rb = find(b);
                    if (!(ra == rb))
                        parent[std::max(ra, rb)] = std::min(ra, rb);
                }
                std::map<Pt, std::set<Pt>> groups;
                for (const auto& [p, q] : parent)
                    groups[find(p)].insert(p);
                std::vector<std::set<Pt>> out;
                for (auto& [r, g] : groups)
                    out.push_back(std::move(g));
                return out;
            }

            // ---- 3. connection ---------------------------------------------------------------------------------------
            struct Piece
            {
                std::set<Pt> points;
                int firstPin = INT_MAX;               // its first pin's place in the net's line order
                bool lonePin = false;
                Pt pin{};
                Dir out = Dir::E;
            };

            void connect(int n)
            {
                Net& net = d_.nets[static_cast<std::size_t>(n)];
                if (net.pins.size() < 2 || net.unroutable)
                    return;
                std::vector<Piece> pieces;
                for (const std::set<Pt>& c : componentsOf(n))
                    pieces.push_back({ c, INT_MAX, false, {}, Dir::E });
                for (std::size_t k = 0; k < net.pins.size(); ++k)
                {
                    const PinAt& pin = pinOf(net.pins[k]);
                    bool on = false;
                    for (Piece& pc : pieces)
                        if (pc.points.count(pin.at) != 0)
                        {
                            pc.firstPin = std::min(pc.firstPin, static_cast<int>(k));
                            on = true;
                        }
                    if (!on)
                        pieces.push_back({ { pin.at }, static_cast<int>(k), true, pin.at, pin.out });
                }
                // the anchor: the input's pin, else the output's, else the first pin in line order
                int anchor = -1;
                for (int pass = 0; pass < 2 && anchor < 0; ++pass)
                    for (std::size_t k = 0; k < net.pins.size() && anchor < 0; ++k)
                        if (d_.parts[static_cast<std::size_t>(net.pins[k].part)].kind == (pass == 0 ? Kind::Input : Kind::Output))
                            anchor = static_cast<int>(k);
                anchor = std::max(anchor, 0);
                const Pt anchorAt = pinOf(net.pins[static_cast<std::size_t>(anchor)]).at;
                std::size_t treeAt = 0;
                for (std::size_t i = 0; i < pieces.size(); ++i)
                    if (pieces[i].points.count(anchorAt) != 0)
                        treeAt = i;
                Piece tree = pieces[treeAt];
                pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(treeAt));
                bool first = tree.lonePin;
                while (!pieces.empty())
                {
                    // the farthest first when the tree is a lone pin (the trunk), else the nearest
                    std::size_t pick = 0;
                    long pickDist = -1;
                    for (std::size_t i = 0; i < pieces.size(); ++i)
                    {
                        long dist = LONG_MAX;
                        for (const Pt a : pieces[i].points)
                            for (const Pt b : tree.points)
                                dist = std::min(dist, static_cast<long>(std::abs(a.x - b.x) + std::abs(a.y - b.y)));
                        const bool better = pickDist < 0 || (first ? dist > pickDist : dist < pickDist)
                                         || (dist == pickDist && pieces[i].firstPin < pieces[pick].firstPin);
                        if (better)
                        {
                            pick = i;
                            pickDist = dist;
                        }
                    }
                    first = false;
                    const Piece from = pieces[pick];
                    pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(pick));
                    std::vector<Pt> path;
                    bool ok = false;
                    for (const int margin : { 8, 16 })
                        if (!ok)
                            ok = search(n, from, tree, bounds_.grown(margin), path);
                    if (!ok)
                    {
                        // drawn as name tags: its wires go
                        for (const UnitEdge& e : edges_[static_cast<std::size_t>(n)])
                            setEdge(e, -1);
                        edges_[static_cast<std::size_t>(n)].clear();
                        net.unroutable = true;
                        return;
                    }
                    for (std::size_t k = 1; k < path.size(); ++k)
                    {
                        const Pt a = path[k - 1], b = path[k];
                        const UnitEdge e = a.y == b.y ? UnitEdge{ { std::min(a.x, b.x), a.y }, true } : UnitEdge{ { a.x, std::min(a.y, b.y) }, false };
                        edges_[static_cast<std::size_t>(n)].insert(e);
                        setEdge(e, n);
                    }
                    tree.points.insert(from.points.begin(), from.points.end());
                    tree.points.insert(path.begin(), path.end());
                    tree.lonePin = false;
                }
            }

            // A* from `from` to any point of `tree`, inside `region` (spec 7.8): a step 1, a bend 4, a crossing 8; a
            // pin entered and left only along its outward direction; bodies, terminals and flags walls; another net's
            // wire crossed only at a right angle through a point where it runs straight. Counted pops.
            bool search(int n, const Piece& from, const Piece& tree, Box region, std::vector<Pt>& path)
            {
                region = { std::max(region.x0, area_.x0), std::max(region.y0, area_.y0), std::min(region.x1, area_.x1),
                           std::min(region.y1, area_.y1) };
                const auto inRegion = [&](Pt p) { return region.contains(p); };
                const std::size_t cells = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
                // goals
                // (each search's state in the router's own arrays, filled again: no allocation once they are sized)
                std::vector<uint8_t>& goal = searchGoal_;
                goal.assign(cells, 0);
                std::vector<Pt> goals;
                for (const Pt p : tree.points)
                {
                    if (!inside(p))
                        continue;
                    const bool pin = pinNet_[at(p)] == n;
                    const int own = ownBits(p, n);
                    if (pin ? own != 0 : (degree(own) > 2 || otherBits(p, n) != 0))
                        continue;
                    goal[at(p)] = 1;
                    goals.push_back(p);
                }
                if (goals.empty())
                    return false;
                // the heuristic: the L1 distance to the nearest goal (a two-pass transform)
                std::vector<int>& h = searchH_;
                h.assign(cells, INT_MAX / 4);
                for (const Pt p : goals)
                    h[at(p)] = 0;
                for (int y = area_.y0; y <= area_.y1; ++y)
                    for (int x = area_.x0; x <= area_.x1; ++x)
                    {
                        int& v = h[at({ x, y })];
                        if (x > area_.x0)
                            v = std::min(v, h[at({ x - 1, y })] + 1);
                        if (y > area_.y0)
                            v = std::min(v, h[at({ x, y - 1 })] + 1);
                    }
                for (int y = area_.y1; y >= area_.y0; --y)
                    for (int x = area_.x1; x >= area_.x0; --x)
                    {
                        int& v = h[at({ x, y })];
                        if (x < area_.x1)
                            v = std::min(v, h[at({ x + 1, y })] + 1);
                        if (y < area_.y1)
                            v = std::min(v, h[at({ x, y + 1 })] + 1);
                    }
                // the net's other pieces and its pins outside the tree and the source are walls
                std::vector<uint8_t>& blocked = searchBlocked_;
                blocked.assign(cells, 0);
                for (const UnitEdge& e : edges_[static_cast<std::size_t>(n)])
                {
                    blocked[at(e.a)] = 1;
                    blocked[at(e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 }))] = 1;
                }
                for (const Pt p : from.points)
                    if (inside(p))
                        blocked[at(p)] = 1;
                struct Node
                {
                    int f, h, y, x, head, g;
                    bool operator>(const Node& o) const noexcept
                    {
                        return std::tie(f, h, y, x, head) > std::tie(o.f, o.h, o.y, o.x, o.head);
                    }
                };
                std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
                std::vector<int>& g = searchG_;
                std::vector<int>& parent = searchParent_;
                std::vector<uint8_t>& isGoal = searchIsGoal_;
                g.assign(cells * 5, INT_MAX);
                parent.assign(cells * 5, -1);
                isGoal.assign(cells * 5, 0);
                const auto key = [&](Pt p, int head) { return at(p) * 5 + static_cast<std::size_t>(head); };
                const auto push = [&](Pt p, int head, int cost, int par, bool done) {
                    const std::size_t k = key(p, head);
                    if (cost >= g[k])
                        return;
                    g[k] = cost;
                    parent[k] = par;
                    isGoal[k] = done ? 1 : 0;
                    const int hv = done ? 0 : h[at(p)];
                    open.push({ cost + hv, hv, p.y, p.x, head, cost });
                };
                if (from.lonePin)
                {
                    if (inside(from.pin))
                        push(from.pin, static_cast<int>(from.out), 0, -1, false);
                }
                else
                    for (const Pt p : from.points)
                    {
                        if (!inside(p) || pinNet_[at(p)] >= 0 || degree(ownBits(p, n)) > 2 || otherBits(p, n) != 0)
                            continue;
                        push(p, kNone, 0, -1, false);
                    }
                long pops = 0;
                while (!open.empty())
                {
                    const Node cur = open.top();
                    open.pop();
                    const Pt p{ cur.x, cur.y };
                    const std::size_t k = key(p, cur.head);
                    if (cur.g != g[k])
                        continue;                         // a stale entry
                    ++pops;
                    ++budget_.pops;
                    if (pops > kPopsPerConnection || budget_.pops > kPopsPerLayout)
                        break;
                    if (isGoal[k] != 0)
                    {
                        budget_.most = std::max(budget_.most, pops);
                        path.clear();
                        for (long s = static_cast<long>(k); s >= 0; s = parent[static_cast<std::size_t>(s)])
                        {
                            const std::size_t cell = static_cast<std::size_t>(s) / 5;
                            path.push_back({ area_.x0 + static_cast<int>(cell % static_cast<std::size_t>(w_)),
                                             area_.y0 + static_cast<int>(cell / static_cast<std::size_t>(w_)) });
                        }
                        std::reverse(path.begin(), path.end());
                        return true;
                    }
                    const bool crossing = cur.head != kNone && otherBits(p, n) != 0;
                    const bool atSourcePin = from.lonePin && p == from.pin;
                    for (int dirI = 0; dirI < 4; ++dirI)
                    {
                        const Dir dir = static_cast<Dir>(dirI);
                        if (cur.head != kNone && dir == opposite(static_cast<Dir>(cur.head)))
                            continue;
                        if ((crossing || atSourcePin) && dirI != cur.head)
                            continue;                     // straight through a crossing; out of a pin
                        const Pt q = p + unit(dir);
                        if (!inRegion(q) || !inside(q))
                            continue;
                        const std::size_t qi = at(q);
                        const int bend = cur.head != kNone && dirI != cur.head ? 4 : 0;
                        if (goal[qi] != 0)
                        {
                            const bool pin = pinNet_[qi] == n;
                            if (pin && dir != opposite(static_cast<Dir>(pinOut_[qi])))
                                continue;
                            push(q, dirI, cur.g + 1 + bend, static_cast<int>(k), true);
                            continue;
                        }
                        if (wall_[qi] != 0 || pinNet_[qi] >= 0 || blocked[qi] != 0)
                            continue;
                        const int other = otherBits(q, n);
                        int cost = 1 + bend;
                        if (other != 0)
                        {
                            const int along = horizontal(dir) ? kH : kV, across = horizontal(dir) ? kV : kH;
                            if ((other & along) != 0 || other != across)
                                continue;
                            cost += 8;
                        }
                        push(q, dirI, cur.g + cost, static_cast<int>(k), false);
                    }
                }
                budget_.most = std::max(budget_.most, pops);
                return false;
            }

            Drawing& d_;
            Budget& budget_;
            std::vector<int> order_;
            Box bounds_{}, area_{};
            int w_ = 0, h_ = 0;
            std::vector<uint8_t> wall_;
            std::vector<int> pinNet_;
            std::vector<int8_t> pinOut_;
            std::vector<int> hEdge_, vEdge_;
            std::vector<std::set<UnitEdge>> edges_;
            bool wide_ = false;
            std::vector<uint8_t> searchGoal_, searchBlocked_, searchIsGoal_;
            std::vector<int> searchH_, searchG_, searchParent_;
        };
    }

    void repairAll(Drawing& d, Budget& budget)
    {
        Router(d, budget).run();
    }
}
