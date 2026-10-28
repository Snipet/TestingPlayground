// PROBE name=gui.shaders timeout=240
//
// gui.shaders: a .gui file's shaders (fminus/Shaders.h), built by the compiler with the shaderc the build uses (the
// environment's <prefix>SHADERC, as CTest sets it) and drawn by the Panel, headless, over a real Processor; written to
// <build>/probe-out/:
//   gui.shaders.png        a panel with a shader that builds (SoftRaster draws FunkGui's placeholder for it) and one
//                          that does not (the plugin's placeholder, saying why)
// and checks:
//   PROMPT    the system prompt's shader example is there, and it builds
//   BUILD     a program whose panel.gui has a shader of its own file runs with no note; the shader's binary is in the
//             status's panel, under its bytes' hash; run again, the same (the cache)
//   ERRORS    a shader that does not compile is a note on its own file's line (the name it does not know found in
//             the file), and the program still plays; a shader of no file, and a fifth one, are notes of the .gui file
//   PANEL     the Panel registers the built shader with FunkGui (shaders::has) and shows it over its cell; its inputs
//             follow a parameter's 0..1 and the output's level; a shader that reads u_time moves every frame (never
//             settled), one that does not settles; a shader that was not built says why (to a screen reader too)
#include "Probe.h"

#include "editor/GuiItems.h"
#include "editor/Panel.h"
#include "fminus/Shaders.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    std::string program()
    {
        const std::string params = "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.25)]];\n"
                                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    float env;\n";
        if constexpr (kInstrument)
            return "module Voice\n{\n" + params + "    float ph, step;\n    bool held;\n"
                   "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; held = true; }\n"
                   "    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            const float s = 0.25f * sinf(6.28318531f * ph);\n"
                   "            ph += step;\n            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n        }\n"
                   "        env = 0.5f;\n        return held;\n    }\n};\n";
        else
            return "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        env = 0.5f;\n    }\n};\n";
    }

    // A still shader (no u_time) and a moving one, of the prompt's kind.
    const char* const kStill = "// still.sc\n"
                               "void main()\n"
                               "{\n"
                               "    vec3 c = mix(vec3(0.1, 0.1, 0.2), vec3(1.0, 0.6, 0.2), in_cutoff * v_uv.x);\n"
                               "    gl_FragColor = vec4(c, 1.0);\n"
                               "}\n";
    const char* const kMoving = "// moving.sc\n"
                                "void main()\n"
                                "{\n"
                                "    float w = 0.5 + 0.5 * sin(6.2831853 * (v_uv.x * 3.0 - u_time));\n"
                                "    gl_FragColor = vec4(vec3(w * in_out), 1.0);\n"
                                "}\n";
    // Line 5 names what is not there.
    const char* const kBroken = "// broken.sc\n"
                                "void main()\n"
                                "{\n"
                                "    vec3 c = vec3(v_uv, 0.5);\n"
                                "    gl_FragColor = vec4(c, 1.0) + undefinedThing;\n"
                                "}\n";

    const char* const kGui = R"GUI(panel {
    row {
        section "STILL" { shader "still.sc" inputs="cutoff" width=160 height=90 }
        section "MOVING" { shader "moving.sc" inputs="out" width=160 height=90 }
        section "BROKEN" { shader "broken.sc" width=200 height=90 }
    }
    row { knob cutoff; knob level }
}
)GUI";

    void load(pg::Processor& p, const pg::Sources& files)
    {
        p.editorPort().setDraft({ files, 0 });
        p.editorPort().run(files);
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // The prompt's example: from the line "// glow.sc" to the "}" that closes its main().
    std::string promptExample(const std::string& prompt)
    {
        const std::size_t a = prompt.find("// glow.sc:");
        if (a == std::string::npos)
            return {};
        const std::size_t m = prompt.find("void main()", a);
        const std::size_t b = m == std::string::npos ? std::string::npos : prompt.find("\n}\n", m);
        return b == std::string::npos ? std::string() : prompt.substr(a, b + 3 - a);
    }

    pg::ShaderTools tools()
    {
        pg::ShaderTools t;
        const char* env = std::getenv((std::string(pg::product::kEnvPrefix) + "SHADERC").c_str());
        t.shaderc = env != nullptr ? env : "";
        t.prelude = funkgui::shaders::fragmentPrelude();
        t.varyingDef = funkgui::shaders::varyingDef();
        t.bgfxShaderSh = funkgui::shaders::bgfxShaderSh();
        return t;
    }

    const pg::ui::GuiShaderItem* shaderShowing(const pg::ui::Panel& panel, const std::string& title)
    {
        for (int k = 0; k < 4; ++k)
        {
            const auto& s = static_cast<const pg::ui::GuiShaderItem&>(panel.guiItem(4, k));
            char line[256];
            s.specLine(line, sizeof line);
            if (s.shown() && has(line, title))
                return &s;
        }
        return nullptr;
    }
}

PROBE(gui, shaders)
{
    const funkgui::HeadlessGuiScope scope;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    funkgui::UiPreferences::get().setTheme(0);

    // ---- PROMPT --------------------------------------------------------------------------------------------------------
    const pg::ShaderTools t = tools();
    P.eq("prompt.shaderc", t.usable() ? 1 : 0, 1);
    {
        const std::string example = promptExample(pg::ChatAgent::systemPrompt());
        P.eq("prompt.example_there", example.empty() ? 0 : 1, 1);
        const pg::ShaderBuild b = pg::buildShader("glow.sc", example, { "cutoff", "out" }, t);
        if (!b.ok())
            std::printf("the prompt's example:\n%s", b.error.c_str());
        P.eq("prompt.example_builds", b.ok() ? 1 : 0, 1);
        std::string why;
        P.eq("prompt.example_taken", b.ok() && funkgui::shaders::set(pg::shaderId(b.binary), b.binary.data(),
                                                                       static_cast<uint32_t>(b.binary.size()), why) ? 1 : 0, 1);
        if (!why.empty())
            std::printf("FunkGui: %s\n", why.c_str());
    }

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    processor.prepareToPlay(48000.0, 256);
    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    {
        const funkgui::Point at = panel.centreOfView(true);
        host.click(at.x, at.y);
        host.settle();
    }

    // ---- BUILD and ERRORS ----------------------------------------------------------------------------------------------
    const pg::Sources files{ { pg::kProgramName, program() }, { "panel.gui", kGui }, { "still.sc", kStill },
                             { "moving.sc", kMoving }, { "broken.sc", kBroken } };
    load(processor, files);
    processor.settle();
    host.tick(2);
    host.settle(60);
    const pg::CompileStatus s = port.status();
    std::printf("notes:\n%s", s.notes.c_str());
    P.eq("build.plays", s.ok ? 1 : 0, 1);
    P.eq("build.three", static_cast<int64_t>(s.panel.shaders.size()), 3);
    const bool built = s.panel.shaders.size() == 3 && !s.panel.shaders[0].binary.empty() && !s.panel.shaders[1].binary.empty();
    P.eq("build.binaries", built ? 1 : 0, 1);
    P.eq("build.ids", built && s.panel.shaders[0].id == pg::shaderId(s.panel.shaders[0].binary) && s.panel.shaders[0].id != 0 ? 1 : 0, 1);
    P.eq("build.no_note_for_good_ones", has(s.notes, "still.sc") || has(s.notes, "moving.sc") ? 0 : 1, 1);
    P.eq("errors.broken_note", has(s.notes, "broken.sc:5:1: note: the shader does not compile:")
                                   && has(s.notes, "undefinedThing") ? 1 : 0, 1);
    P.eq("errors.broken_unbuilt", s.panel.shaders.size() == 3 && s.panel.shaders[2].binary.empty() ? 1 : 0, 1);
    {
        load(processor, files);                          // again: the cache's builds, the same ids
        processor.settle();
        const pg::CompileStatus again = port.status();
        P.eq("build.cached_same", again.panel.shaders.size() == 3 && built
                                      && again.panel.shaders[0].id == s.panel.shaders[0].id ? 1 : 0, 1);
        host.tick(2);
    }
    {
        pg::Processor other;
        const std::string five = "shader \"a.sc\"\nshader \"a.sc\"\nshader \"a.sc\"\nshader \"a.sc\"\nshader \"a.sc\"\n"
                                 "shader \"nofile.sc\"\nknob cutoff\nknob level\n";
        load(other, { { pg::kProgramName, program() }, { "panel.gui", five }, { "a.sc", kStill } });
        other.settle();
        const pg::CompileStatus o = other.editorPort().status();
        P.eq("errors.fifth", has(o.notes, "panel.gui:5:1: note: a panel draws 4 shaders at most") ? 1 : 0, 1);
        P.eq("errors.no_file", has(o.notes, "panel.gui:6:8: note: there is no file nofile.sc") ? 1 : 0, 1);
    }

    // ---- PANEL ---------------------------------------------------------------------------------------------------------
    {
        const pg::ui::GuiShaderItem* still = shaderShowing(panel, "still.sc");
        const pg::ui::GuiShaderItem* moving = shaderShowing(panel, "moving.sc");
        const pg::ui::GuiShaderItem* broken = shaderShowing(panel, "broken.sc");
        P.eq("panel.shown", still != nullptr && moving != nullptr && broken != nullptr ? 1 : 0, 1);
        if (still != nullptr && moving != nullptr && broken != nullptr && built)
        {
            P.eq("panel.registered", still->program() == s.panel.shaders[0].id && funkgui::shaders::has(still->program()) ? 1 : 0, 1);
            P.eq("panel.broken_says_why", broken->program() == 0 && has(broken->why(), "does not compile") ? 1 : 0, 1);
            host.tick(2);
            P.near("panel.input_param", static_cast<double>(still->inputs()[0]), 0.25, 1e-3);
            P.eq("panel.still_settles", still->settled() ? 1 : 0, 1);
            P.eq("panel.moving_moves", moving->settled() ? 0 : 1, 1);
            bool a11y = false;
            for (const funkgui::A11yItem& a : host.accessibility())
                a11y = a11y || (a.title == "Shader broken.sc" && has(a.value, "does not compile"));
            P.eq("panel.a11y_why", a11y ? 1 : 0, 1);

            // The output's level reaches the moving one: an instrument plays a note, an effect passes a tone.
            juce::AudioBuffer<float> buffer(2, 256);
            double ph = 0.0;
            for (int b = 0; b < 40; ++b)
            {
                juce::MidiBuffer midi;
                if (kInstrument && b == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(127)), 0);
                for (int i = 0; i < 256; ++i)
                {
                    const float v = kInstrument ? 0.0f : static_cast<float>(0.5 * std::sin(ph));
                    ph += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
                    buffer.setSample(0, i, v);
                    buffer.setSample(1, i, v);
                }
                processor.processBlock(buffer, midi);
                host.tick(1);
            }
            P.ge("panel.input_level", static_cast<double>(moving->inputs()[0]), 0.2);
        }
    }
    {
        const std::string path = probe::outPath("gui.shaders.png");
        host.draw();
        P.eq("png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }
    return P.finish();
}
