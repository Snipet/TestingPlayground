// Source/plugin/ChatAgent.h: the chat with Claude, owned by the processor (so a turn goes on while the editor is
// closed) and shown by the editor through ChatPort.
//
// A turn: the user's message (with the plugin's files, the last compile and the dials, when they changed since Claude
// last saw them) goes to the Messages API, streamed; while Claude asks for tools, the agent runs them and sends the
// results back, up to kMaxRounds requests a turn. Claude's tools act on the plugin through ChatHost: read, write,
// edit, rename and delete the files (the editor's tabs follow, each change undoable there), run the program (RUN: the
// compile's outcome comes back), set a dial, render and measure what the program puts out (measure_output,
// dsp/Measure.h: Claude cannot hear), look at the panel (view_panel: a PNG the message thread draws headless,
// ChatHost::chatPicture, sent as an image in the tool's result and left out of the saved session), read the
// plugin's status, and ask the user (ask_user: questions with options, which the chat shows as cards to click).
//
// Checking its own work. view_sound renders the editor's files offline and draws the sound (dsp/SoundView.h,
// SoundPicture.h: the waveform and level, a spectrogram, the spectrum and a close-up) as a PNG in the result, which the
// chat shows too (the tool entry's picture; the session keeps its thumbnail, not the PNG). set_checks sets what the work
// must pass: metrics (dsp/Measure.h's metrics()) with bounds or targets (dsp/Goals.h), each in a render of its own; the
// agent measures them (evaluateChecks: one compile, one render for checks that share one) after each run_program that
// compiles, its results after the run's, and the chat shows them as one card a turn (ChatEntry::Kind::checks, brought
// up to date in place). A reply that ends a turn in which a tool changed something (the files, a run, a dial, the
// checks) while a check fails gets the results back as the user's line and the turn goes on, kMaxCheckNudges times a
// turn at most (a turn that only talks is never sent on); clearChecks (the card's Clear) and an empty set_checks stop
// it. The session keeps the checks; set_checks without checks lists them as it takes them, and its mode merge or
// remove changes some by label or index (remove refuses an index whose check has another label: the list moved). A
// check that names a dial the program lacks fails, saying so, and run_program names the checks a declaration change
// left so, and those from a snapshot that holds such a dial. tune_dials (dsp/Tune.h) searches up to eight float
// dials for goals by rendering offline (up to 400 renders and `seconds`, 120 by default and 170 at most, the activity
// counting them, the stop button or the time limit ending the search with the best so far) and sets the best values
// as set_dial does; its sweep renders one dial at given values instead, a table of the goals' metrics, and sets
// nothing. The tools that return numbers alone render up to 300 s, a picture's 10 (dsp/Measure.h, "How long a render
// may be"), and every render stops as it goes at the stop button, an MCP cancel or kRenderSeconds (renderCancel),
// its result saying how far it got.
//
// Projects (the projects tool, ChatProjects.cpp; ChatHost::chatProjects, ProjectCall below): Claude lists the user's
// projects and presets and, when the user asks, saves a version or a preset, opens a project or applies a preset, as
// the editor's PROJECTS does. Safe by design: a save only adds, a preset is replaced only with replace, unsaved changes
// are dropped only with discard, a factory project is never written, and nothing is deleted.
//
// A planner (setPlanner, chat.json "planner"): a second model plans each of the user's requests before the chosen one
// builds it (Claude Fable 5.1 planning the DSP and the architecture, Claude Sonnet 5.5 writing, running and testing).
// planTurn sends the conversation to the planner as another model sees it (forAnotherModel: no thinking, which is bound
// to the builder's model; no compaction or web blocks, which go back only to their own requests; no field another
// provider's answers left on a call, which the API refuses (providers::forAnthropic); the tools defined
// and tool_choice "none", so the history's calls stay valid and it calls none) with a brief after the user's message;
// its plan streams into a plan entry (ChatEntry::Kind::plan) and joins the user's message, which the builder has not
// seen yet, so the history stays append-only. The builder's ask_planner asks it again mid-turn (kMaxPlannerAsks a
// turn): the history up to the reply that asks, the question after it, the answer as the tool's result. A planner the
// models do not offer, or one that fails, leaves the request to the builder, with a note.
//
// Web search (setWeb; the API's server tools web_search and web_fetch, version 20260209, five of each a request, a
// fetch at most kWebFetchTokens of text, its citations on): Claude searches the web and reads pages and PDFs (a
// part's data sheet, a paper), on a model that takes it (/v1/models). The API runs them and returns their blocks
// (server_tool_use and the results, encrypted, and with dynamic filtering the code that filtered them), which go back
// in the history as they came; a reply paused in the middle (stop_reason pause_turn) is sent back as it is and goes on.
// What Claude searched and read is shown, and the answer's sources under it (the docs ask for citations). A model that
// cannot run dynamic filtering (a 400 that names allowed_callers) gets the tools called directly; an organisation that
// has web search turned off gets the turn without it, and the switch off. The tools a reply was made with bind its
// thinking: a turn whose tools differ from the history's (web search turned on or off) drops the thinking first.
//
// Pictures (attach): each file is read on the job thread (ChatImages.h: scaled to the API's advice, a thumbnail
// made) and waits in chatState().attached; send() puts them, as image blocks, before the message's text. A message
// takes kMaxPerMessage, the history kMaxInChat (each request carries every picture in it); the session keeps them,
// and each one's thumbnail for the conversation.
//
// A question (ask_user) pauses the turn: the reply's other tools run, their results wait in the history, and the
// question's entry stays open (ChatState::asking) until the user answers, with a click (answer(): its result is the
// labels chosen) or with a message (send(): its result says the user's own words follow, and they do, in the same
// message). Either starts the next turn. A turn's reply may ask once; a second ask_user in it is refused in its
// result. The session keeps an open question, so a project saved while one waits opens with it still waiting.
//
// Requests (ClaudeApi.h; the claude-api reference): stream on; adaptive thinking with summarized display and the chosen
// effort (the models list comes from /v1/models, filtered to those with adaptive thinking); max_tokens the model's own
// limit (from /v1/models, at most 128K), so a large file written in one call is not cut off; the system prompt
// (ChatPrompt.cpp) frozen and cached for an hour, the conversation cached by the top-level automatic breakpoint; the
// tools fixed for the session, each with eager_input_streaming, their input parsed strictly here; "fallbacks":
// "default" on the models that take it, so a classifier's refusal is retried on another model. The history is
// append-only (preserved thinking): every response's content goes back unchanged, but after a fallback block the
// blocks before it lose their thinking and tool calls, as the API asks. A refusal, the stop button and an error each
// end the turn with the history still valid.
//
// Fast mode (the fast-mode docs), when chosen, on the models that take it (/v1/models does not say which: the docs
// list Claude Opus 5.5, and Claude Opus 5 and Opus 4.8 in preview, behind the fast-mode-2026-02-01 beta): the request
// asks for "speed": "fast", up to 2.5 times the output's speed at twice the price, and goes without "fallbacks" (the
// docs say nothing of the two together). The footer says "fast" when the response's usage does. When fast capacity
// stays busy (a 429 or 529 again, after waiting once) the turn goes on at standard speed; when the API refuses fast
// mode itself (not open to the key, say), the turn goes on at standard speed and fast mode is turned off. A change of
// speed reads the conversation without the cache once: requests at different speeds share no cached prefix.
//
// Large work. A long conversation is compacted server-side (beta compact-2026-01-12, on the models that take it): once
// a request's input passes kCompactTrigger tokens the API summarizes what came before (with instructions that keep
// the sound's design and leave out code) and answers from the summary; the compaction block goes into the history, and
// what came before it is dropped: the history then starts at the block, in its assistant message, as the API's own
// examples continue after one. (Not at the user message before it: in the middle of a turn that is the results of
// tool calls the summary took away, which the API refuses.) A history from a session is repaired the same way when it
// loads, and before each message (repairHistory). The plugin's state sends a file's text only when Claude has not seen
// that text (in an earlier block, through read_file, or as it wrote it); after a compaction it sends every file
// again, with the next message or, when the summary came in the middle of a turn, with the tool results that follow
// it.
//
// A reply holds max_tokens, thinking included, and at high effort a hard design can think that long. The system prompt
// says so, and at xhigh and max effort a note with the user's message says it again (the API's advice: at those
// levels a long deliverable may be drafted in the thinking first). A reply cut off there goes back to the history as
// it came, and the turn goes on with a line that says so (after a cut-off tool call, its error result); a second reply
// cut off in a row ends the turn, with a note, since a reply that only thinks costs a whole reply each time. A session
// saved with another system prompt or other tools (an earlier version) loses its thinking blocks when it loads: the
// API binds them to the conversation they were made in (preserved thinking) and would refuse them.
//
// A request
// that fails on the connection, a rate limit, an overload or a server error, or whose stream breaks off before its
// end, is sent again (kMaxAttempts, retry-after honoured; what the broken stream showed is taken back). While a file
// streams in, the activity says which and how much has arrived. A tool's long output (a compile's messages) is cut.
//
// Access. The requests go to Anthropic with the user's own key (x-api-key), stored encrypted (ChatStore.h). A 401 means
// the key was refused: it stays, and the chat says so until the user replaces or removes it. When it starts, the
// agent deletes what an earlier version kept of a sign-in it no longer has (chatstore::forgetOldSignIn).
//
// Other providers (Providers.h; setProvider): an OpenAI-compatible server by its base URL (OpenAI, OpenRouter, Ollama
// or LM Studio on this Mac) or Google Gemini, each with its own key, encrypted and bound to its server (none for a server
// on this Mac or the local network), and its models (the server's list, or one typed; with none chosen, Gemini's and a
// local server's first, else the user chooses: Providers.h, picksFirst). Anthropic stays the default and its requests
// are as they were. Another's turn (startTurn: its provider, key and model from one moment) runs the same loop over
// the same history: the request is the
// Messages API's translated, the answer translated back, so the tools, the checks and their nudges, the retries, a
// reply cut off and ask_user work alike. What only Anthropic has is off for it (thinking kept, caching, compaction,
// fallbacks, fast mode, web search and the planner: their pills are hidden). The thinking level is its model's own where
// the model takes one (OpenRouter's, OpenAI's and Gemini's: Providers.h, Reasoning), chosen in the same menu and kept
// for each of its models (chat.json "efforts"); none chosen, none is sent (the model's own default, shown as the level
// when its server says it). A model that refuses the level sent is asked again at once without it, and has no levels
// for the rest of the session (effortRefused_), with a note. Its own reasoning
// shows and is never kept; a model that takes no pictures (its list says so, or it refuses one) gets them as lines of
// text, with a note; a model the server says cannot serve this chat leaves the menu; a small context window gets the
// prompt's short form (shortPrompt); a server that cut the conversation to fit its context is noted once (its
// prompt's tokens far under the request's).
//
// The chats, on this Mac only (ChatStore.h; CLAUDE.md, "The chats: on this Mac only"). The conversation shown is one of
// its owner's chats: the project open's (by the project's id), or, while none is, this instance's own. The processor
// names the owner (setOwner): opening another project keeps the chat shown with its project and shows that project's
// newest (a new chat when it has none); saving the editor's files as a project carries the chat to it (moved from the
// instance, copied from another project), and so does a project the chat's own turn opens (the projects tool: the
// conversation that asked for it goes on with it). The chat is written to the store (keep(): the conversation as
// saveState(true) has it, the panel's pictures kept, so a chat opened again keeps its thinking) when a turn ends, when
// the session is saved (keepIdle: not while a turn runs, whose end does), before another chat shows and when the agent
// goes; the session keeps only which chat it is (chatRef()), and a session from before keeps its conversation here
// once (adoptChat). A copy of a state in another live instance shows its chat as a chat of its own (restoreChat's `from`;
// the processor decides). The chat list (chatList) is its owner's, the newest first: new, open, rename, delete.
//
// Threads: the ChatPort calls run on the message thread; a turn runs on the agent's worker thread, which calls
// ChatHost (whose implementation must be safe from that thread) and the transport. Pictures are read on a job thread
// of their own, so the message thread never waits for them.
#pragma once

#include "plugin/Capture.h"
#include "plugin/ChatPort.h"
#include "plugin/ChatStore.h"
#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"
#include "plugin/Live.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Goals.h"
#include "dsp/Sources.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace playground::projectstore
{
    struct Project;
    struct Version;
}

namespace playground
{
    // Claude's projects (the projects tool: ChatProjects.cpp, ChatHost::chatProjects). Safe by design: a save only adds
    // (a version, a preset, the user's own copy of a factory project; a project keeps its newest kMaxVersions versions,
    // the result naming one let go), a preset is replaced only with `replace` (the old values said), and opening a
    // project over unsaved changes (the editor's files, its samples or the dials changed since they were opened or
    // saved), or applying a preset over dials moved so (the files stay), only with `discard`, which the user must have
    // asked for; nothing is deleted (that is the user's alone).
    struct ProjectCall
    {
        enum class Action : uint8_t { list, saveVersion, savePreset, applyPreset, open };
        Action action = Action::list;
        std::string name;                        // list: the project shown whole (none: every one); save_version: the
                                                 // project (none: the one open; another: a new one, or a version of it);
                                                 // save_preset, apply_preset: the preset; open: the project, Name@N a
                                                 // version (none or "@N": the one open)
        std::string note;                        // save_version: what changed, in Claude's words (the version's note)
        std::string category;                    // save_preset: the preset's; save_version: the project's
        bool replace = false;                    // save_preset: over a preset of that name
        bool discard = false;                    // open, apply_preset: over unsaved changes
        bool desktop = false;                    // Claude Desktop's (MCP: not journalled, it knows), else the plugin's chat
    };
    struct ProjectAnswer
    {
        bool ok = false;
        std::string text;                        // the tool's result (why, when not ok)
        std::string entry;                       // the chat's line ("saved version 5 of \"Engine\""), "" for none
        bool another = false;                    // open: another project's program is the editor's now (Claude's checks
                                                 // were the one before's: the agent clears them)
    };

    // What Claude's tools act on. Every call may come from the agent's worker thread.
    class ChatHost
    {
    public:
        virtual ~ChatHost() = default;

        virtual Draft chatDraft() const = 0;                        // the editor's files and tab
        // The files Claude wrote (the editor follows), `draft` being what the tool made of `read`, the draft it read:
        // the files it changed (their text, a name, one added or deleted) are written, the others keep their text as it
        // is now, and a tab the user added since stays, so typing that landed between the read and the write is never
        // reverted.
        virtual void chatSetDraft(const Draft& draft, const Draft& read) = 0;
        // chatSetDraft's conditional write (edit_file, edit_circuit): each file `read` names gets its text from `draft`
        // (and the tab `draft` shows is shown), in one change of the draft, under the same lock as the test, only while
        // every one of them is still the text the tool read (`read`'s); false, and nothing set, when a gesture or the
        // typing of the user's changed any of them meanwhile (or one is gone). The other files stay as they are now.
        virtual bool chatSetDraftIf(const Draft& draft, const Sources& read) = 0;
        // A netlist's session pins (EditorPort::schematicPins: *@ part, input and output lines, "" for none).
        virtual std::string chatSchematicPins(const std::string& file) const = 0;
        virtual CompileStatus chatRun(const Sources& files) = 0;    // RUN, then the compile's outcome
        virtual CompileStatus chatLastStatus() const = 0;
        virtual Sources chatRunning() const = 0;
        // The same files, and in `samples` the samples that program compiled with (CompileStatus::assets) and in `pins`
        // (when given) the configuration its netlists play with (CompileStatus::circuits, not the self-test's store), from
        // one look: what a render of the program that plays takes (compare's playing, trace's, view_panel's at).
        virtual Sources chatPlaying(Assets& samples, CircuitPins* pins = nullptr) const
        {
            samples = chatAssets();
            if (pins != nullptr)
                pins->clear();
            return chatRunning();
        }
        // What played before the last run that changed what plays (its files, a netlist's drawing aside, or the samples
        // its lines read), and in `samples` the samples it compiled with: compare's previous_run. None until a run has
        // changed the program since the plugin opened or loaded a state (it is not kept with the session, and a state's
        // program is no run's).
        virtual Sources chatPrevious(Assets& samples) const
        {
            samples = nullptr;
            return {};
        }
        // A project version's samples, decoded (those the samples now hold of the same file shared, not decoded again;
        // a file it names that cannot be one left out, in words in `refused`): what a render of that version takes.
        virtual Assets chatVersionSamples(const projectstore::Project& p, const projectstore::Version& v,
                                          std::vector<std::string>& refused) const
        {
            (void)p, (void)v, (void)refused;
            return chatAssets();
        }
        virtual Controls chatDials() const = 0;                     // the running program's
        virtual float chatDialValue(int slot) const = 0;            // plain value
        virtual void chatSetDial(int slot, float plain) = 0;        // a host gesture, applied soon on the message thread
        virtual float chatLoad() const = 0;
        virtual uint32_t chatAlerts() const = 0;
        // A picture (view_panel), made on the message thread; this thread waits for it (10 s at most, and no longer than
        // the agent's stopping(): the message thread may be the one waiting for the turn to end): the panel (`name`
        // a tabs page's title, "" for the user's), the whole editor, or a netlist's schematic (`name` its .cir file);
        // the panel or the editor with `state` drawn in place of what plays (picturePanel: view_panel's values and at).
        virtual PanelPicture chatPicture(PictureView view, const std::string& name, const PictureState& state) = 0;
        // For the live tools (listen, get_status: any thread, beside a running call; Live.h): what plays now; a tool
        // that listens (the meters and the scopes are measured only while one does, on: true while it listens); what
        // changed since a token (Journal.h; `issue`: the token returned is handed out); the dials' options' names.
        virtual void chatLive(LiveRead& io) const = 0;
        virtual void chatListening(bool on) = 0;
        virtual Changes chatChanges(const std::string& since, bool issue) = 0;
        virtual std::array<std::vector<std::string>, kMaxControls> chatDialOptions() const = 0;
        // The recorder (plugin/Capture.h; the capture tool): arm a recording (the program's own code plays while it is
        // armed), stop it (its take kept), take (the take kept), status, forget (all of it freed, off the audio thread).
        // stop and status are live (CLAUDE.md, "A live tool"): the recorder's own mutex and atomics only.
        virtual capture::Reply chatCapture(capture::Action action, const capture::Options& options) = 0;
        // For a render's "host" (ChatScenario.h): the host's sample rate and its largest block (0: not known); and the
        // project open in the editor (compare's version:N and preset:Name; "" for none).
        virtual double chatHostRate() const { return 0.0; }
        virtual int chatHostBlock() const { return 0; }
        virtual std::string chatProject() const { return {}; }
        // The user's A/B listening (EditorPort::ab) in words, one line ("" while there is none): its sides, the one that
        // plays, the dials, the loudness. Any thread (a copy under its own mutex): get_status and plugin_state say it.
        virtual std::string chatAb() const { return {}; }
        // run_function's "playing" (ChatFunction.cpp): the state that plays, copied by the audio thread at a block's end
        // into `s`'s buffers (Engine::Snapshot); waits for a block (kSnapshotWaitMs at most) and takes the ask back when
        // none comes, false then, with why. Never the audio thread's; nothing of what plays is written.
        static constexpr int kSnapshotWaitMs = 500;
        virtual bool chatSnapshot(Engine::Snapshot& s, std::string& why)
        {
            (void)s;
            why = "this host gives no copy of what plays";
            return false;
        }
        // run_program's line on a .gui file's layout (EditorPort.h's panelFitLine): its content larger than the PANEL view
        // at the editor's size, in words; "" when it fits, or when it cannot be said now. The worker's.
        virtual std::string chatPanelFit(const PanelSpec&, const Controls&) { return {}; }
        // MIDI learn (plugin/MidiLearn.h): the mappings and the dial learning. Any thread (under its own mutex): the dial
        // lines and get_status say them, matched to the controls they list by name (its `words` are by the slots of
        // the program apply() took, which a compile apply() has not taken yet may have moved).
        virtual midilearn::View chatMidi() const { return {}; }
        // The program's samples (dsp/Assets.h; the explorer's ASSETS), which its #pragma sample lines read: every render
        // of a tool compiles with them (scenario::resolve). Any thread (a copy under its own mutex). chatAddAsset adds
        // the audio file at `path` (a WAV, AIFF or FLAC on this computer) as `name` (its own name tidied when ""),
        // decoded on the calling thread; chatRemoveAsset and chatRenameAsset act on one by name. False, with why in
        // words (the caps, a file that is none of those), when it cannot; `done` says what was done.
        virtual Assets chatAssets() const { return {}; }
        virtual bool chatAddAsset(const std::string& path, const std::string& name, std::string& done, std::string& why)
        {
            (void)path, (void)name, (void)done;
            why = "this plugin keeps no samples";
            return false;
        }
        virtual bool chatRemoveAsset(const std::string& name, std::string& why)
        {
            (void)name;
            why = "this plugin keeps no samples";
            return false;
        }
        virtual bool chatRenameAsset(const std::string& name, const std::string& to, std::string& why)
        {
            (void)name, (void)to;
            why = "this plugin keeps no samples";
            return false;
        }
        // The projects tool (ProjectCall below): the projects and presets listed, or one saved, opened or applied as the
        // editor's PROJECTS would (on the message thread: this thread waits), and the answer in words. Never a delete.
        virtual ProjectAnswer chatProjects(const ProjectCall&) { return { false, "This plugin keeps no projects.", {}, false }; }
    };

    // A file's text as the editor holds it (TextEditor::sanitise: printable ASCII, tabs and LF), the one rule for
    // every text Claude's tools put in the draft (write_file's content, files and from, edit_file's new texts,
    // edit_circuit's values and names), so that the draft, a version, seen_, what runs and the tab are one text and
    // the editor's next push of its tabs changes nothing. What it changed, as a result's clause ("CR LF line ends
    // read as LF; 2 characters outside ASCII became '?', as the editor holds ASCII"), or "".
    std::string asEditorHolds(std::string& text);

    class ChatAgent final : public ChatPort
    {
    public:
        static constexpr int kMaxRounds = 80;                      // requests in one turn

        ChatAgent(ChatHost& host, std::unique_ptr<claude::Transport> transport);
        ~ChatAgent() override;                                      // stops a turn and waits for it
        ChatAgent(const ChatAgent&) = delete;
        ChatAgent& operator=(const ChatAgent&) = delete;

        // ChatPort
        uint64_t chatRevision() const override;
        ChatState chatState() const override;
        bool saveKey(const std::string& key, std::string& why) override;
        void forgetKey() override;
        bool setProvider(const ChatProvider& provider, const std::string& key, std::string& why) override;
        ChatProviderInfo providerInfo(const ChatProvider& provider) const override;
        void forgetProviderKey(const ChatProvider& provider) override;
        void setModel(const std::string& id) override;
        void setPlanner(const std::string& id) override;
        void setEffort(const std::string& effort) override;
        void setFast(bool on) override;
        void setWeb(bool on) override;
        void attach(const std::vector<std::string>& paths) override;
        void detach(uint32_t id) override;
        bool send(const std::string& text) override;
        void answer(const std::vector<std::string>& answers) override;
        void clearChecks() override;
        void stop() override;
        void newChat() override;
        ChatList chatList() const override;
        bool openChat(const std::string& id) override;
        void renameChat(const std::string& title) override;
        void deleteChat() override;
        std::shared_ptr<const UsageView> usage(usage::Range range) override;
        void resetUsage() override;
        // Probes only: how long a view of usage() stands before it is read again with nothing else moved (another
        // process's write: kUsageRereadSeconds by default).
        void setUsageRereadSecondsForTests(double seconds);
        static constexpr double kUsageRereadSeconds = 3.0;

        // The conversation (the API's messages and the view's entries, as JSON), and back. A turn that is running is not
        // saved half-done: its user message and what came back so far are. The panel's pictures (view_panel's,
        // view_sound's) are left out, a line in each one's place, unless `pictures` (the chat's own file on this Mac):
        // a history without them loads without its thinking. A chat whose tools' pictures pass kKeptPictureBytes (base64)
        // in all is kept without them too: its file would be rewritten whole after every turn.
        static constexpr std::size_t kKeptPictureBytes = 12u << 20;
        std::string saveState(bool pictures = false) const;
        void loadState(const std::string& json);

        // The chats (above). setOwner: the owner the conversation belongs to now (a project's id, or
        // chatstore::instanceOwner of the instance's id) and its name for the list ("" for none); open shows the new
        // owner's newest chat (the one shown kept with its owner), carry takes the chat shown along (a project saved:
        // from the instance it moves, from another project a copy goes, the old project keeping its own), move takes it
        // along from a project whose file is gone (nothing is left with that project). The same owner again changes
        // nothing but the name. An open asked from a turn's own thread waits for the turn's end (a carry or a move runs
        // at once: it never joins the worker). True when another chat shows. restoreChat: a session's chat (its id) when
        // it is this owner's (read from `from`'s chats when that is not "": a copy of another instance's state, shown as
        // a new chat of `owner`, written at once); else, for a project, its newest, and for an instance (or none
        // found) a new one. adoptChat: a session from before the chats were kept here: its conversation, when it holds
        // more than the chat's notes (adoptable), kept under `owner`, its id made from its text, so a session loaded
        // again finds the chat it became (and what was said since); else a new chat (restoreChat with no id). chatRef:
        // what the session keeps (keep() first). keep: the chat written when it changed since it was last written and
        // holds more than the chat's notes (any thread, no lock of the agent's held); a chat whose file another writer
        // changed meanwhile (another instance with it open) is kept under a new id, nothing lost; a project's chat
        // whose folder is gone since it was kept (the project deleted, with its chats, in another instance) is not
        // written back. A write that fails (or is held back so) says so in the chat once, is tried again at each keep,
        // and says so again when it is kept. keepIdle: keep, unless a turn or a call of Claude Desktop's runs (its
        // end, or the next keep, writes it): getStateInformation's, which a host may call at every change.
        enum class OwnerChange : uint8_t { open, carry, move };
        bool setOwner(const std::string& owner, const std::string& label, OwnerChange how);
        // A thread acting for the turn that runs (the message thread, while it runs a job a tool of the turn waits for:
        // the projects tool's, Processor::runClaudeJob): an open asked from it waits for the turn's end, as one asked
        // from the turn's own thread does (it could not stop and join the turn that waits for it). Such an open is
        // Claude Desktop's (the plugin's own chat's open is a carry: the processor's openProjectAs).
        void actForTurn(bool on);
        void restoreChat(const std::string& owner, const std::string& label, const std::string& id,
                         const std::string& from = {});
        void adoptChat(const std::string& json, const std::string& owner, const std::string& label);
        struct ChatRef
        {
            std::string owner, id;                                  // id "" while the chat holds nothing to keep
        };
        ChatRef chatRef() const;
        void keep();
        void keepIdle();
        // The id adoptChat gives a session's conversation (32 hex digits from its text); a session from before has no
        // instance id either, so the processor takes this one for it when the conversation is adoptable and kept with
        // the instance: the same session loaded again is the same owner. adoptable: whether a session's conversation
        // holds more than the chat's notes (a message, a check, an entry other than a note or an error): an empty one
        // (every instance's alike) or a lone note (the key saved) is nothing to keep, and no instance's.
        static std::string sessionChatId(const std::string& json);
        static bool adoptable(const std::string& json);
        // Whether the turn (or Claude Desktop's call) running was asked to stop: a host's wait for another thread
        // (chatPicture's for the message thread) ends at it, so a switch that stops and joins the turn never waits long.
        bool stopping() const noexcept { return cancel_.load(); }
        std::string chatOwner() const;

        // For probes: the transport (the next turn uses it), waiting for the current turn (and for the pictures
        // being read).
        void setTransport(std::unique_ptr<claude::Transport> transport);
        void waitIdle();
        // For probes: called in send() between its two looks at whether the chat is busy (the last turn's thread
        // joined, the plugin's state read), on the message thread: a probe starts a call of Claude Desktop's there.
        void setSendHookForTests(std::function<void()> hook) { sendHook_ = std::move(hook); }
        // For probes: called on the job thread before each picture is read: a probe holds the reads there.
        void setPictureReadHookForTests(std::function<void()> hook) { pictureReadHook_ = std::move(hook); }
        // For probes: called where edit_circuit has read the netlist and applied its commands, before each conditional
        // write (attempt 0, then 1 after a conflict), on the thread the tool runs on: a probe plants a gesture there.
        void setCircuitWriteHookForTests(std::function<void(int attempt)> hook) { circuitWriteHook_ = std::move(hook); }
        // For probes: the same for edit_file, where it has read the files and applied the edits, before each write.
        void setFileWriteHookForTests(std::function<void(int attempt)> hook) { fileWriteHook_ = std::move(hook); }

        // The system prompt and the tools as sent (for probes and the docs).
        static const std::string& systemPrompt();
        // The prompt's short form, for another provider's model whose context window is small (under
        // kShortPromptBelow tokens, or not said by a server on this Mac or the local network): systemPrompt() up to
        // its THREADS, then a line of what was left out (the large program, the interface, the library, the examples,
        // the projects, the circuits) and how to work without it.
        static constexpr int kShortPromptBelow = 96000;
        static const std::string& shortPrompt();
        // The system prompt as the MCP guide has it (mcpGuide): without the lines of "How you work" that only the
        // plugin's own chat needs (its <plugin_state>, the planner, web search, ask_user, its panel's markdown), which
        // are chatOnlyLines(), a line each, in their order; the two together are systemPrompt()'s text.
        static const std::string& guidePrompt();
        static std::string chatOnlyLines();
        // The MCP guide's cheat sheet (part "cheatsheet", mcp::guide::kCheatSheet): the contract, FMinus, the DSP numbers
        // and pitfalls and the library a line a file, on a page (ChatPrompt.cpp); mcpGuide puts it after the tools' notes.
        static std::string cheatSheet();
        // The program in several files the system prompt shows, for this plugin's kind (the program first).
        static Sources promptExample();
        static juce::var toolDefinitions();
        static juce::var compareDefinition();                       // compare's (ChatCompare.cpp)
        static juce::var traceDefinition();                         // trace's (ChatTrace.cpp)
        static juce::var projectsDefinition();                      // projects' (ChatProjects.cpp)
        static juce::var functionDefinition();                      // run_function's (ChatFunction.cpp)

        // MCP (plugin/McpLink.h): an assistant outside the plugin (Claude Desktop, through mcp/Main.cpp) working with the
        // chat's own tools. mcpTools(): toolDefinitions() but ask_user and ask_planner, in MCP's form ({ name, title,
        // description, inputSchema, annotations }), each description cut to its first paragraph (its first sentence when
        // it has one: Claude Desktop puts the tools in every conversation), view_panel and view_sound with MCP's own
        // `detail`, the annotations only what MCP's defaults do not say (mcp/McpSchema.h). mcpGuide(): a word on
        // working from outside, the system prompt as guidePrompt() has it, then what the descriptions lost (THE TOOLS:
        // WHAT THEIR SHORT DESCRIPTIONS LEAVE OUT, each tool's in its guide part), and last the lines only the plugin's
        // chat needs (chatOnlyLines(), under mcp::guide::kChatOnly: in part "all" alone); mcpGuidePart(): a part of it
        // (mcp/McpGuide.h; library:<file>:code from the library, adding nothing to the editor; library:host.fm from
        // the guide's THE HOST).
        //
        // mcpCall() runs one tool as a turn's would, on the caller's thread: refused while a turn of the chat runs; while
        // it runs the chat is busy (nothing can be sent; its stop button cancels a search), its activity says so, and what
        // it did shows in the conversation. `call` numbers it for mcpCancel (0: none), which stops it from any thread
        // (as the stop button would, but a tune stopped so leaves the dials as they were); a cancel that comes before the
        // call starts still counts. A live call (mcpLive: a read of what plays now) runs beside a running one, without
        // the busy state, and is never refused. Its content is MCP's: text blocks and images ({ type, data, mimeType }),
        // each picture at most kMcpPictureBytes (made smaller to fit, the text saying what was sent) unless its
        // arguments ask for `detail` "high".
        struct McpResult
        {
            juce::var content;
            bool isError = false;
        };
        static constexpr std::size_t kMcpPictureBytes = 75000;      // 100,000 characters in base64
        static juce::var mcpTools();
        static std::string mcpGuide();
        static std::string mcpGuidePart(const std::string& part, bool& found);
        // The calls that run on the MCP link's poll thread, beside a running call, without the busy state: get_status
        // (ChatHost's locked getters and atomics, the journal), listen (the meters, the scopes, the blocks' ring), and
        // capture's status and stop. One line a tool; such a tool keeps CLAUDE.md's live-tool contract.
        static bool mcpLive(const std::string& name, const juce::var& arguments);
        // Of the live calls, those that take seconds (listen): the link runs each on a thread of its own, so its poll
        // thread stays free (McpLink.h). One line a tool.
        static bool mcpLiveWaits(const std::string& name);
        // A tool's title in words, as Claude Desktop shows it ("Measure the sound"; the name itself for one with none).
        static std::string mcpTitle(const std::string& name);
        McpResult mcpCall(const std::string& name, const juce::var& arguments, uint64_t call = 0);
        void mcpCancel(uint64_t call);
        // A call of Claude Desktop's that is not live runs (any thread): what it changes is its own, which get_status
        // `since` does not report back to it (the Processor's journal asks).
        bool mcpWorking() const noexcept { return mcpWorking_.load(); }
        // MCP's marks as the session keeps them (the Processor's state, Live.h's KeptMark): the newest kMcpMarks tokens
        // get_status gave, oldest first, those kept from before the plugin was opened again first; and back, as marks
        // of before (mcpKept_), which the new journal does not know: get_status with one of their tokens says which
        // files changed since it (Live.cpp). Any thread.
        std::vector<KeptMark> mcpMarksToKeep();
        void restoreMcpMarks(std::vector<KeptMark> marks);

    private:
        static claude::Headers headersFor(const std::string& key);  // the key and the API version

        void runTurn();                                             // the worker
        bool streamOnce(const std::string& body, const std::string& beta, claude::MessageBuilder& builder,
                        int& status, std::string& error, int& retryAfter);
        juce::var buildRequest(std::string& beta) const;
        // The request the turn sends (ChatProviderTurn.cpp): buildRequest's for Anthropic, as it was; for another
        // provider, buildRequest's translated (Providers.h) with the turn's options. streamOther: streamOnce's for
        // another provider (its server, its key, its answer translated back into the builder). The worker's.
        std::string requestBody(std::string& beta);
        bool streamOther(const std::string& body, claude::MessageBuilder& builder, int& status, std::string& error,
                         int& retryAfter);
        // The usage ledger (ChatUsage.cpp, Usage.h): every request that reached an API and reported usage, whatever
        // became of it, added once (streamOnce, after its transport returns: a turn's rounds, each retry and broken
        // stream, the fast-mode and web tools' second requests, the planner's, a provider's work-around request), under
        // the model that served it; its cost added to the turn's (turnCost_), for the footer. Without mutex_ held.
        void countUsage(const claude::MessageBuilder& builder, bool other);
        // ", $0.12" (or ", ~$0.12", an estimate) for the footer while every request this turn has a cost; "" else.
        // Under mutex_.
        std::string turnCostText() const;
        // The Usage page's reads and its reset (ChatPort::usage, resetUsage), on the job thread: a snapshot of the
        // range priced and published as the view when it differs from the one held; the reset done (when still
        // pending: the destructor runs one the job thread never reached), then the view read again for the range last
        // asked. Under usageMutex_ only while the view and the flags change, never across the file's I/O.
        void readUsage(usage::Range range);
        void runUsageReset();
        void publishUsage(UsageView view);          // usageMutex_ held
        // Another provider: the key of `p`'s server ("" for none), read with mutex_ released (the keyring may ask the
        // user to unlock it, and wait: never under the lock the editor's chatState() takes) and kept as otherKey_ while
        // `p` is the provider chosen and no key was saved or removed meanwhile (keyGeneration_); whether one is kept for
        // `p` (no prompt: chatState's, before it takes the lock); its models fetched (the worker's, `cancel_` ending it,
        // or the job thread's, `listCancel_`: once a provider; a failure kept in otherListError_), whether this turn is
        // another provider's, and a request refused for a reason the turn can work around (a thinking level the model
        // does not take, a model that takes no pictures, Gemini refusing the tools' JSON Schema): the request made again
        // without it, said in the chat; true when the turn should ask again.
        std::string otherKeyOf(const ChatProvider& p);
        bool keyKept(const ChatProvider& p) const;
        void fetchOtherModels(const std::atomic<bool>& cancel);
        bool otherTurn() const;
        bool workAround(int status, const std::string& error);
        // The start of a turn (the worker's, first): the provider, and for another one its key, its models and the
        // turn's model and options, all from one moment (a provider chosen while the list was on its way, nothing
        // sent yet, is the turn's: it looks again). anthropic: the turn is Anthropic's (turnProvider_ set);
        // other: another provider's, ready (`maxTokens` its model's, 0 when its server keeps a limit of its own);
        // ended: it cannot start, said in the chat.
        enum class Start : uint8_t { anthropic, other, ended };
        Start startTurn(int& maxTokens);
        // The provider's state in chatState (mutex held; `kept`: keyKept's, asked before), the settings kept
        // (chat.json), a model's entry of the list; the model in effect (the one chosen, else the list's first where a
        // turn takes it: picksFirst); a model no list described, as the provider's (its levels by its name, none after
        // a refusal); the thinking level chosen for the provider's model ("" for none), and its key in otherEfforts_.
        void otherState(ChatState& s, bool kept) const;
        void saveProviderChoice() const;
        const ChatModel* otherModel(const std::string& id) const;
        std::string otherModelNow() const;
        ChatModel otherPlain(const std::string& id) const;
        std::string otherEffort(const ChatProvider& p, const std::string& model) const;
        static std::pair<std::string, std::string> effortKey(const ChatProvider& p, const std::string& model);
        std::string contextBlock();                                 // the plugin's state, when it changed
        juce::var runTool(const juce::var& toolUse);               // a tool_result block; its rows failed if an error
        juce::var toolResult(const juce::var& toolUse);            // runTool's work
        juce::var compareTool(const juce::var& toolUse);           // compare's (ChatCompare.cpp)
        juce::var traceTool(const juce::var& toolUse);             // trace's (ChatTrace.cpp)
        juce::var captureTool(const juce::var& toolUse);           // capture's (ChatCapture.cpp)
        std::string projectsText(const juce::var& input, bool& isError);   // projects' (ChatProjects.cpp)
        // view_panel's values and at: the state its picture draws (at's render of the program that plays, offline, to
        // its moment, the dials not named the plugin's or, with dials_from, the defaults: `defaults`, which the result's
        // first line says); "" or why it cannot be.
        std::string panelState(const juce::var& input, PictureState& state, bool& defaults);
        std::string toolText(const std::string& name, const juce::var& input, bool& isError);
        // read_file and edit_file (ChatFiles.cpp): a file whole, a range of its lines or the lines that hold a text or
        // match a pattern (regex), in one file or in every file, numbered (context lines around each; or only how many,
        // count; past the first skip), a part read marking nothing seen; one passage or a list of them replaced, in one
        // file or in several (`files`: a list, or a unified diff), or every match of find (replaceText), all or none, in one write that never overwrites what the user typed
        // meanwhile (chatSetDraftIf), and with `expected` only while the file is still that version. Every read and
        // write says the file's version (its text's hash); keepVersion keeps the text of the newest kFileVersions
        // given (mutex held), so a refused write shows what changed since. A refusal's text (`refused`) is kept too, but
        // counts as no call that gave it: a version kept already keeps when and how it was given, a new one is not
        // `asked` (so a refusal never makes a change Claude's own).
        // run_function (ChatFunction.cpp): one of the program's functions called by name, in a compile of its own on
        // this thread (Patch::call), on a fresh instance of the editor's files or a copy of the state that plays
        // (ChatHost::chatSnapshot); its result or what went wrong, in words.
        std::string runFunction(const juce::var& input, bool& isError);
        // write_file (ChatImport.cpp): a file written whole; several as one change (files); a file or a folder on this
        // Mac the user named brought in (from: its program files, text, checked, all or none); the editor's files out to
        // a folder of their own on this Mac (to), adding files only.
        std::string writeFileText(const juce::var& input, bool& isError);
        std::string readFileText(const juce::var& input, bool& isError);
        std::string editFileText(const juce::var& input, bool& isError);
        // edit_file's find: every match of a text or a pattern replaced by new_text, line by line (never a netlist's *@
        // line), in one file or in every file, all or none in one chatSetDraftIf of every file it read.
        std::string replaceText(const juce::var& input, bool& isError);
        // A write refused because `file` is no longer `version` (`now` its text): whose change, and its lines changed
        // since (the kept text of `version`); `many`: the call named several files, none written.
        std::string changedSinceText(const std::string& file, const std::string& version, const std::string& now, bool many);
        std::string keepVersion(const std::string& name, const std::string& text, bool refused = false);
        // The checks measured on the editor's files and the plugin's dials: their entry brought up to date, and the
        // results in words for Claude ("" with no checks). The worker's.
        std::string evaluateChecks();
        // The planner (setPlanner). planTurn: before a request's first reply, its plan, joined to the user's message.
        // consultPlanner: one request to the planning model with `history` (another model's view of it: no thinking,
        // no server tools' blocks) and `brief` after its last user message, no tool it may call, streamed into a plan
        // entry; its text, or "" with why. The worker's.
        void planTurn();
        std::string consultPlanner(const juce::Array<juce::var>& history, const std::string& brief, bool answer,
                                   std::string& why);
        std::map<std::string, float> currentDials() const;          // the running program's, by name
        void fetchModels();                                         // the worker, before a turn, once a key is known
        void ensureKey();                                           // the key, loaded (mutex_ not held: it is
                                                                    // read without it, as otherKeyOf's)
        void refused(const std::string& key, int status, const std::string& why);   // a 401: that key is refused
                                                                    // (mutex held)
        void queueJob(std::function<void()> job);                   // a picture to read, on the job thread
        void jobLoop();                                             // the job thread
        void waitJobs();                                            // until no job is queued or running
        void touch();                                               // revision + 1 (mutex held)
        void appendAnswer(const juce::Array<juce::var>& content);   // an assistant message into the history (mutex held)
        // The results the last reply's tool calls still lack (an open question's says `instead`), for the next
        // user message; the open question closes (mutex held).
        juce::Array<juce::var> missingResults(const std::string& instead);
        int picturesInHistory() const;                              // the user's pictures in the history (mutex held)
        void deliver(juce::Array<juce::var> content);               // a user message's blocks into the history, and
                                                                    // a turn (mutex held; the caller starts it)
        void addMessage(juce::Array<juce::var> content);            // deliver's history part (mutex held)
        // A message's blocks: the missing results, the pictures read up to `upTo` (into `entry`), the words, the state
        // (mutex held).
        juce::Array<juce::var> userMessage(const std::string& words, const std::string& context, uint64_t upTo,
                                           ChatEntry& entry);
        bool takePending();                                         // the worker: a message waiting for its pictures
        static constexpr int kPictureWaitSeconds = 30;              // the longest it waits
        void repairHistory();                                       // one the API takes (mutex held)
        void dropThinking();                                        // the history without thinking (mutex held)
        int maxTokensFor(const std::string& id) const;              // a request's max_tokens (mutex held)
        static std::string promptPrint();                           // the system prompt's and tools' hash
        void markSeen(const std::string& name, const std::string& text);   // the chat's Claude knows this text (mutex
                                                                    // held; Claude Desktop's views mark nothing here)
        // Its own edit turned `before` into `after`: it knows `after` only if it knew `before` (mutex held). A file it
        // read a part of, or the user changed since it saw it, stays unseen, so the next state sends it.
        void carrySeen(const std::string& name, const std::string& before, const std::string& after);
        void addEntry(ChatEntry::Kind kind, std::string text);     // mutex held
        void appendToLast(ChatEntry::Kind kind, std::string_view text);   // mutex held
        void setActivity(std::string activity);
        // A render's cancel, for the tools that render (Measure.h's Cancel): the chat's stop and an MCP cancel
        // (cancel_), measure::kRenderSeconds at most, and a long render's progress in the activity ("Measuring the
        // sound: 84 of 300 s") a few times a second.
        measure::Cancel renderCancel(std::string doing);
        void startWorker();
        void joinWorker();
        // loadState without stopping a turn (its own thread may call it: a change of owner it waited for).
        void readState(const std::string& json);

        // The chats (ChatChats.cpp). keepLocked: keep() with storeMutex_ held. The chat shown replaced (storeMutex_ held,
        // no turn running): by a chat of the store, by a copy of one (another owner's, as a new chat of `owner`, written
        // at once), by the owner's newest (else a new one), by a new one. clearChat: the conversation forgotten (mutex
        // held). worthKeeping, titleNow: mutex held. applyPendingOwner: a change of owner asked from the turn's thread,
        // at its end (an MCP call's: before it frees the chat), unless another was asked meanwhile (setOwner, a session's
        // restoreChat or adoptChat drop it).
        void keepLocked();
        void showChat(const chatstore::Loaded& chat, const std::string& owner, const std::string& label);
        void showCopy(const chatstore::Loaded& chat, const std::string& owner, const std::string& label);
        void showNewest(const std::string& owner, const std::string& label);
        void showFresh(const std::string& owner, const std::string& label);
        void clearChat();
        bool worthKeeping() const;
        std::string titleNow() const;
        void applyPendingOwner();
        bool onTurnThread() const;

        ChatHost& host_;
        std::unique_ptr<claude::Transport> transport_;
        std::function<void(int)> circuitWriteHook_;                 // setCircuitWriteHookForTests
        std::function<void(int)> fileWriteHook_;                    // setFileWriteHookForTests
        std::function<void()> sendHook_;                            // setSendHookForTests
        std::function<void()> pictureReadHook_;                     // setPictureReadHookForTests

        mutable std::mutex mutex_;
        uint64_t revision_ = 1;
        std::vector<ChatEntry> entries_;
        uint64_t conversation_ = 0;                                 // ChatState::conversation: readState, clearChat
        juce::Array<juce::var> messages_;                           // the API's history
        std::vector<ChatModel> models_;
        bool modelsFetched_ = false;
        std::string model_, effort_;
        std::string turnModel_, turnEffort_;                        // the turn under way's (the worker's)
        bool fast_ = false;                                         // fast mode chosen
        bool turnFast_ = false;                                     // the turn under way asks for it
        bool web_ = false;                                          // web search chosen
        bool turnWeb_ = false;                                      // the turn under way has the web tools
        bool historyWeb_ = false;                                   // the tools the history's thinking was made with
        std::set<std::string> webDirect_;                           // models that call the web tools directly
        std::string activity_;
        std::string usage_;
        std::string key_;                                           // loaded on first use
        std::string keyProblem_;
        uint64_t keyGeneration_ = 0;                                // + 1 for each key saved or removed: a read
                                                                    // begun before is not kept
        // The provider (Providers.h; setProvider), machine-wide. Anthropic's state is the members above (model_,
        // models_, key_, effort_, ...), untouched while another is chosen; another's is here: its model, its models
        // (the server's list, else the one chosen), its key (bound to its server) and why it cannot be used, the model
        // chosen with each provider (chatstore::modelKey). The turn under way's: its provider, whether its pictures go
        // (the model takes them), Gemini's schemas as JSON Schema, the prompt's short form (a small context).
        ChatProvider provider_;
        std::vector<ChatModel> otherModels_;
        std::string otherModel_;
        std::string otherKey_, otherKeyFor_, otherKeyProblem_;     // the key, the server it was read for
        std::map<std::string, std::string> otherChosen_;
        // The thinking level chosen for each provider's model ((modelKey, model) -> level; chat.json "efforts"), and the
        // models that refused one this session: no levels for them until the agent starts again.
        std::map<std::pair<std::string, std::string>, std::string> otherEfforts_;
        std::set<std::pair<std::string, std::string>> effortRefused_;
        bool otherFetched_ = false;
        std::string otherListError_;                                // why its list was not read, in words ("" when it was)
        std::atomic<bool> listCancel_{ false };                     // the job's list fetch: set by stop() and the end
        ChatProvider turnProvider_;
        bool turnImages_ = true, turnJsonSchema_ = true, turnShort_ = false;
        bool turnHadPictures_ = false;                              // the request holds pictures
        bool schemaRefused_ = false;                                // Gemini refused JSON Schema: its subset from then on
        std::size_t turnTextBytes_ = 0;                             // the request's text (its pictures aside), bytes
        bool cutNoted_ = false;                                     // the chat said a server cut the conversation
        bool picturesNoted_ = false;                                // the chat said the model gets no pictures
        std::map<std::string, uint64_t> seen_;                      // file name -> the text the chat's Claude saw
                                                                    // (hashText; Claude Desktop's: mcpMarks_)
        struct FileVersion
        {
            std::string name, version;                              // a file, and its text's version as read_file says it
            std::shared_ptr<const std::string> text;
            uint64_t given = 0;                                     // when it was given last (versionsGiven_ then)
            bool asked = true;                                      // given by a read or a write, not a refusal alone
            bool desktop = false;                                   // given to Claude Desktop (an MCP call), else to the
                                                                    // plugin's own chat
        };
        static constexpr std::size_t kFileVersions = 16;
        std::deque<FileVersion> fileVersions_;                      // the newest versions read or written (keepVersion)
        uint64_t versionsGiven_ = 0;                                // keepVersion's count
        uint64_t lastSignature_ = 0;                                // the plugin's state as Claude last got it
        bool resendState_ = false;                                  // a summary came: the state goes with what follows
        bool busy_ = false;
        std::string asking_;                                        // the open question's tool_use id ("" for none)
        std::vector<measure::Check> checks_;                        // Claude's checks (set_checks), in the session
        std::string planner_;                                       // the planning model ("" for none), machine-wide
        bool planNext_ = false;                                     // the next turn starts with a plan (a message)
        int plannerAsks_ = 0;                                       // ask_planner's calls this turn
        std::string turnPlanUsage_;                                 // the planner's tokens this turn, for the footer
        struct TurnCost
        {
            int64_t microUsd = 0;
            int requests = 0;                                       // the requests counted this turn
            bool unknown = false;                                   // one had no price: no cost said
            bool estimate = false;                                  // one was an estimate
        };
        TurnCost turnCost_;                                         // this turn's requests' cost (countUsage)
        bool checksFailing_ = false;                                // one failed when they were last measured
        struct Attached
        {
            ChatPicturePtr picture;
            std::string mediaType;
            std::vector<uint8_t> data;                              // as sent
            uint64_t seq = 0;                                       // its read's number (attachIssued_)
        };
        std::vector<Attached> attached_;                            // for the next message
        int attaching_ = 0;                                         // files the job thread still reads
        uint64_t attachEpoch_ = 0;                                  // a new chat: files read for the old one go
        // The pictures' reads, numbered as they are asked for (attach) and as they end (the job thread runs them in
        // order): a message sent while some are read waits for those asked for before it (Pending, the turn's worker).
        uint64_t attachIssued_ = 0, attachDone_ = 0;
        std::condition_variable attachCv_;                          // attachDone_ moved, or a stop
        // A message sent while pictures were still being read: its words, the plugin's state as it was sent, the
        // reads it waits for, and its entry, which shows at once and gets the pictures when they are in.
        struct Pending
        {
            std::string words, context;
            uint64_t upTo = 0;
            std::size_t entry = 0;
        };
        std::optional<Pending> pending_;

        // The live tools (Live.cpp). liveTool: listen and get_status, from a turn (`caller` none: the stop button stops
        // listen) or from MCP (by its call's number: mcpCancel stops it); other live calls go to runTool. MCP's own view
        // (never the chat's seen_: what one Claude saw the other did not): the files' texts at each token get_status
        // gave it (a mark; `since` diffs against its token's, so each conversation of a server shared by several has
        // its own), and the journal's token at its last result (the hint a result starts with when something changed
        // since: one for the instance, whichever conversation called).
        struct LiveCall
        {
            bool mcp = false;
            uint64_t call = 0;
        };
        struct McpMark
        {
            std::string token;
            std::shared_ptr<const std::vector<SourceFile>> files;   // shared while they stay the same
        };
        static constexpr std::size_t kMcpMarks = 16;                // the newest tokens' files kept
        static constexpr std::size_t kMcpStopped = 64;              // cancels kept for calls not started (the newest)
        static juce::var statusDefinition();                        // get_status's (toolDefinitions)
        static juce::var listenDefinition();                        // listen's
        juce::var liveTool(const juce::var& use, const LiveCall& caller);
        std::string listenText(const juce::var& input, const std::function<bool()>& stopped, bool& isError);
        std::string statusText(const juce::var& input, bool mcp, bool& isError);
        std::string mcpHint();                                      // "" when nothing changed since MCP's last result
        std::mutex liveMutex_;                                      // the members below (a leaf)
        std::deque<McpMark> mcpMarks_;                              // the newest kMcpMarks tokens MCP was given
        std::deque<KeptMark> mcpKept_;                              // and those of before the plugin was opened again
        std::set<uint64_t> mcpStopped_;                             // MCP calls cancelled that are not the one running
                                                                    // (a listen, one about to start): mcpCancel's
        std::string mcpLook_;                                       // the journal's token at MCP's last result
        std::atomic<bool> mcpWorking_{ false };                     // a call of Claude Desktop's, not live, runs

        // The chat shown (above): its owner, the owner's name, its id, the user's title (named), when it was made, its file
        // as last written or read here (another writer's change shows), revision_ when it was last written and what it
        // held then (storedDigest_, keepLocked's: a redraw that changed nothing writes nothing; 0: unknown); storeMutex_
        // keeps the writes and the replacing of the chat one at a time (never taken under mutex_). An open asked from a
        // turn's own thread (turnThread_: the worker's, or an MCP call's; Claude Desktop's projects open) waits in
        // pendingOwner_.
        std::string chatOwner_, ownerLabel_;
        std::string chatId_ = chatstore::newId();
        std::string chatTitle_;
        bool chatNamed_ = false;
        int64_t chatCreated_ = 0;
        chatstore::Stamp chatStamp_;
        uint64_t storedRevision_ = 0;
        uint64_t storedDigest_ = 0;
        mutable std::mutex storeMutex_;
        struct PendingOwner
        {
            bool set = false;
            std::string owner, label;
        };
        PendingOwner pendingOwner_;                                 // mutex held
        bool keepFailed_ = false;                                   // the last keep failed, and the chat said so (mutex held)
        std::atomic<std::thread::id> turnThread_{};
        std::atomic<std::thread::id> helperThread_{};               // actForTurn's

        std::atomic<bool> cancel_{ false };
        bool mcpNoted_ = false;                                      // the conversation says Claude Desktop is at work
        uint64_t mcpRunning_ = 0;                                   // the MCP call running (its number; mutex held)
        bool mcpRunningCancelled_ = false;                          // that call was cancelled (mutex held): its own
                                                                    // cancel, never another call's after it
        std::thread worker_;
        // Pictures to read: jobs run in order on one thread, started on first use, so the message thread never waits
        // for them.
        std::mutex jobMutex_;
        std::condition_variable jobCv_;
        std::deque<std::function<void()>> jobs_;
        bool jobRunning_ = false;                                   // a job is running
        bool jobStop_ = false;
        std::thread jobThread_;
        // The Usage page's view (usage(), above): under usageMutex_ (the editor's, the job thread's; never mutex_).
        mutable std::mutex usageMutex_;
        std::shared_ptr<const UsageView> usageView_ = std::make_shared<const UsageView>();
        bool usageReading_ = false;                                 // a read is queued or running
        usage::Range usageAsked_ = usage::Range::week;              // the range last asked for
        uint64_t usageReadRevision_ = 0;                            // usage::revision() when the last read began
        std::chrono::steady_clock::time_point usageReadAt_{};       // when the last read ended
        bool usageResetPending_ = false;                            // a reset confirmed and not yet run
        double usageReread_ = kUsageRereadSeconds;
    };
}
