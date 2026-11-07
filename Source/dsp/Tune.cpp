// Source/dsp/Tune.cpp: see Tune.h.
#include "dsp/Tune.h"

#include "dsp/Controls.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace playground::measure
{
    namespace
    {
        std::string seconds2(double s)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f s", s);
            return buf;
        }

        std::string number(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.4g", v);
            return buf;
        }

        constexpr std::array<int, kMaxTuned> kPrimes{ 2, 3, 5, 7, 11, 13, 17, 19 };

        // The k-th point of a Halton sequence in n dimensions (bases the first primes), each coordinate in (0, 1).
        std::vector<double> halton(int k, std::size_t n)
        {
            std::vector<double> x(n);
            for (std::size_t d = 0; d < n; ++d)
            {
                double f = 1.0, r = 0.0;
                for (int i = k; i > 0; i /= kPrimes[d])
                {
                    f /= kPrimes[d];
                    r += f * (i % kPrimes[d]);
                }
                x[d] = r;
            }
            return x;
        }

        struct Point
        {
            std::vector<double> x;
            double score = 0.0;
        };

        // A swept dial's value as the table shows it: a bool off or on, an int whole, a float as a number.
        std::string dialValue(const Control& k, float v)
        {
            if (k.type == ControlType::toggle)
                return v >= 0.5f ? "on" : "off";
            if (k.type == ControlType::integer)
                return std::to_string(std::lround(v));
            return number(v);
        }

        // Why a sweep's value cannot be ("" when it can): outside the dial's range, or not whole for an int or a bool.
        std::string sweepValueWhy(const Control& k, float v)
        {
            const float lo = std::min(k.min, k.max), hi = std::max(k.min, k.max);
            const std::string name = k.name;
            if (k.type == ControlType::toggle && v != 0.0f && v != 1.0f)
                return "sweep: " + number(v) + " is not 0 or 1, and " + name + " is a bool (0 off, 1 on).";
            if (k.type == ControlType::integer && v != std::round(v))
                return "sweep: " + number(v) + " is not a whole number, and " + name + " is an int.";
            if (!std::isfinite(v) || v < lo || v > hi)
                return "sweep: " + number(v) + " is outside " + name + "'s range (" + number(lo) + " to " + number(hi) + ").";
            return {};
        }

        // The goals a row meets, of those judged (Goals.h's judged).
        std::pair<int, int> metOf(const std::vector<Goal>& goals, const Metrics& values)
        {
            int met = 0, judgedCount = 0;
            for (const Goal& g : goals)
                if (judged(g))
                {
                    ++judgedCount;
                    const auto it = values.find(g.metric);
                    met += measure::met(g, it == values.end() ? std::nan("") : it->second) ? 1 : 0;
                }
            return { met, judgedCount };
        }

        // A sweep (TuneRequest::sweep): the one dial at each value in turn, the goals' metrics at each, as a table.
        void sweepDial(Session& session, const Control& k, const TuneRequest& request,
                       std::chrono::steady_clock::time_point t0, TuneResult& out)
        {
            for (const float v : request.sweep)
                if (std::string why = sweepValueWhy(k, v); !why.empty())
                {
                    out.error = std::move(why);
                    return;
                }
            const std::string name = k.name;
            Request base = request.render;
            for (const Goal& g : request.goals)
                base.want.insert(g.metric);
            struct Missing
            {
                int count = 0;
                std::string why;
                float first = 0.0f;
            };
            std::map<std::string, Missing> unmeasured;   // a goal's metric some value left unmeasured: how often, the first why
            for (const float v : request.sweep)
            {
                Request r = base;
                r.dials[name] = v;
                const Result m = session.measure(r);
                TuneResult::SweepRow row;
                row.value = v;
                for (const Goal& g : request.goals)
                {
                    const auto it = m.values.find(g.metric);
                    if (it != m.values.end())
                        row.values[g.metric] = it->second;
                    if (judged(g))
                        row.score += g.weight * penalty(g, it == m.values.end() ? std::nan("") : it->second);
                }
                for (const auto& [metric, why] : m.unmeasured)
                    if (row.values.count(metric) == 0 && base.want.count(metric) != 0)
                    {
                        Missing& u = unmeasured[metric];
                        if (u.count++ == 0)
                        {
                            u.why = why;
                            u.first = v;
                        }
                    }
                out.sweep.push_back(std::move(row));
                ++out.renders;
                if (request.progress && !request.progress(out.renders, static_cast<int>(request.sweep.size())))
                    out.stopped = true;
                if (request.seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= request.seconds)
                    out.stopped = out.timedOut = true;
                if (out.stopped)
                    break;
            }
            out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            // The table: a row a value, a column a goal (its bound or target in the header, met or not in each cell).
            const std::size_t all = request.sweep.size(), done = out.sweep.size();
            const std::string stopped = out.timedOut ? ", stopped at the time limit after " + std::to_string(done) + " of " + std::to_string(all)
                                      : out.stopped  ? ", stopped by the user after " + std::to_string(done) + " of " + std::to_string(all)
                                                     : std::string();
            std::string t = "Swept " + name + " over " + std::to_string(all) + " value" + (all == 1 ? "" : "s") + " (" + seconds2(out.seconds)
                          + stopped + "):\n" + name
                          + (k.type == ControlType::real && k.unit[0] != '\0' && k.scale == 1.0f ? " (" + std::string(k.unit) + ")" : "");
            bool anyJudged = false;
            for (const Goal& g : request.goals)
            {
                t += " | " + g.metric + (judged(g) ? " (" + goalText(g) + ")" : "");
                anyJudged = anyJudged || judged(g);
            }
            t += "\n";
            std::size_t best = 0;
            std::vector<std::string> everyMet;
            for (std::size_t i = 0; i < done; ++i)
            {
                const TuneResult::SweepRow& row = out.sweep[i];
                t += dialValue(k, row.value);
                for (const Goal& g : request.goals)
                {
                    const auto it = row.values.find(g.metric);
                    t += " | " + (it == row.values.end() ? std::string("not measured") : valueText(g.metric, it->second));
                    if (judged(g))
                        t += measure::met(g, it == row.values.end() ? std::nan("") : it->second) ? ", met" : ", NOT met";
                }
                t += "\n";
                if (row.score < out.sweep[best].score)
                    best = i;
                if (const auto [met, of] = metOf(request.goals, row.values); of > 0 && met == of)
                    everyMet.push_back(dialValue(k, row.value));
            }
            if (anyJudged && done > 0)
            {
                if (!everyMet.empty())
                {
                    std::string list;
                    for (std::size_t i = 0; i < everyMet.size(); ++i)
                        list += (i == 0 ? "" : i + 1 == everyMet.size() ? " and " : ", ") + everyMet[i];
                    t += "Every goal is met at " + name + " " + list + ".\n";
                }
                else
                {
                    const auto [met, of] = metOf(request.goals, out.sweep[best].values);
                    t += "No value meets every goal; the nearest: " + name + " " + dialValue(k, out.sweep[best].value) + " ("
                       + std::to_string(met) + " of " + std::to_string(of) + " met).\n";
                }
            }
            for (const auto& [metric, u] : unmeasured)
                t += metric + " is not measured at " + (u.count == static_cast<int>(done) ? std::string("any value")
                                                        : std::to_string(u.count) + " of " + std::to_string(done) + " values")
                   + " (at " + dialValue(k, u.first) + ": " + u.why + ").\n";
            out.text = std::move(t);
        }
    }

    TuneResult tune(const Sources& files, const TuneRequest& request)
    {
        TuneResult out;
        const auto t0 = std::chrono::steady_clock::now();
        const bool sweeping = !request.sweep.empty();
        if (sweeping && request.dials.size() != 1)
        {
            out.error = "A sweep takes one dial in tune (it names " + std::to_string(request.dials.size()) + ").";
            return out;
        }
        if (sweeping && request.sweep.size() > static_cast<std::size_t>(kMaxSweep))
        {
            out.error = "A sweep takes 1 to " + std::to_string(kMaxSweep) + " values (it gives "
                      + std::to_string(request.sweep.size()) + ").";
            return out;
        }
        if (request.dials.empty() || request.dials.size() > static_cast<std::size_t>(kMaxTuned))
        {
            out.error = "Name 1 to " + std::to_string(kMaxTuned) + " dials to tune.";
            return out;
        }
        if (request.goals.empty() || request.goals.size() > static_cast<std::size_t>(kMaxGoals))
        {
            out.error = "Give 1 to " + std::to_string(kMaxGoals) + " goals.";
            return out;
        }
        for (const Goal& g : request.goals)
            if (const std::string why = invalid(g, sweeping); !why.empty())
            {
                out.error = why;
                return out;
            }
        Session session(files, request.render.rate, nullptr, request.render.assets);
        if (!session.compiled())
        {
            out.error = "It did not compile, so nothing was tuned:\n" + session.errors();
            return out;
        }
        const Controls& controls = session.controls();
        std::vector<int> slots;
        std::vector<std::string> names;
        for (const std::string& want : request.dials)
        {
            int found = -1;
            for (int c = 0; c < kMaxControls; ++c)
                if (controls[static_cast<std::size_t>(c)].used && want == controls[static_cast<std::size_t>(c)].name)
                    found = c;
            if (found < 0)
            {
                std::string have;                        // a search's float dials; a sweep's every dial
                for (const Control& k : controls)
                    if (k.used && (sweeping || k.type == ControlType::real))
                        have += (have.empty() ? "" : ", ") + std::string(k.name);
                out.error = "The program has no dial '" + want + (sweeping ? "'. Its dials: " : "'. Its float dials: ")
                          + (have.empty() ? "none" : have) + ".";
                return out;
            }
            const Control& k = controls[static_cast<std::size_t>(found)];
            if (sweeping)
            {
                sweepDial(session, k, request, t0, out);
                return out;
            }
            if (k.type != ControlType::real)
            {
                out.error = std::string(k.name) + " is " + (k.type == ControlType::toggle ? "a bool" : "an int")
                          + ": tune_dials tunes float dials only (set_dial sets the others).";
                return out;
            }
            if (std::find(slots.begin(), slots.end(), found) != slots.end())
            {
                out.error = std::string(k.name) + " is named twice.";
                return out;
            }
            slots.push_back(found);
            names.push_back(k.name);
        }
        const std::size_t n = slots.size();
        const int budget = std::clamp(request.budget, 1, kMaxBudget);

        // Where each tuned dial starts: as the request's dials say, else as the plugin has it, else its default.
        std::vector<double> x0(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const Control& k = controls[static_cast<std::size_t>(slots[i])];
            float plain = k.def;
            if (const auto it = request.render.dials.find(names[i]); it != request.render.dials.end())
                plain = it->second;
            else if (const auto cur = request.render.current.find(names[i]); cur != request.render.current.end())
                plain = cur->second;
            x0[i] = normOf(k, std::clamp(plain, std::min(k.min, k.max), std::max(k.min, k.max)));
        }

        Request base = request.render;
        for (const Goal& g : request.goals)
            base.want.insert(g.metric);
        struct Seen
        {
            std::vector<double> x;
            double score;
            Metrics values;
        };
        std::vector<Seen> seen;
        Point best;
        Metrics bestValues;
        bool allMet = false;
        // A goal whose metric the render leaves unmeasured, and why (Result::unmeasured: its window, which no dial
        // changes): left out of the score and of "every goal met", and named in the outcome.
        std::map<std::string, std::string> unmeasured;
        const auto plainAt = [&](const std::vector<double>& x, std::size_t i) {
            return plainOf(controls[static_cast<std::size_t>(slots[i])], static_cast<float>(x[i]));
        };
        // The score at x (clamped to the ranges); a point seen before is not rendered again.
        const auto evaluate = [&](std::vector<double> x) {
            for (double& v : x)
                v = std::clamp(v, 0.0, 1.0);
            for (const Seen& s : seen)
            {
                double d = 0.0;
                for (std::size_t i = 0; i < n; ++i)
                    d = std::max(d, std::fabs(s.x[i] - x[i]));
                if (d < 1e-7)
                    return Point{ x, s.score };
            }
            Request r = base;
            for (std::size_t i = 0; i < n; ++i)
                r.dials[names[i]] = plainAt(x, i);
            const Result m = session.measure(r);
            if (out.renders == 0)
                unmeasured = m.unmeasured;
            double score = 0.0;
            bool every = true;
            for (const Goal& g : request.goals)
            {
                if (unmeasured.count(g.metric) != 0)
                    continue;
                const auto it = m.values.find(g.metric);
                const double v = it == m.values.end() ? std::nan("") : it->second;
                score += g.weight * penalty(g, v);
                every = every && met(g, v);
            }
            ++out.renders;
            seen.push_back({ x, score, m.values });
            if (out.renders == 1 || score < best.score)
            {
                best = { x, score };
                bestValues = m.values;
            }
            allMet = allMet || every;
            if (request.progress && !request.progress(out.renders, budget))
                out.stopped = true;
            if (request.seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= request.seconds)
                out.stopped = out.timedOut = true;
            return Point{ x, score };
        };
        const auto goOn = [&] { return out.renders < budget && !out.stopped && !allMet; };

        const Point start = evaluate(x0);
        out.startScore = start.score;
        out.startValues = bestValues;
        for (std::size_t i = 0; i < n; ++i)
            out.start[names[i]] = plainAt(x0, i);
        // No goal measured in this render: nothing to tune for (the dials stay as they are).
        if (std::all_of(request.goals.begin(), request.goals.end(), [&](const Goal& g) { return unmeasured.count(g.metric) != 0; }))
        {
            std::string why;
            std::vector<std::string> named;
            for (const Goal& g : request.goals)
                if (std::find(named.begin(), named.end(), g.metric) == named.end())
                {
                    named.push_back(g.metric);
                    why += (why.empty() ? "" : "; ") + g.metric + " is not measured in this render: " + unmeasured[g.metric];
                }
            out.error = "Nothing to tune for: " + why + ".";
            return out;
        }

        // A few points spread over the ranges, then Nelder-Mead from the best (and again from the best when it settles).
        const int spread = std::min(static_cast<int>(2 * n + 2), budget / 5);
        for (int k = 1; k <= spread && goOn(); ++k)
            evaluate(halton(k, n));
        double step = 0.2;
        for (int restart = 0; restart < 6 && goOn(); ++restart)
        {
            std::vector<Point> simplex{ best };
            for (std::size_t i = 0; i < n && goOn(); ++i)
            {
                std::vector<double> x = best.x;
                x[i] += (x[i] + step <= 1.0) ? step : -step;
                simplex.push_back(evaluate(x));
            }
            while (goOn() && simplex.size() == n + 1)
            {
                std::sort(simplex.begin(), simplex.end(), [](const Point& a, const Point& b) { return a.score < b.score; });
                double size = 0.0;
                for (std::size_t j = 1; j <= n; ++j)
                    for (std::size_t i = 0; i < n; ++i)
                        size = std::max(size, std::fabs(simplex[j].x[i] - simplex[0].x[i]));
                if (size < 1e-4 || simplex[n].score - simplex[0].score < 1e-9 * (1.0 + simplex[0].score))
                    break;                               // settled
                std::vector<double> c(n, 0.0);           // the centroid of all but the worst
                for (std::size_t j = 0; j < n; ++j)
                    for (std::size_t i = 0; i < n; ++i)
                        c[i] += simplex[j].x[i] / static_cast<double>(n);
                const auto along = [&](double t) {
                    std::vector<double> x(n);
                    for (std::size_t i = 0; i < n; ++i)
                        x[i] = c[i] + t * (simplex[n].x[i] - c[i]);
                    return x;
                };
                const Point reflected = evaluate(along(-1.0));
                if (reflected.score < simplex[0].score && goOn())
                {
                    const Point expanded = evaluate(along(-2.0));
                    simplex[n] = expanded.score < reflected.score ? expanded : reflected;
                }
                else if (reflected.score < simplex[n - 1].score)
                    simplex[n] = reflected;
                else if (goOn())
                {
                    const Point contracted = evaluate(along(reflected.score < simplex[n].score ? -0.5 : 0.5));
                    if (contracted.score < std::min(reflected.score, simplex[n].score))
                        simplex[n] = contracted;
                    else
                        for (std::size_t j = 1; j <= n && goOn(); ++j)   // shrink toward the best
                        {
                            std::vector<double> x(n);
                            for (std::size_t i = 0; i < n; ++i)
                                x[i] = simplex[0].x[i] + 0.5 * (simplex[j].x[i] - simplex[0].x[i]);
                            simplex[j] = evaluate(x);
                        }
                }
            }
            step = restart % 2 == 0 ? 0.05 : 0.3;        // finer, then wider again
        }

        out.bestScore = best.score;
        out.bestValues = bestValues;
        for (std::size_t i = 0; i < n; ++i)
            out.best[names[i]] = plainAt(best.x, i);
        for (const Goal& g : request.goals)
        {
            const auto at = [&](const Metrics& m) {
                const auto it = m.find(g.metric);
                return it == m.end() ? std::nan("") : it->second;
            };
            out.startMet.push_back(met(g, at(out.startValues)));
            out.bestMet.push_back(met(g, at(out.bestValues)));
        }
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        // The outcome in words.
        const int metCount = static_cast<int>(std::count(out.bestMet.begin(), out.bestMet.end(), true));
        const int metBefore = static_cast<int>(std::count(out.startMet.begin(), out.startMet.end(), true));
        std::string t = "Tuned " + std::to_string(n) + " dial" + (n == 1 ? "" : "s") + " in " + std::to_string(out.renders)
                      + " render" + (out.renders == 1 ? "" : "s") + " (" + seconds2(out.seconds)
                      + (out.timedOut ? ", stopped at the time limit: the best so far" : out.stopped ? ", stopped by the user" : "") + "): " + std::to_string(metCount) + " of "
                      + std::to_string(request.goals.size()) + " goals met (" + std::to_string(metBefore)
                      + " as the dials were).\n";
        if (metBefore == static_cast<int>(request.goals.size()) && out.renders == 1)
            t = "Every goal is met already, as the dials are (1 render): nothing to tune.\n";
        t += "Dials:";
        for (std::size_t i = 0; i < n; ++i)
        {
            const Control& k = controls[static_cast<std::size_t>(slots[i])];
            t += std::string(i == 0 ? " " : ", ") + names[i] + " " + number(out.start[names[i]]) + " -> "
               + number(out.best[names[i]]) + (k.unit[0] != '\0' ? std::string(" ") + k.unit : std::string())
               + " (range " + number(k.min) + " to " + number(k.max) + ")";
        }
        t += ".\nGoals, at the best (and as the dials were):\n";
        for (std::size_t j = 0; j < request.goals.size(); ++j)
        {
            const Goal& g = request.goals[j];
            if (const auto why = unmeasured.find(g.metric); why != unmeasured.end())
            {
                t += "- " + g.metric + " (" + goalText(g) + "): not measured, " + why->second + ": left out of the search.\n";
                continue;
            }
            const auto value = [&](const Metrics& m) {
                const auto it = m.find(g.metric);
                return it == m.end() ? std::string("not measured") : valueText(g.metric, it->second);
            };
            t += "- " + g.metric + " " + value(out.bestValues) + " (" + goalText(g) + "): "
               + (out.bestMet[j] ? "met" : "NOT met") + "; it was " + value(out.startValues) + ".\n";
        }
        int unmet = 0;                                   // the measured goals not met
        for (std::size_t j = 0; j < request.goals.size(); ++j)
            unmet += !out.bestMet[j] && unmeasured.count(request.goals[j].metric) == 0 ? 1 : 0;
        if (unmet > 0)
            t += "Not every goal was met within " + std::to_string(budget)
               + " renders: the dials may not reach it (a goal the program cannot give, or one at odds with another), or "
                 "another dial matters. Measure the best, change the program, or tune again with other dials or goals.\n";
        out.text = std::move(t);
        return out;
    }
}
