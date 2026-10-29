// Source/fminus/HostFile.cpp: host.fm, the one file the plugin provides (Patch::hostSource; Patch.h, "module Host"),
// apart from Patch.cpp, so that what reads programs without compiling them (Lang.cpp: the editor's language service,
// the GUI sandbox's dials) takes its text without FMinus. A new field: this text, HostBlock, HostLayout and
// Patch::setHost together (CLAUDE.md, "The Host block").
#include "fminus/Patch.h"

namespace playground
{
    namespace
    {
        constexpr const char* kHostFile = R"FM(// host.fm: what the host and the player are doing, for a program to read. The plugin provides this file: #include it.
//
// Declare a field `Host* host;` in the main module (each Voice, or the Effect; Shared may have one too) and the plugin
// points it at the one Host, which it fills before every call of start, stop and process. Read it; never write it.
//
//   host->bpm          the tempo, beats a minute (the host's; 120 when it says none)
//   host->beat         the song position at this call's first sample, in quarter notes; sample i of the call is at
//                      host->beat + i * host->bpm / (60 * rate). It moves while the host plays (when the host says
//                      nothing, as the Standalone, it runs on at 120 BPM from when the program started)
//   host->barStart     where the bar the song is in began, in quarter notes (host->beat - host->barStart: the beats
//                      into it)
//   host->beatsPerBar, host->beatUnit   the time signature (4 and 4)
//   host->playing      the host's transport runs
//   host->modwheel     the mod wheel (MIDI CC 1), 0 to 1
//   host->pitchbend    the pitch wheel, -1 to 1 (0 at rest): the program chooses its range (2 semitones is usual)
//   host->aftertouch   channel pressure, 0 to 1
//   host->sustain      the sustain pedal is down (the plugin holds the notes itself: a key let go while it is down
//                      gets its stop when the pedal comes up)
//
// A synced LFO: its rate host->bpm / 60 / beats (beats a cycle: 1 a quarter note, 4 a bar of 4/4, 0.5 an eighth), or,
// to follow the song (the same phase at the same place, whenever it is played), its phase from the position:
// phase = fract((host->beat + i * host->bpm / (60 * rate)) / beats).
module Host
{
    float bpm;
    float beat;
    float barStart;
    int beatsPerBar;
    int beatUnit;
    bool playing;
    float modwheel;
    float pitchbend;
    float aftertouch;
    bool sustain;
};
)FM";
    }

    std::string_view Patch::hostSource() noexcept { return kHostFile; }
}
