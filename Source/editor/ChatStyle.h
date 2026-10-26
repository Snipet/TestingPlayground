// Source/editor/ChatStyle.h: the chat column's look (ChatView, ChatItems, the Panel's chat card and key dialog). The
// panel around it is square-edged, uppercase and tracked; the chat is set apart from it the way a modern chat app is:
// a rounded card a shade off the ground, rounded bubbles, cards and pills, sentence case at a reading size, and line
// icons. Every colour is mixed from the theme's tokens, so both themes (and a product accent) carry over.
#pragma once

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/text/TextStyle.h>

#include <cmath>
#include <cstdint>

namespace testingplaygroundsynth::ui::chat
{
    struct Palette
    {
        funkgui::Col card;             // the chat column's ground
        funkgui::Col cardEdge;
        funkgui::Col bubble;           // the user's messages
        funkgui::Col code;             // code blocks, inline code
        funkgui::Col codeEdge;
        funkgui::Col composer;         // the message box
        funkgui::Col composerEdge;
        funkgui::Col hover;            // a control under the pointer
        funkgui::Col raised;           // a card on the card (actions, chips)
        funkgui::Col text, soft, dim, faint;
        funkgui::Col accent, onAccent;
        funkgui::Col danger;           // errors (the product's signal colour, as the editor's)
    };

    inline Palette palette(const funkgui::Theme& th)
    {
        const bool dark = th.ground.r < 128;
        constexpr funkgui::Col white{ 255, 255, 255, 255 }, black{ 0, 0, 0, 255 };
        Palette p;
        p.card = dark ? funkgui::mix(th.ground, th.ink100, 0.035f) : funkgui::mix(th.ground, white, 0.62f);
        p.cardEdge = dark ? funkgui::mix(th.ground, th.ink100, 0.085f) : funkgui::mix(th.ground, th.ink100, 0.11f);
        p.bubble = dark ? funkgui::mix(th.ground, th.ink100, 0.115f) : funkgui::mix(th.ground, th.ink100, 0.07f);
        p.code = dark ? funkgui::mix(th.ground, black, 0.32f) : funkgui::mix(th.ground, th.ink100, 0.035f);
        p.codeEdge = dark ? funkgui::mix(th.ground, th.ink100, 0.07f) : funkgui::mix(th.ground, th.ink100, 0.09f);
        p.composer = dark ? funkgui::mix(th.ground, th.ink100, 0.07f) : white;
        p.composerEdge = dark ? funkgui::mix(th.ground, th.ink100, 0.14f) : funkgui::mix(th.ground, th.ink100, 0.16f);
        p.hover = dark ? funkgui::mix(th.ground, th.ink100, 0.12f) : funkgui::mix(th.ground, th.ink100, 0.08f);
        p.raised = dark ? funkgui::mix(th.ground, th.ink100, 0.055f) : funkgui::mix(th.ground, white, 0.85f);
        p.text = th.ink100;
        p.soft = th.ink70;
        p.dim = th.ink52;
        p.faint = th.ink32;
        p.accent = th.accent;
        p.onAccent = dark ? funkgui::mix(th.ground, black, 0.4f) : white;
        p.danger = th.signal;
        return p;
    }

    // The one typeface (JetBrains Mono) at a reading size; weight through the SDF threshold (~0.05 reads semibold).
    inline constexpr funkgui::TextStyle kBody{ 12.0f, 0.0f, 0.0f, false };
    inline constexpr funkgui::TextStyle kStrong{ 12.0f, 0.0f, 0.065f, false };
    inline constexpr funkgui::TextStyle kSmall{ 10.5f, 0.0f, 0.0f, false };
    inline constexpr funkgui::TextStyle kSmallStrong{ 10.5f, 0.0f, 0.06f, false };
    inline constexpr funkgui::TextStyle kCodeText{ 11.0f, 0.0f, 0.0f, false };
    inline constexpr funkgui::TextStyle kTitle{ 12.5f, 0.15f, 0.07f, false };
    inline constexpr funkgui::TextStyle kH1{ 14.0f, 0.0f, 0.075f, false };
    inline constexpr funkgui::TextStyle kH2{ 13.0f, 0.0f, 0.075f, false };
    inline constexpr funkgui::TextStyle kH3{ 12.0f, 0.0f, 0.075f, false };
    inline constexpr float kBodyLine = 19.0f;      // px between lines of a paragraph
    inline constexpr float kSmallLine = 15.0f;
    inline constexpr float kCodeLine = 16.0f;

    // Line icons, drawn in a box of `size` px centred on (cx, cy).
    enum class Icon : uint8_t
    {
        none, plus, key, chevronDown, chevronRight, chevronUp, arrowUp, stop, copy, check, file, play, dial, wave, alert,
        pencil, bolt
    };

    inline void drawIcon(funkgui::Canvas& c, Icon icon, float cx, float cy, float size, funkgui::Col ink)
    {
        const float s = size * 0.5f;                  // half the box
        const float w = std::fmax(1.2f, size * 0.1f); // the stroke
        switch (icon)
        {
            case Icon::none:
                break;
            case Icon::plus:
                c.segment(cx - s * 0.7f, cy, cx + s * 0.7f, cy, w, ink);
                c.segment(cx, cy - s * 0.7f, cx, cy + s * 0.7f, w, ink);
                break;
            case Icon::pencil:
            {
                // A pencil over a line: "write a new message".
                c.segment(cx - s * 0.55f, cy + s * 0.55f, cx + s * 0.45f, cy - s * 0.45f, w * 1.6f, ink);
                c.segment(cx - s * 0.75f, cy + s * 0.75f, cx - s * 0.55f, cy + s * 0.55f, w, ink);
                c.segment(cx - s * 0.1f, cy + s * 0.8f, cx + s * 0.8f, cy + s * 0.8f, w, ink);
                break;
            }
            case Icon::key:
                c.disc(cx - s * 0.42f, cy, s * 0.34f, ink.withAlpha(0.0f), w, ink);
                c.segment(cx - s * 0.08f, cy, cx + s * 0.8f, cy, w, ink);
                c.segment(cx + s * 0.62f, cy, cx + s * 0.62f, cy + s * 0.36f, w, ink);
                c.segment(cx + s * 0.34f, cy, cx + s * 0.34f, cy + s * 0.28f, w, ink);
                break;
            case Icon::chevronDown:
            {
                const float xs[3] = { cx - s * 0.5f, cx, cx + s * 0.5f };
                const float ys[3] = { cy - s * 0.22f, cy + s * 0.28f, cy - s * 0.22f };
                c.polyline(xs, ys, 3, w, ink);
                break;
            }
            case Icon::chevronUp:
            {
                const float xs[3] = { cx - s * 0.5f, cx, cx + s * 0.5f };
                const float ys[3] = { cy + s * 0.22f, cy - s * 0.28f, cy + s * 0.22f };
                c.polyline(xs, ys, 3, w, ink);
                break;
            }
            case Icon::chevronRight:
            {
                const float xs[3] = { cx - s * 0.22f, cx + s * 0.28f, cx - s * 0.22f };
                const float ys[3] = { cy - s * 0.5f, cy, cy + s * 0.5f };
                c.polyline(xs, ys, 3, w, ink);
                break;
            }
            case Icon::arrowUp:
            {
                c.segment(cx, cy + s * 0.7f, cx, cy - s * 0.6f, w * 1.3f, ink);
                const float xs[3] = { cx - s * 0.55f, cx, cx + s * 0.55f };
                const float ys[3] = { cy - s * 0.05f, cy - s * 0.65f, cy - s * 0.05f };
                c.polyline(xs, ys, 3, w * 1.3f, ink);
                break;
            }
            case Icon::stop:
                c.rrect(cx - s * 0.45f, cy - s * 0.45f, s * 0.9f, s * 0.9f, s * 0.18f, ink);
                break;
            case Icon::bolt:
            {
                // A lightning bolt's outline: fast mode.
                const float xs[7] = { cx + s * 0.2f, cx - s * 0.45f, cx - s * 0.02f, cx - s * 0.2f,
                                      cx + s * 0.45f, cx + s * 0.02f, cx + s * 0.2f };
                const float ys[7] = { cy - s * 0.9f, cy + s * 0.12f, cy + s * 0.12f, cy + s * 0.9f,
                                      cy - s * 0.12f, cy - s * 0.12f, cy - s * 0.9f };
                c.polyline(xs, ys, 7, w, ink);
                break;
            }
            case Icon::copy:
                c.rrect(cx - s * 0.7f, cy - s * 0.7f, s * 1.0f, s * 1.0f, s * 0.18f, ink.withAlpha(0.0f), w, ink);
                c.rrect(cx - s * 0.3f, cy - s * 0.3f, s * 1.0f, s * 1.0f, s * 0.18f, ink.withAlpha(0.0f), w, ink);
                break;
            case Icon::check:
            {
                const float xs[3] = { cx - s * 0.6f, cx - s * 0.15f, cx + s * 0.65f };
                const float ys[3] = { cy + s * 0.02f, cy + s * 0.48f, cy - s * 0.5f };
                c.polyline(xs, ys, 3, w * 1.2f, ink);
                break;
            }
            case Icon::file:
                c.rrect(cx - s * 0.55f, cy - s * 0.75f, s * 1.1f, s * 1.5f, s * 0.16f, ink.withAlpha(0.0f), w, ink);
                c.segment(cx - s * 0.28f, cy - s * 0.25f, cx + s * 0.28f, cy - s * 0.25f, w, ink);
                c.segment(cx - s * 0.28f, cy + s * 0.1f, cx + s * 0.28f, cy + s * 0.1f, w, ink);
                c.segment(cx - s * 0.28f, cy + s * 0.45f, cx + s * 0.1f, cy + s * 0.45f, w, ink);
                break;
            case Icon::play:
            {
                const float xs[4] = { cx - s * 0.4f, cx + s * 0.6f, cx - s * 0.4f, cx - s * 0.4f };
                const float ys[4] = { cy - s * 0.6f, cy, cy + s * 0.6f, cy - s * 0.6f };
                c.polyline(xs, ys, 4, w, ink);
                break;
            }
            case Icon::dial:
                c.disc(cx, cy, s * 0.72f, ink.withAlpha(0.0f), w, ink);
                c.segment(cx, cy, cx + s * 0.42f, cy - s * 0.42f, w, ink);
                break;
            case Icon::wave:
            {
                float xs[9], ys[9];
                for (int k = 0; k < 9; ++k)
                {
                    const float t = static_cast<float>(k) / 8.0f;
                    xs[k] = cx - s * 0.8f + t * s * 1.6f;
                    ys[k] = cy - s * 0.5f * std::sin(t * 6.2831853f);
                }
                c.polyline(xs, ys, 9, w, ink);
                break;
            }
            case Icon::alert:
                c.disc(cx, cy, s * 0.78f, ink.withAlpha(0.0f), w, ink);
                c.segment(cx, cy - s * 0.42f, cx, cy + s * 0.1f, w * 1.2f, ink);
                c.disc(cx, cy + s * 0.38f, w * 0.75f, ink);
                break;
        }
    }
}
