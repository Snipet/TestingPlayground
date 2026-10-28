// Source/editor/Picture.cpp: picturePanel (plugin/EditorPort.h), the chat's view_panel tool's picture: a Panel of its
// own, headless, at the editor's size, its frame rasterised by FunkGui's SoftRaster and written as a PNG.
#include "editor/Panel.h"
#include "plugin/EditorPort.h"

#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>

namespace playground
{
    namespace
    {
        constexpr float kDpi = 1.5f;             // the picture's pixels a logical px: the panel's text stays legible

        // The PNG of `img` cropped to `crop` (physical px), its bytes.
        std::string pngOf(const funkgui::Image& img, int x0, int y0, int w, int h)
        {
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

        // The meters are measured only while someone reads them (EditorPort::addMeterReader). When the picture's Panel
        // is the only reader (no editor open), wait for two blocks measured (the one under way may have begun before
        // it), so its displays show the sound playing now: 100 ms at most, for an audio thread that is not running.
        void waitForMeters(EditorPort& port)
        {
            if (port.meterReaders() > 1)
                return;
            Meters m;
            for (int k = 0; k < 10 && !port.meters(m); ++k)
                std::this_thread::yield();
            const uint32_t from = m.frame;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (std::chrono::steady_clock::now() < until)
            {
                if (port.meters(m) && m.frame - from >= 2)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    PanelPicture picturePanel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, int width, int height,
                              bool window, const std::string& page)
    {
        PanelPicture out;
        // What the user has, given back after: the view, the chat's fold, the pages.
        const bool view = port.panelView();
        const bool chatOpen = port.chatOpen();
        const std::vector<std::string> pages = port.guiPages();
        {
            ui::Panel panel(ports, port);
            waitForMeters(port);
            funkgui::HeadlessHost host(panel, funkgui::UiPreferences::get().theme(), kDpi);
            host.resize(std::max(width > 0 ? width : ui::kDefaultWidth, ui::kMinWidth),
                        std::max(height > 0 ? height : ui::kDefaultHeight, ui::kMinHeight));
            host.settle(120);
            if (!window)
                panel.setPanelView(true);
            // The page asked for: the first tabs element with a page of that title.
            std::string shownPage;
            const std::vector<GuiNode>& nodes = panel.panelSpec().gui.nodes;
            std::string allPages;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (nodes[i].kind != GuiKind::tabs)
                    continue;
                std::string these;
                for (std::size_t k = 0; k < nodes[i].children.size(); ++k)
                {
                    const std::string& title = nodes[static_cast<std::size_t>(nodes[i].children[k])].text;
                    these += (these.empty() ? "" : " / ") + title;
                    if (!page.empty() && shownPage.empty() && title == page)
                    {
                        panel.chooseGuiPage(static_cast<int>(i), static_cast<int>(k));
                        shownPage = title;
                    }
                }
                allPages += (allPages.empty() ? "" : "; ") + these;
            }
            host.settle(120);
            const funkgui::PrimList& frame = host.draw();
            const funkgui::Image img = funkgui::rasterise(frame, funkgui::FontService::get().atlas(), 1);
            if (img.w <= 0 || img.h <= 0)
                out.why = "the editor drew nothing";
            else
            {
                // The panel's own area (a .gui file's interface, else the program's card) or the whole editor.
                funkgui::Rect crop{ 0.0f, 0.0f, static_cast<float>(img.w) / kDpi, static_cast<float>(img.h) / kDpi };
                if (!window)
                    crop = panel.hasGui() ? panel.guiLayout().area : panel.programCard();
                const int x0 = std::clamp(static_cast<int>(std::floor(crop.x * kDpi)), 0, img.w - 1);
                const int y0 = std::clamp(static_cast<int>(std::floor(crop.y * kDpi)), 0, img.h - 1);
                const int w = std::clamp(static_cast<int>(std::ceil(crop.w * kDpi)), 1, img.w - x0);
                const int h = std::clamp(static_cast<int>(std::ceil(crop.h * kDpi)), 1, img.h - y0);
                out.png = pngOf(img, x0, y0, w, h);
                out.width = w;
                out.height = h;
                if (out.png.empty())
                    out.why = "the picture could not be written";
                const int lw = static_cast<int>(std::lround(crop.w)), lh = static_cast<int>(std::lround(crop.h));
                out.about = std::string(window ? "The whole editor, as the user has it" : "The program's panel, in the PANEL view")
                          + ", " + std::to_string(lw) + " x " + std::to_string(lh) + " logical px (the picture "
                          + std::to_string(w) + " x " + std::to_string(h) + " pixels)"
                          + (window ? "" : (panel.hasGui() ? ", from panel.gui" : ", the dials (no .gui file)"))
                          + (allPages.empty() ? std::string() : ". Its pages: " + allPages)
                          + (shownPage.empty() ? (page.empty() ? std::string() : ". There is no page " + page + "; the first pages are shown")
                                               : ". Shown: page " + shownPage)
                          + ". Shaders show as a striped placeholder (the GPU draws them), and the displays show the sound "
                            "playing now.";
            }
        }
        port.setPanelView(view);
        port.setChatOpen(chatOpen);
        port.setGuiPages(pages);
        return out;
    }
}
