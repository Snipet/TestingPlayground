// PROBE name=plugin.reload timeout=300
//
// plugin.reload: a panel's edit (Compiler.h, "A panel's edit"): a RUN that changes only the panel's files (the .gui, a
// shader's .sc) is applied to the program that plays without compiling it. Two processors at 48 kHz in blocks of 256
// play one program (an effect a 440 Hz sine in, an instrument A4 held), the freezer off; the program keeps a phase of
// its own, so a restart or a crossfade shows in the samples, and writes the fields its displays read. One is edited
// between two blocks, the other never: its twin. Both have their dials off their defaults (gain 1.5, mode 1).
//   GUI     a .gui edit while it sounds (a label, a unit, a display of another field ahead of the meter, whose field so
//           moves to another tap): the status a panel's edit (panelOnly), its program the one that plays and nothing
//           offered; the output the twin's, bit for bit, every block after it; no voice fading (an instrument's); the
//           dials' values as they were; the processor's panel the new one (the label, the unit, the display); the new
//           display's field read in the next block (Engine::setTaps: its tap 0.75), the meter's at its new tap (the
//           gain, 1.5); the panel naming the edit's table, and each frame the table it was read by (before the block:
//           the program's own, after it the edit's); the editor's status line says PANEL UPDATED (gui PNG)
//   TAPS    what a Panel reads of a frame (ui::TapsShown): its own program's and table's fields; the old panel given a
//           frame of the edit's table (the audio thread read it before the editor had the status) holds its last
//           fields; the new panel given a frame of the old table (the status out while the audio thread is idle) reads
//           none; another program's, none
//   SHADER  a .sc edit: a panel's edit too, the shader built again (another binary) when this build has shaderc, the
//           output still the twin's
//   FULL    what is no panel's edit compiles, a new program each: a taper= (a dial's meaning), after which the output
//           is no longer the twin's (the effect crossfades to a program whose phase starts again; the instrument's held
//           key starts again on it, its old voice fading); an option renamed (its values' names), the same files again
//           (a RUN starts the program afresh), a .gui removed and back, the program's code (a comment)
//   FIT     run_program (Claude Desktop's call, on this thread): a panel's edit in its words; a panel taller than the
//           PANEL view (853 x 612 at the editor's default size, the chat open: gui_sandbox's area) in one line, by how
//           much and what is out of view (the shared check, GuiLayout.h's guiFitLine); one that fits, no such line
//   SKIPPED a taper= compiled (Q), then an edit of Q's panel run before poll() takes Q's status, so poll() sees only the
//           edit's: Q's carry is written all the same (the gain 1.5 under the log taper), compare's previous_run is
//           what played before Q (before poll() and after: only panel.gui differs from what plays), and a dial moved
//           under Q (1.0) keeps its value across the next compile (its meaning Q's, not the program's before)
//   WAIT    an edit that adds a display while the audio thread plays blocks of its own, a millisecond apart: when its
//           status is out, the newest frame was read by the edit's table already (editPanel waited for that block). A
//           try in which the audio thread stalled 15 ms or more (a busy machine; the wait rightly takes 20 ms for idle)
//           proves nothing and is made again with another edit (a display taken away, then added), eight at most;
//           skipped, saying so, when every try saw it idle (an emulator's pauses)
//   LISTEN  then, before poll() takes the edit (the processor's panel the one before it, every frame the edit's table's):
//           listen counts no field under a display (their indices are other fields there); after it, the displays'
//           own (mark 0.75, the meter's gain 1)
//   OPEN    a project version opened whose files differ from what plays only in panel.gui compiles (as it was saved)
//   STRAY   an edit that puts a stray { in the .gui (one around the row's elements, one the file ends inside) returns, a
//           panel's edit with the reader's notes on them, the switch still the row's; the program plays on as its twin
//   AB      a RUN of the panel's files alone while an A/B is on compiles. Not covered: a held refine, a stopped
//           program and a .sc that FMinus #includes, which also compile (Compiler::editPanel's first checks)
#include "Probe.h"

#include "Audio.h"

#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"
#include "editor/Panel.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // A program with a phase of its own (a sine: the effect's at 480 Hz over half its input, the voice's at its note's
    // pitch), whose fields `mark` (0.75) and `level` (its gain) the displays read.
    std::string program(const std::string& comment = "")
    {
        const std::string params = comment + "    param float gain [[fminus::range(0.1, 2)]] [[fminus::default(1)]];\n"
                                             "    param int mode [[fminus::range(0, 1)]];\n"
                                             "    float ph;\n    float mark;\n    float level;\n";
        const std::string loop = "        for (int i = 0; i < n; ++i)\n        {\n"
                                 "            const float s = 0.25f * gain[i] * sinf(6.28318531f * ph);\n"
                                 "            ph += step;\n            if (ph >= 1) ph -= 1;\n";
        if constexpr (kInstrument)
            return "module Voice\n{\n" + params + "    float step;\n    bool held;\n"
                   "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; held = true; }\n"
                   "    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + loop +
                   "            left[i] += s;\n            right[i] += s;\n"
                   "            level = gain[i];\n        }\n        mark = 0.75f;\n        return held;\n    }\n};\n";
        else
            return "module Effect\n{\n" + params + "    float step;\n    void init(float rate) { step = 480.0f / rate; }\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n" + loop +
                   "            left[i] = 0.5f * left[i] + s;\n            right[i] = left[i];\n"
                   "            level = gain[i];\n        }\n        mark = 0.75f;\n    }\n};\n";
    }

    const char* const kGui = R"GUI(panel {
    row {
        knob gain label="GAIN"
        switch mode "OFF" "ON"
        meter level min=0 max=2
        shader "glow.sc" inputs="gain" width=200 height=120
    }
}
)GUI";
    // A label, a unit and a display of another field, ahead of the meter (whose field so moves to the second tap): a
    // panel's edit.
    const char* const kGuiEdited = R"GUI(panel {
    row {
        knob gain label="LEVEL" unit="x"
        switch mode "OFF" "ON"
        number mark
        meter level min=0 max=2
        shader "glow.sc" inputs="gain" width=200 height=120
    }
}
)GUI";
    // A taper: what a value means changes (the carry's): a compile.
    const char* const kGuiTaper = R"GUI(panel {
    row {
        knob gain label="LEVEL" unit="x" taper=log
        switch mode "OFF" "ON"
        number mark
        meter level min=0 max=2
        shader "glow.sc" inputs="gain" width=200 height=120
    }
}
)GUI";
    // An option renamed: a value keeps its name across a run (the carry's): a compile.
    const char* const kGuiOptions = R"GUI(panel {
    row {
        knob gain label="LEVEL" unit="x" taper=log
        switch mode "LOW" "HIGH"
        number mark
        meter level min=0 max=2
        shader "glow.sc" inputs="gain" width=200 height=120
    }
}
)GUI";
    // Taller than the PANEL view: the meter under the spacer is out of view until scrolled.
    const char* const kGuiTall = R"GUI(panel {
    column {
        knob gain label="LEVEL" unit="x" taper=log
        switch mode "LOW" "HIGH"
        spacer height=700
        meter level min=0 max=2
    }
}
)GUI";

    const char* const kGlow = "void main()\n{\n    gl_FragColor = vec4(vec3(in_gain * v_uv.x), 1.0);\n}\n";
    const char* const kGlowEdited = "void main()\n{\n    gl_FragColor = vec4(vec3(in_gain * v_uv.y), 1.0);\n}\n";

    pg::Sources filesOf(const std::string& code, const std::string& gui, const std::string& glow)
    {
        pg::Sources f{ { pg::kProgramName, code } };
        if (!gui.empty())
            f.push_back({ "panel.gui", gui });
        if (!glow.empty())
            f.push_back({ "glow.sc", glow });
        return f;
    }

    // A processor as a host drives it, its blocks' output kept.
    struct Host
    {
        pg::Processor p;
        std::vector<float> out;
        std::size_t pos = 0;

        Host()
        {
            p.compiler().setFreezing(false);         // the twins' bits: no frozen copy (its math is Math::Fast)
            p.compiler().setSelfTesting(false);
            p.setRateAndBufferSizeDetails(kFs, kBlock);
            p.prepareToPlay(kFs, kBlock);
        }

        pg::CompileStatus run(const pg::Sources& files)
        {
            p.editorPort().setDraft({ files, 0 });
            p.editorPort().run(files);
            p.settle();
            return p.editorPort().status();
        }

        void blocks(int count)
        {
            for (int b = 0; b < count; ++b)
            {
                juce::AudioBuffer<float> buffer(2, kBlock);
                for (int i = 0; i < kBlock; ++i)
                {
                    const float x = kInstrument ? 0.0f
                                                : static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * 440.0
                                                                                     * static_cast<double>(pos + static_cast<std::size_t>(i)) / kFs));
                    buffer.setSample(0, i, x);
                    buffer.setSample(1, i, x);
                }
                juce::MidiBuffer midi;
                if (kInstrument && pos == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 69, 0.8f), 0);
                p.processBlock(buffer, midi);
                for (int i = 0; i < kBlock; ++i)
                    out.push_back(buffer.getSample(0, i));
                pos += static_cast<std::size_t>(kBlock);
            }
        }

        pg::Meters meters()
        {
            pg::Meters m;
            for (int k = 0; k < 100 && !p.editorPort().meters(m); ++k)
            {
            }
            return m;
        }

        // A dial as a host sets it (its plain value through the program's declaration), and as it reads.
        void setDial(int slot, float plain) { p.getParameters()[slot]->setValueNotifyingHost(pg::normOf(p.control(slot), plain)); }
        float dial(int slot) { return pg::plainOf(p.control(slot), p.getParameters()[slot]->getValue()); }
        float norm(int slot) { return p.getParameters()[slot]->getValue(); }
    };

    // Whether the two outputs are the same from sample `from` on (bit for bit).
    bool same(const std::vector<float>& a, const std::vector<float>& b, std::size_t from)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = from; i < a.size(); ++i)
            if (a[i] != b[i])
                return false;
        return true;
    }

    int tapOf(const pg::PanelSpec& spec, const std::string& source)
    {
        for (const pg::PanelDisplay& d : spec.displays)
            if (d.source == source)
                return d.tap;
        return -1;
    }

    std::string text(const pg::ChatAgent::McpResult& r)
    {
        std::string out;
        if (const juce::Array<juce::var>* blocks = r.content.getArray())
            for (const juce::var& b : *blocks)
                if (b.getProperty("type", {}).toString() == "text")
                    out += (out.empty() ? "" : "\n") + b.getProperty("text", {}).toString().toStdString();
        return out;
    }

    // compare's line on the source, previous_run (A) against the program that plays (B): "" when it has none.
    std::string previousAgainstPlaying(pg::Processor& p)
    {
        auto* in = new juce::DynamicObject();
        in->setProperty("a", "previous_run");
        in->setProperty("b", "playing");
        const std::string all = text(p.chatAgent().mcpCall("compare", juce::var(in)));
        const std::size_t at = all.find("Source, B against A: ");
        return at == std::string::npos ? std::string() : all.substr(at, all.find('\n', at) - at);
    }

    std::string relabelled(const std::string& gui, const std::string& from, const std::string& to)
    {
        std::string out = gui;
        if (const std::size_t at = out.find(from); at != std::string::npos)
            out.replace(at, from.size(), to);
        return out;
    }
}

PROBE(plugin, reload)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    Host edited, twin;
    const pg::Sources first = filesOf(program(), kGui, kGlow);
    const pg::CompileStatus a0 = edited.run(first);
    const pg::CompileStatus b0 = twin.run(first);
    P.eq("start.compiles", a0.ok && b0.ok && !a0.panelOnly ? 1 : 0, 1);
    if (!a0.ok)
        std::printf("%s%s", a0.errors.c_str(), a0.notes.c_str());
    for (Host* h : { &edited, &twin })
    {
        h->setDial(0, 1.5f);                         // the dials off their defaults: a panel's edit leaves them so
        h->setDial(1, 1.0f);
    }
    const float gainNorm = edited.norm(0), modeNorm = edited.norm(1);
    edited.blocks(40);
    twin.blocks(40);
    P.eq("start.twins", same(edited.out, twin.out, 0) ? 1 : 0, 1);
    const bool shaders = !a0.panel.shaders.empty() && a0.panel.shaders[0].id != 0;
    std::printf("the shader %s\n", shaders ? "built" : "not built (no shaderc here): its rows read the notes");

    // ---- GUI: a label, a unit, a display of another field, while it sounds ---------------------------------------------
    {
        const pg::Meters before = edited.meters();
        const int levelWas = tapOf(a0.panel, "level");
        const pg::Sources files = filesOf(program(), kGuiEdited, kGlow);
        const pg::CompileStatus s = edited.run(files);
        P.eq("gui.panel_only", s.ok && s.panelOnly ? 1 : 0, 1);
        P.eq("gui.same_program", static_cast<int64_t>(s.program), static_cast<int64_t>(a0.program));
        P.eq("gui.new_request", s.request > a0.request ? 1 : 0, 1);
        P.eq("gui.nothing_offered", static_cast<int64_t>(edited.p.editorPort().program()), static_cast<int64_t>(a0.program));
        P.eq("gui.sources", s.sources == files && edited.p.editorPort().running() == files ? 1 : 0, 1);
        const pg::PanelSpec spec = edited.p.editorPort().panel();
        P.eq("gui.label", spec.labels[0] == "LEVEL" ? 1 : 0, 1);
        P.eq("gui.unit", std::string(edited.p.editorPort().controls()[0].unit) == "x" ? 1 : 0, 1);
        const int markTap = tapOf(spec, "mark"), levelTap = tapOf(spec, "level");
        P.eq("gui.new_display", markTap >= 0 && levelTap >= 0 && markTap != levelTap ? 1 : 0, 1);
        P.eq("gui.old_display_moved", levelWas >= 0 && levelTap >= 0 && levelTap != levelWas ? 1 : 0, 1);
        P.eq("gui.dials_kept", edited.norm(0) == gainNorm && edited.norm(1) == modeNorm ? 1 : 0, 1);
        P.near("gui.gain_kept", static_cast<double>(edited.dial(0)), 1.5, 1e-5);
        P.near("gui.mode_kept", static_cast<double>(edited.dial(1)), 1.0, 1e-9);
        // The table its displays index: the edit's, named in the panel; the frame before the next block was read by
        // the program's own (the probe's audio is idle as the edit lands: the status is out before a block reads it).
        P.eq("gui.table_named", s.panel.tapTable != 0 && spec.tapTable == s.panel.tapTable ? 1 : 0, 1);
        const pg::Meters idle = edited.meters();
        P.eq("gui.frame_before_own_table", idle.tapTable == 0 && before.tapTable == 0 ? 1 : 0, 1);
        edited.blocks(1);
        twin.blocks(1);
        const pg::Meters m = edited.meters();
        P.eq("gui.engine_plays_it", static_cast<int64_t>(m.program), static_cast<int64_t>(before.program));
        P.eq("gui.frame_of_edit_table", static_cast<int64_t>(m.tapTable), static_cast<int64_t>(s.panel.tapTable));
        P.near("gui.new_field_read", markTap >= 0 ? static_cast<double>(m.taps[markTap]) : -1.0, 0.75, 1e-6);
        P.near("gui.old_field_its_own", levelTap >= 0 ? static_cast<double>(m.taps[levelTap]) : -1.0, 1.5, 1e-5);
        if constexpr (kInstrument)
            P.eq("gui.no_fade", static_cast<int64_t>(m.fading), 0);

        // ---- TAPS: what a Panel reads of those frames (ui::TapsShown) ----------------------------------------------
        if (levelWas >= 0 && markTap >= 0 && levelTap >= 0)
        {
            const uint64_t prog = a0.program, table = s.panel.tapTable;
            // The old panel (the program's own table): its frame; then the edit's table, read before it has the
            // edit's status: its last fields held (the meter's at its old tap, not the new display's 0.75).
            pg::ui::TapsShown old;
            pg::Meters x = before;
            const bool own = old.show(x, prog, 0);
            pg::Meters y = m;
            const bool held = old.show(y, prog, 0);
            P.eq("taps.own", own && x.taps[levelWas] == before.taps[levelWas] ? 1 : 0, 1);
            P.eq("taps.old_panel_holds", held && y.taps[levelWas] == before.taps[levelWas] && y.taps[markTap] == before.taps[markTap] ? 1 : 0, 1);
            // The new panel (the edit's table) given the frame before (the status out with the audio thread idle):
            // none of it; then its own.
            pg::ui::TapsShown now;
            pg::Meters z = before;
            const bool early = now.show(z, prog, table);
            P.eq("taps.new_panel_old_frame_none", !early && z.taps[markTap] == 0.0f && z.taps[levelWas] == 0.0f ? 1 : 0, 1);
            pg::Meters w = m;
            const bool its = now.show(w, prog, table);
            P.eq("taps.new_panel_own", its && w.taps[markTap] == 0.75f && w.taps[levelTap] == m.taps[levelTap] ? 1 : 0, 1);
            // Another program's frame: none, and nothing held for it.
            pg::Meters v = m;
            v.program = prog + 1000;
            const bool other = now.show(v, prog, table);
            P.eq("taps.other_program_none", !other && v.taps[markTap] == 0.0f ? 1 : 0, 1);
        }
        else
            P.eq("taps.displays_found", 0, 1);

        edited.blocks(39);
        twin.blocks(39);
        P.eq("gui.same_samples", same(edited.out, twin.out, 0) ? 1 : 0, 1);
        std::printf("gui: %.1f ms, the notes: %s\n", s.ms, s.notes.empty() ? "none" : s.notes.c_str());
    }

    // ---- SHADER: a .sc edit ------------------------------------------------------------------------------------------
    {
        const pg::CompileStatus before = edited.p.editorPort().status();
        const pg::CompileStatus s = edited.run(filesOf(program(), kGuiEdited, kGlowEdited));
        P.eq("shader.panel_only", s.ok && s.panelOnly ? 1 : 0, 1);
        P.eq("shader.same_program", static_cast<int64_t>(s.program), static_cast<int64_t>(a0.program));
        if (shaders)
            P.eq("shader.rebuilt", !s.panel.shaders.empty() && s.panel.shaders[0].id != 0
                                       && s.panel.shaders[0].binary != before.panel.shaders[0].binary ? 1 : 0, 1);
        else
            P.eq("shader.noted", has(s.notes, "glow.sc:1:1: note:") ? 1 : 0, 1);
        edited.blocks(20);
        twin.blocks(20);
        P.eq("shader.same_samples", same(edited.out, twin.out, 0) ? 1 : 0, 1);
    }

    // ---- FULL: what compiles -----------------------------------------------------------------------------------------
    {
        uint64_t playing = a0.program;
        const auto compiles = [&](const std::string& row, const pg::Sources& files) {
            const pg::CompileStatus s = edited.run(files);
            P.eq(row + "_compiles", s.ok && !s.panelOnly && s.program != playing ? 1 : 0, 1);
            if (!s.ok)
                std::printf("%s: %s", row.c_str(), s.errors.c_str());
            playing = s.program;
        };
        // A taper= is a dial's meaning: a compile, so the program starts again (the effect crossfades to a program whose
        // phase starts from 0, the instrument's held key starts again on it, the old voice fading).
        const std::size_t at = edited.out.size();
        compiles("full.taper", filesOf(program(), kGuiTaper, kGlowEdited));
        edited.blocks(1);
        twin.blocks(1);
        if constexpr (kInstrument)
            P.ge("full.taper_fades", static_cast<double>(edited.meters().fading), 1.0);
        edited.blocks(9);
        twin.blocks(9);
        P.eq("full.taper_restarts", same(edited.out, twin.out, at) ? 0 : 1, 1);
        compiles("full.options", filesOf(program(), kGuiOptions, kGlowEdited));
        compiles("full.same_files", filesOf(program(), kGuiOptions, kGlowEdited));
        compiles("full.gui_removed", filesOf(program(), "", kGlowEdited));
        compiles("full.gui_back", filesOf(program(), kGuiOptions, kGlowEdited));
        compiles("full.code", filesOf(program("    // a comment: the program's own code changed\n"), kGuiOptions, kGlowEdited));
    }

    // ---- FIT: run_program's words and its line on the layout ---------------------------------------------------------
    {
        pg::ChatAgent& agent = edited.p.chatAgent();
        const auto runProgram = [&](const pg::Sources& files) {
            edited.p.editorPort().setDraft({ files, 0 });
            const pg::ChatAgent::McpResult r = agent.mcpCall("run_program", juce::var(new juce::DynamicObject()));
            edited.p.settle();
            return std::make_pair(r.isError, text(r));
        };
        const std::string code = program("    // a comment: the program's own code changed\n");
        const auto [tallError, tall] = runProgram(filesOf(code, kGuiTall, kGlowEdited));
        std::printf("run_program, a tall panel:\n%s\n", tall.c_str());
        P.eq("fit.panel_edit_words", !tallError && has(tall, "Applied in ") && has(tall, "as the program plays: only its panel's "
                                                                                       "files changed (panel.gui)") ? 1 : 0, 1);
        P.eq("fit.line", has(tall, "Panel layout: its content is ") && has(tall, " px taller than the PANEL view's area at "
                                                                              "the editor's default size, the chat open (853 x 612), so it scrolls") ? 1 : 0, 1);
        P.eq("fit.out_of_view", has(tall, "; out of view until scrolled: meter level (line 6).") ? 1 : 0, 1);
        const auto [fitsError, fits] = runProgram(filesOf(code, kGuiOptions, kGlowEdited));
        P.eq("fit.fits_no_line", !fitsError && has(fits, "Its interface is panel.gui's") && !has(fits, "Panel layout:") ? 1 : 0, 1);
    }

    // ---- the editor's status line after a panel's edit (a PNG) ----------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(0);
        pg::ui::Panel panel(edited.p.ports(), edited.p.editorPort());
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.settle();
        // Claude's write of panel.gui (the editor merges it, as it does what the chat writes), then Claude's run.
        std::string relabelled = kGuiOptions;
        relabelled.replace(relabelled.find("\"LEVEL\""), 7, "\"DRIVE\"");
        pg::ChatAgent& agent = edited.p.chatAgent();
        auto* write = new juce::DynamicObject();
        write->setProperty("name", "panel.gui");
        write->setProperty("content", juce::String(relabelled));
        P.eq("png.written_by_claude", agent.mcpCall("write_file", juce::var(write)).isError ? 0 : 1, 1);
        host.tick(2);
        host.settle();
        P.eq("png.run", agent.mcpCall("run_program", juce::var(new juce::DynamicObject())).isError ? 0 : 1, 1);
        edited.p.settle();
        const pg::CompileStatus s = edited.p.editorPort().status();
        P.eq("png.panel_only", s.ok && s.panelOnly ? 1 : 0, 1);
        host.tick(2);
        host.settle();
        std::printf("status line: %s\n", panel.statusText().c_str());
        P.eq("png.status_says_panel", has(panel.statusText(), "RUNNING   PANEL UPDATED IN ") ? 1 : 0, 1);
        const std::string path = probe::outPath("plugin.reload.panel_updated.png");
        host.draw();
        P.eq("png.written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // ---- SKIPPED: an edit's status seen, its compile's not -----------------------------------------------------------
    Host skip;
    {
        const pg::Sources p0 = filesOf(program(), kGui, kGlow);
        const pg::CompileStatus s0 = skip.run(p0);
        skip.setDial(0, 1.5f);                       // under the linear taper
        // Q: a taper= (a compile); then an edit of Q's panel alone, run before poll() takes Q's status.
        const pg::Sources q = filesOf(program(), kGuiTaper, kGlow);
        const pg::Sources qEdited = filesOf(program(), relabelled(kGuiTaper, "\"LEVEL\"", "\"DRIVE\""), kGlow);
        pg::EditorPort& port = skip.p.editorPort();
        port.setDraft({ q, 0 });
        port.run(q);
        skip.p.compiler().wait();
        const pg::CompileStatus sq = port.status();
        port.setDraft({ qEdited, 0 });
        port.run(qEdited);
        skip.p.compiler().wait();
        const pg::CompileStatus se = port.status();
        P.eq("skipped.setup", s0.ok && sq.ok && !sq.panelOnly && sq.program != s0.program && se.ok && se.panelOnly
                                  && se.program == sq.program && port.program() == s0.program ? 1 : 0, 1);
        const std::string polledNot = previousAgainstPlaying(skip.p);   // poll() has taken neither
        skip.p.poll();                                                    // ... and now only the edit's status
        P.eq("skipped.program_taken", static_cast<int64_t>(port.program()), static_cast<int64_t>(sq.program));
        P.near("skipped.carry_written", static_cast<double>(skip.dial(0)), 1.5, 1e-4);
        const std::string polled = previousAgainstPlaying(skip.p);
        std::printf("skipped: previous_run against playing, before poll(): %s\n  after: %s\n", polledNot.c_str(), polled.c_str());
        const auto onlyPanel = [](const std::string& line) {
            return has(line, "Source, B against A: ") && has(line, "panel.gui") && !has(line, "program.fm") && !has(line, "glow.sc");
        };
        P.eq("skipped.previous_run_before_poll", onlyPanel(polledNot) ? 1 : 0, 1);
        P.eq("skipped.previous_run", onlyPanel(polled) ? 1 : 0, 1);
        // A dial moved under Q keeps its value across the next compile: its values mean Q's now.
        skip.setDial(0, 1.0f);
        const pg::CompileStatus next = skip.run(filesOf(program("    // a comment\n"), relabelled(kGuiTaper, "\"LEVEL\"", "\"DRIVE\""), kGlow));
        P.eq("skipped.next_compiles", next.ok && !next.panelOnly && next.program != sq.program ? 1 : 0, 1);
        P.near("skipped.moved_dial_kept", static_cast<double>(skip.dial(0)), 1.0, 1e-4);
    }

    // ---- WAIT: an edit's new table read before its status is out, the audio thread playing ---------------------------
    // Then LISTEN: listen while the processor's panel is still the one before the edit (poll() has not taken its status)
    // and every frame is of the edit's table, and again once poll() has.
    {
        using Clock = std::chrono::steady_clock;
        const auto nsNow = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); };
        std::atomic<bool> stop{ false };
        std::atomic<int64_t> longestGap{ 0 };        // ns from a block's end to the next one's start, since reset
        std::atomic<int> played{ 0 };
        std::thread audio([&] {
            juce::AudioBuffer<float> buffer(2, kBlock);
            juce::MidiBuffer midi;
            if constexpr (kInstrument)
                midi.addEvent(juce::MidiMessage::noteOn(1, 69, 0.8f), 0);   // a voice sounds: its fields are the displays'
            int64_t ended = nsNow();
            while (!stop.load())
            {
                const int64_t began = nsNow();
                for (int64_t was = longestGap.load(); began - ended > was && !longestGap.compare_exchange_weak(was, began - ended);)
                {
                }
                buffer.clear();
                skip.p.processBlock(buffer, midi);
                midi.clear();
                ended = nsNow();
                played.fetch_add(1);
                // A millisecond apart: an edit's status out with no wait would most often find the frame before its
                // table, and the gap far under the 20 ms that make the audio thread idle (Compiler::audioIdle).
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        // The row holds whenever the audio thread kept playing: an edit whose wait saw it idle (a gap of 20 ms between
        // two of its blocks, which a busy machine may give a thread that sleeps a millisecond; or the wait's 500 ms
        // spent) rightly sent its status out without the read, and says nothing of the wait: it is tried again, another
        // edit each time (a display added, then taken away), up to eight times. A try whose audio kept playing and
        // whose status came out before the read fails the row.
        // The audio thread playing first: before its first blocks the Engine's last block ended long ago, which the wait
        // rightly takes for idle.
        for (const auto until = Clock::now() + std::chrono::seconds(2); played.load() < 8 && Clock::now() < until;)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pg::EditorPort& port = skip.p.editorPort();
        pg::CompileStatus s;
        pg::Meters m;
        int tries = 0, idle = 0;
        bool read = false, conclusive = false;
        for (; tries < 8 && !conclusive; ++tries)
        {
            // The try before's edit taken here, not as it ended: the last try's stays the processor's pending one, so
            // LISTEN finds the panel before it whether or not any try was conclusive.
            if (tries > 0)
                skip.p.poll();
            std::string more = relabelled(kGuiTaper, "\"LEVEL\"", "\"DRIVE" + std::to_string(tries) + "\"");
            if (tries % 2 == 0)
                more = relabelled(more, "        meter level", "        number ph\n        meter level");
            const pg::Sources files = filesOf(program("    // a comment\n"), more, kGlow);
            longestGap.store(0);
            port.setDraft({ files, 0 });
            port.run(files);
            skip.p.compiler().wait();
            s = port.status();
            m = skip.meters();                       // the newest frame as the status comes out
            // Two blocks more, so that a gap still open as the status came out is counted too.
            const int seen = played.load();
            for (const auto until = Clock::now() + std::chrono::seconds(2); played.load() < seen + 2 && Clock::now() < until;)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const double gapMs = static_cast<double>(longestGap.load()) * 1e-6;
            read = m.program == s.program && m.tapTable == s.panel.tapTable;
            const bool sawIdle = gapMs >= 15.0 || s.ms >= 450.0;
            conclusive = read || !sawIdle;
            std::printf("wait, try %d: the edit out in %.1f ms (its table %llu, the newest frame's %llu), the audio's longest gap "
                        "%.1f ms%s\n", tries + 1, s.ms, static_cast<unsigned long long>(s.panel.tapTable),
                        static_cast<unsigned long long>(m.tapTable), gapMs, conclusive ? "" : ": idle as far as the wait knew, again");
            if (!conclusive)
                ++idle;
        }
        std::printf("wait: %d tr%s, %d of them with the audio thread idle as far as the wait knew\n", tries, tries == 1 ? "y" : "ies", idle);
        P.eq("wait.panel_only", s.ok && s.panelOnly && s.panel.tapTable != 0 ? 1 : 0, 1);
        // Every try idle as far as the wait knew (an emulator's pauses, a machine far too busy): the row says nothing
        // there, and is skipped, saying so.
        const std::string idleWhy = "the audio thread was idle as far as the wait knew (a gap of 15 ms or more between "
                                    "its blocks, Compiler::audioIdle's 20 ms near) in all " + std::to_string(tries) + " tries";
        if (conclusive)
            P.eq("wait.table_read_first", read ? 1 : 0, 1);
        else
            probe::skip(P, "wait.table_read_first", idleWhy);

        // LISTEN: the processor's panel the one before the edit, every frame the edit's table's (whose indices are
        // other fields: a display added moves the meter's to the second tap, one taken away the meter's field into the
        // added one's place): listen counts no field under a display; once poll() has taken the edit, its own.
        const auto listen = [&] {
            auto* in = new juce::DynamicObject();
            in->setProperty("seconds", 0.3);
            return text(skip.p.chatAgent().mcpCall("listen", juce::var(in)));
        };
        const bool stale = skip.p.editorPort().panel().tapTable != s.panel.tapTable;
        const std::string before = listen();
        skip.p.poll();
        const std::string after = listen();
        std::printf("listen, the panel before the edit's: %s\nlisten, the edit's: %s\n", before.c_str(), after.c_str());
        P.eq("listen.panel_before_edit", stale ? 1 : 0, 1);
        if (conclusive)
            P.eq("listen.other_table_no_field", read && !has(before, "Displays:") ? 1 : 0, 1);
        else
            probe::skip(P, "listen.other_table_no_field", "no frame of the edit's table proven read: " + idleWhy);
        P.eq("listen.own_table_fields", has(after, "Displays: ") && has(after, "mark (number) now 0.75") && has(after, "level (meter) now 1")
                                            ? 1 : 0, 1);
        stop.store(true);
        audio.join();
    }

    // ---- OPEN and AB: a version opened, a RUN during an A/B ------------------------------------------------------------
    {
        const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getChildFile(juce::String(pg::product::kEnvPrefix) + "plugin-reload-"
                                                  + juce::String(juce::Time::currentTimeMillis()));
        temp.deleteRecursively();
        pg::projectstore::setDirectoryForTests(temp);
        {
            Host other;
            pg::EditorPort& port = other.p.editorPort();
            const std::string project = "Reload Test";
            std::string why;
            pg::SavedVersion saved;
            const pg::Sources p0 = filesOf(program(), kGui, kGlow);
            const pg::CompileStatus s0 = other.run(p0);
            P.eq("open.saved", s0.ok && port.saveVersion(project, saved, why) && saved.number == 1 ? 1 : 0, 1);
            // What plays then differs from version 1 in panel.gui alone (a panel's edit), and opening it compiles.
            const pg::CompileStatus e = other.run(filesOf(program(), kGuiEdited, kGlow));
            P.eq("open.edit_plays", e.ok && e.panelOnly ? 1 : 0, 1);
            const bool opened = port.openProject(project, 1, why);
            other.p.settle();
            const pg::CompileStatus o = port.status();
            P.eq("open.compiles", opened && o.ok && !o.panelOnly && o.program != e.program && o.sources == p0 ? 1 : 0, 1);
            if (!opened)
                std::printf("open: %s\n", why.c_str());

            // A/B: Current (another program's code: a comment) against version 1; a RUN of its panel alone compiles.
            const pg::Sources current = filesOf(program("    // a comment\n"), kGui, kGlow);
            const pg::CompileStatus c = other.run(current);
            const bool started = c.ok && port.startAb(project, 0, 1, why) && port.ab().on;
            P.eq("ab.started", started ? 1 : 0, 1);
            if (!started)
                std::printf("ab: %s\n", why.c_str());
            const pg::CompileStatus a = other.run(filesOf(program("    // a comment\n"), kGuiEdited, kGlow));
            P.eq("ab.run_compiles", a.ok && !a.panelOnly && a.program != c.program ? 1 : 0, 1);
            port.endAb();
            other.p.settle();
            other.p.releaseResources();
        }
        pg::projectstore::setDirectoryForTests({});
        temp.deleteRecursively();
    }
    // ---- STRAY: a { where an element's name should be, in a panel's edit ------------------------------------------------
    {
        Host h, ht;
        const pg::CompileStatus s0 = h.run(first);
        ht.run(first);
        h.blocks(4);
        ht.blocks(4);
        // A { alone after `row {` (one after a knob would be the knob's block, as a { on the next line is) and one
        // the file ends inside.
        const std::string wrapped = relabelled(kGui, "    row {\n", "    row {\n    {\n");
        const std::string stray = relabelled(wrapped, "    }\n}\n", "    }\n    }\n}\n") + "{\n";
        const pg::CompileStatus s = h.run(filesOf(program(), stray, kGlow));
        std::printf("STRAY: ok %d panelOnly %d program %llu (before %llu)\nerrors:\n%snotes:\n%s", s.ok ? 1 : 0, s.panelOnly ? 1 : 0,
                    static_cast<unsigned long long>(s.program), static_cast<unsigned long long>(s0.program), s.errors.c_str(), s.notes.c_str());
        P.eq("stray.panel_only", s.ok && s.panelOnly && s.program == s0.program ? 1 : 0, 1);
        P.eq("stray.noted", has(s.notes, "panel.gui:3:5: note: a { with no element before it")
                                && has(s.notes, "panel.gui:11:1: note: this { has no }") ? 1 : 0, 1);
        const pg::GuiTree& t = h.p.editorPort().panel().gui;
        int row = -1, sw = -1;
        for (std::size_t i = 0; i < t.nodes.size(); ++i)
        {
            if (t.nodes[i].kind == pg::GuiKind::row && row < 0)
                row = static_cast<int>(i);
            if (t.nodes[i].kind == pg::GuiKind::toggle && sw < 0)
                sw = static_cast<int>(i);
        }
        P.eq("stray.switch_kept", row >= 0 && sw >= 0 && t.nodes[static_cast<std::size_t>(sw)].parent == row ? 1 : 0, 1);
        h.blocks(4);
        ht.blocks(4);
        P.eq("stray.plays_on", same(h.out, ht.out, 0) ? 1 : 0, 1);
    }

    return P.finish();
}
