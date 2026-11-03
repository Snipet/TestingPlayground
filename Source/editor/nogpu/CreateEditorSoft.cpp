// Source/editor/nogpu/CreateEditorSoft.cpp: the plugin's editor factory in a build without FunkGui's GPU layer
// (FP_WITH_GPU off): the Panel in a SoftEditorHost (SoftEditorHost.h), configured as
// Source/editor/gpu/CreateEditorGpu.cpp configures FunkGui's EditorHost. CMakeLists.txt compiles one of the two.
#include "editor/Panel.h"
#include "editor/nogpu/SoftEditorHost.h"
#include "plugin/Processor.h"

#include <memory>

namespace playground
{
    juce::AudioProcessorEditor* createEditor(Processor& processor)
    {
        ui::SoftEditorConfig config;
        config.zoomSteps = { 100, 125, 150, 200 };     // the Panel's ZOOM cells offer these steps
        config.defaultZoomPercent = 100;
        config.zoomPrefKey = "uiZoom";                 // machine-wide, beside the theme; never in the session
        config.sizeChanged = [&processor](int w, int h) { processor.setEditorSize(w, h); };
        auto panel = std::make_unique<ui::Panel>(processor.ports(), processor.editorPort());
        if (processor.editorWidth() > 0 && processor.editorHeight() > 0)
            panel->resize(processor.editorWidth(), processor.editorHeight());
        return new ui::SoftEditorHost(processor, std::move(config), std::move(panel));
    }
}
