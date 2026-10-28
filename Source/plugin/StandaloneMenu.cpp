// Source/plugin/StandaloneMenu.cpp: see StandaloneMenu.h.
#include "plugin/StandaloneMenu.h"

namespace playground::standalone
{
    std::vector<Menu> menuBar(std::string_view appName)
    {
        const std::string name(appName);
        const MenuItem line{};
        return {
            { name,
              { { Command::about, "About " + name, {} },
                line,
                { Command::settings, "Settings\xe2\x80\xa6", { ',' } },
                line,
                { Command::services, "Services", {} },
                line,
                { Command::hide, "Hide " + name, { 'h' } },
                { Command::hideOthers, "Hide Others", { 'h', false, true } },
                { Command::showAll, "Show All", {} },
                line,
                { Command::quit, "Quit " + name, { 'q' } } } },
            { "File",
              { { Command::loadState, "Load State\xe2\x80\xa6", {} },
                { Command::saveState, "Save State As\xe2\x80\xa6", {} },
                line,
                { Command::resetState, "Reset to Default State", {} } } },
            { "Edit",
              { { Command::undo, "Undo", { 'z' } },
                { Command::redo, "Redo", { 'z', true } },
                line,
                { Command::cut, "Cut", { 'x' } },
                { Command::copy, "Copy", { 'c' } },
                { Command::paste, "Paste", { 'v' } },
                { Command::selectAll, "Select All", { 'a' } } } },
            { "Window",
              { { Command::minimize, "Minimize", { 'm' } },
                { Command::zoom, "Zoom", {} },
                line,
                { Command::bringAllToFront, "Bring All to Front", {} } } },
        };
    }

    bool isEdit(Command command)
    {
        switch (command)
        {
            case Command::undo:
            case Command::redo:
            case Command::cut:
            case Command::copy:
            case Command::paste:
            case Command::selectAll:
                return true;
            case Command::separator:
            case Command::about:
            case Command::settings:
            case Command::services:
            case Command::hide:
            case Command::hideOthers:
            case Command::showAll:
            case Command::quit:
            case Command::loadState:
            case Command::saveState:
            case Command::resetState:
            case Command::minimize:
            case Command::zoom:
            case Command::bringAllToFront:
                return false;
        }
        return false;
    }

    Shortcut shortcutOf(Command command)
    {
        for (const Menu& m : menuBar({}))
            for (const MenuItem& it : m.items)
                if (it.command == command && command != Command::separator)
                    return it.shortcut;
        return {};
    }

    std::string keysOf(const Shortcut& shortcut)
    {
        if (shortcut.key == 0)
            return {};
        std::string s = "cmd+";
        if (shortcut.shift)
            s += "shift+";
        if (shortcut.option)
            s += "alt+";
        s += shortcut.key;
        return s;
    }
}
