// PROBE name=gui.shaders timeout=240
//
// gui.shaders: a .gui file's shaders (fminus/Shaders.h), built by the compiler with the shaderc the build uses (the
// environment's <prefix>SHADERC, as CTest sets it) and drawn by the Panel, headless, over a real Processor; written to
// <build>/probe-out/:
//   gui.shaders.png        a panel with a shader that builds (SoftRaster draws FunkGui's placeholder for it) and one
//                          that does not (the plugin's placeholder, saying why)
//   gui.shaders.gpu.png    the same frame drawn on the GPU (editor/Picture.h): the shaders drawn
//   gui.shaders.picture.png, gui.shaders.picture-soft.png   view_panel's picture of the panel, on the GPU and through
//                          the fallback (SoftRaster, forced by the test hook)
// and checks:
//   PROMPT    the system prompt's shader example is there, and it builds
//   BUILD     a program whose panel.gui has a shader of its own file runs with no note; the shader's binary is in the
//             status's panel, under its bytes' hash; run again, the same (the cache)
//   ERRORS    a shader that does not compile is a note on its own file's line (the name it does not know found in
//             the file), and the program still plays; a shader of no file, and a fifth one, are notes of the .gui file
//   PANEL     the Panel registers the built shader with FunkGui (shaders::has) and shows it over its cell; its inputs
//             follow a parameter's 0..1 and the output's level; a shader that reads u_time moves every frame (never
//             settled), one that does not settles; a shader that was not built says why (to a screen reader too)
//   PICTURE   the panel's frame drawn for a picture (picture::draw) on the GPU on this Mac, at SoftRaster's size; in the
//             still shader's cell SoftRaster's image is FunkGui's striped placeholder and the GPU's is not, but the
//             shader's colours (its gradient's ends, from in_cutoff); away from the shaders' cells the two images agree
//             within FunkGui's bounds (gpu/GpuPicture.h); the headless GPU context the picture started is held, kept
//             by picturesIdle within kPictureHoldSeconds and let go after; view_panel's picture (picturePanel) drawn
//             by the GPU, its text saying so; with the GPU forced off (picture::forceSoftRasterForTests), a picture
//             still, its text saying each shader shows as a striped placeholder, and why
#include "Probe.h"

#include "editor/GuiItems.h"
#include "editor/Panel.h"
#include "editor/Picture.h"
#include "fminus/Shaders.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/gpu/GpuPicture.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

    // An image's pixels in a rect of logical px (at `dpi`), inset by `inset` logical px; the whole image's otherwise.
    struct Px
    {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    Px pixelsOf(const funkgui::Image& img, const funkgui::Rect& r, float dpi, float inset)
    {
        Px p;
        p.x0 = std::clamp(static_cast<int>(std::ceil((r.x + inset) * dpi)), 0, img.w);
        p.y0 = std::clamp(static_cast<int>(std::ceil((r.y + inset) * dpi)), 0, img.h);
        p.x1 = std::clamp(static_cast<int>(std::floor((r.x + r.w - inset) * dpi)), p.x0, img.w);
        p.y1 = std::clamp(static_cast<int>(std::floor((r.y + r.h - inset) * dpi)), p.y0, img.h);
        return p;
    }

    const uint8_t* at(const funkgui::Image& img, int x, int y)
    {
        return img.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.w) + static_cast<std::size_t>(x)) * 4;
    }

    // Whether a pixel is one of SoftRaster's placeholder colours (its fill, stripe or border), within 2 a channel.
    bool placeholderColour(const uint8_t* q)
    {
        for (const funkgui::Col c : { funkgui::shaderPlaceholder::fill, funkgui::shaderPlaceholder::stripe,
                                      funkgui::shaderPlaceholder::border })
            if (std::abs(q[0] - c.r) <= 2 && std::abs(q[1] - c.g) <= 2 && std::abs(q[2] - c.b) <= 2)
                return true;
        return false;
    }

    double placeholderShare(const funkgui::Image& img, const Px& p)
    {
        int n = 0, hits = 0;
        for (int y = p.y0; y < p.y1; ++y)
            for (int x = p.x0; x < p.x1; ++x, ++n)
                hits += placeholderColour(at(img, x, y)) ? 1 : 0;
        return n > 0 ? static_cast<double>(hits) / n : 0.0;
    }

    int channelDiff(const uint8_t* a, const uint8_t* b)
    {
        return std::max({ std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2]) });
    }

    // The mean colour (r, g, b) of the pixels [x0, x1) x [y0, y1).
    std::array<double, 3> meanColour(const funkgui::Image& img, int x0, int x1, int y0, int y1)
    {
        std::array<double, 3> m{};
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x, ++n)
                for (int k = 0; k < 3; ++k)
                    m[static_cast<std::size_t>(k)] += at(img, x, y)[k];
        for (double& v : m)
            v /= std::max(n, 1);
        return m;
    }

    bool writeBytes(const std::string& path, const std::string& bytes)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out) && !bytes.empty();
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

    // ---- PICTURE -------------------------------------------------------------------------------------------------------
    {
        const pg::ui::GuiShaderItem* still = shaderShowing(panel, "still.sc");
        const pg::ui::GuiShaderItem* moving = shaderShowing(panel, "moving.sc");
        constexpr float kDpi = 2.0f;                         // the host's
        const funkgui::PrimList& frame = host.draw();
        const bool heldBefore = funkgui::gpuPictureContextHeld();
        const auto t0 = std::chrono::steady_clock::now();
        const pg::picture::Drawn g = pg::picture::draw(frame);
        const double firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const auto t1 = std::chrono::steady_clock::now();
        const pg::picture::Drawn again = pg::picture::draw(frame);
        const double againMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        const funkgui::Image soft = funkgui::rasterise(frame, funkgui::FontService::get().atlas(), 1);
        std::printf("picture: %s%s, %d x %d (SoftRaster's %d x %d); the first %.1f ms (bgfx started headless), the next %.1f ms\n",
                    g.gpu ? "the GPU" : "SoftRaster: ", g.why.c_str(), g.image.w, g.image.h, soft.w, soft.h, firstMs, againMs);
        P.eq("picture.gpu", g.gpu && again.gpu ? 1 : 0, 1);
        P.eq("picture.size", g.image.w == soft.w && g.image.h == soft.h && soft.w > 0 ? 1 : 0, 1);
        P.eq("picture.held", !heldBefore && funkgui::gpuPictureContextHeld() ? 1 : 0, 1);
        if (still != nullptr && moving != nullptr && g.gpu && g.image.w == soft.w && g.image.h == soft.h)
        {
            const Px cell = pixelsOf(soft, still->cell(), kDpi, 2.0f);
            const double softShare = placeholderShare(soft, cell), gpuShare = placeholderShare(g.image, cell);
            double diff = 0.0;
            int n = 0;
            for (int y = cell.y0; y < cell.y1; ++y)
                for (int x = cell.x0; x < cell.x1; ++x, ++n)
                    diff += channelDiff(at(soft, x, y), at(g.image, x, y));
            diff /= std::max(n, 1);
            // still.sc: mix((0.1, 0.1, 0.2), (1.0, 0.6, 0.2), in_cutoff * v_uv.x), in_cutoff 0.25: from (26, 26, 51) at
            // its left edge to (83, 57, 51) at its right, in 0..255.
            const int w8 = std::max((cell.x1 - cell.x0) / 16, 1), ym0 = cell.y0 + (cell.y1 - cell.y0) / 3, ym1 = cell.y1 - (cell.y1 - cell.y0) / 3;
            const std::array<double, 3> left = meanColour(g.image, cell.x0, cell.x0 + w8, ym0, ym1);
            const std::array<double, 3> right = meanColour(g.image, cell.x1 - w8, cell.x1, ym0, ym1);
            std::printf("picture: the still shader's cell %d x %d px: placeholder colours %.1f%% in SoftRaster's, %.1f%% in the "
                        "GPU's; the mean difference %.1f; its left (%.0f, %.0f, %.0f), its right (%.0f, %.0f, %.0f)\n",
                        cell.x1 - cell.x0, cell.y1 - cell.y0, 100.0 * softShare, 100.0 * gpuShare, diff, left[0], left[1],
                        left[2], right[0], right[1], right[2]);
            P.ge("picture.soft_placeholder", softShare, 0.95);
            P.le("picture.gpu_not_placeholder", gpuShare, 0.02);
            P.ge("picture.gpu_differs", diff, 10.0);
            const auto near = [](const std::array<double, 3>& c, double r, double gr, double b) {
                return std::fabs(c[0] - r) <= 8.0 && std::fabs(c[1] - gr) <= 8.0 && std::fabs(c[2] - b) <= 8.0;
            };
            // The gradient's ends, averaged over a sixteenth of the cell's width: within 8 of the shader's colours there
            // (the band's own slope is 3.6 across it).
            P.eq("picture.gpu_shader_colours", near(left, 26.0, 26.0, 51.0) && near(right, 81.0, 56.0, 51.0) ? 1 : 0, 1);

            // Away from the shaders' cells (grown by 2 logical px): FunkGui's bounds for the GPU against SoftRaster at
            // one sample a pixel, no channel off by more than 4 and at most 1% of the pixels by more than 2.
            const auto inCell = [&](int x, int y) {
                for (const pg::ui::GuiShaderItem* sh : { still, moving })
                {
                    const funkgui::Rect& r = sh->cell();
                    const float fx = (static_cast<float>(x) + 0.5f) / kDpi, fy = (static_cast<float>(y) + 0.5f) / kDpi;
                    if (fx >= r.x - 2.0f && fx < r.x + r.w + 2.0f && fy >= r.y - 2.0f && fy < r.y + r.h + 2.0f)
                        return true;
                }
                return false;
            };
            int worst = 0, over2 = 0, over4 = 0, rest = 0;
            for (int y = 0; y < soft.h; ++y)
                for (int x = 0; x < soft.w; ++x)
                {
                    if (inCell(x, y))
                        continue;
                    ++rest;
                    const int d = channelDiff(at(soft, x, y), at(g.image, x, y));
                    worst = std::max(worst, d);
                    over2 += d > 2 ? 1 : 0;
                    over4 += d > 4 ? 1 : 0;
                }
            std::printf("picture: away from the shaders, %d px: the largest difference %d, %d over 2, %d over 4\n", rest, worst,
                        over2, over4);
            P.le("picture.rest_over_2_share", static_cast<double>(over2) / std::max(rest, 1), 0.01);
            P.eq("picture.rest_over_4", over4, 0);
        }
        P.eq("picture.gpu_png", g.image.w > 0 && funkgui::writePng(g.image, probe::outPath("gui.shaders.gpu.png").c_str()) ? 1 : 0, 1);

        // The hold: kept while pictures come, let go once they stop (the processor's timer calls picturesIdle).
        pg::picturesIdle();
        P.eq("picture.hold_kept", funkgui::gpuPictureContextHeld() ? 1 : 0, 1);
        pg::picturesIdle(0.0);
        P.eq("picture.hold_released", funkgui::gpuPictureContextHeld() ? 0 : 1, 1);

        // view_panel's picture: on the GPU (bgfx started again), and through the fallback.
        const pg::PanelPicture onGpu = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        std::printf("view_panel: %s\n", onGpu.about.c_str());
        P.eq("picture.panel_gpu", !onGpu.png.empty() && onGpu.gpu && has(onGpu.about, "Drawn by the GPU as the user's screen draws it, shaders included")
                                      && funkgui::gpuPictureContextHeld() ? 1 : 0, 1);
        P.eq("picture.panel_png", writeBytes(probe::outPath("gui.shaders.picture.png"), onGpu.png) ? 1 : 0, 1);
        pg::picture::forceSoftRasterForTests(true);
        const pg::PanelPicture onSoft = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        pg::picture::forceSoftRasterForTests(false);
        std::printf("view_panel, the GPU forced off: %s\n", onSoft.about.c_str());
        P.eq("picture.fallback_png", !onSoft.png.empty() && !onSoft.gpu && onSoft.width == onGpu.width && onSoft.height == onGpu.height ? 1 : 0, 1);
        P.eq("picture.fallback_note", has(onSoft.about, "Drawn without the GPU (forced by a test): each shader shows as a striped placeholder") ? 1 : 0, 1);
        P.eq("picture.fallback_differs", onSoft.png != onGpu.png ? 1 : 0, 1);
        writeBytes(probe::outPath("gui.shaders.picture-soft.png"), onSoft.png);
        pg::picturesIdle(0.0);
    }
    return P.finish();
}
