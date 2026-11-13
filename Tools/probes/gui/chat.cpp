// PROBE name=gui.chat timeout=240
//
// gui.chat: the editor's chat column, its key dialog and the resizable panel, headless (funkgui::HeadlessHost), over a
// real processor whose chat talks to a scripted API (../plugin/FakeClaude.h): nothing reaches the network, and the key
// folder is the probe's own. It writes, to <build>/probe-out/:
//   gui.chat.empty.png    the empty chat: its title, the API key button, the suggestions
//   gui.chat.png          a conversation: a bubble, Claude's thinking (opened), its answer with a code block, the card
//                         of what it did, the message box grown to three lines
//   gui.chat.key.png      the key dialog with a key pasted (masked)
//   gui.chat.stored.png   the key dialog with a key stored: its line, Remove key
//   gui.chat.markdown.png an answer in markdown: a heading, bold, inline code, numbered and bulleted lists, a quote,
//                         a rule
//   gui.chat.paper.png    the conversation in PAPER
//   gui.chat.small.png    the panel at its least size
//   gui.chat.large.png    the panel large
//   gui.chat.folded.png   the chat folded into its rail, an answer waiting (the dot)
//   gui.chat.ask.png      Claude's question of two parts: one choice (a ring) and several (boxes), some picked, and
//                         Send answers
//   gui.chat.answered.png the question answered: the choices marked, the rest muted
//   gui.chat.pictures.png two pictures in the message box, each with its remove button
//   gui.chat.six.png      six pictures in the message box at the least size, on two rows
//   gui.chat.sent.png     a message sent with two pictures: their thumbnails above its bubble
//   gui.chat.plan.png     a planning model's plan as a card above the builder's answer
//   gui.chat.checks.png   Claude's checks card, every check passing, and the picture of the sound it looked at
//   gui.chat.checks-fail.png  a check that fails, after the turn went on twice
//   gui.chat.view_sound.png   the picture view_sound gave Claude there (an instrument's saw at A2; an effect's sweep)
//   gui.chat.chats.png    the header's chats pill naming the chat shown, an older chat opened from its list
//   gui.chat.rename.png   the rename: the key dialog's box with the chat's name in its field
//   (the list itself, its menu: gui.menus.chats.png)
// and checks:
//   EMPTY    with no key the empty chat has a key button (which opens the key dialog), and no other way in (the
//            panel speaks of no sign-in), and suggestions
//   KEY      a message sent with no key opens the key dialog and stays in the message box; Escape closes the dialog
//            (the message stays, unsent); sent again, it waits for the key; while the dialog is open the pointer and
//            the keys reach nothing else; a key that is not one is refused (the dialog says why, to a screen reader
//            too; nothing is stored);
//            Cmd-V pastes a key, drawn masked (its public prefix, stars, its last four); nothing the panel speaks
//            holds the key; SAVE stores it, closes the dialog and sends
//            the waiting message to api.anthropic.com with the key; the header has no account, the line under the box
//            no credit; KEY opens the dialog; REMOVE (shown once a key is stored) forgets it; Return in the dialog
//            saves one
//   SEND     Shift-Return breaks the line in the message box and the box grows with its rows, Return sends it (the box
//            empties and shrinks); the conversation shows the turn (YOU, CLAUDE, the thinking, what Claude did, its
//            answer) and the line under the box the usage; the thinking row opens and closes; a code block's Copy puts
//            its code on the clipboard; the files Claude wrote are the editor's tabs and play; Cmd-Z in the program's
//            editor gives back the text it had before Claude wrote it, as one step
//   PLANNER  the model menu's PLAN WITH: a planning model chosen, the pill says both ("Opus 5.5 > Sonnet 5.5"); a
//            message: the plan as a card (a screen reader's toggle) above the builder's answer; No planner again
//   MODEL    the model's pill opens a menu (BUILD WITH: the models, the chosen one ticked; PLAN WITH: No planner,
//            ticked, and the models); choosing another changes it; the
//            thinking pill's menu has that model's levels, and choosing one sets it; fast mode's pill, after it on
//            Claude Opus 5.5, switches fast mode on and off (a switch to a screen reader, on or off), and is gone
//            on a model that takes no fast mode
//   WEB      web search's pill, after fast mode's: a switch (to a screen reader too), on and off
//   STOP     while Claude works the line under the box says what it is doing and send is stop: a click ends the turn;
//            a double-click on send sends once and stops nothing (its second press is the same click's), the turn
//            running to its answer; a click on send leaves the caret in the message box, so Space and Return typed
//            while Claude works go there and stop nothing; one press on stop ends the turn at once
//   ASK      Claude's question (ask_user) as a card: the turn waits and the message box says so; each option a button
//            (to a screen reader a radio button, or a toggle for several); a click marks it (another of one choice
//            replaces it, several stay); Send answers waits until each question has a pick, then sends the labels and
//            the turn goes on; the card then has no buttons and the transcript the answers; a question of one choice
//            has no Send answers, and its click answers
//   PICTURES the message box's picture button asks the host for files (several, pictures); the files chosen show as
//            thumbnails in the box (drawn, none missing) and their remove buttons take one back; pictures dropped on
//            the chat's card come too (the card says so while pictures are dragged over the editor, not audio files),
//            not on the editor, and a text file is no drop the chat takes; a picture alone may be sent with the
//            send button; six at the least size wrap to a second row, each drawn and removable; sent, the pictures
//            go before the words and show above the message; FunkGui holds each thumbnail while the chat does,
//            and a new chat lets them go
//   CHECKS   Claude's checks (set_checks) as a card, a row each (its label, its value against the goal), and the
//            picture of the sound it looked at (view_sound) under its row, drawn; one fails, the turn goes on, a dial
//            set and a run fix it: the card brought up to date (every check passing); a check that cannot pass shows
//            failing; the card's Clear (spoken as a button) forgets the checks and goes
//   NEW      NEW asks nothing and forgets nothing: a new, empty chat, the one before kept in the chat list; a suggestion
//            goes into the message box, unsent; Tab leaves the message box
//   CHATS    the header's pill names the chat shown (a combo box to a screen reader); its menu lists the chats, the
//            newest first, the one shown dotted, then New chat, Rename and Delete; an older one chosen shows (its words
//            back); Rename opens the dialog with its name, selected: typed over and Return, the chat has that name (the
//            pill says it); a click in the name drops the selection and places the caret; Escape
//            cancels; Delete asks first, then the chat is gone and the newest other one shows;
//            another chat shown is another conversation to the view (ChatState::conversation)
//   SCROLL   a conversation taller than the view scrolls with the wheel, and the round button jumps back to its end; as
//            the code scrolls (editor/Scroll.h): a notch three body lines, reversed set (a built WheelEvent: HeadlessHost
//            cannot set it) the same way, its thumb dragged in proportion; the message box keeps its keys, Shift-Tab
//            reaches the conversation, where Page Up, Home and End scroll; a screen reader has every block, and its
//            focus on one out of view scrolls it in
//   VIEW     the conversation alone, over a made-up state: the user's words as typed (no markdown); a Copy copies the
//            entry's own UTF-8; inline code in small text runs over no word; a tool row is drawn failed by its outcome,
//            not its words; an answered question marks the option chosen alone; a thinking row opened stays open while
//            the conversation is the same, and another one's starts closed
//   COLLAPSE the header's chevron folds the chat into a rail (the processor keeps it, its state too): the editor takes
//            the room, the chat's items go (the message box among them); an answer while folded lights the rail's dot;
//            a Panel made while it is folded asks for no frame (but the safety net's) and no layout while idle (two
//            seconds at the frame pump's cadence); the rail's button opens the chat again, the dot gone, the answer in it, the editor as
//            wide as before, the message box at its height at once
//   RESIZE   a size below the least is raised to it; at the least size (with sixteen files, the most, their tabs narrowed) and at
//            a large one every item lies inside the panel, the chat's column right of the editor; the editor and the conversation grow with the height; the
//            processor keeps the editor's size in the session; at the least size the message box's bar (Claude Opus
//            5.5: fast mode's and web search's pills shown) has no item over another or past it, none under send, and
//            a click on stop's centre stops the turn and leaves web search as it was
//   DRAW     an answer that lands with no input asks for a frame (Panel.h "Unchanged frames"), and the conversation
//            settled, the panel asks for none
#include "Cadence.h"
#include "Probe.h"

#include "../plugin/FakeClaude.h"

#include "editor/ChatItems.h"
#include "editor/ChatView.h"
#include "editor/CodeAssist.h"
#include "editor/Panel.h"
#include "plugin/ChatAgent.h"
#include "plugin/ChatStore.h"
#include "plugin/EditorPort.h"
#include "plugin/ClaudeApi.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/canvas/Images.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <juce_graphics/juce_graphics.h>

namespace pg = playground;

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

    void clickInput(funkgui::HeadlessHost& host, pg::ui::Panel& panel)
    {
        const funkgui::Rect r = panel.chatInput().bounds();
        host.click(r.centreX(), r.centreY());
    }

    // The turn finished, its compile and dials taken, the panel caught up.
    void settleChat(pg::Processor& p, funkgui::HeadlessHost& host)
    {
        p.chatAgent().waitIdle();
        p.settle();
        host.tick(4);
        host.settle();
    }

    pg::ChatState chat(pg::Processor& p) { return p.chatAgent().chatState(); }

    // The editor's check while typing (CodeAssist) landed: a check is due once the files change and is sent once they
    // have rested (0.6 s of the host's clock: CodeAssist::settled() is false until then), then answers on the
    // compiler's thread whenever that thread has a core; its outcome takes the status line's Checking... away, a frame
    // of its own at a moment of the real clock, not idleness (gui.switch RATE, gui.dump DRAW). So this ticks until no
    // check is due or running and no compile runs, and a row that counts an idle Panel's frames begins once it is true.
    // settled() also waits for the hover card's delay while the pointer rests over the editor and for the caret's
    // occurrences after the editor takes the keys: each a fixed span of the host's clock, which these ticks pass.
    bool checkLanded(pg::ui::Panel& panel, funkgui::HeadlessHost& host, pg::EditorPort& port)
    {
        const pg::ui::CodeAssist& code = panel.codeAssist();
        const auto busy = [&] { return !code.settled() || code.checking() || port.compiling(); };
        for (const auto by = std::chrono::steady_clock::now() + probe::kWorkWait;
             busy() && std::chrono::steady_clock::now() < by;)
        {
            host.tick(1, 1.0f / 12.0f);                  // a calm editor's cadence
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return !busy();
    }

    // The script's answers no request took (a row that failed left them): dropped, so the next turns get their own.
    void dropUnplayed(fake::Script& script)
    {
        const std::lock_guard<std::mutex> lock(script.mutex);
        script.posts.clear();
    }

    int count(const std::string& s, const std::string& part)
    {
        int n = 0;
        for (std::size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + part.size()))
            ++n;
        return n;
    }

    // The message box's bar: every item shown in it lies inside it, and none over another (so none under send).
    bool barFits(const funkgui::HeadlessHost& host, const pg::ui::Panel& panel)
    {
        const funkgui::Rect bar = panel.chatInputItem().bar();
        std::vector<funkgui::A11yItem> items;
        for (const funkgui::A11yItem& a : host.accessibility())
            for (const char* t : { "Add pictures", "Model", "Thinking level", "Fast mode", "Web search", "Send or stop" })
                if (a.title == t)
                    items.push_back(a);
        bool ok = items.size() >= 3;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const funkgui::Rect& a = items[i].bounds;
            std::printf("bar: %s at %.1f to %.1f (the bar %.1f to %.1f)\n", items[i].title.c_str(), static_cast<double>(a.x),
                        static_cast<double>(a.x + a.w), static_cast<double>(bar.x), static_cast<double>(bar.x + bar.w));
            if (a.w <= 0.0f || a.x < bar.x - 0.5f || a.x + a.w > bar.x + bar.w + 0.5f || a.y < bar.y - 0.5f || a.y + a.h > bar.y + bar.h + 0.5f)
                ok = false;
            for (std::size_t j = i + 1; j < items.size(); ++j)
            {
                const funkgui::Rect& b = items[j].bounds;
                if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h)
                    ok = false;
            }
        }
        return ok;
    }

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

    // The item the panel speaks of by this title, if it does.
    std::optional<funkgui::A11yItem> a11yTitled(const funkgui::HeadlessHost& host, const std::string& title)
    {
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.title == title)
                return item;
        return std::nullopt;
    }

    // A switch the panel speaks of, on or off.
    bool a11ySwitch(const funkgui::HeadlessHost& host, const std::string& title, bool on)
    {
        const auto item = a11yTitled(host, title);
        return item && item->role == funkgui::A11yRole::toggleButton && item->checkable && item->checked == on;
    }

    // Every item the panel speaks of lies inside it.
    bool allInside(const funkgui::HeadlessHost& host, const pg::ui::Panel& panel)
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
        if constexpr (pg::product::kIsInstrument)
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
        return pg::claude::object(fields);
    }

    // A program that sounds, with a dial to fix: an instrument's sine with its level (dB) and a release (s); an effect's
    // gain (dB).
    std::string soundProgram()
    {
        if constexpr (pg::product::kIsInstrument)
            return R"(module Voice
{
    param float level [[fminus::range(-40, 0)]] [[fminus::default(-20)]];
    param float release [[fminus::range(0.01, 3)]] [[fminus::default(0.5)]];
    float rate, ph, step, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 1; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float g = expf(0.115129255f * level[0]);
        const float fall = expf(-6.90775528f / (release[0] * rate));
        for (int i = 0; i < n; ++i)
        {
            if (!held) env *= fall;
            const float out = g * env * (2 * ph - 1);
            ph += step;
            if (ph >= 1) ph -= 1;
            left[i] += out;
            right[i] += out;
        }
        return held || env > 0.00001f;
    }
};
)";
        else
            return R"(module Effect
{
    param float gain [[fminus::range(-24, 24)]] [[fminus::default(0)]];
    void process(float* restrict left, float* restrict right, int n)
    {
        const float g = expf(0.115129255f * gain[0]);
        for (int i = 0; i < n; ++i)
        {
            left[i] = tanhf(g * left[i]);
            right[i] = tanhf(g * right[i]);
        }
    }
};
)";
    }
}

PROBE(gui, chat)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File keys = juce::File(juce::String(probe::outPath("gui-chat-keys")));
    keys.deleteRecursively();
    pg::chatstore::setDirectoryForTests(keys);
    const std::string key = fake::fakeKey();

    pg::Processor processor;
    auto script = std::make_shared<fake::Script>();
    script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "medium", "high", "xhigh", "max" } },
                                        { "claude-sonnet-5-5", "Claude Sonnet 5.5", true, { "low", "medium", "high", "xhigh" } },
                                        { "claude-haiku-5-5", "Claude Haiku 5.5", true, { "low", "medium", "high" } } });
    processor.chatAgent().setTransport(std::make_unique<fake::Transport>(script));

    pg::ui::Panel panel(processor.ports(), processor.editorPort());
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    P.le("open.settle_frames", host.settle(), 599.0);
    P.eq("open.default_size", panel.width() == pg::ui::kDefaultWidth && panel.height() == pg::ui::kDefaultHeight ? 1 : 0, 1);
    P.eq("open.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);

    // ---- EMPTY ---------------------------------------------------------------------------------------------------------
    {
        const auto addKey = panel.chatView().centreOfAddKey();
        P.eq("empty.key_button", addKey.has_value() ? 1 : 0, 1);
        P.eq("empty.suggestions", panel.chatView().centreOfSuggestion(3).has_value() ? 1 : 0, 1);
        bool other = false;
        for (const funkgui::A11yItem& a : host.accessibility())
            for (const std::string* t : { &a.title, &a.description, &a.value })
                other = other || has(*t, "ign in") || has(*t, "ccount");
        P.eq("empty.key_only", other ? 1 : 0, 0);
        writePng(P, host, "gui.chat.empty.png");
        if (addKey)
            clickAt(host, *addKey);
        host.settle();
        P.eq("empty.key_button_opens_dialog", panel.keyDialogOpen() ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
    }

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
        P.eq("key.refused_announced", !host.log.announcements.empty() && host.log.announcements.back() == panel.keyDialogWhy() ? 1 : 0, 1);
        const auto keyField = a11yTitled(host, "Key");
        P.eq("key.refused_on_its_field", keyField && keyField->help == panel.keyDialogWhy() ? 1 : 0, 1);

        host.setClipboard(key + "\n");
        host.keys("cmd+a,cmd+v");
        host.settle();
        P.eq("key.pasted_clears_why", panel.keyDialogWhy().empty() ? 1 : 0, 1);
        P.eq("key.a11y_never_the_key", a11yHoldsKey(host, key) ? 1 : 0, 0);
        {
            // Drawn masked: Anthropic's public prefix, stars, the last four (of what was pasted: its line break a
            // space, trimmed when it is saved); nothing else of it.
            const std::string pasted = key + " ";
            const std::string shown = panel.keyDialogShown();
            std::string clear;
            for (const char c : shown)
                if (c != '*')
                    clear += c;
            P.eq("key.masked", clear == "sk-ant-" + pasted.substr(pasted.size() - 4) && shown.rfind("sk-ant-", 0) == 0 ? 1 : 0, 1);
        }
        writePng(P, host, "gui.chat.key.png");

        script->add(fake::Response::ok(fake::Sse().start().text("Hello! What shall we build?").end()));
        clickAt(host, panel.centreOfKeySave());
        settleChat(processor, host);
        P.eq("key.saved_closes", panel.keyDialogOpen() ? 1 : 0, 0);
        P.eq("key.stored", chat(processor).hasKey && pg::chatstore::loadKey().value_or("") == key ? 1 : 0, 1);
        P.eq("key.waiting_message_sent", script->postCount(), 1);
        P.eq("key.sent_with_key", script->lastHost("POST /v1/messages") == pg::claude::kHost
                                      && script->lastHeader("POST /v1/messages", "x-api-key") == key
                                      && !script->lastHas("POST /v1/messages", "authorization") ? 1 : 0, 1);
        P.eq("key.box_emptied", panel.chatInput().text().empty() ? 1 : 0, 1);
        P.eq("key.answer_shown", has(panel.transcript(), "YOU\nhello claude\n") && has(panel.transcript(), "CLAUDE\nHello!") ? 1 : 0, 1);
        P.eq("key.no_credit", has(panel.chatActivity(), "credit") ? 1 : 0, 0);
        bool account = false;
        for (const funkgui::A11yItem& a : host.accessibility())
            account = account || has(a.title, "ccount") || has(a.title, "ign in");
        P.eq("key.no_account", account ? 1 : 0, 0);

        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("key.button_opens", panel.keyDialogOpen() ? 1 : 0, 1);
        const auto remove = panel.centreOfKeyRemove();
        P.eq("key.remove_shown", remove.has_value() ? 1 : 0, 1);
        P.eq("key.a11y_stored_never_the_key", a11yHoldsKey(host, key) ? 1 : 0, 0);
        writePng(P, host, "gui.chat.stored.png");
        if (remove)
            clickAt(host, *remove);
        host.tick(2);
        host.settle();
        P.eq("key.removed", chat(processor).hasKey || pg::chatstore::hasKey() ? 1 : 0, 0);
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
        const float oneRow = panel.chatInput().bounds().h;
        type(host, "a level");
        host.keys("shift+return");
        type(host, "control");
        host.keys("shift+return");
        type(host, "please");
        host.settle();
        P.eq("send.shift_return_breaks", panel.chatInput().text() == "a level\ncontrol\nplease" ? 1 : 0, 1);
        P.near("send.box_grows", static_cast<double>(panel.chatInput().bounds().h - oneRow),
               2.0 * static_cast<double>(panel.chatInput().rowHeight()), 0.5);
        host.keys("backspace,backspace,backspace,backspace,backspace,backspace,backspace");   // "\nplease"
        host.keys("return");
        host.settle();
        P.eq("send.box_emptied", panel.chatInput().text().empty() ? 1 : 0, 1);
        P.near("send.box_shrinks", static_cast<double>(panel.chatInput().bounds().h), static_cast<double>(oneRow), 0.5);
        settleChat(processor, host);
        const std::string t = panel.transcript();
        P.eq("send.you", has(t, "YOU\na level\ncontrol\n") ? 1 : 0, 1);
        P.eq("send.thinking", has(t, "  ~ A level control: one dial") ? 1 : 0, 1);
        P.eq("send.tools", has(t, "  > wrote program.fm") && has(t, "  > ran it") && has(t, "  > set level to 1.5") ? 1 : 0, 1);
        P.eq("send.answer", has(t, "Done: the level dial is at 1.5.") ? 1 : 0, 1);
        P.eq("send.code_fence", has(t, "```\nleft[i] *= level[i];\n```") ? 1 : 0, 1);
        P.eq("send.usage", has(panel.chatActivity(), "Last reply") ? 1 : 0, 1);
        P.eq("send.tabs", panel.fileCount() == 2 && panel.fileName(1) == "notes.fm" ? 1 : 0, 1);
        P.eq("send.plays", processor.editorPort().running() == processor.editorPort().draft().files ? 1 : 0, 1);
        P.near("send.dial", processor.getParameters()[0]->getValue(), 0.75, 1e-4);
        P.eq("send.dial_shown", panel.centreOfDial(0).has_value() && !panel.centreOfDial(1).has_value() ? 1 : 0, 1);
        std::printf("transcript:\n%s\n", t.c_str());
        pg::ui::ChatView& view = panel.chatView();
        view.scrollTo(0.0f);
        host.settle();
        const auto think = view.centreOfThinking(0);
        P.eq("send.thinking_row", think.has_value() ? 1 : 0, 1);
        if (think)
            clickAt(host, *think);
        host.settle();
        P.eq("send.thinking_opens", view.thinkingOpen(0) ? 1 : 0, 1);
        P.eq("send.code_copy_shown", view.copies() >= 1 ? 1 : 0, 1);
        if (const auto copy = view.centreOfCopy(0))
            clickAt(host, *copy);
        else
        {
            view.scrollTo(view.contentHeight());
            host.settle();
            for (int k = 0; k < 40 && !view.centreOfCopy(0); ++k)
                view.scrollTo(view.scroll() - 30.0f);
            if (const auto again = view.centreOfCopy(0))
                clickAt(host, *again);
        }
        P.eq("send.code_copied", host.log.lastCopy == "left[i] *= level[i];\n" ? 1 : 0, 1);
        view.scrollTo(0.0f);
        host.settle();
        if (const auto answer = view.centreOfAnswerCopy(0))
            clickAt(host, *answer);
        P.eq("send.answer_copied", host.log.lastCopy == "Hello! What shall we build?" ? 1 : 0, 1);
        clickInput(host, panel);
        type(host, "more");
        host.keys("shift+return");
        type(host, "and more");
        host.keys("shift+return");
        type(host, "and more still");
        view.scrollTo(0.0f);
        host.settle();
        writePng(P, host, "gui.chat.png");
        host.keys("cmd+a,backspace");
        host.settle();
        if (think)
            clickAt(host, *think);
        host.settle();
        P.eq("send.thinking_closes", view.thinkingOpen(0) ? 1 : 0, 0);

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
        pg::ui::Panel paper(processor.ports(), processor.editorPort());
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
        // BUILD WITH and its three models, a rule, PLAN WITH, No planner and the three again.
        P.eq("model.menu", menu != nullptr && menu->items.size() == 10 ? 1 : 0, 1);
        int ticked = 0;
        bool opusTicked = false, noPlanner = false;
        if (menu != nullptr)
            for (const funkgui::MenuItem& m : menu->items)
            {
                ticked += m.checked ? 1 : 0;
                opusTicked = opusTicked || (m.checked && m.label == "Claude Opus 5.5");
                noPlanner = noPlanner || (m.checked && m.label == "No planner");
            }
        P.eq("model.chosen_ticked", ticked == 2 && opusTicked && noPlanner ? 1 : 0, 1);
        host.chooseMenuItem("Claude Haiku 5.5");
        host.tick(2);
        host.settle();
        P.eq("model.changed", chat(processor).model == "claude-haiku-5-5" ? 1 : 0, 1);
        clickAt(host, panel.centreOfThinking());
        const funkgui::MenuRequest* levels = host.pendingMenu();
        P.eq("model.levels_follow", levels != nullptr && levels->items.size() == 3 ? 1 : 0, 1);
        host.chooseMenuItem("Low");
        host.tick(2);
        host.settle();
        P.eq("model.level_chosen", chat(processor).effort == "low" ? 1 : 0, 1);
        clickAt(host, panel.centreOfModel());
        host.chooseMenuItem("Claude Opus 5.5");
        host.tick(2);
        host.settle();
        clickAt(host, panel.centreOfThinking());
        P.eq("model.five_levels", host.pendingMenu() != nullptr && host.pendingMenu()->items.size() == 5 ? 1 : 0, 1);
        host.chooseMenuItem("Medium (the default)");
        host.tick(2);
        host.settle();
        P.eq("model.back", chat(processor).model == "claude-opus-5-5" && chat(processor).effort == "medium" ? 1 : 0, 1);

        // Fast mode's pill: on Claude Opus 5.5, after the thinking one, a switch; gone on a model that takes none.
        const auto fast = panel.centreOfFast();
        P.eq("fast.pill_shown", fast.has_value() && fast->x > panel.centreOfThinking().x ? 1 : 0, 1);
        if (fast)
            clickAt(host, *fast);
        host.tick(2);
        host.settle();
        P.eq("fast.switched_on", chat(processor).fast ? 1 : 0, 1);
        P.eq("fast.a11y_on", a11ySwitch(host, "Fast mode", true) ? 1 : 0, 1);
        if (const auto again = panel.centreOfFast())
            clickAt(host, *again);
        host.tick(2);
        host.settle();
        P.eq("fast.switched_off", chat(processor).fast ? 1 : 0, 0);
        P.eq("fast.a11y_off", a11ySwitch(host, "Fast mode", false) ? 1 : 0, 1);
        clickAt(host, panel.centreOfModel());
        host.chooseMenuItem("Claude Haiku 5.5");
        host.tick(2);
        host.settle();
        P.eq("fast.gone_on_haiku", panel.centreOfFast().has_value() || a11yTitled(host, "Fast mode") ? 1 : 0, 0);
        clickAt(host, panel.centreOfModel());
        host.chooseMenuItem("Claude Opus 5.5");
        host.tick(2);
        host.settle();
        P.eq("fast.back_on_opus", panel.centreOfFast().has_value() && chat(processor).model == "claude-opus-5-5" ? 1 : 0, 1);

        // Web search's pill: after fast mode's, a switch (the key in use).
        const auto web = panel.centreOfWeb();
        const auto fastNow = panel.centreOfFast();
        P.eq("web.pill_shown", web && fastNow && web->x > fastNow->x ? 1 : 0, 1);
        if (web)
            clickAt(host, *web);
        host.tick(2);
        host.settle();
        P.eq("web.switched_on", chat(processor).web ? 1 : 0, 1);
        P.eq("web.a11y_on", a11ySwitch(host, "Web search", true) ? 1 : 0, 1);
        if (const auto again = panel.centreOfWeb())
            clickAt(host, *again);
        host.tick(2);
        host.settle();
        P.eq("web.switched_off", chat(processor).web ? 1 : 0, 0);
    }

    // ---- PLANNER -------------------------------------------------------------------------------------------------------
    {
        // Sonnet builds, Opus plans (the menu's PLAN WITH ids start at 1001, in the models' order).
        clickAt(host, panel.centreOfModel());
        host.chooseMenuItem("Claude Sonnet 5.5");
        host.tick(2);
        host.settle();
        clickAt(host, panel.centreOfModel());
        host.chooseMenuItem(1001);
        host.tick(2);
        host.settle();
        P.eq("planner.chosen", chat(processor).planner == "claude-opus-5-5" && chat(processor).model == "claude-sonnet-5-5" ? 1 : 0, 1);
        const auto pill = a11yTitled(host, "Model");
        std::printf("planner: the pill says \"%s\"\n", pill ? pill->value.c_str() : "");
        P.eq("planner.pill_says_both", pill && has(pill->value, "Opus 5.5 > Sonnet 5.5") ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().text(
            "### Plan\n1. **Oscillators**: two saws, detuned 7 cents (osc.fm).\n2. **Filter**: ladder.fm, its cutoff on "
            "an envelope.\n3. Checks: `centroid_hz` at least 2000, `alerts` at most 0.").end()));
        script->add(fake::Response::ok(fake::Sse().start().text("Built it from the plan.").end()));
        clickInput(host, panel);
        type(host, "a bright lead");
        host.keys("return");
        settleChat(processor, host);
        pg::ui::ChatView& view = panel.chatView();
        view.scrollTo(view.contentHeight());
        host.settle();
        P.eq("planner.card", has(panel.transcript(), "  % ### Plan") && has(panel.transcript(), "Built it from the plan.") ? 1 : 0, 1);
        P.eq("planner.card_a11y", a11yTitled(host, "Show the plan").has_value() ? 1 : 0, 1);
        writePng(P, host, "gui.chat.plan.png");
        // Back to Opus alone, as the rest expects.
        clickAt(host, panel.centreOfModel());
        host.chooseMenuItem("No planner");
        host.tick(2);
        host.settle();
        clickAt(host, panel.centreOfModel());
        host.chooseMenuItem("Claude Opus 5.5");
        host.tick(2);
        host.settle();
        P.eq("planner.off", chat(processor).planner.empty() && chat(processor).model == "claude-opus-5-5" ? 1 : 0, 1);
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
        P.eq("stop.activity", has(panel.chatActivity(), "Thinking") ? 1 : 0, 1);
        std::printf("activity: %s\n", panel.chatActivity().c_str());
        clickAt(host, panel.centreOfSend());
        settleChat(processor, host);
        P.eq("stop.ended", chat(processor).busy ? 1 : 0, 0);
        P.eq("stop.note", has(panel.transcript(), "- Stopped.") ? 1 : 0, 1);

        // A double-click on send: its first press sends (send is stop under the pointer then), its second, the same
        // double-click's, stops nothing: the turn runs to its answer.
        {
            const int posts = script->postCount();
            const int stopped = count(panel.transcript(), "- Stopped.");
            script->add(fake::Response::ok(fake::Sse().start().text("Answered after a double-click.").end()));
            clickInput(host, panel);
            type(host, "twice");
            const funkgui::Point s = panel.centreOfSend();
            host.doubleClick(s.x, s.y);
            settleChat(processor, host);
            std::printf("double-click on send: %d requests, stopped %d times\n", script->postCount() - posts,
                        count(panel.transcript(), "- Stopped.") - stopped);
            P.eq("stop.double_click_sends_once", script->postCount() > posts && has(panel.transcript(), "Answered after a double-click.")
                                                     && count(panel.transcript(), "- Stopped.") == stopped ? 1 : 0, 1);
            dropUnplayed(*script);
        }
        // A click on send leaves the caret in the message box: Space, letters and Return typed while Claude works go
        // there and stop nothing. Then one press on stop ends the turn at once, the caret still in the box.
        {
            script->add(stall);
            const int stopped = count(panel.transcript(), "- Stopped.");
            clickInput(host, panel);
            type(host, "click");
            clickAt(host, panel.centreOfSend());
            const auto t1 = std::chrono::steady_clock::now();
            while (script->streaming.load() == 0 && std::chrono::steady_clock::now() - t1 < std::chrono::seconds(10))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            host.tick(4);
            P.eq("send.click_keeps_caret", panel.chatInput().focused() ? 1 : 0, 1);
            host.keys("space");
            type(host, "ok");
            host.keys("return");
            host.tick(4);
            P.eq("send.keys_stop_nothing", chat(processor).busy && count(panel.transcript(), "- Stopped.") == stopped ? 1 : 0, 1);
            P.eq("send.keys_in_the_box", panel.chatInput().text() == " ok" ? 1 : 0, 1);
            clickAt(host, panel.centreOfSend());
            const bool caret = panel.chatInput().focused();
            if (chat(processor).busy)
                processor.chatAgent().stop();
            settleChat(processor, host);
            dropUnplayed(*script);
            P.eq("stop.one_press_stops", !chat(processor).busy && count(panel.transcript(), "- Stopped.") == stopped + 1 ? 1 : 0, 1);
            P.eq("stop.click_keeps_caret", caret ? 1 : 0, 1);
            panel.chatInput().setText("");
            host.settle();
        }
    }

    // ---- MARKDOWN ------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::ok(fake::Sse().start().text(
            "### What it does\n\nA **two-oscillator** voice: `osc.fm` makes the waves and `filter.fm` shapes them.\n\n"
            "1. The envelope opens the filter.\n2. The LFO sweeps it, a little.\n\n"
            "- **cutoff**: where the filter starts\n- **resonance**: its peak\n  (higher rings more)\n\n"
            "> Keep the level near -14 dB with sixteen voices.\n\n---\n\nDone.").end()));
        clickInput(host, panel);
        type(host, "explain it");
        host.keys("return");
        // The answer lands with no input (the agent's worker): the frame it shows in draws (Panel.h "Unchanged
        // frames"); then the panel is quiet.
        host.tick(1);
        host.draw();
        processor.chatAgent().waitIdle();
        bool answered = false;
        for (int k = 0; k < 60 && !answered; ++k)
        {
            host.tick(1, 1.0f / 12.0f);
            answered = has(panel.transcript(), "Done.");
            if (!answered)
                host.draw();
        }
        P.eq("draw.chat_answer", answered && panel.needsDraw() ? 1 : 0, 1);
        settleChat(processor, host);
        P.eq("draw.chat_check_landed", checkLanded(panel, host, processor.editorPort()) ? 1 : 0, 1);
        host.settle();
        host.draw();
        int asked = 0;
        for (int k = 0; k < 11; ++k)
        {
            host.tick(1, 1.0f / 12.0f);
            if (panel.needsDraw())
            {
                ++asked;
                std::printf("  a quiet chat's frame asked to draw at %.3f s: %s\n", host.nowSeconds(),
                            panel.drawReason().c_str());
            }
        }
        P.eq("draw.chat_quiet", asked, 0);
        const std::string t = panel.transcript();
        P.eq("markdown.answer", has(t, "### What it does") && has(t, "Done.") ? 1 : 0, 1);
        panel.chatView().scrollTo(panel.chatView().contentHeight());
        host.settle();
        writePng(P, host, "gui.chat.markdown.png");
    }

    // ---- ASK -----------------------------------------------------------------------------------------------------------
    {
        const auto option = [](const char* label, const char* description) {
            return input({ { "label", label }, { "description", description } });
        };
        const juce::var two = input(
            { { "questions",
                juce::Array<juce::var>{
                    input({ { "header", "Sound" },
                            { "question", "What should it sound like?" },
                            { "options", juce::Array<juce::var>{ option("Warm (Recommended)", "Analog and round, a soft low-pass"),
                                                                 option("Bright", "Glassy and digital") } } }),
                    input({ { "header", "Panel" },
                            { "question", "What should the panel show?" },
                            { "multi_select", true },
                            { "options", juce::Array<juce::var>{ option("Knobs", ""), option("Meters", "Levels, live"),
                                                                 option("A scope", "The wave as it plays") } } }) } } });
        const juce::var one = input(
            { { "questions", juce::Array<juce::var>{ input({ { "question", "Mono or poly?" },
                                                             { "options", juce::Array<juce::var>{ option("Poly", "Chords"),
                                                                                                  option("Mono", "One note, with glide") } } }) } } });
        pg::ui::ChatView& view = panel.chatView();
        script->add(fake::Response::ok(fake::Sse().start().text("Two questions first.").tool("toolu_q1", "ask_user", two).end("tool_use")));
        clickInput(host, panel);
        type(host, "make me a synth");
        host.keys("return");
        settleChat(processor, host);
        view.scrollTo(view.contentHeight());
        host.settle();
        P.eq("ask.waits", chat(processor).asking && !chat(processor).busy ? 1 : 0, 1);
        P.eq("ask.box_says", has(panel.chatPlaceholder(), "Answer above") ? 1 : 0, 1);
        const auto warm = view.centreOfOption(0, 0), meters = view.centreOfOption(1, 1), scope = view.centreOfOption(1, 2);
        P.eq("ask.options_shown", warm && meters && scope && view.centreOfSubmit() ? 1 : 0, 1);
        const auto radio = a11yTitled(host, "Warm (Recommended)");
        const auto box = a11yTitled(host, "Meters");
        P.eq("ask.a11y", radio && radio->role == funkgui::A11yRole::radioButton && radio->checkable && !radio->checked
                             && box && box->role == funkgui::A11yRole::toggleButton ? 1 : 0, 1);
        const int sent = script->postCount();
        if (warm)
            clickAt(host, *warm);
        host.settle();
        P.eq("ask.picked", view.optionPicked(0, 0) ? 1 : 0, 1);
        if (const auto s = view.centreOfSubmit())
            clickAt(host, *s);
        host.settle();
        P.eq("ask.submit_waits_for_each", script->postCount() == sent && chat(processor).asking ? 1 : 0, 1);
        if (const auto bright = view.centreOfOption(0, 1))
            clickAt(host, *bright);
        if (meters)
            clickAt(host, *meters);
        if (scope)
            clickAt(host, *scope);
        host.settle();
        P.eq("ask.one_replaces", view.optionPicked(0, 1) && !view.optionPicked(0, 0) ? 1 : 0, 1);
        P.eq("ask.several_stay", view.optionPicked(1, 1) && view.optionPicked(1, 2) && !view.optionPicked(1, 0) ? 1 : 0, 1);
        if (warm)
            clickAt(host, *warm);
        host.settle();
        const auto checked = a11yTitled(host, "Warm (Recommended)");
        P.eq("ask.a11y_checked", checked && checked->checked ? 1 : 0, 1);
        writePng(P, host, "gui.chat.ask.png");
        script->add(fake::Response::ok(fake::Sse().start().text("A warm synth with meters and a scope.").end()));
        if (const auto s = view.centreOfSubmit())
            clickAt(host, *s);
        settleChat(processor, host);
        P.eq("ask.sent", script->postCount() == sent + 1 && has(script->body(sent), "Warm (Recommended)")
                             && has(script->body(sent), "Meters, A scope") ? 1 : 0, 1);
        P.eq("ask.answered", !chat(processor).asking && !view.centreOfOption(0, 0) && !view.centreOfSubmit() ? 1 : 0, 1);
        P.eq("ask.transcript", has(panel.transcript(), "= Warm (Recommended) | Meters, A scope")
                                   && has(panel.transcript(), "A warm synth with meters and a scope.") ? 1 : 0, 1);
        view.scrollTo(view.contentHeight());
        host.settle();
        writePng(P, host, "gui.chat.answered.png");
        // One question of one choice: no Send answers; a click answers.
        script->add(fake::Response::ok(fake::Sse().start().tool("toolu_q2", "ask_user", one).end("tool_use")));
        clickInput(host, panel);
        type(host, "now a bass");
        host.keys("return");
        settleChat(processor, host);
        view.scrollTo(view.contentHeight());
        host.settle();
        P.eq("ask.single_no_submit", view.centreOfOption(0, 1) && !view.centreOfSubmit() ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().text("Mono it is.").end()));
        const int at = script->postCount();
        if (const auto mono = view.centreOfOption(0, 1))
            clickAt(host, *mono);
        settleChat(processor, host);
        P.eq("ask.click_answers", script->postCount() == at + 1 && has(script->body(at), "Mono or poly? Mono")
                                      && !chat(processor).asking ? 1 : 0, 1);
    }

    // ---- PICTURES ------------------------------------------------------------------------------------------------------
    std::vector<uint32_t> pictureIds;                        // the sent pictures', which NEW lets go
    {
        const juce::File dir(juce::String(probe::outPath("gui-chat-pictures")));
        dir.deleteRecursively();
        dir.createDirectory();
        const auto picture = [&](const char* name, int w, int h, juce::Colour a, juce::Colour b) {
            juce::Image img(juce::Image::RGB, w, h, true);
            {
                juce::Graphics g(img);
                g.setGradientFill(juce::ColourGradient(a, 0.0f, 0.0f, b, static_cast<float>(w), static_cast<float>(h), false));
                g.fillAll();
                g.setColour(juce::Colours::white);
                g.fillEllipse(static_cast<float>(w) * 0.3f, static_cast<float>(h) * 0.3f, static_cast<float>(w) * 0.4f,
                              static_cast<float>(h) * 0.4f);
            }
            const juce::File f = dir.getChildFile(name);
            juce::FileOutputStream out(f);
            juce::PNGImageFormat().writeImageToStream(img, out);
            out.flush();
            return f.getFullPathName().toStdString();
        };
        const std::string wide = picture("panel.png", 600, 300, juce::Colours::darkslategrey, juce::Colours::orange);
        const std::string tall = picture("knob.png", 240, 400, juce::Colours::navy, juce::Colours::hotpink);
        const std::string third = picture("sketch.png", 320, 320, juce::Colours::darkgreen, juce::Colours::yellow);
        const juce::File notes = dir.getChildFile("notes.txt");
        notes.replaceWithText("not a picture");
        const auto settlePictures = [&] {
            processor.chatAgent().waitIdle();
            host.tick(4);
            host.settle();
        };
        const pg::ui::InputItem& box = panel.chatInputItem();
        clickAt(host, panel.centreOfAttach());
        const funkgui::FileRequest* asked = host.pendingFiles();
        P.eq("pictures.chooser", asked != nullptr && asked->mode == funkgui::FileRequest::Mode::openMany
                                     && has(asked->pattern, "*.png") && has(asked->pattern, "*.webp") ? 1 : 0, 1);
        host.returnFiles({ wide, tall });
        settlePictures();
        P.eq("pictures.in_box", box.pictureCount(), 2);
        P.ge("pictures.drawn", static_cast<double>(host.draw().images.size()), 2.0);
        P.eq("pictures.none_missing", static_cast<int64_t>(host.draw().missingImages), 0);
        writePng(P, host, "gui.chat.pictures.png");
        if (const auto x = box.centreOfRemove(1))
            clickAt(host, *x);
        settlePictures();
        P.eq("pictures.removed", box.pictureCount(), 1);
        // A picture alone may be sent: by Return, and by the send button too.
        panel.chatInput().setText("");
        host.settle();
        const auto sendButton = a11yTitled(host, "Send or stop");
        P.eq("pictures.send_alone_enabled", sendButton && sendButton->enabled ? 1 : 0, 1);
        // A drop: on the chat's card it comes; on the editor it does not; a text file is no drop the chat takes.
        const funkgui::Point onChat = panel.centreOfSend();
        P.eq("pictures.text_refused", host.dropFiles({ notes.getFullPathName().toStdString() }, onChat.x, onChat.y) ? 0 : 1, 1);
        P.eq("pictures.drop_taken", host.dropFiles({ tall }, onChat.x - 120.0f, onChat.y - 160.0f) ? 1 : 0, 1);
        settlePictures();
        P.eq("pictures.dropped_in", box.pictureCount(), 2);
        host.dropFiles({ third }, 40.0f, 200.0f);
        settlePictures();
        P.eq("pictures.not_on_the_editor", box.pictureCount(), 2);
        P.eq("pictures.drag_shown_done", panel.dropShown() ? 0 : 1, 1);
        // The chat's overlay is for pictures: an audio file dragged over (ASSETS takes it) does not light it.
        const std::string kick = dir.getChildFile("kick.wav").getFullPathName().toStdString();
        panel.filesDragEnter({ kick }, onChat.x - 120.0f, onChat.y - 160.0f);
        P.eq("pictures.drag_audio_no_overlay", panel.dropShown() ? 1 : 0, 0);
        panel.filesDragExit();
        panel.filesDragEnter({ kick, tall }, onChat.x - 120.0f, onChat.y - 160.0f);
        P.eq("pictures.drag_picture_overlay", panel.dropShown() ? 1 : 0, 1);
        panel.filesDragExit();
        // Six at the least size: none hidden, each with its remove button in the box (the tiles wrap to a second row).
        host.resize(500, 300);
        host.settle();
        clickAt(host, panel.centreOfAttach());
        if (host.pendingFiles() != nullptr)
            host.returnFiles({ third, picture("four.png", 200, 200, juce::Colours::maroon, juce::Colours::white),
                               picture("five.png", 300, 200, juce::Colours::teal, juce::Colours::black),
                               picture("six.png", 200, 300, juce::Colours::purple, juce::Colours::gold) });
        settlePictures();
        P.eq("pictures.six_attached", box.pictureCount(), 6);
        bool inside = box.pictureCount() == 6;
        for (int i = 0; i < box.pictureCount(); ++i)
        {
            const funkgui::Rect r = box.pictureRect(i);
            const auto x = box.centreOfRemove(i);
            inside = inside && x && box.contains(*x) && r.x >= box.box().x && r.right() <= box.box().right()
                  && r.bottom() <= box.editor().bounds().y;
        }
        P.eq("pictures.six_inside", inside ? 1 : 0, 1);
        P.ge("pictures.six_drawn", static_cast<double>(host.draw().images.size()), 6.0);
        writePng(P, host, "gui.chat.six.png");
        if (const auto x = box.centreOfRemove(5))
            clickAt(host, *x);
        settlePictures();
        P.eq("pictures.sixth_removed", box.pictureCount(), 5);
        for (int i = 4; i >= 2; --i)
            if (const auto x = box.centreOfRemove(i))
                clickAt(host, *x);
        settlePictures();
        P.eq("pictures.back_to_two", box.pictureCount(), 2);
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
        // Sent: before the words, and above the message.
        script->add(fake::Response::ok(fake::Sse().start().text("I see a panel and a knob.").end()));
        const int at = script->postCount();
        clickInput(host, panel);
        type(host, "a panel like these");
        host.keys("return");
        settleChat(processor, host);
        pg::ui::ChatView& view = panel.chatView();
        view.scrollTo(view.contentHeight());
        host.settle();
        {
            const juce::var body = pg::claude::parseJson(script->body(at));
            const juce::Array<juce::var>* m = body.getProperty("messages", {}).getArray();
            int images = 0;
            if (m != nullptr && !m->isEmpty())
                if (const auto* c = (*m)[m->size() - 1].getProperty("content", {}).getArray())
                    for (const juce::var& b : *c)
                        images += b.getProperty("type", "").toString() == "image" ? 1 : 0;
            P.eq("pictures.sent", images, 2);
        }
        P.eq("pictures.box_empty", box.pictureCount() == 0 && box.stripHeight() == 0.0f ? 1 : 0, 1);
        P.eq("pictures.above_the_message", view.tiles(), 2);
        P.eq("pictures.drawn_sent", host.draw().missingImages == 0 && host.draw().images.size() >= 2 ? 1 : 0, 1);
        writePng(P, host, "gui.chat.sent.png");
        for (const pg::ChatEntry& e : chat(processor).entries)
            for (const pg::ChatPicturePtr& pic : e.pictures)
                pictureIds.push_back(pic->id);
        bool held = !pictureIds.empty();
        for (const uint32_t id : pictureIds)
            held = held && funkgui::images::has(id);
        P.eq("pictures.registered", held ? 1 : 0, 1);
    }

    // ---- CHECKS --------------------------------------------------------------------------------------------------------
    {
        pg::Draft d;
        d.files = pg::singleFile(soundProgram());
        processor.editorPort().setDraft(d);
        processor.editorPort().run(d.files);
        processor.settle();
        // The sound at its defaults: a run keeps a dial's value in its units, so the level the program before left at
        // 1 would be 0 dB here (clamped into -40 to 0).
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (const pg::Control k = processor.control(c); k.used)
                processor.getParameters()[c]->setValueNotifyingHost(pg::normOf(k, k.def));
        juce::Array<juce::var> checks;
        if constexpr (pg::product::kIsInstrument)
        {
            checks.add(input({ { "label", "A sensible level" }, { "metric", "rms_db" }, { "min", -40.0 }, { "max", -10.0 } }));
            checks.add(input({ { "label", "Dies away quickly" }, { "metric", "release_s" }, { "max", 0.2 } }));
            checks.add(input({ { "label", "No alerts" }, { "metric", "alerts" }, { "max", 0.0 } }));
        }
        else
        {
            checks.add(input({ { "label", "No boost" }, { "metric", "sine_gain_db" }, { "max", 1.0 } }));
            checks.add(input({ { "label", "Quieter by 6 dB" }, { "metric", "sine_gain_db" }, { "max", -6.0 } }));
            checks.add(input({ { "label", "No alerts" }, { "metric", "alerts" }, { "max", 0.0 } }));
        }
        const char* fixDial = pg::product::kIsInstrument ? "release" : "gain";
        const double fixValue = pg::product::kIsInstrument ? 0.1 : -8.0;
        script->add(fake::Response::ok(fake::Sse()
                                           .start()
                                           .tool("toolu_k1", "set_checks", input({ { "checks", checks } }))
                                           .tool("toolu_k2", "view_sound", pg::product::kIsInstrument
                                                                               ? input({ { "notes", juce::Array<juce::var>{ 45 } } })
                                                                               : input({}))
                                           .end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("Here is the sound.").end()));
        script->add(fake::Response::ok(fake::Sse()
                                           .start()
                                           .tool("toolu_k3", "set_dial", input({ { "name", fixDial }, { "value", fixValue } }))
                                           .tool("toolu_k4", "run_program", input({}))
                                           .end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("Fixed: every check passes now.").end()));
        const int at = script->postCount();
        clickInput(host, panel);
        type(host, "check it");
        host.keys("return");
        for (int k = 0; k < 6000 && (chat(processor).busy || script->postCount() < at + 4); ++k)
        {
            processor.poll();
            host.tick(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        settleChat(processor, host);
        pg::ui::ChatView& view = panel.chatView();
        view.scrollTo(view.contentHeight());
        host.settle();
        P.eq("checks.turn", script->postCount() - at, 4);
        {
            // The picture Claude got (view_sound's, the reply's second result), for a look at a fuller sound than a sine.
            const juce::var body = pg::claude::parseJson(script->body(at + 1));
            const juce::Array<juce::var>* m = body.getProperty("messages", {}).getArray();
            std::string data;
            if (m != nullptr && !m->isEmpty())
                if (const auto* results = (*m)[m->size() - 1].getProperty("content", {}).getArray())
                    for (const juce::var& r : *results)
                        if (const auto* content = r.getProperty("content", {}).getArray())
                            for (const juce::var& c : *content)
                                if (c.getProperty("type", "").toString() == "image")
                                    data = c.getProperty("source", {}).getProperty("data", "").toString().toStdString();
            juce::MemoryOutputStream png;
            const bool decoded = !data.empty() && juce::Base64::convertFromBase64(png, juce::String(data));
            if (decoded)
                juce::File(juce::String(probe::outPath("gui.chat.view_sound.png"))).replaceWithData(png.getData(), png.getDataSize());
            P.eq("checks.sound_png", decoded ? 1 : 0, 1);
        }
        P.eq("checks.rows", view.checkRows(), 3);
        P.eq("checks.sound_picture", view.tiles() >= 1 && host.draw().missingImages == 0 ? 1 : 0, 1);
        P.eq("checks.transcript", has(panel.transcript(), "  # Checks: 3 of 3 pass") ? 1 : 0, 1);
        P.eq("checks.inside", allInside(host, panel) ? 1 : 0, 1);
        // The card at the top of the view, then the sound's picture.
        view.scrollTo(std::max(0.0f, view.contentHeight() - 900.0f));
        host.settle();
        P.eq("checks.clear_shown", view.centreOfClearChecks().has_value() ? 1 : 0, 1);
        P.eq("checks.a11y", a11yTitled(host, "Clear the checks").has_value() ? 1 : 0, 1);
        writePng(P, host, "gui.chat.checks.png");
        // A check that cannot pass: the card shows it failing; Clear forgets the checks.
        juce::Array<juce::var> impossible;
        impossible.add(input({ { "label", "Impossibly loud" }, { "metric", pg::product::kIsInstrument ? "rms_db" : "sine_rms_db" },
                               { "min", 30.0 } }));
        script->add(fake::Response::ok(fake::Sse().start().tool("toolu_k5", "set_checks", input({ { "checks", impossible } })).end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("Done.").end()));
        script->add(fake::Response::ok(fake::Sse().start().text("Still.").end()));
        script->add(fake::Response::ok(fake::Sse().start().text("It cannot pass: that is beyond the output.").end()));
        clickInput(host, panel);
        type(host, "louder");
        host.keys("return");
        settleChat(processor, host);
        view.scrollTo(view.contentHeight());
        host.settle();
        writePng(P, host, "gui.chat.checks-fail.png");
        P.eq("checks.failing_shown", has(panel.transcript(), "  # Checks: 0 of 1 pass") ? 1 : 0, 1);
        if (const auto clear = view.centreOfClearChecks())
            clickAt(host, *clear);
        host.tick(2);
        host.settle();
        P.eq("checks.cleared", chat(processor).checks ? 0 : 1, 1);
        P.eq("checks.clear_gone", view.centreOfClearChecks().has_value() ? 0 : 1, 1);
        for (const pg::ChatEntry& e : chat(processor).entries)
            if (e.kind == pg::ChatEntry::Kind::tool)
                for (const pg::ChatPicturePtr& pic : e.pictures)
                    pictureIds.push_back(pic->id);
    }

    // ---- NEW -----------------------------------------------------------------------------------------------------------
    std::string before;                                  // the chat before the new one: its name
    {
        before = chat(processor).title;
        clickAt(host, panel.centreOfNewChat());
        P.eq("new.asks_nothing", host.pendingMenu() != nullptr ? 1 : 0, 0);
        host.tick(2);
        host.settle();
        P.eq("new.empty", chat(processor).entries.empty() ? 1 : 0, 1);
        const pg::ChatList list = processor.chatAgent().chatList();
        P.eq("new.kept_in_list", list.chats.size() >= 2 && list.chats[0].current && list.chats[0].title == "New chat"
                                     && list.chats[1].title == before && !before.empty() ? 1 : 0, 1);
        bool gone = !pictureIds.empty();
        for (const uint32_t id : pictureIds)
            gone = gone && !funkgui::images::has(id);
        P.eq("new.pictures_let_go", gone ? 1 : 0, 1);
        P.eq("new.empty_again", panel.chatView().blocks() == 0 && panel.chatView().centreOfSuggestion(0).has_value() ? 1 : 0,
             1);
        const int sent = script->postCount();
        if (const auto chip = panel.chatView().centreOfSuggestion(1))
            clickAt(host, *chip);
        host.settle();
        P.eq("new.suggestion_in_box", has(panel.chatInput().text(), " ") && panel.chatInput().focused() ? 1 : 0, 1);
        P.eq("new.suggestion_unsent", script->postCount(), sent);
        host.keys("cmd+a,backspace");
        clickInput(host, panel);
        host.keys("tab");
        type(host, "z");
        P.eq("new.tab_leaves_box", panel.chatInput().text().empty() && !panel.chatInput().focused() ? 1 : 0, 1);
    }

    // ---- CHATS ---------------------------------------------------------------------------------------------------------
    {
        const auto pill = [&]() -> std::optional<funkgui::A11yItem> {
            for (const funkgui::A11yItem& a : host.accessibility())
                if (a.visible && a.title == "Chats")
                    return a;
            return std::nullopt;
        };
        const auto shown = pill();
        P.eq("chats.pill", shown && shown->role == funkgui::A11yRole::comboBox && shown->value == "New chat" ? 1 : 0, 1);
        clickAt(host, panel.centreOfChats());
        host.tick(1);
        const funkgui::MenuRequest* m = host.pendingMenu();
        int radios = 0, commands = 0, olderId = 0;
        if (m != nullptr)
            for (const funkgui::MenuItem& it : m->items)
            {
                if (it.radio)
                {
                    ++radios;
                    if (it.label == before)
                        olderId = it.id;
                }
                else if (!it.header && !it.separator)
                    ++commands;
            }
        P.eq("chats.menu", m != nullptr && radios >= 2 && commands == 3 && olderId > 0 ? 1 : 0, 1);
        const uint64_t conversation = chat(processor).conversation;
        if (m != nullptr)
            host.chooseMenuItem(olderId > 0 ? olderId : 1);
        host.tick(2);
        host.settle();
        P.eq("chats.older_shown", chat(processor).title == before && !chat(processor).entries.empty() ? 1 : 0, 1);
        P.eq("chats.conversation_moves", chat(processor).conversation != conversation ? 1 : 0, 1);   // the view's state goes
        const auto again = pill();
        P.eq("chats.pill_follows", again && again->value == before ? 1 : 0, 1);
        writePng(P, host, "gui.chat.chats.png");
        // Rename, from the menu: the dialog, the name in it selected; typed over and Return.
        constexpr int kRename = 100002, kDelete = 100003;
        clickAt(host, panel.centreOfChats());
        host.tick(1);
        if (host.pendingMenu() != nullptr)
            host.chooseMenuItem(kRename);
        host.tick(2);
        P.eq("chats.rename_opens", panel.renameDialogOpen() && !panel.keyDialogOpen() ? 1 : 0, 1);
        bool named = false;
        for (const funkgui::A11yItem& a : host.accessibility())
            named = named || (a.title == "Name" && a.value == before);
        P.eq("chats.rename_has_name", named ? 1 : 0, 1);
        host.settle();
        writePng(P, host, "gui.chat.rename.png");
        host.keys("escape");
        host.tick(2);
        P.eq("chats.rename_escape", !panel.renameDialogOpen() && chat(processor).title == before ? 1 : 0, 1);
        // A click in the name drops its selection and puts the caret where it lands: past its end, a word added.
        clickAt(host, panel.centreOfChats());
        host.tick(1);
        if (host.pendingMenu() != nullptr)
            host.chooseMenuItem(kRename);
        host.tick(2);
        {
            const funkgui::Point field = panel.centreOfKeyField();
            clickAt(host, { field.x + 200.0f, field.y });
            P.eq("chats.rename_click_caret_end", panel.keyDialogCaret(), static_cast<int64_t>(before.size()));
            type(host, "x");
            bool added = false;
            for (const funkgui::A11yItem& a : host.accessibility())
                added = added || (a.title == "Name" && a.value == before + "x");
            P.eq("chats.rename_click_keeps_name", added ? 1 : 0, 1);
            const auto name = a11yTitled(host, "Name");
            const float ab = funkgui::text::width(funkgui::FontService::get().atlas(), "ab", pg::ui::chat::kBody);
            clickAt(host, { name ? name->bounds.x + 14.0f + ab + 1.0f : field.x, field.y });   // past two characters
            P.eq("chats.rename_click_caret_placed", panel.keyDialogCaret(), 2);
        }
        host.keys("escape");
        host.tick(2);
        P.eq("chats.rename_click_cancelled", chat(processor).title == before ? 1 : 0, 1);
        clickAt(host, panel.centreOfChats());
        host.tick(1);
        if (host.pendingMenu() != nullptr)
            host.chooseMenuItem(kRename);
        host.tick(2);
        type(host, "Glass pad talk");
        bool typed = false;
        for (const funkgui::A11yItem& a : host.accessibility())
            typed = typed || (a.title == "Name" && a.value == "Glass pad talk");
        P.eq("chats.rename_typed_over", typed ? 1 : 0, 1);
        host.keys("return");
        host.tick(2);
        host.settle();
        P.eq("chats.renamed", !panel.renameDialogOpen() && chat(processor).title == "Glass pad talk" ? 1 : 0, 1);
        const auto renamed = pill();
        P.eq("chats.pill_renamed", renamed && renamed->value == "Glass pad talk" ? 1 : 0, 1);
        // Delete: asked first, then gone; the newest other one shows.
        const std::string owner = processor.chatAgent().chatOwner();
        const std::size_t had = pg::chatstore::chatsOf(owner).size();
        clickAt(host, panel.centreOfChats());
        host.tick(1);
        if (host.pendingMenu() != nullptr)
            host.chooseMenuItem(kDelete);
        host.tick(1);
        const funkgui::MenuRequest* ask = host.pendingMenu();
        P.eq("chats.delete_asks", ask != nullptr && !ask->items.empty() && has(ask->items[0].label, "Glass pad talk") ? 1 : 0, 1);
        if (ask != nullptr)
            host.chooseMenuItem(1);
        host.tick(2);
        host.settle();
        const pg::ChatList after = processor.chatAgent().chatList();
        bool gone = true;
        for (const pg::ChatListEntry& e : after.chats)
            gone = gone && e.title != "Glass pad talk";
        P.eq("chats.deleted", gone && pg::chatstore::chatsOf(owner).size() + 1 == had && chat(processor).title != "Glass pad talk"
                                  ? 1 : 0, 1);
    }

    // ---- SCROLL --------------------------------------------------------------------------------------------------------
    {
        for (int k = 0; k < 6; ++k)
            script->add(fake::Response::ok(fake::Sse().start().text("A paragraph that takes room in the view, so the "
                                                                    "conversation grows taller than it.\n\n- one\n- two\n- three").end()));
        for (int k = 0; k < 6; ++k)
        {
            clickInput(host, panel);
            type(host, "more " + std::to_string(k));
            host.keys("return");
            settleChat(processor, host);
        }
        pg::ui::ChatView& view = panel.chatView();
        P.eq("scroll.overflows", view.contentHeight() > view.bounds().h ? 1 : 0, 1);
        P.eq("scroll.follows_end", view.atEnd() ? 1 : 0, 1);
        const funkgui::Rect vb = view.bounds();
        host.wheel(vb.centreX(), vb.centreY(), 2.0f);
        host.settle();
        P.eq("scroll.wheel_up", view.atEnd() ? 0 : 1, 1);
        {
            const float notchDy = funkgui::scroll::kWheelNotch;
            const pg::ui::Scroll s0 = view.scroller();
            host.wheel(vb.centreX(), vb.centreY(), 1000.0f);  // the top
            host.wheel(vb.centreX(), vb.centreY(), -notchDy);
            const double notch = std::min(3.0 * pg::ui::chat::kBodyLine, static_cast<double>(s0.max()));
            P.near("scroll.notch_three_lines", view.scroll(), notch, 0.01);
            host.wheel(vb.centreX(), vb.centreY(), 1000.0f);
            funkgui::WheelEvent w;
            w.x = vb.centreX();
            w.y = vb.centreY();
            w.dy = -notchDy;
            w.reversed = true;
            panel.wheel(w);
            P.near("scroll.reversed_as_code", view.scroll(), notch, 0.01);
            host.wheel(vb.centreX(), vb.centreY(), 1000.0f);
            host.settle();
            const funkgui::Rect area = view.barArea(), t = view.scroller().thumb(area);
            host.drag(t.centreX(), t.centreY(), t.centreX(), t.centreY() + 20.0f, 4);
            host.settle();
            P.near("scroll.thumb_drags", view.scroll(), 20.0 * static_cast<double>(s0.max()) / static_cast<double>(area.h - t.h), 0.05);
            P.eq("scroll.thumb_let_go", view.scroller().dragging() ? 1 : 0, 0);
        }
        const auto jump = view.centreOfJump();
        P.eq("scroll.jump_shown", jump.has_value() ? 1 : 0, 1);
        if (jump)
            clickAt(host, *jump);
        host.settle();
        P.eq("scroll.jumped_to_end", view.atEnd() ? 1 : 0, 1);
        // The keys: the message box keeps its own while it has the caret; Shift-Tab from it reaches the conversation (a
        // Tab stop), where Page Up goes back a view less two lines, Home to the start and End to the end.
        clickInput(host, panel);
        host.keys("pageup");
        host.settle();
        P.eq("scroll.box_keeps_keys", view.atEnd() && panel.chatInput().focused() ? 1 : 0, 1);
        host.keys("shift+tab");
        host.keys("pageup");
        host.settle();
        const double most = std::max(0.0, static_cast<double>(view.contentHeight() - view.bounds().h));
        const double page = std::max(static_cast<double>(pg::ui::chat::kBodyLine),
                                     static_cast<double>(view.bounds().h) - 2.0 * pg::ui::chat::kBodyLine);
        P.near("scroll.key_page_up", view.scroll(), std::max(0.0, most - page), 0.5);
        host.keys("home");
        host.settle();
        P.near("scroll.key_home", view.scroll(), 0.0, 0.01);
        host.keys("end");
        host.settle();
        P.eq("scroll.key_end", view.atEnd() ? 1 : 0, 1);
        // A screen reader has every block, those out of view too, and its focus on one scrolls it into view.
        {
            const std::vector<funkgui::A11yItem> items = host.accessibility();
            int blocks = 0;
            uint32_t firstYou = 0;
            for (const funkgui::A11yItem& a : items)
                if (a.parent == 0x20000000u && a.role == funkgui::A11yRole::listItem)
                {
                    ++blocks;
                    if (firstYou == 0 && a.title == "You")
                        firstYou = a.id;
                }
            P.eq("scroll.a11y_every_block", blocks, view.blocks());
            if (firstYou != 0)
                panel.a11yAction(firstYou, funkgui::A11yAction::focus, 0.0);
            host.settle();
            bool inView = false;
            for (const funkgui::A11yItem& a : host.accessibility())
                if (a.id == firstYou && firstYou != 0)
                    inView = a.bounds.y >= vb.y - 0.5f && a.bounds.bottom() <= vb.bottom() + 0.5f;
            P.eq("scroll.a11y_focus_reveals", inView && !view.atEnd() ? 1 : 0, 1);
        }
        host.keys("end");
        host.settle();
        clickInput(host, panel);
    }

    // ---- VIEW ----------------------------------------------------------------------------------------------------------
    {
        // The conversation alone (ChatView over a made-up state): what it draws and what Copy copies.
        pg::ui::ChatView view(0x30000000u, {});
        view.attach(&host);
        funkgui::GestureController gestures(host);
        view.setBounds({ 0.0f, 0.0f, 520.0f, 2400.0f });   // all of it in view
        using K = pg::ChatEntry::Kind;
        const auto entry = [](K k, std::string t) {
            pg::ChatEntry e;
            e.kind = k;
            e.text = std::move(t);
            return e;
        };
        const std::string answer = "Gr\xc3\xb6\xc3\x9f" "e: 5 \xc2\xb5s\n```\n// a \xe2\x80\x94 b \xc2\xb0\n```\n";
        pg::ChatState st;
        st.hasKey = true;
        st.conversation = 7;
        st.entries.push_back(entry(K::user, "make cutoff = 200*2 and rename `lp` to float* out"));
        st.entries.push_back(entry(K::thinking, "The user wants `[[fminus::range(0, 2)]]` on the level dial and a `gain` too."));
        st.entries.push_back(entry(K::assistant, answer));
        st.entries.push_back(entry(K::tool, "wrote errors.fm (12 lines)"));
        pg::ChatEntry q = entry(K::question, "Which?");
        q.questions.push_back({ "", "Which?", { { "Yes", "" }, { "Yes, but quieter", "" }, { "No", "" } }, false });
        q.answers.push_back("Yes, but quieter");
        st.entries.push_back(q);
        view.show(st, {});
        const auto press = [&](const std::string& title) {
            std::vector<funkgui::A11yItem> items;
            view.accessibility(items);
            for (const funkgui::A11yItem& a : items)
                if (a.title == title)
                {
                    view.a11yAction(a.id, funkgui::A11yAction::press, 0.0, gestures);
                    return true;
                }
            return false;
        };
        // The user's words as typed: no markdown read in them.
        const std::string shown = view.shownText();
        P.eq("view.user_as_typed", has(shown, "200*2") && has(shown, "`lp`") && has(shown, "float* out") ? 1 : 0, 1);
        // Copy: the entry's own text (UTF-8), not what the font draws.
        P.eq("view.code_copy_as_held", press("Copy the code") && host.log.lastCopy == "// a \xe2\x80\x94 b \xc2\xb0\n" ? 1 : 0, 1);
        P.eq("view.answer_copy_as_held", press("Copy the answer") && host.log.lastCopy == answer ? 1 : 0, 1);
        // Inline code in the opened thinking (small text) fills the cells it is laid out in: nothing runs over the next
        // word.
        press("Show Claude's thinking");
        view.show(st, {});
        P.eq("view.thinking_opened", view.thinkingOpen(0) ? 1 : 0, 1);
        P.le("view.small_code_fits", static_cast<double>(view.overlap()), 0.5);
        // A tool row is drawn as failed by its outcome, never by its words.
        P.eq("view.named_error_not_failed", view.failedRows(), 0);
        pg::ChatEntry stopped = entry(K::tool, "ran it: it stopped");
        stopped.failed = true;
        st.entries.insert(st.entries.begin() + 4, stopped);
        view.show(st, {});
        P.eq("view.stopped_failed", view.failedRows(), 1);
        // An answered question marks the option chosen, not one whose label begins it.
        P.eq("view.answer_marks_chosen", !view.chosen(0, 0) && view.chosen(0, 1) && !view.chosen(0, 2) ? 1 : 0, 1);
        // Another conversation of as many entries: nothing opened in the last one shows opened.
        view.show(st, {});
        P.eq("view.same_conversation_keeps", view.thinkingOpen(0) ? 1 : 0, 1);
        pg::ChatState other = st;
        other.conversation = 8;
        other.entries[1].text = "Another conversation's thinking.";
        view.show(other, {});
        P.eq("view.other_conversation_closed", view.thinkingOpen(0) ? 0 : 1, 1);
    }

    // ---- COLLAPSE ------------------------------------------------------------------------------------------------------
    {
        const float editorW = panel.editor().bounds().w;
        const funkgui::Point fold = panel.centreOfChatToggle();
        P.eq("collapse.toggle_in_header", fold.x > panel.chatCard().x && fold.y < panel.chatCard().y + 40.0f ? 1 : 0, 1);
        clickAt(host, fold);
        host.settle();
        P.eq("collapse.folded", !panel.chatOpen() && !processor.editorPort().chatOpen() ? 1 : 0, 1);
        P.le("collapse.rail", static_cast<double>(panel.chatCard().w), 48.0);
        P.ge("collapse.editor_wider", static_cast<double>(panel.editor().bounds().w - editorW), 250.0);
        bool input = false;
        for (const funkgui::A11yItem& a : host.accessibility())
            input = input || a.title == "Message to Claude";
        P.eq("collapse.chat_hidden", input ? 0 : 1, 1);
        P.eq("collapse.toggle_on_rail", panel.chatCard().contains(panel.centreOfChatToggle()) ? 1 : 0, 1);
        // Claude writes while the chat is folded: the rail's dot.
        script->add(fake::Response::ok(fake::Sse().start().text("An answer while folded.").end()));
        processor.chatAgent().send("while folded");
        settleChat(processor, host);
        P.eq("collapse.unread", panel.chatUnread() ? 1 : 0, 1);
        writePng(P, host, "gui.chat.folded.png");
        {
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            pg::Processor second;
            second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            P.eq("collapse.in_session", second.editorPort().chatOpen() ? 0 : 1, 1);
        }
        {
            // A Panel made while the chat is folded (an editor opened again): idle, it asks for no frame and lays
            // nothing out, at the frame pump's cadence; unfolded, its message box has its height at once. Idle from
            // when its first check while typing has landed (checkLanded): until then it has work of its own.
            pg::ui::Panel again(processor.ports(), processor.editorPort());
            funkgui::HeadlessHost h2(again, 0, 2.0f);
            h2.settle();
            probe::runCadence(h2, 120, [](int) {}, [&] {
                if (again.needsDraw())
                    h2.draw();
                return again.wantsFullRate();
            });
            P.eq("collapse.made_folded_check_landed", checkLanded(again, h2, processor.editorPort()) ? 1 : 0, 1);
            h2.settle();
            h2.draw();
            const uint32_t rev = again.a11yRevision();
            int asked = 0;
            const probe::Cadence idle = probe::runCadence(h2, 120, [](int) {}, [&] {
                if (again.needsDraw())
                {
                    ++asked;
                    std::printf("  frame asked to draw at %.3f s: %s\n", h2.nowSeconds(), again.drawReason().c_str());
                    h2.draw();
                }
                return again.wantsFullRate();
            });
            std::printf("a Panel made folded: %d of %d calm frames asked to draw\n", asked, idle.frames);
            P.le("collapse.made_folded_quiet", asked, 2.0);   // the safety net's, once a second at most
            P.eq("collapse.made_folded_layout_rests", again.a11yRevision() == rev ? 1 : 0, 1);
            clickAt(h2, again.centreOfChatToggle());
            const pg::ui::InputItem& box = again.chatInputItem();
            P.eq("collapse.made_folded_opens", again.chatOpen() ? 1 : 0, 1);
            P.near("collapse.made_folded_box_height", static_cast<double>(box.box().h),
                   static_cast<double>(box.desiredHeight(box.box().w, box.bar().h)), 0.5);
            processor.editorPort().setChatOpen(false);   // the probe's panel folded still, as the session says
        }
        clickAt(host, panel.centreOfChatToggle());
        host.settle();
        P.eq("collapse.opened", panel.chatOpen() && !panel.chatUnread() && panel.chatCard().w > 300.0f ? 1 : 0, 1);
        P.near("collapse.editor_back", static_cast<double>(panel.editor().bounds().w), static_cast<double>(editorW), 0.5);
        P.eq("collapse.answer_there", has(panel.transcript(), "An answer while folded.") ? 1 : 0, 1);
    }

    // ---- RESIZE --------------------------------------------------------------------------------------------------------
    {
        host.resize(500, 300);
        host.settle();
        // Sixteen files (kMaxFiles): their tabs narrow to fit the bar, + reachable until the last.
        for (int k = 0; k < 2 * static_cast<int>(pg::kMaxFiles) && panel.fileCount() < static_cast<int>(pg::kMaxFiles); ++k)
        {
            const funkgui::Point plus = panel.centreOfNewTab();
            const int had = panel.fileCount();
            clickAt(host, plus);
            host.keys("escape");
            host.settle();
            if (panel.fileCount() == had)
                std::printf("INFO     resize: + at %.1f %.1f added nothing (%d files; the program's card %.1f to %.1f; "
                            "explorer %.1f wide, open %d shown %d; width %d)\n",
                            static_cast<double>(plus.x), static_cast<double>(plus.y), had,
                            static_cast<double>(panel.programCard().x), static_cast<double>(panel.programCard().right()),
                            static_cast<double>(panel.explorerCard().w), panel.explorerOpen() ? 1 : 0,
                            panel.explorerShown() ? 1 : 0, panel.width());
        }
        P.eq("resize.sixteen_files", panel.fileCount(), static_cast<int64_t>(pg::kMaxFiles));
        P.eq("resize.least_w", panel.width(), pg::ui::kMinWidth);
        P.eq("resize.least_h", panel.height(), pg::ui::kMinHeight);
        P.eq("resize.least_inside", allInside(host, panel) ? 1 : 0, 1);
        const float smallEditor = panel.editor().bounds().h;
        const float smallChat = panel.chatInput().bounds().y;
        const float smallView = panel.chatView().bounds().h;
        P.ge("resize.least_chat_right", static_cast<double>(panel.chatInput().bounds().x),
             static_cast<double>(panel.editor().bounds().right()));
        P.eq("resize.least_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
        writePng(P, host, "gui.chat.small.png");
        // The message box's bar at the least size, on Claude Opus 5.5 (fast mode's and web search's pills shown): no
        // item over another or past the bar, so none under send; stop's centre stops, web search as it was.
        {
            processor.chatAgent().setModel("claude-opus-5-5");
            host.tick(4);
            host.settle();
            P.eq("resize.least_bar_switches", a11yTitled(host, "Fast mode") && a11yTitled(host, "Web search") ? 1 : 0, 1);
            P.eq("resize.least_bar_fits", barFits(host, panel) ? 1 : 0, 1);
            fake::Response stall = fake::Response::ok(fake::Sse().start().thinking("Considering", "sig-r"));
            stall.stall = true;
            script->add(stall);
            const bool web = chat(processor).web;
            clickInput(host, panel);
            type(host, "small");
            host.keys("return");
            const auto t0 = std::chrono::steady_clock::now();
            while (script->streaming.load() == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            host.tick(4);
            P.eq("resize.least_bar_fits_busy", barFits(host, panel) ? 1 : 0, 1);
            const int stopped = count(panel.transcript(), "- Stopped.");
            clickAt(host, panel.centreOfSend());
            host.tick(2);
            const bool webKept = chat(processor).web == web;
            if (!webKept)
                processor.chatAgent().stop();            // the click switched web search instead: ended here
            settleChat(processor, host);
            dropUnplayed(*script);
            P.eq("resize.least_stop_stops", webKept && !chat(processor).busy && count(panel.transcript(), "- Stopped.") == stopped + 1 ? 1 : 0, 1);
        }
        host.resize(1900, 1100);
        host.settle();
        P.eq("resize.large", panel.width() == 1900 && panel.height() == 1100 ? 1 : 0, 1);
        P.eq("resize.large_inside", allInside(host, panel) ? 1 : 0, 1);
        P.near("resize.editor_grows", static_cast<double>(panel.editor().bounds().h - smallEditor),
               1100.0 - static_cast<double>(pg::ui::kMinHeight), 0.5);
        P.near("resize.chat_follows", static_cast<double>(panel.chatInput().bounds().y - smallChat),
               1100.0 - static_cast<double>(pg::ui::kMinHeight), 0.5);
        P.near("resize.conversation_grows", static_cast<double>(panel.chatView().bounds().h - smallView),
               1100.0 - static_cast<double>(pg::ui::kMinHeight), 0.5);
        writePng(P, host, "gui.chat.large.png");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();

        processor.setEditorSize(1500, 900);
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        pg::Processor other;
        other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("resize.session_keeps_size", other.editorWidth() == 1500 && other.editorHeight() == 900 ? 1 : 0, 1);
        processor.setEditorSize(-3, 70000);
        P.eq("resize.nonsense_size_dropped", processor.editorWidth() == 0 && processor.editorHeight() == 0 ? 1 : 0, 1);
    }

    pg::chatstore::setDirectoryForTests(juce::File());
    keys.deleteRecursively();
    return P.finish();
}
