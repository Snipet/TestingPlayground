// Source/editor/ShaderFont.h: the font a panel's shader draws text in (fminus/ShaderLib.h, funk/text.sh), as FunkGui's
// texture.
//
// The atlas is made once a process from FontService's (FunkGui's bake of its bundled face, which the panel's own text is
// drawn with), each glyph's signed distance field moved into its cell of ShaderLib.h's grid at its pen, by its bearing
// (bilinear, so a fraction of a texel stays one). bindShaderFont() registers it with FunkGui (shaders::setTexture, R8,
// linear and clamped: once a process, the registry keeping the texels across bgfx's restarts) and binds it to a
// shader's s_funkFont (shaders::bindTexture): GuiView::registerShaders does so for every shader it registers, whether
// or not it includes text.sh (a shader that declares no such sampler reads nothing of it). Message thread, as
// FontService and FunkGui's registry are.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace playground::ui
{
    struct ShaderFontAtlas
    {
        std::vector<uint8_t> texels;     // R8, ShaderLib.h's kAtlasW x kAtlasH, the first row the top; empty: none
        bool ok = false;
        std::string why;                 // when not ok: why there is none
        // What the bake measured, which ShaderLib.h's constants must be (gui.shaders TEXT): its size in px, its field's
        // reach, the advance of every glyph (one when they agree, else -1), the capitals' height; and each glyph's
        // reach inside its cell, every one in it (none clipped).
        float basePx = 0.0f, spread = 0.0f, advance = 0.0f, capHeight = 0.0f;
        bool allInCells = false;
        int glyphs = 0;                  // the cells filled (codes past the space with a glyph)
    };

    // The atlas, made at the first call (a few milliseconds). Message thread.
    const ShaderFontAtlas& shaderFontAtlas();

    // The atlas registered with FunkGui (once) and bound to `shaderId`'s sampler ShaderLib.h's kSampler. Message thread.
    void bindShaderFont(uint32_t shaderId);
}
