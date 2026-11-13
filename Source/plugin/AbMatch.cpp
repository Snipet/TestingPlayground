// Source/plugin/AbMatch.cpp: see AbMatch.h.
#include "plugin/AbMatch.h"

#include "dsp/Faults.h"
#include "dsp/Measure.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace playground::ab
{
    namespace
    {
        constexpr double kFromSeconds = 0.05;                 // the loudness from here (a note's or the noise's onset)
        constexpr double kHoldSeconds = 1.5;                  // an instrument's chord
        constexpr double kNoiseSeconds = 2.0;                 // an effect's input
        constexpr double kNoiseRmsDb = -18.0;

        // Pink noise (Paul Kellett's three-pole filter over a fixed LCG's white), at kNoiseRmsDb RMS.
        [[maybe_unused]] std::vector<float> pinkNoise(std::size_t n)
        {
            std::vector<float> out(n);
            uint32_t seed = 0x2545F491u;
            double b0 = 0.0, b1 = 0.0, b2 = 0.0, energy = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                const double white = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;   // [-1, 1)
                b0 = 0.99765 * b0 + white * 0.0990460;
                b1 = 0.96300 * b1 + white * 0.2965164;
                b2 = 0.57000 * b2 + white * 1.0526913;
                const double pink = b0 + b1 + b2 + white * 0.1848;
                out[i] = static_cast<float>(pink);
                energy += pink * pink;
            }
            const double rms = n > 0 ? std::sqrt(energy / static_cast<double>(n)) : 0.0;
            const double gain = rms > 0.0 ? std::pow(10.0, kNoiseRmsDb / 20.0) / rms : 0.0;
            for (float& x : out)
                x = static_cast<float>(x * gain);
            return out;
        }
    }

    double loudness(const Sources& files, const std::map<std::string, float>& dials,
                    const std::map<std::string, float>& current, double rate, std::string* why, const Assets& assets)
    {
        measure::Session session(files, rate, nullptr, assets);
        if (!session.compiled())
        {
            if (why != nullptr)
            {
                const std::string& e = session.errors();
                *why = "does not compile" + (e.empty() ? std::string() : ": " + e.substr(0, e.find('\n')));
            }
            return std::numeric_limits<double>::quiet_NaN();
        }
        measure::Request request;
        request.rate = rate;
        request.dials = dials;
        request.current = current;
        measure::Session::Render played;
        if constexpr (product::kIsInstrument)
        {
            request.notes = { 48, 52, 55, 60 };
            request.velocity = 0.8f;
            request.holdSeconds = static_cast<float>(kHoldSeconds);
            request.tailSeconds = 0.0f;
            played = session.render(request, nullptr, nullptr, 0);
        }
        else
        {
            const auto n = static_cast<std::size_t>(kNoiseSeconds * rate);
            const std::vector<float> noise = pinkNoise(n);
            request.holdSeconds = static_cast<float>(kNoiseSeconds);
            request.tailSeconds = 0.0f;
            played = session.render(request, &noise, &noise, n);
        }
        if (played.stopCall != Patch::Method::none)
        {
            // It stopped partway (Patch.h, "The loop budget", "Bounds checks"): what follows is its fade, then silence.
            if (why != nullptr)
                *why = "stops in the test (a chord, or noise through it): " + Patch::stopWhy(played.stopCall, &played.stopBounds);
            return std::numeric_limits<double>::quiet_NaN();
        }
        const std::size_t end = std::min(played.l.size(), played.heldUntil > 0 ? played.heldUntil : played.l.size());
        const auto from = static_cast<std::size_t>(kFromSeconds * rate);
        if (end <= from)
            return -240.0;
        return measure::loudness(played.l, played.r, rate, { { from, end } });
    }

    std::array<float, 2> trims(const std::array<double, 2>& lufs)
    {
        std::array<float, 2> out{ 0.0f, 0.0f };
        if (!std::isfinite(lufs[0]) || !std::isfinite(lufs[1]) || lufs[0] < kSilentLufs || lufs[1] < kSilentLufs)
            return out;
        const std::size_t louder = lufs[0] > lufs[1] ? 0 : 1;
        const double down = lufs[1 - louder] - lufs[louder];
        out[louder] = static_cast<float>(std::max(static_cast<double>(kMaxTrimDb), down));
        return out;
    }

    std::optional<Result> measure(const Job& job, const std::function<bool()>& stale)
    {
        Result r;
        r.id = job.id;
        std::array<std::string, 2> failed;
        for (std::size_t k = 0; k < 2; ++k)
        {
            if (stale && stale())
                return std::nullopt;
            r.lufs[k] = loudness(job.files[k], job.dials[k], job.current, job.rate, &failed[k], job.assets);
        }
        r.trimDb = trims(r.lufs);
        const char* names[2] = { "A", "B" };
        for (std::size_t k = 0; k < 2 && r.why.empty(); ++k)
            if (!std::isfinite(r.lufs[k]))
                r.why = std::string(names[k]) + " " + (failed[k].empty() ? std::string("does not compile") : failed[k]);
            else if (r.lufs[k] < kSilentLufs)
                r.why = std::string(names[k]) + " is silent in the test (a chord, or noise through it): the loudness is not matched";
        if (r.why.empty() && (r.trimDb[0] <= kMaxTrimDb || r.trimDb[1] <= kMaxTrimDb))
            r.why = "They differ by more than 24 dB: the louder is turned down 24 dB";
        return r;
    }

    // ---- Matcher -----------------------------------------------------------------------------------------------------
    Matcher::~Matcher()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            wanted_ = 0;
            waiting_.reset();
        }
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    void Matcher::start(Job job)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            wanted_ = job.id;
            waiting_ = std::move(job);
            done_.reset();
            if (!thread_.joinable())
                thread_ = std::thread([this] { loop(); });
        }
        wake_.notify_all();
    }

    bool Matcher::take(Result& out)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!done_)
            return false;
        out = std::move(*done_);
        done_.reset();
        return true;
    }

    void Matcher::cancel()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        wanted_ = 0;
        waiting_.reset();
        done_.reset();
    }

    bool Matcher::busy() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return waiting_.has_value() || running_;
    }

    void Matcher::loop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;)
        {
            wake_.wait(lock, [this] { return stop_ || waiting_.has_value(); });
            if (stop_)
                return;
            const Job job = std::move(*waiting_);
            waiting_.reset();
            running_ = true;
            lock.unlock();
            std::optional<Result> r;
            try
            {
                r = measure(job, [this, id = job.id] {
                    const std::lock_guard<std::mutex> l(mutex_);
                    return stop_ || wanted_ != id;
                });
            }
            catch (...)
            {
                // The measure failed inside the plugin (out of memory): not matched, saying why, and never past this
                // thread (std::terminate would end the host).
                try
                {
                    Result failed;
                    failed.id = job.id;
                    failed.why = "The loudness could not be measured: it failed inside the plugin ("
                               + faults::words(std::current_exception()) + ")";
                    r = std::move(failed);
                }
                catch (...)
                {
                    r.reset();
                }
            }
            lock.lock();
            running_ = false;
            if (r && wanted_ == job.id && !stop_)
                done_ = *r;
        }
    }
}
