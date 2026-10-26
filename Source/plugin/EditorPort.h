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
//   panel()           its interface: a .gui file's (GuiSpec.h), or how its #pragma panel lines lay its dials out
//                     (PanelSpec.h)
//   panelView()       the editor shows the PANEL view (the interface across the left column) rather than the CODE view;
//                     kept in the session, set with setPanelView()
//   program()         the compile it came from (CompileStatus::request): the meters' taps are its fields once
//                     Meters::program says so (until the audio thread takes it, they are the last program's)
//   meters()          the last block's levels and the program's taps, for the panel's displays (Engine::Meters); false
//                     while the audio thread publishes (keep the last)
//   scope()           the last n samples of the input or the output (Engine::scope)
//   load()            the audio thread's busiest recent block, as a share of its time (Engine::load)
//   alerts()          the output guard's alerts of the last two seconds (Engine::Alert bits)
//   presets()         the presets saved on this Mac (PresetStore.h), by name, with when each was last saved
//   presetFolder()    where they are, "~/..." for the home folder
//   tidyPresetName()  a typed name as savePreset would save it ("" when nothing is left of it)
//   savePreset()      the editor's files (the draft) and the dials as they are now, by name, under `name` (over one of
//                     that name); false, with why, when it cannot
//   loadPreset()      a preset's files become the draft (the editor shows them: draftRevision bumps) and run; its dials
//                     are set by name once they compile. False, with why, when there is none, or it is not this
//                     plugin's, or its file is damaged
//   deletePreset()    its file deleted
//   presetName()      the preset loaded or saved last ("" for none), kept in the session
//   presetEdited()    whether the draft has changed since (a new instance's first files count as saved)
//   chat()            the chat with Claude (ChatPort.h)
#pragma once

#include "plugin/ChatPort.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Sources.h"

#include <cstdint>
#include <string>
#include <vector>

namespace testingplayground
{
    // The editor's files, a tab each (the first the program), and the tab it shows.
    struct Draft
    {
        Sources files;
        int active = 0;

        friend bool operator==(const Draft&, const Draft&) = default;
    };

    struct PresetEntry
    {
        std::string name;
        std::string when;                        // its last save, "2026-10-25 14:02" (local time)
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
        virtual PanelSpec panel() const = 0;
        virtual uint64_t program() const = 0;
        virtual bool meters(Meters& out) const = 0;
        virtual void scope(bool input, float* out, int n) const = 0;

        virtual float load() const = 0;
        virtual uint32_t alerts() const = 0;

        virtual std::vector<PresetEntry> presets() const = 0;
        virtual std::string presetFolder() const = 0;
        virtual std::string tidyPresetName(const std::string& typed) const = 0;
        virtual bool savePreset(const std::string& name, std::string& why) = 0;
        virtual bool loadPreset(const std::string& name, std::string& why) = 0;
        virtual bool deletePreset(const std::string& name, std::string& why) = 0;
        virtual std::string presetName() const = 0;
        virtual bool presetEdited() const = 0;
        virtual ChatPort& chat() = 0;

        virtual bool panelView() const = 0;
        virtual void setPanelView(bool on) = 0;

        // The pages the panel's interface shows: each tabs element's chosen page (the tabs in the order the .gui has
        // them), by its title. Kept in the session, so a reopened editor and a reloaded session show them again.
        virtual std::vector<std::string> guiPages() const = 0;
        virtual void setGuiPages(const std::vector<std::string>& titles) = 0;

        // A note from the panel's keyboard (a .gui file's keyboard element): `velocity` 0..1 plays it, 0 releases it.
        // An instrument plays it at the start of its next block; an effect ignores it. 64 can wait at once; past that
        // a note is dropped.
        virtual void playNote(int note, float velocity) = 0;
    };
}
