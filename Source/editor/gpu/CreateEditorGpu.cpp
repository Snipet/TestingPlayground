// Source/editor/gpu/CreateEditorGpu.cpp: the plugin's editor factory (the plugin target only; the probes compile
// Source/plugin/CreateEditorGeneric.cpp instead). A funkgui::EditorHost runs the Panel on the GPU (bgfx on Metal):
// it owns the window surface, the frame clock, input, accessibility and the host's parameter menu, and it implements
// HostServices for the Panel. The Panel owns no JUCE component and draws the same frames headless (the gui.dump probe).
//
// Diagnostics (FunkGui reads them under the product's prefix, PRODUCT_ENV_PREFIX in products/<key>.cmake):
// <prefix>UI_THEME=1 opens on the light theme without saving it, <prefix>GPU_LOG=1 logs the surface's life,
// <prefix>UI_ZOOM=<percent> pins the zoom.
#include "editor/Panel.h"
#include "plugin/Processor.h"

#include <funkgui/gpu/EditorHost.h>

#include <memory>

namespace playground
{
    juce::AudioProcessorEditor* createEditor(Processor& processor)
    {
        funkgui::EditorConfig config;                  // width/height 0: the Panel's own size
        config.zoomSteps = { 100, 125, 150, 200 };     // UI zoom: the Panel's ZOOM cells offer these steps
        config.defaultZoomPercent = 100;
        config.zoomPrefKey = "uiZoom";                 // machine-wide, beside the theme; never in the session
        // The window is resizable (the Panel is): it opens at the size the session left it at, and each size the user
        // drags it to goes back to the processor, into the session.
        config.sizeChanged = [&processor](int w, int h) { processor.setEditorSize(w, h); };
        auto panel = std::make_unique<ui::Panel>(processor.ports(), processor.editorPort());
        if (processor.editorWidth() > 0 && processor.editorHeight() > 0)
            panel->resize(processor.editorWidth(), processor.editorHeight());
        return new funkgui::EditorHost(processor, std::move(config), std::move(panel));
    }
}
