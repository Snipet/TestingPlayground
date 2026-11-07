// Source/dsp/Assets.cpp: see Assets.h.
#include "dsp/Assets.h"

#include "dsp/Sources.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>

namespace playground::assets
{
    namespace
    {
        std::atomic<uint64_t> resampled{ 0 };

        constexpr double kBeta = 10.0;            // the Kaiser window's: its sidelobes about 100 dB down
        constexpr int kPhases = 4096;             // the kernel's table: steps a zero crossing, read between them

        // The modified Bessel function of the first kind, order 0 (the Kaiser window's), by its series.
        double besselI0(double x)
        {
            const double q = 0.25 * x * x;
            double term = 1.0, sum = 1.0;
            for (int k = 1; k < 64 && term > 1.0e-17 * sum; ++k)
            {
                term *= q / (static_cast<double>(k) * static_cast<double>(k));
                sum += term;
            }
            return sum;
        }

        bool endsWith(std::string_view name, std::string_view tail)
        {
            if (name.size() <= tail.size())
                return false;
            for (std::size_t i = 0; i < tail.size(); ++i)
            {
                char c = name[name.size() - tail.size() + i];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                if (c != tail[i])
                    return false;
            }
            return true;
        }

        std::string number(const char* format, double v)
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, format, v);
            return buf;
        }

        std::string rateText(double rate)
        {
            const double k = rate / 1000.0;
            return (std::fabs(k - std::round(k)) < 1.0e-9 ? number("%.0f", k) : number("%.4g", k)) + " kHz";
        }
    }

    bool isAudioName(std::string_view name)
    {
        return endsWith(name, ".wav") || endsWith(name, ".wave") || endsWith(name, ".aif") || endsWith(name, ".aiff")
            || endsWith(name, ".flac");
    }

    bool validName(std::string_view name)
    {
        return validFileName(name) && isAudioName(name);
    }

    std::string tidyName(std::string_view fileName, const std::vector<std::string>& taken)
    {
        if (const std::size_t slash = fileName.find_last_of("/\\"); slash != std::string_view::npos)
            fileName = fileName.substr(slash + 1);
        const std::size_t dot = fileName.rfind('.');
        if (dot == std::string_view::npos || !isAudioName(fileName))
            return {};
        std::string ext(fileName.substr(dot));
        for (char& c : ext)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        std::string stem;
        for (const char c : fileName.substr(0, dot))
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
            if (ok)
                stem += c;
            else if (!stem.empty() && stem.back() != '-')
                stem += '-';
        }
        while (!stem.empty() && (stem.back() == '-' || stem.back() == '.'))
            stem.pop_back();
        while (!stem.empty() && stem.front() == '.')
            stem.erase(stem.begin());
        if (stem.empty() || ext.size() >= kMaxNameLength)
            return {};
        const auto fit = [&](const std::string& tail) {
            std::string s = stem.substr(0, kMaxNameLength - ext.size() - tail.size());
            while (!s.empty() && (s.back() == '-' || s.back() == '.'))
                s.pop_back();
            return s + tail + ext;
        };
        std::string name = fit("");
        for (int n = 2; std::find(taken.begin(), taken.end(), name) != taken.end() && n < 1000; ++n)
            name = fit("-" + std::to_string(n));
        return validName(name) ? name : std::string();
    }

    Audio resample(const Audio& from, double rate)
    {
        resampled.fetch_add(1, std::memory_order_relaxed);
        if (rate == from.rate)
            return from;
        Audio out;
        out.rate = rate;
        out.channels = from.channels;
        if (from.frames <= 0 || from.channels <= 0 || !(from.rate > 0.0) || !(rate > 0.0))
            return out;
        const double ratio = rate / from.rate;                 // output frames an input frame
        const double fc = kCutoff * std::min(1.0, ratio);      // the cutoff, as a share of the input's Nyquist
        const double half = static_cast<double>(kZeros) / fc;  // the kernel's half-width, in input frames
        const auto frames = static_cast<int>(std::clamp(std::llround(static_cast<double>(from.frames) * ratio), 1LL,
                                                        static_cast<long long>(1) << 30));
        out.frames = frames;
        out.data.assign(static_cast<std::size_t>(frames) * static_cast<std::size_t>(from.channels), 0.0f);

        // h(t) = sinc(t) times the Kaiser window over kZeros crossings, t in zero crossings, kPhases steps of each.
        const std::size_t steps = static_cast<std::size_t>(kZeros) * kPhases;
        std::vector<double> table(steps + 2, 0.0);
        const double i0 = besselI0(kBeta);
        for (std::size_t j = 0; j < steps; ++j)
        {
            const double t = static_cast<double>(j) / kPhases;
            const double s = j == 0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
            const double u = t / kZeros;
            table[j] = s * besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - u * u))) / i0;
        }

        const long long last = from.frames - 1;
        for (int c = 0; c < from.channels; ++c)
        {
            const float* in = from.data.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(from.frames);
            float* y = out.data.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(frames);
            for (int n = 0; n < frames; ++n)
            {
                const double p = static_cast<double>(n) * from.rate / rate;   // its time, in input frames
                const long long k0 = std::max(0LL, static_cast<long long>(std::ceil(p - half)));
                const long long k1 = std::min(last, static_cast<long long>(std::floor(p + half)));
                double acc = 0.0;
                for (long long k = k0; k <= k1; ++k)
                {
                    const double x = std::fabs(p - static_cast<double>(k)) * fc * kPhases;
                    const auto j = static_cast<std::size_t>(x);
                    if (j >= steps)
                        continue;
                    const double f = x - static_cast<double>(j);
                    acc += static_cast<double>(in[k]) * (table[j] + f * (table[j + 1] - table[j]));
                }
                y[n] = static_cast<float>(acc * fc);
            }
        }
        return out;
    }

    uint64_t resamples() noexcept
    {
        return resampled.load(std::memory_order_relaxed);
    }

    std::string hashOf(std::string_view bytes)
    {
        uint64_t h = 14695981039346656037ull;
        for (const char c : bytes)
            h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
        return buf;
    }

    // ---- Asset ------------------------------------------------------------------------------------------------------
    struct Asset::Body
    {
        std::string bytes;
        std::string hash;
        std::shared_ptr<const Audio> audio;
        std::mutex mutex;                                     // the rates made
        std::vector<std::shared_ptr<const Audio>> rates;      // the newest last
    };

    Asset::Asset(std::string name, std::string bytes, Audio audio) : name_(std::move(name)), body_(std::make_shared<Body>())
    {
        body_->hash = hashOf(bytes);
        body_->bytes = std::move(bytes);
        body_->audio = std::make_shared<const Audio>(std::move(audio));
    }

    Asset::Asset(std::string name, const Asset& as) : name_(std::move(name)), body_(as.body_) {}

    const std::string& Asset::bytes() const noexcept { return body_->bytes; }
    const std::string& Asset::hash() const noexcept { return body_->hash; }
    const Audio& Asset::audio() const noexcept { return *body_->audio; }

    std::shared_ptr<const Audio> Asset::at(double rate) const
    {
        if (rate == body_->audio->rate)
            return body_->audio;
        const std::lock_guard<std::mutex> lock(body_->mutex);
        std::vector<std::shared_ptr<const Audio>>& made = body_->rates;
        for (std::size_t k = 0; k < made.size(); ++k)
            if (made[k]->rate == rate)
            {
                std::shared_ptr<const Audio> found = made[k];
                made.erase(made.begin() + static_cast<std::ptrdiff_t>(k));
                made.push_back(found);                        // the newest last
                return found;
            }
        auto audio = std::make_shared<const Audio>(resample(*body_->audio, rate));
        if (made.size() >= kRates)
            made.erase(made.begin());
        made.push_back(audio);
        return audio;
    }

    std::string Asset::describe() const
    {
        const Audio& a = audio();
        return number("%.2f s", a.seconds()) + ", " + (a.channels == 1 ? "mono" : a.channels == 2 ? "stereo"
                                                                                  : std::to_string(a.channels) + " channels")
             + ", " + rateText(a.rate) + ", " + sizeText(bytes().size());
    }

    // ---- Set --------------------------------------------------------------------------------------------------------
    std::shared_ptr<const Asset> Set::find(std::string_view name) const
    {
        for (const std::shared_ptr<const Asset>& a : items)
            if (a->name() == name)
                return a;
        return nullptr;
    }

    std::size_t Set::bytes() const noexcept
    {
        std::size_t n = 0;
        for (const std::shared_ptr<const Asset>& a : items)
            n += a->bytes().size();
        return n;
    }

    std::string sizeText(std::size_t bytes)
    {
        if (bytes < 1024)
            return std::to_string(bytes) + " bytes";
        if (bytes < (std::size_t{ 1 } << 20))
            return number("%.0f KB", static_cast<double>(bytes) / 1024.0);
        return number("%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }

    std::string refusal(const Set* to, std::string_view name, std::size_t bytes)
    {
        const std::string n(name);
        if (!isAudioName(name))
            return n + " is not a WAV, AIFF or FLAC file (.wav, .aif, .aiff or .flac): a sample is one of those";
        if (!validFileName(name))
            return n + ": a sample's name has 1 to " + std::to_string(kMaxNameLength)
                 + " letters, digits, _, - and ., not starting with '.'";
        if (bytes > kMaxFileBytes)
            return n + " is " + sizeText(bytes) + ": a sample may be " + sizeText(kMaxFileBytes) + " at most";
        std::size_t total = bytes;
        bool replaces = false;
        if (to != nullptr)
            for (const std::shared_ptr<const Asset>& a : to->items)
            {
                if (a->name() == name)
                    replaces = true;
                else
                    total += a->bytes().size();
            }
        if (to != nullptr && !replaces && to->items.size() >= kMaxAssets)
            return "a program has " + std::to_string(kMaxAssets) + " samples at most: remove one before adding " + n;
        if (total > kMaxTotalBytes)
            return "with " + n + " the samples would take " + sizeText(total) + ": a program's may take "
                 + sizeText(kMaxTotalBytes) + " at most";
        return {};
    }

    std::string refusal(std::string_view name, const Audio& audio)
    {
        const std::string n(name);
        if (audio.frames <= 0 || audio.channels <= 0 || !(audio.rate > 0.0))
            return n + " holds no sound";
        if (audio.channels > kMaxChannels)
            return n + " has " + std::to_string(audio.channels) + " channels: a sample has one or two";
        if (audio.seconds() > kMaxSeconds)
            return n + " is " + number("%.1f s", audio.seconds()) + " long: a sample may be " + number("%.0f s", kMaxSeconds)
                 + " at most";
        return {};
    }

    std::shared_ptr<const Set> with(const std::shared_ptr<const Set>& set, std::shared_ptr<const Asset> asset)
    {
        auto out = std::make_shared<Set>();
        bool placed = false;
        if (set != nullptr)
        {
            for (const std::shared_ptr<const Asset>& a : set->items)
            {
                if (a->name() == asset->name())
                {
                    out->items.push_back(asset);
                    placed = true;
                }
                else
                    out->items.push_back(a);
            }
        }
        if (!placed)
            out->items.push_back(std::move(asset));
        return out;
    }

    std::shared_ptr<const Set> without(const std::shared_ptr<const Set>& set, std::string_view name)
    {
        auto out = std::make_shared<Set>();
        if (set != nullptr)
            for (const std::shared_ptr<const Asset>& a : set->items)
                if (a->name() != name)
                    out->items.push_back(a);
        return out;
    }

    std::shared_ptr<const Set> renamed(const std::shared_ptr<const Set>& set, std::string_view from, std::string_view to)
    {
        auto out = std::make_shared<Set>();
        if (set != nullptr)
        {
            for (const std::shared_ptr<const Asset>& a : set->items)
            {
                if (a->name() == from)
                    out->items.push_back(std::make_shared<const Asset>(std::string(to), *a));
                else if (a->name() != to)
                    out->items.push_back(a);
            }
        }
        return out;
    }

    bool same(const Set* a, const Set* b)
    {
        const std::size_t na = a != nullptr ? a->items.size() : 0, nb = b != nullptr ? b->items.size() : 0;
        if (na != nb)
            return false;
        for (std::size_t k = 0; k < na; ++k)
        {
            const std::shared_ptr<const Asset> other = b->find(a->items[k]->name());
            if (other == nullptr || other->hash() != a->items[k]->hash())
                return false;
        }
        return true;
    }
}
