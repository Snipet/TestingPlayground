// Source/editor/PreviewRows.cpp: preview::drawRows (Preview.h): a program with no .gui file, its dials in plain rows or
// in its #pragma panel lines' sections, as the PANEL view lays them out (DialLayout.h's layoutDials, the PANEL view's
// two passes) and draws them (Looks.h, FunkGui's Dial for a plain knob, DisplayItem for a display over the GUI
// sandbox's demo meters). A static copy of Panel.cpp's drawing of the dials (its DialItem, its ControlModel and its
// section frames): gui.preview ROWS holds it to view_panel's picture, cell by cell, so that the two cannot drift.
#include "editor/Preview.h"

#include "editor/ChatStyle.h"
#include "editor/DialLayout.h"
#include "editor/DisplayItem.h"
#include "editor/GuiView.h"
#include "editor/Items.h"
#include "editor/Looks.h"
#include "editor/PanelView.h"
#include "editor/Picture.h"
#include "editor/Theme.h"
#include "sandbox/SandboxProtocol.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/Dial.h>
#include <funkgui/widgets/ValueModel.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>

namespace playground::preview
{
    namespace
    {
        constexpr funkgui::TextStyle kSectionTitle{ 10.0f, 2.4f, 0.10f, false };   // Panel.cpp's
        constexpr uint32_t kA11yStride = 64;

        // Panel.cpp's ControlModel, over a mock port: a dial's label, value text and values' names.
        class RowModel final : public funkgui::ValueModel
        {
        public:
            RowModel(funkgui::ParamPort& port, const Control& c, const std::string& label, const std::vector<std::string>& options)
                : port_(port), control_(c)
            {
                for (const std::string& o : options)
                    options_.push_back(ui::upper(ui::asciiText(o)));
                const std::string name = ui::upper(ui::asciiText(label.empty() ? std::string(c.name) : label));
                std::snprintf(label_, sizeof label_, "%s", name.c_str());
            }
            const char* label() const noexcept { return label_; }
            int count() const
            {
                if (control_.type == ControlType::toggle)
                    return 2;
                if (control_.type == ControlType::integer)
                    return static_cast<int>(std::lround(control_.max) - std::lround(control_.min)) + 1;
                return 0;
            }
            int index() const { return static_cast<int>(std::lround(plainOf(control_, value01())) - std::lround(control_.min)); }
            std::string nameOf(int k) const
            {
                if (k >= 0 && static_cast<std::size_t>(k) < options_.size() && !options_[static_cast<std::size_t>(k)].empty())
                    return options_[static_cast<std::size_t>(k)];
                if (control_.type == ControlType::toggle)
                    return k == 0 ? "OFF" : "ON";
                return std::to_string(std::lround(control_.min) + k);
            }
            float value01() const { return port_.value01(); }
            bool atDefault() const { return std::fabs(plainOf(control_, value01()) - control_.def) < 1e-6f; }
            std::string valueText() const
            {
                funkgui::ValueView v;
                view(v);
                return v.text.value;
            }
            uint64_t key() const override { return std::bit_cast<uint32_t>(port_.value01()); }
            void view(funkgui::ValueView& v) const override
            {
                const float t = value01();
                v = funkgui::ValueView{};
                v.state = funkgui::ValueState::continuous;
                v.label = label_;
                v.track = t;
                v.trackDefault = normOf(control_, control_.def);
                v.atDefault = atDefault();
                const float p = plainOf(control_, t);
                switch (control_.type)
                {
                    case ControlType::toggle:
                    case ControlType::integer:
                        std::snprintf(v.text.value, sizeof v.text.value, "%s", nameOf(index()).c_str());
                        break;
                    case ControlType::real:
                        formatValue(control_, p, v.text.value, sizeof v.text.value);
                        break;
                }
                std::snprintf(v.text.spoken, sizeof v.text.spoken, "%s %s", control_.name, v.text.value);
            }
            funkgui::ParamPort* port() override { return &port_; }
            float host01FromTrack(float t) const override { return std::clamp(t, 0.0f, 1.0f); }
            float defaultHost01() const override { return normOf(control_, control_.def); }

        private:
            funkgui::ParamPort& port_;
            Control control_{};
            char label_[40] = {};
            std::vector<std::string> options_;
        };

        int stepsOf(const Control& c) { return c.type == ControlType::real ? 0 : std::max(1, static_cast<int>(std::lround(c.max - c.min))); }

        // The PANEL view's area for a program with no .gui: its dials and displays as Panel::layout places them there.
        class RowsView final : public funkgui::Panel
        {
        public:
            RowsView(const Prepared& p, int width, int height) : p_(p), width_(width), height_(height)
            {
                for (int c = 0; c < kMaxControls; ++c)
                {
                    const Control& k = p.controls[static_cast<std::size_t>(c)];
                    ports_.push_back(std::make_unique<sandbox::MockPort>(c));
                    ports_.back()->set(p.values01[static_cast<std::size_t>(c)], normOf(k, k.def), stepsOf(k));
                }
                ui::DialMeasure measure;
                measure.label = [this](int slot, bool small) {
                    const std::string& label = p_.spec.labels[static_cast<std::size_t>(slot)];
                    const std::string& name = p_.controls[static_cast<std::size_t>(slot)].name;
                    const std::string text = ui::upper(ui::asciiText(label.empty() ? name : label));
                    return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(),
                                                small ? funkgui::type::kMicro : funkgui::type::kCaption);
                };
                measure.title = [](const std::string& title) {
                    return funkgui::text::width(funkgui::FontService::get().atlas(), ui::upper(ui::asciiText(title)).c_str(), kSectionTitle);
                };
                measure.caption = [](const std::string& text) {
                    return funkgui::text::width(funkgui::FontService::get().atlas(), ui::upper(ui::asciiText(text)).c_str(), funkgui::type::kCaption);
                };
                measure.micro = [](const std::string& text) {
                    return funkgui::text::width(funkgui::FontService::get().atlas(), ui::upper(ui::asciiText(text)).c_str(), funkgui::type::kMicro);
                };
                measure.readout = [](const std::string& text) {
                    return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(), ui::looks::kReadout);
                };
                // The PANEL view's two passes (Panel::layout): the dials from the area's top, as tall as all its room.
                const float W = static_cast<float>(width_), H = static_cast<float>(height_);
                const ui::DialsLayout first = ui::layoutDials(p_.spec, p_.controls, measure, 0.0f, W, H, H, 0.0f, ui::kTightRowGap);
                layout_ = ui::layoutDials(p_.spec, p_.controls, measure, 0.0f, W, H - first.top, H, 0.0f, ui::kTightRowGap);
                for (int c = 0; c < kMaxControls; ++c)
                {
                    const Control& k = p_.controls[static_cast<std::size_t>(c)];
                    if (!k.used)
                        continue;
                    const ui::ItemPlace& at = layout_.dials[static_cast<std::size_t>(c)];
                    Cell cell;
                    cell.slot = c;
                    cell.place = at;
                    cell.model = std::make_unique<RowModel>(*ports_[static_cast<std::size_t>(c)], k, p_.spec.labels[static_cast<std::size_t>(c)],
                                                            p_.spec.options[static_cast<std::size_t>(c)]);
                    const DialStyle style = p_.spec.styles[static_cast<std::size_t>(c)];
                    const funkgui::DialGeom g = style == DialStyle::knob || style == DialStyle::vintage
                        ? funkgui::DialGeom{ at.x, at.top, at.small ? funkgui::DialSize::compact : funkgui::DialSize::primary }
                        : funkgui::DialGeom{ std::round(at.x + 0.5f * at.w - 0.5f * ui::kDialW), at.top, funkgui::DialSize::primary };
                    cell.dial = std::make_unique<funkgui::Dial>(*cell.model, g, kA11yStride * static_cast<uint32_t>(c + 1));
                    cells_.push_back(std::move(cell));
                }
                for (std::size_t d = 0; d < p_.spec.displays.size() && d < static_cast<std::size_t>(kMaxDisplays); ++d)
                {
                    items_.push_back(std::make_unique<ui::DisplayItem>(kA11yStride * static_cast<uint32_t>(kMaxControls + d + 1)));
                    auto& item = static_cast<ui::DisplayItem&>(*items_.back());
                    item.setDisplay(&p_.spec.displays[d]);
                    item.setCell(layout_.displays[d]);
                    displays_.push_back(static_cast<int>(items_.size()) - 1);
                }
                demoMeters();
            }

            const ui::DialsLayout& layout() const noexcept { return layout_; }
            int pictureWidth() const noexcept { return static_cast<int>(std::ceil(static_cast<float>(width_) * sandbox::kDpi)); }
            int pictureHeight() const noexcept { return static_cast<int>(std::ceil(static_cast<float>(height_) * sandbox::kDpi)); }

            void attach(funkgui::HostServices&) override {}
            int width() const override { return width_ + width_ % 2; }
            int height() const override { return height_ + height_ % 2; }
            void tick(float dt) override
            {
                demoMeters();
                ui::updateDisplays(items_, displays_, meters_,
                                   [this](bool input, float* out, int n) { sandbox::demoScope(input, meters_.rate, out, n); }, feed_, dt);
                for (Cell& c : cells_)
                    c.dial->tick(dt, false, false, false);
                const funkgui::Point far{ -1.0e4f, -1.0e4f };
                for (const auto& item : items_)
                    item->tick(dt, false, false, far);
                dirty_ = false;
            }
            void draw(funkgui::Canvas& c, const funkgui::Theme& hostTheme) override
            {
                const funkgui::Theme th = ui::productTheme(hostTheme);
                const ui::chat::Palette pal = ui::chat::palette(th);
                c.rrect(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()), 0.0f, pal.card);
                drawSections(c, th);
                for (const Cell& cell : cells_)
                    drawDial(c, th, cell);
                for (const auto& item : items_)
                    if (item->shown())
                        item->draw(c, th, false);
            }
            bool wantsFullRate() const override
            {
                return dirty_ || std::any_of(items_.begin(), items_.end(), [](const auto& it) { return !it->settled(); })
                    || std::any_of(cells_.begin(), cells_.end(), [](const Cell& c) { return !c.dial->settled(); });
            }
            void accessibility(std::vector<funkgui::A11yItem>&) const override {}
            uint32_t a11yRevision() const override { return 0; }
            void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
            void closeGestures() override {}

        private:
            struct Cell
            {
                int slot = 0;
                ui::ItemPlace place;
                std::unique_ptr<RowModel> model;
                std::unique_ptr<funkgui::Dial> dial;
            };

            // Panel::draw's section frames: a frame each, its title cut into the top edge.
            void drawSections(funkgui::Canvas& c, const funkgui::Theme& th) const
            {
                for (const ui::SectionFrame& f : layout_.sections)
                {
                    const funkgui::Rect& r = f.r;
                    const bool tinted = !f.color.empty();
                    const funkgui::Col col = ui::looks::panelColor(f.color, th, th.accent);
                    const funkgui::Col edge = tinted ? funkgui::premix(th.ground, col, 0.65f) : th.ink32;
                    const funkgui::Col fill = tinted ? funkgui::premix(funkgui::mix(th.ground, th.ink16, 0.22f), col, 0.08f)
                                                     : funkgui::mix(th.ground, th.ink16, 0.22f);
                    c.rrect(r.x, r.y, r.w, r.h, 0.0f, fill);
                    const std::string title = ui::upper(ui::asciiText(f.title));
                    if (title.empty())
                        c.hairlineH(r.x, r.y, r.w, edge);
                    else
                    {
                        const float tw = std::min(c.textWidth(title.c_str(), kSectionTitle), r.w - 16.0f);
                        const float l = r.centreX() - 0.5f * tw - 7.0f, rr = r.centreX() + 0.5f * tw + 7.0f;
                        c.hairlineH(r.x, r.y, std::max(0.0f, l - r.x), edge);
                        c.hairlineH(rr, r.y, std::max(0.0f, r.right() - rr), edge);
                        c.text(ui::fitText(c, title, kSectionTitle, r.w - 16.0f).c_str(), r.centreX(), c.capCentreTop(r.y, kSectionTitle),
                               kSectionTitle, tinted ? col : th.ink70, funkgui::Align::centre);
                    }
                    c.hairlineV(r.x, r.y, r.h, edge);
                    c.hairlineV(r.right() - 1.0f, r.y, r.h, edge);
                    c.hairlineH(r.x, r.bottom() - 1.0f, r.w, edge);
                }
            }

            // Panel.cpp's DialItem::drawDial.
            void drawDial(funkgui::Canvas& c, const funkgui::Theme& th, const Cell& cell) const
            {
                const std::string& color = p_.spec.colors[static_cast<std::size_t>(cell.slot)];
                const DialStyle style = p_.spec.styles[static_cast<std::size_t>(cell.slot)];
                const bool tinted = !color.empty();
                const funkgui::Col col = ui::looks::panelColor(color, th, th.accent);
                if (style == DialStyle::knob)
                {
                    if (!tinted)
                        return cell.dial->draw(c, th, false);
                    funkgui::Theme t = th;
                    t.accent = col;
                    t.accentDim = funkgui::premix(th.ground, col, 0.4f);
                    return cell.dial->draw(c, t, false);
                }
                const RowModel& m = *cell.model;
                const std::string value = m.valueText();
                ui::looks::Knob k;
                k.cell = { cell.place.x, cell.place.top, cell.place.w, cell.place.h };
                k.small = cell.place.small;
                k.t = m.value01();
                k.label = m.label();
                k.value = value.c_str();
                k.atDefault = m.atDefault();
                k.color = col;
                k.tinted = tinted;
                switch (style)
                {
                    case DialStyle::knob:
                    case DialStyle::vintage: ui::looks::vintage(c, th, k); break;
                    case DialStyle::fader:   ui::looks::fader(c, th, k); break;
                    case DialStyle::toggle:  ui::looks::toggle(c, th, k, value.c_str()); break;
                    case DialStyle::buttons:
                    {
                        std::vector<std::string> names;
                        for (int i = 0; i < std::min(m.count(), kMaxOptions); ++i)
                            names.push_back(m.nameOf(i));
                        ui::looks::buttons(c, th, k, names, m.index(), -1);
                        break;
                    }
                }
            }

            // The GUI sandbox's demo meters (PanelView's MockMeters): a steady tone's levels, each field moving slowly.
            void demoMeters()
            {
                ++meters_.frame;
                meters_.rate = 48000.0f;
                meters_.program = 1;
                meters_.outPeak[0] = 0.22f;
                meters_.outPeak[1] = 0.21f;
                meters_.outRms[0] = 0.108f;
                meters_.outRms[1] = 0.104f;
                if (!p_.instrument)
                {
                    meters_.inPeak[0] = 0.25f;
                    meters_.inPeak[1] = 0.24f;
                    meters_.inRms[0] = 0.124f;
                    meters_.inRms[1] = 0.120f;
                }
                const float phase = 6.2831853f * static_cast<float>(meters_.frame) / 96.0f;
                for (std::size_t d = 0; d < p_.spec.displays.size(); ++d)
                {
                    const PanelDisplay& x = p_.spec.displays[d];
                    if (x.tap < 0 || x.tap >= kMaxDisplays || x.hidden)
                        continue;
                    const float t = 0.55f + 0.3f * std::sin(phase + 1.7f * static_cast<float>(d));
                    const float v = x.lo + t * (x.hi - x.lo);
                    meters_.taps[x.tap] = x.integer ? std::round(v) : v;
                }
            }

            const Prepared& p_;
            int width_, height_;
            std::vector<std::unique_ptr<sandbox::MockPort>> ports_;
            std::vector<Cell> cells_;
            ui::GuiView::Items items_;
            std::vector<int> displays_;
            ui::DialsLayout layout_;
            Meters meters_{};
            ui::DisplayFeed feed_;
            bool dirty_ = true;
        };
    }

    Picture drawRows(const Prepared& p, int theme, int maxWidth)
    {
        Picture out;
        RowsView view(p, kAreaWidth, kAreaHeight);
        funkgui::HeadlessHost host(view, theme == 1 ? 1 : 0, sandbox::kDpi);
        host.settle(120);
        const picture::Drawn drawn = picture::draw(host.draw());
        out.gpu = drawn.gpu;
        out.why = drawn.why;
        if (drawn.image.w <= 0 || drawn.image.h <= 0)
        {
            out.why = "nothing was drawn";
            return out;
        }
        out.image = areaOf(drawn.image, view.pictureWidth(), view.pictureHeight(), maxWidth);
        return out;
    }

    RowsPlaces rowsPlaces(const Prepared& p)
    {
        RowsPlaces out;
        const RowsView view(p, kAreaWidth, kAreaHeight);
        const ui::DialsLayout& l = view.layout();
        for (int c = 0; c < kMaxControls; ++c)
            if (p.controls[static_cast<std::size_t>(c)].used)
            {
                const ui::ItemPlace& at = l.dials[static_cast<std::size_t>(c)];
                out.dials[static_cast<std::size_t>(c)] = { at.x, at.top, at.w, at.h };
            }
        for (std::size_t d = 0; d < p.spec.displays.size() && d < l.displays.size(); ++d)
            out.displays.push_back({ l.displays[d].x, l.displays[d].top, l.displays[d].w, l.displays[d].h });
        return out;
    }
}
