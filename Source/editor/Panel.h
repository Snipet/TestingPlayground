// Source/editor/Panel.h: the plugin's interface, a funkgui::Panel: an FMinus program's editor and its dials, and the
// chat with Claude beside them. It is GPU-free: funkgui::EditorHost runs it live in the plugin
// (Source/editor/gpu/CreateEditorGpu.cpp) and funkgui::HeadlessHost runs it in the probes (gui.dump, gui.chat), drawing
// the same frames. A .gui file's shaders it hands to FunkGui's registry (funkgui/gpu/ShaderLibrary.h, which makes no
// GPU call until bgfx draws them) and records as Canvas::shader draws: the GPU draws them, SoftRaster a placeholder.
//
// It is resizable, in both directions, from kMinWidth x kMinHeight (logical px) up: the chat takes about a third of the
// width at the right, the explorer about a sixth at the left, until the user drags their edges (Columns.h: the
// explorer's right edge, the chat's left; each within its least and its most, the program's card keeping
// kProgramMinW, a double-click resetting it; the session keeps the widths, EditorPort::explorerWidth and chatWidth, and
// this Mac keeps them as the default for new instances); the editor and the chat's transcript take the height that is
// left.
//
//   explorer  at the left, a card like the chat's (Explorer.h): the program's files (a click shows one, a double click
//             renames it, its cross closes it, the header's + makes one), its LLVM IR (a click opens the IR tab on a
//             view, IrView.h), the library (a click adds a file as a tab, #included, or shows it), the examples (a
//             click starts afresh from one, as EXAMPLES) and the presets on this Mac (a click loads one, asking first
//             when the editor has changes no preset holds; the header's + opens PRESETS); its
//             sections fold, and the whole card folds away (its chevron; the button at the left of the program's card's
//             header opens it; EditorPort::explorerOpen); in the PANEL view, or a window too narrow for it, it is away
//   header    the product name, its kind and version, the project's pill and, while a project is open, its presets
//             (the arrows either side of the preset's name and place, "Bright 2/7", a dot when its dials moved; the
//             name opens their list, PresetPicker.h; [ and ] step them while no text field has the caret), and the
//             THEME cells (GRAPHITE, PAPER)
//   toolbar   RUN (Cmd-Return in the editor too), EXAMPLES (a menu: the example starts afresh, as a new instance,
//             its files the only tabs and the dials at their defaults, asking first when changes would be lost; it
//             runs), LIBRARY (a menu of ready-made building blocks, Library.h: one becomes a tab and the program
//             #includes it), the status (running, edited, compiling, the errors' count, the compile time) and, at
//             the right, the audio thread's load, a snowflake left of it while the frozen copy plays (the dials compiled
//             in, Patch.h) and the output guard's alerts further left (NaN replaced, clipped)
//   files     a tab for each of the program's files (Sources.h; at most kMaxFiles): the program's first, then the
//             files it can #include by name. A click shows a file, a double-click edits its name in place (Return keeps
//             it, Escape cancels), the + adds a file (its name being edited), a tab's x closes its file (asking first
//             when it has text). A tab whose file the last run's errors name has a mark before its name. At the bar's
//             right end, apart, the IR tab while it is open (IrView.h: the program's LLVM IR, read only, none of its
//             files), opened from the explorer's LLVM IR rows; left of it, while a netlist's tab shows, the Netlist /
//             Schematic pills (SchematicView.h: its drawing, a view of the file that edits its text; Cmd-Shift-D)
//   editor    a funkgui::TextEditor for each file, the shown file's drawn (the processor keeps the files and the tab:
//             EditorPort::draft), made a code editor by CodeAssist.h: FMinus highlighted, the problems' squiggles and
//             marks (RUN's, and a check while typing), completion, signature help, hover cards, Format Document, the
//             find bar, the line commands, go to definition, and a status line under it
//   problems  the last run's messages, a line each (the first four, and how many more), each with its file; a click
//             on one with a line number shows its file and puts the caret there
//   dials     the running program's parameters, P1 to P32 (a funkgui::Dial each, labelled with the parameter's name
//             or its #pragma panel label; an unused slot shows nothing), placed by DialLayout.h: in the titled
//             sections the program's #pragma panel lines ask for, framed as on an analog synth's panel, or else in
//             order, in as few rows as the column's width holds. A drag, the wheel or the keys write through the
//             processor's ports, and a double-click returns to the parameter's fminus::default. The editor gives up
//             the height that more rows take. A program with a .gui file (fminus/GuiSpec.h) has its interface instead:
//             its containers, controls, displays and text placed by GuiLayout.h and made of GuiItems.h, as tall as it
//             is under the editor (up to the room the editor leaves), and scrolled by the wheel where it is taller or
//             wider than its area. In the CODE view the editor's status line's Panel switch (Cmd-J) hides it (the
//             dials or the interface, and a netlist's fold bar): the editor then has the column. The session keeps one
//             choice for the program's code and one for its interface's files (.gui, .sc, .sh), both shown at first
//   view      the toolbar's CODE and PANEL cells: PANEL gives the interface the whole left column (the tabs, the editor
//             and the problems go), CODE brings the editor back; the session keeps the choice (EditorPort::panelView)
//   window    the button left of them (the plugin's editor only): the code editor and the explorer in a window of their
//             own (Role::code, below), this window showing the panel full size meanwhile; the session keeps it
//             (EditorPort::codeWindow)
//   chat      at the right, a rounded card set apart from the square panel (ChatStyle.h): its header (the title, a new
//             chat (asking first), the API key), the conversation (ChatView.h: bubbles, light markdown, thinking rows,
//             cards of what Claude did, code blocks with Copy, an empty chat's suggestions), the message box that grows
//             with its text (Return sends it, Shift-Return breaks the line) with the model and thinking pills (menus)
//             and the round send-or-stop button in its bar (ChatItems.h), and the line under it (what Claude is doing,
//             else the last reply's tokens)
//   key       a dialog over the panel for the model provider and its API key: the Provider pill at its title's right (a
//             menu: Anthropic, the default, OpenAI, OpenRouter, Google Gemini, Ollama or LM Studio on this Mac, another
//             OpenAI-compatible server), a masked key field (Cmd-V pastes), and for another provider its server's
//             address (an OpenAI-compatible one's) and its model, Tab moving between them; Save, Cancel and, when a key
//             is stored for that provider, Remove key. Anthropic's is as it always was. It opens from the header's
//             key, the empty chat's button, and when a message is sent before the chat can send (the message goes
//             once it is saved)
//   settings  the Standalone's only (EditorPort::devices): SETTINGS, the audio and MIDI devices in a card over the panel
//             (SettingsCard.h), opened by the app's Settings... (Cmd-,: EditorPort::settingsAsked, polled each frame)
//             and by the muted notice's button; while an effect's input is muted (JUCE's guard against a feedback loop)
//             the status bar says so at its right, beside that button
//   footer    the spec line of the control under the hand (or focused, or last touched), and the ZOOM cells
//
// Keys go to a text field while it has the caret (the editor: Tab indents there; the chat's message box and its
// transcript; Escape leaves each), to a tab's name while it is edited, and to the key dialog while it is open;
// otherwise Tab moves the focus ring over the controls, and the arrows, Page and Home/End adjust the focused dial.
// Every parameter write goes through one funkgui::GestureController (begin/set/end gestures for the host).
//
// Unchanged frames (FunkGui's Panel::needsDraw, v0.25.0): an editor whose picture has not changed records, uploads and
// draws nothing; its last frame stays up. needsDraw() is true only when something drawn changed since the last
// draw():
//   - dirty_, set wherever the Panel changes what it shows (input, a refresh of the processor's state, a layout, a
//     menu's or a file chooser's answer: the host the items reach is HostProxy, which marks the Panel before the
//     callback runs), which each tick hands on to undrawn_, kept until draw();
//   - a shown item moving (settled() false: an ease, a drag, a display's new values, Claude working, a shader that
//     animates, and what moves with no input to it: a .gui control's value or live= mark, a pad's, an envelope's, a
//     shader's inputs, a circuit's knob, held a quarter second after the last move: Items.h Stirred), and the frame
//     after it settles;
//   - the mix of every shown item's drawKey() (Items.h; a hidden item's is left out, as draw() leaves the item out)
//     and of the Panel's own words and states, taken each tick, which catches what changes with no input and no
//     motion: a host's automation of a dial, a live= mark, a shader's inputs, a number display's new readout (eight a
//     second, a frame each, the rate calm), a label read through a callback, a notice that ran out.
// Once a second it records the frame anyway (kRedrawEvery), a safety net: EditorHost then submits it only if it
// differs from the last frame drawn (canvas/FrameCompare.h), so an idle editor still sends the GPU nothing.
// wantsFullRate() (FunkGui's FramePump: 60 Hz, else about 12) is a shown item moving, a change not ticked yet, a
// compile running, or input in the last kInputHold seconds (a pointer's move, press, drag, release or exit, a wheel, a
// key, a double-click, a file drag): what is hidden (a dial a .gui file's interface draws, a page not shown) never keeps
// the rate up. Without the hold the pump went back to its calm 12 Hz after each input frame (a hover changes what is
// drawn at once, no ease), and the next event's frame waited for the calm tick: the hover trailed the pointer.
// SoftEditorHost holds after input too. A release and the pointer's exit also nudge the host's pump at once
// (HostServices::nudgeFullRate), which FunkGui's EditorHost does for the other events.
// gui.dump DRAW checks the sources, gui.code TYPES the type names' colours, gui.styles PAGES a hidden page's knob and
// RATE what moves with no input (a knob's, a pad's, an envelope's), gui.shaders DRAW a shader's inputs, gui.panel
// SETTLE displays that stand still and readouts that change.
//
// The meters' reader: the audio thread measures the levels and fills the scopes' rings only while someone reads them
// (Engine::readers). A Panel is a reader from its making, and while the host ticks it (the code window's never: it
// shows no display). EditorHost ticks a Panel only while its window shows (a minimised or hidden window is skipped)
// and calls idle() ten times a second whatever, so a Panel that sees kHiddenIdles idle calls and kHiddenAfter seconds
// with no tick lets the reader go (its displays then show nothing anyone sees), and takes it again at its next tick:
// the levels start again from that block and the scopes from silence, as for any reader that comes (Engine.h).
// HeadlessHost ticks before each idle call, so a probe's Panel stays a reader unless the probe calls idle() alone.
// gui.panel HIDDEN checks.
//
// Message thread only. The ports and the EditorPort belong to the processor and outlive the Panel.
#pragma once

#include "editor/Columns.h"
#include "editor/Explorer.h"
#include "editor/AbCard.h"
#include "editor/ExportCard.h"
#include "editor/NameCard.h"
#include "editor/PresetPicker.h"
#include "editor/ProjectBrowser.h"
#include "editor/SettingsCard.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>

#include "dsp/Controls.h"
#include "dsp/Sources.h"
#include "dsp/Engine.h"
#include "editor/DialLayout.h"
#include "editor/DisplayItem.h"
#include "editor/GuiLayout.h"
#include "fminus/Lang.h"
#include "fminus/PanelSpec.h"

#include "plugin/ChatPort.h"

#include <funkgui/text/LineEdit.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace funkgui
{
    class Canvas;
    class CellModel;
    class HostServices;
    class TextEditor;
    struct Theme;
}

namespace playground
{
    class EditorPort;
}

namespace playground::ui
{
    class Item;                                          // one control or view on the panel (Panel.cpp)
    class FilesItem;                                     // the files' tabs and editors (Panel.cpp)
    class ChatView;                                      // the chat's conversation (ChatView.h)
    class InputItem;                                     // the chat's message box (ChatItems.h)
    class CodeAssist;                                    // the editor's language features (CodeAssist.h)
    class SchematicView;                                 // a netlist's Schematic view (SchematicView.h)
    class DisplayItem;                                   // a program's display (DisplayItem.h)
    struct DisplayFeed;                                  // the displays' samples and spectra (DisplayItem.h)
    class GuiControlItem;                                // a .gui file's control (GuiItems.h)
    class GuiView;                                       // a .gui file's interface: its items (GuiView.h)

    // The size (logical px): the default, the least.
    inline constexpr int kDefaultWidth = 1360, kDefaultHeight = 760;
    inline constexpr int kMinWidth = 1000, kMinHeight = 600;
    // After any input the full rate holds this long (s; Panel.h "Unchanged frames", Panel::wantsFullRate): the next
    // event of a hover, a drag or typing finds the pump at 60 Hz.
    inline constexpr double kInputHold = 0.3;
    // A notice (a save's outcome, RANDOM's or PROJECTS' word) stands in the status line this long (s, the host's clock)
    // in place of the compile's status.
    inline constexpr double kNoticeFor = 4.0;

    // Where the PANEL view puts a .gui file's interface in an editor W x H (logical px) whose chat is open (else its
    // rail) and `chatWant` px wide (0: its share), as layout() does: the program's card across the window but the chat's
    // column, from the tabs' line to over the status bar. With the explorer open (`explorerOpen`, `explorerWant` px
    // wide, 0: its share) the chat is as wide as the CODE view leaves it beside the explorer, narrower in some windows
    // (layout(): a switch never lays the conversation out again); pass false while the code is in its own window, which
    // has the explorer. The panel's fit (EditorPort.h's panelFitLine) lays it out there.
    funkgui::Rect panelViewArea(float W, float H, bool chatOpen, float chatWant, bool explorerOpen, float explorerWant);

    // The panel's own primitive tags, for probes that find what they check by tag.
    inline constexpr funkgui::Tag kHeaderTextTag = funkgui::tags::firstProduct;       // name, kind and version
    inline constexpr funkgui::Tag kStatusTag = funkgui::tags::firstProduct + 1;       // the toolbar's status
    inline constexpr funkgui::Tag kProblemTag = funkgui::tags::firstProduct + 2;      // the problems' lines
    inline constexpr funkgui::Tag kMarkTag = funkgui::tags::firstProduct + 3;         // unused: the editor's line marks
                                                                                      // (TextEditor, EDIT_MARK) show errors
    inline constexpr funkgui::Tag kTabTag = funkgui::tags::firstProduct + 4;          // the files' tabs
    inline constexpr funkgui::Tag kChatTag = funkgui::tags::firstProduct + 5;         // the chat's own text
    inline constexpr funkgui::Tag kKeyDialogTag = funkgui::tags::firstProduct + 6;    // the key dialog
    inline constexpr funkgui::Tag kProjectsTag = funkgui::tags::firstProduct + 8;     // the projects' card
    inline constexpr funkgui::Tag kSectionTag = funkgui::tags::firstProduct + 7;      // the dials' sections
    inline constexpr funkgui::Tag kExplorerTag = funkgui::tags::firstProduct + 9;     // the explorer's card
    inline constexpr funkgui::Tag kExportTag = funkgui::tags::firstProduct + 10;      // EXPORT's card
    inline constexpr funkgui::Tag kFrozenTag = funkgui::tags::firstProduct + 11;      // the top bar's snowflake
    inline constexpr funkgui::Tag kSettingsTag = funkgui::tags::firstProduct + 12;    // SETTINGS' card
    inline constexpr funkgui::Tag kMutedTag = funkgui::tags::firstProduct + 13;       // the muted input's notice
    inline constexpr funkgui::Tag kLogoTag = funkgui::tags::firstProduct + 14;        // the top bar's mark (LogoMark.h)
    inline constexpr funkgui::Tag kAbTag = funkgui::tags::firstProduct + 15;          // A/B's card
    inline constexpr funkgui::Tag kNameTag = funkgui::tags::firstProduct + 16;        // the name card
    inline constexpr funkgui::Tag kPresetsTag = funkgui::tags::firstProduct + 17;     // the top bar's list of presets
    inline constexpr funkgui::Tag kRecordTag = funkgui::tags::firstProduct + 18;      // the recorder's notice (REC)

    // One message of the last run, as the problems list shows it.
    struct Problem
    {
        std::string file;                                // the file it is in ("program.fm")
        int line = 0, column = 0;                        // 1-based; 0: no place (the plugin's own message)
        bool error = true;                               // else a note
        bool warning = false;                            // a warning (not an error)
        std::string text;                                // the message, without its file and place
    };

    // FMinus's messages ("program.fm:3:12: error: ...", one a line) as Problems, in order; clang's include stack
    // ("In file included from program.fm:2:") left out.
    std::vector<Problem> parseProblems(const std::string& messages);

    // An interface's pages as EditorPort keeps them: each tabs node's chosen page's title (in the nodes' order), and
    // back, by node (a title no page has: the first page).
    std::vector<std::string> guiPageTitles(const GuiTree& gui, const std::vector<int>& pages);
    std::vector<int> guiPagesOf(const GuiTree& gui, const std::vector<std::string>& titles);

    // The code window's size (logical px): the default, the least. It has no chat and no panel, so it can be smaller.
    inline constexpr int kCodeDefaultWidth = 1080, kCodeDefaultHeight = 760;
    inline constexpr int kCodeMinWidth = 720, kCodeMinHeight = 480;

    class Panel final : public funkgui::Panel
    {
    public:
        // What a Panel is for: the editor (the plugin's window; the probes'), a picture (view_panel's headless Panel,
        // Picture.cpp: it owns no probe and publishes no session pins, so what it lays out never becomes the user's), or
        // the code window (below: the code editor and the explorer in a top-level window of their own).
        enum class Role : uint8_t { editor, picture, code };
        Panel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, Role role);
        Panel(std::span<funkgui::ParamPort* const> ports, EditorPort& port, bool picture = false)
            : Panel(ports, port, picture ? Role::picture : Role::editor)
        {
        }
        ~Panel() override;

        // The code window (EditorPort::codeWindow; the plugin's editor makes it: editor/gpu/CodeWindow.h). The top bar's
        // window button (or the session) takes the code editor out of the editor's window into a window of its own, with
        // the explorer: the same editor, not a copy (its tabs, their texts, undo steps, carets and scrolls, the find bar,
        // the IR tab, the netlists' views and probes), which lendEditor hands to the code window's Panel (Role::code) and
        // takeEditorBack takes back. While it is out the editor's window shows the panel full size (as the PANEL view,
        // which the session's choice keeps for when it comes back), its Run runs the code window's files, and its Code
        // half brings that window to the front; the code window has the top bar, the explorer and the program's card
        // (no chat, no panel under the code), its Panel half bringing the editor's window to the front. The button, or
        // the code window's close, brings the editor back. The window's owner gives the editor's Panel its hooks; a
        // Panel without them (headless, without the GPU) shows no button and keeps the editor in place.
        struct WindowHooks
        {
            std::function<void(bool out)> out;           // the window to follow the session's choice (made or let go,
                                                         // later on the message thread: never inside the click)
            std::function<void()> raise;                 // the other window to the front
        };
        void setWindowHooks(WindowHooks hooks);
        Role role() const noexcept { return role_; }
        bool editorOut() const noexcept { return out_; } // the editor's Panel: the editor is in the code window (a
                                                         // picture's: an editor had it out when it was made,
                                                         // EditorPort::codeWindowOpen)
        bool windowButtonShown() const;
        funkgui::Point centreOfWindowButton() const;
        void setEditorOut(bool out);                     // the button: the session's choice, then the hooks' out
        void lendEditor(Panel& code);                    // the editor's Panel: `code` takes the editor (then shows it)
        void takeEditorBack();                           // ... and gives it back (before it goes)
        Panel* peer() const noexcept { return peer_; }   // the other window's Panel while the editor is out

        Panel(const Panel&) = delete;
        Panel& operator=(const Panel&) = delete;

        // For probes.
        funkgui::TextEditor& editor() noexcept;         // the shown file's
        CodeAssist& codeAssist() noexcept;               // its completion, hover, find bar, problems (CodeAssist.h)
        funkgui::TextEditor& fileEditor(int i);          // file i's (shown or not)
        int codeTextSize() const;                        // the text size its editors draw in, tenths of a px
        int fileCount() const;
        std::string fileName(int i) const;
        int activeFile() const;
        bool renamingFile() const;
        funkgui::Point centreOfTab(int i) const;
        funkgui::Point centreOfNewTab() const;
        funkgui::Point centreOfCloseTab(int i) const;
        std::optional<funkgui::Point> centreOfDial(int slot) const;    // nullopt for an unused slot
        std::string dialSpecLine(int slot) const;                      // its footer line (the dials', not a .gui's)
        bool dialMidiMarked(int slot) const;                           // its MIDI mark: a control moves it ...
        bool dialMidiLearning(int slot) const;                         // ... it learns one
        int dialRows() const noexcept { return dials_.rows; }             // the rows of dials, or of sections
        const DialsLayout& dialsLayout() const noexcept { return dials_; }
        const DisplayItem& displayItem(int d) const;                      // display d of the program's (shown or not)
        const DisplayFeed& displayFeed() const noexcept { return *feed_; } // the displays' samples and spectra
        // A reader of the meters now (EditorPort::addMeterReader; "The meters' reader" above).
        bool readingMeters() const noexcept { return reading_; }
        // A shown item unsettled at the last tick: the motion part of needsDraw() and wantsFullRate(), without the
        // Panel's own changes (dirty_: a refresh's, the top bar's CPU figure among them, which moves with real time).
        bool moving() const noexcept { return moving_; }
        // A .gui file's interface: whether the program has one, the PANEL view, its layout, slot's control, a page
        // chosen, a scroll (the wheel's, by px).
        bool hasGui() const noexcept { return !panelSpec_.gui.empty(); }
        bool panelView() const noexcept { return panelView_; }
        void setPanelView(bool on);
        // The panel under the code (the editor's status line's Panel switch, Cmd-J): whether the CODE view shows the
        // program's panel (its dials, or its .gui file's interface) under the editor. One choice while the program's
        // code shows (its FMinus files, a netlist, the IR), one while a file of its interface does (a .gui file, a
        // shader's .sc or .sh: Items.h's isInterfaceFile), both shown at first; the session keeps them (EditorPort::codePanel)
        // and the tab shown picks the one that applies. Hidden, the editor (or a netlist's drawing) has the column; the
        // PANEL view always shows the panel, and so do view_panel's pictures of it.
        bool codePanelShown() const;                     // the choice for the tab shown
        bool codePanelHidden() const noexcept { return panelHidden_; }   // hidden as layout() placed it (CODE view)
        void setCodePanel(bool shown);                   // the choice for the tab shown, kept in the session
        void toggleCodePanel() { setCodePanel(!codePanelShown()); }
        funkgui::Point centreOfPanelSwitch() const;      // the status line's switch
        bool panelSwitchLabelled() const;                // its word beside its icon (else the icon alone)
        const GuiLayout& guiLayout() const noexcept;
        Scroll guiScroller(bool horizontal) const noexcept;   // a .gui interface's scroll on that axis (Scroll.h), and
        funkgui::Rect guiBarArea(bool horizontal) const noexcept;   // where its bar is
        const funkgui::Rect& programCard() const noexcept { return programCard_; }   // the program's card
        const PanelSpec& panelSpec() const noexcept { return panelSpec_; }      // the program's that the panel shows
        const GuiControlItem& guiControl(int slot) const;
        const Item& guiItem(int kind, int k) const;   // kind 0: a pad, 1: an envelope, 2: tabs, 3: a keyboard, 4: a shader
        void chooseGuiPage(int node, int page);
        void scrollGui(float dx, float dy);
        int layouts() const noexcept { return layouts_; }    // layout() calls so far (a scroll makes none)
        funkgui::Point centreOfView(bool panel) const;                    // the CODE or the PANEL cell
        funkgui::Point centreOfButton(int slot, int value) const;         // a buttons dial's row for that value
        funkgui::Rect chatCard() const noexcept { return card_; }
        funkgui::Point centreOfRun() const;
        funkgui::Point centreOfExamples() const;
        funkgui::Point centreOfLibrary() const;
        funkgui::Point centreOfRandom() const;
        funkgui::Point centreOfProjects() const;
        // PROJECTS: the card (ProjectBrowser.h), opened on the project open, its name (or the program's title: its
        // first line's words before a colon) in its field; with `preset`, the field a preset's name.
        void openProjects(bool preset = false);
        ProjectBrowser& projectBrowser() noexcept { return projects_; }
        // EXPORT (ExportCard.h): a project's info and its export; Import's chooser (a .zip or .fmpack, also dropped on the
        // panel), each a project of its own, PROJECTS then open on it.
        void openExport(const std::string& project);
        ExportCard& exportCard() noexcept { return export_; }
        NameCard& nameCard() noexcept { return name_; }
        // Claude Desktop as this Panel shows it (probes): the chat header's subtitle and badge ("" for none), and where
        // the badge is while it shows (the chat open), and its mark on the rail (the chat folded).
        const std::string& desktopLine() const noexcept { return desktop_; }
        const std::string& desktopBadge() const noexcept { return desktopBadge_; }
        std::optional<funkgui::Point> centreOfDesktopBadge() const;
        std::optional<funkgui::Point> centreOfDesktopMark() const;
        void chooseImport();
        // A/B (AbCard.h): the editor's files against a version of `project` (0: its newest), played in turn.
        void openAb(const std::string& project, int version);
        AbCard& abCard() noexcept { return ab_; }
        // The top bar's presets (EditorPort::presetCycle): the arrows step the project open's presets (and [ and ],
        // while no text field has the caret), wrapping; the name between them opens their list (PresetPicker.h).
        void stepPresets(int step);
        void openPresets();
        PresetPicker& presetPicker() noexcept { return picker_; }
        const PresetCycle& presetCycle() const noexcept { return cycle_; }
        funkgui::Point centreOfPresetStep(int step) const;   // the arrow: -1 the previous, +1 the next
        funkgui::Point centreOfPresetName() const;
        std::string presetLabel() const;                 // the name between the arrows: "Bright", "(no preset)"
        std::string presetDetail() const;                // and its place: "3/7", "-/7"
        // "PROJECT  NAME  V3  EDITED  PRESET  BRIGHT  EDITED" (what the top bar's pill says, in words), "" for none.
        const std::string& projectText() const noexcept { return projectText_; }
        // Cmd-S: a new version of the project open (its name in the status), or, with none open, PROJECTS.
        void saveVersion();
        // RANDOM (fminus/Random.h): the program for `seed` and its library files, a fresh start as EXAMPLES makes one
        // (the only tabs, the dials at their defaults, no project open), run. False when it asked first (a menu: the
        // editor's changes would be lost), the menu's choice then making it.
        bool makeRandom(uint32_t seed);
        // LIBRARY's choice (Library.h): the file as a tab, #included by the program; shown when it is there. False for
        // a name the library has not, or no room for a tab.
        bool addLibraryFile(const std::string& name);
        funkgui::Point centreOfProblem(int i) const;
        const std::vector<Problem>& problems() const noexcept { return problems_; }
        const std::string& statusText() const noexcept { return status_; }
        // The top bar's mark: its tile's top-left as last drawn (LogoMark.h's px are from it).
        funkgui::Point markTile() const noexcept { return markTile_; }
        // The top bar's load at its right: the alerts' words and the CPU's share ("CLIPPED   CPU 4.2%"), and the
        // snowflake left of the share while the frozen copy plays (EditorPort::frozen), its place.
        const std::string& meterText() const noexcept { return meter_; }
        bool frozenShown() const noexcept { return frozenShown_; }
        funkgui::Point centreOfFrozen() const;
        void runNow();                                   // RUN
        // The draft and the tabs. mergeDraft: a draft the editor has not merged (a host's load, Claude's write) into
        // the tabs, each keeping what was typed in it since the editor last gave its files where that draft left the
        // file as given, and the tabs added, renamed or closed since where it left those files as given (an added tab
        // shown stays shown, its name still being typed; true when one came). pushFiles: the tabs to the processor,
        // never over such a draft (which
        // merges first: EditorPort::setDraftIf); false when another came meanwhile, for the next refresh to merge.
        bool mergeDraft();
        bool pushFiles();
        // The chat.
        funkgui::TextEditor& chatInput() noexcept;       // the message box
        const std::string& chatPlaceholder() const;      // what it says while empty
        const std::string& transcript() const;           // the conversation as shown (or the hint)
        const std::string& chatActivity() const noexcept { return activity_; }
        funkgui::Point centreOfSend() const;             // send, or stop
        funkgui::Point centreOfModel() const;            // the model's pill (a menu)
        funkgui::Point centreOfThinking() const;         // the thinking level's pill (a menu)
        std::optional<funkgui::Point> centreOfFast() const;   // fast mode's pill (a switch), on a model that takes it
        std::optional<funkgui::Point> centreOfWeb() const;    // web search's pill (a switch), on a model that takes it
        funkgui::Point centreOfAttach() const;                 // the message box's button that adds pictures
        const InputItem& chatInputItem() const;                // the message box (its pictures)
        bool dropShown() const noexcept { return dropActive_; }
        funkgui::Point centreOfNewChat() const;
        funkgui::Point centreOfKey() const;
        // The explorer at the left (Explorer.h): open, or folded away (the program's card takes the room); its header's
        // chevron folds it, the button at the left of the program's card's header opens it; the session keeps it
        // (EditorPort::explorerOpen).
        bool explorerOpen() const noexcept { return explorerOpen_; }
        // Open, in the CODE view and given its room (the program's card keeping kProgramMinW): else nothing at the left
        // but its button in the program's card's header, which opens it (folding the chat when there is no room).
        bool explorerShown() const noexcept { return explorerShown_; }
        void setExplorerOpen(bool open);
        funkgui::Point centreOfExplorerToggle() const;
        ExplorerItem& explorer();
        const ExplorerItem& explorer() const;
        int explorerBuilds() const noexcept { return explorerBuilds_; }   // its rows made, so far
        funkgui::Rect explorerCard() const noexcept { return explorerCard_; }
        // EXAMPLES' choice (Examples.h): the example and its library files, a fresh start (startAfresh), run.
        void loadExample(int i);
        // A fresh start (EditorPort::startAfresh) from `next`, asking first (a menu, its item `ask`) when the editor's
        // files have changes nothing holds; `what` names it in the journal. True when it started at once.
        bool startAfresh(const Sources& next, const std::string& what, const std::string& ask);
        // The IR tab (IrView.h): open, shown, its view (an irview::View) and name; openIrView() as the explorer's LLVM IR
        // row does (the tab opened on that view, shown, the editor focused), closeIrView() as its close box.
        bool irOpen() const;
        bool irShown() const;
        int irView() const;
        std::string irName() const;
        void openIrView(int view);
        void closeIrView();
        funkgui::Point centreOfIrTab() const;
        funkgui::Point centreOfIrClose() const;
        funkgui::Rect irTabRect() const;                 // the IR tab (empty while it is closed)
        funkgui::Rect fileTabRect(int i) const;          // file i's tab
        // A netlist's two views (SchematicView.h): the pills at the tab bar's right end while a .cir tab shows (Netlist,
        // Schematic; icon-only when their labels would narrow the files' tabs below their least), Cmd-Shift-D, the view
        // and its drawing. In the Schematic view the problems list is hidden, the editor keeps at least
        // max(312, 55 % of the column), and dials that do not fit under it fold into a bar ("32 DIALS   SHOW").
        bool schematicShown() const;
        void setSchematic(bool on);                      // the netlist shown's view, as its pills set it
        SchematicView& schematic();
        funkgui::Point centreOfViewPill(bool schematic) const;
        funkgui::Rect viewPillRect(bool schematic) const;
        bool viewPillsShown() const;
        bool viewPillsLabelled() const;
        funkgui::Rect editorArea() const;                // the files' editor's rect (a view's, the status line's too)
        bool dialsFolded() const noexcept { return dialsFolded_; }   // the dials in the fold's bar, hidden
        bool dialsBarShown() const noexcept { return dialsBar_; }    // the bar (SHOW or HIDE) under the editor
        funkgui::Point centreOfDialsBar() const;
        // Whether the program's card's header shows the whole status at the panel's size (no ellipsis).
        bool statusWhole() const;
        // The schematic's live probes (SchematicView.h): a Panel owns its set from its first armed one, unless told
        // never to (view_panel's window picture, Picture.cpp: a headless Panel that owns no probe and changes none).
        void setOwnsProbes(bool owns) noexcept { ownsProbes_ = owns; }
        bool ownsProbes() const noexcept { return ownsProbes_; }
        int probeOwner() const noexcept { return probeOwner_; }
        // What a picture's Panel shows in place of what the Engine publishes (view_panel's values and at, Picture.cpp):
        // `frames` (none: the Engine's meters) at the first until playShown(), then the next one each frame, the last
        // kept (each display's readout made again from it); the scopes' samples with them (oldest first); and over
        // either, `taps` (a tap's value) and the audio's levels (linear: peak and RMS; < 0: as they are).
        struct ShownState
        {
            std::vector<Meters> frames;
            std::vector<float> scopeIn, scopeOut;
            std::vector<std::pair<int, float>> taps;
            float inLevel = -1.0f, outLevel = -1.0f;
        };
        void showState(ShownState state);
        void playShown() noexcept { shownPlaying_ = true; }
        bool shownDone() const noexcept { return shown_.frames.empty() || shownAt_ + 1 >= shown_.frames.size(); }
        // The chat's column: open, or folded into a rail at the right (the editor and the panel take the room). The
        // header's chevron folds it, the rail's button opens it; the session keeps it (EditorPort::chatOpen).
        bool chatOpen() const noexcept { return chatOpen_; }
        void setChatOpen(bool open);
        funkgui::Point centreOfChatToggle() const;       // the header's chevron, or the rail's button
        bool chatUnread() const noexcept { return unread_; }   // folded, Claude has written since
        ChatView& chatView();                            // the conversation (its own probe hooks)
        const ChatView& chatView() const;
        // The columns' widths (Columns.h): the explorer's card (0: away) and the chat's (or its rail) as layout() fit them,
        // and each edge's reach; the edges a user drags (the explorer's right, the chat's left: shown while the card is),
        // their middles. The widths asked: the session's (EditorPort::explorerWidth, chatWidth), else this Mac's default,
        // else 0 (a share of the window).
        const columns::Fit& columnFit() const noexcept { return fit_; }
        float explorerWant() const noexcept { return explorerWant_; }
        float chatWant() const noexcept { return chatWant_; }
        bool explorerEdgeShown() const;
        bool chatEdgeShown() const;
        bool edgeFocused(bool chat) const noexcept { return focus_ >= 0 && focus_ == (chat ? chatEdgeItem_ : explorerEdgeItem_); }
        funkgui::Point centreOfExplorerEdge() const;
        funkgui::Point centreOfChatEdge() const;
        // The key dialog.
        bool keyDialogOpen() const noexcept { return keyOpen_ && !renaming_; }
        bool renameDialogOpen() const noexcept { return keyOpen_ && renaming_; }
        funkgui::Point centreOfChats() const;            // the chats' pill (the chat shown: its menu)
        const std::string& keyDialogWhy() const noexcept { return keyWhy_; }
        funkgui::Point centreOfKeyField() const;
        funkgui::Point centreOfKeySave() const;
        funkgui::Point centreOfKeyCancel() const;
        std::optional<funkgui::Point> centreOfKeyRemove() const;      // nullopt while no key is stored
        // The provider's settings in it: the Provider pill, the server's address and the model fields (nullopt while the
        // provider shown has none), the preset shown (an index of kChatPresets) and the field the keys go to (0 the
        // key, 1 the address, 2 the model), and a preset chosen as the pill's menu would.
        funkgui::Point centreOfKeyProvider() const;
        std::optional<funkgui::Point> centreOfKeyBase() const;
        std::optional<funkgui::Point> centreOfKeyModel() const;
        int keyDialogPreset() const noexcept { return dialogPreset_; }
        int keyDialogField() const noexcept { return dialogField_; }
        void chooseKeyPreset(int preset);
        // SETTINGS (the Standalone's: EditorPort::devices; nothing in a host): the card, opened (false with no devices or
        // while another card is open); the notice while an effect's input is muted, and its Settings... button.
        bool openSettings();
        SettingsCard& settingsCard() noexcept { return settings_; }

        // The dropdowns (Dropdown.h): a menu the Panel's host shows is open and was opened from `anchor` (a trigger's
        // rect): that trigger draws open until the menu is answered, dismissed or replaced (HostProxy keeps it).
        bool menuOpenAt(const funkgui::Rect& anchor) const noexcept;
        bool mutedNoticeShown() const noexcept { return mutedShown_; }
        funkgui::Point centreOfMutedSettings() const;
        // The recorder (EditorPort::recording: the capture tool's, recording for Claude): its notice in the status bar
        // (REC, the seconds, the marks) while it is armed, with Mark; markNow, the user's mark of this moment (Mark,
        // Cmd-Shift-M), which the status confirms.
        bool recordShown() const noexcept { return recording_.stage != RecordingState::Stage::off; }
        std::string recordText() const;
        funkgui::Point centreOfMark() const;
        funkgui::Point centreOfZoom() const;             // the status bar's zoom pill
        void markNow();

        // funkgui::Panel
        void  attach(funkgui::HostServices&) override;
        int   width() const override;
        int   height() const override;
        bool  resizable() const override { return true; }
        int   minWidth() const override { return role_ == Role::code ? kCodeMinWidth : kMinWidth; }
        int   minHeight() const override { return role_ == Role::code ? kCodeMinHeight : kMinHeight; }
        void  resize(int w, int h) override;
        void  tick(float dt) override;
        void  idle(double nowSec) override;
        void  draw(funkgui::Canvas&, const funkgui::Theme&) override;
        bool  needsDraw() const override;                // above, "Unchanged frames"
        bool  wantsFullRate() const override;
        void  pointerMove(const funkgui::PointerEvent&) override;
        void  pointerExit() override;
        void  pointerDown(const funkgui::PointerEvent&) override;
        void  pointerDrag(const funkgui::PointerEvent&) override;
        void  pointerUp(const funkgui::PointerEvent&) override;
        void  doubleClick(const funkgui::PointerEvent&) override;
        bool  wheel(const funkgui::WheelEvent&) override;
        bool  key(const funkgui::KeyEvent&) override;
        funkgui::Cursor cursor() const override;
        void  accessibility(std::vector<funkgui::A11yItem>&) const override;
        uint32_t a11yRevision() const override;
        void  a11yAction(uint32_t id, funkgui::A11yAction, double value = 0) override;
        void  closeGestures() override;
        // Pictures dropped on the chat's card go to the next message (ChatPort::attach); while a drag of pictures is
        // over the editor the card says so.
        bool  filesInterest(const std::vector<std::string>& paths) const override;
        void  filesDragEnter(const std::vector<std::string>& paths, float x, float y) override;
        void  filesDragMove(float x, float y) override;
        void  filesDragExit() override;
        void  filesDropped(const std::vector<std::string>& paths) override;

        // The Standalone's window (EditorPort::windowButtons): the room the top bar keeps at its left for the window's
        // buttons (logical px at the host's zoom; 0 in a host's window), where the product's mark then starts. A press
        // on the top bar where nothing is drags that window (EditorPort::dragWindow).
        float windowButtonsRoom() const noexcept { return buttonsRoom_; }

    private:
        // The code window (above).
        Role role_ = Role::editor;
        bool out_ = false;                               // editorOut()
        Panel* peer_ = nullptr;
        WindowHooks windowHooks_;
        int windowItem_ = -1;                            // the top bar's window button
        bool hasChat() const noexcept { return role_ != Role::code; }
        bool full() const noexcept { return role_ != Role::code && (panelView_ || out_); }   // laid out as PANEL
        void bindFiles();                                // the files' hooks to this Panel (made, lent, taken back)
        void swapEditor(Panel& other);                   // the editor and what follows it, between two Panels
        Panel& editorPanel() noexcept { return out_ && peer_ != nullptr ? *peer_ : *this; }   // whose files RUN runs
        void pushEditor();                               // its files to the processor (a rename typed ended)

        float buttonsRoomNow() const;                    // the room for the port's buttons now (layout() keeps it)
        void syncWindowButtons();                        // each frame: a new room lays out again (layout() tells
                                                         // the port where they go)
        float buttonsRoom_ = 0.0f;
        funkgui::Point markTile_{};                      // the mark's tile, as draw() last placed it
        bool buttonsPlaced_ = false;                     // the port was told where they go (while they are there)
        bool windowPress_ = false;                       // the last press went to the window (dragWindow): its
                                                         // double-click is none of the panel's
        bool closingPress_ = false;                      // the last press closed a card (outside it, or its Done):
                                                         // the rest of its click (a double-click's second press,
                                                         // its doubleClick) reaches nothing under it
        bool projectsPress_ = false;                     // the press began in PROJECTS: its drag and release go there
        bool guiBarPress_ = false;                       // the press was on a .gui interface's bar: its thumb drags

        int  itemAt(funkgui::Point) const;               // the item under p, -1 for none (an edge before the cards'
                                                         // items, under the code editor's overlays)
        bool moveFocus(int dir);
        void setFocus(int item);
        void refresh(double nowSec);                     // the port's status, draft and dials, when they changed
        void openExamples();
        void openLibrary();
        uint32_t nextSeed();                             // RANDOM's: from the clock and a count
        void notice(const std::string& text);            // the status says it for a few seconds
        void jumpTo(const Problem&);
        void closeFile(int tab);                         // a tab's x: asks first when the file has text
        FilesItem& files();
        const FilesItem& files() const;
        void layout();                                   // every item's place, for width_ x height_
        bool layoutSchematic_ = false;                   // layout() gave the Schematic view its room
        bool layoutStale() const;                        // the tab shown wants another room than layout() gave
        // The panel under the code: the choices ([0] the code's, [1] the interface's files'), as the port last had
        // them (refresh() takes another Panel's or a session's); hidden by layout().
        bool codePanel_[2] = { true, true };
        bool panelHidden_ = false;
        bool interfaceShown() const;                     // the tab shown is a file of the interface
        void syncCodePanel();
        bool dialsOpen_ = false;                         // the fold's bar shows the dials (per Panel; folded at first)
        bool dialsFolded_ = false;                       // the dials folded into the bar
        bool dialsBar_ = false;                          // the bar shown
        int dialsBarItem_ = -1;
        void persistFileViews();                         // the session keeps the netlists' views (a user's gesture)
        int  textItem() const;                           // the focused item when it takes every key, else -1

        // The chat.
        void refreshChat(double nowSec);
        void reshowChat();                               // the conversation laid out again (a row opened, ...)
        void sendChat();                                 // the message box's text, to Claude
        void suggest(const std::string& text);           // a suggestion into the message box
        void openModels();
        void openThinking();
        void newChat();
        // The chats (ChatPort::chatList): the pill's label (the chat shown's name), its menu, Delete's question, and the
        // rename, in the key dialog's box (renaming_).
        std::string chatsLabel() const;
        std::string chatTitle() const;                   // the card's title: Claude, or another provider's name
        void openChats();
        void askDeleteChat();
        void openRenameDialog();
        void renameFromDialog();
        void layoutChat();                               // the chat card's insides
        void placeChatBar();                             // the message box's bar: the pills, send
        std::string modelLabel() const;
        std::string thinkingLabel() const;
        std::vector<std::string> chatEfforts() const;    // the chosen model's thinking levels
        bool chatTakesFast() const;                      // the chosen model takes fast mode
        bool chatTakesWeb() const;                       // ... and web search
        bool chatTakesThinking() const;                  // ... and thinking levels (Anthropic's, another's own)
        std::string barLabels() const;                   // what the bar's layout depends on
        InputItem& inputItem();
        const InputItem& inputItem() const;
        funkgui::Point centreOf(int item) const;

        // The key dialog.
        struct KeyDialog
        {
            funkgui::Rect box, field, save, cancel, remove;
            funkgui::Rect provider, base, model;         // the pill; another provider's fields (empty: none)
        };
        KeyDialog keyDialog() const;                     // its places, for the panel's size
        void openKeyDialog();
        void closeKeyDialog();
        void saveKeyFromDialog();
        void removeKeyFromDialog();
        bool keyDialogKey(const funkgui::KeyEvent&);
        void keyDialogPointerDown(funkgui::Point);
        void drawKeyDialog(funkgui::Canvas&, const funkgui::Theme&) const;
        std::string maskedKey() const;
        // The provider in the dialog (ChatPort::setProvider): its preset, whether it is Anthropic's and the chat's now
        // (the dialog then works as it always did), the provider as its fields say it, whether a key is stored for it,
        // the Provider pill's menu, the field the keys go to (Tab: the next shown).
        const ChatPreset& dialogPresetRef() const;
        bool dialogAnthropic() const;
        bool dialogAsBefore() const;
        ChatProvider dialogProvider() const;
        bool dialogHasKey() const;
        void openProviders();
        funkgui::text::LineEdit& dialogEdit();
        int dialogFields() const;                        // the fields shown, as a mask of 1 << field

        EditorPort& port_;
        class HostProxy;                                 // the host as host_ and the items reach it (Panel.cpp, above:
        std::unique_ptr<HostProxy> proxy_;               // "Unchanged frames"); before the items, which it outlives
        std::vector<std::unique_ptr<Item>> items_;
        std::vector<int> itemOfSlot_;                    // dial slot -> item index
        std::vector<int> itemOfDisplay_;                 // display -> item index
        Meters meters_{};                                // the Engine's, as the last frame read them
        std::unique_ptr<DisplayFeed> feed_;              // the scopes' samples, read each frame they show, and spectra
        ShownState shown_;                               // a picture's state (showState), else empty
        bool showing_ = false, shownPlaying_ = false;
        std::size_t shownAt_ = 0;                        // the frame shown
        int editorItem_ = -1, problemsItem_ = -1, runItem_ = -1, examplesItem_ = -1, libraryItem_ = -1, randomItem_ = -1,
            projectsItem_ = -1;
        int presetPrevItem_ = -1, presetItem_ = -1, presetNextItem_ = -1;   // the top bar's presets, right of the pill
        PresetCycle cycle_;                              // the project open's presets, as the last refresh read them
        bool presetsShown() const noexcept { return !cycle_.project.empty(); }
        int themeItem_ = -1, zoomItem_ = -1;
        int panelViewItem_ = -1;                         // the Code / Panel switch's Panel half (viewItem_ the Code one)
        float actionsX_ = 0.0f;                          // the program's card's actions start here (at its right)
        funkgui::Rect programCard_;                      // the program's card (the tabs, the editor or the panel, the dials)
        std::unique_ptr<funkgui::CellModel> zoomModel_;  // the zoom's steps (zoom_ points at it)
        void openZoom();                                 // the zoom's menu
        std::string statusShown() const;                 // the status as the card's header shows it (sentence case)
        float statusX() const;                           // where it starts (it ends before the actions, actionsX_)
        int newChatItem_ = -1, keyItem_ = -1, chatViewItem_ = -1, inputItem_ = -1;
        int chatsItem_ = -1;                             // the chats' pill (the chat shown, its menu)
        std::string chatsLabel_;                         // its label when layoutChat placed it
        int chatToggleItem_ = -1;                        // fold or open the chat
        bool chatOpen_ = true;
        // The explorer (Explorer.h): its item, its fold button, its card; the projects and the open one's presets it
        // lists (read again every few seconds while it is open: listsRevision_ bumps when what was read differs); what
        // its rows were made from, as a hash taken each frame (syncExplorer rebuilds them when it changes); how often
        // they were made.
        int explorerItem_ = -1, explorerToggleItem_ = -1;
        bool explorerOpen_ = true;
        // The columns' widths (Columns.h): the edges' items; the widths asked (0: the share); the port's as this Panel
        // last saw them (another Panel's drag, a session loaded: refresh() takes them); what layout() fit; a step's
        // width on its way to this Mac's default (-1: none), and when the last step was.
        int explorerEdgeItem_ = -1, chatEdgeItem_ = -1;
        float explorerWant_ = 0.0f, chatWant_ = 0.0f;
        int seenExplorerWidth_ = 0, seenChatWidth_ = 0;
        columns::Fit fit_;
        int stepDefault_[2] = { -1, -1 };                // [0] the explorer's, [1] the chat's
        double stepAt_ = 0.0;
        bool isEdge(int item) const noexcept { return item >= 0 && (item == explorerEdgeItem_ || item == chatEdgeItem_); }
        float wantedWidth(int session, bool chat) const; // the session's width, else this Mac's default, else 0
        void setColumnWidth(bool chat, float px, columns::Keep keep);
        void syncColumnWidths();                         // refresh(): the port's widths, when another set them
        void keepStepDefaults(double nowSec, bool now);  // a step's width to this Mac's default, once the steps pause
                                                         // (now: at once)
        bool explorerShown_ = true;                      // open, in the CODE view, and the window has room for it
        void toggleExplorer();
        funkgui::Rect explorerCard_;
        float leftX_ = 0.0f;                             // the program's card's left edge (after the explorer)
        std::vector<ProjectEntry> projectList_;
        std::vector<ProjectEntry> factoryList_;          // the factory projects (EditorPort::factoryProjects), read again
        ProjectsView projectsView_;                      // PROJECTS' stars and folded groups, as the poll last read them
        bool factoryRead_ = false;
        uint64_t factorySeen_ = 0;                       // the factory set's generation factoryList_ was read at
        std::vector<PresetEntry> presetList_;            // the project open's
        std::string presetsOf_;                          // the project presetList_ is of
        double projectsAt_ = -100.0;
        uint64_t listsRevision_ = 0;
        uint64_t explorerKey_ = 0;
        bool explorerStale_ = true;                      // made again at the next frame, whatever the hash
        int explorerBuilds_ = 0;
        uint64_t projectsReadSeen_ = 0;                  // EditorPort::projectsRead at the last poll
        bool explorerAway_ = true;                       // hidden at the last frame (its poll then waits: syncExplorer)
        void syncExplorer();
        void layoutExplorer();
        void openFromExplorer(ExplorerItem::Section section, const ExplorerItem::Row& row);
        // The folder (EditorPort::folder, plugin/FolderSync.h): what this Panel last read of it (read again when its
        // revision moves: refresh()), the rows of its explorer section, what a row does, a conflict's choice (a menu at
        // the pointer: which side to keep), and the notices a save that landed or a new conflict give.
        FolderState folderState_;
        uint64_t folderSeen_ = 0;
        bool folderKnown_ = false;                       // folderState_ read once (the notices start from it)
        void syncFolder();
        // MIDI learn (EditorPort::midi; plugin/MidiLearn.h): each dial's mark and footer line, read again when its
        // revision moves (refresh()); a dial's menu (right-click, ctrl-click, a screen reader's menu action: Learn MIDI
        // CC, Forget, Forget all, the host's own menu), or in an effect, which learns no MIDI, the host's menu as before.
        uint64_t midiSeen_ = 0;
        void syncMidi();
        void dialMenu(int slot, funkgui::Point at);
        void folderRow(const ExplorerItem::Row& row);
        void chooseConflict(const std::string& file);
        // The IR view: this Panel is an IR viewer while its IR tab is open (EditorPort::addIrViewer), and the view shown
        // has its text made from what there is (refresh(); a view shown again catches up then).
        bool irViewing_ = false;
        std::string irSeen_;                             // what syncIr last looked at
        void syncIr();
        void openProjectNamed(const std::string& name);  // its newest version, asking first over unsaved edits
        void applyPresetNamed(const std::string& name);  // a preset of the project open
        void afterProjects(bool wasOpen);                // the card's notice to the status, the focus back
        void afterExport(bool wasOpen);                  // its notice; an export asked for: the save chooser, the file
        void afterAb(bool wasOpen);                      // A/B's notice to the status, the focus back once it closed
        void importFiles(const std::vector<std::string>& paths);
        bool dialogOpen() const noexcept
        {
            return keyOpen_ || projects_.isOpen() || export_.isOpen() || ab_.isOpen() || settings_.isOpen() || name_.isOpen()
                || picker_.isOpen();
        }
        std::string projectLabel() const;                // the top bar's pill: "Name v3 - Preset", "edited", or Projects
        bool unread_ = false;                            // folded, and the conversation grew since
        // Pictures: the ids this Panel has in FunkGui's registry (images::set, counted across Panels: Picture.cpp
        // makes one too), and a drag of pictures over the editor (over the chat's card).
        std::vector<uint32_t> pictures_;
        bool dropActive_ = false, dropHover_ = false;
        int attachItem_ = -1;
        void syncPictures();                             // the registry follows chat_ (the attached and the sent)
        void releasePictures();
        void openPictures();                             // the attach button's file chooser
        void openSamples();                              // the ASSETS header's: audio files as samples
        void addSamples(const std::vector<std::string>& paths);
        std::size_t entriesAtFold_ = 0;
        int modelItem_ = -1, thinkingItem_ = -1, fastItem_ = -1, webItem_ = -1, sendItem_ = -1;
        funkgui::CellModel* zoom_ = nullptr;             // the zoom's steps (zoomModel_)
        float width_ = 0.0f, height_ = 0.0f;
        uint32_t layoutRevision_ = 0;                    // bumps with each layout() and scroll (what moved)
        bool filesPlaced_ = false;                       // the editors given a place once (layout(): kept while hidden)
        double inputAt_ = -1.0e9;                        // the last input's time (the host's clock: kInputHold)
        void noteInput(bool nudge = false);
        bool inputHeld() const;
        int layouts_ = 0;                                // layout()'s calls (probes)
        std::string title_, subtitle_, status_, meter_;
        std::string meterAlerts_, meterLoad_;            // meter_'s two parts, drawn either side of the snowflake
        bool frozenShown_ = false;                       // the snowflake: the frozen copy plays
        int frozenItem_ = -1;
        void placeMeter();                               // the snowflake's place, left of the load's text
        std::string projectText_;                        // the project open, its version, the preset applied, in words
        ProjectBrowser projects_{ port_ };
        PresetPicker picker_{ port_ };
        int focusBeforePicker_ = -2;                     // the focus to give back when the list closes (-2: none)
        void afterPicker(bool wasOpen);                  // its notice to the status, the focus back once it closed
        ExportCard export_{ port_ };
        AbCard ab_{ port_ };
        uint64_t abSeen_ = 0;                            // the A/B's revision this Panel last looked at (attach)
        uint64_t claudeSeen_ = 0;                        // EditorPort::claudeNotice's count this Panel last said
        uint64_t saveSeen_ = 0;                          // EditorPort::saving().done this Panel last said
        bool saving_ = false;                            // a save of the editor's in flight (its notice SAVING)
        void savingNotice();                             // SAVING, or what came of it when it ended at once
        void syncSave();                                 // what a save came to, once it has ended
        int focusBeforeProjects_ = -1;
        // SETTINGS: the card; the app's requests seen (EditorPort::settingsAsked); the muted notice and its button.
        SettingsCard settings_{ port_ };
        uint64_t seenSettingsAsk_ = 0;
        uint32_t seenSettingsRev_ = 0;
        uint32_t seenPrefsRev_ = ~0u;                    // UiPreferences' revision the text size was read at
        int focusBeforeSettings_ = -1;
        int mutedItem_ = -1;
        bool mutedShown_ = false;
        void syncSettings();                             // refresh(): the devices, the app's request, the notice
        void afterSettings(bool wasOpen);                // the card closed: the focus back
        void placeMutedNotice();                         // its button, left of the zoom in the status bar
        int markItem_ = -1;                              // the recorder's Mark, left of the muted notice and the zoom
        RecordingState recording_{};
        std::string recordShownText_;                    // recordText() as last drawn: a frame when it changes
        double recordingAt_ = -1.0;                      // the last look (ten a second at most)
        void syncRecording(double nowSec);
        void placeRecordNotice();
        std::string notice_;                             // RANDOM's or PROJECTS' word in the status, until noticeUntil_
        double noticeUntil_ = -1.0;
        uint32_t randomCount_ = 0;

        // The places layout() found, which draw() uses.
        float leftW_ = 0.0f;                             // the left column's width (the editor's)
        Controls controls_{};                            // the dials of the program that plays, as refresh() saw them
        std::vector<lang::CircuitBinding> circuitDials_; // ... and its circuit fields (CompileStatus::circuitDials)
        std::vector<circuits::Probed> circuitProbes_;    // ... its probe table (CompileStatus::probes)
        uint64_t probeProgram_ = 0;                      // ... its id (CompileStatus::program)
        int probeOwner_ = 0;                             // this Panel's probes' owner (EditorPort::addProbeOwner; 0: none yet)
        bool ownsProbes_ = true;                         // false: a picture's Panel, which never arms one
        PanelSpec panelSpec_;                            // its #pragma panel lines
        uint64_t panelProgram_ = 0;                      // the compile they came from (EditorPort::program)
        TapsShown tapsShown_;                            // the fields its displays read, of its program and tap table
        DialsLayout dials_;                              // their places
        // A .gui file's interface (when panelSpec_.gui is not empty): its items (a control a slot, pools of tabs, pads,
        // envelopes, keyboards and shaders, in items_), its places, its tabs' pages and its scroll (GuiView.h).
        std::unique_ptr<GuiView> guiView_;
        bool panelView_ = false;
        int viewItem_ = -1;                              // the Code / Panel switch's Code half
        float chatX_ = 0.0f, chatW_ = 0.0f;
        funkgui::Rect card_;                             // the chat's card
        float headerY_ = 0.0f;                           // its header row
        float composerH_ = 0.0f;                         // the message box's height (it follows its text)
        float usageY_ = 0.0f;                            // the line under the message box
        std::string barLabels_;                          // the pills' labels when they were placed

        // The chat, as refresh() last saw it.
        uint64_t seenChat_ = 0;
        double chatAt_ = -1.0;                           // when it was last read (the host's clock)
        double busySince_ = -1.0;                        // when the turn under way began
        ChatState chat_;
        std::string activity_;                           // the line under the message box
        // Claude Desktop (EditorPort::desktop): what this Panel last read of it (syncDesktop, each refresh: a copy); the
        // chat header's subtitle (what a call does here now, else the name Claude Desktop calls it by while it is the
        // target) and its badge, while Claude Desktop is connected and its calls go to another or to none (a click makes
        // this one the target; folded, the badge is a mark on the rail, under its button, which does the same); the
        // explorer's CLAUDE DESKTOP rows (desktopRow: what a row does); the name card.
        DesktopState desktopState_;
        std::string desktop_;                            // the subtitle ("" for none)
        std::string desktopBadge_;                       // the badge's label ("" for none)
        std::string desktopAgo_;                         // when the last call was, in words ("now", "just now", "4 min ago")
        std::string desktopSeen_;                        // what the explorer's rows show of it, as text
        uint64_t desktopRevision_ = 0;                   // bumps when that changes
        int desktopItem_ = -1;                           // the badge
        int desktopMarkItem_ = -1;                       // ... its mark on the folded chat's rail
        void syncDesktop();
        void placeDesktopBadge();                        // and the chats' pill, which ends at the badge while it shows
        float chatsPillX() const;                        // the chats' pill's left: after the title and its light
        static constexpr float kChatsPillLeast = 48.0f;  // the room the pill keeps before the badge takes any
        void desktopRow(const ExplorerItem::Row& row);
        NameCard name_{ port_ };
        int focusBeforeName_ = -1;
        void afterName(bool wasOpen);                    // its notice to the status, the focus back once it closed
        uint32_t chatA11y_ = 0;                          // bumps when the conversation's items change
        std::string pendingSend_;                        // a message waiting for a key

        // The key dialog.
        bool keyOpen_ = false;
        bool renaming_ = false;                          // the dialog renames the chat shown (keyOpen_ too)
        funkgui::text::LineEdit keyEdit_;
        std::string keyWhy_;                             // why the key typed was refused
        int focusBeforeKey_ = -1;
        int dialogPreset_ = 0;                           // the provider shown (kChatPresets), the server's address and
        funkgui::text::LineEdit baseEdit_, modelEdit_;   // the model typed, the field typed in, what is kept for it
        int dialogField_ = 0;
        ChatProviderInfo dialogInfo_;

        funkgui::HostServices* host_ = nullptr;
        std::optional<funkgui::GestureController> gestures_;
        funkgui::Point pointer_{};
        bool hasPointer_ = false;
        bool ring_ = false;                              // the focus ring is shown (keyboard use)
        // The menu open (menuOpenAt): its anchor, and the request it belongs to (a later one replaces it).
        bool menuOpen_ = false;
        funkgui::Rect menuAnchor_{};
        uint32_t menuSerial_ = 0;
        funkgui::Rect triggerRect(int item) const;       // a ButtonItem trigger's bounds: its menu's anchor
        bool dirty_ = true;                              // a change since the last tick (input, a refresh, a layout)
        // Unchanged frames (above): a change not drawn yet (dirty_ as each tick found it, kept until draw()); an item
        // unsettled at the last tick, and at the tick before (the frame a motion ends in draws too); the mix of what
        // draw() reads (watchKey) at the last tick and at the last draw; when that was.
        bool undrawn_ = true;
        bool moving_ = false, settling_ = false;
        uint64_t watched_ = 0, drawnKey_ = 0;
        double drawnAt_ = -1.0e9;
        uint64_t watchKey() const;
        // The meters' reader (above): held, the last tick's time (the host's clock; -1 before any tick or idle call),
        // and the idle calls since.
        bool reading_ = false;
        double tickedAt_ = -1.0;
        int idlesSinceTick_ = 0;
        int  pressed_ = -1;                              // the item holding the pointer between down and up
        int  focus_ = -1;
        int  touched_ = -1;                              // the last item used, for the spec line

        // What refresh() last saw.
        uint64_t seenStatus_ = 0, seenDraft_ = 0;
        uint64_t syncedFiles_ = 0;                       // the files' revision last given to the port
        Draft given_;                                    // the port's draft as the editor last gave or merged it
        bool pushedFiles_ = false;                       // ... by a schematic gesture since the last refresh
        bool edited_ = false;                            // the files differ from the program's that plays
        Sources running_;                                // the files of the program that plays, as statuses say them
        double meterAt_ = -1.0;                          // when meter_ was last made (the host's clock)
        bool compiling_ = false;
        bool lastOk_ = true;
        double lastMs_ = 0.0;
        bool lastPanelOnly_ = false;                     // the last RUN was a panel's edit (Compiler.h): no compile
        std::string lastStop_;                       // a program that stopped (Compiler.h): the status line's words,
        std::string lastStopWhy_;                    // and why (a loop past its budget, an index outside its array)
        uint32_t controlsRevision_ = 0;                  // bumps when the dials' program changes
        std::vector<Problem> problems_;
    };
}
