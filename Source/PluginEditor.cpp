#include "PluginEditor.h"

TestingPlaygroundEditor::VisageHost::VisageHost(TestingPlaygroundProcessor& p)
    : proc(p)
{
    setOpaque(true);
}

TestingPlaygroundEditor::VisageHost::~VisageHost() = default;

void TestingPlaygroundEditor::VisageHost::visibilityChanged()
{
    initialiseIfReady();
}

void TestingPlaygroundEditor::VisageHost::parentHierarchyChanged()
{
    initialiseIfReady();
}

void TestingPlaygroundEditor::VisageHost::resized()
{
    if (app_)
    {
        app_->setWindowDimensions(getWidth(), getHeight());
        if (rootFrame_)
            rootFrame_->setBounds(0, 0, app_->width(), app_->height());
        if (graphEditor_)
            graphEditor_->setBounds(0, 0, app_->width(), app_->height());
    }
}

void TestingPlaygroundEditor::VisageHost::initialiseIfReady()
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
        canvas.setColor(0xff0c0f17);   // backstop — matches GraphEditor bg
        canvas.fill(0, 0, rootFrame_->width(), rootFrame_->height());
    };

    graphEditor_ = std::make_unique<graph::GraphEditor>();

    // Save As — modal native dialog. Stash the chooser on the host so it
    // outlives the lambda; juce::FileChooser is move-only and the async
    // callback fires after launchAsync returns.
    graphEditor_->requestSavePath =
        [this](std::function<void(std::string)> done)
    {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Save LFO graph",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.lfo");

        chooser_->launchAsync(
            juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
            [done](const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            done(file.getFullPathName().toStdString());
        });
    };
    graphEditor_->requestLoadPath =
        [this](std::function<void(std::string)> done)
    {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Load LFO graph",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.lfo");

        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectFiles,
            [done](const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            done(file.getFullPathName().toStdString());
        });
    };

    rootFrame_->addChild(graphEditor_.get());
    rootFrame_->setBounds(0, 0, static_cast<float>(getWidth()), static_cast<float>(getHeight()));
    graphEditor_->setBounds(0, 0, static_cast<float>(getWidth()), static_cast<float>(getHeight()));

    app_->onResize() = [this]()
    {
        if (rootFrame_)
            rootFrame_->setBounds(0, 0, app_->width(), app_->height());
        if (graphEditor_)
            graphEditor_->setBounds(0, 0, app_->width(), app_->height());
    };

    app_->addChild(rootFrame_.get());

    // JUCE getWidth()/getHeight() return logical pixels; visage::Dimension's
    // implicit float constructor wraps as logicalPixels, which visage then
    // scales by the display DPI to get native pixels. Using nativePixels here
    // would skip the scale and render at 1/dpi size on Retina displays.
    app_->show(static_cast<float>(getWidth()),
               static_cast<float>(getHeight()),
               nativeHandle);
}

TestingPlaygroundEditor::TestingPlaygroundEditor(TestingPlaygroundProcessor& p)
    : juce::AudioProcessorEditor(p), proc(p)
{
    visageHost_ = std::make_unique<VisageHost>(proc);
    addAndMakeVisible(visageHost_.get());
    setSize(900, 600);
}

TestingPlaygroundEditor::~TestingPlaygroundEditor() = default;

void TestingPlaygroundEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void TestingPlaygroundEditor::resized()
{
    visageHost_->setBounds(getLocalBounds());
}
