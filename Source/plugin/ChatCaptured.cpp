// Source/plugin/ChatCaptured.cpp: scenario::captured (ChatScenario.h), the one seam the sources "capture" go
// through: a scenario's phrase or input, compare's a or b, trace's source. It replays the take the recorder keeps
// (capture stop's; plugin/CaptureReplay.h): `request` plays it again as the processor played it (its rate, its host
// blocks and parts, its events, transport and dials, the Engine's clock, an effect's input), `l` and `r` are what
// played. The first replay of a take null-tests a replay of the program that played against what played (a compile and
// a render, once), and the recorder keeps the line with the take (capture::Action::replayed): capture stop and status
// report it, and every replay's `about` says it.
#include "plugin/ChatScenario.h"

#include "plugin/CaptureReplay.h"

#include "Product.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <tuple>

namespace playground::scenario
{
    namespace
    {
        [[maybe_unused]] std::string noteName(int n)
        {
            static constexpr const char* kNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            return std::string(kNames[std::clamp(n, 0, 127) % 12]) + std::to_string(std::clamp(n, 0, 127) / 12 - 1);
        }

        std::string number(const char* f, double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, f, v);
            return buf;
        }
    }

    bool captured(ChatHost& host, Captured& out, std::string& why, double from, double to)
    {
        measure::Request part;
        part.captureFrom = from;
        part.captureTo = to;
        return captured(host, out, why, part);
    }

    bool captured(ChatHost& host, Captured& out, std::string& why, const measure::Request& part)
    {
        double from = part.captureFrom, to = part.captureTo;
        const capture::Reply reply = host.chatCapture(capture::Action::take, {});
        if (reply.take == nullptr)
        {
            why = reply.state == capture::State::off
                      ? "nothing recorded yet: capture arm records what the user plays (tell them first), and capture stop keeps it"
                      : "the recording has not been stopped yet: capture stop keeps it, then it replays";
            return false;
        }
        const capture::Take& t = *reply.take;
        // The user's marks the part names, at their times in the take.
        for (const auto& [mark, into, key] : { std::tuple<int, double*, const char*>{ part.captureFromMark, &from, "from" },
                                               { part.captureToMark, &to, "to" } })
        {
            if (mark <= 0)
                continue;
            const int wanted = mark;
            const auto m = std::find_if(t.marks.begin(), t.marks.end(), [wanted](const capture::Mark& x) { return x.number == wanted; });
            if (m == t.marks.end())
            {
                std::string have;
                for (const capture::Mark& x : t.marks)
                    have += (have.empty() ? "" : ", ") + ("m" + std::to_string(x.number));
                why = std::string(key) + " is m" + std::to_string(mark) + ", but the recording "
                    + (have.empty() ? std::string("has no marks") : "has " + have)
                    + (t.marksBefore > 0 ? " (" + std::to_string(t.marksBefore) + " fell before the part it kept)" : std::string());
                return false;
            }
            *into = static_cast<double>(m->sample) / t.rate;
        }
        if (part.captureFromMark > 0 && part.captureToMark > 0 && !(to > from))
        {
            why = "to's mark is not after from's";
            return false;
        }
        capture::Replay replay;
        if (!capture::replayOf(t, from, to, replay, why))
        {
            why = "the recording: " + why;
            return false;
        }
        replay.request.assets = host.chatAssets();           // the samples as they are (a take keeps none of its own)

        // How closely a replay gives what played: tested once a take (the whole of it), then kept with it.
        std::string fidelity = reply.replay;
        if (fidelity.empty())
        {
            capture::Replay whole;
            std::string unused;
            if (capture::replayOf(t, 0.0, -1.0, whole, unused))
            {
                whole.request.assets = host.chatAssets();    // after replayOf, which starts from an empty Replay
                fidelity = capture::nullTest(t, whole).line;
            }
            else
                fidelity = "Replay: nothing to replay.";
            capture::Options keep;
            keep.take = reply.take;
            keep.replay = fidelity;
            host.chatCapture(capture::Action::replayed, keep);
        }

        // What it is, in a line.
        int notes = 0, lo = 128, hi = -1;
        for (const auto& [at, e] : replay.request.events)
            if (e.kind == EventKind::noteOn)
            {
                ++notes;
                lo = std::min(lo, static_cast<int>(e.note));
                hi = std::max(hi, static_cast<int>(e.note));
            }
        const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - t.madeAt).count() + t.endedAgo;
        std::string about = "the user's recording, " + number("%.2f s", static_cast<double>(replay.length) / t.rate);
        if (!replay.fromStart || replay.last < t.blocks.size())
            about += number(" of its %.2f s", t.seconds()) + number(" (from %.2f s", static_cast<double>(replay.from) / t.rate)
                   + number(" to %.2f s)", static_cast<double>(replay.from + replay.length) / t.rate);
        about += number(" at %.0f Hz", t.rate);
        if constexpr (product::kIsInstrument)
            about += notes == 0 ? std::string(", no notes")
                                : ", " + std::to_string(notes) + (notes == 1 ? " note" : " notes")
                                      + (lo == hi ? " (" + noteName(lo) + ")" : " (" + noteName(lo) + " to " + noteName(hi) + ")");
        else
            about += ", its input as it came";
        about += number(", ended %.0f s ago", age);
        std::string said = fidelity.rfind("Replay: ", 0) == 0 ? fidelity.substr(8) : fidelity;
        if (!said.empty() && said.back() == '.')
            said.pop_back();
        about += "; its replay " + said;
        if (!replay.fromStart)
            about += "; this part starts mid-take, where a replay starts afresh";

        out.request = std::move(replay.request);
        out.request.recording = about;
        out.files = replay.files != nullptr ? *replay.files : Sources{};
        out.pins = replay.files != nullptr && replay.pins != nullptr ? *replay.pins : CircuitPins{};
        out.l = std::move(replay.played[0]);
        out.r = std::move(replay.played[1]);
        out.about = about;
        return true;
    }

    // ---- resolve and evaluate: the plugin's side of a render's settings (ChatScenario.h) -------------------------
    bool resolve(measure::Request& r, ChatHost& host, std::string& why) { return resolve(r, host, why, nullptr); }

    bool resolve(measure::Request& r, ChatHost& host, std::string& why, const Captured* recording)
    {
        r.assets = host.chatAssets();                        // the program's samples, whatever renders it
        if (r.hostRate)
            if (const double rate = host.chatHostRate(); rate > 1000.0)
                r.rate = std::clamp(rate, kMinRate, kMaxRate);
        if (r.hostBlock)
            if (const int block = host.chatHostBlock(); block > 0)
                r.block = std::clamp(block, 1, measure::kMaxBlock);
        if (r.fromCapture)
        {
            Captured c;
            if (recording == nullptr)
            {
                if (!captured(host, c, why, r))
                    return false;
                recording = &c;
            }
            const measure::Request& k = recording->request;
            r.rate = k.rate;
            r.block = k.block;
            r.blocks = k.blocks;
            r.events = k.events;
            r.transport = k.transport;
            const auto bySample = [](const auto& x, const auto& y) { return x.first < y.first; };
            std::stable_sort(r.events.begin(), r.events.end(), bySample);
            std::stable_sort(r.transport.begin(), r.transport.end(), bySample);
            r.input = k.input;
            r.frames = k.frames;                             // its dials, clock and parts: a replay gives what played
            r.frameControls = k.frameControls;
            r.clock = k.clock;
            r.part = k.part;
            r.recording = k.recording;
            r.modwheel = k.modwheel;
            r.pitchbend = k.pitchbend;
            for (const auto& [name, v] : k.current)          // its dials at its start, for the report's line
                r.current[name] = v;
            r.phrase.clear();
            std::map<std::string, float> dials = k.dials;          // the recording's at its start, under the request's own
            for (const auto& [name, x] : r.dials)
                dials[name] = x;
            r.dials = std::move(dials);
            std::vector<measure::DialMove> moves = k.automate;   // the recording's, then the request's own
            moves.insert(moves.end(), r.automate.begin(), r.automate.end());
            r.automate = std::move(moves);
        }
        return fits(r, why);
    }

    std::vector<measure::CheckResult> evaluate(const Sources& files, const std::vector<measure::Check>& checks,
                                               const std::map<std::string, float>& current, ChatHost& host, std::string* errors,
                                               const measure::Cancel* cancel)
    {
        std::vector<measure::CheckResult> out(checks.size());
        std::vector<measure::Check> ready;
        std::vector<int> at(checks.size(), -1);
        for (std::size_t i = 0; i < checks.size(); ++i)
        {
            measure::Check k = checks[i];
            std::string why;
            if (!resolve(k.request, host, why))
            {
                out[i].text = "not measured: " + why;
                continue;
            }
            at[i] = static_cast<int>(ready.size());
            ready.push_back(std::move(k));
        }
        const std::vector<measure::CheckResult> measured = measure::evaluate(files, ready, current, errors, cancel);
        for (std::size_t i = 0; i < checks.size(); ++i)
            if (at[i] >= 0 && static_cast<std::size_t>(at[i]) < measured.size())
                out[i] = measured[static_cast<std::size_t>(at[i])];
        return out;
    }
}
