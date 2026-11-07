// Tools/probes/common/Pictures.h: whether this build and platform draw GPU pictures (editor/Picture.h: FunkGui's
// renderGpuPicture), for the probes whose rows look at one (gui.shaders, plugin.chat, plugin.mcp, plugin.sandbox).
// Where they do not, those rows are skipped with noGpuPicturesSkip() as the reason (probe::skip), and the rows that
// check the fallback, a picture drawn with SoftRaster whose text says why (noGpuPictures()), run in their place.
#pragma once

#include <string>

#if defined(FUNKGUI_HAS_BGFX) && FUNKGUI_HAS_BGFX
#include <funkgui/gpu/GpuPicture.h>
#endif

namespace probe
{
    // Why no GPU picture is drawn here, in the words a picture's text says it (editor/PictureDraw.cpp: "Drawn without the
    // GPU (<why>)"); "" when GPU pictures are drawn.
    inline std::string noGpuPictures()
    {
#if defined(FUNKGUI_HAS_BGFX) && FUNKGUI_HAS_BGFX
        return funkgui::gpuPicturesSupported() ? std::string() : std::string("no GPU pictures on this platform");
#else
        return "this build has no GPU";
#endif
    }

    // The reason a row that looks at a GPU picture's pixels is skipped here.
    inline std::string noGpuPicturesSkip()
    {
#if defined(__linux__) && defined(FUNKGUI_HAS_BGFX) && FUNKGUI_HAS_BGFX
        return "no GPU picture on Linux: FunkGui draws on Vulkan there but reads no frame back (gpuPicturesSupported)";
#else
        return "no GPU picture here: " + noGpuPictures();
#endif
    }
}
