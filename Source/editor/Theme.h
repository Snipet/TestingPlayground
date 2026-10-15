// Source/editor/Theme.h: the palette the panel draws with. FunkGui has two themes, GRAPHITE (index 0, dark) and PAPER
// (index 1, light); the host hands the Panel the one the user chose (the THEME cells in the header; a machine-wide
// preference), and productTheme() may change its colours before anything is drawn.
//
// Every colour is a token (funkgui::Theme): ground, the ink ladder ink100 > ink70 > ink52 > ink32 > ink16, accent (the
// control under the hand), accentDim (its hover fill) and signal (one job of your choosing, e.g. a meter). To give this
// plugin its own accent, set it here for both themes (keep it readable on both grounds), for example:
//
//     const bool dark = host.ground.r < 128;
//     t.accent    = dark ? funkgui::Col{ 0x4F, 0xC3, 0xF7 } : funkgui::Col{ 0x0B, 0x6E, 0x99 };
//     t.accentDim = dark ? funkgui::Col{ 0x1D, 0x4E, 0x63 } : funkgui::Col{ 0xA9, 0xD3, 0xE6 };
//
// Leave textGamma alone: the host records the theme's text gamma into the frame before the Panel draws.
#pragma once

#include <funkgui/core/Theme.h>

namespace testingplayground::ui
{
    inline funkgui::Theme productTheme(const funkgui::Theme& host)
    {
        funkgui::Theme t = host;
        return t;
    }
}
