// Source/editor/Panel.cpp: see Panel.h.
#include "editor/Panel.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/ChatView.h"
#include "editor/Items.h"

#include "editor/Theme.h"

#include "plugin/EditorPort.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Engine.h"

#include "Examples.h"
#include "Product.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
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
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string_view>
#include <utility>

namespace testingplaygroundsynth::ui
{

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
                rest = rest.substr(9);
            }
            p.text = rest;
            out.push_back(std::move(p));
        }
        return out;
    }

    namespace
    {
        // ---- layout (logical px) ----------------------------------------------------------------------------------
        constexpr float kMargin = 20.0f;
        constexpr float kTitleTop = 17.0f;
        constexpr float kSubtitleGap = 14.0f;
        constexpr float kThemeW = 128.0f;
        constexpr float kHeaderRuleY = 46.0f;
        constexpr float kToolbarY = 56.0f;
        constexpr float kButtonH = 22.0f;
        constexpr float kTabsY = 88.0f;                  // the files' tabs, over the editor
        constexpr float kEditorY = kTabsY + 26.0f;
        constexpr float kProblemRowH = 15.0f;
        constexpr float kPlaceW = 150.0f;                // a problem's file, line and column
        constexpr int   kProblemRows = 4;
        constexpr float kDialCellW = 60.0f;              // funkgui::DialGeom, primary: 60 x 80
        constexpr float kDialCellH = 80.0f;
        constexpr float kDialGap = 30.0f;                // between dials, while the column has room
        constexpr float kDialMinGap = 16.0f;             // the least, which keeps long labels (kCaption) apart
        constexpr float kDialRowGap = 12.0f;             // between rows of dials
        constexpr float kFooterH = 36.0f;
        constexpr float kBottomGap = 12.0f;              // over the footer's rule
        // The chat's column, at the right.
        constexpr float kChatShare = 0.32f;              // of the width
        constexpr float kChatMinW = 340.0f;
        constexpr float kChatMaxW = 560.0f;
        constexpr float kChatGap = 24.0f;                // between the columns (the rule in its middle)
        constexpr float kCardPad = 10.0f;                // inside the chat's card
        constexpr float kCardRadius = 14.0f;
        constexpr float kIconButton = 28.0f;             // the header's new chat and key
        constexpr float kBarH = 36.0f;                   // the message box's bottom bar (the pills, send)
        constexpr float kPillH = 26.0f;
        constexpr float kSendD = 28.0f;
        constexpr float kUsageH = 16.0f;                 // the last reply's tokens, under the message box
        constexpr uint32_t kChatA11yBase = 0x20000000u;  // the conversation's accessibility ids (ChatView)
        // The key dialog.
        constexpr float kKeyDialogW = 540.0f;
        constexpr float kKeyDialogH = 240.0f;
        constexpr int   kMaxKeyLength = 320;
        constexpr uint32_t kA11yStride = 64;             // item i's ids: kA11yStride * (i + 1) + 0..63

        // ---- the dials' grid: as few rows as the column's width holds, the dials shared out evenly ------------------
        struct DialGrid
        {
            int rows = 1, cols = 1;
            float gap = kDialGap;
        };
        DialGrid dialGrid(int used, float width)
        {
            const int n = std::max(used, 1);
            const int fit = std::clamp(static_cast<int>(std::floor((width + kDialMinGap) / (kDialCellW + kDialMinGap))), 1,
                                       kMaxControls);
            DialGrid g;
            g.rows = (n + fit - 1) / fit;
            g.cols = (n + g.rows - 1) / g.rows;
            if (g.cols > 1)
                g.gap = std::clamp((width - kDialCellW * static_cast<float>(g.cols)) / static_cast<float>(g.cols - 1),
                                   kDialMinGap, kDialGap);
            return g;
        }

        // ---- a dial over P1..P16, labelled with the running program's parameter ------------------------------------
        class ControlModel final : public funkgui::ValueModel
        {
        public:
            explicit ControlModel(funkgui::ParamPort& port) : port_(port) {}

            void setControl(const Control& c, uint32_t revision)
            {
                control_ = c;
                revision_ = revision;
                const std::string name = upper(c.name);
                std::snprintf(label_, sizeof label_, "%s", name.c_str());
            }
            const Control& control() const noexcept { return control_; }

            uint64_t key() const override
            {
                return (static_cast<uint64_t>(revision_) << 32) | std::bit_cast<uint32_t>(port_.value01());
            }
            void view(funkgui::ValueView& v) const override
            {
                const float raw = port_.value01();
                if (std::isfinite(raw))
                    last01_ = raw;
                v = funkgui::ValueView{};
                v.state = funkgui::ValueState::continuous;
                v.label = label_;
                v.track = last01_;
                v.trackDefault = normOf(control_, control_.def);
                v.atDefault = std::fabs(plainOf(control_, last01_) - control_.def) < 1e-6f;
                const float p = plainOf(control_, last01_);
                switch (control_.type)
                {
                    case ControlType::toggle:
                        std::snprintf(v.text.value, sizeof v.text.value, "%s", p >= 0.5f ? "ON" : "OFF");
                        break;
                    case ControlType::integer:
                        std::snprintf(v.text.value, sizeof v.text.value, "%d", static_cast<int>(p));
                        break;
                    case ControlType::real:
                    {
                        const double a = std::fabs(static_cast<double>(p));
                        std::snprintf(v.text.value, sizeof v.text.value,
                                      a >= 100.0 ? "%.0f" : (a >= 10.0 ? "%.1f" : "%.2f"), static_cast<double>(p));
                        break;
                    }
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
            mutable float last01_ = 0.5f;                // the last finite value01 view() saw: a host's NaN shows it
        };

        class DialItem final : public Item
        {
        public:
            DialItem(funkgui::ParamPort& port, funkgui::DialGeom g, uint32_t id) : model_(port), dial_(model_, g, id) {}

            ControlModel& model() noexcept { return model_; }
            void setGeom(funkgui::DialGeom g) { dial_.setGeom(g); }

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
            bool shown() const override { return model_.control().used; }

        private:
            ControlModel model_;                         // before dial_, which refers to it
            funkgui::Dial dial_;
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
        // and undo steps stay with it.
        constexpr float kTabH = 22.0f;
        constexpr float kTabPad = 12.0f;                 // before a tab's name, and after it
        constexpr float kTabCloseW = 14.0f;              // a closable tab's close box, after its name
        constexpr float kTabMinW = 64.0f;
        constexpr float kTabNarrowW = 44.0f;             // the least a tab narrows to when many do not fit
        constexpr float kTabMaxW = 150.0f;
        constexpr float kTabGap = 2.0f;
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
            : bar_(bar), editorBounds_(editor), id_(id), problems_(problems), close_(std::move(close))
        {
            load(Draft{ singleFile(""), 0 });
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
            for (Tab& t : tabs_)
                t.editor->setBounds(editorBounds_);
        }

        void attach(funkgui::HostServices* host)
        {
            host_ = host;
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
                tabs_[static_cast<std::size_t>(renaming_)].name = typed;
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
        }

        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override
        {
            namespace type = funkgui::type;
            const funkgui::TextEditor& ed = editor();
            ed.draw(c, th, ring);
            drawMarks(c, th);

            const funkgui::Canvas::Scope s(c, kTabTag, false);
            c.hairlineH(bar_.x, bar_.bottom() - 1.0f, bar_.w, th.ink16);
            for (int i = 0; i < count(); ++i)
            {
                const funkgui::Rect r = tabRect(i);
                const bool shown = i == active_;
                const bool hover = r.contains(pointer_);
                if (shown)
                {
                    c.rrect4(r.x, r.y, r.w, r.h, 2.0f, 2.0f, 0.0f, 0.0f, th.ground, 1.0f, th.ink16);
                    c.rrect(r.x + 1.0f, r.bottom() - 2.0f, r.w - 2.0f, 2.0f, 0.0f, th.ink70);
                }
                const float textTop = c.capCentreTop(r.centreY(), type::kMicro);
                if (hasErrors(name(i)))
                    c.disc(r.x + 6.0f, r.centreY(), 2.0f, th.signal);
                const float pad = tabPad(i);
                const float nameW = r.w - 2.0f * pad - (closeShown(i) ? kTabCloseW : 0.0f);
                if (i == renaming_)
                {
                    // The name being edited: its text, the selection (all of it, until a key), the caret.
                    const std::string& b = edit_.buffer;
                    const float w = c.textWidth(b.c_str(), type::kMicro);
                    if (edit_.allSelected && !b.empty())
                        c.rrect(r.x + pad - 1.0f, r.y + 4.0f, std::min(w, nameW) + 2.0f, r.h - 8.0f, 1.0f,
                                th.accentDim);
                    c.text(fitText(c, b, type::kMicro, nameW).c_str(), r.x + pad, textTop, type::kMicro,
                           why_.empty() ? th.ink100 : th.signal);
                    const std::string head = b.substr(0, static_cast<std::size_t>(std::clamp(
                                                             edit_.caret, 0, static_cast<int>(b.size()))));
                    const float cx = r.x + pad + std::min(c.textWidth(head.c_str(), type::kMicro), nameW);
                    c.rrect(cx, r.y + 4.0f, 1.5f, r.h - 8.0f, 0.0f, th.accent);
                }
                else
                {
                    const funkgui::Col ink = hover ? th.accent : (shown ? th.ink100 : th.ink52);
                    // A narrowed tab keeps the name's first letters, without an ellipsis (the footer has it whole).
                    std::string label = name(i);
                    if (narrow(i))
                        while (label.size() > 1 && c.textWidth(label.c_str(), type::kMicro) > nameW)
                            label.pop_back();
                    else
                        label = fitText(c, label, type::kMicro, nameW);
                    c.text(label.c_str(), r.x + pad, textTop, type::kMicro, ink);
                }
                if (closeShown(i))
                    drawCross(c, closeRect(i), 3.0f, closeRect(i).contains(pointer_) ? th.accent : th.ink32);
            }
            if (canAdd())
            {
                const funkgui::Rect n = newTabRect();
                const funkgui::Col ink = n.contains(pointer_) ? th.accent : th.ink52;
                const float cx = n.centreX(), cy = n.centreY();
                c.rrect(cx - 4.0f, cy - 0.6f, 8.0f, 1.2f, 0.0f, ink);
                c.rrect(cx - 0.6f, cy - 4.0f, 1.2f, 8.0f, 0.0f, ink);
            }
        }

        bool contains(funkgui::Point p) const override { return bar_.contains(p) || editor().contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point p) const override
        {
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
            return !bar_.contains({ e.x, e.y }) && editor().wheel(e);
        }
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override
        {
            if (renaming_ < 0)
                return editor().key(e);
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
            editor().setFocused(focused);
            if (!focused)
                endRename();
        }
        bool takesAllKeys() const override { return editor().focused() || renaming_ >= 0; }

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
            o.placeholder = tabs_.empty() ? "An FMinus program: EXAMPLES has some to start from"
                                          : "A file the program can #include by its name";
            t.editor->setOptions(o);
            t.editor->setBounds(editorBounds_);
            t.editor->setHighlighter(std::make_unique<funkgui::text::KeywordHighlighter>(fminusRules()));
            t.editor->setPalette(funkgui::TextEditor::defaultPalette());
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
            const float w = funkgui::text::width(funkgui::FontService::get().atlas(), name(i).c_str(),
                                                 funkgui::type::kMicro);
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

        // A mark in the gutter, beside the line number, on each line of the shown file the last run's errors name.
        void drawMarks(funkgui::Canvas& c, const funkgui::Theme& th) const
        {
            const funkgui::TextEditor& ed = editor();
            const std::string& file = name(active_);
            const funkgui::Rect b = ed.bounds();
            const funkgui::Rect area = ed.textArea();
            const float rowH = ed.rowHeight();
            const float scrollY = ed.scroll().y;
            c.pushClip({ b.x + 1.0f, b.y + 1.0f, b.w - 2.0f, b.h - 2.0f });
            {
                const funkgui::Canvas::Scope s(c, kMarkTag, false);
                for (const Problem& p : problems_)
                {
                    if (!p.error || p.file != file || p.line < 1 || p.line > ed.lineCount())
                        continue;
                    const int row = ed.rowOf({ p.line - 1, 0 });
                    const float y = area.y + static_cast<float>(row) * rowH - scrollY;
                    if (y + rowH < b.y || y > b.bottom())
                        continue;
                    c.rrect(b.x + 3.0f, y + 2.0f, 3.0f, rowH - 4.0f, 1.0f, th.signal);
                }
            }
            c.popClip();
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
        };
    }

    // ---- construction ---------------------------------------------------------------------------------------------------
    // The items are made with no place: layout() gives them their places for the panel's size. Their order is the Tab
    // order.
    Panel::Panel(std::span<funkgui::ParamPort* const> ports, EditorPort& port) : port_(port)
    {
        title_ = upper(product::kName);
        subtitle_ = upper(product::kKind) + "  " + product::kVersion + "  FMINUS";
        width_ = static_cast<float>(kDefaultWidth);
        height_ = static_cast<float>(kDefaultHeight);
        const auto idFor = [this] { return kA11yStride * static_cast<uint32_t>(items_.size() + 1); };
        const auto cells = [](int n) { return std::vector<funkgui::Rect>(static_cast<std::size_t>(n)); };
        const auto add = [this](std::unique_ptr<Item> item) {
            items_.push_back(std::move(item));
            return static_cast<int>(items_.size()) - 1;
        };

        // The toolbar's buttons.
        runItem_ = add(std::make_unique<CellsItem>(
            std::make_unique<ButtonModel>("RUN", "Run the program", [this] { runNow(); }), cells(1),
            funkgui::CellStyle::boxed, idFor(),
            [](char* out, std::size_t n) { std::snprintf(out, n, "RUN   COMPILE THE PROGRAM AND PLAY IT (CMD-RETURN)"); }));
        examplesItem_ = add(std::make_unique<CellsItem>(
            std::make_unique<ButtonModel>("EXAMPLES", "Examples", [this] { openExamples(); }), cells(1),
            funkgui::CellStyle::boxed, idFor(),
            [](char* out, std::size_t n) { std::snprintf(out, n, "EXAMPLES   REPLACE THE TEXT WITH ONE, AND RUN IT"); }));

        // The editor, the problems under it, the dials under them.
        editorItem_ = add(std::make_unique<FilesItem>(funkgui::Rect{}, funkgui::Rect{}, idFor(), problems_,
                                                      [this](int tab) { closeFile(tab); }));
        problemsItem_ = add(std::make_unique<ProblemsItem>(funkgui::Rect{}, idFor(), problems_,
                                                           [this](const Problem& p) { jumpTo(p); }));
        itemOfSlot_.assign(static_cast<std::size_t>(kMaxControls), -1);
        for (int c = 0; c < kMaxControls && c < static_cast<int>(ports.size()); ++c)
            itemOfSlot_[static_cast<std::size_t>(c)] = add(std::make_unique<DialItem>(
                *ports[static_cast<std::size_t>(c)], funkgui::DialGeom{ 0.0f, 0.0f }, idFor()));

        // The chat: the header's buttons (a new chat, the API key), the conversation, the message box and the buttons in
        // its bar (the model, the thinking level, send or stop).
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
                          "Anthropic API key", "API KEY   STORED ENCRYPTED ON THIS MAC, FOR BOTH PLAYGROUND PLUGINS")));
        chatViewItem_ = add(std::make_unique<ChatView>(
            kChatA11yBase, ChatView::Hooks{ [this](const std::string& s) { suggest(s); }, [this] { openKeyDialog(); } }));
        inputItem_ = add(std::make_unique<InputItem>(idFor(), [this] { sendChat(); }));
        modelItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::pill, {}, [this] { return modelLabel(); }, true, [] { return B::Look::ghost; }, {},
                          [this] { openModels(); }, "Model", "MODEL   THE CLAUDE MODEL THE CHAT TALKS TO")));
        thinkingItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::pill, {}, [this] { return thinkingLabel(); }, true, [] { return B::Look::ghost; },
                          [this] { return !chatEfforts().empty(); }, [this] { openThinking(); }, "Thinking level",
                          "THINKING   HOW MUCH CLAUDE THINKS BEFORE IT ANSWERS, AND HOW HARD IT WORKS")));
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

        // THEME, in the header's right corner.
        themeItem_ = add(std::make_unique<CellsItem>(
            std::make_unique<funkgui::ThemeCells>(cells(2), nullptr, funkgui::Point{}, idFor()),
            [this](char* out, std::size_t n) {
                std::snprintf(out, n, "THEME   %s", funkgui::Theme::name(host_ != nullptr ? host_->themeIndex() : 0));
            }));

        // ZOOM, in the footer's right corner; shown when the host offers zoom steps (the plugin's editor does).
        auto zoom = std::make_unique<ZoomModel>();
        ZoomModel* zoomModel = zoom.get();
        zoomItem_ = add(std::make_unique<CellsItem>(
            std::move(zoom), cells(ZoomModel::kMaxSteps), funkgui::CellStyle::text, idFor(),
            [zoomModel](char* out, std::size_t n) {
                const int a = zoomModel->active();
                std::snprintf(out, n, "ZOOM   %s", a >= 0 ? zoomModel->label(a) : "");
            },
            [zoomModel] { return zoomModel->count() > 0; }));
        zoom_ = zoomModel;

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

        // The columns.
        chatW_ = std::clamp(std::round(W * kChatShare), kChatMinW, kChatMaxW);
        chatX_ = W - kMargin - chatW_;
        leftW_ = chatX_ - kChatGap - kMargin;

        // The left column: the toolbar, the tabs and the editor, the problems, the dials.
        static_cast<CellsItem&>(*items_[static_cast<std::size_t>(runItem_)])
            .setCells({ { kMargin, kToolbarY, 64.0f, kButtonH } });
        static_cast<CellsItem&>(*items_[static_cast<std::size_t>(examplesItem_)])
            .setCells({ { kMargin + 72.0f, kToolbarY, 96.0f, kButtonH } });
        // The dials in rows, the first on top: slot c in row c / cols, column c % cols (an unused slot is not shown).
        const DialGrid grid = dialGrid(dialsUsed_, leftW_);
        dialRows_ = grid.rows;
        dialCols_ = grid.cols;
        const float rowStep = kDialCellH + kDialRowGap;
        const float dialsY = bottom - kDialCellH - static_cast<float>(grid.rows - 1) * rowStep;
        const float problemsH = kProblemRowH * static_cast<float>(kProblemRows);
        const float problemsY = dialsY - 16.0f - problemsH;
        files().setGeometry({ kMargin, kTabsY, leftW_, kTabH },
                            { kMargin, kEditorY, leftW_, std::max(80.0f, problemsY - 8.0f - kEditorY) });
        static_cast<ProblemsItem&>(*items_[static_cast<std::size_t>(problemsItem_)])
            .setBounds({ kMargin, problemsY, leftW_, problemsH });
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0)
                static_cast<DialItem&>(*items_[static_cast<std::size_t>(i)])
                    .setGeom(funkgui::DialGeom{ kMargin + static_cast<float>(c % grid.cols) * (kDialCellW + grid.gap),
                                                dialsY + static_cast<float>(c / grid.cols) * rowStep });

        // The chat's card.
        const float right = W - kMargin;
        card_ = { chatX_, kToolbarY, chatW_, bottom - kToolbarY };
        layoutChat();

        // THEME, ZOOM.
        static_cast<CellsItem&>(*items_[static_cast<std::size_t>(themeItem_)])
            .setCells({ { right - kThemeW, 15.0f, 72.0f, 18.0f }, { right - 54.0f, 15.0f, 54.0f, 18.0f } });
        std::vector<funkgui::Rect> zoomCells;
        for (int k = 0; k < ZoomModel::kMaxSteps; ++k)
            zoomCells.push_back({ right - 44.0f * static_cast<float>(ZoomModel::kMaxSteps - k), H - 28.0f, 42.0f,
                                  18.0f });
        static_cast<CellsItem&>(*items_[static_cast<std::size_t>(zoomItem_)]).setCells(std::move(zoomCells));
        ++layoutRevision_;
        dirty_ = true;
    }

    // The chat's card: the header, the conversation, the message box (as tall as its text), its bar's buttons (as wide
    // as their labels) and the usage line.
    void Panel::layoutChat()
    {
        const float inX = card_.x + kCardPad, inW = card_.w - 2.0f * kCardPad;
        const float hy = card_.y + kCardPad;
        static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(keyItem_)])
            .setBounds({ card_.right() - kCardPad - kIconButton, hy, kIconButton, kIconButton });
        static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(newChatItem_)])
            .setBounds({ card_.right() - kCardPad - 2.0f * kIconButton - 4.0f, hy, kIconButton, kIconButton });
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

    // The pills in the message box's bar: as wide as their labels, the thinking one narrowed first when they do not fit.
    void Panel::placeChatBar()
    {
        const funkgui::Rect bar = inputItem().bar();
        auto& model = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(modelItem_)]);
        auto& thinking = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(thinkingItem_)]);
        auto& send = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(sendItem_)]);
        const float y = bar.y + std::floor((bar.h - kPillH) * 0.5f) - 3.0f;
        model.setBounds({ bar.x + 6.0f, y, kPillH, kPillH });
        thinking.setBounds({ bar.x + 6.0f, y, kPillH, kPillH });
        const float room = bar.w - 12.0f - kSendD - 8.0f;
        float mw = model.preferredWidth(), tw = thinking.preferredWidth();
        if (mw + 4.0f + tw > room)
            tw = std::max(64.0f, room - 4.0f - mw);
        if (mw + 4.0f + tw > room)
            mw = std::max(64.0f, room - 4.0f - tw);
        model.setBounds({ bar.x + 6.0f, y, mw, kPillH });
        thinking.setBounds({ bar.x + 6.0f + mw + 4.0f, y, tw, kPillH });
        send.setBounds({ bar.right() - 6.0f - kSendD, bar.y + std::floor((bar.h - kSendD) * 0.5f) - 3.0f, kSendD, kSendD });
        barLabels_ = modelLabel() + '|' + thinkingLabel();
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
    Panel::~Panel() = default;

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
    funkgui::Point Panel::centreOfThinking() const { return centreOf(thinkingItem_); }
    funkgui::Point Panel::centreOfKeyField() const
    {
        const funkgui::Rect r = keyDialog().field;
        return { r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfKeySave() const
    {
        const funkgui::Rect r = keyDialog().save;
        return { r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfKeyCancel() const
    {
        const funkgui::Rect r = keyDialog().cancel;
        return { r.centreX(), r.centreY() };
    }
    std::optional<funkgui::Point> Panel::centreOfKeyRemove() const
    {
        if (!chat_.hasKey)
            return std::nullopt;
        const funkgui::Rect r = keyDialog().remove;
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
        // A finished compile: its messages, and the dials of the program that plays.
        bool statusChanged = false;
        if (const uint64_t r = port_.statusRevision(); r != seenStatus_)
        {
            seenStatus_ = r;
            const CompileStatus s = port_.status();
            lastOk_ = s.ok;
            lastMs_ = s.ms;
            problems_ = parseProblems(s.errors + s.notes);
            // The dials and the source of the program that plays: this compile's when it compiled (the processor
            // takes them on its own timer, which may not have run yet), else the ones before.
            const Controls cs = s.ok ? s.controls : port_.controls();
            if (s.ok)
                running_ = s.sources;
            ++controlsRevision_;
            int used = 0;
            for (int c = 0; c < kMaxControls; ++c)
            {
                if (cs[static_cast<std::size_t>(c)].used)
                    used = c + 1;
                if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0)
                    static_cast<DialItem&>(*items_[static_cast<std::size_t>(i)])
                        .model()
                        .setControl(cs[static_cast<std::size_t>(c)], controlsRevision_);
            }
            // Another count of dials may take another grid (a second row takes height from the editor).
            const DialGrid grid = dialGrid(used, leftW_);
            dialsUsed_ = used;
            if (grid.rows != dialRows_ || grid.cols != dialCols_)
                layout();
            statusChanged = true;
        }
        compiling_ = port_.compiling();
        if (textChanged || statusChanged)
            edited_ = files().draft().files != running_;
        const std::string statusBefore = status_;

        // The status line and the meter.
        if (compiling_)
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
            std::snprintf(meter, sizeof meter, "%s%sCPU %.1f%%",
                          (a & Engine::nonFinite) != 0 ? "NAN REPLACED   " : "",
                          (a & Engine::clipped) != 0 ? "CLIPPED   " : "", static_cast<double>(port_.load()) * 100.0);
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
            if (!s.hasKey)
                e.body += " First add your Anthropic API key: it is stored encrypted on this Mac, for both playground "
                          "plugins, and the chat's requests are billed to it.";
            if (!s.keyProblem.empty())
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
            return asciiText(s.usage);
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
        for (const ChatModel& m : chat_.models)
            if (m.id == chat_.model)
                return m.name.rfind("Claude ", 0) == 0 ? m.name.substr(7) : m.name;
        return chat_.model.empty() ? std::string("Model") : chat_.model;
    }

    std::vector<std::string> Panel::chatEfforts() const
    {
        for (const ChatModel& m : chat_.models)
            if (m.id == chat_.model)
                return m.efforts;
        return {};
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
            const bool hadKey = chat_.hasKey;
            const bool wasBusy = chat_.busy;
            chat_ = chat.chatState();
            if (chat_.busy && !wasBusy)
                busySince_ = nowSec;
            chatView().show(chat_, emptyChat(chat_));
            inputItem().setPlaceholder(chat_.busy ? "Claude is working (the square button stops it)"
                                                  : (product::kIsInstrument ? "Ask Claude for a sound, or about the code"
                                                                            : "Ask Claude for an effect, or about the code"));
            if (modelLabel() + '|' + thinkingLabel() != barLabels_)
                placeChatBar();
            if (hadKey != chat_.hasKey || changed)
            {
                dirty_ = true;
                ++chatA11y_;
            }
        }
        const std::string a = activityText(chat_, nowSec - busySince_);
        if (a != activity_)
        {
            activity_ = a;
            dirty_ = true;
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
        if (text.find_first_not_of(" \t\n") == std::string::npos || chat_.busy)
            return;
        if (!chat_.hasKey)
        {
            pendingSend_ = text;                         // it goes once a key is saved
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
        funkgui::MenuRequest request;
        int id = 1;
        for (const ChatModel& m : chat_.models)
        {
            funkgui::MenuItem item;
            item.id = id++;
            item.label = m.name;
            item.checked = m.id == chat_.model;
            request.items.push_back(std::move(item));
        }
        request.anchor = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(modelItem_)]).bounds();
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        const std::vector<ChatModel> models = chat_.models;
        host_->showMenu(request, [this, models](int chosen) {
            if (chosen < 1 || chosen > static_cast<int>(models.size()))
                return;
            port_.chat().setModel(models[static_cast<std::size_t>(chosen - 1)].id);
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

    // ---- the key dialog -----------------------------------------------------------------------------------------------
    Panel::KeyDialog Panel::keyDialog() const
    {
        KeyDialog d;
        const float w = std::min(kKeyDialogW, width_ - 2.0f * kMargin);
        d.box = { std::round((width_ - w) * 0.5f), std::round((height_ - kKeyDialogH) * 0.42f), w, kKeyDialogH };
        d.field = { d.box.x + 24.0f, d.box.y + 118.0f, w - 48.0f, 34.0f };
        const float by = d.box.bottom() - 24.0f - 32.0f;
        d.save = { d.box.right() - 24.0f - 84.0f, by, 84.0f, 32.0f };
        d.cancel = { d.save.x - 8.0f - 84.0f, by, 84.0f, 32.0f };
        d.remove = { d.box.x + 24.0f, by, 112.0f, 32.0f };
        return d;
    }

    void Panel::openKeyDialog()
    {
        if (keyOpen_)
            return;
        keyOpen_ = true;
        keyEdit_.set("", kMaxKeyLength);
        keyWhy_.clear();
        focusBeforeKey_ = focus_;
        setFocus(-1);
        ring_ = false;
        dirty_ = true;
    }

    void Panel::closeKeyDialog()
    {
        keyOpen_ = false;
        keyEdit_.set("", kMaxKeyLength);                 // nothing of the key stays in the panel
        keyWhy_.clear();
        pendingSend_.clear();
        if (focusBeforeKey_ >= 0)
            setFocus(focusBeforeKey_);
        focusBeforeKey_ = -1;
        dirty_ = true;
    }

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

    // Every key is the dialog's while it is open: Return saves, Escape cancels, Cmd-V pastes; the caret stays at the end
    // (the key is masked, so there is nothing to move it over).
    bool Panel::keyDialogKey(const funkgui::KeyEvent& e)
    {
        using funkgui::Key;
        if (e.key == Key::enter)
            saveKeyFromDialog();
        else if (e.key == Key::escape)
            closeKeyDialog();
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host_ != nullptr && keyEdit_.paste(host_->clipboardText(), kMaxKeyLength))
                keyWhy_.clear();
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
            keyEdit_.allSelected = !keyEdit_.buffer.empty();
        else if (e.key == Key::left || e.key == Key::right || e.key == Key::home || e.key == Key::end
                 || e.key == Key::up || e.key == Key::down || e.key == Key::tab)
        {
        }
        else if (keyEdit_.key(e, kMaxKeyLength))
            keyWhy_.clear();
        keyEdit_.caret = static_cast<int>(keyEdit_.buffer.size());
        dirty_ = true;
        return true;
    }

    void Panel::keyDialogPointerDown(funkgui::Point p)
    {
        const KeyDialog d = keyDialog();
        if (d.save.contains(p))
            saveKeyFromDialog();
        else if (d.cancel.contains(p))
            closeKeyDialog();
        else if (chat_.hasKey && d.remove.contains(p))
            removeKeyFromDialog();
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
        chat::drawIcon(c, chat::Icon::key, x + 15.0f, d.box.y + 37.0f, 15.0f, p.accent);
        c.text("Anthropic API key", x + 42.0f, c.capCentreTop(d.box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        const char* lines[] = { "Claude runs on your own key, from console.anthropic.com (API keys).",
                                "It is stored encrypted on this Mac, for both playground plugins,",
                                "and sent only to api.anthropic.com. Requests are billed to it." };
        float y = d.box.y + 66.0f;
        for (const char* line : lines)
        {
            c.text(fitText(c, line, chat::kSmall, d.box.w - 48.0f).c_str(), x, c.capCentreTop(y + 8.0f, chat::kSmall),
                   chat::kSmall, p.soft);
            y += 16.0f;
        }

        // The field: the key masked (its first and last characters shown), the caret at its end.
        const funkgui::Rect f = d.field;
        c.rrect(f.x, f.y, f.w, f.h, 10.0f, p.composer, 1.0f,
                keyWhy_.empty() ? funkgui::mix(p.composerEdge, p.accent, 0.45f) : p.danger);
        const float textTop = c.capCentreTop(f.centreY(), chat::kBody);
        const std::string shown = keyEdit_.buffer.empty() ? std::string() : maskedKey();
        const float maxW = f.w - 28.0f;
        if (shown.empty())
            c.text("sk-ant-...   (Cmd-V pastes it)", f.x + 14.0f, textTop, chat::kBody, p.faint);
        else
        {
            const float w = std::min(c.textWidth(shown.c_str(), chat::kBody), maxW);
            if (keyEdit_.allSelected)
                c.rrect(f.x + 12.0f, f.y + 8.0f, w + 4.0f, f.h - 16.0f, 3.0f, th.accentDim);
            c.text(fitText(c, shown, chat::kBody, maxW).c_str(), f.x + 14.0f, textTop, chat::kBody, p.text);
        }
        const float cx = f.x + 14.0f + (shown.empty() ? 0.0f : std::min(c.textWidth(shown.c_str(), chat::kBody), maxW));
        c.rrect(cx, f.y + 9.0f, 1.5f, f.h - 18.0f, 0.75f, p.accent);
        if (!keyWhy_.empty())
        {
            const std::string why = asciiText(keyWhy_);
            c.text(fitText(c, why, chat::kSmall, f.w).c_str(), f.x + 2.0f, c.capCentreTop(f.bottom() + 14.0f, chat::kSmall),
                   chat::kSmall, p.danger);
        }

        // The buttons: pills.
        const auto button = [&](const funkgui::Rect& r, const char* label, int look) {   // 0 primary, 1 outlined, 2 quiet
            const bool hover = hasPointer_ && r.contains(pointer_);
            funkgui::Col fill = p.card.withAlpha(0.0f), edge = p.card.withAlpha(0.0f), ink = p.soft;
            if (look == 0)
            {
                fill = hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent;
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
            c.text(label, r.centreX(), c.capCentreTop(r.centreY(), st), st, ink, funkgui::Align::centre);
        };
        button(d.save, "Save", 0);
        button(d.cancel, "Cancel", 1);
        if (chat_.hasKey)
            button(d.remove, "Remove key", 2);
    }

    void Panel::openExamples()
    {
        if (host_ == nullptr)
            return;
        funkgui::MenuRequest request;
        int id = 1;
        for (const Example& e : kExamples)
            request.items.push_back({ id++, e.title });
        request.anchor = static_cast<const CellsItem&>(*items_[static_cast<std::size_t>(examplesItem_)]).bounds();
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) {
            if (chosen < 1 || chosen > static_cast<int>(std::size(kExamples)))
                return;
            // The example replaces the program's text as one edit (Cmd-Z brings the text back), and runs; the other
            // files stay.
            files().endRename();
            files().select(0);
            funkgui::TextEditor& program = files().editor();
            program.selectAll();
            program.insert(kExamples[static_cast<std::size_t>(chosen - 1)].source);
            program.setCaret({ 0, 0 });
            program.setScroll(0.0f, 0.0f);
            setFocus(editorItem_);
            runNow();
        });
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
        // The message box follows its text, one row to eight: the conversation gives it the room.
        if (std::fabs(inputItem().desiredHeight(inputItem().box().w, kBarH) - composerH_) > 0.5f)
            layoutChat();
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
        const float contentW = width_ - 2.0f * kMargin;
        const float leftRight = kMargin + leftW_;

        // Header: the name, the kind and version.
        {
            const funkgui::Canvas::Scope s(c, kHeaderTextTag, false);
            c.text(title_.c_str(), kMargin, kTitleTop, funkgui::type::kWordmark, th.ink100);
            const float titleRight = kMargin + c.textWidth(title_.c_str(), funkgui::type::kWordmark);
            c.text(subtitle_.c_str(), titleRight + kSubtitleGap,
                   c.sharedBaselineTop(kTitleTop, funkgui::type::kWordmark, funkgui::type::kMicro),
                   funkgui::type::kMicro, th.ink32);
        }
        {
            const funkgui::Canvas::Scope s(c, funkgui::tags::none, false);
            c.hairlineH(kMargin, kHeaderRuleY, contentW, th.ink16);
            c.hairlineH(kMargin, height_ - kFooterH, contentW, th.ink16);
        }

        // The toolbar's status and meter, over the editor.
        {
            const funkgui::Canvas::Scope s(c, kStatusTag, false);
            const float statusX = kMargin + 72.0f + 96.0f + 20.0f;
            const float textTop = c.capCentreTop(kToolbarY + kButtonH * 0.5f, kStatusStyle);
            const float meterW = c.textWidth(meter_.c_str(), kStatusStyle);
            const funkgui::Col statusInk = !lastOk_ && !compiling_ ? th.signal : (edited_ ? th.ink70 : th.ink52);
            c.text(fitText(c, status_, kStatusStyle, std::max(0.0f, leftRight - statusX - meterW - 24.0f)).c_str(),
                   statusX, textTop, kStatusStyle, statusInk);
            const bool alarm = port_.alerts() != 0;
            c.text(meter_.c_str(), leftRight, textTop, kStatusStyle, alarm ? th.signal : th.ink32, funkgui::Align::right);
        }

        // The chat's card (rounded, a shade off the ground: the panel around it is square), its title, and the line under
        // the message box (what Claude is doing, else the last reply's tokens).
        {
            const funkgui::Canvas::Scope s(c, kChatTag, false);
            const chat::Palette p = chat::palette(th);
            c.rrect(card_.x, card_.y, card_.w, card_.h, kCardRadius, p.card, 1.0f, p.cardEdge);
            const float ty = headerY_ + kIconButton * 0.5f;
            const float tx = card_.x + kCardPad + 4.0f;
            c.disc(tx + 4.0f, ty, chat_.busy ? 4.0f : 3.5f, chat_.busy ? p.accent : funkgui::mix(p.card, p.accent, 0.75f));
            c.text("Claude", tx + 15.0f, c.capCentreTop(ty, chat::kTitle), chat::kTitle, p.text);
            const float sub = tx + 15.0f + c.textWidth("Claude", chat::kTitle) + 10.0f;
            const float subRoom = card_.right() - kCardPad - 2.0f * kIconButton - 12.0f - sub;
            const std::string kind = product::kIsInstrument ? "builds and edits your synth" : "builds and edits your effect";
            if (subRoom > 60.0f)
                c.text(fitText(c, kind, chat::kSmall, subRoom).c_str(), sub, c.capCentreTop(ty, chat::kSmall), chat::kSmall, p.faint);
            const funkgui::Rect box = inputItem().box();
            c.text(fitText(c, activity_, chat::kSmall, box.w - 8.0f).c_str(), box.x + 6.0f,
                   c.capCentreTop(usageY_ + kUsageH * 0.5f, chat::kSmall), chat::kSmall, chat_.busy ? p.dim : p.faint);
        }

        for (std::size_t i = 0; i < items_.size(); ++i)
            if (items_[i]->shown())
                items_[i]->draw(c, th, ring_ && focus_ == static_cast<int>(i));

        // Footer: the spec line of the control under the hand, else the focused one, else the last one touched; the
        // dialog's own while it is open.
        char line[256] = {};
        if (keyOpen_)
            std::snprintf(line, sizeof line, "KEY   CMD-V PASTES IT   RETURN SAVES IT   ESCAPE CANCELS");
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
            const float zoomW = items_[static_cast<std::size_t>(zoomItem_)]->shown() ? 44.0f * ZoomModel::kMaxSteps + 12.0f
                                                                                    : 0.0f;
            const std::string shown = fitFields(c, line, contentW - zoomW);
            const funkgui::Canvas::Scope s(c, funkgui::tags::hint, false);
            c.text(shown.c_str(), kMargin, height_ - 24.0f, funkgui::type::kMicro, th.ink52);
        }

        if (keyOpen_)
            drawKeyDialog(c, th);
    }

    // ---- input --------------------------------------------------------------------------------------------------------
    int Panel::itemAt(funkgui::Point p) const
    {
        if (keyOpen_)
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
        const int i = itemAt({ e.x, e.y });
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
        // RUN, from anywhere in the panel.
        if (e.key == funkgui::Key::enter && (e.mods.cmd || e.mods.ctrl))
        {
            runNow();
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
            const bool button = d.save.contains(pointer_) || d.cancel.contains(pointer_)
                             || (chat_.hasKey && d.remove.contains(pointer_));
            return button ? funkgui::Cursor::pointingHand : funkgui::Cursor::normal;
        }
        const int i = pressed_ >= 0 ? pressed_ : (hasPointer_ ? itemAt(pointer_) : -1);
        return i >= 0 ? items_[static_cast<std::size_t>(i)]->cursorAt(pointer_) : funkgui::Cursor::normal;
    }

    // ---- accessibility ------------------------------------------------------------------------------------------------
    namespace
    {
        // The key dialog's ids, past every item's.
        constexpr uint32_t kKeyDialogId = 0x7FFF0000u;
        constexpr uint32_t kKeyFieldId = kKeyDialogId + 1, kKeySaveId = kKeyDialogId + 2,
                           kKeyCancelId = kKeyDialogId + 3, kKeyRemoveId = kKeyDialogId + 4;
    }

    void Panel::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (keyOpen_)
        {
            const KeyDialog d = keyDialog();
            funkgui::A11yItem box;
            box.id = kKeyDialogId;
            box.role = funkgui::a11yrole::group;
            box.bounds = d.box;
            box.title = "Anthropic API key";
            box.description = "Stored encrypted on this Mac, for both playground plugins";
            out.push_back(box);
            funkgui::A11yItem field;
            field.id = kKeyFieldId;
            field.parent = kKeyDialogId;
            field.role = funkgui::a11yrole::editableText;
            field.bounds = d.field;
            field.title = "Key";
            field.value = keyEdit_.buffer.empty() ? "empty" : std::to_string(keyEdit_.buffer.size()) + " characters";
            field.help = keyWhy_.empty() ? "Command V pastes it" : keyWhy_;
            out.push_back(std::move(field));
            const auto button = [&](uint32_t id, const funkgui::Rect& r, const char* title) {
                funkgui::A11yItem b;
                b.id = id;
                b.parent = kKeyDialogId;
                b.role = funkgui::A11yRole::button;
                b.bounds = r;
                b.title = title;
                out.push_back(std::move(b));
            };
            button(kKeySaveId, d.save, "Save");
            button(kKeyCancelId, d.cancel, "Cancel");
            if (chat_.hasKey)
                button(kKeyRemoveId, d.remove, "Remove the stored key");
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
        static_assert(kMaxControls <= 16, "the dials' bits");
        uint32_t r = items_[static_cast<std::size_t>(zoomItem_)]->shown() ? 1u : 0u;
        for (int c = 0; c < kMaxControls; ++c)                // bits 1 to 16
            if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0 && items_[static_cast<std::size_t>(i)]->shown())
                r |= 2u << c;
        r |= (keyOpen_ ? 1u : 0u) << 17;
        r |= (chat_.hasKey ? 1u : 0u) << 18;
        // The counters times odd constants: a change in any one of them changes the revision.
        return r ^ (files().layoutRevision() * 0x85EBCA77u) ^ (layoutRevision_ * 0xC2B2AE3Du) ^ (chatA11y_ * 2654435761u);
    }

    void Panel::a11yAction(uint32_t id, funkgui::A11yAction a, double value)
    {
        if (!gestures_)
            return;
        if (keyOpen_)
        {
            if (id == kKeySaveId)
                saveKeyFromDialog();
            else if (id == kKeyCancelId)
                closeKeyDialog();
            else if (id == kKeyRemoveId && chat_.hasKey)
                removeKeyFromDialog();
            dirty_ = true;
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
    }
}
