// Source/editor/SchematicView.cpp: see SchematicView.h.
#include "editor/SchematicView.h"

#include "editor/ChatStyle.h"
#include "editor/Items.h"
#include "editor/Theme.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <set>
#include <utility>

namespace playground::ui
{
    namespace
    {
        constexpr float kDragPx = 3.0f;                  // a press that moves this far is a drag
        constexpr float kPinHitPx = 6.0f, kWireHitPx = 4.0f;
        constexpr float kButtonW = 24.0f, kButtonGap = 2.0f;
        constexpr float kEaseTau = 0.08f;                // a fit's motion
        constexpr float kStepZoom = 1.25f;               // -, +, Cmd-=, Cmd--

        // The toolbar's parts, in order (R C L D P X S).
        constexpr cir::Kind kBarKinds[] = { cir::Kind::R, cir::Kind::C, cir::Kind::L, cir::Kind::D,
                                            cir::Kind::Pot, cir::Kind::OpAmp, cir::Kind::S };

        // The More menu: the parts the toolbar has no icon for, and the terminals.
        struct MoreItem
        {
            cir::Kind kind;
            const char* label;
        };
        constexpr MoreItem kMore[] = {
            { cir::Kind::Vcvs, "VCVS: a voltage-controlled voltage source   E" },
            { cir::Kind::Vccs, "VCCS: a voltage-controlled current source   Shift-G" },
            { cir::Kind::Ota, "OTA: a transconductance amplifier   O" },
            { cir::Kind::V, "DC voltage   V" },
            { cir::Kind::I, "DC current   I" },
            { cir::Kind::Rheostat, "Rheostat: a pot of two pins   Shift-P" },
            { cir::Kind::Input, "Input" },
            { cir::Kind::Output, "Output" },
        };

        float dist(funkgui::Point a, funkgui::Point b) noexcept { return std::hypot(a.x - b.x, a.y - b.y); }

        float distToSegment(funkgui::Point p, funkgui::Point a, funkgui::Point b) noexcept
        {
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float l2 = dx * dx + dy * dy;
            float t = l2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            return dist(p, { a.x + t * dx, a.y + t * dy });
        }

        cir::Pt edgeEnd(cir::UnitEdge e) noexcept { return e.a + (e.h ? cir::Pt{ 1, 0 } : cir::Pt{ 0, 1 }); }

        // apply()'s error without its "op 1 (place R1): " (the info strip says what the gesture was).
        std::string friendly(const std::string& error)
        {
            if (error.rfind("op ", 0) == 0)
                if (const std::size_t at = error.find("): "); at != std::string::npos)
                    return error.substr(at + 3);
            return error;
        }

        std::string lower(std::string s)
        {
            for (char& ch : s)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }

        // A run's points in order, unit step by unit step (a polyline cut() takes).
        std::vector<cir::Pt> pathOf(const std::vector<cir::UnitEdge>& edges)
        {
            std::map<cir::Pt, std::vector<cir::Pt>> adj;
            for (const cir::UnitEdge& e : edges)
            {
                adj[e.a].push_back(edgeEnd(e));
                adj[edgeEnd(e)].push_back(e.a);
            }
            if (adj.empty())
                return {};
            cir::Pt start = adj.begin()->first;
            for (const auto& [p, n] : adj)
                if (n.size() == 1)
                {
                    start = p;
                    break;
                }
            std::vector<cir::Pt> out{ start };
            std::set<cir::Pt> seen{ start };
            cir::Pt at = start;
            for (bool more = true; more;)
            {
                more = false;
                for (const cir::Pt& n : adj[at])
                    if (seen.count(n) == 0)
                    {
                        out.push_back(n);
                        seen.insert(n);
                        at = n;
                        more = true;
                        break;
                    }
            }
            return out;
        }

        uint32_t fnv(const std::string& s) noexcept
        {
            uint32_t h = 2166136261u;
            for (const char ch : s)
            {
                h ^= static_cast<uint8_t>(ch);
                h *= 16777619u;
            }
            return h;
        }

        bool isTerminal(const cir::Part& p) noexcept { return p.kind == cir::Kind::Input || p.kind == cir::Kind::Output; }

        // The pin's name as the tools say it: R4.2, XU1.inn; a terminal its name alone.
        std::string pinName(const cir::Part& p, int pin)
        {
            if (isTerminal(p))
                return p.name;
            return p.name + "." + p.pins[static_cast<std::size_t>(pin)].name;
        }
    }

    SchematicView::SchematicView(Hooks hooks) : hooks_(std::move(hooks))
    {
        buttons_.resize(static_cast<std::size_t>(Button::count));
    }

    // ---- the file shown, the geometry ---------------------------------------------------------------------------------
    void SchematicView::show(funkgui::TextEditor* ed, const std::string& file, SchematicFile* state)
    {
        if (ed == ed_ && state == st_ && file == file_)
            return;
        leave();
        ed_ = ed;
        file_ = file;
        st_ = state;
        valid_ = false;
        runs_.clear();
        flash_.clear();
        shapes_.clear();
        stepAt_ = -1;
        easing_ = false;
        notice_.clear();
        noticeLeft_ = 0.0f;
        structureKey_.clear();
        ++structure_;
        fitPending_ = st_ != nullptr && !st_->framed;
        if (showing())
        {
            ensure();
            computeMarks();
        }
    }

    void SchematicView::setArea(const funkgui::Rect& r)
    {
        area_ = r;
        toolbar_ = { r.x, r.y, r.w, kToolbarH };
        strip_ = { r.x, r.bottom() - kStripH, r.w, kStripH };
        canvas_ = { r.x, r.y + kToolbarH, r.w, std::max(0.0f, r.h - kToolbarH - kStripH) };
        layoutBars();
    }

    void SchematicView::layoutBars()
    {
        const float y = toolbar_.y + 0.5f * (kToolbarH - kButtonW);
        float x = toolbar_.x + 4.0f;
        const auto put = [&](Button b, float w) {
            buttons_[static_cast<std::size_t>(b)] = { x, y, w, kButtonW };
            x += w + kButtonGap;
        };
        put(Button::select, kButtonW);
        put(Button::ground, kButtonW);
        x += 6.0f;
        put(Button::r, kButtonW);
        put(Button::c, kButtonW);
        put(Button::l, kButtonW);
        put(Button::d, kButtonW);
        put(Button::p, kButtonW);
        put(Button::x, kButtonW);
        put(Button::s, kButtonW);
        put(Button::more, 52.0f);
        // the right end, right to left
        float r = toolbar_.right() - 4.0f;
        const auto putRight = [&](Button b, float w) {
            r -= w;
            buttons_[static_cast<std::size_t>(b)] = { r, y, w, kButtonW };
            r -= kButtonGap;
        };
        putRight(Button::zoomIn, kButtonW);
        putRight(Button::zoom, 44.0f);
        putRight(Button::zoomOut, kButtonW);
        r -= 6.0f;
        putRight(Button::tidy, 40.0f);
        r -= 6.0f;
        putRight(Button::redo, kButtonW);
        putRight(Button::undo, kButtonW);
    }

    funkgui::Rect SchematicView::buttonRect(Button b) const { return buttons_[static_cast<std::size_t>(b)]; }

    funkgui::Point SchematicView::centreOfButton(Button b) const
    {
        const funkgui::Rect r = buttonRect(b);
        return { r.centreX(), r.centreY() };
    }

    sch::View SchematicView::view() const
    {
        sch::View v;
        const float z = st_ != nullptr ? st_->zoom : 1.0f;
        v.scale = sch::kUnitPx * z;
        const float cx = st_ != nullptr ? st_->cx : 0.0f, cy = st_ != nullptr ? st_->cy : 0.0f;
        v.ox = canvas_.centreX() - cx * v.scale;
        v.oy = canvas_.centreY() - cy * v.scale;
        return v;
    }

    funkgui::Point SchematicView::screenOf(cir::Pt p) const { return view().at(p); }

    cir::Pt SchematicView::gridAt(funkgui::Point p) const
    {
        const funkgui::Point w = view().world(p);
        return { static_cast<int>(std::lround(w.x)), static_cast<int>(std::lround(w.y)) };
    }

    void SchematicView::leave()
    {
        // (a fit pending stays: show() sets it again for a file shown anew, and a view hidden under the PANEL view
        // fits when it shows again)
        drag_ = Drag::none;
        moveBy_ = {};
        moveRefused_ = false;
        tool_ = Tool::select;
        kind_ = cir::Kind::Unknown;
        orient_ = cir::Orient::r0;
    }

    // ---- the model ----------------------------------------------------------------------------------------------------
    void SchematicView::ensure() const
    {
        if (!showing())
            return;
        if (valid_ && rev_ == ed_->revision())
            return;
        const bool flash = st_->merged && !shapes_.empty();
        const std::map<std::string, std::string> before = flash ? shapes_ : std::map<std::string, std::string>{};
        valid_ = true;
        rev_ = ed_->revision();
        text_ = ed_->text();
        nl_ = cir::read(text_);
        d_ = cir::reconcile(text_, st_->pins);
        // Session pins: the places drawn for parts without a part line, so that the next reconcile keeps them.
        cir::SessionPins pins;
        for (const cir::Part& p : d_.parts)
            if (p.element >= 0 && p.source != cir::Source::stored)
                pins.parts.push_back({ p.name, p.origin, p.orient, p.extra, -1 });
        st_->pins = std::move(pins);
        // What a part looks like (its place, its turn, its value): a merge flashes those it changed.
        shapes_.clear();
        for (const cir::Part& p : d_.parts)
            shapes_[cir::folded(p.name)] = std::to_string(p.origin.x) + "," + std::to_string(p.origin.y) + " "
                                         + cir::orientName(p.orient) + " " + sch::valueText(nl_, p);
        if (flash)
            for (const auto& [name, shape] : shapes_)
                if (const auto it = before.find(name); it == before.end() || it->second != shape)
                {
                    const int i = d_.part(name);
                    if (i >= 0)
                        flash_[d_.parts[static_cast<std::size_t>(i)].name] = kFlashSeconds;
                }
        st_->merged = false;
        // Parts and nets that come or go: the accessibility tree's structure (its ids).
        std::string key;
        for (const cir::Part& p : d_.parts)
            key += "p:" + p.name + '\n';
        for (const cir::Net& n : d_.nets)
            if (!n.pins.empty())
                key += "n:" + n.name + '\n';
        if (key != structureKey_)
        {
            structureKey_ = key;
            ++structure_;
            std::vector<std::string> keys;
            for (const cir::Part& p : d_.parts)
                keys.push_back("p:" + p.name);
            for (const cir::Net& n : d_.nets)
                if (!n.pins.empty())
                    keys.push_back("n:" + n.name);
            std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) { return cir::naturalLess(a, b); });
            ids_.clear();
            std::set<uint32_t> taken;
            for (const std::string& k : keys)
            {
                uint32_t h = fnv(k) & 0xFFFFFFu;
                while (h < 16u || taken.count(h) != 0)
                    h = (h + 1u) & 0xFFFFFFu;
                taken.insert(h);
                ids_[k] = kA11yBase | h;
            }
        }
        // The selection survives by name; the wires selected while their net keeps them.
        std::vector<std::string> kept;
        for (const std::string& s : st_->selected)
            if (d_.part(s) >= 0)
                kept.push_back(d_.parts[static_cast<std::size_t>(d_.part(s))].name);
        st_->selected = std::move(kept);
        std::vector<Run> runs;
        for (const Run& r : runs_)
        {
            const int n = d_.net(r.net);
            if (n < 0)
                continue;
            const std::vector<cir::UnitEdge>& edges = d_.nets[static_cast<std::size_t>(n)].edges;
            if (std::all_of(r.edges.begin(), r.edges.end(), [&](const cir::UnitEdge& e) { return std::binary_search(edges.begin(), edges.end(), e); }))
                runs.push_back(r);
        }
        runs_ = std::move(runs);
        computeLoose();
        computeMarks();                                  // their parts are this drawing's
    }

    void SchematicView::computeLoose() const
    {
        loose_.clear();
        // every net's wire ends (a pin of another net touching one connects nothing)
        std::map<cir::Pt, std::vector<int>> ends;
        for (std::size_t n = 0; n < d_.nets.size(); ++n)
            if (d_.nets[n].style == cir::Style::wired && !d_.nets[n].unroutable)
                for (const cir::UnitEdge& e : d_.nets[n].edges)
                {
                    ends[e.a].push_back(static_cast<int>(n));
                    ends[edgeEnd(e)].push_back(static_cast<int>(n));
                }
        std::map<cir::Pt, std::vector<int>> pins;        // the nets of the pins at each point
        for (const cir::Part& p : d_.parts)
            for (const cir::PinAt& pin : p.pins)
                pins[pin.at].push_back(pin.net);
        for (std::size_t i = 0; i < d_.parts.size(); ++i)
            for (const cir::PinAt& pin : d_.parts[i].pins)
            {
                // alone on its net (ground connects whatever it holds: never alone)
                const bool ground = pin.net >= 0 && d_.nets[static_cast<std::size_t>(pin.net)].style == cir::Style::ground;
                if (!ground && (pin.net < 0 || d_.nets[static_cast<std::size_t>(pin.net)].totalPins <= 1))
                {
                    loose_.push_back({ static_cast<int>(i), pin.at, true });
                    continue;
                }
                bool touches = false;
                for (const int n : pins[pin.at])
                    touches = touches || n != pin.net;
                if (const auto it = ends.find(pin.at); it != ends.end())
                    for (const int n : it->second)
                        touches = touches || n != pin.net;
                if (touches)
                    loose_.push_back({ static_cast<int>(i), pin.at, false });
            }
    }

    const cir::Drawing& SchematicView::drawing() const
    {
        ensure();
        return d_;
    }

    const cir::Netlist& SchematicView::netlist() const
    {
        ensure();
        return nl_;
    }

    void SchematicView::computeMarks() const
    {
        marks_.clear();
        partMarks_.assign(d_.parts.size(), {});
        if (!showing())
            return;
        std::vector<Diagnostic> problems = hooks_.problems ? hooks_.problems() : std::vector<Diagnostic>{};
        const auto partOfElement = [&](int element) {
            for (std::size_t i = 0; i < d_.parts.size(); ++i)
                if (d_.parts[i].element == element)
                    return static_cast<int>(i);
            return -1;
        };
        const auto terminal = [&](cir::Kind k) {
            for (std::size_t i = 0; i < d_.parts.size(); ++i)
                if (d_.parts[i].kind == k)
                    return static_cast<int>(i);
            return -1;
        };
        for (const Diagnostic& g : problems)
        {
            Mark m;
            m.line = std::max(0, g.line - 1);
            m.col = std::max(0, g.column - 1);
            m.severity = g.severity == Diagnostic::Severity::error ? 0 : g.severity == Diagnostic::Severity::warning ? 1 : 2;
            m.text = g.text;
            // an element's logical line: that part
            for (std::size_t e = 0; e < nl_.elements.size(); ++e)
                if (nl_.elements[e].firstLine <= m.line && m.line <= nl_.elements[e].lastLine)
                    if (const int p = partOfElement(static_cast<int>(e)); p >= 0)
                    {
                        m.parts.push_back(p);
                        // ... and the pin, when the column falls on a node token
                        const cir::Element& el = nl_.elements[e];
                        for (std::size_t k = 0; k < el.nodes.size() && k < el.tokenOf.size(); ++k)
                        {
                            const cir::Token& t = el.nodes[k];
                            if (t.at.line != m.line || m.col < t.at.col || m.col >= t.at.col + std::max(1, t.len))
                                continue;
                            const std::vector<cir::PinAt>& pins = d_.parts[static_cast<std::size_t>(p)].pins;
                            for (std::size_t j = 0; j < pins.size() && j < 8; ++j)
                                if (pins[j].token == el.tokenOf[k])
                                    m.pins.push_back({ p, static_cast<int>(j) });
                        }
                    }
            // a .pot line: every part whose expression names it
            for (const cir::PotLine& pot : nl_.pots)
                if (pot.line == m.line)
                    for (std::size_t e = 0; e < nl_.elements.size(); ++e)
                        for (const cir::Token& t : nl_.elements[e].tokens)
                        {
                            if (t.text.empty() || t.text.front() != '{')
                                continue;
                            const std::string body = lower(t.text), word = lower(pot.name);
                            bool uses = false;
                            for (std::size_t at = body.find(word); at != std::string::npos && !uses; at = body.find(word, at + 1))
                            {
                                const auto ident = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; };
                                uses = (at == 0 || !ident(body[at - 1])) && (at + word.size() >= body.size() || !ident(body[at + word.size()]));
                            }
                            if (uses)
                                if (const int p = partOfElement(static_cast<int>(e)); p >= 0)
                                    m.parts.push_back(p);
                        }
            // a .model line: every diode of that model
            for (const auto& [model, line] : nl_.models)
                if (line == m.line)
                    for (std::size_t e = 0; e < nl_.elements.size(); ++e)
                    {
                        const cir::Element& el = nl_.elements[e];
                        if (el.kind == cir::Kind::D && el.valueToken >= 0
                            && cir::folded(el.tokens[static_cast<std::size_t>(el.valueToken)].text) == cir::folded(model))
                            if (const int p = partOfElement(static_cast<int>(e)); p >= 0)
                                m.parts.push_back(p);
                    }
            // an .input or .output line: its terminal
            if (nl_.input && nl_.input->line == m.line)
                if (const int p = terminal(cir::Kind::Input); p >= 0)
                    m.parts.push_back(p);
            if (nl_.output && nl_.output->line == m.line)
                if (const int p = terminal(cir::Kind::Output); p >= 0)
                    m.parts.push_back(p);
            std::sort(m.parts.begin(), m.parts.end());
            m.parts.erase(std::unique(m.parts.begin(), m.parts.end()), m.parts.end());
            marks_.push_back(std::move(m));
        }
        std::stable_sort(marks_.begin(), marks_.end(), [](const Mark& a, const Mark& b) {
            return a.line != b.line ? a.line < b.line : a.col < b.col;
        });
        for (const Mark& m : marks_)
            for (const int p : m.parts)
            {
                sch::PartMarks& pm = partMarks_[static_cast<std::size_t>(p)];
                (m.severity == 0 ? pm.errors : m.severity == 1 ? pm.warnings : pm.notes) += 1;
                pm.outline = pm.outline || m.severity == 0;
            }
        for (const Mark& m : marks_)
            for (const auto& [p, pin] : m.pins)
                partMarks_[static_cast<std::size_t>(p)].pinMask[std::clamp(m.severity, 0, 2)] |= static_cast<uint8_t>(1u << pin);
        // a hand edit's overlap: both outlined
        for (const auto& [a, b] : d_.overlaps)
            for (const std::string& item : { a, b })
            {
                std::string name = item;
                if (const std::size_t at = name.find("'s"); at != std::string::npos)
                    name.resize(at);
                if (const std::size_t dot = name.find('.'); dot != std::string::npos)
                    name.resize(dot);
                if (const int p = d_.part(name); p >= 0)
                    partMarks_[static_cast<std::size_t>(p)].outline = true;
            }
    }

    int SchematicView::problemCount() const { return static_cast<int>(marks_.size()); }

    // ---- hit-testing --------------------------------------------------------------------------------------------------
    SchematicView::Hit SchematicView::hitAt(funkgui::Point p) const
    {
        Hit h;
        if (!showing() || !canvas_.contains(p))
            return h;
        if (const std::optional<funkgui::Point> s = centreOfStart(); s && d_.parts.empty())
        {
            const funkgui::TextStyle& st = chat::kSmall;
            const float w = funkgui::text::width(funkgui::FontService::get().atlas(), "Start a circuit", st) + 16.0f;
            if (funkgui::Rect{ s->x - 0.5f * w, s->y - 11.0f, w, 22.0f }.contains(p))
            {
                h.kind = Hit::Kind::start;
                return h;
            }
        }
        const sch::View v = view();
        // a badge
        for (std::size_t i = 0; i < d_.parts.size() && i < partMarks_.size(); ++i)
        {
            const sch::PartMarks& m = partMarks_[i];
            if (m.errors + m.warnings + m.notes == 0)
                continue;
            if (dist(p, sch::badgeAt(v, d_.parts[i])) <= sch::kBadgeR + 2.0f)
            {
                h.kind = Hit::Kind::badge;
                h.part = static_cast<int>(i);
                return h;
            }
        }
        // a pin (the nearest within its radius)
        float best = kPinHitPx;
        for (std::size_t i = 0; i < d_.parts.size(); ++i)
            for (std::size_t k = 0; k < d_.parts[i].pins.size(); ++k)
            {
                const float dd = dist(p, v.at(d_.parts[i].pins[k].at));
                if (dd <= best)
                {
                    best = dd;
                    h.kind = Hit::Kind::pin;
                    h.part = static_cast<int>(i);
                    h.pin = static_cast<int>(k);
                    h.net = d_.parts[i].pins[k].net;
                    h.at = d_.parts[i].pins[k].at;
                }
            }
        if (h.kind == Hit::Kind::pin)
            return h;
        // a body
        const funkgui::Point w = v.world(p);
        for (std::size_t i = d_.parts.size(); i-- > 0;)
        {
            const cir::Box& b = d_.parts[i].body;
            if (w.x >= static_cast<float>(b.x0) - 0.3f && w.x <= static_cast<float>(b.x1) + 0.3f
                && w.y >= static_cast<float>(b.y0) - 0.3f && w.y <= static_cast<float>(b.y1) + 0.3f)
            {
                h.kind = Hit::Kind::part;
                h.part = static_cast<int>(i);
                return h;
            }
        }
        // a wire
        best = kWireHitPx;
        for (std::size_t n = 0; n < d_.nets.size(); ++n)
        {
            const cir::Net& net = d_.nets[n];
            if (net.style != cir::Style::wired || net.unroutable)
                continue;
            for (const cir::UnitEdge& e : net.edges)
            {
                const float dd = distToSegment(p, v.at(e.a), v.at(edgeEnd(e)));
                if (dd <= best)
                {
                    best = dd;
                    h.kind = Hit::Kind::wire;
                    h.net = static_cast<int>(n);
                    h.edge = e;
                    h.at = dist(p, v.at(e.a)) <= dist(p, v.at(edgeEnd(e))) ? e.a : edgeEnd(e);
                }
            }
        }
        return h;
    }

    SchematicView::Run SchematicView::runAt(int net, cir::UnitEdge e) const
    {
        Run r;
        if (net < 0 || net >= static_cast<int>(d_.nets.size()))
            return r;
        const cir::Net& n = d_.nets[static_cast<std::size_t>(net)];
        r.net = n.key;
        std::map<cir::Pt, std::vector<cir::UnitEdge>> at;
        for (const cir::UnitEdge& x : n.edges)
        {
            at[x.a].push_back(x);
            at[edgeEnd(x)].push_back(x);
        }
        std::set<cir::Pt> stops;
        for (const cir::PinRef& pr : n.pins)
            stops.insert(d_.parts[static_cast<std::size_t>(pr.part)].pins[static_cast<std::size_t>(pr.pin)].at);
        std::set<cir::UnitEdge> got{ e };
        std::vector<cir::Pt> todo{ e.a, edgeEnd(e) };
        while (!todo.empty())
        {
            const cir::Pt p = todo.back();
            todo.pop_back();
            const std::vector<cir::UnitEdge>& es = at[p];
            if (es.size() != 2 || stops.count(p) != 0)
                continue;                                // a junction, a pin or an end: the run stops
            for (const cir::UnitEdge& x : es)
                if (got.insert(x).second)
                    todo.push_back(x.a == p ? edgeEnd(x) : x.a);
        }
        r.edges.assign(got.begin(), got.end());
        return r;
    }

    std::vector<SchematicView::Run> SchematicView::runsOf(int net) const
    {
        std::vector<Run> out;
        if (net < 0 || net >= static_cast<int>(d_.nets.size()))
            return out;
        std::set<cir::UnitEdge> done;
        for (const cir::UnitEdge& e : d_.nets[static_cast<std::size_t>(net)].edges)
        {
            if (done.count(e) != 0)
                continue;
            Run r = runAt(net, e);
            done.insert(r.edges.begin(), r.edges.end());
            out.push_back(std::move(r));
        }
        return out;
    }

    // ---- gestures into text -------------------------------------------------------------------------------------------
    void SchematicView::say(const std::string& text)
    {
        notice_ = asciiText(text);
        noticeLeft_ = kNoticeSeconds;
    }

    bool SchematicView::commit(const std::vector<cir::Op>& ops)
    {
        if (!showing())
            return false;
        ensure();
        if (d_.readOnly)
        {
            say("Drawn by a newer playground: read only here");
            return false;
        }
        const cir::Applied a = cir::apply(text_, d_, ops, st_->pins);
        if (!a.error.empty())
        {
            say(friendly(a.error));
            return false;
        }
        if (!a.said.empty())
        {
            // what each op did, in order (the info strip cuts what does not fit)
            std::string words;
            for (const std::string& line : a.said)
                words += (words.empty() ? "" : "; ") + line;
            say(words);
        }
        if (a.edits.empty())
            return true;
        std::vector<funkgui::TextEdit> edits;
        edits.reserve(a.edits.size());
        for (const cir::Edit& e : a.edits)
            edits.push_back({ { e.from.line, e.from.col }, { e.to.line, e.to.col }, e.text });
        if (!ed_->applyEdits(std::move(edits)))
        {
            say("The edit could not be made (its parts overlapped): nothing changed");
            return false;
        }
        ensure();
        return true;
    }

    bool SchematicView::commitText(const std::string& text)
    {
        if (!showing())
            return false;
        ensure();
        if (text == text_)
            return true;
        std::vector<funkgui::TextEdit> edits;
        for (const cir::Edit& e : cir::editsBetween(text_, text))
            edits.push_back({ { e.from.line, e.from.col }, { e.to.line, e.to.col }, e.text });
        if (!ed_->applyEdits(std::move(edits)))
        {
            say("The edit could not be made: nothing changed");
            return false;
        }
        ensure();
        return true;
    }

    void SchematicView::cancelGesture(const char* why)
    {
        if (drag_ == Drag::none)
            return;
        drag_ = Drag::none;
        moveBy_ = {};
        moveRefused_ = false;
        if (why != nullptr)
            say(why);
    }

    bool SchematicView::gestureOn() const noexcept
    {
        return drag_ == Drag::move || drag_ == Drag::wire || drag_ == Drag::marquee || drag_ == Drag::pan || drag_ == Drag::carry;
    }

    void SchematicView::persist()
    {
        if (st_ == nullptr)
            return;
        st_->stored = true;
        if (hooks_.persist)
            hooks_.persist();
    }

    // ---- the selection --------------------------------------------------------------------------------------------------
    std::vector<std::string> SchematicView::selection() const
    {
        ensure();
        return st_ != nullptr ? st_->selected : std::vector<std::string>{};
    }

    std::vector<cir::UnitEdge> SchematicView::selectedEdges() const
    {
        std::vector<cir::UnitEdge> out;
        for (const Run& r : runs_)
            out.insert(out.end(), r.edges.begin(), r.edges.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    void SchematicView::setSelection(std::vector<std::string> parts, std::vector<Run> runs)
    {
        if (st_ == nullptr)
            return;
        st_->selected = std::move(parts);
        runs_ = std::move(runs);
    }

    void SchematicView::selectPart(const std::string& name, bool centreIt)
    {
        ensure();
        const int i = d_.part(name);
        if (i < 0 || st_ == nullptr)
            return;
        const cir::Part& p = d_.parts[static_cast<std::size_t>(i)];
        setSelection({ p.name }, {});
        if (centreIt)
        {
            st_->cx = 0.5f * static_cast<float>(p.body.x0 + p.body.x1);
            st_->cy = 0.5f * static_cast<float>(p.body.y0 + p.body.y1);
            st_->framed = true;
            easing_ = false;
            fitPending_ = false;
        }
    }

    bool SchematicView::selectAtLine(int line, bool centreIt)
    {
        ensure();
        for (const cir::Part& p : d_.parts)
        {
            if (p.element >= 0)
            {
                const cir::Element& e = nl_.elements[static_cast<std::size_t>(p.element)];
                if (e.firstLine <= line && line <= e.lastLine)
                {
                    selectPart(p.name, centreIt);
                    return true;
                }
            }
            else if ((p.kind == cir::Kind::Input && nl_.input && nl_.input->line == line)
                     || (p.kind == cir::Kind::Output && nl_.output && nl_.output->line == line))
            {
                selectPart(p.name, centreIt);
                return true;
            }
        }
        return false;
    }

    void SchematicView::selectAll()
    {
        ensure();
        std::vector<std::string> parts;
        for (const cir::Part& p : d_.parts)
            parts.push_back(p.name);
        std::vector<Run> runs;
        for (std::size_t n = 0; n < d_.nets.size(); ++n)
            for (Run& r : runsOf(static_cast<int>(n)))
                runs.push_back(std::move(r));
        setSelection(std::move(parts), std::move(runs));
    }

    void SchematicView::stepPart(int dir)
    {
        ensure();
        if (d_.parts.empty() || st_ == nullptr)
            return;
        int at = -1;
        if (!st_->selected.empty())
            at = d_.part(st_->selected.front());
        const int n = static_cast<int>(d_.parts.size());
        at = at < 0 ? (dir > 0 ? 0 : n - 1) : ((at + dir) % n + n) % n;
        selectPart(d_.parts[static_cast<std::size_t>(at)].name, true);
        announceSelection();
    }

    void SchematicView::announceSelection()
    {
        if (host_ == nullptr || st_ == nullptr || st_->selected.empty())
            return;
        const int i = d_.part(st_->selected.front());
        if (i < 0)
            return;
        host_->announce(partWords(i));
        if (const auto it = ids_.find("p:" + d_.parts[static_cast<std::size_t>(i)].name); it != ids_.end())
            host_->a11yFocus(it->second);
    }

    // ---- the view's motion --------------------------------------------------------------------------------------------
    void SchematicView::fit(bool opening)
    {
        if (!showing() || canvas_.w <= 0.0f || canvas_.h <= 0.0f)
            return;
        ensure();
        const cir::Box b = d_.bounds();
        float z = 1.0f, cx = 0.0f, cy = 0.0f;
        if (!b.empty())
        {
            // a margin of 4 units: the names and values above the parts, and right of the vertical ones
            const float bw = static_cast<float>(b.x1 - b.x0 + 8), bh = static_cast<float>(b.y1 - b.y0 + 8);
            z = std::min(canvas_.w / (bw * sch::kUnitPx), canvas_.h / (bh * sch::kUnitPx));
            z = std::clamp(z, kMinZoom, opening ? 2.0f : kMaxZoom);
            if (opening)
                z = std::max(z, kOpenMinZoom);
            cx = 0.5f * static_cast<float>(b.x0 + b.x1);
            cy = 0.5f * static_cast<float>(b.y0 + b.y1);
            const float scale = sch::kUnitPx * z;
            // Wider than the canvas (opening at 60 %): the input's side shows; taller: the top.
            if (bw * scale > canvas_.w)
                cx = static_cast<float>(b.x0 - 4) + 0.5f * canvas_.w / scale;
            if (bh * scale > canvas_.h)
                cy = static_cast<float>(b.y0 - 4) + 0.5f * canvas_.h / scale;
        }
        if (opening)
        {
            st_->zoom = z;
            st_->cx = cx;
            st_->cy = cy;
            easing_ = false;
        }
        else
        {
            zoomTo_ = z;
            cxTo_ = cx;
            cyTo_ = cy;
            easing_ = true;
        }
        st_->framed = true;
        fitPending_ = false;
    }

    void SchematicView::zoomTo(float z, funkgui::Point about)
    {
        if (st_ == nullptr)
            return;
        z = std::clamp(z, kMinZoom, kMaxZoom);
        const sch::View v = view();
        const funkgui::Point w = v.world(about);
        const float scale = sch::kUnitPx * z;
        st_->zoom = z;
        st_->cx = w.x + (canvas_.centreX() - about.x) / scale;
        st_->cy = w.y + (canvas_.centreY() - about.y) / scale;
        st_->framed = true;
        easing_ = false;
        fitPending_ = false;
    }

    void SchematicView::panBy(float dx, float dy)
    {
        if (st_ == nullptr)
            return;
        const float scale = sch::kUnitPx * st_->zoom;
        st_->cx -= dx / scale;
        st_->cy -= dy / scale;
        st_->framed = true;
        easing_ = false;
        fitPending_ = false;
    }

    // ---- actions --------------------------------------------------------------------------------------------------------
    void SchematicView::setTool(Tool t, cir::Kind kind)
    {
        tool_ = t;
        kind_ = t == Tool::place ? kind : cir::Kind::Unknown;
        if (t != Tool::place)
            orient_ = cir::Orient::r0;
        if (drag_ == Drag::carry)
            drag_ = Drag::none;
    }

    bool SchematicView::placeAt(cir::Kind kind, cir::Pt at, cir::Orient o)
    {
        ensure();
        cir::Op op;
        op.type = cir::Op::Type::add;
        op.at = at;
        std::string name;
        if (kind == cir::Kind::Input || kind == cir::Kind::Output)
        {
            const bool in = kind == cir::Kind::Input;
            if (in ? nl_.input.has_value() : nl_.output.has_value())
            {
                say(in ? "The netlist has its input (" + nl_.input->name + "): FMinus takes exactly one"
                       : "The netlist has its output (" + nl_.output->name + ")");
                return false;
            }
            const std::string base = in ? "u" : "vout";
            name = base;
            for (int k = 1; nl_.element(name) >= 0 || d_.part(name) >= 0 || nl_.node(name) >= 0; ++k)
                name = base + std::to_string(k);
            op.lines = { std::string(in ? ".input " : ".output ") + name + " " + cir::freshNode(nl_) };
        }
        else
        {
            op.lines = cir::placementLines(nl_, kind, &name);
            op.orient = o;
        }
        if (op.lines.empty())
            return false;
        if (!commit({ op }))
            return false;
        selectPart(name, false);
        return true;
    }

    bool SchematicView::startCircuit()
    {
        ensure();
        if (!showing() || !nl_.elements.empty() || nl_.input || nl_.output)
            return false;
        std::string base = text_;
        const bool blank = base.find_first_not_of(" \t\n") == std::string::npos;
        if (blank)
            base = "* " + file_ + ": a circuit drawn in the schematic\n.input u in\n.output vout out\n.end\n";
        else
        {
            if (!base.empty() && base.back() != '\n')
                base += '\n';
            const cir::Netlist nl = cir::read(base);
            const std::string seed = ".input u in\n.output vout out\n";
            if (nl.endLine >= 0)
            {
                std::size_t at = 0;
                for (int l = 0; l < nl.endLine; ++l)
                    at = base.find('\n', at) + 1;
                base.insert(at, seed);
            }
            else
                base += seed + ".end\n";
        }
        std::vector<cir::Op> ops(2);
        ops[0].type = ops[1].type = cir::Op::Type::place;
        ops[0].name = "u";
        ops[0].at = cir::Pt{ 0, 0 };
        ops[1].name = "vout";
        ops[1].at = cir::Pt{ 24, 0 };
        const cir::Applied a = cir::apply(base, cir::reconcile(base), ops);
        if (!a.error.empty())
        {
            say(friendly(a.error));
            return false;
        }
        if (!commitText(a.text))
            return false;
        fit(false);
        return true;
    }

    bool SchematicView::moving(int part) const
    {
        if (drag_ != Drag::move || st_ == nullptr)
            return false;
        const std::string& n = d_.parts[static_cast<std::size_t>(part)].name;
        return std::find(st_->selected.begin(), st_->selected.end(), n) != st_->selected.end();
    }

    bool SchematicView::rotate(bool clockwise)
    {
        ensure();
        if (tool_ == Tool::place)
        {
            // a part carried: its ghost turns (Cmd-R, Cmd-Shift-R; a popup click)
            orient_ = clockwise ? cir::rotatedCw(orient_) : cir::rotatedCcw(orient_);
            return true;
        }
        if (st_ == nullptr || st_->selected.empty())
            return false;
        std::vector<int> parts;
        cir::Box box;
        for (const std::string& s : st_->selected)
            if (const int i = d_.part(s); i >= 0)
            {
                parts.push_back(i);
                box = box.joined(d_.parts[static_cast<std::size_t>(i)].body);
            }
        if (parts.empty())
            return false;
        std::vector<cir::Op> ops;
        if (parts.size() == 1)
        {
            const cir::Part& p = d_.parts[static_cast<std::size_t>(parts[0])];
            if (isTerminal(p))
            {
                say("A terminal does not turn");
                return false;
            }
            cir::Op op;
            op.type = cir::Op::Type::place;
            op.name = p.name;
            op.at = p.origin;
            op.orient = clockwise ? cir::rotatedCw(p.orient) : cir::rotatedCcw(p.orient);
            ops.push_back(op);
        }
        else
        {
            // as a group about the selection's centre, on the grid
            const cir::Pt c{ (box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2 };
            for (const int i : parts)
            {
                const cir::Part& p = d_.parts[static_cast<std::size_t>(i)];
                const cir::Pt r = p.origin - c;
                cir::Op op;
                op.type = cir::Op::Type::place;
                op.name = p.name;
                op.at = c + (clockwise ? cir::Pt{ -r.y, r.x } : cir::Pt{ r.y, -r.x });
                if (!isTerminal(p))
                    op.orient = clockwise ? cir::rotatedCw(p.orient) : cir::rotatedCcw(p.orient);
                ops.push_back(op);
            }
        }
        return commit(ops);
    }

    bool SchematicView::mirror()
    {
        ensure();
        if (tool_ == Tool::place)
        {
            orient_ = cir::mirrored(orient_);            // a part carried: its ghost mirrors (Cmd-E)
            return true;
        }
        if (st_ == nullptr || st_->selected.empty())
            return false;
        std::vector<int> parts;
        cir::Box box;
        for (const std::string& s : st_->selected)
            if (const int i = d_.part(s); i >= 0)
            {
                parts.push_back(i);
                box = box.joined(d_.parts[static_cast<std::size_t>(i)].body);
            }
        if (parts.empty())
            return false;
        const int cx = (box.x0 + box.x1) / 2;
        std::vector<cir::Op> ops;
        for (const int i : parts)
        {
            const cir::Part& p = d_.parts[static_cast<std::size_t>(i)];
            if (isTerminal(p) && parts.size() == 1)
            {
                say("A terminal does not turn");
                return false;
            }
            cir::Op op;
            op.type = cir::Op::Type::place;
            op.name = p.name;
            op.at = parts.size() == 1 ? p.origin : cir::Pt{ 2 * cx - p.origin.x, p.origin.y };
            if (!isTerminal(p))
                op.orient = cir::mirrored(p.orient);
            ops.push_back(op);
        }
        return commit(ops);
    }

    bool SchematicView::deleteSelection()
    {
        ensure();
        if (st_ == nullptr || (st_->selected.empty() && runs_.empty()))
            return false;
        if (d_.readOnly)
        {
            say("Drawn by a newer playground: read only here");
            return false;
        }
        std::string text = text_;
        // The wires first, one cut at a time (a cut may rename the side it splits off): each by an edge of its run.
        for (const Run& r : runs_)
        {
            const cir::Drawing d = cir::reconcile(text, st_->pins);
            std::string net;
            for (const cir::Net& n : d.nets)
                if (std::binary_search(n.edges.begin(), n.edges.end(), r.edges.front()))
                    net = n.name;
            if (net.empty())
                continue;
            cir::Op cut;
            cut.type = cir::Op::Type::cut;
            cut.name = net;
            cut.path = pathOf(r.edges);
            const cir::Applied a = cir::apply(text, d, std::span<const cir::Op>(&cut, 1), st_->pins);
            if (!a.error.empty())
            {
                say(friendly(a.error));
                return false;
            }
            text = a.text;
        }
        if (!st_->selected.empty())
        {
            cir::Op del;
            del.type = cir::Op::Type::remove;
            del.names = st_->selected;
            const cir::Applied a = cir::apply(text, cir::reconcile(text, st_->pins), std::span<const cir::Op>(&del, 1), st_->pins);
            if (!a.error.empty())
            {
                say(friendly(a.error));
                return false;
            }
            text = a.text;
        }
        const std::size_t parts = st_->selected.size(), wires = runs_.size();
        if (!commitText(text))
            return false;
        setSelection({}, {});
        char what[96];
        std::snprintf(what, sizeof what, "Deleted %zu part%s and %zu wire%s", parts, parts == 1 ? "" : "s", wires,
                      wires == 1 ? "" : "s");
        say(what);
        return true;
    }

    bool SchematicView::nudge(int dx, int dy)
    {
        ensure();
        if (st_ == nullptr || st_->selected.empty())
            return false;
        std::vector<cir::Op> ops;
        for (const std::string& s : st_->selected)
            if (const int i = d_.part(s); i >= 0)
            {
                const cir::Part& p = d_.parts[static_cast<std::size_t>(i)];
                cir::Op op;
                op.type = cir::Op::Type::place;
                op.name = p.name;
                op.at = p.origin + cir::Pt{ dx, dy };
                if (!isTerminal(p))
                    op.orient = p.orient;
                ops.push_back(op);
            }
        return commit(ops);
    }

    bool SchematicView::tidy()
    {
        cir::Op op;
        op.type = cir::Op::Type::layout;
        const bool ok = commit({ op });
        if (ok)
            fit(false);
        return ok;
    }

    void SchematicView::undo()
    {
        if (ed_ != nullptr && ed_->undo())
            ensure();
    }

    void SchematicView::redo()
    {
        if (ed_ != nullptr && ed_->redo())
            ensure();
    }

    void SchematicView::openNetlistAtPart(int part)
    {
        if (part < 0 || part >= static_cast<int>(d_.parts.size()) || !hooks_.showNetlist)
            return;
        const cir::Part& p = d_.parts[static_cast<std::size_t>(part)];
        if (p.element >= 0)
        {
            const cir::Element& e = nl_.elements[static_cast<std::size_t>(p.element)];
            const cir::Token& t = e.valueToken >= 0 && e.valueToken < static_cast<int>(e.tokens.size())
                                      ? e.tokens[static_cast<std::size_t>(e.valueToken)]
                                      : e.tokens.front();
            hooks_.showNetlist(t.at.line, t.at.col);
            return;
        }
        const std::optional<cir::Terminal>& t = p.kind == cir::Kind::Input ? nl_.input : nl_.output;
        if (t)
            hooks_.showNetlist(t->line, t->nameToken.at.col);
    }

    void SchematicView::stepProblem(int dir)
    {
        if (marks_.empty())
            return;
        const int n = static_cast<int>(marks_.size());
        stepAt_ = stepAt_ < 0 ? (dir > 0 ? 0 : n - 1) : ((stepAt_ + dir) % n + n) % n;
        const Mark& m = marks_[static_cast<std::size_t>(stepAt_)];
        if (m.parts.empty())
        {
            if (hooks_.showNetlist)
                hooks_.showNetlist(m.line, m.col);
            return;
        }
        selectPart(d_.parts[static_cast<std::size_t>(m.parts.front())].name, true);
        say(m.text);
    }

    // ---- the pointer ----------------------------------------------------------------------------------------------------
    cir::End SchematicView::endOf(const Hit& h) const
    {
        cir::End e;
        if (h.kind == Hit::Kind::pin)
        {
            const cir::Part& p = d_.parts[static_cast<std::size_t>(h.part)];
            e.part = p.name;
            e.pin = isTerminal(p) ? std::string() : p.pins[static_cast<std::size_t>(h.pin)].name;
        }
        else if (h.kind == Hit::Kind::wire)
            e.node = d_.nets[static_cast<std::size_t>(h.net)].name;
        return e;
    }

    std::vector<cir::Pt> SchematicView::wirePath() const
    {
        const cir::Pt corner = wireH_ ? cir::Pt{ wireTo_.x, wireFrom_.y } : cir::Pt{ wireFrom_.x, wireTo_.y };
        std::vector<cir::Pt> path{ wireFrom_ };
        if (corner != wireFrom_ && corner != wireTo_)
            path.push_back(corner);
        path.push_back(wireTo_);
        return path;
    }

    void SchematicView::pointerMove(funkgui::Point p)
    {
        if (drag_ == Drag::none)
            pointer_ = p;
    }

    void SchematicView::pointerExit()
    {
        if (drag_ == Drag::none)
            pointer_ = { -1.0e4f, -1.0e4f };
    }

    void SchematicView::pointerDown(const funkgui::PointerEvent& e)
    {
        if (!showing())
            return;
        ensure();
        const funkgui::Point p{ e.x, e.y };
        pointer_ = p;
        if (toolbar_.contains(p))
        {
            drag_ = Drag::none;
            if (e.popup)
                return;
            for (int b = 0; b < static_cast<int>(Button::count); ++b)
            {
                if (!buttons_[static_cast<std::size_t>(b)].contains(p))
                    continue;
                const Button btn = static_cast<Button>(b);
                switch (btn)
                {
                    case Button::select: setTool(Tool::select); break;
                    case Button::ground: setTool(tool_ == Tool::ground ? Tool::select : Tool::ground); break;
                    case Button::r: case Button::c: case Button::l: case Button::d: case Button::p: case Button::x:
                    case Button::s:
                        setTool(Tool::place, kBarKinds[b - static_cast<int>(Button::r)]);
                        break;
                    case Button::more: openMenu(p); break;
                    case Button::undo: undo(); break;
                    case Button::redo: redo(); break;
                    case Button::tidy: tidy(); break;
                    case Button::zoomOut: zoomTo(zoom() / kStepZoom, { canvas_.centreX(), canvas_.centreY() }); persist(); break;
                    case Button::zoom: fit(false); persist(); break;
                    case Button::zoomIn: zoomTo(zoom() * kStepZoom, { canvas_.centreX(), canvas_.centreY() }); persist(); break;
                    case Button::count: break;
                }
                return;
            }
            return;
        }
        if (strip_.contains(p))
        {
            drag_ = Drag::none;
            if (!e.popup && p.x >= strip_.centreX() && !marks_.empty())
                stepProblem(e.mods.shift ? -1 : 1);
            return;
        }
        if (!canvas_.contains(p))
            return;
        press_ = Press{ p, e.mods, e.popup, hitAt(p), ed_->revision(), view().world(p) };
        drag_ = Drag::press;
        panned_ = false;
    }

    void SchematicView::beginDrag(funkgui::Point p)
    {
        if (ed_->revision() != press_.rev)
        {
            drag_ = Drag::none;
            say("The circuit changed: drag again");
            return;
        }
        if (press_.popup || press_.mods.alt)
        {
            drag_ = Drag::pan;
            panLast_ = press_.at;
            followPointer(p);
            return;
        }
        if (tool_ == Tool::place)
        {
            drag_ = Drag::carry;
            return;
        }
        if (tool_ == Tool::ground)
        {
            drag_ = Drag::none;
            return;
        }
        const Hit& h = press_.hit;
        switch (h.kind)
        {
            case Hit::Kind::part:
            {
                const std::string& name = d_.parts[static_cast<std::size_t>(h.part)].name;
                if (std::find(st_->selected.begin(), st_->selected.end(), name) == st_->selected.end())
                    setSelection({ name }, {});
                drag_ = Drag::move;
                moveBy_ = {};
                moveRefused_ = false;
                break;
            }
            case Hit::Kind::pin:
            case Hit::Kind::wire:
                drag_ = Drag::wire;
                wireFrom_ = h.at;
                wireTo_ = h.at;
                wireDecided_ = false;
                wireH_ = true;
                wireStart_ = endOf(h);
                break;
            case Hit::Kind::none:
                drag_ = Drag::marquee;
                marqueeA_ = press_.world;
                marqueeB_ = press_.world;
                break;
            case Hit::Kind::badge:
            case Hit::Kind::start:
                drag_ = Drag::none;
                break;
        }
        followPointer(p);
    }

    void SchematicView::followPointer(funkgui::Point p)
    {
        const funkgui::Point w = view().world(p);
        switch (drag_)
        {
            case Drag::move:
            {
                moveBy_ = { static_cast<int>(std::lround(w.x - press_.world.x)), static_cast<int>(std::lround(w.y - press_.world.y)) };
                // a drop onto another body, terminal or flag is refused (and drawn so)
                moveRefused_ = false;
                for (std::size_t i = 0; i < d_.parts.size() && !moveRefused_; ++i)
                {
                    if (!moving(static_cast<int>(i)))
                        continue;
                    const cir::Box b = d_.parts[i].body.moved(moveBy_);
                    for (std::size_t j = 0; j < d_.parts.size() && !moveRefused_; ++j)
                        moveRefused_ = !moving(static_cast<int>(j)) && b.meets(d_.parts[j].body);
                    for (const cir::Flag& f : d_.flags)
                        moveRefused_ = moveRefused_ || (!moving(f.pin.part) && b.meets(f.box));
                }
                break;
            }
            case Drag::wire:
            {
                wireTo_ = gridAt(p);
                if (!wireDecided_)
                {
                    const float dx = std::fabs(w.x - static_cast<float>(wireFrom_.x)), dy = std::fabs(w.y - static_cast<float>(wireFrom_.y));
                    if (std::max(dx, dy) >= 0.6f)
                    {
                        wireH_ = dx >= dy;               // the L closer to the path taken: its first leg
                        wireDecided_ = true;
                    }
                }
                break;
            }
            case Drag::marquee:
                marqueeB_ = w;
                break;
            case Drag::pan:
                panBy(p.x - panLast_.x, p.y - panLast_.y);
                panLast_ = p;
                panned_ = true;
                break;
            case Drag::none:
            case Drag::press:
            case Drag::carry:
                break;
        }
    }

    void SchematicView::pointerDrag(const funkgui::PointerEvent& e)
    {
        if (!showing())
            return;
        const funkgui::Point p{ e.x, e.y };
        pointer_ = p;
        if (drag_ == Drag::press)
        {
            if (dist(p, press_.at) >= kDragPx)
                beginDrag(p);
            return;
        }
        if (drag_ != Drag::none)
            followPointer(p);
    }

    void SchematicView::pointerUp(const funkgui::PointerEvent& e)
    {
        if (!showing())
            return;
        const funkgui::Point p{ e.x, e.y };
        pointer_ = p;
        if (drag_ == Drag::press)
        {
            drag_ = Drag::none;
            if (ed_->revision() == press_.rev)
                click(e);
            return;
        }
        if (drag_ != Drag::none)
            finishDrag(p);
        drag_ = Drag::none;
    }

    void SchematicView::finishDrag(funkgui::Point p)
    {
        const Drag d = drag_;
        drag_ = Drag::none;
        if (d != Drag::pan && ed_->revision() != press_.rev)
        {
            say("The circuit changed: drag again");
            return;
        }
        ensure();
        switch (d)
        {
            case Drag::move:
            {
                if (moveBy_ == cir::Pt{})
                    break;
                std::vector<cir::Op> ops;
                for (const std::string& s : st_->selected)
                    if (const int i = d_.part(s); i >= 0)
                    {
                        const cir::Part& part = d_.parts[static_cast<std::size_t>(i)];
                        cir::Op op;
                        op.type = cir::Op::Type::place;
                        op.name = part.name;
                        op.at = part.origin + moveBy_;
                        if (!isTerminal(part))
                            op.orient = part.orient;
                        ops.push_back(op);
                    }
                const std::vector<std::string> keep = st_->selected;
                if (commit(ops))
                    st_->selected = keep;
                moveBy_ = {};
                moveRefused_ = false;
                break;
            }
            case Drag::wire:
            {
                followPointer(p);
                const Hit h = hitAt(p);
                cir::End to;
                if (h.kind == Hit::Kind::pin && !(h.at == wireFrom_))
                {
                    to = endOf(h);
                    wireTo_ = h.at;
                }
                else if (h.kind == Hit::Kind::wire)
                {
                    to = endOf(h);
                    wireTo_ = h.at;
                }
                else
                {
                    say("A wire ends on a pin or a wire: none drawn");
                    break;
                }
                if (wireTo_ == wireFrom_)
                    break;
                cir::Op op;
                op.type = cir::Op::Type::connect;
                op.a = wireStart_;
                op.b = to;
                op.path = wirePath();
                commit({ op });
                break;
            }
            case Drag::marquee:
            {
                const float x0 = std::min(marqueeA_.x, marqueeB_.x), x1 = std::max(marqueeA_.x, marqueeB_.x);
                const float y0 = std::min(marqueeA_.y, marqueeB_.y), y1 = std::max(marqueeA_.y, marqueeB_.y);
                const auto inside = [&](cir::Pt q) {
                    return static_cast<float>(q.x) >= x0 && static_cast<float>(q.x) <= x1 && static_cast<float>(q.y) >= y0
                        && static_cast<float>(q.y) <= y1;
                };
                std::vector<std::string> parts = press_.mods.shift ? st_->selected : std::vector<std::string>{};
                std::vector<Run> runs = press_.mods.shift ? runs_ : std::vector<Run>{};
                for (const cir::Part& part : d_.parts)
                    if (inside({ part.body.x0, part.body.y0 }) && inside({ part.body.x1, part.body.y1 })
                        && std::find(parts.begin(), parts.end(), part.name) == parts.end())
                        parts.push_back(part.name);
                for (std::size_t n = 0; n < d_.nets.size(); ++n)
                    for (Run& r : runsOf(static_cast<int>(n)))
                        if (std::all_of(r.edges.begin(), r.edges.end(), [&](const cir::UnitEdge& x) { return inside(x.a) && inside(edgeEnd(x)); }))
                            runs.push_back(std::move(r));
                setSelection(std::move(parts), std::move(runs));
                break;
            }
            case Drag::pan:
                if (panned_)
                    persist();
                break;
            case Drag::carry:
                placeAt(kind_, gridAt(p), orient_);
                break;
            case Drag::none:
            case Drag::press:
                break;
        }
    }

    void SchematicView::click(const funkgui::PointerEvent& e)
    {
        const Hit& h = press_.hit;
        if (press_.popup)
        {
            if (tool_ == Tool::place)
                orient_ = cir::rotatedCw(orient_);       // the ghost turns; the part stays carried
            else if (tool_ == Tool::ground)
                setTool(Tool::select);
            else
                openMenu(press_.at);
            return;
        }
        if (press_.mods.alt)
            return;                                      // a probe's pin (s6)
        if (tool_ == Tool::place)
        {
            placeAt(kind_, gridAt({ e.x, e.y }), orient_);
            return;
        }
        if (tool_ == Tool::ground)
        {
            if (h.kind == Hit::Kind::pin || h.kind == Hit::Kind::wire)
            {
                cir::Op op;
                op.type = cir::Op::Type::connect;
                op.a = endOf(h);
                op.b.node = "0";
                commit({ op });
            }
            return;
        }
        const bool shift = press_.mods.shift;
        std::vector<std::string> parts = st_->selected;
        std::vector<Run> runs = runs_;
        const auto toggle = [&](const std::string& name) {
            const auto it = std::find(parts.begin(), parts.end(), name);
            if (it == parts.end())
                parts.push_back(name);
            else
                parts.erase(it);
        };
        switch (h.kind)
        {
            case Hit::Kind::badge:
            {
                setSelection({ d_.parts[static_cast<std::size_t>(h.part)].name }, {});
                for (const Mark& m : marks_)
                    if (std::find(m.parts.begin(), m.parts.end(), h.part) != m.parts.end())
                    {
                        say(m.text);
                        break;
                    }
                return;
            }
            case Hit::Kind::pin:
                setSelection({ d_.parts[static_cast<std::size_t>(h.part)].name }, {});
                return;
            case Hit::Kind::part:
                if (shift)
                {
                    toggle(d_.parts[static_cast<std::size_t>(h.part)].name);
                    setSelection(std::move(parts), std::move(runs));
                }
                else
                    setSelection({ d_.parts[static_cast<std::size_t>(h.part)].name }, {});
                announceSelection();
                return;
            case Hit::Kind::wire:
            {
                Run r = runAt(h.net, h.edge);
                if (shift)
                {
                    const auto it = std::find_if(runs.begin(), runs.end(), [&](const Run& x) { return x.net == r.net && x.edges == r.edges; });
                    if (it == runs.end())
                        runs.push_back(std::move(r));
                    else
                        runs.erase(it);
                    setSelection(std::move(parts), std::move(runs));
                }
                else
                    setSelection({}, { std::move(r) });
                return;
            }
            case Hit::Kind::start:
                startCircuit();
                return;
            case Hit::Kind::none:
                if (!shift)
                    setSelection({}, {});
                return;
        }
    }

    void SchematicView::doubleClick(const funkgui::PointerEvent& e)
    {
        if (!showing())
            return;
        ensure();
        drag_ = Drag::none;
        const Hit h = hitAt({ e.x, e.y });
        switch (h.kind)
        {
            case Hit::Kind::badge:
                for (const Mark& m : marks_)
                    if (std::find(m.parts.begin(), m.parts.end(), h.part) != m.parts.end())
                    {
                        if (hooks_.showNetlist)
                            hooks_.showNetlist(m.line, m.col);
                        return;
                    }
                return;
            case Hit::Kind::part:
            case Hit::Kind::pin:
                if (tool_ == Tool::select)
                    openNetlistAtPart(h.part);           // s4: edits it in place
                return;
            case Hit::Kind::wire:
                setSelection({}, runsOf(h.net));
                return;
            case Hit::Kind::start:
            case Hit::Kind::none:
                return;
        }
    }

    bool SchematicView::wheel(const funkgui::WheelEvent& e, double nowSec)
    {
        if (!showing() || !canvas_.contains({ e.x, e.y }))
            return false;
        const float dy = e.reversed ? -e.dy : e.dy, dx = e.reversed ? -e.dx : e.dx;
        const bool zooms = !e.smooth || e.mods.cmd || e.mods.ctrl || nowSec - zoomedAt_ < 1.0;
        if (zooms)
        {
            const float f = std::pow(2.0f, std::clamp(dy, -0.5f, 0.5f) * 2.0f);
            zoomTo(zoom() * f, { e.x, e.y });
            zoomedAt_ = nowSec;
        }
        else if (e.mods.shift && dx == 0.0f)
            panBy(dy * 512.0f, 0.0f);
        else
            panBy(dx * 512.0f, dy * 512.0f);
        persist();
        return true;
    }

    // ---- keys ---------------------------------------------------------------------------------------------------------
    bool SchematicView::key(const funkgui::KeyEvent& e)
    {
        if (!showing())
            return false;
        ensure();
        const bool cmd = e.mods.cmd, shift = e.mods.shift;
        switch (e.key)
        {
            case funkgui::Key::escape:
                if (gestureOn())
                {
                    cancelGesture(nullptr);
                    return true;
                }
                if (tool_ != Tool::select)
                {
                    setTool(Tool::select);
                    return true;
                }
                if (!st_->selected.empty() || !runs_.empty())
                {
                    setSelection({}, {});
                    return true;
                }
                return false;                            // leave the canvas
            case funkgui::Key::tab:
                stepPart(shift ? -1 : 1);
                return true;
            case funkgui::Key::enter:
                if (!cmd && st_->selected.size() == 1)
                    openNetlistAtPart(d_.part(st_->selected.front()));
                return !cmd;
            case funkgui::Key::backspace:
            case funkgui::Key::del:
                deleteSelection();
                return true;
            case funkgui::Key::left: nudge(shift ? -4 : -1, 0); return true;
            case funkgui::Key::right: nudge(shift ? 4 : 1, 0); return true;
            case funkgui::Key::up: nudge(0, shift ? -4 : -1); return true;
            case funkgui::Key::down: nudge(0, shift ? 4 : 1); return true;
            case funkgui::Key::home:
                fit(false);
                persist();
                return true;
            case funkgui::Key::end:
            case funkgui::Key::pageUp:
            case funkgui::Key::pageDown:
            case funkgui::Key::space:
                return true;
            case funkgui::Key::character:
                break;
        }
        const char ch = static_cast<char>(std::tolower(static_cast<int>(e.ch < 128 ? e.ch : 0)));
        if (cmd)
        {
            switch (ch)
            {
                case 'z': shift ? redo() : undo(); return true;
                case 'y': redo(); return true;
                case 'a': selectAll(); return true;
                case 'r': rotate(!shift); return true;
                case 'e': mirror(); return true;
                case '0': zoomTo(1.0f, { canvas_.centreX(), canvas_.centreY() }); persist(); return true;
                case '=':
                case '+': zoomTo(zoom() * kStepZoom, { canvas_.centreX(), canvas_.centreY() }); persist(); return true;
                case '-':
                case '_': zoomTo(zoom() / kStepZoom, { canvas_.centreX(), canvas_.centreY() }); persist(); return true;
                default: return false;                   // the clipboard's keys are s4's; the rest the host's
            }
        }
        cir::Kind kind = cir::Kind::Unknown;
        switch (ch)
        {
            case 'f': fit(false); persist(); return true;
            case 'a': openMenu(centreOfButton(Button::more)); return true;
            case 'g':
                if (!shift)
                {
                    setTool(tool_ == Tool::ground ? Tool::select : Tool::ground);
                    return true;
                }
                kind = cir::Kind::Vccs;
                break;
            case 'r': kind = cir::Kind::R; break;
            case 'c': kind = cir::Kind::C; break;
            case 'l': kind = cir::Kind::L; break;
            case 'd': kind = cir::Kind::D; break;
            case 'p': kind = shift ? cir::Kind::Rheostat : cir::Kind::Pot; break;
            case 'x': kind = cir::Kind::OpAmp; break;
            case 's': kind = cir::Kind::S; break;
            case 'e': kind = cir::Kind::Vcvs; break;
            case 'v': kind = cir::Kind::V; break;
            case 'i': kind = cir::Kind::I; break;
            case 'o': kind = cir::Kind::Ota; break;
            default: return true;                        // the canvas takes every key
        }
        if (canvas_.contains(pointer_))
        {
            setTool(Tool::place, kind);
            return true;
        }
        // No pointer over the canvas: at the view's centre, or 4 units right of the selection.
        cir::Pt at = gridAt({ canvas_.centreX(), canvas_.centreY() });
        if (!st_->selected.empty())
            if (const int i = d_.part(st_->selected.back()); i >= 0)
            {
                const cir::Part& s = d_.parts[static_cast<std::size_t>(i)];
                const cir::Box body = cir::turned(cir::Orient::r0, cir::symbol(kind).body);
                at = { s.body.x1 + 4 - body.x0, s.origin.y };
            }
        placeAt(kind, at, cir::Orient::r0);
        return true;
    }

    // ---- menus --------------------------------------------------------------------------------------------------------
    void SchematicView::openMenu(funkgui::Point at)
    {
        if (host_ == nullptr || !showing())
            return;
        ensure();
        funkgui::MenuRequest request;
        request.anchor = { at.x, at.y, 1.0f, 1.0f };
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        if (toolbar_.contains(at))
        {
            request.anchor = buttonRect(Button::more);
            for (std::size_t i = 0; i < std::size(kMore); ++i)
            {
                funkgui::MenuItem item{ static_cast<int>(i) + 1, kMore[i].label };
                if (kMore[i].kind == cir::Kind::Input && nl_.input)
                {
                    item.enabled = false;
                    item.label = "Input (the netlist has one: FMinus takes exactly one)";
                }
                if (kMore[i].kind == cir::Kind::Output && nl_.output)
                {
                    item.enabled = false;
                    item.label = "Output (the netlist has one)";
                }
                request.items.push_back(std::move(item));
            }
            host_->showMenu(request, [this](int chosen) {
                if (chosen >= 1 && chosen <= static_cast<int>(std::size(kMore)) && showing())
                    setTool(Tool::place, kMore[static_cast<std::size_t>(chosen - 1)].kind);
            });
            return;
        }
        const Hit h = hitAt(at);
        if (h.kind == Hit::Kind::part || h.kind == Hit::Kind::pin || h.kind == Hit::Kind::badge)
        {
            const std::string name = d_.parts[static_cast<std::size_t>(h.part)].name;
            if (std::find(st_->selected.begin(), st_->selected.end(), name) == st_->selected.end())
                setSelection({ name }, {});
            request.items = { { 1, "Rotate   Cmd-R" }, { 2, "Rotate back   Cmd-Shift-R" }, { 3, "Mirror   Cmd-E" },
                              { 4, "Delete   Delete" }, { 0, {}, true, false, true }, { 5, "Show in netlist" } };
            host_->showMenu(request, [this, name](int chosen) {
                if (!showing())
                    return;
                switch (chosen)
                {
                    case 1: rotate(true); break;
                    case 2: rotate(false); break;
                    case 3: mirror(); break;
                    case 4: deleteSelection(); break;
                    case 5: openNetlistAtPart(d_.part(name)); break;
                    default: break;
                }
            });
            return;
        }
        if (h.kind == Hit::Kind::wire)
        {
            Run r = runAt(h.net, h.edge);
            const bool selected = std::any_of(runs_.begin(), runs_.end(), [&](const Run& x) { return x.net == r.net && x.edges == r.edges; });
            if (!selected)
                setSelection({}, { r });
            const std::string net = d_.nets[static_cast<std::size_t>(h.net)].name;
            request.items = { { 11, "Delete wire   Delete" }, { 12, "Select net" }, { 0, {}, true, false, true },
                              { 13, "Show in netlist" } };
            host_->showMenu(request, [this, net](int chosen) {
                if (!showing())
                    return;
                ensure();
                const int n = d_.net(net);
                if (chosen == 11)
                    deleteSelection();
                else if (chosen == 12 && n >= 0)
                    setSelection({}, runsOf(n));
                else if (chosen == 13 && n >= 0 && hooks_.showNetlist)
                {
                    // its first mention: a node token of its first pin's line
                    for (const cir::Element& e : nl_.elements)
                        for (const cir::Token& t : e.nodes)
                            if (cir::folded(t.text) == cir::folded(net) || (net == "0" && (t.text == "0" || cir::folded(t.text) == "gnd")))
                            {
                                hooks_.showNetlist(t.at.line, t.at.col);
                                return;
                            }
                }
            });
            return;
        }
        request.items = { { 21, "Select all   Cmd-A" }, { 22, "Fit   F" }, { 23, "Tidy" }, { 0, {}, true, false, true },
                          { 24, "Netlist view   Cmd-Shift-D" }, { 0, {}, true, false, true },
                          { 25, "Undo   Cmd-Z", ed_->canUndo() }, { 26, "Redo   Cmd-Shift-Z", ed_->canRedo() } };
        host_->showMenu(request, [this](int chosen) {
            if (!showing())
                return;
            switch (chosen)
            {
                case 21: selectAll(); break;
                case 22: fit(false); persist(); break;
                case 23: tidy(); break;
                case 24:
                    if (hooks_.showNetlist)
                        hooks_.showNetlist(-1, -1);
                    break;
                case 25: undo(); break;
                case 26: redo(); break;
                default: break;
            }
        });
    }

    // ---- frames -------------------------------------------------------------------------------------------------------
    void SchematicView::focusChanged(bool focused) { focused_ = focused; }

    void SchematicView::tick(float dt, funkgui::Point pointer, bool focused)
    {
        focused_ = focused;
        if (!showing())
            return;
        if (drag_ == Drag::none || drag_ == Drag::press)
            pointer_ = pointer;
        ensure();
        if (fitPending_ && canvas_.w > 0.0f && canvas_.h > 0.0f)
            fit(true);
        computeMarks();
        if (easing_)
        {
            st_->zoom = funkgui::ease::toward(st_->zoom, zoomTo_, dt, kEaseTau, 1e-4f);
            st_->cx = funkgui::ease::toward(st_->cx, cxTo_, dt, kEaseTau, 1e-3f);
            st_->cy = funkgui::ease::toward(st_->cy, cyTo_, dt, kEaseTau, 1e-3f);
            easing_ = !(st_->zoom == zoomTo_ && st_->cx == cxTo_ && st_->cy == cyTo_);
        }
        if (noticeLeft_ > 0.0f)
        {
            noticeLeft_ = std::max(0.0f, noticeLeft_ - dt);
            if (noticeLeft_ == 0.0f)
                notice_.clear();
        }
        for (auto it = flash_.begin(); it != flash_.end();)
        {
            it->second -= dt;
            it = it->second <= 0.0f ? flash_.erase(it) : std::next(it);
        }
        // A gesture whose base the text left (Claude's write merged, a session loaded): cancelled, nothing applied.
        if ((drag_ == Drag::move || drag_ == Drag::wire || drag_ == Drag::marquee || drag_ == Drag::carry || drag_ == Drag::press)
            && ed_->revision() != press_.rev)
        {
            drag_ = Drag::none;
            moveBy_ = {};
            moveRefused_ = false;
            say("The circuit changed: drag again");
        }
        // Edge auto-pan: a left drag, or a part carried, near the canvas's edge.
        const bool carrying = tool_ == Tool::place && canvas_.contains(pointer_);
        if (drag_ == Drag::move || drag_ == Drag::wire || drag_ == Drag::marquee || drag_ == Drag::carry || carrying)
        {
            const auto depth = [](float fromEdge) { return std::clamp(kEdgePx - fromEdge, 0.0f, kEdgePx); };
            const float l = depth(pointer_.x - canvas_.x), r = depth(canvas_.right() - pointer_.x);
            const float t = depth(pointer_.y - canvas_.y), b = depth(canvas_.bottom() - pointer_.y);
            const float vx = (r - l) / kEdgePx * kEdgeSpeed * dt, vy = (b - t) / kEdgePx * kEdgeSpeed * dt;
            if (vx != 0.0f || vy != 0.0f)
            {
                panBy(-vx, -vy);
                autoPanned_ = true;
                if (drag_ != Drag::none && drag_ != Drag::carry)
                    followPointer(pointer_);
                return;
            }
        }
        // An auto-pan that ended moved the user's view: the session keeps it (a user's gesture).
        if (autoPanned_)
        {
            autoPanned_ = false;
            persist();
        }
    }

    bool SchematicView::settled() const
    {
        if (!showing())
            return true;
        // (a notice's 4 s count down at the idle rate: it asks for no frames of its own)
        if (easing_ || fitPending_ || !flash_.empty())
            return false;
        if (drag_ == Drag::move || drag_ == Drag::wire || drag_ == Drag::marquee || drag_ == Drag::carry)
            return false;
        // a part carried near the edge pans
        if (tool_ == Tool::place && canvas_.contains(pointer_))
            return !(pointer_.x - canvas_.x < kEdgePx || canvas_.right() - pointer_.x < kEdgePx || pointer_.y - canvas_.y < kEdgePx
                     || canvas_.bottom() - pointer_.y < kEdgePx);
        return true;
    }

    funkgui::Cursor SchematicView::cursorAt(funkgui::Point p) const
    {
        if (!showing())
            return funkgui::Cursor::normal;
        if (toolbar_.contains(p) || strip_.contains(p))
        {
            for (const funkgui::Rect& b : buttons_)
                if (b.contains(p))
                    return funkgui::Cursor::pointingHand;
            return strip_.contains(p) && p.x >= strip_.centreX() && !marks_.empty() ? funkgui::Cursor::pointingHand
                                                                                      : funkgui::Cursor::normal;
        }
        if (!canvas_.contains(p))
            return funkgui::Cursor::normal;
        switch (drag_)
        {
            case Drag::move: return moveRefused_ ? funkgui::Cursor::notAllowed : funkgui::Cursor::grabbing;
            case Drag::wire: return funkgui::Cursor::crosshair;
            case Drag::pan: return funkgui::Cursor::grabbing;
            case Drag::carry: return funkgui::Cursor::copy;
            case Drag::marquee:
            case Drag::press:
            case Drag::none:
                break;
        }
        if (tool_ == Tool::place)
            return funkgui::Cursor::copy;
        const Hit h = hitAt(p);
        if (tool_ == Tool::ground)
            return h.kind == Hit::Kind::pin || h.kind == Hit::Kind::wire ? funkgui::Cursor::crosshair : funkgui::Cursor::normal;
        switch (h.kind)
        {
            case Hit::Kind::pin:
            case Hit::Kind::wire: return funkgui::Cursor::crosshair;
            case Hit::Kind::part: return funkgui::Cursor::grab;
            case Hit::Kind::badge:
            case Hit::Kind::start: return funkgui::Cursor::pointingHand;
            case Hit::Kind::none: break;
        }
        return funkgui::Cursor::normal;
    }

    std::optional<funkgui::Point> SchematicView::centreOfPart(const std::string& name) const
    {
        ensure();
        const int i = d_.part(name);
        if (i < 0)
            return std::nullopt;
        const funkgui::Rect r = view().rect(d_.parts[static_cast<std::size_t>(i)].body);
        return funkgui::Point{ r.centreX(), r.centreY() };
    }

    std::optional<funkgui::Point> SchematicView::centreOfPin(const std::string& part, const std::string& pin) const
    {
        ensure();
        const int i = d_.part(part);
        if (i < 0)
            return std::nullopt;
        const cir::Part& p = d_.parts[static_cast<std::size_t>(i)];
        const int k = isTerminal(p) ? 0 : cir::pinIndex(p.kind, pin);
        if (k < 0 || k >= static_cast<int>(p.pins.size()))
            return std::nullopt;
        return view().at(p.pins[static_cast<std::size_t>(k)].at);
    }

    std::optional<funkgui::Point> SchematicView::centreOfBadge(const std::string& part) const
    {
        ensure();
        const int i = d_.part(part);
        if (i < 0 || i >= static_cast<int>(partMarks_.size()))
            return std::nullopt;
        const sch::PartMarks& m = partMarks_[static_cast<std::size_t>(i)];
        if (m.errors + m.warnings + m.notes == 0)
            return std::nullopt;
        return sch::badgeAt(view(), d_.parts[static_cast<std::size_t>(i)]);
    }

    funkgui::Point SchematicView::centreOfProblems() const { return { strip_.right() - 60.0f, strip_.centreY() }; }

    std::optional<funkgui::Point> SchematicView::centreOfStart() const
    {
        if (!showing() || !d_.parts.empty())
            return std::nullopt;
        return funkgui::Point{ canvas_.centreX(), canvas_.centreY() + 14.0f };
    }

    float SchematicView::flashOf(const std::string& part) const
    {
        const auto it = flash_.find(part);
        return it == flash_.end() ? 0.0f : std::clamp(it->second / kFlashSeconds, 0.0f, 1.0f);
    }

    // ---- words --------------------------------------------------------------------------------------------------------
    std::string SchematicView::partWords(int part) const
    {
        const cir::Part& p = d_.parts[static_cast<std::size_t>(part)];
        std::string s = p.name + ", " + cir::symbol(p.kind).word;
        const std::string value = sch::valueText(nl_, p);
        if (!value.empty())
            s += " " + value;
        const auto netName = [&](const cir::PinAt& pin) {
            return pin.net >= 0 ? d_.nets[static_cast<std::size_t>(pin.net)].name : std::string("nothing");
        };
        if (p.pins.size() == 1)
            s += ", on " + netName(p.pins[0]);
        else if (p.pins.size() == 2)
            s += ", from " + netName(p.pins[0]) + " to " + netName(p.pins[1]);
        else
            for (std::size_t k = 0; k < p.pins.size(); ++k)
                s += (k == 0 ? ", " : "; ") + p.pins[k].name + " " + netName(p.pins[k]);
        return s;
    }

    std::string SchematicView::netWords(int net) const
    {
        const cir::Net& n = d_.nets[static_cast<std::size_t>(net)];
        std::string s = "net " + n.name + ":";
        for (std::size_t k = 0; k < n.pins.size(); ++k)
        {
            const cir::Part& p = d_.parts[static_cast<std::size_t>(n.pins[k].part)];
            s += (k == 0 ? " " : ", ") + pinName(p, n.pins[k].pin);
        }
        return s;
    }

    std::string SchematicView::stripLeft() const
    {
        if (!notice_.empty())
            return notice_;
        if (st_ == nullptr)
            return {};
        if (st_->selected.size() == 1 && runs_.empty())
        {
            const int i = d_.part(st_->selected.front());
            if (i >= 0)
            {
                const cir::Part& p = d_.parts[static_cast<std::size_t>(i)];
                std::string s = p.name;
                if (const std::string v = sch::valueText(nl_, p); !v.empty())
                    s += "   " + v;
                std::string nets;
                for (const cir::PinAt& pin : p.pins)
                    nets += (nets.empty() ? "" : " - ") + (pin.net >= 0 ? d_.nets[static_cast<std::size_t>(pin.net)].name : std::string("?"));
                return s + "   " + nets;
            }
        }
        if (st_->selected.empty() && runs_.size() == 1)
        {
            const int n = d_.net(runs_.front().net);
            if (n >= 0)
                return "Wire of " + d_.nets[static_cast<std::size_t>(n)].name + "   " + netWords(n).substr(netWords(n).find(':') + 2);
        }
        if (!st_->selected.empty() || !runs_.empty())
        {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%zu part%s, %zu wire%s selected", st_->selected.size(),
                          st_->selected.size() == 1 ? "" : "s", runs_.size(), runs_.size() == 1 ? "" : "s");
            return buf;
        }
        if (d_.readOnly)
            return "Drawn by a newer playground: read only here";
        switch (tool_)
        {
            case Tool::place:
                return std::string("Click to place a ") + cir::symbol(kind_).word + "   Cmd-R turns it   Escape ends";
            case Tool::ground:
                return "Click a pin or a wire: its net goes to ground   Escape ends";
            case Tool::select:
                break;
        }
        if (d_.parts.empty())
            return "An empty netlist";
        return "Drag from a pin to wire it   R C L D P X S place parts   G grounds";
    }

    std::string SchematicView::stripRight() const
    {
        if (!marks_.empty())
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%zu PROBLEM%s", marks_.size(), marks_.size() == 1 ? "" : "S");
            return buf;
        }
        if (!d_.notDrawn.empty())
        {
            std::string s = "Not drawn: ";
            for (std::size_t i = 0; i < d_.notDrawn.size(); ++i)
                s += (i == 0 ? "" : ", ") + d_.notDrawn[i];
            return s;
        }
        if (hooks_.played && !hooks_.played())
            return "Not played: no program #includes " + file_;
        return {};
    }

    std::string SchematicView::statusWords() const
    {
        if (!showing())
            return {};
        int nets = 0;
        for (const cir::Net& n : d_.nets)
            nets += n.pins.empty() ? 0 : 1;
        char buf[128];
        std::snprintf(buf, sizeof buf, "Schematic   %zu part%s   %d net%s   %zu problem%s   %d%%", d_.parts.size(),
                      d_.parts.size() == 1 ? "" : "s", nets, nets == 1 ? "" : "s", marks_.size(), marks_.size() == 1 ? "" : "s",
                      static_cast<int>(std::lround(zoom() * 100.0f)));
        return buf;
    }

    bool SchematicView::specLine(char* out, std::size_t n, funkgui::Point pointer) const
    {
        if (!showing() || !area_.contains(pointer))
            return false;
        static const char* const kButtonWords[] = {
            "SELECT   CLICK A PART, A WIRE OR NOTHING; DRAG TO MOVE OR TO DRAW A MARQUEE",
            "GROUND (G)   CLICK A PIN OR A WIRE: ITS NET GOES TO GROUND",
            "RESISTOR (R)   CLICK, THEN CLICK THE CANVAS TO PLACE ONE",
            "CAPACITOR (C)   CLICK, THEN CLICK THE CANVAS TO PLACE ONE",
            "INDUCTOR (L)   CLICK, THEN CLICK THE CANVAS TO PLACE ONE",
            "DIODE (D)   CLICK, THEN CLICK THE CANVAS TO PLACE ONE",
            "POT (P)   A POT AND ITS .POT LINE: CLICK, THEN CLICK THE CANVAS",
            "OP AMP (X)   CLICK, THEN CLICK THE CANVAS TO PLACE ONE",
            "SWITCH (S)   A SWITCH AND ITS .SWITCH LINE: CLICK, THEN CLICK THE CANVAS",
            "MORE (A)   VCVS, VCCS, OTA, DC SOURCES, A RHEOSTAT, THE INPUT AND THE OUTPUT",
            "UNDO (CMD-Z)   THE TAB'S OWN: THE NETLIST VIEW'S STEPS TOO",
            "REDO (CMD-SHIFT-Z)",
            "TIDY   LAY THE WHOLE DRAWING OUT AGAIN (ONE STEP: CMD-Z TAKES IT BACK)",
            "ZOOM OUT (CMD--)",
            "ZOOM   A CLICK FITS THE DRAWING (F)",
            "ZOOM IN (CMD-=)",
        };
        for (std::size_t b = 0; b < buttons_.size(); ++b)
            if (buttons_[b].contains(pointer))
            {
                std::snprintf(out, n, "%s", kButtonWords[b]);
                return true;
            }
        if (strip_.contains(pointer))
        {
            if (!marks_.empty())
                std::snprintf(out, n, "PROBLEMS   A CLICK SELECTS THE NEXT ONE'S PART   SHIFT-CLICK THE ONE BEFORE");
            else
                std::snprintf(out, n, "SCHEMATIC   CMD-SHIFT-D SHOWS THE NETLIST   EVERY GESTURE IS ONE UNDO STEP");
            return true;
        }
        if (!canvas_.contains(pointer))
            return false;
        const Hit h = hitAt(pointer);
        switch (h.kind)
        {
            case Hit::Kind::badge:
                for (const Mark& m : marks_)
                    if (std::find(m.parts.begin(), m.parts.end(), h.part) != m.parts.end())
                    {
                        std::snprintf(out, n, "%s:%d   %s", upper(file_).c_str(), m.line + 1, asciiText(m.text).c_str());
                        return true;
                    }
                break;
            case Hit::Kind::pin:
            {
                const cir::Part& p = d_.parts[static_cast<std::size_t>(h.part)];
                const std::string net = h.net >= 0 ? d_.nets[static_cast<std::size_t>(h.net)].name : std::string("?");
                std::snprintf(out, n, "%s ON %s   DRAG TO A PIN OR A WIRE TO CONNECT", upper(pinName(p, h.pin)).c_str(),
                              upper(net).c_str());
                return true;
            }
            case Hit::Kind::part:
                std::snprintf(out, n, "%s   DRAG MOVES IT   DOUBLE-CLICK SHOWS ITS VALUE IN THE NETLIST   CMD-R TURNS IT",
                              upper(asciiText(partWords(h.part))).c_str());
                return true;
            case Hit::Kind::wire:
                std::snprintf(out, n, "%s   CLICK SELECTS THE RUN   DRAG DRAWS A WIRE FROM HERE   DOUBLE-CLICK THE NET",
                              upper(asciiText(netWords(h.net))).c_str());
                return true;
            case Hit::Kind::start:
                std::snprintf(out, n, "START A CIRCUIT   AN INPUT AND AN OUTPUT, 24 UNITS APART");
                return true;
            case Hit::Kind::none:
                break;
        }
        std::snprintf(out, n, "SCHEMATIC   WHEEL OR CMD-WHEEL ZOOMS   TRACKPAD PANS   RIGHT-DRAG PANS   F FITS");
        return true;
    }

    // ---- accessibility ------------------------------------------------------------------------------------------------
    uint32_t SchematicView::a11yRevision() const
    {
        ensure();
        return structure_;
    }

    uint32_t SchematicView::idOf(const std::string& key) const
    {
        const auto it = ids_.find(key);
        return it == ids_.end() ? 0u : it->second;
    }

    void SchematicView::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (!showing())
            return;
        ensure();
        const auto clamp = [&](funkgui::Rect r) {
            const float x0 = std::clamp(r.x, canvas_.x, canvas_.right() - 1.0f), y0 = std::clamp(r.y, canvas_.y, canvas_.bottom() - 1.0f);
            const float x1 = std::clamp(r.right(), x0 + 1.0f, canvas_.right()), y1 = std::clamp(r.bottom(), y0 + 1.0f, canvas_.bottom());
            return funkgui::Rect{ x0, y0, x1 - x0, y1 - y0 };
        };
        funkgui::A11yItem group;
        group.id = kA11yBase;
        group.role = funkgui::a11yrole::group;
        group.bounds = area_;
        group.title = "Schematic, " + file_;
        group.description = statusWords();
        group.help = "The Netlist view (Command Shift D) is the keyboard's way to edit it";
        out.push_back(group);
        funkgui::A11yItem parts;
        parts.id = kA11yBase + 1;
        parts.parent = kA11yBase;
        parts.role = funkgui::a11yrole::list;
        parts.bounds = canvas_;
        parts.title = "Parts";
        out.push_back(parts);
        const sch::View v = view();
        // at most kA11yItems in all (the group and its two lists among them): parts first, then nets
        std::size_t left = kA11yItems - 3;
        for (std::size_t i = 0; i < d_.parts.size() && left > 0; ++i, --left)
        {
            funkgui::A11yItem it;
            it.id = idOf("p:" + d_.parts[i].name);
            it.parent = parts.id;
            it.role = funkgui::A11yRole::listItem;
            it.bounds = clamp(v.rect(d_.parts[i].body));
            it.title = asciiText(partWords(static_cast<int>(i)));
            it.checkable = true;
            it.checked = std::find(st_->selected.begin(), st_->selected.end(), d_.parts[i].name) != st_->selected.end();
            out.push_back(std::move(it));
        }
        funkgui::A11yItem nets;
        nets.id = kA11yBase + 2;
        nets.parent = kA11yBase;
        nets.role = funkgui::a11yrole::list;
        nets.bounds = canvas_;
        nets.title = "Nets";
        out.push_back(nets);
        for (std::size_t n = 0; n < d_.nets.size() && left > 0; ++n)
        {
            const cir::Net& net = d_.nets[n];
            if (net.pins.empty())
                continue;
            --left;
            cir::Box b;
            for (const cir::PinRef& pr : net.pins)
                b = b.joined(cir::Box::of(d_.parts[static_cast<std::size_t>(pr.part)].pins[static_cast<std::size_t>(pr.pin)].at,
                                          d_.parts[static_cast<std::size_t>(pr.part)].pins[static_cast<std::size_t>(pr.pin)].at));
            for (const cir::UnitEdge& e : net.edges)
                b = b.joined(cir::Box::of(e.a, edgeEnd(e)));
            funkgui::A11yItem it;
            it.id = idOf("n:" + net.name);
            it.parent = nets.id;
            it.role = funkgui::A11yRole::listItem;
            it.bounds = clamp(v.rect(b).expanded(3.0f));
            it.title = asciiText(netWords(static_cast<int>(n)));
            out.push_back(std::move(it));
        }
    }

    bool SchematicView::a11yAction(uint32_t id, funkgui::A11yAction a)
    {
        if (!showing() || (id & 0xFF000000u) != kA11yBase)
            return false;
        ensure();
        (void)a;
        if (id == kA11yBase || id == kA11yBase + 1 || id == kA11yBase + 2)
            return true;
        for (const cir::Part& p : d_.parts)
            if (idOf("p:" + p.name) == id)
            {
                selectPart(p.name, true);
                return true;
            }
        for (std::size_t n = 0; n < d_.nets.size(); ++n)
            if (!d_.nets[n].pins.empty() && idOf("n:" + d_.nets[n].name) == id)
            {
                setSelection({}, runsOf(static_cast<int>(n)));
                cir::Box b;
                for (const cir::PinRef& pr : d_.nets[n].pins)
                    b = b.joined(cir::Box::of(d_.parts[static_cast<std::size_t>(pr.part)].pins[static_cast<std::size_t>(pr.pin)].at,
                                              d_.parts[static_cast<std::size_t>(pr.part)].pins[static_cast<std::size_t>(pr.pin)].at));
                if (!b.empty() && st_ != nullptr)
                {
                    st_->cx = 0.5f * static_cast<float>(b.x0 + b.x1);
                    st_->cy = 0.5f * static_cast<float>(b.y0 + b.y1);
                    st_->framed = true;
                    easing_ = false;
                }
                return true;
            }
        return true;
    }

    // ---- drawing ------------------------------------------------------------------------------------------------------
    void SchematicView::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focused) const
    {
        if (!showing())
            return;
        ensure();
        const chat::Palette pal = chat::palette(th);
        const funkgui::TextEditorOptions& eo = ed_->options();
        sch::Inks k = sch::inksFor(th, eo.inks.error.resolve(th), eo.inks.warning.resolve(th), eo.inks.info.resolve(th), focused);
        k.ground = codeGround(th);                       // the canvas is the code editor's ground
        const sch::View v = view();
        const float sw = v.stroke();
        const float px = sch::labelPx(v, false);

        // ---- the toolbar and the info strip -----------------------------------------------------------------------------
        {
            const funkgui::Canvas::Scope s(c, sch::kSchematicTag, false);
            c.hairlineH(toolbar_.x, toolbar_.bottom() - 1.0f, toolbar_.w, th.ink16);
            for (int b = 0; b < static_cast<int>(Button::count); ++b)
            {
                const funkgui::Rect r = buttons_[static_cast<std::size_t>(b)];
                const Button btn = static_cast<Button>(b);
                const bool hover = r.contains(pointer_);
                bool active = false, enabled = true;
                if (btn == Button::select)
                    active = tool_ == Tool::select;
                else if (btn == Button::ground)
                    active = tool_ == Tool::ground;
                else if (btn >= Button::r && btn <= Button::s)
                    active = tool_ == Tool::place && kind_ == kBarKinds[b - static_cast<int>(Button::r)];
                else if (btn == Button::more)
                    active = tool_ == Tool::place && std::any_of(std::begin(kMore), std::end(kMore), [&](const MoreItem& m) { return m.kind == kind_; });
                else if (btn == Button::undo)
                    enabled = ed_->canUndo();
                else if (btn == Button::redo)
                    enabled = ed_->canRedo();
                if (active)
                    c.rrect(r.x, r.y, r.w, r.h, 6.0f, pal.composer, 1.0f, pal.composerEdge);
                else if (hover && enabled)
                    c.rrect(r.x, r.y, r.w, r.h, 6.0f, pal.hover);
                const funkgui::Col ink = !enabled ? pal.faint : active || hover ? pal.text : pal.soft;
                const float cx = r.centreX(), cy = r.centreY();
                switch (btn)
                {
                    case Button::select:
                    {
                        // the arrow
                        const float xs[5] = { cx - 4.0f, cx - 4.0f, cx - 1.0f, cx + 4.0f, cx - 4.0f };
                        const float ys[5] = { cy - 7.0f, cy + 5.0f, cy + 1.5f, cy + 1.5f, cy - 7.0f };
                        c.polyline(xs, ys, 5, 1.3f, ink);
                        c.segment(cx - 1.0f, cy + 1.5f, cx + 1.5f, cy + 6.5f, 1.3f, ink);
                        break;
                    }
                    case Button::ground:
                        c.segment(cx, cy - 7.0f, cx, cy - 1.0f, 1.3f, ink);
                        c.segment(cx - 6.0f, cy - 1.0f, cx + 6.0f, cy - 1.0f, 1.3f, ink);
                        c.segment(cx - 3.8f, cy + 2.0f, cx + 3.8f, cy + 2.0f, 1.3f, ink);
                        c.segment(cx - 1.6f, cy + 5.0f, cx + 1.6f, cy + 5.0f, 1.3f, ink);
                        break;
                    case Button::r: case Button::c: case Button::l: case Button::d: case Button::p: case Button::x:
                    case Button::s:
                        sch::icon(c, kBarKinds[b - static_cast<int>(Button::r)], cx, cy, 18.0f, ink);
                        break;
                    case Button::more:
                        c.text("More", r.x + 7.0f, c.capCentreTop(cy, chat::kSmall), chat::kSmall, ink);
                        chat::drawIcon(c, chat::Icon::chevronDown, r.right() - 9.0f, cy + 0.5f, 8.0f, ink);
                        break;
                    case Button::undo:
                    case Button::redo:
                    {
                        // a hook and its arrowhead
                        const float dir = btn == Button::undo ? 1.0f : -1.0f;
                        float xs[8], ys[8];
                        for (int i = 0; i < 8; ++i)
                        {
                            const float a = 3.1415927f * (static_cast<float>(i) / 7.0f);
                            xs[i] = cx - dir * 4.5f * std::cos(a) + dir * 0.5f;
                            ys[i] = cy + 1.0f - 4.5f * std::sin(a) + 2.0f;
                        }
                        c.polyline(xs, ys, 8, 1.3f, ink);
                        const float hx = xs[0], hy = ys[0];
                        c.segment(hx, hy, hx + dir * 3.0f, hy - 2.5f, 1.3f, ink);
                        c.segment(hx, hy, hx - dir * 0.5f, hy - 3.8f, 1.3f, ink);
                        break;
                    }
                    case Button::tidy:
                        c.text("Tidy", cx, c.capCentreTop(cy, chat::kSmall), chat::kSmall, ink, funkgui::Align::centre);
                        break;
                    case Button::zoomOut:
                        c.segment(cx - 5.0f, cy, cx + 5.0f, cy, 1.4f, ink);
                        break;
                    case Button::zoomIn:
                        c.segment(cx - 5.0f, cy, cx + 5.0f, cy, 1.4f, ink);
                        c.segment(cx, cy - 5.0f, cx, cy + 5.0f, 1.4f, ink);
                        break;
                    case Button::zoom:
                    {
                        char z[16];
                        std::snprintf(z, sizeof z, "%d%%", static_cast<int>(std::lround(zoom() * 100.0f)));
                        c.text(z, cx, c.capCentreTop(cy, chat::kSmall), chat::kSmall, ink, funkgui::Align::centre);
                        break;
                    }
                    case Button::count:
                        break;
                }
            }
            // the info strip
            c.hairlineH(strip_.x, strip_.y, strip_.w, th.ink16);
            const funkgui::TextStyle& st = chat::kSmall;
            const float top = c.capCentreTop(strip_.centreY(), st);
            const std::string right = stripRight();
            float rightW = 0.0f;
            if (!right.empty())
            {
                const bool problems = !marks_.empty();
                const bool hot = problems && strip_.contains(pointer_) && pointer_.x >= strip_.centreX();
                const std::string shown = fitText(c, right, st, strip_.w * 0.5f - 8.0f);
                rightW = c.textWidth(shown.c_str(), st) + 12.0f;
                funkgui::Col ink = pal.soft;
                if (problems)
                {
                    const bool error = std::any_of(marks_.begin(), marks_.end(), [](const Mark& m) { return m.severity == 0; });
                    ink = hot ? pal.accent : error ? k.error : k.warning;
                }
                c.text(shown.c_str(), strip_.right() - 6.0f, top, st, ink, funkgui::Align::right);
            }
            const std::string left = stripLeft();
            c.text(fitText(c, left, st, std::max(0.0f, strip_.w - rightW - 16.0f)).c_str(), strip_.x + 6.0f, top, st,
                   notice_.empty() ? pal.soft : pal.text);
        }

        // ---- the canvas -------------------------------------------------------------------------------------------------
        const funkgui::Rect clipR{ c.snapX(canvas_.x), c.snapY(canvas_.y), c.snapX(canvas_.right()) - c.snapX(canvas_.x),
                                   c.snapY(canvas_.bottom()) - c.snapY(canvas_.y) };
        const funkgui::Canvas::ClipScope clip(c, clipR);
        {
            const funkgui::Canvas::Scope s(c, sch::kSchematicTag, false);
            sch::grid(c, v, canvas_, k.grid);
        }
        const funkgui::Point w0 = v.world({ canvas_.x, canvas_.y }), w1 = v.world({ canvas_.right(), canvas_.bottom() });
        const cir::Box viewBox{ static_cast<int>(std::floor(w0.x)) - 4, static_cast<int>(std::floor(w0.y)) - 4,
                                static_cast<int>(std::ceil(w1.x)) + 4, static_cast<int>(std::ceil(w1.y)) + 4 };
        const std::vector<cir::UnitEdge> chosen = selectedEdges();

        // wires (a selected run with its halo), junctions
        {
            const funkgui::Canvas::Scope s(c, sch::kWireTag, false);
            if (!chosen.empty())
            {
                cir::Net halo;
                halo.edges = chosen;
                sch::wires(c, v, halo, sw + 4.0f, k.halo);
            }
            for (const cir::Net& n : d_.nets)
            {
                if (n.style != cir::Style::wired || n.unroutable || n.edges.empty())
                    continue;
                sch::wires(c, v, n, sw, k.wire);
                sch::junctions(c, v, n, k.wire);
            }
            if (!chosen.empty())
            {
                cir::Net sel;
                sel.edges = chosen;
                sch::wires(c, v, sel, sw, k.symbol);
            }
            // the wires of moved pins, rubber-banded: an L from where they end now to the pin's new place
            if (drag_ == Drag::move && !(moveBy_ == cir::Pt{}))
                for (std::size_t i = 0; i < d_.parts.size(); ++i)
                    if (moving(static_cast<int>(i)))
                        for (const cir::PinAt& pin : d_.parts[i].pins)
                        {
                            if (pin.net < 0 || d_.nets[static_cast<std::size_t>(pin.net)].style != cir::Style::wired)
                                continue;
                            const cir::Pt a = pin.at, b = pin.at + moveBy_;
                            const funkgui::Point pa = v.at(a), pc = v.at(cir::Pt{ b.x, a.y }), pb = v.at(b);
                            c.segment(pa.x, pa.y, pc.x, pc.y, sw, k.wire);
                            c.segment(pc.x, pc.y, pb.x, pb.y, sw, k.wire);
                        }
        }

        // labels on routes, flags at pins
        {
            const funkgui::Canvas::Scope s(c, sch::kPartTag, false);
            for (const cir::Net& n : d_.nets)
                for (const cir::Pt& at : n.labels)
                    sch::tag(c, v, at, n.name, k.name, px);
            for (const cir::Flag& f : d_.flags)
            {
                if (!viewBox.meets(f.box))
                    continue;
                const cir::Net& n = d_.nets[static_cast<std::size_t>(f.net)];
                const cir::Style style = n.unroutable && n.style == cir::Style::wired ? cir::Style::flag : n.style;
                if (moving(f.pin.part))
                {
                    cir::Flag g = f;
                    g.at = g.at + moveBy_;
                    sch::flag(c, v, g, style, n.positive, n.name, sw, k.symbol, k.name, px);
                }
                else
                    sch::flag(c, v, f, style, n.positive, n.name, sw, k.symbol, k.name, px);
            }
        }

        // parts: the selection's and the flash's halos, the symbols, the names and values
        {
            const funkgui::Canvas::Scope s(c, sch::kPartTag, false);
            const Hit hover = drag_ == Drag::none && tool_ == Tool::select ? hitAt(pointer_) : Hit{};
            std::vector<std::string> values;
            values.reserve(d_.parts.size());
            for (const cir::Part& p : d_.parts)
                values.push_back(sch::valueText(nl_, p));
            const std::vector<sch::LabelSpot> spots = sch::placeLabels(c, v, d_, values, px);
            for (std::size_t i = 0; i < d_.parts.size(); ++i)
            {
                const cir::Part& p = d_.parts[i];
                const cir::Pt off = moving(static_cast<int>(i)) ? moveBy_ : cir::Pt{};
                if (!viewBox.meets(p.body.moved(off)))
                    continue;
                const bool sel = std::find(st_->selected.begin(), st_->selected.end(), p.name) != st_->selected.end();
                const bool hot = (hover.kind == Hit::Kind::part || hover.kind == Hit::Kind::pin) && hover.part == static_cast<int>(i);
                const float x = static_cast<float>(p.origin.x + off.x), y = static_cast<float>(p.origin.y + off.y);
                const bool box = sch::lodBox(v, p.kind);
                const float fl = flashOf(p.name);
                if (fl > 0.0f)
                {
                    const funkgui::Col halo = funkgui::premix(k.ground, k.flash, fl);
                    box || p.element < 0 ? sch::bodyBox(c, v, p.kind, p.orient, x, y, sw + 6.0f, halo)
                                         : sch::symbol(c, v, p.kind, p.orient, x, y, sw + 6.0f, halo);
                }
                if (sel)
                    box ? sch::bodyBox(c, v, p.kind, p.orient, x, y, sw + 4.0f, k.halo)
                        : sch::symbol(c, v, p.kind, p.orient, x, y, sw + 4.0f, k.halo);
                funkgui::Col ink = hot ? k.accent : k.symbol;
                if (drag_ == Drag::move && moving(static_cast<int>(i)) && moveRefused_)
                    ink = k.error;
                if (p.element < 0)
                {
                    cir::Part q = p;
                    q.origin = q.origin + off;
                    sch::terminal(c, v, q, sw, ink, k.name, px);
                }
                else if (box)
                    sch::bodyBox(c, v, p.kind, p.orient, x, y, sw, ink);
                else
                {
                    sch::symbol(c, v, p.kind, p.orient, x, y, sw, ink);
                    if (hover.kind == Hit::Kind::pin && hover.part == static_cast<int>(i))
                    {
                        const funkgui::Point pp = v.at(p.pins[static_cast<std::size_t>(hover.pin)].at);
                        c.disc(pp.x, pp.y, std::max(2.5f, sw * 1.6f), k.accent);
                    }
                }
                if (p.element >= 0 && px > 0.0f)
                    sch::labels(c, spots[i], p.name, values[i], k.name, k.value, k.ground, px, static_cast<float>(off.x) * v.scale,
                                static_cast<float>(off.y) * v.scale);
            }
        }

        // the problems' marks and the structural ones
        {
            const funkgui::Canvas::Scope s(c, sch::kBadgeTag, false);
            for (std::size_t i = 0; i < d_.parts.size() && i < partMarks_.size(); ++i)
                if (!moving(static_cast<int>(i)))
                    sch::marks(c, v, d_.parts[i], partMarks_[i], k);
            // an unconnected pin: a small open circle; posts that touch without connecting: a dot
            const float r = std::max(2.0f, v.scale * 0.22f);
            for (const Loose& l : loose_)
            {
                if (moving(l.part) || !viewBox.contains(l.at))
                    continue;
                const funkgui::Point at = v.at(l.at);
                if (l.alone)
                    c.disc(at.x, at.y, r, k.error.withAlpha(0.0f), 1.2f, k.error);
                else
                    c.disc(at.x, at.y, r, k.error);
            }
        }

        // what a gesture shows: the ghost carried, the wire being drawn, the marquee
        {
            const funkgui::Canvas::Scope s(c, sch::kSchematicTag, false);
            if (tool_ == Tool::place && canvas_.contains(pointer_) && kind_ != cir::Kind::Unknown)
            {
                const cir::Pt g = gridAt(pointer_);
                const funkgui::Col ghost = funkgui::premix(k.ground, k.accent, 0.7f);
                if (kind_ == cir::Kind::Input || kind_ == cir::Kind::Output)
                    sch::symbol(c, v, kind_, cir::Orient::r0, static_cast<float>(g.x), static_cast<float>(g.y), sw, ghost);
                else
                    sch::symbol(c, v, kind_, orient_, static_cast<float>(g.x), static_cast<float>(g.y), sw, ghost);
            }
            if (drag_ == Drag::wire)
            {
                const std::vector<cir::Pt> path = wirePath();
                for (std::size_t i = 0; i + 1 < path.size(); ++i)
                {
                    const funkgui::Point a = v.at(path[i]), b = v.at(path[i + 1]);
                    c.segment(a.x, a.y, b.x, b.y, sw, k.accent);
                }
                const funkgui::Point a = v.at(wireFrom_);
                c.disc(a.x, a.y, std::max(2.5f, sw * 1.6f), k.accent);
            }
            if (drag_ == Drag::marquee)
            {
                const funkgui::Point a = v.at(marqueeA_.x, marqueeA_.y), b = v.at(marqueeB_.x, marqueeB_.y);
                const float x = std::min(a.x, b.x), y = std::min(a.y, b.y);
                c.rrect(x, y, std::fabs(b.x - a.x), std::fabs(b.y - a.y), 0.0f, k.halo.withAlpha(0.3f),
                        1.0f, k.halo);
            }
            // an empty file: what to do
            if (d_.parts.empty())
            {
                const funkgui::TextStyle& st = chat::kSmall;
                c.text("Place parts from the toolbar, or", canvas_.centreX(), c.capCentreTop(canvas_.centreY() - 10.0f, st), st,
                       pal.soft, funkgui::Align::centre);
                if (const std::optional<funkgui::Point> sp = centreOfStart())
                {
                    const float bw = c.textWidth("Start a circuit", st) + 16.0f;
                    const bool hot = funkgui::Rect{ sp->x - 0.5f * bw, sp->y - 11.0f, bw, 22.0f }.contains(pointer_);
                    c.rrect(sp->x - 0.5f * bw, sp->y - 11.0f, bw, 22.0f, 11.0f, hot ? pal.hover : pal.composer, 1.0f, pal.composerEdge);
                    c.text("Start a circuit", sp->x, c.capCentreTop(sp->y, st), st, hot ? pal.text : pal.soft, funkgui::Align::centre);
                }
            }
        }
    }
}
