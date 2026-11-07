// Source/editor/ShaderFont.cpp: see ShaderFont.h.
#include "editor/ShaderFont.h"

#include "fminus/ShaderLib.h"

#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/text/FontAtlasSdf.h>
#include <funkgui/text/FontService.h>

#include <algorithm>
#include <cmath>

namespace playground::ui
{
    namespace
    {
        namespace lib = shaderlib;

        // The bake's field at a texel position (x, y: texel centres at whole numbers) of the patch at (x0, y0), w x h,
        // bilinear; 0 (far outside) off the patch.
        float fieldAt(const std::vector<uint8_t>& px, int x0, int y0, int w, int h, float x, float y)
        {
            const auto texel = [&](int tx, int ty) -> float {
                if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                    return 0.0f;
                const std::size_t at = static_cast<std::size_t>(y0 + ty) * funkgui::FontAtlasSdf::kAtlasW
                                     + static_cast<std::size_t>(x0 + tx);
                return at < px.size() ? static_cast<float>(px[at]) : 0.0f;
            };
            const float fx = std::floor(x), fy = std::floor(y);
            const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
            const float ax = x - fx, ay = y - fy;
            const float top = texel(ix, iy) + (texel(ix + 1, iy) - texel(ix, iy)) * ax;
            const float bottom = texel(ix, iy + 1) + (texel(ix + 1, iy + 1) - texel(ix, iy + 1)) * ax;
            return top + (bottom - top) * ay;
        }

        ShaderFontAtlas made()
        {
            ShaderFontAtlas out;
            const funkgui::FontAtlasSdf& bake = funkgui::FontService::get().atlas();
            if (!bake.baked())
            {
                out.why = "FunkGui's font atlas is not baked";
                return out;
            }
            using Sdf = funkgui::FontAtlasSdf;
            out.basePx = Sdf::kBasePx;
            out.spread = static_cast<float>(Sdf::kSpread);
            out.capHeight = bake.capHeight();
            out.allInCells = true;
            out.texels.assign(static_cast<std::size_t>(lib::kAtlasW) * lib::kAtlasH, 0);
            const std::vector<uint8_t>& px = bake.pixels();
            for (int code = 33; code < lib::kCodes; ++code)
            {
                const Sdf::Glyph* g = bake.glyph(lib::codePointOf(code));
                if (g == nullptr)
                {
                    out.allInCells = false;            // a code with no glyph: gui.shaders TEXT says so
                    continue;
                }
                out.advance = out.advance == 0.0f || std::fabs(out.advance - g->advance) < 1e-3f ? g->advance : -1.0f;
                if (!(g->w > 0.0f))
                    continue;
                const int x0 = static_cast<int>(std::lround(g->u0 * Sdf::kAtlasW));
                const int y0 = static_cast<int>(std::lround(g->v0 * Sdf::kAtlasH));
                const int w = static_cast<int>(std::lround(g->w)), h = static_cast<int>(std::lround(g->h));
                // The patch's place in the cell: its left and top edges, from the pen at (kPenX, kBaseline).
                const float left = static_cast<float>(lib::kPenX) + g->bx, top = static_cast<float>(lib::kBaseline) + g->by;
                if (left < 1.0f || top < 1.0f || left + static_cast<float>(w) > static_cast<float>(lib::kCellW - 1)
                    || top + static_cast<float>(h) > static_cast<float>(lib::kCellH - 1))
                    out.allInCells = false;
                const int cell = code - 32;
                const int cx = (cell % lib::kColumns) * lib::kCellW, cy = (cell / lib::kColumns) * lib::kCellH;
                for (int ty = 0; ty < lib::kCellH; ++ty)
                    for (int tx = 0; tx < lib::kCellW; ++tx)
                    {
                        const float v = fieldAt(px, x0, y0, w, h, static_cast<float>(tx) - left, static_cast<float>(ty) - top);
                        out.texels[static_cast<std::size_t>(cy + ty) * lib::kAtlasW + static_cast<std::size_t>(cx + tx)] =
                            static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
                    }
                ++out.glyphs;
            }
            out.ok = true;
            return out;
        }
    }

    const ShaderFontAtlas& shaderFontAtlas()
    {
        static const ShaderFontAtlas atlas = made();
        return atlas;
    }

    void bindShaderFont(uint32_t shaderId)
    {
        const ShaderFontAtlas& atlas = shaderFontAtlas();
        if (!atlas.ok)
            return;
        if (!funkgui::shaders::hasTexture(lib::kTextureId))
            funkgui::shaders::setTexture(lib::kTextureId, lib::kAtlasW, lib::kAtlasH, funkgui::shaders::TextureFormat::r8,
                                         atlas.texels.data());
        funkgui::shaders::bindTexture(shaderId, lib::kSampler, lib::kTextureId);
    }
}
