// Source/plugin/EditorPort.h: what the editor reaches besides the parameter ports (JUCE-free; message thread only).
// The processor implements it (Processor.h); the Panel takes it as it takes the ParamPorts, so the editor never
// includes the processor.
//
//   draft()           the editor's files and the tab it shows, kept by the processor (and in the session) so they
//                     outlive the editor's window
//   draftRevision()   bumps when the draft changes other than by setDraft (a host loading a session), so an open editor
//                     shows the new files
//   setDraft()        the editor's files, as they are typed, and its tab
//   run()             RUN: compiles `files` off the message thread (Compiler.h) and plays them once they compile; the
//                     dials of parameters the program did not have before move to their defaults then
//   compiling()       a run is waiting or compiling
//   status()          the last compile's outcome: FMinus's messages, the dials, the time (CompileStatus)
//   statusRevision()  bumps with each compile that finishes
//   running()         the files of the program that plays (none before the first compiles)
//   controls()        the dials of the program that plays
//   load()            the audio thread's busiest recent block, as a share of its time (Engine::load)
//   alerts()          the output guard's alerts of the last two seconds (Engine::Alert bits)
#pragma once

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <cstdint>
#include <string>

namespace testingplaygroundsynth
{
    // The editor's files, a tab each (the first the program), and the tab it shows.
    struct Draft
    {
        Sources files;
        int active = 0;

        friend bool operator==(const Draft&, const Draft&) = default;
    };

    class EditorPort
    {
    public:
        virtual ~EditorPort() = default;

        virtual Draft draft() const = 0;
        virtual uint64_t draftRevision() const = 0;
        virtual void setDraft(const Draft& draft) = 0;

        virtual void run(const Sources& files) = 0;
        virtual bool compiling() const = 0;
        virtual CompileStatus status() const = 0;
        virtual uint64_t statusRevision() const = 0;
        virtual Sources running() const = 0;
        virtual Controls controls() const = 0;

        virtual float load() const = 0;
        virtual uint32_t alerts() const = 0;
    };
}
