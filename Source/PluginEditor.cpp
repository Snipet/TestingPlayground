#include "PluginEditor.h"
#include "embedded/plugin_fonts.h"

#include <algorithm>

namespace
{
    // Knobs are laid out left-to-right in this order.
    constexpr int   kNumKnobs = 3;
    const char*     kKnobLabels[kNumKnobs] = { "ATTACK", "RELEASE", "GAIN" };
}

TestingPlaygroundSynthEditor::VisageHost::VisageHost(TestingPlaygroundSynthProcessor& p)
    : proc(p)
{
    setOpaque(true);
}

TestingPlaygroundSynthEditor::VisageHost::~VisageHost() = default;

void TestingPlaygroundSynthEditor::VisageHost::visibilityChanged()
{
    initialiseIfReady();
}

void TestingPlaygroundSynthEditor::VisageHost::parentHierarchyChanged()
{
    initialiseIfReady();
}

void TestingPlaygroundSynthEditor::VisageHost::resized()
{
    if (app_)
    {
        app_->setWindowDimensions(getWidth(), getHeight());
        layoutChildren();
    }
}

void TestingPlaygroundSynthEditor::VisageHost::knobGeometry(
    int index, int& x, int& y, int& size) const
{
    const int w = app_->width();
    const int h = app_->height();
    size = std::min(w / (kNumKnobs + 1), h * 38 / 100);
    const int cx = w * (index + 1) / (kNumKnobs + 1);
    const int cy = h * 44 / 100;          // nudged up to leave room for labels
    x = cx - size / 2;
    y = cy - size / 2;
}

void TestingPlaygroundSynthEditor::VisageHost::layoutChildren()
{
    if (rootFrame_)
        rootFrame_->setBounds(0, 0, app_->width(), app_->height());

    Knob<juce::AudioParameterFloat>* knobs[kNumKnobs] =
        { attackKnob_.get(), releaseKnob_.get(), gainKnob_.get() };

    for (int i = 0; i < kNumKnobs; ++i)
    {
        if (knobs[i] == nullptr) continue;
        int x, y, size;
        knobGeometry(i, x, y, size);
        knobs[i]->setBounds(x, y, size, size);
    }
}

void TestingPlaygroundSynthEditor::VisageHost::initialiseIfReady()
{
    if (app_ || !isShowing()) return;

    auto* peer = getPeer();
    if (peer == nullptr) return;

    void* nativeHandle = peer->getNativeHandle();
    if (nativeHandle == nullptr) return;

    app_ = std::make_unique<visage::ApplicationWindow>();
    rootFrame_ = std::make_unique<visage::Frame>();

    rootFrame_->onDraw() = [this](visage::Canvas& canvas)
    {
        canvas.setColor(0xff1c1c20);
        canvas.fill(0, 0, rootFrame_->width(), rootFrame_->height());

        // A label under each knob.
        const float font_size = std::max(11.0f, app_->width() * 0.022f);
        const visage::Font font(font_size, resources::fonts::Roboto_Condensed_Regular_ttf);
        canvas.setColor(0xffb0b0b8);

        for (int i = 0; i < kNumKnobs; ++i)
        {
            int x, y, size;
            knobGeometry(i, x, y, size);
            canvas.text(std::string(kKnobLabels[i]), font, visage::Font::kCenter,
                        x, y + size + 6, size, static_cast<int>(font_size) + 4);
        }
    };

    attackKnob_ = std::make_unique<Knob<juce::AudioParameterFloat>>(
        proc.apvts, "attack");
    rootFrame_->addChild(attackKnob_.get());

    releaseKnob_ = std::make_unique<Knob<juce::AudioParameterFloat>>(
        proc.apvts, "release");
    rootFrame_->addChild(releaseKnob_.get());

    gainKnob_ = std::make_unique<Knob<juce::AudioParameterFloat>>(
        proc.apvts, "gain");
    rootFrame_->addChild(gainKnob_.get());

    app_->onResize() = [this]() { layoutChildren(); };

    app_->addChild(rootFrame_.get());

    // JUCE getWidth()/getHeight() return logical pixels; visage::Dimension's
    // implicit float constructor wraps as logicalPixels, which visage then
    // scales by the display DPI to get native pixels. Using nativePixels here
    // would skip the scale and render at 1/dpi size on Retina displays.
    app_->show(static_cast<float>(getWidth()),
               static_cast<float>(getHeight()),
               nativeHandle);
}

TestingPlaygroundSynthEditor::TestingPlaygroundSynthEditor(TestingPlaygroundSynthProcessor& p)
    : juce::AudioProcessorEditor(p), proc(p)
{
    visageHost_ = std::make_unique<VisageHost>(proc);
    addAndMakeVisible(visageHost_.get());
    setSize(600, 300);
}

TestingPlaygroundSynthEditor::~TestingPlaygroundSynthEditor() = default;

void TestingPlaygroundSynthEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void TestingPlaygroundSynthEditor::resized()
{
    visageHost_->setBounds(getLocalBounds());
}
