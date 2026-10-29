// Source/editor/Panel.h: the plugin's interface, a funkgui::Panel: an FMinus program's editor and its dials, and the
// chat with Claude beside them. It is GPU-free: funkgui::EditorHost runs it live in the plugin
// (Source/editor/gpu/CreateEditorGpu.cpp) and funkgui::HeadlessHost runs it in the probes (gui.dump, gui.chat), drawing
// the same frames. A .gui file's shaders it hands to FunkGui's registry (funkgui/gpu/ShaderLibrary.h, which makes no
// GPU call until bgfx draws them) and records as Canvas::shader draws: the GPU draws them, SoftRaster a placeholder.
//
// It is resizable, in both directions, from kMinWidth x kMinHeight (logical px) up: the chat takes about a third of the
// width at the right (kChatMinW to kChatMaxW), the explorer about a sixth at the left, the editor and the chat's
// transcript take the height that is left.
//
//   explorer  at the left, a card like the chat's (Explorer.h): the program's files (a click shows one, a double click
//             renames it, its cross closes it, the header's + makes one), its LLVM IR (a click opens the IR tab on a
//             view, IrView.h), the library (a click adds a file as a tab,
//             #included, or shows it), the examples (a click loads one, undoably) and the presets on this Mac (a click
//             loads one, asking first when the editor has changes no preset holds; the header's + opens PRESETS); its
//             sections fold, and the whole card folds away (its chevron; the button at the left of the program's card's
//             header opens it; EditorPort::explorerOpen); in the PANEL view, or a window too narrow for it, it is away
//   header    the product name, its kind and version, and the THEME cells (GRAPHITE, PAPER)
//   toolbar   RUN (Cmd-Return in the editor too), EXAMPLES (a menu: the example replaces the text, undoably, and
//             runs), LIBRARY (a menu of ready-made building blocks, Library.h: one becomes a tab and the program
//             #includes it), the status (running, edited, compiling, the errors' count, the compile time) and, at
//             the right, the audio thread's load, a snowflake left of it while the frozen copy plays (the dials compiled
//             in, Patch.h) and the output guard's alerts further left (NaN replaced, clipped)
//   files     a tab for each of the program's files (Sources.h; at most kMaxFiles): the program's first, then the
//             files it can #include by name. A click shows a file, a double-click edits its name in place (Return keeps
//             it, Escape cancels), the + adds a file (its name being edited), a tab's x closes its file (asking first
//             when it has text). A tab whose file the last run's errors name has a mark before its name. At the bar's
//             right end, apart, the IR tab while it is open (IrView.h: the program's LLVM IR, read only, none of its
//             files), opened from the explorer's LLVM IR rows
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
//             wider than its area
//   view      the toolbar's CODE and PANEL cells: PANEL gives the interface the whole left column (the tabs, the editor
//             and the problems go), CODE brings the editor back; the session keeps the choice (EditorPort::panelView)
//   chat      at the right, a rounded card set apart from the square panel (ChatStyle.h): its header (the title, a new
//             chat (asking first), the API key), the conversation (ChatView.h: bubbles, light markdown, thinking rows,
//             cards of what Claude did, code blocks with Copy, an empty chat's suggestions), the message box that grows
//             with its text (Return sends it, Shift-Return breaks the line) with the model and thinking pills (menus)
//             and the round send-or-stop button in its bar (ChatItems.h), and the line under it (what Claude is doing,
//             else the last reply's tokens)
//   key       a dialog over the panel for the Anthropic API key: a masked field (Cmd-V pastes), Save, Cancel and, when a
//             key is stored, Remove key. It opens from the header's key, the empty chat's button, and when a message
//             is sent before any key is stored (the message goes once a key is saved)
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
// Message thread only. The ports and the EditorPort belong to the processor and outlive the Panel.
#pragma once

#include "editor/Explorer.h"
#include "editor/ExportCard.h"
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
#include "editor/GuiLayout.h"
#include "fminus/PanelSpec.h"

#include "plugin/ChatPort.h"

#include <funkgui/text/LineEdit.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
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
    class DisplayItem;                                   // a program's display (DisplayItem.h)
    class GuiControlItem;                                // a .gui file's control (GuiItems.h)
    class GuiView;                                       // a .gui file's interface: its items (GuiView.h)

    // The size (logical px): the default, the least.
    inline constexpr int kDefaultWidth = 1360, kDefaultHeight = 760;
    inline constexpr int kMinWidth = 1000, kMinHeight = 600;

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

    class Panel final : public funkgui::Panel
    {
    public:
        Panel(std::span<funkgui::ParamPort* const> ports, EditorPort& port);
        ~Panel() override;

        Panel(const Panel&) = delete;
        Panel& operator=(const Panel&) = delete;

        // For probes.
        funkgui::TextEditor& editor() noexcept;         // the shown file's
        CodeAssist& codeAssist() noexcept;               // its completion, hover, find bar, problems (CodeAssist.h)
        int fileCount() const;
        std::string fileName(int i) const;
        int activeFile() const;
        bool renamingFile() const;
        funkgui::Point centreOfTab(int i) const;
        funkgui::Point centreOfNewTab() const;
        funkgui::Point centreOfCloseTab(int i) const;
        std::optional<funkgui::Point> centreOfDial(int slot) const;    // nullopt for an unused slot
        int dialRows() const noexcept { return dials_.rows; }             // the rows of dials, or of sections
        const DialsLayout& dialsLayout() const noexcept { return dials_; }
        const DisplayItem& displayItem(int d) const;                      // display d of the program's (shown or not)
        // A .gui file's interface: whether the program has one, the PANEL view, its layout, slot's control, a page
        // chosen, a scroll (the wheel's, by px).
        bool hasGui() const noexcept { return !panelSpec_.gui.empty(); }
        bool panelView() const noexcept { return panelView_; }
        void setPanelView(bool on);
        const GuiLayout& guiLayout() const noexcept;
        const funkgui::Rect& programCard() const noexcept { return programCard_; }   // the program's card
        const PanelSpec& panelSpec() const noexcept { return panelSpec_; }      // the program's that the panel shows
        const GuiControlItem& guiControl(int slot) const;
        const Item& guiItem(int kind, int k) const;   // kind 0: a pad, 1: an envelope, 2: tabs, 3: a keyboard, 4: a shader
        void chooseGuiPage(int node, int page);
        void scrollGui(float dx, float dy);
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
        void chooseImport();
        // "PROJECT  NAME  V3  EDITED  PRESET  BRIGHT  EDITED" (what the top bar's pill says, in words), "" for none.
        const std::string& projectText() const noexcept { return projectText_; }
        // Cmd-S: a new version of the project open (its name in the status), or, with none open, PROJECTS.
        void saveVersion();
        // RANDOM (fminus/Random.h): the program for `seed` (its library files as tabs, those the editor lacks; its text
        // in place of the program's, an edit Cmd-Z takes back), run. When the tabs left cannot hold its files, the next
        // seeds are tried (up to 64); false, with a notice in the status, when none fits.
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
        funkgui::Rect explorerCard() const noexcept { return explorerCard_; }
        // EXAMPLES' choice (Examples.h): its text in place of the program's (one edit Cmd-Z takes back), run.
        void loadExample(int i);
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
        // The chat's column: open, or folded into a rail at the right (the editor and the panel take the room). The
        // header's chevron folds it, the rail's button opens it; the session keeps it (EditorPort::chatOpen).
        bool chatOpen() const noexcept { return chatOpen_; }
        void setChatOpen(bool open);
        funkgui::Point centreOfChatToggle() const;       // the header's chevron, or the rail's button
        bool chatUnread() const noexcept { return unread_; }   // folded, Claude has written since
        ChatView& chatView();                            // the conversation (its own probe hooks)
        const ChatView& chatView() const;
        // The key dialog.
        bool keyDialogOpen() const noexcept { return keyOpen_; }
        const std::string& keyDialogWhy() const noexcept { return keyWhy_; }
        funkgui::Point centreOfKeyField() const;
        funkgui::Point centreOfKeySave() const;
        funkgui::Point centreOfKeyCancel() const;
        std::optional<funkgui::Point> centreOfKeyRemove() const;      // nullopt while no key is stored
        // SETTINGS (the Standalone's: EditorPort::devices; nothing in a host): the card, opened (false with no devices or
        // while another card is open); the notice while an effect's input is muted, and its Settings... button.
        bool openSettings();
        SettingsCard& settingsCard() noexcept { return settings_; }
        bool mutedNoticeShown() const noexcept { return mutedShown_; }
        funkgui::Point centreOfMutedSettings() const;

        // funkgui::Panel
        void  attach(funkgui::HostServices&) override;
        int   width() const override;
        int   height() const override;
        bool  resizable() const override { return true; }
        int   minWidth() const override { return kMinWidth; }
        int   minHeight() const override { return kMinHeight; }
        void  resize(int w, int h) override;
        void  tick(float dt) override;
        void  idle(double nowSec) override;
        void  draw(funkgui::Canvas&, const funkgui::Theme&) override;
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

        int  itemAt(funkgui::Point) const;               // the item under p, -1 for none
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
        int  textItem() const;                           // the focused item when it takes every key, else -1

        // The chat.
        void refreshChat(double nowSec);
        void reshowChat();                               // the conversation laid out again (a row opened, ...)
        void sendChat();                                 // the message box's text, to Claude
        void suggest(const std::string& text);           // a suggestion into the message box
        void openModels();
        void openThinking();
        void newChat();
        void layoutChat();                               // the chat card's insides
        void placeChatBar();                             // the message box's bar: the pills, send
        std::string modelLabel() const;
        std::string thinkingLabel() const;
        std::vector<std::string> chatEfforts() const;    // the chosen model's thinking levels
        bool chatTakesFast() const;                      // the chosen model takes fast mode
        bool chatTakesWeb() const;                       // ... and web search
        std::string barLabels() const;                   // what the bar's layout depends on
        InputItem& inputItem();
        const InputItem& inputItem() const;
        funkgui::Point centreOf(int item) const;

        // The key dialog.
        struct KeyDialog
        {
            funkgui::Rect box, field, save, cancel, remove;
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

        EditorPort& port_;
        std::vector<std::unique_ptr<Item>> items_;
        std::vector<int> itemOfSlot_;                    // dial slot -> item index
        std::vector<int> itemOfDisplay_;                 // display -> item index
        Meters meters_{};                                // the Engine's, as the last frame read them
        std::vector<float> scopeIn_, scopeOut_;          // the scopes' samples, read each frame they show
        int editorItem_ = -1, problemsItem_ = -1, runItem_ = -1, examplesItem_ = -1, libraryItem_ = -1, randomItem_ = -1,
            projectsItem_ = -1;
        int themeItem_ = -1, zoomItem_ = -1;
        int panelViewItem_ = -1;                         // the Code / Panel switch's Panel half (viewItem_ the Code one)
        float actionsX_ = 0.0f;                          // the program's card's actions start here (at its right)
        funkgui::Rect programCard_;                      // the program's card (the tabs, the editor or the panel, the dials)
        std::unique_ptr<funkgui::CellModel> zoomModel_;  // the zoom's steps (zoom_ points at it)
        void openZoom();                                 // the zoom's menu
        std::string statusShown() const;                 // the status as the card's header shows it (sentence case)
        int newChatItem_ = -1, keyItem_ = -1, chatViewItem_ = -1, inputItem_ = -1;
        int chatToggleItem_ = -1;                        // fold or open the chat
        bool chatOpen_ = true;
        // The explorer (Explorer.h): its item, its fold button, its card; the projects and the open one's presets it
        // lists (read again every few seconds while it is open); what its rows were made from (syncExplorer rebuilds
        // them when it changes).
        int explorerItem_ = -1, explorerToggleItem_ = -1;
        bool explorerOpen_ = true;
        bool explorerShown_ = true;                      // open, in the CODE view, and the window has room for it
        void toggleExplorer();
        funkgui::Rect explorerCard_;
        float leftX_ = 0.0f;                             // the program's card's left edge (after the explorer)
        std::vector<ProjectEntry> projectList_;
        std::vector<PresetEntry> presetList_;            // the project open's
        std::string presetsOf_;                          // the project presetList_ is of
        double projectsAt_ = -100.0;
        std::string explorerKey_;
        void syncExplorer();
        void layoutExplorer();
        void openFromExplorer(ExplorerItem::Section section, const ExplorerItem::Row& row);
        // The IR view: this Panel is an IR viewer while its IR tab is open (EditorPort::addIrViewer), and the view shown
        // has its text made from what there is (refresh(); a view shown again catches up then).
        bool irViewing_ = false;
        void syncIr();
        void openProjectNamed(const std::string& name);  // its newest version, asking first over unsaved edits
        void applyPresetNamed(const std::string& name);  // a preset of the project open
        void afterProjects(bool wasOpen);                // the card's notice to the status, the focus back
        void afterExport(bool wasOpen);                  // its notice; an export asked for: the save chooser, the file
        void importFiles(const std::vector<std::string>& paths);
        bool dialogOpen() const noexcept { return keyOpen_ || projects_.isOpen() || export_.isOpen() || settings_.isOpen(); }
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
        std::size_t entriesAtFold_ = 0;
        int modelItem_ = -1, thinkingItem_ = -1, fastItem_ = -1, webItem_ = -1, sendItem_ = -1;
        funkgui::CellModel* zoom_ = nullptr;             // the zoom's steps (zoomModel_)
        float width_ = 0.0f, height_ = 0.0f;
        uint32_t layoutRevision_ = 0;                    // bumps with each layout()
        std::string title_, subtitle_, status_, meter_;
        std::string meterAlerts_, meterLoad_;            // meter_'s two parts, drawn either side of the snowflake
        bool frozenShown_ = false;                       // the snowflake: the frozen copy plays
        int frozenItem_ = -1;
        void placeMeter();                               // the snowflake's place, left of the load's text
        std::string projectText_;                        // the project open, its version, the preset applied, in words
        ProjectBrowser projects_{ port_ };
        ExportCard export_{ port_ };
        int focusBeforeProjects_ = -1;
        // SETTINGS: the card; the app's requests seen (EditorPort::settingsAsked); the muted notice and its button.
        SettingsCard settings_{ port_ };
        uint64_t seenSettingsAsk_ = 0;
        uint32_t seenSettingsRev_ = 0;
        int focusBeforeSettings_ = -1;
        int mutedItem_ = -1;
        bool mutedShown_ = false;
        void syncSettings();                             // refresh(): the devices, the app's request, the notice
        void afterSettings(bool wasOpen);                // the card closed: the focus back
        void placeMutedNotice();                         // its button, left of the zoom in the status bar
        std::string notice_;                             // RANDOM's or PROJECTS' word in the status, until noticeUntil_
        double noticeUntil_ = -1.0;
        uint32_t randomCount_ = 0;

        // The places layout() found, which draw() uses.
        float leftW_ = 0.0f;                             // the left column's width (the editor's)
        Controls controls_{};                            // the dials of the program that plays, as refresh() saw them
        PanelSpec panelSpec_;                            // its #pragma panel lines
        uint64_t panelProgram_ = 0;                      // the compile they came from (EditorPort::program)
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
        std::string desktop_;                            // Claude Desktop's name for this instance, while connected
        uint32_t chatA11y_ = 0;                          // bumps when the conversation's items change
        std::string pendingSend_;                        // a message waiting for a key

        // The key dialog.
        bool keyOpen_ = false;
        funkgui::text::LineEdit keyEdit_;
        std::string keyWhy_;                             // why the key typed was refused
        int focusBeforeKey_ = -1;

        funkgui::HostServices* host_ = nullptr;
        std::optional<funkgui::GestureController> gestures_;
        funkgui::Point pointer_{};
        bool hasPointer_ = false;
        bool ring_ = false;                              // the focus ring is shown (keyboard use)
        bool dirty_ = true;                              // an input event since the last tick
        int  pressed_ = -1;                              // the item holding the pointer between down and up
        int  focus_ = -1;
        int  touched_ = -1;                              // the last item used, for the spec line

        // What refresh() last saw.
        uint64_t seenStatus_ = 0, seenDraft_ = 0;
        uint64_t syncedFiles_ = 0;                       // the files' revision last given to the port
        bool edited_ = false;                            // the files differ from the program's that plays
        Sources running_;                                // the files of the program that plays, as statuses say them
        double meterAt_ = -1.0;                          // when meter_ was last made (the host's clock)
        bool compiling_ = false;
        bool lastOk_ = true;
        double lastMs_ = 0.0;
        uint32_t controlsRevision_ = 0;                  // bumps when the dials' program changes
        std::vector<Problem> problems_;
    };
}
