// Source/editor/Panel.h: the plugin's interface, a funkgui::Panel: an FMinus program's editor and its dials, and the
// chat with Claude beside them. It is GPU-free: funkgui::EditorHost runs it live in the plugin
// (Source/editor/gpu/CreateEditorGpu.cpp) and funkgui::HeadlessHost runs it in the probes (gui.dump, gui.chat), drawing
// the same frames.
//
// It is resizable, in both directions, from kMinWidth x kMinHeight (logical px) up: the chat takes about a third of the
// width at the right (kChatMinW to kChatMaxW), the editor and the chat's transcript take the height that is left.
//
//   header    the product name, its kind and version, and the THEME cells (GRAPHITE, PAPER)
//   toolbar   RUN (Cmd-Return in the editor too), EXAMPLES (a menu: the example replaces the text, undoably, and
//             runs), the status (running, edited, compiling, the errors' count, the compile time) and, at the right,
//             the audio thread's load and the output guard's alerts (NaN replaced, clipped)
//   files     a tab for each of the program's files (Sources.h; at most kMaxFiles): the program's first, then the
//             files it can #include by name. A click shows a file, a double-click edits its name in place (Return keeps
//             it, Escape cancels), the + adds a file (its name being edited), a tab's x closes its file (asking first
//             when it has text). A tab whose file the last run's errors name has a mark before its name
//   editor    a funkgui::TextEditor for each file, the shown file's drawn (the processor keeps the files and the tab:
//             EditorPort::draft), FMinus's words highlighted; a mark in the gutter on each line of the shown file the
//             last run's errors name
//   problems  the last run's messages, a line each (the first four, and how many more), each with its file; a click
//             on one with a line number shows its file and puts the caret there
//   dials     the running program's parameters, P1 to P8 (a funkgui::Dial each, labelled with the parameter's name;
//             an unused slot shows nothing): a drag, the wheel or the keys write through the processor's ports, and a
//             double-click returns to the parameter's fminus::default
//   chat      at the right: the model (a menu of the models the key can use), NEW (forgets the conversation, asking
//             first), KEY (the key dialog); the thinking levels the model takes; the conversation (ChatItems.h); what
//             Claude is doing, and SEND or STOP; the message box (Return sends it, Shift-Return breaks the line)
//   key       a dialog over the panel for the Anthropic API key: a masked field (Cmd-V pastes), SAVE, CANCEL and, when a
//             key is stored, REMOVE. It opens from KEY, and when a message is sent before any key is stored (the
//             message goes once a key is saved)
//   footer    the spec line of the control under the hand (or focused, or last touched), and the ZOOM cells
//
// Keys go to a text field while it has the caret (the editor: Tab indents there; the chat's message box and its
// transcript; Escape leaves each), to a tab's name while it is edited, and to the key dialog while it is open;
// otherwise Tab moves the focus ring over the controls, and the arrows, Page and Home/End adjust the focused dial.
// Every parameter write goes through one funkgui::GestureController (begin/set/end gestures for the host).
//
// Message thread only. The ports and the EditorPort belong to the processor and outlive the Panel.
#pragma once

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>

#include "dsp/Sources.h"

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

namespace testingplayground
{
    class EditorPort;
}

namespace testingplayground::ui
{
    class Item;                                          // one control or view on the panel (Panel.cpp)
    class FilesItem;                                     // the files' tabs and editors (Panel.cpp)
    class TranscriptItem;                                // the chat's conversation (ChatItems.h)
    class InputItem;                                     // the chat's message box (ChatItems.h)

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
        funkgui::Point centreOfRun() const;
        funkgui::Point centreOfExamples() const;
        funkgui::Point centreOfProblem(int i) const;
        const std::vector<Problem>& problems() const noexcept { return problems_; }
        const std::string& statusText() const noexcept { return status_; }
        void runNow();                                   // RUN
        // The chat.
        funkgui::TextEditor& chatInput() noexcept;       // the message box
        const std::string& transcript() const;           // the conversation as shown (or the hint)
        const std::string& chatActivity() const noexcept { return activity_; }
        funkgui::Point centreOfSend() const;             // SEND or STOP
        funkgui::Point centreOfModel() const;
        funkgui::Point centreOfNewChat() const;
        funkgui::Point centreOfKey() const;
        std::optional<funkgui::Point> centreOfEffort(int i) const;   // nullopt past the model's levels
        int effortCount() const;
        // The key dialog.
        bool keyDialogOpen() const noexcept { return keyOpen_; }
        const std::string& keyDialogWhy() const noexcept { return keyWhy_; }
        funkgui::Point centreOfKeyField() const;
        funkgui::Point centreOfKeySave() const;
        funkgui::Point centreOfKeyCancel() const;
        std::optional<funkgui::Point> centreOfKeyRemove() const;      // nullopt while no key is stored

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
        void jumpTo(const Problem&);
        void closeFile(int tab);                         // a tab's x: asks first when the file has text
        FilesItem& files();
        const FilesItem& files() const;
        void layout();                                   // every item's place, for width_ x height_
        int  textItem() const;                           // the focused item when it takes every key, else -1

        // The chat.
        void refreshChat(double nowSec);
        void sendChat();                                 // the message box's text, to Claude
        void openModels();
        void newChat();
        TranscriptItem& transcriptItem();
        const TranscriptItem& transcriptItem() const;
        InputItem& inputItem();
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
        int editorItem_ = -1, problemsItem_ = -1, runItem_ = -1, examplesItem_ = -1;
        int themeItem_ = -1, zoomItem_ = -1;
        int modelItem_ = -1, newChatItem_ = -1, keyItem_ = -1, effortItem_ = -1;
        int transcriptItem_ = -1, inputItem_ = -1, sendItem_ = -1;
        funkgui::CellModel* zoom_ = nullptr;             // the ZOOM cells' model (an item owns it)
        funkgui::CellModel* effort_ = nullptr;           // the thinking levels' model (an item owns it)
        float width_ = 0.0f, height_ = 0.0f;
        uint32_t layoutRevision_ = 0;                    // bumps with each layout()
        std::string title_, subtitle_, status_, meter_;

        // The places layout() found, which draw() uses.
        float leftW_ = 0.0f;                             // the left column's width (the editor's)
        float chatX_ = 0.0f, chatW_ = 0.0f;
        float activityY_ = 0.0f;                         // the chat's activity row
        float thinkingY_ = 0.0f;                         // the chat's thinking levels' row

        // The chat, as refresh() last saw it.
        uint64_t seenChat_ = 0;
        double chatAt_ = -1.0;                           // when it was last read (the host's clock)
        double busySince_ = -1.0;                        // when the turn under way began
        ChatState chat_;
        std::string activity_;                           // the activity row's text
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
