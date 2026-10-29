// Source/installer/Panel.h: the installer's window, a FunkGui Panel drawn in the editor's look (editor/ChatStyle.h's
// rounded cards, pills and switches; GRAPHITE or PAPER, as the system's appearance is), at a fixed size. App.cpp hosts
// it in an EditorHost in a native window whose buttons sit over its sidebar; the probes draw it headless.
//
//   sidebar    the project's name, and the pages as steps: Welcome, Choose, Install (or Uninstall), Done
//   Welcome    both products' marks (editor/LogoMark.h, each in its kind's accent), a line on what they are, each
//              product with the version this installer holds; Continue
//   Choose     each product's four parts as switches, what is installed now beside each (its version, and what
//              installing does: install, upgrade, replace a newer one, already installed, or why it cannot), the scope
//              ("Just for me", "All users": macOS asks for an administrator's password), notes on running apps and
//              hosts; Uninstall..., Back, Install (the default)
//   Installing each step with its state and a progress bar; Stop (after the step running)
//   Done       what went where, per product: "Open <app>" for an app installed, "Add to Claude Desktop" for an extension
//              (open -a Claude <file>, only when clicked, and only when Claude Desktop is installed; else how), a note to
//              rescan plugins in the DAW; an error in plain words, with what was and was not changed; Done (quits)
//   Uninstall  every copy found, each a switch (on), where it is and its version; Back, Move to Trash (the default)
//
// Keys: Return presses the page's default button, Escape goes back (Choose, Uninstall) or stops (Installing), Tab and
// Shift-Tab (or Down and Up) move the focus over the page's controls, Space presses the focused one, Left and Right
// choose the scope while it has the focus. Every control has an accessibility title. Runs happen on a worker thread
// of the Panel's; tick() takes their progress, and the Done page their report.
#pragma once

#include "installer/Engine.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace playground::installer
{
    // What the app does for the Panel (App.cpp); a probe's fake records it.
    class AppPort
    {
    public:
        virtual ~AppPort() = default;
        virtual void quit() = 0;
        virtual bool openApp(const fs::path& app) = 0;              // `open <app>`
        virtual fs::path claudeDesktop() const = 0;                 // Claude Desktop's app; empty when not installed
        virtual bool addToClaude(const fs::path& mcpb) = 0;         // `open -a Claude <mcpb>`

        // The native window's close, minimise and zoom buttons over the sidebar (0 x 0: none, as headless), where the
        // Panel keeps their room (logical px: their left edge and middle), and a press where nothing is (drags it).
        struct Buttons
        {
            float width = 0.0f, height = 0.0f;
        };
        virtual Buttons windowButtons() const { return {}; }
        virtual void placeWindowButtons(float x, float middle) { (void) x; (void) middle; }
        virtual void dragWindow(int clicks) { (void) clicks; }
    };

    class Panel final : public funkgui::Panel
    {
    public:
        static constexpr int kWidth = 780, kHeight = 540;
        static constexpr float kButtonsX = 20.0f, kButtonsMiddle = 22.0f;   // the window's buttons' place

        enum class Page : uint8_t { welcome, choose, installing, done, uninstall };

        // What a control does.
        enum class Act : uint8_t
        {
            next, back, install, toUninstall, part, scope, openApp, addToClaude, finish, copy, trash, stop
        };

        struct Control
        {
            enum class Kind : uint8_t { primary, secondary, small, toggle, radio };
            Act act = Act::next;
            int index = 0;                       // a part's (product * 4 + part), a scope's, a copy's, a product's
            Kind kind = Kind::secondary;
            funkgui::Rect r;
            std::string label;                   // what it shows
            std::string title;                   // its accessibility title
            std::string help;                    // its accessibility help (a switch: what installing it does)
            bool on = false;
            bool enabled = true;
            uint32_t id = 0;
        };

        // A text the page shows (drawn from r.x, centred on r's middle, fitted to r.w; `wrap`: lines of r.w).
        struct Line
        {
            enum class Kind : uint8_t { hero, title, subtitle, strong, body, label, value, note, faint, error, warn };
            Kind kind = Kind::body;
            std::string text;
            funkgui::Rect r;
            bool right = false;                  // right-aligned at r.right()
        };

        Panel(Engine& engine, AppPort& app, std::string installerName);
        ~Panel() override;

        Panel(const Panel&) = delete;
        Panel& operator=(const Panel&) = delete;

        // funkgui::Panel
        void attach(funkgui::HostServices& host) override;
        int width() const override { return kWidth; }
        int height() const override { return kHeight; }
        void tick(float dt) override;
        void idle(double nowSec) override;
        void draw(funkgui::Canvas& c, const funkgui::Theme& theme) override;
        bool wantsFullRate() const override;
        void pointerMove(const funkgui::PointerEvent& e) override;
        void pointerExit() override;
        void pointerDown(const funkgui::PointerEvent& e) override;
        bool key(const funkgui::KeyEvent& e) override;
        funkgui::Cursor cursor() const override;
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        uint32_t a11yRevision() const override { return revision_; }
        void a11yAction(uint32_t id, funkgui::A11yAction action, double value) override;
        void closeGestures() override {}

        // For the app and the probes.
        Page page() const noexcept { return page_; }
        void show(Page page);                    // Welcome, Choose or Uninstall (refreshed); the others follow runs
        std::vector<Control> controls() const;   // the page's, placed, in the Tab order
        std::vector<Line> lines() const;         // the page's texts, placed
        const Plan& choices() const noexcept { return choices_; }   // every part the payload offers, planned
        Scope scope() const noexcept { return scope_; }
        bool chosen(const std::string& key, Part part) const;
        void choose(const std::string& key, Part part, bool on);
        void setScope(Scope scope);
        bool busy() const;                       // a run is going
        void waitForRun();                       // blocks until the run's thread is over (the next tick takes it)
        const Report& report() const noexcept { return report_; }
        bool press(Act act, int index = 0);      // as a click on that control (false: none such, or disabled)
        std::string a11yDump() const;            // the accessibility items, one a11yDumpLine each

    private:
        struct Layout
        {
            std::vector<Control> controls;
            std::vector<Line> lines;
            struct Mark
            {
                funkgui::Rect r;                 // the tile
                bool instrument = false;
                float scale = 1.0f;
            };
            std::vector<Mark> marks;
            struct Glyph
            {
                funkgui::Point c;
                State state = State::waiting;
            };
            std::vector<Glyph> glyphs;
            std::vector<funkgui::Rect> cards;
            std::vector<funkgui::Rect> rules;    // hairlines
            funkgui::Rect progress{};
            float fraction = -1.0f;              // the progress bar's (< 0: none)
        };

        Layout layout() const;
        void layoutWelcome(Layout& L) const;
        void layoutChoose(Layout& L) const;
        void layoutRun(Layout& L) const;
        void layoutDone(Layout& L) const;
        void layoutUninstall(Layout& L) const;
        void footer(Layout& L, std::vector<Control> left, std::vector<Control> right) const;

        void act(const Control& c);
        void refresh();                          // the plan of every part, and the copies found
        void startInstall();
        void startUninstall();
        void finishRun();
        int focusIndex(const std::vector<Control>& cs) const;
        const Control* defaultButton(const std::vector<Control>& cs) const;
        void changed() { ++revision_; }
        std::string kindWords(const Product& p) const;
        std::string statusOf(const Step& s) const;

        Engine& engine_;
        AppPort& app_;
        std::string name_;                       // "<Project> Installer"
        funkgui::HostServices* host_ = nullptr;

        Page page_ = Page::welcome;
        Scope scope_ = Scope::user;
        Plan choices_;                           // every part offered, in this scope
        std::map<std::string, bool> on_;         // "<key>/<part>": the switch, once the user (or a default) set it
        std::vector<Copy> copies_;               // what is installed now (Uninstall)
        std::vector<bool> copyOn_;
        std::set<std::string> copyOff_;          // the copies switched off, by path (kept across a refresh)
        double refreshedAt_ = -1.0;
        double now_ = 0.0;

        // The run (install or uninstall) on its thread.
        bool uninstalling_ = false;              // the last run's kind
        Plan runPlan_;                           // what Install ran
        std::vector<Copy> runCopies_;            // what Uninstall ran
        std::thread worker_;
        mutable std::mutex runMutex_;
        std::vector<State> runStates_;
        std::vector<std::string> runLines_;
        bool runFinished_ = false;
        bool running_ = false;
        Report runReport_;
        Report report_;
        std::atomic<bool> cancel_{ false };
        uint32_t runSeen_ = 0, runSerial_ = 0;   // progress taken by tick / given by the worker
        float spin_ = 0.0f;                      // the working step's glyph turns

        funkgui::Point pointer_{};
        bool hasPointer_ = false;
        struct Focus
        {
            Act act = Act::next;
            int index = -1;                      // -1: none
        } focus_;
        bool ring_ = false;                      // the focus shown (the keys moved it)
        uint32_t revision_ = 1;
        bool buttonsPlaced_ = false;
    };
}
