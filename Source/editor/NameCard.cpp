// Source/editor/NameCard.cpp: see NameCard.h.
#include "editor/NameCard.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/Items.h"

#include "Product.h"

#include <algorithm>
#include <cmath>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 460.0f, kBoxH = 236.0f;
        constexpr float kPad = 24.0f;
        constexpr float kButtonH = 32.0f;
        constexpr uint32_t kBoxId = NameCard::kA11yBase, kFieldId = kBoxId + 1, kSaveId = kBoxId + 2, kCancelId = kBoxId + 3;
        const char* const kThing = product::kIsInstrument ? "synth" : "effect";
    }

    void NameCard::open()
    {
        const DesktopState d = port_.desktop();
        number_ = d.number;
        field_.set(d.name == d.number ? std::string() : d.name, kMaxLength);
        why_.clear();
        open_ = true;
        ++revision_;
    }

    void NameCard::close()
    {
        open_ = false;
        why_.clear();
        ++revision_;
    }

    void NameCard::save()
    {
        std::string why;
        if (!port_.setDesktopName(field_.buffer, why))
        {
            why_ = "Not named: " + why + ".";
            ++revision_;
            return;
        }
        notice_ = "NAMED: " + upper(port_.desktopName());
        close();
    }

    std::string NameCard::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    NameCard::Places NameCard::places(float width, float height) const
    {
        Places at;
        const float w = std::min(kBoxW, width - 32.0f);
        at.box = { std::round((width - w) * 0.5f), std::round((height - kBoxH) * 0.42f), w, kBoxH };
        at.field = { at.box.x + kPad, at.box.y + 112.0f, w - 2.0f * kPad, 34.0f };
        const float by = at.box.bottom() - kPad - kButtonH;
        at.save = { at.box.right() - kPad - 96.0f, by, 96.0f, kButtonH };
        at.cancel = { at.save.x - 8.0f - 84.0f, by, 84.0f, kButtonH };
        return at;
    }

    void NameCard::pointerDown(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        if (at.save.contains(p))
            save();
        else if (at.cancel.contains(p))
            close();
        else if (at.field.contains(p))
        {
            field_.allSelected = false;
            field_.caret = static_cast<int>(field_.buffer.size());
        }
        ++revision_;
    }

    void NameCard::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float, float)
    {
        using funkgui::Key;
        if (e.key == Key::enter)
            save();
        else if (e.key == Key::escape)
            close();
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host != nullptr && field_.paste(host->clipboardText(), kMaxLength))
                why_.clear();
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
            field_.allSelected = !field_.buffer.empty();
        else if (field_.key(e, kMaxLength))
            why_.clear();
        ++revision_;
    }

    funkgui::Cursor NameCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        if (at.save.contains(p) || at.cancel.contains(p))
            return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void NameCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                        bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        c.rrect(0.0f, 0.0f, width, height, 0.0f, th.ground.withAlpha(0.68f));
        constexpr funkgui::Col black{ 0, 0, 0, 255 };
        for (int k = 3; k >= 1; --k)                     // a soft shadow under the card
        {
            const float g = static_cast<float>(k) * 3.0f;
            c.rrect(box.x - g, box.y - g + 6.0f, box.w + 2.0f * g, box.h + 2.0f * g, 16.0f + g, black.withAlpha(0.07f));
        }
        c.rrect(box.x, box.y, box.w, box.h, 16.0f, p.card, 1.0f, p.cardEdge);

        const float x = box.x + kPad;
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::pencil, x + 15.0f, box.y + 37.0f, 15.0f, p.accent);
        c.text((std::string("Name this ") + kThing).c_str(), x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        const std::string lines[] = { "Claude Desktop and the plugin call it by this name; its number",
                                      "(" + number_ + ") still reaches it. Kept with the session; empty: " + number_ + "." };
        float y = box.y + 66.0f;
        for (const std::string& line : lines)
        {
            c.text(fitText(c, line, chat::kSmall, box.w - 2.0f * kPad).c_str(), x, c.capCentreTop(y + 8.0f, chat::kSmall), chat::kSmall,
                   p.soft);
            y += 16.0f;
        }

        // The field: the name, selected while it is as it was; the caret after what is typed.
        const funkgui::Rect& r = at.field;
        c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.composer, 1.0f, funkgui::mix(p.composerEdge, p.accent, 0.6f));
        const funkgui::TextStyle& st = chat::kBody;
        const float top = c.capCentreTop(r.centreY(), st);
        const float maxW = r.w - 28.0f;
        if (field_.buffer.empty())
            c.text(number_.c_str(), r.x + 14.0f, top, st, p.faint);
        else
        {
            if (field_.allSelected)
                c.rrect(r.x + 12.0f, r.y + 8.0f, std::min(c.textWidth(field_.buffer.c_str(), st), maxW) + 4.0f, r.h - 16.0f, 3.0f, th.accentDim);
            c.text(fitText(c, field_.buffer, st, maxW).c_str(), r.x + 14.0f, top, st, p.text);
        }
        const std::string before = field_.buffer.substr(0, static_cast<std::size_t>(std::clamp(field_.caret, 0, static_cast<int>(field_.buffer.size()))));
        c.rrect(r.x + 14.0f + std::min(c.textWidth(before.c_str(), st), maxW), r.y + 9.0f, 1.5f, r.h - 18.0f, 0.75f, p.accent);
        if (!why_.empty())
            c.text(fitText(c, why_, chat::kSmall, box.w - 2.0f * kPad).c_str(), x + 2.0f, c.capCentreTop(r.bottom() + 14.0f, chat::kSmall),
                   chat::kSmall, p.danger);

        const auto button = [&](const funkgui::Rect& b, const char* label, bool primary) {
            const bool hover = hasPointer && b.contains(pointer);
            const funkgui::Col fill = primary ? (hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent) : (hover ? p.hover : p.card.withAlpha(0.0f));
            c.rrect(b.x, b.y, b.w, b.h, b.h * 0.5f, fill, primary ? 0.0f : 1.0f, p.composerEdge);
            const funkgui::TextStyle& ts = primary ? chat::kSmallStrong : chat::kSmall;
            c.text(label, b.centreX(), c.capCentreTop(b.centreY(), ts), ts, primary ? p.onAccent : (hover ? p.text : p.soft), funkgui::Align::centre);
        };
        button(at.cancel, "Cancel", false);
        button(at.save, "Save", true);
    }

    void NameCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = std::string("Name this ") + kThing;
        box.description = why_;
        out.push_back(box);
        funkgui::A11yItem field;
        field.id = kFieldId;
        field.parent = kBoxId;
        field.role = funkgui::a11yrole::editableText;
        field.bounds = at.field;
        field.title = "Name";
        field.value = field_.buffer;
        field.help = "Empty: " + number_;
        out.push_back(field);
        const auto button = [&](uint32_t id, const funkgui::Rect& r, const char* title) {
            funkgui::A11yItem b;
            b.id = id;
            b.parent = kBoxId;
            b.role = funkgui::A11yRole::button;
            b.bounds = r;
            b.title = title;
            out.push_back(std::move(b));
        };
        button(kSaveId, at.save, "Save");
        button(kCancelId, at.cancel, "Cancel");
    }

    bool NameCard::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height)
    {
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus)
            return false;
        const Places at = places(width, height);
        if (id == kSaveId)
            pointerDown({ at.save.centreX(), at.save.centreY() }, width, height);
        else if (id == kCancelId)
            pointerDown({ at.cancel.centreX(), at.cancel.centreY() }, width, height);
        else if (id == kFieldId)
            pointerDown({ at.field.centreX(), at.field.centreY() }, width, height);
        else
            return false;
        return true;
    }
}
