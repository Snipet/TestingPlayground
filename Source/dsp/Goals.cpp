// Source/dsp/Goals.cpp: see Goals.h.
#include "dsp/Goals.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace playground::measure
{
    namespace
    {
        constexpr double kUnmeasured = 1.0e6;        // a value not measured, or never reached: worse than any measured

        // e tolerances away: e squared up to twenty, then on in a straight line, so a search far off still sees
        // which way is nearer.
        double far(double e)
        {
            const double a = std::fabs(e);
            return std::min(a <= 20.0 ? a * a : 400.0 + 40.0 * (a - 20.0), kUnmeasured * 0.5);
        }
    }

    std::string invalid(const Goal& g)
    {
        if (metricInfo(g.metric) == nullptr)
        {
            std::string names;
            for (const MetricInfo& m : metrics())
                names += (names.empty() ? "" : ", ") + std::string(m.name);
            return "There is no metric '" + g.metric + "'. This plugin's: " + names + ".";
        }
        if (!g.hasMin && !g.hasMax && !g.hasTarget)
            return "A goal on " + g.metric + " needs min, max or target.";
        if (g.hasTarget && (g.hasMin || g.hasMax))
            return "A goal on " + g.metric + " takes a target or bounds (min, max), not both.";
        if (g.hasMin && g.hasMax && g.min > g.max)
            return "A goal on " + g.metric + " has its min above its max.";
        if (!(g.tolerance >= 0.0) || !(g.weight >= 0.0))
            return "A goal's tolerance and weight are 0 or more.";
        for (const double v : { g.min, g.max, g.target, g.tolerance, g.weight })
            if (!std::isfinite(v))
                return "A goal's numbers are finite.";
        return {};
    }

    double toleranceOf(const Goal& g)
    {
        if (g.tolerance > 0.0)
            return g.tolerance;
        const MetricInfo* m = metricInfo(g.metric);
        if (m == nullptr)
            return 1.0;
        if (!m->relative)
            return m->tolerance;
        const double ref = g.hasTarget ? g.target : g.hasMax ? g.max : g.min;
        return std::max(m->tolerance * std::fabs(ref), m->floor);
    }

    bool met(const Goal& g, double v)
    {
        if (std::isnan(v))
            return false;
        if (g.hasTarget)
            return std::isfinite(v) && std::fabs(v - g.target) <= toleranceOf(g);
        if (g.hasMin && !(v >= g.min))
            return false;
        if (g.hasMax && !(v <= g.max))
            return false;
        return true;
    }

    double penalty(const Goal& g, double v)
    {
        const double tol = std::max(toleranceOf(g), 1e-12);
        if (std::isnan(v))
            return kUnmeasured;
        if (g.hasTarget)
        {
            if (!std::isfinite(v))
                return kUnmeasured;
            return far((v - g.target) / tol);
        }
        if (g.hasMin && v < g.min)
            return std::isfinite(v) ? far((g.min - v) / tol) : kUnmeasured;
        if (g.hasMax && v > g.max)
            return std::isfinite(v) ? far((v - g.max) / tol) : kUnmeasured;
        return 0.0;
    }

    std::string goalText(const Goal& g)
    {
        if (g.hasTarget)
            return valueText(g.metric, g.target) + ", within " + valueText(g.metric, toleranceOf(g));
        if (g.hasMin && g.hasMax)
            return "between " + valueText(g.metric, g.min) + " and " + valueText(g.metric, g.max);
        if (g.hasMin)
            return "at least " + valueText(g.metric, g.min);
        return "at most " + valueText(g.metric, g.max);
    }

    std::vector<CheckResult> evaluate(const Sources& files, const std::vector<Check>& checks,
                                      const std::map<std::string, float>& current, std::string* errors)
    {
        std::vector<CheckResult> out(checks.size());
        if (checks.empty())
            return out;
        // One compile a sample rate the checks render at (a check's scenario may name one).
        std::map<double, std::unique_ptr<Session>> sessions;
        const auto sessionFor = [&](double rate) -> Session& {
            std::unique_ptr<Session>& s = sessions[rate];
            if (!s)
                s = std::make_unique<Session>(files, rate);
            return *s;
        };
        if (Session& first = sessionFor(checks.front().request.rate); !first.compiled())
        {
            for (CheckResult& r : out)
                r.text = "not measured: the program does not compile";
            if (errors != nullptr)
                *errors = first.errors();
            return out;
        }
        // The checks that share a render, measured together, each group once with the metrics its checks want.
        std::vector<bool> done(checks.size(), false);
        for (std::size_t i = 0; i < checks.size(); ++i)
        {
            if (done[i])
                continue;
            Request r = checks[i].request;
            r.current = current;
            r.want.clear();
            std::vector<std::size_t> group;
            for (std::size_t j = i; j < checks.size(); ++j)
            {
                Request o = checks[j].request;
                o.current = current;
                if (!done[j] && o.sameRender(r))
                {
                    group.push_back(j);
                    r.want.insert(checks[j].goal.metric);
                    done[j] = true;
                }
            }
            const Result m = sessionFor(r.rate).measure(r);
            for (const std::size_t j : group)
            {
                const Goal& g = checks[j].goal;
                CheckResult& c = out[j];
                const auto it = m.values.find(g.metric);
                if (it == m.values.end())
                {
                    c.text = "not measured: " + g.metric + " needs "
                           + (g.metric == "inharmonic_db" || g.metric == "chord_peak_db" ? "one note" : "another render");
                    continue;
                }
                c.measured = true;
                c.value = it->second;
                c.pass = met(g, c.value);
                c.text = valueText(g.metric, c.value) + "; " + goalText(g);
            }
        }
        return out;
    }
}
