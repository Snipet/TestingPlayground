// Source/plugin/ChatCapture.cpp: the capture tool: see ChatCapture.h, and Capture.h for the recorder.
#include "plugin/ChatCapture.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace playground
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr std::size_t kDialsShown = 6;              // dials named in the report (then "and N more")

        __attribute__((format(printf, 1, 2))) std::string fmt(const char* f, ...)
        {
            char buf[512];
            va_list args;
            va_start(args, f);
            std::vsnprintf(buf, sizeof buf, f, args);
            va_end(args);
            return buf;
        }

        juce::var prop(const char* type, const char* description)
        {
            return claude::object({ { "type", type }, { "description", description } });
        }

        juce::var choice(std::initializer_list<const char*> values, const char* description)
        {
            juce::Array<juce::var> e;
            for (const char* v : values)
                e.add(juce::String(v));
            juce::var o = claude::object({ { "type", "string" }, { "enum", e } });
            if (description != nullptr)
                o.getDynamicObject()->setProperty("description", description);
            return o;
        }

        [[maybe_unused]] std::string noteName(int n)              // an instrument's (unused in an effect)
        {
            static constexpr const char* kNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            return std::string(kNames[std::clamp(n, 0, 127) % 12]) + std::to_string(std::clamp(n, 0, 127) / 12 - 1);
        }

        std::string db(double linear)
        {
            return linear > 0.0 ? fmt("%.1f", 20.0 * std::log10(linear)) : std::string("-inf");
        }

        std::string megabytes(std::size_t bytes)
        {
            return fmt("%.0f MB", std::max(1.0, static_cast<double>(bytes) / 1.0e6));
        }

        // A dial's value as the panel shows it (Controls.h), from a normalised one.
        std::string dialValue(const Control& c, float v)
        {
            if (!c.used)
                return fmt("%.2f", static_cast<double>(v));
            const float p = plainOf(c, v);
            switch (c.type)
            {
                case ControlType::toggle:  return p >= 0.5f ? "on" : "off";
                case ControlType::integer: return std::to_string(static_cast<int>(p));
                case ControlType::real:    break;
            }
            char buf[32];
            formatValue(c, p, buf, sizeof buf);
            return buf;
        }

        std::string levelsLine(const char* what, const capture::Levels& l)
        {
            if (!(l.peak > 0.0))
                return std::string(what) + ": silent" + (l.nonFinite > 0 ? fmt(" (%llu samples not numbers)", static_cast<unsigned long long>(l.nonFinite)) : "") + ".\n";
            const double crest = l.rms > 0.0 ? 20.0 * std::log10(l.peak / l.rms) : 0.0;
            return std::string(what) + ": peak " + db(l.peak) + " dBFS, RMS " + db(l.rms) + " dBFS, crest " + fmt("%.1f", crest)
                 + " dB, DC " + (std::fabs(l.dc) > 0.0 ? db(std::fabs(l.dc)) + " dB" : std::string("none"))
                 + (l.nonFinite > 0 ? fmt("; %llu samples were not numbers", static_cast<unsigned long long>(l.nonFinite)) : "")
                 + ".\n";
        }

        const char* startWords(capture::Start s)
        {
            switch (s)
            {
                case capture::Start::arm:       return "when it was armed";
                case capture::Start::note:      return "at the first note";
                case capture::Start::transport: return "when the host started playing";
                case capture::Start::sound:     return "at the input's first sound";
            }
            return "";
        }

        const char* waitWords()
        {
            return kInstrument ? "the next note (or the host starting to play)" : "the input's first sound (or the host starting to play)";
        }

        // The first line: what the recorder does, and the take's extent.
        std::string headLine(const capture::Reply& r, const capture::Take& t)
        {
            const std::string mode = t.options.mode == capture::Mode::last
                                       ? fmt("mode last: keeps the last %.0f s", t.options.seconds)
                                       : fmt("mode next: %.0f s once", t.options.seconds);
            switch (r.state)
            {
                case capture::State::waiting:
                    if (!t.claimed && t.armedAgo >= 1.0)
                        return "Armed (" + mode + fmt("), but no audio has reached the plugin in the %.0f s since: the host may not "
                                                      "be running it (a track not playing or not record-armed, the Standalone's "
                                                      "audio off).\n", t.armedAgo);
                    if (t.options.mode == capture::Mode::next)
                        return "Armed (" + mode + "), waiting for " + waitWords() + ": nothing recorded yet.\n";
                    return "Armed (" + mode + "): it records from the plugin's next block of audio.\n";
                case capture::State::recording:
                    return fmt("Recording now (live; %s): %.2f s so far, started %.0f s ago.\n", mode.c_str(), t.recordedSeconds,
                               t.startedAgo);
                case capture::State::ended:
                case capture::State::kept:
                case capture::State::off:
                    break;
            }
            std::string how;
            switch (t.end)
            {
                case capture::End::none:
                case capture::End::stopped:  how = "stopped"; break;
                case capture::End::full:     how = "filled and stopped by itself"; break;
                case capture::End::alert:    how = "stopped by itself half a second after the first alert"; break;
                case capture::End::prepared: how = "ended when the host restarted the audio (a new rate or block size)"; break;
            }
            const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - t.madeAt).count();
            std::string line = fmt("Recording %s (live: what was played; %s). Kept %.2f s", how.c_str(), mode.c_str(), t.seconds());
            if (t.options.mode == capture::Mode::last && t.fromSeconds > 0.0)
                line += fmt(" of the %.2f s recorded", t.recordedSeconds);
            line += fmt("; it started %s, %.0f s ago", startWords(t.start), t.startedAgo + age);
            if (r.state == capture::State::kept && age >= 1.0)
                line += fmt(", and was taken %.0f s ago", age);
            return line + ".\n";
        }
    }

    juce::var captureToolDefinition()
    {
        juce::var props = claude::object({ { "action", choice({ "arm", "stop", "status", "forget" }, nullptr) },
                                           { "mode", choice({ "last", "next" }, nullptr) },
                                           { "seconds", prop("number", "At most 20 (default 10)") },
                                           { "stop_on_alert", claude::object({ { "type", "boolean" } }) } });
        juce::var schema = claude::object({ { "type", "object" },
                                            { "properties", props },
                                            { "required", juce::Array<juce::var>{ "action" } } });
        const char* description =
            "Record what the user plays live, in the plugin's memory, and report it in numbers. Tell the user before "
            "arming; arm returns at once, then they play and you call stop.\n\n"
            "Only numbers come back; the audio and MIDI stay in the plugin's memory until forget, the next arm or ten "
            "minutes without a call. mode last (the default) records continuously and stop keeps the last `seconds`; "
            "next waits for the next note-on (an effect: its input's first sound) or for the host to start playing, then "
            "fills `seconds` once. stop_on_alert ends it 0.5 s after a NaN or a clip. status and stop are live; status "
            "after stop reads the take again. While armed the program's own code plays, not its frozen copy, so the take "
            "replays exactly; the CPU may rise a little.";
        return claude::object({ { "name", "capture" },
                                { "description", description },
                                { "eager_input_streaming", true },
                                { "input_schema", schema } });
    }

    std::string captureReport(const capture::Reply& r, const Controls& controls)
    {
        if (r.take == nullptr)
            return "The recorder is off: nothing armed, nothing kept. capture arm starts a recording.\n";
        const capture::Take& t = *r.take;
        std::string out = headLine(r, t);
        if (t.blocks.empty())
            return out;
        const capture::Summary s = capture::summarize(t);

        // The host's blocks and the rate.
        out += fmt("Rate %.0f Hz. Host blocks: ", t.rate);
        if (s.smallest == s.largest)
            out += fmt("%d samples (%d blocks", s.common, s.blocks);
        else if (2 * s.commonCount >= s.blocks)
            out += fmt("%d to %d samples, mostly %d (%d blocks", s.smallest, s.largest, s.common, s.blocks);
        else
            out += fmt("%d to %d samples, varied (%d blocks", s.smallest, s.largest, s.blocks);
        out += s.empty > 0 ? fmt(", %d of them empty)", s.empty) : std::string(")");
        if (s.largest > t.part && t.part > 0)
            out += fmt("; the plugin cuts them at %d", t.part);
        out += ".\n";
        if ((t.trimmed & capture::blocksLimit) != 0)
            out += "Kept less than asked: the host's blocks were small, more of them than the recorder holds.\n";
        if ((t.trimmed & capture::eventsLimit) != 0)
            out += "Kept less than asked: more MIDI events than the recorder holds.\n";

        // What was played.
        if constexpr (kInstrument)
        {
            if (s.notes == 0)
                out += "Notes: none.\n";
            else
            {
                out += fmt("Notes: %d, %s to %s, velocity %d to %d; at most %d held at once", s.notes, noteName(s.lowest).c_str(),
                           noteName(s.highest).c_str(), s.softest, s.hardest, s.mostHeld);
                if (s.mostSounding > s.mostHeld)
                    out += fmt(" (%d with the pedal's)", s.mostSounding);
                out += ".\n";
            }
            std::string played;
            if (s.pedalDowns > 0)
                played += fmt("pedal down %d times", s.pedalDowns);
            if (s.modEvents > 0)
                played += (played.empty() ? "" : "; ") + fmt("mod wheel %.2f to %.2f", static_cast<double>(s.modLow), static_cast<double>(s.modHigh));
            if (s.bendEvents > 0)
                played += (played.empty() ? "" : "; ") + fmt("pitch bend %+.2f to %+.2f", static_cast<double>(s.bendLow), static_cast<double>(s.bendHigh));
            if (s.pressureEvents > 0)
                played += (played.empty() ? "" : "; ") + fmt("pressure up to %.2f", static_cast<double>(s.pressureHigh));
            if (s.allNotesOff > 0)
                played += (played.empty() ? "" : "; ") + fmt("all notes off %d times", s.allNotesOff);
            out += played.empty() ? "Pedal and wheels: not used.\n" : "Played: " + played + ".\n";
            if (s.steals > 0 || s.dropped > 0)
                out += fmt("Voices: %d stolen, %d dropped (sixteen sound at once at most).\n", s.steals, s.dropped);
            if (t.start == capture::Start::note || t.start == capture::Start::transport)
                if (t.voicesAtStart > 0)
                    out += fmt("When it started %d voices were still sounding (the program's state was not fresh).\n", t.voicesAtStart);
        }
        else if (t.peakBefore > 0.0f && t.start != capture::Start::arm)
            out += "When it started the output was not silent (a tail still rang: the program's state was not fresh).\n";

        // The dials, the program, the frozen copy.
        if (s.dials.empty())
            out += "Dials: none moved.\n";
        else
        {
            out += "Dials moved: ";
            for (std::size_t k = 0; k < s.dials.size() && k < kDialsShown; ++k)
            {
                const capture::DialMove& d = s.dials[k];
                const Control& c = controls[static_cast<std::size_t>(d.slot)];
                const std::string name = c.used ? std::string(c.name) : "P" + std::to_string(d.slot + 1);
                out += (k == 0 ? "" : "; ") + name + " " + dialValue(c, d.low) + " to " + dialValue(c, d.high)
                     + (d.moves == 1 && d.first == d.last ? fmt(" (at %.2f s)", d.first)
                                                          : fmt(" (%d move%s, %.2f to %.2f s)", d.moves, d.moves == 1 ? "" : "s", d.first, d.last));
            }
            if (s.dials.size() > kDialsShown)
                out += fmt("; and %zu more", s.dials.size() - kDialsShown);
            out += ".\n";
        }
        if (s.programs.size() <= 1)
            out += "Program: one throughout (no run swapped it).\n";
        else
        {
            out += "Program: swapped at";
            for (std::size_t k = 1; k < s.programs.size() && k <= 4; ++k)
                out += fmt("%s %.2f s", k == 1 ? "" : ",", s.programs[k].first);
            out += fmt(" (%zu programs played; a swap fades over 20 ms).\n", s.programs.size());
        }
        if (s.frozenSeconds > 0.0)
            out += fmt("Frozen copy: played from %.2f to %.2f s (Math::Fast: a replay there matches to about -80 dB, not bit for "
                       "bit).\n", s.frozenFrom, s.frozenTo);
        else
            out += "Frozen copy: off; the program's own code played (so a replay can match it bit for bit).\n";

        // The host.
        if (!s.transport)
            out += "Host: no transport (the Standalone, or the host said nothing): the plugin's own clock at 120 BPM.\n";
        else
        {
            const std::string tempo = s.bpmLow == s.bpmHigh ? fmt("%.1f BPM", s.bpmLow) : fmt("%.1f to %.1f BPM", s.bpmLow, s.bpmHigh);
            if (s.playingSeconds <= 0.0)
                out += "Host: stopped (" + tempo + fmt(", %d/%d).\n", s.beatsPerBar, s.beatUnit);
            else
            {
                out += "Host: playing at " + tempo + fmt(" in %d/%d, from beat %.2f to %.2f", s.beatsPerBar, s.beatUnit, s.beatFrom, s.beatTo);
                if (s.transportStarts > 0 || s.transportStops > 0)
                    out += fmt(" (started %d, stopped %d times)", s.transportStarts, s.transportStops);
                out += ".\n";
            }
        }
        if (s.bypassSeconds > 0.0)
            out += fmt("Bypass: on for %.2f s, from %.2f to %.2f s (the program ran on, unheard; its output is what is kept).\n",
                       s.bypassSeconds, s.bypassFrom, s.bypassTo);

        // Alerts and the CPU.
        if (s.alertBlocks == 0)
            out += "Alerts: none.\n";
        else
        {
            out += "Alerts:";
            for (std::size_t k = 0; k < s.alerts.size(); ++k)
            {
                const capture::Alert& a = s.alerts[k];
                out += fmt("%s %s at %.3f s", k == 0 ? "" : ";",
                           a.nonFinite && a.clipped ? "NaN or inf replaced and samples clipped"
                           : a.nonFinite            ? "NaN or inf replaced"
                                                    : "samples clipped",
                           a.at);
            }
            if (s.alertBlocks > static_cast<int>(s.alerts.size()))
                out += fmt(" (%d blocks in all, the last at %.3f s)", s.alertBlocks, s.lastAlert);
            out += ".\n";
        }
        out += fmt("CPU: the worst block took %.0f%% of its time (at %.2f s), the median %.0f%% (live: this machine, as it was "
                   "loaded).\n",
                   static_cast<double>(s.worstLoad) * 100.0, s.worstAt, static_cast<double>(s.medianLoad) * 100.0);

        // The levels.
        if (t.input)
            out += levelsLine("In", s.in);
        out += levelsLine(t.input ? "Out (the program's, before the bypass)" : "Out", s.out);
        out += "It stays in the plugin's memory (" + megabytes(r.bytes) + "), never on disk or sent; capture forget frees it.\n";
        return out;
    }

    std::string captureToolText(ChatHost& host, const juce::var& input, bool& isError,
                                const std::function<void(std::string)>& note)
    {
        const std::string action = input.getProperty("action", juce::var()).toString().toStdString();
        capture::Options o;
        const std::string mode = input.getProperty("mode", juce::var()).toString().toStdString();
        if (mode == "next")
            o.mode = capture::Mode::next;
        else if (!mode.empty() && mode != "last")
        {
            isError = true;
            return "mode is last or next.";
        }
        if (const juce::var v = input.getProperty("seconds", juce::var()); v.isDouble() || v.isInt() || v.isInt64())
            o.seconds = std::clamp(static_cast<double>(v), capture::kMinSeconds, capture::kMaxSeconds);
        o.stopOnAlert = static_cast<bool>(input.getProperty("stop_on_alert", false));

        if (action == "arm")
        {
            const capture::Reply r = host.chatCapture(capture::Action::arm, o);
            if (!r.armedNow)
            {
                isError = true;
                return "Not armed: " + r.why + ". Ask the user to start the host's playback (or turn on the Standalone's "
                       "audio), then arm again.";
            }
            const std::string what = o.mode == capture::Mode::last
                                       ? fmt("recording what plays from now on (mode last: stop keeps the last %.0f s)", o.seconds)
                                       : fmt("the recording starts at %s and fills %.0f s once (mode next)", waitWords(), o.seconds);
            note(fmt("started recording what plays (%.0f s; it stays in the plugin's memory)", o.seconds));
            return "Armed (live): " + what + (o.stopOnAlert ? ", ending half a second after the first alert" : "")
                 + ". Tell the user it is recording, ask them to play the passage, then call capture stop when they say it is "
                   "done (status shows how far it has got). The audio and MIDI stay in the plugin's memory (about "
                 + megabytes(r.bytes) + "), never on disk or sent; capture forget frees them. Until it stops the program's own "
                   "code plays rather than its frozen copy, so the take replays exactly: the CPU load may rise a little.\n";
        }
        if (action == "forget")
        {
            const capture::Reply r = host.chatCapture(capture::Action::forget, o);
            note("forgot the recording");
            if (r.freed == 0)
                return "There was nothing to forget: nothing armed, nothing kept.\n";
            return "Forgotten: the recording and its take are freed (" + megabytes(r.freed)
                 + (r.bytes > 0 ? "; what the audio thread still holds goes at its next block" : "") + ").\n";
        }
        if (action == "stop")
        {
            const capture::Reply r = host.chatCapture(capture::Action::stop, o);
            if (r.state == capture::State::off)
            {
                isError = r.take == nullptr;
                if (r.take == nullptr)
                    return "Nothing is recording and nothing is kept: arm it first (capture arm).\n";
                return std::string("Stopped before it started: ")
                     + (r.take->claimed ? std::string("no ") + (kInstrument ? "note came" : "sound came in") + " and the host did not start playing"
                                        : "no audio reached the plugin while it was armed")
                     + ", so nothing was recorded. Arm again when the user is ready.\n";
            }
            return captureReport(r, host.chatDials());
        }
        if (action == "status")
            return captureReport(host.chatCapture(capture::Action::status, o), host.chatDials());
        isError = true;
        return "action is arm, stop, status or forget.";
    }
}
