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
// Declare `Host* host;` in the main module (each Voice, or the Effect; Shared may have one too): the plugin points it
// at the one Host and fills it before every call of start, stop and process. Read it; never write it.
//
//   host->bpm          the tempo, beats a minute (the host's; 120 when it says none)
//   host->beat         the song position at this call's first sample, in quarter notes; sample i of the call is at
//                      host->beat + i * host->bpm / (60 * rate). It moves while the host plays (a host that says
//                      nothing, as the Standalone: on at 120 BPM from the program's start)
//   host->barStart     the bar's start, in quarter notes (host->beat - host->barStart: the beats into the bar)
//   host->beatsPerBar, host->beatUnit   the time signature (4 and 4)
//   host->playing      the host's transport runs
//   host->modwheel     the mod wheel (MIDI CC 1), 0 to 1
//   host->pitchbend    the pitch wheel, -1 to 1 (0 at rest)
//   host->aftertouch   channel pressure, 0 to 1
//   host->sustain      the sustain pedal is down (the plugin holds the notes)
//   host->steering     a racing wheel (MIDI CCs 16 to 24; the Standalone's G29): -1 left to 1 right
//   host->throttle, host->brake, host->clutch   its pedals, 0 released to 1
//   host->gear         -1 reverse, 0 neutral, 1 to 7
//   host->buttons      a bit each, a G29's: 0 cross, 1 square, 2 circle, 3 triangle, 4 right paddle, 5 left paddle,
//                      6 R2, 7 L2, 8 share, 9 options, 10 R3, 11 L3, 12 +, 13 -, 14 dial right, 15 dial left,
//                      16 return, 17 PS (also the start key: note 36 while held), 18-21 the d-pad up, right, down, left
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
    float steering, throttle, brake, clutch;
    int gear, buttons;
};
)FM";
    }

    std::string_view Patch::hostSource() noexcept { return kHostFile; }
}
