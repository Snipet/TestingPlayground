// Source/editor/Panel.cpp: see Panel.h.
#include "editor/Panel.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/ChatView.h"
#include "editor/CodeAssist.h"
#include "editor/DialLayout.h"
#include "editor/DisplayItem.h"
#include "editor/GuiItems.h"
#include "editor/Looks.h"
#include "editor/Items.h"

#include "editor/Theme.h"

#include "plugin/EditorPort.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "fminus/Random.h"

#include "Examples.h"
#include "Library.h"
#include "Product.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Images.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/Highlighter.h>
#include <funkgui/text/LineEdit.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/Dial.h>
#include <funkgui/widgets/SegmentedSelector.h>
#include <funkgui/widgets/TextEditor.h>
#include <funkgui/widgets/ThemeCells.h>
#include <funkgui/widgets/ValueModel.h>

#include <algorithm>
#include <map>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <functional>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace playground::ui
{

    namespace
    {
        // A count as the account's view shows it: 900, 12.3K, 123K, 1.2M.
        std::string countText(int64_t n)
        {
            char buf[32];
            if (n < 1000)
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
            else if (n < 100000)
                std::snprintf(buf, sizeof buf, "%.1fK", static_cast<double>(n) / 1e3);
            else if (n < 1000000)
                std::snprintf(buf, sizeof buf, "%.0fK", static_cast<double>(n) / 1e3);
            else
                std::snprintf(buf, sizeof buf, "%.1fM", static_cast<double>(n) / 1e6);
            return buf;
        }

        // An ISO 8601 date ("2026-10-26T00:00:00.000Z") as "26 Oct 2026"; anything else as it is.
        std::string shortDate(const std::string& iso)
        {
            static constexpr const char* kMonths[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
            int y = 0, m = 0, d = 0;
            if (iso.size() < 10 || std::sscanf(iso.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12)
                return iso;
            return std::to_string(d) + " " + kMonths[m - 1] + " " + std::to_string(y);
        }
    }

    std::vector<std::string> guiPageTitles(const GuiTree& gui, const std::vector<int>& pages)
    {
        std::vector<std::string> titles;
        for (std::size_t i = 0; i < gui.nodes.size(); ++i)
        {
            const GuiNode& n = gui.nodes[i];
            if (n.kind != GuiKind::tabs || n.children.empty())
                continue;
            const int page = i < pages.size() ? pages[i] : 0;
            const int k = page >= 0 && page < static_cast<int>(n.children.size()) ? page : 0;
            titles.push_back(gui.nodes[static_cast<std::size_t>(n.children[static_cast<std::size_t>(k)])].text);
        }
        return titles;
    }

    std::vector<int> guiPagesOf(const GuiTree& gui, const std::vector<std::string>& titles)
    {
        std::vector<int> pages(gui.nodes.size(), 0);
        std::size_t t = 0;
        for (std::size_t i = 0; i < gui.nodes.size(); ++i)
        {
            const GuiNode& n = gui.nodes[i];
            if (n.kind != GuiKind::tabs)
                continue;
            if (t < titles.size())
                for (std::size_t k = 0; k < n.children.size(); ++k)
                    if (gui.nodes[static_cast<std::size_t>(n.children[k])].text == titles[t])
                    {
                        pages[i] = static_cast<int>(k);
                        break;
                    }
            ++t;
        }
        return pages;
    }

    std::vector<Problem> parseProblems(const std::string& messages)
    {
        std::vector<Problem> out;
        std::istringstream in(messages);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty())
                continue;
            // clang's include stack, before a message in an included file: the message names its file itself.
            if (line.rfind("In file included from ", 0) == 0)
                continue;
            Problem p;
            // "<file>:<line>:<col>: <kind>: <text>" or "<file>: <kind>: <text>"
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos)
                p.file = line.substr(0, colon);
            std::string rest = colon == std::string::npos ? line : line.substr(colon + 1);
            int ln = 0, col = 0, used = 0;
            if (std::sscanf(rest.c_str(), "%d:%d: %n", &ln, &col, &used) >= 2 && used > 0)
            {
                p.line = ln;
                p.column = col;
                rest = rest.substr(static_cast<std::size_t>(used));
            }
            while (!rest.empty() && rest.front() == ' ')
                rest.erase(rest.begin());
            if (rest.rfind("error: ", 0) == 0)
                rest = rest.substr(7);
            else if (rest.rfind("note: ", 0) == 0)
            {
                p.error = false;
                rest = rest.substr(6);
            }
            else if (rest.rfind("warning: ", 0) == 0)
            {
                p.error = false;
                p.warning = true;
                rest = rest.substr(9);
            }
            p.text = rest;
            out.push_back(std::move(p));
        }
        return out;
    }

    namespace
    {
        // The problems as the code editor marks them.
        std::vector<Diagnostic> diagnosticsOf(const std::vector<Problem>& problems)
        {
            std::vector<Diagnostic> out;
            for (const Problem& p : problems)
            {
                Diagnostic d;
                d.file = p.file;
                d.line = p.line;
                d.column = p.column;
                d.severity = p.error ? Diagnostic::Severity::error : p.warning ? Diagnostic::Severity::warning : Diagnostic::Severity::info;
                d.text = p.text;
                out.push_back(std::move(d));
            }
            return out;
        }

        // ---- layout (logical px) ----------------------------------------------------------------------------------
        constexpr float kMargin = 20.0f;
        // The top bar: the product, the project, the view, the load and the theme. Under it two cards: the program's
        // (its tabs and actions, the editor or the panel, the problems, the dials) and the chat's.
        constexpr float kTopBarY = 10.0f, kTopBarH = 40.0f;
        constexpr float kTopButtonH = 28.0f;             // the top bar's pills
        constexpr float kMainY = kTopBarY + kTopBarH + 10.0f;   // the cards' top
        constexpr float kInset = 10.0f;                  // inside the program's card
        constexpr float kActionH = 28.0f;                // its header's first row: the status and the actions
        constexpr float kActionsY = kMainY + 6.0f;
        constexpr float kTabsY = kActionsY + kActionH + 4.0f;   // its second: the files' tabs, over the editor
        constexpr float kEditorY = kTabsY + 26.0f + 6.0f;
        constexpr float kSegmentW = 84.0f;               // a half of the Code / Panel switch
        constexpr float kProjectW = 340.0f;              // the project pill, at most
        constexpr float kProblemRowH = 15.0f;
        constexpr float kPlaceW = 150.0f;                // a problem's file, line and column
        constexpr int   kProblemRows = 4;
        constexpr float kFooterH = 30.0f;
        constexpr float kBottomGap = 10.0f;              // over the status bar
        // The chat's column, at the right.
        constexpr float kChatShare = 0.32f;              // of the width
        constexpr float kGuiScrollGutter = 8.0f;         // a .gui interface's scroll bar, at its area's right
        constexpr float kChatMinW = 330.0f;
        constexpr float kChatMaxW = 560.0f;
        constexpr float kChatGap = 12.0f;                // between the two cards
        constexpr float kChatRailW = 44.0f;              // the chat folded: its rail's width
        constexpr float kChatRailGap = 12.0f;            // and the gap before it
        constexpr float kExplorerShare = 0.16f;          // the explorer, open: of the width
        constexpr float kExplorerMinW = 180.0f;
        constexpr float kExplorerMaxW = 250.0f;
        constexpr float kExplorerGap = 12.0f;            // between it and the program's card
        constexpr float kProgramMinW = 600.0f;           // the program's card keeps this much beside an open explorer
        constexpr float kCardPad = 10.0f;                // inside the chat's card
        constexpr float kCardRadius = 14.0f;
        constexpr float kIconButton = 28.0f;             // the header's new chat and key
        constexpr float kBarH = 36.0f;                   // the message box's bottom bar (the pills, send)
        constexpr float kPillH = 26.0f;
        constexpr float kSendD = 28.0f;
        constexpr float kUsageH = 16.0f;                 // the last reply's tokens, under the message box
        constexpr uint32_t kChatA11yBase = 0x20000000u;  // the conversation's accessibility ids (ChatView)
        constexpr uint32_t kExplorerA11yBase = 0x30000000u;   // the explorer's (Explorer.h)
        // The key dialog.
        constexpr float kKeyDialogW = 540.0f;
        constexpr float kKeyDialogH = 296.0f;
        constexpr float kAccountDialogH = 404.0f;        // the account tab, signed in: this month's use under the card
        constexpr int   kMaxKeyLength = 320;
        constexpr int   kMaxUsernameLength = 64;         // the server's limits
        constexpr int   kMaxPasswordLength = 256;
        constexpr uint32_t kA11yStride = 64;             // item i's ids: kA11yStride * (i + 1) + 0..63

        // Where a new #include goes in a program: after the last #include before its first line of code, else after
        // the comment lines it starts with (its description), else first.
        int includeLineFor(const std::string& text)
        {
            std::istringstream in(text);
            std::string l;
            int row = 0, lastInclude = -1, comments = 0;
            bool leading = true;
            while (std::getline(in, l))
            {
                const std::size_t first = l.find_first_not_of(" \t\r");
                const std::string_view t = first == std::string::npos ? std::string_view{} : std::string_view(l).substr(first);
                const bool comment = t.rfind("//", 0) == 0;
                if (t.rfind("#include", 0) == 0)
                    lastInclude = row;
                else if (!t.empty() && !comment && t[0] != '#')
                    break;                               // the first line of code
                if (leading && comment)
                    comments = row + 1;
                leading = leading && comment;
                ++row;
            }
            return lastInclude >= 0 ? lastInclude + 1 : comments;
        }

        // A section's title, cut into its frame's top edge (DialLayout.h).
        constexpr funkgui::TextStyle kSectionTitle{ 10.0f, 2.4f, 0.10f, false };

        // ---- a dial over P1..P32, labelled with the running program's parameter (or its #pragma panel label) --------
        class ControlModel final : public funkgui::ValueModel
        {
        public:
            explicit ControlModel(funkgui::ParamPort& port) : port_(port) {}

            void setControl(const Control& c, const std::string& label, const std::vector<std::string>& options,
                            uint32_t revision)
            {
                control_ = c;
                revision_ = revision;
                options_.clear();
                for (const std::string& o : options)
                    options_.push_back(upper(asciiText(o)));
                const std::string name = upper(asciiText(label.empty() ? std::string(c.name) : label));
                std::snprintf(label_, sizeof label_, "%s", name.c_str());
            }
            const Control& control() const noexcept { return control_; }
            const char* label() const noexcept { return label_; }

            // An int's or a bool's values: how many (0 for a real dial), which one it is at, and each one's name.
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
            float normOfIndex(int k) const { return normOf(control_, control_.min + static_cast<float>(k)); }
            float value01() const
            {
                const float raw = port_.value01();
                if (std::isfinite(raw))
                    last01_ = raw;
                return last01_;
            }
            bool atDefault() const { return std::fabs(plainOf(control_, value01()) - control_.def) < 1e-6f; }
            std::string valueText() const
            {
                funkgui::ValueView v;
                view(v);
                return v.text.value;
            }

            uint64_t key() const override
            {
                return (static_cast<uint64_t>(revision_) << 32) | std::bit_cast<uint32_t>(port_.value01());
            }
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
                        formatValue(control_, p, v.text.value, sizeof v.text.value);   // its unit, decimals and scale
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
            uint32_t revision_ = 0;
            char label_[40] = {};
            std::vector<std::string> options_;
            mutable float last01_ = 0.5f;                // the last finite value01 seen: a host's NaN shows it
        };

        // A dial in its style (DialLayout.h's cell, Looks.h's drawing). FunkGui's Dial takes a knob's, a vintage knob's
        // and a fader's input (a drag up raises it, the wheel, the keys, a double-click for the default) and every
        // style's accessibility; a switch flips and a button chooses its value on a click, one gesture each.
        class DialItem final : public Item
        {
        public:
            DialItem(funkgui::ParamPort& port, funkgui::DialGeom g, uint32_t id) : model_(port), dial_(model_, g, id) {}

            ControlModel& model() noexcept { return model_; }
            const ControlModel& model() const noexcept { return model_; }
            DialStyle style() const noexcept { return style_; }
            const funkgui::Rect& cell() const noexcept { return cell_; }

            // Its style, colour and place.
            void setLook(DialStyle style, const std::string& color, const ItemPlace& p)
            {
                style_ = style;
                color_ = color;
                small_ = p.small;
                cell_ = { p.x, p.top, p.w, p.h };
                if (style == DialStyle::knob || style == DialStyle::vintage)
                    dial_.setGeom(funkgui::DialGeom{ p.x, p.top, p.small ? funkgui::DialSize::compact : funkgui::DialSize::primary });
                else
                    dial_.setGeom(funkgui::DialGeom{ std::round(cell_.centreX() - 0.5f * kDialW), p.top, funkgui::DialSize::primary });
            }

            void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) override
            {
                dial_.tick(dt, hovered, focused, false);
                hovered_ = hovered;
                hoverRow_ = hovered && style_ == DialStyle::buttons ? rowAt(pointer) : -1;
            }
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override
            {
                const bool tinted = !color_.empty();
                const funkgui::Col col = looks::panelColor(color_, th, th.accent);
                if (style_ == DialStyle::knob)
                {
                    if (!tinted)
                        return dial_.draw(c, th, ring);
                    funkgui::Theme t = th;
                    t.accent = col;
                    t.accentDim = funkgui::premix(th.ground, col, 0.4f);
                    return dial_.draw(c, t, ring);
                }
                const std::string value = model_.valueText();
                looks::Knob k;
                k.cell = cell_;
                k.small = small_;
                k.t = model_.value01();
                k.label = model_.label();
                k.value = value.c_str();
                k.atDefault = model_.atDefault();
                k.hover = hovered_;
                k.focus = ring;
                k.color = col;
                k.tinted = tinted;
                switch (style_)
                {
                    case DialStyle::knob:
                    case DialStyle::vintage: looks::vintage(c, th, k); break;
                    case DialStyle::fader:   looks::fader(c, th, k); break;
                    case DialStyle::toggle:  looks::toggle(c, th, k, value.c_str()); break;
                    case DialStyle::buttons:
                    {
                        std::vector<std::string> names;
                        for (int i = 0; i < std::min(model_.count(), kMaxOptions); ++i)
                            names.push_back(model_.nameOf(i));
                        looks::buttons(c, th, k, names, model_.index(), hoverRow_);
                        break;
                    }
                }
            }
            bool contains(funkgui::Point p) const override
            {
                return style_ == DialStyle::knob ? dial_.contains(p) : cell_.contains(p);
            }
            funkgui::Cursor cursorAt(funkgui::Point p) const override
            {
                if (style_ == DialStyle::toggle || style_ == DialStyle::buttons)
                    return funkgui::Cursor::pointingHand;
                return dial_.cursorAt(p);
            }
            void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g,
                             funkgui::HostServices& h) override
            {
                if (style_ == DialStyle::toggle)
                {
                    g.tap(*model_.port(), model_.normOfIndex(model_.index() == 0 ? model_.count() - 1 : 0));
                    return;
                }
                if (style_ == DialStyle::buttons)
                {
                    if (const int row = rowAt({ e.x, e.y }); row >= 0)
                        g.tap(*model_.port(), model_.normOfIndex(row));
                    return;
                }
                dial_.pointerDown(e, g, h);
            }
            void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g) override
            {
                if (clicks())
                    return;
                dial_.pointerDrag(e, g);
            }
            void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController& g) override
            {
                if (clicks())
                    return;
                dial_.pointerUp(e, g);
            }
            void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController& g) override
            {
                if (!clicks())
                    dial_.doubleClick(g);
            }
            bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController& g, double now) override
            {
                return dial_.wheel(e, g, now);
            }
            bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g) override
            {
                if (clicks() && (e.key == funkgui::Key::enter || e.key == funkgui::Key::space))
                {
                    const int n = std::max(model_.count(), 1);
                    g.tap(*model_.port(), model_.normOfIndex((model_.index() + 1) % n));
                    return true;
                }
                return dial_.key(e, g);
            }
            void accessibility(std::vector<funkgui::A11yItem>& out) const override
            {
                funkgui::A11yItem item;
                dial_.accessibility(item);
                if (style_ != DialStyle::knob)
                    item.bounds = cell_;
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
            funkgui::Point centre() const override
            {
                return style_ == DialStyle::knob ? dial_.geom().centre() : funkgui::Point{ cell_.centreX(), cell_.centreY() };
            }
            bool shown() const override { return model_.control().used && !hidden_; }
            void setHidden(bool hidden) { hidden_ = hidden; }   // a .gui file's interface draws the dials itself

            // A button's row under p (buttons), else -1.
            int rowAt(funkgui::Point p) const
            {
                if (!cell_.contains(p))
                    return -1;
                const int row = static_cast<int>(std::floor((p.y - cell_.y - kButtonsTop) / kButtonRowH));
                return row >= 0 && row < std::min(model_.count(), kMaxOptions) ? row : -1;
            }

        private:
            bool clicks() const noexcept { return style_ == DialStyle::toggle || style_ == DialStyle::buttons; }

            ControlModel model_;                         // before dial_, which refers to it
            funkgui::Dial dial_;
            DialStyle style_ = DialStyle::knob;
            std::string color_;
            bool small_ = false;
            funkgui::Rect cell_;
            bool hovered_ = false;
            int hoverRow_ = -1;
            bool hidden_ = false;
        };

        // ---- a row of cells: THEME, ZOOM, and the RUN and EXAMPLES buttons ------------------------------------------
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

        // One cell that acts when chosen: RUN, EXAMPLES.
        // The toolbar's CODE and PANEL cells: which view the left column shows.
        class ViewModel final : public funkgui::CellModel
        {
        public:
            ViewModel(std::function<bool()> panel, std::function<void(bool)> set) : panel_(std::move(panel)), set_(std::move(set)) {}

            int count() const override { return 2; }
            int active() const override { return panel_() ? 1 : 0; }
            const char* label(int i) const override { return i == 0 ? "CODE" : "PANEL"; }
            const char* spoken(int i) const override { return i == 0 ? "Code view" : "Panel view"; }
            void select(int i, funkgui::GestureController&) override { set_(i == 1); }

        private:
            std::function<bool()> panel_;
            std::function<void(bool)> set_;
        };

        class ButtonModel final : public funkgui::CellModel
        {
        public:
            ButtonModel(const char* label, const char* spoken, std::function<void()> act)
                : label_(label), spoken_(spoken), act_(std::move(act)) {}

            int count() const override { return 1; }
            int active() const override { return -1; }
            const char* label(int) const override { return label_; }
            const char* spoken(int) const override { return spoken_; }
            void select(int, funkgui::GestureController&) override
            {
                if (act_)
                    act_();
            }

        private:
            const char* label_;
            const char* spoken_;
            std::function<void()> act_;
        };

        class CellsItem final : public Item
        {
        public:
            using SpecFn = std::function<void(char*, std::size_t)>;
            using ShownFn = std::function<bool()>;

            CellsItem(std::unique_ptr<funkgui::CellModel> model, std::vector<funkgui::Rect> cells,
                      funkgui::CellStyle style, uint32_t id, SpecFn spec, ShownFn shown = {})
                : model_(std::move(model)),
                  own_(std::make_unique<funkgui::SegmentedSelector>(*model_, std::move(cells), style, nullptr,
                                                                    funkgui::Point{}, id)),
                  sel_(own_.get()), spec_(std::move(spec)), shown_(std::move(shown)) {}

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
            funkgui::Rect bounds() const { return sel_->bounds(); }
            void setCells(std::vector<funkgui::Rect> cells) { sel_->setCells(std::move(cells)); }
            bool shown() const override { return !shown_ || shown_(); }

        private:
            std::unique_ptr<funkgui::CellModel> model_;
            std::unique_ptr<funkgui::SegmentedSelector> own_;
            std::unique_ptr<funkgui::ThemeCells> theme_;
            funkgui::SegmentedSelector* sel_ = nullptr;
            SpecFn spec_;
            ShownFn shown_;
        };

        // ---- the program's files: a tab each, over the editor of the one shown ----------------------------------------
        // The tab bar: a tab per file (the program's first, which cannot be closed; the others with a close box after
        // their names), then the new-file box. A click shows a file, a double-click edits its name in place (Return
        // keeps it, Escape cancels), the close box closes it (asking first when it has text). The tab of a file the
        // last run's errors name has a mark before its name. Each file has an editor of its own, so its caret, scroll
        // and undo steps stay with it; CodeAssist.h makes them a code editor (the status line under them, the find bar,
        // the completion list, the signature and the hover card, the problems' squiggles and marks).
        constexpr float kTabH = 26.0f;
        constexpr float kTabPad = 12.0f;                 // before a tab's name, and after it
        constexpr float kTabCloseW = 14.0f;              // a closable tab's close box, after its name
        constexpr float kTabMinW = 64.0f;
        constexpr float kTabNarrowW = 36.0f;             // the least a tab narrows to when many do not fit (twelve fit
                                                         // a program's card of kProgramMinW)
        constexpr float kTabMaxW = 150.0f;
        constexpr float kTabGap = 4.0f;
        constexpr float kNewTabW = 24.0f;
        constexpr uint64_t kStructureStep = uint64_t{ 1 } << 40;   // FilesItem::revision: a change of the tabs
    }

    // Not in the unnamed namespace: Panel.h names it.
    class FilesItem final : public Item
    {
    public:
        using Close = std::function<void(int tab)>;  // a closable tab's close box: the Panel asks, then remove()

        FilesItem(funkgui::Rect bar, funkgui::Rect editor, uint32_t id, const std::vector<Problem>& problems,
                  Close close)
            : bar_(bar), editorBounds_(editor), id_(id), problems_(problems), close_(std::move(close)),
              assist_(CodeAssist::Tabs{
                  [this] { return count(); }, [this] { return active(); },
                  [this](int i) -> funkgui::TextEditor& { return editorOf(i); }, [this](int i) { return name(i); },
                  [this](int i) { select(i); },
                  [this](const std::string& n, const std::string& text) { return addFileQuiet(n, text); },
                  [this](const std::string& text) {
                      if (notice_)
                          notice_(text);
                  } })
        {
            assist_.setArea(editor);
            load(Draft{ singleFile(""), 0 });
        }

        CodeAssist& assist() noexcept { return assist_; }
        const CodeAssist& assist() const noexcept { return assist_; }
        void setNotice(std::function<void(const std::string&)> notice) { notice_ = std::move(notice); }
        // The completion list, the signature and the hover card, over everything (the Panel draws them last).
        void drawOverlays(funkgui::Canvas& c, const funkgui::Theme& th, float w, float h) const
        {
            if (!hidden_)
                assist_.drawOverlays(c, th, w, h);
        }

        funkgui::TextEditor& editor() noexcept { return *tabs_[static_cast<std::size_t>(active_)].editor; }
        const funkgui::TextEditor& editor() const noexcept
        {
            return *tabs_[static_cast<std::size_t>(active_)].editor;
        }
        int active() const noexcept { return active_; }
        int count() const noexcept { return static_cast<int>(tabs_.size()); }
        const std::string& name(int i) const { return tabs_[static_cast<std::size_t>(i)].name; }
        int find(std::string_view n) const
        {
            for (int i = 0; i < count(); ++i)
                if (name(i) == n)
                    return i;
            return -1;
        }
        bool renaming() const noexcept { return renaming_ >= 0; }
        // Bumps when a tab comes, goes, is renamed or shown (not when text changes).
        uint32_t layoutRevision() const noexcept { return static_cast<uint32_t>(structure_ / kStructureStep); }

        // Bumps whenever a file's text, a name, the files or the tab shown change.
        uint64_t revision() const noexcept
        {
            uint64_t r = structure_;
            for (const Tab& t : tabs_)
                r += t.editor->revision();
            return r;
        }

        Draft draft() const
        {
            Draft d;
            d.active = active_;
            for (const Tab& t : tabs_)
                d.files.push_back({ t.name, t.editor->text() });
            return d;
        }

        // Every tab anew: the processor's draft (a host loaded a session, or the editor opened).
        void load(const Draft& d)
        {
            renaming_ = -1;
            why_.clear();
            tabs_.clear();
            for (const SourceFile& f : d.files)
                if (tabs_.size() < kMaxFiles)
                    tabs_.push_back(makeTab(f.name, f.text));
            if (tabs_.empty())
                tabs_.push_back(makeTab(kProgramName, ""));
            active_ = std::clamp(d.active, 0, count() - 1);
            show();
            structure_ += kStructureStep;
        }

        // The processor's draft into the tabs (a host loaded a session, or Claude changed the files): a file whose tab
        // is open keeps its editor (its caret, scroll and undo steps), its text replaced as one undoable edit when it
        // changed; a file at the place of a tab whose name is gone is that file renamed; a new file gets a tab, and a
        // tab whose file is gone closes. The tabs follow the draft's order and the tab it shows.
        void merge(const Draft& d)
        {
            renaming_ = -1;
            why_.clear();
            editor().setFocused(false);
            std::vector<Tab> old = std::move(tabs_);
            tabs_.clear();
            const std::size_t n = std::min(d.files.size(), kMaxFiles);
            std::vector<int> match(n, -1);
            std::vector<bool> used(old.size(), false);
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t k = 0; k < old.size(); ++k)
                    if (!used[k] && old[k].name == d.files[i].name)
                    {
                        match[i] = static_cast<int>(k);
                        used[k] = true;
                        break;
                    }
            for (std::size_t i = 0; i < n; ++i)
                if (match[i] < 0 && i < old.size() && !used[i]
                    && std::none_of(d.files.begin(), d.files.end(),
                                    [&](const SourceFile& f) { return f.name == old[i].name; }))
                {
                    match[i] = static_cast<int>(i);
                    used[i] = true;
                }
            for (std::size_t i = 0; i < n; ++i)
            {
                const SourceFile& f = d.files[i];
                if (match[i] < 0)
                {
                    tabs_.push_back(makeTab(f.name, f.text));
                    continue;
                }
                Tab t = std::move(old[static_cast<std::size_t>(match[i])]);
                if (rulesFor(t.name).types != rulesFor(f.name).types)   // renamed to or from a .gui file
                    assist_.configure(*t.editor, f.name);
                t.name = f.name;
                replaceText(*t.editor, f.text);
                tabs_.push_back(std::move(t));
            }
            if (tabs_.empty())
                tabs_.push_back(makeTab(kProgramName, ""));
            active_ = std::clamp(d.active, 0, count() - 1);
            show();
            structure_ += kStructureStep;
        }

        // Where the tabs and the editors are (the Panel's layout).
        void setGeometry(funkgui::Rect bar, funkgui::Rect editorBounds)
        {
            bar_ = bar;
            editorBounds_ = editorBounds;
            assist_.setArea(editorBounds_);
            for (Tab& t : tabs_)
                t.editor->setBounds(assist_.editorRect());
        }

        void attach(funkgui::HostServices* host)
        {
            host_ = host;
            assist_.attach(host);
            for (Tab& t : tabs_)
                t.editor->attach(host);
        }

        void select(int i)
        {
            if (i < 0 || i >= count() || i == active_)
                return;
            editor().setFocused(false);
            active_ = i;
            show();
            assist_.tabShown();
            structure_ += kStructureStep;
        }

        // A new file after the others, shown, its name being edited. False when there is no room for its tab.
        bool add()
        {
            if (tabs_.size() >= kMaxFiles)
                return false;
            endRename();
            Sources files;
            for (const Tab& t : tabs_)
                files.push_back({ t.name, {} });
            tabs_.push_back(makeTab(freshFileName(files), ""));
            select(count() - 1);
            beginRename(count() - 1);
            structure_ += kStructureStep;
            return true;
        }

        // A file after the others with this name and text, shown. False when there is no room or the name is taken.
        bool addFile(const std::string& fileName, const std::string& text)
        {
            if (tabs_.size() >= kMaxFiles || find(fileName) >= 0)
                return false;
            endRename();
            tabs_.push_back(makeTab(fileName, text));
            select(count() - 1);
            structure_ += kStructureStep;
            return true;
        }

        funkgui::TextEditor& editorOf(int i) { return *tabs_[static_cast<std::size_t>(i)].editor; }

        // A file after the others with this name and text, not shown (a library file completion adds). False when there
        // is no room or the name is taken.
        bool addFileQuiet(const std::string& fileName, const std::string& text)
        {
            if (tabs_.size() >= kMaxFiles || find(fileName) >= 0)
                return false;
            tabs_.push_back(makeTab(fileName, text));
            structure_ += kStructureStep;
            return true;
        }

        // Closes file i (not the program's); the tab before it is shown when it was.
        void remove(int i)
        {
            if (i <= 0 || i >= count())
                return;
            if (renaming_ == i)
                renaming_ = -1;
            else if (renaming_ > i)
                --renaming_;
            const bool focused = editor().focused();
            editor().setFocused(false);
            tabs_.erase(tabs_.begin() + i);
            if (active_ >= i)
                active_ = std::max(0, active_ - 1);
            show();
            editor().setFocused(focused);
            structure_ += kStructureStep;
        }

        void beginRename(int i)
        {
            if (i < 0 || i >= count())
                return;
            renaming_ = i;
            why_.clear();
            edit_.set(name(i), static_cast<int>(kMaxNameLength));
        }

        // Return: the name typed, when it can name the file; else the edit goes on and says why not.
        bool commitRename()
        {
            if (renaming_ < 0)
                return true;
            const std::string& typed = edit_.buffer;
            if (typed != name(renaming_))
            {
                if (!validFileName(typed))
                {
                    why_ = "A NAME IS LETTERS, DIGITS, _, - AND ., NOT STARTING WITH .";
                    return false;
                }
                if (find(typed) >= 0)
                {
                    why_ = "ANOTHER FILE HAS THAT NAME";
                    return false;
                }
                Tab& tab = tabs_[static_cast<std::size_t>(renaming_)];
                if (rulesFor(tab.name).types != rulesFor(typed).types)
                    assist_.configure(*tab.editor, typed);
                tab.name = typed;
                structure_ += kStructureStep;
            }
            renaming_ = -1;
            why_.clear();
            return true;
        }

        // A press elsewhere: the name typed when it can name the file, else the one before.
        void endRename()
        {
            if (!commitRename())
            {
                renaming_ = -1;
                why_.clear();
            }
        }

        // ---- geometry (logical px): the tabs from the bar's left, each as wide as its name -------------------
        funkgui::Rect tabRect(int i) const
        {
            float x = bar_.x;
            for (int k = 0; k < i; ++k)
                x += tabWidth(k) + kTabGap;
            return { x, bar_.y, tabWidth(i), kTabH };
        }
        funkgui::Rect closeRect(int i) const
        {
            const funkgui::Rect t = tabRect(i);
            return { t.right() - tabPad(i) - kTabCloseW + 4.0f, t.y + 4.0f, kTabCloseW, kTabH - 8.0f };
        }
        // A tab narrowed to fit: less padding, and its close box only while it is shown or under the pointer.
        bool narrow(int i) const { return tabWidth(i) < naturalWidth(i) - 0.5f; }
        float tabPad(int i) const { return narrow(i) ? 6.0f : kTabPad; }
        bool closeShown(int i) const
        {
            return i > 0 && (!narrow(i) || i == active_ || tabRect(i).contains(pointer_));
        }
        funkgui::Rect newTabRect() const
        {
            const funkgui::Rect last = tabRect(count() - 1);
            return { last.right() + kTabGap, bar_.y, kNewTabW, kTabH };
        }
        bool canAdd() const noexcept { return tabs_.size() < kMaxFiles; }

        // ---- Item --------------------------------------------------------------------------------------------
        void tick(float dt, bool, bool, funkgui::Point pointer) override
        {
            pointer_ = pointer;
            editor().tick(dt);
            assist_.tick(dt, pointer, focused_);
        }
        bool settled() const override { return assist_.settled(); }

        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override
        {
            namespace type = funkgui::type;
            const funkgui::TextEditor& ed = editor();
            assist_.drawGround(c, th);
            ed.draw(c, th, ring);
            assist_.drawInEditor(c, th);

            // The tabs: pills, the shown one raised, a file the last run's errors name marked before its name.
            const funkgui::Canvas::Scope s(c, kTabTag, false);
            const chat::Palette p = chat::palette(th);
            const funkgui::TextStyle& st = chat::kSmall;
            for (int i = 0; i < count(); ++i)
            {
                const funkgui::Rect r = tabRect(i);
                const bool shown = i == active_;
                const bool hover = r.contains(pointer_);
                if (shown)
                    c.rrect(r.x, r.y, r.w, r.h, 8.0f, p.composer, 1.0f, p.composerEdge);
                else if (hover)
                    c.rrect(r.x, r.y, r.w, r.h, 8.0f, p.hover);
                const float textTop = c.capCentreTop(r.centreY(), st);
                if (hasErrors(name(i)))
                    c.disc(r.x + 6.0f, r.centreY(), 2.5f, p.danger);
                const float pad = tabPad(i);
                const float nameW = r.w - 2.0f * pad - (closeShown(i) ? kTabCloseW : 0.0f);
                if (i == renaming_)
                {
                    // The name being edited: its text, the selection (all of it, until a key), the caret.
                    const std::string& b = edit_.buffer;
                    const float w = c.textWidth(b.c_str(), st);
                    if (edit_.allSelected && !b.empty())
                        c.rrect(r.x + pad - 1.0f, r.y + 5.0f, std::min(w, nameW) + 2.0f, r.h - 10.0f, 2.0f, th.accentDim);
                    c.text(fitText(c, b, st, nameW).c_str(), r.x + pad, textTop, st, why_.empty() ? p.text : p.danger);
                    const std::string head = b.substr(0, static_cast<std::size_t>(std::clamp(
                                                             edit_.caret, 0, static_cast<int>(b.size()))));
                    const float cx = r.x + pad + std::min(c.textWidth(head.c_str(), st), nameW);
                    c.rrect(cx, r.y + 5.0f, 1.5f, r.h - 10.0f, 0.0f, p.accent);
                }
                else
                {
                    const funkgui::Col ink = shown || hover ? p.text : p.soft;
                    // A narrowed tab keeps the name's first letters, without an ellipsis (the footer has it whole).
                    std::string label = name(i);
                    if (narrow(i))
                        while (label.size() > 1 && c.textWidth(label.c_str(), st) > nameW)
                            label.pop_back();
                    else
                        label = fitText(c, label, st, nameW);
                    c.text(label.c_str(), r.x + pad, textTop, st, ink);
                }
                if (closeShown(i))
                    drawCross(c, closeRect(i), 3.0f, closeRect(i).contains(pointer_) ? p.text : p.faint);
            }
            if (canAdd())
            {
                const funkgui::Rect n = newTabRect();
                const bool hot = n.contains(pointer_);
                if (hot)
                    c.rrect(n.x, n.y, n.w, n.h, 8.0f, p.hover);
                chat::drawIcon(c, chat::Icon::plus, n.centreX(), n.centreY(), 10.0f, hot ? p.text : p.soft);
            }
        }

        bool contains(funkgui::Point p) const override
        {
            return bar_.contains(p) || editor().contains(p) || (!hidden_ && assist_.over(p));
        }
        funkgui::Cursor cursorAt(funkgui::Point p) const override
        {
            if (const std::optional<funkgui::Cursor> own = assist_.cursorAt(p))
                return *own;
            if (bar_.contains(p))
                return tabAt(p) >= 0 || (canAdd() && newTabRect().contains(p)) ? funkgui::Cursor::pointingHand
                                                                                : funkgui::Cursor::normal;
            return editor().cursorAt(p);
        }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&) override
        {
            const funkgui::Point p{ e.x, e.y };
            if (!bar_.contains(p))
            {
                endRename();
                if (assist_.pointerDown(e))
                {
                    pressedEditor_ = false;
                    return;
                }
                pressedEditor_ = true;
                editor().pointerDown(e);
                return;
            }
            pressedEditor_ = false;
            if (canAdd() && newTabRect().contains(p))
            {
                add();
                return;
            }
            const int i = tabAt(p);
            if (i < 0)
                return;
            if (i != renaming_)
                endRename();
            if (closeShown(i) && closeRect(i).contains(p))
            {
                if (close_)
                    close_(i);
                return;
            }
            select(i);
        }
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&) override
        {
            if (pressedEditor_)
                editor().pointerDrag(e);
        }
        void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController&) override
        {
            if (pressedEditor_)
                editor().pointerUp(e);
            pressedEditor_ = false;
        }
        void doubleClick(const funkgui::PointerEvent& e, funkgui::GestureController&) override
        {
            const funkgui::Point p{ e.x, e.y };
            if (!bar_.contains(p))
            {
                editor().doubleClick(e);
                return;
            }
            const int i = tabAt(p);
            if (i >= 0 && !(i > 0 && closeRect(i).contains(p)))
                beginRename(i);
        }
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController&, double) override
        {
            return !bar_.contains({ e.x, e.y }) && (assist_.wheel(e) || editor().wheel(e));
        }
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override
        {
            if (renaming_ < 0)
                return assist_.key(e);
            // The name being edited takes every key: Return keeps it, Escape cancels, Cmd-V pastes.
            if (e.key == funkgui::Key::enter)
                commitRename();
            else if (e.key == funkgui::Key::escape)
            {
                renaming_ = -1;
                why_.clear();
            }
            else if (e.key == funkgui::Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
            {
                if (host_ != nullptr)
                    edit_.paste(host_->clipboardText(), static_cast<int>(kMaxNameLength));
            }
            else if (edit_.key(e, static_cast<int>(kMaxNameLength)))
                why_.clear();
            return true;
        }
        void accessibility(std::vector<funkgui::A11yItem>& out) const override
        {
            funkgui::A11yItem group;
            group.id = id_ + 1;
            group.role = funkgui::A11yRole::radioGroup;
            group.bounds = bar_;
            group.title = "Files";
            out.push_back(group);
            for (int i = 0; i < count(); ++i)
            {
                funkgui::A11yItem tab;
                tab.id = id_ + 2 + static_cast<uint32_t>(i);
                tab.parent = group.id;
                tab.role = funkgui::A11yRole::radioButton;
                tab.bounds = tabRect(i);
                tab.title = name(i);
                tab.checkable = true;
                tab.checked = i == active_;
                tab.help = i == 0 ? "the program" : "a file the program can include";
                out.push_back(std::move(tab));
                if (i > 0)
                {
                    funkgui::A11yItem close;
                    close.id = id_ + 2 + static_cast<uint32_t>(kMaxFiles) + static_cast<uint32_t>(i);
                    close.role = funkgui::A11yRole::button;
                    close.bounds = closeRect(i);
                    close.title = "Close " + name(i);
                    out.push_back(std::move(close));
                }
            }
            if (canAdd())
            {
                funkgui::A11yItem add;
                add.id = id_ + 2 + 2 * static_cast<uint32_t>(kMaxFiles);
                add.role = funkgui::A11yRole::button;
                add.bounds = newTabRect();
                add.title = "New file";
                out.push_back(std::move(add));
            }
            funkgui::A11yItem text;
            editor().accessibility(text);
            out.push_back(std::move(text));
        }
        bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override
        {
            const uint32_t files = static_cast<uint32_t>(kMaxFiles);
            if (id == editor().a11yId() || id == id_ + 1)
                return true;
            if (id >= id_ + 2 && id < id_ + 2 + files)
            {
                select(static_cast<int>(id - id_ - 2));
                return true;
            }
            if (id > id_ + 2 + files && id < id_ + 2 + 2 * files)
            {
                if (close_)
                    close_(static_cast<int>(id - id_ - 2 - files));
                return true;
            }
            if (id == id_ + 2 + 2 * files)
            {
                add();
                return true;
            }
            return false;
        }
        void specLine(char* out, std::size_t n) const override
        {
            if (renaming_ >= 0)
            {
                std::snprintf(out, n, "RENAME   %s", why_.empty() ? "RETURN KEEPS THE NAME   ESCAPE CANCELS"
                                                                  : why_.c_str());
                return;
            }
            if (bar_.contains(pointer_))
            {
                if (canAdd() && newTabRect().contains(pointer_))
                    std::snprintf(out, n, "NEW FILE   ONE THE PROGRAM CAN #INCLUDE BY ITS NAME");
                else if (const int i = tabAt(pointer_); i == 0)
                    std::snprintf(out, n, "%s   THE PROGRAM   DOUBLE-CLICK RENAMES IT", upper(name(0)).c_str());
                else if (i > 0)
                    std::snprintf(out, n, "%s   #INCLUDE \"%s\"   DOUBLE-CLICK RENAMES IT   X CLOSES IT",
                                  upper(name(i)).c_str(), name(i).c_str());
                else if (n > 0)
                    out[0] = '\0';
                return;
            }
            if (assist_.specLine(out, n, pointer_))
                return;
            const funkgui::TextPos p = editor().caret();
            const std::string sel = editor().selectedText();
            const std::string file = upper(name(active_));
            if (sel.empty())
                std::snprintf(out, n, "%s   LINE %d   COLUMN %d   CMD-RETURN RUNS THE PROGRAM", file.c_str(),
                              p.line + 1, p.col + 1);
            else
                std::snprintf(out, n, "%s   LINE %d   COLUMN %d   %zu SELECTED   CMD-RETURN RUNS THE PROGRAM",
                              file.c_str(), p.line + 1, p.col + 1, sel.size());
        }
        funkgui::Point centre() const override { return { editorBounds_.centreX(), editorBounds_.centreY() }; }
        void focusChanged(bool focused) override
        {
            focused_ = focused;
            assist_.focusChanged(focused);
            editor().setFocused(focused && !assist_.takesKeys());
            if (!focused)
                endRename();
        }
        bool takesAllKeys() const override { return editor().focused() || renaming_ >= 0 || assist_.takesKeys(); }

    private:
        struct Tab
        {
            std::string name;
            std::unique_ptr<funkgui::TextEditor> editor;
        };

        Tab makeTab(std::string_view n, std::string_view text)
        {
            Tab t{ std::string(n), std::make_unique<funkgui::TextEditor>(id_) };
            funkgui::TextEditorOptions o;
            o.style = funkgui::editorFont(funkgui::EditorFont::code);
            o.softTabs = true;
            o.tabWidth = 4;
            o.a11yTitle = "File";
            o.placeholder = tabs_.empty()                          ? "An FMinus program: EXAMPLES has some to start from"
                          : n.size() > 4 && n.substr(n.size() - 4) == ".gui" ? "The program's interface: rows, sections, knobs, displays"
                                                                             : "A file the program can #include by its name";
            t.editor->setOptions(o);
            t.editor->setBounds(assist_.editorRect());
            assist_.configure(*t.editor, n);
            t.editor->setText(text);
            t.editor->attach(host_);
            return t;
        }

        // The shown editor takes the item's focus.
        void show() { editor().setFocused(focused_); }

        // `text` in place of the editor's, as one undoable edit; the caret and the scroll stay where they can.
        static void replaceText(funkgui::TextEditor& ed, const std::string& text)
        {
            if (ed.text() == funkgui::TextEditor::sanitise(text))
                return;
            const funkgui::TextPos caret = ed.caret();
            const funkgui::Point scroll = ed.scroll();
            ed.selectAll();
            if (!ed.insert(text))
                ed.deleteSelection();                    // the new text is empty
            ed.setCaret(caret);
            ed.setScroll(scroll.x, scroll.y);
        }

        // A tab as wide as its name (kTabMinW to kTabMaxW), all narrowed alike, down to kTabNarrowW, when they do not
        // fit the bar.
        float tabWidth(int i) const { return std::min(naturalWidth(i), widthCap()); }

        float naturalWidth(int i) const
        {
            const float w = funkgui::text::width(funkgui::FontService::get().atlas(), name(i).c_str(), chat::kSmall);
            // 2 px over the name's width: the canvas measures it a hair wider than the atlas at some sizes.
            return std::clamp(w + 2.0f * kTabPad + (i > 0 ? kTabCloseW : 0.0f) + 2.0f, kTabMinW, kTabMaxW);
        }

        float widthCap() const
        {
            const int n = count();
            const float room = bar_.w - (canAdd() ? kNewTabW + kTabGap : 0.0f) - kTabGap * static_cast<float>(n - 1);
            std::vector<float> w;
            float sum = 0.0f;
            for (int k = 0; k < n; ++k)
            {
                w.push_back(naturalWidth(k));
                sum += w.back();
            }
            if (sum <= room)
                return kTabMaxW;
            // The widest narrowed first: the cap that makes them fit.
            std::sort(w.begin(), w.end());
            float left = room;
            for (int k = 0; k < n; ++k)
            {
                const float cap = left / static_cast<float>(n - k);
                if (w[static_cast<std::size_t>(k)] >= cap)
                    return std::max(cap, kTabNarrowW);
                left -= w[static_cast<std::size_t>(k)];
            }
            return kTabMaxW;
        }

        int tabAt(funkgui::Point p) const
        {
            for (int i = 0; i < count(); ++i)
                if (tabRect(i).contains(p))
                    return i;
            return -1;
        }

        bool hasErrors(const std::string& file) const
        {
            return std::any_of(problems_.begin(), problems_.end(),
                               [&](const Problem& p) { return p.error && p.file == file; });
        }

        static void drawCross(funkgui::Canvas& c, const funkgui::Rect& r, float half, funkgui::Col ink)
        {
            const float cx = r.centreX(), cy = r.centreY();
            const float xs1[2] = { cx - half, cx + half }, ys1[2] = { cy - half, cy + half };
            const float xs2[2] = { cx - half, cx + half }, ys2[2] = { cy + half, cy - half };
            c.polyline(xs1, ys1, 2, 1.2f, ink);
            c.polyline(xs2, ys2, 2, 1.2f, ink);
        }

        funkgui::Rect bar_, editorBounds_;
        uint32_t id_;
        const std::vector<Problem>& problems_;       // the Panel's
        Close close_;
        std::vector<Tab> tabs_;
        int active_ = 0;
        uint64_t structure_ = 0;
        bool focused_ = false;
        bool pressedEditor_ = false;
        funkgui::HostServices* host_ = nullptr;
        funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
        int renaming_ = -1;                          // the tab whose name is being edited
        funkgui::text::LineEdit edit_;
        std::string why_;                            // why the name typed cannot be the file's
        bool hidden_ = false;                        // the PANEL view
        std::function<void(const std::string&)> notice_;
        // Last: its tabs' callbacks reach the members above.
        CodeAssist assist_;

    public:
        void setHidden(bool hidden) { hidden_ = hidden; }
        bool shown() const override { return !hidden_; }
    };

    namespace
    {

        // ---- the last run's messages ----------------------------------------------------------------------------------
        class ProblemsItem final : public Item
        {
        public:
            using Jump = std::function<void(const Problem&)>;

            ProblemsItem(funkgui::Rect bounds, uint32_t id, const std::vector<Problem>& problems, Jump jump)
                : bounds_(bounds), id_(id), problems_(problems), jump_(std::move(jump)) {}

            funkgui::Rect rowRect(int i) const
            {
                return { bounds_.x, bounds_.y + kProblemRowH * static_cast<float>(i), bounds_.w, kProblemRowH };
            }
            void setBounds(funkgui::Rect b) { bounds_ = b; }

            void tick(float, bool, bool, funkgui::Point pointer) override { pointer_ = pointer; }
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const override
            {
                namespace type = funkgui::type;
                const funkgui::Canvas::Scope s(c, kProblemTag, false);
                const int shown = std::min(static_cast<int>(problems_.size()), kProblemRows);
                for (int i = 0; i < shown; ++i)
                {
                    const Problem& p = problems_[static_cast<std::size_t>(i)];
                    const funkgui::Rect r = rowRect(i);
                    const bool last = i == kProblemRows - 1 && static_cast<int>(problems_.size()) > kProblemRows;
                    const bool hover = p.line > 0 && r.contains(pointer_);
                    std::string place = p.file;
                    if (p.line > 0)
                        place += ":" + std::to_string(p.line) + ":" + std::to_string(p.column);
                    c.text(fitText(c, place, type::kMicro, kPlaceW - 8.0f).c_str(), r.x, r.y + 2.0f, type::kMicro,
                           hover ? th.accent : th.ink52);
                    const float x0 = r.x + kPlaceW;
                    std::string text = p.text;
                    if (last)
                    {
                        char more[32];
                        std::snprintf(more, sizeof more, "   (and %d more)",
                                      static_cast<int>(problems_.size()) - kProblemRows + 1);
                        text += more;
                    }
                    const funkgui::Col ink = p.error ? th.signal : th.ink52;
                    c.text(fitText(c, text, type::kMicro, r.right() - x0).c_str(), x0, r.y + 2.0f, type::kMicro,
                           hover ? th.accent : ink);
                }
            }
            bool contains(funkgui::Point p) const override { return bounds_.contains(p); }
            funkgui::Cursor cursorAt(funkgui::Point p) const override
            {
                const int i = rowAt(p);
                return i >= 0 && problems_[static_cast<std::size_t>(i)].line > 0 ? funkgui::Cursor::pointingHand
                                                                                 : funkgui::Cursor::normal;
            }
            void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&) override
            {
                const int i = rowAt({ e.x, e.y });
                if (i >= 0 && jump_)
                    jump_(problems_[static_cast<std::size_t>(i)]);
            }
            void accessibility(std::vector<funkgui::A11yItem>& out) const override
            {
                funkgui::A11yItem item;
                item.id = id_;
                item.role = funkgui::A11yRole::staticText;
                item.bounds = bounds_;
                item.title = "Problems";
                for (const Problem& p : problems_)
                    item.value += (item.value.empty() ? "" : "; ") + p.text;
                if (item.value.empty())
                    item.value = "none";
                item.readOnly = true;
                out.push_back(std::move(item));
            }
            bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override
            {
                return id == id_;
            }
            void specLine(char* out, std::size_t n) const override
            {
                const int i = rowAt(pointer_);
                if (i >= 0 && problems_[static_cast<std::size_t>(i)].line > 0)
                {
                    const Problem& p = problems_[static_cast<std::size_t>(i)];
                    std::snprintf(out, n, "PROBLEM   CLICK TO GO TO %s LINE %d", upper(p.file).c_str(), p.line);
                }
                else if (n > 0)
                    std::snprintf(out, n, "PROBLEMS   %zu", problems_.size());
            }
            funkgui::Point centre() const override { return { bounds_.centreX(), bounds_.centreY() }; }
            bool focusable() const override { return false; }

        private:
            int rowAt(funkgui::Point p) const
            {
                const int shown = std::min(static_cast<int>(problems_.size()), kProblemRows);
                for (int i = 0; i < shown; ++i)
                    if (rowRect(i).contains(p))
                        return i;
                return -1;
            }

            funkgui::Rect bounds_;
            uint32_t id_;
            const std::vector<Problem>& problems_;       // the Panel's
            Jump jump_;
            funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
            bool hidden_ = false;                        // the PANEL view

        public:
            void setHidden(bool hidden) { hidden_ = hidden; }
            bool shown() const override { return !hidden_; }
        };
    }

    // ---- construction ---------------------------------------------------------------------------------------------------
    // The items are made with no place: layout() gives them their places for the panel's size. Their order is the Tab
    // order.
    Panel::Panel(std::span<funkgui::ParamPort* const> ports, EditorPort& port) : port_(port)
    {
        title_ = upper(product::kName);
        subtitle_ = std::string(product::kIsInstrument ? "Instrument" : "Effect") + "  v" + product::kVersion + "  FMinus";
        width_ = static_cast<float>(kDefaultWidth);
        height_ = static_cast<float>(kDefaultHeight);
        const auto idFor = [this] { return kA11yStride * static_cast<uint32_t>(items_.size() + 1); };
        const auto add = [this](std::unique_ptr<Item> item) {
            items_.push_back(std::move(item));
            return static_cast<int>(items_.size()) - 1;
        };

        // The program's card's actions (Run, Random, Library, Examples) and the top bar's controls (the project, the
        // view), as the chat's buttons are (ChatItems.h).
        using TB = ButtonItem;
        const auto button = [&](TB::Shape shape, chat::Icon icon, std::function<std::string()> label, bool chevron,
                                std::function<TB::Look()> look, std::function<void()> act, std::string title,
                                std::string footer) {
            TB::Spec sp;
            sp.shape = shape;
            sp.icon = [icon] { return icon; };
            sp.label = std::move(label);
            sp.chevron = chevron;
            sp.look = std::move(look);
            sp.act = std::move(act);
            sp.a11yTitle = std::move(title);
            sp.spec = std::move(footer);
            return add(std::make_unique<TB>(idFor(), std::move(sp)));
        };
        runItem_ = button(TB::Shape::pill, chat::Icon::play, [] { return std::string("Run"); }, false,
                          [] { return TB::Look::primary; }, [this] { runNow(); }, "Run the program",
                          "RUN   COMPILE THE PROGRAM AND PLAY IT (CMD-RETURN)");
        examplesItem_ = button(TB::Shape::pill, chat::Icon::book, [] { return std::string("Examples"); }, true,
                               [] { return TB::Look::ghost; }, [this] { openExamples(); }, "Examples",
                               "EXAMPLES   REPLACE THE TEXT WITH ONE, AND RUN IT");
        libraryItem_ = button(TB::Shape::pill, chat::Icon::blocks, [] { return std::string("Library"); }, true,
                              [] { return TB::Look::ghost; }, [this] { openLibrary(); }, "Library",
                              "LIBRARY   A READY-MADE BUILDING BLOCK AS A TAB OF ITS OWN, #INCLUDED BY THE PROGRAM");
        randomItem_ = button(TB::Shape::pill, chat::Icon::dice, [] { return std::string("Random"); }, false,
                             [] { return TB::Look::ghost; }, [this] { makeRandom(nextSeed()); }, "A random program",
                             "RANDOM   A NEW PROGRAM FROM THE LIBRARY'S BUILDING BLOCKS, FOR INSPIRATION");
        projectsItem_ = button(TB::Shape::pill, chat::Icon::list, [this] { return projectLabel(); }, true,
                               [] { return TB::Look::outlined; }, [this] { openProjects(); }, "Projects",
                               "PROJECTS   SAVE THE PROGRAM ON THIS MAC, A VERSION EACH SAVE (CMD-S); OPEN AN OLDER ONE; ITS PRESETS");
        panelView_ = port_.panelView();
        chatOpen_ = port_.chatOpen();
        explorerOpen_ = port_.explorerOpen();
        viewItem_ = button(TB::Shape::pill, chat::Icon::code, [] { return std::string("Code"); }, false,
                           [this] { return panelView_ ? TB::Look::ghost : TB::Look::raised; },
                           [this] { setPanelView(false); }, "Code view",
                           "CODE / PANEL   THE EDITOR, OR THE PROGRAM'S PANEL ACROSS THE WHOLE COLUMN");
        panelViewItem_ = button(TB::Shape::pill, chat::Icon::dial, [] { return std::string("Panel"); }, false,
                                [this] { return panelView_ ? TB::Look::raised : TB::Look::ghost; },
                                [this] { setPanelView(true); }, "Panel view",
                                "CODE / PANEL   THE EDITOR, OR THE PROGRAM'S PANEL ACROSS THE WHOLE COLUMN");

        // The editor, the problems under it, the dials under them.
        editorItem_ = add(std::make_unique<FilesItem>(funkgui::Rect{}, funkgui::Rect{}, idFor(), problems_,
                                                      [this](int tab) { closeFile(tab); }));
        {
            FilesItem& f = static_cast<FilesItem&>(*items_[static_cast<std::size_t>(editorItem_)]);
            f.setNotice([this](const std::string& text) { notice(text); });
            Sources library;
            for (const LibraryFile& l : kLibrary)
                library.push_back({ l.name, l.source });
            f.assist().setLibrary(library);
            f.assist().setInstrument(product::kIsInstrument);
        }
        problemsItem_ = add(std::make_unique<ProblemsItem>(funkgui::Rect{}, idFor(), problems_,
                                                           [this](const Problem& p) { jumpTo(p); }));
        itemOfSlot_.assign(static_cast<std::size_t>(kMaxControls), -1);
        for (int c = 0; c < kMaxControls && c < static_cast<int>(ports.size()); ++c)
            itemOfSlot_[static_cast<std::size_t>(c)] = add(std::make_unique<DialItem>(
                *ports[static_cast<std::size_t>(c)], funkgui::DialGeom{ 0.0f, 0.0f }, idFor()));
        // The program's displays (#pragma panel meter, vu, led, number, scope), shown as it has them.
        itemOfDisplay_.assign(static_cast<std::size_t>(kMaxDisplays), -1);
        for (int d = 0; d < kMaxDisplays; ++d)
            itemOfDisplay_[static_cast<std::size_t>(d)] = add(std::make_unique<DisplayItem>(idFor()));
        // A .gui file's interface (GuiItems.h): its tabs first (their frames under the rest), a control a slot, and
        // pools of pads and envelopes.
        constexpr int kGuiPool = 8;
        for (int k = 0; k < kGuiPool; ++k)
            guiTabs_.push_back(add(std::make_unique<GuiTabsItem>(idFor(), [this](int node, int page) { chooseGuiPage(node, page); })));
        itemOfGuiSlot_.assign(static_cast<std::size_t>(kMaxControls), -1);
        for (int c = 0; c < kMaxControls && c < static_cast<int>(ports.size()); ++c)
            itemOfGuiSlot_[static_cast<std::size_t>(c)] = add(std::make_unique<GuiControlItem>(
                *ports[static_cast<std::size_t>(c)], idFor(), [this] {
                    return productTheme(host_ != nullptr ? funkgui::Theme::byIndex(host_->themeIndex()) : funkgui::Theme::graphite());
                }));
        for (int k = 0; k < kGuiPool; ++k)
            guiPads_.push_back(add(std::make_unique<GuiXyItem>(idFor())));
        for (int k = 0; k < kGuiPool; ++k)
            guiEnvelopes_.push_back(add(std::make_unique<GuiEnvelopeItem>(idFor())));
        for (int k = 0; k < 2; ++k)
            guiKeyboards_.push_back(add(std::make_unique<GuiKeyboardItem>(
                idFor(), [this](int note, float velocity) { port_.playNote(note, velocity); },
                [this](bool pitch, float value) { port_.playWheel(pitch, value); })));
        for (int k = 0; k < 4; ++k)
            guiShaders_.push_back(add(std::make_unique<GuiShaderItem>(idFor())));
        shaderNode_.assign(guiShaders_.size(), -1);

        // The chat: the header's buttons (a new chat, the API key), the conversation, the message box and the buttons in
        // its bar (the model, the thinking level, fast mode on a model that takes it, send or stop).
        using B = ButtonItem;
        const auto spec = [](B::Shape shape, std::function<chat::Icon()> icon, std::function<std::string()> label, bool chevron,
                             std::function<B::Look()> look, std::function<bool()> enabled, std::function<void()> act,
                             std::string title, std::string footer) {
            B::Spec sp;
            sp.shape = shape;
            sp.icon = std::move(icon);
            sp.label = std::move(label);
            sp.chevron = chevron;
            sp.look = std::move(look);
            sp.enabled = std::move(enabled);
            sp.act = std::move(act);
            sp.a11yTitle = std::move(title);
            sp.spec = std::move(footer);
            return sp;
        };
        newChatItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::square, [] { return chat::Icon::pencil; }, {}, false, {}, {}, [this] { newChat(); },
                          "New chat", "NEW CHAT   FORGET THIS CONVERSATION AND START ANOTHER")));
        keyItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::square, [] { return chat::Icon::key; }, {}, false, {}, {}, [this] { openKeyDialog(); },
                          "Account or API key", "ACCESS   SIGN IN, OR USE YOUR OWN ANTHROPIC API KEY")));
        chatToggleItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::square, [this] { return chatOpen_ ? chat::Icon::chevronRight : chat::Icon::bubble; }, {},
                          false, {}, {}, [this] { setChatOpen(!chatOpen_); }, "Fold or open the chat",
                          "CHAT   FOLD IT AWAY FOR ROOM; THE RAIL AT THE RIGHT OPENS IT AGAIN")));
        // The explorer (Explorer.h): its rows act as the tabs, LIBRARY, EXAMPLES and PROJECTS do.
        {
            ExplorerItem::Hooks hooks;
            hooks.open = [this](ExplorerItem::Section s, const ExplorerItem::Row& r) { openFromExplorer(s, r); };
            hooks.rename = [this](ExplorerItem::Section s, const ExplorerItem::Row& r) {
                if (s != ExplorerItem::Section::files || r.index < 0 || r.index >= files().count())
                    return;
                files().select(r.index);
                files().beginRename(r.index);
                setFocus(editorItem_);
                dirty_ = true;
            };
            hooks.close = [this](ExplorerItem::Section s, const ExplorerItem::Row& r) {
                if (s == ExplorerItem::Section::files)
                    closeFile(r.index);
            };
            hooks.action = [this](ExplorerItem::Section s) {
                if (s == ExplorerItem::Section::files)
                {
                    if (files().add())
                        setFocus(editorItem_);         // its name is being typed in its tab
                    dirty_ = true;
                }
                else if (s == ExplorerItem::Section::presets)
                    openProjects(true);
                else if (s == ExplorerItem::Section::projects)
                    openProjects(false);
            };
            explorerItem_ = add(std::make_unique<ExplorerItem>(kExplorerA11yBase, std::move(hooks)));
        }
        {
            B::Spec sp = spec(B::Shape::square, [this] { return explorerShown_ ? chat::Icon::chevronLeft : chat::Icon::list; }, {},
                              false, {}, {}, [this] { toggleExplorer(); }, "Fold or open the explorer",
                              "EXPLORER   THE PROGRAM'S FILES, THE LIBRARY, THE EXAMPLES AND THE PRESETS; FOLD IT AWAY FOR ROOM");
            sp.shown = [this] { return !panelView_; };   // the PANEL view is the plugin's face: no explorer
            explorerToggleItem_ = add(std::make_unique<B>(idFor(), std::move(sp)));
        }
        {
            B::Spec sp = spec(B::Shape::pill, [] { return chat::Icon::person; }, [this] { return accountLabel(); }, false,
                              [] { return B::Look::ghost; }, {}, [this] { openKeyDialog(DialogTab::account); }, "Your account",
                              "ACCOUNT   WHO IS SIGNED IN, THE CREDIT LEFT AND THIS MONTH'S USE");
            sp.shown = [this] { return chat_.signedIn; };
            accountItem_ = add(std::make_unique<B>(idFor(), std::move(sp)));
        }
        chatViewItem_ = add(std::make_unique<ChatView>(
            kChatA11yBase, ChatView::Hooks{ [this](const std::string& s) { suggest(s); },
                                            [this] { openKeyDialog(DialogTab::key); },
                                            [this] { openKeyDialog(DialogTab::account); },
                                            [this](const std::vector<std::string>& a) { port_.chat().answer(a); },
                                            [this] { port_.chat().clearChecks(); } }));
        inputItem_ = add(std::make_unique<InputItem>(idFor(), [this] { sendChat(); }, [this](uint32_t id) {
            port_.chat().detach(id);
            chatAt_ = -1.0;
            dirty_ = true;
        }));
        attachItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::circle, [] { return chat::Icon::image; }, {}, false, [] { return B::Look::ghost; },
                          [this] { return !chat_.busy; }, [this] { openPictures(); }, "Add pictures",
                          "PICTURES   ADD PICTURES FOR CLAUDE (A PANEL TO TAKE AFTER, A SKETCH), OR DROP THEM ON THE CHAT")));
        modelItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::pill, {}, [this] { return modelLabel(); }, true, [] { return B::Look::ghost; }, {},
                          [this] { openModels(); }, "Model", "MODEL   THE MODEL THAT BUILDS, AND ONE THAT PLANS EACH REQUEST FIRST")));
        thinkingItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::pill, {}, [this] { return thinkingLabel(); }, true, [] { return B::Look::ghost; },
                          [this] { return !chatEfforts().empty(); }, [this] { openThinking(); }, "Thinking level",
                          "THINKING   HOW MUCH CLAUDE THINKS BEFORE IT ANSWERS, AND HOW HARD IT WORKS")));
        {
            B::Spec fast = spec(B::Shape::pill, [] { return chat::Icon::bolt; }, [] { return std::string("Fast"); }, false,
                                [this] { return chat_.fast ? B::Look::primary : B::Look::ghost; }, {},
                                [this] {
                                    port_.chat().setFast(!chat_.fast);
                                    chatAt_ = -1.0;
                                    dirty_ = true;
                                },
                                "Fast mode",
                                "FAST   THE SAME MODEL, ITS ANSWERS WRITTEN UP TO 2.5X FASTER, AT TWICE THE PRICE (THE API'S "
                                "FAST MODE)");
            fast.shown = [this] { return chatTakesFast(); };
            fast.checked = [this] { return chat_.fast; };
            fastItem_ = add(std::make_unique<B>(idFor(), std::move(fast)));
        }
        {
            B::Spec web = spec(B::Shape::pill, [] { return chat::Icon::globe; }, [] { return std::string("Web"); }, false,
                               [this] { return chat_.web && chat_.webBlocked.empty() ? B::Look::primary : B::Look::ghost; },
                               [this] { return chat_.webBlocked.empty(); },
                               [this] {
                                   port_.chat().setWeb(!chat_.web);
                                   chatAt_ = -1.0;
                                   dirty_ = true;
                               },
                               "Web search",
                               "WEB   CLAUDE SEARCHES THE WEB AND READS PAGES AND PDFS (DATA SHEETS, PAPERS): $10 A THOUSAND "
                               "SEARCHES, WITH YOUR OWN API KEY");
            web.shown = [this] { return chatTakesWeb(); };
            web.checked = [this] { return chat_.web && chat_.webBlocked.empty(); };
            webItem_ = add(std::make_unique<B>(idFor(), std::move(web)));
        }
        sendItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::circle, [this] { return chat_.busy ? chat::Icon::stop : chat::Icon::arrowUp; }, {},
                          false, [] { return B::Look::primary; },
                          [this] {
                              return chat_.busy || inputItem().editor().text().find_first_not_of(" \t\n") != std::string::npos;
                          },
                          [this] {
                              if (chat_.busy)
                                  port_.chat().stop();
                              else
                                  sendChat();
                          },
                          "Send or stop", "SEND   THE MESSAGE TO CLAUDE (RETURN IN THE MESSAGE BOX)   STOP ENDS ITS TURN")));

        // The theme (the top bar's right corner): one button that switches Graphite and Paper, machine-wide.
        {
            TB::Spec sp;
            sp.shape = TB::Shape::square;
            sp.icon = [this] { return (host_ != nullptr ? host_->themeIndex() : 0) == 0 ? chat::Icon::moon : chat::Icon::sun; };
            sp.act = [this] {
                const int now = host_ != nullptr ? host_->themeIndex() : 0;
                funkgui::UiPreferences::get().setTheme(now == 0 ? 1 : 0);
                dirty_ = true;
            };
            sp.a11yTitle = "Theme: Graphite or Paper";
            sp.spec = "THEME   GRAPHITE (DARK) OR PAPER (LIGHT), FOR EVERY PLAYGROUND EDITOR ON THIS MAC";
            themeItem_ = add(std::make_unique<TB>(idFor(), std::move(sp)));
        }

        // The zoom (the status bar's right corner): a pill of the zoom in use, its menu the host's steps; shown when the
        // host offers zoom steps (the plugin's editor does).
        zoomModel_ = std::make_unique<ZoomModel>();
        zoom_ = zoomModel_.get();
        {
            TB::Spec sp;
            sp.shape = TB::Shape::pill;
            sp.label = [this] {
                const auto& z = *static_cast<const ZoomModel*>(zoom_);
                const int a = z.active();
                return std::string(a >= 0 ? z.label(a) : "Zoom");
            };
            sp.chevron = true;
            sp.look = [] { return TB::Look::ghost; };
            sp.shown = [this] { return static_cast<const ZoomModel*>(zoom_)->count() > 0; };
            sp.act = [this] { openZoom(); };
            sp.a11yTitle = "Zoom";
            sp.spec = "ZOOM   THE EDITOR'S SIZE ON SCREEN";
            zoomItem_ = add(std::make_unique<TB>(idFor(), std::move(sp)));
        }

        layout();

        // The processor's files and status, and the chat.
        files().load(port_.draft());
        seenDraft_ = port_.draftRevision();
        syncedFiles_ = files().revision();
        running_ = port_.running();
        refresh(0.0);
        focus_ = editorItem_;
        setFocus(editorItem_);
    }

    void Panel::layout()
    {
        const float W = width_, H = height_;
        const float footerTop = H - kFooterH;
        const float bottom = footerTop - kBottomGap;     // the dials' and the message box's bottom

        // The columns: the chat about a third of the width, or, folded, a rail.
        chatW_ = chatOpen_ ? std::clamp(std::round(W * kChatShare), kChatMinW, kChatMaxW) : kChatRailW;
        chatX_ = W - kMargin - chatW_;
        // The explorer at the left, about a sixth of the width, as far as the program's card keeps kProgramMinW, in the
        // CODE view only; folded, too narrow a window or the PANEL view: nothing (its button at the left of the program's
        // card's header). The program's card after it.
        float explorerW = 0.0f;
        if (explorerOpen_ && !panelView_)
        {
            const float room = chatX_ - (chatOpen_ ? kChatGap : kChatRailGap) - kMargin - kExplorerGap - kProgramMinW;
            const float want = std::clamp(std::round(W * kExplorerShare), kExplorerMinW, kExplorerMaxW);
            if (room >= kExplorerMinW)
                explorerW = std::min(want, std::floor(room));
        }
        explorerShown_ = explorerW > 0.0f;
        explorerCard_ = explorerShown_ ? funkgui::Rect{ kMargin, kMainY, explorerW, bottom - kMainY } : funkgui::Rect{};
        leftX_ = kMargin + (explorerShown_ ? explorerW + kExplorerGap : 0.0f);
        leftW_ = chatX_ - (chatOpen_ ? kChatGap : kChatRailGap) - leftX_;

        // The top bar: the project in its middle (clear of the product's name), the view's switch, the load and the
        // theme at its right.
        const auto at = [this](int i) -> ButtonItem& { return static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(i)]); };
        const float barMid = kTopBarY + 0.5f * kTopBarH;
        const float right = W - kMargin;
        at(themeItem_).setBounds({ right - 8.0f - kIconButton, barMid - 0.5f * kIconButton, kIconButton, kIconButton });
        const float segRight = right - 8.0f - kIconButton - 92.0f;        // the load's text between them
        at(panelViewItem_).setBounds({ segRight - 3.0f - kSegmentW, barMid - 0.5f * (kTopButtonH - 6.0f), kSegmentW, kTopButtonH - 6.0f });
        at(viewItem_).setBounds({ segRight - 3.0f - 2.0f * kSegmentW, barMid - 0.5f * (kTopButtonH - 6.0f), kSegmentW, kTopButtonH - 6.0f });
        {
            const funkgui::FontAtlasSdf& atlas = funkgui::FontService::get().atlas();
            const float nameRight = kMargin + 50.0f + std::max(funkgui::text::width(atlas, product::kName, chat::kStrong),
                                                               funkgui::text::width(atlas, subtitle_.c_str(), chat::kSmall));
            const float segLeft = segRight - 3.0f - 2.0f * kSegmentW - 3.0f;
            const float w = std::min(kProjectW, std::max(120.0f, segLeft - 24.0f - (nameRight + 24.0f)));
            const float x = std::clamp(std::round(0.5f * (W - w)), nameRight + 24.0f, std::max(nameRight + 24.0f, segLeft - 24.0f - w));
            at(projectsItem_).setBounds({ x, barMid - 0.5f * kTopButtonH, w, kTopButtonH });
        }

        // The program's card's header: the actions at its right (Run last), the tabs from its left.
        const float inX = leftX_ + kInset, inW = leftW_ - 2.0f * kInset;
        const float ay = kActionsY + 0.5f * (kActionH - 26.0f);
        float ax = leftX_ + leftW_ - kInset;
        for (const int i : { runItem_, randomItem_, libraryItem_, examplesItem_ })
        {
            ButtonItem& b = at(i);
            b.setBounds({ 0.0f, ay, 10.0f, 26.0f });     // its height first: its width follows its label
            const float w = b.preferredWidth();
            ax -= w;
            b.setBounds({ ax, ay, w, 26.0f });
            ax -= i == runItem_ ? 10.0f : 4.0f;
        }
        actionsX_ = ax;
        // The dials: in plain rows, or in the sections the program's #pragma panel lines ask for (DialLayout.h), as
        // tall as the panel can give them while the editor keeps 120 px.
        const float problemsH = kProblemRowH * static_cast<float>(kProblemRows);
        const float inBottom = bottom - kInset;          // the program's card's inside
        const float roomForDials = inBottom - kEditorY - 120.0f - 8.0f - problemsH - 16.0f;
        DialMeasure measure;
        measure.label = [this](int slot, bool small) {
            const std::string& label = panelSpec_.labels[static_cast<std::size_t>(slot)];
            const std::string& name = controls_[static_cast<std::size_t>(slot)].name;
            const std::string text = upper(asciiText(label.empty() ? name : label));
            return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(),
                                        small ? funkgui::type::kMicro : funkgui::type::kCaption);
        };
        measure.title = [](const std::string& title) {
            return funkgui::text::width(funkgui::FontService::get().atlas(), upper(asciiText(title)).c_str(), kSectionTitle);
        };
        measure.caption = [](const std::string& text) {
            return funkgui::text::width(funkgui::FontService::get().atlas(), upper(asciiText(text)).c_str(), funkgui::type::kCaption);
        };
        measure.micro = [](const std::string& text) {
            return funkgui::text::width(funkgui::FontService::get().atlas(), upper(asciiText(text)).c_str(), funkgui::type::kMicro);
        };
        // A .gui file's interface (GuiLayout.h) in place of the dials: under the editor as tall as it is (up to the
        // room the editor leaves, scrolling past that), or across the whole column in the PANEL view.
        const bool gui = !panelSpec_.gui.empty();
        float panelTop = inBottom;
        if (gui)
        {
            const GuiMeasure gm = [](const std::string& text, const funkgui::TextStyle& st) {
                return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(), st);
            };
            // Under the editor it takes half the column at most (the editor keeps the rest), and scrolls past that.
            const GuiExtent natural = naturalGuiSize(panelSpec_, controls_, gm, inW);   // a row that wraps: as wide
            const float h = std::max(40.0f, std::min({ natural.h, roomForDials, 0.5f * (inBottom - kEditorY) }));
            funkgui::Rect area = panelView_ ? funkgui::Rect{ inX, kTabsY, inW, inBottom - kTabsY }
                                            : funkgui::Rect{ inX, inBottom - h, inW, h };
            guiPages_.resize(panelSpec_.gui.nodes.size(), 0);
            gui_ = layoutGui(panelSpec_, controls_, gm, guiPages_, area, guiScrollX_, guiScrollY_);
            if (gui_.maxScrollY > 0.0f)                  // a gutter for the scroll bar
            {
                area.w -= kGuiScrollGutter;
                gui_ = layoutGui(panelSpec_, controls_, gm, guiPages_, area, guiScrollX_, guiScrollY_);
            }
            guiPages_ = gui_.pages;
            guiScrollX_ = std::clamp(guiScrollX_, 0.0f, gui_.maxScrollX);
            guiScrollY_ = std::clamp(guiScrollY_, 0.0f, gui_.maxScrollY);
            dials_ = DialsLayout{};
            dials_.top = area.y;
            panelTop = area.y;
        }
        else
        {
            gui_ = GuiLayout{};
            if (panelView_)
            {
                // The PANEL view: the dials from the column's top, as tall as they would be with all its room.
                const DialsLayout first = layoutDials(panelSpec_, controls_, measure, inX, inW, inBottom, inBottom - kTabsY);
                dials_ = layoutDials(panelSpec_, controls_, measure, inX, inW, kTabsY + (inBottom - first.top), inBottom - kTabsY);
            }
            else
                dials_ = layoutDials(panelSpec_, controls_, measure, inX, inW, inBottom, roomForDials);
            panelTop = dials_.top;
            for (int d = 0; d < kMaxDisplays; ++d)
            {
                auto& item = static_cast<DisplayItem&>(*items_[static_cast<std::size_t>(itemOfDisplay_[static_cast<std::size_t>(d)])]);
                const bool has = d < static_cast<int>(panelSpec_.displays.size());
                item.setDisplay(has ? &panelSpec_.displays[static_cast<std::size_t>(d)] : nullptr);
                item.setCell(dials_.displays[static_cast<std::size_t>(d)]);
                item.setArea({});
            }
        }
        const float problemsY = panelTop - 16.0f - problemsH;
        files().setHidden(panelView_);
        static_cast<ProblemsItem&>(*items_[static_cast<std::size_t>(problemsItem_)]).setHidden(panelView_);
        files().setGeometry({ inX, kTabsY, inW, kTabH },
                            { inX, kEditorY, inW, std::max(80.0f, problemsY - 8.0f - kEditorY) });
        static_cast<ProblemsItem&>(*items_[static_cast<std::size_t>(problemsItem_)])
            .setBounds({ inX, problemsY, inW, problemsH });
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0)
            {
                auto& dial = static_cast<DialItem&>(*items_[static_cast<std::size_t>(i)]);
                dial.setLook(panelSpec_.styles[static_cast<std::size_t>(c)], panelSpec_.colors[static_cast<std::size_t>(c)],
                             dials_.dials[static_cast<std::size_t>(c)]);
                dial.setHidden(gui);
            }
        layoutGuiItems();

        // The chat's card.
        card_ = { chatX_, kMainY, chatW_, bottom - kMainY };
        programCard_ = { leftX_, kMainY, leftW_, bottom - kMainY };
        layoutChat();
        layoutExplorer();

        // The zoom, in the status bar's right corner.
        {
            ButtonItem& z = at(zoomItem_);
            z.setBounds({ 0.0f, H - kFooterH + 4.0f, 10.0f, kFooterH - 8.0f });
            const float w = z.preferredWidth();
            z.setBounds({ right - w, H - kFooterH + 4.0f, w, kFooterH - 8.0f });
        }
        ++layoutRevision_;
        dirty_ = true;
    }

    // The chat's card: the header, the conversation, the message box (as tall as its text), its bar's buttons (as wide
    // as their labels) and the usage line.
    void Panel::layoutChat()
    {
        const float inX = card_.x + kCardPad, inW = card_.w - 2.0f * kCardPad;
        const float hy = card_.y + kCardPad;
        // Folded: every item of the chat hidden but the rail's button, at its top.
        for (const int i : { newChatItem_, keyItem_, accountItem_, modelItem_, thinkingItem_, fastItem_, webItem_, attachItem_,
                             sendItem_ })
            static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(i)]).setHidden(!chatOpen_);
        chatView().setHidden(!chatOpen_);
        inputItem().setHidden(!chatOpen_);
        auto& toggle = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(chatToggleItem_)]);
        if (!chatOpen_)
        {
            toggle.setBounds({ card_.centreX() - 0.5f * kIconButton, card_.y + 8.0f, kIconButton, kIconButton });
            headerY_ = hy;
            headerLabel_ = accountLabel();
            ++layoutRevision_;
            dirty_ = true;
            return;
        }
        // The header's buttons, right to left: fold, the key, a new chat, and the account pill.
        const float fx = card_.right() - kCardPad - kIconButton;
        toggle.setBounds({ fx, hy, kIconButton, kIconButton });
        static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(keyItem_)])
            .setBounds({ fx - kIconButton - 4.0f, hy, kIconButton, kIconButton });
        static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(newChatItem_)])
            .setBounds({ fx - 2.0f * kIconButton - 8.0f, hy, kIconButton, kIconButton });
        {
            // The account pill, left of them, while signed in (as wide as its label, up to a third of the card).
            auto& pill = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(accountItem_)]);
            const float w = chat_.signedIn ? std::min(pill.preferredWidth(), std::floor(card_.w / 3.0f)) : 0.0f;
            pill.setBounds({ fx - 2.0f * kIconButton - 8.0f - 6.0f - w, hy, w, kIconButton });
            headerLabel_ = accountLabel();
        }
        headerY_ = hy;
        const float headerBottom = hy + kIconButton;

        InputItem& in = inputItem();
        in.setBounds({ inX, card_.y, inW, 200.0f }, kBarH);   // its width first: the rows depend on it
        composerH_ = in.desiredHeight(inW, kBarH);
        usageY_ = card_.bottom() - kCardPad + 2.0f - kUsageH;
        const float composerY = usageY_ - 4.0f - composerH_;
        in.setBounds({ inX, composerY, inW, composerH_ }, kBarH);
        chatView().setBounds({ card_.x + 2.0f, headerBottom + 6.0f, card_.w - 4.0f,
                               std::max(40.0f, composerY - 8.0f - (headerBottom + 6.0f)) });
        reshowChat();                                    // the conversation laid out for its width now
        placeChatBar();
        ++layoutRevision_;
        dirty_ = true;
    }

    // The pills in the message box's bar: as wide as their labels (fast mode's after the thinking one, on a model that
    // takes it), the thinking one narrowed first when they do not fit, then the model's.
    void Panel::placeChatBar()
    {
        const funkgui::Rect bar = inputItem().bar();
        auto& model = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(modelItem_)]);
        auto& thinking = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(thinkingItem_)]);
        auto& fast = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(fastItem_)]);
        auto& web = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(webItem_)]);
        auto& send = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(sendItem_)]);
        auto& attach = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(attachItem_)]);
        const float y = bar.y + std::floor((bar.h - kPillH) * 0.5f) - 3.0f;
        // The pictures' button first, then the pills.
        attach.setBounds({ bar.x + 6.0f, y, kPillH, kPillH });
        const float x0 = bar.x + 6.0f + kPillH + 4.0f;
        model.setBounds({ x0, y, kPillH, kPillH });
        thinking.setBounds({ x0, y, kPillH, kPillH });
        fast.setBounds({ x0, y, kPillH, kPillH });
        web.setBounds({ x0, y, kPillH, kPillH });
        const bool withFast = chatTakesFast(), withWeb = chatTakesWeb();
        const float fw = withFast ? fast.preferredWidth() + 4.0f : 0.0f;
        const float ww = withWeb ? web.preferredWidth() + 4.0f : 0.0f;
        const float room = bar.right() - 6.0f - kSendD - 8.0f - fw - ww - x0;
        float mw = model.preferredWidth(), tw = thinking.preferredWidth();
        if (mw + 4.0f + tw > room)
            tw = std::max(64.0f, room - 4.0f - mw);
        if (mw + 4.0f + tw > room)
            mw = std::max(64.0f, room - 4.0f - tw);
        model.setBounds({ x0, y, mw, kPillH });
        thinking.setBounds({ x0 + mw + 4.0f, y, tw, kPillH });
        fast.setBounds({ x0 + mw + 4.0f + tw + 4.0f, y, withFast ? fw - 4.0f : 0.0f, kPillH });
        web.setBounds({ x0 + mw + 4.0f + tw + 4.0f + fw, y, withWeb ? ww - 4.0f : 0.0f, kPillH });
        send.setBounds({ bar.right() - 6.0f - kSendD, bar.y + std::floor((bar.h - kSendD) * 0.5f) - 3.0f, kSendD, kSendD });
        barLabels_ = barLabels();
    }

    void Panel::resize(int w, int h)
    {
        const float nw = static_cast<float>(std::max(w, kMinWidth));
        const float nh = static_cast<float>(std::max(h, kMinHeight));
        if (nw == width_ && nh == height_)
            return;
        width_ = nw;
        height_ = nh;
        layout();
    }

    // The host closes the open gestures before it lets go of the Panel; no HostServices call from here.
    Panel::~Panel()
    {
        for (const int i : guiKeyboards_)                // a note held as the editor closes is let go
            static_cast<GuiKeyboardItem&>(*items_[static_cast<std::size_t>(i)]).release();
        releasePictures();
    }

    // ---- pictures ------------------------------------------------------------------------------------------------------
    namespace
    {
        // How many Panels hold each picture in FunkGui's registry (the message thread's): the last to let go removes it.
        std::map<uint32_t, int>& pictureUse()
        {
            static std::map<uint32_t, int> use;
            return use;
        }
    }

    void Panel::syncPictures()
    {
        std::vector<const ChatPicture*> want;
        for (const ChatPicturePtr& p : chat_.attached)
            want.push_back(p.get());
        for (const ChatEntry& e : chat_.entries)
            for (const ChatPicturePtr& p : e.pictures)
                want.push_back(p.get());
        std::vector<uint32_t> now;
        for (const ChatPicture* p : want)
        {
            if (p->thumb.empty() || std::find(now.begin(), now.end(), p->id) != now.end())
                continue;
            now.push_back(p->id);
            if (std::find(pictures_.begin(), pictures_.end(), p->id) != pictures_.end())
                continue;
            if (pictureUse()[p->id]++ == 0)
                funkgui::images::set(p->id, p->thumbWidth, p->thumbHeight, p->thumb.data());
            pictures_.push_back(p->id);
        }
        for (auto it = pictures_.begin(); it != pictures_.end();)
        {
            if (std::find(now.begin(), now.end(), *it) != now.end())
            {
                ++it;
                continue;
            }
            if (--pictureUse()[*it] <= 0)
            {
                pictureUse().erase(*it);
                funkgui::images::remove(*it);
            }
            it = pictures_.erase(it);
        }
    }

    void Panel::releasePictures()
    {
        for (const uint32_t id : pictures_)
            if (--pictureUse()[id] <= 0)
            {
                pictureUse().erase(id);
                funkgui::images::remove(id);
            }
        pictures_.clear();
    }

    void Panel::openPictures()
    {
        if (host_ == nullptr)
            return;
        funkgui::FileRequest request;
        request.mode = funkgui::FileRequest::Mode::openMany;
        request.title = "Add pictures for Claude";
        request.pattern = "*.png;*.jpg;*.jpeg;*.gif;*.webp;*.heic;*.heif;*.tif;*.tiff;*.bmp";
        host_->chooseFiles(request, [this](const std::vector<std::string>& paths) {
            if (paths.empty())
                return;
            port_.chat().attach(paths);
            chatAt_ = -1.0;
            dirty_ = true;
        });
    }

    bool Panel::filesInterest(const std::vector<std::string>& paths) const
    {
        if (dialogOpen())
            return false;
        return std::any_of(paths.begin(), paths.end(), [this](const std::string& p) { return (chatOpen_ && isPictureName(p)) || isProjectFile(p); });
    }

    void Panel::filesDragEnter(const std::vector<std::string>&, float x, float y)
    {
        dropActive_ = true;
        dropHover_ = card_.contains({ x, y });
        dirty_ = true;
    }

    void Panel::filesDragMove(float x, float y)
    {
        const bool over = card_.contains({ x, y });
        if (over != dropHover_)
        {
            dropHover_ = over;
            dirty_ = true;
        }
    }

    void Panel::filesDragExit()
    {
        dropActive_ = dropHover_ = false;
        dirty_ = true;
    }

    void Panel::filesDropped(const std::vector<std::string>& paths)
    {
        const bool onChat = dropHover_;
        dropActive_ = dropHover_ = false;
        dirty_ = true;
        std::vector<std::string> projects;
        for (const std::string& p : paths)
            if (isProjectFile(p))
                projects.push_back(p);
        if (!projects.empty())
            importFiles(projects);                       // anywhere on the panel
        if (!onChat)
            return;
        std::vector<std::string> pictures;
        for (const std::string& p : paths)
            if (isPictureName(p))
                pictures.push_back(p);
        if (!pictures.empty())
        {
            port_.chat().attach(pictures);
            chatAt_ = -1.0;
        }
    }

    funkgui::Point Panel::centreOfAttach() const { return centreOf(attachItem_); }
    const InputItem& Panel::chatInputItem() const { return inputItem(); }

    std::optional<funkgui::Point> Panel::centreOfDial(int slot) const
    {
        if (slot < 0 || slot >= kMaxControls)
            return std::nullopt;
        const Item& item = *items_[static_cast<std::size_t>(itemOfSlot_[static_cast<std::size_t>(slot)])];
        if (!item.shown())
            return std::nullopt;
        return item.centre();
    }

    funkgui::Point Panel::centreOfRun() const { return items_[static_cast<std::size_t>(runItem_)]->centre(); }
    funkgui::Point Panel::centreOfExamples() const
    {
        return items_[static_cast<std::size_t>(examplesItem_)]->centre();
    }
    const DisplayItem& Panel::displayItem(int d) const
    {
        return static_cast<const DisplayItem&>(
            *items_[static_cast<std::size_t>(itemOfDisplay_[static_cast<std::size_t>(std::clamp(d, 0, kMaxDisplays - 1))])]);
    }

    funkgui::Point Panel::centreOfButton(int slot, int value) const
    {
        const ItemPlace& p = dials_.dials[static_cast<std::size_t>(std::clamp(slot, 0, kMaxControls - 1))];
        return { p.x + 0.5f * p.w, p.top + kButtonsTop + (static_cast<float>(value) + 0.5f) * kButtonRowH };
    }

    funkgui::Point Panel::centreOfLibrary() const { return items_[static_cast<std::size_t>(libraryItem_)]->centre(); }
    funkgui::Point Panel::centreOfRandom() const { return items_[static_cast<std::size_t>(randomItem_)]->centre(); }
    funkgui::Point Panel::centreOfProjects() const { return items_[static_cast<std::size_t>(projectsItem_)]->centre(); }
    funkgui::Point Panel::centreOfProblem(int i) const
    {
        const auto& p = static_cast<const ProblemsItem&>(*items_[static_cast<std::size_t>(problemsItem_)]);
        const funkgui::Rect r = p.rowRect(i);
        return { r.x + 20.0f, r.centreY() };
    }

    funkgui::Point Panel::centreOf(int item) const { return items_[static_cast<std::size_t>(item)]->centre(); }
    funkgui::Point Panel::centreOfSend() const { return centreOf(sendItem_); }
    funkgui::Point Panel::centreOfModel() const { return centreOf(modelItem_); }
    funkgui::Point Panel::centreOfNewChat() const { return centreOf(newChatItem_); }
    funkgui::Point Panel::centreOfKey() const { return centreOf(keyItem_); }

    funkgui::Point Panel::centreOfChatToggle() const { return centreOf(chatToggleItem_); }

    // ---- the explorer (Explorer.h) ---------------------------------------------------------------------------------
    funkgui::Point Panel::centreOfExplorerToggle() const { return centreOf(explorerToggleItem_); }

    ExplorerItem& Panel::explorer() { return static_cast<ExplorerItem&>(*items_[static_cast<std::size_t>(explorerItem_)]); }

    const ExplorerItem& Panel::explorer() const
    {
        return static_cast<const ExplorerItem&>(*items_[static_cast<std::size_t>(explorerItem_)]);
    }

    // The chevron folds it; the button in the program's card's header opens it, folding the chat when the window has no
    // room for both.
    void Panel::toggleExplorer()
    {
        if (explorerShown_)
        {
            setExplorerOpen(false);
            return;
        }
        setExplorerOpen(true);
        if (!explorerShown_ && chatOpen_)
            setChatOpen(false);
    }

    void Panel::setExplorerOpen(bool open)
    {
        if (open == explorerOpen_)
            return;
        explorerOpen_ = open;
        port_.setExplorerOpen(open);
        if (open)
        {
            projectsAt_ = -100.0;                        // the projects read again at once
            explorerKey_.clear();
        }
        layout();
        dirty_ = true;
    }

    // The card's header (its title, drawn by draw(), and the chevron at its right) and the list under it; not shown, the
    // button at the left of the program's card's header, the status after it.
    void Panel::layoutExplorer()
    {
        auto& toggle = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(explorerToggleItem_)]);
        ExplorerItem& ex = explorer();
        ex.setHidden(!explorerShown_);
        const funkgui::Rect& r = explorerCard_;
        if (!explorerShown_)
        {
            toggle.setBounds({ leftX_ + kInset - 4.0f, kActionsY + 0.5f * (kActionH - 26.0f), 26.0f, 26.0f });
            ex.setBounds({});
            return;
        }
        const float hy = r.y + kCardPad;
        toggle.setBounds({ r.right() - kCardPad - kIconButton, hy, kIconButton, kIconButton });
        const float top = hy + kIconButton + 6.0f;
        ex.setBounds({ r.x + 2.0f, top, r.w - 4.0f, std::max(0.0f, r.bottom() - 6.0f - top) });
    }

    // The explorer's rows from what they show (the tabs, the errors, the library files the program has, the projects
    // and the open one's presets), rebuilt only when that changes. The projects are read again every three seconds while
    // it is open (one saved by another instance, or by PROJECTS, appears), the presets when the project open changes.
    void Panel::syncExplorer()
    {
        if (!explorerShown_ || explorerItem_ < 0)
            return;
        const double now = host_ != nullptr ? host_->nowSeconds() : 0.0;
        const ProjectState st = port_.project();
        if (projectsAt_ < 0.0 || now - projectsAt_ > 3.0 || presetsOf_ != st.name)
        {
            projectList_ = port_.projects();
            std::sort(projectList_.begin(), projectList_.end(),
                      [](const ProjectEntry& a, const ProjectEntry& b) { return a.when != b.when ? a.when > b.when : a.name < b.name; });
            presetList_ = st.name.empty() ? std::vector<PresetEntry>() : port_.presets(st.name);
            presetsOf_ = st.name;
            projectsAt_ = now;
        }
        const auto erred = [this](const std::string& file) {
            return std::any_of(problems_.begin(), problems_.end(), [&](const Problem& p) { return p.error && p.file == file; });
        };
        std::string key;
        for (int i = 0; i < files().count(); ++i)
            key += files().name(i) + (i == files().active() ? "*" : "") + (erred(files().name(i)) ? "!" : "") + '\x1f';
        key += files().count() < static_cast<int>(kMaxFiles) ? "+" : "-";
        key += '\x1c' + st.name + '\x1d' + st.preset;
        for (const ProjectEntry& e : projectList_)
            key += '\x1e' + e.name + '\x1d' + e.when + '\x1d' + std::to_string(e.latest);
        for (const PresetEntry& e : presetList_)
            key += '\x1b' + e.name + '\x1d' + std::to_string(e.from);
        if (key == explorerKey_)
            return;
        explorerKey_ = std::move(key);

        using S = ExplorerItem::Section;
        std::vector<ExplorerItem::Group> groups;
        {
            ExplorerItem::Group g;
            g.section = S::files;
            g.title = "FILES";
            for (int i = 0; i < files().count(); ++i)
            {
                ExplorerItem::Row r;
                r.label = files().name(i);
                const bool gui = r.label.size() > 4 && r.label.substr(r.label.size() - 4) == ".gui";
                r.icon = i == 0 ? chat::Icon::code : gui ? chat::Icon::dial : chat::Icon::file;
                r.active = i == files().active();
                r.error = erred(r.label);
                r.closable = i > 0;
                r.index = i;
                g.rows.push_back(std::move(r));
            }
            if (files().count() < static_cast<int>(kMaxFiles))
            {
                g.action = chat::Icon::plus;
                g.actionTitle = "New file";
            }
            groups.push_back(std::move(g));
        }
        {
            ExplorerItem::Group g;
            g.section = S::presets;
            g.title = "PRESETS";
            if (!st.name.empty())
            {
                g.action = chat::Icon::plus;
                g.actionTitle = "Save a preset";
            }
            g.empty = st.name.empty() ? "Save the project to keep presets" : "None yet: + saves the dials";
            for (const PresetEntry& e : presetList_)
            {
                ExplorerItem::Row r;
                r.label = e.name;
                r.detail = e.from > 0 ? "v" + std::to_string(e.from) : std::string();
                r.icon = chat::Icon::dial;
                r.active = e.name == st.preset;
                r.key = e.name;
                g.rows.push_back(std::move(r));
            }
            groups.push_back(std::move(g));
        }
        {
            ExplorerItem::Group g;
            g.section = S::projects;
            g.title = "PROJECTS";
            g.action = chat::Icon::plus;
            g.actionTitle = "Save the project";
            g.empty = "None yet: + saves this program";
            for (const ProjectEntry& e : projectList_)
            {
                ExplorerItem::Row r;
                r.label = e.name;
                r.detail = "v" + std::to_string(e.latest);
                r.icon = chat::Icon::list;
                r.active = e.name == st.name;
                r.key = e.name;
                g.rows.push_back(std::move(r));
            }
            groups.push_back(std::move(g));
        }
        {
            ExplorerItem::Group g;
            g.section = S::library;
            g.title = "LIBRARY";
            std::string category;
            for (std::size_t i = 0; i < std::size(kLibrary); ++i)
            {
                const LibraryFile& f = kLibrary[i];
                if (category != f.category)
                {
                    category = f.category;
                    ExplorerItem::Row h;
                    h.label = upper(category);
                    h.heading = true;
                    g.rows.push_back(std::move(h));
                }
                ExplorerItem::Row r;
                r.label = f.name;
                r.icon = chat::Icon::blocks;
                r.marked = files().find(f.name) >= 0;
                r.index = static_cast<int>(i);
                g.rows.push_back(std::move(r));
            }
            groups.push_back(std::move(g));
        }
        {
            ExplorerItem::Group g;
            g.section = S::examples;
            g.title = "EXAMPLES";
            for (std::size_t i = 0; i < std::size(kExamples); ++i)
            {
                ExplorerItem::Row r;
                r.label = kExamples[i].title;
                r.icon = chat::Icon::play;
                r.index = static_cast<int>(i);
                g.rows.push_back(std::move(r));
            }
            groups.push_back(std::move(g));
        }
        explorer().setContent(std::move(groups));
        dirty_ = true;
    }

    void Panel::openFromExplorer(ExplorerItem::Section section, const ExplorerItem::Row& row)
    {
        using S = ExplorerItem::Section;
        switch (section)
        {
            case S::files:
                if (row.index >= 0 && row.index < files().count())
                {
                    files().endRename();
                    files().select(row.index);
                    setFocus(editorItem_);
                }
                break;
            case S::library:
                if (row.index >= 0 && row.index < static_cast<int>(std::size(kLibrary))
                    && !addLibraryFile(kLibrary[static_cast<std::size_t>(row.index)].name))
                    notice("NO ROOM FOR ANOTHER TAB: CLOSE ONE FIRST");
                break;
            case S::examples:
                loadExample(row.index);
                break;
            case S::presets:
                applyPresetNamed(row.key);
                break;
            case S::projects:
                openProjectNamed(row.key);
                break;
        }
        explorerKey_.clear();                            // what it shows may have changed (a tab, a tick)
        dirty_ = true;
    }

    // A project from the explorer: its newest version, as PROJECTS' Open, asking first (a menu at the pointer) when the
    // editor's files have changes no version holds.
    void Panel::openProjectNamed(const std::string& name)
    {
        const auto load = [this, name] {
            std::string why;
            if (port_.openProject(name, 0, why))
                notice("OPENED: " + upper(name) + " V" + std::to_string(port_.project().version));
            else
                notice("NOT OPENED: " + upper(why));
            projectsAt_ = -100.0;
            dirty_ = true;
        };
        files().endRename();
        port_.setDraft(files().draft());                 // what "edited" means is what the tabs show
        if (!port_.project().edited || host_ == nullptr)
        {
            load();
            return;
        }
        funkgui::MenuRequest request;
        request.items.push_back({ 1, "Open " + name + " (the editor's changes are lost)" });
        request.anchor = { pointer_.x, pointer_.y, 1.0f, 1.0f };
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [load](int chosen) {
            if (chosen == 1)
                load();
        });
    }

    // A preset of the project open, from the explorer: its dials, at once.
    void Panel::applyPresetNamed(const std::string& name)
    {
        const std::string project = port_.project().name;
        std::string why;
        if (project.empty())
            return;
        if (port_.applyPreset(project, name, why))
            notice("PRESET: " + upper(name));
        else
            notice("NOT APPLIED: " + upper(why));
        dirty_ = true;
    }

    void Panel::saveVersion()
    {
        files().endRename();
        port_.setDraft(files().draft());                 // what is saved is what the tabs show
        const ProjectState st = port_.project();
        if (st.name.empty())
        {
            openProjects();
            return;
        }
        SavedVersion saved;
        std::string why;
        if (!port_.saveVersion(st.name, saved, why))
        {
            notice("NOT SAVED: " + upper(why));
            return;
        }
        const std::string at = upper(st.name) + " V" + std::to_string(saved.number);
        notice(saved.unchanged ? "NOTHING NEW TO SAVE: " + at + " HAS IT" : "SAVED: " + at);
        projectsAt_ = -100.0;
        dirty_ = true;
    }

    void Panel::setChatOpen(bool open)
    {
        if (open == chatOpen_)
            return;
        chatOpen_ = open;
        port_.setChatOpen(open);
        if (open)
            unread_ = false;
        else
        {
            entriesAtFold_ = chat_.entries.size();
            unread_ = false;
            // The focus leaves the chat's items with them.
            const int f = focus_;
            if (f == inputItem_ || f == modelItem_ || f == thinkingItem_ || f == fastItem_ || f == webItem_ || f == sendItem_
                || f == attachItem_ || f == newChatItem_
                || f == keyItem_ || f == accountItem_)
                setFocus(panelView_ ? -1 : editorItem_);
        }
        layout();
    }

    std::optional<funkgui::Point> Panel::centreOfAccount() const
    {
        if (!chat_.signedIn)
            return std::nullopt;
        return centreOf(accountItem_);
    }

    std::string Panel::accountLabel() const
    {
        if (!chat_.signedIn)
            return {};
        std::string name = asciiText(chat_.account.empty() ? std::string("Account") : chat_.account);
        if (name.size() > 16)
            name = name.substr(0, 15) + "~";
        if (!chat_.details.known)
            return name;
        char credit[32];
        std::snprintf(credit, sizeof credit, "  $%.2f", chat_.details.balanceUsd);
        return name + credit;
    }
    funkgui::Point Panel::centreOfThinking() const { return centreOf(thinkingItem_); }
    std::optional<funkgui::Point> Panel::centreOfFast() const
    {
        if (!items_[static_cast<std::size_t>(fastItem_)]->shown())
            return std::nullopt;
        return centreOf(fastItem_);
    }
    std::optional<funkgui::Point> Panel::centreOfWeb() const
    {
        if (!items_[static_cast<std::size_t>(webItem_)]->shown())
            return std::nullopt;
        return centreOf(webItem_);
    }
    funkgui::Point Panel::centreOfDialogTab(bool account) const
    {
        const funkgui::Rect r = account ? keyDialog().accountTab : keyDialog().keyTab;
        return { r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfKeyField() const
    {
        const funkgui::Rect r = keyDialog().field;
        return { r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfKeySave() const
    {
        const funkgui::Rect r = keyDialog().primary;
        return { r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfKeyCancel() const
    {
        const funkgui::Rect r = keyDialog().secondary;
        return { r.centreX(), r.centreY() };
    }
    std::optional<funkgui::Point> Panel::centreOfKeyRemove() const
    {
        if (tab_ != DialogTab::key || !chat_.hasKey)
            return std::nullopt;
        const funkgui::Rect r = keyDialog().danger;
        return funkgui::Point{ r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfUsername() const
    {
        const funkgui::Rect r = keyDialog().user;
        return { r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfPassword() const
    {
        const funkgui::Rect r = keyDialog().pass;
        return { r.centreX(), r.centreY() };
    }
    std::optional<funkgui::Point> Panel::centreOfSignOut() const
    {
        if (tab_ != DialogTab::account || dialogButtons().danger.empty())
            return std::nullopt;
        const funkgui::Rect r = keyDialog().danger;
        return funkgui::Point{ r.centreX(), r.centreY() };
    }
    std::optional<funkgui::Point> Panel::centreOfUseThis() const
    {
        const DialogButtons b = dialogButtons();
        const bool primary = b.primary == "Use account";
        if (!primary && b.extra != "Use this key")
            return std::nullopt;
        const funkgui::Rect r = primary ? keyDialog().primary : keyDialog().extra;
        return funkgui::Point{ r.centreX(), r.centreY() };
    }

    ChatView& Panel::chatView() { return static_cast<ChatView&>(*items_[static_cast<std::size_t>(chatViewItem_)]); }
    const ChatView& Panel::chatView() const
    {
        return static_cast<const ChatView&>(*items_[static_cast<std::size_t>(chatViewItem_)]);
    }
    InputItem& Panel::inputItem() { return static_cast<InputItem&>(*items_[static_cast<std::size_t>(inputItem_)]); }
    const InputItem& Panel::inputItem() const
    {
        return static_cast<const InputItem&>(*items_[static_cast<std::size_t>(inputItem_)]);
    }
    funkgui::TextEditor& Panel::chatInput() noexcept { return inputItem().editor(); }
    const std::string& Panel::chatPlaceholder() const { return inputItem().placeholder(); }
    const std::string& Panel::transcript() const { return chatView().text(); }

    void Panel::attach(funkgui::HostServices& host)
    {
        host_ = &host;
        gestures_.emplace(host);
        if (zoom_ != nullptr)
            static_cast<ZoomModel*>(zoom_)->attach(host);
        files().attach(&host);
        chatView().attach(&host);
        inputItem().attach(&host);
    }

    int Panel::width() const { return static_cast<int>(width_); }
    int Panel::height() const { return static_cast<int>(height_); }

    // ---- the processor's state ------------------------------------------------------------------------------------------
    namespace
    {
        FilesItem& filesIn(std::vector<std::unique_ptr<Item>>& items, int i)
        {
            return static_cast<FilesItem&>(*items[static_cast<std::size_t>(i)]);
        }
    }

    FilesItem& Panel::files() { return filesIn(items_, editorItem_); }
    const FilesItem& Panel::files() const
    {
        return static_cast<const FilesItem&>(*items_[static_cast<std::size_t>(editorItem_)]);
    }
    funkgui::TextEditor& Panel::editor() noexcept { return files().editor(); }
    CodeAssist& Panel::codeAssist() noexcept { return files().assist(); }
    int Panel::fileCount() const { return files().count(); }
    std::string Panel::fileName(int i) const { return i >= 0 && i < files().count() ? files().name(i) : std::string(); }
    int Panel::activeFile() const { return files().active(); }
    bool Panel::renamingFile() const { return files().renaming(); }
    funkgui::Point Panel::centreOfTab(int i) const
    {
        const funkgui::Rect r = files().tabRect(i);
        return { r.x + kTabPad + 4.0f, r.centreY() };
    }
    funkgui::Point Panel::centreOfNewTab() const
    {
        const funkgui::Rect r = files().newTabRect();
        return { r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfCloseTab(int i) const
    {
        const funkgui::Rect r = files().closeRect(i);
        return { r.centreX(), r.centreY() };
    }

    void Panel::runNow()
    {
        files().endRename();
        if (files().assist().formatOnRun())
            files().assist().formatAll();
        const Draft draft = files().draft();
        port_.setDraft(draft);
        syncedFiles_ = files().revision();
        port_.run(draft.files);
        compiling_ = true;
        dirty_ = true;
    }

    void Panel::refresh(double nowSec)
    {
        // The draft, when a host loaded a session while the editor was open, or Claude changed the files.
        bool textChanged = false;
        if (const uint64_t d = port_.draftRevision(); d != seenDraft_)
        {
            seenDraft_ = d;
            files().merge(port_.draft());
            syncedFiles_ = files().revision();
            textChanged = true;
            dirty_ = true;
        }
        // The files as typed, and the tab shown, to the processor (it outlives this window).
        if (files().revision() != syncedFiles_)
        {
            port_.setDraft(files().draft());
            syncedFiles_ = files().revision();
            textChanged = true;
        }
        // The editor's check while typing (CodeAssist.h): the files once they have rested, its messages when it is done.
        if (files().assist().takeCheckRequest())
        {
            port_.check(files().draft().files);
            files().assist().checkSent();
        }
        if (files().assist().checking())
            if (const CheckStatus c = port_.checkStatus(); files().assist().takeCheckResult(c.revision))
            {
                files().assist().setDiagnostics(diagnosticsOf(parseProblems(c.errors)), c.sources);
                files().assist().checkDone();
                dirty_ = true;
            }
        // A finished compile: its messages, and the dials of the program that plays.
        bool statusChanged = false;
        if (const uint64_t r = port_.statusRevision(); r != seenStatus_)
        {
            seenStatus_ = r;
            const CompileStatus s = port_.status();
            lastOk_ = s.ok;
            lastMs_ = s.ms;
            problems_ = parseProblems(s.errors + s.notes);
            files().assist().setDiagnostics(diagnosticsOf(problems_), s.sources);
            // The dials and the source of the program that plays: this compile's when it compiled (the processor
            // takes them on its own timer, which may not have run yet), else the ones before.
            const Controls cs = s.ok ? s.controls : port_.controls();
            if (s.ok)
                running_ = s.sources;
            const PanelSpec spec = s.ok ? s.panel : port_.panel();
            ++controlsRevision_;
            for (int c = 0; c < kMaxControls; ++c)
                if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0)
                    static_cast<DialItem&>(*items_[static_cast<std::size_t>(i)])
                        .model()
                        .setControl(cs[static_cast<std::size_t>(c)], spec.labels[static_cast<std::size_t>(c)],
                                    spec.options[static_cast<std::size_t>(c)], controlsRevision_);
            for (int c = 0; c < kMaxControls; ++c)
                if (const int i = itemOfGuiSlot_[static_cast<std::size_t>(c)]; i >= 0)
                    static_cast<GuiControlItem&>(*items_[static_cast<std::size_t>(i)])
                        .value()
                        .set(cs[static_cast<std::size_t>(c)], spec.labels[static_cast<std::size_t>(c)],
                             spec.options[static_cast<std::size_t>(c)]);
            // Other dials or another panel: their places again (a second row takes height from the editor).
            bool sameDials = spec == panelSpec_;
            for (int c = 0; c < kMaxControls && sameDials; ++c)
            {
                const Control& a = cs[static_cast<std::size_t>(c)];
                const Control& b = controls_[static_cast<std::size_t>(c)];
                sameDials = a.used == b.used && std::strcmp(a.name, b.name) == 0;
            }
            controls_ = cs;
            const bool otherGui = !(spec.gui == panelSpec_.gui);
            // Another interface from its top, unless it is this one edited (as many elements); an edited one the
            // same pages, by their titles (as the session keeps them).
            if (otherGui && spec.gui.nodes.size() != panelSpec_.gui.nodes.size())
                guiScrollX_ = guiScrollY_ = 0.0f;
            const bool otherShaders = !(spec.shaders == panelSpec_.shaders);
            panelSpec_ = spec;
            if (otherGui)
                guiPages_ = guiPagesOf(panelSpec_.gui, port_.guiPages());
            if (otherShaders || otherGui)
            {
                registerShaders(s.notes);
                if (sameDials)
                    layoutGuiItems();
            }
            panelProgram_ = s.ok ? s.request : port_.program();
            if (!sameDials)
                layout();
            statusChanged = true;
        }
        compiling_ = port_.compiling();
        if (textChanged || statusChanged)
            edited_ = files().draft().files != running_;
        const std::string statusBefore = status_;

        // The project open, in words.
        {
            const ProjectState st = port_.project();
            std::string text;
            if (!st.name.empty())
            {
                text = "PROJECT  " + upper(st.name) + (st.version > 0 ? "  V" + std::to_string(st.version) : "")
                     + (st.edited || (st.preset.empty() && st.dialsEdited) ? "  EDITED" : "");
                if (!st.preset.empty())
                    text += "  PRESET  " + upper(st.preset) + (st.dialsEdited ? "  EDITED" : "");
            }
            if (text != projectText_)
            {
                projectText_ = std::move(text);
                dirty_ = true;
            }
        }
        // The status line and the meter.
        if (nowSec < noticeUntil_)
            status_ = notice_;
        else if (compiling_)
            status_ = "COMPILING";
        else if (!lastOk_)
        {
            int errors = 0;
            for (const Problem& p : problems_)
                errors += p.error ? 1 : 0;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%d ERROR%s   THE LAST PROGRAM THAT COMPILED PLAYS ON", errors,
                          errors == 1 ? "" : "S");
            status_ = buf;
        }
        else
        {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s   COMPILED IN %.0f MS", edited_ ? "EDITED: CMD-RETURN RUNS IT" : "RUNNING",
                          lastMs_);
            status_ = buf;
        }
        if (status_ != statusBefore)
            dirty_ = true;

        // The meter, four times a second at most: a frame for it when it changed.
        if (meterAt_ < 0.0 || nowSec - meterAt_ >= 0.25 || nowSec < meterAt_)
        {
            meterAt_ = nowSec;
            char meter[64];
            const uint32_t a = port_.alerts();
            std::snprintf(meter, sizeof meter, "%s%s%sCPU %.1f%%",
                          (a & Engine::nonFinite) != 0 ? "NAN REPLACED   " : "",
                          (a & Engine::clipped) != 0 ? "CLIPPED   " : "",
                          port_.frozen() ? "FROZEN   " : "",       // the dials compiled in, while they hold still
                          static_cast<double>(port_.load()) * 100.0);
            if (meter_ != meter)
            {
                meter_ = meter;
                dirty_ = true;
            }
        }
        refreshChat(nowSec);
    }

    // ---- the chat -----------------------------------------------------------------------------------------------------
    namespace
    {
        // What an empty conversation shows: a title, a line, and suggestions to start from.
        ChatView::Empty emptyChat(const ChatState& s)
        {
            ChatView::Empty e;
            e.title = "What shall we build?";
            e.body = product::kIsInstrument
                         ? "Describe a sound and Claude writes the voice in FMinus, runs it, measures it and sets the "
                           "dials. Ask it about FMinus, synthesis or the code, too."
                         : "Describe an effect and Claude writes it in FMinus, runs it, measures it and sets the dials. "
                           "Ask it about FMinus, DSP or the code, too.";
            if (!s.ready() && s.useAccount && !s.accountProblem.empty())
                e.body += " Sign in again: " + s.accountProblem;
            else if (!s.ready())
                e.body += " First sign in with your TestingPlayground account (its credit pays for Claude), or add your "
                          "own Anthropic API key (billed to you).";
            if (!s.useAccount && !s.keyProblem.empty())
                e.body += " The stored key cannot be used: " + s.keyProblem;
            e.body = asciiText(e.body);
            if (product::kIsInstrument)
                e.suggestions = { "A warm detuned pad with a slow filter sweep",
                                  "A plucked string with a soft attack and a long decay",
                                  "A punchy bass with a filter envelope and some drive", "Explain what this voice does" };
            else
                e.suggestions = { "A tape delay with wow and flutter", "A lush stereo chorus",
                                  "A plate reverb with a damping dial", "Explain what this program does" };
            return e;
        }

        // The line under the message box: what Claude is doing and for how long, else the last reply's tokens.
        std::string activityText(const ChatState& s, double seconds)
        {
            if (s.busy)
            {
                char buf[128];
                std::snprintf(buf, sizeof buf, "%s, %.0f s", s.activity.empty() ? "Working" : asciiText(s.activity).c_str(),
                              std::max(0.0, seconds));
                return buf;
            }
            const std::string credit = s.useAccount ? s.credit : std::string();
            return asciiText(s.usage.empty() || credit.empty() ? s.usage + credit : s.usage + "; " + credit);
        }

        std::string effortName(const std::string& e)
        {
            if (e == "xhigh")
                return "Extra high";
            std::string n = e;
            if (!n.empty())
                n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
            return n;
        }
    }

    std::string Panel::modelLabel() const
    {
        const auto shortName = [this](const std::string& id) {
            for (const ChatModel& m : chat_.models)
                if (m.id == id)
                    return m.name.rfind("Claude ", 0) == 0 ? m.name.substr(7) : m.name;
            return id;
        };
        const std::string builder = chat_.model.empty() ? std::string("Model") : shortName(chat_.model);
        // With a planner: "Fable 5.1 > Sonnet 5.5", the plan's model first.
        return chat_.planner.empty() ? builder : shortName(chat_.planner) + " > " + builder;
    }

    std::vector<std::string> Panel::chatEfforts() const
    {
        for (const ChatModel& m : chat_.models)
            if (m.id == chat_.model)
                return m.efforts;
        return {};
    }

    bool Panel::chatTakesFast() const
    {
        for (const ChatModel& m : chat_.models)
            if (m.id == chat_.model)
                return m.fast;
        return false;
    }

    bool Panel::chatTakesWeb() const
    {
        for (const ChatModel& m : chat_.models)
            if (m.id == chat_.model)
                return m.web;
        return false;
    }

    // What the bar's layout depends on: the pills' labels, and whether fast mode's is there.
    std::string Panel::barLabels() const
    {
        return modelLabel() + '|' + thinkingLabel() + (chatTakesFast() ? "|fast" : "") + (chatTakesWeb() ? "|web" : "");
    }

    std::string Panel::thinkingLabel() const
    {
        return chat_.effort.empty() || chatEfforts().empty() ? std::string("Thinking") : "Thinking: " + effortName(chat_.effort);
    }

    void Panel::refreshChat(double nowSec)
    {
        ChatPort& chat = port_.chat();
        const uint64_t r = chat.chatRevision();
        // The conversation as it streams, 30 times a second at most; the key's state once a second (the other playground
        // plugin may have stored one).
        const bool changed = r != seenChat_ && (chatAt_ < 0.0 || nowSec - chatAt_ >= 1.0 / 30.0 || nowSec < chatAt_
                                               || !chat_.busy);
        const bool stale = chatAt_ < 0.0 || nowSec - chatAt_ >= 1.0 || nowSec < chatAt_;
        if (changed || stale)
        {
            seenChat_ = r;
            chatAt_ = nowSec;
            const bool hadKey = chat_.ready();
            const bool wasBusy = chat_.busy;
            chat_ = chat.chatState();
            if (chat_.busy && !wasBusy)
                busySince_ = nowSec;
            syncPictures();
            {
                const float strip = inputItem().stripHeight();
                inputItem().setPictures(chat_.attached, chat_.attaching);
                if (inputItem().stripHeight() != strip)
                    layoutChat();                        // the row of pictures came or went: the box grows or shrinks
            }
            chatView().show(chat_, emptyChat(chat_));
            inputItem().setPlaceholder(chat_.busy     ? "Claude is working (the square button stops it)"
                                       : chat_.asking ? "Answer above, or write your own answer"
                                       : (product::kIsInstrument ? "Ask Claude for a sound, or about the code"
                                                                 : "Ask Claude for an effect, or about the code"));
            if (barLabels() != barLabels_)
                placeChatBar();
            if (accountLabel() != headerLabel_)          // signed in or out, or the credit came
                layoutChat();
            if (!chatOpen_ && !unread_ && chat_.entries.size() > entriesAtFold_)
            {
                unread_ = true;                          // the rail's dot
                dirty_ = true;
            }
            if (hadKey != chat_.ready() || changed)
            {
                dirty_ = true;
                ++chatA11y_;
            }
            if (awaitingSignIn_ && keyOpen_ && !chat_.signingIn)
                afterSignIn();
        }
        const std::string a = activityText(chat_, nowSec - busySince_);
        if (a != activity_)
        {
            activity_ = a;
            dirty_ = true;
        }
        // Claude Desktop's name for this instance, while it talks to it (the chat card's subtitle says so).
        const std::string desktop = port_.desktopConnected() ? port_.desktopName() : std::string();
        if (desktop != desktop_)
        {
            desktop_ = desktop;
            dirty_ = true;
            ++chatA11y_;
        }
    }

    void Panel::reshowChat()
    {
        chatView().show(chat_, emptyChat(chat_));
        ++chatA11y_;
        dirty_ = true;
    }

    void Panel::suggest(const std::string& text)
    {
        funkgui::TextEditor& in = inputItem().editor();
        in.setText(text);
        in.setCaret(in.endPos());
        setFocus(inputItem_);
        dirty_ = true;
    }

    void Panel::openThinking()
    {
        const std::vector<std::string> efforts = chatEfforts();
        if (host_ == nullptr || efforts.empty())
            return;
        funkgui::MenuRequest request;
        int id = 1;
        for (const std::string& e : efforts)
        {
            funkgui::MenuItem item;
            item.id = id++;
            item.label = effortName(e) + (e == "medium" ? " (the default)" : "");
            item.checked = e == chat_.effort;
            request.items.push_back(std::move(item));
        }
        request.anchor = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(thinkingItem_)]).bounds();
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this, efforts](int chosen) {
            if (chosen < 1 || chosen > static_cast<int>(efforts.size()))
                return;
            port_.chat().setEffort(efforts[static_cast<std::size_t>(chosen - 1)]);
            chatAt_ = -1.0;
            dirty_ = true;
        });
    }

    void Panel::sendChat()
    {
        funkgui::TextEditor& in = inputItem().editor();
        const std::string text = in.text();
        if ((text.find_first_not_of(" \t\n") == std::string::npos && chat_.attached.empty()) || chat_.busy)
            return;
        if (!chat_.ready())
        {
            pendingSend_ = text;                         // it goes once a key is saved, or the user signs in
            openKeyDialog();
            return;
        }
        port_.chat().send(text);
        in.setText("");
        chatAt_ = -1.0;                                  // the user's message, shown at once
        refreshChat(host_ != nullptr ? host_->nowSeconds() : 0.0);
        dirty_ = true;
    }

    void Panel::openModels()
    {
        if (host_ == nullptr || chat_.models.empty())
            return;
        // Two choices: the model that builds (writes the files, runs, measures, answers), and the one that plans each
        // request first, or none (ChatAgent.h, "A planner").
        constexpr int kPlanNone = 1000, kPlanFirst = 1001, kHeading = 2000;
        funkgui::MenuRequest request;
        request.items.push_back({ kHeading, "BUILD WITH", false });
        int id = 1;
        for (const ChatModel& m : chat_.models)
        {
            funkgui::MenuItem item;
            item.id = id++;
            item.label = m.name;
            item.checked = m.id == chat_.model;
            request.items.push_back(std::move(item));
        }
        request.items.push_back({ 0, {}, false, false, true });
        request.items.push_back({ kHeading + 1, "PLAN WITH (EACH REQUEST, FIRST)", false });
        {
            funkgui::MenuItem none;
            none.id = kPlanNone;
            none.label = "No planner";
            none.checked = chat_.planner.empty();
            request.items.push_back(std::move(none));
        }
        id = kPlanFirst;
        for (const ChatModel& m : chat_.models)
        {
            funkgui::MenuItem item;
            item.id = id++;
            item.label = m.name;
            item.checked = m.id == chat_.planner;
            request.items.push_back(std::move(item));
        }
        request.anchor = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(modelItem_)]).bounds();
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        const std::vector<ChatModel> models = chat_.models;
        host_->showMenu(request, [this, models](int chosen) {
            const int n = static_cast<int>(models.size());
            if (chosen >= 1 && chosen <= n)
                port_.chat().setModel(models[static_cast<std::size_t>(chosen - 1)].id);
            else if (chosen == kPlanNone)
                port_.chat().setPlanner({});
            else if (chosen >= kPlanFirst && chosen < kPlanFirst + n)
                port_.chat().setPlanner(models[static_cast<std::size_t>(chosen - kPlanFirst)].id);
            else
                return;
            chatAt_ = -1.0;
            dirty_ = true;
        });
    }

    void Panel::newChat()
    {
        const auto forget = [this] {
            port_.chat().newChat();
            chatAt_ = -1.0;
            dirty_ = true;
        };
        if (chat_.entries.empty() || host_ == nullptr)
        {
            forget();
            return;
        }
        funkgui::MenuRequest request;
        request.items.push_back({ 1, chat_.busy ? "Stop Claude and start a new chat (this one is forgotten)"
                                                : "Start a new chat (this one is forgotten)" });
        request.anchor = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(newChatItem_)]).bounds();
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [forget](int chosen) {
            if (chosen == 1)
                forget();
        });
    }

    // ---- the access dialog --------------------------------------------------------------------------------------------
    Panel::KeyDialog Panel::keyDialog() const
    {
        KeyDialog d;
        const float w = std::min(kKeyDialogW, width_ - 2.0f * kMargin);
        const float h = tab_ == DialogTab::account && chat_.signedIn ? kAccountDialogH : kKeyDialogH;
        d.box = { std::round((width_ - w) * 0.5f), std::round((height_ - h) * 0.42f), w, h };
        d.keyTab = { d.box.right() - 24.0f - 88.0f, d.box.y + 24.0f, 88.0f, 26.0f };
        d.accountTab = { d.keyTab.x - 88.0f, d.keyTab.y, 88.0f, 26.0f };
        d.field = { d.box.x + 24.0f, d.box.y + 134.0f, w - 48.0f, 34.0f };
        d.user = { d.box.x + 24.0f, d.box.y + 128.0f, w - 48.0f, 34.0f };
        d.pass = { d.box.x + 24.0f, d.user.bottom() + 8.0f, w - 48.0f, 34.0f };
        const float by = d.box.bottom() - 24.0f - 32.0f;
        d.primary = { d.box.right() - 24.0f - 96.0f, by, 96.0f, 32.0f };
        d.secondary = { d.primary.x - 8.0f - 84.0f, by, 84.0f, 32.0f };
        d.danger = { d.box.x + 24.0f, by, 104.0f, 32.0f };
        d.extra = { d.danger.right() + 8.0f, by, 112.0f, 32.0f };
        if (dialogButtons().primary.empty())
            d.secondary = { d.box.right() - 24.0f - 84.0f, by, 84.0f, 32.0f };   // Close, alone at the right
        return d;
    }

    Panel::DialogButtons Panel::dialogButtons() const
    {
        DialogButtons b;
        if (tab_ == DialogTab::account)
        {
            if (chat_.signedIn)
            {
                b.secondary = "Close";
                b.danger = "Sign out";
                b.extra = chat_.details.refreshing ? "Refreshing" : "Refresh";
                if (!chat_.useAccount)
                    b.primary = "Use account";
            }
            else
            {
                b.primary = chat_.signingIn || awaitingSignIn_ ? "Signing in" : "Sign in";
                b.secondary = "Cancel";
            }
            return b;
        }
        b.primary = "Save";
        b.secondary = "Cancel";
        if (chat_.hasKey)
        {
            b.danger = "Remove key";
            if (chat_.useAccount)
                b.extra = "Use this key";
        }
        return b;
    }

    void Panel::openKeyDialog() { openKeyDialog(chat_.useAccount ? DialogTab::account : DialogTab::key); }

    void Panel::openKeyDialog(DialogTab tab)
    {
        if (keyOpen_)
            return;
        keyOpen_ = true;
        focusBeforeKey_ = focus_;
        setFocus(-1);
        ring_ = false;
        showTab(tab);
    }

    void Panel::showTab(DialogTab tab)
    {
        tab_ = tab;
        keyEdit_.set("", kMaxKeyLength);
        std::fill(passEdit_.buffer.begin(), passEdit_.buffer.end(), '\0');   // the password typed goes from memory
        passEdit_.set("", kMaxPasswordLength);
        userEdit_.set(chat_.account, kMaxUsernameLength);
        userEdit_.allSelected = false;
        accountField_ = chat_.account.empty() ? 0 : 1;   // the password, when the username is known
        keyWhy_.clear();
        if (tab == DialogTab::account && !chat_.signedIn && !chat_.accountProblem.empty())
            keyWhy_ = asciiText(chat_.accountProblem);
        if (tab == DialogTab::account && chat_.signedIn)
            port_.chat().refreshAccount();             // the account's view, as the server has it now
        dirty_ = true;
    }

    void Panel::closeKeyDialog()
    {
        keyOpen_ = false;
        keyEdit_.set("", kMaxKeyLength);                 // nothing of the key or the password stays in the panel
        std::fill(passEdit_.buffer.begin(), passEdit_.buffer.end(), '\0');
        passEdit_.set("", kMaxPasswordLength);
        keyWhy_.clear();
        pendingSend_.clear();
        awaitingSignIn_ = false;
        if (focusBeforeKey_ >= 0)
            setFocus(focusBeforeKey_);
        focusBeforeKey_ = -1;
        dirty_ = true;
    }

    // Closes the dialog and sends the message that waited for it, now that the chat can.
    void Panel::saveKeyFromDialog()
    {
        std::string typed = keyEdit_.buffer;
        while (!typed.empty() && typed.back() == ' ')
            typed.pop_back();
        while (!typed.empty() && typed.front() == ' ')
            typed.erase(typed.begin());
        if (typed.empty())
        {
            keyWhy_ = "Paste a key first (Cmd-V).";
            dirty_ = true;
            return;
        }
        std::string why;
        if (!port_.chat().saveKey(typed, why))
        {
            keyWhy_ = asciiText(why);
            dirty_ = true;
            return;
        }
        const std::string pending = pendingSend_;
        closeKeyDialog();
        chatAt_ = -1.0;
        refreshChat(host_ != nullptr ? host_->nowSeconds() : 0.0);
        if (!pending.empty())
        {
            setFocus(inputItem_);
            sendChat();
        }
    }

    void Panel::removeKeyFromDialog()
    {
        port_.chat().forgetKey();
        closeKeyDialog();
        chatAt_ = -1.0;
        refreshChat(host_ != nullptr ? host_->nowSeconds() : 0.0);
    }

    // The username and password go to the agent, which signs in on a thread of its own; the password field empties at
    // once. afterSignIn() acts on the answer.
    void Panel::signInFromDialog()
    {
        if (chat_.signingIn || awaitingSignIn_)
            return;
        std::string user = userEdit_.buffer;
        while (!user.empty() && user.back() == ' ')
            user.pop_back();
        while (!user.empty() && user.front() == ' ')
            user.erase(user.begin());
        if (user.empty())
        {
            keyWhy_ = "Type your username.";
            accountField_ = 0;
            dirty_ = true;
            return;
        }
        if (passEdit_.buffer.empty())
        {
            keyWhy_ = "Type your password (Cmd-V pastes it).";
            accountField_ = 1;
            dirty_ = true;
            return;
        }
        keyWhy_.clear();
        awaitingSignIn_ = true;
        port_.chat().signIn(user, passEdit_.buffer);
        std::fill(passEdit_.buffer.begin(), passEdit_.buffer.end(), '\0');
        passEdit_.set("", kMaxPasswordLength);
        chatAt_ = -1.0;
        refreshChat(host_ != nullptr ? host_->nowSeconds() : 0.0);
        dirty_ = true;
    }

    void Panel::afterSignIn()
    {
        awaitingSignIn_ = false;
        dirty_ = true;
        if (!chat_.signedIn || !chat_.signInError.empty())
        {
            keyWhy_ = asciiText(chat_.signInError.empty() ? std::string("The sign-in failed.") : chat_.signInError);
            accountField_ = 1;
            return;
        }
        const std::string pending = pendingSend_;
        closeKeyDialog();
        if (!pending.empty())
        {
            setFocus(inputItem_);
            sendChat();
        }
    }

    void Panel::signOutFromDialog()
    {
        port_.chat().signOut();
        closeKeyDialog();
        chatAt_ = -1.0;
        refreshChat(host_ != nullptr ? host_->nowSeconds() : 0.0);
    }

    void Panel::useFromDialog()
    {
        port_.chat().useAccount(tab_ == DialogTab::account);
        const std::string pending = pendingSend_;
        closeKeyDialog();
        chatAt_ = -1.0;
        refreshChat(host_ != nullptr ? host_->nowSeconds() : 0.0);
        if (!pending.empty())
        {
            setFocus(inputItem_);
            sendChat();
        }
    }

    void Panel::dialogAction(const std::string& label)
    {
        if (label == "Save")
            saveKeyFromDialog();
        else if (label == "Sign in")
            signInFromDialog();
        else if (label == "Cancel" || label == "Close")
            closeKeyDialog();
        else if (label == "Remove key")
            removeKeyFromDialog();
        else if (label == "Sign out")
            signOutFromDialog();
        else if (label == "Use account" || label == "Use this key")
            useFromDialog();
        else if (label == "Refresh")
            port_.chat().refreshAccount();
        dirty_ = true;
    }

    // Every key is the dialog's while it is open: Return saves (or signs in), Escape cancels, Cmd-V pastes. On the
    // account tab Tab moves between the username and the password, and the arrows move the username's caret; a masked
    // field keeps its caret at its end (there is nothing to move it over).
    bool Panel::keyDialogKey(const funkgui::KeyEvent& e)
    {
        using funkgui::Key;
        const bool account = tab_ == DialogTab::account;
        const bool form = account && !chat_.signedIn;
        funkgui::text::LineEdit& edit = !account ? keyEdit_ : (accountField_ == 0 ? userEdit_ : passEdit_);
        const int limit = !account ? kMaxKeyLength : (accountField_ == 0 ? kMaxUsernameLength : kMaxPasswordLength);
        const bool masked = !account || accountField_ == 1;
        if (e.key == Key::escape)
        {
            closeKeyDialog();
            return true;
        }
        if (account && !form)
        {
            if (e.key == Key::enter)
                dialogAction(dialogButtons().primary.empty() ? std::string("Close") : dialogButtons().primary);
            dirty_ = true;
            return true;                             // signed in: nothing to type
        }
        if (e.key == Key::enter)
        {
            if (!account)
                saveKeyFromDialog();
            else if (accountField_ == 0 && passEdit_.buffer.empty())
                accountField_ = 1;
            else
                signInFromDialog();
        }
        else if (e.key == Key::tab && account)
            accountField_ = 1 - accountField_;
        else if (account && (chat_.signingIn || awaitingSignIn_))
        {
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host_ != nullptr && edit.paste(host_->clipboardText(), limit))
                keyWhy_.clear();
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
            edit.allSelected = !edit.buffer.empty();
        else if (masked && (e.key == Key::left || e.key == Key::right || e.key == Key::home || e.key == Key::end))
        {
        }
        else if (e.key == Key::up || e.key == Key::down || e.key == Key::tab)
        {
        }
        else if (edit.key(e, limit))
            keyWhy_.clear();
        if (masked)
            edit.caret = static_cast<int>(edit.buffer.size());
        dirty_ = true;
        return true;
    }

    void Panel::keyDialogPointerDown(funkgui::Point p)
    {
        const KeyDialog d = keyDialog();
        const DialogButtons b = dialogButtons();
        if (d.accountTab.contains(p) && tab_ != DialogTab::account && !awaitingSignIn_)
            showTab(DialogTab::account);
        else if (d.keyTab.contains(p) && tab_ != DialogTab::key && !awaitingSignIn_)
            showTab(DialogTab::key);
        else if (tab_ == DialogTab::account && !chat_.signedIn && d.user.contains(p))
            accountField_ = 0;
        else if (tab_ == DialogTab::account && !chat_.signedIn && d.pass.contains(p))
            accountField_ = 1;
        else if (!b.primary.empty() && d.primary.contains(p))
            dialogAction(b.primary);
        else if (!b.secondary.empty() && d.secondary.contains(p))
            dialogAction(b.secondary);
        else if (!b.danger.empty() && d.danger.contains(p))
            dialogAction(b.danger);
        else if (!b.extra.empty() && d.extra.contains(p))
            dialogAction(b.extra);
        dirty_ = true;
    }

    // The key as the field shows it: its first characters ("sk-ant-"), then a star for each of the others (at most 40),
    // then its last four.
    std::string Panel::maskedKey() const
    {
        const std::string& b = keyEdit_.buffer;
        const std::size_t head = std::min<std::size_t>(7, b.size());
        const std::size_t tail = b.size() > head + 4 ? 4 : 0;
        const std::size_t hidden = b.size() - head - tail;
        return b.substr(0, head) + std::string(std::min<std::size_t>(hidden, 40), '*') + b.substr(b.size() - tail);
    }

    void Panel::drawKeyDialog(funkgui::Canvas& c, const funkgui::Theme& th) const
    {
        const chat::Palette p = chat::palette(th);
        const KeyDialog d = keyDialog();
        const DialogButtons buttons = dialogButtons();
        const bool account = tab_ == DialogTab::account;
        const funkgui::Canvas::Scope s(c, kKeyDialogTag, false);
        c.rrect(0.0f, 0.0f, width_, height_, 0.0f, th.ground.withAlpha(0.68f));
        constexpr funkgui::Col black{ 0, 0, 0, 255 };
        for (int k = 3; k >= 1; --k)                     // a soft shadow under the card
        {
            const float g = static_cast<float>(k) * 3.0f;
            c.rrect(d.box.x - g, d.box.y - g + 6.0f, d.box.w + 2.0f * g, d.box.h + 2.0f * g, 16.0f + g, black.withAlpha(0.07f));
        }
        c.rrect(d.box.x, d.box.y, d.box.w, d.box.h, 16.0f, p.card, 1.0f, p.cardEdge);

        const float x = d.box.x + 24.0f;
        c.rrect(x, d.box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, account ? chat::Icon::person : chat::Icon::key, x + 15.0f, d.box.y + 37.0f, 15.0f, p.accent);
        c.text(account ? "Your account" : "Anthropic API key", x + 42.0f, c.capCentreTop(d.box.y + 37.0f, chat::kH2),
               chat::kH2, p.text);

        // The tabs: a segmented pill, the chosen one filled; a dot on the one the chat uses.
        {
            const funkgui::Rect all{ d.accountTab.x, d.accountTab.y, d.keyTab.right() - d.accountTab.x, d.keyTab.h };
            c.rrect(all.x, all.y, all.w, all.h, all.h * 0.5f, p.raised, 1.0f, p.cardEdge);
            const auto tab = [&](const funkgui::Rect& r, const char* label, bool chosen, bool inUse) {
                const bool hover = hasPointer_ && r.contains(pointer_) && !chosen;
                if (chosen)
                    c.rrect(r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h - 4.0f, (r.h - 4.0f) * 0.5f, p.composer, 1.0f, p.composerEdge);
                const funkgui::TextStyle& st = chosen ? chat::kSmallStrong : chat::kSmall;
                const float tw = c.textWidth(label, st);
                const float tx = r.centreX() - tw * 0.5f + (inUse ? 5.0f : 0.0f);
                if (inUse)
                    c.disc(tx - 9.0f, r.centreY(), 2.5f, p.accent);
                c.text(label, tx, c.capCentreTop(r.centreY(), st), st, chosen || hover ? p.text : p.soft);
            };
            const bool ready = chat_.ready();
            tab(d.accountTab, "Account", account, ready && chat_.useAccount);
            tab(d.keyTab, "API key", !account, ready && !chat_.useAccount);
        }

        // The lines under the title: what this access is, and its state.
        std::vector<std::string> lines;
        if (account && chat_.signedIn)
            lines = { "Claude runs on the TestingPlayground server, charged to this",
                      "account's credit at Anthropic's prices. The sign-in is kept,",
                      "encrypted, for both playground plugins on this Mac." };
        else if (account)
            lines = { "Sign in with the account the TestingPlayground admin gave you:",
                      "Claude then runs on the server, charged to the account's credit.",
                      "The password is sent once; only the sign-in is kept, encrypted." };
        else
            lines = { "Claude runs on your own key, from console.anthropic.com (API keys).",
                      "It is stored encrypted on this Mac, for both playground plugins,",
                      "and sent only to api.anthropic.com. Requests are billed to it.",
                      !chat_.hasKey       ? std::string()
                      : chat_.useAccount ? std::string("A key is stored; the chat uses your account now.")
                                         : std::string("A key is stored, and the chat uses it.") };
        float y = d.box.y + 66.0f;
        for (const std::string& line : lines)
        {
            c.text(fitText(c, asciiText(line), chat::kSmall, d.box.w - 48.0f).c_str(), x, c.capCentreTop(y + 8.0f, chat::kSmall),
                   chat::kSmall, p.soft);
            y += 16.0f;
        }

        // A field: its text (or a hint), the selection, and the caret where it has one.
        const auto field = [&](const funkgui::Rect& f, const std::string& shown, const char* hint, bool caret,
                               int caretAt, bool selected) {
            c.rrect(f.x, f.y, f.w, f.h, 10.0f, p.composer, 1.0f,
                    !caret ? p.composerEdge : keyWhy_.empty() ? funkgui::mix(p.composerEdge, p.accent, 0.45f) : p.danger);
            const float textTop = c.capCentreTop(f.centreY(), chat::kBody);
            const float maxW = f.w - 28.0f;
            if (shown.empty())
                c.text(hint, f.x + 14.0f, textTop, chat::kBody, p.faint);
            else
            {
                const float w = std::min(c.textWidth(shown.c_str(), chat::kBody), maxW);
                if (selected)
                    c.rrect(f.x + 12.0f, f.y + 8.0f, w + 4.0f, f.h - 16.0f, 3.0f, th.accentDim);
                c.text(fitText(c, shown, chat::kBody, maxW).c_str(), f.x + 14.0f, textTop, chat::kBody, p.text);
            }
            if (caret)
            {
                const std::string before = shown.substr(0, static_cast<std::size_t>(std::clamp(caretAt, 0, static_cast<int>(shown.size()))));
                const float cx = f.x + 14.0f + std::min(c.textWidth(before.c_str(), chat::kBody), maxW);
                c.rrect(cx, f.y + 9.0f, 1.5f, f.h - 18.0f, 0.75f, p.accent);
            }
        };
        float whyY = 0.0f, whyX = x, whyW = d.box.w - 48.0f;
        if (!account)
        {
            // The key masked (its first and last characters shown), the caret at its end.
            const std::string shown = keyEdit_.buffer.empty() ? std::string() : maskedKey();
            field(d.field, shown, "sk-ant-...   (Cmd-V pastes it)", true, static_cast<int>(shown.size()),
                  keyEdit_.allSelected);
            whyY = d.field.bottom() + 14.0f;
        }
        else if (chat_.signedIn)
        {
            // Who is signed in, the credit left, and whether the chat uses the account.
            const funkgui::Rect r{ d.user.x, d.user.y, d.user.w, d.pass.bottom() - d.user.y };
            c.rrect(r.x, r.y, r.w, r.h, 12.0f, p.raised, 1.0f, p.cardEdge);
            c.disc(r.x + 34.0f, r.centreY(), 17.0f, p.accent.withAlpha(0.16f));
            chat::drawIcon(c, chat::Icon::person, r.x + 34.0f, r.centreY(), 16.0f, p.accent);
            const float tx = r.x + 64.0f, tw = r.w - 64.0f - 120.0f;
            c.text(fitText(c, asciiText(chat_.account), chat::kStrong, tw).c_str(), tx,
                   c.capCentreTop(r.centreY() - 10.0f, chat::kStrong), chat::kStrong, p.text);
            const std::string credit = chat_.credit.empty() ? std::string("The credit left shows after a message")
                                                            : chat_.credit;
            c.text(fitText(c, asciiText(credit), chat::kSmall, tw).c_str(), tx, c.capCentreTop(r.centreY() + 11.0f, chat::kSmall),
                   chat::kSmall, p.soft);
            const char* state = chat_.useAccount ? "In use" : "Not in use";
            const float sw = c.textWidth(state, chat::kSmall) + 24.0f;
            const funkgui::Rect pill{ r.right() - 16.0f - sw, r.centreY() - 12.0f, sw, 24.0f };
            c.rrect(pill.x, pill.y, pill.w, pill.h, 12.0f, chat_.useAccount ? p.accent.withAlpha(0.16f) : p.card, 1.0f,
                    chat_.useAccount ? p.accent.withAlpha(0.0f) : p.composerEdge);
            c.text(state, pill.centreX(), c.capCentreTop(pill.centreY(), chat::kSmall), chat::kSmall,
                   chat_.useAccount ? p.accent : p.dim, funkgui::Align::centre);

            // This month's use, four tiles; under them the sign-in's dates and what made it.
            const ChatAccount& info = chat_.details;
            float ty = r.bottom() + 16.0f;
            c.text("THIS MONTH", x + 2.0f, c.capCentreTop(ty, funkgui::type::kMicro), funkgui::type::kMicro, p.faint);
            ty += 12.0f;
            const float gap = 8.0f, tw4 = std::floor((r.w - 3.0f * gap) / 4.0f), th4 = 50.0f;
            char spent[32];
            std::snprintf(spent, sizeof spent, "$%.2f", info.costUsd);
            const std::string values[4] = { info.known ? std::to_string(info.requests) : "-", info.known ? spent : "-",
                                            info.known ? countText(info.inputTokens) : "-",
                                            info.known ? countText(info.outputTokens) : "-" };
            const char* const names[4] = { info.requests == 1 ? "request" : "requests", "spent", "tokens in", "tokens out" };
            for (int k = 0; k < 4; ++k)
            {
                const funkgui::Rect t{ r.x + static_cast<float>(k) * (tw4 + gap), ty, tw4, th4 };
                c.rrect(t.x, t.y, t.w, t.h, 10.0f, p.raised, 1.0f, p.cardEdge);
                c.text(fitText(c, values[k], chat::kH2, t.w - 20.0f).c_str(), t.x + 12.0f, c.capCentreTop(t.y + 18.0f, chat::kH2),
                       chat::kH2, info.known ? p.text : p.faint);
                c.text(names[k], t.x + 12.0f, c.capCentreTop(t.y + 36.0f, chat::kSmall), chat::kSmall, p.soft);
            }
            ty += th4 + 18.0f;
            std::string line, tone = "soft";
            if (!info.error.empty())
            {
                line = info.error;
                tone = "danger";
            }
            else if (!info.known)
                line = info.refreshing ? "Asking the server..." : "Refresh asks the server for the account.";
            else
            {
                line = "Signed in " + shortDate(info.since) + (info.client.empty() ? std::string() : " from " + info.client)
                     + "; it lasts until " + shortDate(info.expires) + ".";
            }
            c.text(fitText(c, asciiText(line), chat::kSmall, r.w).c_str(), x + 2.0f, c.capCentreTop(ty, chat::kSmall), chat::kSmall,
                   tone == "danger" ? p.danger : p.soft);
        }
        else
        {
            const bool waiting = chat_.signingIn || awaitingSignIn_;
            field(d.user, userEdit_.buffer, "Username", accountField_ == 0 && !waiting, userEdit_.caret, userEdit_.allSelected);
            const std::string stars(std::min<std::size_t>(passEdit_.buffer.size(), 40), '*');
            field(d.pass, stars, "Password   (Cmd-V pastes it)", accountField_ == 1 && !waiting,
                  static_cast<int>(stars.size()), passEdit_.allSelected);
            whyY = d.pass.bottom() + 14.0f;
        }
        if (!keyWhy_.empty() && whyY > 0.0f)
            c.text(fitText(c, asciiText(keyWhy_), chat::kSmall, whyW).c_str(), whyX + 2.0f, c.capCentreTop(whyY, chat::kSmall),
                   chat::kSmall, p.danger);

        // The buttons: pills.
        const auto button = [&](const funkgui::Rect& r, const std::string& label, int look) {   // 0 primary, 1 outlined, 2 quiet
            if (label.empty())
                return;
            const bool waiting = label == "Signing in" || label == "Refreshing";
            const bool hover = hasPointer_ && r.contains(pointer_) && !waiting;
            funkgui::Col fill = p.card.withAlpha(0.0f), edge = p.card.withAlpha(0.0f), ink = p.soft;
            if (look == 0)
            {
                fill = hover ? funkgui::mix(p.accent, p.text, 0.12f) : waiting ? p.accent.withAlpha(0.55f) : p.accent;
                ink = p.onAccent;
            }
            else
            {
                if (look == 1)
                    edge = p.composerEdge;
                if (hover)
                    fill = p.hover;
                ink = look == 2 ? p.danger : (hover ? p.text : p.soft);
            }
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, fill, look == 1 ? 1.0f : 0.0f, edge);
            const funkgui::TextStyle& st = look == 0 ? chat::kSmallStrong : chat::kSmall;
            c.text(waiting ? (label + "...").c_str() : label.c_str(), r.centreX(), c.capCentreTop(r.centreY(), st), st,
                   waiting ? p.faint : ink, funkgui::Align::centre);
        };
        button(d.primary, buttons.primary, 0);
        button(d.secondary, buttons.secondary, buttons.primary.empty() ? 0 : 1);
        button(d.danger, buttons.danger, 2);
        button(d.extra, buttons.extra, 1);
    }

    void Panel::openExamples()
    {
        if (host_ == nullptr)
            return;
        funkgui::MenuRequest request;
        int id = 1;
        for (const Example& e : kExamples)
            request.items.push_back({ id++, e.title });
        request.anchor = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(examplesItem_)]).bounds();
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) { loadExample(chosen - 1); });
    }

    void Panel::loadExample(int i)
    {
        if (i < 0 || i >= static_cast<int>(std::size(kExamples)))
            return;
        // The example replaces the program's text as one edit (Cmd-Z brings the text back), and runs; the other files
        // stay.
        files().endRename();
        files().select(0);
        funkgui::TextEditor& program = files().editor();
        program.selectAll();
        program.insert(kExamples[static_cast<std::size_t>(i)].source);
        program.setCaret({ 0, 0 });
        program.setScroll(0.0f, 0.0f);
        setFocus(editorItem_);
        runNow();
    }

    // LIBRARY: the library's files by category, a tick on those the program has. One the program lacks becomes a tab
    // of its own, shown, and the program #includes it (one more line at its top, an edit Cmd-Z takes back); one it has
    // is shown. With no room for another tab, the others are greyed.
    void Panel::openLibrary()
    {
        if (host_ == nullptr)
            return;
        funkgui::MenuRequest request;
        const bool room = files().count() < static_cast<int>(kMaxFiles);
        constexpr int kHeading = 1000;                   // a category's heading: an id of its own, greyed (never chosen)
        std::string category;
        for (std::size_t i = 0; i < std::size(kLibrary); ++i)
        {
            const LibraryFile& f = kLibrary[i];
            if (category != f.category)
            {
                if (!category.empty())
                    request.items.push_back({ 0, {}, false, false, true });
                category = f.category;
                request.items.push_back({ kHeading + static_cast<int>(i), upper(category), false });
            }
            const bool have = files().find(f.name) >= 0;
            request.items.push_back({ static_cast<int>(i) + 1, std::string(f.name) + "   " + f.summary, have || room, have });
        }
        request.anchor = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(libraryItem_)]).bounds();
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) {
            if (chosen >= 1 && chosen <= static_cast<int>(std::size(kLibrary)))
                addLibraryFile(kLibrary[static_cast<std::size_t>(chosen - 1)].name);
        });
    }

    // RANDOM: a program from the library's building blocks (Random.h), as EXAMPLES puts one in: the program's text
    // replaced as one edit and run, the other files staying; the library files it #includes as tabs.
    bool Panel::makeRandom(uint32_t seed)
    {
        random::Program p;
        bool fits = false;
        for (uint32_t k = 0; k < 64 && !fits; ++k)
        {
            p = random::make(seed + k);
            int missing = 0;
            for (const std::string& f : p.library)
                missing += files().find(f) < 0 ? 1 : 0;
            fits = files().count() + missing <= static_cast<int>(kMaxFiles);
        }
        if (!fits)
        {
            notice("RANDOM NEEDS ROOM FOR ITS LIBRARY FILES: CLOSE A TAB");
            return false;
        }
        files().endRename();
        for (const std::string& name : p.library)
            if (files().find(name) < 0)
                if (const LibraryFile* f = findLibraryFile(name))
                    files().addFile(f->name, f->source);
        files().select(0);
        funkgui::TextEditor& program = files().editor();
        program.selectAll();
        program.insert(p.text);
        program.setCaret({ 0, 0 });
        program.setScroll(0.0f, 0.0f);
        setFocus(editorItem_);
        runNow();
        dirty_ = true;
        return true;
    }

    uint32_t Panel::nextSeed()
    {
        const auto ticks = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        ++randomCount_;
        return 1 + static_cast<uint32_t>((ticks / 1000 + static_cast<uint64_t>(randomCount_) * 7919u) % random::kMaxSeed);
    }

    void Panel::openProjects(bool preset)
    {
        if (dialogOpen())
            return;
        std::string name = port_.project().name;
        if (name.empty())
        {
            // The program's title: its first line's words after "//", before a colon or "(random".
            const Draft d = port_.draft();
            const std::string text = d.files.empty() ? std::string() : d.files.front().text;
            std::string first = text.substr(0, text.find('\n'));
            if (first.rfind("//", 0) == 0)
            {
                first = first.substr(2);
                first = first.substr(0, std::min(first.find(':'), first.find(" (random")));
                name = port_.tidyName(first);
            }
        }
        files().endRename();
        port_.setDraft(files().draft());                 // what Save saves is what the tabs show
        projects_.open(name, preset);
        focusBeforeProjects_ = focus_;
        setFocus(-1);
        ring_ = false;
        dirty_ = true;
    }

    void Panel::afterProjects(bool wasOpen)
    {
        if (const std::string n = projects_.takeNotice(); !n.empty())
            notice(n);
        projectsAt_ = -100.0;                            // the explorer reads what it saved, opened or deleted
        const bool importIt = projects_.takeImport();
        const std::string exportIt = projects_.takeExport();
        if (wasOpen && !projects_.isOpen() && exportIt.empty())
        {
            setFocus(focusBeforeProjects_ >= 0 ? focusBeforeProjects_ : editorItem_);
            focusBeforeProjects_ = -1;
        }
        if (importIt)
            chooseImport();
        if (!exportIt.empty())
        {
            export_.open(exportIt);
            setFocus(-1);
        }
        dirty_ = true;
    }

    void Panel::openExport(const std::string& project)
    {
        if (dialogOpen() || project.empty())
            return;
        files().endRename();
        focusBeforeProjects_ = focus_;
        export_.open(project);
        setFocus(-1);
        ring_ = false;
        dirty_ = true;
    }

    // The save chooser for the export EXPORT asked for, then the file; or, with no chooser, the Desktop.
    void Panel::afterExport(bool wasOpen)
    {
        if (const std::string n = export_.takeNotice(); !n.empty())
            notice(n);
        const ExportCard::Want want = export_.takeWant();
        if (want != ExportCard::Want::none)
        {
            const bool package = want == ExportCard::Want::package;
            const std::string project = export_.project();
            const bool every = export_.allVersions();
            const std::string ext = package ? ".fmpack" : ".zip";
            const auto write = [this, project, package, every](const std::string& path) {
                Exported out;
                std::string why;
                if (port_.exportProject(project, package, every, path, out, why))
                {
                    const auto kb = [](int64_t b) { return b < 10240 ? std::to_string(b) + " bytes" : std::to_string((b + 512) / 1024) + " KB"; };
                    const std::string file = juce::File(juce::String::fromUTF8(path.c_str())).getFileName().toStdString();
                    export_.exported(true, "Exported " + file + ": " + kb(out.bytes) + (package ? " (" + kb(out.raw) + " of text, " + out.method + ")" : std::string())
                                               + ", " + std::to_string(out.versions) + (out.versions == 1 ? " version." : " versions."));
                    notice("EXPORTED: " + upper(file));
                }
                else
                    export_.exported(false, "Not exported: " + why + ".");
                dirty_ = true;
            };
            funkgui::FileRequest request;
            request.mode = funkgui::FileRequest::Mode::save;
            request.title = "Export " + project;
            request.pattern = "*" + ext;
            request.suggestedName = project + ext;
            if (host_ == nullptr || !host_->chooseFiles(request, [write](const std::vector<std::string>& paths) {
                    if (!paths.empty())
                        write(paths.front());
                }))
            {
                // No chooser: the Desktop, under a name no file there has.
                const juce::File desk(juce::String::fromUTF8(port_.exportFolder().c_str()));
                juce::File to = desk.getChildFile(juce::String::fromUTF8((project + ext).c_str()));
                for (int k = 2; to.exists() && k < 1000; ++k)
                    to = desk.getChildFile(juce::String::fromUTF8((project + " " + std::to_string(k) + ext).c_str()));
                write(to.getFullPathName().toStdString());
            }
        }
        if (wasOpen && !export_.isOpen())
        {
            setFocus(focusBeforeProjects_ >= 0 ? focusBeforeProjects_ : editorItem_);
            focusBeforeProjects_ = -1;
        }
        projectsAt_ = -100.0;
        dirty_ = true;
    }

    void Panel::chooseImport()
    {
        if (host_ == nullptr)
            return;
        funkgui::FileRequest request;
        request.mode = funkgui::FileRequest::Mode::openMany;
        request.title = "Import projects";
        request.pattern = "*.fmpack;*.zip";
        host_->chooseFiles(request, [this](const std::vector<std::string>& paths) { importFiles(paths); });
    }

    // Each a project of its own; the last one chosen in PROJECTS, opened over the panel.
    void Panel::importFiles(const std::vector<std::string>& paths)
    {
        std::string last, failed;
        int done = 0;
        for (const std::string& p : paths)
        {
            std::string name, why;
            if (port_.importProject(p, name, why))
            {
                last = name;
                ++done;
            }
            else
                failed = why;
        }
        projectsAt_ = -100.0;
        if (done == 0)
        {
            if (!failed.empty())
                notice("NOT IMPORTED: " + upper(failed));
            return;
        }
        notice(done == 1 ? "IMPORTED: " + upper(last) : "IMPORTED " + std::to_string(done) + " PROJECTS");
        if (!dialogOpen())
        {
            openProjects();
            projects_.chooseProjectNamed(last, width_, height_);
        }
        dirty_ = true;
    }

    void Panel::notice(const std::string& text)
    {
        notice_ = text;
        noticeUntil_ = (host_ != nullptr ? host_->nowSeconds() : 0.0) + 4.0;
        dirty_ = true;
    }

    bool Panel::addLibraryFile(const std::string& name)
    {
        const LibraryFile* f = findLibraryFile(name);
        if (f == nullptr)
            return false;
        files().endRename();
        if (const int tab = files().find(name); tab >= 0)
        {
            files().select(tab);
            setFocus(editorItem_);
            return true;
        }
        if (!files().addFile(f->name, f->source))
            return false;
        // The program includes it, unless it does already.
        funkgui::TextEditor& program = files().editorOf(0);
        const std::string line = "#include \"" + name + "\"";
        if (program.text().find(line) == std::string::npos)
        {
            const int at = includeLineFor(program.text());
            const funkgui::TextPos caret = program.caret();
            program.setCaret({ at, 0 });
            program.insert(line + "\n");
            program.setCaret({ caret.line >= at ? caret.line + 1 : caret.line, caret.col });
        }
        setFocus(editorItem_);
        dirty_ = true;
        return true;
    }

    void Panel::jumpTo(const Problem& p)
    {
        if (p.line <= 0)
            return;
        const int tab = files().find(p.file);
        if (tab < 0)
            return;                                      // a file no tab has (none should be)
        files().select(tab);
        funkgui::TextEditor& ed = files().editor();
        ed.setCaret({ p.line - 1, std::max(0, p.column - 1) });
        ed.scrollToCaret();
        setFocus(editorItem_);
        dirty_ = true;
    }

    void Panel::closeFile(int tab)
    {
        if (tab <= 0 || tab >= files().count())
            return;
        const std::string name = files().name(tab);
        const auto close = [this, name] {
            // The tab by its name: the menu's answer may come after other changes.
            if (const int i = files().find(name); i > 0)
            {
                files().remove(i);
                dirty_ = true;
            }
        };
        const std::string text = files().draft().files[static_cast<std::size_t>(tab)].text;
        if (text.find_first_not_of(" \t\n") == std::string::npos || host_ == nullptr)
        {
            close();                                     // nothing in it to lose
            return;
        }
        funkgui::MenuRequest request;
        request.items.push_back({ 1, "Close " + name + " (its text is lost)" });
        request.anchor = files().closeRect(tab);
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [close](int chosen) {
            if (chosen == 1)
                close();
        });
    }

    // ---- a .gui file's interface ----------------------------------------------------------------------------------------
    void Panel::layoutGuiItems()
    {
        guiItem_.assign(items_.size(), false);
        const bool gui = !panelSpec_.gui.empty();
        const std::vector<GuiNode>& nodes = panelSpec_.gui.nodes;
        const funkgui::Rect area = gui_.area;
        const GuiMeasure gm = [](const std::string& text, const funkgui::TextStyle& st) {
            return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(), st);
        };
        // A node on the pages shown and, at least in part, in the area.
        const auto seen = [&](std::size_t i) {
            if (!gui || i >= gui_.rects.size() || !gui_.visible[i])
                return false;
            const funkgui::Rect& r = gui_.rects[i];
            return r.right() > area.x && r.x < area.right() && r.bottom() > area.y && r.y < area.bottom();
        };
        const auto control = [this](int slot) -> GuiControlItem& {
            return static_cast<GuiControlItem&>(*items_[static_cast<std::size_t>(itemOfGuiSlot_[static_cast<std::size_t>(slot)])]);
        };
        std::vector<int> nodeOfSlot(static_cast<std::size_t>(kMaxControls), -1), nodeOfDisplay(static_cast<std::size_t>(kMaxDisplays), -1);
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const GuiNode& n = nodes[i];
            const bool isControl = n.kind == GuiKind::knob || n.kind == GuiKind::slider || n.kind == GuiKind::toggle
                                || n.kind == GuiKind::buttons || n.kind == GuiKind::menu;
            if (isControl && n.slot >= 0 && n.slot < kMaxControls)
                nodeOfSlot[static_cast<std::size_t>(n.slot)] = static_cast<int>(i);
            if (n.kind == GuiKind::display && n.display >= 0 && n.display < kMaxDisplays)
                nodeOfDisplay[static_cast<std::size_t>(n.display)] = static_cast<int>(i);
        }
        for (int c = 0; c < kMaxControls; ++c)
        {
            if (itemOfGuiSlot_[static_cast<std::size_t>(c)] < 0)
                continue;
            const int i = nodeOfSlot[static_cast<std::size_t>(c)];
            if (i >= 0)
                control(c).place(nodes[static_cast<std::size_t>(i)], gui_.rects[static_cast<std::size_t>(i)], area,
                                 seen(static_cast<std::size_t>(i)), nodes[static_cast<std::size_t>(i)].color);
            else
                control(c).place(GuiNode{}, {}, area, false, "");
            guiItem_[static_cast<std::size_t>(itemOfGuiSlot_[static_cast<std::size_t>(c)])] = true;
        }
        std::size_t pad = 0, env = 0, tab = 0, keys = 0, shade = 0;
        for (std::size_t i = 0; i < nodes.size() && gui; ++i)
        {
            const GuiNode& n = nodes[i];
            if (n.kind == GuiKind::shader && shade < guiShaders_.size())
            {
                const std::size_t k = n.shader >= 0 ? static_cast<std::size_t>(n.shader) : panelSpec_.shaders.size();
                const bool known = k < panelSpec_.shaders.size() && k < shaderIds_.size();
                shaderNode_[shade] = static_cast<int>(i);
                static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(guiShaders_[shade++])])
                    .place(n, gui_.rects[i], area, seen(i), known ? shaderIds_[k] : 0u, n.file,
                           known ? shaderWhy_[k] : std::string("not built"), known && shaderMoves_[k]);
            }
            else if (n.kind == GuiKind::keyboard && keys < guiKeyboards_.size())
                static_cast<GuiKeyboardItem&>(*items_[static_cast<std::size_t>(guiKeyboards_[keys++])])
                    .place(n, gui_.rects[i], area, seen(i), n.color);
            else if (n.kind == GuiKind::xy && pad < guiPads_.size() && n.slot >= 0 && n.slot2 >= 0)
            {
                auto& item = static_cast<GuiXyItem&>(*items_[static_cast<std::size_t>(guiPads_[pad++])]);
                item.bind(&control(n.slot).value(), &control(n.slot2).value());
                item.place(n, gui_.rects[i], area, seen(i), n.color);
            }
            else if (n.kind == GuiKind::envelope && env < guiEnvelopes_.size())
            {
                auto& item = static_cast<GuiEnvelopeItem&>(*items_[static_cast<std::size_t>(guiEnvelopes_[env++])]);
                std::array<GuiValue*, 4> adsr{};
                for (std::size_t k = 0; k < 4; ++k)
                    adsr[k] = n.env[k] >= 0 ? &control(n.env[k]).value() : nullptr;
                item.bind(adsr);
                item.place(n, gui_.rects[i], area, seen(i), n.color);
            }
            else if (n.kind == GuiKind::tabs && tab < guiTabs_.size())
            {
                auto& item = static_cast<GuiTabsItem&>(*items_[static_cast<std::size_t>(guiTabs_[tab++])]);
                std::vector<std::string> titles;
                for (const int child : n.children)
                    titles.push_back(nodes[static_cast<std::size_t>(child)].text);
                item.place(static_cast<int>(i), gui_.rects[i], guiTabRects(panelSpec_, static_cast<int>(i), gui_.rects[i], gm),
                           std::move(titles), gui_.pages[i], area, seen(i), n.color, n.style);
            }
        }
        for (; pad < guiPads_.size(); ++pad)
            static_cast<GuiXyItem&>(*items_[static_cast<std::size_t>(guiPads_[pad])]).place(GuiNode{}, {}, area, false, "");
        for (; env < guiEnvelopes_.size(); ++env)
            static_cast<GuiEnvelopeItem&>(*items_[static_cast<std::size_t>(guiEnvelopes_[env])]).place(GuiNode{}, {}, area, false, "");
        for (; tab < guiTabs_.size(); ++tab)
            static_cast<GuiTabsItem&>(*items_[static_cast<std::size_t>(guiTabs_[tab])]).place(-1, {}, {}, {}, 0, area, false, "", {});
        for (; keys < guiKeyboards_.size(); ++keys)
            static_cast<GuiKeyboardItem&>(*items_[static_cast<std::size_t>(guiKeyboards_[keys])]).place(GuiNode{}, {}, area, false, "");
        for (const int i : guiKeyboards_)
            guiItem_[static_cast<std::size_t>(i)] = true;
        for (; shade < guiShaders_.size(); ++shade)
        {
            shaderNode_[shade] = -1;
            static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(guiShaders_[shade])])
                .place(GuiNode{}, {}, area, false, 0u, "", "", false);
        }
        for (const int i : guiShaders_)
            guiItem_[static_cast<std::size_t>(i)] = true;
        for (const int i : guiPads_)
            guiItem_[static_cast<std::size_t>(i)] = true;
        for (const int i : guiEnvelopes_)
            guiItem_[static_cast<std::size_t>(i)] = true;
        for (const int i : guiTabs_)
            guiItem_[static_cast<std::size_t>(i)] = true;
        if (!gui)
            return;
        // The displays: where the interface puts them, while it shows them.
        for (int d = 0; d < kMaxDisplays; ++d)
        {
            const int idx = itemOfDisplay_[static_cast<std::size_t>(d)];
            auto& item = static_cast<DisplayItem&>(*items_[static_cast<std::size_t>(idx)]);
            const int i = nodeOfDisplay[static_cast<std::size_t>(d)];
            const bool has = d < static_cast<int>(panelSpec_.displays.size()) && i >= 0;
            item.setDisplay(has && seen(static_cast<std::size_t>(i)) ? &panelSpec_.displays[static_cast<std::size_t>(d)] : nullptr);
            item.setRect(has ? gui_.rects[static_cast<std::size_t>(i)] : funkgui::Rect{});
            item.setArea(area);
            guiItem_[static_cast<std::size_t>(idx)] = true;
        }
    }

    // A shader's program goes to FunkGui's registry (process-wide, keyed by its bytes' hash: the same shader in two
    // instances is one program) the first time a panel shows it; a program is never removed, since another instance
    // may draw it too. One that was not built says why from the compile's notes; one FunkGui refuses (Metal's own
    // compiler, say), from FunkGui.
    void Panel::registerShaders(const std::string& notes)
    {
        const std::size_t n = panelSpec_.shaders.size();
        shaderIds_.assign(n, 0u);
        shaderWhy_.assign(n, std::string());
        shaderMoves_.assign(n, false);
        for (std::size_t k = 0; k < n; ++k)
        {
            const PanelShader& sh = panelSpec_.shaders[k];
            const auto f = std::find_if(running_.begin(), running_.end(), [&](const SourceFile& x) { return x.name == sh.file; });
            if (f != running_.end())
                shaderMoves_[k] = f->text.find("u_time") != std::string::npos || f->text.find("u_viewSize") != std::string::npos;
            if (sh.binary.empty() || sh.id == 0)
            {
                std::string why = f == running_.end() ? "there is no file " + sh.file + " among the program's" : std::string("not built");
                std::istringstream in(notes);
                std::string line;
                while (std::getline(in, line))
                    if (line.rfind(sh.file + ":", 0) == 0)
                        if (const std::size_t at = line.find("note: "); at != std::string::npos)
                        {
                            why = line.substr(at + 6);
                            break;
                        }
                shaderWhy_[k] = why;
                continue;
            }
            std::string why;
            if (funkgui::shaders::has(sh.id)
                || funkgui::shaders::set(sh.id, sh.binary.data(), static_cast<uint32_t>(sh.binary.size()), why))
                shaderIds_[k] = sh.id;
            else
                shaderWhy_[k] = why.empty() ? std::string("the GPU refused it") : why;
        }
    }

    void Panel::setPanelView(bool on)
    {
        if (on == panelView_)
            return;
        panelView_ = on;
        port_.setPanelView(on);
        guiScrollX_ = guiScrollY_ = 0.0f;
        if (on && (focus_ == editorItem_ || focus_ == problemsItem_))
            setFocus(-1);
        layout();
    }

    const GuiControlItem& Panel::guiControl(int slot) const
    {
        return static_cast<const GuiControlItem&>(
            *items_[static_cast<std::size_t>(itemOfGuiSlot_[static_cast<std::size_t>(std::clamp(slot, 0, kMaxControls - 1))])]);
    }

    const Item& Panel::guiItem(int kind, int k) const
    {
        const std::vector<int>& pool = kind == 0   ? guiPads_
                                     : kind == 1 ? guiEnvelopes_
                                     : kind == 2 ? guiTabs_
                                     : kind == 3 ? guiKeyboards_
                                                 : guiShaders_;
        return *items_[static_cast<std::size_t>(pool[static_cast<std::size_t>(std::clamp(k, 0, static_cast<int>(pool.size()) - 1))])];
    }

    void Panel::chooseGuiPage(int node, int page)
    {
        if (node < 0 || static_cast<std::size_t>(node) >= panelSpec_.gui.nodes.size())
            return;
        guiPages_.resize(panelSpec_.gui.nodes.size(), 0);
        if (guiPages_[static_cast<std::size_t>(node)] == page)
            return;
        guiPages_[static_cast<std::size_t>(node)] = page;
        port_.setGuiPages(guiPageTitles(panelSpec_.gui, guiPages_));
        layout();
    }

    void Panel::scrollGui(float dx, float dy)
    {
        const float x = std::clamp(guiScrollX_ + dx, 0.0f, gui_.maxScrollX), y = std::clamp(guiScrollY_ + dy, 0.0f, gui_.maxScrollY);
        if (x == guiScrollX_ && y == guiScrollY_)
            return;
        guiScrollX_ = x;
        guiScrollY_ = y;
        layout();
    }

    funkgui::Point Panel::centreOfView(bool panel) const { return centreOf(panel ? panelViewItem_ : viewItem_); }

    std::string Panel::projectLabel() const
    {
        const ProjectState st = port_.project();
        if (st.name.empty())
            return "Projects";
        std::string label = asciiText(st.name) + (st.version > 0 ? " v" + std::to_string(st.version) : "");
        if (st.edited || (st.preset.empty() && st.dialsEdited))
            label += " (edited)";
        if (!st.preset.empty())
            label += "  -  " + asciiText(st.preset) + (st.dialsEdited ? " (edited)" : "");
        return label;
    }

    void Panel::openZoom()
    {
        const auto* z = static_cast<const ZoomModel*>(zoom_);
        if (host_ == nullptr || z == nullptr || z->count() == 0)
            return;
        funkgui::MenuRequest request;
        for (int i = 0; i < z->count(); ++i)
            request.items.push_back({ i + 1, z->label(i), z->enabled(i), i == z->active(), false });
        request.anchor = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(zoomItem_)]).bounds();
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) {
            auto* m = static_cast<ZoomModel*>(zoom_);
            if (m != nullptr && chosen >= 1 && chosen <= m->count() && gestures_)
                m->select(chosen - 1, *gestures_);
        });
    }

    // The status, "RUNNING   COMPILED IN 111 MS", as the card's header shows it: "Running · compiled in 111 ms".
    std::string Panel::statusShown() const
    {
        std::string out;
        bool first = true;
        for (std::size_t i = 0; i < status_.size(); ++i)
        {
            const char ch = status_[i];
            if (ch == ' ' && i + 2 < status_.size() && status_[i + 1] == ' ' && status_[i + 2] == ' ')
            {
                out += " - ";
                i += 2;
                while (i + 1 < status_.size() && status_[i + 1] == ' ')
                    ++i;
                continue;
            }
            const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            out += first && std::isalpha(static_cast<unsigned char>(ch)) ? ch : lower;
            if (std::isalpha(static_cast<unsigned char>(ch)))
                first = false;
        }
        return out;
    }

    void Panel::setFocus(int item)
    {
        if (focus_ != item && focus_ >= 0)
            items_[static_cast<std::size_t>(focus_)]->focusChanged(false);
        focus_ = item;
        if (item >= 0)
        {
            touched_ = item;
            items_[static_cast<std::size_t>(item)]->focusChanged(true);
        }
    }

    // ---- frame --------------------------------------------------------------------------------------------------------
    void Panel::tick(float dt)
    {
        refresh(host_ != nullptr ? host_->nowSeconds() : 0.0);
        syncExplorer();
        // The message box follows its text, one row to eight: the conversation gives it the room.
        if (std::fabs(inputItem().desiredHeight(inputItem().box().w, kBarH) - composerH_) > 0.5f)
            layoutChat();
        // The displays: what the Engine published since the last frame, and the scopes' samples.
        {
            Meters m;
            if (port_.meters(m))
                meters_ = m;
            // The fields are this panel's program's once the audio thread plays it; until then they are another's,
            // and read as 0.
            Meters shown = meters_;
            if (shown.program != panelProgram_)
                std::fill(std::begin(shown.taps), std::end(shown.taps), 0.0f);
            bool readIn = false, readOut = false;
            for (const int i : itemOfDisplay_)
            {
                bool input = false;
                if (static_cast<const DisplayItem&>(*items_[static_cast<std::size_t>(i)]).wantsScope(input))
                    (input ? readIn : readOut) = true;
            }
            if (readIn || readOut)
                scopeIn_.resize(DisplayItem::kScopeSamples), scopeOut_.resize(DisplayItem::kScopeSamples);
            if (readIn)
                port_.scope(true, scopeIn_.data(), DisplayItem::kScopeSamples);
            if (readOut)
                port_.scope(false, scopeOut_.data(), DisplayItem::kScopeSamples);
            for (const int i : itemOfDisplay_)
            {
                auto& item = static_cast<DisplayItem&>(*items_[static_cast<std::size_t>(i)]);
                bool input = false;
                const bool scope = item.wantsScope(input);
                item.update(shown, scope ? (input ? scopeIn_.data() : scopeOut_.data()) : nullptr, dt);
            }
            // The controls' live= fields: where the program has each parameter now (none until it plays this program).
            for (const int i : itemOfGuiSlot_)
            {
                if (i < 0)
                    continue;
                auto& item = static_cast<GuiControlItem&>(*items_[static_cast<std::size_t>(i)]);
                const int d = item.liveDisplay();
                if (d < 0 || d >= static_cast<int>(panelSpec_.displays.size()))
                    continue;
                const int t = panelSpec_.displays[static_cast<std::size_t>(d)].tap;
                const bool now = meters_.program == panelProgram_ && t >= 0 && t < kMaxDisplays;
                item.setLive(now ? meters_.taps[t] : std::numeric_limits<float>::quiet_NaN());
            }
            // The shaders' inputs: a parameter's 0..1, a field's value, the audio's level.
            for (std::size_t k = 0; k < guiShaders_.size(); ++k)
            {
                auto& item = static_cast<GuiShaderItem&>(*items_[static_cast<std::size_t>(guiShaders_[k])]);
                const int node = shaderNode_[k];
                if (!item.shown() || node < 0 || static_cast<std::size_t>(node) >= panelSpec_.gui.nodes.size())
                    continue;
                const GuiNode& n = panelSpec_.gui.nodes[static_cast<std::size_t>(node)];
                std::array<float, 8> v{};
                for (std::size_t j = 0; j < v.size(); ++j)
                {
                    const std::string& in = n.inputs[j];
                    if (in == "in")
                        v[j] = std::max(shown.inPeak[0], shown.inPeak[1]);
                    else if (in == "out")
                        v[j] = std::max(shown.outPeak[0], shown.outPeak[1]);
                    else if (in.rfind("param:", 0) == 0)
                    {
                        const int slot = std::atoi(in.c_str() + 6);
                        if (slot >= 0 && slot < kMaxControls && itemOfGuiSlot_[static_cast<std::size_t>(slot)] >= 0)
                            v[j] = static_cast<GuiControlItem&>(*items_[static_cast<std::size_t>(itemOfGuiSlot_[static_cast<std::size_t>(slot)])])
                                       .value()
                                       .value01();
                    }
                    else if (in.rfind("field:", 0) == 0)
                    {
                        const int d = std::atoi(in.c_str() + 6);
                        const int t = d >= 0 && d < static_cast<int>(panelSpec_.displays.size())
                                        ? panelSpec_.displays[static_cast<std::size_t>(d)].tap
                                        : -1;
                        v[j] = t >= 0 && t < kMaxDisplays ? shown.taps[t] : 0.0f;
                    }
                }
                item.setInputs(v);
            }
        }
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
        refresh(nowSec);                                 // a compile finishing while nothing moves
    }

    bool Panel::wantsFullRate() const
    {
        if (dirty_ || compiling_)
            return true;
        return std::any_of(items_.begin(), items_.end(), [](const auto& it) { return !it->settled(); });
    }

    void Panel::draw(funkgui::Canvas& c, const funkgui::Theme& hostTheme)
    {
        const funkgui::Theme th = productTheme(hostTheme);
        files().assist().setTheme(th);                   // the code's colours follow the theme
        const float contentW = width_ - 2.0f * kMargin;

        const chat::Palette pal = chat::palette(th);
        // The top bar: the product's mark, its name and kind; the project, the view's switch, the theme (items); the
        // load (the CPU's share), red while the output guard acts.
        {
            const funkgui::Canvas::Scope s(c, kHeaderTextTag, false);
            const funkgui::Rect bar{ kMargin, kTopBarY, contentW, kTopBarH };
            c.rrect(bar.x, bar.y, bar.w, bar.h, 12.0f, pal.card, 1.0f, pal.cardEdge);
            const float mid = bar.centreY();
            const float mx = bar.x + 10.0f;
            c.rrect(mx, mid - 13.0f, 26.0f, 26.0f, 8.0f, pal.accent.withAlpha(0.16f));
            chat::drawIcon(c, chat::Icon::wave, mx + 13.0f, mid, 12.0f, pal.accent);
            c.text(product::kName, mx + 36.0f, c.capCentreTop(mid - 7.0f, chat::kStrong), chat::kStrong, pal.text);
            c.text(subtitle_.c_str(), mx + 36.0f, c.capCentreTop(mid + 8.0f, chat::kSmall), chat::kSmall, pal.faint);
            // The Code / Panel switch's track, under its two halves.
            const funkgui::Rect code = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(viewItem_)]).bounds();
            const funkgui::Rect panel = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(panelViewItem_)]).bounds();
            c.rrect(code.x - 3.0f, code.y - 3.0f, panel.right() - code.x + 6.0f, code.h + 6.0f, 0.5f * (code.h + 6.0f), pal.raised,
                    1.0f, pal.cardEdge);
            const funkgui::Rect theme = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(themeItem_)]).bounds();
            const bool alarm = port_.alerts() != 0;
            c.text(meter_.c_str(), theme.x - 10.0f, c.capCentreTop(mid, chat::kSmall), chat::kSmall, alarm ? pal.danger : pal.faint,
                   funkgui::Align::right);
        }

        // The program's card: its header's status (a light, and what the last run did), left of its actions.
        {
            const funkgui::Canvas::Scope s(c, kStatusTag, false);
            const funkgui::Rect& r = programCard_;
            c.rrect(r.x, r.y, r.w, r.h, kCardRadius, pal.card, 1.0f, pal.cardEdge);
            const float mid = kActionsY + 0.5f * kActionH;
            const bool failed = !lastOk_ && !compiling_;
            const std::string shown = statusShown();
            const float tx = r.x + kInset + 14.0f + (explorerShown_ || panelView_ ? 0.0f : 30.0f);   // after the explorer's button
            const std::string text = fitText(c, shown, chat::kSmall, std::max(0.0f, actionsX_ - 12.0f - tx));
            constexpr funkgui::Col kPlaying{ 0x46, 0xBE, 0x82, 255 };   // the program plays as written
            const funkgui::Col light = failed ? pal.danger : (compiling_ ? pal.soft : (edited_ ? pal.faint : kPlaying));
            if (!text.empty())
            {
                c.disc(tx - 9.0f, mid, 3.0f, light);
                c.text(text.c_str(), tx, c.capCentreTop(mid, chat::kSmall), chat::kSmall, failed ? pal.danger : pal.soft);
            }
        }

        // The explorer's card, as the chat's: its title in the header (its chevron is an item, its rows another).
        if (explorerShown_)
        {
            const funkgui::Canvas::Scope s(c, kExplorerTag, false);
            const funkgui::Rect& r = explorerCard_;
            c.rrect(r.x, r.y, r.w, r.h, kCardRadius, pal.card, 1.0f, pal.cardEdge);
            const float ty = r.y + kCardPad + 0.5f * kIconButton;
            c.text("Explorer", r.x + kCardPad + 4.0f, c.capCentreTop(ty, chat::kTitle), chat::kTitle, pal.text);
        }

        // The chat's card (rounded, a shade off the ground: the panel around it is square), its title, and the line under
        // the message box (what Claude is doing, else the last reply's tokens).
        {
            const funkgui::Canvas::Scope s(c, kChatTag, false);
            const chat::Palette p = chat::palette(th);
            c.rrect(card_.x, card_.y, card_.w, card_.h, kCardRadius, p.card, 1.0f, p.cardEdge);
            if (!chatOpen_)
            {
                // The rail: its button (drawn as an item), and under it a light while Claude works, or a dot when it
                // has written since the chat was folded.
                const float dy = card_.y + 8.0f + kIconButton + 14.0f;
                if (chat_.busy)
                    c.disc(card_.centreX(), dy, 4.0f, p.accent);
                else if (unread_)
                    c.disc(card_.centreX(), dy, 4.0f, p.accent.withAlpha(0.0f), 1.5f, p.accent);
            }
        }
        if (chatOpen_)
        {
            const funkgui::Canvas::Scope s(c, kChatTag, false);
            const chat::Palette p = chat::palette(th);
            const float ty = headerY_ + kIconButton * 0.5f;
            const float tx = card_.x + kCardPad + 4.0f;
            c.disc(tx + 4.0f, ty, chat_.busy ? 4.0f : 3.5f, chat_.busy ? p.accent : funkgui::mix(p.card, p.accent, 0.75f));
            c.text("Claude", tx + 15.0f, c.capCentreTop(ty, chat::kTitle), chat::kTitle, p.text);
            const float sub = tx + 15.0f + c.textWidth("Claude", chat::kTitle) + 10.0f;
            const float pillW = chat_.signedIn ? static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(accountItem_)]).bounds().w + 6.0f
                                               : 0.0f;
            const float subRoom = card_.right() - kCardPad - 3.0f * kIconButton - 16.0f - pillW - sub;
            const std::string kind = !desktop_.empty()             ? "Claude Desktop calls this " + desktop_
                                   : product::kIsInstrument          ? "builds and edits your synth"
                                                                     : "builds and edits your effect";
            if (subRoom > 60.0f)
                c.text(fitText(c, kind, chat::kSmall, subRoom).c_str(), sub, c.capCentreTop(ty, chat::kSmall), chat::kSmall, p.faint);
            const funkgui::Rect box = inputItem().box();
            c.text(fitText(c, activity_, chat::kSmall, box.w - 8.0f).c_str(), box.x + 6.0f,
                   c.capCentreTop(usageY_ + kUsageH * 0.5f, chat::kSmall), chat::kSmall, chat_.busy ? p.dim : p.faint);
        }

        // The dials' sections, as a panel's printing: a frame each, its title cut into the top edge.
        {
            const funkgui::Canvas::Scope s(c, kSectionTag, false);
            for (const SectionFrame& f : dials_.sections)
            {
                const funkgui::Rect& r = f.r;
                // A colour tints the frame's fill and edge, and its title.
                const bool tinted = !f.color.empty();
                const funkgui::Col col = looks::panelColor(f.color, th, th.accent);
                const funkgui::Col edge = tinted ? funkgui::premix(th.ground, col, 0.65f) : th.ink32;
                const funkgui::Col fill = tinted ? funkgui::premix(funkgui::mix(th.ground, th.ink16, 0.22f), col, 0.08f)
                                                 : funkgui::mix(th.ground, th.ink16, 0.22f);
                c.rrect(r.x, r.y, r.w, r.h, 0.0f, fill);
                const std::string title = upper(asciiText(f.title));
                if (title.empty())
                    c.hairlineH(r.x, r.y, r.w, edge);
                else
                {
                    const float tw = std::min(c.textWidth(title.c_str(), kSectionTitle), r.w - 16.0f);
                    const float l = r.centreX() - 0.5f * tw - 7.0f, rr = r.centreX() + 0.5f * tw + 7.0f;
                    c.hairlineH(r.x, r.y, std::max(0.0f, l - r.x), edge);
                    c.hairlineH(rr, r.y, std::max(0.0f, r.right() - rr), edge);
                    c.text(fitText(c, title, kSectionTitle, r.w - 16.0f).c_str(), r.centreX(), c.capCentreTop(r.y, kSectionTitle),
                           kSectionTitle, tinted ? col : th.ink70, funkgui::Align::centre);
                }
                c.hairlineV(r.x, r.y, r.h, edge);
                c.hairlineV(r.right() - 1.0f, r.y, r.h, edge);
                c.hairlineH(r.x, r.bottom() - 1.0f, r.w, edge);
            }
        }

        // A .gui file's interface: its ground, frames, labels and dividers, then its items, all in its area, in its
        // theme (GuiItems.h), and a bar at its edge where it scrolls.
        const bool gui = !panelSpec_.gui.empty();
        const funkgui::Theme gth = gui ? guiTheme(th, panelSpec_.gui) : th;
        if (gui)
        {
            const funkgui::Canvas::Scope s(c, kSectionTag, false);
            const funkgui::Canvas::ClipScope clip(c, gui_.area);
            drawGuiPanel(c, gth, panelSpec_, gui_);
        }
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            if (!items_[i]->shown())
                continue;
            if (gui && i < guiItem_.size() && guiItem_[i])
            {
                const funkgui::Canvas::ClipScope clip(c, gui_.area);
                items_[i]->draw(c, gth, ring_ && focus_ == static_cast<int>(i));
            }
            else
                items_[i]->draw(c, th, ring_ && focus_ == static_cast<int>(i));
        }
        files().drawOverlays(c, th, width_, height_);    // the completion list, the signature, the hover card
        if (gui && (gui_.maxScrollY > 0.0f || gui_.maxScrollX > 0.0f))
        {
            const funkgui::Rect& a = gui_.area;
            if (gui_.maxScrollY > 0.0f)
            {
                const float h = std::max(24.0f, a.h * a.h / (a.h + gui_.maxScrollY));
                const float y = a.y + (a.h - h) * guiScrollY_ / gui_.maxScrollY;
                c.rrect(a.right() + kGuiScrollGutter - 4.0f, y, 3.0f, h, 1.5f, th.ink32);
            }
            if (gui_.maxScrollX > 0.0f)
            {
                const float w = std::max(24.0f, a.w * a.w / (a.w + gui_.maxScrollX));
                const float x = a.x + (a.w - w) * guiScrollX_ / gui_.maxScrollX;
                c.rrect(x, a.bottom() - 4.0f, w, 3.0f, 1.5f, th.ink32);
            }
        }

        // Footer: the spec line of the control under the hand, else the focused one, else the last one touched; the
        // dialog's own while it is open.
        char line[256] = {};
        if (keyOpen_)
            std::snprintf(line, sizeof line, "%s",
                          tab_ == DialogTab::key ? "API KEY   CMD-V PASTES IT   RETURN SAVES IT   ESCAPE CANCELS"
                          : chat_.signedIn       ? "ACCOUNT   SIGNED IN   ESCAPE CLOSES"
                                                 : "ACCOUNT   TAB MOVES TO THE PASSWORD   CMD-V PASTES   RETURN SIGNS IN   ESCAPE CANCELS");
        else if (export_.isOpen())
            std::snprintf(line, sizeof line, "EXPORT   TAB THE NEXT FIELD   A PILL TOGGLES   ESCAPE KEEPS THE INFO AND CLOSES");
        else if (projects_.isOpen())
            std::snprintf(line, sizeof line, "PROJECTS   UP AND DOWN CHOOSE   TAB THE NEXT LIST   RETURN SAVES A NAME TYPED, ELSE OPENS   ESCAPE CLOSES");
        else
        {
            int src = pressed_;
            if (src < 0 && hasPointer_)
                src = itemAt(pointer_);
            if (src < 0)
                src = focus_ >= 0 ? focus_ : touched_;
            if (src >= 0)
                items_[static_cast<std::size_t>(src)]->specLine(line, sizeof line);
        }
        if (line[0] != '\0')
        {
            const ButtonItem& zoom = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(zoomItem_)]);
            const float zoomW = zoom.shown() ? zoom.bounds().w + 12.0f : 0.0f;
            const std::string shown = fitFields(c, line, contentW - zoomW - 8.0f);
            const funkgui::Canvas::Scope s(c, funkgui::tags::hint, false);
            c.text(shown.c_str(), kMargin + 8.0f, c.capCentreTop(height_ - 0.5f * kFooterH, funkgui::type::kMicro),
                   funkgui::type::kMicro, th.ink52);
        }

        if (dropActive_ && chatOpen_)
        {
            // A drag of pictures over the editor: the chat's card takes them.
            const funkgui::Canvas::Scope s(c, kChatTag, false);
            const chat::Palette p = chat::palette(th);
            const funkgui::Rect r{ card_.x + 6.0f, card_.y + 6.0f, card_.w - 12.0f, card_.h - 12.0f };
            c.rrect(r.x, r.y, r.w, r.h, kCardRadius - 4.0f, dropHover_ ? p.accent.withAlpha(0.14f) : p.card.withAlpha(0.6f),
                    dropHover_ ? 2.0f : 1.0f, dropHover_ ? p.accent : p.composerEdge);
            chat::drawIcon(c, chat::Icon::image, r.centreX(), r.centreY() - 18.0f, 26.0f, dropHover_ ? p.accent : p.soft);
            c.text(dropHover_ ? "Drop to add for Claude" : "Drop pictures here for Claude", r.centreX(),
                   c.capCentreTop(r.centreY() + 14.0f, chat::kStrong), chat::kStrong, dropHover_ ? p.text : p.soft,
                   funkgui::Align::centre);
        }
        if (keyOpen_)
            drawKeyDialog(c, th);
        if (projects_.isOpen())
        {
            const funkgui::Canvas::Scope s(c, kProjectsTag, false);
            projects_.draw(c, th, width_, height_, pointer_, hasPointer_);
        }
        if (export_.isOpen())
        {
            const funkgui::Canvas::Scope s(c, kExportTag, false);
            export_.draw(c, th, width_, height_, pointer_, hasPointer_);
        }
    }

    // ---- input --------------------------------------------------------------------------------------------------------
    int Panel::itemAt(funkgui::Point p) const
    {
        if (dialogOpen())
            return -1;                                   // the dialog covers the panel
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

    int Panel::textItem() const
    {
        return focus_ >= 0 && items_[static_cast<std::size_t>(focus_)]->takesAllKeys() ? focus_ : -1;
    }

    void Panel::pointerDown(const funkgui::PointerEvent& e)
    {
        ring_ = false;                                   // the pointer is the affordance now
        pointer_ = { e.x, e.y };
        hasPointer_ = true;
        if (keyOpen_)
        {
            pressed_ = -1;
            keyDialogPointerDown(pointer_);
            return;
        }
        if (export_.isOpen())
        {
            pressed_ = -1;
            export_.pointerDown(pointer_, width_, height_);
            afterExport(true);
            return;
        }
        if (projects_.isOpen())
        {
            pressed_ = -1;
            projects_.pointerDown(pointer_, width_, height_);
            afterProjects(true);
            return;
        }
        pressed_ = itemAt(pointer_);
        if (pressed_ >= 0 && items_[static_cast<std::size_t>(pressed_)]->focusable())
            setFocus(pressed_);
        else if (pressed_ < 0 && textItem() >= 0)
            setFocus(-1);                                // a press beside everything takes the caret away
        if (pressed_ >= 0 && gestures_ && host_ != nullptr)
        {
            touched_ = pressed_;
            items_[static_cast<std::size_t>(pressed_)]->pointerDown(e, *gestures_, *host_);
            if (pressed_ == chatViewItem_)
                reshowChat();                            // a thinking row opened or closed
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
        if (keyOpen_)
            return;
        if (export_.isOpen())
            return;
        if (projects_.isOpen())
        {
            projects_.doubleClick({ e.x, e.y }, width_, height_);
            afterProjects(true);
            return;
        }
        const int i = itemAt({ e.x, e.y });
        if (i >= 0 && gestures_)
        {
            touched_ = i;
            items_[static_cast<std::size_t>(i)]->doubleClick(e, *gestures_);
        }
        dirty_ = true;
    }

    bool Panel::wheel(const funkgui::WheelEvent& e)
    {
        dirty_ = true;
        if (export_.isOpen())
            return true;
        if (projects_.isOpen())
        {
            projects_.wheel(e, width_, height_);
            return true;
        }
        const int i = itemAt({ e.x, e.y });
        // A .gui interface taller or wider than its area scrolls, but for a control under the pointer (it takes the
        // wheel itself).
        if (!panelSpec_.gui.empty() && gui_.area.contains({ e.x, e.y }) && (gui_.maxScrollX > 0.0f || gui_.maxScrollY > 0.0f)
            && (i < 0 || std::find(itemOfGuiSlot_.begin(), itemOfGuiSlot_.end(), i) == itemOfGuiSlot_.end()))
        {
            const float k = e.smooth ? 512.0f : 292.0f;     // px a unit of the wheel (the chat's)
            const float dy = e.reversed ? -e.dy : e.dy, dx = e.reversed ? -e.dx : e.dx;
            if (e.mods.shift && dx == 0.0f)
                scrollGui(-dy * k, 0.0f);
            else
                scrollGui(-dx * k, -dy * k);
            return true;
        }
        if (i < 0 || !gestures_ || host_ == nullptr)
            return false;
        touched_ = i;
        if (i == chatViewItem_)
            ++chatA11y_;                                 // what the conversation shows moved
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
                setFocus(i);
                return true;
            }
        }
        return false;
    }

    bool Panel::key(const funkgui::KeyEvent& e)
    {
        dirty_ = true;
        if (keyOpen_)
            return keyDialogKey(e);
        if (export_.isOpen())
        {
            export_.key(e, host_, width_, height_);
            afterExport(true);
            return true;
        }
        if (projects_.isOpen())
        {
            projects_.key(e, host_, width_, height_);
            afterProjects(true);
            return true;
        }
        // RUN, from anywhere in the panel; Cmd-S, a new version of the project open.
        if (e.key == funkgui::Key::enter && (e.mods.cmd || e.mods.ctrl))
        {
            runNow();
            return true;
        }
        if (e.key == funkgui::Key::character && e.mods.cmd && (e.ch == 's' || e.ch == 'S'))
        {
            saveVersion();
            return true;
        }
        // A text field, while it has the caret: every key is its own (Tab indents in the editor), but Escape, which
        // leaves it, and a Tab it does not use, which moves on.
        if (const int t = textItem(); t >= 0)
        {
            if (gestures_ && items_[static_cast<std::size_t>(t)]->key(e, *gestures_))
                return true;
            if (e.key == funkgui::Key::escape)
            {
                setFocus(-1);
                return true;
            }
            if (e.key == funkgui::Key::tab)
            {
                ring_ = moveFocus(e.mods.shift ? -1 : 1);
                return ring_;
            }
            return false;
        }
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
        if (keyOpen_)
        {
            const KeyDialog d = keyDialog();
            const DialogButtons b = dialogButtons();
            const bool button = (!b.primary.empty() && d.primary.contains(pointer_))
                             || (!b.secondary.empty() && d.secondary.contains(pointer_))
                             || (!b.danger.empty() && d.danger.contains(pointer_))
                             || (!b.extra.empty() && d.extra.contains(pointer_)) || d.accountTab.contains(pointer_)
                             || d.keyTab.contains(pointer_);
            return button ? funkgui::Cursor::pointingHand : funkgui::Cursor::normal;
        }
        if (export_.isOpen())
            return export_.cursor(pointer_, width_, height_);
        if (projects_.isOpen())
            return projects_.cursor(pointer_, width_, height_);
        const int i = pressed_ >= 0 ? pressed_ : (hasPointer_ ? itemAt(pointer_) : -1);
        return i >= 0 ? items_[static_cast<std::size_t>(i)]->cursorAt(pointer_) : funkgui::Cursor::normal;
    }

    // ---- accessibility ------------------------------------------------------------------------------------------------
    namespace
    {
        // The access dialog's ids, past every item's.
        constexpr uint32_t kKeyDialogId = 0x7FFF0000u;
        constexpr uint32_t kKeyFieldId = kKeyDialogId + 1, kKeySaveId = kKeyDialogId + 2,
                           kKeyCancelId = kKeyDialogId + 3, kKeyRemoveId = kKeyDialogId + 4,
                           kKeyExtraId = kKeyDialogId + 5, kAccountTabId = kKeyDialogId + 6,
                           kKeyTabId = kKeyDialogId + 7, kUserFieldId = kKeyDialogId + 8,
                           kPassFieldId = kKeyDialogId + 9;
    }

    void Panel::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (keyOpen_)
        {
            // Never the key or the password: their lengths only. The username is not a secret.
            const KeyDialog d = keyDialog();
            const DialogButtons b = dialogButtons();
            const bool account = tab_ == DialogTab::account;
            funkgui::A11yItem box;
            box.id = kKeyDialogId;
            box.role = funkgui::a11yrole::group;
            box.bounds = d.box;
            box.title = account ? "Your TestingPlayground account" : "Anthropic API key";
            box.description = account ? (chat_.signedIn ? "Signed in as " + chat_.account
                                                               + (chat_.credit.empty() ? std::string() : ", " + chat_.credit)
                                                         : std::string("Claude runs on the server, charged to the account's credit"))
                                      : std::string("Stored encrypted on this Mac, for both playground plugins");
            if (account && chat_.signedIn && chat_.details.known)
            {
                const ChatAccount& info = chat_.details;
                char spent[32];
                std::snprintf(spent, sizeof spent, "$%.2f", info.costUsd);
                box.description += ". This month: " + std::to_string(info.requests) + (info.requests == 1 ? " request, " : " requests, ")
                                 + spent + " spent, " + countText(info.inputTokens) + " tokens in, "
                                 + countText(info.outputTokens) + " out. Signed in " + shortDate(info.since) + ", until "
                                 + shortDate(info.expires);
            }
            out.push_back(box);
            const auto tab = [&](uint32_t id, const funkgui::Rect& r, const char* title, bool chosen) {
                funkgui::A11yItem t;
                t.id = id;
                t.parent = kKeyDialogId;
                t.role = funkgui::A11yRole::toggleButton;
                t.bounds = r;
                t.title = title;
                t.checkable = true;
                t.checked = chosen;
                out.push_back(std::move(t));
            };
            tab(kAccountTabId, d.accountTab, "Account", account);
            tab(kKeyTabId, d.keyTab, "API key", !account);
            const auto field = [&](uint32_t id, const funkgui::Rect& r, const char* title, std::string value,
                                   std::string help) {
                funkgui::A11yItem f;
                f.id = id;
                f.parent = kKeyDialogId;
                f.role = funkgui::a11yrole::editableText;
                f.bounds = r;
                f.title = title;
                f.value = std::move(value);
                f.help = keyWhy_.empty() ? std::move(help) : keyWhy_;
                out.push_back(std::move(f));
            };
            const auto length = [](const std::string& t) {
                return t.empty() ? std::string("empty") : std::to_string(t.size()) + " characters";
            };
            if (!account)
                field(kKeyFieldId, d.field, "Key", length(keyEdit_.buffer), "Command V pastes it");
            else if (!chat_.signedIn)
            {
                field(kUserFieldId, d.user, "Username", userEdit_.buffer.empty() ? "empty" : userEdit_.buffer, "");
                field(kPassFieldId, d.pass, "Password", length(passEdit_.buffer), "Command V pastes it");
            }
            const auto button = [&](uint32_t id, const funkgui::Rect& r, const std::string& title) {
                if (title.empty())
                    return;
                funkgui::A11yItem a;
                a.id = id;
                a.parent = kKeyDialogId;
                a.role = funkgui::A11yRole::button;
                a.bounds = r;
                a.title = title == "Remove key" ? std::string("Remove the stored key")
                        : title == "Signing in" ? std::string("Signing in")
                                                : title;
                out.push_back(std::move(a));
            };
            button(kKeySaveId, d.primary, b.primary);
            button(kKeyCancelId, d.secondary, b.secondary);
            button(kKeyRemoveId, d.danger, b.danger);
            button(kKeyExtraId, d.extra, b.extra);
            return;
        }
        if (export_.isOpen())
        {
            export_.accessibility(out, width_, height_);
            return;
        }
        if (projects_.isOpen())
        {
            projects_.accessibility(out, width_, height_);
            return;
        }
        for (const auto& item : items_)
            if (item->shown())
                item->accessibility(out);
    }

    uint32_t Panel::a11yRevision() const
    {
        // Items come and go with the program's dials, the host's zoom, the files' tabs, the conversation (as it grows and
        // scrolls) and the key dialog, and move with the panel's size.
        static_assert(kMaxControls <= 32, "the dials' mask");
        uint32_t r = items_[static_cast<std::size_t>(zoomItem_)]->shown() ? 1u : 0u;
        uint32_t dials = 0;                                   // the dials shown, one bit each, mixed in below
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0 && items_[static_cast<std::size_t>(i)]->shown())
                dials |= 1u << c;
        r ^= dials * 0x27D4EB2Fu;
        r |= (keyOpen_ ? 1u : 0u) << 17;
        r ^= (projects_.isOpen() ? 0x9E3779B1u : 0u) ^ (projects_.revision() * 0x165667B1u);
        r ^= (export_.isOpen() ? 0x85EBCA77u : 0u) ^ (export_.revision() * 0xC2B2AE3Du);
        r |= (chat_.ready() ? 1u : 0u) << 18;
        r ^= (tab_ == DialogTab::account ? 0x6A09E667u : 0u) ^ (chat_.signedIn ? 0xBB67AE85u : 0u)
           ^ (chat_.hasKey ? 0x3C6EF372u : 0u) ^ (chat_.signingIn || awaitingSignIn_ ? 0xA54FF53Au : 0u)
           ^ (chat_.useAccount ? 0x510E527Fu : 0u);
        static_assert(kMaxDisplays <= 32, "the displays' mask");
        uint32_t displays = 0;                                // the displays shown, one bit each, mixed in
        for (int d = 0; d < kMaxDisplays; ++d)
            if (items_[static_cast<std::size_t>(itemOfDisplay_[static_cast<std::size_t>(d)])]->shown())
                displays |= 1u << d;
        r ^= displays * 0x9E3779B9u;
        uint32_t gui = 0;                                     // a .gui interface's controls shown
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = itemOfGuiSlot_[static_cast<std::size_t>(c)]; i >= 0 && items_[static_cast<std::size_t>(i)]->shown())
                gui |= 1u << c;
        r ^= gui * 0x2C1B3C6Du;
        // The counters times odd constants: a change in any one of them changes the revision.
        return r ^ (files().layoutRevision() * 0x85EBCA77u) ^ (layoutRevision_ * 0xC2B2AE3Du) ^ (chatA11y_ * 2654435761u)
             ^ (explorer().revision() * 0x27220A95u) ^ (explorerShown_ ? 0x1B873593u : 0u);
    }

    void Panel::a11yAction(uint32_t id, funkgui::A11yAction a, double value)
    {
        if (!gestures_)
            return;
        if (keyOpen_)
        {
            const DialogButtons b = dialogButtons();
            if (id == kKeySaveId)
                dialogAction(b.primary);
            else if (id == kKeyCancelId)
                dialogAction(b.secondary);
            else if (id == kKeyRemoveId)
                dialogAction(b.danger);
            else if (id == kKeyExtraId)
                dialogAction(b.extra);
            else if (id == kAccountTabId && !awaitingSignIn_)
                showTab(DialogTab::account);
            else if (id == kKeyTabId && !awaitingSignIn_)
                showTab(DialogTab::key);
            else if (id == kUserFieldId || id == kPassFieldId)
                accountField_ = id == kUserFieldId ? 0 : 1;
            dirty_ = true;
            return;
        }
        if (export_.isOpen())
        {
            export_.a11yAction(id, a, width_, height_);
            afterExport(true);
            return;
        }
        if (projects_.isOpen())
        {
            projects_.a11yAction(id, a, width_, height_);
            afterProjects(true);
            return;
        }
        for (const auto& item : items_)
            if (item->shown() && item->a11yAction(id, a, value, *gestures_))
                break;
        if (id >= kChatA11yBase && id < kChatA11yBase + (1u << 24))
            reshowChat();
        dirty_ = true;
    }

    void Panel::closeGestures()
    {
        if (gestures_)
            gestures_->closeAll();
        for (const int i : guiKeyboards_)
            static_cast<GuiKeyboardItem&>(*items_[static_cast<std::size_t>(i)]).release();
    }
}
