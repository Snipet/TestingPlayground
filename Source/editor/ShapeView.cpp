// Source/editor/ShapeView.cpp: see ShapeView.h.
#include "editor/ShapeView.h"

#include "plugin/EditorPort.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/widgets/FocusRing.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace testingplayground::ui
{
    namespace
    {
        constexpr float kEps = 1e-3f;                    // the legacy kInteriorEps: interior points stay inside
        constexpr float kPointR = 4.0f, kMidR = 2.5f;
        constexpr int kCurveSamples = 129;              // 128 segments across the plot
        constexpr int kMenuDelete = 1, kMenuReset = 2;

        float dist2(funkgui::Point a, funkgui::Point b) noexcept
        {
            const float dx = a.x - b.x, dy = a.y - b.y;
            return dx * dx + dy * dy;
        }

        // y in [0, 1] as the bipolar modulation it is, with U+2212 for a minus (FunkGui's minus policy).
        void bipolar(float y, char* out, std::size_t n)
        {
            const float v = 2.0f * y - 1.0f;
            if (std::fabs(v) < 0.005f)
                std::snprintf(out, n, "0.00");
            else
                std::snprintf(out, n, "%s%.2f", v < 0.0f ? "\xE2\x88\x92" : "+", static_cast<double>(std::fabs(v)));
        }
    }

    ShapeView::ShapeView(EditorPort& port, funkgui::Rect bounds, uint32_t a11yId)
        : port_(port), bounds_(bounds), a11yId_(a11yId)
    {
        refresh();
    }

    // ---- geometry ---------------------------------------------------------------------------------------------------------
    funkgui::Rect ShapeView::plot() const noexcept
    {
        return { bounds_.x + kLabelW, bounds_.y + kCaptionH, bounds_.w - kLabelW, bounds_.h - kCaptionH };
    }

    float ShapeView::toX(float px) const noexcept
    {
        const funkgui::Rect r = plot();
        return std::clamp((px - r.x) / r.w, 0.0f, 1.0f);
    }

    float ShapeView::toY(float py) const noexcept
    {
        const funkgui::Rect r = plot();
        return std::clamp(1.0f - (py - r.y) / r.h, 0.0f, 1.0f);
    }

    funkgui::Point ShapeView::pointAt(int i) const
    {
        const funkgui::Rect r = plot();
        const ShapePoint& p = shape_.points[static_cast<std::size_t>(std::clamp(i, 0, shape_.count - 1))];
        return { r.x + p.x * r.w, r.y + (1.0f - p.y) * r.h };
    }

    funkgui::Point ShapeView::midpointAt(int i) const
    {
        const funkgui::Rect r = plot();
        const int k = std::clamp(i, 0, std::max(shape_.count - 2, 0));
        const ShapePoint& a = shape_.points[static_cast<std::size_t>(k)];
        const ShapePoint& b = shape_.points[static_cast<std::size_t>(k + 1)];
        const float x = 0.5f * (a.x + b.x);
        const float y = a.y + (b.y - a.y) * applyCurve(0.5f, a.curve);
        return { r.x + x * r.w, r.y + (1.0f - y) * r.h };
    }

    int ShapeView::pointNear(funkgui::Point p, float radius) const
    {
        int best = -1;
        float bestD2 = radius * radius;
        for (int i = 0; i < shape_.count; ++i)
            if (const float d2 = dist2(p, pointAt(i)); d2 < bestD2)
            {
                bestD2 = d2;
                best = i;
            }
        return best;
    }

    int ShapeView::midpointNear(funkgui::Point p, float radius) const
    {
        int best = -1;
        float bestD2 = radius * radius;
        for (int i = 0; i + 1 < shape_.count; ++i)
            if (const float d2 = dist2(p, midpointAt(i)); d2 < bestD2)
            {
                bestD2 = d2;
                best = i;
            }
        return best;
    }

    void ShapeView::hoverAt(funkgui::Point p)
    {
        hoverPoint_ = pointNear(p, kHitRadius);
        hoverMid_ = hoverPoint_ < 0 ? midpointNear(p, kHitRadius) : -1;   // a point wins over a handle
    }

    // ---- the shape ----------------------------------------------------------------------------------------------------------
    void ShapeView::refresh()
    {
        const uint64_t r = port_.shapeRevision();
        if (r == revision_)
            return;
        revision_ = r;
        shape_ = port_.shape();
        if (selected_ >= shape_.count)
            selected_ = -1;
        if (hoverPoint_ >= shape_.count)
            hoverPoint_ = -1;
        if (hoverMid_ >= shape_.count - 1)
            hoverMid_ = -1;
        if (dragIndex_ >= shape_.count)
        {
            drag_ = Drag::none;
            dragIndex_ = -1;
        }
    }

    void ShapeView::commit()
    {
        port_.setShape(shape_);
        edited_ = true;
        revision_ = 0;                                   // read back what the port kept (it sanitises)
        refresh();
    }

    void ShapeView::finish()
    {
        port_.endShapeEdit();
        edited_ = false;
    }

    void ShapeView::deletePoint(int i)
    {
        const int last = shape_.count - 1;
        if (i <= 0 || i >= last)
            return;                                      // the edge points stay
        auto* p = shape_.points.data();
        std::move(p + i + 1, p + shape_.count, p + i);
        shape_.points[static_cast<std::size_t>(last)] = ShapePoint{};
        --shape_.count;
        selected_ = -1;
        hoverPoint_ = -1;
        hoverMid_ = -1;
        commit();
        finish();
    }

    void ShapeView::insertPoint(funkgui::Point at)
    {
        if (shape_.count >= LfoShape::kMaxPoints || shape_.count < 2)
            return;
        const float x = std::clamp(toX(at.x), kEps, 1.0f - kEps), y = toY(at.y);
        int k = 1;
        while (k < shape_.count - 1 && shape_.points[static_cast<std::size_t>(k)].x <= x)
            ++k;
        auto* p = shape_.points.data();
        std::move_backward(p + k, p + shape_.count, p + shape_.count + 1);
        p[k] = { x, y, 0.0f };
        ++shape_.count;
        selected_ = k;
        hoverPoint_ = k;
        drag_ = Drag::point;                             // the button is still down: the drag carries the new point
        dragIndex_ = k;
        commit();
    }

    void ShapeView::movePoint(int i, float x, float y, bool keepBetween)
    {
        const int last = shape_.count - 1;
        auto* p = shape_.points.data();
        y = std::clamp(y, 0.0f, 1.0f);
        if (i == 0 || i == last)                         // the edges: x pinned, y shared (the cycle wraps there)
        {
            p[0].y = y;
            p[last].y = y;
            return;
        }
        x = std::clamp(x, kEps, 1.0f - kEps);
        if (keepBetween)
        {
            const float lo = p[i - 1].x + kEps, hi = p[i + 1].x - kEps;
            x = lo < hi ? std::clamp(x, lo, hi) : p[i].x;
        }
        p[i].x = x;
        p[i].y = y;
        while (i > 1 && p[i].x < p[i - 1].x)             // keep the order: the point (and its curve) moves past others
        {
            std::swap(p[i], p[i - 1]);
            --i;
        }
        while (i < last - 1 && p[i].x > p[i + 1].x)
        {
            std::swap(p[i], p[i + 1]);
            ++i;
        }
        dragIndex_ = i;
        selected_ = i;
    }

    // ---- frame --------------------------------------------------------------------------------------------------------------
    void ShapeView::tick(float, bool hovered, bool focused)
    {
        refresh();
        hovered_ = hovered;
        focused_ = focused;
        if (!hovered && drag_ == Drag::none)
        {
            hoverPoint_ = -1;
            hoverMid_ = -1;
        }
    }

    void ShapeView::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focusRing) const
    {
        namespace type = funkgui::type;
        const funkgui::Rect r = plot();

        {
            const funkgui::Canvas::Scope s(c, funkgui::tags::none, false);
            c.text("MODULATION", bounds_.x, bounds_.y, type::kCaption, th.ink52);
            c.text("LFO SHAPE, ONE CYCLE", bounds_.right(), bounds_.y + 1.0f, type::kMicro, th.ink32,
                   funkgui::Align::right);
            c.text("+1", r.x - 5.0f, c.capCentreTop(r.y, type::kMicro), type::kMicro, th.ink32, funkgui::Align::right);
            c.text("0", r.x - 5.0f, c.capCentreTop(r.centreY(), type::kMicro), type::kMicro, th.ink32,
                   funkgui::Align::right);
            c.text("\xE2\x88\x92" "1", r.x - 5.0f, c.capCentreTop(r.bottom(), type::kMicro), type::kMicro, th.ink32,
                   funkgui::Align::right);
        }

        // The well and the grid: eighths across (the snap), quarters and the zero line stronger.
        const funkgui::Col faint = funkgui::premix(th.ground, th.ink16, 0.5f);
        c.rrect(r.x, r.y, r.w, r.h, 2.0f, funkgui::premix(th.ground, th.ink16, 0.22f), 1.0f, th.ink16);
        for (int k = 1; k < kGrid; ++k)
        {
            const float x = r.x + r.w * static_cast<float>(k) / static_cast<float>(kGrid);
            c.hairlineV(x, r.y + 1.0f, r.h - 2.0f, (k % 2) == 0 ? th.ink16 : faint);
        }
        for (int k = 1; k < 4; ++k)
        {
            const float y = r.y + r.h * static_cast<float>(k) / 4.0f;
            c.hairlineH(r.x + 1.0f, y, r.w - 2.0f, k == 2 ? th.ink32 : faint);
        }

        // The curve and its fill to the zero line.
        if (shape_.count >= 2)
        {
            std::array<float, kCurveSamples> xs{}, ys{}, top{}, bottom{};
            const float zero = r.centreY();
            for (int k = 0; k < kCurveSamples; ++k)
            {
                const float t = static_cast<float>(k) / static_cast<float>(kCurveSamples - 1);
                const auto u = static_cast<std::size_t>(k);
                xs[u] = r.x + t * r.w;
                ys[u] = r.y + (1.0f - sampleCurve(shape_, t)) * r.h;
                top[u] = std::min(ys[u], zero);
                bottom[u] = std::max(ys[u], zero);
            }
            c.areaStrip(xs.data(), kCurveSamples, top.data(), bottom.data(), zero,
                        funkgui::premix(th.ground, th.signal, 0.16f));
            c.polyline(xs.data(), ys.data(), kCurveSamples, 1.5f, th.signal);
        }

        // The bend handles, then the points over them.
        for (int i = 0; i + 1 < shape_.count; ++i)
        {
            const funkgui::Point m = midpointAt(i);
            const bool hot = i == hoverMid_ || (drag_ == Drag::curve && i == dragIndex_);
            c.disc(m.x, m.y, hot ? kMidR + 1.0f : kMidR, hot ? th.ink100 : th.ink52, 1.0f, th.ground);
        }
        for (int i = 0; i < shape_.count; ++i)
        {
            const funkgui::Point p = pointAt(i);
            const bool sel = i == selected_, hot = i == hoverPoint_;
            const funkgui::Col fill = sel ? th.accent : (hot ? th.ink100 : th.ink70);
            c.disc(p.x, p.y, hot || sel ? kPointR + 1.0f : kPointR, fill, 1.5f, th.ground);
        }

        if (focusRing)
            funkgui::drawFocusRing(c, r.expanded(3.0f), th.accent);
    }

    bool ShapeView::contains(funkgui::Point p) const
    {
        return plot().expanded(kPointR + 2.0f).contains(p);
    }

    funkgui::Cursor ShapeView::cursorAt(funkgui::Point p) const
    {
        if (drag_ == Drag::point)
            return funkgui::Cursor::grabbing;
        if (drag_ == Drag::curve)
            return funkgui::Cursor::upDown;
        if (pointNear(p, kHitRadius) >= 0)
            return funkgui::Cursor::grab;
        if (midpointNear(p, kHitRadius) >= 0)
            return funkgui::Cursor::upDown;
        return plot().contains(p) ? funkgui::Cursor::crosshair : funkgui::Cursor::normal;
    }

    // ---- input --------------------------------------------------------------------------------------------------------------
    void ShapeView::pointerMove(const funkgui::PointerEvent& e)
    {
        refresh();
        hoverAt({ e.x, e.y });
    }

    void ShapeView::pointerExit()
    {
        hoverPoint_ = -1;
        hoverMid_ = -1;
    }

    void ShapeView::pointerDown(const funkgui::PointerEvent& e, funkgui::HostServices& host,
                                const funkgui::Theme& menuTheme)
    {
        refresh();
        hoverAt({ e.x, e.y });
        if (e.popup)
        {
            const int last = shape_.count - 1;
            const int target = hoverPoint_ > 0 && hoverPoint_ < last ? hoverPoint_ : -1;
            funkgui::MenuRequest req;
            req.items = { funkgui::MenuItem{ .id = kMenuDelete, .label = "Delete point", .enabled = target > 0 },
                          funkgui::MenuItem{ .separator = true },
                          funkgui::MenuItem{ .id = kMenuReset, .label = "Reset shape" } };
            req.anchor = { e.x, e.y, 1.0f, 1.0f };
            req.theme = menuTheme;
            host.showMenu(req, [this, target](int id) {
                refresh();
                if (id == kMenuDelete && target > 0 && target < shape_.count - 1)
                    deletePoint(target);
                else if (id == kMenuReset)
                {
                    shape_ = defaultShape();
                    selected_ = -1;
                    commit();
                    finish();
                }
            });
            return;
        }
        if (e.clicks >= 2)
            return;                                      // doubleClick() follows
        edited_ = false;
        if (hoverPoint_ >= 0)
        {
            selected_ = hoverPoint_;
            drag_ = Drag::point;
            dragIndex_ = hoverPoint_;
        }
        else if (hoverMid_ >= 0)
        {
            drag_ = Drag::curve;
            dragIndex_ = hoverMid_;
        }
        else
        {
            selected_ = -1;
        }
    }

    void ShapeView::pointerDrag(const funkgui::PointerEvent& e)
    {
        if (drag_ == Drag::point && dragIndex_ >= 0 && dragIndex_ < shape_.count)
        {
            float x = toX(e.x), y = toY(e.y);
            if (e.mods.alt)
            {
                const auto n = static_cast<float>(kGrid);
                x = std::round(x * n) / n;
                y = std::round(y * n) / n;
            }
            movePoint(dragIndex_, x, y, e.mods.cmd);
            commit();
        }
        else if (drag_ == Drag::curve && dragIndex_ >= 0 && dragIndex_ + 1 < shape_.count)
        {
            ShapePoint& a = shape_.points[static_cast<std::size_t>(dragIndex_)];
            const ShapePoint& b = shape_.points[static_cast<std::size_t>(dragIndex_ + 1)];
            const float dy = b.y - a.y;
            if (std::fabs(dy) < 1e-4f)
                return;                                  // a level segment cannot bend (the legacy's rule)
            a.curve = curveThrough((toY(e.y) - a.y) / dy);
            commit();
        }
    }

    void ShapeView::pointerUp(const funkgui::PointerEvent&)
    {
        if (edited_)
            finish();
        drag_ = Drag::none;
        dragIndex_ = -1;
    }

    void ShapeView::doubleClick(const funkgui::PointerEvent& e)
    {
        refresh();
        const funkgui::Point at{ e.x, e.y };
        hoverAt(at);
        if (hoverPoint_ >= 0)
        {
            deletePoint(hoverPoint_);                    // the edge points refuse
            drag_ = Drag::none;
            dragIndex_ = -1;
            return;
        }
        if (plot().contains(at))
            insertPoint(at);
    }

    bool ShapeView::key(const funkgui::KeyEvent& e)
    {
        refresh();
        const int last = shape_.count - 1;
        switch (e.key)
        {
            case funkgui::Key::del:
            case funkgui::Key::backspace:
                if (selected_ <= 0 || selected_ >= last)
                    return false;
                deletePoint(selected_);
                return true;
            case funkgui::Key::up:
            case funkgui::Key::down:
            case funkgui::Key::left:
            case funkgui::Key::right:
            {
                if (selected_ < 0)
                {
                    selected_ = 0;
                    return true;
                }
                const float step = e.mods.shift ? 1.0f / 256.0f : 1.0f / 64.0f;
                const ShapePoint p = shape_.points[static_cast<std::size_t>(selected_)];
                const float dx = e.key == funkgui::Key::left ? -step : (e.key == funkgui::Key::right ? step : 0.0f);
                const float dy = e.key == funkgui::Key::down ? -step : (e.key == funkgui::Key::up ? step : 0.0f);
                movePoint(selected_, p.x + dx, p.y + dy, true);
                commit();
                finish();
                return true;
            }
            case funkgui::Key::escape:
                if (selected_ < 0)
                    return false;
                selected_ = -1;
                return true;
            case funkgui::Key::character:
            case funkgui::Key::tab:
            case funkgui::Key::pageUp:
            case funkgui::Key::pageDown:
            case funkgui::Key::home:
            case funkgui::Key::end:
            case funkgui::Key::enter:
            case funkgui::Key::space:
                return false;
        }
        return false;
    }

    // ---- accessibility and the spec line --------------------------------------------------------------------------------
    void ShapeView::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        funkgui::A11yItem item;
        item.id = a11yId_;
        item.role = funkgui::A11yRole::image;
        item.bounds = plot();
        item.title = "LFO shape";
        char value[96];
        if (selected_ >= 0 && selected_ < shape_.count)
        {
            const ShapePoint& p = shape_.points[static_cast<std::size_t>(selected_)];
            std::snprintf(value, sizeof value, "%d points, point %d selected at %.0f percent of the cycle, %+.2f",
                          shape_.count, selected_ + 1, static_cast<double>(p.x * 100.0f),
                          static_cast<double>(2.0f * p.y - 1.0f));
        }
        else
        {
            std::snprintf(value, sizeof value, "%d points", shape_.count);
        }
        item.value = value;
        item.help = "Double-click adds or removes a point. Drag a point, or a segment's middle handle to bend it. "
                    "Arrow keys move the selected point; Delete removes it.";
        out.push_back(std::move(item));
    }

    bool ShapeView::a11yAction(uint32_t id, funkgui::A11yAction, double)
    {
        return id == a11yId_;
    }

    void ShapeView::specLine(char* out, std::size_t n) const
    {
        if (n == 0)
            return;
        if (selected_ >= 0 && selected_ < shape_.count)
        {
            const ShapePoint& p = shape_.points[static_cast<std::size_t>(selected_)];
            char y[16];
            bipolar(p.y, y, sizeof y);
            std::snprintf(out, n, "SHAPE   %d POINTS   POINT %d   X %.3f   Y %s", shape_.count, selected_ + 1,
                          static_cast<double>(p.x), y);
        }
        else
        {
            std::snprintf(out, n, "SHAPE   %d POINTS   DOUBLE-CLICK ADDS A POINT   RIGHT-CLICK FOR THE MENU",
                          shape_.count);
        }
    }
}
