// PROBE name=gui.providers timeout=240
//
// gui.providers: the key dialog as the model provider's settings (Panel.h, "key"; plugin/Providers.h), headless
// (funkgui::HeadlessHost), over a real processor whose chat talks to scripted servers (../plugin/FakeClaude.h):
// nothing reaches the network, and the keys' folder is the probe's own. It writes, to <build>/probe-out/:
//   gui.providers.anthropic.png   Anthropic's dialog as it always was, with the Provider pill at its title's right
//   gui.providers.menu.png        the Provider pill's menu: the providers, Anthropic dotted
//   gui.providers.openrouter.png  OpenRouter's: its server's address, the key pasted (masked), a model typed
//   gui.providers.bar.png         the chat on OpenRouter: the model's pill named by its list, no thinking pill
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
//   TURN       a message goes to openrouter.ai, its answer shown
//   LOCAL      Ollama: no key asked for (its field says none is needed), saved without one, the chat ready
//   GEMINI     Gemini: its key and model alone; saved with a key
//   CUSTOM     another server: the keys go to its address first; an http address on the internet refused
//   REMOVE     Gemini's stored key removed from the dialog
//   BACK       Anthropic again: Save with nothing typed and no Anthropic key refused; a key pasted saves it, and
//              the thinking pill is back
#include "Probe.h"

#include "../plugin/FakeClaude.h"

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

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
                                                                 { "meta/llama-probe", "Llama via OpenRouter", 1761000000, 131072, true, false } },
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
