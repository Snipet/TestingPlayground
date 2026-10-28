// Source/plugin/standalone/App.cpp: the Standalone app. CMakeLists.txt compiles Source/plugin/standalone/ into each
// product's Standalone target only, with JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP, so JUCE's own app (its
// StandaloneFilterApp, and a window with a drawn title bar and an Options button) is left out and this one starts
// instead. The AU and the VST3 are unchanged.
//
// What JUCE's app did stays: juce::StandalonePluginHolder runs the processor through the audio and MIDI devices, and
// keeps the plugin's state and the devices' settings in JUCE's own file (~/Library/Application Support/<Name>.settings,
// so what a user had carries over); the window's place is kept there too (windowX, windowY, as JUCE kept it); its size
// is the editor's, which the session keeps; the editor stays resizable within its limits; and an effect starts with its
// input muted (JUCE's guard against a feedback loop), a bar under the editor saying so.
//
// What is new: the window is macOS's own (NativeWindow.mm) without a title bar of its own: its buttons sit over the
// editor's top bar, which keeps them room (EditorPort::windowButtons, through Processor::setWindowHooks), and the top
// bar's empty parts drag it. JUCE's Options moved to the menu bar (StandaloneMenu.h): Settings... (Cmd-,) in the app
// menu, Load State..., Save State As... and Reset to Default State in File; an Edit menu reaches the focused text field
// (an item sends its shortcut to the focused component, as typing it would); Window minimises and zooms.
#include "plugin/Processor.h"
#include "plugin/StandaloneMenu.h"
#include "plugin/standalone/NativeWindow.h"

#include "Product.h"

#include <funkgui/gpu/EditorHost.h>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#include <functional>
#include <memory>
#include <string>

namespace playground::standalone
{
    namespace
    {
        // JUCE's own Standalone's settings file (StandaloneFilterApp's options): the plugin's state ("filterState"),
        // the devices ("audioSetup", "shouldMuteInput"), the last state file and the window's place.
        juce::PropertiesFile::Options settingsOptions()
        {
            juce::PropertiesFile::Options o;
            o.applicationName = juce::String::fromUTF8(product::kName);
            o.filenameSuffix = ".settings";
            o.osxLibrarySubFolder = "Application Support";
            o.folderName = "";
            return o;
        }

        // The editor's zoom (FunkGui's UI zoom: 1 at 100 %), by which its logical px become the window's points.
        float zoomOf(const juce::AudioProcessorEditor* editor)
        {
            if (const auto* host = dynamic_cast<const funkgui::EditorHost*>(editor))
                return static_cast<float>(host->diagnostics().zoomPercent) * 0.01f;
            return 1.0f;
        }

        // A shortcut as the focused component would get it typed: the key's lower-case code, no text, the modifiers
        // (EditorHost's UI_KEYS sends the same, and StandaloneMenu.h's keysOf writes it).
        juce::KeyPress keyPressOf(const Shortcut& s)
        {
            int mods = juce::ModifierKeys::commandModifier;
            if (s.shift)
                mods |= juce::ModifierKeys::shiftModifier;
            if (s.option)
                mods |= juce::ModifierKeys::altModifier;
            return juce::KeyPress(static_cast<int>(s.key), juce::ModifierKeys(mods), 0);
        }
    }

    // Under the editor while an effect's input is muted (StandalonePluginHolder::shouldMuteInput: JUCE's guard against
    // a feedback loop, until the user unmutes it in Settings).
    class MutedBar final : public juce::Component
    {
    public:
        static constexpr int kHeight = 30;

        explicit MutedBar(std::function<void()> settings) : settings_(std::move(settings))
        {
            setOpaque(true);
            text_.setText("Audio input is muted to avoid a feedback loop: unmute it in Settings, with headphones on.",
                          juce::dontSendNotification);
            text_.setColour(juce::Label::textColourId, juce::Colours::black);
            button_.onClick = [this] {
                if (settings_)
                    settings_();
            };
            addAndMakeVisible(text_);
            addAndMakeVisible(button_);
        }

        void paint(juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.setColour(juce::Colours::darkgoldenrod);
            g.fillRect(r.removeFromTop(1));
            g.setColour(juce::Colours::lightgoldenrodyellow);
            g.fillRect(r);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(5);
            button_.setBounds(r.removeFromRight(90));
            text_.setBounds(r);
        }

    private:
        std::function<void()> settings_;
        juce::Label text_;
        juce::TextButton button_{ juce::String::fromUTF8("Settings\xe2\x80\xa6") };
    };

    // The window's content: the editor at the top (its size the window's), and the muted bar under it.
    class Content final : public juce::Component, private juce::Value::Listener, private juce::ComponentListener
    {
    public:
        Content(juce::StandalonePluginHolder& holder, std::function<void()> settings, std::function<void()> editorMoved)
            : holder_(holder), bar_(std::move(settings)), editorMoved_(std::move(editorMoved))
        {
            juce::AudioProcessor& processor = *holder_.processor;
            editor_.reset(processor.hasEditor() ? processor.createEditorIfNeeded()
                                                : new juce::GenericAudioProcessorEditor(processor));
            addChildComponent(bar_);
            if (holder_.getProcessorHasPotentialFeedbackLoop())
            {
                muted_.referTo(holder_.getMuteInputValue());
                muted_.addListener(this);
                showBar_ = static_cast<bool>(muted_.getValue());
            }
            bar_.setVisible(showBar_);
            if (editor_ != nullptr)
            {
                editor_->addComponentListener(this);
                addAndMakeVisible(*editor_);
                fitEditor();
            }
        }

        ~Content() override
        {
            if (editor_ != nullptr)
            {
                editor_->removeComponentListener(this);
                holder_.processor->editorBeingDeleted(editor_.get());
                editor_.reset();
            }
        }

        juce::AudioProcessorEditor* editor() const noexcept { return editor_.get(); }
        int barHeight() const noexcept { return showBar_ ? MutedBar::kHeight : 0; }
        juce::ComponentBoundsConstrainer* editorConstrainer() const
        {
            return editor_ != nullptr ? editor_->getConstrainer() : nullptr;
        }

        void resized() override
        {
            auto r = getLocalBounds();
            if (showBar_)
                bar_.setBounds(r.removeFromBottom(MutedBar::kHeight));
            if (editor_ == nullptr)
                return;
            if (fitting_)
                editor_->setTopLeftPosition(0, 0);
            else
                editor_->setBoundsConstrained(r);
        }

    private:
        // The content follows the editor: its size plus the bar (the window follows the content).
        void fitEditor()
        {
            const juce::ScopedValueSetter<bool> scope(fitting_, true);
            setSize(editor_->getWidth(), editor_->getHeight() + barHeight());
        }

        void componentMovedOrResized(juce::Component&, bool, bool) override
        {
            fitEditor();
            if (editorMoved_)
                editorMoved_();                          // its zoom may have changed: the buttons follow
        }

        void valueChanged(juce::Value& value) override
        {
            showBar_ = static_cast<bool>(value.getValue());
            bar_.setVisible(showBar_);
            if (editor_ != nullptr)
                fitEditor();
        }

        juce::StandalonePluginHolder& holder_;
        MutedBar bar_;
        std::function<void()> editorMoved_;
        std::unique_ptr<juce::AudioProcessorEditor> editor_;
        juce::Value muted_;
        bool showBar_ = false;
        bool fitting_ = false;
    };

    // The window's limits are the editor's (its own constrainer, the Panel's least size at its zoom) with the bar and
    // any native frame around them, so a drag never sets a size the editor would then refuse (JUCE's standalone does
    // the same).
    class Constrainer final : public juce::BorderedComponentBoundsConstrainer
    {
    public:
        void setWindow(juce::DocumentWindow* window) { window_ = window; }
        void setContent(Content* content) { content_ = content; }

        juce::ComponentBoundsConstrainer* getWrappedConstrainer() const override
        {
            return content_ != nullptr ? content_->editorConstrainer() : nullptr;
        }

        juce::BorderSize<int> getAdditionalBorder() const override
        {
            juce::BorderSize<int> b;
            if (window_ != nullptr)
            {
                if (auto* peer = window_->getPeer())
                    if (const auto frame = peer->getFrameSizeIfPresent())
                        b = *frame;
                b = b.addedTo(window_->getContentComponentBorder());
            }
            return b.addedTo(juce::BorderSize<int>{ 0, 0, content_ != nullptr ? content_->barHeight() : 0, 0 });
        }

    private:
        juce::DocumentWindow* window_ = nullptr;
        Content* content_ = nullptr;
    };

    class Window final : public juce::DocumentWindow
    {
    public:
        explicit Window(juce::StandalonePluginHolder& holder)
            : juce::DocumentWindow(juce::String::fromUTF8(product::kName),
                                   juce::LookAndFeel::getDefaultLookAndFeel().findColour(
                                       juce::ResizableWindow::backgroundColourId),
                                   juce::DocumentWindow::allButtons),
              holder_(holder)
        {
            constrainer_.setWindow(this);
            setUsingNativeTitleBar(true);                // macOS's window (styled in parentHierarchyChanged)
            setConstrainer(&constrainer_);
            updateContent();
            restorePlace();
            if (auto* c = content(); c != nullptr && c->editor() != nullptr)
                setResizable(c->editor()->isResizable(), false);
        }

        ~Window() override
        {
            if (auto* props = holder_.settings.get())
            {
                props->setValue("windowX", getX());
                props->setValue("windowY", getY());
            }
            unhook();
            holder_.stopPlaying();
            constrainer_.setContent(nullptr);
            clearContentComponent();
        }

        void closeButtonPressed() override
        {
            holder_.savePluginState();
            juce::JUCEApplicationBase::quit();
        }

        // File > Reset to Default State: a new processor in its first state (JUCE's Options menu did the same).
        void resetToDefaultState()
        {
            unhook();
            holder_.stopPlaying();
            constrainer_.setContent(nullptr);
            clearContentComponent();
            holder_.deletePlugin();
            if (auto* props = holder_.settings.get())
                props->removeValue("filterState");
            holder_.createPlugin();
            updateContent();
            holder_.startPlaying();
        }

        // A new peer (JUCE makes one when the title bar or the resizing changes): macOS's window gets its look.
        void parentHierarchyChanged() override
        {
            juce::DocumentWindow::parentHierarchyChanged();
            auto* peer = getPeer();
            if (peer == nullptr || !isUsingNativeTitleBar())
                return;
            const juce::Component::SafePointer<Window> safe(this);
            buttons_ = native::styleWindow(peer->getNativeHandle(), [safe](bool fullScreen) {
                if (safe != nullptr)
                    safe->fullScreenChanged(fullScreen);
            });
            hook();
            placeButtons();
        }

    private:
        Content* content() const { return dynamic_cast<Content*>(getContentComponent()); }

        void updateContent()
        {
            auto* c = new Content(holder_, [this] { holder_.showAudioSettingsDialog(); }, [this] { placeButtons(); });
            constrainer_.setContent(c);
            setContentOwned(c, true);
            hook();
            placeButtons();                              // where the new editor asked for them, before it shows
        }

        // The processor's editor reaches this window through these (EditorPort::windowButtons, placeWindowButtons,
        // dragWindow). In full screen the system shows the title bar itself: no room is kept.
        void hook()
        {
            auto* processor = dynamic_cast<Processor*>(holder_.processor.get());
            if (processor == nullptr)
                return;
            Processor::WindowHooks h;
            if (!fullScreen_)
                h.buttons = { buttons_.width, buttons_.height };
            h.place = [this](float x, float middle) {
                place_ = { x, middle };
                placed_ = true;
                placeButtons();
            };
            h.drag = [this](int clicks) {
                if (auto* peer = getPeer())
                    native::dragWindow(peer->getNativeHandle(), clicks);
            };
            processor->setWindowHooks(std::move(h));
        }

        void unhook()
        {
            if (auto* processor = dynamic_cast<Processor*>(holder_.processor.get()))
                processor->setWindowHooks({});
        }

        void fullScreenChanged(bool on)
        {
            fullScreen_ = on;
            hook();
        }

        // Where the editor keeps the buttons' room (its logical px), in the window's points: at its zoom, from its
        // place.
        void placeButtons()
        {
            auto* peer = getPeer();
            auto* c = content();
            if (!placed_ || peer == nullptr || c == nullptr || c->editor() == nullptr)
                return;
            const auto* editor = c->editor();
            const float z = zoomOf(editor);
            const auto at = getLocalPoint(editor, juce::Point<float>(place_.x * z, place_.y * z));
            native::placeButtons(peer->getNativeHandle(), at.x, at.y);
        }

        // The place the window was left at, on a display that still shows it (else centred on the main display).
        void restorePlace()
        {
            const int w = getWidth(), h = getHeight();
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            juce::Rectangle<int> r{ w, h };
            if (const auto* main = displays.getPrimaryDisplay())
                r = r.withCentre(main->userArea.getCentre());
            if (auto* props = holder_.settings.get())
            {
                constexpr int kNone = -100;
                const int x = props->getIntValue("windowX", kNone), y = props->getIntValue("windowY", kNone);
                if (x != kNone && y != kNone)
                    if (const auto* d = displays.getDisplayForRect({ x, y, w, h }))
                    {
                        const auto area = d->userArea;
                        r = { juce::jlimit(area.getX(), juce::jmax(area.getX(), area.getRight() - w), x),
                              juce::jlimit(area.getY(), juce::jmax(area.getY(), area.getBottom() - h), y), w, h };
                    }
            }
            setBoundsConstrained(r);
        }

        juce::StandalonePluginHolder& holder_;
        Constrainer constrainer_;
        native::ButtonsSize buttons_;
        juce::Point<float> place_;
        bool placed_ = false;
        bool fullScreen_ = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Window)
    };

    class App final : public juce::JUCEApplication
    {
    public:
        App() { properties_.setStorageParameters(settingsOptions()); }

        const juce::String getApplicationName() override { return juce::String::fromUTF8(product::kName); }
        const juce::String getApplicationVersion() override { return product::kVersion; }
        bool moreThanOneInstanceAllowed() override { return true; }
        void anotherInstanceStarted(const juce::String&) override {}

        void initialise(const juce::String&) override
        {
            using Holder = juce::StandalonePluginHolder;
            holder_ = std::make_unique<Holder>(properties_.getUserSettings(), false, juce::String{}, nullptr,
                                               juce::Array<Holder::PluginInOuts>{}, false);
            native::installMenuBar(menuBar(product::kName), [this](Command c) { return run(c); });
            if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
                return;                                  // no display: it plays without a window, as JUCE's did
            window_ = std::make_unique<Window>(*holder_);
            window_->setVisible(true);
        }

        void shutdown() override
        {
            window_ = nullptr;
            holder_ = nullptr;
            properties_.saveIfNeeded();
        }

        void systemRequestedQuit() override
        {
            if (holder_ != nullptr)
                holder_->savePluginState();
            if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
            {
                juce::Timer::callAfterDelay(100, [] {
                    if (auto* app = juce::JUCEApplicationBase::getInstance())
                        app->systemRequestedQuit();
                });
            }
            else
                quit();
        }

    private:
        // A menu item's command (StandaloneMenu.h; AppKit runs its own: Hide, Quit, Minimize, ...). False for an Edit
        // command while no JUCE window is key: AppKit's own action goes to that window instead.
        bool run(Command c)
        {
            if (isEdit(c))
            {
                // To the focused component of the key window, as if typed there (the editor's text fields take it).
                const juce::KeyPress key = keyPressOf(shortcutOf(c));
                for (int i = juce::ComponentPeer::getNumPeers(); --i >= 0;)
                    if (auto* peer = juce::ComponentPeer::getPeer(i); peer != nullptr && peer->isFocused())
                    {
                        peer->handleKeyPress(key);
                        return true;
                    }
                return false;
            }
            if (c == Command::about)
            {
                const std::string credits = std::string(product::kIsInstrument ? "A synth" : "An effect")
                                          + " whose DSP you write in FMinus while it plays.\nJUCE "
                                          + product::kJuceVersion + ", FunkGui " + product::kFunkGuiVersion + ".";
                native::showAbout(product::kName, product::kVersion, credits.c_str());
                return true;
            }
            if (holder_ == nullptr || juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0)
                return true;                             // a dialog or a chooser is open: it goes first
            switch (c)
            {
                case Command::settings:   holder_->showAudioSettingsDialog(); break;
                case Command::loadState:  holder_->askUserToLoadState(); break;
                case Command::saveState:  holder_->askUserToSaveState(); break;
                case Command::resetState:
                    if (window_ != nullptr)
                        window_->resetToDefaultState();
                    break;
                case Command::separator:
                case Command::about:
                case Command::services:
                case Command::hide:
                case Command::hideOthers:
                case Command::showAll:
                case Command::quit:
                case Command::undo:
                case Command::redo:
                case Command::cut:
                case Command::copy:
                case Command::paste:
                case Command::selectAll:
                case Command::minimize:
                case Command::zoom:
                case Command::bringAllToFront:
                    break;
            }
            return true;
        }

        juce::ApplicationProperties properties_;
        std::unique_ptr<juce::StandalonePluginHolder> holder_;
        std::unique_ptr<Window> window_;
    };
}

START_JUCE_APPLICATION(playground::standalone::App)
