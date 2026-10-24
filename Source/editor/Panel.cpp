// Source/editor/Panel.cpp: see Panel.h.
#include "editor/Panel.h"

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

namespace testingplayground::ui
{
    // ---- one control or view on the panel ---------------------------------------------------------------------------
    class Item
    {
    public:
        virtual ~Item() = default;

        virtual void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) = 0;
        virtual void draw(funkgui::Canvas&, const funkgui::Theme&, bool focusRing) const = 0;
        virtual bool contains(funkgui::Point) const = 0;
        virtual funkgui::Cursor cursorAt(funkgui::Point) const { return funkgui::Cursor::normal; }
        virtual void pointerMove(const funkgui::PointerEvent&) {}
        virtual void pointerExit() {}
        virtual void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) = 0;
        virtual void pointerDrag(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual bool wheel(const funkgui::WheelEvent&, funkgui::GestureController&, double /*nowSec*/) { return false; }
        virtual bool key(const funkgui::KeyEvent&, funkgui::GestureController&) { return false; }
        virtual void accessibility(std::vector<funkgui::A11yItem>&) const = 0;
        virtual bool a11yAction(uint32_t id, funkgui::A11yAction, double value, funkgui::GestureController&) = 0;
        virtual void specLine(char* out, std::size_t n) const = 0;
        virtual bool settled() const { return true; }
        virtual funkgui::Point centre() const = 0;
        virtual bool shown() const { return true; }      // false: not drawn, no input, no Tab stop
        virtual bool focusable() const { return true; }  // false: no Tab stop (a readout)
        virtual void focusChanged(bool /*focused*/) {}
    };

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
        constexpr float kWidth = 980.0f;
        constexpr float kTitleTop = 17.0f;
        constexpr float kSubtitleGap = 14.0f;
        constexpr float kThemeW = 128.0f;
        constexpr float kHeaderRuleY = 46.0f;
        constexpr float kToolbarY = 56.0f;
        constexpr float kButtonH = 22.0f;
        constexpr float kTabsY = 88.0f;                  // the files' tabs, over the editor
        constexpr float kEditorY = kTabsY + 26.0f;
        constexpr float kEditorH = 378.0f;
        constexpr float kProblemRowH = 15.0f;
        constexpr float kPlaceW = 150.0f;                // a problem's file, line and column
        constexpr int   kProblemRows = 4;
        constexpr float kDialCellW = 60.0f;              // funkgui::DialGeom, primary: 60 x 80
        constexpr float kDialGap = 30.0f;
        constexpr float kFooterH = 36.0f;
        constexpr uint32_t kA11yStride = 64;             // item i's ids: kA11yStride * (i + 1) + 0..63

        constexpr funkgui::TextStyle kStatusStyle{ 10.0f, 1.4f, 0.03f, false };

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

        // `s` cut to `maxW` px of `style`, with an ellipsis when it is cut.
        std::string fitText(const funkgui::Canvas& c, std::string s, const funkgui::TextStyle& style, float maxW)
        {
            if (c.textWidth(s.c_str(), style) <= maxW)
                return s;
            while (!s.empty() && c.textWidth((s + "...").c_str(), style) > maxW)
                s.pop_back();
            return s + "...";
        }

        // FMinus's words, for the editor's highlighter: C's that FMinus has, and its own (module, param, the vector
        // types). It has no strings, and of the preprocessor only #include and #pragma: a line that starts with # is
        // one preprocessor span.
        funkgui::text::KeywordRules fminusRules()
        {
            funkgui::text::KeywordRules r;
            r.keywords = { "break", "const", "continue", "else", "false", "for", "if", "module", "param", "restrict",
                           "return", "sizeof", "this", "true", "while" };
            r.types = { "bool", "float", "float2", "float4", "float8", "int", "poly", "void" };
            r.lineComment = "//";
            r.blockOpen = "/*";
            r.blockClose = "*/";
            r.quotes = "";
            r.preprocessor = '#';
            return r;
        }

        // ---- a dial over P1..P8, labelled with the running program's parameter -------------------------------------
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
            return { t.right() - kTabPad - kTabCloseW + 4.0f, t.y + 4.0f, kTabCloseW, kTabH - 8.0f };
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
                const float nameW = r.w - 2.0f * kTabPad - (i > 0 ? kTabCloseW : 0.0f);
                if (i == renaming_)
                {
                    // The name being edited: its text, the selection (all of it, until a key), the caret.
                    const std::string& b = edit_.buffer;
                    const float w = c.textWidth(b.c_str(), type::kMicro);
                    if (edit_.allSelected && !b.empty())
                        c.rrect(r.x + kTabPad - 1.0f, r.y + 4.0f, std::min(w, nameW) + 2.0f, r.h - 8.0f, 1.0f,
                                th.accentDim);
                    c.text(fitText(c, b, type::kMicro, nameW).c_str(), r.x + kTabPad, textTop, type::kMicro,
                           why_.empty() ? th.ink100 : th.signal);
                    const std::string head = b.substr(0, static_cast<std::size_t>(std::clamp(
                                                             edit_.caret, 0, static_cast<int>(b.size()))));
                    const float cx = r.x + kTabPad + std::min(c.textWidth(head.c_str(), type::kMicro), nameW);
                    c.rrect(cx, r.y + 4.0f, 1.5f, r.h - 8.0f, 0.0f, th.accent);
                }
                else
                {
                    const funkgui::Col ink = hover ? th.accent : (shown ? th.ink100 : th.ink52);
                    c.text(fitText(c, name(i), type::kMicro, nameW).c_str(), r.x + kTabPad, textTop, type::kMicro,
                           ink);
                }
                if (i > 0)
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
            if (i > 0 && closeRect(i).contains(p))
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

        float tabWidth(int i) const
        {
            const float w = funkgui::text::width(funkgui::FontService::get().atlas(), name(i).c_str(),
                                                 funkgui::type::kMicro);
            return std::clamp(w + 2.0f * kTabPad + (i > 0 ? kTabCloseW : 0.0f), kTabMinW, kTabMaxW);
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

    // ---- construction: the layout -----------------------------------------------------------------------------------
    Panel::Panel(std::span<funkgui::ParamPort* const> ports, EditorPort& port) : port_(port)
    {
        title_ = upper(product::kName);
        subtitle_ = upper(product::kKind) + "  " + product::kVersion + "  FMINUS";
        width_ = kWidth;
        const float contentW = width_ - 2.0f * kMargin;
        const auto idFor = [this] { return kA11yStride * static_cast<uint32_t>(items_.size() + 1); };

        // The toolbar's buttons.
        auto run = std::make_unique<ButtonModel>("RUN", "Run the program", [this] { runNow(); });
        runItem_ = static_cast<int>(items_.size());
        items_.push_back(std::make_unique<CellsItem>(
            std::move(run), std::vector<funkgui::Rect>{ { kMargin, kToolbarY, 64.0f, kButtonH } },
            funkgui::CellStyle::boxed, idFor(),
            [](char* out, std::size_t n) { std::snprintf(out, n, "RUN   COMPILE THE PROGRAM AND PLAY IT (CMD-RETURN)"); }));
        auto examples = std::make_unique<ButtonModel>("EXAMPLES", "Examples", [this] { openExamples(); });
        examplesItem_ = static_cast<int>(items_.size());
        items_.push_back(std::make_unique<CellsItem>(
            std::move(examples), std::vector<funkgui::Rect>{ { kMargin + 72.0f, kToolbarY, 96.0f, kButtonH } },
            funkgui::CellStyle::boxed, idFor(),
            [](char* out, std::size_t n) { std::snprintf(out, n, "EXAMPLES   REPLACE THE TEXT WITH ONE, AND RUN IT"); }));

        // The editor.
        auto filesItem = std::make_unique<FilesItem>(funkgui::Rect{ kMargin, kTabsY, contentW, kTabH },
                                                     funkgui::Rect{ kMargin, kEditorY, contentW, kEditorH }, idFor(),
                                                     problems_, [this](int tab) { closeFile(tab); });
        editorItem_ = static_cast<int>(items_.size());
        items_.push_back(std::move(filesItem));

        // The problems, under it.
        const float problemsY = kEditorY + kEditorH + 8.0f;
        const funkgui::Rect problems{ kMargin, problemsY, contentW, kProblemRowH * static_cast<float>(kProblemRows) };
        problemsItem_ = static_cast<int>(items_.size());
        items_.push_back(std::make_unique<ProblemsItem>(problems, idFor(), problems_,
                                                        [this](const Problem& p) { jumpTo(p); }));

        // The dials, in one row under the problems.
        const float dialsY = problems.bottom() + 16.0f;
        itemOfSlot_.assign(static_cast<std::size_t>(kMaxControls), -1);
        for (int c = 0; c < kMaxControls && c < static_cast<int>(ports.size()); ++c)
        {
            itemOfSlot_[static_cast<std::size_t>(c)] = static_cast<int>(items_.size());
            const float x = kMargin + static_cast<float>(c) * (kDialCellW + kDialGap);
            items_.push_back(std::make_unique<DialItem>(*ports[static_cast<std::size_t>(c)],
                                                        funkgui::DialGeom{ x, dialsY }, idFor()));
        }
        height_ = dialsY + 80.0f + 12.0f + kFooterH;

        // THEME, in the header's right corner.
        const float right = width_ - kMargin;
        auto theme = std::make_unique<funkgui::ThemeCells>(
            std::vector<funkgui::Rect>{ { right - kThemeW, 15.0f, 72.0f, 18.0f },
                                        { right - 54.0f, 15.0f, 54.0f, 18.0f } },
            nullptr, funkgui::Point{}, idFor());
        items_.push_back(std::make_unique<CellsItem>(std::move(theme), [this](char* out, std::size_t n) {
            std::snprintf(out, n, "THEME   %s", funkgui::Theme::name(host_ != nullptr ? host_->themeIndex() : 0));
        }));

        // ZOOM, in the footer's right corner; shown when the host offers zoom steps (the plugin's editor does).
        auto zoom = std::make_unique<ZoomModel>();
        ZoomModel* zoomModel = zoom.get();
        std::vector<funkgui::Rect> zoomCells;
        for (int k = 0; k < ZoomModel::kMaxSteps; ++k)
            zoomCells.push_back({ right - 44.0f * static_cast<float>(ZoomModel::kMaxSteps - k), height_ - 28.0f, 42.0f,
                                  18.0f });
        items_.push_back(std::make_unique<CellsItem>(
            std::move(zoom), std::move(zoomCells), funkgui::CellStyle::text, idFor(),
            [zoomModel](char* out, std::size_t n) {
                const int a = zoomModel->active();
                std::snprintf(out, n, "ZOOM   %s", a >= 0 ? zoomModel->label(a) : "");
            },
            [zoomModel] { return zoomModel->count() > 0; }));
        zoom_ = zoomModel;

        // The processor's files and status.
        files().load(port_.draft());
        seenDraft_ = port_.draftRevision();
        syncedFiles_ = files().revision();
        running_ = port_.running();
        refresh(0.0);
        focus_ = editorItem_;
        setFocus(editorItem_);
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

    void Panel::attach(funkgui::HostServices& host)
    {
        host_ = &host;
        gestures_.emplace(host);
        if (zoom_ != nullptr)
            static_cast<ZoomModel*>(zoom_)->attach(host);
        files().attach(&host);
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
        // The draft, when a host loaded a session while the editor was open.
        bool textChanged = false;
        if (const uint64_t d = port_.draftRevision(); d != seenDraft_)
        {
            seenDraft_ = d;
            files().load(port_.draft());
            syncedFiles_ = files().revision();
            textChanged = true;
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
            for (int c = 0; c < kMaxControls; ++c)
                if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0)
                    static_cast<DialItem&>(*items_[static_cast<std::size_t>(i)])
                        .model()
                        .setControl(cs[static_cast<std::size_t>(c)], controlsRevision_);
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

        // The toolbar's status and meter.
        {
            const funkgui::Canvas::Scope s(c, kStatusTag, false);
            const float statusX = kMargin + 72.0f + 96.0f + 20.0f;
            const float textTop = c.capCentreTop(kToolbarY + kButtonH * 0.5f, kStatusStyle);
            const float meterW = c.textWidth(meter_.c_str(), kStatusStyle);
            const funkgui::Col statusInk = !lastOk_ && !compiling_ ? th.signal : (edited_ ? th.ink70 : th.ink52);
            c.text(fitText(c, status_, kStatusStyle, contentW - 188.0f - meterW - 24.0f).c_str(), statusX, textTop,
                   kStatusStyle, statusInk);
            const bool alarm = port_.alerts() != 0;
            c.text(meter_.c_str(), width_ - kMargin, textTop, kStatusStyle, alarm ? th.signal : th.ink32,
                   funkgui::Align::right);
        }

        for (std::size_t i = 0; i < items_.size(); ++i)
            if (items_[i]->shown())
                items_[i]->draw(c, th, ring_ && focus_ == static_cast<int>(i));

        // Footer: the spec line of the control under the hand, else the focused one, else the last one touched.
        int src = pressed_;
        if (src < 0 && hasPointer_)
            src = itemAt(pointer_);
        if (src < 0)
            src = focus_ >= 0 ? focus_ : touched_;
        if (src >= 0)
        {
            char line[256] = {};
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
        if (pressed_ >= 0 && items_[static_cast<std::size_t>(pressed_)]->focusable())
            setFocus(pressed_);
        else if (pressed_ < 0 && focus_ == editorItem_)
            setFocus(-1);                                // a press beside everything takes the caret away
        if (pressed_ >= 0 && gestures_ && host_ != nullptr)
        {
            touched_ = pressed_;
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
        // RUN, from anywhere in the panel.
        if (e.key == funkgui::Key::enter && (e.mods.cmd || e.mods.ctrl))
        {
            runNow();
            return true;
        }
        // The editor, while it has the caret: every key is its own (Tab indents), but Escape, which leaves it.
        if (focus_ == editorItem_ && files().editor().focused())
        {
            if (gestures_ && items_[static_cast<std::size_t>(editorItem_)]->key(e, *gestures_))
                return true;
            if (e.key == funkgui::Key::escape)
            {
                setFocus(-1);
                return true;
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
        // Items come and go with the program's dials, the host's zoom and the files' tabs.
        uint32_t r = items_.back()->shown() ? 1u : 0u;
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0 && items_[static_cast<std::size_t>(i)]->shown())
                r |= 2u << c;
        return r | (files().layoutRevision() << 12);
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
