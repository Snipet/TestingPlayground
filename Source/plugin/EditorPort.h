// Source/plugin/EditorPort.h: what the editor reaches besides the parameter ports (JUCE-free; message thread only).
// The processor implements it (Processor.h); the Panel takes it as it takes the ParamPorts, so the editor never
// includes the processor.
//
//   draft()           the editor's text, kept by the processor (and in the session) so it outlives the editor's window
//   draftRevision()   bumps when the draft changes other than by setDraft (a host loading a session), so an open editor
//                     shows the new text
//   setDraft()        the editor's text, as it is typed
//   run()             RUN: compiles `source` off the message thread (Compiler.h) and plays it once it compiles; the
//                     dials of parameters the program did not have before move to their defaults then
//   compiling()       a run is waiting or compiling
//   status()          the last compile's outcome: FMinus's messages, the dials, the time (CompileStatus)
//   statusRevision()  bumps with each compile that finishes
//   runningSource()   the source of the program that plays ("" before the first compiles)
//   controls()        the dials of the program that plays
//   load()            the audio thread's busiest recent block, as a share of its time (Engine::load)
//   alerts()          the output guard's alerts of the last two seconds (Engine::Alert bits)
#pragma once

#include "dsp/Compiler.h"
#include "dsp/Controls.h"

#include <cstdint>
#include <string>

namespace testingplaygroundsynth
{
    class EditorPort
    {
    public:
        virtual ~EditorPort() = default;

        virtual std::string draft() const = 0;
        virtual uint64_t draftRevision() const = 0;
        virtual void setDraft(const std::string& text) = 0;

        virtual void run(const std::string& source) = 0;
        virtual bool compiling() const = 0;
        virtual CompileStatus status() const = 0;
        virtual uint64_t statusRevision() const = 0;
        virtual std::string runningSource() const = 0;
        virtual Controls controls() const = 0;

        virtual float load() const = 0;
        virtual uint32_t alerts() const = 0;
    };
}
