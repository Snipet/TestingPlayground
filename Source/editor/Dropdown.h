// Source/editor/Dropdown.h: what opens a menu, drawn one way, and the menus' rows, built one way.
//
// A trigger is every control whose click opens a menu of choices or commands: the program card's Examples and Library,
// the top bar's Projects (its card), the chat's model and thinking pills, the status bar's zoom, the Settings card's
// selects, a .gui interface's menu control and the Schematic view's More and probe chip. Each is a pill with its label
// at the left and a chevron at its right, in one of three bases (a toolbar's ghost, an outlined pill, a form's field),
// and every one has the same states:
//   - rest      the base's fill and edge, its label in `soft` (a field's in `text`)
//   - hover     `hover`'s fill, the label in `text`
//   - open      while its menu is open (the press that opened it, or the keys): the accent's tint over `hover` and an
//               accent edge, the chevron turned up; the Panel knows it from the menu's anchor (Panel::menuOpenAt)
//   - focus     the focus ring, 1.5 px of the accent 2 px outside the pill
//   - disabled  `faint`, and no hover
// The chevron sits kChevronInset from the right edge whatever the trigger's size, and the label stops kChevronRoom from
// it. The menus themselves are FunkGui's (MenuLook, 0.25.0: the canvas's palette, radii and face, in a native window):
// a menu of choices is a dropdown's, as wide as its trigger at least, its current choice a radio dot, highlighted and
// scrolled into view when it opens (`dropdownMenu`); a command that has a key shows it in the shortcut column
// (`command`); a long list has section headers (`header`). Header-only: the GUI sandbox draws a .gui's menu control too.
#pragma once

#include "editor/ChatStyle.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/HostServices.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace playground::ui::dropdown
{
    enum class Base : uint8_t
    {
        ghost,                                           // a toolbar's: nothing drawn at rest
        outlined,                                        // a hairline edge at rest (the top bar's Projects)
        field                                            // a form's: the composer's fill and edge (Settings, a .gui's menu)
    };

    struct State
    {
        bool enabled = true;
        bool hover = false;
        bool open = false;                               // its menu is open
        bool focus = false;                              // the focus ring shows on it
    };

    inline constexpr float kChevronPx = 8.0f;            // the chevron's box
    inline constexpr float kChevronInset = 14.0f;        // its centre from the trigger's right edge
    inline constexpr float kChevronRoom = 22.0f;         // the label's room ends this far from the right edge
    inline constexpr float kRingGap = 2.0f;
    inline constexpr float kRingPx = 1.5f;
    inline constexpr float kOpenTint = 0.10f;            // the accent over `hover` while open

    // A trigger's colours in a state.
    struct Look
    {
        funkgui::Col fill, edge, ink;
        float edgePx = 0.0f;
    };

    // `tint`: a .gui control's colour (its edge at rest), or none (alpha 0).
    inline Look look(const chat::Palette& p, Base base, State s, funkgui::Col tint = {})
    {
        Look k;
        k.fill = p.card.withAlpha(0.0f);
        k.edge = p.card.withAlpha(0.0f);
        k.ink = base == Base::field ? p.text : p.soft;
        if (base == Base::field)
        {
            k.fill = p.composer;
            k.edge = tint.a > 0 ? tint : p.composerEdge;
            k.edgePx = 1.0f;
        }
        else if (base == Base::outlined)
        {
            k.edge = p.composerEdge;
            k.edgePx = 1.0f;
        }
        if (!s.enabled)
        {
            k.ink = p.faint;
            return k;
        }
        if (s.open)
        {
            k.fill = funkgui::mix(p.hover, p.accent, kOpenTint);
            k.edge = p.accent;
            k.edgePx = 1.0f;
            k.ink = p.text;
        }
        else if (s.hover)
        {
            k.fill = p.hover;
            k.ink = p.text;
        }
        return k;
    }

    inline void drawChevron(funkgui::Canvas& c, const funkgui::Rect& r, bool open, funkgui::Col ink)
    {
        chat::drawIcon(c, open ? chat::Icon::chevronUp : chat::Icon::chevronDown, r.right() - kChevronInset,
                       r.centreY() + (open ? -0.5f : 0.5f), kChevronPx, ink);
    }

    inline void drawRing(funkgui::Canvas& c, const chat::Palette& p, const funkgui::Rect& r, float radius)
    {
        c.rrect(r.x - kRingGap, r.y - kRingGap, r.w + 2.0f * kRingGap, r.h + 2.0f * kRingGap, radius + kRingGap,
                p.card.withAlpha(0.0f), kRingPx, p.accent);
    }

    // The pill, the chevron and the ring; the label is the caller's (its text style is the context's), in the ink this
    // returns, at the left of `labelRoom`.
    inline funkgui::Col drawTrigger(funkgui::Canvas& c, const chat::Palette& p, const funkgui::Rect& r, Base base, State s,
                                    funkgui::Col tint = {})
    {
        const Look k = look(p, base, s, tint);
        const float radius = r.h * 0.5f;
        c.rrect(r.x, r.y, r.w, r.h, radius, k.fill, k.edgePx, k.edge);
        if (s.focus)
            drawRing(c, p, r, radius);
        drawChevron(c, r, s.open, s.enabled ? (s.open || s.hover ? p.text : p.soft) : p.faint);
        return k.ink;
    }

    // The label's room in a trigger whose label starts at `x`.
    inline float labelRoom(const funkgui::Rect& r, float x) { return r.right() - kChevronRoom - x; }

    // ---- the menus' rows ----------------------------------------------------------------------------------------------

    inline funkgui::MenuItem header(std::string title)
    {
        funkgui::MenuItem h;
        h.label = std::move(title);
        h.header = true;
        return h;
    }

    inline funkgui::MenuItem separator()
    {
        funkgui::MenuItem s;
        s.separator = true;
        return s;
    }

    // One choice of a set: the current one has the dot.
    inline funkgui::MenuItem choice(int id, std::string label, bool current, bool enabled = true)
    {
        funkgui::MenuItem it{ id, std::move(label), enabled, current };
        it.radio = true;
        return it;
    }

    // A command whose text is "Label   Key" (the key after three spaces, as the panel writes it): the label, and the key
    // in the shortcut column. The caller gives the text as the user's keyboard names it (EditorPort.h's shortcutText:
    // Ctrl- on Windows); a text with no key is the label alone.
    inline funkgui::MenuItem command(int id, std::string_view text, bool enabled = true)
    {
        funkgui::MenuItem it{ id, std::string(text), enabled };
        if (const std::size_t cut = text.rfind("   "); cut != std::string_view::npos && cut > 0)
        {
            std::size_t key = cut + 3;
            while (key < text.size() && text[key] == ' ')
                ++key;
            it.label = std::string(text.substr(0, cut));
            it.shortcut = std::string(text.substr(key));
        }
        return it;
    }

    // A dropdown's menu: under its trigger, at least as wide, the current choice (its id, 0 for none) highlighted and
    // scrolled into view when it opens.
    inline void dropdownMenu(funkgui::MenuRequest& request, const funkgui::Rect& trigger, int currentId)
    {
        request.anchor = trigger;
        request.minWidth = trigger.w;
        request.selectedId = currentId;
    }
}
