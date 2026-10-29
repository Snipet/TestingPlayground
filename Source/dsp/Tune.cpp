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
    }

    TuneResult tune(const Sources& files, const TuneRequest& request)
    {
        TuneResult out;
        const auto t0 = std::chrono::steady_clock::now();
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
            if (const std::string why = invalid(g); !why.empty())
            {
                out.error = why;
                return out;
            }
        Session session(files, request.render.rate);
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
                std::string have;
                for (const Control& k : controls)
                    if (k.used && k.type == ControlType::real)
                        have += (have.empty() ? "" : ", ") + std::string(k.name);
                out.error = "The program has no dial '" + want + "'. Its float dials: " + (have.empty() ? "none" : have) + ".";
                return out;
            }
            const Control& k = controls[static_cast<std::size_t>(found)];
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
            double score = 0.0;
            bool every = true;
            for (const Goal& g : request.goals)
            {
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
            const auto value = [&](const Metrics& m) {
                const auto it = m.find(g.metric);
                return it == m.end() ? std::string("not measured") : valueText(g.metric, it->second);
            };
            t += "- " + g.metric + " " + value(out.bestValues) + " (" + goalText(g) + "): "
               + (out.bestMet[j] ? "met" : "NOT met") + "; it was " + value(out.startValues) + ".\n";
        }
        if (metCount < static_cast<int>(request.goals.size()))
            t += "Not every goal was met within " + std::to_string(budget)
               + " renders: the dials may not reach it (a goal the program cannot give, or one at odds with another), or "
                 "another dial matters. Measure the best, change the program, or tune again with other dials or goals.\n";
        out.text = std::move(t);
        return out;
    }
}
