// Source/editor/SchematicDraw.cpp: see SchematicDraw.h.
#include "editor/SchematicDraw.h"

#include <algorithm>
#include <array>
#include <cmath>
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
                                       const std::vector<std::string>& values, float px)
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
}
