// Source/sandbox/Sandbox.h: the GUI sandbox's document, in the helper (Source/sandbox/Main.cpp): a .gui file being
// designed, the program files beside it (from an instance or a folder: read for the dials and fields they declare and
// the shaders they hold, never compiled), the dials (sandbox/Dials.h) and what the plugin would make of them: GuiSpec's
// tree and notes (fminus/GuiSpec.h), its shaders built with shaderc (fminus/Shaders.h), its layout (editor/GuiLayout.h)
// and its picture (sandbox/View.h). Each request of SandboxProtocol.h is a method; each returns its reply (ok, text and
// what the op adds). One document at a time: open replaces it.
//
// The layout report (open's and edit's): the natural size against the area (the last render's, else the PANEL view's
// at the default size); one line an element on the pages shown (its line, kind and name, its place in the area);
// overlaps; what sticks out of its container, what is cut, what scrolls; controls that name no dial (GuiSpec's notes);
// dials no control names (the plugin puts them last, in a section without a title). About 40 lines; `detail` all.
#pragma once

#include "sandbox/Dials.h"

#include "dsp/Controls.h"
#include "dsp/Sources.h"
#include "fminus/PanelSpec.h"
#include "fminus/Shaders.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace playground::sandbox
{
    class Sandbox
    {
    public:
        explicit Sandbox(bool instrument);

        // A request (SandboxProtocol.h), its reply without the id.
        juce::var handle(const juce::var& request);

    private:
        juce::var open(const juce::var& r);
        juce::var edit(const juce::var& r);
        juce::var render(const juce::var& r);
        juce::var state() const;

        void rebuild();                          // the controls, the fields, GuiSpec's tree and notes, the shaders
        std::string summary() const;             // what open says first
        std::string report(bool detail) const;   // the layout report, at the area
        std::string problems(bool detail) const; // GuiSpec's notes and the dials', by line
        const Dial* dialNamed(const std::string& name, int* slot = nullptr) const;
        bool readPath(const std::string& path, Sources& out, std::string& why) const;

        bool instrument_;
        bool open_ = false;
        int number_ = 0;                         // opens so far (the result names the sandbox by it)
        std::string origin_;                     // where it came from, in words
        Sources files_;                          // the program's files, the .gui among them (files_[gui_])
        int gui_ = -1;
        std::vector<Dial> dials_;                // in slot order
        Declared declared_;                      // what the program declares
        std::vector<std::string> mockedFields_;  // fields a display names with no program to declare them
        std::vector<PanelField> fields_;
        std::string dialNotes_;                  // the dials' mistakes (parseDials)
        Controls controls_{};                    // as readGuiSpec left them (its tapers, units)
        PanelSpec spec_;
        std::string notes_;                      // readGuiSpec's, and the shaders' builds
        ShaderTools tools_;
        ShaderCache shaders_;
        int areaW_, areaH_;                      // the area the report lays it out in (the last render's)
    };
}
