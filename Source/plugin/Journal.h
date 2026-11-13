// Source/plugin/Journal.h: what changed in the plugin, as get_status `since` tells Claude Desktop (Live.h's Changes): the
// dials moved (by the user in the editor, by the host's automation or its own controls, by the plugin's own chat), the
// files edited, added or closed, the runs, the projects opened and saved, the presets, the output guard's alerts, the
// host's bypass, transport and settings. What Claude Desktop does itself is not kept: it knows.
//
// Bounded: the newest kMax changes. Drags are merged: a change goes into the last one of its kind and subject (a dial,
// a file, the bypass) while that one is recent and no token has been handed out since it was made, so a knob turned
// for two seconds is one line ("14:02:31-33 the user moved cutoff 1.20 kHz -> 3.40 kHz") and a reader never misses part
// of one. A reader gets each dial's moves after its token as one line, its net change (first -> last, at the first
// move's place, with how many and who): forty-five minutes of play is a line a dial, not a line a move. A change that
// sets dials without a line for each (dialsNote: a run, a version, a preset, a fresh start, a state loaded) ends those
// lines: a dial moved before it and after it has a line on either side of it; it and a run that sets no dial
// (runNote) end a file's typing so too. The lines say
// who in the third person ("the user", "the host", "the plugin's own chat"): "you" reads as the reader, a Claude. A
// token is the journal's identity (made at random, so a token of another instance or of before a reload is known as
// not this one's) and the number of the last change: tokens rather than a cursor here, since one server process may
// serve several conversations, each with its own. Each token handed out is a new one (a number of its own when nothing
// changed since the last), so what Live.cpp keeps of the files at a token is that token's alone.
//
// Any thread, under the journal's own mutex (a leaf: nothing is called with it held). The Processor writes it from the
// message thread (poll(), the editor's calls), the agent's and the link's threads (the chat's tools), and
// prepareToPlay's; get_status reads it from the link's poll thread.
//
// GesturePort: the editor's ParamPort (FunkGui's JuceParamPort, wrapped), which also remembers that the editor moved
// its parameter, so poll() tells the user's moves from the host's (message thread, as every port call is).
#pragma once

#include "plugin/Live.h"

#include "dsp/Compiler.h"

#include <funkgui/params/JuceParamPort.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace playground
{
    class Journal
    {
    public:
        static constexpr std::size_t kMax = 256;
        enum class Who : uint8_t
        {
            user,                                     // the user, in the editor
            host,                                     // the host: its automation, its own controls, its transport
            chat,                                     // the plugin's own chat (its Claude)
            plugin,                                   // the plugin itself (the output guard, a program that stopped)
        };

        Journal();

        // A dial moved from one value to another (as text, in its units).
        void dial(Who who, int slot, const std::string& name, const std::string& from, const std::string& to);
        // A file `what`: "edited", "added", "closed", "renamed to x.fm", "wrote", "redrew" (a netlist's *@ lines alone,
        // its schematic; merged while typing, as "edited" is).
        void file(Who who, const std::string& name, const std::string& what);
        // Anything else, in words ("ran it: it compiled, 9 dials"). With a `key`, it goes into the last change of that
        // key when it may (above), its words replacing that one's.
        void note(Who who, const std::string& words, const std::string& key = {});
        // A note of a change that may set dials without a line for each (a run that compiled, a version opened, a
        // preset applied, a fresh start, a state loaded): a dial's moves before it and after it are two lines, one on
        // either side of it, so that it never reads as if it came after the dial's last value.
        void dialsNote(Who who, const std::string& words);
        // A run's note that sets no dial (a panel's edit, a run that failed, an A/B side kept with the dials as they
        // are). This and a dialsNote are where a file's typing breaks: an edit after a run is a line after the run's,
        // never merged into the one before it.
        void runNote(Who who, const std::string& words);
        // The bypass's edges: "bypassed", then "bypassed (14:04:10-14:05:01)" once it ends (and, when a token was
        // handed out meanwhile, "un-bypassed" for that token's readers).
        void bypass(bool on);
        // The output guard's alerts (Engine::Alert bits), merged while they go on.
        void alert(uint32_t bits);

        // The changes after token `since` ("" or another journal's: none, known false), a dial's as its net change;
        // `issue`: the token returned is handed out (later changes go into new lines, not into those it covers).
        Changes since(const std::string& since, bool issue);
        std::string token() const;

    private:
        struct Change
        {
            uint64_t seq = 0;
            Who who = Who::user;
            std::string key;                          // what it is about: "dial:3", "file:program.fm", "bypass"
            std::string words;                        // a dial's name, a file's verb, a note's words
            std::string from, to;                     // a dial's values
            int64_t fromMs = 0, toMs = 0;             // when (the wall clock, ms)
            bool open = false;                        // the bypass still on
            bool setsDials = false;                   // a dialsNote: a dial's moves are not one line across it
            bool run = false;                         // a run (runNote, dialsNote): nor a file's edits
            uint64_t ends = 0;                        // an un-bypass: the bypass it ends (a reader of both reads one)
        };
        static constexpr int64_t kMergeMs = 1500;     // a drag's pause at most
        static constexpr int64_t kTypingMs = 60000;   // typing's
        Change* mergeable(const std::string& key, Who who, int64_t windowMs, int64_t now);   // mutex held
        void add(Change c);                                                                   // mutex held
        std::string line(const Change& c) const;
        std::string netLine(const std::vector<const Change*>& moves) const;   // a dial's moves, oldest first, as one

        mutable std::mutex mutex_;
        std::deque<Change> changes_;
        uint64_t seq_ = 0;                            // the last change's number
        uint64_t issued_ = 0;                         // the last token handed out
        uint64_t dropped_ = 0;                        // the newest change dropped (to keep kMax)
        uint32_t epoch_ = 0;
    };

    // A run's outcome in words, for the journal: "ran it: it compiled, 9 dials".
    std::string runWords(const CompileStatus& s);

    // A dial's port as the editor writes it: a host parameter's (a JuceParamPort), or a panel dial's (Controls.h's
    // isPanelDial: no host parameter, `inner` the processor's own port), saying when the editor moved it.
    class GesturePort final : public funkgui::ParamPort
    {
    public:
        explicit GesturePort(juce::RangedAudioParameter& p) : inner_(std::make_unique<funkgui::JuceParamPort>(p)) {}
        explicit GesturePort(std::unique_ptr<funkgui::ParamPort> inner) : inner_(std::move(inner)) {}

        float value01() const override { return inner_->value01(); }
        float default01() const override { return inner_->default01(); }
        int numSteps() const override { return inner_->numSteps(); }
        void beginGesture() override;
        void setValue01(float v) override;
        void endGesture() override;
        const char* id() const override { return inner_->id(); }
        void* native() const override { return inner_->native(); }

        // The editor moved it since the last call, or moves it now.
        bool takeMoved() noexcept;

        // A gesture of the processor's own (Processor::writeDial: Claude's set_dial, the dials' carry, a version's or
        // an example's dials), nested in any open one as beginGesture's is (the editor's drag, a MIDI touch: the host
        // sees one gesture, and its end is the last end's), but not the editor's move: the journal's attribution
        // (takeMoved) stays as it was. Ended by endGesture.
        void beginOwnGesture();

    private:
        std::unique_ptr<funkgui::ParamPort> inner_;
        int gestures_ = 0;
        bool moved_ = false;
    };
}
