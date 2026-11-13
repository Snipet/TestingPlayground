// Source/editor/TabStrip.cpp: see TabStrip.h.
#include "editor/TabStrip.h"

#include <algorithm>
#include <bit>
#include <cstddef>

namespace playground::ui
{
    using funkgui::Point;
    using funkgui::Rect;

    void TabStrip::layout(const Rect& bar, const std::vector<float>& widths, float fixedRight, float newTabW, float gap,
                          float areaBottom)
    {
        x_.clear();
        w_ = widths;
        float content = 0.0f;
        for (const float w : widths)
        {
            x_.push_back(content);
            content += w;
        }
        areaBottom_ = std::max(areaBottom, bar.bottom());
        const float plus = newTabW > 0.0f ? newTabW + gap : 0.0f;
        const float room = std::max(0.0f, bar.w - fixedRight - plus);
        overflow_ = content > room + 0.01f;
        float viewW = content;
        if (overflow_)
        {
            viewW = std::max(0.0f, room - kMenuW - gap);
            menu_ = { bar.x + viewW + gap, bar.y, kMenuW, bar.h };
        }
        else
            menu_ = {};
        view_ = { bar.x, bar.y, viewW, bar.h };
        const float after = overflow_ ? menu_.right() : view_.right();
        newTab_ = newTabW > 0.0f ? Rect{ after + gap, bar.y, newTabW, bar.h } : Rect{};
        scroll_.horizontal = true;
        scroll_.row = kNotchRow;
        scroll_.content = content;
        scroll_.view = viewW;
        scroll_.clamp();
    }

    Rect TabStrip::area() const noexcept { return barArea(); }

    Rect TabStrip::tab(int i) const
    {
        if (i < 0 || i >= count())
            return {};
        const auto k = static_cast<std::size_t>(i);
        return { view_.x - scroll_.offset + x_[k], view_.y, w_[k], view_.h };
    }

    Rect TabStrip::shown(int i) const
    {
        const Rect t = tab(i);
        const float l = std::max(t.x, view_.x), r = std::min(t.right(), view_.right());
        if (r <= l)
            return {};
        return { l, t.y, r - l, t.h };
    }

    Rect TabStrip::chevron(bool right) const
    {
        if (!overflow_)
            return {};
        if (right ? scroll_.offset >= scroll_.max() - 0.5f : scroll_.offset <= 0.5f)
            return {};
        return right ? Rect{ view_.right() - kChevronW, view_.y, kChevronW, view_.h } : Rect{ view_.x, view_.y, kChevronW, view_.h };
    }

    int TabStrip::tabAt(Point p) const
    {
        if (!view_.contains(p) || chevron(false).contains(p) || chevron(true).contains(p))
            return -1;
        for (int i = 0; i < count(); ++i)
            if (tab(i).contains(p))
                return i;
        return -1;
    }

    bool TabStrip::wheel(const funkgui::WheelEvent& e)
    {
        if (!overflow_)
            return false;
        // Either axis: a trackpad's sideways stroke, or a wheel's notch (as JUCE gave it: Scroll.h's rule).
        const float delta = e.dx != 0.0f ? e.dx : e.dy;
        return scroll_.wheel(delta, e.smooth);
    }

    void TabStrip::reveal(int i)
    {
        if (!overflow_ || i < 0 || i >= count())
            return;
        const auto k = static_cast<std::size_t>(i);
        // Clear of the chevrons that would cover its edges (none at the strip's ends).
        const float from = x_[k] - (i > 0 ? kChevronW : 0.0f);
        const float to = x_[k] + w_[k] + (i + 1 < count() ? kChevronW : 0.0f);
        scroll_.reveal(from, to);
    }

    bool TabStrip::step(bool right)
    {
        if (!overflow_)
            return false;
        // The first tab hidden that way (under the chevron counts as hidden), or the one after it when it is hidden
        // by less than half: a step always shows a tab more.
        const float left = scroll_.offset + kChevronW, rightEdge = scroll_.offset + scroll_.view - kChevronW;
        const auto at = [&](int i) { return x_[static_cast<std::size_t>(i)]; };
        const auto width = [&](int i) { return w_[static_cast<std::size_t>(i)]; };
        int pick = -1;
        if (right)
        {
            for (int i = 0; i < count() && pick < 0; ++i)
                if (at(i) + width(i) > rightEdge + 0.5f)
                    pick = (at(i) + width(i) - rightEdge < 0.5f * width(i) && i + 1 < count()) ? i + 1 : i;
        }
        else
            for (int i = count() - 1; i >= 0 && pick < 0; --i)
                if (at(i) < left - 0.5f)
                    pick = (left - at(i) < 0.5f * width(i) && i > 0) ? i - 1 : i;
        if (pick < 0)
            return false;
        reveal(pick);
        return true;
    }

    uint64_t TabStrip::key() const noexcept
    {
        uint64_t h = scroll_.key();
        h = h * 0x9E3779B97F4A7C15ull ^ (overflow_ ? 1u : 0u);
        h = h * 0x9E3779B97F4A7C15ull ^ std::bit_cast<uint32_t>(view_.w);
        return h;
    }
}
