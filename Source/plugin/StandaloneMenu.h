// Source/plugin/StandaloneMenu.h: the Standalone app's menu bar, as data (JUCE-free). The app (plugin/standalone/)
// builds the macOS menu bar from it, and the gui.window probe reads it: what JUCE's Options button offered is in the
// app menu and in File, the Edit menu reaches the focused text field, and Window minimises and zooms.
//
//   <Name>   About <Name>; Settings... (Cmd-,: the audio and MIDI devices); Services; Hide <Name> (Cmd-H), Hide Others
//            (Cmd-Option-H), Show All; Quit <Name> (Cmd-Q)
//   File     Load State..., Save State As..., Reset to Default State (JUCE's Options menu's three)
//   Edit     Undo (Cmd-Z), Redo (Cmd-Shift-Z), Cut, Copy, Paste, Select All (Cmd-X, C, V, A); Find..., Find and
//            Replace..., Find in All Files... (no shortcut of the menu's: the editor's Cmd-F, Cmd-Alt-F, Shift-Cmd-F)
//   Window   Minimize (Cmd-M), Zoom; Bring All to Front
//
// A shortcut always holds Command. The editor takes its keys first (the window sends a key to the focused component
// before the menu bar sees it): a shortcut reaches the menu only when the editor does not use the key, so no menu
// takes a key the editor uses (Cmd-Return, Cmd-S, the text fields' own). An Edit item chosen with the mouse sends its
// own shortcut to the focused component, so it does what typing the shortcut does there (editKeys); the find items have
// no shortcut (the editor's keys are never a menu's) and send the editor's key instead (editorKeyOf), which the Panel
// takes from anywhere while the code shows.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playground::standalone
{
    enum class Command
    {
        separator,
        about, settings, services, hide, hideOthers, showAll, quit,             // the app menu
        loadState, saveState, resetState,                                       // File
        undo, redo, cut, copy, paste, selectAll, find, findReplace, findInFiles,   // Edit
        minimize, zoom, bringAllToFront                                         // Window
    };

    struct Shortcut
    {
        char key = 0;                            // with Command, lower case ('q', ','); 0: none
        bool shift = false;
        bool option = false;

        friend bool operator==(const Shortcut&, const Shortcut&) = default;
    };

    struct MenuItem
    {
        Command command = Command::separator;
        std::string title;                       // "" for a separator
        Shortcut shortcut;
    };

    struct Menu
    {
        std::string title;                       // the app menu's is the app's name
        std::vector<MenuItem> items;
    };

    // The menu bar of the app `appName` (product::kName), in order: the app menu, File, Edit, Window.
    std::vector<Menu> menuBar(std::string_view appName);

    // The Edit menu's commands (undo to findInFiles): chosen, each sends its key to the focused component.
    bool isEdit(Command command);

    // The key an Edit command sends to the focused component: its shortcut, or the editor's key for one that has none
    // (Find... Cmd-F, Find and Replace... Cmd-Alt-F, Find in All Files... Shift-Cmd-F). None for any other command.
    Shortcut editorKeyOf(Command command);

    // A command's shortcut in the menu bar (none for a command it has not, or one without).
    Shortcut shortcutOf(Command command);

    // A shortcut in the keys' grammar of FunkGui's hosts ("cmd+shift+z": HeadlessHost::keys, EditorHost's UI_KEYS),
    // which turns a token into the juce::KeyPress EditorHost::keyPressed receives: the key's lower-case code, no text,
    // the modifiers. "" for none. The app builds its KeyPress the same way.
    std::string keysOf(const Shortcut& shortcut);
}
