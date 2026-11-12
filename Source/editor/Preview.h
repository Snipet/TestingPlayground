// Source/editor/Preview.h: a picture of a version's panel for the Projects card (ProjectBrowser.h), made without
// compiling anything: the program's dials read by FMinus's language service (editor/ProgramDials.h, as the GUI sandbox
// reads them), set to the version's saved values by name, and its .gui file drawn by the editor's own code
// (editor/PanelView.h, the GUI sandbox's panel: GuiView over mock ports and the sandbox's demo meters) at the PANEL
// view's area for the editor's default size (853 x 612), as view_panel's picture shows it but for the displays, which
// show the sandbox's demo values (a steady tone, fields moving slowly), since nothing plays.
//
// In two steps, as the threads allow:
//   prepare()   any thread but the audio thread: the dials, readGuiSpec, the shaders built through the process's
//               shaderCache() (a hit costs nothing, a miss about 60 ms a shader; no shaderc: the placeholder, said in
//               `notes`). The Processor runs it on a thread of its own (ProcessorPreviews.cpp)
//   draw()      the message thread only (bgfx, as editor/Picture.h): the panel laid out and drawn by picture::draw (the
//               GPU, else SoftRaster with each shader a striped placeholder), then scaled to `maxWidth`
// A program with no .gui file has no preview yet: prepare() says so ("this program has no panel.gui").
#pragma once

#include "dsp/Controls.h"
#include "dsp/Sources.h"
#include "fminus/PanelSpec.h"
#include "fminus/Shaders.h"

#include <funkgui/canvas/SoftRaster.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace playground::preview
{
    inline constexpr int kAreaWidth = 853, kAreaHeight = 612;  // the PANEL view at the editor's default size (logical px)
    inline constexpr int kMaxWidth = 640;                      // the picture registered for the card (physical px)

    // A dial as the version keeps it: its name, its plain value and its value's option name ("" for none).
    struct Dial
    {
        std::string name;
        float value = 0.0f;
        std::string label;
    };

    struct Request
    {
        Sources files;                           // the version's
        std::vector<Dial> dials;                 // its saved values, by name
        bool instrument = false;
    };

    // What draw() needs, made off the message thread.
    struct Prepared
    {
        bool ok = false;
        std::string why;                         // when not ok: why there is no preview, in words
        PanelSpec spec;
        Controls controls{};
        std::array<float, kMaxControls> values01{};
        Sources files;
        std::string notes;                       // readGuiSpec's and the shaders' builds'
        bool instrument = false;
    };

    // `tools` the shaders' (the Processor's, as its Compiler has them; a tools without shaderc leaves each a
    // placeholder).
    Prepared prepare(const Request& r, const ShaderTools& tools);

    struct Picture
    {
        funkgui::Image image;                    // straight RGBA8, at most maxWidth across (0: the area at 1.5 px a px)
        bool gpu = false;                        // drawn on the GPU (shaders as on screen); false: SoftRaster
        std::string why;                         // SoftRaster's reason ("" on the GPU), or why nothing was drawn
    };
    // Message thread only. `theme` FunkGui's (0 graphite, 1 paper); `maxWidth` 0 keeps the picture whole (the probes
    // compare it with view_panel's).
    Picture draw(const Prepared& p, int theme, int maxWidth);
}
