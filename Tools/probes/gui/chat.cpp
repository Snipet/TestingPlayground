// PROBE name=gui.chat timeout=240
//
// gui.chat: the editor's chat column, its key dialog and the resizable panel, headless (funkgui::HeadlessHost), over a
// real processor whose chat talks to a scripted API (../plugin/FakeClaude.h): nothing reaches the network, and the key
// folder is the probe's own. It writes, to <build>/probe-out/:
//   gui.chat.png          a conversation: the user's message, Claude's thinking, what it did, its answer with code
//   gui.chat.key.png      the key dialog with a key pasted (masked)
//   gui.chat.paper.png    the conversation in PAPER
//   gui.chat.small.png    the panel at its least size
//   gui.chat.large.png    the panel large
// and checks:
//   HINT     with no key the conversation says how to start, naming KEY
//   KEY      a message sent with no key opens the key dialog and stays in the message box; Escape closes the dialog
//            (the message stays, unsent); sent again, it waits for the key; while the dialog is open the pointer and
//            the keys reach nothing else; a key
//            that is not one is refused (the dialog says why; nothing is stored); Cmd-V pastes a key; nothing the
//            panel shows or speaks holds the key; SAVE stores it, closes the dialog and sends the waiting message;
//            KEY opens the dialog; REMOVE (shown once a key is stored) forgets it; Return in the dialog saves one
//   SEND     Shift-Return breaks the line in the message box, Return sends it (the box empties); the conversation
//            shows the turn (YOU, CLAUDE, the thinking, what Claude did, its answer) and the activity line the usage;
//            the files Claude wrote are the editor's tabs and play; Cmd-Z in the program's editor gives back the
//            text it had before Claude wrote it, as one step
//   MODEL    the model's button opens a menu of the models, the chosen one ticked; choosing another changes the
//            model and the thinking levels shown (the model's); a click on a level chooses it
//   STOP     while Claude works, the activity line says so and SEND is STOP: a click ends the turn
//   NEW      NEW asks first, then forgets the conversation (the hint is back); Tab leaves the message box
//   RESIZE   a size below the least is raised to it; at the least size and at a large one every item lies inside the
//            panel, the chat's column right of the editor; the editor and the transcript grow with the height; the
//            processor keeps the editor's size in the session
#include "Probe.h"

#include "../plugin/FakeClaude.h"

#include "editor/Panel.h"
#include "plugin/ChatAgent.h"
#include "plugin/ChatStore.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace tp = testingplaygroundsynth;

namespace
{
    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // Typed key by key (letters, digits and spaces; keys() takes no comma).
    void type(funkgui::HeadlessHost& host, std::string_view text)
    {
        std::string spec;
        for (const char c : text)
        {
            if (!spec.empty())
                spec += ',';
            spec += c == ' ' ? std::string("space") : std::string(1, c);
        }
        host.keys(spec.c_str());
    }

    void clickAt(funkgui::HeadlessHost& host, funkgui::Point p) { host.click(p.x, p.y); }

    void clickInput(funkgui::HeadlessHost& host, tp::ui::Panel& panel)
    {
        const funkgui::Rect r = panel.chatInput().bounds();
        host.click(r.centreX(), r.centreY());
    }

    // The turn finished, its compile and dials taken, the panel caught up.
    void settleChat(tp::Processor& p, funkgui::HeadlessHost& host)
    {
        p.chatAgent().waitIdle();
        p.settle();
        host.tick(4);
        host.settle();
    }

    tp::ChatState chat(tp::Processor& p) { return p.chatAgent().chatState(); }

    // Nothing the panel speaks holds the key, or any 12 characters of it.
    bool a11yHoldsKey(const funkgui::HeadlessHost& host, const std::string& key)
    {
        for (const funkgui::A11yItem& item : host.accessibility())
            for (const std::string* s : { &item.title, &item.description, &item.help, &item.value })
                for (std::size_t p = 0; p + 12 <= key.size(); ++p)
                    if (s->find(key.substr(p, 12)) != std::string::npos)
                        return true;
        return false;
    }

    // Every item the panel speaks of lies inside it.
    bool allInside(const funkgui::HeadlessHost& host, const tp::ui::Panel& panel)
    {
        const float w = static_cast<float>(panel.width()), h = static_cast<float>(panel.height());
        for (const funkgui::A11yItem& item : host.accessibility())
        {
            const funkgui::Rect& b = item.bounds;
            if (b.x < 0.0f || b.y < 0.0f || b.x + b.w > w + 0.5f || b.y + b.h > h + 0.5f)
            {
                std::printf("outside: %s (%g %g %g %g) in %g x %g\n", item.title.c_str(), static_cast<double>(b.x),
                            static_cast<double>(b.y), static_cast<double>(b.w), static_cast<double>(b.h),
                            static_cast<double>(w), static_cast<double>(h));
                return false;
            }
        }
        return true;
    }

    // A program with one dial, "level", from 0 to 2.
    std::string levelProgram()
    {
        const std::string param = "    param float level [[fminus::range(0, 2)]] [[fminus::default(1)]];\n";
        if constexpr (tp::product::kIsInstrument)
            return "module Voice\n{\n" + param +
                   "    void start(float n, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "module Effect\n{\n" + param +
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            left[i] *= level[i];\n            right[i] *= level[i];\n        }\n    }\n};\n";
    }

    juce::var input(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        return tp::claude::object(fields);
    }
}

PROBE(gui, chat)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File keys = juce::File(juce::String(probe::outPath("gui-chat-keys")));
    keys.deleteRecursively();
    tp::chatstore::setDirectoryForTests(keys);
    const std::string key = fake::fakeKey();

    tp::Processor processor;
    auto script = std::make_shared<fake::Script>();
    script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "medium", "high", "xhigh", "max" } },
                                        { "claude-sonnet-5-5", "Claude Sonnet 5.5", true, { "low", "medium", "high", "xhigh" } },
                                        { "claude-haiku-5-5", "Claude Haiku 5.5", true, { "low", "medium", "high" } } });
    processor.chatAgent().setTransport(std::make_unique<fake::Transport>(script));

    tp::ui::Panel panel(processor.ports(), processor.editorPort());
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    P.le("open.settle_frames", host.settle(), 599.0);
    P.eq("open.default_size", panel.width() == tp::ui::kDefaultWidth && panel.height() == tp::ui::kDefaultHeight ? 1 : 0, 1);
    P.eq("open.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);

    // ---- HINT ----------------------------------------------------------------------------------------------------------
    P.eq("hint.names_key", has(panel.transcript(), "KEY") && has(panel.transcript(), "API key") ? 1 : 0, 1);

    // ---- KEY -----------------------------------------------------------------------------------------------------------
    {
        clickInput(host, panel);
        type(host, "hello claude");
        host.keys("return");
        host.settle();
        P.eq("key.send_opens_dialog", panel.keyDialogOpen() ? 1 : 0, 1);
        P.eq("key.message_kept", panel.chatInput().text() == "hello claude" ? 1 : 0, 1);
        P.eq("key.nothing_sent", static_cast<int64_t>(script->paths.size()), 0);
        host.keys("escape");
        host.settle();
        P.eq("key.escape_closes", panel.keyDialogOpen() ? 1 : 0, 0);
        P.eq("key.message_still_kept", panel.chatInput().text() == "hello claude" ? 1 : 0, 1);

        clickInput(host, panel);
        host.keys("return");                            // the message waits again, for a key
        host.settle();
        P.eq("key.return_opens_again", panel.keyDialogOpen() ? 1 : 0, 1);
        P.eq("key.no_remove_without_key", panel.centreOfKeyRemove().has_value() ? 1 : 0, 0);
        // Modal: a click on RUN runs nothing; keys go to the field.
        const uint64_t runs = processor.editorPort().statusRevision();
        const std::string programText = panel.editor().text();
        clickAt(host, panel.centreOfRun());
        type(host, "x");
        processor.settle();
        host.settle();
        P.eq("key.modal_pointer", processor.editorPort().statusRevision() == runs && !processor.editorPort().compiling() ? 1 : 0, 1);
        P.eq("key.modal_keys", panel.editor().text() == programText ? 1 : 0, 1);
        clickAt(host, panel.centreOfKeySave());
        host.settle();
        P.eq("key.refused_stays_open", panel.keyDialogOpen() ? 1 : 0, 1);
        P.eq("key.refused_says_why", panel.keyDialogWhy().empty() ? 0 : 1, 1);
        P.eq("key.refused_not_stored", chat(processor).hasKey ? 1 : 0, 0);

        host.setClipboard(key + "\n");
        host.keys("cmd+a,cmd+v");
        host.settle();
        P.eq("key.pasted_clears_why", panel.keyDialogWhy().empty() ? 1 : 0, 1);
        P.eq("key.a11y_never_the_key", a11yHoldsKey(host, key) ? 1 : 0, 0);
        P.eq("key.transcript_never_the_key", has(panel.transcript(), key.substr(7, 12)) ? 1 : 0, 0);
        writePng(P, host, "gui.chat.key.png");

        script->add(fake::Response::ok(fake::Sse().start().text("Hello! What shall we build?").end()));
        clickAt(host, panel.centreOfKeySave());
        settleChat(processor, host);
        P.eq("key.saved_closes", panel.keyDialogOpen() ? 1 : 0, 0);
        P.eq("key.stored", chat(processor).hasKey && tp::chatstore::loadKey().value_or("") == key ? 1 : 0, 1);
        P.eq("key.waiting_message_sent", script->postCount(), 1);
        P.eq("key.box_emptied", panel.chatInput().text().empty() ? 1 : 0, 1);
        P.eq("key.answer_shown", has(panel.transcript(), "YOU\nhello claude\n") && has(panel.transcript(), "CLAUDE\nHello!") ? 1 : 0, 1);

        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("key.button_opens", panel.keyDialogOpen() ? 1 : 0, 1);
        const auto remove = panel.centreOfKeyRemove();
        P.eq("key.remove_shown", remove.has_value() ? 1 : 0, 1);
        if (remove)
            clickAt(host, *remove);
        host.tick(2);
        host.settle();
        P.eq("key.removed", chat(processor).hasKey || tp::chatstore::hasKey() ? 1 : 0, 0);
        P.eq("key.remove_closes", panel.keyDialogOpen() ? 1 : 0, 0);
        clickAt(host, panel.centreOfKey());
        host.setClipboard(key);
        host.keys("cmd+v,return");
        host.tick(2);
        host.settle();
        P.eq("key.return_saves", chat(processor).hasKey && !panel.keyDialogOpen() ? 1 : 0, 1);
    }

    // ---- SEND ----------------------------------------------------------------------------------------------------------
    const std::string program = levelProgram();
    {
        const std::string before = processor.editorPort().draft().files[0].text;
        script->add(fake::Response::ok(fake::Sse()
                                           .start("claude-opus-5-5", 5200, 4800, 300)
                                           .thinking("A level control: one dial, from 0 to 2.", "sig-1")
                                           .text("Here it is. The dial scales both channels:\n```\nleft[i] *= level[i];\n```\n")
                                           .tool("toolu_1", "write_file", input({ { "name", "program.fm" },
                                                                                  { "content", juce::String(program) } }))
                                           .tool("toolu_2", "write_file", input({ { "name", "notes.fm" },
                                                                                  { "content", "// ideas for later\n" } }))
                                           .tool("toolu_3", "run_program", input({}))
                                           .end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().tool("toolu_4", "set_dial", input({ { "name", "level" }, { "value", 1.5 } }))
                                           .end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 6100, 5900, 0).text("Done: the level dial is at 1.5.").end()));
        clickInput(host, panel);
        type(host, "a level");
        host.keys("shift+return");
        type(host, "control");
        P.eq("send.shift_return_breaks", panel.chatInput().text() == "a level\ncontrol" ? 1 : 0, 1);
        host.keys("return");
        P.eq("send.box_emptied", panel.chatInput().text().empty() ? 1 : 0, 1);
        settleChat(processor, host);
        const std::string t = panel.transcript();
        P.eq("send.you", has(t, "YOU\na level\ncontrol\n") ? 1 : 0, 1);
        P.eq("send.thinking", has(t, "  ~ A level control: one dial") ? 1 : 0, 1);
        P.eq("send.tools", has(t, "  > wrote program.fm") && has(t, "  > ran it") && has(t, "  > set level to 1.5") ? 1 : 0, 1);
        P.eq("send.answer", has(t, "Done: the level dial is at 1.5.") ? 1 : 0, 1);
        P.eq("send.code_fence", has(t, "```\nleft[i] *= level[i];\n```") ? 1 : 0, 1);
        P.eq("send.usage", has(panel.chatActivity(), "LAST REPLY") ? 1 : 0, 1);
        P.eq("send.tabs", panel.fileCount() == 2 && panel.fileName(1) == "notes.fm" ? 1 : 0, 1);
        P.eq("send.plays", processor.editorPort().running() == processor.editorPort().draft().files ? 1 : 0, 1);
        P.near("send.dial", processor.getParameters()[0]->getValue(), 0.75, 1e-4);
        P.eq("send.dial_shown", panel.centreOfDial(0).has_value() && !panel.centreOfDial(1).has_value() ? 1 : 0, 1);
        std::printf("transcript:\n%s\n", t.c_str());
        writePng(P, host, "gui.chat.png");

        clickAt(host, panel.centreOfTab(0));
        host.settle();
        P.eq("send.editor_has_program", panel.editor().text() == program ? 1 : 0, 1);
        const funkgui::Rect eb = panel.editor().bounds();
        host.click(eb.centreX(), eb.centreY());
        host.keys("cmd+z");
        host.settle();
        P.eq("send.undo_one_step", panel.editor().text() == before ? 1 : 0, 1);
        host.keys("cmd+shift+z");
        host.settle();
        P.eq("send.redo", panel.editor().text() == program ? 1 : 0, 1);
    }

    // ---- PAPER ---------------------------------------------------------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        tp::ui::Panel paper(processor.ports(), processor.editorPort());
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        paperHost.settle();
        P.eq("paper.shows_conversation", paper.transcript() == panel.transcript() ? 1 : 0, 1);
        writePng(P, paperHost, "gui.chat.paper.png");
        funkgui::UiPreferences::get().setTheme(0);
    }

    // ---- MODEL ---------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfModel());
        const funkgui::MenuRequest* menu = host.pendingMenu();
        P.eq("model.menu", menu != nullptr && menu->items.size() == 3 ? 1 : 0, 1);
        int ticked = 0;
        bool opusTicked = false;
        if (menu != nullptr)
            for (const funkgui::MenuItem& m : menu->items)
            {
                ticked += m.checked ? 1 : 0;
                opusTicked = opusTicked || (m.checked && m.label == "Claude Opus 5.5");
            }
        P.eq("model.chosen_ticked", ticked == 1 && opusTicked ? 1 : 0, 1);
        P.eq("model.five_levels", panel.effortCount(), 5);
        host.chooseMenuItem("Claude Haiku 5.5");
        host.tick(2);
        host.settle();
        P.eq("model.changed", chat(processor).model == "claude-haiku-5-5" ? 1 : 0, 1);
        P.eq("model.levels_follow", panel.effortCount(), 3);
        P.eq("model.sixth_level_none", panel.centreOfEffort(3).has_value() ? 1 : 0, 0);
        clickAt(host, *panel.centreOfEffort(0));
        host.tick(2);
        host.settle();
        P.eq("model.level_chosen", chat(processor).effort == "low" ? 1 : 0, 1);
        clickAt(host, panel.centreOfModel());
        host.chooseMenuItem("Claude Opus 5.5");
        host.tick(2);
        host.settle();
        clickAt(host, *panel.centreOfEffort(1));
        host.tick(2);
        host.settle();
        P.eq("model.back", chat(processor).model == "claude-opus-5-5" && chat(processor).effort == "medium" ? 1 : 0, 1);
    }

    // ---- STOP ----------------------------------------------------------------------------------------------------------
    {
        fake::Response stall = fake::Response::ok(fake::Sse().start().thinking("Considering", "sig-s"));
        stall.stall = true;
        script->add(stall);
        clickInput(host, panel);
        type(host, "go");
        host.keys("return");
        const auto t0 = std::chrono::steady_clock::now();
        while (script->streaming.load() == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        host.tick(4);
        P.eq("stop.activity", has(panel.chatActivity(), "THINKING") ? 1 : 0, 1);
        std::printf("activity: %s\n", panel.chatActivity().c_str());
        clickAt(host, panel.centreOfSend());
        settleChat(processor, host);
        P.eq("stop.ended", chat(processor).busy ? 1 : 0, 0);
        P.eq("stop.note", has(panel.transcript(), "- Stopped.") ? 1 : 0, 1);
    }

    // ---- NEW -----------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfNewChat());
        P.eq("new.asks", host.pendingMenu() != nullptr ? 1 : 0, 1);
        host.chooseMenuItem(1);
        host.tick(2);
        host.settle();
        P.eq("new.forgotten", chat(processor).entries.empty() ? 1 : 0, 1);
        P.eq("new.hint_back", has(panel.transcript(), "YOU") ? 0 : 1, 1);
        clickInput(host, panel);
        host.keys("tab");
        type(host, "z");
        P.eq("new.tab_leaves_box", panel.chatInput().text().empty() && !panel.chatInput().focused() ? 1 : 0, 1);
    }

    // ---- RESIZE --------------------------------------------------------------------------------------------------------
    {
        host.resize(500, 300);
        host.settle();
        P.eq("resize.least_w", panel.width(), tp::ui::kMinWidth);
        P.eq("resize.least_h", panel.height(), tp::ui::kMinHeight);
        P.eq("resize.least_inside", allInside(host, panel) ? 1 : 0, 1);
        const float smallEditor = panel.editor().bounds().h;
        const float smallChat = panel.chatInput().bounds().y;
        P.ge("resize.least_chat_right", static_cast<double>(panel.chatInput().bounds().x),
             static_cast<double>(panel.editor().bounds().right()));
        P.eq("resize.least_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
        writePng(P, host, "gui.chat.small.png");
        host.resize(1900, 1100);
        host.settle();
        P.eq("resize.large", panel.width() == 1900 && panel.height() == 1100 ? 1 : 0, 1);
        P.eq("resize.large_inside", allInside(host, panel) ? 1 : 0, 1);
        P.near("resize.editor_grows", static_cast<double>(panel.editor().bounds().h - smallEditor),
               1100.0 - static_cast<double>(tp::ui::kMinHeight), 0.5);
        P.near("resize.chat_follows", static_cast<double>(panel.chatInput().bounds().y - smallChat),
               1100.0 - static_cast<double>(tp::ui::kMinHeight), 0.5);
        writePng(P, host, "gui.chat.large.png");
        host.resize(tp::ui::kDefaultWidth, tp::ui::kDefaultHeight);
        host.settle();

        processor.setEditorSize(1500, 900);
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        tp::Processor other;
        other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("resize.session_keeps_size", other.editorWidth() == 1500 && other.editorHeight() == 900 ? 1 : 0, 1);
        processor.setEditorSize(-3, 70000);
        P.eq("resize.nonsense_size_dropped", processor.editorWidth() == 0 && processor.editorHeight() == 0 ? 1 : 0, 1);
    }

    tp::chatstore::setDirectoryForTests(juce::File());
    keys.deleteRecursively();
    return P.finish();
}
