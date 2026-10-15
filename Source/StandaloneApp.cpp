// Custom JUCE Standalone host. Replaces JUCE's default StandaloneFilterApp /
// StandaloneFilterWindow so we get:
//   - the macOS native title bar (close/minimise/zoom), not JUCE's drawn one
//   - no "Options" gear button in the title bar
//   - no "Audio input is muted to avoid feedback loop" notification banner
//   - the content area sized exactly to the editor, with no leftover space
//
// Activated by JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 in CMakeLists.txt —
// that flag suppresses JUCE's built-in StandaloneFilterApp definition and
// makes JUCE's main() call our juce_CreateApplication() instead.

#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_Standalone

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace
{
    class CustomStandaloneWindow : public juce::DocumentWindow
    {
    public:
        CustomStandaloneWindow(const juce::String& title,
                               std::unique_ptr<juce::StandalonePluginHolder> holderIn)
            : DocumentWindow(title,
                             juce::LookAndFeel::getDefaultLookAndFeel()
                                 .findColour(juce::ResizableWindow::backgroundColourId),
                             DocumentWindow::allButtons),
              pluginHolder(std::move(holderIn))
        {
            setUsingNativeTitleBar(true);

            // Feedback-loop mute defaults to true for stereo-in/stereo-out
            // effects. We're not displaying JUCE's notification banner, but
            // we still want the audio path to actually pass through.
            pluginHolder->getMuteInputValue().setValue(false);

            auto* processor = pluginHolder->processor.get();
            editor.reset(processor->createEditorIfNeeded());

            if (editor != nullptr)
            {
                setContentNonOwned(editor.get(), true);
                setResizable(editor->isResizable(), false);
            }

            centreWithSize(getWidth(), getHeight());
        }

        ~CustomStandaloneWindow() override
        {
            clearContentComponent();
            if (editor != nullptr && pluginHolder && pluginHolder->processor)
                pluginHolder->processor->editorBeingDeleted(editor.get());
            editor.reset();
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplicationBase::quit();
        }

        std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;
        std::unique_ptr<juce::AudioProcessorEditor> editor;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomStandaloneWindow)
    };

    class TestingPlaygroundStandaloneApp final : public juce::JUCEApplication
    {
    public:
        TestingPlaygroundStandaloneApp()
        {
            juce::PropertiesFile::Options options;
            options.applicationName     = juce::CharPointer_UTF8(JucePlugin_Name);
            options.filenameSuffix      = ".settings";
            options.osxLibrarySubFolder = "Application Support";
            appProperties.setStorageParameters(options);
        }

        const juce::String getApplicationName() override    { return juce::CharPointer_UTF8(JucePlugin_Name); }
        const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
        bool moreThanOneInstanceAllowed() override          { return true; }
        void anotherInstanceStarted(const juce::String&) override {}

        void initialise(const juce::String&) override
        {
            if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
                return;

            auto holder = std::make_unique<juce::StandalonePluginHolder>(
                appProperties.getUserSettings(),
                false /* takeOwnershipOfSettings */);

            mainWindow = std::make_unique<CustomStandaloneWindow>(getApplicationName(),
                                                                   std::move(holder));
            mainWindow->setVisible(true);
        }

        void shutdown() override
        {
            mainWindow.reset();
            appProperties.saveIfNeeded();
        }

        void systemRequestedQuit() override
        {
            if (mainWindow && mainWindow->pluginHolder)
                mainWindow->pluginHolder->savePluginState();
            quit();
        }

    private:
        juce::ApplicationProperties appProperties;
        std::unique_ptr<CustomStandaloneWindow> mainWindow;
    };
}

juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new TestingPlaygroundStandaloneApp();
}

#endif
