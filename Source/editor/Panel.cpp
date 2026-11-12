// Source/editor/Panel.cpp: see Panel.h.
#include "editor/Panel.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/ChatView.h"
#include "editor/CodeAssist.h"
#include "editor/DialLayout.h"
#include "editor/DisplayItem.h"
#include "editor/Dropdown.h"
#include "editor/GuiItems.h"
#include "editor/GuiView.h"
#include "editor/IrView.h"
#include "editor/Looks.h"
#include "editor/ProjectGroups.h"
#include "editor/Items.h"
#include "editor/LogoMark.h"
#include "editor/SchematicView.h"

#include "editor/Theme.h"

#include "plugin/EditorPort.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "fminus/CirDoc.h"
#include "fminus/CirTemplates.h"
#include "fminus/Lang.h"
#include "fminus/LibraryIncludes.h"
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
        // The FOLDER section's rows (Explorer.h): what each does (ExplorerItem::Row::index).
        constexpr int kFolderOn = 0, kFolderCode = 1, kFolderShow = 2, kFolderRun = 3, kFolderConflict = 4;
        // The CLAUDE DESKTOP section's rows: its name, Use with Claude Desktop, the last call, another instance open.
        constexpr int kDesktopName = 0, kDesktopUse = 1, kDesktopLast = 2, kDesktopOther = 3;
        const char* const kDesktopThing = product::kIsInstrument ? "synth" : "effect";

        // When Claude Desktop's last call here was, in words that change at most once a minute (the explorer's rows are
        // made again when they do): "now" while it runs.
        std::string agoText(bool running, double seconds)
        {
            if (running)
                return "now";
            if (seconds < 60.0)
                return "just now";
            if (seconds < 3600.0)
                return std::to_string(static_cast<int>(seconds / 60.0)) + " min ago";
            return std::to_string(static_cast<int>(seconds / 3600.0)) + " h ago";
        }
#if defined(_WIN32)
        constexpr const char* kShowFolder = "Show in Explorer";
#elif defined(__APPLE__)
        constexpr const char* kShowFolder = "Show in Finder";
#else
        constexpr const char* kShowFolder = "Show in File Manager";
#endif
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
        constexpr float kMargin = columns::kMargin;
        // The top bar: the product, the project, the view, the load and the theme. Under it two cards: the program's
        // (its tabs and actions, the editor or the panel, the problems, the dials) and the chat's.
        constexpr float kTopBarY = 10.0f, kTopBarH = 40.0f;
        constexpr float kTopButtonH = 28.0f;             // the top bar's pills
        constexpr float kMainY = kTopBarY + kTopBarH + 10.0f;   // the cards' top
        // The Standalone window's buttons (EditorPort::windowButtons) sit inside the top bar's left end, the product's
        // mark after them and this gap.
        constexpr float kButtonsX = kMargin + 10.0f, kButtonsGap = 12.0f;
        constexpr float kInset = 10.0f;                  // inside the program's card
        constexpr float kActionH = 28.0f;                // its header's first row: the status and the actions
        constexpr float kActionsY = kMainY + 6.0f;
        constexpr float kTabsY = kActionsY + kActionH + 4.0f;   // its second: the files' tabs, over the editor
        constexpr float kEditorY = kTabsY + 26.0f + 6.0f;
        constexpr float kSegmentW = 84.0f;               // a half of the Code / Panel switch
        constexpr float kProjectW = 340.0f;              // the project pill, at most
        constexpr float kProjectLeast = 90.0f;           // ... at least, beside the presets
        constexpr float kPresetGap = 6.0f;               // between the pill and the presets' arrow
        constexpr float kPresetArrowW = 26.0f, kPresetArrowGap = 2.0f;
        constexpr float kPresetNameW = 200.0f, kPresetNameLeast = 64.0f;   // the preset's name between the arrows
        constexpr float kProblemRowH = 15.0f;
        constexpr float kPlaceW = 150.0f;                // a problem's file, line and column
        constexpr int   kProblemRows = 4;
        constexpr float kFooterH = 30.0f;
        constexpr float kBottomGap = 10.0f;              // over the status bar
        // The columns: the explorer at the left, the program's card, the chat at the right (Columns.h: their widths, the
        // edges the user drags, the gaps).
        using columns::kChatGap;
        using columns::kChatRailGap;
        using columns::kExplorerGap;
        constexpr double kRedrawEvery = 1.0;             // s: needsDraw()'s safety net (Panel.h "Unchanged frames")
        // A panel the host no longer ticks (a minimised or hidden window) lets the meters' reader go after this many of
        // its idle calls (10 a second) and this long with no tick (Panel.h "The meters' reader").
        constexpr int kHiddenIdles = 5;
        constexpr double kHiddenAfter = 1.0;             // s
        // The explorer's projects and presets are polled this often while it shows (s), and a poll that fell due while
        // it was away comes this long after it shows again, not on that frame (Panel::syncExplorer).
        constexpr double kProjectsPoll = 3.0;
        constexpr double kPollAfterShow = 0.25;
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
        constexpr float kKeyDialogH = 256.0f;
        constexpr float kRenameDialogH = 196.0f;          // the chat's rename (the key dialog's box, two lines)
        constexpr int   kMaxKeyLength = 320;
        // Another provider's rows (its server's address, its key, its model): their top, their pitch, their labels'
        // column; the Provider pill.
        constexpr float kProviderRowsTop = 124.0f;
        constexpr float kProviderRowPitch = 44.0f;
        constexpr float kProviderLabelW = 64.0f;
        constexpr float kProviderPillW = 104.0f;
        constexpr int   kMaxBaseLength = 300;
        constexpr int   kMaxModelLength = 200;
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

        // MIDI learn on a dial (EditorPort::midi): what its footer line says after its own ("" for nothing); its mark is
        // looks::midiMark's.
        std::string midiSpec(const std::string& words, bool learning, const std::string& note)
        {
            if (learning)
                return "   LEARNING: MOVE A CONTROL ON A MIDI CONTROLLER" + (note.empty() ? std::string() : "   " + upper(note));
            return words.empty() ? std::string() : "   MIDI " + upper(words);
        }
        void appendSpec(char* out, std::size_t n, const std::string& more)
        {
            const std::size_t len = std::strlen(out);
            if (!more.empty() && len + 1 < n)
                std::snprintf(out + len, n - len, "%s", more.c_str());
        }

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

            // What a right-click (or a ctrl-click, or a screen reader's menu action) opens at a point: the Panel's dial
            // menu (MIDI learn's, and the host's). None: the host's own menu, as FunkGui's Dial opens it.
            void setMenu(std::function<void(funkgui::Point)> menu) { menu_ = std::move(menu); }
            // MIDI learn (EditorPort::midi): the control that moves it (in words, "" for none), whether it learns, and
            // learning's note.
            void setMidi(const std::string& words, bool learning, const std::string& note)
            {
                midiWords_ = words;
                learning_ = learning;
                midiNote_ = note;
            }
            bool midiMarked() const noexcept { return !midiWords_.empty(); }
            bool midiLearning() const noexcept { return learning_; }

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
                drawDial(c, th, ring);
                looks::midiMark(c, th, looks::midiMarkAt(cell_), !midiWords_.empty(), learning_);
            }
            void drawDial(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const
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
                if (e.popup && menu_)
                {
                    menu_({ e.x, e.y });                 // a menu, never a write
                    return;
                }
                if (e.popup && style_ != DialStyle::knob && style_ != DialStyle::vintage)
                {
                    if (funkgui::ParamPort* p = model_.port())
                        h.showParamMenu(*p, e.x, e.y);   // the host's menu, never a write
                    return;
                }
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
                if (a == funkgui::A11yAction::showMenu && menu_)
                    menu_({ cell_.centreX(), cell_.centreY() });
                else
                    dial_.a11yAction(a, v, g);
                return true;
            }
            void specLine(char* out, std::size_t n) const override
            {
                dial_.specLine(out, n);
                appendSpec(out, n, midiSpec(midiWords_, learning_, midiNote_));
            }
            bool settled() const override { return dial_.settled(); }
            // The parameter's value as the host has it (automation, Claude, a preset move it with no input here) and
            // the program's control (ControlModel::key), the look and the hover.
            uint64_t drawKey() const override
            {
                return DrawKey{}.mix(model_.key()).mix(static_cast<int>(style_)).mix(color_).mix(cell_).mix(hovered_).mix(hoverRow_)
                    .mix(midiWords_).mix(learning_).mix(midiNote_).h;
            }
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

            std::function<void(funkgui::Point)> menu_;
            std::string midiWords_, midiNote_;
            bool learning_ = false;
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
            uint64_t drawKey() const override            // the cell chosen and those enabled (the zoom's: the display)
            {
                DrawKey k;
                k.mix(sel_->bounds());
                if (model_)
                {
                    k.mix(model_->count()).mix(model_->active());
                    for (int i = 0; i < model_->count(); ++i)
                        k.mix(model_->enabled(i)).mix(model_->label(i));
                }
                return k.h;
            }
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
        constexpr float kTabNarrowW = 26.0f;             // the least a tab narrows to when many do not fit (kMaxFiles,
                                                         // sixteen, fit a program's card of kProgramMinW, the IR tab
                                                         // open and a netlist's pills too: it takes no more than they
                                                         // leave); a letter of its name, and its close box beside it
                                                         // while it shows or is under the pointer
        constexpr float kTabMaxW = 150.0f;
        constexpr float kTabGap = 4.0f;
        constexpr float kNewTabW = 24.0f;
        constexpr uint64_t kStructureStep = uint64_t{ 1 } << 40;   // FilesItem::revision: a change of the tabs
        constexpr float kIrIconW = 16.0f;                // the IR tab's icon, before its name
        // A .cir tab's Netlist / Schematic pills, at the bar's right end (left of the IR tab): icon-only when their
        // labels would narrow the files' tabs below kTabNarrowW.
        constexpr float kPillIconW = 28.0f;
        constexpr float kPillTrack = 3.0f;               // the track around the pair
        constexpr uint32_t kPillsA11y = 40;              // FilesItem's ids: the group, then Netlist and Schematic
        constexpr uint32_t kPanelSwitchA11y = 44;        // ... the status line's Panel switch
    }

    // Not in the unnamed namespace: Panel.h names it.
    class FilesItem final : public Item
    {
    public:
        using Close = std::function<void(int tab)>;  // a closable tab's close box: the Panel asks, then remove()

        FilesItem(funkgui::Rect bar, funkgui::Rect editor, uint32_t id, const std::vector<Problem>& problems,
                  Close close)
            : bar_(bar), editorBounds_(editor), id_(id), problems_(&problems), close_(std::move(close)),
              // The IR tab (IrView.h) is the one past the files while it is shown: none of their rules reach it.
              assist_(CodeAssist::Tabs{
                  [this] { return count(); }, [this] { return irShown() ? count() : active(); },
                  [this](int i) -> funkgui::TextEditor& { return i >= count() && irShown() ? this->editor() : editorOf(i); },
                  [this](int i) { return i >= count() ? irName() : name(i); },
                  [this](int i) { select(i); },
                  [this](const std::string& n, const std::string& text) { return addFileQuiet(n, text); },
                  [this](const std::string& text) {
                      if (notice_)
                          notice_(text);
                  } }),
              schematic_(SchematicView::Hooks{
                  [this](int line, int col) { showNetlistAt(line, col); },
                  [this] { return assist_.diagnosticsOf(active_); },
                  [this] { return played(); },
                  [this] {
                      if (persist_)
                          persist_();
                  },
                  [this](bool redo) { return undoStep(redo); } })
        {
            assist_.setArea(editor);
            assist_.setCodeSize(size_);
            assist_.setSizeHook([this](int tenths) { setCodeSize(tenths); });
            assist_.setStatusWords([this] { return schematicShown() ? schematic_.statusWords() : std::string(); });
            assist_.setStatusProblems([this](bool back) { schematic_.stepProblem(back ? -1 : 1); });
            schematic_.setArea(assist_.editorRect());
            load(Draft{ singleFile(""), 0 });
        }

        CodeAssist& assist() noexcept { return assist_; }
        const CodeAssist& assist() const noexcept { return assist_; }
        void setNotice(std::function<void(const std::string&)> notice) { notice_ = std::move(notice); }
        // The Panel that holds it: its problems (the tabs' marks) and a tab's close box (the code window takes the item
        // from one Panel to another: Panel::lendEditor).
        void setPanelHooks(const std::vector<Problem>& problems, Close close)
        {
            problems_ = &problems;
            close_ = std::move(close);
        }
        // ... and its menuOpenAt, which the Schematic view's triggers (More, the probe chip) draw open by.
        void setMenuOpen(std::function<bool(const funkgui::Rect&)> open) { schematic_.setMenuOpen(std::move(open)); }
        // The Schematic view's circuit dials and Play this circuit (SchematicView.h): the program that plays's circuit
        // fields, a dial's port and control, and RUN (the Panel's).
        void setCircuitHooks(std::function<const std::vector<lang::CircuitBinding>&()> bindings,
                             std::function<funkgui::ParamPort*(int)> port, std::function<Control(int)> control,
                             std::function<void()> run)
        {
            run_ = std::move(run);
            SchematicView::DialHooks h;
            h.bindings = std::move(bindings);
            h.port = std::move(port);
            h.control = std::move(control);
            h.play = [this] { return playCircuit(); };
            h.playedNote = [this] { return playedNote(); };
            h.undoPlay = [this] { undoPlay(); };
            schematic_.setDialHooks(std::move(h));
        }

        // Play this circuit: program.fm's text (the first file's) replaced by a host program of the product's kind for
        // the netlist shown (fminus/CirTemplates.h), as one undoable edit, the library files it #includes added as tabs
        // first (as EXAMPLES adds them), then RUN; the view stays on the netlist. Why not ("" when it played).
        std::string playCircuit()
        {
            if (irShown() || active_ <= 0 || fileKindOf(name(active_)) != FileKind::Circuit)
                return "Play this circuit plays a netlist that is not the program's first file";
            const std::string file = name(active_);
            const cir::HostProgram hp = cir::hostProgram(file, editorOf(active_).text(), product::kIsInstrument);
            if (!hp.why.empty())
                return hp.why;
            const Sources need = withLibraryIncludes(singleFile(hp.text));
            std::string names;
            int missing = 0;
            for (std::size_t k = 1; k < need.size(); ++k)
                if (find(need[k].name) < 0)
                {
                    ++missing;
                    names += (names.empty() ? "" : " and ") + need[k].name;
                }
            if (count() + missing > static_cast<int>(kMaxFiles))
                return "Play this circuit needs room for " + names + ": close a tab";
            endRename();
            for (std::size_t k = 1; k < need.size(); ++k)
                if (find(need[k].name) < 0)
                    addFileQuiet(need[k].name, need[k].text);
            (void)replaceCounted(tabs_.front(), hp.text);
            playFile_ = file;
            playProgram_ = name(0);
            if (run_)
                run_();
            playRev_ = editorOf(0).revision();           // after RUN (Format on Run leaves a template as it is)
            playOn_ = true;
            return {};
        }
        // "program.fm now plays pedal.cir" while program.fm's text is what Play wrote and that netlist shows; "" else.
        std::string playedNote() const
        {
            if (!playOn_ || tabs_.empty() || irShown() || tabs_.front().editor->revision() != playRev_ || name(0) != playProgram_
                || name(active_) != playFile_)
                return {};
            return name(0) + " now plays " + playFile_;
        }
        // Its Undo: program.fm's own undo step (Cmd-Z in its tab does the same).
        void undoPlay()
        {
            if (playedNote().empty())
                return;
            (void)ownStep(0, false);
            playOn_ = false;
        }
        // The completion list, the signature and the hover card, over everything (the Panel draws them last).
        void drawOverlays(funkgui::Canvas& c, const funkgui::Theme& th, float w, float h) const
        {
            if (!hidden_)
                assist_.drawOverlays(c, th, w, h);
        }

        // The editor shown: the file's, or the IR view's while the IR tab is shown.
        funkgui::TextEditor& editor() noexcept
        {
            return irShown() ? *ir_.editors[static_cast<std::size_t>(ir_.view)] : *tabs_[static_cast<std::size_t>(active_)].editor;
        }
        const funkgui::TextEditor& editor() const noexcept
        {
            return irShown() ? *ir_.editors[static_cast<std::size_t>(ir_.view)] : *tabs_[static_cast<std::size_t>(active_)].editor;
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
            groups_.clear();
            for (const SourceFile& f : d.files)
                if (tabs_.size() < kMaxFiles)
                    tabs_.push_back(makeTab(f.name, f.text));
            if (tabs_.empty())
                tabs_.push_back(makeTab(kProgramName, ""));
            active_ = std::clamp(d.active, 0, count() - 1);
            show();
            structure_ += kStructureStep;
            syncSchematic();
        }

        // The processor's draft into the tabs (a host loaded a session, or Claude changed the files): a file whose tab
        // is open keeps its editor (its caret, scroll and undo steps), its text replaced as one undoable edit when it
        // changed; a file at the place of a tab whose name is gone is that file renamed; a new file gets a tab, and a
        // tab whose file is gone closes. The tabs follow the draft's order and the tab it shows. Fresh (an example or
        // RANDOM's program: EditorPort::startAfresh): a tab is kept only for a file of its name, its text set anew (no
        // undo steps, the caret and the scroll at the top), as a new window has it; every other file gets a new tab.
        // When it changes several tabs' texts (not fresh), their steps are one undo step across them (groups_).
        void merge(const Draft& d, bool fresh = false)
        {
            recount();                                   // what was typed before it takes those tabs out of the groups
            UndoGroup changed;
            // a name being typed (a new tab's) goes on when its tab stays under that name (the Panel's merge keeps a tab
            // added since the editor last gave its files)
            const std::string naming = renaming_ >= 0 && renaming_ < count() && !fresh ? name(renaming_) : std::string();
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
            for (std::size_t i = 0; i < n && !fresh; ++i)
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
                if (fileKindOf(t.name) != fileKindOf(f.name))   // renamed to another kind (a .gui file, a netlist)
                    assist_.configure(*t.editor, f.name);
                if (t.name != f.name)
                    renameView(t.name, f.name);
                t.name = f.name;
                if (fresh)
                {
                    t.editor->setText(f.text);
                    t.editor->setScroll(0.0f, 0.0f);
                }
                else
                {
                    // A netlist the draft changed: the schematic flashes what moved (Claude's write, merged)
                    if (const auto v = views_.find(f.name);
                        v != views_.end() && t.editor->text() != funkgui::TextEditor::sanitise(f.text))
                        v->second.merged = true;
                    if (replaceCounted(t, f.text))
                        changed.members.push_back({ t.serial, t.step });
                }
                tabs_.push_back(std::move(t));
            }
            if (tabs_.empty())
                tabs_.push_back(makeTab(kProgramName, ""));
            if (fresh)
                groups_.clear();                         // every tab's text set anew: no undo steps
            else if (changed.members.size() > 1)
            {
                groups_.push_back(std::move(changed));
                if (groups_.size() > kMaxUndoGroups)
                    groups_.erase(groups_.begin());
            }
            for (auto v = views_.begin(); v != views_.end();)   // a file gone takes its view (and its pins) with it
                v = find(v->first) < 0 ? forgetView(v) : std::next(v);
            active_ = std::clamp(d.active, 0, count() - 1);
            if (!naming.empty())
                renaming_ = find(naming);                // -1 when it went: the edit ends
            show();
            structure_ += kStructureStep;
            syncSchematic();
        }

        funkgui::Rect editorArea() const noexcept { return editorBounds_; }
        // Where the tabs and the editors are (the Panel's layout).
        void setGeometry(funkgui::Rect bar, funkgui::Rect editorBounds)
        {
            bar_ = bar;
            editorBounds_ = editorBounds;
            assist_.setArea(editorBounds_);
            schematic_.setArea(assist_.editorRect());
            for (Tab& t : tabs_)
                t.editor->setBounds(assist_.editorRect());
            for (const std::unique_ptr<funkgui::TextEditor>& e : ir_.editors)
                if (e)
                    e->setBounds(assist_.editorRect());
        }

        void attach(funkgui::HostServices* host)
        {
            host_ = host;
            assist_.attach(host);
            schematic_.attach(host);
            for (Tab& t : tabs_)
                t.editor->attach(host);
            for (const std::unique_ptr<funkgui::TextEditor>& e : ir_.editors)
                if (e)
                    e->attach(host);
        }

        // The code's colours follow the theme: the files' (CodeAssist::setTheme) and the IR views'.
        void setTheme(const funkgui::Theme& th)
        {
            assist_.setTheme(th);
            const bool dark = th.ground.r < 128;
            if (irThemed_ && dark == irDark_)
                return;
            irThemed_ = true;
            irDark_ = dark;
            irTheme_ = th;
            for (const std::unique_ptr<funkgui::TextEditor>& e : ir_.editors)
                if (e)
                    e->setPalette(codePalette(th));
        }

        // The text size (CodeAssist.h's codesize, this machine's preference) on every file tab and IR view, each keeping
        // its place: the line at its top before stays at its top (setStyle clamps the scroll in px), the caret where it
        // was. The code window holds this same item, so it follows too.
        void setCodeSize(int tenths)
        {
            tenths = std::clamp(tenths, codesize::kLeast, codesize::kMost);
            if (tenths == size_)
                return;
            const float ratio = static_cast<float>(tenths) / static_cast<float>(size_);
            size_ = tenths;
            assist_.setCodeSize(tenths);
            const funkgui::TextStyle st = codesize::style(tenths);
            const auto restyle = [&](funkgui::TextEditor& e) {
                const int top = e.visibleLineRange().first;
                const float x = e.scroll().x * ratio;
                e.setStyle(st);
                e.setScroll(x, static_cast<float>(e.rowOf({ top, 0 })) * e.rowHeight());
            };
            for (Tab& t : tabs_)
                restyle(*t.editor);
            for (const std::unique_ptr<funkgui::TextEditor>& e : ir_.editors)
                if (e)
                    restyle(*e);
        }
        int codeSize() const noexcept { return size_; }

        // File i's tab shown (from the IR tab too).
        void select(int i)
        {
            if (i < 0 || i >= count() || (i == active_ && !irShown()))
                return;
            schematic_.endField();                       // a field open in the drawing written first, as a press out
            editor().setFocused(false);
            ir_.shown = false;
            active_ = i;
            show();
            assist_.tabShown();
            structure_ += kStructureStep;
            syncSchematic();
        }

        // ---- the IR tab (IrView.h): what FMinus made of the program, read only, never one of its files -----------
        bool irOpen() const noexcept { return ir_.open; }
        bool irShown() const noexcept { return ir_.open && ir_.shown; }
        int irView() const noexcept { return ir_.view; }
        std::string irName() const { return irview::name(static_cast<irview::View>(ir_.view), name(0)); }
        // The IR tab at `view`, shown (opened when it was not); the view's editor is made the first time it shows.
        void openIr(int view)
        {
            view = std::clamp(view, 0, irview::kViews - 1);
            if (irShown() && ir_.view == view)
                return;
            endRename();
            schematic_.endField();
            editor().setFocused(false);
            ir_.open = true;
            ir_.shown = true;
            ir_.view = view;
            std::unique_ptr<funkgui::TextEditor>& e = ir_.editors[static_cast<std::size_t>(view)];
            if (!e)
            {
                e = std::make_unique<funkgui::TextEditor>(id_);
                irview::configure(*e, irTheme_, codesize::style(size_));
                e->setBounds(assist_.editorRect());
                e->attach(host_);
            }
            show();
            assist_.tabShown();
            structure_ += kStructureStep;
            syncSchematic();
        }
        // The IR tab closed: its views and their texts go, and the file under it shows again.
        void closeIr()
        {
            if (!ir_.open)
                return;
            editor().setFocused(false);
            ir_ = IrTab{};
            show();
            assist_.tabShown();
            structure_ += kStructureStep;
            syncSchematic();
        }
        // What a view's text was made from ("" for nothing yet).
        const std::string& irMade(int view) const
        {
            static const std::string none;
            return view >= 0 && view < irview::kViews ? ir_.made[static_cast<std::size_t>(view)] : none;
        }
        // A view's editor, while the tab is open and the view has shown (else null).
        funkgui::TextEditor* irEditor(int view)
        {
            return ir_.open && view >= 0 && view < irview::kViews ? ir_.editors[static_cast<std::size_t>(view)].get() : nullptr;
        }
        // A view's text, made from `key` (what it shows: the Panel's) with LLVM's text `from` starting on line `body`,
        // unless it is already: the selection (the caret and its anchor) and the scroll stay where they were in LLVM's
        // text, moved by as many lines as the comment lines above it grew or shrank. False when nothing changed.
        bool setIrText(int view, const std::string& key, const std::string& text, std::shared_ptr<const IrText> from, int body)
        {
            funkgui::TextEditor* e = irEditor(view);
            const std::size_t v = static_cast<std::size_t>(view);
            if (e == nullptr || ir_.made[v] == key)
                return false;
            const bool had = !ir_.made[v].empty();
            const int moved = body - ir_.body[v];
            const auto follow = [&](funkgui::TextPos p) {
                if (p.line >= ir_.body[v])
                    p.line = std::max(0, p.line + moved);
                return p;
            };
            const funkgui::TextPos caret = follow(e->caret()), anchor = follow(e->anchor());
            const funkgui::Point scroll = e->scroll();
            const int top = static_cast<int>(scroll.y / std::max(1.0f, e->rowHeight()));
            e->setText(text);
            if (had)
            {
                e->select(anchor, caret);
                e->setScroll(scroll.x, top >= ir_.body[v] ? scroll.y + static_cast<float>(moved) * e->rowHeight() : scroll.y);
            }
            ir_.made[v] = key;
            ir_.from[v] = std::move(from);
            ir_.body[v] = body;
            return true;
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
            if (const auto v = views_.find(name(i)); v != views_.end())
                forgetView(v);
            tabs_.erase(tabs_.begin() + i);
            if (active_ >= i)
                active_ = std::max(0, active_ - 1);
            show();
            editor().setFocused(focused && !schematicShown());
            structure_ += kStructureStep;
            syncSchematic();
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
                if (fileKindOf(tab.name) != fileKindOf(typed))
                    assist_.configure(*tab.editor, typed);
                renameView(tab.name, typed);
                tab.name = typed;
                structure_ += kStructureStep;
                renaming_ = -1;
                why_.clear();
                syncSchematic();
                return true;
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
            const float right = t.right() - tabPad(i) + 4.0f;
            // A narrowed tab's box keeps clear of its middle, so that a click there shows its file (at kTabNarrowW).
            const float x = narrow(i) ? std::max(right - kTabCloseW, t.centreX() + 1.0f) : right - kTabCloseW;
            return { x, t.y + 4.0f, right - x, kTabH - 8.0f };
        }
        // A tab narrowed to fit: less padding, and its close box only while it is shown (not under the IR tab) or under
        // the pointer.
        bool narrow(int i) const { return tabWidth(i) < naturalWidth(i) - 0.5f; }
        float tabPad(int i) const { return narrow(i) ? 6.0f : kTabPad; }
        bool closeShown(int i) const
        {
            return i > 0 && (!narrow(i) || (i == active_ && !irShown()) || tabRect(i).contains(pointer_));
        }
        funkgui::Rect newTabRect() const
        {
            const funkgui::Rect last = tabRect(count() - 1);
            return { last.right() + kTabGap, bar_.y, kNewTabW, kTabH };
        }
        bool canAdd() const noexcept { return tabs_.size() < kMaxFiles; }
        // The IR tab, apart from the files' at the bar's right end: its icon, its name, its close box. As wide as its
        // name (up to 40% of the bar), but never wider than the files' tabs leave at their narrowest (kTabNarrowW each,
        // and the new-file box), so that they never run under it: its name is cut first.
        float irTabWidth() const
        {
            if (!ir_.open)
                return 0.0f;
            const float w = funkgui::text::width(funkgui::FontService::get().atlas(), irName().c_str(), chat::kSmall);
            const float natural = std::clamp(w + 2.0f * kTabPad + kIrIconW + kTabCloseW + 2.0f, kTabMinW,
                                             std::max(kTabMinW, bar_.w * 0.4f));
            const int n = count();
            const float files = kTabNarrowW * static_cast<float>(n) + kTabGap * static_cast<float>(n - 1)
                              + (canAdd() ? kNewTabW + kTabGap : 0.0f) + 3.0f * kTabGap + pillsReserve();
            // Beside a netlist's pills it narrows further, to kTabNarrowW (its icon; its box under the pointer).
            return std::max(pillsShown() ? kTabNarrowW : kTabMinW, std::min(natural, bar_.w - files));
        }
        bool irNarrow() const { return irTabWidth() < kTabMinW - 0.5f; }
        funkgui::Rect irTabRect() const
        {
            const float w = irTabWidth();
            return { bar_.right() - w, bar_.y, w, kTabH };
        }
        funkgui::Rect irCloseRect() const
        {
            const funkgui::Rect t = irTabRect();
            return { t.right() - kTabPad - kTabCloseW + 4.0f, t.y + 4.0f, kTabCloseW, kTabH - 8.0f };
        }

        // ---- Item --------------------------------------------------------------------------------------------
        void tick(float dt, bool, bool, funkgui::Point pointer) override
        {
            pointer_ = pointer;
            if (schematicShown())
            {
                // the drawing, not the text: no hover card for a word under the canvas; hidden (the PANEL view), the
                // drawing waits (no edge auto-pan under the dials, nothing written to the session)
                if (!hidden_)
                    schematic_.tick(dt, pointer, focused_);
                assist_.tick(dt, { -1.0e4f, -1.0e4f }, focused_);
                return;
            }
            editor().tick(dt);
            assist_.tick(dt, pointer, focused_);
        }
        bool settled() const override { return assist_.settled() && (hidden_ || schematic_.settled()); }
        // The tabs, the editor shown (its text, caret, selection, scroll, squiggles), the code editor's own drawing and
        // the Schematic view's, the name being edited: what the tab bar and the editor draw (Item::drawKey).
        uint64_t drawKey() const override
        {
            DrawKey k;
            k.mix(revision()).mix(active_).mix(tabs_.size()).mix(problems_->size()).mix(pointer_).mix(focused_).mix(hidden_);
            k.mix(ir_.open).mix(ir_.shown).mix(ir_.view).mix(renaming_).mix(edit_.buffer).mix(edit_.caret).mix(edit_.allSelected)
                .mix(why_).mix(playOn_).mix(bar_).mix(editorBounds_).mix(size_);
            k.mix(editor()).mix(assist_.drawKey());
            if (schematicShown())
                k.mix(schematic_.drawKey());
            return k.h;
        }

        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override
        {
            namespace type = funkgui::type;
            const funkgui::TextEditor& ed = editor();
            assist_.drawGround(c, th);
            if (schematicShown())
                schematic_.draw(c, th, focused_);
            else
                ed.draw(c, th, ring);
            assist_.drawInEditor(c, th);

            // The tabs: pills, the shown one raised, a file the last run's errors name marked before its name.
            const funkgui::Canvas::Scope s(c, kTabTag, false);
            const chat::Palette p = chat::palette(th);
            const funkgui::TextStyle& st = chat::kSmall;
            for (int i = 0; i < count(); ++i)
            {
                const funkgui::Rect r = tabRect(i);
                const bool shown = i == active_ && !irShown();   // the IR tab's over it
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
            // The IR tab: outlined (a view, not a file), the chip before its name, its close box always there.
            if (ir_.open)
            {
                const funkgui::Rect r = irTabRect();
                const bool shown = irShown();
                const bool hover = r.contains(pointer_);
                if (shown)
                    c.rrect(r.x, r.y, r.w, r.h, 8.0f, p.composer, 1.0f, p.composerEdge);
                else
                    c.rrect(r.x, r.y, r.w, r.h, 8.0f, hover ? p.hover : p.composer.withAlpha(0.0f), 1.0f, p.cardEdge);
                if (irNarrow())
                {
                    // beside a netlist's pills (only while the IR is not shown): its chip alone, a click anywhere on
                    // it opening it; it closes once shown, its tab wide again
                    chat::drawIcon(c, chat::Icon::chip, r.centreX(), r.centreY(), 11.0f, hover ? p.text : p.dim);
                }
                else
                {
                    chat::drawIcon(c, chat::Icon::chip, r.x + kTabPad + 4.0f, r.centreY(), 11.0f, shown ? p.accent : p.dim);
                    const float nameX = r.x + kTabPad + kIrIconW;
                    const float nameW = irCloseRect().x - 4.0f - nameX;
                    c.text(fitText(c, irName(), st, nameW).c_str(), nameX, c.capCentreTop(r.centreY(), st), st,
                           shown || hover ? p.text : p.soft);
                    drawCross(c, irCloseRect(), 3.0f, irCloseRect().contains(pointer_) ? p.text : p.faint);
                }
            }
            // A netlist's views: Netlist and Schematic, styled as the top bar's Code / Panel switch.
            if (pillsShown())
            {
                const bool labelled = pillsLabelled();
                const funkgui::Rect a = pillRect(0), b = pillRect(1);
                c.rrect(a.x - kPillTrack, a.y - kPillTrack, b.right() - a.x + 2.0f * kPillTrack, a.h + 2.0f * kPillTrack,
                        0.5f * (a.h + 2.0f * kPillTrack), p.raised, 1.0f, p.cardEdge);
                for (int k = 0; k < 2; ++k)
                {
                    const funkgui::Rect r = pillRect(k);
                    const bool on = (k == 1) == schematicShown();
                    const bool hover = r.contains(pointer_);
                    if (on)
                        c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, p.composer, 1.0f, p.composerEdge);
                    else if (hover)
                        c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, p.hover);
                    const funkgui::Col ink = on || hover ? p.text : p.soft;
                    const chat::Icon icon = k == 0 ? chat::Icon::code : chat::Icon::circuit;
                    if (!labelled)
                    {
                        chat::drawIcon(c, icon, r.centreX(), r.centreY(), 12.0f, ink);
                        continue;
                    }
                    chat::drawIcon(c, icon, r.x + 12.0f + 6.0f, r.centreY(), 12.0f, ink);
                    c.text(k == 0 ? "Netlist" : "Schematic", r.x + 30.0f, c.capCentreTop(r.centreY(), st), st, ink);
                }
            }
        }

        bool contains(funkgui::Point p) const override { return bar_.contains(p) || editor().contains(p) || overlayAt(p); }
        // Over what the code editor draws over everything (the completion list, the find and go-to bars, the status
        // line): the window bounds the list, not the program's card, so it may lie over a column's edge.
        bool overlayAt(funkgui::Point p) const { return !hidden_ && assist_.over(p); }
        funkgui::Cursor cursorAt(funkgui::Point p) const override
        {
            if (schematicShown() && !bar_.contains(p) && !assist_.statusRect().contains(p))
                return schematic_.cursorAt(p);
            if (const std::optional<funkgui::Cursor> own = assist_.cursorAt(p))
                return *own;
            if (bar_.contains(p))
                return tabAt(p) >= 0 || (canAdd() && newTabRect().contains(p)) || (ir_.open && irTabRect().contains(p))
                               || (pillsShown() && (pillRect(0).contains(p) || pillRect(1).contains(p)))
                           ? funkgui::Cursor::pointingHand
                           : funkgui::Cursor::normal;
            return editor().cursorAt(p);
        }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g, funkgui::HostServices&) override
        {
            const funkgui::Point p{ e.x, e.y };
            pressedSchematic_ = false;
            schematic_.setGestures(&g);                  // a circuit dial's knob writes through it
            if (!bar_.contains(p))
            {
                endRename();
                if (schematicShown() && !assist_.statusRect().contains(p))
                {
                    pressedEditor_ = false;
                    pressedSchematic_ = true;
                    schematic_.pointerDown(e);
                    return;
                }
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
            if (pillsShown())
                for (int k = 0; k < 2; ++k)
                    if (pillRect(k).contains(p))
                    {
                        endRename();
                        if (!e.popup)
                            setSchematic(k == 1);
                        return;
                    }
            if (ir_.open && irTabRect().contains(p))
            {
                endRename();
                if (irCloseRect().contains(p) && !irNarrow())   // narrow, it has no close box: it opens
                    closeIr();
                else
                    openIr(ir_.view);
                return;
            }
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
        void pointerMove(const funkgui::PointerEvent& e) override
        {
            pointer_ = { e.x, e.y };
            schematic_.pointerMove(pointer_);
        }
        void pointerExit() override
        {
            pointer_ = { -1.0e4f, -1.0e4f };
            schematic_.pointerExit();
        }
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g) override
        {
            schematic_.setGestures(&g);
            if (pressedSchematic_)
                schematic_.pointerDrag(e);
            else if (pressedEditor_)
                editor().pointerDrag(e);
        }
        void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController& g) override
        {
            schematic_.setGestures(&g);
            if (pressedSchematic_)
                schematic_.pointerUp(e);
            else if (pressedEditor_)
                editor().pointerUp(e);
            pressedEditor_ = false;
            pressedSchematic_ = false;
        }
        void doubleClick(const funkgui::PointerEvent& e, funkgui::GestureController& g) override
        {
            const funkgui::Point p{ e.x, e.y };
            schematic_.setGestures(&g);
            if (!bar_.contains(p))
            {
                if (schematicShown())
                {
                    if (!assist_.statusRect().contains(p))
                        schematic_.doubleClick(e);
                    return;
                }
                editor().doubleClick(e);
                return;
            }
            const int i = tabAt(p);
            if (i >= 0 && !(i > 0 && closeRect(i).contains(p)))
                beginRename(i);
        }
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController&, double nowSec) override
        {
            if (bar_.contains({ e.x, e.y }))
                return false;
            if (schematicShown())
                return schematic_.wheel(e, nowSec);
            return assist_.wheel(e) || editor().wheel(e);
        }
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override
        {
            if (renaming_ < 0 && schematicShown())
            {
                recount();
                return schematic_.key(e);                // its undo and redo come back through undoStep
            }
            if (renaming_ < 0)
            {
                // Undo and redo where the text has the keys: a group's step, or the tab's own through the code assist (it
                // follows the caret), counted.
                recount();
                const int undo = undoKey(e);
                if (undo == 0 || irShown() || assist_.takesKeys() || !editor().focused())
                    return assist_.key(e);
                if (const UndoGroup* g = groupAt(undo > 0))
                {
                    groupStep(*g, undo > 0);
                    return true;
                }
                Tab& t = tabs_[static_cast<std::size_t>(active_)];
                const uint32_t was = t.editor->revision();
                const bool used = assist_.key(e);
                if (t.editor->revision() != was)
                {
                    t.step += undo;
                    t.counted = t.editor->revision();
                }
                return used;
            }
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
                tab.checked = i == active_ && !irShown();
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
            if (ir_.open)
            {
                funkgui::A11yItem tab;
                tab.id = id_ + 3 + 2 * static_cast<uint32_t>(kMaxFiles);
                tab.parent = group.id;
                tab.role = funkgui::A11yRole::radioButton;
                tab.bounds = irTabRect();
                tab.title = irName();
                tab.checkable = true;
                tab.checked = irShown();
                tab.help = std::string("the program's LLVM IR, ") + irview::word(static_cast<irview::View>(ir_.view))
                         + ", read only: not one of its files";
                out.push_back(std::move(tab));
                funkgui::A11yItem close;
                close.id = id_ + 4 + 2 * static_cast<uint32_t>(kMaxFiles);
                close.role = funkgui::A11yRole::button;
                close.bounds = irCloseRect();
                close.title = "Close " + irName();
                out.push_back(std::move(close));
            }
            if (pillsShown())
            {
                funkgui::A11yItem views;
                views.id = id_ + kPillsA11y;
                views.role = funkgui::A11yRole::radioGroup;
                views.bounds = { pillRect(0).x, bar_.y, pillRect(1).right() - pillRect(0).x, kTabH };
                views.title = "View of " + name(active_);
                out.push_back(views);
                for (int k = 0; k < 2; ++k)
                {
                    funkgui::A11yItem pill;
                    pill.id = id_ + kPillsA11y + 1 + static_cast<uint32_t>(k);
                    pill.parent = views.id;
                    pill.role = funkgui::A11yRole::radioButton;
                    pill.bounds = pillRect(k);
                    pill.title = k == 0 ? "Netlist" : "Schematic";
                    pill.checkable = true;
                    pill.checked = (k == 1) == schematicShown();
                    pill.help = k == 0 ? "the netlist's text (Command Shift D)" : "its drawing (Command Shift D)";
                    out.push_back(std::move(pill));
                }
            }
            // The status line's Panel switch: the panel under the code, for the tab shown.
            if (assist_.hasPanelSwitch())
            {
                funkgui::A11yItem sw;
                sw.id = id_ + kPanelSwitchA11y;
                sw.role = funkgui::A11yRole::toggleButton;
                sw.bounds = assist_.panelSwitchRect();
                sw.title = "Panel under the code";
                sw.checkable = true;
                sw.checked = assist_.panelSwitchOn();
                sw.help = "the program's dials or interface under the editor (Command J)";
                out.push_back(std::move(sw));
            }
            if (schematicShown())
            {
                schematic_.accessibility(out);
                return;
            }
            funkgui::A11yItem text;
            editor().accessibility(text);
            out.push_back(std::move(text));
        }
        bool a11yAction(uint32_t id, funkgui::A11yAction a, double value, funkgui::GestureController& g) override
        {
            const uint32_t files = static_cast<uint32_t>(kMaxFiles);
            if (id == editor().a11yId() || id == id_ + 1)
                return true;
            if (pillsShown() && (id == id_ + kPillsA11y + 1 || id == id_ + kPillsA11y + 2))
            {
                setSchematic(id == id_ + kPillsA11y + 2);
                return true;
            }
            if (id == id_ + kPillsA11y)
                return true;
            if (id == id_ + kPanelSwitchA11y && assist_.hasPanelSwitch())
            {
                assist_.togglePanelSwitch();
                return true;
            }
            if (schematicShown())
            {
                schematic_.setGestures(&g);              // a circuit dial's item writes through it
                if (schematic_.a11yAction(id, a, value))
                    return true;
            }
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
            if (id == id_ + 3 + 2 * files && ir_.open)
            {
                openIr(ir_.view);
                return true;
            }
            if (id == id_ + 4 + 2 * files && ir_.open)
            {
                closeIr();
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
            if (pillsShown() && (pillRect(0).contains(pointer_) || pillRect(1).contains(pointer_)))
            {
                std::snprintf(out, n, "%s   %s   CMD-SHIFT-D SWITCHES THE VIEW", upper(name(active_)).c_str(),
                              pillRect(0).contains(pointer_) ? "NETLIST: THE TEXT" : "SCHEMATIC: ITS DRAWING, WHICH EDITS THE TEXT");
                return;
            }
            if (bar_.contains(pointer_))
            {
                if (ir_.open && irTabRect().contains(pointer_))
                    std::snprintf(out, n, "%s   THE PROGRAM'S LLVM IR, %s   READ ONLY: NOT ONE OF ITS FILES   X CLOSES IT",
                                  upper(irName()).c_str(), upper(irview::word(static_cast<irview::View>(ir_.view))).c_str());
                else if (canAdd() && newTabRect().contains(pointer_))
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
            if (schematicShown())
            {
                if (!schematic_.specLine(out, n, pointer_))
                    std::snprintf(out, n, "%s   SCHEMATIC   CMD-SHIFT-D SHOWS THE NETLIST   CMD-RETURN RUNS THE PROGRAM",
                                  upper(name(active_)).c_str());
                return;
            }
            const funkgui::TextPos p = editor().caret();
            const std::string sel = editor().selectedText();
            const std::string file = upper(irShown() ? irName() : name(active_));
            if (irShown())
                std::snprintf(out, n, "%s   LINE %d   COLUMN %d   READ ONLY   CMD-F FINDS   CTRL-G GOES TO A LINE", file.c_str(),
                              p.line + 1, p.col + 1);
            else if (sel.empty())
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
            editor().setFocused(focused && !assist_.takesKeys() && !schematicShown());
            schematic_.focusChanged(focused);
            if (!focused)
                endRename();
        }
        // The text field's keys; in the Schematic view the canvas takes every key while the item is focused (Escape,
        // when it has nothing left to end, leaves it).
        bool takesAllKeys() const override
        {
            if (schematicShown() && renaming_ < 0)
                return focused_;
            return editor().focused() || renaming_ >= 0 || assist_.takesKeys();
        }

        // ---- a netlist's two views: Netlist (the text) and Schematic (SchematicView.h) -----------------------------
        // A .cir tab is shown in the CODE view (the IR tab not shown): its pills sit at the bar's right end.
        bool circuitShown() const { return !irShown() && fileKindOf(name(active_)) == FileKind::Circuit; }
        bool schematicShown() const
        {
            if (!circuitShown())
                return false;
            const auto it = views_.find(name(active_));
            return it != views_.end() && it->second.schematic;
        }
        SchematicView& schematic() noexcept { return schematic_; }
        const SchematicView& schematic() const noexcept { return schematic_; }
        // The shown netlist's view (a user's gesture: the session keeps it). Into the Netlist view the caret goes to the
        // first selected part's line and their lines are marked; into the Schematic view the part on the caret's line
        // is selected.
        void setSchematic(bool on)
        {
            if (!circuitShown())
                return;
            SchematicFile& f = views_[name(active_)];
            funkgui::TextEditor& ed = editorOf(active_);
            if (f.schematic == on)
                return;
            schematic_.endField();                       // the Netlist view: a field open is written first
            if (on)
            {
                f.schematic = true;
                f.stored = true;
                ed.setDecorations({}, funkgui::decorationBit(funkgui::DecorationKind::highlight));
                const int caretLine = ed.caret().line;
                syncSchematic();
                schematic_.selectAtLine(caretLine, false);
            }
            else
            {
                std::vector<funkgui::Decoration> marks;
                int first = -1;
                if (schematic_.showing())
                {
                    const cir::Netlist& nl = schematic_.netlist();
                    const cir::Drawing& d = schematic_.drawing();
                    for (const std::string& s : schematic_.selection())
                        if (const int i = d.part(s); i >= 0)
                        {
                            const cir::Part& p = d.parts[static_cast<std::size_t>(i)];
                            int a = -1, z = -1;
                            if (p.element >= 0)
                            {
                                a = nl.elements[static_cast<std::size_t>(p.element)].firstLine;
                                z = nl.elements[static_cast<std::size_t>(p.element)].lastLine;
                            }
                            else if (const std::optional<cir::Terminal>& t = p.kind == cir::Kind::Input ? nl.input : nl.output; t)
                                a = z = t->line;
                            if (a < 0)
                                continue;
                            first = first < 0 ? a : std::min(first, a);
                            marks.push_back({ { a, 0 }, { z, static_cast<int>(ed.line(z).size()) }, funkgui::DecorationKind::highlight });
                        }
                }
                f.schematic = false;
                f.stored = true;
                syncSchematic();
                if (first >= 0)
                {
                    ed.setDecorations(std::move(marks), funkgui::decorationBit(funkgui::DecorationKind::highlight));
                    ed.setCaret({ first, 0 });
                    ed.scrollToCaret();
                }
            }
            assist_.tabShown();
            if (persist_)
                persist_();
        }
        void toggleSchematic() { setSchematic(!schematicShown()); }
        // Which of the pills shows: the labelled pair, unless it would narrow the files' tabs below kTabNarrowW.
        bool pillsShown() const { return circuitShown(); }
        bool pillsLabelled() const
        {
            const int n = count();
            const float files = kTabNarrowW * static_cast<float>(n) + kTabGap * static_cast<float>(n - 1)
                              + (canAdd() ? kNewTabW + kTabGap : 0.0f) + (ir_.open ? kTabMinW + 3.0f * kTabGap : 0.0f);
            return files + pillsWidth(true) + 2.0f * kTabGap <= bar_.w;
        }
        float pillWidth(int k, bool labelled) const
        {
            if (!labelled)
                return kPillIconW;
            const float w = funkgui::text::width(funkgui::FontService::get().atlas(), k == 0 ? "Netlist" : "Schematic", chat::kSmall);
            return std::ceil(30.0f + w + 10.0f);
        }
        float pillsWidth(bool labelled) const { return pillWidth(0, labelled) + pillWidth(1, labelled) + 2.0f * kPillTrack; }
        float pillsReserve() const { return pillsShown() ? pillsWidth(pillsLabelled()) + 2.0f * kTabGap : 0.0f; }
        funkgui::Rect pillRect(int k) const
        {
            const bool labelled = pillsLabelled();
            const float right = (ir_.open ? irTabRect().x - kTabGap : bar_.right()) - kPillTrack;
            const float w0 = pillWidth(0, labelled), w1 = pillWidth(1, labelled);
            const float h = kTabH - 2.0f * kPillTrack;
            return k == 0 ? funkgui::Rect{ right - w1 - w0, bar_.y + kPillTrack, w0, h } : funkgui::Rect{ right - w1, bar_.y + kPillTrack, w1, h };
        }
        // The session's file views (EditorPort::fileViews): each netlist's whose view, zoom or pan the user chose.
        std::vector<FileView> fileViews() const
        {
            std::vector<FileView> out;
            for (const auto& [n, f] : views_)
                if (f.stored && fileKindOf(n) == FileKind::Circuit && find(n) >= 0 && out.size() < kMaxFileViews)
                {
                    FileView v;
                    v.file = n;
                    v.schematic = f.schematic;
                    if (f.framed)                        // else zoom 0: none chosen yet, it fits when it opens
                    {
                        v.zoom = f.zoom;
                        v.x = f.cx;
                        v.y = f.cy;
                    }
                    else
                        v.zoom = 0.0f;
                    v.probes = f.probes;                 // the nets pinned to its probes
                    if (v.probes.size() > kMaxFileProbes)
                        v.probes.resize(kMaxFileProbes);
                    out.push_back(std::move(v));
                }
            return out;
        }
        void setFileViews(const std::vector<FileView>& views)
        {
            for (const FileView& v : views)
            {
                if (fileKindOf(v.file) != FileKind::Circuit || !std::isfinite(v.zoom) || !std::isfinite(v.x) || !std::isfinite(v.y))
                    continue;
                SchematicFile& f = views_[v.file];
                f.schematic = v.schematic;
                f.stored = true;
                f.framed = v.zoom > 0.0f;                // zoom 0: the drawing fits when the view opens
                if (f.framed)
                {
                    f.zoom = std::clamp(v.zoom, SchematicView::kMinZoom, SchematicView::kMaxZoom);
                    f.cx = std::clamp(v.x, -1.0e4f, 1.0e4f);
                    f.cy = std::clamp(v.y, -1.0e4f, 1.0e4f);
                }
                f.probes.clear();                        // slots 1 to 3
                for (const std::string& n : v.probes)
                    if (!n.empty() && f.probes.size() < 3)
                        f.probes.push_back(n);
            }
            syncSchematic();
        }
        // The Panel's: a user's gesture moved a file's view (the session keeps them); the view shown may have changed.
        void setViewHooks(std::function<void()> persist, std::function<void()> changed)
        {
            persist_ = std::move(persist);
            changed_ = std::move(changed);
        }
        // The Panel's: a netlist's session pins, as its view reconciles (EditorPort::setSchematicPins).
        void setPinsHook(std::function<void(const std::string&, const std::string&)> pins)
        {
            pins_ = pins;
            schematic_.setPinsHook(std::move(pins));
        }
        void setPinsSource(std::function<std::string(const std::string&)> source) { schematic_.setPinsSource(std::move(source)); }
        // The drawing's open field written (RUN plays what was typed in it).
        void endField() { schematic_.endField(); }
        // The Panel's: a schematic gesture's files to the processor at once (SchematicView::setSyncHook).
        void setSyncHook(std::function<bool()> sync) { schematic_.setSyncHook(std::move(sync)); }
        // The Panel's: the probes' table, program, meters, rings and set (SchematicView::setProbeHooks).
        void setProbeHooks(SchematicView::ProbeHooks hooks) { schematic_.setProbeHooks(std::move(hooks)); }
        const SchematicFile* viewOf(const std::string& file) const
        {
            const auto it = views_.find(file);
            return it == views_.end() ? nullptr : &it->second;
        }

    private:
        struct Tab
        {
            std::string name;
            std::unique_ptr<funkgui::TextEditor> editor;
            uint64_t serial = 0;                         // this tab's, never another's (the undo groups' names)
            int64_t step = 0;                            // its place in its undo history, counted (groups_)
            uint32_t counted = 0;                        // its editor's revision when that was counted
        };

        // One undo step across tabs: what one merge of the draft changed in several tabs' texts at once (Claude's
        // edit_file with files, a session loaded over open tabs), each tab's part its own step in its editor. Cmd-Z in one
        // of them, while it is at that step, undoes the step in each tab still at it (a tab typed in since keeps its own
        // steps and leaves the group); Cmd-Shift-Z or Cmd-Y in one of them, while it has the step undone, redoes it so.
        // Each tab is named by serial, never by address (a tab closed leaves its groups), and its step by its place in
        // the tab's undo history (Tab::step, counted as below).
        struct UndoGroup
        {
            struct Member
            {
                uint64_t tab = 0;                        // Tab::serial
                int64_t step = 0;                        // the tab's place (Tab::step) just after the group's step
            };
            std::vector<Member> members;
        };
        static constexpr std::size_t kMaxUndoGroups = 32;

        // ---- one undo step across tabs (groups_) ----------------------------------------------------------------------
        // Each tab's place in its own undo history is counted (Tab::step): one more for each step a merge or Play this
        // circuit makes, one less or one more for each undo or redo (the text's keys, the Schematic view's keys, buttons
        // and menu, Play's Undo). A group's member is a tab and its place just after the group's step: the tab is at that
        // step while its place is the member's, and has it undone while its place is one less, however far it went back
        // and came again. A change of a tab's text that is not counted (typing, a gesture, Format, a completion) takes the
        // tab out of every group; a new step where the tab could redo takes it out of the groups whose steps those were.

        // Cmd-Z -1, Cmd-Shift-Z and Cmd-Y +1 (as TextEditor reads them), any other key 0.
        static int undoKey(const funkgui::KeyEvent& e)
        {
            if (e.key != funkgui::Key::character || !e.mods.cmd)
                return 0;
            const char32_t c = e.ch >= U'A' && e.ch <= U'Z' ? e.ch - U'A' + U'a' : e.ch;
            return c == U'z' ? (e.mods.shift ? 1 : -1) : c == U'y' ? 1 : 0;
        }

        int tabOf(uint64_t serial) const
        {
            for (int i = 0; i < count(); ++i)
                if (tabs_[static_cast<std::size_t>(i)].serial == serial)
                    return i;
            return -1;
        }

        // The members `out` says out of every group; a group left with fewer than two members is none.
        template <typename Out>
        void leaveGroups(Out out)
        {
            for (UndoGroup& g : groups_)
                g.members.erase(std::remove_if(g.members.begin(), g.members.end(), out), g.members.end());
            groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [](const UndoGroup& g) { return g.members.size() < 2; }),
                          groups_.end());
        }

        // A change of t's text that was not counted: t leaves every group (its steps are its own).
        void recount(Tab& t)
        {
            if (t.editor->revision() == t.counted)
                return;
            t.counted = t.editor->revision();
            leaveGroups([&](const UndoGroup::Member& m) { return m.tab == t.serial; });
        }
        // ... in every tab; and a tab closed leaves the groups.
        void recount()
        {
            for (Tab& t : tabs_)
                recount(t);
            leaveGroups([&](const UndoGroup::Member& m) { return tabOf(m.tab) < 0; });
        }

        // `text` in place of t's as one new step of its undo history (replaceText), counted: the steps it could redo are
        // gone, and the groups' steps among them. True when the text changed.
        bool replaceCounted(Tab& t, const std::string& text)
        {
            recount(t);
            const uint32_t was = t.editor->revision();
            replaceText(*t.editor, text);
            if (t.editor->revision() == was)
                return false;
            leaveGroups([&](const UndoGroup::Member& m) { return m.tab == t.serial && m.step > t.step; });
            ++t.step;
            t.counted = t.editor->revision();
            return true;
        }

        // Tab i's own undo (or redo) step, counted. False when it has none.
        bool ownStep(int i, bool redo)
        {
            Tab& t = tabs_[static_cast<std::size_t>(i)];
            recount(t);
            if (!(redo ? t.editor->redo() : t.editor->undo()))
                return false;
            t.step += redo ? 1 : -1;
            t.counted = t.editor->revision();
            return true;
        }

        // The group whose step the tab shown is at (redo: has undone), the newest; nullptr when none is (after recount()).
        const UndoGroup* groupAt(bool redo) const
        {
            const Tab& shown = tabs_[static_cast<std::size_t>(active_)];
            const int64_t after = shown.step + (redo ? 1 : 0);   // the place the group's step leaves it
            for (auto g = groups_.rbegin(); g != groups_.rend(); ++g)
                if (std::any_of(g->members.begin(), g->members.end(),
                                [&](const UndoGroup::Member& m) { return m.tab == shown.serial && m.step == after; }))
                    return &*g;
            return nullptr;
        }

        // A group's step undone (or redone) in each of its tabs at that step (or with it undone); a tab elsewhere in its
        // history is left as it is and stays in the group.
        void groupStep(const UndoGroup& g, bool redo)
        {
            std::vector<UndoGroup::Member> lost;
            for (const UndoGroup::Member& m : std::vector<UndoGroup::Member>(g.members))
                if (const int i = tabOf(m.tab); i >= 0 && tabs_[static_cast<std::size_t>(i)].step == m.step - (redo ? 1 : 0)
                                                && !ownStep(i, redo))
                    lost.push_back(m);                   // its step is no longer in its history (past maxUndo)
            leaveGroups([&](const UndoGroup::Member& m) {
                return std::any_of(lost.begin(), lost.end(), [&](const UndoGroup::Member& x) { return x.tab == m.tab && x.step == m.step; });
            });
        }

        // Undo (or redo) in the tab shown, as Cmd-Z (Cmd-Shift-Z, Cmd-Y) and the Schematic view's buttons do: its group's
        // step, or its own. False when there is nothing to undo.
        bool undoStep(bool redo)
        {
            recount();
            if (const UndoGroup* g = groupAt(redo))
            {
                groupStep(*g, redo);
                return true;
            }
            return ownStep(active_, redo);
        }


        Tab makeTab(std::string_view n, std::string_view text)
        {
            Tab t{ std::string(n), std::make_unique<funkgui::TextEditor>(id_), ++serials_ };
            funkgui::TextEditorOptions o;
            o.style = codesize::style(size_);
            o.softTabs = true;
            o.tabWidth = 4;
            o.a11yTitle = "File";
            const FileKind kind = fileKindOf(n);
            o.placeholder = tabs_.empty()                ? "An FMinus program: EXAMPLES has some to start from"
                          : kind == FileKind::Gui     ? "The program's interface: rows, sections, knobs, displays"
                          : kind == FileKind::Circuit ? "A circuit netlist (SPICE): the program #includes it as a module"
                                                      : "A file the program can #include by its name";
            t.editor->setOptions(o);
            t.editor->setBounds(assist_.editorRect());
            assist_.configure(*t.editor, n);
            t.editor->setText(text);
            t.editor->attach(host_);
            t.counted = t.editor->revision();
            return t;
        }

        // The shown editor takes the item's focus (none in the Schematic view: the canvas has the keys).
        void show() { editor().setFocused(focused_ && !schematicShown()); }

        // The schematic follows the tab shown and its view.
        void syncSchematic()
        {
            if (schematicShown())
                schematic_.show(&editorOf(active_), name(active_), &views_[name(active_)]);
            else
                schematic_.show(nullptr, {}, nullptr);
            // The find bar is the text's: over the drawing it would cover the toolbar and take no click nor key.
            if (schematicShown() && assist_.findOpen())
                assist_.closeFind();
            editor().setFocused(focused_ && !schematicShown() && !assist_.takesKeys());
            if (viewRenamed_)
            {
                // A file renamed keeps its entry in the session (away from .cir it lost it): written once the tabs
                // hold the new name.
                viewRenamed_ = false;
                if (persist_)
                    persist_();
            }
            if (changed_)
                changed_();
        }

        // A file gone: its view, and the session pins it published (a new file of its name starts afresh).
        std::map<std::string, SchematicFile>::iterator forgetView(std::map<std::string, SchematicFile>::iterator v)
        {
            if (!v->second.published.empty() && pins_)
                pins_(v->first, {});
            return views_.erase(v);
        }

        // A file renamed keeps its view; renamed away from .cir, it loses it.
        void renameView(const std::string& from, const std::string& to)
        {
            const auto it = views_.find(from);
            if (it == views_.end())
                return;
            SchematicFile f = std::move(it->second);
            views_.erase(it);
            viewRenamed_ = viewRenamed_ || f.stored;     // the session's entry follows (syncSchematic keeps it)
            if (!f.published.empty() && pins_)
                pins_(from, {});                         // its pins go under the new name at the next reconcile
            f.published.clear();
            if (fileKindOf(to) == FileKind::Circuit)
                views_[to] = std::move(f);
        }

        // The Netlist view of the netlist shown, its caret at (line, col) (0-based; -1: where it is).
        void showNetlistAt(int line, int col)
        {
            setSchematic(false);
            if (line < 0)
                return;
            funkgui::TextEditor& ed = editorOf(active_);
            ed.setCaret({ line, std::max(0, col) });
            ed.scrollToCaret();
        }

        // Whether a file of the program #includes the netlist shown (lang::includeOn), by the files' revision.
        bool played() const
        {
            const uint64_t r = revision();
            if (r == playedRev_ && playedName_ == name(active_))
                return played_;
            playedRev_ = r;
            playedName_ = name(active_);
            played_ = false;
            for (int i = 0; i < count() && !played_; ++i)
            {
                if (i == active_ || fileKindOf(name(i)) != FileKind::FMinus)
                    continue;
                const funkgui::TextEditor& e = *tabs_[static_cast<std::size_t>(i)].editor;
                for (int l = 0; l < e.lineCount() && !played_; ++l)
                {
                    std::string inc;
                    int a = 0, b = 0;
                    played_ = lang::includeOn(e.line(l), inc, a, b) && inc == playedName_;
                }
            }
            return played_;
        }

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
            const float room = bar_.w - (canAdd() ? kNewTabW + kTabGap : 0.0f) - kTabGap * static_cast<float>(n - 1)
                             - (ir_.open ? irTabWidth() + 3.0f * kTabGap : 0.0f) - pillsReserve();
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
            return std::any_of(problems_->begin(), problems_->end(),
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
        const std::vector<Problem>* problems_;       // the Panel's
        Close close_;
        std::vector<Tab> tabs_;
        int active_ = 0;
        uint64_t structure_ = 0;
        std::vector<UndoGroup> groups_;                  // newest last, at most kMaxUndoGroups
        uint64_t serials_ = 0;
        bool focused_ = false;
        bool pressedEditor_ = false;
        funkgui::HostServices* host_ = nullptr;
        funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
        int renaming_ = -1;                          // the tab whose name is being edited
        funkgui::text::LineEdit edit_;
        std::string why_;                            // why the name typed cannot be the file's
        bool hidden_ = false;                        // the PANEL view
        std::function<void(const std::string&)> notice_;
        // The IR tab (IrView.h): open, shown, its view; each view's editor, made when it first shows, and what its text
        // was made from; the theme a new one takes.
        struct IrTab
        {
            bool open = false;
            bool shown = false;                      // the tab shown (else the file active_ is)
            int view = 0;                            // an irview::View
            std::array<std::unique_ptr<funkgui::TextEditor>, irview::kViews> editors;
            std::array<std::string, irview::kViews> made;
            // The IrText each view's text was made from: held, so that no other text takes its address while `made`
            // names it; and the line LLVM's text starts on (after the comment lines and the blank line).
            std::array<std::shared_ptr<const IrText>, irview::kViews> from;
            std::array<int, irview::kViews> body{};
        };
        IrTab ir_;
        int size_ = codesize::read();                    // the text size, tenths of a px (setCodeSize)
        funkgui::Theme irTheme_ = funkgui::Theme::graphite();
        bool irThemed_ = false, irDark_ = true;
        bool pressedSchematic_ = false;
        // The netlists' views, by file name (SchematicView.h): std::map, so that a view's address holds.
        std::map<std::string, SchematicFile> views_;
        std::function<void()> persist_, changed_;
        std::function<void(const std::string&, const std::string&)> pins_;
        bool viewRenamed_ = false;                   // a stored view's file was renamed: the session follows
        mutable uint64_t playedRev_ = ~uint64_t{ 0 };
        mutable std::string playedName_;
        mutable bool played_ = false;
        // Play this circuit's (playCircuit): what it wrote, into which program, for which netlist; RUN.
        bool playOn_ = false;
        uint64_t playRev_ = 0;
        std::string playFile_, playProgram_;
        std::function<void()> run_;
        // Last: its tabs' callbacks reach the members above.
        CodeAssist assist_;
        SchematicView schematic_;                    // after assist_: its hooks read it

    public:
        // The PANEL view hides the item: the Schematic view's gesture and tool end there, as on leaving the view.
        void setHidden(bool hidden)
        {
            if (hidden && !hidden_)
            {
                schematic_.endField();                   // a field open is written, as a press outside it would
                schematic_.leave();
            }
            hidden_ = hidden;
        }
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
            uint64_t drawKey() const override
            {
                DrawKey k;
                k.mix(bounds_).mix(pointer_).mix(problems_.size());
                for (const Problem& p : problems_)
                    k.mix(p.file).mix(p.line).mix(p.column).mix(p.error).mix(p.warning).mix(p.text);
                return k.h;
            }
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

        // ---- the dials folded under a netlist's Schematic view, or the code: "32 DIALS   SHOW" -----------------------
        // A 24 px bar at the program's column's bottom when the dials do not fit beside the drawing's room (or, under
        // the code, when they would leave the editor less than its floor: 64 in the least window); its button shows
        // them (small; the editor keeping 192 px beside a drawing, its floor beside the code) and hides them again. The
        // PANEL view always shows them all. A panel of displays alone counts its displays.
        class DialsBarItem final : public Item
        {
        public:
            DialsBarItem(uint32_t id, std::function<void()> toggle) : id_(id), toggle_(std::move(toggle)) {}

            void place(funkgui::Rect r, int dials, int displays, int hidden, bool open, bool shown, bool schematic)
            {
                bounds_ = r;
                displays_ = dials == 0;
                dials_ = dials == 0 ? displays : dials;
                hidden_ = hidden;
                open_ = open;
                shown_ = shown;
                schematic_ = schematic;
            }
            funkgui::Rect button() const { return { bounds_.right() - 64.0f, bounds_.y + 2.0f, 60.0f, bounds_.h - 4.0f }; }

            void tick(float, bool hovered, bool, funkgui::Point pointer) override
            {
                hovered_ = hovered;
                pointer_ = pointer;
            }
            uint64_t drawKey() const override
            {
                return DrawKey{}.mix(bounds_).mix(dials_).mix(displays_).mix(hidden_).mix(open_).mix(hovered_).mix(pointer_).h;
            }
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override
            {
                const funkgui::Canvas::Scope s(c, kSectionTag, false);
                const chat::Palette p = chat::palette(th);
                c.rrect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, 6.0f, p.raised, 1.0f, p.cardEdge);
                char text[64];
                const char* what = displays_ ? "DISPLAY" : "DIAL";
                if (open_ && hidden_ > 0)                // more than the window has room for: the PANEL view has all
                    std::snprintf(text, sizeof text, "%d OF %d %sS SHOWN: THE PANEL VIEW HAS ALL", dials_ - hidden_, dials_, what);
                else
                    std::snprintf(text, sizeof text, "%d %s%s", dials_, what, dials_ == 1 ? "" : "S");
                c.text(text, bounds_.x + 10.0f, c.capCentreTop(bounds_.centreY(), funkgui::type::kMicro), funkgui::type::kMicro, th.ink52);
                const funkgui::Rect b = button();
                const bool hot = hovered_ && bounds_.contains(pointer_);
                c.rrect(b.x, b.y, b.w, b.h, 0.5f * b.h, hot ? p.hover : p.composer, 1.0f, ring ? p.accent : p.composerEdge);
                c.text(open_ ? "HIDE" : "SHOW", b.centreX(), c.capCentreTop(b.centreY(), funkgui::type::kMicro), funkgui::type::kMicro,
                       hot ? th.accent : th.ink70, funkgui::Align::centre);
            }
            bool contains(funkgui::Point p) const override { return bounds_.contains(p); }
            funkgui::Cursor cursorAt(funkgui::Point) const override { return funkgui::Cursor::pointingHand; }
            void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&) override
            {
                if (!e.popup && toggle_)
                    toggle_();
            }
            bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override
            {
                if (e.key != funkgui::Key::enter && e.key != funkgui::Key::space)
                    return false;
                if (toggle_)
                    toggle_();
                return true;
            }
            void accessibility(std::vector<funkgui::A11yItem>& out) const override
            {
                funkgui::A11yItem it;
                it.id = id_;
                it.role = funkgui::A11yRole::button;
                it.bounds = bounds_;
                const std::string what = displays_ ? "displays" : "dials";
                it.title = (open_ ? "Hide the " : "Show the ") + what;
                it.value = std::to_string(dials_) + " " + what + (open_ && hidden_ > 0 ? ", " + std::to_string(dials_ - hidden_) + " shown" : "");
                out.push_back(std::move(it));
            }
            bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override
            {
                if (id != id_)
                    return false;
                if (toggle_)
                    toggle_();
                return true;
            }
            void specLine(char* out, std::size_t n) const override
            {
                const char* where = schematic_ ? "THE DRAWING" : "THE CODE";
                std::snprintf(out, n, "%d %s   %s %s%s   THE PANEL VIEW SHOWS THEM ALL", dials_, displays_ ? "DISPLAYS" : "DIALS",
                              open_ ? "HIDE GIVES" : "SHOW PUTS THEM UNDER", where, open_ ? " THEIR ROOM" : ", SMALL");
            }
            funkgui::Point centre() const override { return { button().centreX(), button().centreY() }; }
            bool shown() const override { return shown_; }

        private:
            uint32_t id_;
            std::function<void()> toggle_;
            funkgui::Rect bounds_;
            int dials_ = 0, hidden_ = 0;                 // the program's (its displays' when it has no dial), and those over the editor's floor
            bool displays_ = false;                      // a panel of displays alone: dials_ counts them
            bool open_ = false, shown_ = false, hovered_ = false, schematic_ = true;
            funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
        };

        // ---- the top bar's load: the snowflake while the frozen copy plays ---------------------------------------------
        // The load's text ends left of the theme's button; the snowflake sits just left of it while the dials are
        // frozen into the code (EditorPort::frozen), and the alerts' words further left.
        constexpr float kFrozenIcon = 11.0f;             // the snowflake's box
        constexpr float kFrozenGap = 6.0f;               // between it and the load's text
        constexpr float kAlertsGap = 9.0f;               // between it and the alerts' words

        struct MeterPlaces
        {
            float loadRight = 0.0f;                      // the load's text ends here
            float flakeX = 0.0f;                         // the snowflake's centre
            float alertsRight = 0.0f;                    // the alerts' words end here
        };

        // `loadW` and `spaces` measured with Canvas::textWidth (text::width, which layout uses, is the same measure).
        MeterPlaces meterPlaces(float themeX, float loadW, bool frozen, float spaces)
        {
            MeterPlaces m;
            m.loadRight = themeX - 10.0f;
            const float left = m.loadRight - loadW;
            m.flakeX = left - kFrozenGap - 0.5f * kFrozenIcon;
            m.alertsRight = frozen ? left - kFrozenGap - kFrozenIcon - kAlertsGap : left - spaces;
            return m;
        }

        // The snowflake as a readout: drawn in the accent, its line in the status bar and its title for VoiceOver.
        class FrozenItem final : public Item
        {
        public:
            explicit FrozenItem(uint32_t id) : id_(id) {}

            void place(float cx, float cy) { bounds_ = { cx - 9.0f, cy - 11.0f, 18.0f, 22.0f }; }
            void setShown(bool shown) { shown_ = shown; }

            void tick(float, bool, bool, funkgui::Point) override {}
            void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const override
            {
                const funkgui::Canvas::Scope s(c, kFrozenTag, false);
                chat::drawIcon(c, chat::Icon::snowflake, bounds_.centreX(), bounds_.centreY(), kFrozenIcon,
                               chat::palette(th).accent);
            }
            bool contains(funkgui::Point p) const override { return bounds_.contains(p); }
            void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override {}
            void accessibility(std::vector<funkgui::A11yItem>& out) const override
            {
                funkgui::A11yItem it;
                it.id = id_;
                it.role = funkgui::A11yRole::image;
                it.bounds = bounds_;
                it.title = "Frozen: the dials are compiled into the code while they hold still";
                it.readOnly = true;
                out.push_back(std::move(it));
            }
            bool a11yAction(uint32_t, funkgui::A11yAction, double, funkgui::GestureController&) override { return false; }
            void specLine(char* out, std::size_t n) const override
            {
                std::snprintf(out, n, "FROZEN   THE DIALS ARE COMPILED INTO THE CODE WHILE THEY HOLD STILL");
            }
            funkgui::Point centre() const override { return { bounds_.centreX(), bounds_.centreY() }; }
            bool shown() const override { return shown_; }
            bool focusable() const override { return false; }

        private:
            uint32_t id_;
            funkgui::Rect bounds_;
            bool shown_ = false;
        };
    }

    // ---- the host, as the items reach it -----------------------------------------------------------------------------
    // Every call goes to the host the Panel was attached to. A menu's and a file chooser's answer comes later, outside
    // any input to the Panel (Panel.h "Unchanged frames"): the Panel is marked changed before the callback runs, so
    // what the answer does is drawn, whoever asked (the Panel, the code editor, the Schematic view, a .gui's menu).
    // A menu shown is the open one (menuOpenAt: its trigger draws open) until its answer, dismissMenus() or the next
    // request, which replaces it whether or not that one is shown (the hosts' rule): each request's serial says whether
    // its answer still closes the open one.
    class Panel::HostProxy final : public funkgui::HostServices
    {
    public:
        HostProxy(Panel& panel, funkgui::HostServices& host) : panel_(panel), host_(host) {}

        void setUnboundedDrag(bool on) override { host_.setUnboundedDrag(on); }
        void showParamMenu(funkgui::ParamPort& p, float x, float y) override { host_.showParamMenu(p, x, y); }
        void nudgeFullRate() override { host_.nudgeFullRate(); }
        double nowSeconds() const override { return host_.nowSeconds(); }
        void beginBatch() override { host_.beginBatch(); }
        void endBatch() override { host_.endBatch(); }
        int themeIndex() const override { return host_.themeIndex(); }
        juce::Component* ownerComponent() override { return host_.ownerComponent(); }
        int zoomPercent() const override { return host_.zoomPercent(); }
        void setZoomPercent(int percent) override { host_.setZoomPercent(percent); }
        std::span<const int> zoomSteps() const override { return host_.zoomSteps(); }
        bool zoomFits(int percent) const override { return host_.zoomFits(percent); }
        unsigned services() const override { return host_.services(); }
        bool showMenu(const funkgui::MenuRequest& request, funkgui::MenuCallback done) override
        {
            const uint32_t serial = ++panel_.menuSerial_;
            panel_.menuOpen_ = false;
            panel_.dirty_ = true;
            const bool shown = host_.showMenu(request, [panel = &panel_, serial, then = std::move(done)](int id) {
                panel->dirty_ = true;
                if (panel->menuSerial_ == serial)
                    panel->menuOpen_ = false;
                if (then)
                    then(id);
            });
            if (shown && panel_.menuSerial_ == serial)
            {
                panel_.menuOpen_ = true;
                panel_.menuAnchor_ = request.anchor;
            }
            return shown;
        }
        void dismissMenus() override
        {
            host_.dismissMenus();
            ++panel_.menuSerial_;
            panel_.menuOpen_ = false;
            panel_.dirty_ = true;
        }
        bool chooseFiles(const funkgui::FileRequest& request, funkgui::FilesCallback done) override
        {
            return host_.chooseFiles(request,
                                     [panel = &panel_, then = std::move(done)](const std::vector<std::string>& paths) {
                                         panel->dirty_ = true;
                                         if (then)
                                             then(paths);
                                     });
        }
        bool copyText(std::string_view utf8) override { return host_.copyText(utf8); }
        bool commandKeyIsMeta() const override { return host_.commandKeyIsMeta(); }
        std::string clipboardText() const override { return host_.clipboardText(); }
        void announce(std::string_view text, Announce priority) override { host_.announce(text, priority); }
        void a11yFocus(uint32_t itemId) override { host_.a11yFocus(itemId); }

    private:
        Panel& panel_;
        funkgui::HostServices& host_;
    };

    // ---- construction ---------------------------------------------------------------------------------------------------
    // The items are made with no place: layout() gives them their places for the panel's size. Their order is the Tab
    // order.
    Panel::Panel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, Role role)
        : role_(role), port_(port), ownsProbes_(role != Role::picture)
    {
        const bool picture = role == Role::picture;
        if (role_ != Role::code)
        {
            port_.addMeterReader();                      // the displays read the meters and the scopes while it is shown
            reading_ = true;
        }
        feed_ = std::make_unique<DisplayFeed>();         // their samples and spectra, sized once
        // A picture shows the editor's window as the user has it: the panel full size while an editor's code is in its
        // window now (not the session's choice alone, which an editor without a window's owner never follows).
        out_ = picture && port_.codeWindowOpen();
        seenSettingsAsk_ = port_.settingsAsked();        // the app's requests from now on open SETTINGS
        title_ = upper(product::kName);
        subtitle_ = std::string(product::kIsInstrument ? "Instrument" : "Effect") + "  v" + product::kVersion + "  FMinus";
        width_ = static_cast<float>(role_ == Role::code ? kCodeDefaultWidth : kDefaultWidth);
        height_ = static_cast<float>(role_ == Role::code ? kCodeDefaultHeight : kDefaultHeight);
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
                               "EXAMPLES   START AFRESH FROM ONE: ITS FILES, THE DIALS AT THEIR DEFAULTS");
        libraryItem_ = button(TB::Shape::pill, chat::Icon::blocks, [] { return std::string("Library"); }, true,
                              [] { return TB::Look::ghost; }, [this] { openLibrary(); }, "Library",
                              "LIBRARY   A READY-MADE BUILDING BLOCK AS A TAB OF ITS OWN, #INCLUDED BY THE PROGRAM");
        randomItem_ = button(TB::Shape::pill, chat::Icon::dice, [] { return std::string("Random"); }, false,
                             [] { return TB::Look::ghost; }, [this] { makeRandom(nextSeed()); }, "A random program",
                             "RANDOM   A NEW PROGRAM FROM THE LIBRARY'S BUILDING BLOCKS, FOR INSPIRATION");
        projectsItem_ = button(TB::Shape::pill, chat::Icon::list, [this] { return projectLabel(); }, true,
                               [] { return TB::Look::outlined; }, [this] { openProjects(); }, "Projects",
                               "PROJECTS   SAVE THE PROGRAM ON THIS MAC, A VERSION EACH SAVE (CMD-S); OPEN AN OLDER ONE; ITS PRESETS");
        // The project open's presets, right of the pill: the one before, its name (a click: their list), the next.
        {
            const auto any = [this] { return !cycle_.presets.empty(); };
            const auto shown = [this] { return presetsShown(); };
            TB::Spec sp;
            sp.shape = TB::Shape::square;
            sp.icon = [] { return chat::Icon::chevronLeft; };
            sp.look = [] { return TB::Look::ghost; };
            sp.enabled = any;
            sp.shown = shown;
            sp.act = [this] { stepPresets(-1); };
            sp.a11yTitle = "Previous preset";
            sp.spec = "PREVIOUS PRESET   THE ONE BEFORE, OF THE PROJECT OPEN ([), WRAPPING: ITS DIALS GLIDE THERE";
            presetPrevItem_ = add(std::make_unique<TB>(idFor(), sp));
            TB::Spec name;
            name.shape = TB::Shape::pill;
            name.label = [this] { return presetLabel(); };
            name.detail = [this] { return presetDetail(); };
            name.dot = [this] { return cycle_.applied && cycle_.edited; };
            name.chevron = true;
            name.look = [] { return TB::Look::outlined; };
            name.shown = shown;
            name.act = [this] { openPresets(); };
            name.a11yTitle = "Preset";
            name.specNow = [this] {
                return std::string(cycle_.applied && cycle_.edited
                                       ? "PRESET   ITS DIALS MOVED (THE DOT): SAVE IT IN PROJECTS TO KEEP THEM; THE ARROWS LEAVE THEM"
                                       : "PRESET   THE PROJECT OPEN'S PRESETS IN A LIST: SEARCH, THEIR GROUPS, A RIGHT-CLICK FOR A CATEGORY");
            };
            presetItem_ = add(std::make_unique<TB>(idFor(), std::move(name)));
            sp.icon = [] { return chat::Icon::chevronRight; };
            sp.act = [this] { stepPresets(1); };
            sp.a11yTitle = "Next preset";
            sp.spec = "NEXT PRESET   THE NEXT ONE OF THE PROJECT OPEN (]), WRAPPING: ITS DIALS GLIDE THERE";
            presetNextItem_ = add(std::make_unique<TB>(idFor(), std::move(sp)));
        }
        panelView_ = role_ != Role::code && port_.panelView();   // the code window's is the CODE view
        codePanel_[0] = port_.codePanel(false);
        codePanel_[1] = port_.codePanel(true);
        chatOpen_ = port_.chatOpen();
        explorerOpen_ = port_.explorerOpen();
        seenExplorerWidth_ = port_.explorerWidth();
        seenChatWidth_ = port_.chatWidth();
        explorerWant_ = wantedWidth(seenExplorerWidth_, false);
        chatWant_ = wantedWidth(seenChatWidth_, true);
        // The Code / Panel switch. While the editor is in the code window, the editor's window shows the panel and its
        // Code half brings that window to the front; the code window shows the code and its Panel half brings the
        // editor's window to the front.
        const auto viewSpec = [this] {
            if (role_ == Role::code)
                return std::string("CODE / PANEL   THE CODE IS IN THIS WINDOW: PANEL BRINGS THE EDITOR'S WINDOW TO THE FRONT");
            if (out_)
                return std::string("CODE / PANEL   THE CODE IS IN A WINDOW OF ITS OWN: CODE BRINGS IT TO THE FRONT");
            return std::string("CODE / PANEL   THE EDITOR, OR THE PROGRAM'S PANEL ACROSS THE WHOLE COLUMN");
        };
        viewItem_ = button(TB::Shape::pill, chat::Icon::code, [] { return std::string("Code"); }, false,
                           [this] { return full() ? TB::Look::ghost : TB::Look::raised; },
                           [this] {
                               if (out_)
                               {
                                   if (windowHooks_.raise)
                                       windowHooks_.raise();
                               }
                               else
                                   setPanelView(false);
                           },
                           "Code view", "");
        panelViewItem_ = button(TB::Shape::pill, chat::Icon::dial, [] { return std::string("Panel"); }, false,
                                [this] { return full() ? TB::Look::raised : TB::Look::ghost; },
                                [this] {
                                    if (role_ == Role::code)
                                    {
                                        if (windowHooks_.raise)
                                            windowHooks_.raise();
                                    }
                                    else if (!out_)
                                        setPanelView(true);
                                },
                                "Panel view", "");
        for (const int i : { viewItem_, panelViewItem_ })
            static_cast<TB&>(*items_[static_cast<std::size_t>(i)]).setSpecNow(viewSpec);
        // The window button, left of the switch: the code editor and the explorer in a window of their own, or back.
        {
            TB::Spec sp;
            sp.shape = TB::Shape::square;
            sp.icon = [] { return chat::Icon::window; };
            sp.look = [this] { return out_ || role_ == Role::code ? TB::Look::primary : TB::Look::ghost; };
            sp.shown = [this] { return windowButtonShown(); };
            sp.checked = [this] { return out_ || role_ == Role::code; };
            sp.act = [this] { setEditorOut(!(out_ || role_ == Role::code)); };
            sp.a11yTitle = "Code editor in its own window";
            sp.specNow = [this] {
                return std::string(out_ || role_ == Role::code
                                       ? "WINDOW   THE CODE EDITOR IS IN A WINDOW OF ITS OWN: A CLICK (OR CLOSING IT) BRINGS IT BACK"
                                       : "WINDOW   THE CODE EDITOR AND THE EXPLORER IN A WINDOW OF THEIR OWN, THE PANEL FULL SIZE HERE");
            };
            windowItem_ = add(std::make_unique<TB>(idFor(), std::move(sp)));
        }

        // The editor, the problems under it, the dials under them.
        editorItem_ = add(std::make_unique<FilesItem>(funkgui::Rect{}, funkgui::Rect{}, idFor(), problems_,
                                                      [this](int tab) { closeFile(tab); }));
        {
            FilesItem& f = static_cast<FilesItem&>(*items_[static_cast<std::size_t>(editorItem_)]);
            Sources library;
            for (const LibraryFile& l : kLibrary)
                library.push_back({ l.name, l.source });
            f.assist().setLibrary(library);
            f.assist().setInstrument(product::kIsInstrument);
        }
        problemsItem_ = add(std::make_unique<ProblemsItem>(funkgui::Rect{}, idFor(), problems_,
                                                           [this](const Problem& p) { jumpTo(p); }));
        dialsBarItem_ = add(std::make_unique<DialsBarItem>(idFor(), [this] {
            dialsOpen_ = !dialsOpen_;
            layout();
        }));
        itemOfSlot_.assign(static_cast<std::size_t>(kMaxControls), -1);
        for (int c = 0; c < kMaxControls && c < static_cast<int>(ports.size()); ++c)
        {
            itemOfSlot_[static_cast<std::size_t>(c)] = add(std::make_unique<DialItem>(
                *ports[static_cast<std::size_t>(c)], funkgui::DialGeom{ 0.0f, 0.0f }, idFor()));
            static_cast<DialItem&>(*items_.back()).setMenu([this, c](funkgui::Point at) { dialMenu(c, at); });
        }
        // The program's displays (#pragma panel meter, vu, led, number, scope), shown as it has them.
        itemOfDisplay_.assign(static_cast<std::size_t>(kMaxDisplays), -1);
        for (int d = 0; d < kMaxDisplays; ++d)
            itemOfDisplay_[static_cast<std::size_t>(d)] = add(std::make_unique<DisplayItem>(idFor()));
        // A .gui file's interface (GuiView.h): its tabs first (their frames under the rest), a control a slot, and
        // pools of pads, envelopes, keyboards and shaders; the displays above are its too while it shows them.
        {
            GuiView::Hooks hooks;
            hooks.menuTheme = [this] {
                return productTheme(host_ != nullptr ? funkgui::Theme::byIndex(host_->themeIndex()) : funkgui::Theme::graphite());
            };
            hooks.menuOpen = [this](const funkgui::Rect& r) { return menuOpenAt(r); };
            hooks.menuHost = [this] { return host_; };
            hooks.choosePage = [this](int node, int page) { chooseGuiPage(node, page); };
            hooks.playNote = [this](int note, float velocity) { port_.playNote(note, velocity); };
            hooks.playWheel = [this](bool pitch, float value) { port_.playWheel(pitch, value); };
            hooks.dialMenu = [this](int slot, funkgui::Point at) { dialMenu(slot, at); };
            guiView_ = std::make_unique<GuiView>(items_, ports, itemOfDisplay_, idFor, std::move(hooks));
        }

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
                          "New chat", "NEW CHAT   START ANOTHER; THIS ONE STAYS IN THE CHAT LIST")));
        // The chats (ChatPort::chatList): the chat shown, a dropdown of the project's chats kept on this Mac.
        chatsItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::pill, {}, [this] { return chatsLabel(); }, true, [] { return B::Look::ghost; }, {},
                          [this] { openChats(); }, "Chats",
                          "CHATS   THE PROJECT'S CHATS, KEPT ON THIS MAC ONLY: OPEN ONE, START, RENAME OR DELETE")));
        keyItem_ = add(std::make_unique<B>(
            idFor(), spec(B::Shape::square, [] { return chat::Icon::key; }, {}, false, {}, {}, [this] { openKeyDialog(); },
                          "Model provider and API key",
                          "API KEY   THE MODEL PROVIDER AND ITS KEY, STORED ENCRYPTED ON THIS MAC, FOR BOTH PLAYGROUND PLUGINS")));
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
                else if (s == ExplorerItem::Section::assets)
                {
                    std::string why;
                    notice(port_.removeAsset(r.key, why) ? "SAMPLE REMOVED: " + upper(r.key) : "NOT REMOVED: " + upper(why));
                    explorerStale_ = true;
                    dirty_ = true;
                }
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
                else if (s == ExplorerItem::Section::assets)
                    openSamples();
                else if (s == ExplorerItem::Section::projects)
                    openProjects(false);
                else if (s == ExplorerItem::Section::folder)
                {
                    std::string why;
                    pushEditor();                        // what the folder last gets is what the tabs show
                    (void)port_.setFolderSync(false, why);
                    notice("FOLDER OFF: IT STAYS AS IT IS");
                    syncFolder();
                    dirty_ = true;
                }
            };
            explorerItem_ = add(std::make_unique<ExplorerItem>(kExplorerA11yBase, std::move(hooks)));
        }
        {
            B::Spec sp = spec(B::Shape::square, [this] { return explorerShown_ ? chat::Icon::chevronLeft : chat::Icon::list; }, {},
                              false, {}, {}, [this] { toggleExplorer(); }, "Fold or open the explorer",
                              "EXPLORER   THE PROGRAM'S FILES, THE LIBRARY, THE EXAMPLES AND THE PRESETS; FOLD IT AWAY FOR ROOM");
            sp.shown = [this] { return !full(); };       // the PANEL view is the plugin's face: no explorer
            explorerToggleItem_ = add(std::make_unique<B>(idFor(), std::move(sp)));
        }
        chatViewItem_ = add(std::make_unique<ChatView>(
            kChatA11yBase, ChatView::Hooks{ [this](const std::string& s) { suggest(s); },
                                            [this] { openKeyDialog(); },
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
        {
            B::Spec thinking = spec(B::Shape::pill, {}, [this] { return thinkingLabel(); }, true, [] { return B::Look::ghost; },
                                    [this] { return !chatEfforts().empty(); }, [this] { openThinking(); }, "Thinking level",
                                    "THINKING   HOW MUCH THE MODEL THINKS BEFORE IT ANSWERS, AND HOW HARD IT WORKS");
            thinking.shown = [this] { return chatTakesThinking(); };   // another provider's: where its model has levels
            thinkingItem_ = add(std::make_unique<B>(idFor(), std::move(thinking)));
        }
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
                               [this] { return chat_.web ? B::Look::primary : B::Look::ghost; }, {},
                               [this] {
                                   port_.chat().setWeb(!chat_.web);
                                   chatAt_ = -1.0;
                                   dirty_ = true;
                               },
                               "Web search",
                               "WEB   CLAUDE SEARCHES THE WEB AND READS PAGES AND PDFS (DATA SHEETS, PAPERS): $10 A THOUSAND "
                               "SEARCHES, WITH YOUR OWN API KEY");
            web.shown = [this] { return chatTakesWeb(); };
            web.checked = [this] { return chat_.web; };
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
        frozenItem_ = add(std::make_unique<FrozenItem>(idFor()));   // left of the load, while the dials are frozen

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
        // The dropdowns' triggers draw open while their menu is (Dropdown.h), PROJECTS' while its card is.
        for (const int i : { examplesItem_, libraryItem_, modelItem_, thinkingItem_, zoomItem_, chatsItem_ })
        {
            TB& trigger = static_cast<TB&>(*items_[static_cast<std::size_t>(i)]);
            trigger.setOpen([this, &trigger] { return menuOpenAt(trigger.bounds()); });
        }
        static_cast<TB&>(*items_[static_cast<std::size_t>(projectsItem_)]).setOpen([this] {
            return projects_.isOpen();
        });
        static_cast<TB&>(*items_[static_cast<std::size_t>(presetItem_)]).setOpen([this] { return picker_.isOpen(); });
        picker_.setAnswered([this] { afterPicker(true); });   // a menu's answer, as an event's
        settings_.setMenuOpen([this](const funkgui::Rect& r) { return menuOpenAt(r); });
        projects_.setMenuOpen([this](const funkgui::Rect& r) { return menuOpenAt(r); });
        projects_.setAnswered([this] { afterProjects(true); });   // a menu's answer, as an event's

        // The Standalone's muted input (an effect's: JUCE's guard against a feedback loop): a notice in the status bar,
        // left of the zoom, and this button, which opens SETTINGS.
        {
            TB::Spec sp;
            sp.shape = TB::Shape::pill;
            sp.icon = [] { return chat::Icon::gear; };
            sp.label = [] { return std::string("Settings..."); };
            sp.look = [] { return TB::Look::outlined; };
            sp.shown = [this] { return mutedShown_; };
            sp.act = [this] { openSettings(); };
            sp.a11yTitle = "Settings: the audio input is muted, to avoid a feedback loop";
            sp.spec = "INPUT MUTED   A LOOP THROUGH THE SPEAKERS WOULD FEED BACK: UNMUTE IT IN SETTINGS, WITH HEADPHONES ON";
            mutedItem_ = add(std::make_unique<TB>(idFor(), std::move(sp)));
        }
        // The recorder's Mark (the capture tool's recording for Claude): in the status bar, right of its REC notice, while
        // it records; it marks this moment for Claude (Cmd-Shift-M too).
        {
            TB::Spec sp;
            sp.shape = TB::Shape::pill;
            sp.icon = [] { return chat::Icon::flag; };
            sp.label = [] { return std::string("Mark"); };
            sp.look = [] { return TB::Look::outlined; };
            sp.shown = [this] {
                return recording_.stage == RecordingState::Stage::waiting || recording_.stage == RecordingState::Stage::recording;
            };
            sp.enabled = [this] { return recording_.stage == RecordingState::Stage::recording && recording_.room; };
            sp.act = [this] { markNow(); };
            sp.a11yTitle = "Mark this moment in the recording for Claude";
            sp.spec = upper(shortcutText("Mark   this moment in the recording, for Claude to find (Cmd-Shift-M)"));
            markItem_ = add(std::make_unique<TB>(idFor(), std::move(sp)));
        }
        // Claude Desktop's badge in the chat's header, while it is connected and its calls go to another instance (or to
        // none: several open, none chosen): a click makes this one its target. The chat folded, a mark on its rail says
        // it instead (its footer line the badge's words), and does the same.
        {
            const auto act = [this] {
                port_.chooseDesktop(desktopState_.id, true);
                notice("CLAUDE DESKTOP: ITS CALLS COME HERE NOW");
                syncDesktop();
                dirty_ = true;
            };
            const auto footer = [this] {
                return desktopState_.targetName.empty()
                         ? std::string("CLAUDE DESKTOP   SEVERAL OPEN, NONE CHOSEN: IT ASKS WHICH   A CLICK SENDS ITS CALLS HERE")
                         : "CLAUDE DESKTOP   ITS CALLS GO TO " + upper(desktopState_.targetName) + "   A CLICK SENDS THEM HERE";
            };
            B::Spec sp = spec(B::Shape::pill, [] { return chat::Icon::bubble; }, [this] { return desktopBadge_; }, false,
                              [] { return B::Look::outlined; }, {}, act, "Claude Desktop's target", {});
            sp.specNow = footer;
            desktopItem_ = add(std::make_unique<B>(idFor(), std::move(sp)));
            B::Spec mark = spec(B::Shape::square, [] { return chat::Icon::alert; }, {}, false, [] { return B::Look::outlined; }, {},
                                act, "Claude Desktop's target", {});
            mark.specNow = footer;
            desktopMarkItem_ = add(std::make_unique<B>(idFor(), std::move(mark)));
        }

        // The columns' edges (Columns.h): the explorer's right one and the chat's left one, each a handle that sets its
        // card's width (last of the items: hit by itemAt before the cards' items, drawn over the cards' edges).
        {
            const auto edge = [&](bool chat) {
                columns::EdgeItem::Hooks h;
                h.width = [this, chat] { return chat ? fit_.chat : fit_.explorer; };
                h.least = [this, chat] { return chat ? fit_.chatLeast : fit_.explorerLeast; };
                h.most = [this, chat] { return chat ? fit_.chatMost : fit_.explorerMost; };
                h.set = [this, chat](float px, columns::Keep keep) { setColumnWidth(chat, px, keep); };
                h.reset = [this, chat] { setColumnWidth(chat, 0.0f, columns::Keep::now); };
                return add(std::make_unique<columns::EdgeItem>(
                    idFor(), chat ? -1 : 1, chat ? "Chat width" : "Explorer width",
                    chat ? "CHAT WIDTH" : "EXPLORER WIDTH", std::move(h)));
            };
            explorerEdgeItem_ = edge(false);
            chatEdgeItem_ = edge(true);
        }

        layout();

        // The processor's files and status, and the chat; the netlists' views the session kept.
        bindFiles();
        claudeSeen_ = port_.claudeNotice().count;        // what Claude did before this editor opened: not said again
        saveSeen_ = port_.saving().done;                 // and a save another editor made
        seenDraft_ = port_.draftRevision();
        given_ = port_.draft();
        files().load(given_);
        files().setFileViews(port_.fileViews());
        syncedFiles_ = files().revision();
        running_ = port_.running();
        refresh(0.0);
        if (layoutStale())
            layout();                                    // the tab the session shows: its room (a netlist's, the panel)
        setFocus(editorItem_);                           // none in the PANEL view, where the editor is hidden
    }

    // The files' hooks, to this Panel: when it makes its editor, and when the code window lends or gives it back
    // (lendEditor: the item goes from one Panel to the other, and its hooks with it).
    void Panel::bindFiles()
    {
        FilesItem& fi = files();
        fi.setPanelHooks(problems_, [this](int tab) { closeFile(tab); });
        fi.setMenuOpen([this](const funkgui::Rect& r) { return menuOpenAt(r); });
        fi.setNotice([this](const std::string& text) { notice(text); });
        // The status line's Panel switch: the panel under the code, for the tab shown (none in the code window, which
        // has no panel: the editor's window shows it).
        if (role_ == Role::code)
            fi.assist().setPanelSwitch({}, {});
        else
            fi.assist().setPanelSwitch([this] { return codePanelShown(); }, [this] { toggleCodePanel(); });
        fi.setCircuitHooks([this]() -> const std::vector<lang::CircuitBinding>& { return circuitDials_; },
                           [this](int slot) -> funkgui::ParamPort* {
                               const int i = slot >= 0 && slot < kMaxControls ? itemOfSlot_[static_cast<std::size_t>(slot)] : -1;
                               return i >= 0 ? static_cast<DialItem&>(*items_[static_cast<std::size_t>(i)]).model().port() : nullptr;
                           },
                           [this](int slot) {
                               return slot >= 0 && slot < kMaxControls ? controls_[static_cast<std::size_t>(slot)] : Control{};
                           },
                           [this] { runNow(); });
        fi.setViewHooks([this] { persistFileViews(); }, [this] {
            if (layoutStale())
                layout();
            dirty_ = true;
        });
        // The schematic's session pins, published as its view reconciles (Claude's edit_circuit and pictures draw them),
        // never by a picture's Panel, and taken from the port when a file's view first shows (the user's places).
        if (role_ != Role::picture)
            fi.setPinsHook([this](const std::string& file, const std::string& lines) { port_.setSchematicPins(file, lines); });
        fi.setPinsSource([this](const std::string& file) { return port_.schematicPins(file); });
        // A schematic gesture's files, given to the processor at once unless a draft came that refresh has not merged
        // (Claude's edit_circuit writes only over the text it read: it then sees the gesture and draws on it again).
        fi.setSyncHook([this] {
            if (out_)
                return false;                            // the hidden copy gives nothing
            if (files().revision() == syncedFiles_)
                return true;
            Draft d = files().draft();
            if (!port_.setDraftIf(d, seenDraft_))
                return false;
            syncedFiles_ = files().revision();
            given_ = std::move(d);
            pushedFiles_ = true;
            return true;
        });
        // The schematic's live probes (SchematicView.h): the table and the id of the program that plays (refresh's),
        // the meters this frame, the rings, and this Panel's own set: an owner from its first armed set (never a
        // picture's Panel: setOwnsProbes), one EditorPort::setProbes call a change.
        {
            SchematicView::ProbeHooks h;
            h.table = [this]() -> const std::vector<circuits::Probed>& { return circuitProbes_; };
            h.program = [this] { return probeProgram_; };
            h.meters = [this]() -> const Meters& { return meters_; };
            h.samples = [this](int slot, float* out, int n) { return port_.probe(slot, out, n); };
            h.runs = [this](const std::string& file) {
                for (const SourceFile& f : running_)
                    if (fileKindOf(f.name) == FileKind::FMinus)
                        for (const std::string_view line : lang::splitLines(f.text))
                        {
                            std::string inc;
                            int a = 0, b = 0;
                            if (lang::includeOn(line, inc, a, b) && inc == file)
                                return true;
                        }
                return false;
            };
            h.arm = [this](const ProbeSet& set) {
                if (!ownsProbes_)
                    return;
                if (probeOwner_ == 0)
                {
                    if (!set.armed())
                        return;                          // nothing to clear: never an owner
                    probeOwner_ = port_.addProbeOwner();
                }
                port_.setProbes(probeOwner_, set);
            };
            fi.setProbeHooks(std::move(h));
        }
    }

    void Panel::layout()
    {
        // The Standalone window's buttons: their room, and where they go, told once while they are there.
        buttonsRoom_ = buttonsRoomNow();
        if (buttonsRoom_ > 0.0f && !buttonsPlaced_)
            port_.placeWindowButtons(kButtonsX, kTopBarY + 0.5f * kTopBarH);
        buttonsPlaced_ = buttonsRoom_ > 0.0f;
        const float W = width_, H = height_;
        const float footerTop = H - kFooterH;
        const float bottom = footerTop - kBottomGap;     // the dials' and the message box's bottom

        // The columns (Columns.h): the chat at the right, about a third of the width or as wide as its user set it (folded,
        // a rail); the explorer at the left, about a sixth or its user's width, in the CODE view only; the program's card
        // between them keeping kProgramMinW (the explorer gives way first, then the chat; then the explorer folds away:
        // folded, too narrow a window or the PANEL view leave nothing at the left but its button in the program's card's
        // header).
        {
            columns::Asked asked;
            asked.window = W;
            asked.explorer = explorerOpen_ && !full();
            asked.chat = chatOpen_;
            asked.noChat = !hasChat();                   // the code window: the explorer and the program's card alone
            asked.explorerWant = explorerWant_;
            asked.chatWant = chatWant_;
            fit_ = columns::fit(asked);
            // The chat as wide in the PANEL view as in the CODE view, where the explorer can take some of its room (the
            // give-way order): a switch between them then never changes the conversation's width, which would lay out
            // every block of it again (ChatView::setBounds; tens to hundreds of ms for a long one, at each switch). Its
            // edge's reach is the CODE view's too. Not while the editor is in its own window, which has the explorer.
            if (full() && !out_ && asked.chat && explorerOpen_)
            {
                columns::Asked code = asked;
                code.explorer = true;
                const columns::Fit c = columns::fit(code);
                fit_.chat = c.chat;
                fit_.chatLeast = c.chatLeast;
                fit_.chatMost = c.chatMost;
            }
        }
        chatW_ = fit_.chat;
        chatX_ = W - kMargin - chatW_;
        const float explorerW = fit_.explorer;
        explorerShown_ = explorerW > 0.0f;
        explorerCard_ = explorerShown_ ? funkgui::Rect{ kMargin, kMainY, explorerW, bottom - kMainY } : funkgui::Rect{};
        leftX_ = kMargin + (explorerShown_ ? explorerW + kExplorerGap : 0.0f);
        leftW_ = chatX_ - (!hasChat() ? 0.0f : chatOpen_ ? kChatGap : kChatRailGap) - leftX_;

        // The top bar: the project in its middle (clear of the product's name), the view's switch, the load and the
        // theme at its right.
        const auto at = [this](int i) -> ButtonItem& { return static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(i)]); };
        const float barMid = kTopBarY + 0.5f * kTopBarH;
        const float right = W - kMargin;
        at(themeItem_).setBounds({ right - 8.0f - kIconButton, barMid - 0.5f * kIconButton, kIconButton, kIconButton });
        const float segRight = right - 8.0f - kIconButton - 92.0f;        // the load's text between them
        at(panelViewItem_).setBounds({ segRight - 3.0f - kSegmentW, barMid - 0.5f * (kTopButtonH - 6.0f), kSegmentW, kTopButtonH - 6.0f });
        at(viewItem_).setBounds({ segRight - 3.0f - 2.0f * kSegmentW, barMid - 0.5f * (kTopButtonH - 6.0f), kSegmentW, kTopButtonH - 6.0f });
        // The window button left of the switch's track, while there is a code window to have.
        const float windowW = windowButtonShown() ? kIconButton + 8.0f : 0.0f;
        at(windowItem_).setBounds({ segRight - 3.0f - 2.0f * kSegmentW - 3.0f - 8.0f - kIconButton, barMid - 0.5f * kIconButton,
                                    kIconButton, kIconButton });
        {
            // The project's pill and, while a project is open, its presets right of it (the arrows and the name), the
            // two centred as one between the product's name and the switch: the pill gives way first, then the name.
            const funkgui::FontAtlasSdf& atlas = funkgui::FontService::get().atlas();
            const float nameRight = kMargin + 50.0f + buttonsRoom_ + std::max(funkgui::text::width(atlas, product::kName, chat::kStrong),
                                                               funkgui::text::width(atlas, subtitle_.c_str(), chat::kSmall));
            const float segLeft = segRight - 3.0f - 2.0f * kSegmentW - 3.0f - windowW;
            const float room = segLeft - 24.0f - (nameRight + 24.0f);
            const bool presets = presetsShown();
            const float extras = presets ? kPresetGap + 2.0f * kPresetArrowW + 2.0f * kPresetArrowGap : 0.0f;
            const float nameW = presets ? std::clamp(std::round(0.36f * (room - extras)), kPresetNameLeast, kPresetNameW) : 0.0f;
            const float w = std::min(kProjectW, std::max(presets ? kProjectLeast : 120.0f, room - extras - nameW));
            const float total = w + extras + nameW;
            const float x = std::clamp(std::round(0.5f * (W - total)), nameRight + 24.0f, std::max(nameRight + 24.0f, segLeft - 24.0f - total));
            at(projectsItem_).setBounds({ x, barMid - 0.5f * kTopButtonH, w, kTopButtonH });
            float px = x + w + kPresetGap;
            at(presetPrevItem_).setBounds({ px, barMid - 0.5f * kTopButtonH, kPresetArrowW, kTopButtonH });
            px += kPresetArrowW + kPresetArrowGap;
            at(presetItem_).setBounds({ px, barMid - 0.5f * kTopButtonH, nameW, kTopButtonH });
            px += nameW + kPresetArrowGap;
            at(presetNextItem_).setBounds({ px, barMid - 0.5f * kTopButtonH, kPresetArrowW, kTopButtonH });
            picker_.setAnchor(at(presetItem_).bounds());
            picker_.setSize(W, H);
        }

        // The program's card's header: the actions at its right (Run last), the tabs from its left.
        const float inX = leftX_ + kInset, inW = leftW_ - 2.0f * kInset;
        const float ay = kActionsY + 0.5f * (kActionH - 26.0f);
        float ax = leftX_ + leftW_ - kInset;
        for (const int i : { runItem_, randomItem_, libraryItem_, examplesItem_ })
        {
            ButtonItem& b = at(i);
            // While the editor is out its window keeps Run (the code window's files): the code window has the rest.
            b.setHidden(out_ && i != runItem_);
            if (!b.shown())
                continue;
            b.setBounds({ 0.0f, ay, 10.0f, 26.0f });     // its height first: its width follows its label
            const float w = b.preferredWidth();
            ax -= w;
            b.setBounds({ ax, ay, w, 26.0f });
            ax -= i == runItem_ ? 10.0f : 4.0f;
        }
        actionsX_ = ax;
        // The dials: in plain rows, or in the sections the program's #pragma panel lines ask for (DialLayout.h), as
        // tall as the panel can give them while the editor keeps 120 px. A netlist's Schematic view keeps more: its
        // editor at least E_min = max(312, 55 % of the column's height), the problems list hidden (the drawing's badges
        // and the info strip's count carry them), and the dials, laid out in what is left, folded into a 24 px bar when
        // they do not fit ("32 DIALS   SHOW": shown small then, the editor keeping 192 px, or less, down to its 80 px
        // floor, for rows that need it: every dial shows unless even that is too little, which the bar then says).
        // Under the code the bar comes only when the dials, all small, would leave the editor less than its 80 px
        // floor over the problems list (64 in the least window): SHOW then shows the rows under that line.
        const float problemsH = kProblemRowH * static_cast<float>(kProblemRows);
        const float inBottom = bottom - kInset;          // the program's card's inside
        const bool schematic = !full() && files().schematicShown();
        layoutSchematic_ = schematic;
        // The panel under the code hidden (the CODE view, the choice for the tab shown; always in the code window, whose
        // panel the editor's window shows): no dials, no displays, no interface and no fold's bar; the editor (or the
        // drawing) has the column.
        const bool hidden = !full() && (role_ == Role::code || !codePanelShown());
        panelHidden_ = hidden;
        const float columnH = inBottom - kEditorY;
        const float eMin = std::max(312.0f, std::round(0.55f * columnH));
        const float roomForDials = schematic ? columnH - eMin - 8.0f : inBottom - kEditorY - 120.0f - 8.0f - problemsH - 16.0f;
        constexpr float kDialsBarH = 24.0f, kSchematicKeeps = 192.0f, kEditorFloor = 80.0f;
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
        measure.readout = [](const std::string& text) {
            return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(), looks::kReadout);
        };
        // A .gui file's interface (GuiLayout.h) in place of the dials: under the editor as tall as it is (up to the
        // room the editor leaves, scrolling past that), or across the whole column in the PANEL view.
        const bool gui = !panelSpec_.gui.empty();
        float panelTop = inBottom;
        int used = 0;                                    // the dials the program has
        for (int c = 0; c < kMaxControls; ++c)
            used += controls_[static_cast<std::size_t>(c)].used ? 1 : 0;
        dialsBar_ = false;
        dialsFolded_ = false;
        float hideAbove = -1.0e9f;                       // the dials shown from the fold: rows over the editor's room hidden
        if (hidden)
        {
            guiView_->clear(panelSpec_);                 // its pages and scroll kept for when it shows again
            dials_ = DialsLayout{};
            dials_.top = inBottom;
            panelTop = schematic ? inBottom + 8.0f : inBottom;
            for (int d = 0; d < kMaxDisplays; ++d)
            {
                auto& item = static_cast<DisplayItem&>(*items_[static_cast<std::size_t>(itemOfDisplay_[static_cast<std::size_t>(d)])]);
                item.setDisplay(nullptr);
                item.setCell({});
                item.setArea({});
            }
        }
        else if (gui)
        {
            const GuiMeasure gm = [](const std::string& text, const funkgui::TextStyle& st) {
                return funkgui::text::width(funkgui::FontService::get().atlas(), text.c_str(), st);
            };
            // Under the editor it takes half the column at most (the editor keeps the rest), and scrolls past that.
            const GuiExtent natural = naturalGuiSize(panelSpec_, controls_, gm, inW);   // a row that wraps: as wide
            const float h = std::max(40.0f, std::min({ natural.h, roomForDials, schematic ? roomForDials : 0.5f * (inBottom - kEditorY) }));
            const funkgui::Rect area = full() ? funkgui::Rect{ inX, kTabsY, inW, inBottom - kTabsY }
                                              : funkgui::Rect{ inX, inBottom - h, inW, h };
            guiView_->layout(panelSpec_, controls_, area);   // the items placed too
            dials_ = DialsLayout{};
            dials_.top = area.y;
            panelTop = area.y;
        }
        else
        {
            guiView_->clear(panelSpec_);
            if (full())
            {
                // The PANEL view: the dials from the column's top, as tall as they would be with all its room (small
                // rows closing up to fit it: 64 dials in the least window, eight rows of eight; it has no fold).
                const DialsLayout first = layoutDials(panelSpec_, controls_, measure, inX, inW, inBottom, inBottom - kTabsY, 0.0f, kTightRowGap);
                dials_ = layoutDials(panelSpec_, controls_, measure, inX, inW, kTabsY + (inBottom - first.top), inBottom - kTabsY, 0.0f,
                                     kTightRowGap);
            }
            else
                dials_ = layoutDials(panelSpec_, controls_, measure, inX, inW, inBottom, roomForDials, schematic ? kTightGap : 0.0f);
            panelTop = dials_.top;
            // The code view folds them into the bar too when, all small, they would leave the editor less than its
            // 80 px floor over the problems list (64 dials in the least window): never over the editor, the tabs or
            // the actions.
            const float keepsFloor = kEditorY + kEditorFloor + 8.0f + problemsH + 16.0f;
            const bool codeFold = !full() && !schematic && (used > 0 || !panelSpec_.displays.empty()) && dials_.top < keepsFloor - 0.5f;
            if ((schematic && used > 0 && inBottom - dials_.top > roomForDials + 0.5f) || codeFold)
            {
                dialsBar_ = true;
                const float barTop = inBottom - kDialsBarH;
                if (dialsOpen_ && codeFold)
                {
                    // shown small over the bar; the rows over the editor's floor and the problems list are hidden (the
                    // bar says how many: the PANEL view has all), and the list sits over the first row shown, the
                    // editor taking the rest
                    dials_ = layoutDials(panelSpec_, controls_, measure, inX, inW, barTop - 8.0f, roomForDials - kDialsBarH - 8.0f);
                    hideAbove = keepsFloor;
                    panelTop = barTop;
                    for (int c = 0; c < kMaxControls; ++c)
                        if (const float t = dials_.dials[static_cast<std::size_t>(c)].top; controls_[static_cast<std::size_t>(c)].used && t >= hideAbove)
                            panelTop = std::min(panelTop, t);
                    for (std::size_t d = 0; d < panelSpec_.displays.size() && d < dials_.displays.size(); ++d)
                        if (dials_.displays[d].top >= hideAbove)
                            panelTop = std::min(panelTop, dials_.displays[d].top);
                    for (const SectionFrame& f : dials_.sections)
                        if (f.r.y >= hideAbove)
                            panelTop = std::min(panelTop, std::max(hideAbove, f.r.y - kSectionTitleTop));
                }
                else if (dialsOpen_)
                {
                    // shown small for the room the editor's 192 px leave; rows past it take the editor's room down to
                    // its 80 px floor, and only rows past that are hidden (the bar says how many: the PANEL view has all)
                    dials_ = layoutDials(panelSpec_, controls_, measure, inX, inW, barTop - 8.0f,
                                         columnH - kSchematicKeeps - 8.0f - kDialsBarH - 8.0f, kTightGap);
                    hideAbove = kEditorY + kEditorFloor + 8.0f;
                    panelTop = std::max(dials_.top, hideAbove);
                }
                else
                {
                    dialsFolded_ = true;
                    panelTop = barTop;
                }
                // what is folded, or over the editor's room, draws no section's frame
                std::erase_if(dials_.sections, [&](const SectionFrame& f) { return dialsFolded_ || f.r.y < hideAbove; });
            }
            else if (schematic && used == 0 && panelSpec_.displays.empty())
                panelTop = inBottom + 8.0f;              // nothing under the drawing: it takes the column
            for (int d = 0; d < kMaxDisplays; ++d)
            {
                auto& item = static_cast<DisplayItem&>(*items_[static_cast<std::size_t>(itemOfDisplay_[static_cast<std::size_t>(d)])]);
                const bool has = d < static_cast<int>(panelSpec_.displays.size()) && !dialsFolded_
                              && dials_.displays[static_cast<std::size_t>(d)].top >= hideAbove;
                item.setDisplay(has ? &panelSpec_.displays[static_cast<std::size_t>(d)] : nullptr);
                item.setCell(dials_.displays[static_cast<std::size_t>(d)]);
                item.setArea({});
            }
        }
        const float problemsY = panelTop - 16.0f - problemsH;
        files().setHidden(full());
        static_cast<ProblemsItem&>(*items_[static_cast<std::size_t>(problemsItem_)]).setHidden(full() || schematic);
        const float editorH = schematic ? std::max(kEditorFloor, panelTop - 8.0f - kEditorY) : std::max(kEditorFloor, problemsY - 8.0f - kEditorY);
        // Hidden in the PANEL view, the editors keep the CODE view's place: a width set every switch (the column is
        // wider without the explorer) would measure every line of every tab's text and of the IR views' again
        // (TextEditor::setBounds), both ways. Placed once at least, for a panel that opens in the PANEL view.
        if (!full() || !filesPlaced_)
        {
            files().setGeometry({ inX, kTabsY, inW, kTabH }, { inX, kEditorY, inW, editorH });
            filesPlaced_ = true;
        }
        static_cast<ProblemsItem&>(*items_[static_cast<std::size_t>(problemsItem_)])
            .setBounds({ inX, problemsY, inW, problemsH });
        int hiddenDials = 0;                             // shown from the fold, but over the editor's floor
        for (int c = 0; c < kMaxControls; ++c)
            hiddenDials += dialsBar_ && !dialsFolded_ && controls_[static_cast<std::size_t>(c)].used
                                   && dials_.dials[static_cast<std::size_t>(c)].top < hideAbove ? 1 : 0;
        const int displays = std::min(static_cast<int>(panelSpec_.displays.size()), kMaxDisplays);
        if (used == 0)                                   // a panel of displays alone: the bar counts those
            for (int d = 0; d < displays; ++d)
                hiddenDials += dialsBar_ && !dialsFolded_ && dials_.displays[static_cast<std::size_t>(d)].top < hideAbove ? 1 : 0;
        static_cast<DialsBarItem&>(*items_[static_cast<std::size_t>(dialsBarItem_)])
            .place({ inX, inBottom - kDialsBarH, inW, kDialsBarH }, used, displays, hiddenDials, dialsOpen_, dialsBar_, schematic);
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0)
            {
                auto& dial = static_cast<DialItem&>(*items_[static_cast<std::size_t>(i)]);
                dial.setLook(panelSpec_.styles[static_cast<std::size_t>(c)], panelSpec_.colors[static_cast<std::size_t>(c)],
                             dials_.dials[static_cast<std::size_t>(c)]);
                dial.setHidden(gui || hidden || dialsFolded_ || dials_.dials[static_cast<std::size_t>(c)].top < hideAbove);
            }

        // The chat's card.
        card_ = { chatX_, kMainY, chatW_, bottom - kMainY };
        programCard_ = { leftX_, kMainY, leftW_, bottom - kMainY };
        layoutChat();
        layoutExplorer();
        // The edges a user drags: the explorer's right one while it shows, the chat's left one while it is open.
        static_cast<columns::EdgeItem&>(*items_[static_cast<std::size_t>(explorerEdgeItem_)])
            .place(explorerCard_.right(), kMainY, bottom, explorerShown_);
        static_cast<columns::EdgeItem&>(*items_[static_cast<std::size_t>(chatEdgeItem_)])
            .place(card_.x, kMainY, bottom, chatOpen_ && hasChat());
        // An edge that hides (its card folded, or away for the window or the view) gives up the focus: no key reaches it.
        if (isEdge(focus_) && !items_[static_cast<std::size_t>(focus_)]->shown())
            setFocus(-1);

        // The zoom, in the status bar's right corner.
        {
            ButtonItem& z = at(zoomItem_);
            z.setBounds({ 0.0f, H - kFooterH + 4.0f, 10.0f, kFooterH - 8.0f });
            const float w = z.preferredWidth();
            z.setBounds({ right - w, H - kFooterH + 4.0f, w, kFooterH - 8.0f });
        }
        placeMutedNotice();
        placeRecordNotice();
        placeMeter();
        ++layoutRevision_;
        ++layouts_;
        dirty_ = true;
    }

    void Panel::placeMeter()
    {
        const funkgui::FontAtlasSdf& atlas = funkgui::FontService::get().atlas();
        const funkgui::Rect theme = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(themeItem_)]).bounds();
        const MeterPlaces m = meterPlaces(theme.x, funkgui::text::width(atlas, meterLoad_.c_str(), chat::kSmall), frozenShown_,
                                          funkgui::text::width(atlas, "   ", chat::kSmall));
        auto& flake = static_cast<FrozenItem&>(*items_[static_cast<std::size_t>(frozenItem_)]);
        flake.place(m.flakeX, kTopBarY + 0.5f * kTopBarH);
        flake.setShown(frozenShown_);
    }

    funkgui::Point Panel::centreOfFrozen() const { return centreOf(frozenItem_); }

    // The chat's card: the header, the conversation, the message box (as tall as its text), its bar's buttons (as wide
    // as their labels) and the usage line.
    void Panel::layoutChat()
    {
        const float inX = card_.x + kCardPad, inW = card_.w - 2.0f * kCardPad;
        const float hy = card_.y + kCardPad;
        chatsLabel_ = chatsLabel() + '|' + chatTitle();  // what the chats' pill was placed for (folded too: refreshChat)
        // Folded: every item of the chat hidden but the rail's button, at its top. The code window has no chat at all.
        const bool open = chatOpen_ && hasChat();
        for (const int i : { newChatItem_, keyItem_, chatsItem_, modelItem_, thinkingItem_, fastItem_, webItem_, attachItem_, sendItem_,
                             desktopItem_ })
            static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(i)]).setHidden(!open);
        chatView().setHidden(!open);
        inputItem().setHidden(!open);
        auto& toggle = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(chatToggleItem_)]);
        toggle.setHidden(!hasChat());
        if (!open)
        {
            toggle.setBounds({ card_.centreX() - 0.5f * kIconButton, card_.y + 8.0f, kIconButton, kIconButton });
            headerY_ = hy;
            placeDesktopBadge();                         // its mark on the rail
            ++layoutRevision_;
            dirty_ = true;
            return;
        }
        // The header's buttons, right to left: fold, the key, a new chat.
        const float fx = card_.right() - kCardPad - kIconButton;
        toggle.setBounds({ fx, hy, kIconButton, kIconButton });
        static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(keyItem_)])
            .setBounds({ fx - kIconButton - 4.0f, hy, kIconButton, kIconButton });
        static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(newChatItem_)])
            .setBounds({ fx - 2.0f * kIconButton - 8.0f, hy, kIconButton, kIconButton });
        headerY_ = hy;
        placeDesktopBadge();                             // and the chats' pill after the title, up to it
        const float headerBottom = hy + kIconButton;

        InputItem& in = inputItem();
        in.setBounds({ inX, card_.y, inW, 200.0f }, kBarH);   // its width first: the rows depend on it
        composerH_ = in.desiredHeight(inW, kBarH);
        usageY_ = card_.bottom() - kCardPad + 2.0f - kUsageH;
        const float composerY = usageY_ - 4.0f - composerH_;
        in.setBounds({ inX, composerY, inW, composerH_ }, kBarH);
        const funkgui::Rect viewWas = chatView().bounds();
        chatView().setBounds({ card_.x + 2.0f, headerBottom + 6.0f, card_.w - 4.0f,
                               std::max(40.0f, composerY - 8.0f - (headerBottom + 6.0f)) });
        // The conversation laid out for its place now, when it moved (refreshChat shows it whenever it changes, folded
        // or not): a layout that leaves it where it was (a switch of the view) builds nothing of it again.
        const funkgui::Rect viewIs = chatView().bounds();
        if (viewIs.x != viewWas.x || viewIs.y != viewWas.y || viewIs.w != viewWas.w || viewIs.h != viewWas.h)
            reshowChat();
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
        const bool withFast = chatTakesFast(), withWeb = chatTakesWeb(), withThinking = chatTakesThinking();
        const float fw = withFast ? fast.preferredWidth() + 4.0f : 0.0f;
        const float ww = withWeb ? web.preferredWidth() + 4.0f : 0.0f;
        const float room = bar.right() - 6.0f - kSendD - 8.0f - fw - ww - x0;
        float mw = model.preferredWidth(), tw = withThinking ? thinking.preferredWidth() : -4.0f;
        if (withThinking && mw + 4.0f + tw > room)
            tw = std::max(64.0f, room - 4.0f - mw);
        if (mw + 4.0f + tw > room)
            mw = std::max(64.0f, room - 4.0f - tw);
        model.setBounds({ x0, y, mw, kPillH });
        thinking.setBounds({ x0 + mw + 4.0f, y, std::max(0.0f, tw), kPillH });
        fast.setBounds({ x0 + mw + 4.0f + tw + 4.0f, y, withFast ? fw - 4.0f : 0.0f, kPillH });
        web.setBounds({ x0 + mw + 4.0f + tw + 4.0f + fw, y, withWeb ? ww - 4.0f : 0.0f, kPillH });
        send.setBounds({ bar.right() - 6.0f - kSendD, bar.y + std::floor((bar.h - kSendD) * 0.5f) - 3.0f, kSendD, kSendD });
        barLabels_ = barLabels();
    }

    void Panel::resize(int w, int h)
    {
        const float nw = static_cast<float>(std::max(w, minWidth()));
        const float nh = static_cast<float>(std::max(h, minHeight()));
        if (nw == width_ && nh == height_)
            return;
        width_ = nw;
        height_ = nh;
        layout();
    }

    // The host closes the open gestures before it lets go of the Panel; no HostServices call from here.
    Panel::~Panel()
    {
        keepStepDefaults(0.0, true);                     // a width stepped just before the editor closed
        guiView_->releaseKeys();                         // a note held as the editor closes is let go
        releasePictures();
        if (probeOwner_ != 0)
            port_.removeProbeOwner(probeOwner_);         // its probes go with it (another Panel's set takes over)
        if (reading_)
            port_.removeMeterReader();
        // The other window's Panel (while the editor is out) forgets this one, and nothing else of it is touched: the
        // owner lets both go together (the code window first), or takes the editor back before the code window goes.
        if (peer_ != nullptr && peer_->peer_ == this)
            peer_->peer_ = nullptr;
        if (role_ == Role::editor && out_)
            port_.removeCodeWindow();                    // its code window has gone first (the session keeps it out)
        if (irViewing_)
            port_.removeIrViewer();                      // its IR tab goes with it: nothing kept for it
    }

    // ---- MIDI learn ---------------------------------------------------------------------------------------------------
    void Panel::syncMidi()
    {
        if (const uint64_t r = port_.midiRevision(); r == midiSeen_)
            return;
        const midilearn::View v = port_.midi();
        midiSeen_ = v.revision;
        for (int c = 0; c < kMaxControls; ++c)
        {
            const auto i = static_cast<std::size_t>(c);
            const bool learning = v.learning == c;
            if (const int item = itemOfSlot_[i]; item >= 0)
                static_cast<DialItem&>(*items_[static_cast<std::size_t>(item)]).setMidi(v.words[i], learning, learning ? v.note : std::string());
            guiView_->setMidi(c, !v.words[i].empty(), learning, midiSpec(v.words[i], learning, learning ? v.note : std::string()));
        }
        dirty_ = true;
    }

    void Panel::dialMenu(int slot, funkgui::Point at)
    {
        if (host_ == nullptr || slot < 0 || slot >= kMaxControls)
            return;
        const int item = itemOfSlot_[static_cast<std::size_t>(slot)];
        funkgui::ParamPort* param = item >= 0 ? static_cast<DialItem&>(*items_[static_cast<std::size_t>(item)]).model().port() : nullptr;
        // A host's own parameter menu (its automation, its own MIDI learn): a host parameter's, in a host's window (the
        // Standalone has none to show).
        const bool hostMenu = param != nullptr && !isPanelDial(slot) && port_.windowButtons().width <= 0.0f;
        if (!port_.midiLearnable())
        {
            if (hostMenu)
                host_->showParamMenu(*param, at.x, at.y);   // an effect's dials: the host's menu, as before
            return;
        }
        const Control& k = controls_[static_cast<std::size_t>(slot)];
        if (!k.used)
            return;
        const midilearn::View v = port_.midi();
        const std::string& words = v.words[static_cast<std::size_t>(slot)];
        const bool learning = v.learning == slot;
        const int mapped = static_cast<int>(v.mappings.size());
        enum : int { kLearn = 1, kStop, kForget, kForgetAll, kHost };
        funkgui::MenuRequest request;
        funkgui::MenuItem title;
        title.header = true;
        title.label = upper(asciiText(k.name)) + "   MIDI";
        request.items.push_back(title);
        request.items.push_back(learning ? funkgui::MenuItem{ kStop, "Stop learning" }
                                         : funkgui::MenuItem{ kLearn, words.empty() ? "Learn MIDI CC" : "Learn another MIDI CC" });
        request.items.push_back({ kForget, words.empty() ? std::string("Forget MIDI CC") : "Forget " + words, !words.empty() });
        funkgui::MenuItem gap;
        gap.separator = true;
        request.items.push_back(gap);
        request.items.push_back({ kForgetAll, mapped > 0 ? "Forget every MIDI CC (" + std::to_string(mapped) + ")" : std::string("Forget every MIDI CC"),
                                  mapped > 0 });
        if (hostMenu)
        {
            request.items.push_back(gap);
            request.items.push_back({ kHost, "The host's menu..." });
        }
        request.anchor = { at.x, at.y, 1.0f, 1.0f };
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this, slot, at, param](int chosen) {
            switch (chosen)
            {
                case kLearn:     (void)port_.learnMidi(slot); break;
                case kStop:      port_.cancelMidiLearn(); break;
                case kForget:    (void)port_.forgetMidi(slot); break;
                case kForgetAll: (void)port_.forgetAllMidi(); break;
                case kHost:
                    if (param != nullptr && host_ != nullptr)
                        host_->showParamMenu(*param, at.x, at.y);
                    break;
                default: return;
            }
            syncMidi();
            dirty_ = true;
        });
    }

    std::string Panel::dialSpecLine(int slot) const
    {
        const int item = slot >= 0 && slot < kMaxControls ? itemOfSlot_[static_cast<std::size_t>(slot)] : -1;
        if (item < 0)
            return {};
        char line[256] = {};
        items_[static_cast<std::size_t>(item)]->specLine(line, sizeof line);
        return line;
    }

    bool Panel::dialMidiMarked(int slot) const
    {
        const int item = slot >= 0 && slot < kMaxControls ? itemOfSlot_[static_cast<std::size_t>(slot)] : -1;
        return item >= 0 && static_cast<const DialItem&>(*items_[static_cast<std::size_t>(item)]).midiMarked();
    }

    bool Panel::dialMidiLearning(int slot) const
    {
        const int item = slot >= 0 && slot < kMaxControls ? itemOfSlot_[static_cast<std::size_t>(slot)] : -1;
        return item >= 0 && static_cast<const DialItem&>(*items_[static_cast<std::size_t>(item)]).midiLearning();
    }

    // ---- the folder (plugin/FolderSync.h) ----------------------------------------------------------------------------
    // What the port says of the folder, read again when its revision moves; a save that landed and a new conflict say
    // so in the status (not what this Panel found when it was made).
    void Panel::syncFolder()
    {
        const uint64_t r = port_.folderRevision();
        if (folderKnown_ && r == folderSeen_)
            return;
        FolderState now = port_.folder();
        if (folderKnown_ && now.on)
        {
            if (now.landedCount != folderState_.landedCount && !now.landed.empty())
                notice("FROM THE FOLDER: " + upper(now.landed));
            for (const std::string& file : now.conflicts)
                if (std::find(folderState_.conflicts.begin(), folderState_.conflicts.end(), file) == folderState_.conflicts.end())
                {
                    notice(upper(file) + ": CHANGED HERE AND IN THE FOLDER");
                    break;
                }
        }
        folderState_ = std::move(now);
        folderSeen_ = r;
        folderKnown_ = true;
        explorerStale_ = true;
        dirty_ = true;
    }

    void Panel::folderRow(const ExplorerItem::Row& row)
    {
        std::string why;
        switch (row.index)
        {
            case kFolderOn:
                pushEditor();                            // the folder starts from what the tabs show
                if (port_.setFolderSync(true, why))
                    notice("FOLDER: " + upper(port_.folder().name));
                else
                    notice("NO FOLDER: " + upper(why));
                break;
            case kFolderCode:
                if (!port_.openFolder(FolderApp::vscode, why))
                    notice("NOT OPENED: " + upper(why));
                break;
            case kFolderShow:
                if (!port_.openFolder(FolderApp::files, why))
                    notice("NOT OPENED: " + upper(why));
                break;
            case kFolderRun:
                port_.setFolderRunOnSave(!folderState_.runOnSave);
                break;
            case kFolderConflict:
                chooseConflict(row.key);
                break;
            default:
                break;
        }
        syncFolder();
    }

    // A conflict: which side to keep (a menu at the pointer); the other goes to the folder's .conflicts/.
    void Panel::chooseConflict(const std::string& file)
    {
        if (host_ == nullptr)
            return;
        pushEditor();                                    // the plugin's side is what the tabs show
        funkgui::MenuRequest request;
        request.items.push_back({ 1, "Keep the plugin's " + file + " (the folder's to .conflicts)" });
        request.items.push_back({ 2, "Keep the folder's " + file + " (the plugin's to .conflicts)" });
        request.anchor = { pointer_.x, pointer_.y, 1.0f, 1.0f };
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this, file](int chosen) {
            if (chosen != 1 && chosen != 2)
                return;
            pushEditor();
            std::string why;
            if (port_.resolveFolderConflict(file, chosen == 1, why))
                notice(chosen == 1 ? "KEPT THE PLUGIN'S " + upper(file) : "KEPT THE FOLDER'S " + upper(file));
            else
                notice("NOT KEPT: " + upper(why));
            syncFolder();
            dirty_ = true;
        });
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
        return std::any_of(paths.begin(), paths.end(), [this](const std::string& p) {
            return (chatOpen_ && hasChat() && isPictureName(p)) || isProjectFile(p) || isAudioPath(p);
        });
    }

    // The ASSETS header's +: audio files to add as samples (EditorPort::addAssets), decoded on this thread.
    void Panel::openSamples()
    {
        if (host_ == nullptr)
            return;
        funkgui::FileRequest request;
        request.mode = funkgui::FileRequest::Mode::openMany;
        request.title = "Add samples";
        request.pattern = "*.wav;*.wave;*.aif;*.aiff;*.flac";
        host_->chooseFiles(request, [this](const std::vector<std::string>& paths) { addSamples(paths); });
    }

    void Panel::addSamples(const std::vector<std::string>& paths)
    {
        if (paths.empty())
            return;
        std::string why;
        const int added = port_.addAssets(paths, why);
        if (!why.empty())
            notice((added > 0 ? "ADDED " + std::to_string(added) + "; NOT ADDED: " : std::string("NOT ADDED: ")) + upper(why));
        else
            notice(added == 1 ? std::string("SAMPLE ADDED: #PRAGMA SAMPLE READS IT") : "SAMPLES ADDED: " + std::to_string(added));
        explorerStale_ = true;
        dirty_ = true;
    }

    void Panel::filesDragEnter(const std::vector<std::string>&, float x, float y)
    {
        noteInput();
        dropActive_ = true;
        dropHover_ = card_.contains({ x, y });
        dirty_ = true;
    }

    void Panel::filesDragMove(float x, float y)
    {
        noteInput();
        const bool over = card_.contains({ x, y });
        if (over != dropHover_)
        {
            dropHover_ = over;
            dirty_ = true;
        }
    }

    void Panel::filesDragExit()
    {
        noteInput();
        dropActive_ = dropHover_ = false;
        dirty_ = true;
    }

    void Panel::filesDropped(const std::vector<std::string>& paths)
    {
        noteInput();
        const bool onChat = dropHover_;
        dropActive_ = dropHover_ = false;
        dirty_ = true;
        std::vector<std::string> projects, samples;
        for (const std::string& p : paths)
            if (isProjectFile(p))
                projects.push_back(p);
            else if (isAudioPath(p))
                samples.push_back(p);
        if (!projects.empty())
            importFiles(projects);                       // anywhere on the panel
        addSamples(samples);                             // anywhere too: the explorer's ASSETS
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
    funkgui::Point Panel::centreOfChats() const { return centreOf(chatsItem_); }
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
        if (!explorerShown_ && chatOpen_ && hasChat())   // the code window has no chat to fold
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
            explorerStale_ = true;
        }
        layout();
        dirty_ = true;
    }

    funkgui::Rect panelViewArea(float W, float H, bool chatOpen, float chatWant, bool explorerOpen, float explorerWant)
    {
        // layout()'s arithmetic in the PANEL view (full()): no explorer, the chat (or its rail) at the right, as wide
        // as the CODE view has it when the explorer is open there.
        columns::Asked asked;
        asked.window = W;
        asked.explorer = false;
        asked.chat = chatOpen;
        asked.chatWant = chatWant;
        asked.explorerWant = explorerWant;
        columns::Fit fit = columns::fit(asked);
        if (chatOpen && explorerOpen)
        {
            columns::Asked code = asked;
            code.explorer = true;
            fit.chat = columns::fit(code).chat;
        }
        const float leftX = kMargin;
        const float leftW = W - kMargin - fit.chat - (chatOpen ? kChatGap : kChatRailGap) - leftX;
        const float inBottom = H - kFooterH - kBottomGap - kInset;
        return { leftX + kInset, kTabsY, leftW - 2.0f * kInset, inBottom - kTabsY };
    }

    // ---- the columns' widths (Columns.h) ------------------------------------------------------------------------------
    float Panel::wantedWidth(int session, bool chat) const
    {
        if (session > 0)
            return static_cast<float>(session);
        // None set in this instance: this Mac's default (the last width a user set in any instance), else the share.
        const int most = static_cast<int>(chat ? columns::kChatMaxW : columns::kExplorerMaxW);
        return static_cast<float>(
            funkgui::UiPreferences::get().getInt(chat ? columns::kChatWidthKey : columns::kExplorerWidthKey, 0, 0, most));
    }

    void Panel::setColumnWidth(bool chat, float px, columns::Keep keep)
    {
        const int w = columns::keptWidth(px, chat);
        (chat ? chatWant_ : explorerWant_) = static_cast<float>(w);
        if (keep != columns::Keep::notYet)
        {
            // The session keeps it, and this Mac, as the default for an instance that has set none (0: the share): a
            // gesture's end at once, a step once the steps pause (keepStepDefaults: one write for a held arrow).
            (chat ? seenChatWidth_ : seenExplorerWidth_) = w;
            if (chat)
                port_.setChatWidth(w);
            else
                port_.setExplorerWidth(w);
            int& pending = stepDefault_[chat ? 1 : 0];
            if (keep == columns::Keep::now)
            {
                pending = -1;
                funkgui::UiPreferences::get().setInt(chat ? columns::kChatWidthKey : columns::kExplorerWidthKey, w);
            }
            else
            {
                pending = w;
                stepAt_ = host_ != nullptr ? host_->nowSeconds() : 0.0;
            }
        }
        layout();
        dirty_ = true;
    }

    void Panel::keepStepDefaults(double nowSec, bool now)
    {
        if (stepDefault_[0] < 0 && stepDefault_[1] < 0)
            return;
        if (!now && nowSec - stepAt_ < columns::kDefaultPause && nowSec >= stepAt_)
            return;
        for (int k = 0; k < 2; ++k)
            if (stepDefault_[k] >= 0)
            {
                funkgui::UiPreferences::get().setInt(k == 1 ? columns::kChatWidthKey : columns::kExplorerWidthKey,
                                                     stepDefault_[k]);
                stepDefault_[k] = -1;
            }
    }

    // The port's widths, when another set them since (a second Panel of this processor, a host loading a session): a
    // step of this Panel's not yet in this Mac's default is then no longer the last width set, and goes.
    void Panel::syncColumnWidths()
    {
        const int e = port_.explorerWidth(), c = port_.chatWidth();
        if (e == seenExplorerWidth_ && c == seenChatWidth_)
            return;
        if (e != seenExplorerWidth_)
        {
            explorerWant_ = wantedWidth(e, false);
            stepDefault_[0] = -1;
        }
        if (c != seenChatWidth_)
        {
            chatWant_ = wantedWidth(c, true);
            stepDefault_[1] = -1;
        }
        seenExplorerWidth_ = e;
        seenChatWidth_ = c;
        layout();
    }

    bool Panel::explorerEdgeShown() const { return items_[static_cast<std::size_t>(explorerEdgeItem_)]->shown(); }
    bool Panel::chatEdgeShown() const { return items_[static_cast<std::size_t>(chatEdgeItem_)]->shown(); }
    funkgui::Point Panel::centreOfExplorerEdge() const { return centreOf(explorerEdgeItem_); }
    funkgui::Point Panel::centreOfChatEdge() const { return centreOf(chatEdgeItem_); }

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
        {
            explorerAway_ = true;
            return;
        }
        const double now = host_ != nullptr ? host_->nowSeconds() : 0.0;
        if (explorerAway_)
        {
            // Shown again (the CODE view, the explorer opened, a wider window): its rows as they were on this frame,
            // which lays the panel out already; a poll that fell due while it was away comes kPollAfterShow later.
            explorerAway_ = false;
            if (projectsAt_ >= 0.0)
                projectsAt_ = std::max(projectsAt_, now + kPollAfterShow - kProjectsPoll);
        }
        const ProjectState st = port_.project();
        // The projects and the open one's presets, every kProjectsPoll s, and at once when the store's Reader has read a
        // file since (EditorPort::projectsPolled: no project's file is read whole here; one the store does not hold as
        // it is now is read on its thread, listed as it was until then): a revision when they differ from what the rows
        // show.
        const uint64_t read = port_.projectsRead();
        // The factory projects: read again only when the set changes (one fetched, read from disk), a look at a number.
        if (const uint64_t generation = port_.factoryGeneration(); !factoryRead_ || generation != factorySeen_)
        {
            std::vector<ProjectEntry> factory = port_.factoryProjects();
            if (factoryRead_ && factory != factoryList_)
                ++listsRevision_;
            factoryList_ = std::move(factory);
            factorySeen_ = generation;
            factoryRead_ = true;
        }
        if (projectsAt_ < 0.0 || now - projectsAt_ > kProjectsPoll || presetsOf_ != st.name || read != projectsReadSeen_)
        {
            // The two lists of one state of the store's cache: a Reader's read that lands between them (the listing as
            // it was, the presets as it is) would make the rows twice, so a poll that saw the revision move is made
            // again (the Reader moves it under the cache's lock, with its read), a few times at most.
            std::vector<ProjectEntry> projects;
            std::vector<PresetEntry> presets;
            for (int tries = 0; tries < 3; ++tries)
            {
                projectsReadSeen_ = port_.projectsRead();
                projects = port_.projectsPolled();
                presets = st.name.empty() ? std::vector<PresetEntry>() : port_.presetsPolled(st.name);
                if (port_.projectsRead() == projectsReadSeen_)
                    break;
            }
            std::sort(projects.begin(), projects.end(),
                      [](const ProjectEntry& a, const ProjectEntry& b) { return a.when != b.when ? a.when > b.when : a.name < b.name; });
            ProjectsView view = port_.projectsView();    // the stars and the groups folded (a look at a small file)
            if (projects != projectList_ || presets != presetList_ || presetsOf_ != st.name || view != projectsView_)
                ++listsRevision_;
            projectList_ = std::move(projects);
            presetList_ = std::move(presets);
            projectsView_ = std::move(view);
            presetsOf_ = st.name;
            projectsAt_ = now;
        }
        const auto erred = [this](const std::string& file) {
            return std::any_of(problems_.begin(), problems_.end(), [&](const Problem& p) { return p.error && p.file == file; });
        };
        // What the rows are made from, as a hash each frame (the lists by their revision, not item by item).
        DrawKey key;
        for (int i = 0; i < files().count(); ++i)
            key.mix(files().name(i)).mix(i == files().active()).mix(erred(files().name(i)));
        const bool frozenPlays = port_.frozen();
        key.mix(files().count()).mix(files().irOpen()).mix(files().irShown()).mix(files().irView()).mix(frozenPlays)
            .mix(st.name).mix(st.preset).mix(listsRevision_).mix(folderSeen_).mix(port_.assetsRevision())
            .mix(port_.statusRevision()).mix(desktopRevision_);
        if (!explorerStale_ && key.h == explorerKey_)
            return;
        explorerKey_ = key.h;
        explorerStale_ = false;
        ++explorerBuilds_;

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
                const FileKind kind = fileKindOf(r.label);
                r.icon = i == 0                     ? chat::Icon::code
                       : kind == FileKind::Gui     ? chat::Icon::dial
                       : kind == FileKind::Circuit ? chat::Icon::circuit
                                                   : chat::Icon::file;
                r.active = i == files().active() && !files().irShown();
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
            // The samples (dsp/Assets.h): a row each, ticked when the program that plays reads it; + adds files.
            ExplorerItem::Group g;
            g.section = S::assets;
            g.title = "ASSETS";
            g.action = chat::Icon::plus;
            g.actionTitle = "Add a sample";
            g.empty = "Samples: + or drop a WAV, AIFF or FLAC";
            for (const AssetEntry& e : port_.assets())
            {
                ExplorerItem::Row r;
                r.label = e.name;
                r.detail = e.detail.substr(0, e.detail.find(','));   // its length; the rest under the pointer
                r.icon = chat::Icon::wave;
                r.marked = e.used;
                r.closable = true;
                r.key = e.name;
                r.help = "A sample, " + e.detail + (e.used ? "; the program reads it" : "; #pragma sample reads it");
                r.spec = upper(e.name) + "   " + upper(e.detail) + (e.used ? "   THE PROGRAM READS IT" : "   #PRAGMA SAMPLE READS IT");
                g.rows.push_back(std::move(r));
            }
            groups.push_back(std::move(g));
        }
        {
            // The folder (plugin/FolderSync.h): off, the row that turns it on; on, its actions, its conflicts and its
            // notes.
            ExplorerItem::Group g;
            g.section = S::folder;
            g.title = "FOLDER";
            g.counted = false;
            const FolderState& f = folderState_;
            const auto row = [](std::string label, std::string detail, chat::Icon icon, int index, std::string help,
                                std::string spec) {
                ExplorerItem::Row r;
                r.label = std::move(label);
                r.detail = std::move(detail);
                r.icon = icon;
                r.index = index;
                r.help = std::move(help);
                r.spec = std::move(spec);
                return r;
            };
            if (!f.on)
            {
                g.rows.push_back(row("Edit in a folder", "VS Code, any editor", chat::Icon::folder, kFolderOn,
                                     "Mirrors the program's files to a folder on disk, synced both ways",
                                     "FOLDER   THE PROGRAM'S FILES IN A FOLDER ON DISK FOR VS CODE OR ANY EDITOR, SYNCED BOTH WAYS"));
                if (!f.error.empty())
                {
                    ExplorerItem::Row e;
                    e.label = upper(f.error);
                    e.heading = true;
                    g.rows.push_back(std::move(e));
                }
            }
            else
            {
                g.action = chat::Icon::cross;
                g.actionTitle = "Stop syncing with the folder";
                ExplorerItem::Row where;                 // the folder's name; its path under the pointer
                where.label = upper(f.name);
                where.heading = true;
                where.spec = "FOLDER   " + upper(f.path);
                g.rows.push_back(std::move(where));
                g.rows.push_back(row("Open in VS Code", {}, chat::Icon::code, kFolderCode, "Opens the folder in VS Code",
                                     "VS CODE   OPEN THE FOLDER THERE; A SAVE LANDS HERE AS ONE UNDO STEP"));
                g.rows.push_back(row(kShowFolder, {}, chat::Icon::folder, kFolderShow, "Shows the folder: " + f.path,
                                     upper(std::string(kShowFolder)) + "   " + upper(f.path)));
                ExplorerItem::Row run = row("Run on save", {}, chat::Icon::play, kFolderRun,
                                            "Runs the program each time a save in the folder lands",
                                            "RUN ON SAVE   EACH SAVE IN THE FOLDER THAT LANDS RUNS THE PROGRAM");
                run.marked = f.runOnSave;
                g.rows.push_back(std::move(run));
                for (const std::string& file : f.conflicts)
                {
                    ExplorerItem::Row c = row(file, "in both", chat::Icon::alert, kFolderConflict,
                                              "Changed here and in the folder: choose which to keep",
                                              upper(file) + "   CHANGED HERE AND IN THE FOLDER: KEEP ONE, THE OTHER GOES TO .CONFLICTS");
                    c.error = true;
                    c.key = file;
                    g.rows.push_back(std::move(c));
                }
                for (const std::string& n : f.notes)     // whole under the pointer, as a caption may be cut
                {
                    ExplorerItem::Row e;
                    e.label = upper(n);
                    e.heading = true;
                    e.spec = "FOLDER   " + upper(n);
                    g.rows.push_back(std::move(e));
                }
            }
            groups.push_back(std::move(g));
        }
        if (desktopState_.linked)
        {
            // Claude Desktop (EditorPort::desktop): this instance's name, Use with Claude Desktop (ticked while it is the
            // target), what Claude Desktop's last call here did and when, and the others open.
            const DesktopState& d = desktopState_;
            const std::string thing = kDesktopThing, Thing = upper(thing);
            ExplorerItem::Group g;
            g.section = S::desktop;
            g.title = "CLAUDE DESKTOP";
            g.counted = false;
            ExplorerItem::Row name;
            name.label = d.name;
            name.detail = d.name != d.number ? d.number : "this " + thing;
            name.icon = chat::Icon::pencil;
            name.index = kDesktopName;
            name.help = "This " + thing + "'s name to Claude Desktop" + (d.name != d.number ? " (and " + d.number + ")" : std::string())
                      + ": a click renames it";
            name.spec = "NAME   CLAUDE DESKTOP KNOWS THIS " + Thing + " AS " + upper(d.name)
                      + (d.name != d.number ? " (AND " + upper(d.number) + ")" : std::string()) + "   A CLICK RENAMES IT";
            g.rows.push_back(std::move(name));
            ExplorerItem::Row use;
            use.label = "Use with Claude Desktop";
            use.detail = d.target && !d.chosen ? "only one open" : std::string();
            use.icon = chat::Icon::bubble;
            use.marked = d.target;
            use.index = kDesktopUse;
            use.help = d.chosen   ? "Claude Desktop's calls go to this " + thing + ", as you chose: a click clears the choice"
                     : d.target   ? "Claude Desktop's calls go to this " + thing + ", the only one open: a click keeps it so"
                     : d.targetName.empty() ? "Several are open and none is chosen: a click sends Claude Desktop's calls here"
                                            : "Claude Desktop's calls go to " + d.targetName + ": a click sends them here";
            use.spec = d.chosen ? "CLAUDE DESKTOP   ITS CALLS GO TO THIS " + Thing + "   A CLICK CLEARS THE CHOICE"
                     : d.target ? "CLAUDE DESKTOP   ITS CALLS GO TO THIS " + Thing + ", THE ONLY ONE OPEN   A CLICK KEEPS IT SO"
                     : d.targetName.empty() ? "CLAUDE DESKTOP   SEVERAL OPEN, NONE CHOSEN: IT ASKS WHICH   A CLICK SENDS ITS CALLS HERE"
                                            : "CLAUDE DESKTOP   ITS CALLS GO TO " + upper(d.targetName) + "   A CLICK SENDS THEM HERE";
            g.rows.push_back(std::move(use));
            if (!d.doing.empty())
            {
                // The live line: lit while a call acts here; a click opens the chat, where what it did shows.
                ExplorerItem::Row last;
                last.label = d.doing;
                last.detail = desktopAgo_;
                last.icon = chat::Icon::bolt;
                last.active = d.running;
                last.index = kDesktopLast;
                last.help = std::string(d.running ? "Claude Desktop is acting on this " : "Claude Desktop's last call to this ") + thing + ": "
                          + d.doing + ", " + desktopAgo_ + "; a click opens the chat, where it shows";
                last.spec = std::string(d.running ? "CLAUDE DESKTOP NOW   " : "CLAUDE DESKTOP'S LAST CALL   ") + upper(d.doing) + ", "
                          + upper(desktopAgo_) + "   A CLICK OPENS THE CHAT, WHERE IT SHOWS";
                g.rows.push_back(std::move(last));
            }
            if (!d.others.empty())
            {
                ExplorerItem::Row also;
                also.label = "ALSO OPEN";
                also.heading = true;
                g.rows.push_back(std::move(also));
                for (const DesktopPeer& o : d.others)
                {
                    ExplorerItem::Row r;
                    r.label = o.name;
                    r.detail = !o.track.empty() ? o.track : o.project;
                    r.icon = chat::Icon::window;
                    r.marked = o.target;
                    r.index = kDesktopOther;
                    r.key = o.id;
                    const std::string where = (o.track.empty() ? std::string() : ", on " + o.track)
                                            + (o.project.empty() ? std::string() : ", " + o.project);
                    r.help = "Another " + thing + " open" + where + (o.target ? "; Claude Desktop's target: a click clears the choice"
                                                                          : ": a click makes it Claude Desktop's target");
                    r.spec = upper(o.name) + (o.track.empty() ? std::string() : "   ON " + upper(o.track))
                           + (o.project.empty() ? std::string() : "   " + upper(o.project))
                           + (o.target ? "   CLAUDE DESKTOP'S TARGET: A CLICK CLEARS THE CHOICE" : "   A CLICK MAKES IT CLAUDE DESKTOP'S TARGET");
                    g.rows.push_back(std::move(r));
                }
            }
            groups.push_back(std::move(g));
        }
        {
            // The IR (IrView.h): none of the program's files, which its caption says; a row a view, the frozen copy's
            // while it plays (or while its view is shown).
            ExplorerItem::Group g;
            g.section = S::ir;
            g.title = "LLVM IR";
            ExplorerItem::Row caption;
            caption.label = "COMPILED, READ ONLY";
            caption.heading = true;
            g.rows.push_back(std::move(caption));
            for (int v = 0; v < irview::kViews; ++v)
            {
                const bool shown = files().irShown() && files().irView() == v;
                if (static_cast<irview::View>(v) == irview::View::frozen && !frozenPlays && !shown)
                    continue;
                ExplorerItem::Row r;
                r.label = irview::name(static_cast<irview::View>(v), files().name(0));
                r.detail = irview::word(static_cast<irview::View>(v));
                r.icon = chat::Icon::chip;
                r.active = shown;
                r.index = v;
                g.rows.push_back(std::move(r));
            }
            groups.push_back(std::move(g));
        }
        {
            ExplorerItem::Group g;
            g.section = S::presets;
            g.title = "PRESETS";
            const bool factory = isFactoryName(st.name);     // read-only: saving the project makes the user's own copy
            if (!st.name.empty() && !factory)
            {
                g.action = chat::Icon::plus;
                g.actionTitle = "Save a preset";
            }
            g.empty = st.name.empty() ? "Save the project to keep presets"
                    : factory         ? "None in this factory project"
                                      : "None yet: + saves the dials";
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
            // Grouped as PROJECTS groups them (ProjectGroups.h), the newest first in each (the factory's starred ones
            // among the favourites): a caption a group, each folding its group, unless the one group is UNCATEGORISED
            // (no project given a category, none starred: the flat list). A lone category's or FAVOURITES' caption
            // stays: it says what the list is.
            std::vector<ProjectEntry> all = projectList_;
            all.insert(all.end(), factoryList_.begin(), factoryList_.end());
            projectgroups::Options o;
            o.favourites = &projectsView_.favourites;
            o.folded = &projectsView_.folded;
            o.order = ProjectOrder::saved;
            o.factoryGroup = false;
            const std::vector<projectgroups::Group> grouped = projectgroups::arrange(all, o);
            const bool captions = !(grouped.size() == 1 && grouped[0].kind == projectgroups::Kind::uncategorised);
            g.total = static_cast<int>(projectList_.size());
            for (const projectgroups::Group& grp : grouped)
            {
                if (captions)
                {
                    ExplorerItem::Row h;
                    h.label = upper(grp.title);
                    h.heading = true;
                    h.fold = grp.folded ? 2 : 1;
                    h.key = grp.key;
                    h.detail = std::to_string(grp.total);
                    h.help = grp.title + ": " + std::to_string(grp.total) + (grp.total == 1 ? " project" : " projects");
                    h.spec = upper(grp.title) + "   SHOW OR HIDE ITS PROJECTS (AS PROJECTS DOES)";
                    g.rows.push_back(std::move(h));
                    if (grp.folded)
                        continue;
                }
                for (const int m : grp.members)
                {
                    const ProjectEntry& e = all[static_cast<std::size_t>(m)];
                    ExplorerItem::Row r;
                    r.label = ownName(e.name);
                    r.detail = e.factory ? std::string("factory") : "v" + std::to_string(e.latest);
                    r.icon = chat::Icon::list;
                    r.active = e.name == st.name;
                    r.key = e.name;
                    g.rows.push_back(std::move(r));
                }
            }
            groups.push_back(std::move(g));
        }
        if (!factoryList_.empty())
        {
            // The factory projects (plugin/Factory.h), by name, read-only.
            ExplorerItem::Group g;
            g.section = S::factory;
            g.title = "FACTORY";
            for (const ProjectEntry& e : factoryList_)
            {
                ExplorerItem::Row r;
                r.label = ownName(e.name);
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
                r.icon = isCircuitFile(f.name) ? chat::Icon::circuit : chat::Icon::blocks;   // a netlist: a circuit's
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
            case S::factory:
                if (row.heading)
                {
                    // A group's caption: folded or opened, in PROJECTS too (this Mac's view).
                    ProjectsView v = port_.projectsView();
                    const auto it = std::find_if(v.folded.begin(), v.folded.end(), [&](const std::string& k) { return projectgroups::same(k, row.key); });
                    if (it != v.folded.end())
                        v.folded.erase(it);
                    else
                        v.folded.push_back(row.key);
                    std::string why;
                    if (!port_.setProjectsView(v, why))
                        notice("NOT KEPT: " + upper(why));
                    projectsAt_ = -100.0;                // read again at once
                    break;
                }
                openProjectNamed(row.key);
                break;
            case S::ir:
                if (row.index >= 0 && row.index < irview::kViews)
                    openIrView(row.index);
                break;
            case S::folder:
                folderRow(row);
                break;
            case S::assets:
                notice(upper(row.key) + ": #PRAGMA SAMPLE <FIELD> \"" + upper(row.key) + "\" READS IT (SAMPLE.FM)");
                break;
            case S::desktop:
                desktopRow(row);
                break;
        }
        explorerStale_ = true;                           // what it shows may have changed (a tab, a tick)
        dirty_ = true;
    }

    // A project from the explorer: its newest version, as PROJECTS' Open, asking first (a menu at the pointer) when the
    // editor's files have changes no version holds.
    void Panel::openProjectNamed(const std::string& name)
    {
        const auto load = [this, name] {
            std::string why;
            if (port_.openProject(name, 0, why))
                notice(isFactoryName(name) ? "OPENED: " + upper(ownName(name)) + " (FACTORY, READ-ONLY: A SAVE MAKES YOUR OWN COPY)"
                                           : "OPENED: " + upper(name) + " V" + std::to_string(port_.project().version));
            else
                notice("NOT OPENED: " + upper(why));
            projectsAt_ = -100.0;
            dirty_ = true;
        };
        files().endRename();
        (void)pushFiles();                               // what "edited" means is what the tabs show
        if (!port_.project().edited || host_ == nullptr)
        {
            load();
            return;
        }
        funkgui::MenuRequest request;
        request.items.push_back({ 1, "Open " + ownName(name) + " (the editor's changes are lost)" });
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
        pushEditor();                                    // what is saved is what the tabs show
        const ProjectState st = port_.project();
        if (st.name.empty())
        {
            openProjects();
            return;
        }
        // The project's file is read and written off the message thread (EditorPort::startSave): SAVING meanwhile,
        // then what came of it (syncSave).
        std::string why;
        if (!port_.startSave(st.name, why))
            notice("NOT SAVED: " + upper(why));
        else
            savingNotice();
        dirty_ = true;
    }

    void Panel::savingNotice()
    {
        if (const SaveState s = port_.saving(); s.busy)
        {
            notice("SAVING " + upper(s.name) + "...");
            saving_ = true;
        }
        else
            syncSave();
    }

    // What a save of the editor's came to, said once it has ended (SaveState::done moved): the explorer's lists read
    // again, and PROJECTS' when what it shows changed (a confirmation it asks for stands over lists that did not).
    void Panel::syncSave()
    {
        const SaveState s = port_.saving();
        if (s.done == saveSeen_)
        {
            if (saving_ && !s.busy)
                saving_ = false;
            return;
        }
        saveSeen_ = s.done;
        saving_ = s.busy;
        if (s.busy)
            return;                                      // one more was asked for: its outcome comes next
        const SavedVersion& saved = s.saved;
        const std::string at = upper(saved.name.empty() ? s.name : saved.name) + " V" + std::to_string(saved.number);
        notice(!s.ok           ? "NOT SAVED: " + upper(s.why)
               : saved.copied    ? "SAVED AS YOUR OWN PROJECT: " + upper(saved.name) + " (THE FACTORY'S STAYS AS IT IS)"
               : saved.unchanged ? "NOTHING NEW TO SAVE: " + at + " HAS IT"
               : saved.created   ? "PROJECT SAVED: " + upper(saved.name)
                                 : "SAVED: " + at);
        projectsAt_ = -100.0;
        (void)projects_.rereadIfChanged(ProjectBrowser::Because::saved);
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
                || f == keyItem_)
                setFocus(full() ? -1 : editorItem_);
        }
        layout();
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
        if (!dialogHasKey())
            return std::nullopt;
        const funkgui::Rect r = keyDialog().remove;
        return funkgui::Point{ r.centreX(), r.centreY() };
    }
    funkgui::Point Panel::centreOfKeyProvider() const
    {
        const funkgui::Rect r = keyDialog().provider;
        return { r.centreX(), r.centreY() };
    }
    std::optional<funkgui::Point> Panel::centreOfKeyBase() const
    {
        const funkgui::Rect r = keyDialog().base;
        if (r.w <= 0.0f)
            return std::nullopt;
        return funkgui::Point{ r.centreX(), r.centreY() };
    }
    std::optional<funkgui::Point> Panel::centreOfKeyModel() const
    {
        const funkgui::Rect r = keyDialog().model;
        if (r.w <= 0.0f)
            return std::nullopt;
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

    void Panel::attach(funkgui::HostServices& real)
    {
        proxy_ = std::make_unique<HostProxy>(*this, real);
        funkgui::HostServices& host = *proxy_;
        host_ = &host;
        gestures_.emplace(host);
        if (zoom_ != nullptr)
            static_cast<ZoomModel*>(zoom_)->attach(host);
        files().attach(&host);
        chatView().attach(&host);
        inputItem().attach(&host);
        dirty_ = true;
        // This Mac's default widths as the host found them: EditorHost reloads the preferences after the Panel is made,
        // just before this, so a width another process set since this one first read them counts (the session's win).
        const float e = wantedWidth(seenExplorerWidth_, false), c = wantedWidth(seenChatWidth_, true);
        if (e != explorerWant_ || c != chatWant_)
        {
            explorerWant_ = e;
            chatWant_ = c;
            layout();
        }
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
    funkgui::TextEditor& Panel::fileEditor(int i) { return files().editorOf(std::clamp(i, 0, files().count() - 1)); }
    int Panel::codeTextSize() const { return files().codeSize(); }
    int Panel::fileCount() const { return files().count(); }
    std::string Panel::fileName(int i) const { return i >= 0 && i < files().count() ? files().name(i) : std::string(); }
    int Panel::activeFile() const { return files().active(); }
    bool Panel::renamingFile() const { return files().renaming(); }
    funkgui::Point Panel::centreOfTab(int i) const
    {
        const funkgui::Rect r = files().tabRect(i);
        return { std::min(r.x + kTabPad + 4.0f, r.centreX() - 1.0f), r.centreY() };   // a narrowed tab's: left of its box
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

    // ---- the IR view (IrView.h) ----------------------------------------------------------------------------------------
    bool Panel::irOpen() const { return files().irOpen(); }
    bool Panel::irShown() const { return files().irShown(); }
    int Panel::irView() const { return files().irView(); }
    std::string Panel::irName() const { return files().irName(); }
    funkgui::Point Panel::centreOfIrTab() const
    {
        const funkgui::Rect r = files().irTabRect();
        return { r.x + kTabPad + kIrIconW + 6.0f, r.centreY() };
    }
    funkgui::Point Panel::centreOfIrClose() const
    {
        const funkgui::Rect r = files().irCloseRect();
        return { r.centreX(), r.centreY() };
    }
    funkgui::Rect Panel::irTabRect() const { return files().irOpen() ? files().irTabRect() : funkgui::Rect{}; }
    funkgui::Rect Panel::fileTabRect(int i) const { return files().tabRect(i); }

    // ---- a netlist's Schematic view (SchematicView.h) -----------------------------------------------------------------
    bool Panel::schematicShown() const { return !full() && files().schematicShown(); }
    void Panel::setSchematic(bool on)
    {
        files().setSchematic(on);
        dirty_ = true;
    }
    SchematicView& Panel::schematic() { return files().schematic(); }
    bool Panel::viewPillsShown() const { return files().pillsShown(); }
    bool Panel::viewPillsLabelled() const { return files().pillsLabelled(); }
    funkgui::Rect Panel::viewPillRect(bool schematic) const { return files().pillRect(schematic ? 1 : 0); }
    funkgui::Point Panel::centreOfViewPill(bool schematic) const
    {
        const funkgui::Rect r = viewPillRect(schematic);
        return { r.centreX(), r.centreY() };
    }
    funkgui::Rect Panel::editorArea() const { return files().editorArea(); }
    funkgui::Point Panel::centreOfDialsBar() const { return centreOf(dialsBarItem_); }

    void Panel::persistFileViews()
    {
        port_.setFileViews(files().fileViews());
        dirty_ = true;
    }

    void Panel::openIrView(int view)
    {
        if (out_ && peer_ != nullptr)
        {
            peer_->openIrView(view);                     // the editor is in the code window
            return;
        }
        if (panelView_)
            setPanelView(false);                         // the tabs are the CODE view's
        files().openIr(view);
        syncIr();
        setFocus(editorItem_);
        explorerStale_ = true;
        dirty_ = true;
    }

    void Panel::closeIrView()
    {
        files().closeIr();
        syncIr();
        explorerStale_ = true;
        dirty_ = true;
    }

    // Whether this Panel looks at the IR (its IR tab open: the processor keeps the IR only while someone does), and the
    // shown view's text made from what there is, when that changed: the IR's revision, the last compile, the frozen
    // copy playing, the files edited since, the program's file name.
    void Panel::syncIr()
    {
        const bool open = files().irOpen();
        if (open != irViewing_)
        {
            irViewing_ = open;
            if (open)
                port_.addIrViewer();
            else
                port_.removeIrViewer();
            explorerStale_ = true;
        }
        if (!files().irShown())
            return;
        const int view = files().irView();
        const irview::View v = static_cast<irview::View>(view);
        const bool frozen = port_.frozen();
        // What can change the view's text, read without a lock: the IR's revision, the last compile, the frozen copy
        // playing (the frozen view's alone), the files edited since, the program's name. Unchanged, nothing to do.
        const std::string seen = std::to_string(view) + '/' + std::to_string(port_.irRevision()) + '/'
                               + std::to_string(seenStatus_) + '/' + (v == irview::View::frozen && frozen ? 'f' : '-')
                               + (edited_ ? 'e' : '-') + '/' + files().name(0);
        if (seen == irSeen_ && !files().irMade(view).empty())
            return;
        irSeen_ = seen;
        // What the text reads: its comment lines and LLVM's text (by its address: setIrText holds it). The revision
        // bumps for anything of the IR, the frozen copy's too, which the program's views do not show: they keep their
        // text, its selection and its scroll.
        const IrStatus ir = port_.ir();
        const CompileStatus st = port_.status();
        irview::Source from;
        from.ir = &ir;
        from.status = &st;
        from.frozenPlays = frozen;
        from.edited = edited_;
        from.controls = &controls_;
        const std::string head = irview::header(v, files().name(0), from);
        const std::shared_ptr<const IrText>& shown = irview::shown(v, ir);
        char id[32];
        std::snprintf(id, sizeof id, "%p", static_cast<const void*>(shown.get()));
        const std::string key = head + '\n' + id;
        if (key == files().irMade(view))
            return;
        const int body = static_cast<int>(std::count(head.begin(), head.end(), '\n')) + 1;
        files().setIrText(view, key, irview::text(head, shown.get()), shown, body);
        dirty_ = true;
    }

    bool Panel::mergeDraft()
    {
        const uint64_t r = port_.draftRevision();        // read first: a write after it is merged next time
        if (r == seenDraft_)
            return false;
        Draft d = port_.draft();
        const Draft remote = d;
        bool kept = false;
        if (files().revision() != syncedFiles_)          // typed since the editor last gave its files
        {
            const Draft now = files().draft();
            const auto named = [](const Draft& x, const std::string& name) -> const SourceFile* {
                for (const SourceFile& f : x.files)
                    if (f.name == name)
                        return &f;
                return nullptr;
            };
            // A tab renamed since (a new name at the place of one gone, as FilesItem::merge reads it): the draft's file
            // of the old name takes the new one (and the typing in it, where the draft left its text).
            for (std::size_t i = 0; i < given_.files.size() && i < now.files.size(); ++i)
            {
                const SourceFile &g = given_.files[i], &t = now.files[i];
                if (g.name == t.name || named(now, g.name) != nullptr || named(given_, t.name) != nullptr || named(d, t.name) != nullptr)
                    continue;
                for (SourceFile& f : d.files)
                    if (f.name == g.name)
                    {
                        f.name = t.name;
                        if (f.text == g.text)
                            f.text = t.text;
                        kept = true;
                    }
            }
            for (SourceFile& f : d.files)
                if (const SourceFile *g = named(given_, f.name), *t = named(now, f.name);
                    g != nullptr && t != nullptr && f.text == g->text && t->text != g->text)
                {
                    f.text = t->text;                    // the draft left it: the typing stays, and goes to the port
                    kept = true;
                }
            // A tab closed since that the draft carries as it was given: it stays closed. One the draft changed stays
            // (neither side's change is lost). The tab the draft shows stays shown, by name.
            const std::string drafted = d.files.empty() ? std::string()
                                                        : d.files[static_cast<std::size_t>(std::clamp(d.active, 0, static_cast<int>(d.files.size()) - 1))].name;
            const std::size_t before = d.files.size();
            std::erase_if(d.files, [&](const SourceFile& f) {
                const SourceFile* g = named(given_, f.name);
                return g != nullptr && named(now, f.name) == nullptr && f.text == g->text;
            });
            if (d.files.size() != before)
            {
                kept = true;
                d.active = 0;
                for (std::size_t i = 0; i < d.files.size(); ++i)
                    if (d.files[i].name == drafted)
                        d.active = static_cast<int>(i);
            }
            // A tab added since that the draft lacks: it stays, after the draft's files, and stays shown when it was.
            const std::string shown = now.active >= 0 && now.active < static_cast<int>(now.files.size())
                                          ? now.files[static_cast<std::size_t>(now.active)].name
                                          : std::string();
            for (const SourceFile& t : now.files)
                if (named(given_, t.name) == nullptr && named(d, t.name) == nullptr && d.files.size() < kMaxFiles)
                {
                    d.files.push_back(t);
                    if (t.name == shown)
                        d.active = static_cast<int>(d.files.size()) - 1;
                    kept = true;
                }
            d.active = std::clamp(d.active, 0, std::max(0, static_cast<int>(d.files.size()) - 1));
        }
        seenDraft_ = r;
        files().merge(d);
        given_ = remote;
        dirty_ = true;
        // what was kept is the port's still to have: no revision of the tabs' is ~0, so the next push gives it
        syncedFiles_ = kept ? ~uint64_t{ 0 } : files().revision();
        return true;
    }

    bool Panel::pushFiles()
    {
        if (out_)
            return false;                                // the hidden copy gives nothing (pushEditor: the code window's)
        for (int tries = 0; tries < 2; ++tries)
        {
            (void)mergeDraft();
            if (files().revision() == syncedFiles_)
                return true;
            Draft d = files().draft();
            if (port_.setDraftIf(d, seenDraft_))
            {
                syncedFiles_ = files().revision();
                given_ = std::move(d);
                return true;
            }
        }
        return false;
    }

    // The editor's files to the processor: the code window's Panel's while it has the editor. (This Panel's copy is the
    // files as the code window's Panel loaded them, hidden: it never has the focus, merges no draft and gives nothing
    // meanwhile; the processor's draft is the editor's.)
    void Panel::pushEditor()
    {
        Panel& e = editorPanel();
        e.files().endRename();
        (void)e.pushFiles();
    }

    void Panel::runNow()
    {
        if (out_ && peer_ != nullptr)
        {
            peer_->runNow();                             // RUN runs the code window's files (its Format on Run there)
            compiling_ = true;
            dirty_ = true;
            return;
        }
        files().endRename();
        files().endField();
        if (files().assist().formatOnRun())
            files().assist().formatAll();
        (void)pushFiles();
        const Draft draft = files().draft();
        port_.run(draft.files);
        files().assist().cancelCheck();                  // RUN's messages will say more of these files
        compiling_ = true;
        dirty_ = true;
    }

    void Panel::refresh(double nowSec)
    {
        if (ab_.isOpen())
        {
            const uint32_t before = ab_.revision();
            ab_.tick();                                  // the processor's A/B: a compile landed, the loudness measured
            if (ab_.revision() != before)
                afterAb(true);
        }
        else if (role_ == Role::editor && !dialogOpen() && port_.abRevision() != abSeen_)
        {
            // An A/B on while no card shows it (this editor opened while it plays): its card, as it is.
            abSeen_ = port_.abRevision();
            if (ab_.attach())
            {
                focusBeforeProjects_ = focus_;
                setFocus(-1);
                ring_ = false;
                dirty_ = true;
            }
        }
        syncColumnWidths();
        syncCodePanel();
        // The text size: this machine's preference, which another editor's keys or wheel may have moved (CodeAssist.h).
        if (const uint32_t r = funkgui::UiPreferences::get().revision(); r != seenPrefsRev_)
        {
            seenPrefsRev_ = r;
            if (const int size = codesize::read(); size != files().codeSize())
            {
                files().setCodeSize(size);
                dirty_ = true;
            }
        }
        keepStepDefaults(nowSec, false);
        syncFolder();
        syncMidi();
        // The editor's save in flight (Cmd-S, PROJECTS' Save): what came of it, once it has ended.
        syncSave();
        // PROJECTS and EXPORT read the store's cache alone: what its Reader has read since shows now.
        if (projects_.poll())
            dirty_ = true;
        if (export_.poll())
            dirty_ = true;
        // What Claude's projects tool did (a save, an open, a preset): said once in the status, the lists read again.
        if (role_ == Role::editor)
            if (const ClaudeNotice n = port_.claudeNotice(); n.count != claudeSeen_)
            {
                claudeSeen_ = n.count;
                notice(n.text);
                projectsAt_ = -100.0;
                projects_.reread();
                dirty_ = true;
            }
        // The draft, when a host loaded a session while the editor was open, or Claude changed the files. (While the
        // editor is in the code window, that Panel's: this one's hidden copy takes nothing and gives nothing.)
        bool textChanged = std::exchange(pushedFiles_, false);   // a gesture's files, given at once
        if (!out_ && mergeDraft())
        {
            textChanged = true;
            dirty_ = true;
        }
        // The files as typed, and the tab shown, to the processor (it outlives this window), never over a write that
        // came since the merge above (Claude's, on its worker): that one merges first.
        if (!out_ && files().revision() != syncedFiles_)
        {
            (void)pushFiles();
            textChanged = true;
        }
        // The editor's check while typing (CodeAssist.h): the files once they have rested, its messages when it is done
        // (the code window's, while the editor is out: this Panel's copy asks for none).
        if (!out_ && files().assist().takeCheckRequest())
        {
            const uint64_t before = port_.checkStatus().revision;   // read first: the check may finish at once
            port_.check(files().draft().files);
            files().assist().checkSent(before);
            dirty_ = true;                               // the status line says Checking...
        }
        if (files().assist().checking())
            if (const CheckStatus c = port_.checkStatus(); files().assist().takeCheckResult(c.revision))
            {
                files().assist().setCheckDiagnostics(diagnosticsOf(parseProblems(c.errors)), c.sources);
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
            lastPanelOnly_ = s.panelOnly;
            // A program that stopped as it played (Compiler.h, "A program that stops"): what plays now, in words.
            lastStop_ = !s.stopped                       ? std::string()
                      : s.program == s.stoppedProgram    ? std::string("NOTHING PLAYS")
                      : !s.restored.empty()              ? std::string("THE PROGRAM BEFORE PLAYS AGAIN")
                                                         : std::string("THE PROGRAM BEFORE PLAYS ON");
            lastStopWhy_ = s.bounds ? "AN INDEX OUTSIDE ITS ARRAY" : "A LOOP THAT NEVER ENDS?";   // Patch.h
            problems_ = parseProblems(s.errors + (s.stopped ? std::string() : s.notes));
            files().assist().setRunDiagnostics(diagnosticsOf(problems_), s.sources);
            // The dials and the source of the program that plays: this compile's when it compiled (the processor
            // takes them on its own timer, which may not have run yet), or the program before one that stopped when it
            // plays again; else the ones before.
            const bool own = s.ok || !s.restored.empty();
            const Controls cs = own ? s.controls : port_.controls();
            if (s.ok)
                running_ = s.sources;
            else if (!s.restored.empty())
                running_ = s.restored;
            circuitDials_ = s.circuitDials;              // the program that plays's (a failed compile's: the one before)
            circuitProbes_ = s.probes;                   // ... its probe table, and its id (the probes' sets name it)
            probeProgram_ = s.program;
            const PanelSpec spec = own ? s.panel : port_.panel();
            ++controlsRevision_;
            for (int c = 0; c < kMaxControls; ++c)
                if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0)
                    static_cast<DialItem&>(*items_[static_cast<std::size_t>(i)])
                        .model()
                        .setControl(cs[static_cast<std::size_t>(c)], spec.labels[static_cast<std::size_t>(c)],
                                    spec.options[static_cast<std::size_t>(c)], controlsRevision_);
            guiView_->setControls(cs, spec);
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
                guiView_->resetScroll();
            const bool otherShaders = !(spec.shaders == panelSpec_.shaders);
            panelSpec_ = spec;
            if (otherGui)
                guiView_->setPages(guiPagesOf(panelSpec_.gui, port_.guiPages()));
            if (otherShaders || otherGui)
            {
                guiView_->registerShaders(panelSpec_, running_, s.notes);
                if (sameDials)
                    guiView_->place(panelSpec_);
            }
            panelProgram_ = s.program != 0 ? s.program : port_.program();   // what plays (a refine's too)
            if (!sameDials)
                layout();
            statusChanged = true;
            dirty_ = true;                               // its messages, its dials, its interface
        }
        compiling_ = port_.compiling();
        if (out_ && peer_ != nullptr)
            edited_ = peer_->edited_;                    // the editor's, in the code window (this Panel's copy is hidden)
        else if (textChanged || statusChanged)
            edited_ = !cir::sameProgram(files().draft().files, running_);   // a netlist's *@ lines are no edit
        syncIr();
        const std::string statusBefore = status_;

        // The project open's presets (the top bar's arrows and name; their list while it is open).
        {
            PresetCycle cy = port_.presetCycle();
            if (cy != cycle_)
            {
                const bool layoutNow = presetsShown() != !cy.project.empty();
                cycle_ = std::move(cy);
                dirty_ = true;
                if (layoutNow)
                    layout();                            // the pill makes room for them, or takes it back
            }
            const bool pickerOpen = picker_.isOpen();
            const uint32_t pickerWas = picker_.revision();
            picker_.sync();
            if (picker_.isOpen() && !presetsShown())
                picker_.close();
            if (picker_.revision() != pickerWas)
            {
                if (pickerOpen)
                    afterPicker(true);                   // closed by another project opening: the focus back
                dirty_ = true;
            }
        }
        // The project open, in words.
        {
            const ProjectState st = port_.project();
            std::string text;
            if (!st.name.empty())
            {
                text = "PROJECT  " + upper(st.name) + (st.version > 0 && !isFactoryName(st.name) ? "  V" + std::to_string(st.version) : "")
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
        else if (!lastStop_.empty())
            status_ = "STOPPED: " + lastStopWhy_ + "   " + lastStop_;
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
            std::snprintf(buf, sizeof buf, "%s   %s IN %.0f MS", edited_ ? "EDITED: CMD-RETURN RUNS IT" : "RUNNING",
                          lastPanelOnly_ ? "PANEL UPDATED" : "COMPILED", lastMs_);
            status_ = buf;
        }
        if (status_ != statusBefore)
            dirty_ = true;

        // The meter, four times a second at most: a frame for it when it changed. The snowflake (the dials compiled in,
        // while they hold still) comes and goes with the frame after the change.
        const bool frozen = port_.frozen();
        if (meterAt_ < 0.0 || nowSec - meterAt_ >= 0.25 || nowSec < meterAt_ || frozen != frozenShown_)
        {
            meterAt_ = nowSec;
            const uint32_t a = port_.alerts();
            std::string alerts = (a & Engine::nonFinite) != 0 ? "NAN REPLACED" : "";
            if ((a & Engine::clipped) != 0)
                alerts += (alerts.empty() ? "" : "   ") + std::string("CLIPPED");
            char load[32];
            std::snprintf(load, sizeof load, "CPU %.1f%%", static_cast<double>(port_.load()) * 100.0);
            if (meterAlerts_ != alerts || meterLoad_ != load || frozen != frozenShown_)
            {
                meterAlerts_ = std::move(alerts);
                meterLoad_ = load;
                frozenShown_ = frozen;
                meter_ = meterAlerts_.empty() ? meterLoad_ : meterAlerts_ + "   " + meterLoad_;
                placeMeter();
                dirty_ = true;
            }
        }
        if (role_ == Role::code)
            return;                                      // no settings card and no chat in the code window
        syncSettings();
        syncRecording(nowSec);
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
            if (!s.ready() && s.provider.kind == "anthropic")
                e.body += std::string(" First add your Anthropic API key: it is stored encrypted on ") + kThisMachine
                          + ", for both playground plugins, and the chat's requests are billed to it.";
            else if (!s.ready())
                e.body += " First add your " + s.providerName + " API key (or choose another provider): it is stored "
                          "encrypted on " + kThisMachine + " and sent only to " + s.providerName + ".";
            else if (s.provider.kind != "anthropic")
                e.body += " It runs on " + s.providerName + (s.model.empty() ? std::string() : " (" + s.model + ")")
                          + ": the conversation goes there.";
            if (!s.keyProblem.empty())
                e.body += " The stored key cannot be used: " + s.keyProblem;
            // Where the chat is kept (ChatAgent.h, "The chats").
            e.body += s.owner.empty() ? std::string(" This chat stays on ") + kThisMachine + ", with this plugin until you save a project."
                                      : " This chat stays on " + std::string(kThisMachine) + ", with \"" + s.owner + "\".";
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

        // A thinking level's name: Anthropic's words, and another provider's "none" (thinking off) and "minimal".
        std::string effortName(const std::string& e)
        {
            if (e == "none")
                return "Off";
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
                    return m.name.rfind("Claude ", 0) == 0 && chat_.provider.kind == "anthropic" ? m.name.substr(7) : m.name;
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

    // The thinking levels' pill: Anthropic's (its models have levels), and another provider's where its model has some
    // (OpenRouter's, OpenAI's, Gemini's: ChatModel::efforts); none for a server whose models have none.
    bool Panel::chatTakesThinking() const { return chat_.provider.kind == "anthropic" || !chatEfforts().empty(); }

    // What the bar's layout depends on: the pills' labels, and whether fast mode's is there.
    std::string Panel::barLabels() const
    {
        return modelLabel() + '|' + thinkingLabel() + (chatTakesFast() ? "|fast" : "") + (chatTakesWeb() ? "|web" : "")
             + (chatTakesThinking() ? "" : "|nothinking");
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
            const bool moved = r != seenChat_;
            seenChat_ = r;
            chatAt_ = nowSec;
            const bool hadKey = chat_.ready();
            const std::string hadProblem = chat_.keyProblem;
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
            // Shown again when it changed (its revision) or the key's state did, not at each look once a second: that
            // goes over the whole conversation's text (ChatView::show: every block's key, the transcript), 2 ms for a
            // long one.
            if (moved || hadKey != chat_.ready() || hadProblem != chat_.keyProblem)
                chatView().show(chat_, emptyChat(chat_));
            const bool claude = chat_.provider.kind == "anthropic";   // another provider's model is no Claude
            inputItem().setPlaceholder(chat_.busy     ? (claude ? "Claude is working (the square button stops it)"
                                                                : "The model is working (the square button stops it)")
                                       : chat_.asking ? "Answer above, or write your own answer"
                                       : (product::kIsInstrument ? (claude ? "Ask Claude for a sound, or about the code"
                                                                           : "Ask for a sound, or about the code")
                                                                 : (claude ? "Ask Claude for an effect, or about the code"
                                                                           : "Ask for an effect, or about the code")));
            if (barLabels() != barLabels_)
                placeChatBar();
            if (chatsLabel() + '|' + chatTitle() != chatsLabel_)
                layoutChat();                            // the chats' pill as wide as the chat's name
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
        }
        const std::string a = activityText(chat_, nowSec - busySince_);
        if (a != activity_)
        {
            activity_ = a;
            dirty_ = true;
        }
        syncDesktop();
    }

    // Claude Desktop as this instance's plugin shows it (EditorPort::desktop, a copy each refresh): the chat header's
    // subtitle and badge, and the explorer's rows when what they show changed.
    void Panel::syncDesktop()
    {
        desktopState_ = port_.desktop();
        const DesktopState& d = desktopState_;
        desktopAgo_ = d.doing.empty() ? std::string() : agoText(d.running, d.ago);
        const std::string sub = !d.linked                 ? std::string()
                              : d.running                 ? "Claude Desktop: " + d.doing
                              : d.connected && d.target   ? "Claude Desktop calls this " + d.name
                                                          : std::string();
        const std::string badge = d.linked && d.connected && !d.target
                                    ? (d.targetName.empty() ? std::string("Desktop: none chosen") : "Desktop uses " + d.targetName)
                                    : std::string();
        if (sub != desktop_ || badge != desktopBadge_)
        {
            const bool placed = badge != desktopBadge_;
            desktop_ = sub;
            desktopBadge_ = badge;
            if (placed)
                placeDesktopBadge();
            dirty_ = true;
            ++chatA11y_;
        }
        std::string seen = d.name + '|' + d.number + '|' + (d.chosen ? "c" : "") + (d.target ? "t" : "") + (d.running ? "r" : "") + '|'
                         + d.targetName + '|' + d.doing + '|' + desktopAgo_;
        for (const DesktopPeer& o : d.others)
            seen += '|' + o.id + ':' + o.name + ':' + o.track + ':' + o.project + (o.target ? ":t" : "");
        if (seen != desktopSeen_)
        {
            desktopSeen_ = std::move(seen);
            ++desktopRevision_;
            dirty_ = true;
        }
    }

    // The badge at the left of the header's buttons, as wide as its label where there is room after the title; the chat
    // folded, its mark on the rail, under the rail's button and its light.
    void Panel::placeDesktopBadge()
    {
        if (desktopItem_ < 0 || desktopMarkItem_ < 0 || newChatItem_ < 0)
            return;
        auto& badge = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(desktopItem_)]);
        const funkgui::Rect newChat = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(newChatItem_)]).bounds();
        const float right = newChat.x - 8.0f;
        const float room = right - (chatsPillX() + kChatsPillLeast);   // after the title, its light and the chats' pill
        const float w = std::min(badge.preferredWidth(), room);
        const bool shown = chatOpen_ && hasChat() && !desktopBadge_.empty() && room >= 90.0f;
        badge.setHidden(!shown);
        badge.setBounds({ right - w, headerY_ + std::floor(0.5f * (kIconButton - kPillH)), w, kPillH });
        auto& mark = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(desktopMarkItem_)]);
        mark.setHidden(chatOpen_ || !hasChat() || desktopBadge_.empty());
        mark.setBounds({ card_.centreX() - 0.5f * kIconButton, card_.y + 8.0f + 2.0f * kIconButton, kIconButton, kIconButton });
        // The chats' pill after the title, as wide as the chat's name, up to the badge or the header's buttons.
        {
            auto& chats = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(chatsItem_)]);
            const float x = chatsPillX();
            const float end = shown ? badge.bounds().x - 6.0f : newChat.x - 6.0f;
            chats.setBounds({ x, headerY_ + std::floor((kIconButton - kPillH) * 0.5f), std::max(0.0f, std::min(chats.preferredWidth(), end - x)),
                              kPillH });
        }
        // Either gone once clicked (this one the target now) or with the fold: the focus to the chat's own button.
        if ((focus_ == desktopItem_ || focus_ == desktopMarkItem_) && !items_[static_cast<std::size_t>(focus_)]->shown())
            setFocus(chatToggleItem_);
        ++layoutRevision_;
        dirty_ = true;
    }

    float Panel::chatsPillX() const
    {
        const float tx = card_.x + kCardPad + 4.0f;
        return std::round(tx + 15.0f + funkgui::text::width(funkgui::FontService::get().atlas(), chatTitle().c_str(), chat::kTitle) + 8.0f);
    }

    std::optional<funkgui::Point> Panel::centreOfDesktopBadge() const
    {
        if (desktopItem_ < 0 || !items_[static_cast<std::size_t>(desktopItem_)]->shown())
            return std::nullopt;
        const funkgui::Rect r = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(desktopItem_)]).bounds();
        return funkgui::Point{ r.centreX(), r.centreY() };
    }

    std::optional<funkgui::Point> Panel::centreOfDesktopMark() const
    {
        if (desktopMarkItem_ < 0 || !items_[static_cast<std::size_t>(desktopMarkItem_)]->shown())
            return std::nullopt;
        const funkgui::Rect r = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(desktopMarkItem_)]).bounds();
        return funkgui::Point{ r.centreX(), r.centreY() };
    }

    // A row of the explorer's CLAUDE DESKTOP: rename, choose or clear the target, open the chat.
    void Panel::desktopRow(const ExplorerItem::Row& row)
    {
        const DesktopState& d = desktopState_;
        switch (row.index)
        {
            case kDesktopName:
                name_.open();
                focusBeforeName_ = focus_;
                setFocus(-1);
                ring_ = false;
                break;
            case kDesktopUse:
                port_.chooseDesktop(d.id, !d.chosen);
                notice(d.chosen ? std::string("CLAUDE DESKTOP: NONE CHOSEN") : "CLAUDE DESKTOP: ITS CALLS COME HERE NOW");
                break;
            case kDesktopLast:
                setChatOpen(true);
                break;
            case kDesktopOther:
                for (const DesktopPeer& o : d.others)
                    if (o.id == row.key)
                    {
                        port_.chooseDesktop(o.id, !o.target);
                        notice(o.target ? std::string("CLAUDE DESKTOP: NONE CHOSEN") : "CLAUDE DESKTOP: ITS CALLS GO TO " + upper(o.name));
                    }
                break;
            default:
                break;
        }
        syncDesktop();
        dirty_ = true;
    }

    void Panel::afterName(bool wasOpen)
    {
        if (const std::string n = name_.takeNotice(); !n.empty())
            notice(n);
        if (wasOpen && !name_.isOpen())
        {
            setFocus(focusBeforeName_ >= 0 ? focusBeforeName_ : editorItem_);
            focusBeforeName_ = -1;
            syncDesktop();
        }
        dirty_ = true;
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
        std::vector<std::string> efforts = chatEfforts();
        if (host_ == nullptr || efforts.empty())
            return;
        // The default marked: Anthropic's medium, another provider's model's own as its server says it. Where that is
        // not said, the first row is the model's own default ("": none chosen, nothing sent).
        const bool anthropic = chat_.provider.kind == "anthropic";
        std::string byDefault = anthropic ? std::string("medium") : std::string();
        if (!anthropic)
            for (const ChatModel& m : chat_.models)
                if (m.id == chat_.model)
                    byDefault = m.defaultEffort;
        if (!anthropic && byDefault.empty())
            efforts.insert(efforts.begin(), std::string());
        funkgui::MenuRequest request;
        int id = 1, current = 0;
        for (const std::string& e : efforts)
        {
            if (e == chat_.effort)
                current = id;
            const std::string label = e.empty() ? std::string("The model's default")
                                                : effortName(e) + (e == byDefault ? " (the default)" : "");
            request.items.push_back(dropdown::choice(id++, label, e == chat_.effort));
        }
        dropdown::dropdownMenu(request, triggerRect(thinkingItem_), current);
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
        // Two choices: the model that builds (writes the files, runs, measures, answers), and the one that plans each
        // request first, or none (ChatAgent.h, "A planner").
        // Each a radio set under its header; the builder's choice highlighted when it opens.
        constexpr int kPlanNone = 1000, kPlanFirst = 1001;
        funkgui::MenuRequest request;
        request.items.push_back(dropdown::header(chat_.provider.kind == "anthropic" ? std::string("Build with")
                                                                                   : "Build with (" + asciiText(chat_.providerName) + ")"));
        int id = 1, current = 0;
        for (const ChatModel& m : chat_.models)
        {
            if (m.id == chat_.model)
                current = id;
            request.items.push_back(dropdown::choice(id++, m.name, m.id == chat_.model));
        }
        if (chat_.provider.kind == "anthropic")      // the planner is Anthropic's (another provider's models build alone)
        {
            request.items.push_back(dropdown::separator());
            request.items.push_back(dropdown::header("Plan with (each request, first)"));
            request.items.push_back(dropdown::choice(kPlanNone, "No planner", chat_.planner.empty()));
            id = kPlanFirst;
            for (const ChatModel& m : chat_.models)
                request.items.push_back(dropdown::choice(id++, m.name, m.id == chat_.planner));
        }
        dropdown::dropdownMenu(request, triggerRect(modelItem_), current);
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

    // A new chat: nothing is forgotten (the one shown stays in the chat list), so nothing is asked.
    void Panel::newChat()
    {
        port_.chat().newChat();
        chatAt_ = -1.0;
        dirty_ = true;
    }

    std::string Panel::chatsLabel() const { return asciiText(chat_.title.empty() ? std::string("New chat") : chat_.title); }

    // The chat card's title: Claude, or the provider another model runs on.
    std::string Panel::chatTitle() const
    {
        return chat_.provider.kind == "anthropic" || chat_.providerName.empty() ? std::string("Claude") : asciiText(chat_.providerName);
    }

    // The chats' menu (ChatPort::chatList): the project's chats on this Mac, the newest first, the one shown dotted and
    // each one's time in the shortcut column; then New chat, Rename and Delete, which act on the chat shown.
    void Panel::openChats()
    {
        if (host_ == nullptr)
            return;
        const ChatList list = port_.chat().chatList();
        constexpr int kNew = 100001, kRename = 100002, kDelete = 100003;
        const int n = static_cast<int>(std::min<std::size_t>(list.chats.size(), 100000));
        funkgui::MenuRequest request;
        request.items.push_back(dropdown::header(list.owner.empty() ? std::string("Chats with no project open")
                                                                    : "Chats of " + asciiText(list.owner)));
        int current = 0;
        for (int i = 0; i < n; ++i)
        {
            const ChatListEntry& c = list.chats[static_cast<std::size_t>(i)];
            funkgui::MenuItem item = dropdown::choice(i + 1, asciiText(c.title), c.current);
            item.shortcut = c.when;
            if (c.current)
                current = i + 1;
            request.items.push_back(std::move(item));
        }
        request.items.push_back(dropdown::separator());
        request.items.push_back(dropdown::command(kNew, "New chat"));
        request.items.push_back(dropdown::command(kRename, "Rename this chat..."));
        request.items.push_back(dropdown::command(kDelete, "Delete this chat..."));
        dropdown::dropdownMenu(request, triggerRect(chatsItem_), current);
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        std::vector<std::string> ids;
        for (int i = 0; i < n; ++i)
            ids.push_back(list.chats[static_cast<std::size_t>(i)].id);
        host_->showMenu(request, [this, ids](int chosen) {
            if (chosen >= 1 && chosen <= static_cast<int>(ids.size()))
                port_.chat().openChat(ids[static_cast<std::size_t>(chosen - 1)]);
            else if (chosen == kNew)
                port_.chat().newChat();
            else if (chosen == kRename)
                openRenameDialog();
            else if (chosen == kDelete)
                askDeleteChat();
            else
                return;
            chatAt_ = -1.0;
            dirty_ = true;
        });
    }

    // Delete asks first, under the pill: the chat goes from this Mac for good.
    void Panel::askDeleteChat()
    {
        if (host_ == nullptr)
            return;
        funkgui::MenuRequest request;
        request.items.push_back({ 1, "Delete \"" + chatsLabel() + "\" from " + kThisMachine + " (it cannot be undone)" });
        request.anchor = triggerRect(chatsItem_);
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) {
            if (chosen != 1)
                return;
            port_.chat().deleteChat();
            chatAt_ = -1.0;
            dirty_ = true;
        });
    }

    // ---- the key dialog -----------------------------------------------------------------------------------------------
    Panel::KeyDialog Panel::keyDialog() const
    {
        KeyDialog d;
        const float w = std::min(kKeyDialogW, width_ - 2.0f * kMargin);
        // Anthropic's as it always was; another provider's a row a field (its server's address, its key, its model).
        const int fields = renaming_ ? 1 : dialogFields();
        const int rows = (fields & 1) + ((fields >> 1) & 1) + ((fields >> 2) & 1);
        const bool other = !renaming_ && !dialogAnthropic();
        const float h = renaming_ ? kRenameDialogH : other ? kProviderRowsTop + kProviderRowPitch * static_cast<float>(rows) + 76.0f : kKeyDialogH;
        d.box = { std::round((width_ - w) * 0.5f), std::round((height_ - h) * 0.42f), w, h };
        if (!renaming_)
            d.provider = { d.box.right() - 24.0f - kProviderPillW, d.box.y + 22.0f, kProviderPillW, 30.0f };
        if (other)
        {
            const float fx = d.box.x + 24.0f + kProviderLabelW, fw = w - 48.0f - kProviderLabelW;
            float y = d.box.y + kProviderRowsTop;
            const auto row = [&](funkgui::Rect& r) {
                r = { fx, y, fw, 34.0f };
                y += kProviderRowPitch;
            };
            if ((fields & 2) != 0)
                row(d.base);
            row(d.field);
            if ((fields & 4) != 0)
                row(d.model);
        }
        else
            d.field = { d.box.x + 24.0f, d.box.y + (renaming_ ? 102.0f : 134.0f), w - 48.0f, 34.0f };
        const float by = d.box.bottom() - 24.0f - 32.0f;
        d.save = { d.box.right() - 24.0f - 96.0f, by, 96.0f, 32.0f };
        d.cancel = { d.save.x - 8.0f - 84.0f, by, 84.0f, 32.0f };
        d.remove = { d.box.x + 24.0f, by, 104.0f, 32.0f };
        return d;
    }

    const ChatPreset& Panel::dialogPresetRef() const
    {
        return kChatPresets[static_cast<std::size_t>(std::clamp(dialogPreset_, 0, static_cast<int>(std::size(kChatPresets)) - 1))];
    }

    bool Panel::dialogAnthropic() const { return std::string_view(dialogPresetRef().kind) == "anthropic"; }

    // Anthropic's, and the chat's provider now: the dialog as it always was (its key alone).
    bool Panel::dialogAsBefore() const { return dialogAnthropic() && chat_.provider.kind == "anthropic"; }

    int Panel::dialogFields() const
    {
        const std::string_view kind = dialogPresetRef().kind;
        return kind == "anthropic" ? 1 : kind == "openai" ? (1 | 2 | 4) : (1 | 4);
    }

    ChatProvider Panel::dialogProvider() const
    {
        ChatProvider p;
        p.kind = dialogPresetRef().kind;
        if (p.kind == "openai")
            p.base = baseEdit_.buffer;
        if (p.kind != "anthropic")
            p.model = modelEdit_.buffer;
        return p;
    }

    bool Panel::dialogHasKey() const { return dialogAsBefore() ? chat_.hasKey : dialogInfo_.hasKey; }

    funkgui::text::LineEdit& Panel::dialogEdit() { return dialogField_ == 1 ? baseEdit_ : dialogField_ == 2 ? modelEdit_ : keyEdit_; }

    // A preset shown: its server's address (a custom server's the chat's, when it talks to one), the model kept with it,
    // the key field empty (a key stored stays unless one is typed), the keys going to the first field it needs.
    void Panel::chooseKeyPreset(int preset)
    {
        dialogPreset_ = std::clamp(preset, 0, static_cast<int>(std::size(kChatPresets)) - 1);
        const ChatPreset& p = dialogPresetRef();
        const bool custom = std::string_view(p.id) == "custom";
        const bool current = chat_.provider.kind == p.kind
                          && (std::string_view(p.kind) != "openai" || custom ? true : chat_.provider.base == p.base);
        std::string base = p.base;
        if (custom)
            base = chat_.provider.kind == "openai" && !std::any_of(std::begin(kChatPresets), std::end(kChatPresets), [&](const ChatPreset& q) {
                       return std::string_view(q.kind) == "openai" && chat_.provider.base == q.base && std::string_view(q.id) != "custom";
                   })
                       ? chat_.provider.base
                       : std::string();
        baseEdit_.set(base, kMaxBaseLength);
        baseEdit_.allSelected = false;
        ChatProvider asked;
        asked.kind = p.kind;
        asked.base = base;
        dialogInfo_ = port_.chat().providerInfo(asked);
        modelEdit_.set(current && !custom ? chat_.model : dialogInfo_.model, kMaxModelLength);
        modelEdit_.allSelected = false;
        keyEdit_.set("", kMaxKeyLength);
        keyWhy_.clear();
        dialogField_ = custom && base.empty() ? 1 : (dialogInfo_.keyless && std::string_view(p.kind) == "openai") ? 2 : 0;
        dirty_ = true;
    }

    // The Provider pill's menu: the presets, the one shown dotted.
    void Panel::openProviders()
    {
        if (host_ == nullptr)
            return;
        funkgui::MenuRequest request;
        request.items.push_back(dropdown::header("Chat through"));
        int id = 1;
        for (const ChatPreset& p : kChatPresets)
        {
            request.items.push_back(dropdown::choice(id, p.name, id - 1 == dialogPreset_));
            ++id;
        }
        dropdown::dropdownMenu(request, keyDialog().provider, dialogPreset_ + 1);
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) {
            if (chosen >= 1 && chosen <= static_cast<int>(std::size(kChatPresets)))
                chooseKeyPreset(chosen - 1);
        });
    }

    void Panel::openKeyDialog()
    {
        if (keyOpen_)
            return;
        keyOpen_ = true;
        // The provider the chat talks to: its preset (another OpenAI-compatible server's "custom").
        int preset = 0;
        for (std::size_t i = 0; i < std::size(kChatPresets); ++i)
        {
            const ChatPreset& p = kChatPresets[i];
            if (chat_.provider.kind == p.kind
                && (chat_.provider.kind != "openai" || chat_.provider.base == p.base || std::string_view(p.id) == "custom"))
            {
                preset = static_cast<int>(i);
                break;
            }
        }
        chooseKeyPreset(preset);
        keyEdit_.set("", kMaxKeyLength);
        keyWhy_.clear();
        focusBeforeKey_ = focus_;
        setFocus(-1);
        ring_ = false;
        dirty_ = true;
    }

    // The rename (the chats' menu): the key dialog's box with a plain field, the chat's name in it, selected.
    void Panel::openRenameDialog()
    {
        if (keyOpen_)
            return;
        keyOpen_ = true;
        renaming_ = true;
        keyEdit_.set(chatsLabel(), static_cast<int>(kMaxChatTitle));
        keyWhy_.clear();
        focusBeforeKey_ = focus_;
        setFocus(-1);
        ring_ = false;
        dirty_ = true;
    }

    void Panel::renameFromDialog()
    {
        port_.chat().renameChat(keyEdit_.buffer);
        closeKeyDialog();
        chatAt_ = -1.0;
        refreshChat(host_ != nullptr ? host_->nowSeconds() : 0.0);
    }

    void Panel::closeKeyDialog()
    {
        keyOpen_ = false;
        renaming_ = false;
        keyEdit_.set("", kMaxKeyLength);                 // nothing of the key stays in the panel
        keyWhy_.clear();
        pendingSend_.clear();
        if (focusBeforeKey_ >= 0)
            setFocus(focusBeforeKey_);
        focusBeforeKey_ = -1;
        dirty_ = true;
    }

    // Closes the dialog and sends the message that waited for it, now that the chat can.
    void Panel::saveKeyFromDialog()
    {
        if (renaming_)
        {
            renameFromDialog();
            return;
        }
        std::string typed = keyEdit_.buffer;
        while (!typed.empty() && typed.back() == ' ')
            typed.pop_back();
        while (!typed.empty() && typed.front() == ' ')
            typed.erase(typed.begin());
        // Anthropic's key, the chat's provider now: as it always was. Back to Anthropic: its key stored, or one typed.
        // Another provider: its settings and its key (none typed keeps the one stored; a server here needs none).
        if (typed.empty() && dialogAnthropic() && (dialogAsBefore() || !dialogInfo_.hasKey))
        {
            keyWhy_ = shortcutText("Paste a key first (Cmd-V).");
            dirty_ = true;
            return;
        }
        std::string why;
        const bool ok = dialogAsBefore() ? port_.chat().saveKey(typed, why) : port_.chat().setProvider(dialogProvider(), typed, why);
        if (!ok)
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
        if (dialogAnthropic())
            port_.chat().forgetKey();
        else
            port_.chat().forgetProviderKey(dialogProvider());
        closeKeyDialog();
        chatAt_ = -1.0;
        refreshChat(host_ != nullptr ? host_->nowSeconds() : 0.0);
    }

    // Every key is the dialog's while it is open: Return saves, Escape cancels, Cmd-V pastes; the caret stays at the end
    // (the key is masked, so there is nothing to move it over).
    bool Panel::keyDialogKey(const funkgui::KeyEvent& e)
    {
        using funkgui::Key;
        if (renaming_)
        {
            // A name: the field's own keys (the caret moves), Return renames, Escape cancels, Cmd-V pastes.
            const int most = static_cast<int>(kMaxChatTitle);
            if (e.key == Key::enter)
                renameFromDialog();
            else if (e.key == Key::escape)
                closeKeyDialog();
            else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
            {
                if (host_ != nullptr)
                    keyEdit_.paste(host_->clipboardText(), most);
            }
            else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
                keyEdit_.allSelected = !keyEdit_.buffer.empty();
            else if (e.key != Key::tab && e.key != Key::up && e.key != Key::down)
                keyEdit_.key(e, most);
            dirty_ = true;
            return true;
        }
        if (e.key == Key::enter)
            saveKeyFromDialog();
        else if (e.key == Key::escape)
            closeKeyDialog();
        else if (e.key == Key::tab && !dialogAnthropic())
        {
            // The next field shown (Shift: the one before): the address, the key, the model.
            const int fields = dialogFields();
            const int order[3] = { 1, 0, 2 };
            int at = 0;
            for (int i = 0; i < 3; ++i)
                if (order[i] == dialogField_)
                    at = i;
            for (int step = 1; step <= 3; ++step)
            {
                const int next = order[(at + (e.mods.shift ? 3 - step : step)) % 3];
                if ((fields & (1 << next)) != 0)
                {
                    dialogField_ = next;
                    break;
                }
            }
        }
        else if (dialogField_ != 0)
        {
            // The address or the model: a plain field (the caret moves; the text is no secret).
            funkgui::text::LineEdit& edit = dialogEdit();
            const int most = dialogField_ == 1 ? kMaxBaseLength : kMaxModelLength;
            if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
            {
                if (host_ != nullptr && edit.paste(host_->clipboardText(), most))
                    keyWhy_.clear();
            }
            else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
                edit.allSelected = !edit.buffer.empty();
            else if (e.key != Key::up && e.key != Key::down && edit.key(e, most))
                keyWhy_.clear();
            dirty_ = true;
            return true;
        }
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
        else if (!renaming_ && dialogHasKey() && d.remove.contains(p))
            removeKeyFromDialog();
        else if (!renaming_ && d.provider.contains(p))
            openProviders();
        else if (!renaming_ && d.base.contains(p))
            dialogField_ = 1;
        else if (!renaming_ && d.model.contains(p))
            dialogField_ = 2;
        else if (!renaming_ && d.field.contains(p))
            dialogField_ = 0;
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
        const ChatPreset& preset = dialogPresetRef();
        const bool other = !renaming_ && !dialogAnthropic();
        c.rrect(x, d.box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, renaming_ ? chat::Icon::pencil : chat::Icon::key, x + 15.0f, d.box.y + 37.0f, 15.0f, p.accent);
        const std::string title = renaming_ ? std::string("Rename this chat") : other ? std::string(preset.name) : std::string("Anthropic API key");
        const float titleW = renaming_ ? d.box.w - 48.0f - 42.0f : d.provider.x - 12.0f - (x + 42.0f);
        c.text(fitText(c, title, chat::kH2, titleW).c_str(), x + 42.0f,
               c.capCentreTop(d.box.y + 37.0f, chat::kH2), chat::kH2, p.text);

        // The lines under the title: what the key is, where it goes, and whether one is stored; or what a chat's name is.
        std::vector<std::string> lines;
        const std::string host = std::string(preset.base).empty() ? std::string() : std::string(preset.base).substr(std::string(preset.base).find("://") + 3);
        const std::string server = host.substr(0, host.find('/'));
        if (renaming_)
            lines = { "Its name in the chat list (empty: its first message's words).",
                      std::string("The chats stay on ") + kThisMachine
                          + (chat_.owner.empty() ? ", with this plugin (no project is open)." : ", with the project.") };
        else if (!other)
            lines = { "Claude runs on your own key, from console.anthropic.com (API keys).",
                      std::string("It is stored encrypted on ") + kThisMachine + ", for both playground plugins,",
                      "and sent only to api.anthropic.com. Requests are billed to it.",
                      dialogHasKey() ? std::string("A key is stored, and the chat uses it.") : std::string() };
        else if (std::string_view(preset.id) == "custom")
            lines = { "Any server that speaks OpenAI's Chat Completions, by its base URL:",
                      std::string("https://, or http:// for one on ") + kThisMachine + " or the local network. Its",
                      "key, if it needs one, is stored encrypted and sent only to that server." };
        else if (std::string(preset.key).empty())
            lines = { std::string("A model server on ") + kThisMachine + ": no key, and nothing leaves " + kThisMachine + ".",
                      "Give its model a context of 32K tokens or more, and one that calls tools.",
                      std::string("Start it first") + (std::string_view(preset.id) == "ollama" ? " (ollama serve)." : " (its Developer tab: Start Server).") };
        else
            lines = { "Chat through " + std::string(preset.name) + " on your own key, from " + preset.keys + ".",
                      std::string("It is stored encrypted on ") + kThisMachine + " and sent only to " + server + ".",
                      dialogHasKey() ? std::string("A key is stored: leave the field empty to keep it.")
                                     : std::string("Requests are billed to it.") };
        float y = d.box.y + 66.0f;
        for (const std::string& line : lines)
        {
            c.text(fitText(c, line, chat::kSmall, d.box.w - 48.0f).c_str(), x, c.capCentreTop(y + 8.0f, chat::kSmall),
                   chat::kSmall, p.soft);
            y += 16.0f;
        }

        // The Provider pill: a dropdown's trigger (Dropdown.h), outlined; its menu the presets.
        if (!renaming_)
        {
            const funkgui::Rect r = d.provider;
            dropdown::State st;
            st.hover = hasPointer_ && r.contains(pointer_);
            st.open = menuOpenAt(r);
            const funkgui::Col ink = dropdown::drawTrigger(c, p, r, dropdown::Base::outlined, st);
            c.text(fitText(c, "Provider", chat::kSmall, dropdown::labelRoom(r, r.x + 14.0f)).c_str(), r.x + 14.0f,
                   c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, ink);
        }

        // A field's box: lit while the keys go to it (red while it refused what was typed).
        const auto box = [&](const funkgui::Rect& f, bool focused) {
            const funkgui::Col edge = !focused ? p.composerEdge
                                    : keyWhy_.empty() ? funkgui::mix(p.composerEdge, p.accent, 0.45f) : p.danger;
            c.rrect(f.x, f.y, f.w, f.h, 10.0f, p.composer, 1.0f, edge);
        };
        // A plain field (a name, an address, a model): the text, the selection, the caret where it is.
        const auto plain = [&](const funkgui::Rect& f, const funkgui::text::LineEdit& edit, const std::string& hint, bool focused) {
            const float textTop = c.capCentreTop(f.centreY(), chat::kBody);
            const float maxW = f.w - 28.0f;
            const std::string& b = edit.buffer;
            const std::string shownText = asciiText(b);
            if (focused && edit.allSelected && !b.empty())
                c.rrect(f.x + 12.0f, f.y + 8.0f, std::min(c.textWidth(shownText.c_str(), chat::kBody), maxW) + 4.0f, f.h - 16.0f,
                        3.0f, th.accentDim);
            if (b.empty())
                c.text(fitText(c, hint, chat::kBody, maxW).c_str(), f.x + 14.0f, textTop, chat::kBody, p.faint);
            else
                c.text(fitText(c, shownText, chat::kBody, maxW).c_str(), f.x + 14.0f, textTop, chat::kBody, p.text);
            if (!focused)
                return;
            const std::string before = asciiText(b.substr(0, static_cast<std::size_t>(std::clamp(edit.caret, 0, static_cast<int>(b.size())))));
            const float cx = f.x + 14.0f + std::min(c.textWidth(before.c_str(), chat::kBody), maxW);
            c.rrect(cx, f.y + 9.0f, 1.5f, f.h - 18.0f, 0.75f, p.accent);
        };
        const auto label = [&](const funkgui::Rect& f, const char* text) {
            c.text(text, x, c.capCentreTop(f.centreY(), chat::kSmall), chat::kSmall, p.soft);
        };

        const funkgui::Rect f = d.field;
        const bool keyFocused = renaming_ || !other || dialogField_ == 0;
        box(f, keyFocused);
        const float textTop = c.capCentreTop(f.centreY(), chat::kBody);
        if (renaming_)
            plain(f, keyEdit_, "A name for this chat", true);
        else
        {
            // The key masked (its first and last characters shown), the caret at its end.
            const std::string shown = keyEdit_.buffer.empty() ? std::string() : maskedKey();
            const float maxW = f.w - 28.0f;
            const std::string hint = !other                   ? std::string("sk-ant-...   (Cmd-V pastes it)")
                                   : dialogInfo_.keyless       ? std::string("None needed")
                                   : dialogHasKey()            ? std::string("Stored: empty keeps it   (Cmd-V pastes another)")
                                   : std::string(preset.key).empty() ? std::string("Its key, if it needs one   (Cmd-V pastes it)")
                                                             : std::string(preset.key) + "...   (Cmd-V pastes it)";
            if (shown.empty())
                c.text(fitText(c, shortcutText(hint), chat::kBody, maxW).c_str(), f.x + 14.0f, textTop, chat::kBody, p.faint);
            else
            {
                const float w = std::min(c.textWidth(shown.c_str(), chat::kBody), maxW);
                if (keyEdit_.allSelected)
                    c.rrect(f.x + 12.0f, f.y + 8.0f, w + 4.0f, f.h - 16.0f, 3.0f, th.accentDim);
                c.text(fitText(c, shown, chat::kBody, maxW).c_str(), f.x + 14.0f, textTop, chat::kBody, p.text);
            }
            if (keyFocused)
            {
                const float cx = f.x + 14.0f + (shown.empty() ? 0.0f : std::min(c.textWidth(shown.c_str(), chat::kBody), maxW));
                c.rrect(cx, f.y + 9.0f, 1.5f, f.h - 18.0f, 0.75f, p.accent);
            }
        }
        if (other)
        {
            label(f, "Key");
            if (d.base.w > 0.0f)
            {
                box(d.base, dialogField_ == 1);
                plain(d.base, baseEdit_, "https://host/v1", dialogField_ == 1);
                label(d.base, "Server");
            }
            if (d.model.w > 0.0f)
            {
                box(d.model, dialogField_ == 2);
                plain(d.model, modelEdit_, dialogInfo_.picksFirst ? "Empty: the first it lists (then the model menu)"
                                                                  : "Empty: choose it in the model menu after Save",
                      dialogField_ == 2);
                label(d.model, "Model");
            }
        }
        const funkgui::Rect last = d.model.w > 0.0f ? d.model : f;
        if (!keyWhy_.empty())
            c.text(fitText(c, asciiText(keyWhy_), chat::kSmall, d.box.w - 48.0f).c_str(), other ? x : f.x + 2.0f,
                   c.capCentreTop(last.bottom() + 14.0f, chat::kSmall), chat::kSmall, p.danger);

        // The buttons: pills.
        const auto button = [&](const funkgui::Rect& r, const char* text, int look) {   // 0 primary, 1 outlined, 2 quiet
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
            c.text(text, r.centreX(), c.capCentreTop(r.centreY(), st), st, ink, funkgui::Align::centre);
        };
        button(d.save, renaming_ ? "Rename" : "Save", 0);
        button(d.cancel, "Cancel", 1);
        if (dialogHasKey() && !renaming_)
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
        dropdown::dropdownMenu(request, triggerRect(examplesItem_), 0);
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) { loadExample(chosen - 1); });
    }

    // EXAMPLES: the example and the library files it #includes, as a new instance would play it (startAfresh).
    void Panel::loadExample(int i)
    {
        if (i < 0 || i >= static_cast<int>(std::size(kExamples)))
            return;
        const Example& e = kExamples[static_cast<std::size_t>(i)];
        startAfresh(withLibraryIncludes(singleFile(e.source)), "the example \"" + std::string(e.title) + "\"",
                    std::string("Load ") + e.title);
    }

    // EXAMPLES' and RANDOM's fresh start (EditorPort::startAfresh): `next` the only tabs, the dials at their defaults
    // once they compiled, no project open. Asking first (a menu at the pointer, its item `ask`) when the editor's files
    // have changes that nothing opened, saved or loaded holds. True when it started at once.
    bool Panel::startAfresh(const Sources& next, const std::string& what, const std::string& ask)
    {
        const auto load = [this, next, what] {
            port_.startAfresh(next, what);
            // The editor as a new window shows it: its tabs anew (no undo steps, each at its top), the IR tab and the
            // find bar closed, no problems until it has compiled, the interface from its top at its first pages.
            closeIrView();
            files().assist().closeFind();
            seenDraft_ = port_.draftRevision();
            given_ = port_.draft();
            files().merge(given_, true);
            syncedFiles_ = files().revision();
            problems_.clear();
            files().assist().setRunDiagnostics({}, {});
            files().assist().cancelCheck();
            guiView_->resetScroll();
            guiView_->setPages(guiPagesOf(panelSpec_.gui, {}));
            compiling_ = true;
            layout();
            setFocus(editorItem_);
            explorerStale_ = true;
            dirty_ = true;
        };
        files().endRename();
        (void)pushFiles();                               // what "changed" means is what the tabs show
        if (!port_.project().changed || host_ == nullptr)
        {
            load();
            return true;
        }
        funkgui::MenuRequest request;
        request.items.push_back({ 1, ask + " (the editor's changes are lost)" });
        request.anchor = { pointer_.x, pointer_.y, 1.0f, 1.0f };
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [load](int chosen) {
            if (chosen == 1)
                load();
        });
        return false;
    }

    // LIBRARY: the library's files under their categories' headers, a tick on those the program has, each label
    // starting with its file's name (the menu's type-ahead finds a file by it). One the program lacks becomes a tab of
    // its own, shown, and the program #includes it (one more line at its top, an edit Cmd-Z takes back); one it has is
    // shown. With no room for another tab, the others are greyed.
    void Panel::openLibrary()
    {
        if (host_ == nullptr)
            return;
        funkgui::MenuRequest request;
        const bool room = files().count() < static_cast<int>(kMaxFiles);
        std::string category;
        for (std::size_t i = 0; i < std::size(kLibrary); ++i)
        {
            const LibraryFile& f = kLibrary[i];
            if (category != f.category)
            {
                category = f.category;
                std::string title = category;
                if (!title.empty())
                    title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
                request.items.push_back(dropdown::header(std::move(title)));   // MenuLook writes it in capitals
            }
            const bool have = files().find(f.name) >= 0;
            request.items.push_back({ static_cast<int>(i) + 1, std::string(f.name) + "   " + f.summary, have || room, have });
        }
        dropdown::dropdownMenu(request, triggerRect(libraryItem_), 0);
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) {
            if (chosen >= 1 && chosen <= static_cast<int>(std::size(kLibrary)))
                addLibraryFile(kLibrary[static_cast<std::size_t>(chosen - 1)].name);
        });
    }

    // RANDOM: a program from the library's building blocks (Random.h) and the library files it #includes, a fresh
    // start as EXAMPLES makes one (startAfresh).
    bool Panel::makeRandom(uint32_t seed)
    {
        const random::Program p = random::make(seed);
        Sources next = singleFile(p.text);
        for (const std::string& name : p.library)
            if (const LibraryFile* f = findLibraryFile(name); f != nullptr && findFile(next, name) == nullptr)
                next.push_back({ f->name, f->source });
        return startAfresh(withLibraryIncludes(std::move(next)), "RANDOM's \"" + p.title + "\"",
                           "Make " + p.title + ", a random program");
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
        pushEditor();                                    // what Save saves is what the tabs show
        projects_.setSize(width_, height_);
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
        if (projects_.takeStartedSave())
            savingNotice();
        projectsAt_ = -100.0;                            // the explorer reads what it saved, opened or deleted
        const bool importIt = projects_.takeImport();
        const std::string exportIt = projects_.takeExport();
        const std::pair<std::string, int> abIt = projects_.takeAb();
        if (wasOpen && !projects_.isOpen() && exportIt.empty() && abIt.first.empty())
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
        if (!abIt.first.empty())
        {
            if (ab_.open(abIt.first, abIt.second))
                setFocus(-1);
            else
            {
                notice(upper(ab_.why()));
                setFocus(focusBeforeProjects_ >= 0 ? focusBeforeProjects_ : editorItem_);
                focusBeforeProjects_ = -1;
            }
        }
        dirty_ = true;
    }

    void Panel::openAb(const std::string& project, int version)
    {
        if (dialogOpen())
            return;
        files().endRename();
        pushEditor();                                    // Current is what the tabs show
        focusBeforeProjects_ = focus_;
        if (!ab_.open(project, version))
        {
            notice(upper(ab_.why()));
            focusBeforeProjects_ = -1;
            return;
        }
        setFocus(-1);
        ring_ = false;
        dirty_ = true;
    }

    void Panel::afterAb(bool wasOpen)
    {
        if (const std::string n = ab_.takeNotice(); !n.empty())
            notice(n);
        if (wasOpen && !ab_.isOpen())
        {
            setFocus(focusBeforeProjects_ >= 0 ? focusBeforeProjects_ : editorItem_);
            focusBeforeProjects_ = -1;
            projectsAt_ = -100.0;                        // the explorer reads the project a kept version opened
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
            request.suggestedName = ownName(project) + ext;    // a factory project's own name: no colon in a file's
            if (host_ == nullptr || !host_->chooseFiles(request, [write](const std::vector<std::string>& paths) {
                    if (!paths.empty())
                        write(paths.front());
                }))
            {
                // No chooser: the Desktop, under a name no file there has.
                const juce::File desk(juce::String::fromUTF8(port_.exportFolder().c_str()));
                const std::string base = ownName(project);
                juce::File to = desk.getChildFile(juce::String::fromUTF8((base + ext).c_str()));
                for (int k = 2; to.exists() && k < 1000; ++k)
                    to = desk.getChildFile(juce::String::fromUTF8((base + " " + std::to_string(k) + ext).c_str()));
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

    // ---- SETTINGS: the Standalone's audio and MIDI devices (SettingsCard.h; EditorPort::devices) ------------------------
    namespace
    {
        constexpr const char* kMutedText = "Input muted to avoid a feedback loop";
    }

    funkgui::Rect Panel::triggerRect(int item) const
    {
        return static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(item)]).bounds();
    }

    bool Panel::menuOpenAt(const funkgui::Rect& anchor) const noexcept
    {
        const funkgui::Rect& a = menuAnchor_;
        return menuOpen_ && std::abs(a.x - anchor.x) < 0.5f && std::abs(a.y - anchor.y) < 0.5f
            && std::abs(a.w - anchor.w) < 0.5f && std::abs(a.h - anchor.h) < 0.5f;
    }

    bool Panel::openSettings()
    {
        if (picker_.isOpen())                            // the app's Settings... over the presets' list: it goes
        {
            picker_.close();
            afterPicker(true);
        }
        if (dialogOpen() || port_.devices() == nullptr)
            return false;
        files().endRename();
        if (!settings_.open())
            return false;
        focusBeforeSettings_ = focus_;
        setFocus(-1);
        ring_ = false;
        dirty_ = true;
        return true;
    }

    void Panel::afterSettings(bool wasOpen)
    {
        if (wasOpen && !settings_.isOpen())
        {
            const int back = focusBeforeSettings_;
            setFocus(back >= 0 && items_[static_cast<std::size_t>(back)]->shown() ? back : editorItem_);
            focusBeforeSettings_ = -1;
        }
        dirty_ = true;
    }

    // Each refresh: the devices (read again when their serial moved), the app's Settings... (its count moved), and the
    // muted notice with its button.
    void Panel::syncSettings()
    {
        const bool wasOpen = settings_.isOpen();
        settings_.sync();
        if (wasOpen && !settings_.isOpen())
            afterSettings(true);                         // the window let go of its devices
        if (const uint64_t asked = port_.settingsAsked(); asked != seenSettingsAsk_)
        {
            seenSettingsAsk_ = asked;
            openSettings();
        }
        if (settings_.revision() != seenSettingsRev_)
        {
            seenSettingsRev_ = settings_.revision();
            dirty_ = true;
        }
        if (const bool muted = settings_.inputMuted(); muted != mutedShown_)
        {
            mutedShown_ = muted;
            dirty_ = true;
        }
        placeMutedNotice();
    }

    // The notice's button, left of the zoom's pill in the status bar (the zoom shows once a host offers steps).
    void Panel::placeMutedNotice()
    {
        auto& b = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(mutedItem_)]);
        const auto& zoom = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(zoomItem_)]);
        const float y = height_ - kFooterH + 4.0f, h = kFooterH - 8.0f;
        const funkgui::Rect before = b.bounds();
        b.setBounds({ 0.0f, y, 10.0f, h });
        const float w = b.preferredWidth();
        const float right = zoom.shown() ? zoom.bounds().x - 10.0f : width_ - kMargin;
        b.setBounds({ right - w, y, w, h });
        const funkgui::Rect now = b.bounds();
        if (now.x != before.x || now.y != before.y || now.w != before.w || now.h != before.h)
        {
            ++layoutRevision_;                           // its accessibility item moved
            dirty_ = true;
        }
    }

    funkgui::Point Panel::centreOfMutedSettings() const { return centreOf(mutedItem_); }

    // ---- the recorder: REC in the status bar while the capture tool records for Claude, and the user's marks ----------
    std::string Panel::recordText() const
    {
        char buf[96] = {};
        switch (recording_.stage)
        {
            case RecordingState::Stage::off:     return {};
            case RecordingState::Stage::waiting: return "REC   WAITING TO START";
            case RecordingState::Stage::recording:
            case RecordingState::Stage::ended:
                std::snprintf(buf, sizeof buf, "REC   %.1f S%s", recording_.seconds,
                              recording_.stage == RecordingState::Stage::ended ? ", ENDED" : "");
                break;
        }
        std::string out = buf;
        if (recording_.marks > 0)
            out += "   " + std::to_string(recording_.marks) + (recording_.marks == 1 ? " MARK" : " MARKS");
        return out;
    }

    // Ten looks a second at most (EditorPort::recording never waits): a frame when what the notice says changes. Mark
    // is placed again at each (as the muted notice's button is): the zoom's pill beside it shows once the host offers
    // steps, with no layout of its own.
    void Panel::syncRecording(double nowSec)
    {
        if (recordingAt_ >= 0.0 && nowSec - recordingAt_ < 0.1 && nowSec >= recordingAt_)
            return;
        recordingAt_ = nowSec;
        const RecordingState r = port_.recording();
        const bool shownBefore = recordShown();
        recording_ = r;
        if (const std::string text = recordText(); text != recordShownText_ || shownBefore != recordShown())
        {
            recordShownText_ = text;
            dirty_ = true;
        }
        if (recordShown())
            placeRecordNotice();
    }

    // The Mark button at the status bar's right (left of the muted notice and the zoom); REC's words left of it.
    void Panel::placeRecordNotice()
    {
        auto& b = static_cast<ButtonItem&>(*items_[static_cast<std::size_t>(markItem_)]);
        const auto& zoom = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(zoomItem_)]);
        const auto& muted = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(mutedItem_)]);
        const float y = height_ - kFooterH + 4.0f, h = kFooterH - 8.0f;
        const funkgui::Rect before = b.bounds();
        b.setBounds({ 0.0f, y, 10.0f, h });
        const float w = b.preferredWidth();
        float right = zoom.shown() ? zoom.bounds().x - 10.0f : width_ - kMargin;
        if (mutedShown_)                                 // the muted notice's words and button, right of it
            right = muted.bounds().x - 10.0f - funkgui::text::width(funkgui::FontService::get().atlas(), kMutedText, chat::kSmall)
                  - 18.0f - 14.0f;
        b.setBounds({ right - w, y, w, h });
        const funkgui::Rect now = b.bounds();
        if (now.x != before.x || now.y != before.y || now.w != before.w || now.h != before.h)
        {
            ++layoutRevision_;                           // its accessibility item moved
            dirty_ = true;
        }
    }

    funkgui::Point Panel::centreOfMark() const { return centreOf(markItem_); }

    funkgui::Point Panel::centreOfZoom() const { return centreOf(zoomItem_); }

    void Panel::markNow()
    {
        std::string why;
        recordingAt_ = -1.0;                             // looked at again in the next refresh
        if (const int number = port_.markRecording(why); number > 0)
            notice("MARKED M" + std::to_string(number) + " IN THE RECORDING");   // its own number, never a glance's
        else
            notice("NOT MARKED: " + upper(why));
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
        noticeUntil_ = (host_ != nullptr ? host_->nowSeconds() : 0.0) + kNoticeFor;
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
        // A netlist shown in its Schematic view: the part on that line selected and centred (the caret moved too).
        if (files().schematicShown())
            files().schematic().selectAtLine(p.line - 1, true);
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
    const GuiLayout& Panel::guiLayout() const noexcept { return guiView_->places(); }
    Scroll Panel::guiScroller(bool horizontal) const noexcept { return guiView_->scroller(horizontal); }
    funkgui::Rect Panel::guiBarArea(bool horizontal) const noexcept { return guiView_->barArea(horizontal); }

    void Panel::showState(ShownState state)
    {
        shown_ = std::move(state);
        showing_ = true;
        shownPlaying_ = false;
        shownAt_ = 0;
        dirty_ = true;
    }

    void Panel::setPanelView(bool on)
    {
        if (on == panelView_ || role_ == Role::code)    // the code window has the code alone
            return;
        panelView_ = on;
        port_.setPanelView(on);
        guiView_->resetScroll();
        if (on && (focus_ == editorItem_ || focus_ == problemsItem_))
            setFocus(-1);
        layout();
    }

    // ---- the code window ------------------------------------------------------------------------------------------
    void Panel::setWindowHooks(WindowHooks hooks)
    {
        windowHooks_ = std::move(hooks);
        layout();                                        // the window button comes, and the project's pill makes room
    }

    bool Panel::windowButtonShown() const
    {
        // The editor's Panel given a window's owner (the plugin's editor), and the code window's; never a picture's.
        return role_ == Role::code || (role_ == Role::editor && static_cast<bool>(windowHooks_.out));
    }

    funkgui::Point Panel::centreOfWindowButton() const { return centreOf(windowItem_); }

    void Panel::setEditorOut(bool out)
    {
        if (role_ == Role::picture)
            return;
        CodeWindow w = port_.codeWindow();
        if (w.out != out)
        {
            w.out = out;
            port_.setCodeWindow(w);
        }
        if (windowHooks_.out)
            windowHooks_.out(out);
        dirty_ = true;
    }

    // The editor (FilesItem: the tabs, their editors, CodeAssist, the IR tab, the netlists' views) and what follows it
    // from this Panel to `other` and back: the draft it was last given or merged (what mergeDraft and pushFiles compare
    // with), whether this Panel looks at the IR (EditorPort::addIrViewer: the count stays the same), its probes' owner
    // (the drawing's armed set stays armed). Both Panels are made alike, so the item has the same index and ids in both;
    // its hooks and the host it draws for are this Panel's again after.
    void Panel::swapEditor(Panel& other)
    {
        for (Panel* p : { this, &other })
        {
            if (p->focus_ == p->editorItem_)
                p->setFocus(-1);                         // the caret leaves the editor before it changes hands
            if (p->pressed_ == p->editorItem_)
                p->pressed_ = -1;
        }
        std::swap(items_[static_cast<std::size_t>(editorItem_)], other.items_[static_cast<std::size_t>(other.editorItem_)]);
        std::swap(given_, other.given_);
        std::swap(syncedFiles_, other.syncedFiles_);
        std::swap(seenDraft_, other.seenDraft_);
        std::swap(pushedFiles_, other.pushedFiles_);
        std::swap(irViewing_, other.irViewing_);
        std::swap(irSeen_, other.irSeen_);
        std::swap(probeOwner_, other.probeOwner_);
        for (Panel* p : { this, &other })
        {
            p->bindFiles();
            p->files().attach(p->host_);
            p->explorerStale_ = true;                    // the explorer's rows again (its files)
            p->dirty_ = true;
        }
    }

    void Panel::lendEditor(Panel& code)
    {
        if (role_ != Role::editor || code.role_ != Role::code || out_ || peer_ != nullptr || code.peer_ != nullptr)
            return;
        // What was typed until now goes to the processor first; the editor the code window takes is what the user had.
        // The copy this Panel gets in return (the files as the code window's Panel loaded them) is hidden and inert
        // until the editor comes back: no focus, no draft merged, nothing given (refresh, pushFiles).
        (void)pushFiles();
        swapEditor(code);
        peer_ = &code;
        code.peer_ = this;
        out_ = true;
        port_.addCodeWindow();                           // what a picture shows: the code is out now
        layout();                                        // the panel full size, the copy hidden
        code.layout();
        code.setFocus(code.editorItem_);
        code.ring_ = false;
    }

    void Panel::takeEditorBack()
    {
        if (role_ != Role::editor || !out_)
            return;
        bool caret = false;
        if (Panel* code = peer_; code != nullptr)
        {
            (void)code->pushFiles();
            caret = code->focus_ == code->editorItem_;
            swapEditor(*code);
            code->peer_ = nullptr;
            code->layout();
            peer_ = nullptr;
        }
        out_ = false;
        port_.removeCodeWindow();
        layout();
        if (caret)
            setFocus(editorItem_);
    }

    bool Panel::interfaceShown() const
    {
        const FilesItem& f = files();
        return !f.irShown() && f.active() >= 0 && f.active() < f.count() && isInterfaceFile(f.name(f.active()));
    }

    bool Panel::codePanelShown() const { return codePanel_[interfaceShown() ? 1 : 0]; }

    void Panel::setCodePanel(bool shown)
    {
        const bool gui = interfaceShown();
        if (codePanel_[gui ? 1 : 0] == shown)
            return;
        codePanel_[gui ? 1 : 0] = shown;
        port_.setCodePanel(gui, shown);
        layout();
        // A control of the panel that had the focus hides with it: the focus goes (layout() has hidden it).
        if (focus_ >= 0 && !items_[static_cast<std::size_t>(focus_)]->shown())
            setFocus(-1);
    }

    void Panel::syncCodePanel()
    {
        const bool code = port_.codePanel(false), gui = port_.codePanel(true);
        if (code == codePanel_[0] && gui == codePanel_[1])
            return;
        codePanel_[0] = code;
        codePanel_[1] = gui;
        if (!layoutStale())
            return;
        layout();
        if (focus_ >= 0 && !items_[static_cast<std::size_t>(focus_)]->shown())
            setFocus(-1);
    }

    bool Panel::layoutStale() const
    {
        const bool hide = role_ == Role::code || !codePanelShown();   // the code window never shows the panel
        return !full() && (files().schematicShown() != layoutSchematic_ || hide != panelHidden_);
    }

    funkgui::Point Panel::centreOfPanelSwitch() const
    {
        const funkgui::Rect r = files().assist().panelSwitchRect();
        return { r.centreX(), r.centreY() };
    }

    bool Panel::panelSwitchLabelled() const { return files().assist().panelSwitchLabelled(); }

    const GuiControlItem& Panel::guiControl(int slot) const
    {
        return static_cast<const GuiControlItem&>(*items_[static_cast<std::size_t>(guiView_->controlItem(std::clamp(slot, 0, kMaxControls - 1)))]);
    }

    const Item& Panel::guiItem(int kind, int k) const
    {
        const std::vector<int>& pool = kind == 0   ? guiView_->pads()
                                     : kind == 1 ? guiView_->envelopes()
                                     : kind == 2 ? guiView_->tabs()
                                     : kind == 3 ? guiView_->keyboards()
                                                 : guiView_->shaders();
        return *items_[static_cast<std::size_t>(pool[static_cast<std::size_t>(std::clamp(k, 0, static_cast<int>(pool.size()) - 1))])];
    }

    void Panel::chooseGuiPage(int node, int page)
    {
        if (!guiView_->choosePage(panelSpec_, node, page))
            return;
        port_.setGuiPages(guiPageTitles(panelSpec_.gui, guiView_->pages()));
        layout();
    }

    // The wheel over a .gui file's interface: its items move by the scroll and nothing is laid out again (GuiView.h).
    void Panel::scrollGui(float dx, float dy)
    {
        if (!guiView_->scrollBy(panelSpec_, dx, dy))
            return;
        ++layoutRevision_;                               // their accessibility items moved
        dirty_ = true;
    }

    funkgui::Point Panel::centreOfView(bool panel) const { return centreOf(panel ? panelViewItem_ : viewItem_); }

    std::string Panel::projectLabel() const
    {
        const ProjectState st = port_.project();
        if (st.name.empty())
            return "Projects";
        std::string label = asciiText(st.name) + (st.version > 0 && !isFactoryName(st.name) ? " v" + std::to_string(st.version) : "");
        if (st.edited || (st.preset.empty() && st.dialsEdited))
            label += " (edited)";
        if (!st.preset.empty() && !presetsShown())       // the presets right of it name it, and its dot
            label += "  -  " + asciiText(st.preset) + (st.dialsEdited ? " (edited)" : "");
        return label;
    }

    std::string Panel::presetLabel() const
    {
        if (cycle_.presets.empty())
            return "No presets";
        if (cycle_.current < 0 || cycle_.current >= static_cast<int>(cycle_.presets.size()))
            return "(no preset)";
        return asciiText(cycle_.presets[static_cast<std::size_t>(cycle_.current)].name);
    }

    std::string Panel::presetDetail() const
    {
        if (cycle_.presets.empty())
            return {};
        const std::string n = std::to_string(cycle_.presets.size());
        return cycle_.current < 0 ? "-/" + n : std::to_string(cycle_.current + 1) + "/" + n;
    }

    funkgui::Point Panel::centreOfPresetStep(int step) const
    {
        return items_[static_cast<std::size_t>(step < 0 ? presetPrevItem_ : presetNextItem_)]->centre();
    }

    funkgui::Point Panel::centreOfPresetName() const { return items_[static_cast<std::size_t>(presetItem_)]->centre(); }

    // The arrows, [ and ]: the project open's preset before or after the one now (wrapping; from none, the first or the
    // last), applied as PROJECTS applies one (its dials glide there; changes to the one before are not kept).
    void Panel::stepPresets(int step)
    {
        const PresetCycle before = port_.presetCycle();
        std::string why;
        if (!port_.stepPreset(step, why))
        {
            notice("NOT APPLIED: " + upper(why));
            return;
        }
        cycle_ = port_.presetCycle();
        notice(presetNotice(before, cycle_));
        dirty_ = true;
    }

    void Panel::openPresets()
    {
        if (!presetsShown() || picker_.isOpen())
            return;
        files().endRename();
        picker_.setSize(width_, height_);
        picker_.open(static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(presetItem_)]).bounds());
        focusBeforePicker_ = focus_;
        setFocus(-1);
        ring_ = false;
        dirty_ = true;
    }

    void Panel::afterPicker(bool wasOpen)
    {
        if (const std::string n = picker_.takeNotice(); !n.empty())
        {
            notice(n);
            cycle_ = port_.presetCycle();
        }
        if (wasOpen && !picker_.isOpen() && focusBeforePicker_ != -2)
        {
            setFocus(focusBeforePicker_);
            focusBeforePicker_ = -2;                     // given back once
        }
        dirty_ = true;
    }

    void Panel::openZoom()
    {
        const auto* z = static_cast<const ZoomModel*>(zoom_);
        if (host_ == nullptr || z == nullptr || z->count() == 0)
            return;
        funkgui::MenuRequest request;
        for (int i = 0; i < z->count(); ++i)
            request.items.push_back(dropdown::choice(i + 1, z->label(i), i == z->active(), z->enabled(i)));
        dropdown::dropdownMenu(request, triggerRect(zoomItem_), z->active() >= 0 ? z->active() + 1 : 0);
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this](int chosen) {
            auto* m = static_cast<ZoomModel*>(zoom_);
            if (m != nullptr && chosen >= 1 && chosen <= m->count() && gestures_)
                m->select(chosen - 1, *gestures_);
        });
    }

    float Panel::statusX() const
    {
        return programCard_.x + kInset + 14.0f + (explorerShown_ || full() ? 0.0f : 30.0f);   // after the explorer's button
    }

    bool Panel::statusWhole() const
    {
        // As draw() fits it (the canvas measures a hair wider than the atlas at some sizes: 2 px of slack).
        const float w = funkgui::text::width(funkgui::FontService::get().atlas(), statusShown().c_str(), chat::kSmall);
        return w + 2.0f <= actionsX_ - 12.0f - statusX();
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
        // A hidden item never has the focus (Items.h: no input): the editor in the PANEL view, the hidden copy of it
        // while the code window has it, a folded chat's box. A card closing gives it back to none then.
        if (item >= 0 && !items_[static_cast<std::size_t>(item)]->shown())
            item = -1;
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
        // Shown (the host ticks it): a reader of the meters, again if it had let them go while hidden (never the code
        // window's, which shows no display).
        tickedAt_ = host_ != nullptr ? host_->nowSeconds() : 0.0;
        idlesSinceTick_ = 0;
        if (!reading_ && role_ != Role::code)
        {
            port_.addMeterReader();
            reading_ = true;
        }
        refresh(host_ != nullptr ? host_->nowSeconds() : 0.0);
        if (layoutStale())
            layout();                                    // a netlist's view came or went, or the panel under the code
        syncWindowButtons();
        syncExplorer();
        // The message box follows its text, one row to eight: the conversation gives it the room.
        if (std::fabs(inputItem().desiredHeight(inputItem().box().w, kBarH) - composerH_) > 0.5f)
            layoutChat();
        // The displays: what the Engine published since the last frame, and the scopes' samples.
        {
            Meters m;
            const bool frames = showing_ && !shown_.frames.empty();
            if (frames)
            {
                // A picture's state (showState): its frame, as of this panel's program; the last one's readouts made
                // again from it.
                if (shownPlaying_ && shownAt_ + 1 < shown_.frames.size())
                {
                    ++shownAt_;
                    if (shownAt_ + 1 == shown_.frames.size())
                        for (const int i : itemOfDisplay_)
                            if (i >= 0)
                                static_cast<DisplayItem&>(*items_[static_cast<std::size_t>(i)]).readoutNow();
                }
                meters_ = shown_.frames[shownAt_];
                meters_.program = panelProgram_;
                meters_.tapTable = panelSpec_.tapTable;
            }
            else if (port_.meters(m))
                meters_ = m;
            // The fields are this panel's program's once the audio thread plays it; until then they are another's,
            // and read as 0. A panel's edit's new table read before this panel has its status, the last fields of its
            // own hold; its own new table not read yet, 0 (TapsShown).
            Meters shown = meters_;
            const bool fields = tapsShown_.show(shown, panelProgram_, panelSpec_.tapTable);
            if (showing_)
            {
                for (const auto& [tap, v] : shown_.taps)
                    if (tap >= 0 && tap < kMaxDisplays)
                        shown.taps[tap] = v;
                for (int c = 0; c < 2; ++c)
                {
                    if (shown_.inLevel >= 0.0f)
                        shown.inPeak[c] = shown.inRms[c] = shown_.inLevel;
                    if (shown_.outLevel >= 0.0f)
                        shown.outPeak[c] = shown.outRms[c] = shown_.outLevel;
                }
            }
            const auto scope = [this, frames](bool input, float* out, int n) {
                if (!frames)
                {
                    port_.scope(input, out, n);
                    return;
                }
                // The state's samples, the newest last (silence before them).
                const std::vector<float>& s = input ? shown_.scopeIn : shown_.scopeOut;
                const int have = std::min(n, static_cast<int>(s.size()));
                std::fill(out, out + (n - have), 0.0f);
                std::copy(s.end() - have, s.end(), out + (n - have));
            };
            updateDisplays(items_, itemOfDisplay_, shown, scope, *feed_, dt);
            // The controls' live= marks (none until the audio thread plays this program, but a state's taps: drawn as
            // asked) and the shaders' inputs.
            uint32_t stateTaps = 0;
            if (showing_)
                for (const auto& tv : shown_.taps)
                    if (tv.first >= 0 && tv.first < kMaxDisplays)
                        stateTaps |= 1u << tv.first;
            guiView_->tick(panelSpec_, shown, fields, stateTaps);
        }
        const int hovered = hasPointer_ && pressed_ < 0 ? itemAt(pointer_) : pressed_;
        const funkgui::Point far{ -1.0e4f, -1.0e4f };
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            const int idx = static_cast<int>(i);
            items_[i]->tick(dt, hovered == idx, ring_ && focus_ == idx, hasPointer_ ? pointer_ : far);
        }
        // Unchanged frames (Panel.h): a change not drawn yet, what moves, and what draw() would read now.
        undrawn_ = undrawn_ || dirty_;
        dirty_ = false;
        settling_ = moving_;
        moving_ = std::any_of(items_.begin(), items_.end(), [](const auto& it) { return it->shown() && !it->settled(); });
        watched_ = watchKey();
    }

    // Everything draw() reads that may change with no input and no motion: whether each item shows and, for one that
    // does, its key (Item::drawKey: draw() skips a hidden item, so what a hidden one would draw asks for no frame: a
    // live= knob on a page not shown, a dial the .gui file's interface hides), and the Panel's own words, states and
    // places (most of them mark dirty_ where they change already: this catches the rest).
    uint64_t Panel::watchKey() const
    {
        DrawKey k;
        for (const auto& it : items_)
        {
            const bool shown = it->shown();
            k.mix(shown);
            if (shown)
                k.mix(it->drawKey());
        }
        k.mix(status_).mix(meter_).mix(meterAlerts_).mix(meterLoad_).mix(port_.alerts() != 0).mix(projectText_).mix(title_)
            .mix(subtitle_).mix(activity_).mix(desktop_).mix(chat_.busy).mix(unread_).mix(frozenShown_).mix(mutedShown_)
            .mix(recordShownText_).mix(compiling_).mix(lastOk_).mix(edited_).mix(chatOpen_).mix(explorerShown_).mix(panelView_).mix(dropActive_)
            .mix(dropHover_).mix(keyOpen_).mix(renaming_).mix(keyWhy_).mix(keyEdit_.buffer).mix(keyEdit_.caret).mix(keyEdit_.allSelected)
            .mix(projects_.isOpen()).mix(export_.isOpen()).mix(settings_.isOpen()).mix(settings_.revision())
            .mix(name_.isOpen()).mix(name_.revision()).mix(desktopBadge_).mix(picker_.isOpen()).mix(picker_.revision())
            .mix(focus_).mix(ring_).mix(pressed_).mix(touched_).mix(hasPointer_).mix(pointer_).mix(width_).mix(height_)
            .mix(layoutRevision_).mix(buttonsRoom_);
        // The footer: the spec line of the control under the hand (a dial's value moves with the host's automation).
        if (!dialogOpen())
        {
            int src = pressed_;
            if (src < 0 && hasPointer_)
                src = itemAt(pointer_);
            if (src < 0)
                src = focus_ >= 0 ? focus_ : touched_;
            if (src >= 0)
            {
                char line[256] = {};
                items_[static_cast<std::size_t>(src)]->specLine(line, sizeof line);
                k.mix(static_cast<const char*>(line));
            }
        }
        return k.h;
    }

    bool Panel::needsDraw() const
    {
        if (undrawn_ || dirty_ || moving_ || settling_ || watched_ != drawnKey_)
            return true;
        // The safety net (Panel.h): a frame recorded once a second, which EditorHost submits only when it differs.
        return host_ != nullptr && host_->nowSeconds() - drawnAt_ >= kRedrawEvery;
    }

    void Panel::idle(double nowSec)
    {
        if (gestures_)
            gestures_->poll(nowSec);                     // closes a wheel gesture after its burst
        refresh(nowSec);                                 // a compile finishing while nothing moves
        // The host ticks a panel only while its window shows (EditorHost skips a minimised or hidden one) and calls
        // idle() whatever: idle calls with no tick between them, for long enough, mean no one sees the displays, so the
        // audio thread need not measure for them (Panel.h "The meters' reader").
        if (tickedAt_ < 0.0)
            tickedAt_ = nowSec;                          // never ticked: counted from the first idle call
        if (reading_ && ++idlesSinceTick_ >= kHiddenIdles && nowSec - tickedAt_ >= kHiddenAfter)
        {
            port_.removeMeterReader();
            reading_ = false;
        }
    }

    bool Panel::wantsFullRate() const
    {
        if (dirty_ || compiling_ || saving_ || inputHeld())
            return true;
        return std::any_of(items_.begin(), items_.end(), [](const auto& it) { return it->shown() && !it->settled(); });
    }

    // Input (Panel.h "Unchanged frames", kInputHold): the time of the last, which holds the full rate. `nudge`: tell the
    // host's pump now too, for the events FunkGui's EditorHost does not nudge it on (a release, the pointer leaving),
    // so the frame that draws them runs at the next display tick.
    void Panel::noteInput(bool nudge)
    {
        if (host_ == nullptr)
            return;
        inputAt_ = host_->nowSeconds();
        if (nudge)
            host_->nudgeFullRate();
    }

    bool Panel::inputHeld() const
    {
        return host_ != nullptr && host_->nowSeconds() - inputAt_ < kInputHold;
    }

    void Panel::draw(funkgui::Canvas& c, const funkgui::Theme& hostTheme)
    {
        // What this frame draws is what needsDraw() compares with from now on.
        undrawn_ = false;
        settling_ = false;
        drawnKey_ = watched_;
        drawnAt_ = host_ != nullptr ? host_->nowSeconds() : 0.0;
        const funkgui::Theme th = productTheme(hostTheme);
        files().setTheme(th);                            // the code's colours follow the theme
        const float contentW = width_ - 2.0f * kMargin;

        const chat::Palette pal = chat::palette(th);
        // The top bar: the product's mark, its name and kind; the project, the view's switch, the theme (items); the
        // load (the CPU's share), red while the output guard acts.
        {
            const funkgui::Canvas::Scope s(c, kHeaderTextTag, false);
            const funkgui::Rect bar{ kMargin, kTopBarY, contentW, kTopBarH };
            c.rrect(bar.x, bar.y, bar.w, bar.h, 12.0f, pal.card, 1.0f, pal.cardEdge);
            const float mid = bar.centreY();
            const float mx = bar.x + 10.0f + buttonsRoom_;     // after the Standalone window's buttons
            // The product's mark in its tile (LogoMark.h, generated by Tools/logo/logo.swift): the wave (AREA columns)
            // and the caret (one primitive) in the accent, the drawing hinted for the canvas's scale; the tile's corner
            // on a device px (whole px already: mx and the bar are).
            const float tx = c.snapX(mx), ty = c.snapY(mid - 0.5f * logo::kTile);
            markTile_ = { tx, ty };
            c.rrect(tx, ty, logo::kTile, logo::kTile, logo::kTileRadius, pal.accent.withAlpha(logo::kTileAlpha));
            {
                const funkgui::Canvas::Scope ls(c, kLogoTag, false);
                const logo::Mark& m = logo::mark(product::kIsInstrument, c.dpi());
                std::array<float, logo::kMaxPoints> xs{}, tops{}, bottoms{};
                for (std::size_t k = 0; k < static_cast<std::size_t>(m.wave.n); ++k)
                {
                    xs[k] = tx + m.wave.xs[k];
                    tops[k] = ty + m.wave.tops[k];
                    bottoms[k] = ty + m.wave.bottoms[k];
                }
                c.areaStrip(xs.data(), m.wave.n, tops.data(), bottoms.data(), 0.0f, pal.accent);
                const logo::Stem& st = m.caret;
                if (st.round)
                {
                    const float r = 0.5f * (st.x1 - st.x0), x = tx + 0.5f * (st.x0 + st.x1);
                    c.segment(x, ty + st.y0 + r, x, ty + st.y1 - r, st.x1 - st.x0, pal.accent);
                }
                else
                    c.rrect(tx + st.x0, ty + st.y0, st.x1 - st.x0, st.y1 - st.y0, 0.0f, pal.accent);
            }
            c.text(product::kName, mx + 36.0f, c.capCentreTop(mid - 7.0f, chat::kStrong), chat::kStrong, pal.text);
            c.text(subtitle_.c_str(), mx + 36.0f, c.capCentreTop(mid + 8.0f, chat::kSmall), chat::kSmall, pal.faint);
            // The Code / Panel switch's track, under its two halves.
            const funkgui::Rect code = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(viewItem_)]).bounds();
            const funkgui::Rect panel = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(panelViewItem_)]).bounds();
            c.rrect(code.x - 3.0f, code.y - 3.0f, panel.right() - code.x + 6.0f, code.h + 6.0f, 0.5f * (code.h + 6.0f), pal.raised,
                    1.0f, pal.cardEdge);
            // The load, the snowflake left of it while the dials are frozen (an item: FrozenItem), the alerts left of that.
            const funkgui::Rect theme = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(themeItem_)]).bounds();
            const bool alarm = port_.alerts() != 0;
            const funkgui::Col ink = alarm ? pal.danger : pal.faint;
            const MeterPlaces m = meterPlaces(theme.x, c.textWidth(meterLoad_.c_str(), chat::kSmall), frozenShown_,
                                              c.textWidth("   ", chat::kSmall));
            const float top = c.capCentreTop(mid, chat::kSmall);
            c.text(meterLoad_.c_str(), m.loadRight, top, chat::kSmall, ink, funkgui::Align::right);
            if (!meterAlerts_.empty())
                c.text(meterAlerts_.c_str(), m.alertsRight, top, chat::kSmall, ink, funkgui::Align::right);
        }

        // The program's card: its header's status (a light, and what the last run did), left of its actions.
        {
            const funkgui::Canvas::Scope s(c, kStatusTag, false);
            const funkgui::Rect& r = programCard_;
            c.rrect(r.x, r.y, r.w, r.h, kCardRadius, pal.card, 1.0f, pal.cardEdge);
            const float mid = kActionsY + 0.5f * kActionH;
            const bool failed = !lastOk_ && !compiling_;
            const std::string shown = statusShown();
            const float tx = statusX();
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
        // the message box (what Claude is doing, else the last reply's tokens). None in the code window.
        if (hasChat())
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
        if (chatOpen_ && hasChat())
        {
            const funkgui::Canvas::Scope s(c, kChatTag, false);
            const chat::Palette p = chat::palette(th);
            const float ty = headerY_ + kIconButton * 0.5f;
            const float tx = card_.x + kCardPad + 4.0f;
            c.disc(tx + 4.0f, ty, chat_.busy ? 4.0f : 3.5f, chat_.busy ? p.accent : funkgui::mix(p.card, p.accent, 0.75f));
            c.text(chatTitle().c_str(), tx + 15.0f, c.capCentreTop(ty, chat::kTitle), chat::kTitle, p.text);
            // After the chats' pill, while Claude Desktop acts on or talks to this instance: what it does, its name for it.
            const funkgui::Rect pill = triggerRect(chatsItem_);
            const float sub = pill.right() + 8.0f;
            const bool badge = desktopItem_ >= 0 && items_[static_cast<std::size_t>(desktopItem_)]->shown();
            const float subRoom = (badge ? static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(desktopItem_)]).bounds().x - 10.0f
                                         : card_.right() - kCardPad - 3.0f * kIconButton - 16.0f)
                                - sub;
            if (!desktop_.empty() && subRoom > 60.0f)
                c.text(fitText(c, desktop_, chat::kSmall, subRoom).c_str(), sub, c.capCentreTop(ty, chat::kSmall), chat::kSmall, p.faint);
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
        const funkgui::Theme gth = guiView_->theme(th, panelSpec_);
        guiView_->drawGround(c, gth, panelSpec_, kSectionTag);
        const funkgui::Rect& area = guiView_->places().area;
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            if (!items_[i]->shown())
                continue;
            if (gui && guiView_->owns(i))
            {
                const funkgui::Canvas::ClipScope clip(c, area);
                items_[i]->draw(c, gth, ring_ && focus_ == static_cast<int>(i));
            }
            else
                items_[i]->draw(c, th, ring_ && focus_ == static_cast<int>(i));
        }
        files().drawOverlays(c, th, width_, height_);    // the completion list, the signature, the hover card
        if (gui)
            guiView_->drawScrollBars(c, th);

        // Footer: the spec line of the control under the hand, else the focused one, else the last one touched; the
        // dialog's own while it is open.
        char line[256] = {};
        if (settings_.isOpen())
            std::snprintf(line, sizeof line, "SETTINGS   TAB THE NEXT CONTROL   RETURN OR SPACE CHOOSES   ESCAPE OR DONE CLOSES");
        else if (keyOpen_ && renaming_)
            std::snprintf(line, sizeof line, "RENAME   ITS NAME IN THE CHAT LIST   RETURN RENAMES IT   ESCAPE CANCELS");
        else if (keyOpen_)
            std::snprintf(line, sizeof line, "%s", dialogAnthropic() ? "API KEY   CMD-V PASTES IT   RETURN SAVES IT   ESCAPE CANCELS"
                                                                     : "PROVIDER   TAB THE NEXT FIELD   CMD-V PASTES   RETURN SAVES   ESCAPE CANCELS");
        else if (export_.isOpen() && export_.readOnly())
            std::snprintf(line, sizeof line, "EXPORT   A FACTORY PROJECT: ITS INFO AS IT IS   ESCAPE CLOSES");
        else if (export_.isOpen())
            std::snprintf(line, sizeof line, "EXPORT   TAB THE NEXT FIELD   A PILL TOGGLES   ESCAPE KEEPS THE INFO AND CLOSES");
        else if (name_.isOpen())
            std::snprintf(line, sizeof line, "NAME   RETURN SAVES IT (EMPTY: ITS NUMBER'S)   ESCAPE CANCELS");
        else if (ab_.isOpen())
            std::snprintf(line, sizeof line, "A/B   A, B OR SPACE SWITCH   UP AND DOWN CHOOSE B, SHIFT A   TAB THE NEXT FILE   "
                                             "RETURN KEEPS THE ONE PLAYING   ESCAPE GOES BACK");
        else if (picker_.isOpen())
            std::snprintf(line, sizeof line, "%s",
                          picker_.categoryMode() ? "PRESETS   TYPE A CATEGORY   RETURN SETS IT   ESCAPE GOES BACK"
                                                 : "PRESETS   TYPE TO SEARCH   UP AND DOWN CHOOSE   RETURN OR A CLICK APPLIES   "
                                                   "RIGHT-CLICK: ITS CATEGORY   ESCAPE CLOSES");
        else if (projects_.isOpen())
        {
            const std::string what = upper(shortcutText(   // Ctrl- on Windows
                projects_.searching()    ? "Projects   type to search: names, descriptions, tags   Return chooses the first   Escape clears"
                : projects_.categoryMode() ? "Projects   type a category   Return sets it   Escape goes back"
                : projects_.dragging()     ? "Projects   drop onto a group: its category (Favourites stars them)"
                                           : "Projects   Cmd-F searches   Cmd-click chooses more   right-click or drag: the category   "
                                             "Return saves a name typed, else opens   Escape closes"));
            std::snprintf(line, sizeof line, "%s", what.c_str());
        }
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
        // The muted input's notice at the status bar's right (an effect's, in the Standalone), before its button.
        float noticeW = 0.0f;
        if (mutedShown_)
        {
            const funkgui::Canvas::Scope s(c, kMutedTag, false);
            const funkgui::Rect b = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(mutedItem_)]).bounds();
            const float tx = b.x - 10.0f - c.textWidth(kMutedText, chat::kSmall);
            chat::drawIcon(c, chat::Icon::alert, tx - 11.0f, b.centreY(), 12.0f, pal.accent);
            c.text(kMutedText, tx, c.capCentreTop(b.centreY(), chat::kSmall), chat::kSmall, pal.soft);
            noticeW = b.right() - (tx - 18.0f) + 12.0f;
        }
        // The recorder's notice (REC, its seconds and marks), left of its Mark (or where Mark would be).
        float recordW = 0.0f;
        if (recordShown())
        {
            const funkgui::Canvas::Scope s(c, kRecordTag, false);
            const ButtonItem& mark = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(markItem_)]);
            const funkgui::Rect b = mark.bounds();
            const std::string text = recordText();
            const float right = mark.shown() ? b.x - 10.0f : b.right();
            const float tx = right - c.textWidth(text.c_str(), chat::kSmall);
            constexpr funkgui::Col kRecording{ 0xE5, 0x48, 0x4D, 255 };   // a recorder's red, in either theme
            const bool live = recording_.stage == RecordingState::Stage::recording;
            c.disc(tx - 9.0f, b.centreY(), 3.5f, live ? kRecording : pal.faint);
            c.text(text.c_str(), tx, c.capCentreTop(b.centreY(), chat::kSmall), chat::kSmall, pal.soft);
            recordW = b.right() - (tx - 16.0f) + 12.0f;
        }
        if (line[0] != '\0')
        {
            const ButtonItem& zoom = static_cast<const ButtonItem&>(*items_[static_cast<std::size_t>(zoomItem_)]);
            const float zoomW = zoom.shown() ? zoom.bounds().w + 12.0f : 0.0f;
            const std::string shown = fitFields(c, line, contentW - zoomW - noticeW - recordW - 8.0f);
            const funkgui::Canvas::Scope s(c, funkgui::tags::hint, false);
            c.text(shown.c_str(), kMargin + 8.0f, c.capCentreTop(height_ - 0.5f * kFooterH, funkgui::type::kMicro),
                   funkgui::type::kMicro, th.ink52);
        }

        if (dropActive_ && chatOpen_ && hasChat())
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
        if (name_.isOpen())
        {
            const funkgui::Canvas::Scope s(c, kNameTag, false);
            name_.draw(c, th, width_, height_, pointer_, hasPointer_);
        }
        if (ab_.isOpen())
        {
            const funkgui::Canvas::Scope s(c, kAbTag, false);
            ab_.draw(c, th, width_, height_, pointer_, hasPointer_);
        }
        if (settings_.isOpen())
        {
            const funkgui::Canvas::Scope s(c, kSettingsTag, false);
            settings_.draw(c, th, width_, height_, pointer_, hasPointer_);
        }
        if (picker_.isOpen())
        {
            const funkgui::Canvas::Scope s(c, kPresetsTag, false);
            picker_.draw(c, th, width_, height_, pointer_, hasPointer_);
        }
    }

    // ---- input --------------------------------------------------------------------------------------------------------
    int Panel::itemAt(funkgui::Point p) const
    {
        if (dialogOpen())
            return -1;                                   // the dialog covers the panel
        // A band over a card's edge, before the cards' items; but under the code editor's overlays, which the items'
        // order lets reach a point first, as before the edges were.
        if (editorItem_ < 0 || !(items_[static_cast<std::size_t>(editorItem_)]->shown() && files().overlayAt(p)))
            for (const int e : { explorerEdgeItem_, chatEdgeItem_ })
                if (e >= 0 && items_[static_cast<std::size_t>(e)]->shown() && items_[static_cast<std::size_t>(e)]->contains(p))
                    return e;
        for (std::size_t i = 0; i < items_.size(); ++i)
            if (items_[i]->shown() && items_[i]->contains(p))
                return static_cast<int>(i);
        return -1;
    }

    void Panel::pointerMove(const funkgui::PointerEvent& e)
    {
        noteInput();
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
        noteInput(true);                                 // FunkGui's EditorHost nudges the pump on no exit
        if (hasPointer_)
            if (const int before = itemAt(pointer_); before >= 0)
                items_[static_cast<std::size_t>(before)]->pointerExit();
        hasPointer_ = false;
        dirty_ = true;
    }

    int Panel::textItem() const
    {
        return focus_ >= 0 && items_[static_cast<std::size_t>(focus_)]->shown()
                       && items_[static_cast<std::size_t>(focus_)]->takesAllKeys() ? focus_ : -1;
    }

    void Panel::pointerDown(const funkgui::PointerEvent& e)
    {
        noteInput();
        dirty_ = true;                                   // input: drawn at the next frame
        ring_ = false;                                   // the pointer is the affordance now
        pointer_ = { e.x, e.y };
        hasPointer_ = true;
        windowPress_ = false;
        // A press that closed a card (a click outside the Settings card, a card's Done or Cancel) ends there: the
        // second press of its double-click finds the card gone and would act on what lies under it.
        if (closingPress_ && e.clicks > 1)
        {
            pressed_ = -1;
            return;
        }
        closingPress_ = false;
        projectsPress_ = false;
        if (dialogOpen())
        {
            pressed_ = -1;
            if (settings_.isOpen())
            {
                settings_.pointerDown(pointer_, width_, height_, host_);
                afterSettings(true);
            }
            else if (keyOpen_)
                keyDialogPointerDown(pointer_);
            else if (export_.isOpen())
            {
                export_.pointerDown(pointer_, width_, height_);
                afterExport(true);
            }
            else if (name_.isOpen())
            {
                name_.pointerDown(pointer_, width_, height_);
                afterName(true);
            }
            else if (ab_.isOpen())
            {
                ab_.pointerDown(pointer_, width_, height_);
                afterAb(true);
            }
            else if (picker_.isOpen())
            {
                (void)picker_.pointerDown(e, host_, width_, height_);   // outside it: closed, the press goes no further
                afterPicker(true);
            }
            else
            {
                projectsPress_ = true;
                projects_.pointerDown(e, host_, width_, height_);
                afterProjects(true);
            }
            closingPress_ = !dialogOpen();
            return;
        }
        // A .gui interface's bar (Scroll.h): its thumb drags, a press on its track jumps first.
        guiBarPress_ = !e.popup && !panelSpec_.gui.empty() && guiView_->pressBar(panelSpec_, pointer_);
        if (guiBarPress_)
        {
            pressed_ = -1;
            ++layoutRevision_;                           // the interface's accessibility items moved
            return;
        }
        pressed_ = itemAt(pointer_);
        if (pressed_ >= 0 && items_[static_cast<std::size_t>(pressed_)]->focusable() && !isEdge(pressed_))
            setFocus(pressed_);                          // an edge's drag leaves the focus (the caret) where it was
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
        // The Standalone's window has no title bar: a press on the top bar where nothing is drags it instead.
        windowPress_ = pressed_ < 0 && !e.popup && buttonsRoom_ > 0.0f && pointer_.y < kMainY;
        if (windowPress_)
            port_.dragWindow(e.clicks);
    }

    // ---- the Standalone window's buttons over the top bar (EditorPort::windowButtons) -------------------------------
    float Panel::buttonsRoomNow() const
    {
        if (role_ == Role::code)
            return 0.0f;                                 // the code window has a title bar of its own
        const WindowButtons b = port_.windowButtons();
        if (b.width <= 0.0f)
            return 0.0f;
        // The buttons keep their size in window points whatever the zoom: their logical width shrinks as it grows.
        const float zoom = host_ != nullptr ? static_cast<float>(std::max(host_->zoomPercent(), 25)) * 0.01f : 1.0f;
        return std::ceil(b.width / zoom) + kButtonsGap;
    }

    void Panel::syncWindowButtons()
    {
        if (buttonsRoomNow() != buttonsRoom_)           // the buttons came or went, or the zoom changed
            layout();
    }

    void Panel::pointerDrag(const funkgui::PointerEvent& e)
    {
        noteInput();
        dirty_ = true;                                   // input: drawn at the next frame
        pointer_ = { e.x, e.y };
        if (projectsPress_ && projects_.isOpen())
        {
            projects_.pointerDrag(pointer_, width_, height_);   // a project's row carried onto a group, or a thumb
            return;
        }
        // A card's bar's thumb (Scroll.h: each moves only while its own thumb was pressed).
        if (settings_.isOpen() || ab_.isOpen() || picker_.isOpen())
        {
            if (settings_.isOpen())
                settings_.pointerDrag(pointer_, width_, height_);
            else if (ab_.isOpen())
                ab_.pointerDrag(pointer_, width_, height_);
            else
                picker_.pointerDrag(pointer_, width_, height_);
            return;
        }
        if (guiBarPress_)
        {
            if (guiView_->dragBar(panelSpec_, pointer_))
                ++layoutRevision_;                       // the interface's accessibility items moved
            return;
        }
        if (pressed_ >= 0 && gestures_)
            items_[static_cast<std::size_t>(pressed_)]->pointerDrag(e, *gestures_);
        dirty_ = true;
    }

    void Panel::pointerUp(const funkgui::PointerEvent& e)
    {
        noteInput(true);                                 // FunkGui's EditorHost nudges the pump on no release
        dirty_ = true;                                   // input: drawn at the next frame
        if (std::exchange(projectsPress_, false) && projects_.isOpen())
        {
            projects_.pointerUp({ e.x, e.y }, width_, height_);
            afterProjects(true);
            pressed_ = -1;
            return;
        }
        settings_.pointerUp();                           // a card's thumb let go (none held: nothing)
        ab_.pointerUp();
        picker_.pointerUp();
        if (std::exchange(guiBarPress_, false))
        {
            guiView_->releaseBar();
            pressed_ = -1;
            return;
        }
        if (pressed_ >= 0 && gestures_)
            items_[static_cast<std::size_t>(pressed_)]->pointerUp(e, *gestures_);
        pressed_ = -1;
        dirty_ = true;
    }

    void Panel::doubleClick(const funkgui::PointerEvent& e)
    {
        noteInput();
        dirty_ = true;                                   // input: drawn at the next frame
        // The double-click's second press went to the window (dragWindow), which may have zoomed or minimised since:
        // its release, and so this, comes where the pointer now is over the panel, and is no click of the panel's.
        if (windowPress_ || closingPress_)
            return;
        if (keyOpen_ || settings_.isOpen())
            return;
        if (export_.isOpen() || ab_.isOpen() || name_.isOpen())
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
        noteInput();
        dirty_ = true;
        if (settings_.isOpen())
        {
            settings_.wheel(e, width_, height_);
            return true;
        }
        if (export_.isOpen() || name_.isOpen())
            return true;
        if (ab_.isOpen())
        {
            ab_.wheel(e, width_, height_);
            return true;
        }
        if (projects_.isOpen())
        {
            projects_.wheel(e, width_, height_);
            return true;
        }
        if (picker_.isOpen())
        {
            picker_.wheel(e, width_, height_);
            return true;
        }
        const int i = itemAt({ e.x, e.y });
        // A .gui interface taller or wider than its area scrolls, but for a control under the pointer (it takes the
        // wheel itself).
        const GuiLayout& gl = guiView_->places();
        if (!panelSpec_.gui.empty() && gl.area.contains({ e.x, e.y }) && (gl.maxScrollX > 0.0f || gl.maxScrollY > 0.0f)
            && !guiView_->isControl(i))
        {
            // As the code scrolls (Scroll.h): the deltas as they came, WheelEvent::reversed never read.
            const float k = GuiView::wheelPx(e.smooth);
            const float dy = e.dy, dx = e.dx;
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
        noteInput();
        dirty_ = true;
        if (settings_.isOpen())
        {
            // The card takes no Cmd key: they go on to the app's menu (Cmd-Q, Cmd-H, Cmd-M), as with JUCE's dialog.
            if (e.mods.cmd)
                return false;
            settings_.key(e, host_, width_, height_);
            afterSettings(true);
            return true;
        }
        if (keyOpen_)
            return keyDialogKey(e);
        if (export_.isOpen())
        {
            export_.key(e, host_, width_, height_);
            afterExport(true);
            return true;
        }
        if (name_.isOpen())
        {
            name_.key(e, host_, width_, height_);
            afterName(true);
            return true;
        }
        if (ab_.isOpen())
        {
            ab_.key(e, width_, height_);
            afterAb(true);
            return true;
        }
        if (projects_.isOpen())
        {
            projects_.key(e, host_, width_, height_);
            afterProjects(true);
            return true;
        }
        if (picker_.isOpen())
        {
            picker_.key(e, host_, width_, height_);
            afterPicker(true);
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
        // Cmd-J: the panel under the code shown or hidden (the status line's Panel switch), in the CODE view (the code
        // window has no panel under its code).
        if (e.key == funkgui::Key::character && e.mods.cmd && !e.mods.shift && !e.mods.alt && !e.mods.ctrl
            && (e.ch == 'j' || e.ch == 'J'))
        {
            if (full() || role_ == Role::code)
                return false;
            toggleCodePanel();
            return true;
        }
        // Cmd-Shift-M: a mark in the recording for Claude, while the capture tool records (anywhere in the panel).
        if (e.key == funkgui::Key::character && e.mods.cmd && e.mods.shift && !e.mods.alt && (e.ch == 'm' || e.ch == 'M'))
        {
            if (!recordShown())
                return false;
            markNow();
            return true;
        }
        // Cmd-Shift-D: a netlist's Netlist / Schematic view, in either view; nothing while the IR tab shows.
        if (e.key == funkgui::Key::character && e.mods.cmd && e.mods.shift && (e.ch == 'd' || e.ch == 'D'))
        {
            if (full())
                return false;
            if (files().irShown())
                return true;
            if (!files().circuitShown())
                return false;
            files().toggleSchematic();
            setFocus(editorItem_);
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
        // [ and ]: the project open's preset before and after, while no text field has the caret (above).
        if (e.key == funkgui::Key::character && !e.mods.cmd && !e.mods.ctrl && !e.mods.alt && (e.ch == '[' || e.ch == ']')
            && presetsShown())
        {
            stepPresets(e.ch == '[' ? -1 : 1);
            return true;
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
        if (focus_ < 0 || !gestures_ || !items_[static_cast<std::size_t>(focus_)]->shown())
            return false;                                // an item hidden since it took the focus takes no key
        return items_[static_cast<std::size_t>(focus_)]->key(e, *gestures_);
    }

    funkgui::Cursor Panel::cursor() const
    {
        if (settings_.isOpen())
            return settings_.cursor(pointer_, width_, height_);
        if (keyOpen_)
        {
            const KeyDialog d = keyDialog();
            const bool button = d.save.contains(pointer_) || d.cancel.contains(pointer_)
                             || (dialogHasKey() && !renaming_ && d.remove.contains(pointer_))
                             || (!renaming_ && d.provider.contains(pointer_));
            return button ? funkgui::Cursor::pointingHand : funkgui::Cursor::normal;
        }
        if (export_.isOpen())
            return export_.cursor(pointer_, width_, height_);
        if (name_.isOpen())
            return name_.cursor(pointer_, width_, height_);
        if (ab_.isOpen())
            return ab_.cursor(pointer_, width_, height_);
        if (projects_.isOpen())
            return projects_.cursor(pointer_, width_, height_);
        if (picker_.isOpen())
            return picker_.cursor(pointer_, width_, height_);
        const int i = pressed_ >= 0 ? pressed_ : (hasPointer_ ? itemAt(pointer_) : -1);
        return i >= 0 ? items_[static_cast<std::size_t>(i)]->cursorAt(pointer_) : funkgui::Cursor::normal;
    }

    // ---- accessibility ------------------------------------------------------------------------------------------------
    namespace
    {
        // The key dialog's ids, past every item's.
        constexpr uint32_t kKeyDialogId = 0x7FFF0000u;
        constexpr uint32_t kKeyFieldId = kKeyDialogId + 1, kKeySaveId = kKeyDialogId + 2,
                           kKeyCancelId = kKeyDialogId + 3, kKeyRemoveId = kKeyDialogId + 4,
                           kKeyProviderId = kKeyDialogId + 5, kKeyBaseId = kKeyDialogId + 6, kKeyModelId = kKeyDialogId + 7;
    }

    void Panel::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (settings_.isOpen())
        {
            settings_.accessibility(out, width_, height_);
            return;
        }
        if (keyOpen_)
        {
            // Never the key: its length only. A chat's new name as typed.
            const KeyDialog d = keyDialog();
            funkgui::A11yItem box;
            box.id = kKeyDialogId;
            box.role = funkgui::a11yrole::group;
            box.bounds = d.box;
            box.title = renaming_ ? "Rename this chat" : dialogAnthropic() ? "Anthropic API key" : std::string(dialogPresetRef().name);
            box.description = renaming_ ? std::string("Its name in the chat list, on ") + kThisMachine
                                        : std::string("Stored encrypted on ") + kThisMachine + ", for both playground plugins";
            out.push_back(box);
            const auto plainField = [&](uint32_t id, const funkgui::Rect& r, const char* title, const funkgui::text::LineEdit& e) {
                funkgui::A11yItem f;
                f.id = id;
                f.parent = kKeyDialogId;
                f.role = funkgui::a11yrole::editableText;
                f.bounds = r;
                f.title = title;
                f.value = e.buffer.empty() ? "empty" : asciiText(e.buffer);
                out.push_back(std::move(f));
            };
            if (!renaming_ && keyDialog().base.w > 0.0f)
                plainField(kKeyBaseId, keyDialog().base, "Server's address", baseEdit_);
            funkgui::A11yItem field;
            field.id = kKeyFieldId;
            field.parent = kKeyDialogId;
            field.role = funkgui::a11yrole::editableText;
            field.bounds = d.field;
            field.title = renaming_ ? "Name" : "Key";
            field.value = renaming_ ? asciiText(keyEdit_.buffer)
                        : keyEdit_.buffer.empty() ? "empty" : std::to_string(keyEdit_.buffer.size()) + " characters";
            field.help = !keyWhy_.empty() ? keyWhy_ : renaming_ ? "Return renames it" : "Command V pastes it";
            out.push_back(std::move(field));
            if (!renaming_ && keyDialog().model.w > 0.0f)
                plainField(kKeyModelId, keyDialog().model, "Model", modelEdit_);
            const auto button = [&](uint32_t id, const funkgui::Rect& r, const char* title) {
                funkgui::A11yItem b;
                b.id = id;
                b.parent = kKeyDialogId;
                b.role = funkgui::A11yRole::button;
                b.bounds = r;
                b.title = title;
                out.push_back(std::move(b));
            };
            button(kKeySaveId, d.save, renaming_ ? "Rename" : "Save");
            button(kKeyCancelId, d.cancel, "Cancel");
            if (dialogHasKey() && !renaming_)
                button(kKeyRemoveId, d.remove, "Remove the stored key");
            if (!renaming_)
            {
                // A dropdown: its value the provider shown.
                funkgui::A11yItem b;
                b.id = kKeyProviderId;
                b.parent = kKeyDialogId;
                b.role = funkgui::A11yRole::comboBox;
                b.bounds = d.provider;
                b.title = "Provider";
                b.value = dialogPresetRef().name;
                out.push_back(std::move(b));
            }
            return;
        }
        if (export_.isOpen())
        {
            export_.accessibility(out, width_, height_);
            return;
        }
        if (name_.isOpen())
        {
            name_.accessibility(out, width_, height_);
            return;
        }
        if (ab_.isOpen())
        {
            ab_.accessibility(out, width_, height_);
            return;
        }
        if (projects_.isOpen())
        {
            projects_.accessibility(out, width_, height_);
            return;
        }
        if (picker_.isOpen())
        {
            picker_.accessibility(out, width_, height_);
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
        uint32_t r = items_[static_cast<std::size_t>(zoomItem_)]->shown() ? 1u : 0u;
        uint64_t dials = 0;                                   // the dials shown, one bit each, mixed in below
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = itemOfSlot_[static_cast<std::size_t>(c)]; i >= 0 && items_[static_cast<std::size_t>(i)]->shown())
                dials |= slotBit(c);
        r ^= static_cast<uint32_t>(dials) * 0x27D4EB2Fu ^ static_cast<uint32_t>(dials >> 32) * 0x165667B1u;
        r |= (keyOpen_ ? 1u : 0u) << 17;
        r ^= keyOpen_ ? static_cast<uint32_t>(dialogPreset_ + 1) * 0x2545F491u ^ (dialogHasKey() ? 0x61C88647u : 0u) : 0u;
        r ^= (projects_.isOpen() ? 0x9E3779B1u : 0u) ^ (projects_.revision() * 0x165667B1u);
        r ^= (picker_.isOpen() ? 0x2545F491u : 0u) ^ (picker_.revision() * 0x61C88647u);
        r ^= (export_.isOpen() ? 0x85EBCA77u : 0u) ^ (export_.revision() * 0xC2B2AE3Du);
        r ^= (name_.isOpen() ? 0x4CF5AD43u : 0u) ^ (name_.revision() * 0x2127599Bu);
        r ^= (ab_.isOpen() ? 0x7FEB352Du : 0u) ^ (ab_.revision() * 0x846CA68Bu);
        r ^= (settings_.isOpen() ? 0x27D4EB2Du : 0u) ^ (settings_.revision() * 0x9E3779B1u) ^ (mutedShown_ ? 0x165667B1u : 0u);
        r |= (chat_.ready() ? 1u : 0u) << 18;
        static_assert(kMaxDisplays <= 32, "the displays' mask");
        uint32_t displays = 0;                                // the displays shown, one bit each, mixed in
        for (int d = 0; d < kMaxDisplays; ++d)
            if (items_[static_cast<std::size_t>(itemOfDisplay_[static_cast<std::size_t>(d)])]->shown())
                displays |= 1u << d;
        r ^= displays * 0x9E3779B9u;
        uint64_t gui = 0;                                     // a .gui interface's controls shown
        for (int c = 0; c < kMaxControls; ++c)
            if (const int i = guiView_->controlItem(c); i >= 0 && items_[static_cast<std::size_t>(i)]->shown())
                gui |= slotBit(c);
        r ^= static_cast<uint32_t>(gui) * 0x2C1B3C6Du ^ static_cast<uint32_t>(gui >> 32) * 0x9E3779B1u;
        // The counters times odd constants: a change in any one of them changes the revision.
        // A netlist's Schematic view: its parts and nets as they come and go (never a pan or a zoom).
        r ^= files().schematicShown() ? 0x7FEB352Du ^ (files().schematic().a11yRevision() * 0x846CA68Bu) : 0u;
        r ^= (dialsBar_ ? 0x5BD1E995u : 0u) ^ (dialsFolded_ ? 0x1B56C4E9u : 0u);
        return r ^ (files().layoutRevision() * 0x85EBCA77u) ^ (layoutRevision_ * 0xC2B2AE3Du) ^ (chatA11y_ * 2654435761u)
             ^ (explorer().revision() * 0x27220A95u) ^ (explorerShown_ ? 0x1B873593u : 0u)
             ^ (frozenShown_ ? 0x68E31DA4u ^ std::bit_cast<uint32_t>(centreOfFrozen().x) : 0u);   // it moves with the load's text
    }

    void Panel::a11yAction(uint32_t id, funkgui::A11yAction a, double value)
    {
        dirty_ = true;                                   // input: drawn at the next frame
        if (!gestures_)
            return;
        if (settings_.isOpen())
        {
            settings_.a11yAction(id, a, width_, height_, host_);
            afterSettings(true);
            return;
        }
        if (keyOpen_)
        {
            if (id == kKeySaveId)
                saveKeyFromDialog();
            else if (id == kKeyCancelId)
                closeKeyDialog();
            else if (id == kKeyRemoveId && dialogHasKey() && !renaming_)
                removeKeyFromDialog();
            else if (id == kKeyProviderId && !renaming_)
                openProviders();
            else if (id == kKeyBaseId && !renaming_)
                dialogField_ = 1;
            else if (id == kKeyModelId && !renaming_)
                dialogField_ = 2;
            else if (id == kKeyFieldId && !renaming_)
                dialogField_ = 0;
            dirty_ = true;
            return;
        }
        if (export_.isOpen())
        {
            export_.a11yAction(id, a, width_, height_);
            afterExport(true);
            return;
        }
        if (name_.isOpen())
        {
            name_.a11yAction(id, a, width_, height_);
            afterName(true);
            return;
        }
        if (ab_.isOpen())
        {
            ab_.a11yAction(id, a, width_, height_);
            afterAb(true);
            return;
        }
        if (projects_.isOpen())
        {
            projects_.a11yAction(id, a, host_, width_, height_);
            afterProjects(true);
            return;
        }
        if (picker_.isOpen())
        {
            (void)picker_.a11yAction(id, a, host_, width_, height_);
            afterPicker(true);
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
        dirty_ = true;                                   // input: drawn at the next frame
        if (gestures_)
            gestures_->closeAll();
        guiView_->releaseKeys();
    }
}
