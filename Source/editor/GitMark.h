// Source/editor/GitMark.h: the mark of a project linked to a git repository (editor/GitCard.h; docs/plan/05-git.md):
// a small branch glyph after the project's name, in PROJECTS' rows, the explorer's PROJECTS rows and the top bar's
// project pill, with an up arrow and the versions to push, a down arrow and the commits to pull, when they are known
// (ProjectEntry::ahead, behind: the store's links file, no repository read). Its words, for a screen reader and the
// pill's help: "linked to github.com/me/pad, 2 versions to push".
#pragma once

#include "editor/ChatStyle.h"
#include "plugin/EditorPort.h"

#include <funkgui/canvas/Canvas.h>

#include <string>

namespace playground::ui::gitmark
{
    inline constexpr float kGlyph = 11.0f;               // the branch's box
    inline constexpr float kGap = 6.0f;                  // after the name

    inline std::string countsText(int ahead, int behind)
    {
        std::string s;
        if (ahead > 0)
            s += std::to_string(ahead);
        if (behind > 0)
            s += (s.empty() ? "" : " ") + std::to_string(behind);
        return s;
    }

    // Its width after the name (0 for a project not linked).
    inline float width(const funkgui::Canvas& c, const ProjectEntry& e, const funkgui::TextStyle& st)
    {
        if (!e.git)
            return 0.0f;
        float w = kGap + kGlyph;
        if (e.ahead > 0)
            w += 4.0f + 8.0f + c.textWidth(std::to_string(e.ahead).c_str(), st);
        if (e.behind > 0)
            w += 4.0f + 8.0f + c.textWidth(std::to_string(e.behind).c_str(), st);
        return w;
    }

    // Drawn from x (the name's end), centred on cy; its width.
    inline float draw(funkgui::Canvas& c, const ProjectEntry& e, float x, float cy, const funkgui::TextStyle& st, funkgui::Col ink,
                      funkgui::Col arrows)
    {
        if (!e.git)
            return 0.0f;
        float at = x + kGap;
        chat::drawIcon(c, chat::Icon::branch, at + kGlyph * 0.5f, cy, kGlyph, ink);
        at += kGlyph;
        const auto count = [&](int n, bool up) {
            if (n <= 0)
                return;
            at += 4.0f;
            const float ax = at + 3.5f, s = 3.5f, w = 1.3f;
            c.segment(ax, cy - s, ax, cy + s, w, arrows);
            const float tip = up ? cy - s : cy + s, back = up ? cy - s + 3.0f : cy + s - 3.0f;
            const float xs[3] = { ax - 2.8f, ax, ax + 2.8f };
            const float ys[3] = { back, tip, back };
            c.polyline(xs, ys, 3, w, arrows);
            at += 8.0f;
            const std::string t = std::to_string(n);
            c.text(t.c_str(), at, c.capCentreTop(cy, st), st, arrows);
            at += c.textWidth(t.c_str(), st);
        };
        count(e.ahead, true);
        count(e.behind, false);
        return at - x;
    }

    // In words: "linked to github.com/me/pad, 2 versions to push, 1 commit to pull" ("" for a project not linked).
    inline std::string title(const ProjectEntry& e)
    {
        if (!e.git)
            return {};
        std::string s = "linked to " + e.remote;
        if (e.ahead > 0)
            s += ", " + std::to_string(e.ahead) + (e.ahead == 1 ? " version to push" : " versions to push");
        if (e.behind > 0)
            s += ", " + std::to_string(e.behind) + (e.behind == 1 ? " commit to pull" : " commits to pull");
        return s;
    }
}
