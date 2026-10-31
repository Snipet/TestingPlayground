// Source/editor/SchematicView.cpp: see SchematicView.h.
#include "editor/SchematicView.h"

#include "editor/ChatStyle.h"
#include "editor/Items.h"
#include "editor/Theme.h"

#include "Product.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <set>
#include <span>
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
        constexpr float kPinW = 38.0f, kChipH2 = 16.0f;  // the probes' card: Pin, the chip's height
        constexpr float kUnpinW = 16.0f;                 // ... a lane's unpin
        // A gesture a write of Claude's overtook before the Panel merged it (SchematicView::setSyncHook): undone.
        constexpr const char* kOvertaken = "The circuit changed (Claude's edit came first): do it again";

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

        // Of apply()'s words for a gesture that changed the text, those the info strip says (§3.3: a refusal or a change
        // of nets, never what the user just did): two nets merged (by a wire, or by a name another net has), a net
        // split by a cut, a pin moved to a net of its own.
        bool changesNets(const std::string& said)
        {
            return said.rfind("connected ", 0) == 0 || said.rfind("disconnected ", 0) == 0
                || said.find(" split off") != std::string::npos || said.find(": the nets merged") != std::string::npos
                || said.find(": it is ground now") != std::string::npos;
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
        overtaken_ = false;
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
        put(Button::label, kButtonW);
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
        if (drag_ == Drag::knob && gestures_ != nullptr)
            gestures_->endDrag();                        // a knob turned: its gesture ends with the view
        drag_ = Drag::none;
        moveBy_ = {};
        moveRefused_ = false;
        tool_ = Tool::select;
        kind_ = cir::Kind::Unknown;
        orient_ = cir::Orient::r0;
        closeField();
        clearProbes();                                   // the probes go with the view (its pins stay the file's)
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
        // Session pins: the places drawn for parts and terminals without a line of their own, so that the next
        // reconcile (and Claude's, the picture's) keeps them where the user sees them.
        cir::SessionPins pins;
        for (const cir::Part& p : d_.parts)
        {
            if (!p.placed || p.source == cir::Source::stored)
                continue;
            if (p.element >= 0)
                pins.parts.push_back({ p.name, p.origin, p.orient, p.extra, -1 });
            else
                (p.kind == cir::Kind::Input ? pins.inputs : pins.outputs).push_back({ p.name, p.origin, -1 });
        }
        st_->pins = std::move(pins);
        publishPins();
        // What a part is (its place, its turn, its line's tokens: its nodes and its value): a merge flashes those it
        // changed (Claude's edit_circuit moving, connecting or setting a part).
        shapes_.clear();
        for (const cir::Part& p : d_.parts)
        {
            std::string tokens;
            if (p.element >= 0)
                for (const cir::Token& t : nl_.elements[static_cast<std::size_t>(p.element)].tokens)
                    tokens += t.text + ' ';
            else if (const std::optional<cir::Terminal>& t = p.kind == cir::Kind::Input ? nl_.input : nl_.output; t)
                for (const cir::Token& x : t->tokens)
                    tokens += x.text + ' ';
            shapes_[cir::folded(p.name)] = std::to_string(p.origin.x) + "," + std::to_string(p.origin.y) + " "
                                         + cir::orientName(p.orient) + " " + tokens;
        }
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
                // one dot a point, however many pins touch there
                if (touches && std::none_of(loose_.begin(), loose_.end(), [&](const Loose& l) { return !l.alone && l.at == pin.at; }))
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
        marks_ = sch::mapMarks(nl_, d_, hooks_.problems ? hooks_.problems() : std::vector<Diagnostic>{});
        partMarks_ = sch::partMarksOf(d_, marks_);
    }

    int SchematicView::problemCount() const { return static_cast<int>(marks_.size()); }

    // ---- hit-testing --------------------------------------------------------------------------------------------------
    SchematicView::Hit SchematicView::hitAt(funkgui::Point p) const
    {
        Hit h;
        if (!showing() || !canvas_.contains(p))
            return h;
        if (const std::optional<funkgui::Point> s = centreOfStart())
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
        noticeRev_ = ed_ != nullptr ? ed_->revision() : 0;
    }

    void SchematicView::quiet()
    {
        notice_.clear();
        noticeLeft_ = 0.0f;
    }

    std::string SchematicView::notice() const
    {
        // a notice is its revision's: an undo, a redo, typing or a merge since makes it stale
        return ed_ != nullptr && ed_->revision() == noticeRev_ ? notice_ : std::string();
    }

    void SchematicView::publishPins() const
    {
        if (!showing() || !pins_)
            return;
        std::string lines;
        for (const cir::PartLine& p : st_->pins.parts)
        {
            lines += "*@ part " + p.name + " " + std::to_string(p.at.x) + "," + std::to_string(p.at.y);
            if (p.orient != cir::Orient::r0)
                lines += std::string(" ") + cir::orientName(p.orient);
            for (const std::string& x : p.extra)
                lines += " " + x;
            lines += "\n";
        }
        for (const auto* terminals : { &st_->pins.inputs, &st_->pins.outputs })
            for (const cir::TerminalLine& t : *terminals)
                lines += std::string(terminals == &st_->pins.inputs ? "*@ input " : "*@ output ") + t.name + " " + std::to_string(t.at.x)
                       + "," + std::to_string(t.at.y) + "\n";
        if (lines == st_->published)
            return;
        st_->published = lines;
        pins_(file_, lines);
    }

    bool SchematicView::commit(const std::vector<cir::Op>& ops, std::string* why, std::vector<std::string>* touched)
    {
        if (!showing())
            return false;
        ensure();
        const auto refuse = [&](const std::string& text) {
            if (why != nullptr)
                *why = text;
            else
                say(text);
            return false;
        };
        if (d_.readOnly)
            return refuse("Drawn by a newer playground: read only here");
        const cir::Applied a = cir::apply(text_, d_, ops, st_->pins);
        if (!a.error.empty())
            return refuse(friendly(a.error));
        if (touched != nullptr)
            *touched = a.touched;
        // What the strip says: every op's words when nothing changed (why not), else the changes of nets alone, in
        // order (the strip cuts what does not fit).
        std::string words;
        for (const std::string& line : a.said)
            if (a.edits.empty() || changesNets(line))
                words += (words.empty() ? "" : "; ") + line;
        if (a.edits.empty())
        {
            words.empty() ? quiet() : say(words);
            return true;
        }
        std::vector<funkgui::TextEdit> edits;
        edits.reserve(a.edits.size());
        for (const cir::Edit& e : a.edits)
            edits.push_back({ { e.from.line, e.from.col }, { e.to.line, e.to.col }, e.text });
        if (!ed_->applyEdits(std::move(edits)))
            return refuse("The edit could not be made (its parts overlapped): nothing changed");
        if (!synced())
            return refuse(kOvertaken);
        ensure();
        words.empty() ? quiet() : say(words);            // the notice of this revision
        return true;
    }

    bool SchematicView::synced()
    {
        if (!sync_ || sync_())
            return true;
        ed_->undo();                                     // a write of Claude's came first: it is merged at the next frame
        ensure();
        overtakenRev_ = ed_->revision();                 // said again once that merge moves the text (tick)
        overtaken_ = true;
        return false;
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
        if (!synced())
        {
            say(kOvertaken);
            return false;
        }
        ensure();
        quiet();                                         // a new gesture: an earlier notice goes
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
        if (!showing())
            return false;
        if (!emptyNetlist())
        {
            say("Start a circuit is for an empty netlist");
            return false;
        }
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
        std::string text = text_, words;
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
            for (const std::string& line : a.said)
                if (changesNets(line))
                    words += (words.empty() ? "" : "; ") + line;
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
        if (!commitText(text))
            return false;
        setSelection({}, {});
        if (!words.empty())
            say(words);                                  // a net the cut split: the names its parts are on now
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

    // ---- the field: a part's names and values edited in place, a net's name (the Label tool) ----------------------------
    namespace
    {
        constexpr int kFieldMax = 256;                   // a row's characters (a long {expr} fits)
        constexpr float kRowH = 20.0f, kLabelW = 64.0f, kCardPad = 6.0f;

        std::string trimmed(std::string t)
        {
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
                t.pop_back();
            std::size_t a = 0;
            while (a < t.size() && (t[a] == ' ' || t[a] == '\t'))
                ++a;
            return t.substr(a);
        }

        // The first pot a pot's {expr} names (its .pot line's), "" for none.
        std::string firstPot(const cir::Netlist& nl, const cir::Element& e)
        {
            for (const cir::Token& t : e.tokens)
            {
                if (t.text.empty() || t.text.front() != '{')
                    continue;
                std::string word;
                for (std::size_t c = 0; c <= t.text.size(); ++c)
                {
                    const char ch = c < t.text.size() ? t.text[c] : ' ';
                    if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_')
                    {
                        word += ch;
                        continue;
                    }
                    if (!word.empty() && std::isalpha(static_cast<unsigned char>(word.front())) != 0)
                        for (const cir::PotLine& p : nl.pots)
                            if (cir::folded(p.name) == cir::folded(word))
                                return p.name;
                    word.clear();
                }
            }
            return {};
        }

        bool ohms(cir::Kind k) noexcept { return k == cir::Kind::R || k == cir::Kind::Pot || k == cir::Kind::Rheostat; }
    }

    std::vector<std::string> SchematicView::fieldKeys() const
    {
        std::vector<std::string> out;
        for (const Row& r : field_.rows)
            out.push_back(r.key);
        return out;
    }

    std::string SchematicView::fieldText(const std::string& key) const
    {
        for (const Row& r : field_.rows)
            if (r.key == key)
                return r.edit.buffer;
        return {};
    }

    std::string SchematicView::fieldRow() const
    {
        return field_.on && field_.row >= 0 && field_.row < static_cast<int>(field_.rows.size())
                   ? field_.rows[static_cast<std::size_t>(field_.row)].key
                   : std::string();
    }

    std::string SchematicView::fieldWarning() const
    {
        if (!field_.on || !ohms(field_.kind))
            return {};
        return cir::milliWarning(trimmed(fieldText("value")));
    }

    bool SchematicView::editPart(const std::string& name, const std::string& rowKey)
    {
        ensure();
        if (!showing())
            return false;
        if (d_.readOnly)
        {
            say("Drawn by a newer playground: read only here");
            return false;
        }
        const int i = d_.part(name);
        if (i < 0)
            return false;
        const cir::Part& p = d_.parts[static_cast<std::size_t>(i)];
        Field f;
        f.on = true;
        f.part = p.name;
        f.kind = p.kind;
        f.rev = ed_->revision();
        const auto row = [&](const char* key, const char* label, const std::string& text) {
            Row r;
            r.key = key;
            r.label = label;
            r.edit.set(text, kFieldMax);
            r.was = r.edit.buffer;                       // as the row holds it (cut, made printable): untouched is unchanged
            f.rows.push_back(std::move(r));
        };
        row("name", "NAME", p.name);
        if (p.element >= 0)
        {
            const cir::Element& e = nl_.elements[static_cast<std::size_t>(p.element)];
            const auto tok = [&](int k) { return k >= 0 && k < static_cast<int>(e.tokens.size()) ? e.tokens[static_cast<std::size_t>(k)].text : std::string(); };
            switch (e.kind)
            {
                case cir::Kind::R:
                case cir::Kind::C:
                case cir::Kind::L:
                case cir::Kind::V:
                case cir::Kind::I:
                    row("value", "VALUE", tok(e.valueToken));
                    break;
                case cir::Kind::D:
                    row("model", "MODEL", tok(e.valueToken));
                    break;
                case cir::Kind::Pot:
                case cir::Kind::Rheostat:
                {
                    row("value", "VALUE", tok(e.valueToken));
                    if (const std::string pot = firstPot(nl_, e); !pot.empty())
                    {
                        row("pot", "POT", pot);
                        row("taper", "TAPER", cir::taperOf(nl_, e));   // the one FMinus plays it with
                    }
                    break;
                }
                case cir::Kind::Vcvs:
                case cir::Kind::Vccs:
                case cir::Kind::Ota:
                    row("gain", "GAIN", tok(e.valueToken));
                    break;
                case cir::Kind::S:
                {
                    row("switch", "SWITCH", tok(3));
                    std::string positions;
                    for (std::size_t k = 4; k < e.tokens.size() && e.tokens[k].text.find('=') == std::string::npos; ++k)
                        positions += (positions.empty() ? "" : " ") + e.tokens[k].text;
                    row("positions", "POSITIONS", positions);
                    break;
                }
                case cir::Kind::OpAmp:
                case cir::Kind::Input:
                case cir::Kind::Output:
                case cir::Kind::Unknown:
                    break;
            }
        }
        else
        {
            // a terminal: its scale (none written: 1)
            const std::optional<cir::Terminal>& t = p.kind == cir::Kind::Input ? nl_.input : nl_.output;
            std::string scale;
            if (t)
                for (const cir::Token& x : t->tokens)
                    if (cir::folded(x.text).rfind("scale=", 0) == 0)
                        scale = x.text.substr(6);
            row("scale", "SCALE", scale);
        }
        f.row = 0;
        for (std::size_t k = 0; k < f.rows.size(); ++k)
            if (!rowKey.empty() && (f.rows[k].key == rowKey || (rowKey == "value" && k == 1)))
            {
                f.row = static_cast<int>(k);
                break;
            }
        f.world = { 0.5f * static_cast<float>(p.body.x0 + p.body.x1), static_cast<float>(p.body.y1) };
        if (drag_ == Drag::carry || tool_ != Tool::select)
            setTool(Tool::select);
        field_ = std::move(f);
        setSelection({ p.name }, {});
        return true;
    }

    bool SchematicView::editNetAt(funkgui::Point p)
    {
        ensure();
        if (!showing())
            return false;
        if (d_.readOnly)
        {
            say("Drawn by a newer playground: read only here");
            return false;
        }
        const Hit h = hitAt(p);
        if ((h.kind != Hit::Kind::pin && h.kind != Hit::Kind::wire) || h.net < 0)
            return false;
        const cir::Net& n = d_.nets[static_cast<std::size_t>(h.net)];
        if (n.style == cir::Style::ground)
        {
            say("Ground is node 0: it keeps its name");
            return false;
        }
        Field f;
        f.on = true;
        f.net = n.name;
        f.rev = ed_->revision();
        Row r;
        r.key = "net";
        r.label = "NET";
        r.edit.set(n.name, kFieldMax);
        r.was = r.edit.buffer;
        f.rows.push_back(std::move(r));
        if (h.kind == Hit::Kind::wire && n.style == cir::Style::wired && !n.unroutable)
            f.tag = h.at;                                // its name tag goes where the wire was clicked
        f.world = view().world(p);
        field_ = std::move(f);
        return true;
    }

    void SchematicView::closeField()
    {
        field_ = Field{};
    }

    bool SchematicView::commitField(bool megohms)
    {
        if (!field_.on)
            return true;
        ensure();
        if (ed_->revision() != field_.rev)
        {
            closeField();
            say("The circuit changed: edit it again");
            return false;
        }
        std::vector<cir::Op> ops;
        std::string to;
        for (const Row& r : field_.rows)
        {
            std::string text = trimmed(r.edit.buffer);
            if (megohms && r.key == "value" && ohms(field_.kind))
            {
                std::string meg;
                if (!cir::milliWarning(text, &meg).empty())
                    text = meg;                          // Alt-Return: the megohms FMinus's warning says to write
            }
            if (text == r.was)
                continue;
            if (text.empty())
            {
                field_.why = r.label + " is empty: write one, or Escape";
                return false;
            }
            if (r.key == "name" || r.key == "net")
            {
                to = text;
                continue;
            }
            cir::Op op;
            op.type = cir::Op::Type::set;
            op.name = field_.part;
            op.key = r.key == "value" ? std::string() : r.key;
            op.value = text;
            ops.push_back(std::move(op));
        }
        if (!field_.net.empty() && field_.tag)
        {
            // the Label tool: its name tag where the wire was clicked (label= on the net's line)
            const int n = d_.net(field_.net);
            cir::Op op;
            op.type = cir::Op::Type::label;
            op.name = field_.net;
            if (n >= 0)
                op.points = d_.nets[static_cast<std::size_t>(n)].labels;
            if (std::find(op.points.begin(), op.points.end(), *field_.tag) == op.points.end())
            {
                op.points.push_back(*field_.tag);
                ops.push_back(std::move(op));
            }
        }
        if (!to.empty())
        {
            cir::Op op;
            op.type = cir::Op::Type::rename;
            op.name = field_.part.empty() ? field_.net : field_.part;
            op.to = to;
            ops.push_back(std::move(op));             // last: the sets name the part as it was
        }
        if (ops.empty())
        {
            closeField();
            return true;
        }
        std::string why;
        if (!commit(ops, &why))
        {
            field_.why = why;                            // refused in the field: it stays open
            return false;
        }
        const std::string part = field_.part.empty() ? std::string() : (to.empty() ? field_.part : to);
        closeField();
        if (!part.empty())
            if (const int i = d_.part(part); i >= 0)
                setSelection({ d_.parts[static_cast<std::size_t>(i)].name }, {});
        return true;
    }

    void SchematicView::stepField(int dir, bool decade)
    {
        Row& r = field_.rows[static_cast<std::size_t>(field_.row)];
        if (r.key != "value" && r.key != "gain")
            return;
        if (const std::optional<std::string> v = cir::e12Stepped(trimmed(r.edit.buffer), dir, decade))
        {
            r.edit.set(*v, kFieldMax);
            r.edit.allSelected = false;
            field_.why.clear();
        }
    }

    bool SchematicView::fieldKey(const funkgui::KeyEvent& e)
    {
        if (field_.rows.empty())
        {
            closeField();
            return true;
        }
        field_.row = std::clamp(field_.row, 0, static_cast<int>(field_.rows.size()) - 1);
        Row& r = field_.rows[static_cast<std::size_t>(field_.row)];
        if (e.key == funkgui::Key::enter)
        {
            commitField(e.mods.alt);
            return true;
        }
        if (e.key == funkgui::Key::escape)
        {
            closeField();
            return true;
        }
        if (e.key == funkgui::Key::tab)
        {
            const int n = static_cast<int>(field_.rows.size());
            field_.row = ((field_.row + (e.mods.shift ? -1 : 1)) % n + n) % n;
            Row& next = field_.rows[static_cast<std::size_t>(field_.row)];
            next.edit.caret = static_cast<int>(next.edit.buffer.size());
            next.edit.allSelected = !next.edit.buffer.empty();
            return true;
        }
        if (e.key == funkgui::Key::up || e.key == funkgui::Key::down)
        {
            stepField(e.key == funkgui::Key::up ? 1 : -1, e.mods.shift);
            return true;
        }
        if (e.key == funkgui::Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host_ != nullptr && r.edit.paste(host_->clipboardText(), kFieldMax))
                field_.why.clear();
            return true;
        }
        if (e.key == funkgui::Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
        {
            r.edit.allSelected = !r.edit.buffer.empty();
            return true;
        }
        if (r.edit.key(e, kFieldMax))
            field_.why.clear();
        return true;                                     // the field takes every key
    }

    funkgui::Rect SchematicView::fieldRect() const
    {
        if (!field_.on || !showing())
            return {};
        const auto& atlas = funkgui::FontService::get().atlas();
        const funkgui::TextStyle& st = chat::kSmall;
        float textW = 120.0f;
        for (const Row& r : field_.rows)
            textW = std::max(textW, funkgui::text::width(atlas, r.edit.buffer.c_str(), st) + 16.0f);
        // never wider than the canvas (a long expression is cut on the row, as it is drawn)
        textW = std::min(textW, std::max(120.0f, canvas_.w - kLabelW - 2.0f * kCardPad - 4.0f));
        float w = kLabelW + textW + 2.0f * kCardPad;
        float h = kRowH * static_cast<float>(field_.rows.size()) + 2.0f * kCardPad;
        for (const std::string& line : { fieldWarning(), fieldWarning().empty() ? std::string() : std::string("Return keeps it as typed   Alt-Return writes megohms"),
                                         field_.why })
            if (!line.empty())
            {
                w = std::max(w, funkgui::text::width(atlas, line.c_str(), st) + 2.0f * kCardPad + 4.0f);
                h += 16.0f;
            }
        // under the part (a net's: under the point), kept on the canvas
        const sch::View v = view();
        const funkgui::Point at = v.at(field_.world.x, field_.world.y);
        float x = at.x - 0.5f * w, y = at.y + 8.0f;
        if (y + h > canvas_.bottom() - 2.0f)
            y = at.y - 8.0f - h - (field_.part.empty() ? 0.0f : 3.0f * v.scale);
        x = std::max(canvas_.x + 2.0f, std::min(x, canvas_.right() - w - 2.0f));
        y = std::max(canvas_.y + 2.0f, std::min(y, canvas_.bottom() - h - 2.0f));
        return { x, y, w, h };
    }

    void SchematicView::drawField(funkgui::Canvas& c, const funkgui::Theme& th, const sch::View& v, const sch::Inks& k) const
    {
        (void)v;
        if (!field_.on)
            return;
        const chat::Palette pal = chat::palette(th);
        const funkgui::TextStyle& st = chat::kSmall;
        const funkgui::Rect card = fieldRect();
        c.rrect(card.x, card.y, card.w, card.h, 8.0f, pal.card, 1.0f, pal.cardEdge);
        float y = card.y + kCardPad;
        for (std::size_t i = 0; i < field_.rows.size(); ++i, y += kRowH)
        {
            const Row& r = field_.rows[i];
            const bool active = static_cast<int>(i) == field_.row;
            const float top = c.capCentreTop(y + 0.5f * kRowH, st);
            c.text(r.label.c_str(), card.x + kCardPad + 2.0f, top, st, pal.faint);
            const funkgui::Rect box{ card.x + kCardPad + kLabelW, y + 1.0f, card.w - 2.0f * kCardPad - kLabelW, kRowH - 2.0f };
            c.rrect(box.x, box.y, box.w, box.h, 5.0f, pal.composer, 1.0f, active ? pal.accent : pal.composerEdge);
            // the text, the selection (all of it, until a key), the caret: as a tab's name being edited
            const std::string& b = r.edit.buffer;
            const float tw = c.textWidth(b.c_str(), st);
            if (active && r.edit.allSelected && !b.empty())
                c.rrect(box.x + 5.0f, box.y + 3.0f, std::min(tw, box.w - 10.0f) + 2.0f, box.h - 6.0f, 2.0f, th.accentDim);
            const bool bad = active && !field_.why.empty();
            c.text(fitText(c, b, st, box.w - 12.0f).c_str(), box.x + 6.0f, top, st, bad ? pal.danger : pal.text);
            if (active)
            {
                const std::string head = b.substr(0, static_cast<std::size_t>(std::clamp(r.edit.caret, 0, static_cast<int>(b.size()))));
                const float cx = box.x + 6.0f + std::min(c.textWidth(head.c_str(), st), box.w - 12.0f);
                c.rrect(cx, box.y + 3.0f, 1.5f, box.h - 6.0f, 0.0f, pal.accent);
            }
        }
        y += 2.0f;
        const std::string warning = fieldWarning();
        if (!warning.empty())
        {
            c.text(warning.c_str(), card.x + kCardPad + 2.0f, y, st, k.warning);
            y += 16.0f;
            c.text("Return keeps it as typed   Alt-Return writes megohms", card.x + kCardPad + 2.0f, y, st, pal.soft);
            y += 16.0f;
        }
        if (!field_.why.empty())
            c.text(field_.why.c_str(), card.x + kCardPad + 2.0f, y, st, pal.danger);
    }

    std::optional<funkgui::Point> SchematicView::centreOfValue(const std::string& part) const
    {
        ensure();
        const int i = d_.part(part);
        if (i < 0 || i >= static_cast<int>(valueRects_.size()) || valueRects_[static_cast<std::size_t>(i)].w <= 0.0f)
            return std::nullopt;
        const funkgui::Rect r = valueRects_[static_cast<std::size_t>(i)];
        return funkgui::Point{ r.centreX(), r.centreY() };
    }

    // ---- the clipboard, labels and wires -------------------------------------------------------------------------------
    bool SchematicView::copySelection()
    {
        ensure();
        if (!showing() || st_ == nullptr)
            return false;
        const std::string text = cir::copied(text_, d_, st_->selected);
        if (text.empty())
        {
            say("Copy takes parts: select one or more (the input and the output stay)");
            return false;
        }
        if (host_ != nullptr)
            host_->copyText(text);
        return true;
    }

    bool SchematicView::cutSelection()
    {
        if (!copySelection())
            return false;
        // the parts copied go (the input and the output, which Copy leaves out, stay)
        std::vector<std::string> parts;
        for (const std::string& n : st_->selected)
            if (const int i = d_.part(n); i >= 0 && d_.parts[static_cast<std::size_t>(i)].element >= 0)
                parts.push_back(n);
        st_->selected = parts;
        return deleteSelection();
    }

    namespace
    {
        // A paste op of clipboard text at a point (its lines, a CR before each LF left out).
        cir::Op pasteOp(const std::string& text, cir::Pt at)
        {
            cir::Op op;
            op.type = cir::Op::Type::paste;
            op.at = at;
            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t end = std::min(text.find('\n', start), text.size());
                std::string line = text.substr(start, end - start);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                op.lines.push_back(std::move(line));
                start = end + 1;
            }
            return op;
        }
    }

    bool SchematicView::pasteText(const std::string& text, cir::Pt at)
    {
        const cir::Op op = pasteOp(text, at);
        std::string why;
        std::vector<std::string> touched;
        if (!commit({ op }, &why, &touched))
        {
            say(why.rfind("nothing to paste", 0) == 0 || why.rfind("paste takes", 0) == 0
                    ? "Paste takes netlist lines (a copy of parts): " + why
                    : why);
            return false;
        }
        std::vector<std::string> parts;
        for (const std::string& n : touched)
            if (const int i = d_.part(n); i >= 0)
                parts.push_back(d_.parts[static_cast<std::size_t>(i)].name);
        setSelection(std::move(parts), {});
        return true;
    }

    bool SchematicView::paste()
    {
        ensure();
        if (!showing())
            return false;
        const std::string text = host_ != nullptr ? host_->clipboardText() : std::string();
        if (text.find_first_not_of(" \t\r\n") == std::string::npos)
        {
            say("Nothing to paste: copy parts first");
            return false;
        }
        const cir::Pt at = canvas_.contains(pointer_) ? gridAt(pointer_) : gridAt({ canvas_.centreX(), canvas_.centreY() });
        return pasteText(text, at);
    }

    bool SchematicView::duplicate()
    {
        ensure();
        if (!showing() || st_ == nullptr)
            return false;
        cir::Pt origin;
        const std::string text = cir::copied(text_, d_, st_->selected, &origin);
        if (text.empty())
        {
            say("Duplicate takes parts: select one or more");
            return false;
        }
        // The copy goes just below the selection's box (else right of it, then further), at the first place where it
        // meets nothing: a paste onto parts is refused, as a drop is.
        cir::Box box;
        for (const std::string& n : st_->selected)
            if (const int i = d_.part(n); i >= 0 && d_.parts[static_cast<std::size_t>(i)].element >= 0)
                box = box.joined(d_.parts[static_cast<std::size_t>(i)].body);
        const int w = box.x1 - box.x0 + 2, h = box.y1 - box.y0 + 2;
        for (int k = 1; k <= 4; ++k)
            for (const cir::Pt off : { cir::Pt{ 0, k * h }, cir::Pt{ k * w, 0 } })
            {
                const cir::Op op = pasteOp(text, origin + off);
                if (cir::apply(text_, d_, std::span<const cir::Op>(&op, 1), st_->pins).error.empty())
                    return pasteText(text, origin + off);
            }
        say("No room for a copy beside the selection: copy it and paste it where there is");
        return false;
    }

    bool SchematicView::showAsLabels(const std::string& net, bool labels)
    {
        cir::Op op;
        op.type = cir::Op::Type::style;
        op.name = net;
        op.flag = labels;
        return commit({ op });
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
        if (field_.on)
        {
            // In the field: a row's press puts the caret there. Elsewhere: what was typed, when it can be written (else
            // nothing), and the press goes on.
            const funkgui::Rect card = fieldRect();
            if (card.contains(p))
            {
                const int r = static_cast<int>((p.y - card.y - kCardPad) / kRowH);
                if (r >= 0 && r < static_cast<int>(field_.rows.size()))
                {
                    field_.row = r;
                    Row& row = field_.rows[static_cast<std::size_t>(r)];
                    row.edit.allSelected = false;
                    row.edit.caret = static_cast<int>(row.edit.buffer.size());
                }
                drag_ = Drag::none;
                return;
            }
            if (!commitField())
                closeField();
            ensure();
        }
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
                    case Button::label: setTool(tool_ == Tool::label ? Tool::select : Tool::label); break;
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
            if (!e.popup && countRect().contains(p))
                stepProblem(e.mods.shift ? -1 : 1);
            else if (!e.popup && stripButtonRect().contains(p))
            {
                // Play this circuit (its why, when it cannot), or Undo what it wrote
                if (stripButton() == "Undo")
                {
                    if (dials_.undoPlay)
                        dials_.undoPlay();
                }
                else if (dials_.play)
                    if (const std::string why = dials_.play(); !why.empty())
                        say(why);
            }
            return;
        }
        if (!canvas_.contains(p))
            return;
        // The probes' card and lanes (drawn over the canvas): Pin, the chip's menu, a lane's unpin; a press anywhere
        // else on them selects nothing under them.
        if (const funkgui::Rect card = probeCard(); card.contains(p))
        {
            drag_ = Drag::none;
            if (e.popup)
                return;
            if (const std::optional<funkgui::Point> pin = centreOfPin();
                pin && funkgui::Rect{ pin->x - 0.5f * kPinW, pin->y - 0.5f * kChipH2, kPinW, kChipH2 }.contains(p))
                pinProbe(slots_[0].net);
            else if (const std::optional<funkgui::Point> chip = centreOfChip();
                     chip && funkgui::Rect{ chip->x - 32.0f, chip->y - 0.5f * kChipH2, 64.0f, kChipH2 }.contains(p))
                openChipMenu();
            return;
        }
        if (const int lane = laneAt(p); lane >= 0)
        {
            drag_ = Drag::none;
            const funkgui::Rect r = probeLane(lane);
            if (!e.popup && p.x >= r.right() - 6.0f - kUnpinW)
                unpinProbe(pinnedProbes()[static_cast<std::size_t>(lane)]);
            else if (!e.popup)
                if (const int n = d_.net(pinnedProbes()[static_cast<std::size_t>(lane)]); n >= 0)
                    setSelection({}, runsOf(n));        // a lane's click selects its net (the card shows it)
            return;
        }
        // A circuit dial's knob turns its dial; a switch's chip steps it (the Panel's gestures, the dial's own port).
        if (!e.popup && tool_ == Tool::select && gestures_ != nullptr && dials_.port && dials_.control)
            if (const int k = knobAt(p); k >= 0)
            {
                const Knob kn = knobs()[static_cast<std::size_t>(k)];
                if (funkgui::ParamPort* port = dials_.port(kn.slot))
                {
                    if (kn.chip)
                    {
                        const Control c = dials_.control(kn.slot);
                        const long at = std::lround(plainOf(c, port->value01()) - c.min);
                        gestures_->tap(*port, normOf(c, c.min + static_cast<float>((at + 1) % std::max(kn.positions, 1))));
                        drag_ = Drag::control;
                    }
                    else
                    {
                        gestures_->beginDrag(*port);
                        knobFrom_ = port->value01();
                        knobY_ = p.y;
                        drag_ = Drag::knob;
                    }
                    knobPart_ = kn.part;
                    return;
                }
            }
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
            case Drag::knob:
            case Drag::control:
                break;
        }
    }

    void SchematicView::pointerDrag(const funkgui::PointerEvent& e)
    {
        if (!showing())
            return;
        const funkgui::Point p{ e.x, e.y };
        pointer_ = p;
        if (drag_ == Drag::knob)
        {
            // up raises it: 150 px a whole turn (Shift: 750 px, fine)
            if (gestures_ != nullptr && gestures_->dragging())
                gestures_->dragTo(knobFrom_ + (knobY_ - p.y) / (e.mods.shift ? 750.0f : 150.0f));
            return;
        }
        if (drag_ == Drag::press)
        {
            if (dist(p, press_.at) >= kDragPx)
                beginDrag(p);
            return;
        }
        if (drag_ != Drag::none && drag_ != Drag::control)
            followPointer(p);
    }

    void SchematicView::pointerUp(const funkgui::PointerEvent& e)
    {
        if (!showing())
            return;
        const funkgui::Point p{ e.x, e.y };
        pointer_ = p;
        if (drag_ == Drag::knob || drag_ == Drag::control)
        {
            if (drag_ == Drag::knob && gestures_ != nullptr)
                gestures_->endDrag();
            drag_ = Drag::none;
            return;
        }
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
            case Drag::knob:
            case Drag::control:
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
            else if (tool_ == Tool::ground || tool_ == Tool::label)
                setTool(Tool::select);
            else
                openMenu(press_.at);
            return;
        }
        if (press_.mods.alt)
        {
            // on a wire (or a pin): its net pinned to a probe's slot, or unpinned; ground is 0 V
            if ((h.kind == Hit::Kind::wire || h.kind == Hit::Kind::pin) && h.net >= 0)
            {
                if (d_.nets[static_cast<std::size_t>(h.net)].key == "0")
                {
                    say("Probe: ground is 0 V");
                    if (host_ != nullptr)
                        host_->announce("Probe: ground is 0 V");
                }
                else
                    togglePin(d_.nets[static_cast<std::size_t>(h.net)].name);
            }
            return;
        }
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
        if (tool_ == Tool::label)
        {
            editNetAt(press_.at);                        // its net's name field (the tool stays)
            return;
        }
        // a part's value label: its field, the caret on the value
        if (!press_.mods.shift && (h.kind == Hit::Kind::none || h.kind == Hit::Kind::wire))
            for (std::size_t i = 0; i < valueRects_.size() && i < d_.parts.size(); ++i)
                if (valueRects_[i].contains(press_.at))
                {
                    editPart(d_.parts[i].name, "value");
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
        if (field_.on && fieldRect().contains({ e.x, e.y }))
            return;                                      // in the field: its row, nothing of what lies under it
        // The second press began a knob's drag (FunkGui sends down, up, down, doubleClick, up): its gesture ends here,
        // so the host's change gesture is closed and the default below is a tap of its own.
        if (drag_ == Drag::knob && gestures_ != nullptr)
            gestures_->endDrag();
        drag_ = Drag::none;
        // A knob's double-click: its dial's default (a chip's second click stepped it already).
        if (tool_ == Tool::select)
            if (const int k = knobAt({ e.x, e.y }); k >= 0)
            {
                const Knob kn = knobs()[static_cast<std::size_t>(k)];
                funkgui::ParamPort* port = dials_.port ? dials_.port(kn.slot) : nullptr;
                if (!kn.chip && port != nullptr && gestures_ != nullptr && dials_.control)
                {
                    const Control c = dials_.control(kn.slot);
                    gestures_->tap(*port, normOf(c, c.def));
                }
                return;
            }
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
                    editPart(d_.parts[static_cast<std::size_t>(h.part)].name);   // edited in place
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
        // over the probes' lanes: they scroll (when they do not all fit)
        if (laneAt({ e.x, e.y }) >= 0)
        {
            const funkgui::Rect card = probeCard();
            const float bottom = card.h > 0.0f ? card.y - 4.0f : canvas_.bottom() - 6.0f;
            const float most = std::max(0.0f, static_cast<float>(std::min<std::size_t>(pinnedProbes().size(), 3)) * kLaneH
                                                  - (bottom - lanesTop()));
            laneScroll_ = std::clamp(laneScroll_ - (e.reversed ? -e.dy : e.dy) * (e.smooth ? 512.0f : kLaneH), 0.0f, most);
            return true;
        }
        if (probeCard().contains({ e.x, e.y }))
            return true;
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
        if (field_.on)
            return fieldKey(e);                          // the field takes every key (Escape closes it)
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
                    editPart(st_->selected.front());     // edited in place
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
                case 'c': copySelection(); return true;
                case 'x': cutSelection(); return true;
                case 'v': paste(); return true;
                case 'd':
                    if (shift)
                        return false;                    // Cmd-Shift-D is the Panel's (the views)
                    duplicate();
                    return true;
                case 'r': rotate(!shift); return true;
                case 'e': mirror(); return true;
                case '0': zoomTo(1.0f, { canvas_.centreX(), canvas_.centreY() }); persist(); return true;
                case '=':
                case '+': zoomTo(zoom() * kStepZoom, { canvas_.centreX(), canvas_.centreY() }); persist(); return true;
                case '-':
                case '_': zoomTo(zoom() / kStepZoom, { canvas_.centreX(), canvas_.centreY() }); persist(); return true;
                default: return false;                   // the rest are the host's
            }
        }
        cir::Kind kind = cir::Kind::Unknown;
        switch (ch)
        {
            case 'f': fit(false); persist(); return true;
            case 'a': openMenu(centreOfButton(Button::more)); return true;
            case 'n':
                setTool(tool_ == Tool::label ? Tool::select : Tool::label);
                return true;
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
            const bool terminal = d_.parts[static_cast<std::size_t>(h.part)].element < 0;
            request.items = { { 6, "Edit   Return" },          { 0, {}, true, false, true },
                              { 1, "Rotate   Cmd-R" },         { 2, "Rotate back   Cmd-Shift-R" }, { 3, "Mirror   Cmd-E" },
                              { 0, {}, true, false, true },    { 7, "Copy   Cmd-C", !terminal },  { 8, "Cut   Cmd-X", !terminal },
                              { 9, "Duplicate   Cmd-D", !terminal }, { 4, "Delete   Delete" }, { 0, {}, true, false, true },
                              { 5, "Show in netlist" } };
            // on a pin of a net drawn as name tags: its wires again (a tag has no wire to open the wire's menu on)
            std::string tagged;
            if (h.kind == Hit::Kind::pin && h.net >= 0)
            {
                const cir::Net& n = d_.nets[static_cast<std::size_t>(h.net)];
                if (n.style == cir::Style::flag)
                {
                    tagged = n.name;
                    request.items.push_back({ 10, "Show " + n.name + " as wires" });
                }
            }
            host_->showMenu(request, [this, name, tagged](int chosen) {
                if (!showing())
                    return;
                switch (chosen)
                {
                    case 1: rotate(true); break;
                    case 2: rotate(false); break;
                    case 3: mirror(); break;
                    case 4: deleteSelection(); break;
                    case 5: openNetlistAtPart(d_.part(name)); break;
                    case 6: editPart(name); break;
                    case 7: copySelection(); break;
                    case 8: cutSelection(); break;
                    case 9: duplicate(); break;
                    case 10: showAsLabels(tagged, false); break;
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
            const bool parts = !st_->selected.empty();
            const bool ground = d_.nets[static_cast<std::size_t>(h.net)].key == "0";
            const bool pinned = std::any_of(st_->probes.begin(), st_->probes.end(), [&](const std::string& x) { return cir::folded(x) == cir::folded(net); });
            request.items = { { 14, "Rename net   N" },       { 15, "Show as labels" },          { 0, {}, true, false, true },
                              { 11, "Delete wire   Delete" }, { 12, "Select net" },              { 16, "Copy   Cmd-C", parts },
                              { 17, "Cut   Cmd-X", parts },   { 0, {}, true, false, true },      { 18, "Probe this net" },
                              { 19, pinned ? "Unpin probe   Alt-click" : "Pin probe   Alt-click", !ground },
                              { 0, {}, true, false, true },   { 13, "Show in netlist" } };
            host_->showMenu(request, [this, net, at, r](int chosen) {
                if (!showing())
                    return;
                ensure();
                const int n = d_.net(net);
                if (chosen == 18 && n >= 0)
                    setSelection({}, { r });                     // probe slot 0 follows the run selected
                else if (chosen == 19)
                    togglePin(net);
                else if (chosen == 11)
                    deleteSelection();
                else if (chosen == 14)
                    editNetAt(at);
                else if (chosen == 15 && n >= 0)
                    showAsLabels(net, true);
                else if (chosen == 16)
                    copySelection();
                else if (chosen == 17)
                    cutSelection();
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
        const bool clip = host_->clipboardText().find_first_not_of(" \t\r\n") != std::string::npos;
        const cir::Pt pasteAt = gridAt(at);
        request.items = { { 21, "Select all   Cmd-A" }, { 27, "Paste   Cmd-V", clip }, { 22, "Fit   F" }, { 23, "Tidy" },
                          { 0, {}, true, false, true },   { 24, "Netlist view   Cmd-Shift-D" }, { 0, {}, true, false, true },
                          { 25, "Undo   Cmd-Z", ed_->canUndo() }, { 26, "Redo   Cmd-Shift-Z", ed_->canRedo() } };
        host_->showMenu(request, [this, pasteAt](int chosen) {
            if (!showing())
                return;
            switch (chosen)
            {
                case 21: selectAll(); break;
                case 27:
                    if (host_ != nullptr)
                        pasteText(host_->clipboardText(), pasteAt);   // where the menu was opened
                    break;
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
        updateProbes();
        if (probe_.meters)                               // the meters standing still: an audio thread that stopped
        {
            const uint32_t f = probe_.meters().frame;
            probeStill_ = f != probeFrame_ ? 0.0f : probeStill_ + dt;
            probeFrame_ = f;
        }
        if (easing_)
        {
            st_->zoom = funkgui::ease::toward(st_->zoom, zoomTo_, dt, kEaseTau, 1e-4f);
            st_->cx = funkgui::ease::toward(st_->cx, cxTo_, dt, kEaseTau, 1e-3f);
            st_->cy = funkgui::ease::toward(st_->cy, cyTo_, dt, kEaseTau, 1e-3f);
            easing_ = !(st_->zoom == zoomTo_ && st_->cx == cxTo_ && st_->cy == cyTo_);
        }
        if (!notice_.empty() && ed_->revision() != noticeRev_)
            quiet();                                     // an undo, a redo, typing or a merge since it was said
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
        // A gesture undone for a write of Claude's: said again once its merge moved the text (a notice is its revision's).
        if (overtaken_ && ed_->revision() != overtakenRev_)
        {
            overtaken_ = false;
            say(kOvertaken);
        }
        // A field whose base the text left: closed, nothing written (as a gesture is cancelled below).
        if (field_.on && ed_->revision() != field_.rev)
        {
            closeField();
            say("The circuit changed: edit it again");
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
        if (easing_ || fitPending_ || !flash_.empty() || (probesLive() && probeStill_ < kProbeStill))
            return false;                                // a live readout: 60 Hz while it shows and its meters move
        if (drag_ == Drag::move || drag_ == Drag::wire || drag_ == Drag::marquee || drag_ == Drag::carry || drag_ == Drag::knob)
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
            return countRect().contains(p) || stripButtonRect().contains(p) ? funkgui::Cursor::pointingHand : funkgui::Cursor::normal;
        }
        if (!canvas_.contains(p))
            return funkgui::Cursor::normal;
        switch (drag_)
        {
            case Drag::move: return moveRefused_ ? funkgui::Cursor::notAllowed : funkgui::Cursor::grabbing;
            case Drag::wire: return funkgui::Cursor::crosshair;
            case Drag::pan: return funkgui::Cursor::grabbing;
            case Drag::carry: return funkgui::Cursor::copy;
            case Drag::knob: return funkgui::Cursor::upDown;
            case Drag::control: return funkgui::Cursor::pointingHand;
            case Drag::marquee:
            case Drag::press:
            case Drag::none:
                break;
        }
        if (tool_ == Tool::place)
            return funkgui::Cursor::copy;
        if (const int k = tool_ == Tool::select ? knobAt(p) : -1; k >= 0)
            return knobs()[static_cast<std::size_t>(k)].chip ? funkgui::Cursor::pointingHand : funkgui::Cursor::upDown;
        if (field_.on && fieldRect().contains(p))
            return funkgui::Cursor::normal;
        const Hit h = hitAt(p);
        if (tool_ == Tool::ground || tool_ == Tool::label)
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

    funkgui::Rect SchematicView::countRect() const
    {
        const std::string count = stripCount();
        if (count.empty())
            return {};
        const float w = funkgui::text::width(funkgui::FontService::get().atlas(), count.c_str(), chat::kSmall) + 12.0f;
        return { strip_.right() - w, strip_.y, w, strip_.h };
    }

    funkgui::Point SchematicView::centreOfProblems() const
    {
        const funkgui::Rect r = countRect();
        return r.w > 0.0f ? funkgui::Point{ r.centreX(), r.centreY() } : funkgui::Point{ strip_.right() - 60.0f, strip_.centreY() };
    }

    bool SchematicView::emptyNetlist() const
    {
        ensure();
        return showing() && nl_.elements.empty() && !nl_.input && !nl_.output;
    }

    std::optional<funkgui::Point> SchematicView::centreOfStart() const
    {
        if (!emptyNetlist())
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
        if (std::string n = notice(); !n.empty())
            return n;
        if (st_ == nullptr)
            return {};
        // a circuit dial: the one turned, else the one under the pointer
        if (drag_ == Drag::knob)
        {
            for (const Knob& k : knobs())
                if (k.part == knobPart_)
                    return knobWords(k);
        }
        else if (tool_ == Tool::select && (drag_ == Drag::none || drag_ == Drag::control))
            if (const int k = knobAt(pointer_); k >= 0)
                return knobWords(knobs()[static_cast<std::size_t>(k)]);
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
            case Tool::label:
                return "Click a pin or a wire: name its net (a name it has merges them)   Escape ends";
            case Tool::select:
                break;
        }
        if (d_.parts.empty())
            return emptyNetlist() ? "An empty netlist" : "No part here can be drawn yet";
        return "Drag from a pin to wire it   R C L D P X S place parts   G grounds";
    }

    std::string SchematicView::stripCount() const
    {
        if (marks_.empty())
            return {};
        char buf[48];
        std::snprintf(buf, sizeof buf, "%zu PROBLEM%s", marks_.size(), marks_.size() == 1 ? "" : "S");
        return buf;
    }

    std::string SchematicView::stripBanner() const
    {
        // The parts not drawn first: FMinus refuses each of them, so the count is never without them.
        if (!d_.notDrawn.empty())
        {
            std::string s = "Not drawn: ";
            for (std::size_t i = 0; i < d_.notDrawn.size(); ++i)
                s += (i == 0 ? "" : ", ") + d_.notDrawn[i];
            return s;
        }
        if (hooks_.played && !hooks_.played())
            return "Not played: no program #includes " + file_;
        // the selected net's probe, when it reads nothing and no card says why (one shows for a net that resolves): why
        // (the spec's §9.3)
        if (!slots_[0].reason.empty() && slots_[0].node < 0)
            return "Probe: " + slots_[0].reason;
        if (dials_.playedNote)
            if (std::string note = dials_.playedNote(); !note.empty())
                return note;                             // "program.fm now plays pedal.cir", while Undo can take it back
        if (!slots_[0].reason.empty())
            return "Probe: " + slots_[0].reason;         // ("no voice sounding": the card says it too)
        // Why its pots have no knobs: the program calls the module's set() itself (the binding's own note).
        if (dials_.bindings)
            for (const lang::CircuitBinding& b : dials_.bindings())
                if (b.file == file_ && b.own && !b.why.empty())
                    return b.why;                        // "program.fm:38: Screamer's pots have no dials: the program calls its set()"
        return {};
    }

    std::string SchematicView::stripButton() const
    {
        if (!showing() || !d_.notDrawn.empty())
            return {};
        if (hooks_.played && !hooks_.played())
            return dials_.play ? "Play this circuit" : std::string();
        if (!dials_.playedNote)
            return {};
        const std::string note = dials_.playedNote();    // Undo only beside the note it takes back
        return !note.empty() && stripBanner() == note ? "Undo" : std::string();
    }

    funkgui::Rect SchematicView::stripButtonRect() const
    {
        const std::string label = stripButton();
        if (label.empty())
            return {};
        const funkgui::Rect count = countRect();
        const float w = funkgui::text::width(funkgui::FontService::get().atlas(), label.c_str(), chat::kSmall) + 16.0f;
        const float right = strip_.right() - (count.w > 0.0f ? count.w + 6.0f : 6.0f);
        return { right - w, strip_.y + 3.0f, w, strip_.h - 6.0f };
    }

    std::optional<funkgui::Point> SchematicView::centreOfStripButton() const
    {
        const funkgui::Rect r = stripButtonRect();
        if (r.w <= 0.0f)
            return std::nullopt;
        return funkgui::Point{ r.centreX(), r.centreY() };
    }

    // ---- the circuit dials on the drawing -------------------------------------------------------------------------------
    std::vector<SchematicView::Knob> SchematicView::knobs() const
    {
        std::vector<Knob> out;
        if (!showing() || !dials_.bindings)
            return out;
        ensure();
        const lang::CircuitBinding* b = nullptr;
        for (const lang::CircuitBinding& x : dials_.bindings())
            if (x.file == file_ && x.dialled())
            {
                b = &x;
                break;
            }
        const sch::View v = view();
        if (b == nullptr || sch::labelPx(v, false) <= 0.0f)
            return out;
        const auto folded = [](std::string x) {
            for (char& ch : x)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return x;
        };
        const std::size_t pots = b->pots.size();
        for (std::size_t i = 0; i < d_.parts.size(); ++i)
        {
            const cir::Part& p = d_.parts[i];
            if (p.element < 0 || p.element >= static_cast<int>(nl_.elements.size()))
                continue;
            const cir::Element& el = nl_.elements[static_cast<std::size_t>(p.element)];
            Knob k;
            k.part = static_cast<int>(i);
            if (el.kind == cir::Kind::Pot || el.kind == cir::Kind::Rheostat)
            {
                // the first pot its {expression} reads
                for (const cir::Token& t : el.tokens)
                {
                    if (t.text.empty() || t.text.front() != '{')
                        continue;
                    std::string word;
                    for (std::size_t c = 0; c <= t.text.size() && k.slot < 0; ++c)
                    {
                        const char ch = c < t.text.size() ? t.text[c] : ' ';
                        if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_')
                        {
                            word += ch;
                            continue;
                        }
                        if (!word.empty() && std::isalpha(static_cast<unsigned char>(word.front())) != 0)
                            for (std::size_t a = 0; a < pots && k.slot < 0; ++a)
                                if (folded(b->pots[a].name) == folded(word))
                                    k.slot = b->slots[a];
                        word.clear();
                    }
                    break;
                }
            }
            else if (el.kind == cir::Kind::S && el.tokens.size() > 3)
            {
                for (std::size_t w = 0; w < b->switches.size() && k.slot < 0; ++w)
                    if (folded(b->switches[w].name) == folded(el.tokens[3].text))
                    {
                        k.slot = b->slots[pots + w];
                        k.chip = true;
                        k.positions = b->switches[w].positions;
                    }
            }
            if (k.slot < 0)
                continue;
            out.push_back(k);
        }
        // beside the body, where the fewest bodies, flags, pins and wires are (its labels then keep clear of it)
        std::vector<sch::KnobSpot> spots;
        for (const Knob& k : out)
            spots.push_back({ k.part, k.chip ? 0.5f * kChipW : kKnobR, k.chip ? 0.5f * kChipH : kKnobR, {} });
        sch::placeKnobs(v, d_, spots);
        for (std::size_t n = 0; n < out.size(); ++n)
            out[n].at = spots[n].at;
        return out;
    }

    int SchematicView::knobAt(funkgui::Point p) const
    {
        if (!canvas_.contains(p))
            return -1;
        const std::vector<Knob> ks = knobs();
        for (std::size_t k = 0; k < ks.size(); ++k)
        {
            const bool in = ks[k].chip ? std::fabs(p.x - ks[k].at.x) <= 0.5f * kChipW + 2.0f && std::fabs(p.y - ks[k].at.y) <= 0.5f * kChipH + 2.0f
                                       : dist(p, ks[k].at) <= kKnobR + 2.0f;
            if (in)
                return static_cast<int>(k);
        }
        return -1;
    }

    std::string SchematicView::knobWords(const Knob& k) const
    {
        funkgui::ParamPort* port = dials_.port ? dials_.port(k.slot) : nullptr;
        if (port == nullptr || !dials_.control)
            return {};
        const Control c = dials_.control(k.slot);
        const std::string value = dialText(c, plainOf(c, port->value01()));
        return std::string(c.name) + " " + value
             + (k.chip ? "   a click steps it (0 to " + std::to_string(k.positions - 1) + ")"
                       : "   drag to turn it, double-click: its default");
    }

    std::optional<funkgui::Point> SchematicView::centreOfKnob(const std::string& part) const
    {
        for (const Knob& k : knobs())
            if (d_.parts[static_cast<std::size_t>(k.part)].name == part)
                return k.at;
        return std::nullopt;
    }

    int SchematicView::knobSlot(const std::string& part) const
    {
        for (const Knob& k : knobs())
            if (d_.parts[static_cast<std::size_t>(k.part)].name == part)
                return k.slot;
        return -1;
    }

    int SchematicView::knobCount() const
    {
        return static_cast<int>(knobs().size());
    }

    std::string SchematicView::stripRight() const
    {
        const std::string banner = stripBanner(), count = stripCount();
        return banner.empty() || count.empty() ? banner + count : banner + "   " + count;
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
            "LABEL (N)   CLICK A PIN OR A WIRE: NAME ITS NET; A NAME ANOTHER NET HAS MERGES THEM",
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
        // the probes' card and lanes: what they read (absolute volts; the DC point is the pots' defaults')
        const int lane = laneAt(pointer);
        if (probeCard().contains(pointer) || lane >= 0)
        {
            const int slot = lane >= 0 ? lane + 1 : 0;
            double dc = 0.0;
            if (const int f = probeTable(); f >= 0 && slots_[static_cast<std::size_t>(slot)].node >= 0)
                dc = probe_.table()[static_cast<std::size_t>(f)].nodes[static_cast<std::size_t>(slots_[static_cast<std::size_t>(slot)].node)].dc;
            std::snprintf(out, n, "PROBE   THE NODE'S VOLTS (ITS DC POINT, THE POTS AT THEIR DEFAULTS: %+.2f V)   %s", dc,
                          lane >= 0 ? "X UNPINS   A CLICK SELECTS ITS NET" : "PIN KEEPS IT IN A LANE   ALT-CLICK A WIRE PINS IT");
            return true;
        }
        if (strip_.contains(pointer))
        {
            if (countRect().contains(pointer))
                std::snprintf(out, n, "PROBLEMS   A CLICK SELECTS THE NEXT ONE'S PART   SHIFT-CLICK THE ONE BEFORE");
            else
                std::snprintf(out, n, "SCHEMATIC   CMD-SHIFT-D SHOWS THE NETLIST   EVERY GESTURE IS ONE UNDO STEP");
            return true;
        }
        if (!canvas_.contains(pointer))
            return false;
        if (field_.on && fieldRect().contains(pointer))
        {
            std::snprintf(out, n, "%s   TAB: THE NEXT ROW   UP, DOWN: E12 (SHIFT: A DECADE)   RETURN WRITES   ESCAPE CANCELS",
                          field_.part.empty() ? "THE NET'S NAME" : upper(field_.part).c_str());
            return true;
        }
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
                std::snprintf(out, n, "%s   DRAG MOVES IT   DOUBLE-CLICK OR RETURN EDITS IT   CMD-R TURNS IT",
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
        return structure_ + probeA11y_;                  // parts or nets, or the probes' readouts, came or went
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
        // on the canvas, at least 1 px (bounds kept in order however small the canvas: no std::clamp with lo > hi)
        const auto clamp = [&](funkgui::Rect r) {
            const float x0 = std::max(canvas_.x, std::min(r.x, canvas_.right() - 1.0f));
            const float y0 = std::max(canvas_.y, std::min(r.y, canvas_.bottom() - 1.0f));
            const float x1 = std::max(x0 + 1.0f, std::min(r.right(), canvas_.right()));
            const float y1 = std::max(y0 + 1.0f, std::min(r.bottom(), canvas_.bottom()));
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
        // the probes' readouts (the card's and the lanes'), text items the bridge reads as they change
        for (std::size_t k = 0; k < slots_.size(); ++k)
        {
            const ProbeSlot& p = slots_[k];
            if (p.net.empty())
                continue;
            funkgui::A11yItem it;
            it.id = kA11yBase + 8 + static_cast<uint32_t>(k);
            it.parent = kA11yBase;
            it.role = funkgui::A11yRole::staticText;
            const funkgui::Rect r = k == 0 ? probeCard() : probeLane(static_cast<int>(k) - 1);
            it.bounds = clamp(r.h > 0.0f ? r : strip_);
            it.title = k == 0 ? "Probe " + p.net : "Probe " + std::to_string(k) + ", " + p.net;
            const std::string numbers = probeNumbers(static_cast<int>(k));
            it.value = asciiText(!p.reason.empty() ? p.reason : numbers.empty() ? std::string("waiting for the program") : numbers);
            it.readOnly = true;
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
                else if (btn == Button::label)
                    active = tool_ == Tool::label;
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
                    case Button::label:
                    {
                        // a name tag: a pointed tab on a lead
                        const float xs[6] = { cx - 6.0f, cx + 2.0f, cx + 6.0f, cx + 2.0f, cx - 6.0f, cx - 6.0f };
                        const float ys[6] = { cy - 4.0f, cy - 4.0f, cy, cy + 4.0f, cy + 4.0f, cy - 4.0f };
                        c.polyline(xs, ys, 6, 1.3f, ink);
                        c.disc(cx - 2.5f, cy, 1.3f, ink);
                        break;
                    }
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
            // at the right, the problems' count (a click steps through them) and before it a banner
            const std::string count = stripCount(), banner = stripBanner();
            float rightW = 0.0f;
            if (!count.empty())
            {
                const bool hot = countRect().contains(pointer_);
                const bool error = std::any_of(marks_.begin(), marks_.end(), [](const Mark& m) { return m.severity == 0; });
                c.text(count.c_str(), strip_.right() - 6.0f, top, st, hot ? pal.accent : error ? k.error : k.warning, funkgui::Align::right);
                rightW = c.textWidth(count.c_str(), st) + 12.0f;
            }
            // the banner's button (Play this circuit, Undo) left of the count, a pill as Start's
            if (const std::string label = stripButton(); !label.empty())
            {
                const funkgui::Rect b = stripButtonRect();
                const bool hot = b.contains(pointer_);
                c.rrect(b.x, b.y, b.w, b.h, 0.5f * b.h, hot ? pal.hover : pal.composer, 1.0f, pal.composerEdge);
                c.text(label.c_str(), b.centreX(), c.capCentreTop(b.centreY(), st), st, hot ? pal.text : pal.soft, funkgui::Align::centre);
                rightW = strip_.right() - b.x - 12.0f + (rightW > 0.0f ? 0.0f : 6.0f);
            }
            if (!banner.empty())
            {
                // three quarters of the strip, three fifths with a button (the hint at the left gives way: a banner
                // says something about this netlist now, as "program.fm:38: Screamer's pots have no dials: ...")
                const float share = stripButton().empty() ? 0.75f : 0.6f;
                const float room = strip_.w * share - 8.0f - rightW - (rightW > 0.0f ? 12.0f : 0.0f);
                const std::string shown = fitText(c, banner, st, std::max(0.0f, room));
                c.text(shown.c_str(), strip_.right() - 6.0f - rightW - (rightW > 0.0f ? 12.0f : 0.0f), top, st, pal.soft,
                       funkgui::Align::right);
                rightW += c.textWidth(shown.c_str(), st) + (rightW > 0.0f ? 24.0f : 12.0f);
            }
            const std::string left = stripLeft();
            c.text(fitText(c, left, st, std::max(0.0f, strip_.w - rightW - 16.0f)).c_str(), strip_.x + 6.0f, top, st,
                   notice().empty() ? pal.soft : pal.text);
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
            // a probe's lane under the pointer: its net's wire in the accent
            if (const int lane = laneAt(pointer_); lane >= 0 && lane < static_cast<int>(pinnedProbes().size()))
                if (const int n = d_.net(pinnedProbes()[static_cast<std::size_t>(lane)]); n >= 0)
                    sch::wires(c, v, d_.nets[static_cast<std::size_t>(n)], sw, k.accent);
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
                if (n.style == cir::Style::wired && !n.unroutable)   // a net drawn as tags at its pins says its name there
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
            std::vector<funkgui::Rect> knobRects;        // the circuit dials' knobs: the labels keep clear of them
            for (const Knob& kn : knobs())
                knobRects.push_back(kn.chip ? funkgui::Rect{ kn.at.x - 0.5f * kChipW, kn.at.y - 0.5f * kChipH, kChipW, kChipH }
                                            : funkgui::Rect{ kn.at.x - kKnobR, kn.at.y - kKnobR, 2.0f * kKnobR, 2.0f * kKnobR });
            const std::vector<sch::LabelSpot> spots = sch::placeLabels(c, v, d_, values, px, knobRects);
            valueRects_.assign(d_.parts.size(), {});     // a click on a value edits it
            for (std::size_t i = 0; i < d_.parts.size() && i < spots.size(); ++i)
                if (d_.parts[i].element >= 0 && !moving(static_cast<int>(i)))
                    valueRects_[i] = sch::valueRect(spots[i], d_.parts[i].name, values[i], px);
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

        // the circuit dials (the program that plays drives this file's pots): a knob beside each pot, a chip beside each
        // switch, the one under the pointer (or turned) in the accent
        if (dials_.port)
        {
            const funkgui::Canvas::Scope s(c, sch::kPartTag, false);
            const std::vector<Knob> ks = knobs();
            const int hot = drag_ == Drag::knob ? -1 : (tool_ == Tool::select ? knobAt(pointer_) : -1);
            for (std::size_t n = 0; n < ks.size(); ++n)
            {
                const Knob& kn = ks[n];
                if (moving(kn.part) || !canvas_.contains(kn.at))
                    continue;
                funkgui::ParamPort* port = dials_.port(kn.slot);
                if (port == nullptr)
                    continue;
                const bool lit = static_cast<int>(n) == hot || (drag_ == Drag::knob && kn.part == knobPart_);
                const funkgui::Col ink = lit ? k.accent : k.symbol;
                if (kn.chip)
                {
                    const Control ctl = dials_.control ? dials_.control(kn.slot) : Control{};
                    sch::chip(c, kn.at, kChipW, kChipH, std::to_string(std::lround(plainOf(ctl, port->value01()))), ink, k);
                }
                else
                    sch::knob(c, kn.at, kKnobR, port->value01(), ink, k);
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

        // the probes: the pinned nets' numbers, the lanes and the card, over the canvas's bottom
        drawProbes(c, th, v, k);

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
            // the field, over the drawing
            drawField(c, th, v, k);
            // nothing drawn: what to do (Start a circuit only in an empty netlist)
            if (d_.parts.empty())
            {
                const funkgui::TextStyle& st = chat::kSmall;
                c.text(emptyNetlist() ? "Place parts from the toolbar, or" : "Place parts from the toolbar", canvas_.centreX(),
                       c.capCentreTop(canvas_.centreY() - 10.0f, st), st, pal.soft, funkgui::Align::centre);
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

    // ---- the probes (SchematicView.h, "Live probes") ------------------------------------------------------------------
    namespace
    {
        // Volts as the readouts write them: three significant digits ("0.402 V", "2.31 V", "12.4 V"), the mean signed.
        std::string volts(float v, bool sign)
        {
            char b[32];
            double x = std::isfinite(v) ? static_cast<double>(v) : 0.0;
            const double a = std::fabs(x);
            if (a < 0.0005)
                x = 0.0;                                 // a zero is +0.000, never -0.000
            if (a < 1.0)
                std::snprintf(b, sizeof b, sign ? "%+.3f V" : "%.3f V", x);
            else if (a < 10.0)
                std::snprintf(b, sizeof b, sign ? "%+.2f V" : "%.2f V", x);
            else
                std::snprintf(b, sizeof b, sign ? "%+.1f V" : "%.1f V", x);
            return b;
        }

        constexpr const char* kNoVoice = "no voice sounding";
    }

    const std::vector<std::string>& SchematicView::pinnedProbes() const
    {
        static const std::vector<std::string> kNone;
        return st_ != nullptr ? st_->probes : kNone;
    }

    bool SchematicView::pinProbe(const std::string& net)
    {
        if (!showing())
            return false;
        ensure();
        const int n = d_.net(net);
        if (n < 0 || d_.nets[static_cast<std::size_t>(n)].key == "0")
            return false;
        const std::string name = d_.nets[static_cast<std::size_t>(n)].name;
        std::vector<std::string>& pins = st_->probes;
        if (std::any_of(pins.begin(), pins.end(), [&](const std::string& p) { return cir::folded(p) == cir::folded(name); }))
            return true;
        if (pins.size() >= 3)
            pins.erase(pins.begin());                    // a fourth replaces the oldest
        pins.push_back(name);
        persist();
        return true;
    }

    bool SchematicView::unpinProbe(const std::string& net)
    {
        if (st_ == nullptr)
            return false;
        std::vector<std::string>& pins = st_->probes;
        const auto it = std::find_if(pins.begin(), pins.end(), [&](const std::string& p) { return cir::folded(p) == cir::folded(net); });
        if (it == pins.end())
            return false;
        pins.erase(it);
        laneScroll_ = 0.0f;
        persist();
        return true;
    }

    bool SchematicView::togglePin(const std::string& net)
    {
        return unpinProbe(net) || pinProbe(net);
    }

    std::string SchematicView::probeNet(int slot) const
    {
        return slot >= 0 && slot < 4 ? slots_[static_cast<std::size_t>(slot)].net : std::string();
    }

    std::string SchematicView::probeReason(int slot) const
    {
        return slot >= 0 && slot < 4 ? slots_[static_cast<std::size_t>(slot)].reason : std::string();
    }

    bool SchematicView::slotApplied(const Meters& m, std::size_t slot) const
    {
        const ProbeSet& a = m.probeSet;
        return sent_.armed() && slot < 4 && sent_.nodes[slot] >= 0 && a.program == sent_.program && a.circuit == sent_.circuit
            && a.instance == sent_.instance && a.nodes[slot] == sent_.nodes[slot] && m.program == sent_.program;
    }

    bool SchematicView::slotLive(int slot) const
    {
        if (!showing() || !probe_.meters || slot < 0 || slot >= 4 || slots_[static_cast<std::size_t>(slot)].node < 0)
            return false;
        const Meters& m = probe_.meters();
        return slotApplied(m, static_cast<std::size_t>(slot)) && m.probeProgram == sent_.program && m.probeInstance >= 0;
    }

    bool SchematicView::probesLive() const
    {
        for (int k = 0; k < 4; ++k)
            if (slotLive(k))
                return true;                             // applied, and reading
        return false;
    }

    std::string SchematicView::probeNumbers(int slot) const
    {
        if (!slotLive(slot))
            return {};
        const ProbeMeter& m = probe_.meters().probes[slot];
        if (!m.valid)
            return "reading";
        return "PEAK " + volts(m.peak, false) + "   RMS " + volts(m.rms, false) + "   MEAN " + volts(m.mean, true);
    }

    std::string SchematicView::probeWords(int slot) const
    {
        const std::string numbers = probeNumbers(slot);
        return numbers.empty() ? std::string() : "PROBE " + slots_[static_cast<std::size_t>(slot)].net + "   " + numbers;
    }

    int SchematicView::probeTable() const
    {
        if (!probe_.table || st_ == nullptr)
            return -1;
        const std::vector<circuits::Probed>& t = probe_.table();
        int first = -1;
        for (std::size_t f = 0; f < t.size(); ++f)
        {
            if (t[f].file != file_)
                continue;
            if (first < 0)
                first = static_cast<int>(f);
            if (!st_->probeAt.empty() && std::find(t[f].paths.begin(), t[f].paths.end(), st_->probeAt) != t[f].paths.end())
                return static_cast<int>(f);
        }
        return first;
    }

    int SchematicView::probeElementIndex(int field) const
    {
        if (field < 0 || !probe_.table || st_ == nullptr)
            return 0;
        const std::vector<std::string>& paths = probe_.table()[static_cast<std::size_t>(field)].paths;
        const auto it = std::find(paths.begin(), paths.end(), st_->probeAt);
        return it == paths.end() ? 0 : static_cast<int>(it - paths.begin());
    }

    std::vector<std::string> SchematicView::probeElements() const
    {
        std::vector<std::string> out;
        if (!probe_.table)
            return out;
        for (const circuits::Probed& c : probe_.table())
            if (c.file == file_)
                out.insert(out.end(), c.paths.begin(), c.paths.end());
        return out;
    }

    std::string SchematicView::probeElement() const
    {
        const int f = probeTable();
        if (f < 0)
            return {};
        const std::vector<std::string>& paths = probe_.table()[static_cast<std::size_t>(f)].paths;
        const int e = probeElementIndex(f);
        return e < static_cast<int>(paths.size()) ? paths[static_cast<std::size_t>(e)] : std::string();
    }

    void SchematicView::setProbeElement(const std::string& path)
    {
        if (st_ == nullptr)
            return;
        const std::vector<std::string> paths = probeElements();
        if (std::find(paths.begin(), paths.end(), path) != paths.end())
            st_->probeAt = paths.empty() || path == paths.front() ? std::string() : path;
    }

    void SchematicView::clearProbes()
    {
        for (ProbeSlot& p : slots_)
        {
            p.net.clear();
            p.node = -1;
            p.reason.clear();
            p.valid = 0;
        }
        if (probe_.arm && (!sentAny_ || !(sent_ == ProbeSet{})))
            probe_.arm(ProbeSet{});
        sent_ = ProbeSet{};
        sentAny_ = true;
        spoken_.clear();
        spokenNet_.clear();
        if (probeItems_ != 0)
        {
            probeItems_ = 0;
            ++probeA11y_;
        }
    }

    void SchematicView::updateProbes()
    {
        if (!showing() || !probe_.arm || !probe_.meters)
            return;
        // the nets: slot 0 the selected run's, slots 1 to 3 the pins
        std::array<std::string, 4> nets{};
        if (!runs_.empty())
            if (const int n = d_.net(runs_.front().net); n >= 0)
                nets[0] = d_.nets[static_cast<std::size_t>(n)].name;
        for (std::size_t k = 0; k < 3 && k < st_->probes.size(); ++k)
            nets[k + 1] = st_->probes[k];
        const uint64_t program = probe_.program ? probe_.program() : 0;
        const int field = probeTable();
        const circuits::Probed* c = field >= 0 ? &probe_.table()[static_cast<std::size_t>(field)] : nullptr;
        const bool played = !hooks_.played || hooks_.played();
        ProbeSet set;
        bool any = false;
        for (std::size_t k = 0; k < 4; ++k)
        {
            ProbeSlot& p = slots_[k];
            p.net = nets[k];
            p.node = -1;
            p.reason.clear();
            if (p.net.empty())
                continue;
            const std::string key = cir::folded(p.net);
            if (key == "0" || key == "gnd")
                p.reason = "ground is 0 V";
            else if (!played)
                p.reason = "not played: no program #includes " + file_;
            else if (program == 0 || c == nullptr)
            {
                // the files play it and what plays does not (yet); or what plays reads it but holds none of its module
                const bool runs = program != 0 && probe_.runs && probe_.runs(file_);
                p.reason = runs ? std::string(product::kIsInstrument ? "Voice" : "Effect") + " has no field of " + file_ + "'s module"
                                : std::string("not playing: Cmd-Return runs it");
            }
            else
            {
                int node = -1;
                for (std::size_t i = 0; i < c->nodes.size() && node < 0; ++i)
                    if (cir::folded(c->nodes[i].name) == key)
                        node = static_cast<int>(i);
                if (node < 0)
                    p.reason = p.net + " is not in the program that plays (edited since RUN)";
                else if (!c->nodes[static_cast<std::size_t>(node)].readable)
                    p.reason = p.net + ": " + c->nodes[static_cast<std::size_t>(node)].why;
                else
                {
                    p.node = node;
                    set.nodes[k] = static_cast<int16_t>(node);
                    any = true;
                }
            }
        }
        if (any)
        {
            set.program = program;
            set.circuit = field;
            set.instance = probeElementIndex(field);
        }
        else
            set = ProbeSet{};
        if (!sentAny_ || !(set == sent_))
        {
            sent_ = set;
            sentAny_ = true;
            probe_.arm(set);
        }
        // what the audio thread says of this set, once it plays the set's program: no voice sounding (an instrument's
        // newest, none), or the rings to read
        const Meters& m = probe_.meters();
        for (std::size_t k = 0; k < 4; ++k)
        {
            ProbeSlot& p = slots_[k];
            p.valid = 0;
            if (p.node < 0 || !slotApplied(m, k))
                continue;
            if (m.probeInstance < 0 || m.probeProgram != set.program)
            {
                p.reason = kNoVoice;
                continue;
            }
            if (probe_.samples)
            {
                p.trace.resize(2 * static_cast<std::size_t>(kTraceSamples));
                p.valid = probe_.samples(static_cast<int>(k), p.trace.data(), 2 * kTraceSamples);
            }
        }
        // the selected net's reason, spoken when it changes (the strip or the card draws it); "no voice sounding" comes
        // back after every note let go, so it is spoken once a net, not after each
        const ProbeSlot& s0 = slots_[0];
        const bool sounding = s0.reason.empty() && spoken_ == kNoVoice && spokenNet_ == s0.net;
        if (!sounding && (s0.reason != spoken_ || (s0.reason == kNoVoice && s0.net != spokenNet_)))
        {
            spoken_ = s0.reason;
            spokenNet_ = s0.net;
            if (!spoken_.empty() && host_ != nullptr)
                host_->announce("Probe: " + spoken_);
        }
        int items = 0;
        for (const ProbeSlot& p : slots_)
            items += p.net.empty() ? 0 : 1;
        if (items != probeItems_)
        {
            probeItems_ = items;
            ++probeA11y_;
        }
    }

    funkgui::Rect SchematicView::probeCard() const
    {
        if (!showing() || slots_[0].node < 0)
            return {};
        return { canvas_.x + 6.0f, canvas_.bottom() - 6.0f - kCardH, std::max(0.0f, canvas_.w - 12.0f), kCardH };
    }

    float SchematicView::lanesTop() const
    {
        const funkgui::Rect card = probeCard();
        const float bottom = card.h > 0.0f ? card.y - 4.0f : canvas_.bottom() - 6.0f;
        const int n = static_cast<int>(std::min<std::size_t>(pinnedProbes().size(), 3));
        if (n == 0)
            return bottom;
        const float room = std::max(kProbeShare * canvas_.h - (card.h > 0.0f ? kCardH + 4.0f : 0.0f) - 6.0f, kLaneH);
        return bottom - std::min(static_cast<float>(n) * kLaneH, room);
    }

    funkgui::Rect SchematicView::probeLane(int lane) const
    {
        const int n = static_cast<int>(std::min<std::size_t>(pinnedProbes().size(), 3));
        if (!showing() || lane < 0 || lane >= n)
            return {};
        return { canvas_.x + 6.0f, lanesTop() + static_cast<float>(lane) * kLaneH - laneScroll_, std::max(0.0f, canvas_.w - 12.0f),
                 kLaneH - 2.0f };
    }

    int SchematicView::laneAt(funkgui::Point p) const
    {
        const funkgui::Rect card = probeCard();
        const float bottom = card.h > 0.0f ? card.y - 4.0f : canvas_.bottom() - 6.0f;
        if (p.y < lanesTop() || p.y >= bottom)
            return -1;
        for (int k = 0; k < 3; ++k)
            if (const funkgui::Rect r = probeLane(k); r.h > 0.0f && r.contains(p))
                return k;
        return -1;
    }

    std::optional<funkgui::Point> SchematicView::centreOfPin() const
    {
        const funkgui::Rect card = probeCard();
        if (card.h <= 0.0f)
            return std::nullopt;
        return funkgui::Point{ card.right() - 8.0f - 0.5f * kPinW, card.y + 4.0f + 0.5f * kChipH2 };
    }

    std::optional<funkgui::Point> SchematicView::centreOfChip() const
    {
        const funkgui::Rect card = probeCard();
        if (card.h <= 0.0f || probeElements().size() < 2)
            return std::nullopt;
        const float w = 64.0f;
        return funkgui::Point{ card.right() - 8.0f - kPinW - 6.0f - 0.5f * w, card.y + 4.0f + 0.5f * kChipH2 };
    }

    void SchematicView::openChipMenu()
    {
        if (host_ == nullptr)
            return;
        const std::vector<std::string> paths = probeElements();
        const std::optional<funkgui::Point> at = centreOfChip();
        if (paths.size() < 2 || !at)
            return;
        funkgui::MenuRequest request;
        request.anchor = { at->x - 32.0f, at->y - 0.5f * kChipH2, 64.0f, kChipH2 };
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        const std::string now = probeElement();
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            funkgui::MenuItem item{ static_cast<int>(i) + 1, paths[i] };
            item.checked = paths[i] == now;
            request.items.push_back(std::move(item));
        }
        host_->showMenu(request, [this, paths](int chosen) {
            if (chosen >= 1 && chosen <= static_cast<int>(paths.size()) && showing())
                setProbeElement(paths[static_cast<std::size_t>(chosen - 1)]);
        });
    }

    std::vector<float> SchematicView::probeTrace(int slot) const
    {
        if (!slotLive(slot))
            return {};
        const ProbeSlot& p = slots_[static_cast<std::size_t>(slot)];
        if (p.valid <= 0 || p.trace.empty() || !p.reason.empty())
            return {};
        const int n = static_cast<int>(p.trace.size());
        const int valid = std::min(p.valid, n);
        const float mean = probe_.meters().probes[slot].mean;
        if (valid > kTraceSamples)
        {
            int start = n - kTraceSamples;               // triggered: the last rising crossing that leaves a window
            for (int i = n - kTraceSamples; i > std::max(n - valid, 1); --i)
                if (p.trace[static_cast<std::size_t>(i - 1)] < mean && mean <= p.trace[static_cast<std::size_t>(i)])
                {
                    start = i;
                    break;
                }
            return { p.trace.begin() + start, p.trace.begin() + start + kTraceSamples };
        }
        // No more than a window is of the set (a large block's newest 1,024, §9.3, or a set just armed): triggered on
        // the first rising crossing inside them that leaves half a window, else all of them.
        const int at = sch::risingCrossing(p.trace.data(), n - valid + 1, n - kTraceSamples / 2 + 1, mean);
        return { p.trace.begin() + (at >= 0 ? at : n - valid), p.trace.end() };
    }

    float SchematicView::probeRange(int slot) const
    {
        const std::vector<float> w = probeTrace(slot);
        if (w.empty())
            return 0.0f;
        const float centre = probe_.meters().probes[slot].mean;
        float span = 0.0f;
        for (const float x : w)
            span = std::max(span, std::fabs(x - centre));
        return sch::traceRange(span);
    }

    std::optional<funkgui::Point> SchematicView::flagAt(const std::string& net) const
    {
        const int n = d_.net(net);
        if (n < 0)
            return std::nullopt;
        const cir::Net& x = d_.nets[static_cast<std::size_t>(n)];
        if (!x.edges.empty() && x.style == cir::Style::wired && !x.unroutable)
        {
            // the middle of its longest straight run
            int best = -1;
            std::pair<cir::Pt, cir::Pt> at{};
            for (const std::pair<cir::Pt, cir::Pt>& r : sch::runs(x.edges))
            {
                const int len = std::abs(r.second.x - r.first.x) + std::abs(r.second.y - r.first.y);
                if (len > best)
                {
                    best = len;
                    at = r;
                }
            }
            return funkgui::Point{ 0.5f * static_cast<float>(at.first.x + at.second.x), 0.5f * static_cast<float>(at.first.y + at.second.y) };
        }
        if (!x.pins.empty())
        {
            const cir::PinRef& pr = x.pins.front();
            const cir::Pt p = d_.parts[static_cast<std::size_t>(pr.part)].pins[static_cast<std::size_t>(pr.pin)].at;
            return funkgui::Point{ static_cast<float>(p.x), static_cast<float>(p.y) };
        }
        return std::nullopt;
    }

    void SchematicView::drawProbes(funkgui::Canvas& c, const funkgui::Theme& th, const sch::View& v, const sch::Inks& k) const
    {
        const chat::Palette pal = chat::palette(th);
        const funkgui::TextStyle& st = chat::kSmall;
        const funkgui::TextStyle& strong = chat::kSmallStrong;
        const bool live = probesLive();
        const Meters* m = live ? &probe_.meters() : nullptr;   // (each slot drawn live only when it is: slotLive)
        // the pinned nets' numbers on their wires
        {
            const funkgui::Canvas::Scope s(c, sch::kProbeTag, false);
            for (std::size_t i = 0; i < pinnedProbes().size() && i < 3; ++i)
                if (const std::optional<funkgui::Point> w = flagAt(pinnedProbes()[i]))
                {
                    const funkgui::Point at = v.at(w->x, w->y);
                    if (canvas_.contains(at))
                        sch::probeFlag(c, { at.x, at.y - 9.0f }, static_cast<int>(i) + 1, k);
                }
        }
        // a slot's trace: the newest window of kTraceSamples, from the last rising crossing of its mean that leaves
        // one whole (else free-running: the newest), about its mean, auto-ranged
        const auto drawTrace = [&](int slot, const funkgui::Rect& r) {
            const std::vector<float> w = m != nullptr ? probeTrace(slot) : std::vector<float>{};
            if (w.empty())
            {
                sch::trace(c, r, nullptr, 0, 0.0f, 1.0f, k);
                return;
            }
            const float range = probeRange(slot);
            sch::trace(c, r, w.data(), static_cast<int>(w.size()), m->probes[slot].mean, range, k, kTraceSamples);
        };
        // its range, right of the trace (the trace's edges are its mean +- this)
        const auto drawRange = [&](int slot, float right, float cy) {
            const float range = m != nullptr ? probeRange(slot) : 0.0f;
            if (range <= 0.0f)
                return;
            char lab[24];
            std::snprintf(lab, sizeof lab, range < 1.0f ? "+-%.3g V" : "+-%.0f V", static_cast<double>(range));
            c.text(lab, right, c.capCentreTop(cy, st), st, k.name, funkgui::Align::right);
        };
        const funkgui::Canvas::Scope s(c, sch::kProbeTag, live);
        // the lanes, scrolling within their area
        const int lanes = static_cast<int>(std::min<std::size_t>(pinnedProbes().size(), 3));
        if (lanes > 0)
        {
            const funkgui::Rect card = probeCard();
            const float bottom = card.h > 0.0f ? card.y - 4.0f : canvas_.bottom() - 6.0f;
            const funkgui::Rect area{ canvas_.x, lanesTop(), canvas_.w, std::max(0.0f, bottom - lanesTop()) };
            const funkgui::Canvas::ClipScope clip(c, area);
            const int hot = laneAt(pointer_);
            for (int lane = 0; lane < lanes; ++lane)
            {
                const funkgui::Rect r = probeLane(lane);
                if (r.bottom() <= area.y || r.y >= area.bottom())
                    continue;
                const int slot = lane + 1;
                const ProbeSlot& p = slots_[static_cast<std::size_t>(slot)];
                c.rrect(r.x, r.y, r.w, r.h, 6.0f, pal.raised, 1.0f, lane == hot ? k.accent : pal.composerEdge);
                sch::probeFlag(c, { r.x + 12.0f, r.y + 11.0f }, slot, k);
                const std::string name = p.net.empty() ? pinnedProbes()[static_cast<std::size_t>(lane)] : p.net;
                c.text(name.c_str(), r.x + 24.0f, c.capCentreTop(r.y + 11.0f, strong), strong, pal.text);
                const float left = std::min(250.0f, 0.44f * r.w);
                std::string words;
                if (m != nullptr && slotLive(slot) && p.reason.empty())
                {
                    const ProbeMeter& pm = m->probes[slot];
                    words = pm.valid ? "pk " + volts(pm.peak, false) + "   rms " + volts(pm.rms, false) + "   mean " + volts(pm.mean, true)
                                     : std::string("reading");
                }
                else
                    words = p.reason.empty() ? std::string("waiting for the program") : p.reason;
                c.text(fitText(c, words, st, left - 14.0f).c_str(), r.x + 8.0f, c.capCentreTop(r.y + 25.0f, st), st, pal.soft);
                drawTrace(slot, { r.x + left, r.y + 4.0f, std::max(0.0f, r.w - left - kUnpinW - 62.0f), r.h - 8.0f });
                drawRange(slot, r.right() - kUnpinW - 14.0f, r.centreY());
                // its unpin (x)
                const float ux = r.right() - 6.0f - 0.5f * kUnpinW, uy = r.centreY();
                const bool uh = funkgui::Rect{ ux - 0.5f * kUnpinW, uy - 0.5f * kUnpinW, kUnpinW, kUnpinW }.contains(pointer_);
                const funkgui::Col xi = uh ? pal.text : pal.soft;
                c.segment(ux - 3.5f, uy - 3.5f, ux + 3.5f, uy + 3.5f, 1.3f, xi);
                c.segment(ux - 3.5f, uy + 3.5f, ux + 3.5f, uy - 3.5f, 1.3f, xi);
            }
        }
        // the card: the selected net's readout, its trace, the chip and Pin
        const funkgui::Rect card = probeCard();
        if (card.h <= 0.0f)
            return;
        c.rrect(card.x, card.y, card.w, card.h, 6.0f, pal.raised, 1.0f, pal.composerEdge);
        const ProbeSlot& p = slots_[0];
        const float line = card.y + 4.0f + 0.5f * kChipH2;
        float x = card.x + 8.0f;
        const std::string head = "PROBE " + p.net;
        c.text(head.c_str(), x, c.capCentreTop(line, strong), strong, pal.text);
        x += c.textWidth(head.c_str(), strong) + 14.0f;
        std::string words = p.reason.empty() ? probeNumbers(0) : p.reason;
        if (words.empty())
            words = "waiting for the program";
        const std::optional<funkgui::Point> chip = centreOfChip(), pin = centreOfPin();
        const float right = chip ? chip->x - 32.0f - 8.0f : pin->x - 0.5f * kPinW - 8.0f;
        c.text(fitText(c, words, st, std::max(0.0f, right - x)).c_str(), x, c.capCentreTop(line, st), st, pal.soft);
        if (chip)
        {
            const funkgui::Rect r{ chip->x - 32.0f, chip->y - 0.5f * kChipH2, 64.0f, kChipH2 };
            const bool hot = r.contains(pointer_);
            c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, hot ? pal.hover : pal.composer, 1.0f, pal.composerEdge);
            const std::string e = probeElement();
            c.text(fitText(c, e, st, r.w - 22.0f).c_str(), r.x + 7.0f, c.capCentreTop(r.centreY(), st), st, hot ? pal.text : pal.soft);
            chat::drawIcon(c, chat::Icon::chevronDown, r.right() - 9.0f, r.centreY() + 0.5f, 7.0f, hot ? pal.text : pal.soft);
        }
        {
            const funkgui::Rect r{ pin->x - 0.5f * kPinW, pin->y - 0.5f * kChipH2, kPinW, kChipH2 };
            const bool pinned = std::any_of(pinnedProbes().begin(), pinnedProbes().end(),
                                            [&](const std::string& n) { return cir::folded(n) == cir::folded(p.net); });
            const bool hot = r.contains(pointer_) && !pinned;
            c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, hot ? pal.hover : pal.composer, 1.0f, pal.composerEdge);
            c.text(pinned ? "Pinned" : "Pin", r.centreX(), c.capCentreTop(r.centreY(), st), st, pinned ? pal.faint : hot ? pal.text : pal.soft,
                   funkgui::Align::centre);
        }
        drawTrace(0, { card.x + 8.0f, card.y + 24.0f, card.w - 62.0f, card.h - 30.0f });
        drawRange(0, card.right() - 8.0f, card.y + 24.0f + 0.5f * (card.h - 30.0f));
    }
}
