// Source/plugin/ChatScenario.h: the render tools' `scenario` and `try_edits` (ChatAgent.h; the guide's CHECKING YOUR
// WORK teaches both).
//
// scenario: how a musician plays it, one property of measure_output, view_sound, each of set_checks' checks (which
// keeps it, so it guards every later run), tune_dials, compare and trace (renderOf reads it with the render's other
// settings):
//
//   { phrase:   [{note, at, len, vel}] (at most 256; seconds; len 0.5 and vel 0.8 by default) or "capture",
//     automate: [{dial, at, to, over}] (at most 32: a float glides from where it is to `to` over `over` seconds, as the
//               Engine glides a host's automation; an int or a bool steps),
//     wheels:   [{wheel: mod | bend | pressure | sustain | steering | throttle | brake | clutch | gear | buttons, at,
//               value}] (at most 64; a racing wheel's as host.fm reads them: the steering -1 to 1, the gear -1 to 7, the
//               buttons a mask, a bit a button, as the wheel sends it: Measure.h's Wheel),
//     host:     {bpm, stop_at, jump_to_beat, jump_at},
//     rate:     a number (8000 to 192000) or "host",   block: 1 to 8192 or "host",
//     input:    sweep | noise | impulse | sine | silence | "capture" (an effect's),
//     from, to: with "capture", the part of the recording replayed, in seconds of it }
//
// read() parses it into a measure::Request (Measure.h), "host" and "capture" as flags; resolve() fills those in from
// the plugin just before a render (the host's rate and block from ChatHost; the user's last recording through
// captured()), so a check that keeps "host" renders at whatever the host runs at then. toVar() writes it back, for the
// session. Times stop at 10 s (measure::kMaxPlaySeconds) and the tail follows, as the renders' caps have it.
//
// dials_from: current | defaults, measure_output's, view_sound's and trace's: the dials a render does not name as they
// are in the plugin now (the default; the caller fills Request::current) or at the program's defaults (it leaves it
// empty). A one-shot render has no later, so set_checks' snapshot is not among them; compare's word is its own (dials:
// own, current or defaults; its first line is dialsLine's too). Not in a check's render settings either: set_checks
// takes its own, for every check.
//
// try_edits: [{file, old_text, new_text} or {name, content}] on a copy of the editor's files, in order, with
// edit_file's rule (old_text occurs exactly once) and write_file's (a whole file, new or replaced): measure_output,
// view_sound and compare (its b) render the copy; nothing in the plugin changes, and the result's first line says so.
//
// Any thread (the agent's worker, the MCP link's): no state but what the caller passes.
#pragma once

#include "plugin/ChatAgent.h"

#include "dsp/Goals.h"
#include "dsp/Measure.h"
#include "dsp/Sources.h"

#include <juce_core/juce_core.h>

#include <map>
#include <string>
#include <vector>

namespace playground::scenario
{
    // A render's settings from a tool's input (measure_output's, view_sound's, a check's, tune_dials', trace's): the
    // notes, the velocity, the times, the host's tempo, the wheels at the start, the dials asked for and the scenario
    // (read(): why, when it cannot be), over `request` (trace's source "capture" starts from the recording).
    measure::Request renderOf(const juce::var& input, std::string* why = nullptr, measure::Request request = {});
    // The property's schema: one line, details in the guide.
    juce::var schema();
    // The tool input's `scenario` into `request` (nothing there: unchanged); false with why (naming the part) when it
    // cannot be. With `why` null it reads what it can.
    bool read(const juce::var& input, measure::Request& request, std::string* why);
    // A request's scenario as read() takes it (void: none), for the session.
    juce::var toVar(const measure::Request& request);
    struct Captured;
    // "host" and "capture" filled in: false with why ("nothing recorded yet") when the recording is not there. With
    // `recording` given, that one (compare reads the recording once for both its sides), else captured()'s.
    bool resolve(measure::Request& request, ChatHost& host, std::string& why);
    bool resolve(measure::Request& request, ChatHost& host, std::string& why, const Captured* recording);
    // The checks measured as measure::evaluate measures them, each resolved first (one that cannot be is not measured,
    // saying why).
    std::vector<measure::CheckResult> evaluate(const Sources& files, const std::vector<measure::Check>& checks,
                                               const std::map<std::string, float>& current, ChatHost& host,
                                               std::string* errors);

    // ---- dials_from (above) ------------------------------------------------------------------------------------------
    // Its schema, with its words when `described` (the first tool's: MCP's copy drops them again from the others,
    // mcp/McpSchema.h), else its two words alone (trace's lean schema).
    juce::var dialsFromSchema(bool described);
    // The input's dials_from: `defaults` true when it says defaults; false with why for a word it does not take
    // (absent: current).
    bool dialsFrom(const juce::var& input, bool& defaults, std::string& why);
    // The result's line: "The dials not named are at their defaults.\n" ("" for current).
    std::string dialsLine(bool defaults);

    // ---- try_edits ------------------------------------------------------------------------------------------------
    juce::var trialSchema();
    // The input's `try_edits` applied to `files` (in order): false with why, naming the edit, when one cannot be;
    // `count` the edits applied (0: none asked for).
    bool tryEdits(const juce::var& input, Sources& files, int& count, std::string& why);
    // The result's first line: "With 2 trial edits; nothing in the plugin changed.\n" ("" for none).
    std::string trialLine(int count);

    // ---- the recording (the capture lane's seam) --------------------------------------------------------------------
    // What the user played, recorded in the processor per host block (the events, the dials' values, the transport,
    // the input and the output), as a render: `request` replays it (its rate, its blocks and the processor's parts, its
    // MIDI by sample, its dials as the processor sent them (Request::frames; `dials` and `automate`, the request's own,
    // go over them), its transport, the Engine's clock, an effect's input; plugin/CaptureReplay.h), `l` and `r` are
    // what played (compare renders the replay as long). The events and the transport by sample (resolve() sorts them,
    // keeping the order within a sample). ChatCaptured.cpp.
    struct Captured
    {
        measure::Request request;
        std::vector<float> l, r;
        std::string about;                        // a line: when, how long, the rate, what it holds
        Sources files;                            // the program that played at its start (none: not known), which
                                                  // replays it as it played (trace's source "capture")
        CircuitPins pins;                         // and its netlists' configurations (Sources.h), which it plays with
    };
    // The last recording (its part from..to, in seconds; to < 0: to its end, on its host blocks); false with why
    // ("nothing recorded yet: ...") when there is none. The one function the sources "capture" (scenario's phrase and
    // input, compare's a and b, trace's source) go through. `about` says how closely a replay of it gives what played.
    bool captured(ChatHost& host, Captured& out, std::string& why, double from = 0.0, double to = -1.0);
}
