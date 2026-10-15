#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "gui/GraphEditor.h"
#include "visage_app/application_window.h"
#include "visage_ui/frame.h"

class TestingPlaygroundEditor : public juce::AudioProcessorEditor
{
public:
    explicit TestingPlaygroundEditor(TestingPlaygroundProcessor&);
    ~TestingPlaygroundEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    TestingPlaygroundProcessor& proc;

    // Hosts a visage::ApplicationWindow inside the JUCE component tree.
    // visage needs a native window handle, so we defer construction until
    // the JUCE peer exists (visibilityChanged / parentHierarchyChanged).
    //
    // Drawing lives on a child Frame, not on the ApplicationWindow's own
    // onDraw — embedding the ApplicationWindow into a native window via
    // show(w, h, nativeHandle) only initialises its render pipeline once a
    // child Frame is present.
    class VisageHost : public juce::Component
    {
    public:
        explicit VisageHost(TestingPlaygroundProcessor& p);
        ~VisageHost() override;

        void visibilityChanged() override;
        void parentHierarchyChanged() override;
        void resized() override;

    private:
        void initialiseIfReady();

        TestingPlaygroundProcessor& proc;
        std::unique_ptr<visage::ApplicationWindow> app_;
        std::unique_ptr<visage::Frame> rootFrame_;
        std::unique_ptr<graph::GraphEditor> graphEditor_;
        std::unique_ptr<juce::FileChooser> chooser_;  // kept alive across async dialogs
    };

    std::unique_ptr<VisageHost> visageHost_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestingPlaygroundEditor)
};
