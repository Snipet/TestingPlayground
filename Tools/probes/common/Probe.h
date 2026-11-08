// Tools/probes/common/Probe.h: the probe registration macro (modelled on FSynth's ProbeRegistry.h, simplified).
//
//   // PROBE name=dsp.response timeout=60          <- the file's first such line registers the CTest test
//   #include "Probe.h"
//   PROBE(dsp, response)                           // int (funkgui::test::Probe& P)
//   {
//       P.near("gain.0db.level_db", got, -12.0, 0.05);      // a spec row: judged now, with its tolerance
//       return P.finish();
//   }
//
// One probe per file, in Tools/probes/<layer>/ (dsp: Source/dsp only; plugin: the processor and the Panel). The macro
// registers the subcommand <layer>.<name>, which ProbeMain.cpp dispatches:
//
//   build-dev/<Name>_probes dsp.response            run one probe (the harness flags have defaults; outside CTest
//                                                   the plugin's stores are the user's own folders: use ctest)
//   build-dev/<Name>_probes --list                  the probes
//   ctest --test-dir build-dev -L probe             all of them, both products (-L effect, -L synth: one product's;
//                                                   scripts/test.sh builds first)
//
// funkgui::test::Probe is FunkGui's Harness (<funkgui/test/Harness.h>): spec rows near/le/ge/in/eq print a line each,
// and finish() returns the exit code (0 pass, 1 a spec row failed, 4 a harness error). There are no golden files here:
// use spec rows with tolerances that state what must hold.
//
// A row this machine cannot judge (a GPU picture where FunkGui reads none back: common/Pictures.h; a time budget under
// emulation: emulated(); a menu's window with no X display: noDisplay()) is never left out in silence: probe::skip
// prints it with why, and the rows that can be judged there (the fallback, and that it says why) run in its place.
#pragma once

#include <funkgui/test/Harness.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace probe
{
    using Fn = int (*)(funkgui::test::Probe&);

    // A node of the list ProbeMain walks; constructed only by PROBE, during static initialisation.
    struct Registration
    {
        Registration(const char* layerDotName, Fn fn) noexcept;
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

        const char* name;
        Fn fn;
        const Registration* next;
    };

    const Registration* registrations() noexcept;

    // Where a probe writes its files (WAVs, PNGs): $PROBE_OUT_DIR when set, else <build>/probe-out. Created on use.
    std::filesystem::path outDir();

    // outDir() / name, as a string, for the probe's "wrote ..." line.
    std::string outPath(const std::string& name);

    // A row skipped here, said: "SKIP       <key>  <why>" where its SPEC line would be, and the results' note
    // skip_<key> (its other characters as underscores, 64 at most) holding why, so a reader of the results JSON sees it.
    void skip(funkgui::test::Probe& P, std::string_view key, std::string_view why);

    // Why this process's timings are not the machine's, "" when they are: an x86-64 build run by Rosetta (a Linux
    // container on Apple silicon, whose /proc/cpuinfo names the CPU "VirtualApple"). A row with a time budget is skipped
    // there (skip), saying so; everything else runs as anywhere. (QEMU is not told apart: its CPU's name is the same
    // under KVM, where the timings are the machine's.)
    std::string emulated();

    // Why JUCE cannot show a window here, "" when it can: on Linux, no X display ($DISPLAY unset), where JUCE's X calls
    // end the process, even for a menu shown inside a component (gui.menus' and gui.midi's pictures). The rows that need
    // one are skipped there (skip); the rest of the probe runs.
    std::string noDisplay();

    // The calling thread's CPU time, ms (Windows: its cycles, QueryThreadCycleTime, over the time-stamp counter's rate):
    // what a cost row times, so that the time a busy machine keeps the thread waiting for a core is not counted.
    double threadMs();
}

#define PROBE_FN_(layer, name)  probeFn_##layer##_##name
#define PROBE_REG_(layer, name) probeReg_##layer##_##name

#define PROBE(layer, name)                                                                                          \
    static int PROBE_FN_(layer, name)(::funkgui::test::Probe&);                                                     \
    static const ::probe::Registration PROBE_REG_(layer, name){ #layer "." #name, &PROBE_FN_(layer, name) };        \
    static int PROBE_FN_(layer, name)([[maybe_unused]] ::funkgui::test::Probe& P)
