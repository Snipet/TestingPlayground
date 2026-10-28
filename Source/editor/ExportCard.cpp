// Source/editor/ExportCard.cpp: see ExportCard.h.
#include "editor/ExportCard.h"

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
        constexpr float kBoxW = 760.0f, kBoxH = 620.0f;
        constexpr float kPad = 24.0f;
        constexpr float kCaption = 18.0f;                // a caption over its field
        constexpr float kGap = 14.0f;                    // between the parts
        constexpr float kPillH = 26.0f;
        constexpr float kButtonH = 32.0f;
        constexpr float kLineH = 18.0f;                  // the description's lines
        constexpr uint32_t kBoxId = ExportCard::kA11yBase, kDescriptionId = kBoxId + 1, kAuthorId = kBoxId + 2,
                           kTagsId = kBoxId + 3, kVersionsId = kBoxId + 4, kDoneId = kBoxId + 5, kSourceId = kBoxId + 6,
                           kPackageId = kBoxId + 7, kCategoryId = kBoxId + 0x100, kGenreId = kBoxId + 0x200;

        float pillW(const std::string& label) { return std::round(7.2f * static_cast<float>(label.size()) + 26.0f); }

        std::string joined(const std::vector<std::string>& v)
        {
            std::string out;
            for (const std::string& s : v)
                out += (out.empty() ? "" : ", ") + s;
            return out;
        }

        // "warm, analog,, slow attack" as words (the store tidies them further).
        std::vector<std::string> split(const std::string& s)
        {
            std::vector<std::string> out;
            std::size_t at = 0;
            while (at <= s.size())
            {
                const std::size_t end = std::min(s.find(',', at), s.size());
                std::string w = s.substr(at, end - at);
                while (!w.empty() && w.front() == ' ')
                    w.erase(w.begin());
                while (!w.empty() && w.back() == ' ')
                    w.pop_back();
                if (!w.empty())
                    out.push_back(std::move(w));
                at = end + 1;
            }
            return out;
        }

        bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

        // The description's lines at `maxW`: each line's first byte (words kept whole while they fit).
        std::vector<std::size_t> wrap(const funkgui::Canvas& c, const std::string& s, const funkgui::TextStyle& st, float maxW)
        {
            std::vector<std::size_t> starts{ 0 };
            std::size_t start = 0;
            while (start < s.size())
            {
                std::size_t end = start, lastSpace = std::string::npos;
                while (end < s.size() && c.textWidth(s.substr(start, end - start + 1).c_str(), st) <= maxW)
                {
                    if (s[end] == ' ')
                        lastSpace = end;
                    ++end;
                }
                if (end >= s.size())
                    break;
                const std::size_t next = lastSpace != std::string::npos && lastSpace > start ? lastSpace + 1 : std::max(end, start + 1);
                starts.push_back(next);
                start = next;
            }
            return starts;
        }
    }

    std::vector<std::string> ExportCard::categories()
    {
        std::vector<std::string> out;
        if constexpr (product::kIsInstrument)
            for (const char* c : kInstrumentCategories)
                out.emplace_back(c);
        else
            for (const char* c : kEffectCategories)
                out.emplace_back(c);
        return out;
    }

    void ExportCard::open(const std::string& project)
    {
        open_ = true;
        project_ = project;
        saved_ = port_.projectInfo(project);
        description_.set(saved_.description, kDescriptionLength);
        author_.set(saved_.author, kAuthorLength);
        tags_.set(joined(saved_.tags), kTagsLength);
        for (funkgui::text::LineEdit* f : { &description_, &author_, &tags_ })
            f->allSelected = false;
        category_ = saved_.category;
        genres_ = saved_.genres;
        versions_ = latest_ = presets_ = 0;
        for (const ProjectEntry& e : port_.projects())
            if (e.name == project)
            {
                versions_ = e.versions;
                latest_ = e.latest;
                presets_ = e.presets;
            }
        allVersions_ = true;
        focus_ = Field::description;
        want_ = Want::none;
        why_.clear();
        failed_ = false;
        ++revision_;
    }

    void ExportCard::close()
    {
        const bool changed = !(info() == saved_);
        if (!keepInfo())
            return;                                      // why_ says why; the card stays
        if (changed)
            notice_ = "INFO KEPT: " + upper(project_);
        open_ = false;
        ++revision_;
    }

    ProjectInfo ExportCard::info() const
    {
        return { description_.buffer, author_.buffer, category_, split(tags_.buffer), genres_ };
    }

    bool ExportCard::keepInfo()
    {
        const ProjectInfo now = info();
        if (now == saved_)
            return true;
        std::string why;
        if (!port_.setProjectInfo(project_, now, why))
        {
            why_ = "Not kept: " + why + ".";
            failed_ = true;
            ++revision_;
            return false;
        }
        saved_ = port_.projectInfo(project_);
        return true;
    }

    void ExportCard::ask(Want w)
    {
        if (!keepInfo())
            return;
        want_ = w;
        why_ = "Choose where it goes.";
        failed_ = false;
        ++revision_;
    }

    ExportCard::Want ExportCard::takeWant()
    {
        const Want w = want_;
        want_ = Want::none;
        return w;
    }

    void ExportCard::exported(bool ok, const std::string& line)
    {
        why_ = line;
        failed_ = !ok;
        ++revision_;
    }

    std::string ExportCard::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    funkgui::text::LineEdit& ExportCard::field(Field f)
    {
        return f == Field::description ? description_ : f == Field::author ? author_ : tags_;
    }

    const funkgui::text::LineEdit& ExportCard::field(Field f) const
    {
        return f == Field::description ? description_ : f == Field::author ? author_ : tags_;
    }

    void ExportCard::focusOn(Field f)
    {
        focus_ = f;
        funkgui::text::LineEdit& e = field(f);
        e.allSelected = false;
        e.caret = static_cast<int>(e.buffer.size());
        ++revision_;
    }

    ExportCard::Places ExportCard::places(float width, float height) const
    {
        const float w = std::min(kBoxW, width - 32.0f), h = std::min(kBoxH, height - 32.0f);
        const funkgui::Rect box{ std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        const float x0 = box.x + kPad, inner = w - 2.0f * kPad;
        const auto flow = [&](const std::vector<std::string>& labels, float y, std::vector<funkgui::Rect>& out) {
            float x = x0, row = y;
            for (const std::string& l : labels)
            {
                const float pw = pillW(l);
                if (x + pw > x0 + inner && x > x0)
                {
                    x = x0;
                    row += kPillH + 6.0f;
                }
                out.push_back({ x, row, pw, kPillH });
                x += pw + 6.0f;
            }
            return row + kPillH;
        };
        std::vector<std::string> genres;
        for (const char* g : kGenres)
            genres.emplace_back(g);
        Places at;
        for (int lines = 4; lines >= 1; --lines)
        {
            at = {};
            at.box = box;
            at.descriptionLines = lines;
            float y = box.y + 84.0f + kCaption;
            at.description = { x0, y, inner, static_cast<float>(lines) * kLineH + 16.0f };
            y = at.description.bottom() + kGap + kCaption;
            const float authorW = std::round(inner * 0.36f);
            at.author = { x0, y, authorW, 34.0f };
            at.tags = { x0 + authorW + 12.0f, y, inner - authorW - 12.0f, 34.0f };
            y = at.author.bottom() + kGap + kCaption;
            y = flow(categories(), y, at.categories) + kGap + kCaption;
            y = flow(genres, y, at.genres) + kGap;
            at.versions = { x0, y, 230.0f, kPillH };
            const float by = box.bottom() - kPad - kButtonH;
            const float packW = pillW("Export package (.fmpack)"), sourceW = pillW("Export source (.zip)");
            at.package = { box.right() - kPad - packW, by, packW, kButtonH };
            at.source = { at.package.x - 8.0f - sourceW, by, sourceW, kButtonH };
            at.done = { x0, by, 84.0f, kButtonH };
            if (at.versions.bottom() + 26.0f <= by || lines == 1)
                break;
        }
        return at;
    }

    void ExportCard::pointerDown(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        if (at.description.contains(p))
            focusOn(Field::description);
        else if (at.author.contains(p))
            focusOn(Field::author);
        else if (at.tags.contains(p))
            focusOn(Field::tags);
        else if (at.versions.contains(p))
            allVersions_ = !allVersions_;
        else if (at.done.contains(p))
            close();
        else if (at.source.contains(p))
            ask(Want::source);
        else if (at.package.contains(p))
            ask(Want::package);
        else
        {
            const std::vector<std::string> cats = categories();
            for (std::size_t i = 0; i < at.categories.size(); ++i)
                if (at.categories[i].contains(p))
                    category_ = category_ == cats[i] ? std::string() : cats[i];
            for (std::size_t i = 0; i < at.genres.size(); ++i)
                if (at.genres[i].contains(p))
                {
                    const std::string g = kGenres[i];
                    if (has(genres_, g))
                        genres_.erase(std::find(genres_.begin(), genres_.end(), g));
                    else
                        genres_.push_back(g);
                }
        }
        ++revision_;
    }

    void ExportCard::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float /*width*/, float /*height*/)
    {
        using funkgui::Key;
        const int most = focus_ == Field::description ? kDescriptionLength : focus_ == Field::author ? kAuthorLength : kTagsLength;
        funkgui::text::LineEdit& f = field(focus_);
        if (e.key == Key::escape)
            close();
        else if (e.key == Key::tab || e.key == Key::enter)
        {
            const int step = e.key == Key::tab && e.mods.shift ? 2 : 1;
            if (e.key == Key::enter && focus_ == Field::tags)
                return;
            focusOn(static_cast<Field>((static_cast<int>(focus_) + step) % 3));
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host != nullptr)
                f.paste(host->clipboardText(), most);
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
            f.allSelected = !f.buffer.empty();
        else
            f.key(e, most);
        ++revision_;
    }

    funkgui::Cursor ExportCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        if (at.versions.contains(p) || at.done.contains(p) || at.source.contains(p) || at.package.contains(p))
            return funkgui::Cursor::pointingHand;
        for (const auto* list : { &at.categories, &at.genres })
            for (const funkgui::Rect& r : *list)
                if (r.contains(p))
                    return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void ExportCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                          bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        c.rrect(0.0f, 0.0f, width, height, 0.0f, th.ground.withAlpha(0.68f));
        constexpr funkgui::Col black{ 0, 0, 0, 255 };
        for (int k = 3; k >= 1; --k)
        {
            const float g = static_cast<float>(k) * 3.0f;
            c.rrect(box.x - g, box.y - g + 6.0f, box.w + 2.0f * g, box.h + 2.0f * g, 16.0f + g, black.withAlpha(0.07f));
        }
        c.rrect(box.x, box.y, box.w, box.h, 16.0f, p.card, 1.0f, p.cardEdge);

        // The title: the project, its versions and presets; what the card is for.
        const float x = box.x + kPad;
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::arrowUp, x + 15.0f, box.y + 37.0f, 15.0f, p.accent);
        const std::string title = "Export " + asciiText(project_);
        c.text(fitText(c, title, chat::kH2, box.w * 0.6f).c_str(), x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        const std::string facts = "v" + std::to_string(latest_) + "   " + std::to_string(versions_) + (versions_ == 1 ? " version   " : " versions   ")
                                + std::to_string(presets_) + (presets_ == 1 ? " preset" : " presets");
        c.text(facts.c_str(), box.right() - kPad, c.capCentreTop(box.y + 37.0f, chat::kSmall), chat::kSmall, p.faint, funkgui::Align::right);
        c.text(fitText(c, "What it is goes with it: a description, tags and genres. Done keeps them with the project.", chat::kSmall,
                       box.w - 2.0f * kPad).c_str(),
               x, c.capCentreTop(box.y + 62.0f, chat::kSmall), chat::kSmall, p.soft);

        const auto caption = [&](const funkgui::Rect& r, const char* text) {
            c.text(text, r.x + 2.0f, c.capCentreTop(r.y - 10.0f, chat::kSmallStrong), chat::kSmallStrong, p.faint);
        };
        const auto box3 = [&](const funkgui::Rect& r, bool focused) {
            c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.composer, 1.0f, focused ? funkgui::mix(p.composerEdge, p.accent, 0.6f) : p.composerEdge);
        };

        // The description: its lines wrapped, the caret on its line.
        {
            const funkgui::Rect& r = at.description;
            caption(r, "DESCRIPTION");
            box3(r, focus_ == Field::description);
            const funkgui::TextStyle& st = chat::kBody;
            const float maxW = r.w - 28.0f;
            const std::string& s = description_.buffer;
            const std::vector<std::size_t> starts = wrap(c, s, st, maxW);
            const std::size_t caret = static_cast<std::size_t>(std::clamp(description_.caret, 0, static_cast<int>(s.size())));
            std::size_t caretLine = 0;
            for (std::size_t i = 0; i < starts.size(); ++i)
                if (starts[i] <= caret)
                    caretLine = i;
            const std::size_t lines = static_cast<std::size_t>(at.descriptionLines);
            const std::size_t first = caretLine >= lines ? caretLine - lines + 1 : 0;   // the caret's line in view
            if (s.empty())
                c.text("What it is, how it sounds, how to play it", r.x + 14.0f, c.capCentreTop(r.y + 8.0f + 0.5f * kLineH, st), st, p.faint);
            for (std::size_t i = first; i < starts.size() && i < first + lines; ++i)
            {
                const std::size_t end = i + 1 < starts.size() ? starts[i + 1] : s.size();
                const float ly = r.y + 8.0f + static_cast<float>(i - first) * kLineH;
                c.text(s.substr(starts[i], end - starts[i]).c_str(), r.x + 14.0f, c.capCentreTop(ly + 0.5f * kLineH, st), st, p.text);
            }
            if (focus_ == Field::description)
            {
                const float ly = r.y + 8.0f + static_cast<float>(caretLine - first) * kLineH;
                const float cx = r.x + 14.0f + c.textWidth(s.substr(starts[caretLine], caret - starts[caretLine]).c_str(), st);
                c.rrect(cx, ly + 2.0f, 1.5f, kLineH - 4.0f, 0.75f, p.accent);
            }
        }
        // Author and tags: one line each.
        const auto line = [&](const funkgui::Rect& r, const char* cap, const funkgui::text::LineEdit& e, bool focused, const char* hint) {
            caption(r, cap);
            box3(r, focused);
            const funkgui::TextStyle& st = chat::kBody;
            const float maxW = r.w - 28.0f;
            const float top = c.capCentreTop(r.centreY(), st);
            if (e.buffer.empty())
                c.text(hint, r.x + 14.0f, top, st, p.faint);
            else
            {
                if (e.allSelected && focused)
                    c.rrect(r.x + 12.0f, r.y + 8.0f, std::min(c.textWidth(e.buffer.c_str(), st), maxW) + 4.0f, r.h - 16.0f, 3.0f, th.accentDim);
                c.text(fitText(c, e.buffer, st, maxW).c_str(), r.x + 14.0f, top, st, p.text);
            }
            if (focused)
            {
                const std::string before = e.buffer.substr(0, static_cast<std::size_t>(std::clamp(e.caret, 0, static_cast<int>(e.buffer.size()))));
                const float cx = r.x + 14.0f + std::min(c.textWidth(before.c_str(), st), maxW);
                c.rrect(cx, r.y + 9.0f, 1.5f, r.h - 18.0f, 0.75f, p.accent);
            }
        };
        line(at.author, "AUTHOR", author_, focus_ == Field::author, "Your name");
        line(at.tags, "TAGS (COMMAS BETWEEN)", tags_, focus_ == Field::tags, "warm, analog, slow attack");

        // The pills: the category (one), the genres (any), and the versions' switch.
        const auto pill = [&](const funkgui::Rect& r, const std::string& label, bool on) {
            const bool hover = hasPointer && r.contains(pointer);
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, on ? p.accent : (hover ? p.hover : p.composer), 1.0f, on ? p.accent : p.composerEdge);
            c.text(label.c_str(), r.centreX(), c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, on ? p.onAccent : p.soft,
                   funkgui::Align::centre);
        };
        const std::vector<std::string> cats = categories();
        if (!at.categories.empty())
            caption(at.categories.front(), "CATEGORY");
        for (std::size_t i = 0; i < at.categories.size(); ++i)
            pill(at.categories[i], cats[i], category_ == cats[i]);
        if (!at.genres.empty())
            caption(at.genres.front(), "GENRES");
        for (std::size_t i = 0; i < at.genres.size(); ++i)
            pill(at.genres[i], kGenres[i], has(genres_, kGenres[i]));
        {
            const funkgui::Rect& r = at.versions;
            const std::string label = allVersions_ ? "Every version (" + std::to_string(versions_) + ")"
                                                   : "The newest only (v" + std::to_string(latest_) + ")";
            const bool hover = hasPointer && r.contains(pointer);
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? p.hover : p.composer, 1.0f, p.composerEdge);
            c.rrect(r.x + 8.0f, r.y + 6.0f, 14.0f, 14.0f, 4.0f, allVersions_ ? p.accent : p.card, 1.0f, allVersions_ ? p.accent : p.composerEdge);
            if (allVersions_)
                chat::drawIcon(c, chat::Icon::check, r.x + 15.0f, r.centreY(), 10.0f, p.onAccent);
            c.text(label.c_str(), r.x + 30.0f, c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, p.soft);
        }
        if (!why_.empty())
            c.text(fitText(c, why_, chat::kSmall, box.w - 2.0f * kPad).c_str(), x + 2.0f, c.capCentreTop(at.done.y - 16.0f, chat::kSmall),
                   chat::kSmall, failed_ ? p.danger : p.soft);

        // The buttons.
        const auto button = [&](const funkgui::Rect& r, const char* label, bool primary) {
            const bool hover = hasPointer && r.contains(pointer);
            const funkgui::Col fill = primary ? (hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent) : (hover ? p.hover : p.card.withAlpha(0.0f));
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, fill, primary ? 0.0f : 1.0f, p.composerEdge);
            const funkgui::TextStyle& st = primary ? chat::kSmallStrong : chat::kSmall;
            c.text(label, r.centreX(), c.capCentreTop(r.centreY(), st), st, primary ? p.onAccent : (hover ? p.text : p.soft), funkgui::Align::centre);
        };
        button(at.done, "Done", false);
        button(at.source, "Export source (.zip)", false);
        button(at.package, "Export package (.fmpack)", true);
    }

    void ExportCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Export " + project_;
        box.description = why_;
        out.push_back(box);
        const auto text = [&](uint32_t id, const funkgui::Rect& r, const char* title, const funkgui::text::LineEdit& e) {
            funkgui::A11yItem it;
            it.id = id;
            it.parent = kBoxId;
            it.role = funkgui::a11yrole::editableText;
            it.bounds = r;
            it.title = title;
            it.value = e.buffer;
            out.push_back(std::move(it));
        };
        text(kDescriptionId, at.description, "Description", description_);
        text(kAuthorId, at.author, "Author", author_);
        text(kTagsId, at.tags, "Tags, commas between", tags_);
        const auto toggle = [&](uint32_t id, const funkgui::Rect& r, const std::string& title, bool on, const char* help) {
            funkgui::A11yItem it;
            it.id = id;
            it.parent = kBoxId;
            it.role = funkgui::A11yRole::toggleButton;
            it.bounds = r;
            it.title = title;
            it.checkable = true;
            it.checked = on;
            it.help = help;
            out.push_back(std::move(it));
        };
        const std::vector<std::string> cats = categories();
        for (std::size_t i = 0; i < at.categories.size(); ++i)
            toggle(kCategoryId + static_cast<uint32_t>(i), at.categories[i], cats[i], category_ == cats[i], "Category");
        for (std::size_t i = 0; i < at.genres.size(); ++i)
            toggle(kGenreId + static_cast<uint32_t>(i), at.genres[i], kGenres[i], has(genres_, kGenres[i]), "Genre");
        toggle(kVersionsId, at.versions, "Every version", allVersions_, "Else the newest only");
        const auto button = [&](uint32_t id, const funkgui::Rect& r, const char* title) {
            funkgui::A11yItem b;
            b.id = id;
            b.parent = kBoxId;
            b.role = funkgui::A11yRole::button;
            b.bounds = r;
            b.title = title;
            out.push_back(std::move(b));
        };
        button(kDoneId, at.done, "Done");
        button(kSourceId, at.source, "Export source (.zip)");
        button(kPackageId, at.package, "Export package (.fmpack)");
    }

    bool ExportCard::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height)
    {
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus)
            return false;
        const Places at = places(width, height);
        const auto at_ = [](const funkgui::Rect& r) { return funkgui::Point{ r.centreX(), r.centreY() }; };
        if (id == kDescriptionId)
            focusOn(Field::description);
        else if (id == kAuthorId)
            focusOn(Field::author);
        else if (id == kTagsId)
            focusOn(Field::tags);
        else if (id == kVersionsId || id == kDoneId || id == kSourceId || id == kPackageId)
            pointerDown(at_(id == kVersionsId ? at.versions : id == kDoneId ? at.done : id == kSourceId ? at.source : at.package), width, height);
        else if (id >= kCategoryId && id < kCategoryId + at.categories.size())
            pointerDown(at_(at.categories[id - kCategoryId]), width, height);
        else if (id >= kGenreId && id < kGenreId + at.genres.size())
            pointerDown(at_(at.genres[id - kGenreId]), width, height);
        else
            return false;
        return true;
    }
}
