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
//   build-dev/<Name>_probes dsp.response            run one probe (the harness flags have defaults)
//   build-dev/<Name>_probes --list                  the probes
//   ctest --test-dir build-dev -L probe             all of them (scripts/test.sh builds first)
//
// funkgui::test::Probe is FunkGui's Harness (<funkgui/test/Harness.h>): spec rows near/le/ge/in/eq print a line each,
// and finish() returns the exit code (0 pass, 1 a spec row failed, 4 a harness error). There are no golden files here:
// use spec rows with tolerances that state what must hold.
#pragma once

#include <funkgui/test/Harness.h>

#include <filesystem>
#include <string>

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
}

#define PROBE_FN_(layer, name)  probeFn_##layer##_##name
#define PROBE_REG_(layer, name) probeReg_##layer##_##name

#define PROBE(layer, name)                                                                                          \
    static int PROBE_FN_(layer, name)(::funkgui::test::Probe&);                                                     \
    static const ::probe::Registration PROBE_REG_(layer, name){ #layer "." #name, &PROBE_FN_(layer, name) };        \
    static int PROBE_FN_(layer, name)([[maybe_unused]] ::funkgui::test::Probe& P)
