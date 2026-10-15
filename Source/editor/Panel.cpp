// Source/editor/Panel.cpp: see Panel.h.
#include "editor/Panel.h"

#include "editor/ParamModel.h"
#include "editor/ShapeView.h"
#include "editor/Theme.h"

#include "plugin/EditorPort.h"

#include "dsp/FdnLayout.h"
#include "dsp/Params.h"

#include "Product.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/widgets/Dial.h>
#include <funkgui/widgets/RuleSlider.h>
#include <funkgui/widgets/SegmentedSelector.h>
#include <funkgui/widgets/ThemeCells.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string_view>
#include <utility>

namespace testingplayground::ui
{
    // ---- one control or view on the panel: a Dial, a RuleSlider, a row of cells, the graph, the readout -----------------
    class Item
    {
    public:
        virtual ~Item() = default;

        virtual void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) = 0;
        virtual void draw(funkgui::Canvas&, const funkgui::Theme&, bool focusRing) const = 0;
        virtual bool contains(funkgui::Point) const = 0;
        virtual funkgui::Cursor cursorAt(funkgui::Point) const = 0;
        virtual void pointerMove(const funkgui::PointerEvent&) {}
        virtual void pointerExit() {}
        virtual void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) = 0;
        virtual void pointerDrag(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual bool wheel(const funkgui::WheelEvent&, funkgui::GestureController&, double /*nowSec*/) { return false; }
        virtual bool key(const funkgui::KeyEvent&, funkgui::GestureController&) = 0;
        virtual void accessibility(std::vector<funkgui::A11yItem>&) const = 0;
        virtual bool a11yAction(uint32_t id, funkgui::A11yAction, double value, funkgui::GestureController&) = 0;
        virtual void specLine(char* out, std::size_t n) const = 0;
        virtual bool settled() const = 0;
        virtual funkgui::Point centre() const = 0;
        virtual bool shown() const { return true; }      // false: not drawn, no input, no Tab stop
        virtual bool focusable() const { return true; }  // false: no Tab stop (a readout)
    };

    namespace
    {
        // ---- layout (logical px) --------------------------------------------------------------------------------------
        constexpr float kMargin = 24.0f;
        constexpr float kMinContentW = 552.0f;
        constexpr float kTitleTop = 17.0f;
        constexpr float kHeaderRuleY = 46.0f;
        constexpr float kSectionTop = 64.0f;
        constexpr float kDialCellW = 60.0f;              // funkgui::DialGeom, primary: 60 x 80
        constexpr float kSliderW = 180.0f;               // the rule; the hit rectangle is 12 px wider, 80 px tall
        constexpr float kGapX = 18.0f;                   // between two controls in a row
        constexpr float kControlRowH = 96.0f;            // an 80 px control and the gap below it
        constexpr float kChoiceCellW = 48.0f;            // LINES: 4 | 8 | 16
        constexpr float kChoiceCellH = 22.0f;
        constexpr float kChoiceBlockH = 52.0f;           // a choice's caption and cells
        constexpr float kRuleGap = 14.0f;                // above the modulation section, below its rule
        constexpr float kModColumnGap = 24.0f;           // between the graph and the RATE / DEPTH column
        constexpr float kModColumnH = 176.0f;            // two dials: 80 + 16 + 80
        constexpr float kReadoutH = 26.0f;               // the DELAYS line under the graph
        constexpr float kFooterH = 36.0f;

        constexpr uint32_t kA11yStride = 64;             // item i's ids: kA11yStride * (i + 1) + 0..63

        // The rows laid out in the modulation column, beside the graph, rather than in the reverb row.
        bool isModulation(std::size_t i) noexcept { return i == param::modrate || i == param::moddepth; }

        std::string upper(std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return out;
        }

        // The longest prefix of whole "   "-separated fields of `line` that fits in `maxW` px of kMicro.
        std::string fitFields(const funkgui::Canvas& c, const char* line, float maxW)
        {
            std::string s = line;
            while (!s.empty() && c.textWidth(s.c_str(), funkgui::type::kMicro) > maxW)
            {
                const std::size_t cut = s.rfind("   ");
                if (cut == std::string::npos)
                    return {};
                s.resize(cut);
            }
            return s;
        }

        // ---- a Dial over a parameter ------------------------------------------------------------------------------
        class DialItem final : public Item
        {
        public:
            DialItem(const ParamSpec& spec, funkgui::ParamPort& port, funkgui::DialGeom g, uint32_t id)
                : model_(spec, port), dial_(model_, g, id) {}

            void tick(float dt, bool hovered, bool focused, funkgui::Point) override
            {
                dial_.tick(dt, hovered, focused, false);
            }
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override { dial_.draw(c, th, ring); }
            bool contains(funkgui::Point p) const override { return dial_.contains(p); }
            funkgui::Cursor cursorAt(funkgui::Point p) const override { return dial_.cursorAt(p); }
            void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g,
                             funkgui::HostServices& h) override
            {
                dial_.pointerDown(e, g, h);
            }
            void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g) override
            {
                dial_.pointerDrag(e, g);
            }
            void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController& g) override { dial_.pointerUp(e, g); }
            void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController& g) override { dial_.doubleClick(g); }
            bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController& g, double now) override
            {
                return dial_.wheel(e, g, now);
            }
            bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g) override { return dial_.key(e, g); }
            void accessibility(std::vector<funkgui::A11yItem>& out) const override
            {
                funkgui::A11yItem item;
                dial_.accessibility(item);
                out.push_back(item);
            }
            bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g) override
            {
                if (id != dial_.a11yId())
                    return false;
                dial_.a11yAction(a, v, g);
                return true;
            }
            void specLine(char* out, std::size_t n) const override { dial_.specLine(out, n); }
            bool settled() const override { return dial_.settled(); }
            funkgui::Point centre() const override { return dial_.geom().centre(); }

        private:
            ParamModel model_;                           // before dial_, which refers to it
            funkgui::Dial dial_;
        };

        // ---- a RuleSlider over a parameter ------------------------------------------------------------------------
        class SliderItem final : public Item
        {
        public:
            SliderItem(const ParamSpec& spec, funkgui::ParamPort& port, funkgui::SlotGeom g, uint32_t id)
                : model_(spec, port), slider_(model_, g, id) {}

            void tick(float dt, bool hovered, bool focused, funkgui::Point) override
            {
                slider_.tick(dt, hovered, focused, false);
            }
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override
            {
                slider_.draw(c, th, ring);
            }
            bool contains(funkgui::Point p) const override { return slider_.contains(p); }
            funkgui::Cursor cursorAt(funkgui::Point p) const override { return slider_.cursorAt(p); }
            void pointerMove(const funkgui::PointerEvent& e) override { slider_.pointerMove(e); }
            void pointerExit() override { slider_.pointerExit(); }
            void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g,
                             funkgui::HostServices& h) override
            {
                slider_.pointerDown(e, g, h);
            }
            void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g) override
            {
                slider_.pointerDrag(e, g);
            }
            void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController& g) override
            {
                slider_.pointerUp(e, g);
            }
            void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController& g) override
            {
                slider_.doubleClick(g);
            }
            bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController& g, double now) override
            {
                return slider_.wheel(e, g, now);
            }
            bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g) override { return slider_.key(e, g); }
            void accessibility(std::vector<funkgui::A11yItem>& out) const override
            {
                funkgui::A11yItem item;
                slider_.accessibility(item);
                out.push_back(item);
            }
            bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g) override
            {
                if (id != slider_.a11yId())
                    return false;
                slider_.a11yAction(a, v, g);
                return true;
            }
            void specLine(char* out, std::size_t n) const override { slider_.specLine(out, n); }
            bool settled() const override { return slider_.settled(); }
            funkgui::Point centre() const override
            {
                const funkgui::SlotGeom& g = slider_.geom();
                return { g.x + g.w * 0.5f, g.trackY() };
            }

        private:
            ParamModel model_;
            funkgui::RuleSlider slider_;
        };

        // ---- the ZOOM cells: the host's UI zoom steps (EditorHost's EditorConfig::zoomSteps) ------------------------
        class ZoomModel final : public funkgui::CellModel
        {
        public:
            static constexpr int kMaxSteps = 4;

            void attach(funkgui::HostServices& host) { host_ = &host; }

            int count() const override
            {
                return host_ == nullptr ? 0 : std::min(kMaxSteps, static_cast<int>(host_->zoomSteps().size()));
            }
            int active() const override
            {
                for (int i = 0; i < count(); ++i)
                    if (step(i) == host_->zoomPercent())
                        return i;
                return -1;
            }
            const char* label(int i) const override
            {
                auto& buf = labels_[static_cast<std::size_t>(std::clamp(i, 0, kMaxSteps - 1))];
                std::snprintf(buf.data(), buf.size(), "%d%%", step(i));
                return buf.data();
            }
            const char* spoken(int i) const override
            {
                auto& buf = spoken_[static_cast<std::size_t>(std::clamp(i, 0, kMaxSteps - 1))];
                std::snprintf(buf.data(), buf.size(), "Zoom %d percent", step(i));
                return buf.data();
            }
            bool enabled(int i) const override { return host_ != nullptr && host_->zoomFits(step(i)); }
            void select(int i, funkgui::GestureController&) override
            {
                if (host_ != nullptr && i >= 0 && i < count())
                    host_->setZoomPercent(step(i));
            }

        private:
            int step(int i) const
            {
                if (host_ == nullptr || i < 0 || i >= count())
                    return 0;
                return host_->zoomSteps()[static_cast<std::size_t>(i)];
            }

            funkgui::HostServices* host_ = nullptr;
            mutable std::array<std::array<char, 8>, kMaxSteps> labels_{};
            mutable std::array<std::array<char, 24>, kMaxSteps> spoken_{};
        };

        // ---- a row of cells: a choice parameter (ParamCells), the THEME cells or the ZOOM cells ----------------------
        class CellsItem final : public Item
        {
        public:
            using SpecFn = std::function<void(char*, std::size_t)>;
            using ShownFn = std::function<bool()>;

            // Over a model this item owns (ParamCells, ZoomModel).
            CellsItem(std::unique_ptr<funkgui::CellModel> model, std::vector<funkgui::Rect> cells,
                      funkgui::CellStyle style, const char* caption, funkgui::Point captionAt, uint32_t id,
                      SpecFn spec, ShownFn shown = {})
                : model_(std::move(model)),
                  own_(std::make_unique<funkgui::SegmentedSelector>(*model_, std::move(cells), style, caption,
                                                                    captionAt, id)),
                  sel_(own_.get()), spec_(std::move(spec)), shown_(std::move(shown)) {}

            // The THEME cells (funkgui::ThemeCells: the machine-wide theme preference).
            CellsItem(std::unique_ptr<funkgui::ThemeCells> theme, SpecFn spec)
                : theme_(std::move(theme)), sel_(&theme_->selector()), spec_(std::move(spec)) {}

            void tick(float dt, bool, bool, funkgui::Point pointer) override { sel_->tick(dt, pointer); }
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override { sel_->draw(c, th, ring); }
            bool contains(funkgui::Point p) const override { return sel_->contains(p); }
            funkgui::Cursor cursorAt(funkgui::Point p) const override { return sel_->cursorAt(p); }
            void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g,
                             funkgui::HostServices&) override
            {
                sel_->pointerDown(e, g);
            }
            bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g) override { return sel_->key(e, g); }
            void accessibility(std::vector<funkgui::A11yItem>& out) const override { sel_->accessibility(out); }
            bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g) override
            {
                return sel_->a11yAction(id, a, v, g);
            }
            void specLine(char* out, std::size_t n) const override
            {
                if (n > 0)
                    out[0] = '\0';
                if (spec_)
                    spec_(out, n);
            }
            bool settled() const override { return sel_->settled(); }
            funkgui::Point centre() const override
            {
                const funkgui::Rect b = sel_->bounds();
                return { b.x + b.w * 0.5f, b.y + b.h * 0.5f };
            }
            bool shown() const override { return !shown_ || shown_(); }

        private:
            std::unique_ptr<funkgui::CellModel> model_;
            std::unique_ptr<funkgui::SegmentedSelector> own_;
            std::unique_ptr<funkgui::ThemeCells> theme_;
            funkgui::SegmentedSelector* sel_ = nullptr;
            SpecFn spec_;
            ShownFn shown_;
        };

        // The cell texts of a choice parameter (labels upper case, spoken as written).
        std::vector<funkgui::CellText> choiceTexts(const ParamSpec& s)
        {
            std::vector<funkgui::CellText> texts;
            for (int i = 0; i < s.numChoices; ++i)
                texts.push_back({ s.choices[i], nullptr, nullptr });
            return texts;
        }

        // ---- the LFO shape editor (ShapeView.h) -----------------------------------------------------------------------
        class ShapeItem final : public Item
        {
        public:
            ShapeItem(EditorPort& port, funkgui::Rect bounds, uint32_t id) : view_(port, bounds, id) {}

            ShapeView& view() noexcept { return view_; }

            void tick(float dt, bool hovered, bool focused, funkgui::Point) override { view_.tick(dt, hovered, focused); }
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override { view_.draw(c, th, ring); }
            bool contains(funkgui::Point p) const override { return view_.contains(p); }
            funkgui::Cursor cursorAt(funkgui::Point p) const override { return view_.cursorAt(p); }
            void pointerMove(const funkgui::PointerEvent& e) override { view_.pointerMove(e); }
            void pointerExit() override { view_.pointerExit(); }
            void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&,
                             funkgui::HostServices& h) override
            {
                view_.pointerDown(e, h, productTheme(funkgui::Theme::byIndex(h.themeIndex())));
            }
            void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&) override { view_.pointerDrag(e); }
            void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController&) override { view_.pointerUp(e); }
            void doubleClick(const funkgui::PointerEvent& e, funkgui::GestureController&) override
            {
                view_.doubleClick(e);
            }
            bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override { return view_.key(e); }
            void accessibility(std::vector<funkgui::A11yItem>& out) const override { view_.accessibility(out); }
            bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController&) override
            {
                return view_.a11yAction(id, a, v);
            }
            void specLine(char* out, std::size_t n) const override { view_.specLine(out, n); }
            bool settled() const override { return view_.settled(); }
            funkgui::Point centre() const override
            {
                const funkgui::Rect r = view_.plot();
                return { r.centreX(), r.centreY() };
            }

        private:
            ShapeView view_;
        };

        // ---- the DELAYS readout: the delay lengths SIZE and LINES give, as the Engine computes them ---------------------
        class DelaysItem final : public Item
        {
        public:
            DelaysItem(funkgui::ParamPort& size, funkgui::ParamPort& lines, EditorPort& port, funkgui::Rect bounds,
                       uint32_t id)
                : size_(size), lines_(lines), port_(port), bounds_(bounds), id_(id),
                  lastSize01_(size.default01()), lastLines01_(lines.default01()) {}

            // "DELAYS" and the lengths in samples, at the processor's rate; recomputed only when an input changed. A
            // non-finite value (a host's NaN) shows the last finite one, as the dials do (ParamModel.h).
            const std::string& text() const
            {
                const float size = fromNormalised(kParams[param::size], shownValue01(size_.value01(), lastSize01_));
                const int lines = linesFromChoice(fromNormalised(kParams[param::lines],
                                                                 shownValue01(lines_.value01(), lastLines01_)));
                const float fs = port_.sampleRate();
                if (size != keySize_ || lines != keyLines_ || fs != keyRate_)
                {
                    keySize_ = size;
                    keyLines_ = lines;
                    keyRate_ = fs;
                    const FdnLayout l = makeLayout(fs, size, lines);
                    std::string s;
                    for (int i = 0; i < l.lines; ++i)
                    {
                        if (i > 0)
                            s += ' ';
                        s += std::to_string(l.delay[static_cast<std::size_t>(i)]);
                    }
                    char rate[24];
                    std::snprintf(rate, sizeof rate, "%.1f", static_cast<double>(fs) / 1000.0);
                    numbers_ = s;
                    text_ = "DELAYS " + s + " SAMPLES AT " + rate + " KHZ";
                    rate_ = std::string("SAMPLES AT ") + rate + " KHZ";
                }
                return text_;
            }

            void tick(float, bool, bool, funkgui::Point) override {}
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const override
            {
                namespace type = funkgui::type;
                text();
                const funkgui::Canvas::Scope s(c, funkgui::tags::none, false);
                c.text("DELAYS", bounds_.x, bounds_.y, type::kCaption, th.ink52);
                const float x0 = bounds_.x + c.textWidth("DELAYS", type::kCaption) + 12.0f;
                const float unitW = c.textWidth(rate_.c_str(), type::kMicro);
                c.text(rate_.c_str(), bounds_.right(), bounds_.y + 1.0f, type::kMicro, th.ink32, funkgui::Align::right);
                // The numbers in one line, cut at a whole number (with an ellipsis) when they do not fit.
                constexpr funkgui::TextStyle kNumbers{ 10.0f, 0.4f, 0.03f, true };
                const float room = bounds_.right() - unitW - 12.0f - x0;
                std::string shown = numbers_;
                while (!shown.empty() && c.textWidth(shown.c_str(), kNumbers) > room)
                {
                    const std::size_t cut = shown.rfind(' ');
                    shown.resize(cut == std::string::npos ? 0 : cut);
                    if (!shown.empty() && c.textWidth((shown + " \xE2\x80\xA6").c_str(), kNumbers) <= room)
                    {
                        shown += " \xE2\x80\xA6";
                        break;
                    }
                }
                c.text(shown.c_str(), x0, bounds_.y + 1.0f, kNumbers, th.ink70);
            }
            bool contains(funkgui::Point p) const override { return bounds_.contains(p); }
            funkgui::Cursor cursorAt(funkgui::Point) const override { return funkgui::Cursor::normal; }
            void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override {}
            bool key(const funkgui::KeyEvent&, funkgui::GestureController&) override { return false; }
            void accessibility(std::vector<funkgui::A11yItem>& out) const override
            {
                funkgui::A11yItem item;
                item.id = id_;
                item.role = funkgui::A11yRole::staticText;
                item.bounds = bounds_;
                item.title = "Delay lengths";
                item.value = text();
                item.readOnly = true;
                out.push_back(std::move(item));
            }
            bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override
            {
                return id == id_;
            }
            void specLine(char* out, std::size_t n) const override
            {
                if (n > 0)
                    std::snprintf(out, n, "%s", text().c_str());
            }
            bool settled() const override { return true; }
            funkgui::Point centre() const override { return { bounds_.centreX(), bounds_.centreY() }; }
            bool focusable() const override { return false; }

        private:
            funkgui::ParamPort& size_;
            funkgui::ParamPort& lines_;
            EditorPort& port_;
            funkgui::Rect bounds_;
            uint32_t id_;
            mutable float lastSize01_, lastLines01_;     // the last finite value01 of each (shownValue01)
            mutable float keySize_ = -1.0f, keyRate_ = -1.0f;
            mutable int keyLines_ = -1;
            mutable std::string text_, numbers_, rate_;
        };
    }

    // ---- construction: the layout -------------------------------------------------------------------------------------
    Panel::Panel(std::span<funkgui::ParamPort* const> ports, EditorPort& editorPort)
    {
        title_ = upper(product::kName);
        subtitle_ = upper(product::kKind) + "  " + product::kVersion;

        // The content is wide enough for the reverb row's dials and sliders beside the choice cells.
        float rowW = 0.0f, choiceW = 0.0f;
        for (std::size_t i = 0; i < kParams.size(); ++i)
        {
            const ParamSpec& s = kParams[i];
            if (isModulation(i))
                continue;
            if (s.control == Control::dial)
                rowW += kDialCellW + kGapX;
            else if (s.control == Control::slider)
                rowW += kSliderW + 12.0f + kGapX;
            else if (s.control == Control::choice)
                choiceW = std::max(choiceW, kChoiceCellW * static_cast<float>(s.numChoices));
        }
        const float contentW = std::max(kMinContentW, rowW + (choiceW > 0.0f ? 2.0f * kGapX + choiceW : 0.0f));
        width_ = 2.0f * kMargin + contentW;

        // The reverb row: dials and sliders left to right in table order, wrapping before the choice block on the
        // first row and at the content's width after it; the choice rows stacked at the first row's right end.
        itemOfParam_.assign(kParams.size(), -1);
        const auto idFor = [this] { return kA11yStride * static_cast<uint32_t>(items_.size() + 1); };
        float rowTop = kSectionTop, x = 0.0f, rowH = 0.0f;
        float choiceTop = kSectionTop;
        const auto rowLimit = [&] { return rowTop == kSectionTop && choiceW > 0.0f ? contentW - choiceW - kGapX
                                                                                   : contentW; };
        const auto newRow = [&] {
            if (rowH > 0.0f)
                rowTop += rowH;
            x = 0.0f;
            rowH = 0.0f;
        };
        for (std::size_t i = 0; i < kParams.size() && i < ports.size(); ++i)
        {
            const ParamSpec& s = kParams[i];
            if (isModulation(i))
                continue;
            if (s.control == Control::dial)
            {
                if (x > 0.0f && x + kDialCellW > rowLimit())
                    newRow();
                itemOfParam_[i] = static_cast<int>(items_.size());
                items_.push_back(std::make_unique<DialItem>(s, *ports[i], funkgui::DialGeom{ kMargin + x, rowTop },
                                                            idFor()));
                x += kDialCellW + kGapX;
                rowH = std::max(rowH, kControlRowH);
            }
            else if (s.control == Control::slider)
            {
                if (x > 0.0f && x + kSliderW + 12.0f > rowLimit())
                    newRow();
                itemOfParam_[i] = static_cast<int>(items_.size());
                items_.push_back(std::make_unique<SliderItem>(
                    s, *ports[i], funkgui::SlotGeom{ kMargin + x + 6.0f, rowTop, kSliderW }, idFor()));
                x += kSliderW + 12.0f + kGapX;
                rowH = std::max(rowH, kControlRowH);
            }
            else if (s.control == Control::choice && s.numChoices > 0)
            {
                const float left = kMargin + contentW - kChoiceCellW * static_cast<float>(s.numChoices);
                std::vector<funkgui::Rect> cells;
                for (int k = 0; k < s.numChoices; ++k)
                    cells.push_back({ left + kChoiceCellW * static_cast<float>(k), choiceTop + 18.0f,
                                      kChoiceCellW - 4.0f, kChoiceCellH });
                auto model = std::make_unique<funkgui::ParamCells>(*ports[i], choiceTexts(s));
                const funkgui::ParamCells* cellsModel = model.get();
                const ParamSpec* spec = &s;
                itemOfParam_[i] = static_cast<int>(items_.size());
                items_.push_back(std::make_unique<CellsItem>(
                    std::move(model), std::move(cells), funkgui::CellStyle::boxed, s.label,
                    funkgui::Point{ left, choiceTop }, idFor(), [spec, cellsModel](char* out, std::size_t n) {
                        const int a = cellsModel->active();
                        std::snprintf(out, n, "%s   %s", spec->label, a >= 0 ? cellsModel->label(a) : "");
                    }));
                choiceTop += kChoiceBlockH;
            }
        }
        newRow();
        ruleY_ = std::max(rowTop, choiceTop) + 2.0f;

        // The modulation section: the graph, the RATE / DEPTH column beside it, the DELAYS line under it.
        const float modTop = ruleY_ + kRuleGap;
        const float columnX = kMargin + contentW - kDialCellW;
        float columnTop = modTop, columnBottom = modTop;
        for (std::size_t i = 0; i < kParams.size() && i < ports.size(); ++i)
        {
            const ParamSpec& s = kParams[i];
            if (!isModulation(i) || s.control == Control::hidden)
                continue;
            itemOfParam_[i] = static_cast<int>(items_.size());
            items_.push_back(std::make_unique<DialItem>(s, *ports[i], funkgui::DialGeom{ columnX, columnTop }, idFor()));
            columnBottom = columnTop + 80.0f;
            columnTop += kControlRowH;
        }
        const float graphRight = columnX - kModColumnGap;
        const funkgui::Rect graph{ kMargin, modTop, graphRight - kMargin,
                                   std::max(kModColumnH, columnBottom - modTop) };
        auto shapeItem = std::make_unique<ShapeItem>(editorPort, graph, idFor());
        shape_ = &shapeItem->view();
        items_.push_back(std::move(shapeItem));

        const funkgui::Rect readout{ kMargin, graph.bottom() + 12.0f, contentW, kReadoutH - 12.0f };
        auto delays = std::make_unique<DelaysItem>(*ports[param::size], *ports[param::lines], editorPort, readout,
                                                   idFor());
        readout_ = delays.get();
        items_.push_back(std::move(delays));

        height_ = readout.bottom() + 8.0f + kFooterH;

        // THEME, in the header's right corner.
        const float right = width_ - kMargin;
        auto theme = std::make_unique<funkgui::ThemeCells>(
            std::vector<funkgui::Rect>{ { right - 128.0f, 15.0f, 72.0f, 18.0f }, { right - 54.0f, 15.0f, 54.0f, 18.0f } },
            nullptr, funkgui::Point{}, idFor());
        items_.push_back(std::make_unique<CellsItem>(std::move(theme), [this](char* out, std::size_t n) {
            std::snprintf(out, n, "THEME   %s",
                          funkgui::Theme::name(host_ != nullptr ? host_->themeIndex() : 0));
        }));

        // ZOOM, in the footer's right corner; shown when the host offers zoom steps (the plugin's editor does).
        auto zoom = std::make_unique<ZoomModel>();
        ZoomModel* zoomModel = zoom.get();
        std::vector<funkgui::Rect> zoomCells;
        for (int k = 0; k < ZoomModel::kMaxSteps; ++k)
            zoomCells.push_back({ right - 44.0f * static_cast<float>(ZoomModel::kMaxSteps - k), height_ - 28.0f, 42.0f,
                                  18.0f });
        items_.push_back(std::make_unique<CellsItem>(
            std::move(zoom), std::move(zoomCells), funkgui::CellStyle::text, nullptr, funkgui::Point{}, idFor(),
            [zoomModel](char* out, std::size_t n) {
                const int a = zoomModel->active();
                std::snprintf(out, n, "ZOOM   %s", a >= 0 ? zoomModel->label(a) : "");
            },
            [zoomModel] { return zoomModel->count() > 0; }));
        zoom_ = zoomModel;
    }

    // The host closes the open gestures before it lets go of the Panel (EditorHost, HeadlessHost); the
    // GestureController's own destructor closes anything left. No HostServices call from here: the host may be gone.
    Panel::~Panel() = default;

    std::optional<funkgui::Point> Panel::centreOf(std::size_t param) const
    {
        if (param >= itemOfParam_.size() || itemOfParam_[param] < 0)
            return std::nullopt;
        return items_[static_cast<std::size_t>(itemOfParam_[param])]->centre();
    }

    std::string Panel::delaysText() const
    {
        return static_cast<const DelaysItem*>(readout_)->text();
    }

    void Panel::attach(funkgui::HostServices& host)
    {
        host_ = &host;
        gestures_.emplace(host);
        if (zoom_ != nullptr)
            static_cast<ZoomModel*>(zoom_)->attach(host);
    }

    int Panel::width() const { return static_cast<int>(width_); }
    int Panel::height() const { return static_cast<int>(height_); }

    // ---- frame ----------------------------------------------------------------------------------------------------------
    void Panel::tick(float dt)
    {
        const int hovered = hasPointer_ && pressed_ < 0 ? itemAt(pointer_) : pressed_;
        const funkgui::Point far{ -1.0e4f, -1.0e4f };
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            const int idx = static_cast<int>(i);
            items_[i]->tick(dt, hovered == idx, ring_ && focus_ == idx, hasPointer_ ? pointer_ : far);
        }
        dirty_ = false;
    }

    void Panel::idle(double nowSec)
    {
        if (gestures_)
            gestures_->poll(nowSec);                     // closes a wheel gesture after its burst
    }

    bool Panel::wantsFullRate() const
    {
        if (dirty_)
            return true;
        return std::any_of(items_.begin(), items_.end(), [](const auto& it) { return !it->settled(); });
    }

    void Panel::draw(funkgui::Canvas& c, const funkgui::Theme& hostTheme)
    {
        const funkgui::Theme th = productTheme(hostTheme);
        const float contentW = width_ - 2.0f * kMargin;

        // Header: the name, the kind and version, the rules.
        {
            const funkgui::Canvas::Scope s(c, funkgui::tags::none, false);
            c.text(title_.c_str(), kMargin, kTitleTop, funkgui::type::kWordmark, th.ink100);
            const float tw = c.textWidth(title_.c_str(), funkgui::type::kWordmark);
            c.text(subtitle_.c_str(), kMargin + tw + 14.0f,
                   c.sharedBaselineTop(kTitleTop, funkgui::type::kWordmark, funkgui::type::kMicro),
                   funkgui::type::kMicro, th.ink32);
            c.hairlineH(kMargin, kHeaderRuleY, contentW, th.ink16);
            c.hairlineH(kMargin, ruleY_, contentW, th.ink16);
            c.hairlineH(kMargin, height_ - kFooterH, contentW, th.ink16);
        }

        for (std::size_t i = 0; i < items_.size(); ++i)
            if (items_[i]->shown())
                items_[i]->draw(c, th, ring_ && focus_ == static_cast<int>(i));

        // Footer: the spec line of the control under the hand, else the focused one, else the last one touched.
        int src = pressed_;
        if (src < 0 && hasPointer_)
            src = itemAt(pointer_);
        if (src < 0)
            src = ring_ ? focus_ : touched_;
        if (src >= 0)
        {
            char line[256];
            items_[static_cast<std::size_t>(src)]->specLine(line, sizeof line);
            const float zoomW = items_.back()->shown() ? 44.0f * ZoomModel::kMaxSteps + 12.0f : 0.0f;
            const std::string shown = fitFields(c, line, contentW - zoomW);
            const funkgui::Canvas::Scope s(c, funkgui::tags::hint, false);
            c.text(shown.c_str(), kMargin, height_ - 24.0f, funkgui::type::kMicro, th.ink52);
        }
    }

    // ---- input --------------------------------------------------------------------------------------------------------
    int Panel::itemAt(funkgui::Point p) const
    {
        for (std::size_t i = 0; i < items_.size(); ++i)
            if (items_[i]->shown() && items_[i]->contains(p))
                return static_cast<int>(i);
        return -1;
    }

    void Panel::pointerMove(const funkgui::PointerEvent& e)
    {
        const int before = hasPointer_ ? itemAt(pointer_) : -1;
        pointer_ = { e.x, e.y };
        hasPointer_ = true;
        const int now = itemAt(pointer_);
        if (before >= 0 && before != now)
            items_[static_cast<std::size_t>(before)]->pointerExit();
        if (now >= 0)
            items_[static_cast<std::size_t>(now)]->pointerMove(e);
        dirty_ = true;
    }

    void Panel::pointerExit()
    {
        if (hasPointer_)
            if (const int before = itemAt(pointer_); before >= 0)
                items_[static_cast<std::size_t>(before)]->pointerExit();
        hasPointer_ = false;
        dirty_ = true;
    }

    void Panel::pointerDown(const funkgui::PointerEvent& e)
    {
        ring_ = false;                                   // the pointer is the affordance now
        pointer_ = { e.x, e.y };
        hasPointer_ = true;
        pressed_ = itemAt(pointer_);
        if (pressed_ >= 0 && gestures_ && host_ != nullptr)
        {
            touched_ = pressed_;
            if (items_[static_cast<std::size_t>(pressed_)]->focusable())
                focus_ = pressed_;
            items_[static_cast<std::size_t>(pressed_)]->pointerDown(e, *gestures_, *host_);
        }
        dirty_ = true;
    }

    void Panel::pointerDrag(const funkgui::PointerEvent& e)
    {
        pointer_ = { e.x, e.y };
        if (pressed_ >= 0 && gestures_)
            items_[static_cast<std::size_t>(pressed_)]->pointerDrag(e, *gestures_);
        dirty_ = true;
    }

    void Panel::pointerUp(const funkgui::PointerEvent& e)
    {
        if (pressed_ >= 0 && gestures_)
            items_[static_cast<std::size_t>(pressed_)]->pointerUp(e, *gestures_);
        pressed_ = -1;
        dirty_ = true;
    }

    void Panel::doubleClick(const funkgui::PointerEvent& e)
    {
        const int i = itemAt({ e.x, e.y });
        if (i >= 0 && gestures_)
        {
            touched_ = i;
            items_[static_cast<std::size_t>(i)]->doubleClick(e, *gestures_);   // a control: back to the default
        }
        dirty_ = true;
    }

    bool Panel::wheel(const funkgui::WheelEvent& e)
    {
        dirty_ = true;
        const int i = itemAt({ e.x, e.y });
        if (i < 0 || !gestures_ || host_ == nullptr)
            return false;
        touched_ = i;
        return items_[static_cast<std::size_t>(i)]->wheel(e, *gestures_, host_->nowSeconds());
    }

    bool Panel::moveFocus(int dir)
    {
        const int n = static_cast<int>(items_.size());
        int i = focus_;
        for (int k = 0; k < n; ++k)
        {
            i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
            if (items_[static_cast<std::size_t>(i)]->shown() && items_[static_cast<std::size_t>(i)]->focusable())
            {
                focus_ = i;
                touched_ = i;
                return true;
            }
        }
        return false;
    }

    bool Panel::key(const funkgui::KeyEvent& e)
    {
        dirty_ = true;
        if (e.key == funkgui::Key::tab)
        {
            ring_ = moveFocus(e.mods.shift ? -1 : 1);
            return ring_;
        }
        if (e.key == funkgui::Key::escape && ring_)
        {
            ring_ = false;
            return true;
        }
        if (focus_ < 0 || !gestures_)
            return false;
        return items_[static_cast<std::size_t>(focus_)]->key(e, *gestures_);
    }

    funkgui::Cursor Panel::cursor() const
    {
        const int i = pressed_ >= 0 ? pressed_ : (hasPointer_ ? itemAt(pointer_) : -1);
        return i >= 0 ? items_[static_cast<std::size_t>(i)]->cursorAt(pointer_) : funkgui::Cursor::normal;
    }

    // ---- accessibility ------------------------------------------------------------------------------------------------
    void Panel::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        for (const auto& item : items_)
            if (item->shown())
                item->accessibility(out);
    }

    uint32_t Panel::a11yRevision() const
    {
        return items_.back()->shown() ? 2u : 1u;         // the ZOOM cells come and go with the host's zoom
    }

    void Panel::a11yAction(uint32_t id, funkgui::A11yAction a, double value)
    {
        if (!gestures_)
            return;
        for (const auto& item : items_)
            if (item->shown() && item->a11yAction(id, a, value, *gestures_))
                break;
        dirty_ = true;
    }

    void Panel::closeGestures()
    {
        if (gestures_)
            gestures_->closeAll();
    }
}
