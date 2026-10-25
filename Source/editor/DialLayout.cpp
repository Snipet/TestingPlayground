// Source/editor/DialLayout.cpp: the dials' places (DialLayout.h).
#include "editor/DialLayout.h"

#include <algorithm>
#include <cmath>

namespace testingplayground::ui
{
    namespace
    {
        // Plain rows.
        constexpr float kGap = 30.0f;                    // between dials, while the column has room
        constexpr float kMinGap = 16.0f;                 // the least, which keeps long labels (kCaption) apart
        constexpr float kRowGap = 12.0f;                 // between rows of dials
        // Sections.
        constexpr float kPadX = 10.0f;                   // inside a frame, left and right
        constexpr float kSlotGap = 4.0f;                 // between two dials' slots
        constexpr float kLabelPad = 8.0f;                // a slot is its label's width and this, at least the cell's
        constexpr float kTitleRoom = 14.0f;              // a frame is its title's width and this either side, at least
        constexpr float kBetween = 10.0f;                // between two frames in a row
        constexpr float kSectionRowGap = 10.0f;          // between rows of frames
        constexpr float kLineGap = 8.0f;                 // between lines of dials in a frame
        constexpr float kBottomPad = 4.0f;               // under the dials, inside the frame
        constexpr float kMaxGrow = 2.0f;                 // a frame widened to even out the rows: at most this times
        constexpr float kSmallInRow = 14.0f;             // a small dial's top below a full one's: their knobs in line

        DialsLayout plainRows(const PanelSpec& spec, int used, float x, float width, float bottom)
        {
            DialsLayout out;
            const int n = std::max(used, 1);
            const int fit = std::clamp(static_cast<int>(std::floor((width + kMinGap) / (kDialW + kMinGap))), 1, kMaxControls);
            out.rows = (n + fit - 1) / fit;
            out.cols = (n + out.rows - 1) / out.rows;
            const float gap = out.cols > 1 ? std::clamp((width - kDialW * static_cast<float>(out.cols))
                                                            / static_cast<float>(out.cols - 1),
                                                        kMinGap, kGap)
                                           : kGap;
            const float rowStep = kDialH + kRowGap;
            out.top = bottom - kDialH - static_cast<float>(out.rows - 1) * rowStep;
            for (int c = 0; c < kMaxControls; ++c)
            {
                DialPlace& d = out.dials[static_cast<std::size_t>(c)];
                d.small = spec.small[static_cast<std::size_t>(c)];
                d.x = x + static_cast<float>(c % out.cols) * (kDialW + gap) + (d.small ? 0.5f * (kDialW - kSmallDialW) : 0.0f);
                d.top = out.top + static_cast<float>(c / out.cols) * rowStep + (d.small ? kSmallInRow : 0.0f);
            }
            return out;
        }

        struct Section
        {
            const PanelSection* spec = nullptr;
            std::vector<int> slots;              // its dials that the program has
            std::vector<float> w;                // their slots' natural widths
            int perLine = 1, lines = 1;
            float natural = 0.0f;                // the frame's natural width
            bool full = false;                   // it has a full-size dial
        };

        struct Row
        {
            std::vector<int> sections;
            float natural = 0.0f;
            float h = 0.0f;
            float cellH = kSmallDialH;           // its dials' height: a full dial's when it has one
        };

        DialsLayout sectionRows(const PanelSpec& spec, const Controls& controls, const DialMeasure& measure, float x,
                                float width, float bottom, bool allSmall)
        {
            DialsLayout out;
            out.allSmall = allSmall;
            const auto smallOf = [&](int c) { return allSmall || spec.small[static_cast<std::size_t>(c)]; };
            const auto cellW = [&](int c) { return smallOf(c) ? kSmallDialW : kDialW; };

            // Each section's slots, and as few lines as fit the column.
            std::vector<Section> sections;
            for (const PanelSection& ps : spec.sections)
            {
                Section s;
                s.spec = &ps;
                for (const int c : ps.slots)
                {
                    if (c < 0 || c >= kMaxControls || !controls[static_cast<std::size_t>(c)].used)
                        continue;
                    s.slots.push_back(c);
                    s.w.push_back(std::max(cellW(c), (measure.label ? measure.label(c, smallOf(c)) : 0.0f) + kLabelPad));
                    s.full = s.full || !smallOf(c);
                }
                if (s.slots.empty())
                    continue;
                const int n = static_cast<int>(s.slots.size());
                const float room = width - 2.0f * kPadX;
                float widest = 0.0f;
                for (s.lines = 1;; ++s.lines)
                {
                    s.perLine = (n + s.lines - 1) / s.lines;
                    widest = 0.0f;
                    for (int k = 0; k < n; k += s.perLine)
                    {
                        float line = -kSlotGap;
                        for (int j = k; j < std::min(n, k + s.perLine); ++j)
                            line += s.w[static_cast<std::size_t>(j)] + kSlotGap;
                        widest = std::max(widest, line);
                    }
                    if (widest <= room || s.perLine == 1)
                        break;
                }
                const float titleW = ps.title.empty() || !measure.title ? 0.0f : measure.title(ps.title) + 2.0f * kTitleRoom;
                s.natural = std::min(width, std::max(widest + 2.0f * kPadX, titleW));
                sections.push_back(std::move(s));
            }

            // The rows: a new one where the program asks; a run of sections between two such breaks that does not fit
            // the width takes as few rows as it needs, split where the widest of them is narrowest.
            std::vector<Row> rows;
            const auto rowOf = [&](int from, int to) {
                Row r;
                for (int i = from; i < to; ++i)
                {
                    r.natural += (r.sections.empty() ? 0.0f : kBetween) + sections[static_cast<std::size_t>(i)].natural;
                    r.sections.push_back(i);
                }
                return r;
            };
            const int count = static_cast<int>(sections.size());
            for (int start = 0; start < count;)
            {
                int end = start + 1;
                while (end < count && !sections[static_cast<std::size_t>(end)].spec->newRow)
                    ++end;
                // As few rows as a greedy fill takes, then the split of the run into that many rows (in order) whose
                // widest row is narrowest: best[k][i], the narrowest widest row of the first i sections in k rows.
                int need = 1;
                float line = -kBetween;
                for (int k = start; k < end; ++k)
                {
                    const float w = sections[static_cast<std::size_t>(k)].natural;
                    if (line > 0.0f && line + kBetween + w > width)
                    {
                        ++need;
                        line = w;
                    }
                    else
                        line += kBetween + w;
                }
                const int n = end - start;
                const auto span = [&](int a, int b) { return rowOf(start + a, start + b).natural; };   // sections a..b-1
                std::vector<std::vector<float>> best(static_cast<std::size_t>(need + 1),
                                                     std::vector<float>(static_cast<std::size_t>(n + 1), 1.0e9f));
                std::vector<std::vector<int>> cut(best.size(), std::vector<int>(static_cast<std::size_t>(n + 1), 0));
                best[0][0] = 0.0f;
                for (int k = 1; k <= need; ++k)
                    for (int i = 1; i <= n; ++i)
                        for (int j = k - 1; j < i; ++j)
                        {
                            const float w
                                = std::max(best[static_cast<std::size_t>(k - 1)][static_cast<std::size_t>(j)], span(j, i));
                            if (w < best[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)])
                            {
                                best[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] = w;
                                cut[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] = j;
                            }
                        }
                std::vector<int> bounds{ n };
                for (int k = need, i = n; k > 0; --k)
                {
                    i = cut[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)];
                    bounds.insert(bounds.begin(), i);
                }
                for (std::size_t b = 0; b + 1 < bounds.size(); ++b)
                    if (bounds[b + 1] > bounds[b])
                        rows.push_back(rowOf(start + bounds[b], start + bounds[b + 1]));
                start = end;
            }
            float widestRow = 0.0f, total = 0.0f;
            for (Row& r : rows)
            {
                int lines = 1;
                for (const int i : r.sections)
                {
                    lines = std::max(lines, sections[static_cast<std::size_t>(i)].lines);
                    if (sections[static_cast<std::size_t>(i)].full)
                        r.cellH = kDialH;
                }
                r.h = kSectionDialsTop + static_cast<float>(lines) * r.cellH + static_cast<float>(lines - 1) * kLineGap
                    + kBottomPad;
                widestRow = std::max(widestRow, r.natural);
                total += r.h;
            }
            total += static_cast<float>(rows.size() - 1) * kSectionRowGap;
            out.rows = static_cast<int>(rows.size());
            out.top = bottom - total;

            // The places, row by row: the shorter rows' frames widened toward the widest row.
            for (int c = 0; c < kMaxControls; ++c)
                out.dials[static_cast<std::size_t>(c)] = { x, bottom - kDialH, false };
            float rowTop = out.top;
            for (const Row& r : rows)
            {
                const float extra = std::max(0.0f, std::min(widestRow, width) - r.natural);
                float sumNatural = 0.0f;
                for (const int i : r.sections)
                    sumNatural += sections[static_cast<std::size_t>(i)].natural;
                float sx = x;
                for (const int i : r.sections)
                {
                    const Section& s = sections[static_cast<std::size_t>(i)];
                    const float w = std::min(s.natural + extra * s.natural / std::max(sumNatural, 1.0f), kMaxGrow * s.natural);
                    out.sections.push_back({ { sx, rowTop + kSectionTitleTop, w, r.h - kSectionTitleTop }, s.spec->title });
                    const int n = static_cast<int>(s.slots.size());
                    for (int k = 0, line = 0; k < n; k += s.perLine, ++line)
                    {
                        const int end = std::min(n, k + s.perLine);
                        float natural = -kSlotGap;
                        for (int j = k; j < end; ++j)
                            natural += s.w[static_cast<std::size_t>(j)] + kSlotGap;
                        const float spare = (w - 2.0f * kPadX - natural) / static_cast<float>(end - k);
                        const float lineTop = rowTop + kSectionDialsTop + static_cast<float>(line) * (r.cellH + kLineGap);
                        float slotX = sx + kPadX;
                        for (int j = k; j < end; ++j)
                        {
                            const int c = s.slots[static_cast<std::size_t>(j)];
                            const float slotW = s.w[static_cast<std::size_t>(j)] + spare;
                            DialPlace& d = out.dials[static_cast<std::size_t>(c)];
                            d.small = smallOf(c);
                            d.x = std::round(slotX + 0.5f * (slotW - cellW(c)));
                            d.top = lineTop + (d.small && r.cellH == kDialH ? kSmallInRow : 0.0f);
                            slotX += slotW + kSlotGap;
                        }
                    }
                    sx += w + kBetween;
                }
                rowTop += r.h + kSectionRowGap;
            }
            return out;
        }
    }

    DialsLayout layoutDials(const PanelSpec& spec, const Controls& controls, const DialMeasure& measure, float x,
                            float width, float bottom, float maxHeight)
    {
        int used = 0;
        for (int c = 0; c < kMaxControls; ++c)
            if (controls[static_cast<std::size_t>(c)].used)
                used = c + 1;
        if (spec.sections.empty())
            return plainRows(spec, used, x, width, bottom);
        DialsLayout out = sectionRows(spec, controls, measure, x, width, bottom, false);
        if (bottom - out.top > maxHeight)
            out = sectionRows(spec, controls, measure, x, width, bottom, true);
        return out;
    }
}
