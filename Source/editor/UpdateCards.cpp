// Source/editor/UpdateCards.cpp: see UpdateCards.h.
#include "editor/UpdateCards.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/Items.h"
#include "editor/SettingsPages.h"

#include "Product.h"

#include <algorithm>
#include <cmath>

namespace playground::ui
{
    namespace
    {
        constexpr float kPad = 22.0f;
        constexpr float kButtonH = 30.0f;
        constexpr float kLineH = 16.0f;
        constexpr float kCheckRowH = 26.0f;
        constexpr const char* kHost = "testingplayground.seanfunk.xyz";

        // The offer's words (the factory spec's card, with the update check's line).
        std::string offerText()
        {
            return std::string(product::kName) + " can get two things from " + kHost + ". Nothing about you is sent.";
        }
        constexpr const char* kFactoryLine = "The factory projects, kept up to date";
        constexpr const char* kUpdatesLine = "A note when a new version is out";
        constexpr const char* kOfferFoot = "Either can be changed in Settings, on the Network page.";

        std::vector<std::string> wrap(const std::string& s, const funkgui::TextStyle& st, float w)
        {
            return settings::Builder::wrapped(s, st, w);
        }

        // A card's frame: a soft shadow and the rounded card, the panel dimmed behind it when it is modal.
        void frame(funkgui::Canvas& c, const funkgui::Theme& th, const chat::Palette& p, const funkgui::Rect& box, float width,
                   float height, bool modal)
        {
            if (modal)
                c.rrect(0.0f, 0.0f, width, height, 0.0f, th.ground.withAlpha(0.68f));
            constexpr funkgui::Col black{ 0, 0, 0, 255 };
            for (int k = 3; k >= 1; --k)
            {
                const float g = static_cast<float>(k) * 3.0f;
                c.rrect(box.x - g, box.y - g + 6.0f, box.w + 2.0f * g, box.h + 2.0f * g, 16.0f + g, black.withAlpha(modal ? 0.07f : 0.10f));
            }
            c.rrect(box.x, box.y, box.w, box.h, 16.0f, p.card, 1.0f, modal ? p.cardEdge : funkgui::mix(p.cardEdge, p.accent, 0.45f));
        }

        void heading(funkgui::Canvas& c, const chat::Palette& p, float x, float y, chat::Icon icon, const std::string& title, float w)
        {
            c.rrect(x, y, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
            chat::drawIcon(c, icon, x + 15.0f, y + 15.0f, 15.0f, p.accent);
            c.text(fitText(c, title, chat::kH2, w - 42.0f).c_str(), x + 42.0f, c.capCentreTop(y + 15.0f, chat::kH2), chat::kH2, p.text);
        }

        void button(funkgui::Canvas& c, const chat::Palette& p, const funkgui::Rect& b, const char* label, bool primary, bool hover)
        {
            const funkgui::Col fill = primary ? (hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent) : (hover ? p.hover : p.card.withAlpha(0.0f));
            c.rrect(b.x, b.y, b.w, b.h, b.h * 0.5f, fill, primary ? 0.0f : 1.0f, p.composerEdge);
            const funkgui::TextStyle& ts = primary ? chat::kSmallStrong : chat::kSmall;
            c.text(label, b.centreX(), c.capCentreTop(b.centreY(), ts), ts, primary ? p.onAccent : (hover ? p.text : p.soft), funkgui::Align::centre);
        }

        float buttonW(const char* label, bool primary)
        {
            return std::ceil(28.0f + settings::Builder::textW(label, primary ? chat::kSmallStrong : chat::kSmall));
        }

        void a11yButton(std::vector<funkgui::A11yItem>& out, uint32_t id, uint32_t parent, const funkgui::Rect& r, std::string title)
        {
            funkgui::A11yItem b;
            b.id = id;
            b.parent = parent;
            b.role = funkgui::A11yRole::button;
            b.bounds = r;
            b.title = std::move(title);
            out.push_back(std::move(b));
        }
    }

    // ---- the offer ------------------------------------------------------------------------------------------------------

    void OfferCard::open()
    {
        factory_ = updates_ = true;
        open_ = true;
        ++revision_;
    }

    void OfferCard::close()
    {
        open_ = false;
        ++revision_;
    }

    std::string OfferCard::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    OfferCard::Places OfferCard::places(float width, float height) const
    {
        Places at;
        const float w = std::min(480.0f, width - 32.0f);
        const float textLines = static_cast<float>(wrap(offerText(), chat::kSmall, w - 2.0f * kPad).size());
        const float h = kPad + 30.0f + 12.0f + textLines * kLineH + 10.0f + 2.0f * kCheckRowH + 12.0f + kButtonH + 10.0f + kLineH + kPad - 6.0f;
        // Over the lower middle of the editor, clear of the status bar: never over the top bar's controls.
        at.box = { std::round((width - w) * 0.5f), std::round(std::max(70.0f, height - 52.0f - h)), w, std::round(h) };
        float y = at.box.y + kPad + 30.0f + 12.0f + textLines * kLineH + 10.0f;
        const float x = at.box.x + kPad;
        at.factory = { x, y, w - 2.0f * kPad, kCheckRowH };
        y += kCheckRowH;
        at.updates = { x, y, w - 2.0f * kPad, kCheckRowH };
        y += kCheckRowH + 12.0f;
        const float allowW = buttonW("Allow", true) + 12.0f, laterW = buttonW("Not now", false) + 8.0f;
        at.allow = { at.box.right() - kPad - allowW, y, allowW, kButtonH };
        at.notNow = { at.allow.x - 8.0f - laterW, y, laterW, kButtonH };
        return at;
    }

    void OfferCard::pointerDown(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        if (at.factory.contains(p))
            factory_ = !factory_;
        else if (at.updates.contains(p))
            updates_ = !updates_;
        else if (at.allow.contains(p))
        {
            port_.answerOffer(true, factory_, updates_);
            notice_ = factory_ && updates_ ? "THE FACTORY PROJECTS AND UPDATE NOTES ARE ON: SETTINGS, NETWORK CHANGES EITHER"
                    : factory_            ? "THE FACTORY PROJECTS ARE ON, UPDATE NOTES OFF: SETTINGS, NETWORK CHANGES EITHER"
                    : updates_            ? "UPDATE NOTES ARE ON, THE FACTORY PROJECTS OFF: SETTINGS, NETWORK CHANGES EITHER"
                                          : "NOTHING COMES FROM THE WEBSITE: SETTINGS, NETWORK CHANGES THAT";
            close();
        }
        else if (at.notNow.contains(p))
        {
            port_.answerOffer(false, false, false);
            notice_ = "NOT NOW: NOTHING COMES FROM THE WEBSITE UNTIL SETTINGS, NETWORK SAYS SO";
            close();
        }
        ++revision_;
    }

    funkgui::Cursor OfferCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        if (at.factory.contains(p) || at.updates.contains(p) || at.allow.contains(p) || at.notNow.contains(p))
            return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void OfferCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                         bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        frame(c, th, p, box, width, height, false);
        const float x = box.x + kPad;
        heading(c, p, x, box.y + kPad, chat::Icon::globe, "Use the website?", box.w - 2.0f * kPad);
        float y = box.y + kPad + 30.0f + 12.0f;
        for (const std::string& line : wrap(offerText(), chat::kSmall, box.w - 2.0f * kPad))
        {
            c.text(line.c_str(), x, c.capCentreTop(y + 8.0f, chat::kSmall), chat::kSmall, p.soft);
            y += kLineH;
        }
        const auto check = [&](const funkgui::Rect& r, bool on, const char* label) {
            const bool hover = hasPointer && r.contains(pointer);
            const float bx = r.x, by = r.centreY() - 8.0f;
            if (on)
            {
                c.rrect(bx, by, 16.0f, 16.0f, 4.0f, hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent);
                chat::drawIcon(c, chat::Icon::check, bx + 8.0f, by + 8.0f, 11.0f, p.onAccent);
            }
            else
                c.rrect(bx, by, 16.0f, 16.0f, 4.0f, hover ? p.hover : p.composer, 1.0f, funkgui::mix(p.composerEdge, p.text, 0.25f));
            c.text(fitText(c, label, chat::kBody, r.w - 28.0f).c_str(), bx + 26.0f, c.capCentreTop(r.centreY(), chat::kBody), chat::kBody,
                   p.text);
        };
        check(at.factory, factory_, kFactoryLine);
        check(at.updates, updates_, kUpdatesLine);
        c.text(fitText(c, kOfferFoot, chat::kSmall, at.notNow.x - x - 8.0f).c_str(), x, c.capCentreTop(at.allow.centreY(), chat::kSmall),
               chat::kSmall, p.faint);
        button(c, p, at.notNow, "Not now", false, hasPointer && at.notNow.contains(pointer));
        button(c, p, at.allow, "Allow", true, hasPointer && at.allow.contains(pointer));
    }

    void OfferCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kA11yBase;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Use the website?";
        box.description = offerText();
        out.push_back(box);
        const auto check = [&](uint32_t id, const funkgui::Rect& r, const char* title, bool on) {
            funkgui::A11yItem b;
            b.id = id;
            b.parent = kA11yBase;
            b.role = funkgui::A11yRole::toggleButton;
            b.bounds = r;
            b.title = title;
            b.checkable = true;
            b.checked = on;
            out.push_back(std::move(b));
        };
        check(kA11yBase + 1, at.factory, kFactoryLine, factory_);
        check(kA11yBase + 2, at.updates, kUpdatesLine, updates_);
        a11yButton(out, kA11yBase + 3, kA11yBase, at.allow, "Allow");
        a11yButton(out, kA11yBase + 4, kA11yBase, at.notNow, "Not now");
    }

    bool OfferCard::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height)
    {
        if (!open_ || (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus))
            return false;
        const Places at = places(width, height);
        const funkgui::Rect* r = id == kA11yBase + 1 ? &at.factory
                               : id == kA11yBase + 2 ? &at.updates
                               : id == kA11yBase + 3 ? &at.allow
                               : id == kA11yBase + 4 ? &at.notNow
                                                     : nullptr;
        if (r == nullptr)
            return false;
        if (a == funkgui::A11yAction::press)
            pointerDown({ r->centreX(), r->centreY() }, width, height);
        return true;
    }

    // ---- the update card ------------------------------------------------------------------------------------------------

    void UpdateCard::open(const UpdateState& state)
    {
        version_ = state.latest;
        current_ = state.current;
        date_ = state.date;
        summary_ = state.summary;
        open_ = true;
        ++revision_;
    }

    void UpdateCard::close()
    {
        open_ = false;
        ++revision_;
    }

    std::string UpdateCard::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    UpdateCard::Places UpdateCard::places(float width, float height) const
    {
        Places at;
        const float w = std::min(480.0f, width - 32.0f);
        at.summary = summary_.empty() ? std::vector<std::string>{} : wrap(summary_, chat::kBody, w - 2.0f * kPad);
        if (at.summary.size() > 5)
        {
            at.summary.resize(5);
            at.summary.back() += "...";
        }
        const float sumH = at.summary.empty() ? 0.0f : static_cast<float>(at.summary.size()) * 18.0f + 12.0f;
        const float h = kPad + 30.0f + 12.0f + kLineH + 12.0f + sumH + kLineH + 14.0f + kButtonH + kPad;
        at.box = { std::round((width - w) * 0.5f), std::round(std::max(16.0f, (height - h) * 0.42f)), w, std::round(h) };
        const float by = at.box.bottom() - kPad - kButtonH;
        const float dw = buttonW("Download", true) + 12.0f, sw = buttonW("Skip this version", false), lw = buttonW("Later", false) + 8.0f;
        at.download = { at.box.right() - kPad - dw, by, dw, kButtonH };
        at.later = { at.download.x - 8.0f - lw, by, lw, kButtonH };
        at.skip = { at.box.x + kPad, by, sw, kButtonH };
        return at;
    }

    void UpdateCard::download()
    {
        std::string why;
        notice_ = port_.openDownloadPage(why) ? "THE DOWNLOAD PAGE OPENED IN THE BROWSER" : "THE DOWNLOAD PAGE DID NOT OPEN: " + upper(why);
        close();
    }

    void UpdateCard::skip()
    {
        port_.skipUpdate(version_);
        notice_ = "VERSION " + upper(version_) + " SKIPPED: THE NEXT ONE WILL SHOW";
        close();
    }

    void UpdateCard::pointerDown(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        if (at.download.contains(p))
            download();
        else if (at.skip.contains(p))
            skip();
        else if (at.later.contains(p) || !at.box.contains(p))
            close();                                     // Later, or a press outside the card
        ++revision_;
    }

    void UpdateCard::key(const funkgui::KeyEvent& e, float, float)
    {
        if (e.key == funkgui::Key::escape)
            close();
        else if (e.key == funkgui::Key::enter)
            download();
        ++revision_;
    }

    funkgui::Cursor UpdateCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        if (at.download.contains(p) || at.skip.contains(p) || at.later.contains(p))
            return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void UpdateCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                          bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        frame(c, th, p, box, width, height, true);
        const float x = box.x + kPad, w = box.w - 2.0f * kPad;
        heading(c, p, x, box.y + kPad, chat::Icon::arrowUp, std::string(product::kName) + " " + version_ + " is out", w);
        float y = box.y + kPad + 30.0f + 12.0f;
        const std::string have = "You have " + current_ + "." + (date_.empty() ? std::string() : " Released " + date_ + ".");
        c.text(fitText(c, have, chat::kSmall, w).c_str(), x, c.capCentreTop(y + 8.0f, chat::kSmall), chat::kSmall, p.soft);
        y += kLineH + 12.0f;
        for (const std::string& line : at.summary)
        {
            c.text(fitText(c, line, chat::kBody, w).c_str(), x, c.capCentreTop(y + 9.0f, chat::kBody), chat::kBody, p.text);
            y += 18.0f;
        }
        if (!at.summary.empty())
            y += 12.0f;
        c.text(fitText(c, std::string("Download opens ") + kHost + "/download in the browser.", chat::kSmall, w).c_str(), x,
               c.capCentreTop(y + 8.0f, chat::kSmall), chat::kSmall, p.faint);
        button(c, p, at.skip, "Skip this version", false, hasPointer && at.skip.contains(pointer));
        button(c, p, at.later, "Later", false, hasPointer && at.later.contains(pointer));
        button(c, p, at.download, "Download", true, hasPointer && at.download.contains(pointer));
    }

    void UpdateCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kA11yBase;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = std::string(product::kName) + " " + version_ + " is out";
        box.description = "You have " + current_ + ". " + summary_;
        out.push_back(box);
        a11yButton(out, kA11yBase + 1, kA11yBase, at.download, "Download: opens the download page in the browser");
        a11yButton(out, kA11yBase + 2, kA11yBase, at.skip, "Skip this version");
        a11yButton(out, kA11yBase + 3, kA11yBase, at.later, "Later");
    }

    bool UpdateCard::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height)
    {
        if (!open_ || (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus))
            return false;
        const Places at = places(width, height);
        const funkgui::Rect* r = id == kA11yBase + 1 ? &at.download : id == kA11yBase + 2 ? &at.skip : id == kA11yBase + 3 ? &at.later : nullptr;
        if (r == nullptr)
            return false;
        if (a == funkgui::A11yAction::press)
            pointerDown({ r->centreX(), r->centreY() }, width, height);
        return true;
    }
}
