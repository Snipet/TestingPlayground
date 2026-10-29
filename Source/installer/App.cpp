// Source/installer/App.cpp: "<Project> Installer", the app (cmake/Installer.cmake builds it; Installer.h, generated,
// names it). One window, macOS's own (plugin/standalone/NativeWindow.mm, the Standalone's: the title bar transparent
// over the content, the buttons over the Panel's sidebar where it keeps them room, a press on nothing drags it), whose
// content is a funkgui::EditorHost running the installer's Panel on the GPU: nothing in it is drawn by JUCE. The
// theme follows the system's appearance (GRAPHITE when dark, PAPER when light) and is kept in memory only: the
// installer writes no preferences. The payload is this app's Contents/Resources/payload/ (Engine.h).
//
// The Panel reaches the system only through the Engine (MacSystem) and AppPort below: `open` for an app it installed,
// `open -a Claude` for an extension, each only when its button is clicked. Quitting during a run waits for it: Stop
// first (between steps), or the administrator's script to end.
#include "installer/Engine.h"
#include "installer/Panel.h"
#include "plugin/StandaloneMenu.h"
#include "plugin/standalone/NativeWindow.h"

#include "Installer.h"

#include <funkgui/gpu/EditorHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string>
#include <vector>

namespace playground::installer
{
    namespace
    {
        namespace native = standalone::native;
        using standalone::Command;

        // An AudioProcessor that processes nothing: EditorHost is an AudioProcessorEditor, which belongs to one.
        class NoProcessor final : public juce::AudioProcessor
        {
        public:
            NoProcessor() : juce::AudioProcessor(BusesProperties()) {}
            const juce::String getName() const override { return juce::String::fromUTF8(info::kName); }
            void prepareToPlay(double, int) override {}
            void releaseResources() override {}
            void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
            double getTailLengthSeconds() const override { return 0.0; }
            bool acceptsMidi() const override { return false; }
            bool producesMidi() const override { return false; }
            juce::AudioProcessorEditor* createEditor() override { return nullptr; }
            bool hasEditor() const override { return false; }
            int getNumPrograms() override { return 1; }
            int getCurrentProgram() override { return 0; }
            void setCurrentProgram(int) override {}
            const juce::String getProgramName(int) override { return {}; }
            void changeProgramName(int, const juce::String&) override {}
            void getStateInformation(juce::MemoryBlock&) override {}
            void setStateInformation(const void*, int) override {}
        };

        // The window's own colour behind the content, the theme's ground (seen only while it opens).
        juce::Colour groundOf(int theme)
        {
            const funkgui::Col g = funkgui::Theme::byIndex(theme).ground;
            return juce::Colour(g.r, g.g, g.b);
        }

        fs::path payloadFolder()
        {
            const juce::File app = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
            return fs::path(app.getChildFile("Contents").getChildFile("Resources").getChildFile("payload")
                                .getFullPathName().toStdString());
        }

        std::vector<standalone::Menu> menus(const std::string& name)
        {
            const standalone::MenuItem line{};
            return {
                { name,
                  { { Command::about, "About " + name, {} },
                    line,
                    { Command::services, "Services", {} },
                    line,
                    { Command::hide, "Hide " + name, { 'h' } },
                    { Command::hideOthers, "Hide Others", { 'h', false, true } },
                    { Command::showAll, "Show All", {} },
                    line,
                    { Command::quit, "Quit " + name, { 'q' } } } },
                { "Window",
                  { { Command::minimize, "Minimize", { 'm' } },
                    line,
                    { Command::bringAllToFront, "Bring All to Front", {} } } },
            };
        }
    }

    class Window;

    // What the Panel asks of the app.
    class Port final : public AppPort
    {
    public:
        void setWindow(Window* w) { window_ = w; }

        void quit() override;
        bool openApp(const fs::path& app) override
        {
            return runProgram({ "/usr/bin/open", app.string() }, nullptr, nullptr) == 0;
        }
        fs::path claudeDesktop() const override
        {
            const fs::path home = Places::thisMac().home;
            for (const fs::path& p : { fs::path("/Applications/Claude.app"), home / "Applications" / "Claude.app" })
            {
                std::error_code ec;
                if (fs::is_directory(p, ec))
                    return p;
            }
            return {};
        }
        bool addToClaude(const fs::path& mcpb) override
        {
            return runProgram({ "/usr/bin/open", "-a", "Claude", mcpb.string() }, nullptr, nullptr) == 0;
        }
        Buttons windowButtons() const override;
        void placeWindowButtons(float x, float middle) override;
        void dragWindow(int clicks) override;
        bool returnHeld() const override
        {
            // JUCE's window keeps the keys that are down (their key-down and key-up events, cleared when it loses the
            // focus): a held Return's repeats come while it is down.
            return juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::returnKey);
        }

    private:
        Window* window_ = nullptr;
    };

    class Window final : public juce::DocumentWindow
    {
    public:
        Window(juce::AudioProcessor& processor, Engine& engine, Port& port)
            : juce::DocumentWindow(juce::String::fromUTF8(info::kName), groundOf(funkgui::UiPreferences::get().theme()),
                                   juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
              port_(port)
        {
            setUsingNativeTitleBar(true);
            auto panel = std::make_unique<Panel>(engine, port, info::kName);
            panel_ = panel.get();
            funkgui::EditorConfig config;                // the Panel's own size; no zoom, nothing saved
            editor_ = std::make_unique<funkgui::EditorHost>(processor, std::move(config), std::move(panel));
            setContentNonOwned(editor_.get(), true);
            setResizable(false, false);
            centreWithSize(getWidth(), getHeight());
        }

        ~Window() override
        {
            clearContentComponent();
            editor_ = nullptr;
        }

        Panel& panel() { return *panel_; }

        void closeButtonPressed() override
        {
            port_.quit();
        }

        // A new peer: macOS's window gets its look, and the buttons their place.
        void parentHierarchyChanged() override
        {
            juce::DocumentWindow::parentHierarchyChanged();
            auto* peer = getPeer();
            if (peer == nullptr || !isUsingNativeTitleBar())
                return;
            buttons_ = native::styleWindow(peer->getNativeHandle(), [](bool) {});
            place();
        }

        native::ButtonsSize buttons() const { return buttons_; }

        void placeButtons(float x, float middle)
        {
            at_ = { x, middle };
            placed_ = true;
            place();
        }

        void drag(int clicks)
        {
            if (auto* peer = getPeer())
                native::dragWindow(peer->getNativeHandle(), clicks);
        }

    private:
        void place()
        {
            auto* peer = getPeer();
            if (!placed_ || peer == nullptr || editor_ == nullptr)
                return;
            const auto p = getLocalPoint(editor_.get(), at_);
            native::placeButtons(peer->getNativeHandle(), p.x, p.y);
        }

        Port& port_;
        Panel* panel_ = nullptr;
        std::unique_ptr<funkgui::EditorHost> editor_;
        native::ButtonsSize buttons_;
        juce::Point<float> at_;
        bool placed_ = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Window)
    };

    AppPort::Buttons Port::windowButtons() const
    {
        if (window_ == nullptr)
            return {};
        const native::ButtonsSize b = window_->buttons();
        return { b.width, b.height };
    }

    void Port::placeWindowButtons(float x, float middle)
    {
        if (window_ != nullptr)
            window_->placeButtons(x, middle);
    }

    void Port::dragWindow(int clicks)
    {
        if (window_ != nullptr)
            window_->drag(clicks);
    }

    class App final : public juce::JUCEApplication, private juce::DarkModeSettingListener, private juce::Timer
    {
    public:
        const juce::String getApplicationName() override { return juce::String::fromUTF8(info::kName); }
        const juce::String getApplicationVersion() override { return info::kVersion; }
        bool moreThanOneInstanceAllowed() override { return false; }
        void anotherInstanceStarted(const juce::String&) override {}

        void initialise(const juce::String&) override
        {
            // The theme in memory, from the system's appearance: no preferences file is read or written.
            funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
            followAppearance();
            juce::Desktop::getInstance().addDarkModeSettingListener(this);

            // JUCE's own menu bar first, then ours over it (App.cpp of the Standalone says why).
            juce::MenuBarModel::setMacMainMenu(nullptr);
            native::installMenuBar(menus(info::kName), [this](Command c) { return run(c); });

            engine_ = std::make_unique<Engine>(Payload::read(payloadFolder()), Places::thisMac(info::kName), system_);
            window_ = std::make_unique<Window>(processor_, *engine_, port_);
            port_.setWindow(window_.get());
            window_->setVisible(true);
        }

        void shutdown() override
        {
            stopTimer();
            juce::Desktop::getInstance().removeDarkModeSettingListener(this);
            port_.setWindow(nullptr);
            window_ = nullptr;                           // the Panel joins its run's thread
            engine_ = nullptr;
        }

        void systemRequestedQuit() override { requestQuit(); }

        // Quit, once no run is going: a run asked to stop first (between steps), the administrator's script let end.
        // working(), not busy(): a minimised window's host does not tick the Panel, so a run that ended there is never
        // taken; the worker's own state says it is over.
        void requestQuit()
        {
            if (window_ != nullptr && window_->panel().working())
            {
                window_->panel().press(Panel::Act::stop);
                startTimer(200);
                return;
            }
            quit();
        }

    private:
        void timerCallback() override
        {
            if (window_ == nullptr || !window_->panel().working())
            {
                stopTimer();
                quit();
            }
        }

        void darkModeSettingChanged() override { followAppearance(); }

        void followAppearance()
        {
            funkgui::UiPreferences::get().setTheme(juce::Desktop::getInstance().isDarkModeActive() ? 0 : 1);
        }

        bool run(Command c)
        {
            if (c == Command::about)
            {
                const std::string credits = std::string("Installs ") + info::kProject + "'s plugins, apps and Claude "
                                            "Desktop extensions.\nJUCE " + info::kJuceVersion + ", FunkGui "
                                            + info::kFunkGuiVersion + ".";
                native::showAbout(info::kName, info::kVersion, credits.c_str());
            }
            return true;
        }

        NoProcessor processor_;
        MacSystem system_;
        Port port_;
        std::unique_ptr<Engine> engine_;
        std::unique_ptr<Window> window_;
    };

    void Port::quit()
    {
        if (auto* app = dynamic_cast<App*>(juce::JUCEApplication::getInstance()))
            app->requestQuit();
    }
}

START_JUCE_APPLICATION(playground::installer::App)
