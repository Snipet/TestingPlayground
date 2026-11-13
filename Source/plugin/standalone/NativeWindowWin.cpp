// Source/plugin/standalone/NativeWindowWin.cpp: NativeWindow.h on Windows and Linux (JUCE's alone: nothing here is
// Windows'). The window keeps the system's own title bar (the editor keeps no room for buttons: styleWindow reports
// none, and placeButtons and dragWindow do nothing), and the menu bar is a JUCE MenuBarModel in the window, under the
// title bar (menuBarModel(), which App.cpp gives the window): File (Settings..., Load State..., Save State As..., Reset
// to Default State, Exit), Edit (StandaloneMenu.h's), Help (About). The commands macOS alone has (Services, Hide, Hide
// Others, Show All, Bring All to Front) are left out; Minimize and Zoom are the title bar's. The shortcuts are shown
// with Ctrl, and the editor still gets every key first: a menu's shortcut is only its label here (JUCE's menu bar sends
// none), so no key the editor uses is taken.
#include "plugin/standalone/NativeWindow.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string>
#include <utility>

namespace playground::standalone::native
{
    namespace
    {
        // Commands as JUCE menu ids (0 is no item in JUCE).
        int idOf(Command c) { return static_cast<int>(c) + 1; }
        Command commandOf(int id) { return static_cast<Command>(id - 1); }

        bool macOnly(Command c)
        {
            switch (c)
            {
                case Command::services:
                case Command::hide:
                case Command::hideOthers:
                case Command::showAll:
                case Command::minimize:
                case Command::zoom:
                case Command::bringAllToFront:
                    return true;
                case Command::separator:
                case Command::about:
                case Command::settings:
                case Command::quit:
                case Command::loadState:
                case Command::saveState:
                case Command::resetState:
                case Command::undo:
                case Command::redo:
                case Command::cut:
                case Command::copy:
                case Command::paste:
                case Command::selectAll:
                case Command::find:
                case Command::findReplace:
                case Command::findInFiles:
                    return false;
            }
            return false;
        }

        juce::String shortcutText(const Shortcut& s)
        {
            if (s.key == 0)
                return {};
            juce::String t = "Ctrl+";
            if (s.shift)
                t << "Shift+";
            if (s.option)
                t << "Alt+";
            return t + juce::String::charToString(static_cast<juce::juce_wchar>(s.key)).toUpperCase();
        }

        class Model final : public juce::MenuBarModel
        {
        public:
            void set(std::vector<Menu> macMenus, std::function<bool(Command)> run)
            {
                run_ = std::move(run);
                menus_.clear();
                // macOS's app menu becomes File (its Settings... and Quit, as Exit) and Help (About); File's items join
                // the new File; Edit stays; Window's are the title bar's.
                Menu file{ "File", {} }, help{ "Help", {} }, edit{ "Edit", {} };
                for (const Menu& m : macMenus)
                    for (const MenuItem& it : m.items)
                    {
                        if (it.command == Command::about)
                            help.items.push_back(it);
                        else if (it.command == Command::settings)
                            file.items.push_back(it);
                        else if (it.command == Command::loadState || it.command == Command::saveState
                                 || it.command == Command::resetState)
                            file.items.push_back(it);
                        else if (isEdit(it.command))
                            edit.items.push_back(it);
                        else if (m.title == "Edit" && it.command == Command::separator && !edit.items.empty())
                            edit.items.push_back(it);   // its lines (before the find items)
                    }
                file.items.insert(file.items.begin() + (file.items.empty() ? 0 : 1), MenuItem{});   // after Settings...
                file.items.push_back(MenuItem{});
                file.items.push_back(MenuItem{ Command::quit, "Exit", {} });
                menus_ = { file, edit, help };
                menuItemsChanged();
            }

            juce::StringArray getMenuBarNames() override
            {
                juce::StringArray names;
                for (const Menu& m : menus_)
                    names.add(juce::String::fromUTF8(m.title.c_str()));
                return names;
            }

            juce::PopupMenu getMenuForIndex(int index, const juce::String&) override
            {
                juce::PopupMenu menu;
                if (index < 0 || index >= static_cast<int>(menus_.size()))
                    return menu;
                for (const MenuItem& it : menus_[static_cast<std::size_t>(index)].items)
                {
                    if (it.command == Command::separator || it.title.empty())
                    {
                        menu.addSeparator();
                        continue;
                    }
                    if (macOnly(it.command))
                        continue;
                    juce::PopupMenu::Item item(juce::String::fromUTF8(it.title.c_str()));
                    item.itemID = idOf(it.command);
                    item.shortcutKeyDescription = shortcutText(it.shortcut);
                    menu.addItem(std::move(item));
                }
                return menu;
            }

            void menuItemSelected(int id, int) override
            {
                if (id <= 0)
                    return;
                const Command c = commandOf(id);
                if (c == Command::quit)
                {
                    if (auto* app = juce::JUCEApplicationBase::getInstance())
                        app->systemRequestedQuit();
                    return;
                }
                if (run_)
                    run_(c);
            }

        private:
            std::vector<Menu> menus_;
            std::function<bool(Command)> run_;
        };

        Model& model()
        {
            static Model m;
            return m;
        }
    }

    ButtonsSize styleWindow(void*, std::function<void(bool)>) { return {}; }

    void placeButtons(void*, float, float) {}

    void dragWindow(void*, int) {}

    void installMenuBar(const std::vector<Menu>& menus, std::function<bool(Command)> run)
    {
        model().set(menus, std::move(run));
    }

    juce::MenuBarModel* menuBarModel() { return &model(); }

    void showAbout(const char* name, const char* version, const char* credits)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               juce::String("About ") + juce::String::fromUTF8(name),
                                               juce::String::fromUTF8(name) + " " + juce::String::fromUTF8(version) + "\n\n"
                                                   + juce::String::fromUTF8(credits));
    }
}
