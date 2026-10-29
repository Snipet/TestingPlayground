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
        const capture::Reply reply = host.chatCapture(capture::Action::take, {});
        if (reply.take == nullptr)
        {
            why = reply.state == capture::State::off
                      ? "nothing recorded yet: capture arm records what the user plays (tell them first), and capture stop keeps it"
                      : "the recording has not been stopped yet: capture stop keeps it, then it replays";
            return false;
        }
        const capture::Take& t = *reply.take;
        capture::Replay replay;
        if (!capture::replayOf(t, from, to, replay, why))
        {
            why = "the recording: " + why;
            return false;
        }

        // How closely a replay gives what played: tested once a take (the whole of it), then kept with it.
        std::string fidelity = reply.replay;
        if (fidelity.empty())
        {
            capture::Replay whole;
            std::string unused;
            fidelity = capture::replayOf(t, 0.0, -1.0, whole, unused) ? capture::nullTest(t, whole).line
                                                                     : "Replay: nothing to replay.";
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
        out.l = std::move(replay.played[0]);
        out.r = std::move(replay.played[1]);
        out.about = about;
        return true;
    }
}
