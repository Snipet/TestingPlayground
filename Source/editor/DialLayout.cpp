// Source/editor/DialLayout.cpp: the dials' and displays' places (DialLayout.h).
#include "editor/DialLayout.h"

#include <algorithm>
#include <cmath>

namespace playground::ui
{
    namespace
    {
        // Plain rows.
        constexpr float kGap = 30.0f;                    // between dials, while the column has room
        constexpr float kMinGap = 16.0f;                 // the least, which keeps long labels (kCaption) apart
        constexpr float kRowGap = 12.0f;                 // between rows of dials
        // Sections.
        constexpr float kPadX = 10.0f;                   // inside a frame, left and right
        constexpr float kSlotGap = 4.0f;                 // between two items' slots
        constexpr float kLabelPad = 8.0f;                // a slot is its label's width and this, at least the cell's
        constexpr float kTitleRoom = 14.0f;              // a frame is its title's width and this either side, at least
        constexpr float kBetween = 10.0f;                // between two frames in a row
        constexpr float kSectionRowGap = 10.0f;          // between rows of frames
        constexpr float kLineGap = 8.0f;                 // between lines of items in a frame
        constexpr float kBottomPad = 4.0f;               // under the items, inside the frame
        constexpr float kMaxGrow = 2.0f;                 // a frame widened to even out the rows: at most this times
        constexpr float kSmallInRow = 14.0f;             // a small knob's top below a full one's: their knobs in line
        constexpr float kButtonsExtra = 26.0f;           // buttons: a light, the gaps, beside the widest option

        // Full-size dials in as few rows as the width holds; when those rows are taller than maxHeight, every dial
        // small (as many more a row as their width allows).
        DialsLayout plainRows(const PanelSpec& spec, int used, float x, float width, float bottom, float maxHeight)
        {
            const int n = std::max(used, 1);
            const auto rowsFor = [&](float cellW) {
                const int fit = std::clamp(static_cast<int>(std::floor((width + kMinGap) / (cellW + kMinGap))), 1, kMaxControls);
                return (n + fit - 1) / fit;
            };
            const bool allSmall = static_cast<float>(rowsFor(kDialW)) * (kDialH + kRowGap) - kRowGap > maxHeight;
            const float cellW = allSmall ? kSmallDialW : kDialW, cellH = allSmall ? kSmallDialH : kDialH;
            DialsLayout out;
            out.allSmall = allSmall;
            out.rows = rowsFor(cellW);
            out.cols = (n + out.rows - 1) / out.rows;
            const float gap = out.cols > 1 ? std::clamp((width - cellW * static_cast<float>(out.cols))
                                                            / static_cast<float>(out.cols - 1),
                                                        kMinGap, kGap)
                                           : kGap;
            const float rowStep = cellH + kRowGap;
            out.top = bottom - cellH - static_cast<float>(out.rows - 1) * rowStep;
            for (int c = 0; c < kMaxControls; ++c)
            {
                ItemPlace& d = out.dials[static_cast<std::size_t>(c)];
                d.small = allSmall || spec.small[static_cast<std::size_t>(c)];
                d.w = d.small ? kSmallDialW : kDialW;
                d.h = d.small ? kSmallDialH : kDialH;
                d.x = x + static_cast<float>(c % out.cols) * (cellW + gap) + (d.small && !allSmall ? 0.5f * (kDialW - kSmallDialW) : 0.0f);
                d.top = out.top + static_cast<float>(c / out.cols) * rowStep + (d.small && !allSmall ? kSmallInRow : 0.0f);
            }
            return out;
        }

        // An int's or a bool's values (0 for a real dial).
        int valuesOf(const Control& c)
        {
            if (c.type == ControlType::toggle)
                return 2;
            if (c.type == ControlType::integer)
                return static_cast<int>(std::lround(c.max) - std::lround(c.min)) + 1;
            return 0;
        }

        struct Cell
        {
            float w = kDialW, h = kDialH;
            bool small = false;
            bool knob = false;                   // a knob (small or not): its top sits with the knobs' line
        };

        Cell cellOf(const PanelSpec& spec, const Controls& controls, const DialMeasure& m, PanelItem it, bool allSmall)
        {
            if (it.display)
            {
                const PanelDisplay& d = spec.displays[static_cast<std::size_t>(it.index)];
                switch (d.kind)
                {
                    case DisplayKind::meter:  return { d.audio() ? kMeterW : kMeterMonoW, kDialH, false, false };
                    case DisplayKind::vu:     return { kVuW, kDialH, false, false };
                    case DisplayKind::led:    return { kLedW, kDialH, false, false };
                    case DisplayKind::number: return { kNumberW, kDialH, false, false };
                    case DisplayKind::scope:
                    case DisplayKind::spectrum: return { kScopeW, kDialH, false, false };   // (a .gui file's)
                }
                return {};
            }
            const std::size_t c = static_cast<std::size_t>(it.index);
            switch (spec.styles[c])
            {
                case DialStyle::knob:
                case DialStyle::vintage:
                {
                    const bool small = allSmall || spec.small[c];
                    return { small ? kSmallDialW : kDialW, small ? kSmallDialH : kDialH, small, true };
                }
                case DialStyle::fader:  return { kFaderW, kDialH, false, false };
                case DialStyle::toggle: return { kSwitchW, kDialH, false, false };
                case DialStyle::buttons:
                {
                    const int n = std::clamp(valuesOf(controls[c]), 1, kMaxOptions);
                    float widest = 0.0f;
                    for (int k = 0; k < n; ++k)
                    {
                        const std::vector<std::string>& names = spec.options[c];
                        const std::string text = static_cast<std::size_t>(k) < names.size() && !names[static_cast<std::size_t>(k)].empty()
                                                     ? names[static_cast<std::size_t>(k)]
                                                     : std::to_string(std::lround(controls[c].min) + k);
                        widest = std::max(widest, m.micro ? m.micro(text) : 6.0f * static_cast<float>(text.size()));
                    }
                    return { std::max(kSwitchW, std::round(widest + kButtonsExtra)),
                             std::max(kDialH, kButtonsTop + static_cast<float>(n) * kButtonRowH + 4.0f), false, false };
                }
            }
            return {};
        }

        struct Section
        {
            const PanelSection* spec = nullptr;
            std::vector<PanelItem> items;        // its items that the program has
            std::vector<Cell> cells;
            std::vector<float> w;                // their slots' natural widths
            int perLine = 1, lines = 1;
            float natural = 0.0f;                // the frame's natural width
        };

        struct Row
        {
            std::vector<int> sections;
            float natural = 0.0f;
            float h = 0.0f;
            float lineH = kSmallDialH;           // its lines' height: its tallest item's
        };

        DialsLayout sectionRows(const PanelSpec& spec, const std::vector<PanelSection>& specSections, const Controls& controls,
                                const DialMeasure& measure, float x, float width, float bottom, bool allSmall)
        {
            DialsLayout out;
            out.allSmall = allSmall;

            // Each section's items, and as few lines as fit the column.
            std::vector<Section> sections;
            for (const PanelSection& ps : specSections)
            {
                Section s;
                s.spec = &ps;
                for (const PanelItem& it : ps.items)
                {
                    if (it.display ? it.index < 0 || it.index >= static_cast<int>(spec.displays.size())
                                   : it.index < 0 || it.index >= kMaxControls || !controls[static_cast<std::size_t>(it.index)].used)
                        continue;
                    const Cell cell = cellOf(spec, controls, measure, it, allSmall);
                    float labelW = 0.0f;
                    if (it.display)
                    {
                        const PanelDisplay& d = spec.displays[static_cast<std::size_t>(it.index)];
                        labelW = measure.caption ? measure.caption(d.label.empty() ? d.name : d.label) : 0.0f;
                    }
                    else if (measure.label)
                        labelW = measure.label(it.index, cell.small);
                    s.items.push_back(it);
                    s.cells.push_back(cell);
                    s.w.push_back(std::max(cell.w, labelW + kLabelPad));
                }
                if (s.items.empty())
                    continue;
                const int n = static_cast<int>(s.items.size());
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
                    const Section& s = sections[static_cast<std::size_t>(i)];
                    lines = std::max(lines, s.lines);
                    for (const Cell& cell : s.cells)
                        r.lineH = std::max(r.lineH, cell.h);
                }
                r.h = kSectionDialsTop + static_cast<float>(lines) * r.lineH + static_cast<float>(lines - 1) * kLineGap
                    + kBottomPad;
                widestRow = std::max(widestRow, r.natural);
                total += r.h;
            }
            total += static_cast<float>(std::max<std::size_t>(rows.size(), 1) - 1) * kSectionRowGap;
            out.rows = static_cast<int>(rows.size());
            out.top = bottom - total;

            // The places, row by row: the shorter rows' frames widened toward the widest row.
            for (ItemPlace& d : out.dials)
                d = { x, bottom - kDialH, kDialW, kDialH, false };
            for (ItemPlace& d : out.displays)
                d = { x, bottom - kDialH, kDialW, kDialH, false };
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
                    out.sections.push_back(
                        { { sx, rowTop + kSectionTitleTop, w, r.h - kSectionTitleTop }, s.spec->title, s.spec->color });
                    const int n = static_cast<int>(s.items.size());
                    for (int k = 0, line = 0; k < n; k += s.perLine, ++line)
                    {
                        const int end = std::min(n, k + s.perLine);
                        float natural = -kSlotGap;
                        for (int j = k; j < end; ++j)
                            natural += s.w[static_cast<std::size_t>(j)] + kSlotGap;
                        const float spare = (w - 2.0f * kPadX - natural) / static_cast<float>(end - k);
                        const float lineTop = rowTop + kSectionDialsTop + static_cast<float>(line) * (r.lineH + kLineGap);
                        float slotX = sx + kPadX;
                        for (int j = k; j < end; ++j)
                        {
                            const PanelItem it = s.items[static_cast<std::size_t>(j)];
                            const Cell& cell = s.cells[static_cast<std::size_t>(j)];
                            const float slotW = s.w[static_cast<std::size_t>(j)] + spare;
                            ItemPlace& d = it.display ? out.displays[static_cast<std::size_t>(it.index)]
                                                      : out.dials[static_cast<std::size_t>(it.index)];
                            d.small = cell.small;
                            d.w = cell.w;
                            d.h = cell.h;
                            d.x = std::round(slotX + 0.5f * (slotW - cell.w));
                            d.top = lineTop + (cell.small && r.lineH >= kDialH ? kSmallInRow : 0.0f);
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
        bool knobs = true;
        for (int c = 0; c < kMaxControls; ++c)
            if (controls[static_cast<std::size_t>(c)].used)
            {
                used = c + 1;
                const DialStyle st = spec.styles[static_cast<std::size_t>(c)];
                knobs = knobs && (st == DialStyle::knob || st == DialStyle::vintage);
            }
        if (spec.sections.empty() && spec.displays.empty() && knobs)
            return plainRows(spec, used, x, width, bottom, maxHeight);
        // No section line, but a style a plain row does not hold: one section, untitled, of every dial.
        std::vector<PanelSection> one;
        const std::vector<PanelSection>* sections = &spec.sections;
        if (spec.sections.empty())
        {
            one.emplace_back();
            for (int c = 0; c < kMaxControls; ++c)
                if (controls[static_cast<std::size_t>(c)].used)
                    one.back().items.push_back({ false, c });
            sections = &one;
        }
        DialsLayout out = sectionRows(spec, *sections, controls, measure, x, width, bottom, false);
        if (bottom - out.top > maxHeight)
            out = sectionRows(spec, *sections, controls, measure, x, width, bottom, true);
        return out;
    }
}
