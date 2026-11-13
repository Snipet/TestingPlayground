// PROBE name=gui.providers timeout=240
//
// gui.providers: the key dialog as the model provider's settings (Panel.h, "key"; plugin/Providers.h), headless
// (funkgui::HeadlessHost), over a real processor whose chat talks to scripted servers (../plugin/FakeClaude.h):
// nothing reaches the network, and the keys' folder is the probe's own. It writes, to <build>/probe-out/:
//   gui.providers.anthropic.png   Anthropic's dialog as it always was, with the Provider pill at its title's right
//   gui.providers.menu.png        the Provider pill's menu: the providers, Anthropic dotted
//   gui.providers.openrouter.png  OpenRouter's: its server's address, the key pasted (masked), a model typed
//   gui.providers.bar.png         the chat on OpenRouter: the model's pill named by its list, no thinking pill
//   gui.providers.effort.png      the chat on OpenRouter with a model that reasons: its Thinking pill, High chosen
//   gui.providers.chat.png        a turn on OpenRouter: its reasoning, its answer
//   gui.providers.ollama.png      Ollama's on this Mac: no key needed
//   gui.providers.gemini.png      Google Gemini's: a key and a model, no address
//   gui.providers.custom.png      another server's, an http address off the local network refused, saying why
//   gui.providers.paper.png       OpenRouter's in PAPER
// and checks:
//   ANTHROPIC  with no key the dialog is Anthropic's (its key field alone, no address or model), the Provider pill
//              shown (a button to a screen reader)
//   MENU       the pill's menu lists every provider, the one shown dotted; choosing OpenRouter shows its address,
//              its key and its model fields, the keys going to the key
//   SAVE       nothing typed and no key stored: refused, saying why; a key pasted is masked and never spoken; Tab
//              goes to the model, typed; Return saves: the chat talks to OpenRouter, the key stored for its server
//              alone, the dialog closed
//   BAR        the model pill names the model as its list does; the thinking pill is gone; the model's menu is
//              BUILD WITH (OpenRouter) and has no PLAN WITH
//   EFFORT     a model OpenRouter's list says reasons: the Thinking pill back (a combo box to a screen reader), its
//              default shown; its menu the model's levels in order (Off, Minimal, Low, Medium (the default), High,
//              Extra high), a radio set, the default dotted; High chosen: the pill says it, the chat keeps it; at the
//              least window the message box's bar (the model's and the thinking pills) has no item over another or
//              past it; the model without levels again: the pill gone
//   TURN       a message goes to openrouter.ai, its answer shown
//   LOCAL      Ollama: no key asked for (its field says none is needed), saved without one, the chat ready
//   GEMINI     Gemini: its key and model alone; saved with a key
//   CUSTOM     another server: the keys go to its address first; an http address on the internet refused
//   EDITED     the Server field of OpenRouter's edited: the dialog's title, its lines (where the key is stored and
//              sent) and its stored-key hint and Remove key name the server typed, never the preset's; Remove key then
//              removes that server's key alone; Ollama's with a remote address typed no longer says nothing leaves
//   MASK       for every provider a key pasted, or typed, is drawn as its preset's public prefix (at its own server),
//              stars and, for a key of 20 characters or more, its last four: no other character of it; a key of 12
//              characters shows none of its own
//   REMOVE     Gemini's stored key removed from the dialog
//   BACK       Anthropic again: Save with nothing typed and no Anthropic key refused; a key pasted saves it, and
//              the thinking pill is back
#include "Probe.h"

#include "../plugin/FakeClaude.h"

#include "editor/ChatItems.h"
#include "editor/Panel.h"
#include "plugin/ChatAgent.h"
#include "plugin/ChatStore.h"
#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

    void settleChat(pg::Processor& p, funkgui::HeadlessHost& host)
    {
        p.chatAgent().waitIdle();
        p.settle();
        host.tick(4);
        host.settle();
    }

    pg::ChatState chat(pg::Processor& p) { return p.chatAgent().chatState(); }

    std::optional<funkgui::A11yItem> a11yTitled(const funkgui::HeadlessHost& host, const std::string& title)
    {
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.title == title)
                return item;
        return std::nullopt;
    }

    bool a11yHoldsKey(const funkgui::HeadlessHost& host, const std::string& key)
    {
        for (const funkgui::A11yItem& item : host.accessibility())
            for (const std::string* s : { &item.title, &item.description, &item.help, &item.value })
                for (std::size_t p = 0; p + 12 <= key.size(); ++p)
                    if (s->find(key.substr(p, 12)) != std::string::npos)
                        return true;
        return false;
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

    // The key dialog as a screen reader has it: its title and its lines.
    std::optional<funkgui::A11yItem> dialogGroup(const funkgui::HeadlessHost& host)
    {
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.role == funkgui::a11yrole::group)
                return item;
        return std::nullopt;
    }

    // What of the key the field draws in clear (every character but the stars).
    std::string clearOf(const std::string& shown)
    {
        std::string clear;
        for (const char c : shown)
            if (c != '*')
                clear += c;
        return clear;
    }

    int presetIndex(std::string_view id)
    {
        for (std::size_t i = 0; i < std::size(pg::kChatPresets); ++i)
            if (id == pg::kChatPresets[i].id)
                return static_cast<int>(i);
        return -1;
    }

    // Opens the Provider pill's menu and chooses `name` in it.
    bool choose(funkgui::HeadlessHost& host, pg::ui::Panel& panel, const char* name)
    {
        clickAt(host, panel.centreOfKeyProvider());
        host.settle();
        const bool chosen = host.pendingMenu() != nullptr && host.chooseMenuItem(name);
        host.settle();
        return chosen;
    }
}

PROBE(gui, providers)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File keys = juce::File(juce::String(probe::outPath("gui-providers-keys")));
    keys.deleteRecursively();
    pg::chatstore::setDirectoryForTests(keys);

    pg::Processor processor;
    auto script = std::make_shared<fake::Script>();
    script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "medium", "high", "xhigh", "max" } } });
    script->gets["/api/v1/models"] = { 200, fake::openAiModels({ { "anthropic/claude-probe", "Claude via OpenRouter", 1762000000, 200000 },
                                                                 { "meta/llama-probe", "Llama via OpenRouter", 1761000000, 131072, true, false },
                                                                 { "openai/gpt-probe-5", "GPT-5 via OpenRouter", 1760000000, 400000, true, true,
                                                                   { "xhigh", "high", "medium", "low", "minimal", "none" }, "medium" } },
                                                               true) };
    script->gets["/v1/models"] = { 200, fake::openAiModels({ { "qwen-probe:32b", "", 1762400000 } }) };
    script->gets["/v1beta/models?pageSize=1000"] = { 200, fake::geminiModels({ { "gemini-3-pro-preview", "Gemini 3 Pro Preview", 0, 1048576 } }) };
    processor.chatAgent().setTransport(std::make_unique<fake::Transport>(script));

    pg::ui::Panel panel(processor.ports(), processor.editorPort());
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    P.le("open.settle_frames", host.settle(), 599.0);

    // ---- ANTHROPIC ---------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("anthropic.opens", panel.keyDialogOpen() && panel.keyDialogPreset() == presetIndex("anthropic") ? 1 : 0, 1);
        P.eq("anthropic.key_alone", !panel.centreOfKeyBase() && !panel.centreOfKeyModel() && panel.keyDialogField() == 0 ? 1 : 0, 1);
        const auto pill = a11yTitled(host, "Provider");
        P.eq("anthropic.pill", pill && pill->role == funkgui::A11yRole::comboBox && pill->value == "Anthropic" ? 1 : 0, 1);
        writePng(P, host, "gui.providers.anthropic.png");
    }

    // ---- MENU --------------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfKeyProvider());
        host.settle();
        const funkgui::MenuRequest* menu = host.pendingMenu();
        int listed = 0, dotted = 0;
        if (menu != nullptr)
            for (const funkgui::MenuItem& item : menu->items)
                for (const pg::ChatPreset& p : pg::kChatPresets)
                    if (item.label == p.name)
                    {
                        ++listed;
                        dotted += item.checked && std::string_view(p.id) == "anthropic" ? 1 : 0;
                    }
        P.eq("menu.lists_every_provider", listed, static_cast<int64_t>(std::size(pg::kChatPresets)));
        P.eq("menu.shown_dotted", dotted, 1);
        writePng(P, host, "gui.providers.menu.png");
        P.eq("menu.choose", host.chooseMenuItem("OpenRouter") ? 1 : 0, 1);
        host.settle();
        P.eq("menu.openrouter", panel.keyDialogPreset() == presetIndex("openrouter") && panel.centreOfKeyBase() && panel.centreOfKeyModel()
                                    && panel.keyDialogField() == 0 ? 1 : 0, 1);
        const auto base = a11yTitled(host, "Server's address");
        P.eq("menu.address", base && base->value == "https://openrouter.ai/api/v1" ? 1 : 0, 1);
    }

    // ---- SAVE --------------------------------------------------------------------------------------------------------------
    const std::string orKey = fake::fakeOpenRouterKey();
    {
        clickAt(host, panel.centreOfKeySave());
        host.settle();
        P.eq("save.refused_without_key", panel.keyDialogOpen() && !panel.keyDialogWhy().empty() && chat(processor).provider.kind == "anthropic" ? 1 : 0, 1);
        host.setClipboard(orKey);
        host.keys("cmd+v");
        host.settle();
        P.eq("save.pasted", panel.keyDialogWhy().empty() ? 1 : 0, 1);
        P.eq("save.never_spoken", a11yHoldsKey(host, orKey) ? 1 : 0, 0);
        host.keys("tab");
        P.eq("save.tab_to_model", panel.keyDialogField(), 2);
        type(host, "anthropic/claude-probe");
        host.settle();
        const auto model = a11yTitled(host, "Model");
        P.eq("save.model_typed", model && model->value == "anthropic/claude-probe" ? 1 : 0, 1);
        writePng(P, host, "gui.providers.openrouter.png");
        {
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(processor.ports(), processor.editorPort());
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            clickAt(paperHost, paper.centreOfKey());
            paperHost.settle();
            paper.chooseKeyPreset(presetIndex("openrouter"));
            paperHost.settle();
            writePng(P, paperHost, "gui.providers.paper.png");
            funkgui::UiPreferences::get().setTheme(0);
        }
        host.keys("return");
        settleChat(processor, host);
        const pg::ChatState s = chat(processor);
        P.eq("save.closes", panel.keyDialogOpen() ? 1 : 0, 0);
        P.eq("save.provider", s.provider.kind == "openai" && s.provider.base == "https://openrouter.ai/api/v1" && s.model == "anthropic/claude-probe" ? 1 : 0, 1);
        P.eq("save.key_bound", pg::chatstore::loadProviderKey("openai", "https://openrouter.ai").value_or("") == orKey
                                   && !pg::chatstore::hasKey() ? 1 : 0, 1);
    }

    // ---- BAR ---------------------------------------------------------------------------------------------------------------
    {
        host.tick(4);
        host.settle();
        const auto model = a11yTitled(host, "Model");
        P.eq("bar.model_named", model && model->value == "Claude via OpenRouter" ? 1 : 0, 1);
        P.eq("bar.no_thinking", a11yTitled(host, "Thinking level") ? 1 : 0, 0);
        writePng(P, host, "gui.providers.bar.png");
        clickAt(host, panel.centreOfModel());
        host.settle();
        const funkgui::MenuRequest* menu = host.pendingMenu();
        bool build = false, plan = false, named = false;
        if (menu != nullptr)
            for (const funkgui::MenuItem& item : menu->items)
            {
                build = build || item.label == "Build with (OpenRouter)";
                plan = plan || has(item.label, "Plan with");
                named = named || item.label == "Claude via OpenRouter";
            }
        P.eq("bar.menu", menu != nullptr && build && !plan && named ? 1 : 0, 1);
        host.cancelMenu();
        host.settle();
    }

    // ---- EFFORT ------------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfModel());
        host.settle();
        P.eq("effort.model_chosen", host.pendingMenu() != nullptr && host.chooseMenuItem("GPT-5 via OpenRouter") ? 1 : 0, 1);
        settleChat(processor, host);
        const auto pill = a11yTitled(host, "Thinking level");
        P.eq("effort.pill_shown", pill && pill->role == funkgui::A11yRole::comboBox && pill->value == "Thinking: Medium" ? 1 : 0, 1);
        clickAt(host, panel.centreOfThinking());
        host.settle();
        const funkgui::MenuRequest* menu = host.pendingMenu();
        std::vector<std::string> labels;
        int radios = 0;
        std::string dotted;
        if (menu != nullptr)
            for (const funkgui::MenuItem& item : menu->items)
            {
                labels.push_back(item.label);
                radios += item.radio ? 1 : 0;
                if (item.checked)
                    dotted += item.label;
            }
        std::string said;
        for (const std::string& l : labels)
            said += l + "; ";
        std::printf("thinking menu: %s\n", said.c_str());
        P.eq("effort.menu_rows", labels == std::vector<std::string>{ "Off", "Minimal", "Low", "Medium (the default)", "High", "Extra high" }
                                     && radios == 6 ? 1 : 0, 1);
        P.eq("effort.menu_default_dotted", dotted == "Medium (the default)" ? 1 : 0, 1);
        P.eq("effort.choose", host.chooseMenuItem("High") ? 1 : 0, 1);
        settleChat(processor, host);
        const auto chosen = a11yTitled(host, "Thinking level");
        P.eq("effort.chosen", chosen && chosen->value == "Thinking: High" && chat(processor).effort == "high" ? 1 : 0, 1);
        writePng(P, host, "gui.providers.effort.png");
        host.resize(500, 300);                          // raised to the least
        host.settle();
        P.eq("effort.least_bar_fits", a11yTitled(host, "Thinking level") && barFits(host, panel) ? 1 : 0, 1);
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
        clickAt(host, panel.centreOfModel());
        host.settle();
        P.eq("effort.model_back", host.pendingMenu() != nullptr && host.chooseMenuItem("Claude via OpenRouter") ? 1 : 0, 1);
        settleChat(processor, host);
        P.eq("effort.pill_gone_again", a11yTitled(host, "Thinking level") ? 1 : 0, 0);
    }

    // ---- TURN --------------------------------------------------------------------------------------------------------------
    {
        script->add([] {
            fake::Response r;
            r.stream = fake::OpenAiSse("anthropic/claude-probe").reasoning("A short answer will do.").text("Hello from OpenRouter.")
                           .finish("stop").usage(30000, 12).done().str();
            return r;
        }());
        const funkgui::Rect in = panel.chatInput().bounds();
        host.click(in.centreX(), in.centreY());
        type(host, "hello there");
        host.keys("return");
        settleChat(processor, host);
        P.eq("turn.sent", script->lastHost("POST /api/v1/chat/completions") == "https://openrouter.ai" ? 1 : 0, 1);
        P.eq("turn.answer", has(panel.transcript(), "Hello from OpenRouter.") ? 1 : 0, 1);
        writePng(P, host, "gui.providers.chat.png");
    }

    // ---- LOCAL -------------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("local.opens_on_current", panel.keyDialogPreset() == presetIndex("openrouter") ? 1 : 0, 1);
        P.eq("local.choose", choose(host, panel, "Ollama (this Mac)") ? 1 : 0, 1);
        P.eq("local.no_remove", panel.centreOfKeyRemove() ? 1 : 0, 0);
        writePng(P, host, "gui.providers.ollama.png");
        clickAt(host, panel.centreOfKeySave());
        settleChat(processor, host);
        const pg::ChatState s = chat(processor);
        P.eq("local.saved", !panel.keyDialogOpen() && s.providerName == "Ollama (this Mac)" && s.ready() && s.model == "qwen-probe:32b" ? 1 : 0, 1);
    }

    // ---- GEMINI ------------------------------------------------------------------------------------------------------------
    const std::string gemKey = fake::fakeGeminiKey();
    {
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("gemini.choose", choose(host, panel, "Google Gemini") ? 1 : 0, 1);
        P.eq("gemini.fields", !panel.centreOfKeyBase() && panel.centreOfKeyModel() ? 1 : 0, 1);
        host.setClipboard(gemKey);
        host.keys("cmd+v");
        host.settle();
        writePng(P, host, "gui.providers.gemini.png");
        host.keys("return");
        settleChat(processor, host);
        const pg::ChatState s = chat(processor);
        P.eq("gemini.saved", !panel.keyDialogOpen() && s.provider.kind == "gemini" && s.model == "gemini-3-pro-preview" ? 1 : 0, 1);
    }

    // ---- CUSTOM ------------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("custom.choose", choose(host, panel, "Other OpenAI-compatible server") ? 1 : 0, 1);
        P.eq("custom.address_first", panel.keyDialogField(), 1);
        host.setClipboard("http://api.example.com/v1");
        host.keys("cmd+v");
        host.keys("return");
        host.settle();
        P.eq("custom.http_refused", panel.keyDialogOpen() && has(panel.keyDialogWhy(), "https://") && chat(processor).provider.kind == "gemini" ? 1 : 0, 1);
        writePng(P, host, "gui.providers.custom.png");
        host.keys("escape");
        host.settle();
    }

    // ---- EDITED ------------------------------------------------------------------------------------------------------------
    {
        std::string why;
        P.eq("edited.keys", pg::chatstore::saveProviderKey("openai", "https://api.openai.com", fake::fakeOpenAiKey(), &why) ? 1 : 0, 1);
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("edited.choose", choose(host, panel, "OpenRouter") ? 1 : 0, 1);
        const auto own = dialogGroup(host);
        P.eq("edited.preset_named", own && own->title == "OpenRouter" && has(own->description, "sent only to openrouter.ai")
                                        && panel.centreOfKeyRemove() ? 1 : 0, 1);
        // Another server typed (as it rests): the dialog is that server's.
        const auto typeBase = [&](const char* base) {
            clickAt(host, *panel.centreOfKeyBase());
            host.keys("cmd+a");
            host.setClipboard(base);
            host.keys("cmd+v");
            host.tick(30);
            host.settle();
        };
        typeBase("https://llm.example.test/v1");
        const auto typed = dialogGroup(host);
        std::printf("edited: %s: %s; hint: %s\n", typed ? typed->title.c_str() : "", typed ? typed->description.c_str() : "",
                    panel.keyDialogHint().c_str());
        P.eq("edited.title_follows_server", typed && typed->title == "llm.example.test" && !a11yTitled(host, "OpenRouter") ? 1 : 0, 1);
        P.eq("edited.lines_follow_server", typed && has(typed->description, "sent only to llm.example.test")
                                               && !has(typed->description, "openrouter.ai") ? 1 : 0, 1);
        P.eq("edited.no_stored_promise", !panel.centreOfKeyRemove() && !has(panel.keyDialogHint(), "Stored") ? 1 : 0, 1);
        // Half an address: no server's promise at all.
        typeBase("https://");
        const auto half = dialogGroup(host);
        P.eq("edited.half_address", half && !has(half->description, "openrouter.ai") && !has(half->description, "llm.example.test")
                                        && !panel.centreOfKeyRemove() ? 1 : 0, 1);
        // A server whose key is stored: its hint and Remove key, which removes that one alone.
        typeBase("https://api.openai.com/v1");
        P.eq("edited.stored_follows_server", has(panel.keyDialogHint(), "Stored") && panel.centreOfKeyRemove() ? 1 : 0, 1);
        if (const auto remove = panel.centreOfKeyRemove())
            clickAt(host, *remove);
        host.tick(2);
        host.settle();
        P.eq("edited.remove_is_the_typed_servers", !pg::chatstore::hasProviderKey("openai", "https://api.openai.com")
                                                       && pg::chatstore::hasProviderKey("openai", "https://openrouter.ai") ? 1 : 0, 1);
        // Ollama's with a remote address typed: no "nothing leaves".
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("edited.ollama", choose(host, panel, "Ollama (this Mac)") ? 1 : 0, 1);
        const auto local = dialogGroup(host);
        P.eq("edited.ollama_local", local && has(local->description, "nothing leaves") ? 1 : 0, 1);
        typeBase("https://llm.example.test/v1");
        const auto remote = dialogGroup(host);
        P.eq("edited.ollama_remote", remote && !has(remote->description, "nothing leaves") && has(remote->description, "llm.example.test")
                                         && !has(panel.keyDialogHint(), "None needed") ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        P.eq("edited.provider_untouched", chat(processor).provider.kind == "gemini" ? 1 : 0, 1);
    }

    // ---- MASK --------------------------------------------------------------------------------------------------------------
    {
        const std::string other = "local-probe-NotARealKey-0123456789";
        for (const pg::ChatPreset& preset : pg::kChatPresets)
        {
            const std::string id = preset.id;
            const std::string key = id == "anthropic" ? fake::fakeKey() : id == "openai" ? fake::fakeOpenAiKey() : id == "openrouter" ? fake::fakeOpenRouterKey()
                                  : id == "gemini" ? fake::fakeGeminiKey() : other;
            clickAt(host, panel.centreOfKey());
            host.settle();
            const bool chosen = choose(host, panel, preset.name);
            if (id == "custom")
            {
                clickAt(host, *panel.centreOfKeyBase());
                host.setClipboard("https://llm.example.test/v1");
                host.keys("cmd+v");
            }
            clickAt(host, panel.centreOfKeyField());
            host.setClipboard(key);
            host.keys("cmd+v");
            host.settle();
            const std::string prefix = id == "custom" ? std::string() : std::string(preset.key);
            const std::string want = (!prefix.empty() && key.rfind(prefix, 0) == 0 ? prefix : std::string()) + key.substr(key.size() - 4);
            const std::string shown = panel.keyDialogShown();
            std::printf("mask: %s: %s\n", id.c_str(), shown.c_str());
            P.eq("mask." + id, chosen && shown.size() == std::min<std::size_t>(key.size(), want.size() + 40) && clearOf(shown) == want && shown.rfind(want.substr(0, want.size() - 4), 0) == 0
                                   && a11yHoldsKey(host, key) == false ? 1 : 0, 1);
            host.keys("escape");
            host.settle();
        }
        // A short key (12 characters): none of it shown, pasted or typed; another server's prefix is no public one.
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("mask.custom_choose", choose(host, panel, "Other OpenAI-compatible server") ? 1 : 0, 1);
        host.setClipboard("https://llm.example.test/v1");
        host.keys("cmd+v,tab");
        host.setClipboard("token-abc123");
        host.keys("cmd+v");
        host.settle();
        P.eq("mask.short_none", clearOf(panel.keyDialogShown()) == "" && panel.keyDialogShown().size() == 12 ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("mask.openrouter_choose", choose(host, panel, "OpenRouter") ? 1 : 0, 1);
        type(host, "sk-or-v1-abcdefgh");
        host.settle();
        P.eq("mask.typed", clearOf(panel.keyDialogShown()) == "sk-or-" ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
    }

    // ---- REMOVE ------------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfKey());
        host.settle();
        const auto remove = panel.centreOfKeyRemove();
        P.eq("remove.shown", remove && panel.keyDialogPreset() == presetIndex("gemini") ? 1 : 0, 1);
        if (remove)
            clickAt(host, *remove);
        host.tick(2);
        host.settle();
        P.eq("remove.gone", !pg::chatstore::hasProviderKey("gemini", "https://generativelanguage.googleapis.com") && !panel.keyDialogOpen() ? 1 : 0, 1);
    }

    // ---- BACK --------------------------------------------------------------------------------------------------------------
    {
        clickAt(host, panel.centreOfKey());
        host.settle();
        P.eq("back.choose", choose(host, panel, "Anthropic") ? 1 : 0, 1);
        clickAt(host, panel.centreOfKeySave());
        host.settle();
        P.eq("back.refused_without_key", panel.keyDialogOpen() && !panel.keyDialogWhy().empty() ? 1 : 0, 1);
        host.setClipboard(fake::fakeKey());
        host.keys("cmd+v,return");
        settleChat(processor, host);
        const pg::ChatState s = chat(processor);
        P.eq("back.anthropic", !panel.keyDialogOpen() && s.provider.kind == "anthropic" && s.hasKey && pg::chatstore::hasKey() ? 1 : 0, 1);
        host.tick(4);
        host.settle();
        P.eq("back.thinking", a11yTitled(host, "Thinking level") ? 1 : 0, 1);
    }
    pg::chatstore::setDirectoryForTests(juce::File());
    return P.finish();
}
