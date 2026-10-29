// Source/editor/Picture.h: how view_panel's picture is drawn (Picture.cpp; picturePanel, picturesIdle in
// plugin/EditorPort.h). The headless Panel's frame goes to the GPU, FunkGui's renderGpuPicture (v0.23.0,
// funkgui/gpu/GpuPicture.h), which draws it as an editor's window does, custom shaders included, where this build has
// bgfx (FUNKGUI_HAS_BGFX) and this platform has GPU pictures (gpuPicturesSupported); on any other status, SoftRaster
// draws it (funkgui::rasterise), each shader as FunkGui's striped placeholder, and the picture's text says so and why.
// Message thread only, as everything that touches bgfx.
#pragma once

#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>

#include <string>

namespace playground::picture
{
    struct Drawn
    {
        funkgui::Image image;                    // RGBA8, the frame's own size (logical size x its dpi); empty: nothing
        bool gpu = false;                        // drawn by the GPU (shaders as on screen); false: SoftRaster
        std::string why;                         // SoftRaster's reason ("" on the GPU): FunkGui's status word
                                                 // (toString), or what this build or platform lacks
    };

    // `frame` (HeadlessHost::draw's) drawn at its own size and dpi: on the GPU when it can, else with SoftRaster at one
    // sample a pixel, as the GPU's picture is compared with it (gpu/GpuPicture.h).
    Drawn draw(const funkgui::PrimList& frame);

    // Tests only: while on, every picture takes SoftRaster's path as if the GPU had failed (`why` "forced by a test"),
    // never touching bgfx, so a probe sees the fallback and its note on a Mac whose GPU works.
    void forceSoftRasterForTests(bool on);
}
