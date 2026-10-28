// PROBE name=gui.window timeout=120
//
// gui.window: the Standalone's own window (Source/plugin/standalone/), as far as it is checked without opening one: its
// menu bar as data (plugin/StandaloneMenu.h), and the editor's side of the window, headless over a real Processor whose
// window hooks the probe sets as the app's window does (Processor::setWindowHooks: macOS's buttons, 60 x 14 points), to
// <build>/probe-out/:
//   gui.window.png        the top bar keeping the window's buttons room (the buttons are macOS's: not drawn here)
//   gui.window.host.png   a host's window: no room
// and checks:
//   MENUS  the app menu (About <Name>, Settings... Cmd-, , Services, Hide <Name> Cmd-H, Hide Others Cmd-Option-H, Show
//          All, Quit <Name> Cmd-Q), File (Load State..., Save State As..., Reset to Default State), Edit (Undo Cmd-Z, Redo
//          Cmd-Shift-Z, Cut, Copy, Paste, Select All: Cmd-X, C, V, A) and Window (Minimize Cmd-M, Zoom, Bring All to
//          Front), in that order; no two items share a shortcut; none takes a key the editor uses (Cmd-Return, Cmd-S)
//   HOST   no hooks (a host's window): no room, the product's mark where it always was, a press on the top bar drags
//          nothing
//   ROOM   the window's buttons: the top bar keeps them room at its left (the mark after them and a gap), the project's
//          pill clear of it; the window told once where they go (the top bar's left inner edge, its middle); at 200 %
//          the room in logical px about halves (the buttons keep their size in points), back at 100 %; full screen
//          (no buttons over the editor): no room
//   DRAG   a press on the top bar where nothing is drags the window (clicks 1: its margin, the product's name), a
//          double-click's second press asks for the double-click's action (clicks 2); a press on a button of the top
//          bar (the CODE cell), a right-click, a press under the top bar and a press while PROJECTS is open drag nothing
//   EDIT   each Edit item sends its shortcut to the focused component (as the app does), and the editor's text field
//          takes it: Select All and Copy (the clipboard holds the text), Cut (empty), Paste (back), Undo (empty), Redo
//          (back); with no text field focused they change nothing
#include "Probe.h"

#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"
#include "plugin/StandaloneMenu.h"

#include "Product.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pg = playground;
namespace sm = playground::standalone;

namespace
{
    // The app's window, as the hooks see it.
    struct FakeWindow
    {
        int places = 0;
        float x = -1.0f, middle = -1.0f;
        std::vector<int> drags;                  // each press's clicks

        pg::Processor::WindowHooks hooks(pg::WindowButtons buttons)
        {
            pg::Processor::WindowHooks h;
            h.buttons = buttons;
            h.place = [this](float px, float pm) {
                ++places;
                x = px;
                middle = pm;
            };
            h.drag = [this](int clicks) { drags.push_back(clicks); };
            return h;
        }
    };

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // The top bar's leftmost text (the product's name): where its first glyph starts.
    float nameLeft(funkgui::HeadlessHost& host)
    {
        float left = 1.0e9f;
        for (const funkgui::Prim& p : host.draw().prims)
            if (p.tag == pg::ui::kHeaderTextTag && static_cast<funkgui::PrimKind>(static_cast<int>(p.d2[2])) == funkgui::PrimKind::text
                && p.y0 < 60.0f)
                left = std::min(left, p.x0);
        return left;
    }

    const sm::MenuItem* itemOf(const std::vector<sm::Menu>& bar, sm::Command c)
    {
        for (const sm::Menu& m : bar)
            for (const sm::MenuItem& it : m.items)
                if (it.command == c)
                    return &it;
        return nullptr;
    }

    std::string titles(const sm::Menu& m)
    {
        std::string s;
        for (const sm::MenuItem& it : m.items)
            s += (s.empty() ? "" : "|") + (it.command == sm::Command::separator ? std::string("-") : it.title);
        return s;
    }
}

PROBE(gui, window)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-window-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    pg::projectstore::setDirectoryForTests(folder);

    // ---- MENUS ---------------------------------------------------------------------------------------------------------
    {
        const std::string name = pg::product::kName;
        const std::vector<sm::Menu> bar = sm::menuBar(name);
        P.eq("menus.count", static_cast<int64_t>(bar.size()), 4);
        if (bar.size() == 4)
        {
            P.eq("menus.order", bar[0].title == name && bar[1].title == "File" && bar[2].title == "Edit" && bar[3].title == "Window"
                                    ? 1 : 0, 1);
            const std::string app = titles(bar[0]), file = titles(bar[1]), edit = titles(bar[2]), window = titles(bar[3]);
            std::printf("  %s: %s\n  File: %s\n  Edit: %s\n  Window: %s\n", name.c_str(), app.c_str(), file.c_str(), edit.c_str(),
                        window.c_str());
            P.eq("menus.app", app == "About " + name + "|-|Settings\xe2\x80\xa6|-|Services|-|Hide " + name + "|Hide Others|Show All|-|Quit " + name
                                  ? 1 : 0, 1);
            P.eq("menus.file", file == "Load State\xe2\x80\xa6|Save State As\xe2\x80\xa6|-|Reset to Default State" ? 1 : 0, 1);
            P.eq("menus.edit", edit == "Undo|Redo|-|Cut|Copy|Paste|Select All" ? 1 : 0, 1);
            P.eq("menus.window", window == "Minimize|Zoom|-|Bring All to Front" ? 1 : 0, 1);
        }
        const auto key = [&](sm::Command c) {
            const sm::MenuItem* it = itemOf(bar, c);
            return it != nullptr ? sm::keysOf(it->shortcut) : std::string("(none)");
        };
        P.eq("menus.settings_key", key(sm::Command::settings) == "cmd+," ? 1 : 0, 1);
        P.eq("menus.quit_key", key(sm::Command::quit) == "cmd+q" ? 1 : 0, 1);
        P.eq("menus.hide_keys", key(sm::Command::hide) == "cmd+h" && key(sm::Command::hideOthers) == "cmd+alt+h" ? 1 : 0, 1);
        P.eq("menus.edit_keys", key(sm::Command::undo) == "cmd+z" && key(sm::Command::redo) == "cmd+shift+z"
                                    && key(sm::Command::cut) == "cmd+x" && key(sm::Command::copy) == "cmd+c"
                                    && key(sm::Command::paste) == "cmd+v" && key(sm::Command::selectAll) == "cmd+a"
                                    ? 1 : 0, 1);
        P.eq("menus.window_keys", key(sm::Command::minimize) == "cmd+m" && key(sm::Command::zoom).empty() ? 1 : 0, 1);
        P.eq("menus.file_no_keys", key(sm::Command::loadState).empty() && key(sm::Command::saveState).empty()
                                       && key(sm::Command::resetState).empty() ? 1 : 0, 1);
        std::set<std::string> seen;
        int shared = 0, editorKeys = 0, edits = 0;
        for (const sm::Menu& m : bar)
            for (const sm::MenuItem& it : m.items)
            {
                const std::string k = sm::keysOf(it.shortcut);
                if (k.empty())
                    continue;
                shared += seen.insert(k).second ? 0 : 1;
                editorKeys += (k == "cmd+s" || k == "cmd+return") ? 1 : 0;
                edits += sm::isEdit(it.command) ? 1 : 0;
            }
        P.eq("menus.shortcuts_unique", shared, 0);
        P.eq("menus.none_the_editors", editorKeys, 0);
        P.eq("menus.edit_commands", edits, 6);
        P.eq("menus.shortcut_of", sm::shortcutOf(sm::Command::redo) == sm::Shortcut{ 'z', true, false } ? 1 : 0, 1);
    }

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    processor.settle();
    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();

    // ---- HOST ----------------------------------------------------------------------------------------------------------
    FakeWindow win;
    const float plainLeft = nameLeft(host);
    P.near("host.no_room", static_cast<double>(panel.windowButtonsRoom()), 0.0, 1e-6);
    P.near("host.no_buttons", static_cast<double>(port.windowButtons().width), 0.0, 1e-6);
    host.click(5.0f, 5.0f);
    host.settle();
    P.eq("host.no_drag", static_cast<int64_t>(win.drags.size()), 0);
    writePng(P, host, "gui.window.host.png");

    // ---- ROOM ----------------------------------------------------------------------------------------------------------
    processor.setWindowHooks(win.hooks({ 60.0f, 14.0f }));
    host.tick(2);
    host.settle();
    const float room = panel.windowButtonsRoom();
    P.near("room.kept", static_cast<double>(room), 72.0, 0.5);                       // 60 points at 100 %, and a 12 px gap
    const float roomLeft = nameLeft(host);
    P.near("room.name_moves", static_cast<double>(roomLeft - plainLeft), static_cast<double>(room), 0.5);
    P.ge("room.name_after_buttons", static_cast<double>(roomLeft), 30.0 + 60.0 + 12.0);
    P.ge("room.pill_clear", static_cast<double>(panel.centreOfProjects().x), static_cast<double>(roomLeft) + 120.0);
    P.eq("room.placed_once", win.places, 1);
    P.near("room.place_x", static_cast<double>(win.x), 30.0, 1e-3);                  // the top bar's left inner edge
    P.near("room.place_middle", static_cast<double>(win.middle), 30.0, 1e-3);        // its middle
    writePng(P, host, "gui.window.png");
    host.setZoom({ 100, 125, 150, 200 }, 200);
    host.tick(2);
    host.settle();
    P.near("room.zoom_200", static_cast<double>(panel.windowButtonsRoom()), 30.0 + 12.0, 0.5);
    P.eq("room.zoom_not_placed_again", win.places, 1);                               // the window follows the zoom itself
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.tick(2);
    host.settle();
    P.near("room.zoom_back", static_cast<double>(panel.windowButtonsRoom()), 72.0, 0.5);
    processor.setWindowHooks(win.hooks({}));                                         // full screen: no buttons over it
    host.tick(2);
    host.settle();
    P.near("room.full_screen", static_cast<double>(panel.windowButtonsRoom()), 0.0, 1e-6);
    P.near("room.full_screen_name", static_cast<double>(nameLeft(host)), static_cast<double>(plainLeft), 0.5);
    processor.setWindowHooks(win.hooks({ 60.0f, 14.0f }));
    host.tick(2);
    host.settle();
    P.eq("room.placed_again", win.places, 2);

    // ---- DRAG ----------------------------------------------------------------------------------------------------------
    win.drags.clear();
    host.click(5.0f, 5.0f);                                                          // the margin above the top bar
    host.click(roomLeft + 20.0f, 30.0f);                                             // on the product's name
    host.settle();
    P.eq("drag.empty", static_cast<int64_t>(win.drags.size()), 2);
    P.eq("drag.clicks", win.drags.size() == 2 && win.drags[0] == 1 && win.drags[1] == 1 ? 1 : 0, 1);
    win.drags.clear();
    host.doubleClick(static_cast<float>(panel.width()) * 0.5f, 4.0f);
    host.settle();
    P.eq("drag.double_click", win.drags.size() == 2 && win.drags[0] == 1 && win.drags[1] == 2 ? 1 : 0, 1);
    win.drags.clear();
    const funkgui::Point code = panel.centreOfView(false);
    host.click(code.x, code.y);
    host.click(5.0f, 5.0f, funkgui::Mods{ false, false, false, true });              // a right-click (ctrl)
    host.click(panel.programCard().x + 4.0f, panel.programCard().y + 4.0f);          // under the top bar
    panel.openProjects();
    host.settle();
    host.click(5.0f, 5.0f);
    host.keys("escape");
    host.settle();
    P.eq("drag.none_else", static_cast<int64_t>(win.drags.size()), 0);
    P.eq("drag.projects_closed", panel.projectBrowser().isOpen() ? 0 : 1, 1);

    // ---- EDIT ----------------------------------------------------------------------------------------------------------
    funkgui::TextEditor& ed = panel.editor();
    const funkgui::Rect eb = ed.bounds();
    host.click(eb.x + 0.5f * eb.w, eb.y + 0.5f * eb.h);                              // the caret into the program
    host.settle();
    const std::string text = ed.text();
    const auto send = [&](sm::Command c) {
        host.keys(sm::keysOf(sm::shortcutOf(c)).c_str());
        host.settle();
    };
    send(sm::Command::selectAll);
    send(sm::Command::copy);
    P.eq("edit.copy", host.log.lastCopy == text && !text.empty() ? 1 : 0, 1);
    send(sm::Command::cut);
    P.eq("edit.cut", ed.text().empty() ? 1 : 0, 1);
    send(sm::Command::paste);
    P.eq("edit.paste", ed.text() == text ? 1 : 0, 1);
    send(sm::Command::undo);
    P.eq("edit.undo", ed.text().empty() ? 1 : 0, 1);
    send(sm::Command::redo);
    P.eq("edit.redo", ed.text() == text ? 1 : 0, 1);
    host.keys("escape");                                                             // the caret leaves the editor
    host.settle();
    host.setClipboard("something else");
    send(sm::Command::paste);
    send(sm::Command::selectAll);
    send(sm::Command::cut);
    P.eq("edit.unfocused_nothing", ed.text() == text ? 1 : 0, 1);

    processor.setWindowHooks({});
    folder.deleteRecursively();
    return P.finish();
}
