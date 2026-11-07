// PROBE name=gui.presets timeout=180
//
// gui.presets: the top bar's presets (Panel's arrows and name, editor/PresetPicker.h's list), headless over a real
// Processor, in a folder of the probe's own with a made-up project, to <build>/probe-out/ (each in GRAPHITE and,
// .paper.png, in PAPER):
//   gui.presets.png          the header with a project open: the pill, the arrows and "Bright 2/4"
//   gui.presets.edited.png   the preset applied with a dial moved: its modified dot
//   gui.presets.none.png     the dials none of the presets': "(no preset) -/4"
//   gui.presets.list.png     the list open: its groups (LEADS, PADS, UNCATEGORISED), the current one marked
//   gui.presets.search.png   a search typed in the list
//   gui.presets.least.png    the least window: the header's pill, arrows and name side by side, clear of the rest
//   gui.presets.claude.png   Claude saved a preset: the status says so, the top bar shows it
//   gui.presets.claude.projects.png   PROJECTS open as Claude saved a version: its note listed, the status saying it
// and checks:
//   HEADER   no project open: no arrows and no name; a project open: the arrows and its preset's name and place
//            ("Bright", "2/4"), the pill naming the project alone; the next arrow applies the next preset (its notice
//            "PRESET: DARK  3/4"), wrapping at the end, the one before wraps back; a dial moved off the one applied:
//            the dot (", edited" to a screen reader), and the next arrow says the changes were not saved; the dials
//            none of the presets': "(no preset)", "-/4"
//   KEYS     ] and [ step them while no text field has the caret; with the code editor's caret they are typed, nothing
//            applied
//   LIST     a click on the name opens the list under it, over everything: a caption a category (LEADS, PADS by
//            name, UNCATEGORISED last), the presets by name in each, the current one lit; Down lights the next and
//            Return applies it (the list closes, the status says it); typing searches (name and category: "da" keeps
//            Dark alone) and Return applies the first match; Escape clears the search, a second closes; a press
//            outside closes it and does nothing else; a click on a row applies it
//   MENU     a right-click on a row: its name, Apply, Category (the presets' categories, None (Uncategorised), New
//            category...); a category chosen is the preset's (its group changes); New category... names one in the
//            field (CATEGORY), an empty Return refused, a typed one set; a factory project's Category greyed
//   A11Y     the list a group with its search field, a list, its captions and its rows (each "n of 4", the current
//            "applied"); a row pressed applies it; the header's items: Previous preset, Preset (its value the name, its
//            place, "edited"), Next preset
//   LEAST    in the least window the pill, the arrows and the name sit side by side between the product's name and
//            the Code / Panel switch, none over another
//   CLAUDE   Claude's projects tool saves a preset (Claude Desktop's call): the status says so ("CLAUDE SAVED PRESET
//            CLAUDE PICK IN GLASS BLOOM") and the top bar shows it applied, in its place; PROJECTS open while it saves
//            a version reads its lists again (the version listed, Claude's note on it) and the status says it; a Delete
//            asked of a version as Claude saves another is withdrawn (that version still chosen, by its number; the next
//            click asks again, nothing deleted); an editor opened afterwards says none of it again
#include "Probe.h"

#include "editor/Panel.h"
#include "editor/PresetPicker.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr float W = static_cast<float>(pg::ui::kDefaultWidth), H = static_cast<float>(pg::ui::kDefaultHeight);

    std::string program()
    {
        const std::string params = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float tone [[fminus::range(100, 10000)]] [[fminus::default(1000)]];\n";
        if constexpr (pg::product::kIsInstrument)
            return "// Probe Tone: a level\nmodule Voice\n{\n" + params
                 + "    void start(float n, float v) {}\n    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// Probe Gain: a level\nmodule Effect\n{\n" + params
                 + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const std::string& name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(name + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    void setDial(pg::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(pg::normOf(p.control(slot), plain));
    }

    const funkgui::A11yItem* titled(const std::vector<funkgui::A11yItem>& items, const std::string& title)
    {
        for (const funkgui::A11yItem& it : items)
            if (it.title == title)
                return &it;
        return nullptr;
    }

    const funkgui::MenuItem* itemLabelled(const std::vector<funkgui::MenuItem>& items, const std::string& label)
    {
        for (const funkgui::MenuItem& it : items)
            if (it.label == label)
                return &it;
        return nullptr;
    }

    std::string nameNow(const pg::EditorPort& port)
    {
        const pg::PresetCycle c = port.presetCycle();
        return c.current >= 0 ? c.presets[static_cast<std::size_t>(c.current)].name : std::string("(none)");
    }

    funkgui::Mods ctrl()                                 // a ctrl-click: the context menu's (a right-click)
    {
        funkgui::Mods m;
        m.ctrl = true;
        return m;
    }
}

PROBE(gui, presets)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-presets-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    pg::projectstore::setDirectoryForTests(folder);

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    pg::Draft d;
    d.files = { { "program.fm", program() } };
    port.setDraft(d);
    port.run(d.files);
    processor.settle();

    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    pg::ui::PresetPicker& list = panel.presetPicker();

    // ---- HEADER, no project ------------------------------------------------------------------------------------------
    {
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        P.eq("header.none_without_project", titled(items, "Previous preset") == nullptr && titled(items, "Preset") == nullptr
                                                && titled(items, "Next preset") == nullptr ? 1 : 0, 1);
    }

    // The made-up project: version 1 at level 0.4, its presets Airy and Bright (Leads), Warm (Pads), Dark (none).
    {
        std::string why;
        pg::SavedVersion saved;
        setDial(processor, 0, 0.4f);
        bool ok = port.saveVersion("Glass Bloom", saved, why);
        const auto preset = [&](const char* name, float level, float tone, const char* category) {
            setDial(processor, 0, level);
            setDial(processor, 1, tone);
            ok = ok && port.savePreset(name, why) && port.setPresetCategory("Glass Bloom", name, category, why);
        };
        preset("Airy", 0.9f, 4000.0f, "Leads");
        preset("Warm", 0.5f, 500.0f, "Pads");
        preset("Dark", 0.2f, 1000.0f, "");
        preset("Bright", 0.8f, 1000.0f, "Leads");
        P.eq("setup.project", ok ? 1 : 0, 1);
        if (!why.empty())
            std::printf("setup: %s\n", why.c_str());
    }
    host.settle();
    host.tick(2);
    host.settle();

    // ---- HEADER ------------------------------------------------------------------------------------------------------
    {
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        const funkgui::A11yItem* name = titled(items, "Preset");
        std::printf("header: %s / %s / pill %s\n", panel.presetLabel().c_str(), panel.presetDetail().c_str(),
                    titled(items, "Projects") != nullptr ? titled(items, "Projects")->value.c_str() : "?");
        P.eq("header.shown", titled(items, "Previous preset") != nullptr && name != nullptr && titled(items, "Next preset") != nullptr ? 1 : 0, 1);
        P.eq("header.name", panel.presetLabel() == "Bright" && panel.presetDetail() == "2/4" && name != nullptr && name->value == "Bright, 2/4" ? 1 : 0, 1);
        P.eq("header.pill_project_alone", titled(items, "Projects") != nullptr && titled(items, "Projects")->value == "Glass Bloom v1" ? 1 : 0, 1);
    }
    writePng(P, host, "gui.presets.png");
    {
        const funkgui::Point next = panel.centreOfPresetStep(1), prev = panel.centreOfPresetStep(-1);
        host.click(next.x, next.y);
        host.settle();
        std::printf("next: %s / %s\n", nameNow(port).c_str(), panel.statusText().c_str());
        P.eq("header.next", nameNow(port) == "Dark" && port.project().preset == "Dark" && panel.presetDetail() == "3/4" ? 1 : 0, 1);
        P.eq("header.notice", panel.statusText().find("DARK") != std::string::npos && panel.statusText().find("3/4") != std::string::npos ? 1 : 0, 1);
        host.click(next.x, next.y);
        host.settle();
        host.click(next.x, next.y);
        host.settle();
        P.eq("header.wraps", nameNow(port) == "Airy" && panel.presetDetail() == "1/4" ? 1 : 0, 1);
        host.click(prev.x, prev.y);
        host.settle();
        P.eq("header.wraps_back", nameNow(port) == "Warm" && panel.presetDetail() == "4/4" ? 1 : 0, 1);
        // Bright again, a dial moved off it: the dot.
        host.click(prev.x, prev.y);
        host.settle();
        host.click(prev.x, prev.y);
        host.settle();
        setDial(processor, 0, 0.66f);
        host.tick(2);
        host.settle();
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        const funkgui::A11yItem* name = titled(items, "Preset");
        P.eq("header.edited_dot", panel.presetCycle().edited && name != nullptr && name->value == "Bright, 2/4, edited" ? 1 : 0, 1);
        writePng(P, host, "gui.presets.edited.png");
        host.click(next.x, next.y);
        host.settle();
        std::printf("away: %s\n", panel.statusText().c_str());
        P.eq("header.not_saved_said", nameNow(port) == "Dark" && panel.statusText().find("THE CHANGES TO BRIGHT WERE NOT SAVED") != std::string::npos ? 1 : 0, 1);
        setDial(processor, 0, 0.33f);
        std::string why;
        port.openProject("Glass Bloom", 1, why);         // no preset applied: none matches level 0.33
        processor.settle();
        setDial(processor, 0, 0.33f);
        host.tick(2);
        host.settle();
        P.eq("header.no_preset", panel.presetLabel() == "(no preset)" && panel.presetDetail() == "-/4" ? 1 : 0, 1);
        writePng(P, host, "gui.presets.none.png");
    }

    // ---- KEYS ----------------------------------------------------------------------------------------------------------
    {
        host.keys("escape");                             // no caret anywhere
        host.settle();
        host.keys("]");
        host.settle();
        P.eq("keys.next", nameNow(port) == "Airy" ? 1 : 0, 1);
        host.keys("]");
        host.settle();
        host.keys("[");
        host.settle();
        P.eq("keys.back", nameNow(port) == "Airy" ? 1 : 0, 1);
        // The code editor's caret: typed, nothing applied.
        funkgui::TextEditor& ed = panel.editor();
        const funkgui::Rect area = ed.textArea();
        host.click(area.x + 30.0f, area.y + 4.0f);
        host.settle();
        const std::string before = ed.text();
        host.keys("]");
        host.settle();
        const std::string after = ed.text();
        P.eq("keys.typed_in_editor", nameNow(port) == "Airy" && after.size() == before.size() + 1 ? 1 : 0, 1);
        host.keys("cmd+z");
        host.settle();
        host.keys("escape");
        host.settle();
    }

    // ---- LIST --------------------------------------------------------------------------------------------------------
    {
        const funkgui::Point name = panel.centreOfPresetName();
        host.click(name.x, name.y);
        host.settle();
        P.eq("list.open", list.isOpen() ? 1 : 0, 1);
        std::vector<std::string> titles;
        for (const auto& g : list.groups())
            titles.push_back(g.title);
        std::string all;
        for (const std::string& t : titles)
            all += t + " | ";
        std::printf("list groups: %s rows %zu, lit %d\n", all.c_str(), list.rowList().size(), list.highlighted());
        P.eq("list.groups", titles == std::vector<std::string>{ "Leads", "Pads", "Uncategorised" } && list.captions() ? 1 : 0, 1);
        P.eq("list.rows", list.rowList().size() == 7 && list.rowOf("Airy") == 1 && list.rowOf("Bright") == 2 && list.rowOf("Warm") == 4
                              && list.rowOf("Dark") == 6 ? 1 : 0, 1);
        P.eq("list.current_lit", list.highlighted() == list.rowOf("Airy") ? 1 : 0, 1);
        const pg::ui::PresetPicker::Places at = list.places(W, H);
        P.eq("list.under_name", at.box.y > name.y && at.box.x <= name.x && at.box.right() >= name.x ? 1 : 0, 1);
        writePng(P, host, "gui.presets.list.png");
        host.keys("down");
        host.settle();
        P.eq("list.down", list.highlighted() == list.rowOf("Bright") ? 1 : 0, 1);
        host.keys("down");                               // the PADS caption skipped
        host.settle();
        P.eq("list.down_skips_caption", list.highlighted() == list.rowOf("Warm") ? 1 : 0, 1);
        host.keys("return");
        host.settle();
        P.eq("list.return_applies", !list.isOpen() && nameNow(port) == "Warm" && panel.statusText().find("WARM") != std::string::npos ? 1 : 0, 1);

        host.click(name.x, name.y);
        host.settle();
        host.keys("d,a");
        host.settle();
        P.eq("list.search", list.search() == "da" && list.rowList().size() == 2 && list.rowOf("Dark") == 1
                                && list.highlighted() == 1 ? 1 : 0, 1);
        writePng(P, host, "gui.presets.search.png");
        host.keys("escape");
        host.settle();
        P.eq("list.escape_clears", list.isOpen() && list.search().empty() && list.rowList().size() == 7 ? 1 : 0, 1);
        host.keys("l,e,a,d");                            // the category's word
        host.settle();
        P.eq("list.search_category", list.rowList().size() == 3 && list.rowOf("Airy") >= 0 && list.rowOf("Bright") >= 0 ? 1 : 0, 1);
        host.keys("escape");
        host.keys("escape");
        host.settle();
        P.eq("list.escape_closes", !list.isOpen() && nameNow(port) == "Warm" ? 1 : 0, 1);

        // A press outside (on the explorer's Examples, under the list's box): it closes, nothing else.
        host.click(name.x, name.y);
        host.settle();
        const pg::Draft draftBefore = port.draft();
        const funkgui::Point examples = panel.centreOfExamples();
        P.eq("list.examples_outside", !list.places(W, H).box.contains(examples) ? 1 : 0, 1);
        host.click(examples.x, examples.y);
        host.settle();
        P.eq("list.outside_closes", !list.isOpen() && nameNow(port) == "Warm" && host.pendingMenu() == nullptr
                                        && port.draft().files == draftBefore.files ? 1 : 0, 1);
        // A click on a row.
        host.click(name.x, name.y);
        host.settle();
        const funkgui::Point dark = list.centreOfRow(list.rowOf("Dark"), W, H);
        host.click(dark.x, dark.y);
        host.settle();
        P.eq("list.click_applies", !list.isOpen() && nameNow(port) == "Dark" ? 1 : 0, 1);
    }

    // ---- MENU --------------------------------------------------------------------------------------------------------
    {
        const funkgui::Point name = panel.centreOfPresetName();
        host.click(name.x, name.y);
        host.settle();
        funkgui::Point row = list.centreOfRow(list.rowOf("Dark"), W, H);
        host.click(row.x, row.y, ctrl());
        host.settle();
        const funkgui::MenuRequest* m = host.pendingMenu();
        const funkgui::MenuItem* category = m != nullptr ? itemLabelled(m->items, "Category") : nullptr;
        P.eq("menu.shown", m != nullptr && !m->items.empty() && m->items[0].header && m->items[0].label == "Dark"
                               && itemLabelled(m->items, "Apply") != nullptr ? 1 : 0, 1);
        P.eq("menu.categories", category != nullptr && category->enabled && category->submenu.size() == 5
                                    && category->submenu[0].label == "Leads" && category->submenu[1].label == "Pads"
                                    && category->submenu[3].label == "None (Uncategorised)" && category->submenu[3].radio
                                    && category->submenu[4].label == "New category..." ? 1 : 0, 1);
        P.eq("menu.chosen", host.chooseMenuItem("Pads") ? 1 : 0, 1);
        host.settle();
        P.eq("menu.set", port.presets("Glass Bloom").size() == 4 && port.presets("Glass Bloom")[2].category == "Pads"
                             && list.isOpen() && list.groups().size() == 2 ? 1 : 0, 1);
        row = list.centreOfRow(list.rowOf("Dark"), W, H);
        host.click(row.x, row.y, ctrl());
        host.settle();
        P.eq("menu.new", host.chooseMenuItem("New category...") && list.categoryMode() ? 1 : 0, 1);
        host.settle();
        host.keys("return");
        host.settle();
        P.eq("menu.empty_refused", list.categoryMode() && !list.why().empty() && port.presets("Glass Bloom")[2].category == "Pads" ? 1 : 0, 1);
        host.keys("B,a,s,s,return");
        host.settle();
        P.eq("menu.typed_set", !list.categoryMode() && port.presets("Glass Bloom")[2].category == "Bass" && list.groups().size() == 3
                                   && list.groups()[0].title == "Bass" ? 1 : 0, 1);

        // ---- A11Y ----
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        const uint32_t base = pg::ui::PresetPicker::kA11yBase;
        int rows = 0, captions = 0;
        bool applied = false, field = false;
        for (const funkgui::A11yItem& it : items)
        {
            rows += it.role == funkgui::A11yRole::listItem && it.parent == base + 2 ? 1 : 0;
            captions += it.role == funkgui::A11yRole::staticText && it.parent == base + 2 ? 1 : 0;
            applied = applied || (it.role == funkgui::A11yRole::listItem && it.title == "Dark" && it.value.find("3 of 4, applied") == 0);
            field = field || (it.role == funkgui::a11yrole::editableText && it.title == "Search presets");
        }
        std::printf("a11y: %d rows, %d captions\n", rows, captions);
        P.eq("a11y.rows", rows, 4);
        P.eq("a11y.captions", captions, 3);
        P.eq("a11y.applied", applied && field ? 1 : 0, 1);
        panel.a11yAction(base + 0x100 + static_cast<uint32_t>(list.rowOf("Airy")), funkgui::A11yAction::press, 0.0);
        host.settle();
        P.eq("a11y.press_applies", !list.isOpen() && nameNow(port) == "Airy" ? 1 : 0, 1);
    }

    // ---- PAPER --------------------------------------------------------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        {
            pg::ui::Panel other(processor.ports(), port);
            funkgui::HeadlessHost otherHost(other, 1, 2.0f);
            otherHost.settle();
            otherHost.tick(2);
            otherHost.settle();
            writePng(P, otherHost, "gui.presets.paper.png");
            const funkgui::Point n = other.centreOfPresetName();
            otherHost.click(n.x, n.y);
            otherHost.settle();
            writePng(P, otherHost, "gui.presets.list.paper.png");
            other.presetPicker().close();
        }
        funkgui::UiPreferences::get().setTheme(0);
    }

    // ---- LEAST ---------------------------------------------------------------------------------------------------------
    {
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        host.settle();
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        const funkgui::A11yItem* pill = titled(items, "Projects");
        const funkgui::A11yItem* prev = titled(items, "Previous preset");
        const funkgui::A11yItem* nm = titled(items, "Preset");
        const funkgui::A11yItem* next = titled(items, "Next preset");
        const funkgui::A11yItem* code = titled(items, "Code view");
        const bool all = pill != nullptr && prev != nullptr && nm != nullptr && next != nullptr && code != nullptr;
        if (all)
            std::printf("least: pill %.0f..%.0f, arrows %.0f / %.0f..%.0f, name %.0f..%.0f, code from %.0f\n",
                        static_cast<double>(pill->bounds.x), static_cast<double>(pill->bounds.right()),
                        static_cast<double>(prev->bounds.x), static_cast<double>(next->bounds.x), static_cast<double>(next->bounds.right()),
                        static_cast<double>(nm->bounds.x), static_cast<double>(nm->bounds.right()), static_cast<double>(code->bounds.x));
        P.eq("least.side_by_side", all && pill->bounds.right() <= prev->bounds.x && prev->bounds.right() <= nm->bounds.x
                                       && nm->bounds.right() <= next->bounds.x && next->bounds.right() + 12.0f <= code->bounds.x
                                       && pill->bounds.w >= 90.0f && nm->bounds.w >= 64.0f ? 1 : 0, 1);
        writePng(P, host, "gui.presets.least.png");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
    }

    // ---- CLAUDE: the projects tool saves (Claude Desktop's call, on this, the message thread) ---------------------------
    {
        std::string why;
        port.openProject("Glass Bloom", 0, why);
        processor.settle();
        host.tick(2);
        host.settle();
        auto* args = new juce::DynamicObject();
        args->setProperty("action", "save_preset");
        args->setProperty("name", "Claude Pick");
        args->setProperty("category", "Leads");
        const pg::ChatAgent::McpResult r = processor.chatAgent().mcpCall("projects", juce::var(args));
        host.tick(2);
        host.settle();
        std::printf("claude: %s / %s / %s\n", panel.statusText().c_str(), panel.presetLabel().c_str(), panel.presetDetail().c_str());
        P.eq("claude.notice", !r.isError && panel.statusText().find("CLAUDE SAVED PRESET CLAUDE PICK IN GLASS BLOOM") != std::string::npos
                                  && panel.presetLabel() == "Claude Pick" && panel.presetDetail() == "3/5" ? 1 : 0, 1);
        writePng(P, host, "gui.presets.claude.png");
        // PROJECTS open while Claude saves a version: its lists read again (the new version listed, the one chosen).
        panel.openProjects();
        host.settle();
        const std::size_t before = panel.projectBrowser().versionList().size();
        setDial(processor, 1, 3000.0f);
        auto* save = new juce::DynamicObject();
        save->setProperty("action", "save_version");
        save->setProperty("note", "a brighter tone");
        (void)processor.chatAgent().mcpCall("projects", juce::var(save));
        host.tick(2);
        host.settle();
        const std::vector<pg::VersionEntry>& versions = panel.projectBrowser().versionList();
        P.eq("claude.browser_reread", panel.projectBrowser().isOpen() && versions.size() == before + 1 && !versions.empty()
                                          && versions.front().note.rfind("Claude: a brighter tone", 0) == 0 ? 1 : 0, 1);
        P.eq("claude.version_notice", panel.statusText().find("CLAUDE SAVED VERSION " + std::to_string(versions.empty() ? 0 : versions.front().number) + " OF GLASS BLOOM") != std::string::npos ? 1 : 0, 1);
        writePng(P, host, "gui.presets.claude.projects.png");
        // Delete asked of version 1 (the oldest row), then Claude saves another version before the user confirms: the
        // card withdraws the question, version 1 still the one chosen (by its number: the rows moved), and the next
        // click asks again, nothing deleted.
        {
            using BL = pg::ui::ProjectBrowser::List;
            pg::ui::ProjectBrowser& browser = panel.projectBrowser();
            const auto kept = [] {
                const std::optional<pg::projectstore::Project> p = pg::projectstore::load("Glass Bloom");
                return p ? p->versions.size() : std::size_t{ 0 };
            };
            const funkgui::Point oldest = browser.centreOfRow(BL::versions, static_cast<int>(browser.versionList().size()) - 1, W, H);
            host.click(oldest.x, oldest.y);
            host.settle();
            const funkgui::Rect remove = browser.places(W, H).remove;
            host.click(remove.centreX(), remove.centreY());
            host.settle();
            const bool asked = browser.removeLabel() == "Delete it";
            const std::size_t count = kept();
            setDial(processor, 1, 5000.0f);
            auto* again = new juce::DynamicObject();
            again->setProperty("action", "save_version");
            (void)processor.chatAgent().mcpCall("projects", juce::var(again));
            host.tick(2);
            host.settle();
            const int c = browser.chosen(BL::versions);
            const std::vector<pg::VersionEntry>& vs = browser.versionList();
            std::printf("claude, delete asked: %s / %s / v%d of %zu\n", browser.removeLabel().c_str(), browser.why().c_str(),
                        c >= 0 && c < static_cast<int>(vs.size()) ? vs[static_cast<std::size_t>(c)].number : -1, vs.size());
            P.eq("claude.delete_withdrawn", asked && kept() == count + 1 && vs.size() == count + 1 && browser.focus() == BL::versions
                                                && c == static_cast<int>(vs.size()) - 1 && vs[static_cast<std::size_t>(c)].number == 1
                                                && browser.removeLabel() == "Delete v1"
                                                && browser.why().find("The lists changed") != std::string::npos ? 1 : 0, 1);
            host.click(remove.centreX(), remove.centreY());
            host.settle();
            P.eq("claude.delete_asks_again", kept() == count + 1 && browser.removeLabel() == "Delete it" ? 1 : 0, 1);
        }
        host.keys("escape");
        host.settle();
        // An editor opened since says none of it again.
        pg::ui::Panel later(processor.ports(), port);
        funkgui::HeadlessHost laterHost(later, 0, 2.0f);
        laterHost.settle();
        P.eq("claude.said_once", later.statusText().find("CLAUDE") == std::string::npos ? 1 : 0, 1);
    }

    // ---- FACTORY -------------------------------------------------------------------------------------------------------
    {
        const std::vector<pg::ProjectEntry> factory = port.factoryProjects();
        const auto it = std::find_if(factory.begin(), factory.end(), [](const pg::ProjectEntry& e) { return e.presets > 0; });
        std::string why;
        P.eq("factory.open", it != factory.end() && port.openProject(it->name, 0, why) ? 1 : 0, 1);
        processor.settle();
        host.tick(2);
        host.settle();
        P.eq("factory.cycles", it != factory.end() && panel.presetCycle().project == it->name
                                   && static_cast<int>(panel.presetCycle().presets.size()) == it->presets ? 1 : 0, 1);
        const funkgui::Point name = panel.centreOfPresetName();
        host.click(name.x, name.y);
        host.settle();
        const funkgui::Point row = list.centreOfRow(list.rowOf(panel.presetCycle().presets.front().name), W, H);
        host.click(row.x, row.y, ctrl());
        host.settle();
        const funkgui::MenuRequest* m = host.pendingMenu();
        const funkgui::MenuItem* category = m != nullptr ? itemLabelled(m->items, "Category") : nullptr;
        P.eq("factory.category_greyed", category != nullptr && !category->enabled && itemLabelled(m->items, "Apply") != nullptr ? 1 : 0, 1);
        host.cancelMenu();
        host.keys("escape");
        host.settle();
    }

    pg::projectstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
