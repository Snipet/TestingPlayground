// Source/editor/PictureDraw.cpp: a recorded frame drawn as view_panel's picture is (editor/Picture.h): on the GPU
// (FunkGui's renderGpuPicture), else with SoftRaster, and its PNG. Apart from Picture.cpp (the headless Panel), so that
// the GUI sandbox (Source/sandbox/), which draws a .gui file's interface with no Panel and no plugin, draws it the
// same way.
#include "editor/Picture.h"

#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/text/FontService.h>
#if defined(FUNKGUI_HAS_BGFX) && FUNKGUI_HAS_BGFX
#include <funkgui/gpu/GpuPicture.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace playground::picture
{
    namespace
    {
        std::atomic<bool> softForTests{ false };  // forceSoftRasterForTests
        // When a picture was last drawn on the GPU (steady_clock, ms; 0: never), for picturesIdle. Written and read on
        // the message thread; atomic all the same, as cheap.
        std::atomic<int64_t> lastPictureMs{ 0 };

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    }

    void forceSoftRasterForTests(bool on) { softForTests.store(on); }

    int64_t msSinceGpuPicture()
    {
        const int64_t last = lastPictureMs.load();
        return last == 0 ? -1 : nowMs() - last;
    }

    std::string png(const funkgui::Image& img, int x0, int y0, int w, int h)
    {
        if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0 || x0 + w > img.w || y0 + h > img.h)
            return {};
        funkgui::Image part;
        part.w = w;
        part.h = h;
        part.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        for (int y = 0; y < h; ++y)
            std::copy_n(img.rgba.data() + (static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(img.w) + static_cast<std::size_t>(x0)) * 4,
                        static_cast<std::size_t>(w) * 4,
                        part.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4);
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec)
            return {};
        const std::filesystem::path file = dir / ("tp-panel-" + std::to_string(reinterpret_cast<uintptr_t>(&part)) + ".png");
        if (!funkgui::writePng(part, file.string().c_str()))
            return {};
        std::ifstream in(file, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::filesystem::remove(file, ec);
        return bytes;
    }

    Drawn draw(const funkgui::PrimList& frame)
    {
        Drawn out;
        if (softForTests.load())
            out.why = "forced by a test";
        else
        {
#if defined(FUNKGUI_HAS_BGFX) && FUNKGUI_HAS_BGFX
            if (!funkgui::gpuPicturesSupported())
                out.why = "no GPU pictures on this platform";
            else
            {
                // On the message thread it draws at once, on an open editor's bgfx or a headless one it starts and
                // holds (picturesIdle lets that go).
                funkgui::GpuPicture pic = funkgui::renderGpuPicture(frame);
                lastPictureMs.store(nowMs());
                if (pic.ok() && pic.image.w > 0 && pic.image.h > 0)
                {
                    out.image = std::move(pic.image);
                    out.gpu = true;
                    return out;
                }
                out.why = funkgui::toString(pic.status);
            }
#else
            out.why = "this build has no GPU";
#endif
        }
        // SoftRaster at one sample a pixel, as gpu/GpuPicture.h compares the GPU's picture with it.
        out.image = funkgui::rasterise(frame, funkgui::FontService::get().atlas(), 1);
        return out;
    }
}
