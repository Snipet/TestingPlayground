// Source/editor/SchematicDraw.cpp: see SchematicDraw.h.
#include "editor/SchematicDraw.h"

#include "editor/CodeAssist.h"

#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>

namespace playground::ui::sch
{
    namespace
    {
        struct P2
        {
            float x = 0.0f, y = 0.0f;
        };

        // A symbol-frame point turned (mN mirrors x first, then N degrees clockwise on screen, y down).
        P2 turn(cir::Orient o, P2 p) noexcept
        {
            const int i = static_cast<int>(o);
            if (i >= 4)
                p.x = -p.x;
            for (int r = 0; r < i % 4; ++r)
                p = { -p.y, p.x };
            return p;
        }

        // Strokes in a symbol's frame, drawn through its orientation, its origin and the view.
        class Pen
        {
        public:
            Pen(funkgui::Canvas& c, const View& v, cir::Orient o, float x, float y, float width, funkgui::Col ink)
                : c_(c), v_(v), o_(o), x_(x), y_(y), w_(width), ink_(ink) {}

            funkgui::Point map(P2 p) const noexcept
            {
                const P2 t = turn(o_, p);
                return v_.at(x_ + t.x, y_ + t.y);
            }

            void line(std::initializer_list<P2> pts) const
            {
                std::array<float, 24> xs{}, ys{};
                int n = 0;
                for (const P2& p : pts)
                {
                    if (n >= static_cast<int>(xs.size()))
                        break;
                    const funkgui::Point s = map(p);
                    xs[static_cast<std::size_t>(n)] = s.x;
                    ys[static_cast<std::size_t>(n)] = s.y;
                    ++n;
                }
                c_.polyline(xs.data(), ys.data(), n, w_, ink_);
            }

            // An arc of the circle at `centre` (frame units) from a0 to a1 (radians, 0 = +x, clockwise on screen).
            void arc(P2 centre, float r, float a0, float a1, int steps) const
            {
                std::array<float, 33> xs{}, ys{};
                steps = std::clamp(steps, 1, 32);
                for (int k = 0; k <= steps; ++k)
                {
                    const float a = a0 + (a1 - a0) * static_cast<float>(k) / static_cast<float>(steps);
                    const funkgui::Point s = map({ centre.x + r * std::cos(a), centre.y + r * std::sin(a) });
                    xs[static_cast<std::size_t>(k)] = s.x;
                    ys[static_cast<std::size_t>(k)] = s.y;
                }
                c_.polyline(xs.data(), ys.data(), steps + 1, w_, ink_);
            }

            void circle(P2 centre, float r) const { arc(centre, r, 0.0f, 6.2831853f, 20); }

        private:
            funkgui::Canvas& c_;
            const View& v_;
            cir::Orient o_;
            float x_, y_, w_;
            funkgui::Col ink_;
        };

        void zigzag(const Pen& p)
        {
            p.line({ { -2.0f, 0.0f }, { -1.5f, 0.0f }, { -1.25f, -0.6f }, { -0.75f, 0.6f }, { -0.25f, -0.6f }, { 0.25f, 0.6f },
                     { 0.75f, -0.6f }, { 1.25f, 0.6f }, { 1.5f, 0.0f }, { 2.0f, 0.0f } });
        }

        void arrowHead(const Pen& p, P2 tip, P2 dir, float len, float half)
        {
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            const P2 d{ dir.x / l, dir.y / l };
            const P2 n{ -d.y, d.x };
            const P2 b{ tip.x - d.x * len, tip.y - d.y * len };
            p.line({ { b.x + n.x * half, b.y + n.y * half }, tip, { b.x - n.x * half, b.y - n.y * half } });
        }

        void triangleAmp(const Pen& p)
        {
            p.line({ { -2.0f, -2.0f }, { 2.0f, 0.0f }, { -2.0f, 2.0f }, { -2.0f, -2.0f } });
        }

        void diamond(const Pen& p)
        {
            p.line({ { 1.0f, -1.2f }, { 2.2f, 0.0f }, { 1.0f, 1.2f }, { -0.2f, 0.0f }, { 1.0f, -1.2f } });
            p.line({ { 1.0f, -2.0f }, { 1.0f, -1.2f } });
            p.line({ { 1.0f, 2.0f }, { 1.0f, 1.2f } });
            p.line({ { -3.0f, -1.0f }, { -1.8f, -1.0f } });
            p.line({ { -3.0f, 1.0f }, { -1.8f, 1.0f } });
            p.line({ { -1.75f, -0.55f }, { -1.25f, -0.55f } });   // the controls' + and -
            p.line({ { -1.5f, -0.8f }, { -1.5f, -0.3f } });
            p.line({ { -1.75f, 0.55f }, { -1.25f, 0.55f } });
        }

        funkgui::TextStyle labelStyle(float px) noexcept { return { px, 0.0f, 0.0f, false }; }
    }

    float View::stroke() const noexcept { return std::clamp(scale * 0.13f, 1.0f, 4.0f); }

    funkgui::Rect View::rect(const cir::Box& b) const noexcept
    {
        const funkgui::Point a = at(static_cast<float>(b.x0), static_cast<float>(b.y0));
        const funkgui::Point z = at(static_cast<float>(b.x1), static_cast<float>(b.y1));
        return { std::min(a.x, z.x), std::min(a.y, z.y), std::fabs(z.x - a.x), std::fabs(z.y - a.y) };
    }

    Inks inksFor(const funkgui::Theme& th, funkgui::Col error, funkgui::Col warning, funkgui::Col note, bool focused)
    {
        Inks k;
        k.ground = th.ground;
        k.symbol = th.ink100;
        k.wire = th.ink70;
        k.value = th.ink70;
        k.name = th.ink52;
        k.grid = th.ink16;
        k.accent = th.accent;
        k.halo = focused ? th.accentDim : th.ink16;      // the code editor's selection rule
        k.flash = th.ink32;
        k.error = error;
        k.warning = warning;
        k.note = note;
        k.faint = th.ink32;
        return k;
    }

    bool lodBox(const View& v, cir::Kind kind) noexcept
    {
        const cir::Box& b = cir::symbol(kind).body;
        return static_cast<float>(std::max(b.x1 - b.x0, b.y1 - b.y0)) * v.scale < kSymbolLodPx;
    }

    void bodyBox(funkgui::Canvas& c, const View& v, cir::Kind kind, cir::Orient o, float x, float y, float width,
                 funkgui::Col ink)
    {
        const cir::Box b = cir::turned(o, cir::symbol(kind).body);
        const funkgui::Point a = v.at(x + static_cast<float>(b.x0), y + static_cast<float>(b.y0));
        const funkgui::Point z = v.at(x + static_cast<float>(b.x1), y + static_cast<float>(b.y1));
        c.rrect(a.x, a.y, z.x - a.x, z.y - a.y, 1.0f, ink.withAlpha(0.0f), width, ink);
    }

    void symbol(funkgui::Canvas& c, const View& v, cir::Kind kind, cir::Orient o, float x, float y, float width,
                funkgui::Col ink)
    {
        const Pen p(c, v, o, x, y, width, ink);
        switch (kind)
        {
            case cir::Kind::R:
                zigzag(p);
                break;
            case cir::Kind::C:
                p.line({ { -2.0f, 0.0f }, { -0.3f, 0.0f } });
                p.line({ { 0.3f, 0.0f }, { 2.0f, 0.0f } });
                p.line({ { -0.3f, -0.9f }, { -0.3f, 0.9f } });
                p.line({ { 0.3f, -0.9f }, { 0.3f, 0.9f } });
                break;
            case cir::Kind::L:
                p.line({ { -2.0f, 0.0f }, { -1.6f, 0.0f } });
                p.line({ { 1.6f, 0.0f }, { 2.0f, 0.0f } });
                for (int k = 0; k < 4; ++k)
                    p.arc({ -1.2f + 0.8f * static_cast<float>(k), 0.0f }, 0.4f, 3.1415927f, 6.2831853f, 8);
                break;
            case cir::Kind::D:
                p.line({ { -2.0f, 0.0f }, { -0.6f, 0.0f } });
                p.line({ { 0.6f, 0.0f }, { 2.0f, 0.0f } });
                p.line({ { -0.6f, -0.7f }, { 0.6f, 0.0f }, { -0.6f, 0.7f }, { -0.6f, -0.7f } });
                p.line({ { 0.6f, -0.7f }, { 0.6f, 0.7f } });
                break;
            case cir::Kind::Pot:
                zigzag(p);
                p.line({ { 0.0f, -2.0f }, { 0.0f, -0.75f } });
                arrowHead(p, { 0.0f, -0.75f }, { 0.0f, 1.0f }, 0.4f, 0.28f);
                break;
            case cir::Kind::Rheostat:
                zigzag(p);
                p.line({ { -1.2f, 0.9f }, { 1.2f, -0.9f } });
                arrowHead(p, { 1.2f, -0.9f }, { 2.4f, -1.8f }, 0.45f, 0.25f);
                break;
            case cir::Kind::OpAmp:
                triangleAmp(p);
                p.line({ { -3.0f, -1.0f }, { -2.0f, -1.0f } });
                p.line({ { -3.0f, 1.0f }, { -2.0f, 1.0f } });
                p.line({ { 2.0f, 0.0f }, { 3.0f, 0.0f } });
                p.line({ { -1.7f, -1.0f }, { -1.1f, -1.0f } });   // - on top (inn), + under it (inp)
                p.line({ { -1.7f, 1.0f }, { -1.1f, 1.0f } });
                p.line({ { -1.4f, 0.7f }, { -1.4f, 1.3f } });
                break;
            case cir::Kind::Vcvs:
                diamond(p);
                p.line({ { 0.75f, -0.5f }, { 1.25f, -0.5f } });   // + over -
                p.line({ { 1.0f, -0.75f }, { 1.0f, -0.25f } });
                p.line({ { 0.75f, 0.5f }, { 1.25f, 0.5f } });
                break;
            case cir::Kind::Vccs:
                diamond(p);
                p.line({ { 1.0f, -0.6f }, { 1.0f, 0.6f } });       // the current, from p to n
                arrowHead(p, { 1.0f, 0.6f }, { 0.0f, 1.0f }, 0.35f, 0.25f);
                break;
            case cir::Kind::Ota:
                triangleAmp(p);
                p.line({ { -3.0f, -1.0f }, { -2.0f, -1.0f } });
                p.line({ { -3.0f, 1.0f }, { -2.0f, 1.0f } });
                p.line({ { 2.0f, 0.0f }, { 3.0f, 0.0f } });
                p.line({ { 0.0f, 2.0f }, { 0.0f, 1.0f } });         // its bias, from below
                p.line({ { -1.7f, -1.0f }, { -1.1f, -1.0f } });
                p.line({ { -1.7f, 1.0f }, { -1.1f, 1.0f } });
                p.line({ { -1.4f, 0.7f }, { -1.4f, 1.3f } });
                p.line({ { 0.9f, -0.45f }, { 0.9f, 0.45f } });      // a current out: the transconductance's bar
                break;
            case cir::Kind::V:
                p.circle({ 0.0f, 0.0f }, 0.9f);
                p.line({ { -2.0f, 0.0f }, { -0.9f, 0.0f } });
                p.line({ { 0.9f, 0.0f }, { 2.0f, 0.0f } });
                p.line({ { -0.65f, 0.0f }, { -0.25f, 0.0f } });
                p.line({ { -0.45f, -0.2f }, { -0.45f, 0.2f } });
                p.line({ { 0.25f, 0.0f }, { 0.65f, 0.0f } });
                break;
            case cir::Kind::I:
                p.circle({ 0.0f, 0.0f }, 0.9f);
                p.line({ { -2.0f, 0.0f }, { -0.9f, 0.0f } });
                p.line({ { 0.9f, 0.0f }, { 2.0f, 0.0f } });
                p.line({ { -0.5f, 0.0f }, { 0.5f, 0.0f } });
                arrowHead(p, { 0.5f, 0.0f }, { 1.0f, 0.0f }, 0.3f, 0.25f);
                break;
            case cir::Kind::S:
                p.line({ { -2.0f, 0.0f }, { -1.0f, 0.0f } });
                p.line({ { 1.0f, 0.0f }, { 2.0f, 0.0f } });
                p.circle({ -1.0f, 0.0f }, 0.18f);
                p.circle({ 1.0f, 0.0f }, 0.18f);
                p.line({ { -1.0f, 0.0f }, { 0.85f, -0.75f } });
                break;
            case cir::Kind::Input:
                p.line({ { -3.0f, -0.8f }, { -0.8f, -0.8f }, { 0.0f, 0.0f }, { -0.8f, 0.8f }, { -3.0f, 0.8f }, { -3.0f, -0.8f } });
                break;
            case cir::Kind::Output:
                p.line({ { 0.0f, 0.0f }, { 0.8f, -0.8f }, { 3.0f, -0.8f }, { 3.0f, 0.8f }, { 0.8f, 0.8f }, { 0.0f, 0.0f } });
                break;
            case cir::Kind::Unknown:
                break;
        }
    }

    void icon(funkgui::Canvas& c, cir::Kind kind, float cx, float cy, float size, funkgui::Col ink)
    {
        const cir::Box& b = cir::symbol(kind).body;
        const float w = static_cast<float>(std::max(b.x1 - b.x0, 4)), h = static_cast<float>(std::max(b.y1 - b.y0, 2));
        View v;
        v.scale = std::min(size / w, size / h);
        const float mx = 0.5f * static_cast<float>(b.x0 + b.x1), my = 0.5f * static_cast<float>(b.y0 + b.y1);
        v.ox = cx - mx * v.scale;
        v.oy = cy - my * v.scale;
        symbol(c, v, kind, cir::Orient::r0, 0.0f, 0.0f, std::max(1.0f, size * 0.07f), ink);
    }

    std::vector<std::pair<cir::Pt, cir::Pt>> runs(const std::vector<cir::UnitEdge>& edges)
    {
        // Horizontal edges by (y, x), vertical by (x, y): consecutive ones join.
        std::vector<cir::UnitEdge> h, vv;
        for (const cir::UnitEdge& e : edges)
            (e.h ? h : vv).push_back(e);
        std::sort(h.begin(), h.end(), [](const cir::UnitEdge& a, const cir::UnitEdge& b) {
            return a.a.y != b.a.y ? a.a.y < b.a.y : a.a.x < b.a.x;
        });
        std::sort(vv.begin(), vv.end(), [](const cir::UnitEdge& a, const cir::UnitEdge& b) {
            return a.a.x != b.a.x ? a.a.x < b.a.x : a.a.y < b.a.y;
        });
        std::vector<std::pair<cir::Pt, cir::Pt>> out;
        for (std::size_t i = 0; i < h.size();)
        {
            cir::Pt a = h[i].a, z = a + cir::Pt{ 1, 0 };
            std::size_t j = i + 1;
            while (j < h.size() && h[j].a.y == z.y && h[j].a.x == z.x)
            {
                z = h[j].a + cir::Pt{ 1, 0 };
                ++j;
            }
            out.emplace_back(a, z);
            i = j;
        }
        for (std::size_t i = 0; i < vv.size();)
        {
            cir::Pt a = vv[i].a, z = a + cir::Pt{ 0, 1 };
            std::size_t j = i + 1;
            while (j < vv.size() && vv[j].a.x == z.x && vv[j].a.y == z.y)
            {
                z = vv[j].a + cir::Pt{ 0, 1 };
                ++j;
            }
            out.emplace_back(a, z);
            i = j;
        }
        return out;
    }

    void wires(funkgui::Canvas& c, const View& v, const cir::Net& net, float width, funkgui::Col ink)
    {
        for (const auto& [a, b] : runs(net.edges))
        {
            const funkgui::Point p = v.at(a), q = v.at(b);
            c.segment(p.x, p.y, q.x, q.y, width, ink);
        }
    }

    void junctions(funkgui::Canvas& c, const View& v, const cir::Net& net, funkgui::Col ink)
    {
        // 0.28 of a unit, at most 1.7 strokes, never under 2 px (min then max: below about 90 % the cap falls under
        // the floor, which std::clamp may not be given)
        const float r = std::max(2.0f, std::min(v.scale * 0.28f, 1.7f * v.stroke()));
        for (const cir::Pt& j : net.junctions)
        {
            const funkgui::Point p = v.at(j);
            c.disc(p.x, p.y, r, ink);
        }
    }

    float labelPx(const View& v, bool picture) noexcept
    {
        const float px = std::min(kLabelMaxPx, kLabelPx * v.scale / kUnitPx);
        if (picture)
            return std::max(kPictureLabelPx, px);
        return px < kLabelHidePx ? 0.0f : px;
    }

    namespace
    {
        // Text beside a point in a direction (a flag's name): past it along `out`, upright.
        void textOut(funkgui::Canvas& c, funkgui::Point p, cir::Dir out, const std::string& s, funkgui::Col ink, float px)
        {
            const funkgui::TextStyle st = labelStyle(px);
            const float gap = px * 0.3f;
            switch (out)
            {
                case cir::Dir::E: c.text(s.c_str(), p.x + gap, c.capCentreTop(p.y, st), st, ink); break;
                case cir::Dir::W: c.text(s.c_str(), p.x - gap, c.capCentreTop(p.y, st), st, ink, funkgui::Align::right); break;
                case cir::Dir::N: c.text(s.c_str(), p.x, p.y - gap - px, st, ink, funkgui::Align::centre); break;
                case cir::Dir::S: c.text(s.c_str(), p.x, p.y + gap, st, ink, funkgui::Align::centre); break;
            }
        }
    }

    void flag(funkgui::Canvas& c, const View& v, const cir::Flag& f, cir::Style style, bool positive,
              const std::string& name, float width, funkgui::Col ink, funkgui::Col text, float px)
    {
        (void)positive;
        const cir::Pt u = cir::unit(f.out);
        const cir::Pt side = cir::horizontal(f.out) ? cir::Pt{ 0, 1 } : cir::Pt{ 1, 0 };
        const auto at = [&](float along, float across) {
            return v.at(static_cast<float>(f.at.x) + static_cast<float>(u.x) * along + static_cast<float>(side.x) * across,
                        static_cast<float>(f.at.y) + static_cast<float>(u.y) * along + static_cast<float>(side.y) * across);
        };
        const funkgui::Point p0 = at(0.0f, 0.0f), p1 = at(1.0f, 0.0f);
        c.segment(p0.x, p0.y, p1.x, p1.y, width, ink);
        if (style == cir::Style::ground)
        {
            const float bars[3][2] = { { 1.0f, 0.8f }, { 1.4f, 0.5f }, { 1.8f, 0.2f } };
            for (const auto& b : bars)
            {
                const funkgui::Point a = at(b[0], -b[1]), z = at(b[0], b[1]);
                c.segment(a.x, a.y, z.x, z.y, width, ink);
            }
            return;
        }
        if (style == cir::Style::rail)
        {
            const funkgui::Point a = at(1.0f, -0.8f), z = at(1.0f, 0.8f);
            c.segment(a.x, a.y, z.x, z.y, width, ink);
            if (px > 0.0f)
                textOut(c, at(1.2f, 0.0f), f.out, name, text, px);
            return;
        }
        // a name tag: the lead, a small open end, the name
        const funkgui::Point e = at(1.0f, 0.0f);
        c.disc(e.x, e.y, std::max(1.5f, v.scale * 0.15f), ink.withAlpha(0.0f), width, ink);
        if (px > 0.0f)
            textOut(c, at(1.3f, 0.0f), f.out, name, text, px);
    }

    void tag(funkgui::Canvas& c, const View& v, cir::Pt at, const std::string& name, funkgui::Col ink, float px)
    {
        if (px <= 0.0f)
            return;
        const funkgui::Point p = v.at(at);
        const funkgui::TextStyle st = labelStyle(px);
        c.text(name.c_str(), p.x, p.y - px * 1.25f, st, ink, funkgui::Align::centre);
    }

    void terminal(funkgui::Canvas& c, const View& v, const cir::Part& p, float width, funkgui::Col ink, funkgui::Col text,
                  float px)
    {
        const float x = static_cast<float>(p.origin.x), y = static_cast<float>(p.origin.y);
        if (lodBox(v, p.kind))
            bodyBox(c, v, p.kind, cir::Orient::r0, x, y, width, ink);
        else
            symbol(c, v, p.kind, cir::Orient::r0, x, y, width, ink);
        if (px <= 0.0f)
            return;
        const funkgui::TextStyle st = labelStyle(px);
        const float cx = p.kind == cir::Kind::Input ? x - 1.7f : x + 1.8f;
        const funkgui::Point m = v.at(cx, y);
        c.text(p.name.c_str(), m.x, c.capCentreTop(m.y, st), st, text, funkgui::Align::centre);
    }

    std::vector<LabelSpot> placeLabels(const funkgui::Canvas& c, const View& v, const cir::Drawing& d,
                                       const std::vector<std::string>& values, float px,
                                       const std::vector<funkgui::Rect>& keepOut)
    {
        std::vector<LabelSpot> out(d.parts.size());
        if (px <= 0.0f)
            return out;
        const funkgui::TextStyle st = labelStyle(px);
        const float line = std::round(px * 1.2f), gap = px * 0.3f;
        // What a label should not cover: bodies and flags, the labels placed before, pins and junctions (a connection
        // it would hide), much; a wire's run, less.
        std::vector<funkgui::Rect> bodies, posts, wiresPx;
        const float postR = std::max(2.0f, 0.3f * v.scale);
        const auto post = [&](cir::Pt at) {
            const funkgui::Point q = v.at(at);
            posts.push_back({ q.x - postR, q.y - postR, 2.0f * postR, 2.0f * postR });
        };
        for (const cir::Part& p : d.parts)
        {
            bodies.push_back(v.rect(p.body).expanded(1.0f));
            for (const cir::PinAt& pin : p.pins)
                post(pin.at);
        }
        for (const cir::Flag& f : d.flags)
            bodies.push_back(v.rect(f.box));
        for (const cir::Net& n : d.nets)
            if (n.style == cir::Style::wired && !n.unroutable)
            {
                for (const auto& [a, b] : runs(n.edges))
                {
                    const funkgui::Point p = v.at(a), q = v.at(b);
                    wiresPx.push_back({ std::min(p.x, q.x) - 1.0f, std::min(p.y, q.y) - 1.0f, std::fabs(q.x - p.x) + 2.0f, std::fabs(q.y - p.y) + 2.0f });
                }
                for (const cir::Pt& j : n.junctions)
                    post(j);
            }
        std::vector<funkgui::Rect> placed;
        const auto meets = [](const funkgui::Rect& a, const funkgui::Rect& b) {
            return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
        };
        const float half = 0.5f * v.scale;               // half a unit: a candidate's shifted variants
        // A point's distance to a rect (0 inside): a label nearer another part's body than its own reads as the other's.
        const auto away = [](funkgui::Point q, const funkgui::Rect& r) {
            const float dx = std::max({ r.x - q.x, 0.0f, q.x - r.right() }), dy = std::max({ r.y - q.y, 0.0f, q.y - r.bottom() });
            return std::sqrt(dx * dx + dy * dy);
        };
        for (std::size_t i = 0; i < d.parts.size(); ++i)
        {
            const cir::Part& p = d.parts[i];
            if (p.element < 0)
                continue;
            const funkgui::Rect b = v.rect(p.body);
            const std::string& value = i < values.size() ? values[i] : std::string();
            const float wn = c.textWidth(p.name.c_str(), st), wv = value.empty() ? 0.0f : c.textWidth(value.c_str(), st);
            const bool two = !value.empty();
            const float w2 = std::max(wn, wv), h2 = two ? 2.0f * line : line;
            const float w1 = two ? c.textWidth((p.name + " " + value).c_str(), st) : wn;
            struct Cand
            {
                LabelSpot spot;
                funkgui::Rect box;
            };
            const auto shifted = [](Cand k, float dx, float dy) {
                k.spot.x += dx;
                k.spot.top += dy;
                k.box.x += dx;
                k.box.y += dy;
                return k;
            };
            const auto above = [&](bool one) {
                const float w = one ? w1 : w2, h = one ? line : h2;
                return Cand{ { b.centreX(), b.y - gap - h, one, funkgui::Align::centre, true, true }, { b.centreX() - 0.5f * w, b.y - gap - h, w, h } };
            };
            const auto below = [&](bool one) {
                const float w = one ? w1 : w2, h = one ? line : h2;
                return Cand{ { b.centreX(), b.bottom() + gap, one, funkgui::Align::centre, true, true }, { b.centreX() - 0.5f * w, b.bottom() + gap, w, h } };
            };
            const auto right = [&](bool one) {
                const float w = one ? w1 : w2, h = one ? line : h2;
                const float top = b.centreY() - 0.5f * h;
                return Cand{ { b.right() + gap, top, one, funkgui::Align::left, true, true }, { b.right() + gap, top, w, h } };
            };
            const auto left = [&](bool one) {
                const float w = one ? w1 : w2, h = one ? line : h2;
                const float top = b.centreY() - 0.5f * h;
                return Cand{ { b.x - gap, top, one, funkgui::Align::right, true, true }, { b.x - gap - w, top, w, h } };
            };
            // Beside the body off its centre line, where a horizontal part's leads run (above it: up < 0)...
            const auto besideOff = [&](bool toRight, float up, bool one) {
                const Cand k = toRight ? right(one) : left(one);
                const float top = up < 0.0f ? b.centreY() - postR - 1.0f - k.box.h : b.centreY() + postR + 1.0f;
                return shifted(k, 0.0f, top - k.box.y);
            };
            // ... and above or below the body beside its centre line, where a vertical part's leads run.
            const auto endOff = [&](bool atTop, bool toRight, bool one) {
                Cand k = atTop ? above(one) : below(one);
                const float x = toRight ? b.centreX() + postR + 1.0f : b.centreX() - postR - 1.0f;
                k.spot.x = x;
                k.spot.align = toRight ? funkgui::Align::left : funkgui::Align::right;
                k.box.x = toRight ? x : x - k.box.w;
                return k;
            };
            std::vector<Cand> cands;
            if (b.w >= b.h)
            {
                cands = { above(false), below(false), above(true), below(true) };
                for (const float dx : { -half, half })
                {
                    cands.push_back(shifted(above(true), dx, 0.0f));
                    cands.push_back(shifted(below(true), dx, 0.0f));
                }
                for (const bool one : { false, true })
                    cands.insert(cands.end(), { besideOff(true, -1.0f, one), besideOff(false, -1.0f, one), besideOff(true, 1.0f, one),
                                                besideOff(false, 1.0f, one) });
                cands.insert(cands.end(), { right(true), left(true) });
            }
            else
            {
                cands = { right(false), left(false), right(true), left(true) };
                for (const float dy : { -half, half })
                {
                    cands.push_back(shifted(right(false), 0.0f, dy));
                    cands.push_back(shifted(left(false), 0.0f, dy));
                }
                for (const bool one : { false, true })
                    cands.insert(cands.end(), { endOff(true, true, one), endOff(true, false, one), endOff(false, true, one),
                                                endOff(false, false, one) });
                cands.insert(cands.end(), { above(true), below(true) });
            }
            int best = -1;
            float bestScore = 0.0f;
            bool bestCrosses = false;
            for (std::size_t k = 0; k < cands.size(); ++k)
            {
                const funkgui::Rect& r = cands[k].box;
                float score = 0.0f;
                bool crosses = false;                    // a pin, a junction or a wire under it
                for (std::size_t j = 0; j < bodies.size(); ++j)
                    if (j != i && meets(r, bodies[j]))
                        score += 10.0f;
                for (const funkgui::Rect& l : placed)
                    if (meets(r, l))
                        score += 10.0f;
                for (const funkgui::Rect& o : keepOut)
                    if (meets(r, o))
                        score += 10.0f;
                for (const funkgui::Rect& q : posts)
                    if (meets(r, q))
                    {
                        score += 10.0f;
                        crosses = true;
                    }
                for (const funkgui::Rect& w : wiresPx)
                    if (meets(r, w))
                    {
                        score += 3.0f;
                        crosses = true;
                    }
                const funkgui::Point mid{ r.centreX(), r.centreY() };
                const float own = away(mid, b);
                for (std::size_t j = 0; j < d.parts.size(); ++j)
                    if (j != i && away(mid, bodies[j]) < own)
                    {
                        score += 4.0f;
                        break;
                    }
                if (best < 0 || score < bestScore)
                {
                    best = static_cast<int>(k);
                    bestScore = score;
                    bestCrosses = crosses;
                }
                if (score == 0.0f)
                    break;
            }
            out[i] = cands[static_cast<std::size_t>(best)].spot;
            out[i].backdrop = !bestCrosses;              // over a wire, a pin or a junction, the connection shows through
            placed.push_back(cands[static_cast<std::size_t>(best)].box);
        }
        return out;
    }

    void labels(funkgui::Canvas& c, const LabelSpot& s, const std::string& name, const std::string& value,
                funkgui::Col nameInk, funkgui::Col valueInk, funkgui::Col ground, float px, float dx, float dy)
    {
        if (px <= 0.0f || !s.shown)
            return;
        const funkgui::TextStyle st = labelStyle(px);
        const float line = std::round(px * 1.2f);
        const float x = s.x + dx, top = s.top + dy;
        const auto backdrop = [&](float x0, float w, float y, float h) {
            if (s.backdrop)
                c.rrect(x0 - 1.5f, y - 1.0f, w + 3.0f, h + 1.0f, 2.0f, ground.withAlpha(0.82f));
        };
        const auto left = [&](float w) { return s.align == funkgui::Align::centre ? x - 0.5f * w : s.align == funkgui::Align::right ? x - w : x; };
        if (s.oneLine && !value.empty())
        {
            // "R6 51k": the name in its ink, the value after it in its own
            const float wn = c.textWidth((name + " ").c_str(), st), w = c.textWidth((name + " " + value).c_str(), st);
            const float x0 = left(w);
            backdrop(x0, w, top, line);
            c.text(name.c_str(), x0, top, st, nameInk);
            c.text(value.c_str(), x0 + wn, top, st, valueInk);
            return;
        }
        const float wn = c.textWidth(name.c_str(), st), wv = value.empty() ? 0.0f : c.textWidth(value.c_str(), st);
        const float w = std::max(wn, wv);
        backdrop(left(w), w, top, value.empty() ? line : 2.0f * line);
        c.text(name.c_str(), x, top, st, nameInk, s.align);
        if (!value.empty())
            c.text(value.c_str(), x, top + line, st, valueInk, s.align);
    }

    funkgui::Point badgeAt(const View& v, const cir::Part& p) noexcept
    {
        const funkgui::Rect b = v.rect(p.body);
        return { b.right() + 2.0f, b.y - 2.0f };
    }

    void marks(funkgui::Canvas& c, const View& v, const cir::Part& p, const PartMarks& m, const Inks& ink)
    {
        const funkgui::Rect b = v.rect(p.body);
        if (m.outline)
            c.rrect(b.x - 2.0f, b.y - 2.0f, b.w + 4.0f, b.h + 4.0f, 3.0f, ink.error.withAlpha(0.0f), 1.5f, ink.error);
        for (std::size_t k = 0; k < p.pins.size() && k < 8; ++k)
        {
            const uint8_t bit = static_cast<uint8_t>(1u << k);
            const bool e = (m.pinMask[0] & bit) != 0, w = (m.pinMask[1] & bit) != 0, n = (m.pinMask[2] & bit) != 0;
            if (!e && !w && !n)
                continue;
            const funkgui::Point at = v.at(p.pins[k].at);
            const funkgui::Col pinInk = e ? ink.error : w ? ink.warning : ink.note;
            c.disc(at.x, at.y, std::max(3.5f, v.scale * 0.4f), pinInk.withAlpha(0.0f), 1.5f, pinInk);
        }
        if (m.errors + m.warnings + m.notes == 0)
            return;
        const funkgui::Point at = badgeAt(v, p);
        if (m.errors > 0 || m.warnings > 0)
        {
            const funkgui::Col fill = m.errors > 0 ? ink.error : ink.warning;
            c.disc(at.x, at.y, kBadgeR, fill);
            char n[8];
            std::snprintf(n, sizeof n, "%d", std::min(9, m.errors > 0 ? m.errors : m.warnings));
            const funkgui::TextStyle st{ 9.0f, 0.0f, 0.06f, true };
            c.text(n, at.x, c.capCentreTop(at.y, st), st, ink.ground, funkgui::Align::centre);
            return;
        }
        c.disc(at.x, at.y, kBadgeR - 1.0f, ink.note.withAlpha(0.0f), 1.5f, ink.note);
    }

    void grid(funkgui::Canvas& c, const View& v, const funkgui::Rect& clip, funkgui::Col ink)
    {
        if (v.scale < kGridMinPx)
            return;
        const int steps[] = { 1, 2, 5, 10, 20, 50, 100 };
        for (const int k : steps)
        {
            const float px = v.scale * static_cast<float>(k);
            if (px < kGridMinPx)
                continue;
            const funkgui::Point w0 = v.world({ clip.x, clip.y }), w1 = v.world({ clip.right(), clip.bottom() });
            const int x0 = static_cast<int>(std::ceil(w0.x / static_cast<float>(k))) * k;
            const int y0 = static_cast<int>(std::ceil(w0.y / static_cast<float>(k))) * k;
            const int x1 = static_cast<int>(std::floor(w1.x / static_cast<float>(k))) * k;
            const int y1 = static_cast<int>(std::floor(w1.y / static_cast<float>(k))) * k;
            const long cols = x1 >= x0 ? (x1 - x0) / k + 1 : 0, rows = y1 >= y0 ? (y1 - y0) / k + 1 : 0;
            if (cols * rows > kGridMaxDots)
                continue;
            const float r = 0.75f;
            for (int y = y0; y <= y1; y += k)
                for (int x = x0; x <= x1; x += k)
                {
                    const funkgui::Point p = v.at(static_cast<float>(x), static_cast<float>(y));
                    c.disc(p.x, p.y, r, ink);
                }
            return;
        }
    }

    std::string valueText(const cir::Netlist& nl, const cir::Part& p)
    {
        if (p.element < 0 || p.element >= static_cast<int>(nl.elements.size()))
            return {};
        const cir::Element& e = nl.elements[static_cast<std::size_t>(p.element)];
        const auto token = [&](int i) { return i >= 0 && i < static_cast<int>(e.tokens.size()) ? e.tokens[static_cast<std::size_t>(i)].text : std::string(); };
        switch (e.kind)
        {
            case cir::Kind::OpAmp:
            case cir::Kind::Input:
            case cir::Kind::Output:
            case cir::Kind::Unknown:
                return {};
            case cir::Kind::Pot:
            case cir::Kind::Rheostat:
            {
                std::string v = token(e.valueToken);
                for (std::size_t i = 1; i < e.tokens.size(); ++i)
                    if (!e.tokens[i].text.empty() && e.tokens[i].text.front() == '{')
                    {
                        v += (v.empty() ? "" : " ") + e.tokens[i].text;
                        break;
                    }
                return v;
            }
            case cir::Kind::S:
                return e.tokens.size() > 3 ? token(3) + ": " + token(e.valueToken) : token(e.valueToken);
            case cir::Kind::R:
            case cir::Kind::C:
            case cir::Kind::L:
            case cir::Kind::D:
            case cir::Kind::Vcvs:
            case cir::Kind::Vccs:
            case cir::Kind::Ota:
            case cir::Kind::V:
            case cir::Kind::I:
                break;
        }
        return token(e.valueToken);
    }

    void knob(funkgui::Canvas& c, funkgui::Point at, float r, float t, funkgui::Col ink, const Inks& k)
    {
        constexpr float kPi = 3.14159265f;
        const float a0 = 0.75f * kPi, a1 = 2.25f * kPi;  // 270 degrees, from the lower left round to the lower right
        const float a = a0 + std::clamp(std::isfinite(t) ? t : 0.0f, 0.0f, 1.0f) * (a1 - a0);
        c.disc(at.x, at.y, r, k.ground);
        c.arc(at, r - 1.0f, a0, a1, 1.5f, funkgui::premix(k.ground, k.value, 0.45f));
        if (a > a0)
            c.arc(at, r - 1.0f, a0, a, 2.0f, ink);
        c.segment(at.x, at.y, at.x + std::cos(a) * (r - 3.0f), at.y + std::sin(a) * (r - 3.0f), 1.6f, ink);
    }

    void placeKnobs(const View& v, const cir::Drawing& d, std::vector<KnobSpot>& knobs)
    {
        std::vector<funkgui::Rect> bodies, flags, posts, wiresPx;
        const float postR = std::max(2.0f, 0.3f * v.scale);
        const auto post = [&](cir::Pt at) {
            const funkgui::Point q = v.at(at);
            posts.push_back({ q.x - postR, q.y - postR, 2.0f * postR, 2.0f * postR });
        };
        for (const cir::Part& p : d.parts)
        {
            bodies.push_back(v.rect(p.body).expanded(1.0f));
            for (const cir::PinAt& pin : p.pins)
                post(pin.at);
        }
        for (const cir::Flag& f : d.flags)
            flags.push_back(v.rect(f.box));
        for (const cir::Net& n : d.nets)
            if (n.style == cir::Style::wired && !n.unroutable)
            {
                for (const auto& [a, b] : runs(n.edges))
                {
                    const funkgui::Point p = v.at(a), q = v.at(b);
                    wiresPx.push_back({ std::min(p.x, q.x) - 1.0f, std::min(p.y, q.y) - 1.0f, std::fabs(q.x - p.x) + 2.0f, std::fabs(q.y - p.y) + 2.0f });
                }
                for (const cir::Pt& j : n.junctions)
                    post(j);
            }
        const auto meets = [](const funkgui::Rect& a, const funkgui::Rect& b) {
            return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
        };
        std::vector<funkgui::Rect> placed;
        constexpr float kGap = 4.0f;
        for (KnobSpot& k : knobs)
        {
            if (k.part < 0 || k.part >= static_cast<int>(d.parts.size()))
                continue;
            const funkgui::Rect b = v.rect(d.parts[static_cast<std::size_t>(k.part)].body);
            const float dx = k.hw + kGap, dy = k.hh + kGap;
            const funkgui::Point below{ b.centreX(), b.bottom() + dy }, above{ b.centreX(), b.y - dy };
            const funkgui::Point right{ b.right() + dx, b.centreY() }, left{ b.x - dx, b.centreY() };
            const funkgui::Point br{ b.right() + dx, b.bottom() + dy }, bl{ b.x - dx, b.bottom() + dy };
            const funkgui::Point tr{ b.right() + dx, b.y - dy }, tl{ b.x - dx, b.y - dy };
            const std::vector<funkgui::Point> cands = b.w >= b.h ? std::vector<funkgui::Point>{ below, above, br, bl, tr, tl, right, left }
                                                                 : std::vector<funkgui::Point>{ left, right, tl, bl, tr, br, below, above };
            float bestScore = -1.0f;
            funkgui::Rect bestBox{};
            for (const funkgui::Point& at : cands)
            {
                const funkgui::Rect r{ at.x - k.hw - 1.0f, at.y - k.hh - 1.0f, 2.0f * k.hw + 2.0f, 2.0f * k.hh + 2.0f };
                float score = 0.0f;
                for (std::size_t j = 0; j < bodies.size(); ++j)
                    score += static_cast<int>(j) != k.part && meets(r, bodies[j]) ? 100.0f : 0.0f;
                for (const funkgui::Rect& f : flags)
                    score += meets(r, f) ? 100.0f : 0.0f;
                for (const funkgui::Rect& o : placed)
                    score += meets(r, o) ? 100.0f : 0.0f;
                for (const funkgui::Rect& q : posts)
                    score += meets(r, q) ? 20.0f : 0.0f;
                for (const funkgui::Rect& w : wiresPx)
                    score += meets(r, w) ? 5.0f : 0.0f;
                if (bestScore < 0.0f || score < bestScore)
                {
                    bestScore = score;
                    k.at = at;
                    bestBox = r;
                }
                if (score == 0.0f)
                    break;
            }
            placed.push_back(bestBox);
        }
    }

    void chip(funkgui::Canvas& c, funkgui::Point at, float w, float h, const std::string& text, funkgui::Col ink, const Inks& k)
    {
        c.rrect(at.x - 0.5f * w, at.y - 0.5f * h, w, h, 0.5f * h, k.ground, 1.0f, ink);
        const funkgui::TextStyle st = labelStyle(9.0f);
        c.text(text.c_str(), at.x, c.capCentreTop(at.y, st), st, ink, funkgui::Align::centre);
    }

    // ---- the probes ---------------------------------------------------------------------------------------------------
    void trace(funkgui::Canvas& c, const funkgui::Rect& r, const float* volts, int n, float centre, float range, const Inks& k,
               int span)
    {
        c.rrect(r.x, r.y, r.w, r.h, 3.0f, k.ground, 1.0f, k.grid);
        c.hairlineH(r.x + 2.0f, r.centreY(), r.w - 4.0f, k.faint);
        c.hairlineH(r.x + 2.0f, r.y + 0.25f * r.h, r.w - 4.0f, k.grid);
        c.hairlineH(r.x + 2.0f, r.y + 0.75f * r.h, r.w - 4.0f, k.grid);
        for (int i = 1; i < 4; ++i)
            c.hairlineV(r.x + r.w * static_cast<float>(i) / 4.0f, r.y + 2.0f, r.h - 4.0f, k.grid);
        if (n < 2 || volts == nullptr || !(range > 0.0f))
            return;
        constexpr int kBuckets = 256;
        float xs[2 * kBuckets], ys[2 * kBuckets];
        const int width = std::max(span, n);             // the samples the width stands for
        const float half = 0.5f * r.h - 2.0f;
        int m = 0;
        const auto point = [&](int i) {
            const float y = std::clamp((volts[static_cast<std::size_t>(i)] - centre) / range, -1.0f, 1.0f);
            xs[m] = r.x + 2.0f + (r.w - 4.0f) * static_cast<float>(i) / static_cast<float>(width - 1);
            ys[m] = r.centreY() - y * half;
            ++m;
        };
        if (n <= kBuckets)
            for (int i = 0; i < n; ++i)
                point(i);
        else
            for (int b = 0; b < kBuckets; ++b)
            {
                // the bucket's least and greatest, each at its own sample
                const int from = static_cast<int>(static_cast<int64_t>(b) * n / kBuckets);
                const int to = static_cast<int>(static_cast<int64_t>(b + 1) * n / kBuckets);
                int lo = from, hi = from;
                for (int i = from + 1; i < to; ++i)
                {
                    if (volts[static_cast<std::size_t>(i)] < volts[static_cast<std::size_t>(lo)])
                        lo = i;
                    if (volts[static_cast<std::size_t>(i)] > volts[static_cast<std::size_t>(hi)])
                        hi = i;
                }
                point(std::min(lo, hi));
                if (lo != hi)
                    point(std::max(lo, hi));
            }
        const funkgui::Canvas::ClipScope clip(c, r);
        c.polyline(xs, ys, m, 1.3f, k.symbol);
    }

    float traceRange(float span) noexcept
    {
        float r = 0.125f;
        while (r < 16.0f && !(std::fabs(span) <= r))
            r *= 2.0f;
        return r;
    }

    void probeFlag(funkgui::Canvas& c, funkgui::Point at, int number, const Inks& k)
    {
        const funkgui::TextStyle st = labelStyle(9.0f);
        const std::string t = std::to_string(number);
        c.rrect(at.x - 6.0f, at.y - 6.0f, 12.0f, 12.0f, 3.0f, k.ground, 1.2f, k.symbol);
        c.text(t.c_str(), at.x, c.capCentreTop(at.y, st), st, k.symbol, funkgui::Align::centre);
    }

    int risingCrossing(const float* v, int from, int to, float level) noexcept
    {
        for (int i = std::max(from, 1); i < to; ++i)
            if (v[i - 1] < level && level <= v[i])
                return i;
        return -1;
    }

    funkgui::Rect valueRect(const LabelSpot& s, const std::string& name, const std::string& value, float px)
    {
        if (px <= 0.0f || !s.shown || value.empty())
            return {};
        const funkgui::TextStyle st = labelStyle(px);
        const auto& atlas = funkgui::FontService::get().atlas();
        const float line = std::round(px * 1.2f);
        const auto left = [&](float w) { return s.align == funkgui::Align::centre ? s.x - 0.5f * w : s.align == funkgui::Align::right ? s.x - w : s.x; };
        if (s.oneLine)
        {
            const float wn = funkgui::text::width(atlas, (name + " ").c_str(), st), w = funkgui::text::width(atlas, (name + " " + value).c_str(), st);
            return { left(w) + wn, s.top - 1.0f, w - wn, line + 1.0f };
        }
        const float wv = funkgui::text::width(atlas, value.c_str(), st);
        return { left(wv), s.top + line - 1.0f, wv, line + 1.0f };
    }

    // ---- the problems on the drawing ----------------------------------------------------------------------------------
    namespace
    {
        std::string lowered(std::string s)
        {
            for (char& ch : s)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }
    }

    std::vector<Mark> mapMarks(const cir::Netlist& nl, const cir::Drawing& d, const std::vector<Diagnostic>& problems)
    {
        std::vector<Mark> marks;
        const auto partOfElement = [&](int element) {
            for (std::size_t i = 0; i < d.parts.size(); ++i)
                if (d.parts[i].element == element)
                    return static_cast<int>(i);
            return -1;
        };
        const auto terminal = [&](cir::Kind k) {
            for (std::size_t i = 0; i < d.parts.size(); ++i)
                if (d.parts[i].kind == k)
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
            for (std::size_t e = 0; e < nl.elements.size(); ++e)
                if (nl.elements[e].firstLine <= m.line && m.line <= nl.elements[e].lastLine)
                    if (const int p = partOfElement(static_cast<int>(e)); p >= 0)
                    {
                        m.parts.push_back(p);
                        // ... and the pin, when the column falls on a node token
                        const cir::Element& el = nl.elements[e];
                        for (std::size_t k = 0; k < el.nodes.size() && k < el.tokenOf.size(); ++k)
                        {
                            const cir::Token& t = el.nodes[k];
                            if (t.at.line != m.line || m.col < t.at.col || m.col >= t.at.col + std::max(1, t.len))
                                continue;
                            const std::vector<cir::PinAt>& pins = d.parts[static_cast<std::size_t>(p)].pins;
                            for (std::size_t j = 0; j < pins.size() && j < 8; ++j)
                                if (pins[j].token == el.tokenOf[k])
                                    m.pins.push_back({ p, static_cast<int>(j) });
                        }
                    }
            // a .pot line: every part whose expression names it
            for (const cir::PotLine& pot : nl.pots)
                if (pot.line == m.line)
                    for (std::size_t e = 0; e < nl.elements.size(); ++e)
                        for (const cir::Token& t : nl.elements[e].tokens)
                        {
                            if (t.text.empty() || t.text.front() != '{')
                                continue;
                            const std::string body = lowered(t.text), word = lowered(pot.name);
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
            for (const auto& [model, line] : nl.models)
                if (line == m.line)
                    for (std::size_t e = 0; e < nl.elements.size(); ++e)
                    {
                        const cir::Element& el = nl.elements[e];
                        if (el.kind == cir::Kind::D && el.valueToken >= 0
                            && cir::folded(el.tokens[static_cast<std::size_t>(el.valueToken)].text) == cir::folded(model))
                            if (const int p = partOfElement(static_cast<int>(e)); p >= 0)
                                m.parts.push_back(p);
                    }
            // an .input or .output line: its terminal
            if (nl.input && nl.input->line == m.line)
                if (const int p = terminal(cir::Kind::Input); p >= 0)
                    m.parts.push_back(p);
            if (nl.output && nl.output->line == m.line)
                if (const int p = terminal(cir::Kind::Output); p >= 0)
                    m.parts.push_back(p);
            std::sort(m.parts.begin(), m.parts.end());
            m.parts.erase(std::unique(m.parts.begin(), m.parts.end()), m.parts.end());
            marks.push_back(std::move(m));
        }
        std::stable_sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) {
            return a.line != b.line ? a.line < b.line : a.col < b.col;
        });
        return marks;
    }

    std::vector<PartMarks> partMarksOf(const cir::Drawing& d, const std::vector<Mark>& marks)
    {
        std::vector<PartMarks> out(d.parts.size());
        for (const Mark& m : marks)
            for (const int p : m.parts)
                if (p >= 0 && p < static_cast<int>(out.size()))
                {
                    PartMarks& pm = out[static_cast<std::size_t>(p)];
                    (m.severity == 0 ? pm.errors : m.severity == 1 ? pm.warnings : pm.notes) += 1;
                    pm.outline = pm.outline || m.severity == 0;
                }
        for (const Mark& m : marks)
            for (const auto& [p, pin] : m.pins)
                if (p >= 0 && p < static_cast<int>(out.size()))
                    out[static_cast<std::size_t>(p)].pinMask[std::clamp(m.severity, 0, 2)] |= static_cast<uint8_t>(1u << pin);
        // a hand edit's overlap: both outlined
        for (const auto& [a, b] : d.overlaps)
            for (const std::string& item : { a, b })
            {
                std::string name = item;
                if (const std::size_t at = name.find("'s"); at != std::string::npos)
                    name.resize(at);
                if (const std::size_t dot = name.find('.'); dot != std::string::npos)
                    name.resize(dot);
                if (const int p = d.part(name); p >= 0)
                    out[static_cast<std::size_t>(p)].outline = true;
            }
        return out;
    }

    int still(funkgui::Canvas& c, const View& v, const cir::Netlist& nl, const cir::Drawing& d, const std::vector<PartMarks>& marks,
              const Inks& k, const funkgui::Rect& clip, float px)
    {
        int labelled = 0;
        const float sw = v.stroke();
        grid(c, v, clip, k.grid);
        for (const cir::Net& n : d.nets)
        {
            if (n.style != cir::Style::wired || n.unroutable || n.edges.empty())
                continue;
            wires(c, v, n, sw, k.wire);
            junctions(c, v, n, k.wire);
        }
        for (const cir::Net& n : d.nets)
            if (n.style == cir::Style::wired && !n.unroutable)       // a net drawn as tags at its pins says its name there
                for (const cir::Pt& at : n.labels)
                    tag(c, v, at, n.name, k.name, px);
        for (const cir::Flag& f : d.flags)
        {
            const cir::Net& n = d.nets[static_cast<std::size_t>(f.net)];
            const cir::Style style = n.unroutable && n.style == cir::Style::wired ? cir::Style::flag : n.style;
            flag(c, v, f, style, n.positive, n.name, sw, k.symbol, k.name, px);
        }
        std::vector<std::string> values;
        values.reserve(d.parts.size());
        for (const cir::Part& p : d.parts)
            values.push_back(valueText(nl, p));
        const std::vector<LabelSpot> spots = placeLabels(c, v, d, values, px);
        for (std::size_t i = 0; i < d.parts.size(); ++i)
        {
            const cir::Part& p = d.parts[i];
            const float x = static_cast<float>(p.origin.x), y = static_cast<float>(p.origin.y);
            if (p.element < 0)
                terminal(c, v, p, sw, k.symbol, k.name, px);
            else if (lodBox(v, p.kind))
                bodyBox(c, v, p.kind, p.orient, x, y, sw, k.symbol);
            else
                symbol(c, v, p.kind, p.orient, x, y, sw, k.symbol);
            if (p.element >= 0 && px > 0.0f && i < spots.size())
            {
                labels(c, spots[i], p.name, values[i], k.name, k.value, k.ground, px);
                labelled += spots[i].shown ? 1 : 0;
            }
        }
        for (std::size_t i = 0; i < d.parts.size() && i < marks.size(); ++i)
            sch::marks(c, v, d.parts[i], marks[i], k);
        return labelled;
    }
}
