// PROBE name=gui.chat timeout=240
//
// gui.chat: the editor's chat column, its key dialog and the resizable panel, headless (funkgui::HeadlessHost), over a
// real processor whose chat talks to a scripted API (../plugin/FakeClaude.h): nothing reaches the network, and the key
// folder is the probe's own. It writes, to <build>/probe-out/:
//   gui.chat.empty.png    the empty chat: its title, the Sign in and API key buttons, the suggestions
//   gui.chat.png          a conversation: a bubble, Claude's thinking (opened), its answer with a code block, the card
//                         of what it did, the message box grown to three lines
//   gui.chat.key.png      the access dialog's API key tab with a key pasted (masked)
//   gui.chat.signin.png   its account tab: a username typed and a password pasted (masked), after a wrong password
//   gui.chat.account.png  its account tab signed in: who, the credit left, this month's use, the sign-in's dates, Refresh
//                         and Sign out
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
//   gui.chat.sent.png     a message sent with two pictures: their thumbnails above its bubble
//   gui.chat.checks.png   Claude's checks card, every check passing, and the picture of the sound it looked at
//   gui.chat.checks-fail.png  a check that fails, after the turn went on twice
//   gui.chat.view_sound.png   the picture view_sound gave Claude there (an instrument's saw at A2; an effect's sweep)
// and checks:
//   EMPTY    with neither a sign-in nor a key the empty chat has a Sign in button (which opens the access dialog on its
//            account tab), an API key button (on its API key tab) and suggestions
//   KEY      a message sent before the chat can send opens the dialog, on the account tab, and stays in the message
//            box; Escape closes the dialog (the message stays, unsent); sent again, it waits; while the dialog is open
//            the pointer and the keys reach nothing else; on the API key tab a key that is not one is refused (the
//            dialog says why; nothing is stored); Cmd-V pastes a key; nothing the panel speaks holds the key; SAVE
//            stores it, closes the dialog and sends the waiting message; KEY opens the dialog on the API key tab (the
//            key in use); REMOVE (shown once a key is stored) forgets it; Return in the dialog saves one
//   ACCOUNT  the account tab (the key in use): typed keys go to the username, Tab to the password, Cmd-V pastes it,
//            masked; nothing the panel speaks holds the password; a wrong password keeps the dialog open with the
//            server's message, the password field emptied; Return signs in: the dialog closes, the token is stored,
//            the chat goes through the account and the line under the box shows the credit; a message then goes to
//            the server with the token, and the credit after it is the server's; the header's account pill (shown
//            while signed in) opens the account tab (in use), which asks the server again and says who, the credit,
//            this month's requests, spending and tokens and the sign-in's dates, and speaks no token; Refresh asks
//            again; the API key tab offers Use this key, which switches to the key, and
//            the account tab then Use account, which switches back; Sign out forgets the token, tells the server and
//            leaves the key in use
//   SEND     Shift-Return breaks the line in the message box and the box grows with its rows, Return sends it (the box
//            empties and shrinks); the conversation shows the turn (YOU, CLAUDE, the thinking, what Claude did, its
//            answer) and the line under the box the usage; the thinking row opens and closes; a code block's Copy puts
//            its code on the clipboard; the files Claude wrote are the editor's tabs and play; Cmd-Z in the program's
//            editor gives back the text it had before Claude wrote it, as one step
//   MODEL    the model's pill opens a menu of the models, the chosen one ticked; choosing another changes it; the
//            thinking pill's menu has that model's levels, and choosing one sets it; fast mode's pill, after it on
//            Claude Opus 5.5, switches fast mode on and off (a switch to a screen reader, on or off), and is gone
//            on a model that takes no fast mode
//   WEB      web search's pill, after fast mode's: a switch (to a screen reader too), on and off
//   STOP     while Claude works the line under the box says what it is doing and send is stop: a click ends the turn
//   ASK      Claude's question (ask_user) as a card: the turn waits and the message box says so; each option a button
//            (to a screen reader a radio button, or a toggle for several); a click marks it (another of one choice
//            replaces it, several stay); Send answers waits until each question has a pick, then sends the labels and
//            the turn goes on; the card then has no buttons and the transcript the answers; a question of one choice
//            has no Send answers, and its click answers
//   PICTURES the message box's picture button asks the host for files (several, pictures); the files chosen show as
//            thumbnails in the box (drawn, none missing) and their remove buttons take one back; pictures dropped on
//            the chat's card come too (the card says so while the drag is over the editor), not on the editor, and a
//            text file is no drop the chat takes; sent, the pictures go before the words and show above the message;
//            FunkGui holds each thumbnail while the chat does, and a new chat lets them go
//   CHECKS   Claude's checks (set_checks) as a card, a row each (its label, its value against the goal), and the
//            picture of the sound it looked at (view_sound) under its row, drawn; one fails, the turn goes on, a dial
//            set and a run fix it: the card brought up to date (every check passing); a check that cannot pass shows
//            failing; the card's Clear (spoken as a button) forgets the checks and goes
//   NEW      NEW asks first, then forgets the conversation; a suggestion goes into the message box, unsent; Tab leaves
//            the message box
//   SCROLL   a conversation taller than the view scrolls with the wheel, and the round button jumps back to its end
//   COLLAPSE the header's chevron folds the chat into a rail (the processor keeps it, its state too): the editor takes
//            the room, the chat's items go (the message box among them); an answer while folded lights the rail's dot;
//            the rail's button opens the chat again, the dot gone, the answer in it, the editor as wide as before
//   RESIZE   a size below the least is raised to it; at the least size (with twelve files, their tabs narrowed) and at
//            a large one every item lies inside the panel, the chat's column right of the editor; the editor and the conversation grow with the height; the
//            processor keeps the editor's size in the session
#include "Probe.h"

#include "../plugin/FakeClaude.h"

#include "editor/ChatItems.h"
#include "editor/ChatView.h"
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
#include <funkgui/widgets/TextEditor.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <juce_graphics/juce_graphics.h>

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

    // A program that sounds, with a dial to fix: an instrument's sine with its level (dB) and a release (s); an effect's
    // gain (dB).
    std::string soundProgram()
    {
        if constexpr (tp::product::kIsInstrument)
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

    // ---- EMPTY ---------------------------------------------------------------------------------------------------------
    {
        const auto addKey = panel.chatView().centreOfAddKey();
        const auto signIn = panel.chatView().centreOfSignIn();
        P.eq("empty.key_button", addKey.has_value() ? 1 : 0, 1);
        P.eq("empty.signin_button", signIn.has_value() ? 1 : 0, 1);
        P.eq("empty.suggestions", panel.chatView().centreOfSuggestion(3).has_value() ? 1 : 0, 1);
        writePng(P, host, "gui.chat.empty.png");
        if (addKey)
            clickAt(host, *addKey);
        host.settle();
        P.eq("empty.key_button_opens_dialog", panel.keyDialogOpen() && !panel.dialogOnAccount() ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        if (signIn)
            clickAt(host, *signIn);
        host.settle();
        P.eq("empty.signin_button_opens_account", panel.keyDialogOpen() && panel.dialogOnAccount() ? 1 : 0, 1);
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
        P.eq("key.opens_on_account", panel.dialogOnAccount() ? 1 : 0, 1);
        clickAt(host, panel.centreOfDialogTab(false));
        host.settle();
        P.eq("key.tab_switches", panel.keyDialogOpen() && !panel.dialogOnAccount() ? 1 : 0, 1);
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
        P.eq("key.button_opens", panel.keyDialogOpen() && !panel.dialogOnAccount() ? 1 : 0, 1);
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

    // ---- ACCOUNT -------------------------------------------------------------------------------------------------------
    {
        const std::string before = processor.chatAgent().saveState();
        const std::string password = fake::fakePassword(), token = fake::fakeToken();
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("account.key_tab_first", panel.keyDialogOpen() && !panel.dialogOnAccount() ? 1 : 0, 1);
        clickAt(host, panel.centreOfDialogTab(true));
        host.settle();
        P.eq("account.tab", panel.dialogOnAccount() ? 1 : 0, 1);
        type(host, "alice");
        host.keys("tab");
        host.setClipboard(password);
        host.keys("cmd+v");
        host.settle();
        P.eq("account.a11y_username", a11yTitled(host, "Username").has_value()
                                          && a11yTitled(host, "Username")->value == "alice" ? 1 : 0, 1);
        P.eq("account.a11y_never_the_password", a11yHoldsKey(host, password) ? 1 : 0, 0);
        script->logins.push_back(fake::Response::failure(401, "authentication_error", "Wrong username or password."));
        host.keys("return");
        settleChat(processor, host);
        P.eq("account.wrong_stays_open", panel.keyDialogOpen() && panel.dialogOnAccount() ? 1 : 0, 1);
        P.eq("account.wrong_says_why", panel.keyDialogWhy() == "Wrong username or password." ? 1 : 0, 1);
        P.eq("account.wrong_password_emptied", a11yTitled(host, "Password").has_value()
                                                   && a11yTitled(host, "Password")->value == "empty" ? 1 : 0, 1);
        P.eq("account.wrong_not_signed_in", chat(processor).signedIn ? 1 : 0, 0);
        host.keys("cmd+v");                             // the caret is in the password again
        host.settle();
        writePng(P, host, "gui.chat.signin.png");
        host.keys("return");
        settleChat(processor, host);
        tp::ChatState s = chat(processor);
        P.eq("account.signed_in_closes", panel.keyDialogOpen() ? 1 : 0, 0);
        P.eq("account.signed_in", s.signedIn && s.useAccount && tp::chatstore::loadToken().value_or("") == token ? 1 : 0, 1);
        P.eq("account.credit_shown", has(panel.chatActivity(), "$5.00 credit left") ? 1 : 0, 1);

        script->add(fake::Response::ok(fake::Sse().start().text("Hello from the server.").end()));
        clickInput(host, panel);
        type(host, "through the account");
        host.keys("return");
        settleChat(processor, host);
        P.eq("account.sent_to_server", script->lastHost("POST /v1/messages") == tp::claude::serverHost()
                                           && script->lastHeader("POST /v1/messages", "authorization") == "Bearer " + token
                                           && !script->lastHas("POST /v1/messages", "x-api-key") ? 1 : 0, 1);
        P.eq("account.answer_shown", has(panel.transcript(), "CLAUDE\nHello from the server.") ? 1 : 0, 1);
        P.eq("account.credit_after", has(panel.chatActivity(), "$4.50 credit left") ? 1 : 0, 1);

        // The header's account pill: who, and the credit once the server has said.
        const auto pill = panel.centreOfAccount();
        P.eq("account.pill_shown", pill.has_value() ? 1 : 0, 1);
        bool pillSays = false;
        for (const funkgui::A11yItem& a : host.accessibility())
            pillSays = pillSays || (a.title == "Your account" && a.role == funkgui::A11yRole::button);
        P.eq("account.pill_a11y", pillSays ? 1 : 0, 1);
        const int asks = script->count("GET /account");
        if (pill)
            clickAt(host, *pill);
        settleChat(processor, host);
        P.eq("account.button_opens_account", panel.keyDialogOpen() && panel.dialogOnAccount() ? 1 : 0, 1);
        P.eq("account.view_asks", script->count("GET /account") == asks + 1 ? 1 : 0, 1);
        P.eq("account.signed_in_says_who", a11yTitled(host, "Your TestingPlayground account").has_value()
                                               && has(a11yTitled(host, "Your TestingPlayground account")->description, "alice") ? 1 : 0, 1);
        {
            const tp::ChatAccount info = chat(processor).details;
            P.eq("account.details", info.known && info.requests == 3 && info.inputTokens == 12000 && info.outputTokens == 900
                                        && info.client == "probe" ? 1 : 0, 1);
            const auto box = a11yTitled(host, "Your TestingPlayground account");
            P.eq("account.details_spoken", box.has_value() && has(box->description, "3 requests, $0.50 spent, 12.0K tokens in, 900 out")
                                               && has(box->description, "Signed in 26 Oct 2026, until 24 Jan 2027") ? 1 : 0, 1);
        }
        {
            const auto refresh = a11yTitled(host, "Refresh");
            P.eq("account.refresh_offered", refresh.has_value() ? 1 : 0, 1);
            if (refresh)
                clickAt(host, { refresh->bounds.centreX(), refresh->bounds.centreY() });
            settleChat(processor, host);
            P.eq("account.refresh_asks", script->count("GET /account") == asks + 2 ? 1 : 0, 1);
        }
        P.eq("account.a11y_never_the_token", a11yHoldsKey(host, token) ? 1 : 0, 0);
        P.eq("account.no_use_while_in_use", panel.centreOfUseThis().has_value() ? 1 : 0, 0);
        writePng(P, host, "gui.chat.account.png");
        clickAt(host, panel.centreOfDialogTab(false));
        host.settle();
        const auto useKey = panel.centreOfUseThis();
        P.eq("account.use_key_offered", useKey.has_value() ? 1 : 0, 1);
        if (useKey)
            clickAt(host, *useKey);
        host.tick(2);
        host.settle();
        P.eq("account.use_key", !chat(processor).useAccount && !panel.keyDialogOpen() ? 1 : 0, 1);
        clickAt(host, panel.centreOfKey());
        host.settle();
        clickAt(host, panel.centreOfDialogTab(true));
        host.settle();
        const auto useAccount = panel.centreOfUseThis();
        P.eq("account.use_account_offered", useAccount.has_value() ? 1 : 0, 1);
        if (useAccount)
            clickAt(host, *useAccount);
        host.tick(2);
        host.settle();
        P.eq("account.use_account", chat(processor).useAccount && !panel.keyDialogOpen() ? 1 : 0, 1);

        clickAt(host, panel.centreOfKey());
        host.settle();
        const auto signOut = panel.centreOfSignOut();
        P.eq("account.sign_out_shown", signOut.has_value() ? 1 : 0, 1);
        const int logouts = script->count("POST /auth/logout");
        if (signOut)
            clickAt(host, *signOut);
        settleChat(processor, host);
        s = chat(processor);
        P.eq("account.signed_out", !s.signedIn && !tp::chatstore::hasToken() && !panel.keyDialogOpen() ? 1 : 0, 1);
        P.eq("account.server_told", script->count("POST /auth/logout") == logouts + 1 ? 1 : 0, 1);
        P.eq("account.key_in_use_after", !s.useAccount && s.ready() ? 1 : 0, 1);
        processor.chatAgent().loadState(before);       // the conversation the sections below go on with
        settleChat(processor, host);
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
        tp::ui::ChatView& view = panel.chatView();
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
        settleChat(processor, host);
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
        tp::ui::ChatView& view = panel.chatView();
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
        const tp::ui::InputItem& box = panel.chatInputItem();
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
        // Sent: before the words, and above the message.
        script->add(fake::Response::ok(fake::Sse().start().text("I see a panel and a knob.").end()));
        const int at = script->postCount();
        clickInput(host, panel);
        type(host, "a panel like these");
        host.keys("return");
        settleChat(processor, host);
        tp::ui::ChatView& view = panel.chatView();
        view.scrollTo(view.contentHeight());
        host.settle();
        {
            const juce::var body = tp::claude::parseJson(script->body(at));
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
        for (const tp::ChatEntry& e : chat(processor).entries)
            for (const tp::ChatPicturePtr& pic : e.pictures)
                pictureIds.push_back(pic->id);
        bool held = !pictureIds.empty();
        for (const uint32_t id : pictureIds)
            held = held && funkgui::images::has(id);
        P.eq("pictures.registered", held ? 1 : 0, 1);
    }

    // ---- CHECKS --------------------------------------------------------------------------------------------------------
    {
        tp::Draft d;
        d.files = tp::singleFile(soundProgram());
        processor.editorPort().setDraft(d);
        processor.editorPort().run(d.files);
        processor.settle();
        juce::Array<juce::var> checks;
        if constexpr (tp::product::kIsInstrument)
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
        const char* fixDial = tp::product::kIsInstrument ? "release" : "gain";
        const double fixValue = tp::product::kIsInstrument ? 0.1 : -8.0;
        script->add(fake::Response::ok(fake::Sse()
                                           .start()
                                           .tool("toolu_k1", "set_checks", input({ { "checks", checks } }))
                                           .tool("toolu_k2", "view_sound", tp::product::kIsInstrument
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
        tp::ui::ChatView& view = panel.chatView();
        view.scrollTo(view.contentHeight());
        host.settle();
        P.eq("checks.turn", script->postCount() - at, 4);
        {
            // The picture Claude got (view_sound's, the reply's second result), for a look at a fuller sound than a sine.
            const juce::var body = tp::claude::parseJson(script->body(at + 1));
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
        impossible.add(input({ { "label", "Impossibly loud" }, { "metric", tp::product::kIsInstrument ? "rms_db" : "sine_rms_db" },
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
        for (const tp::ChatEntry& e : chat(processor).entries)
            if (e.kind == tp::ChatEntry::Kind::tool)
                for (const tp::ChatPicturePtr& pic : e.pictures)
                    pictureIds.push_back(pic->id);
    }

    // ---- NEW -----------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfNewChat());
        P.eq("new.asks", host.pendingMenu() != nullptr ? 1 : 0, 1);
        host.chooseMenuItem(1);
        host.tick(2);
        host.settle();
        P.eq("new.forgotten", chat(processor).entries.empty() ? 1 : 0, 1);
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
        tp::ui::ChatView& view = panel.chatView();
        P.eq("scroll.overflows", view.contentHeight() > view.bounds().h ? 1 : 0, 1);
        P.eq("scroll.follows_end", view.atEnd() ? 1 : 0, 1);
        const funkgui::Rect vb = view.bounds();
        host.wheel(vb.centreX(), vb.centreY(), 2.0f);
        host.settle();
        P.eq("scroll.wheel_up", view.atEnd() ? 0 : 1, 1);
        const auto jump = view.centreOfJump();
        P.eq("scroll.jump_shown", jump.has_value() ? 1 : 0, 1);
        if (jump)
            clickAt(host, *jump);
        host.settle();
        P.eq("scroll.jumped_to_end", view.atEnd() ? 1 : 0, 1);
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
            tp::Processor second;
            second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            P.eq("collapse.in_session", second.editorPort().chatOpen() ? 0 : 1, 1);
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
        // Twelve files: their tabs narrow to fit the bar.
        while (panel.fileCount() < static_cast<int>(tp::kMaxFiles))
        {
            const funkgui::Point plus = panel.centreOfNewTab();
            clickAt(host, plus);
            host.keys("escape");
            host.settle();
        }
        P.eq("resize.twelve_files", panel.fileCount(), static_cast<int64_t>(tp::kMaxFiles));
        P.eq("resize.least_w", panel.width(), tp::ui::kMinWidth);
        P.eq("resize.least_h", panel.height(), tp::ui::kMinHeight);
        P.eq("resize.least_inside", allInside(host, panel) ? 1 : 0, 1);
        const float smallEditor = panel.editor().bounds().h;
        const float smallChat = panel.chatInput().bounds().y;
        const float smallView = panel.chatView().bounds().h;
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
        P.near("resize.conversation_grows", static_cast<double>(panel.chatView().bounds().h - smallView),
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
