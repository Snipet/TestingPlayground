// Source/editor/Panel.h: the plugin's interface, a funkgui::Panel: an FMinus program's editor and its dials, and the
// chat with Claude beside them. It is GPU-free: funkgui::EditorHost runs it live in the plugin
// (Source/editor/gpu/CreateEditorGpu.cpp) and funkgui::HeadlessHost runs it in the probes (gui.dump, gui.chat), drawing
// the same frames. A .gui file's shaders it hands to FunkGui's registry (funkgui/gpu/ShaderLibrary.h, which makes no
// GPU call until bgfx draws them) and records as Canvas::shader draws: the GPU draws them, SoftRaster a placeholder.
//
// It is resizable, in both directions, from kMinWidth x kMinHeight (logical px) up: the chat takes about a third of the
// width at the right (kChatMinW to kChatMaxW), the editor and the chat's transcript take the height that is left.
//
//   header    the product name, its kind and version, and the THEME cells (GRAPHITE, PAPER)
//   toolbar   RUN (Cmd-Return in the editor too), EXAMPLES (a menu: the example replaces the text, undoably, and
//             runs), LIBRARY (a menu of ready-made building blocks, Library.h: one becomes a tab and the program
//             #includes it), the status (running, edited, compiling, the errors' count, the compile time) and, at
//             the right, the audio thread's load and the output guard's alerts (NaN replaced, clipped)
//   files     a tab for each of the program's files (Sources.h; at most kMaxFiles): the program's first, then the
//             files it can #include by name. A click shows a file, a double-click edits its name in place (Return keeps
//             it, Escape cancels), the + adds a file (its name being edited), a tab's x closes its file (asking first
//             when it has text). A tab whose file the last run's errors name has a mark before its name
//   editor    a funkgui::TextEditor for each file, the shown file's drawn (the processor keeps the files and the tab:
//             EditorPort::draft), FMinus's words highlighted; a mark in the gutter on each line of the shown file the
//             last run's errors name
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
//             chat (asking first), access: the account or the API key), the conversation (ChatView.h: bubbles, light markdown, thinking rows,
//             cards of what Claude did, code blocks with Copy, an empty chat's suggestions), the message box that grows
//             with its text (Return sends it, Shift-Return breaks the line) with the model and thinking pills (menus)
//             and the round send-or-stop button in its bar (ChatItems.h), and the line under it (what Claude is doing,
//             else the last reply's tokens, and through the account the credit left)
//   access    a dialog over the panel with two tabs, the one in use marked with a dot. ACCOUNT: signed out, the
//             username and a masked password (Tab moves between them, Return signs in, Cmd-V pastes) and Sign in,
//             which waits for the server and then closes, or says why not; signed in, who and the credit left, and
//             Sign out. API KEY: a masked field for an Anthropic API key (Cmd-V pastes), Save and, when a key is stored,
//             Remove key. Each tab whose access is stored but not in use offers to use it (Use account, Use this key).
//             It opens from the header's key, the empty chat's buttons (Sign in, Use an API key), and when a message
//             is sent before the chat can send it (the message goes once it can)
//   footer    the spec line of the control under the hand (or focused, or last touched), and the ZOOM cells
//
// Keys go to a text field while it has the caret (the editor: Tab indents there; the chat's message box and its
// transcript; Escape leaves each), to a tab's name while it is edited, and to the key dialog while it is open;
// otherwise Tab moves the focus ring over the controls, and the arrows, Page and Home/End adjust the focused dial.
// Every parameter write goes through one funkgui::GestureController (begin/set/end gestures for the host).
//
// Message thread only. The ports and the EditorPort belong to the processor and outlive the Panel.
#pragma once

#include "editor/PresetBrowser.h"

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

namespace testingplaygroundsynth
{
    class EditorPort;
}

namespace testingplaygroundsynth::ui
{
    class Item;                                          // one control or view on the panel (Panel.cpp)
    class FilesItem;                                     // the files' tabs and editors (Panel.cpp)
    class ChatView;                                      // the chat's conversation (ChatView.h)
    class InputItem;                                     // the chat's message box (ChatItems.h)
    class DisplayItem;                                   // a program's display (DisplayItem.h)
    class GuiControlItem;                                // a .gui file's control (GuiItems.h)

    // The size (logical px): the default, the least.
    inline constexpr int kDefaultWidth = 1360, kDefaultHeight = 760;
    inline constexpr int kMinWidth = 1000, kMinHeight = 600;

    // The panel's own primitive tags, for probes that find what they check by tag.
    inline constexpr funkgui::Tag kHeaderTextTag = funkgui::tags::firstProduct;       // name, kind and version
    inline constexpr funkgui::Tag kStatusTag = funkgui::tags::firstProduct + 1;       // the toolbar's status
    inline constexpr funkgui::Tag kProblemTag = funkgui::tags::firstProduct + 2;      // the problems' lines
    inline constexpr funkgui::Tag kMarkTag = funkgui::tags::firstProduct + 3;         // an error's mark in the gutter
    inline constexpr funkgui::Tag kTabTag = funkgui::tags::firstProduct + 4;          // the files' tabs
    inline constexpr funkgui::Tag kChatTag = funkgui::tags::firstProduct + 5;         // the chat's own text
    inline constexpr funkgui::Tag kKeyDialogTag = funkgui::tags::firstProduct + 6;    // the key dialog
    inline constexpr funkgui::Tag kPresetsTag = funkgui::tags::firstProduct + 8;      // the preset browser
    inline constexpr funkgui::Tag kSectionTag = funkgui::tags::firstProduct + 7;      // the dials' sections

    // One message of the last run, as the problems list shows it.
    struct Problem
    {
        std::string file;                                // the file it is in ("program.fm")
        int line = 0, column = 0;                        // 1-based; 0: no place (the plugin's own message)
        bool error = true;                               // else a note
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
        const GuiLayout& guiLayout() const noexcept { return gui_; }
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
        funkgui::Point centreOfPresets() const;
        // PRESETS: the browser (PresetBrowser.h), opened with the preset's name, or the program's title (its first
        // line's words before a colon), in its field.
        void openPresets();
        PresetBrowser& presetBrowser() noexcept { return presets_; }
        const std::string& presetText() const noexcept { return presetText_; }   // the header's "PRESET  NAME  EDITED"
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
        void runNow();                                   // RUN
        // The chat.
        funkgui::TextEditor& chatInput() noexcept;       // the message box
        const std::string& transcript() const;           // the conversation as shown (or the hint)
        const std::string& chatActivity() const noexcept { return activity_; }
        funkgui::Point centreOfSend() const;             // send, or stop
        funkgui::Point centreOfModel() const;            // the model's pill (a menu)
        funkgui::Point centreOfThinking() const;         // the thinking level's pill (a menu)
        std::optional<funkgui::Point> centreOfFast() const;   // fast mode's pill (a switch), on a model that takes it
        funkgui::Point centreOfNewChat() const;
        funkgui::Point centreOfKey() const;
        // The chat's column: open, or folded into a rail at the right (the editor and the panel take the room). The
        // header's chevron folds it, the rail's button opens it; the session keeps it (EditorPort::chatOpen).
        bool chatOpen() const noexcept { return chatOpen_; }
        void setChatOpen(bool open);
        funkgui::Point centreOfChatToggle() const;       // the header's chevron, or the rail's button
        bool chatUnread() const noexcept { return unread_; }   // folded, Claude has written since
        std::optional<funkgui::Point> centreOfAccount() const;        // the header's account pill, while signed in
        ChatView& chatView();                            // the conversation (its own probe hooks)
        const ChatView& chatView() const;
        // The access dialog. Its primary button is Save on the API key tab and Sign in on the account tab.
        bool keyDialogOpen() const noexcept { return keyOpen_; }
        bool dialogOnAccount() const noexcept { return tab_ == DialogTab::account; }
        const std::string& keyDialogWhy() const noexcept { return keyWhy_; }
        funkgui::Point centreOfDialogTab(bool account) const;
        funkgui::Point centreOfKeyField() const;
        funkgui::Point centreOfKeySave() const;
        funkgui::Point centreOfKeyCancel() const;
        std::optional<funkgui::Point> centreOfKeyRemove() const;      // nullopt while no key is stored (the key tab)
        funkgui::Point centreOfUsername() const;
        funkgui::Point centreOfPassword() const;
        std::optional<funkgui::Point> centreOfSignOut() const;        // the account tab, while signed in
        std::optional<funkgui::Point> centreOfUseThis() const;        // Use account / Use this key, when offered

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

    private:
        int  itemAt(funkgui::Point) const;               // the item under p, -1 for none
        bool moveFocus(int dir);
        void setFocus(int item);
        void refresh(double nowSec);                     // the port's status, draft and dials, when they changed
        void openExamples();
        void openLibrary();
        uint32_t nextSeed();                             // RANDOM's: from the clock and a count
        void afterPresets(bool wasOpen);                 // the browser's notice to the status, the focus back
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
        std::string barLabels() const;                   // what the bar's layout depends on
        InputItem& inputItem();
        const InputItem& inputItem() const;
        funkgui::Point centreOf(int item) const;

        // The access dialog.
        enum class DialogTab : uint8_t { account, key };
        struct KeyDialog
        {
            funkgui::Rect box, accountTab, keyTab;
            funkgui::Rect field;                         // the key tab's
            funkgui::Rect user, pass;                    // the account tab's
            funkgui::Rect primary, secondary;            // Save or Sign in; Cancel or Close (at the right)
            funkgui::Rect danger, extra;                 // Remove key or Sign out; Use this key or Use account (left)
        };
        // What the dialog's buttons are now (empty: not shown).
        struct DialogButtons
        {
            std::string primary, secondary, danger, extra;
        };
        KeyDialog keyDialog() const;                     // its places, for the panel's size
        DialogButtons dialogButtons() const;
        void openKeyDialog();                            // on the tab of the access in use
        void openKeyDialog(DialogTab tab);
        void showTab(DialogTab tab);
        void closeKeyDialog();
        void saveKeyFromDialog();
        void removeKeyFromDialog();
        void signInFromDialog();
        void signOutFromDialog();
        void useFromDialog();                            // Use account / Use this key
        void dialogAction(const std::string& label);     // a button, by what it says
        void afterSignIn();                              // the server answered a sign-in
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
            presetsItem_ = -1;
        int themeItem_ = -1, zoomItem_ = -1;
        int panelViewItem_ = -1;                         // the Code / Panel switch's Panel half (viewItem_ the Code one)
        float actionsX_ = 0.0f;                          // the program's card's actions start here (at its right)
        funkgui::Rect programCard_;                      // the program's card (the tabs, the editor or the panel, the dials)
        std::unique_ptr<funkgui::CellModel> zoomModel_;  // the zoom's steps (zoom_ points at it)
        std::string presetLabel() const;                 // the top bar's pill: the preset's name, "edited", or Presets
        void openZoom();                                 // the zoom's menu
        std::string statusShown() const;                 // the status as the card's header shows it (sentence case)
        int newChatItem_ = -1, keyItem_ = -1, accountItem_ = -1, chatViewItem_ = -1, inputItem_ = -1;
        int chatToggleItem_ = -1;                        // fold or open the chat
        bool chatOpen_ = true;
        bool unread_ = false;                            // folded, and the conversation grew since
        std::size_t entriesAtFold_ = 0;
        int modelItem_ = -1, thinkingItem_ = -1, fastItem_ = -1, sendItem_ = -1;
        funkgui::CellModel* zoom_ = nullptr;             // the zoom's steps (zoomModel_)
        float width_ = 0.0f, height_ = 0.0f;
        uint32_t layoutRevision_ = 0;                    // bumps with each layout()
        std::string title_, subtitle_, status_, meter_;
        std::string presetText_;                         // the header's: the preset loaded or saved last
        PresetBrowser presets_{ port_ };
        int focusBeforePresets_ = -1;
        std::string notice_;                             // RANDOM's or PRESETS' word in the status, until noticeUntil_
        double noticeUntil_ = -1.0;
        uint32_t randomCount_ = 0;

        // The places layout() found, which draw() uses.
        float leftW_ = 0.0f;                             // the left column's width (the editor's)
        Controls controls_{};                            // the dials of the program that plays, as refresh() saw them
        PanelSpec panelSpec_;                            // its #pragma panel lines
        uint64_t panelProgram_ = 0;                      // the compile they came from (EditorPort::program)
        DialsLayout dials_;                              // their places
        // A .gui file's interface (when panelSpec_.gui is not empty): its places, its tabs' pages, its scroll, and the
        // items it is made of (a control a slot, pools of pads, envelopes and tabs).
        GuiLayout gui_;
        std::vector<int> guiPages_;                      // by node
        float guiScrollX_ = 0.0f, guiScrollY_ = 0.0f;
        bool panelView_ = false;
        int viewItem_ = -1;                              // the Code / Panel switch's Code half
        std::vector<int> itemOfGuiSlot_;                 // slot -> its GuiControlItem
        std::vector<int> guiPads_, guiEnvelopes_, guiTabs_, guiKeyboards_, guiShaders_;
        std::vector<int> shaderNode_;                    // by guiShaders_: the node it shows (-1: none)
        // The shaders' programs, by PanelSpec::shaders: the id registered with FunkGui (0: none), why not, and whether
        // its code reads the time (it then moves every frame).
        std::vector<uint32_t> shaderIds_;
        std::vector<std::string> shaderWhy_;
        std::vector<bool> shaderMoves_;
        void registerShaders(const std::string& notes);   // after panelSpec_ changed (the compile's notes say why not)
        std::vector<bool> guiItem_;                      // by item: drawn in the interface's area, with its theme
        void layoutGuiItems();                           // after layout() found gui_
        float chatX_ = 0.0f, chatW_ = 0.0f;
        funkgui::Rect card_;                             // the chat's card
        float headerY_ = 0.0f;                           // its header row
        float composerH_ = 0.0f;                         // the message box's height (it follows its text)
        float usageY_ = 0.0f;                            // the line under the message box
        std::string barLabels_;                          // the pills' labels when they were placed
        std::string headerLabel_;                        // the account pill's when the header was placed
        std::string accountLabel() const;                // the account pill's: the username, and the credit when known

        // The chat, as refresh() last saw it.
        uint64_t seenChat_ = 0;
        double chatAt_ = -1.0;                           // when it was last read (the host's clock)
        double busySince_ = -1.0;                        // when the turn under way began
        ChatState chat_;
        std::string activity_;                           // the line under the message box
        uint32_t chatA11y_ = 0;                          // bumps when the conversation's items change
        std::string pendingSend_;                        // a message waiting for a key or a sign-in

        // The access dialog.
        bool keyOpen_ = false;
        DialogTab tab_ = DialogTab::account;
        funkgui::text::LineEdit keyEdit_;
        funkgui::text::LineEdit userEdit_, passEdit_;    // the account tab's username and password
        int accountField_ = 0;                           // the account tab's field with the caret: 0 username, 1 password
        bool awaitingSignIn_ = false;                    // a sign-in was sent; the server has not answered yet
        std::string keyWhy_;                             // why the key typed, or the sign-in, was refused
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
