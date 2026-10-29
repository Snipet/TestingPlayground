// Source/sandbox/View.cpp: see View.h.
#include "sandbox/View.h"

#include "sandbox/SandboxProtocol.h"

#include "editor/ChatStyle.h"
#include "editor/DisplayItem.h"
#include "editor/Theme.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Theme.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace playground::sandbox
{
    namespace
    {
        constexpr uint32_t kA11yStride = 64;     // the Panel's: item i's ids are kA11yStride * (i + 1) + 0..63

        std::vector<std::unique_ptr<MockPort>> makePorts()
        {
            std::vector<std::unique_ptr<MockPort>> out;
            for (int c = 0; c < kMaxControls; ++c)
                out.push_back(std::make_unique<MockPort>(c));
            return out;
        }

        std::vector<funkgui::ParamPort*> listOf(const std::vector<std::unique_ptr<MockPort>>& ports)
        {
            std::vector<funkgui::ParamPort*> out;
            for (const auto& p : ports)
                out.push_back(p.get());
            return out;
        }

        // The displays first, as the Panel makes them before the interface's items.
        std::vector<int> makeDisplays(ui::GuiView::Items& items)
        {
            std::vector<int> out;
            for (int d = 0; d < kMaxDisplays; ++d)
            {
                items.push_back(std::make_unique<ui::DisplayItem>(kA11yStride * static_cast<uint32_t>(items.size() + 1)));
                out.push_back(static_cast<int>(items.size()) - 1);
            }
            return out;
        }

        ui::GuiView::Hooks hooks()
        {
            ui::GuiView::Hooks h;
            h.menuTheme = [] { return ui::productTheme(funkgui::Theme::graphite()); };
            h.choosePage = [](int, int) {};
            h.playNote = [](int, float) {};
            h.playWheel = [](bool, float) {};
            return h;
        }

        int stepsOf(const Control& c)
        {
            return c.type == ControlType::real ? 0 : std::max(1, static_cast<int>(std::lround(c.max - c.min)));
        }
    }

    void demoScope(bool input, float rate, float* out, int n)
    {
        const float k = 2.0f * std::numbers::pi_v<float> / (rate > 0.0f ? rate : 48000.0f);
        const float gain = input ? 1.15f : 1.0f;
        uint32_t noise = 0x9E3779B9u;
        for (int i = 0; i < n; ++i)
        {
            noise = noise * 1664525u + 1013904223u;
            const float t = static_cast<float>(i);
            const float x = 0.140f * std::sin(k * 220.0f * t) + 0.050f * std::sin(k * 440.0f * t + 0.7f)
                          + 0.030f * std::sin(k * 660.0f * t + 1.3f) + 0.012f * std::sin(k * 1760.0f * t + 0.4f)
                          + 0.002f * (static_cast<float>(noise >> 8) / 8388608.0f - 1.0f);
            out[i] = gain * x;
        }
    }

    View::View(const PanelSpec& spec, const Controls& controls, const std::array<float, kMaxControls>& values01,
               const Sources& files, const std::string& notes, int width, int height, const std::string& page, bool instrument)
        : spec_(spec), controls_(controls), instrument_(instrument), width_(width), height_(height), ports_(makePorts()),
          portList_(listOf(ports_)), displays_(makeDisplays(items_)),
          view_(items_, portList_, displays_, [this] { return kA11yStride * static_cast<uint32_t>(items_.size() + 1); }, hooks())
    {
        for (int c = 0; c < kMaxControls; ++c)
        {
            const Control& k = controls_[static_cast<std::size_t>(c)];
            ports_[static_cast<std::size_t>(c)]->set(values01[static_cast<std::size_t>(c)], normOf(k, k.def), stepsOf(k));
        }
        view_.setControls(controls_, spec_);
        view_.registerShaders(spec_, files, notes);
        // The page asked for: the first tabs element with a page of that title (as view_panel's picture finds it).
        const std::vector<GuiNode>& nodes = spec_.gui.nodes;
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].kind != GuiKind::tabs)
                continue;
            std::string these;
            for (std::size_t k = 0; k < nodes[i].children.size(); ++k)
            {
                const std::string& title = nodes[static_cast<std::size_t>(nodes[i].children[k])].text;
                these += (these.empty() ? "" : " / ") + title;
                if (!page.empty() && shownPage_.empty() && title == page)
                {
                    view_.choosePage(spec_, static_cast<int>(i), static_cast<int>(k));
                    shownPage_ = title;
                }
            }
            allPages_ += (allPages_.empty() ? "" : "; ") + these;
        }
        view_.layout(spec_, controls_, funkgui::Rect{ 0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_) });
        demoMeters();
    }

    void View::demoMeters()
    {
        ++meters_.frame;
        meters_.rate = 48000.0f;
        meters_.program = 1;
        // The audio's levels, as the Engine measures them for the demo signal (demoScope).
        meters_.outPeak[0] = 0.22f;
        meters_.outPeak[1] = 0.21f;
        meters_.outRms[0] = 0.108f;
        meters_.outRms[1] = 0.104f;
        if (!instrument_)
        {
            meters_.inPeak[0] = 0.25f;
            meters_.inPeak[1] = 0.24f;
            meters_.inRms[0] = 0.124f;
            meters_.inRms[1] = 0.120f;
        }
        // A field moves slowly across its range (a bool blinks, an int steps); a live= field sits a little under its
        // knob, as a modulated cutoff does.
        const float phase = 2.0f * std::numbers::pi_v<float> * static_cast<float>(meters_.frame) / 96.0f;
        for (std::size_t d = 0; d < spec_.displays.size(); ++d)
        {
            const PanelDisplay& x = spec_.displays[d];
            if (x.tap < 0 || x.tap >= kMaxDisplays || x.hidden)
                continue;
            const float t = 0.55f + 0.3f * std::sin(phase + 1.7f * static_cast<float>(d));
            meters_.taps[x.tap] = x.lo + t * (x.hi - x.lo);
        }
        for (const GuiNode& n : spec_.gui.nodes)
        {
            if (n.live < 0 || n.slot < 0 || n.slot >= kMaxControls || static_cast<std::size_t>(n.live) >= spec_.displays.size())
                continue;
            const int tap = spec_.displays[static_cast<std::size_t>(n.live)].tap;
            if (tap < 0 || tap >= kMaxDisplays)
                continue;
            const Control& c = controls_[static_cast<std::size_t>(n.slot)];
            const float v = ports_[static_cast<std::size_t>(n.slot)]->value01();
            meters_.taps[tap] = plainOf(c, std::clamp(v - 0.14f + 0.04f * std::sin(0.5f * phase), 0.0f, 1.0f));
        }
    }

    void View::tick(float dt)
    {
        demoMeters();
        ui::updateDisplays(items_, displays_, meters_, [this](bool input, float* out, int n) { demoScope(input, meters_.rate, out, n); },
                           scopeIn_, scopeOut_, dt);
        view_.tick(spec_, meters_, true);
        const funkgui::Point far{ -1.0e4f, -1.0e4f };
        for (const auto& item : items_)
            item->tick(dt, false, false, far);
        dirty_ = false;
    }

    // As Panel::draw draws the PANEL view's area: the program's card under it, then the interface's ground, frames,
    // labels and dividers, its items in their theme (clipped to the area), and its scroll bars.
    void View::draw(funkgui::Canvas& c, const funkgui::Theme& hostTheme)
    {
        const funkgui::Theme th = ui::productTheme(hostTheme);
        const ui::chat::Palette pal = ui::chat::palette(th);
        c.rrect(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()), 0.0f, pal.card);
        const funkgui::Theme gth = view_.theme(th, spec_);
        view_.drawGround(c, gth, spec_, funkgui::tags::none);
        const funkgui::Rect& area = view_.places().area;
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            if (!items_[i]->shown() || !view_.owns(i))
                continue;
            const funkgui::Canvas::ClipScope clip(c, area);
            items_[i]->draw(c, gth, false);
        }
        view_.drawScrollBars(c, th);
    }

    int View::pictureWidth() const noexcept { return static_cast<int>(std::ceil(static_cast<float>(width_) * kDpi)); }
    int View::pictureHeight() const noexcept { return static_cast<int>(std::ceil(static_cast<float>(height_) * kDpi)); }

    bool View::wantsFullRate() const
    {
        return dirty_ || std::any_of(items_.begin(), items_.end(), [](const auto& it) { return !it->settled(); });
    }
}
