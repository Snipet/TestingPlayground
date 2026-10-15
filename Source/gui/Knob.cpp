#include "gui/Knob.h"
#include "embedded/plugin_shaders.h"
#include "embedded/plugin_fonts.h"

#include <algorithm>

template <typename T>
Knob<T>::Knob(juce::AudioProcessorValueTreeState& state,
              const juce::String& paramID_,
              bool square)
    : background_shader(resources::shaders::vs_shader_quad,
                        resources::shaders::fs_knob_background2,
                        visage::BlendMode::Alpha),
      apvts(state),
      paramID(paramID_)
{
    param = dynamic_cast<T*>(apvts.getParameter(paramID));
    jassert(param != nullptr);

    norm_value = param != nullptr ? param->convertTo0to1(apvts.getRawParameterValue(paramID)->load())
                                  : 0.0f;
    last_ms = visage::time::milliseconds();
    square_mask = square ? 0xff : 0x00;
}

template <typename T>
Knob<T>::~Knob() = default;

template <typename T>
void Knob<T>::draw(visage::Canvas& canvas)
{
    const float raw = apvts.getRawParameterValue(paramID)->load();
    norm_value = param->convertTo0to1(raw);

    const auto value_byte = static_cast<unsigned int>(norm_value * 255.0f);
    const float anim = updateAnimation();
    const auto thickness_byte = static_cast<unsigned int>(anim * 255.0f);
    const auto roundness_byte = static_cast<unsigned int>(square_mask);

    // Pack three channels of state into the canvas color. The fragment
    // shader unpacks them out of input_values.r/.g/.z (= color.r/.g/.b).
    unsigned int packed = value_byte
                        | (thickness_byte << 8)
                        | (roundness_byte << 16);
    canvas.setColor(packed);
    canvas.shader(&background_shader, 0.0f, 0.0f, width(), height());

    // Centre value-as-text.
    const juce::String value_text = param->getCurrentValueAsText();
    float shrink = 0.0f;
    if (value_text.length() > 4)
        shrink = (value_text.length() - 4) * 0.01f;

    const float font_size = width() * (0.17f - shrink);
    const visage::Font knob_font(font_size, resources::fonts::Roboto_Condensed_Regular_ttf);

    const unsigned char alpha = static_cast<unsigned char>(190 + anim * (255 - 190));
    const unsigned int text_color = (static_cast<unsigned int>(alpha) << 24) | 0x00ffffff;
    canvas.setColor(text_color);
    canvas.text(value_text.toStdString(), knob_font, visage::Font::kCenter,
                0.0f, 0.0f, width(), height());

    redraw();
}

template <typename T>
void Knob<T>::resized() {}

template <typename T>
void Knob<T>::startAnimation(bool grow)
{
    animation_growing = grow;
    last_ms = visage::time::milliseconds();
    animating = true;
}

template <typename T>
float Knob<T>::updateAnimation()
{
    const long long ms = visage::time::milliseconds();
    const float delta = static_cast<float>(ms - last_ms) / ring_animation_time_ms;
    ring_thickness = animation_growing ? std::min(ring_thickness + delta, 1.0f)
                                       : std::max(ring_thickness - delta, 0.0f);
    last_ms = ms;
    if ((ring_thickness >= 1.0f && animation_growing) ||
        (ring_thickness <= 0.0f && !animation_growing))
        animating = false;
    return ring_thickness;
}

template <typename T>
void Knob<T>::mouseDown(const visage::MouseEvent& e)
{
    lasty = e.position.y;
}

template <typename T>
void Knob<T>::mouseEnter(const visage::MouseEvent&)
{
    startAnimation(true);
    redraw();
}

template <typename T>
void Knob<T>::mouseExit(const visage::MouseEvent&)
{
    startAnimation(false);
    redraw();
}

template <typename T>
void Knob<T>::mouseDrag(const visage::MouseEvent& e)
{
    if (lasty == -1.0f)
        lasty = e.position.y;

    const float dy = lasty - e.position.y;
    const float sensitivity = e.isCtrlDown() ? 0.002f : 0.008f;
    norm_value = std::clamp(norm_value + dy * sensitivity, 0.0f, 1.0f);
    lasty = e.position.y;

    param->setValueNotifyingHost(norm_value);
    redraw();
}

template class Knob<juce::AudioParameterFloat>;
template class Knob<juce::AudioParameterInt>;
