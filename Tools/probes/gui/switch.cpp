// PROBE name=gui.switch timeout=300
//
// gui.switch: what the editor does after input and on a switch between the Code and the Panel views, headless over a
// real Processor. Its large session: a program of about 270 KB (FMinus), a panel.gui with two animated shaders, a long
// conversation with Claude (about 390 KB of text, 320 blocks), and a projects' folder of about 15 MB (three projects
// of 18 versions each, about 5 MB apiece). It checks:
//   RATE     after any input (a pointer's move, press, drag, release and exit, a wheel, a key, a double-click) the
//            panel keeps the full rate for kInputHold (Panel::wantsFullRate) with nothing moving, and is calm after
//            (once the editor's first check while typing has landed, a change of its own);
//            a release and the pointer's exit nudge the host's pump; a hover's moves at 15 a second keep the frames at
//            60 a second (Tools/probes/common/Cadence.h), where they fell to the calm cadence between moves
//   SWITCH   the chat as wide in both views (a chat the user widened, which gives way to the explorer in CODE), so a
//            switch lays out no block of the conversation (ChatView::blockLayouts); the editors keep their place
//            while the PANEL view hides them (no width set, no line measured again); idle, the chat's look once a
//            second (the key's state) shows nothing of the conversation again (ChatView::shows)
//   POLL     the explorer's poll does not land on the frame that shows it again (projectstore::listings), but
//            kPollAfterShow after it; no switch's frame reads a project's file whole (projectstore::parses), after a
//            save (the store kept what it wrote) or another process's write (the store's Reader reads it, on its own
//            thread, and the explorer shows it after)
// and prints, never checks (TIME), what a switch's frame costs on the message thread (the switch, the frame's tick and
// its draw, wall clock) in both directions with the explorer's poll due, after a save and another process's write; the
// conversation alone, shown again and laid out for another width; the store's listing and a project read whole.
#include "Probe.h"

#include "editor/ChatView.h"
#include "editor/CodeAssist.h"
#include "editor/Columns.h"
#include "editor/Panel.h"
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

#include "Cadence.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace ps = playground::projectstore;

namespace
{
    constexpr int kFunctions = 900;                      // the program's helpers: about 200 KB of code

    // A large program of the product's kind: kFunctions helpers (a few lines each, the kind a big program has), sixteen
    // dials, and a process that calls a few of them.
    std::string bigProgram(int edit)
    {
        std::string s = "// Engine: a large program, for the view switch's measurement (edit " + std::to_string(edit) + ")\n";
        for (int f = 0; f < kFunctions; ++f)
        {
            const std::string n = std::to_string(f);
            s += "\n// shape" + n + ": a soft curve, its knee at k (0..1), and a little of the input under it\n";
            s += "float shape" + n + "(float x, float k)\n{\n";
            s += "    const float a = x * (1.0f + k * 0." + std::to_string(10 + f % 89) + "f);\n";
            s += "    const float b = a / (1.0f + fabsf(a));\n";
            s += "    const float c = b * b * (3.0f - 2.0f * fabsf(b));\n";
            s += "    return 0.5f * (b + c) + 0.25f * x;\n}\n";
        }
        std::string params;
        for (int p = 0; p < 16; ++p)
            params += "    param float p" + std::to_string(p) + " [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        if constexpr (pg::product::kIsInstrument)
            s += "\nmodule Voice\n{\n" + params + "    float ph, step, env;\n    bool held;\n"
                 "    void start(float note, float v) { step = 440.0f * expf(0.0577622650f * (note - 69)) / 48000; held = true; env = 1; }\n"
                 "    void stop() { held = false; }\n"
                 "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                 "        for (int i = 0; i < n; ++i)\n        {\n"
                 "            const float s = 0.2f * env * shape1(shape0(2 * ph - 1, p0[i]), p1[i]);\n"
                 "            ph += step;\n            if (ph >= 1) ph -= 1;\n"
                 "            if (!held) env *= 0.999f;\n"
                 "            left[i] += s;\n            right[i] += s;\n        }\n"
                 "        return held || env > 0.0001f;\n    }\n};\n";
        else
            s += "\nmodule Effect\n{\n" + params +
                 "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                 "        for (int i = 0; i < n; ++i)\n        {\n"
                 "            left[i] = shape1(shape0(left[i], p0[i]), p1[i]);\n"
                 "            right[i] = shape1(shape0(right[i], p0[i]), p1[i]);\n        }\n    }\n};\n";
        return s;
    }

    // Two shaders that read u_time (they move every frame) and a panel.gui that shows them over its knobs.
    const char* const kWaves = "// waves.sc\nvoid main()\n{\n"
                               "    float w = 0.5 + 0.5 * sin(6.2831853 * (v_uv.x * 3.0 - u_time));\n"
                               "    gl_FragColor = vec4(vec3(w * (0.25 + in_out)), 1.0);\n}\n";
    const char* const kRings = "// rings.sc\nvoid main()\n{\n"
                               "    float r = length(v_uv - vec2(0.5));\n"
                               "    float w = 0.5 + 0.5 * sin(40.0 * r - 3.0 * u_time);\n"
                               "    gl_FragColor = vec4(vec3(w * (0.25 + in_out), w * 0.5, 0.3), 1.0);\n}\n";
    const char* const kGui = R"GUI(panel {
    row {
        section "WAVES" { shader "waves.sc" inputs="out" width=220 height=120 }
        section "RINGS" { shader "rings.sc" inputs="out" width=220 height=120 }
    }
    row { knob p0; knob p1; knob p2; knob p3; knob p4; knob p5; knob p6; knob p7 }
    row { knob p8; knob p9; knob p10; knob p11; knob p12; knob p13; knob p14; knob p15 }
}
)GUI";

    pg::Sources engineFiles(int edit)
    {
        return { { pg::kProgramName, bigProgram(edit) }, { "panel.gui", kGui }, { "waves.sc", kWaves }, { "rings.sc", kRings } };
    }

    // A project of `versions` versions of the large program, each a line apart, with its dials and a few presets.
    ps::Project engineProject(const std::string& name, int versions)
    {
        ps::Project p;
        p.name = name;
        std::vector<ps::Dial> dials;
        for (int d = 0; d < 16; ++d)
            dials.push_back({ "p" + std::to_string(d), 0.5f });
        for (int v = 1; v <= versions; ++v)
        {
            ps::Version x;
            x.number = v;
            x.savedMs = 1761580000000 + 60000LL * v;
            x.note = v == 1 ? "first save" : "program.fm +1 -1";
            x.files = engineFiles(v);
            x.dials = dials;
            p.versions.push_back(std::move(x));
        }
        for (const char* preset : { "Soft", "Wide", "Hard" })
            p.presets.push_back({ preset, 1761590000000, versions, dials });
        return p;
    }

    // The conversation of a long session with Claude, as the session keeps it (ChatAgent::saveState): `turns` turns,
    // each the user's message, Claude's thinking, what it did and its answer in markdown with a code block.
    std::string transcript(int turns)
    {
        juce::Array<juce::var> entries;
        const auto entry = [&](const char* kind, const std::string& text) {
            auto* o = new juce::DynamicObject();
            o->setProperty("k", kind);
            o->setProperty("t", juce::String::fromUTF8(text.c_str()));
            entries.add(juce::var(o));
        };
        std::string code;
        for (int l = 0; l < 40; ++l)
            code += "    const float g" + std::to_string(l) + " = shape" + std::to_string(l) + "(x, k * 0.5f);   // the "
                  + std::to_string(l) + "th stage of the engine's chain\n";
        for (int t = 0; t < turns; ++t)
        {
            const std::string n = std::to_string(t);
            entry("user", "Turn " + n + ": make the engine's drive warmer and give the second stage a slower attack, "
                          "then check that the output stays under -1 dBFS with the loud preset and the panel still fits.");
            entry("thinking", "The user wants a warmer drive: lower the knee of shape" + n + " and blend in more of the "
                              "input. The attack of the second stage is p3; a slower one means a longer time constant. "
                              "I should measure the output with the loud preset after the change, then look at the panel "
                              "to see that the two shaders and the sixteen knobs still fit in their rows.");
            entry("tool", "Read program.fm (lines 1-400)");
            entry("tool", "Wrote program.fm (" + std::to_string(3000 + t) + " lines)");
            entry("tool", "Ran the program: it compiles and plays");
            entry("tool", "Measured the output: peak -1.4 dBFS, loudness -14.2 LUFS");
            std::string answer = "## Turn " + n + ": a warmer drive\n\nI lowered the knee of the drive's curve and blended in "
                                 "a quarter of the dry signal, so the **attack** stays clear while the sustain thickens. "
                                 "The second stage's attack (`p3`) now runs from 5 to 80 ms instead of 1 to 20 ms.\n\n"
                                 "- the drive: knee at 0.37, a quarter dry under it\n"
                                 "- the second stage: attack 5 to 80 ms, release unchanged\n"
                                 "- the output: peak -1.4 dBFS with the loud preset, so it stays under -1 dBFS\n\n"
                                 "```\nfloat engine(float x, float k)\n{\n" + code + "    return g0 + g1;\n}\n```\n\n"
                                 "Play a sustained chord with the loud preset and turn `p3` up slowly: the attack should "
                                 "soften without the level jumping. The panel still fits: both shaders in the top row, the "
                                 "knobs in two rows of eight under them.";
            entry("assistant", answer);
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("version", 1);
        root->setProperty("prompt", "");
        root->setProperty("messages", juce::Array<juce::var>());
        root->setProperty("entries", entries);
        return juce::JSON::toString(juce::var(root), true).toStdString();
    }

    double msSince(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    struct Times
    {
        std::vector<double> ms;
        double median() const
        {
            if (ms.empty())
                return 0.0;
            std::vector<double> s = ms;
            std::sort(s.begin(), s.end());
            return s[s.size() / 2];
        }
        double most() const { return ms.empty() ? 0.0 : *std::max_element(ms.begin(), ms.end()); }
    };

    // One switch as the user makes it: the view set (a click's work), then the frame that shows it (its tick and its
    // draw, which EditorHost runs at the next display tick).
    double switchFrame(pg::ui::Panel& panel, funkgui::HeadlessHost& host, bool toPanel)
    {
        const auto t0 = std::chrono::steady_clock::now();
        panel.setPanelView(toPanel);
        host.tick(1);
        host.draw();
        return msSince(t0);
    }

    // `rounds` switches each way, the explorer's poll due at each (3.3 s of frames between them).
    void measureSwitches(pg::ui::Panel& panel, funkgui::HeadlessHost& host, int rounds, Times& toCode, Times& toPanel)
    {
        for (int r = 0; r < rounds; ++r)
        {
            host.tick(200);
            toPanel.ms.push_back(switchFrame(panel, host, true));
            host.tick(200);
            toCode.ms.push_back(switchFrame(panel, host, false));
        }
    }
}

PROBE(gui, switch)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-switch-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    ps::setDirectoryForTests(folder);

    // ---- RATE: the full rate held after input (a panel of its own, Init's: nothing moves) -------------------------------
    {
        pg::Processor calmProcessor;
        pg::ui::Panel calm(calmProcessor.ports(), calmProcessor.editorPort());
        funkgui::HeadlessHost h(calm, 0, 2.0f);
        h.settle();
        calmProcessor.settle();
        h.tick(4);
        h.settle();
        // The editor's first check while typing (CodeAssist), asked once Init's text has rested (the settle above),
        // answers on the compiler's thread whenever that thread has a core, and its outcome changes the status line: a
        // change the Panel marks in the idle call it lands in, so wantsFullRate() is true until the next tick. On a
        // busy machine it lands during the rows below, on any frame, a row's last too; so they begin once it has landed.
        pg::EditorPort& calmPort = calmProcessor.editorPort();
        for (int k = 0; k < 2000 && (calm.codeAssist().checking() || calmPort.compiling()); ++k)
        {
            h.tick(1, 1.0f / 12.0f);                     // a calm editor's cadence
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        P.eq("rate.check_landed", calm.codeAssist().checking() || calmPort.compiling() ? 0 : 1, 1);
        h.settle();
        P.eq("rate.calm_at_first", calm.wantsFullRate() ? 0 : 1, 1);
        const int within = static_cast<int>(std::floor(pg::ui::kInputHold * 60.0)) - 2;   // frames after the input's own
        const int past = static_cast<int>(std::ceil(pg::ui::kInputHold * 60.0)) + 2;
        // Where nothing acts: the status bar, under no control.
        const float x = 0.5f * static_cast<float>(calm.width()), y = static_cast<float>(calm.height()) - 8.0f;
        struct Input
        {
            const char* name;
            std::function<void()> send;
        };
        const std::vector<Input> inputs = {
            { "move", [&] { h.move(x, y); } },
            { "press", [&] { h.press(x, y); } },
            { "drag", [&] { h.moveTo(x + 6.0f, y, 1); } },
            { "release", [&] { h.release(); } },
            { "exit", [&] { calm.pointerExit(); } },
            { "wheel", [&] { h.wheel(x, y, -0.25f); } },
            { "key", [&] { h.keys("escape"); } },
            { "double_click", [&] { h.doubleClick(x, y); } },
        };
        for (const Input& in : inputs)
        {
            h.settle();
            const int nudges = h.log.nudges;
            const uint64_t checks = calmPort.checkStatus().revision, compiles = calmPort.statusRevision();
            const double sentAt = h.nowSeconds();
            in.send();
            const int nudged = h.log.nudges - nudges;
            h.tick(1);                                   // the frame that draws it
            h.tick(within);
            const bool held = calm.wantsFullRate() && !calm.moving();
            const double heldAt = h.nowSeconds() - sentAt;
            h.tick(past - within);
            const bool after = !calm.wantsFullRate();
            P.eq(std::string("rate.") + in.name + "_held", held ? 1 : 0, 1);
            P.eq(std::string("rate.") + in.name + "_calm_after", after ? 1 : 0, 1);
            // What else holds the full rate (Panel::wantsFullRate) besides the input: something moving, a check or a
            // compile running, or one that landed during the row (a change marked in an idle call).
            if (!held || !after)
                std::printf("rate: %s held %d at %.3f s, calm %d at %.3f s (hold %.2f s); moving %d, checking %d, "
                            "compiling %d, a check landed %d, a compile landed %d\n",
                            in.name, held ? 1 : 0, heldAt, after ? 1 : 0, h.nowSeconds() - sentAt, pg::ui::kInputHold,
                            calm.moving() ? 1 : 0, calm.codeAssist().checking() ? 1 : 0, calmPort.compiling() ? 1 : 0,
                            calmPort.checkStatus().revision != checks ? 1 : 0,
                            calmPort.statusRevision() != compiles ? 1 : 0);
            if (std::string(in.name) == "release" || std::string(in.name) == "exit")
                P.ge(std::string("rate.") + in.name + "_nudges", nudged, 1);
        }
        // A hover's moves, 15 a second (every fourth tick of a 60 Hz link): every tick runs a frame.
        h.settle();
        const probe::Cadence hover = probe::runCadence(h, 120, [&](int t) {
            if (t % 4 == 0)
                h.move(x - 200.0f + static_cast<float>(t), y);
        }, [&] { return calm.wantsFullRate(); });
        std::printf("rate: a hover's frames %.1f a second\n", hover.framesPerSecond());
        P.eq("rate.hover_full_rate", hover.frames, hover.ticks);
        h.settle();
    }

    // ---- the session: the projects' folder, the program, the conversation ----------------------------------------------
    {
        int64_t bytes = 0;
        for (const char* name : { "Engine", "Engine B", "Engine C" })
            (void)ps::add(engineProject(name, 18));
        for (const juce::File& f : folder.findChildFiles(juce::File::findFiles, false, "*.fmproject"))
            bytes += f.getSize();
        std::printf("session: projects %.1f MB, the program %.0f KB\n", static_cast<double>(bytes) / 1.0e6,
                    static_cast<double>(bigProgram(0).size()) / 1.0e3);
    }
    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    {
        std::string why;
        P.eq("session.opened", port.openProject("Engine", 0, why) ? 1 : 0, 1);
    }
    processor.settle();
    processor.chatAgent().loadState(transcript(80));
    port.setChatWidth(600);                              // a chat the user widened: it gives way to the explorer in CODE

    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    processor.settle();
    host.tick(4);
    P.eq("session.compiled", port.status().ok ? 1 : 0, 1);
    P.eq("session.interface", panel.hasGui() ? 1 : 0, 1);
    std::printf("session: chat %zu entries, %.0f KB of text\n", processor.chatAgent().chatState().entries.size(),
                static_cast<double>(panel.transcript().size()) / 1.0e3);

    // ---- SWITCH: the chat as wide in both views, nothing of it laid out again; the editors kept in place ----------------
    {
        host.tick(200);
        const float codeChat = panel.chatCard().w;
        const funkgui::Rect codeEditor = panel.editor().textArea();
        const int blocks0 = panel.chatView().blockLayouts();
        panel.setPanelView(true);
        host.tick(1);
        const float panelChat = panel.chatCard().w;
        const funkgui::Rect hiddenEditor = panel.editor().textArea();
        panel.setPanelView(false);
        host.tick(1);
        for (int k = 0; k < 3; ++k)
        {
            panel.setPanelView(true);
            host.tick(1);
            panel.setPanelView(false);
            host.tick(1);
        }
        const funkgui::Rect backEditor = panel.editor().textArea();
        std::printf("switch: the chat %.0f px in CODE (asked 600: it gave way to the explorer), %.0f in PANEL\n", codeChat, panelChat);
        P.eq("switch.chat_gave_way", codeChat < 600.0f ? 1 : 0, 1);
        P.eq("switch.chat_width_kept", panelChat == codeChat ? 1 : 0, 1);
        P.eq("switch.chat_not_laid_out", panel.chatView().blockLayouts() - blocks0, 0);
        const auto same = [](const funkgui::Rect& a, const funkgui::Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
        P.eq("switch.editor_kept_hidden", same(hiddenEditor, codeEditor) ? 1 : 0, 1);
        P.eq("switch.editor_back", same(backEditor, codeEditor) ? 1 : 0, 1);
        // The chat's own rules stand in the PANEL view: its least and its most (the CODE view's reach).
        panel.setPanelView(true);
        host.tick(1);
        const pg::ui::columns::Fit f = panel.columnFit();
        P.eq("switch.chat_reach", f.chatLeast <= panelChat && panelChat <= f.chatMost && f.chatLeast >= pg::ui::columns::kChatMinW
                                      && f.chatMost <= pg::ui::columns::kChatMaxW ? 1 : 0, 1);
        panel.setPanelView(false);
        host.tick(1);
        // Idle, the chat's look once a second (the key's state) shows nothing of the conversation again.
        const int shows0 = panel.chatView().shows();
        host.tick(200);
        P.eq("switch.idle_shows_nothing", panel.chatView().shows() - shows0, 0);
    }

    // ---- POLL: never on the frame that shows the explorer again, never a project's file read whole on the frame --------
    {
        host.tick(200);
        panel.setPanelView(true);
        host.tick(200);                                  // hidden for 3.3 s: the poll is due
        const int listed0 = ps::listings(), parsed0 = ps::parses();
        panel.setPanelView(false);
        host.tick(1);
        P.eq("poll.not_on_the_switch", ps::listings() - listed0, 0);
        host.tick(static_cast<int>(std::ceil(0.25 * 60.0)) + 2);   // kPollAfterShow (Panel.cpp) and two frames
        P.eq("poll.after_the_switch", ps::listings() - listed0, 1);
        P.eq("poll.switch_reads_no_file", ps::parses() - parsed0, 0);
        // A save, then away and back: the store kept what it wrote, so no poll reads the file again.
        pg::Draft d = port.draft();
        d.files[0].text += "\n// a save\n";
        port.setDraft(d);
        host.tick(2);
        pg::SavedVersion saved;
        std::string why;
        P.eq("poll.saved", port.saveVersion("Engine", saved, why) ? 1 : 0, 1);
        const int parsed1 = ps::parses();
        panel.setPanelView(true);
        host.tick(200);
        panel.setPanelView(false);
        host.tick(200);
        P.eq("poll.save_read_no_file", ps::parses() - parsed1, 0);
        // Another process writes a project: no file read on the message thread; the Reader reads it, and the
        // explorer's projects then show its new version.
        const juce::File f = folder.getChildFile("Engine C.fmproject");
        juce::var root = juce::JSON::parse(f.loadFileAsString());
        int newest = 0;
        if (juce::Array<juce::var>* vs = root["versions"].getArray(); vs != nullptr && !vs->isEmpty())
        {
            juce::var v = vs->getLast().clone();
            newest = static_cast<int>(v["number"]) + 1;
            v.getDynamicObject()->setProperty("number", newest);
            vs->add(v);
        }
        f.replaceWithText(juce::JSON::toString(root));
        const int parsed2 = ps::parses(), reader2 = ps::readerParses();
        const auto latestOf = [&](const std::string& name) {
            for (const pg::ProjectEntry& e : port.projectsPolled())
                if (e.name == name)
                    return e.latest;
            return -1;
        };
        panel.setPanelView(true);
        host.tick(200);
        panel.setPanelView(false);
        host.tick(1);
        const int onFrame = ps::parses() - parsed2;
        const std::shared_ptr<ps::Reader> reader = ps::reader();
        for (int k = 0; k < 2000 && (ps::readerParses() == reader2 || !reader->idle()); ++k)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            host.tick(1);
        }
        host.tick(4);
        P.eq("poll.other_not_read_on_the_frame", onFrame, 0);
        P.eq("poll.other_read_by_the_reader", ps::readerParses() - reader2, 1);
        P.eq("poll.other_listed", latestOf("Engine C") == newest ? 1 : 0, 1);
    }

    // ---- TIME: the switch's frame -------------------------------------------------------------------------------------
    {
        Times warm1, warm2;
        measureSwitches(panel, host, 1, warm1, warm2);   // the first of each, apart
        Times toCode, toPanel;
        measureSwitches(panel, host, 6, toCode, toPanel);
        std::printf("TIME switch, chat 600 wide: to PANEL median %.2f ms, most %.2f; to CODE median %.2f ms, most %.2f\n",
                    toPanel.median(), toPanel.most(), toCode.median(), toCode.most());
        // A save in the CODE view (Cmd-S's work), then a switch away at once and back with the poll due: the saved
        // file is the store's to list again.
        {
            pg::SavedVersion saved;
            std::string why;
            pg::Draft d = port.draft();
            d.files[0].text += "\n// saved\n";
            port.setDraft(d);
            host.tick(2);
            const auto t0 = std::chrono::steady_clock::now();
            P.eq("time.saved", port.saveVersion("Engine", saved, why) ? 1 : 0, 1);
            const double saveMs = msSince(t0);
            const double away = switchFrame(panel, host, true);
            host.tick(200);
            const double back = switchFrame(panel, host, false);
            std::printf("TIME save %.1f ms; then to PANEL %.2f ms, to CODE %.2f ms (the poll after the save due)\n", saveMs, away, back);
        }
        // Another process writes a project while the PANEL view shows (its file rewritten, a version more): the
        // switch back to CODE with the poll due.
        {
            host.tick(200);
            (void)switchFrame(panel, host, true);
            const juce::File f = folder.getChildFile("Engine B.fmproject");
            juce::var root = juce::JSON::parse(f.loadFileAsString());
            if (juce::Array<juce::var>* vs = root["versions"].getArray(); vs != nullptr && !vs->isEmpty())
                vs->add(vs->getLast().clone());
            f.replaceWithText(juce::JSON::toString(root));
            host.tick(200);
            const double back = switchFrame(panel, host, false);
            host.tick(200);
            const double steady = switchFrame(panel, host, true);
            std::printf("TIME another process wrote a 5 MB project: to CODE %.2f ms (the poll due), then to PANEL %.2f ms\n",
                        back, steady);
            host.tick(200);
            (void)switchFrame(panel, host, false);
        }
    }
    {
        // The chat at its share (as wide in both views at this size), and the explorer folded (no poll, the editor as
        // wide in both views): what is left of a switch.
        port.setChatWidth(0);
        host.settle();                                   // the Panel follows the port's width (syncColumnWidths)
        Times toCode, toPanel;
        measureSwitches(panel, host, 4, toCode, toPanel);
        std::printf("TIME switch, chat at its share: to PANEL median %.2f ms, most %.2f; to CODE median %.2f ms, most %.2f\n",
                    toPanel.median(), toPanel.most(), toCode.median(), toCode.most());
        panel.setExplorerOpen(false);
        host.settle();
        Times foldedCode, foldedPanel;
        measureSwitches(panel, host, 4, foldedCode, foldedPanel);
        std::printf("TIME switch, explorer folded: to PANEL median %.2f ms, most %.2f; to CODE median %.2f ms, most %.2f\n",
                    foldedPanel.median(), foldedPanel.most(), foldedCode.median(), foldedCode.most());
        panel.setExplorerOpen(true);
        host.settle();
    }
    {
        // The conversation alone (ChatView): shown again unchanged (refreshChat does so once a second), and laid out
        // for another width (what a switch did when the chat's width differed between the views).
        pg::ui::ChatView view(0x20000000u, {});
        const pg::ChatState st = processor.chatAgent().chatState();
        view.setBounds({ 0.0f, 0.0f, 596.0f, 500.0f });
        view.show(st, {});
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 10; ++k)
            view.show(st, {});
        const double same = msSince(t0) / 10.0;
        t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 4; ++k)
        {
            view.setBounds({ 0.0f, 0.0f, k % 2 == 0 ? 512.0f : 596.0f, 500.0f });
            view.show(st, {});
        }
        const double relaid = msSince(t0) / 4.0;
        std::printf("TIME chat: shown again unchanged %.2f ms, laid out for another width %.2f ms (%d blocks)\n", same,
                    relaid, view.blocks());
    }
    {
        // The store alone: the listing with its cache cold and warm, and the presets the explorer asks for.
        ps::setDirectoryForTests(juce::File());
        ps::setDirectoryForTests(folder);
        auto t0 = std::chrono::steady_clock::now();
        (void)ps::list();
        const double warmList = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 10; ++k)
            (void)port.projects();
        const double cachedList = msSince(t0) / 10.0;
        t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 10; ++k)
            (void)port.presets("Engine");
        const double presets = msSince(t0) / 10.0;
        t0 = std::chrono::steady_clock::now();
        (void)ps::load("Engine");
        const double load = msSince(t0);
        const double megabytes = static_cast<double>(folder.getChildFile("Engine.fmproject").getSize()) / 1.0e6;
        std::printf("TIME store: list %.2f ms, projects() cached %.3f ms, presets() cached %.3f ms, a project of %.1f MB read "
                    "whole %.1f ms\n", warmList, cachedList, presets, megabytes, load);
    }

    ps::setDirectoryForTests(juce::File());
    folder.deleteRecursively();
    return P.finish();
}
