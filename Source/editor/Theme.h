// Source/editor/Theme.h: the palette the panel draws with. FunkGui has two themes, GRAPHITE (index 0, dark) and PAPER
// (index 1, light); the host hands the Panel the one the user chose (the THEME cells in the header; a machine-wide
// preference), and productTheme() may change its colours before anything is drawn.
//
// Every colour is a token (funkgui::Theme): ground, the ink ladder ink100 > ink70 > ink52 > ink32 > ink16, accent (the
// control under the hand), accentDim (its hover fill) and signal (one job of the product's choosing: here, errors: the
// problems list's, the status line's, ChatStyle.h's `danger`). The accent is the product's own colour, and its mark's
// caret is drawn in it (the top bar's, LogoMark.h; the app icon's, Tools/logo/logo.swift), so the mark, the editor's
// caret and RUN agree. The effect keeps FunkGui's orange (#FF5A1F, PAPER #E8451A) and its signal (#7FD4E8, PAPER
// #1E7E96); the instrument is blue (keep an accent readable on both grounds), and its errors take the orange, since
// FunkGui's signal is a blue of the accent's family and an error must never read as the accent.
//
// codeSecond() is the one colour here that is not a token: the code's types (CodeAssist's highlighter) and a note's
// marks (its squiggle, its ring in the margin, its disc in a hover card), apart from the keywords (the accent) and the
// errors (signal). The effect's is FunkGui's signal, as it always was; the instrument's a teal (#4EC9B0, PAPER
// #0F7A68), as its signal's orange is the effect's accent and its accent is blue.
//
// The colours are a kind's (kindTheme, kindCodeSecond: effect or instrument), so the installer (Source/installer/),
// which is no product's, draws each product's mark in its own accent. productTheme() and codeSecond() are the product's
// kind's, defined in Theme.cpp: this header names no product (no Product.h), so code built for no product includes it.
//
// Tools/logo/logo.swift reads the instrument's accent from the `t.accent = dark ? funkgui::Col{...} : ...` line below
// (keep its shape): change it, run the script, and the icons follow.
//
// Leave textGamma alone: the host records the theme's text gamma into the frame before the Panel draws.
#pragma once

#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>

namespace playground::ui
{
    // The theme of a product of that kind (an instrument's when `instrument`, else an effect's).
    inline funkgui::Theme kindTheme(const funkgui::Theme& host, bool instrument)
    {
        funkgui::Theme t = host;
        if (instrument)
        {
            const bool dark = host.ground.r < 128;
            t.accent = dark ? funkgui::Col{ 0x4F, 0xC3, 0xF7 } : funkgui::Col{ 0x0B, 0x6E, 0x99 };
            t.accentDim = dark ? funkgui::Col{ 0x1D, 0x4E, 0x63 } : funkgui::Col{ 0xA9, 0xD3, 0xE6 };
            t.signal = dark ? funkgui::Col{ 0xFF, 0x5A, 0x1F } : funkgui::Col{ 0xE8, 0x45, 0x1A };
        }
        return t;
    }

    // The code's types and a note's marks (above), for the theme kindTheme() made for that kind.
    inline funkgui::Col kindCodeSecond(const funkgui::Theme& th, bool instrument)
    {
        const bool dark = th.ground.r < 128;
        if (instrument)
            return dark ? funkgui::Col{ 0x4E, 0xC9, 0xB0 } : funkgui::Col{ 0x0F, 0x7A, 0x68 };
        return dark ? funkgui::Col{ 0x7F, 0xD4, 0xE8 } : funkgui::Col{ 0x1E, 0x7E, 0x96 };
    }

    // The product's: kindTheme() and kindCodeSecond() for its kind (Theme.cpp, compiled into each product).
    funkgui::Theme productTheme(const funkgui::Theme& host);
    funkgui::Col codeSecond(const funkgui::Theme& th);
}
