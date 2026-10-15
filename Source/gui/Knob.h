#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "visage_ui/frame.h"
#include "visage_widgets/shader_quad.h"

// Custom-shader rotary knob.
//
// - Background is rendered by fs_knob_background2.sc; the shader reads
//   the current value, hover-ring thickness, and a "squareness" flag out
//   of the RGB channels of the canvas color.
// - The current parameter value is drawn as text in the centre using the
//   embedded Roboto Condensed font.
// - Drag vertically to change. Ctrl + drag = fine adjust.
//
// Templated on the JUCE parameter type so the same widget works for
// AudioParameterFloat and AudioParameterInt. Explicit instantiations live
// at the bottom of Knob.cpp.
template <typename T>
class Knob : public visage::Frame
{
public:
    Knob(juce::AudioProcessorValueTreeState& state,
         const juce::String& paramID,
         bool square = false);
    ~Knob() override;

    void draw(visage::Canvas& canvas) override;
    void resized() override;
    void mouseDrag(const visage::MouseEvent& e) override;
    void mouseDown(const visage::MouseEvent& e) override;
    void mouseEnter(const visage::MouseEvent& e) override;
    void mouseExit(const visage::MouseEvent& e) override;

private:
    void startAnimation(bool grow);
    float updateAnimation();

    visage::Shader background_shader;
    juce::AudioProcessorValueTreeState& apvts;
    juce::String paramID;
    T* param = nullptr;

    float norm_value = 0.0f;     // [0, 1]
    float lasty = -1.0f;
    float ring_thickness = 0.0f; // hover ring, [0, 1]
    long long last_ms = 0;
    float ring_animation_time_ms = 40.0f;
    bool animating = false;
    bool animation_growing = false;
    unsigned char square_mask = 0;
};
