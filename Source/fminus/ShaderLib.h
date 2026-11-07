// Source/fminus/ShaderLib.h: what a panel's shader may #include besides the program's own .sh files (fminus/Shaders.h),
// the built-in library, and the font its text is drawn in (JUCE-free and FunkGui-free).
//
// The library: funk/text.sh (text and numbers: text(), number(), textWidth()) and funk/shapes.sh (rounded boxes,
// circles, rings, segments, arcs, tick marks, needles, seven-segment digits, gradients, and cover(), the antialiasing
// they share), each in px from the shader's top left corner with y down (v_px), each a coverage (0..1) to mix a colour
// in by. A shader names them as `#include "funk/text.sh"` (or <funk/text.sh>); the build writes them beside it for
// shaderc, each wrapped in an include guard, so two files may both include one.
//
// The font: the panel's own face (FunkGui's bundled JetBrains Mono, monospaced), as FunkGui's committed bake of it
// (its signed distance field at kBasePx, kSpread texels each way), laid out again one glyph a cell of a grid
// (editor/ShaderFont.h makes it once a process and binds it to every shader's s_funkFont): the cell of glyph code c is
// (c - 32) % kColumns across and (c - 32) / kColumns down, kCellW x kCellH texels, the glyph's pen (the left end of
// its baseline) at kPenX, kBaseline in it. text.sh's numbers are these constants; gui.shaders TEXT holds them to the
// bake. Glyph codes: printable ASCII as itself (32 to 126), then kExtra's code points from 127 on (FunkGui's extra
// glyphs, in its order). A shader's string literal ("RPM") becomes its codes, three a float (the first the lowest
// byte), up to 12 in a vec4 and up to kMaxString in a mat4, before shaderc runs (Shaders.cpp's pre-pass).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playground::shaderlib
{
    // The font's grid (texels).
    inline constexpr int kColumns = 16;
    inline constexpr int kRows = 8;
    inline constexpr int kCellW = 40;
    inline constexpr int kCellH = 56;
    inline constexpr int kPenX = 8;
    inline constexpr int kBaseline = 40;
    inline constexpr int kAtlasW = kColumns * kCellW;
    inline constexpr int kAtlasH = kRows * kCellH;
    // The bake's: its size in px (FunkGui's text size: a JUCE font's height), its field's reach each way in texels, and
    // the face's advance (every glyph's) and capitals' height at that size.
    inline constexpr float kBasePx = 48.0f;
    inline constexpr float kSpread = 6.0f;
    inline constexpr float kAdvance = 21.818176f;
    inline constexpr float kCapHeight = 26.545456f;

    // FunkGui's texture id for the atlas, and the sampler text.sh reads it through (stage 0).
    inline constexpr uint32_t kTextureId = 0x464E5431u;      // "FNT1"
    inline constexpr const char* kSampler = "s_funkFont";

    // The code points past ASCII the font has: codes 127 on.
    inline constexpr std::array<uint32_t, 19> kExtra = {
        0x00B0, 0x00B1, 0x00D7, 0x2013, 0x2192, 0x221E, 0x00B7, 0x2191, 0x2193, 0x00B5,
        0x2212, 0x2026, 0x2264, 0x2265, 0x2248, 0x0394, 0x2014, 0x2022, 0x2190,
    };
    inline constexpr int kFirstExtra = 127;
    inline constexpr int kCodes = kFirstExtra + static_cast<int>(kExtra.size());   // codes below this
    static_assert(kCodes - 32 <= kColumns * kRows, "every glyph has a cell");

    inline constexpr int kMaxString = 48;                    // a string literal's characters, at most (a mat4's)

    // A code point's glyph code, or -1 when the font has none (a control character among them).
    int glyphCode(uint32_t codePoint);
    // A glyph code's code point (32 to kCodes - 1), else 0.
    uint32_t codePointOf(int code);

    struct LibraryFile
    {
        std::string name;                                    // "funk/text.sh"
        std::string text;                                    // as shaderc reads it (with its include guard)
    };

    // The library's files, in order.
    const std::vector<LibraryFile>& library();
    // The library's file named `name`, or nullptr.
    const LibraryFile* libraryFile(std::string_view name);
}
