// Source/editor/Scroll.cpp: see Scroll.h.
#include "editor/Scroll.h"

#include <algorithm>
#include <bit>
#include <initializer_list>

namespace playground::ui
{
    using funkgui::Point;
    using funkgui::Rect;

    void Scroll::clamp() noexcept { offset = std::clamp(offset, 0.0f, max()); }

    bool Scroll::wheel(float delta, bool smooth) noexcept
    {
        if (!shown())
            return false;
        offset -= funkgui::scroll::wheelPx(delta, smooth, row);
        clamp();
        return true;
    }

    void Scroll::reveal(float from, float to) noexcept
    {
        if (from < offset)
            offset = from;
        else if (to > offset + view)
            offset = to - view;
        clamp();
    }

    bool Scroll::moveTo(float to) noexcept
    {
        const float was = offset;
        offset = to;
        clamp();
        return offset != was;
    }

    Rect Scroll::track(const Rect& a) const noexcept
    {
        using namespace funkgui::scroll;
        return horizontal ? Rect{ a.x, a.bottom() - kBarInset - kBarW, a.w, kBarW }
                          : Rect{ a.right() - kBarInset - kBarW, a.y, kBarW, a.h };
    }

    Rect Scroll::thumb(const Rect& a) const noexcept
    {
        const Rect t = track(a);
        const funkgui::scroll::Thumb th = funkgui::scroll::thumb(horizontal ? t.w : t.h, content, view, offset);
        return horizontal ? Rect{ t.x + th.pos, t.y, th.size, t.h } : Rect{ t.x, t.y + th.pos, t.w, th.size };
    }

    bool Scroll::hits(Point p, const Rect& a) const noexcept { return shown() && track(a).expanded(kBarHit).contains(p); }

    bool Scroll::press(Point p, const Rect& a) noexcept
    {
        if (!hits(p, a))
            return false;
        const Rect th = thumb(a);
        const float start = horizontal ? th.x : th.y, size = horizontal ? th.w : th.h;
        if (along(p) >= start && along(p) < start + size)
            grab_ = along(p) - start;
        else
        {
            grab_ = size * 0.5f;                     // a press on the track: the thumb jumps under the pointer
            drag(p, a);
        }
        return true;
    }

    void Scroll::drag(Point p, const Rect& a) noexcept
    {
        if (!dragging())
            return;
        const Rect t = track(a), th = thumb(a);
        const float len = horizontal ? t.w : t.h, size = horizontal ? th.w : th.h;
        const float room = len - size;
        offset = room > 0.0f ? (along(p) - grab_ - (horizontal ? t.x : t.y)) / room * max() : 0.0f;
        clamp();
    }

    void Scroll::draw(funkgui::Canvas& c, const Rect& a, const funkgui::Theme& th) const
    {
        if (!shown())
            return;
        const Rect r = thumb(a);
        c.rrect(r.x, r.y, r.w, r.h, funkgui::scroll::kBarW * 0.5f, dragging() ? th.ink52 : th.ink32);
    }

    uint64_t Scroll::key() const noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (const float v : { offset, content, view })
        {
            h ^= std::bit_cast<uint32_t>(v);
            h *= 1099511628211ull;
        }
        h ^= dragging() ? 1u : 0u;
        h *= 1099511628211ull;
        return h;
    }
}
