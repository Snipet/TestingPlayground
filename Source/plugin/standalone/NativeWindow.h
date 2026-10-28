// Source/plugin/standalone/NativeWindow.h: the Standalone app's macOS side (NativeWindow.mm, Objective-C++), for
// App.cpp. Each call takes a JUCE peer's native handle (ComponentPeer::getNativeHandle: its NSView) and acts on its
// NSWindow. Message thread only.
//
//   styleWindow()   the unified look: the title bar transparent over the content, which fills the window (full-size
//                   content), and the title hidden, so the window's close, minimise and zoom buttons sit over the
//                   editor's own top bar. A view of ours in the title bar keeps the buttons where placeButtons() put
//                   them whenever AppKit lays the title bar out again (a resize, a style change). Entering full
//                   screen, where the system shows the title bar itself, and leaving it are reported; after leaving,
//                   the look is applied again (JUCE resets the window's style then).
//   placeButtons()  the buttons' left edge and middle, in points from the window's top left
//   dragWindow()    a press on the editor's top bar where nothing is: the press (NSApp's current event) drags the
//                   window; the second press of a double-click does what the system's setting says (Desktop & Dock:
//                   zoom, minimise or nothing)
//   installMenuBar() the menu bar from StandaloneMenu.h: AppKit's own actions for Services, Hide, Hide Others, Show
//                   All, Quit (terminate:, which JUCE turns into systemRequestedQuit), Minimize, Zoom and Bring All to
//                   Front; every other command goes to `run`, and an Edit command `run` does not take (false: no JUCE
//                   window is key) goes to the first responder as AppKit's own (paste:, ...), for a save panel's field.
//                   Window is the windows menu, Services the services menu
//   showAbout()     the system's About panel, with the app's name, version and a line of credits
#pragma once

#include "plugin/StandaloneMenu.h"

#include <functional>
#include <vector>

namespace playground::standalone::native
{
    struct ButtonsSize
    {
        float width = 0.0f, height = 0.0f;       // points; 0 x 0 when the window has none
    };

    ButtonsSize styleWindow(void* peerView, std::function<void(bool fullScreen)> fullScreenChanged);
    void placeButtons(void* peerView, float x, float middle);
    void dragWindow(void* peerView, int clicks);
    void installMenuBar(const std::vector<Menu>& menus, std::function<bool(Command)> run);
    void showAbout(const char* name, const char* version, const char* credits);
}
