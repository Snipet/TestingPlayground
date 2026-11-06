// Source/editor/gpu/CodeWindow.h: the plugin's editor (FunkGui's EditorHost over the editor's Panel) and the code window
// it opens: the code editor and the explorer in a top-level window of their own, the editor's window showing the panel
// full size meanwhile (editor/Panel.h, "The code window"; EditorPort::codeWindow).
//
// The window follows the session's choice (EditorPort::CodeWindow::out): the top bar's window button sets it and asks
// the editor to follow, which it does later on the message thread (never inside the click that asked, whose window may
// be the one that goes); a session loaded with it out opens it once the editor is on screen, and one loaded with it in
// closes it. Opening makes a second Panel (Panel::Role::code) in a second EditorHost (FunkGui runs several editors in one
// host since v0.21.0, and resizes a second window since v0.24.0), and the editor's Panel lends it the editor
// (Panel::lendEditor): the same tabs, undo steps and carets, not a copy. Closing (the window's close button, the window
// button in either window, a session) takes the editor back (Panel::takeEditorBack) before the window goes; when the
// editor itself goes (a host closes its window), the code window goes first and the session keeps it out, so the next
// editor opens it again.
//
// The window is JUCE's, with the system's title bar, resizable within the code Panel's limits at the editor's zoom. Its
// size (the Panel's, logical px) and its place (screen points) go to the session as the user changes them, and it opens
// there again when that place is still on a display, else beside the editor's window. In a host it floats over the
// host's windows as the host's own plug-in windows do, while the host is the active app (and drops back when another
// app is). In the Standalone it is a window like the app's own.
//
// Message thread only.
#pragma once

#include <funkgui/gpu/EditorHost.h>

#include <juce_events/juce_events.h>

#include <memory>

namespace playground
{
    class Processor;
}

namespace playground::ui
{
    class Panel;

    // Both windows' EditorHost config: the UI zoom's steps and its machine-wide preference (never in the session).
    funkgui::EditorConfig editorConfig();

    class Editor final : public funkgui::EditorHost
    {
    public:
        Editor(Processor& owner, funkgui::EditorConfig config, std::unique_ptr<Panel> panel);
        ~Editor() override;

        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;

    private:
        class Window;                                    // the code window (CodeWindow.cpp)

        void follow();                                   // the window as the session has it: opened or closed
        void followLater();                              // ... on the message thread, after the event that asked
        void open();
        void close();
        void raiseCode();                                // the code window to the front (the Code half, out)
        void raiseMain();                                // the editor's window to the front (the code window's Panel half)
        void tick();                                     // 4 Hz: follow the session; float with the host

        Processor& processor_;
        Panel& panel_;
        std::unique_ptr<Window> window_;
        juce::TimedCallback timer_;
        bool hostFloat_ = false;                         // the window floats over the host's windows now
    };
}
