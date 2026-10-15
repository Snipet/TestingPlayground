// Source/editor/EnvelopeView.cpp: see EnvelopeView.h.
#include "editor/EnvelopeView.h"

#include "editor/ParamModel.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace testingplaygroundsynth::ui
{
    namespace
    {
        constexpr const char* kIds[4] = { "attack", "decay", "sustain", "release" };
        constexpr float kHeadroom = 4.0f;                      // above the peak, so the stroke is not cut
        constexpr float kStroke = 1.5f;
        constexpr float kHighlight = 2.0f;
    }

    std::optional<EnvelopeView> EnvelopeView::make(std::span<funkgui::ParamPort* const> ports, funkgui::Rect plot)
    {
        EnvelopeView v;
        v.plot_ = plot;
        for (std::size_t k = 0; k < 4; ++k)
        {
            bool found = false;
            for (std::size_t i = 0; i < kParams.size() && i < ports.size(); ++i)
                if (std::strcmp(kParams[i].id, kIds[k]) == 0 && ports[i] != nullptr)
                {
                    v.param_[k] = i;
                    v.port_[k] = ports[i];
                    v.last01_[k] = ports[i]->default01();
                    found = true;
                }
            if (!found)
                return std::nullopt;
        }
        v.maxMs_ = std::max({ kParams[v.param_[attack]].max, kParams[v.param_[decay]].max,
                              kParams[v.param_[release]].max, 1.0f });
        return v;
    }

    float EnvelopeView::plain(Seg s) const
    {
        return fromNormalised(kParams[param_[s]], shownValue01(port_[s]->value01(), last01_[s]));
    }

    bool EnvelopeView::shows(std::size_t param) const noexcept
    {
        return std::find(param_.begin(), param_.end(), param) != param_.end();
    }

    std::array<funkgui::Point, 5> EnvelopeView::corners() const
    {
        const auto width = [this](float ms) {
            return kSegment * plot_.w * std::sqrt(std::clamp(ms, 0.0f, maxMs_) / maxMs_);
        };
        const float top = plot_.y + kHeadroom, bottom = plot_.bottom();
        const float held = std::clamp(plain(Seg::sustain) / 100.0f, 0.0f, 1.0f);
        const float ySustain = bottom - held * (bottom - top);
        const float x0 = plot_.x, x4 = plot_.right();
        const float x1 = x0 + width(plain(Seg::attack));
        const float x2 = x1 + width(plain(Seg::decay));
        const float x3 = x4 - width(plain(Seg::release));
        return { { { x0, bottom }, { x1, top }, { x2, ySustain }, { x3, ySustain }, { x4, bottom } } };
    }

    void EnvelopeView::draw(funkgui::Canvas& c, const funkgui::Theme& th, int highlight) const
    {
        const funkgui::Canvas::Scope scope(c, kEnvelopeTag, false);
        const std::array<funkgui::Point, 5> p = corners();
        float xs[5], ys[5];
        for (std::size_t i = 0; i < 5; ++i)
        {
            xs[i] = p[i].x;
            ys[i] = p[i].y;
        }
        c.hairlineH(plot_.x, plot_.bottom(), plot_.w, th.ink16);
        c.areaStrip(xs, 5, ys, nullptr, plot_.bottom(), funkgui::premix(th.ground, th.ink16, 0.6f), kStroke, th.ink70);

        // The segment of the control under the hand: attack p0-p1, decay p1-p2, sustain p2-p3, release p3-p4.
        for (std::size_t k = 0; k < 4; ++k)
            if (highlight >= 0 && param_[k] == static_cast<std::size_t>(highlight))
            {
                const float hx[2] = { p[k].x, p[k + 1].x }, hy[2] = { p[k].y, p[k + 1].y };
                c.polyline(hx, hy, 2, kHighlight, th.ink100);
            }
    }

    void EnvelopeView::accessibility(std::vector<funkgui::A11yItem>& out, uint32_t id) const
    {
        std::string value;
        for (std::size_t k = 0; k < 4; ++k)
        {
            const ParamSpec& s = kParams[param_[k]];
            funkgui::ValueText t;
            ParamModel::format(s, plain(static_cast<Seg>(k)), t);
            value += (k == 0 ? "" : ", ") + std::string(s.name) + " " + t.spoken;
        }
        funkgui::A11yItem item;
        item.id = id;
        item.role = funkgui::A11yRole::image;
        item.bounds = plot_;
        item.title = "Envelope";
        item.value = value;
        item.readOnly = true;
        out.push_back(item);
    }
}
