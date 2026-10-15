#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "gui/Knob.h"
#include "visage_app/application_window.h"
#include "visage_ui/frame.h"

class TestingPlaygroundSynthEditor : public juce::AudioProcessorEditor
{
public:
    explicit TestingPlaygroundSynthEditor(TestingPlaygroundSynthProcessor&);
    ~TestingPlaygroundSynthEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    TestingPlaygroundSynthProcessor& proc;

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
        explicit VisageHost(TestingPlaygroundSynthProcessor& p);
        ~VisageHost() override;

        void visibilityChanged() override;
        void parentHierarchyChanged() override;
        void resized() override;

    private:
        void initialiseIfReady();
        void layoutChildren();
        // Shared knob geometry so layout and label drawing stay in sync.
        void knobGeometry(int index, int& x, int& y, int& size) const;

        TestingPlaygroundSynthProcessor& proc;
        std::unique_ptr<visage::ApplicationWindow> app_;
        std::unique_ptr<visage::Frame> rootFrame_;
        std::unique_ptr<Knob<juce::AudioParameterFloat>> attackKnob_;
        std::unique_ptr<Knob<juce::AudioParameterFloat>> releaseKnob_;
        std::unique_ptr<Knob<juce::AudioParameterFloat>> gainKnob_;
    };

    std::unique_ptr<VisageHost> visageHost_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestingPlaygroundSynthEditor)
};
