// Source/plugin/CaptureReplay.cpp: see CaptureReplay.h.
#include "plugin/CaptureReplay.h"

#include "dsp/Compare.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <numbers>

namespace playground::capture
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr std::size_t kWindow = 4096;               // the spectrum's windows (Hann)
        constexpr std::size_t kWindows = 48;                // at most, spread over the sound

        __attribute__((format(printf, 1, 2))) std::string fmt(const char* f, ...)
        {
            char buf[256];
            va_list args;
            va_start(args, f);
            std::vsnprintf(buf, sizeof buf, f, args);
            va_end(args);
            return buf;
        }

        double dbOf(double linear) { return linear > 1e-12 ? 20.0 * std::log10(linear) : -240.0; }

        Transport transportOf(const Block& b)
        {
            Transport t;
            t.known = b.has(Block::kKnown);
            t.bpm = b.bpm;
            t.beat = b.beat;
            t.barStart = b.barStart;
            t.beatsPerBar = b.beatsPerBar;
            t.beatUnit = b.beatUnit;
            t.playing = b.has(Block::kPlaying);
            return t;
        }

        // Why a replay differs where it does (the first that applies), in words.
        std::string causeOf(const Take& t, const Replay& r, double firstDiffers)
        {
            const double block = !r.request.blocks.empty() ? static_cast<double>(r.request.blocks.back()) / t.rate : 0.0;
            if (r.swappedAt >= 0.0 && firstDiffers >= r.swappedAt - block)
                return fmt("another program came in at %.2f s (a run), and a replay plays the one at its start throughout", r.swappedAt);
            if (r.frozenFrom >= 0.0 && firstDiffers >= r.frozenFrom - block)
                return fmt("the frozen copy played from %.2f s (Math::Fast: its last bits are not the program's own)", r.frozenFrom);
            if (!r.fromStart)
                return "this part starts in the middle of the take, on the state the program had there, which a replay starts "
                       "without";
            if (t.start == Start::arm && t.fromSeconds > 0.0)
                return "it started in the middle of the playing (mode last), on the program's state then, not a fresh one";
            if (kInstrument && t.voicesAtStart > 0)
                return fmt("%d voice%s still sounded when it started (the program's state was not fresh)", t.voicesAtStart,
                           t.voicesAtStart == 1 ? "" : "s");
            if (!kInstrument && t.peakBefore > 1e-5f && t.start != Start::arm)
                return "a tail still rang when it started (the program's state was not fresh)";
            if (t.start == Start::arm && !t.restarted)
                return "it started in the middle of the playing (mode last), on the program's state then, not a fresh one";
            if (t.restarted)
                return "the program started afresh, yet its output differs: it reads something a recording does not hold (a "
                       "clock or a state of its own that a fresh compile does not reset)";
            return "the program's state at the start was not a fresh one (what played before left its mark)";
        }

        bool stateCause(const Take& t, const Replay& r, double firstDiffers)
        {
            const double block = !r.request.blocks.empty() ? static_cast<double>(r.request.blocks.back()) / t.rate : 0.0;
            return !(r.swappedAt >= 0.0 && firstDiffers >= r.swappedAt - block)
                && !(r.frozenFrom >= 0.0 && firstDiffers >= r.frozenFrom - block) && !t.restarted && r.fromStart;
        }

        const char* const kExact = " For an exact take: capture arm with mode next and restart (ask the user first: it cuts "
                                   "what still sounds).";
    }

    bool replayOf(const Take& t, double from, double to, Replay& out, std::string& why, bool samples)
    {
        out = Replay{};
        const uint64_t total = t.samples();
        if (t.blocks.empty() || total == 0 || !(t.rate > 0.0))
        {
            why = "the recording holds no audio";
            return false;
        }
        const double rate = t.rate;
        const uint64_t a = from > 0.0 && std::isfinite(from) ? static_cast<uint64_t>(std::llround(from * rate)) : 0;
        const uint64_t b = to >= 0.0 && std::isfinite(to) ? std::min<uint64_t>(total, static_cast<uint64_t>(std::llround(to * rate))) : total;
        if (a >= total)
        {
            why = fmt("from %.2f s is past its end (it is %.2f s long)", from, t.seconds());
            return false;
        }
        if (b <= a)
        {
            why = "to must come after from";
            return false;
        }
        // Its host blocks: from the one a is in to the one b - 1 is in.
        const std::size_t n = t.blocks.size();
        std::size_t first = 0;
        while (first + 1 < n && t.blocks[first + 1].sample <= a)
            ++first;
        std::size_t last = first;
        while (last < n && t.blocks[last].sample < b)
            ++last;
        const uint64_t s0 = t.blocks[first].sample, s1 = last < n ? t.blocks[last].sample : total;
        out.first = first;
        out.last = last;
        out.from = static_cast<std::size_t>(s0);
        out.length = static_cast<std::size_t>(s1 - s0);
        out.fromStart = first == 0;
        out.program = t.blocks[first].program;
        for (std::size_t k = 0; k < t.programs.size(); ++k)
            if (t.programs[k].first == out.program)
                out.files = &t.programs[k].second;
        for (std::size_t k = 0; k < t.programDials.size(); ++k)
            if (t.programDials[k].first == out.program)
                out.controls = &t.programDials[k].second;
        for (std::size_t k = 0; k < t.programPins.size(); ++k)
            if (t.programPins[k].first == out.program)
                out.pins = &t.programPins[k].second;

        measure::Request& q = out.request;
        q.rate = rate;
        q.tailSeconds = 0.0f;
        q.part = t.part;
        q.clock = t.clock + s0;
        int lastSize = measure::kDefaultBlock;
        std::array<float, kMaxControls> dials{};
        for (std::size_t k = first; k < last; ++k)
        {
            const Block& x = t.blocks[k];
            const std::size_t at = static_cast<std::size_t>(x.sample - s0);
            q.transport.push_back({ at, transportOf(x) });
            if (k == first || x.dials != dials)
            {
                q.frames.push_back({ at, x.dials });
                dials = x.dials;
            }
            for (uint32_t left = x.size; left > 0;)             // a block of 0 samples: none (the next takes its values)
            {
                const auto size = static_cast<int>(std::min<uint32_t>(left, static_cast<uint32_t>(measure::kMaxBlock)));
                q.blocks.push_back(size);
                lastSize = size;
                left -= static_cast<uint32_t>(size);
            }
            if (out.swappedAt < 0.0 && x.program != out.program)
                out.swappedAt = static_cast<double>(at) / rate;
            if (out.frozenFrom < 0.0 && x.has(Block::kFrozen))
                out.frozenFrom = static_cast<double>(at) / rate;
        }
        q.block = std::clamp(lastSize, 1, measure::kMaxBlock);
        {
            // What the host would send next, for a tail after it.
            const Block& x = t.blocks[last - 1];
            Transport e = transportOf(x);
            if (e.known && e.playing)
                e.beat += static_cast<double>(x.size) * e.bpm / (60.0 * rate);
            q.transport.push_back({ out.length, e });
        }
        if (out.controls != nullptr)
        {
            q.frameControls = std::make_shared<const Controls>(*out.controls);
            for (int c = 0; c < kMaxControls; ++c)                // the report's dials: as they stood at its start
            {
                const Control& k = (*out.controls)[static_cast<std::size_t>(c)];
                if (k.used)
                    q.current[k.name] = plainOf(k, t.blocks[first].dials[static_cast<std::size_t>(c)]);
            }
        }
        if constexpr (kInstrument)
        {
            // The wheels and the pedal as they stood, always (whatever a render before left; and so a take without MIDI
            // is still a recording, not the default notes: Measure's noteParts), then what the Engine got.
            const std::array<float, 4>& w = t.blocks[first].wheels;
            q.modwheel = w[0];
            q.pitchbend = w[1];
            q.events.push_back({ 0, Event{ 0, EventKind::pressure, 0, w[2] } });
            q.events.push_back({ 0, Event{ 0, EventKind::controller, 64, w[3] >= 0.5f ? 1.0f : 0.0f } });
            for (const Note& x : t.events)
                if (x.sample >= s0 && x.sample < s1)
                    q.events.push_back({ static_cast<std::size_t>(x.sample - s0), x.event(0) });
        }
        else if (samples && t.in[0].size() >= s1)
        {
            auto in = std::make_shared<measure::Input>();
            in->l.assign(t.in[0].begin() + static_cast<std::ptrdiff_t>(s0), t.in[0].begin() + static_cast<std::ptrdiff_t>(s1));
            in->r.assign(t.in[1].begin() + static_cast<std::ptrdiff_t>(s0), t.in[1].begin() + static_cast<std::ptrdiff_t>(s1));
            q.input = std::move(in);
        }
        for (std::size_t c = 0; c < 2 && samples; ++c)
            out.played[c].assign(t.out[c].begin() + static_cast<std::ptrdiff_t>(s0), t.out[c].begin() + static_cast<std::ptrdiff_t>(s1));
        return true;
    }

    Fidelity nullTest(const Take& t, const Replay& r)
    {
        Fidelity f;
        if (r.files == nullptr || r.files->empty())
        {
            f.line = "Replay: cannot be made: the files of the program that played are not known (it played before the "
                     "recorder knew it).";
            return f;
        }
        measure::Session session(*r.files, t.rate, r.pins);
        if (!session.compiled())
        {
            const std::string e = session.errors();
            f.line = "Replay: cannot be made: the program that played does not compile here (" + e.substr(0, e.find('\n')) + ").";
            return f;
        }
        measure::Session::Render played = session.render(r.request, nullptr, nullptr, r.length);
        f.rendered = true;
        std::vector<float>& L = played.l;
        std::vector<float>& R = played.r;
        L.resize(r.length, 0.0f);
        R.resize(r.length, 0.0f);
        if (t.channels == 1)                                 // a mono layout: the Engine's one channel
        {
            if constexpr (kInstrument)
                for (std::size_t i = 0; i < r.length; ++i)
                    L[i] = 0.5f * (L[i] + R[i]);
            R = L;
        }
        const std::vector<float>& A = r.played[0];
        const std::vector<float>& B = r.played[1];
        const std::size_t n = std::min({ r.length, A.size(), B.size() });
        f.identical = n == r.length && std::memcmp(L.data(), A.data(), n * sizeof(float)) == 0
                   && std::memcmp(R.data(), B.data(), n * sizeof(float)) == 0;
        const double rate = t.rate;
        if (f.identical)
        {
            f.matchesFrom = 0.0;
            f.line = "Replay: matches what played bit for bit.";
            return f;
        }
        const double limit = std::pow(10.0, kMatchDb / 20.0);
        long firstBad = -1, lastBad = -1;
        double worst = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double d = std::max(std::fabs(static_cast<double>(L[i]) - A[i]), std::fabs(static_cast<double>(R[i]) - B[i]));
            worst = std::max(worst, d);
            if (d > limit)
            {
                if (firstBad < 0)
                    firstBad = static_cast<long>(i);
                lastBad = static_cast<long>(i);
            }
        }
        f.worstDb = dbOf(worst);
        if (firstBad < 0)
        {
            f.matchesFrom = 0.0;
            f.line = fmt("Replay: matches what played to %.0f dB throughout (not bit for bit: at most %.0f dBFS apart).", kMatchDb,
                         f.worstDb);
            return f;
        }
        f.firstDiffers = static_cast<double>(firstBad) / rate;
        const std::size_t tail = n - static_cast<std::size_t>(lastBad + 1);
        const std::string cause = causeOf(t, r, f.firstDiffers);
        const std::string hint = stateCause(t, r, f.firstDiffers) ? kExact : "";
        if (tail >= static_cast<std::size_t>(0.05 * rate))
        {
            f.matchesFrom = static_cast<double>(lastBad + 1) / rate;
            f.line = fmt("Replay: matches what played to %.0f dB from %.2f s; before that it differs (up to %.0f dBFS, from %.2f s): ",
                         kMatchDb, f.matchesFrom, f.worstDb, f.firstDiffers)
                   + cause + "." + hint;
        }
        else
            f.line = fmt("Replay: differs from %.2f s to its end (up to %.0f dBFS): ", f.firstDiffers, f.worstDb) + cause + "." + hint;
        return f;
    }

    std::string forecast(const Take& t)
    {
        Replay r;
        std::string why;
        if (!replayOf(t, 0.0, -1.0, r, why, false))
            return "Replay: nothing to replay (" + why + ").";
        if (r.files == nullptr)
            return "Replay: cannot be made: the files of the program that played are not known.";
        if (r.frozenFrom >= 0.0)
            return fmt("Replay: it will differ from %.2f s, where the frozen copy played (not the program's own code); a "
                       "render of \"capture\" null-tests it.", r.frozenFrom);
        if (r.swappedAt >= 0.0)
            return fmt("Replay: plays the program at its start throughout, so from %.2f s (another program came in) it "
                       "differs; a render of \"capture\" null-tests it.", r.swappedAt);
        const bool fresh = t.restarted || (t.start != Start::arm && t.voicesAtStart == 0 && (kInstrument || t.peakBefore <= 1e-5f));
        if (fresh)
            return std::string("Replay: ")
                 + (t.restarted ? "it started on the program's fresh state" : kInstrument ? "no voice sounded when it started"
                                                                                           : "no tail rang when it started")
                 + " and the program's own code played, so a replay should match it bit for bit"
                 + (t.restarted ? "" : " (unless the program keeps a state from before, as an LFO's phase)")
                 + "; the first render of \"capture\" null-tests it.";
        const std::string why2 = t.start == Start::arm && t.fromSeconds > 0.0 ? std::string("it started mid-playing (mode last)")
                               : kInstrument && t.voicesAtStart > 0 ? fmt("%d voice%s still sounded when it started", t.voicesAtStart,
                                                                          t.voicesAtStart == 1 ? "" : "s")
                               : !kInstrument && t.peakBefore > 1e-5f && t.start != Start::arm ? std::string("a tail still rang when it started")
                                                                                               : std::string("it started mid-playing (mode last)");
        return "Replay: " + why2 + ", so a replay differs at first; the first render of \"capture\" null-tests it." + kExact;
    }

    Spectrum spectrumOf(const std::vector<float>& l, const std::vector<float>& r, double rate)
    {
        Spectrum s;
        s.bandsDb.fill(-240.0);
        const std::size_t total = std::min(l.size(), r.size());
        double ll = 0.0, rr = 0.0, lr = 0.0;
        for (std::size_t i = 0; i < total; ++i)
        {
            const double x = l[i], y = r[i];
            if (!std::isfinite(x) || !std::isfinite(y))
                continue;
            ll += x * x;
            rr += y * y;
            lr += x * y;
        }
        s.silent = !(ll + rr > 1e-18);
        s.correlation = ll > 1e-18 && rr > 1e-18 ? lr / std::sqrt(ll * rr) : 1.0;
        if (s.silent || total < kWindow || !(rate > 0.0))
            return s;
        // Hann windows of the mono sum, spread evenly over it.
        const std::size_t count = std::min(kWindows, (total - kWindow) / (kWindow / 2) + 1);
        const std::size_t span = total - kWindow;
        std::vector<double> win(kWindow);
        double w2 = 0.0;
        for (std::size_t i = 0; i < kWindow; ++i)
        {
            win[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(kWindow));
            w2 += win[i] * win[i];
        }
        std::vector<double> power(kWindow / 2, 0.0);
        std::vector<std::complex<double>> a(kWindow);
        for (std::size_t w = 0; w < count; ++w)
        {
            const std::size_t at = count > 1 ? span * w / (count - 1) : 0;
            for (std::size_t i = 0; i < kWindow; ++i)
            {
                const double x = 0.5 * (static_cast<double>(l[at + i]) + static_cast<double>(r[at + i]));
                a[i] = { std::isfinite(x) ? x * win[i] : 0.0, 0.0 };
            }
            measure::fft(a);
            for (std::size_t k = 1; k < kWindow / 2; ++k)
                power[k] += std::norm(a[k]);
        }
        // Each bin's share of the mean square (Parseval, the window's power taken out), averaged over the windows.
        const double scale = 2.0 / (static_cast<double>(kWindow) * w2 * static_cast<double>(count));
        std::array<double, 10> bands{};
        double weighted = 0.0, sum = 0.0;
        for (std::size_t k = 1; k < kWindow / 2; ++k)
        {
            const double hz = static_cast<double>(k) * rate / static_cast<double>(kWindow);
            const double ms = power[k] * scale;
            const double mag = std::sqrt(power[k]);
            weighted += hz * mag;
            sum += mag;
            for (std::size_t b = 0; b < bands.size(); ++b)
            {
                const double c = measure::Comparison::kBands[b];
                if (hz >= c / std::numbers::sqrt2 && hz < c * std::numbers::sqrt2)
                    bands[b] += ms;
            }
        }
        for (std::size_t b = 0; b < bands.size(); ++b)
            s.bandsDb[b] = bands[b] > 1e-24 ? 10.0 * std::log10(bands[b]) : -240.0;
        s.centroidHz = sum > 0.0 ? weighted / sum : 0.0;
        return s;
    }

    measure::Timeline timelineOf(const Take& t, const Replay& r, const Controls& controls)
    {
        measure::Timeline tl = measure::timeline(r.request, r.controls != nullptr ? *r.controls : controls, t.rate, r.length);
        for (std::size_t k = r.first + 1; k < r.last; ++k)
        {
            const Block& a = t.blocks[k - 1];
            const Block& b = t.blocks[k];
            const auto at = static_cast<std::size_t>(b.sample - r.from);
            if (b.program != a.program)
                tl.marks.push_back({ at, "another program coming in (a run: its swap fades over 20 ms)", -1, false });
            if (b.has(Block::kFrozen) != a.has(Block::kFrozen))
                tl.marks.push_back({ at, b.has(Block::kFrozen) ? "the frozen copy starting to play" : "the program's own code "
                                                                                                       "playing again",
                                     -1, false });
            if (b.has(Block::kBypassed) != a.has(Block::kBypassed))
                tl.marks.push_back({ at, b.has(Block::kBypassed) ? "the bypass turning on" : "the bypass turning off", -1, false });
        }
        std::stable_sort(tl.marks.begin(), tl.marks.end(), [](const measure::Mark& x, const measure::Mark& y) { return x.at < y.at; });
        return tl;
    }
}
