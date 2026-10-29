// Source/dsp/TextDiff.cpp: see TextDiff.h.
#include "dsp/TextDiff.h"

#include <algorithm>

namespace playground::textdiff
{
    std::vector<std::string_view> lines(std::string_view text)
    {
        std::vector<std::string_view> out;
        std::size_t at = 0;
        while (at < text.size())
        {
            const std::size_t end = text.find('\n', at);
            if (end == std::string_view::npos)
            {
                out.push_back(text.substr(at));
                break;
            }
            out.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        return out;
    }

    namespace
    {
        // Myers's greedy search over a[0, n) and b[0, m) (offsets into the whole texts: a0, b0), its script appended to
        // `out`; false when it would take more than maxD edits.
        bool myers(const std::vector<std::string_view>& a, std::size_t a0, std::size_t n, const std::vector<std::string_view>& b,
                   std::size_t b0, std::size_t m, std::size_t maxD, std::vector<Edit>& out)
        {
            const auto N = static_cast<int>(n), M = static_cast<int>(m);
            const int most = static_cast<int>(std::min<std::size_t>(n + m, maxD));
            std::vector<int> v(static_cast<std::size_t>(2 * (N + M) + 3), 0);
            const int off = N + M + 1;
            std::vector<std::vector<int>> trace;         // trace[d]: v[-d..d] after step d
            const auto same = [&](int x, int y) {
                return a[a0 + static_cast<std::size_t>(x)] == b[b0 + static_cast<std::size_t>(y)];
            };
            int found = -1;
            for (int d = 0; d <= most && found < 0; ++d)
            {
                for (int k = -d; k <= d; k += 2)
                {
                    int x = (k == -d || (k != d && v[static_cast<std::size_t>(k - 1 + off)] < v[static_cast<std::size_t>(k + 1 + off)]))
                                 ? v[static_cast<std::size_t>(k + 1 + off)]
                                 : v[static_cast<std::size_t>(k - 1 + off)] + 1;
                    int y = x - k;
                    while (x < N && y < M && same(x, y))
                    {
                        ++x;
                        ++y;
                    }
                    v[static_cast<std::size_t>(k + off)] = x;
                    if (x >= N && y >= M)
                    {
                        found = d;
                        break;
                    }
                }
                trace.emplace_back(v.begin() + (off - d), v.begin() + (off + d + 1));
            }
            if (found < 0)
                return false;
            // Back from (N, M): each step d came from a diagonal of step d - 1, then ran along a snake.
            std::vector<Edit> back;
            int x = N, y = M;
            for (int d = found; d > 0; --d)
            {
                const std::vector<int>& pv = trace[static_cast<std::size_t>(d - 1)];
                const auto at = [&](int k) { return pv[static_cast<std::size_t>(k + d - 1)]; };
                const int k = x - y;
                const int pk = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
                const int px = at(pk), py = px - pk;
                while (x > px && y > py)
                {
                    --x;
                    --y;
                    back.push_back({ Op::same, a0 + static_cast<std::size_t>(x), b0 + static_cast<std::size_t>(y) });
                }
                if (x == px)
                    back.push_back({ Op::added, a0 + static_cast<std::size_t>(px), b0 + static_cast<std::size_t>(py) });
                else
                    back.push_back({ Op::removed, a0 + static_cast<std::size_t>(px), b0 + static_cast<std::size_t>(py) });
                x = px;
                y = py;
            }
            while (x > 0 && y > 0)
            {
                --x;
                --y;
                back.push_back({ Op::same, a0 + static_cast<std::size_t>(x), b0 + static_cast<std::size_t>(y) });
            }
            out.insert(out.end(), back.rbegin(), back.rend());
            return true;
        }
    }

    std::vector<Edit> diff(const std::vector<std::string_view>& before, const std::vector<std::string_view>& after, bool* whole,
                           std::size_t maxEdits)
    {
        std::vector<Edit> out;
        const std::size_t n = before.size(), m = after.size();
        std::size_t head = 0;
        while (head < n && head < m && before[head] == after[head])
            ++head;
        std::size_t tail = 0;
        while (tail < n - head && tail < m - head && before[n - 1 - tail] == after[m - 1 - tail])
            ++tail;
        for (std::size_t i = 0; i < head; ++i)
            out.push_back({ Op::same, i, i });
        const std::size_t mn = n - head - tail, mm = m - head - tail;
        const bool ok = myers(before, head, mn, after, head, mm, maxEdits, out);
        if (!ok)
        {
            for (std::size_t i = 0; i < mn; ++i)
                out.push_back({ Op::removed, head + i, head });
            for (std::size_t j = 0; j < mm; ++j)
                out.push_back({ Op::added, head + mn, head + j });
        }
        if (whole != nullptr)
            *whole = ok;
        for (std::size_t i = 0; i < tail; ++i)
            out.push_back({ Op::same, n - tail + i, m - tail + i });
        return out;
    }

    Unified unified(std::string_view before, std::string_view after, int context, int maxLines)
    {
        Unified u;
        const std::vector<std::string_view> a = lines(before), b = lines(after);
        const std::vector<Edit> script = diff(a, b);
        for (const Edit& e : script)
        {
            u.added += e.op == Op::added ? 1 : 0;
            u.removed += e.op == Op::removed ? 1 : 0;
        }
        if (u.added == 0 && u.removed == 0)
            return u;
        const auto c = static_cast<std::size_t>(std::max(0, context));
        const auto put = [&](const std::string& line) {
            if (u.lines >= maxLines)
            {
                u.cut = true;
                return false;
            }
            u.text += line;
            u.text += '\n';
            ++u.lines;
            return true;
        };
        std::size_t i = 0;
        while (i < script.size())
        {
            // The next change, and its hunk: from `context` lines before it to `context` after the last change that
            // lies within 2 * context lines of the one before.
            std::size_t first = i;
            while (first < script.size() && script[first].op == Op::same)
                ++first;
            if (first == script.size())
                break;
            const std::size_t from = first >= c ? first - c : 0;
            std::size_t last = first;                // the hunk's last change
            for (std::size_t k = first + 1; k < script.size(); ++k)
            {
                if (script[k].op == Op::same)
                    continue;
                if (k - last > 2 * c + 1)
                    break;
                last = k;
            }
            const std::size_t to = std::min(script.size(), last + c + 1);
            std::size_t oldStart = 0, newStart = 0, oldCount = 0, newCount = 0;
            bool started = false;
            for (std::size_t k = from; k < to; ++k)
            {
                const Edit& e = script[k];
                if (!started)
                {
                    oldStart = e.a;
                    newStart = e.b;
                    started = true;
                }
                oldCount += e.op != Op::added ? 1 : 0;
                newCount += e.op != Op::removed ? 1 : 0;
            }
            // As diff -u numbers them: a range of no lines names the line before it.
            const std::size_t oldLine = oldCount == 0 ? oldStart : oldStart + 1;
            const std::size_t newLine = newCount == 0 ? newStart : newStart + 1;
            if (!put("@@ -" + std::to_string(oldLine) + "," + std::to_string(oldCount) + " +" + std::to_string(newLine) + ","
                     + std::to_string(newCount) + " @@"))
                return u;
            for (std::size_t k = from; k < to; ++k)
            {
                const Edit& e = script[k];
                const std::string_view text = e.op == Op::added ? b[e.b] : a[e.a];
                const char mark = e.op == Op::same ? ' ' : (e.op == Op::removed ? '-' : '+');
                if (!put(std::string(1, mark) + std::string(text)))
                    return u;
            }
            i = to;
        }
        return u;
    }
}
