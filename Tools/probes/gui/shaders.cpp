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
//   gui.shaders.text.png, gui.shaders.text.moved.png   TEXT's panel on the GPU: text, numbers and a gauge of shapes
// and checks:
//   PROMPT    the system prompt's shader examples are there, and they build (glow.sc, gauge.sc, and dial.sc, the
//             pointer's: its .gui line read, its pointer inputs built)
//   ANIMATES  shaderAnimates: u_time and u_viewSize as words of the code animate; in a // comment (one a backslash
//             continues too) or a /* */ one, or part of a longer name, they do not; the probe's still shader names
//             u_time in a comment only, its moving one reads it, and so does the prompt's example
//   BUILD     a program whose panel.gui has a shader of its own file runs with no note; the shader's binary is in the
//             status's panel, under its bytes' hash; run again, the same (the cache); the renderer checked it while the
//             program compiled (shaders::check), so the Panel's shaders::set() runs Metal's compiler not once: the
//             two shaders unregistered before the Panel's two frames and registered after them, the count it reads
//             moving by one with a set() of a code nobody checked (PROMPT's example)
//   ERRORS    a shader that does not compile is a note on its own file's line (the name it does not know found in
//             the file), and the program still plays; a shader of no file, and a fifth one, are notes of the .gui file
//   PANEL     the Panel registers the built shader with FunkGui (shaders::has) and shows it over its cell; its inputs
//             follow a parameter's 0..1 and the output's level; a shader that reads u_time moves every frame (never
//             settled, the panel asking for every frame), one that names it only in a comment settles; a shader that
//             was not built says why (to a screen reader too)
//   PICTURE   the panel's frame drawn for a picture (picture::draw) on the GPU on this Mac, at SoftRaster's size; in the
//             still shader's cell SoftRaster's image is FunkGui's striped placeholder and the GPU's is not, but the
//             shader's colours (its gradient's ends, from in_cutoff); away from the shaders' cells the two images agree
//             within FunkGui's bounds (gpu/GpuPicture.h); the headless GPU context the picture started is held, kept
//             by picturesIdle within kPictureHoldSeconds and let go after; view_panel's picture (picturePanel) drawn
//             by the GPU, its text saying so; with the GPU forced off (picture::forceSoftRasterForTests), a picture
//             still, its text saying each shader shows as a striped placeholder, and why. On Linux, where FunkGui draws
//             no GPU picture (common/Pictures.h), the pictures are SoftRaster's, their text saying why, and no GPU
//             context is held; the rows on the GPU's pixels are skipped, saying so (probe::skip), and so are TEXT's
//   DRAW      a panel whose one shader is still asks for no frame while nothing changes, and for one when the parameter
//             it reads moves with no input (Panel.h "Unchanged frames"): the shader's inputs ask, the parameter's knob
//             being on a page not shown (a hidden item's key is not watched) and its dial hidden in the PANEL view; the
//             move keeps the full rate (the shader unsettled) for a quarter second, then the panel is calm
//   POINTER   a shader whose inputs are the pointer over it (pointer.x, .hover, .down) and whose y= drags cutoff: at
//             rest its inputs 0; the pointer coming over it a frame, its place and hover its inputs; held still, no frame
//             asked between events; a press its down, a relative drag up 50 px a quarter of cutoff, the release its
//             down 0; on the GPU its picture lit by hover and by the press (skipped where there are no GPU pictures)
//   CACHE     the process's cache and the disk's (Shaders.h, "The cache"; the disk's folder under CTest the sandbox's,
//             shaderCacheFolder): a second instance runs no shaderc (its builds from memory, the same panel); a restart
//             (the memory let go) runs none either, reading both from the disk; a changed shaderc (a copy, its time
//             moved on) builds again where the same one read the disk; a corrupt entry (a byte flipped, cut in half,
//             empty, garbage, another shader's entry under its name) is ignored, built again right and written again
//             whole; four threads asking one cache for one new build run shaderc once, four caches (as four processes)
//             building one at once leave one whole entry, no temporary file; a running shaderc holds none of the
//             process's other files (on Windows one open as a shaderc started was inherited by it, until its start
//             listed the handles it takes), so files written and closed while builds start are renamed and removed at
//             once; the cap removes the entries used least
//             lately down to three quarters of it, the newest kept; the folder scanned at the process's first write
//             there, not at each (shaderCacheScans), that scan removing a temporary file left long ago and keeping a
//             new one; RUN's early builds take a shader's inputs at their word (readGuiShaders), and a wrong guess (an
//             input the program lacks) costs one build more (two shaderc runs for one shader), never the result; a
//             compile's four new shaders run shaderc four times and the builds after the compile find them made, and
//             with a renderer's check of 150 ms (a wait) a program of 100 ms or more has every check begun before its
//             compile can have ended and is past both in less than the two's sum (the early builds run while FMinus
//             compiles, not before it or after it: measured up to three times, as a wrong order fails each attempt and a
//             busy machine one at most now and then); a shaderc that does not run (no permission to execute)
//             leaves a note and nothing kept, in memory or on disk, so that once it runs the same cache builds it; a
//             shader that does not compile is kept in memory (no second shaderc) and never written to disk
//   SIXTEEN   a shader of 16 inputs (kMaxShaderInputs: FunkGui's u_params, 4 vec4s), the program's 17 dials named in
//             its inputs=: the 17th a note, the shader built with no note of its own, reading in15 and in_p16; the
//             Panel's item has the 16 inputs, the first and the 16th the dials' turns
//   TEXT      text and includes (Shaders.h; fminus/ShaderLib.h; editor/ShaderFont.h): the atlas made from FunkGui's
//             bake, every glyph in its cell, ShaderLib.h's numbers the bake's; the pre-pass (a string a vec4 of its
//             codes, a longer one a mat4, the escapes; one too long, a character the font lacks, one with no end and a
//             wrong escape each a note at its line and column; comments and #include lines left alone, line for line);
//             the includes (the library's and the program's .sh files, in turn theirs, each once; a missing one a note at
//             its line, in the shader or in the .sh; a clock read in an included .sh animates the shader, the library
//             reads none); a panel of text.sc (text, numbers, a mat4 string from ink.sh, a .sh of the program's own that
//             includes the library too), dash.sc (a gauge of shapes.sh) and odo.sc (8999001, past 2^23): built with no
//             note, registered with the font's texture, still; text.sc's cell drawn by the GPU against funk/text.sh's
//             arithmetic on the CPU over the same atlas, pixel for pixel (gui.shaders.text.png), and again with its
//             numbers moved by their dials (.text.moved.png); odo.sc's number7() and number() the same pixels as its
//             digits drawn one by one (digit7, text) under them; dash.sc's first and last ticks as wide on both sides of
//             their centre lines, a sweep either way round; the cache's key: a .sh no shader includes changed builds
//             nothing, ink.sh changed builds text.sc alone again; a .sh that does not compile a note on its own line,
//             which the shader that includes it shows as why (its placeholder, its screen reader's value, view_panel's
//             line), an #include of no file a note and no build; the shaders built for another renderer too, Vulkan's
//             SPIR-V (Metal's on Linux, whose own is SPIR-V: text.metal; Direct3D 11's compiler is Windows' alone);
//             PROMPT's gauge example (text.sh and shapes.sh) builds, the renderer takes it, and it is still
//   TIME      printed, never checked: a RUN's compile (a small program, and one of 100 ms or more) alone and with four
//             new shaders, the same four again, on a second instance and after a restart; and four shaders built and
//             checked one after another (as RUN built them before) and at once
#include "Pictures.h"
#include "Probe.h"

#include "dsp/Compiler.h"
#include "dsp/Engine.h"
#include "editor/CodeAssist.h"
#include "editor/GuiItems.h"
#include "editor/Panel.h"
#include "editor/Picture.h"
#include "editor/ShaderFont.h"
#include "fminus/GuiSpec.h"
#include "fminus/ShaderLib.h"
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;

// FunkGui's count of the times shaders::check() and set() ran Metal's compiler (its src/gpu/ShaderLibraryDetail.h;
// 0 off macOS): BUILD checks that the Panel's set() runs none, the compiler's thread having checked the code.
namespace funkgui::shaders::detail
{
    uint64_t metalCompileCount();
}

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
#if defined(__APPLE__)
    constexpr bool kMetal = true;                        // FunkGui's renderer runs Metal's compiler (metalCompileCount)
#else
    constexpr bool kMetal = false;
#endif

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

    // A still shader (it names u_time only in a comment) and a moving one, of the prompt's kind.
    const char* const kStill = "// still.sc: it reads no u_time, so it changes only with its input\n"
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
        section "STILL" { shader "still.sc" inputs="cutoff" width=200 height=120 }
        section "MOVING" { shader "moving.sc" inputs="out" width=200 height=120 }
        section "BROKEN" { shader "broken.sc" width=200 height=120 }
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

    // The prompt's example: from the line "// glow.sc" (or another's) to the "}" that closes its main().
    std::string promptExample(const std::string& prompt, const std::string& file = "glow.sc")
    {
        const std::size_t a = prompt.find("// " + file + ":");
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

    // TIME's four shaders, of the prompt's kind, each with `salt` in a constant that reaches its colour, so that a new
    // salt is a new code to shaderc and to the renderer's check alike.
    // `heavy`: the program with forty more methods in its process' path, a compile of 100 ms or more.
    pg::Sources timedFiles(int salt, bool withShaders, bool heavy)
    {
        std::string text = program();
        if (heavy)
        {
            std::string methods = "    float shape(float x)\n    {\n";
            std::string helpers;
            for (int k = 0; k < 40; ++k)
            {
                const std::string n = std::to_string(k);
                helpers += "    float h" + n + "(float x)\n    {\n        float y = x;\n"
                           "        for (int i = 0; i < 3; ++i)\n            y = 0.5f * y + 0.25f * sinf(y * " + n
                         + ".5f + 0.1f);\n        return y;\n    }\n";
                methods += "        x = h" + n + "(x);\n";
            }
            methods += "        return x;\n    }\n";
            text.insert(text.rfind("};"), helpers + methods);
            const std::size_t at = text.find("env = 0.5f;");
            text.replace(at, 11, "env = shape(0.5f);");
        }
        pg::Sources files{ { pg::kProgramName, text } };
        if (!withShaders)
            return files;
        const std::string k = "(1.0 - " + std::to_string(salt) + ".0e-7)";
        files.push_back({ "panel.gui", "panel { row {\n"
                                       "    shader \"glow.sc\" inputs=\"cutoff out\" width=200 height=120\n"
                                       "    shader \"rings.sc\" inputs=\"level\" width=200 height=120\n"
                                       "    shader \"bars.sc\" inputs=\"out env\" width=200 height=120\n"
                                       "    shader \"sky.sc\" inputs=\"cutoff level\" width=200 height=120\n"
                                       "} row { knob cutoff; knob level } }\n" });
        files.push_back({ "glow.sc", "float ring(vec2 p, float r, float w) { return smoothstep(w, 0.0, abs(length(p) - r)); }\n"
                                     "void main()\n{\n"
                                     "    vec2 p = (v_px - 0.5 * u_size) / u_size.y;\n"
                                     "    float glow = exp(-length(p) * (14.0 - 10.0 * in_cutoff)) * (0.35 + 0.65 * in_out);\n"
                                     "    glow += 0.25 * ring(p, 0.3 + 0.02 * sin(2.0 * u_time), 0.01);\n"
                                     "    vec3 c = mix(vec3(0.05, 0.02, 0.0), vec3(1.0, 0.55, 0.1), clamp(glow, 0.0, 1.0));\n"
                                     "    gl_FragColor = vec4(c * " + k + ", 1.0);\n}\n" });
        files.push_back({ "rings.sc", "void main()\n{\n"
                                      "    vec2 p = v_uv - 0.5;\n"
                                      "    float d = length(p * vec2(u_size.x / u_size.y, 1.0));\n"
                                      "    float r = 0.5 + 0.5 * cos(40.0 * d * (0.5 + in_level) - 3.0 * u_time);\n"
                                      "    vec3 c = mix(vec3(0.02, 0.04, 0.08), vec3(0.3, 0.7, 1.0), r * smoothstep(0.5, 0.0, d));\n"
                                      "    gl_FragColor = vec4(c * " + k + ", 1.0);\n}\n" });
        files.push_back({ "bars.sc", "void main()\n{\n"
                                     "    float n = 16.0;\n"
                                     "    float i = floor(v_uv.x * n);\n"
                                     "    float h = fract(sin(i * 12.9898) * 43758.5453) * in_out + 0.1 * in_env;\n"
                                     "    float on = step(1.0 - v_uv.y, h) * step(0.1, fract(v_uv.x * n));\n"
                                     "    vec3 c = mix(vec3(0.1, 0.1, 0.1), mix(vec3(0.2, 0.9, 0.3), vec3(1.0, 0.2, 0.1), v_uv.y), on);\n"
                                     "    gl_FragColor = vec4(c * " + k + ", 1.0);\n}\n" });
        files.push_back({ "sky.sc", "float noise(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
                                    "void main()\n{\n"
                                    "    vec2 p = v_px / 8.0;\n"
                                    "    float a = mix(noise(floor(p)), noise(floor(p) + 1.0), smoothstep(0.0, 1.0, fract(p.x)));\n"
                                    "    vec3 top = vec3(0.1, 0.2, 0.5) * (0.5 + in_cutoff), low = vec3(1.0, 0.6, 0.3) * in_level;\n"
                                    "    vec3 c = mix(top, low, v_uv.y) + 0.1 * a;\n"
                                    "    gl_FragColor = vec4(c * " + k + ", 1.0);\n}\n" });
        return files;
    }

    // A disk cache's files: its entries (.shader), a writer's temporary ones (.tmp) and their bytes.
    struct Folder
    {
        int entries = 0, temps = 0;
        std::uintmax_t bytes = 0;
    };
    Folder folder(const std::filesystem::path& dir)
    {
        Folder f;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
        {
            std::error_code e;
            if (it->path().extension() == ".shader")
            {
                ++f.entries;
                f.bytes += it->file_size(e);
            }
            else if (it->path().extension() == ".tmp")
                ++f.temps;
        }
        return f;
    }

    // The one entry of a folder that holds one.
    std::filesystem::path onlyEntry(const std::filesystem::path& dir)
    {
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
            if (it->path().extension() == ".shader")
                return it->path();
        return {};
    }

    std::string bytesOf(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // CACHE's still shader with `salt` in a comment (a new code to shaderc, the same output).
    std::string saltedStill(int salt) { return std::string(kStill) + "// " + std::to_string(salt) + "\n"; }

    double median(std::vector<double> v)
    {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
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

    // ---- TEXT's reference: funk/text.sh's arithmetic on the CPU, over the atlas the GPU samples ---------------------------
    namespace lib = pg::shaderlib;

    struct TextRef
    {
        const std::vector<uint8_t>& texels;
        float px;                                        // u_px

        // The atlas bilinear at (x, y) in texels (texel k covering [k, k + 1]), clamped to its edges, 0..1.
        float sample(float x, float y) const
        {
            const auto at = [&](int tx, int ty) {
                tx = std::clamp(tx, 0, lib::kAtlasW - 1);
                ty = std::clamp(ty, 0, lib::kAtlasH - 1);
                return static_cast<float>(texels[static_cast<std::size_t>(ty) * lib::kAtlasW + static_cast<std::size_t>(tx)]) / 255.0f;
            };
            const float sx = x - 0.5f, sy = y - 0.5f;
            const float fx = std::floor(sx), fy = std::floor(sy);
            const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
            const float ax = sx - fx, ay = sy - fy;
            const float top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * ax;
            const float bottom = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * ax;
            return top + (bottom - top) * ay;
        }

        float glyph(float x, float y, float penX, float penY, float scale, int code) const
        {
            const float lx = (x - penX) / scale + static_cast<float>(lib::kPenX), ly = (y - penY) / scale + static_cast<float>(lib::kBaseline);
            if (code <= 32 || lx < 0.0f || ly < 0.0f || lx > static_cast<float>(lib::kCellW) || ly > static_cast<float>(lib::kCellH))
                return 0.0f;
            const int cell = code - 32;
            const float d = sample(static_cast<float>((cell % lib::kColumns) * lib::kCellW) + lx,
                                   static_cast<float>((cell / lib::kColumns) * lib::kCellH) + ly);
            return std::clamp((d - 0.5f) * 2.0f * lib::kSpread * scale / px + 0.5f, 0.0f, 1.0f);
        }

        float text(float x, float y, float atX, float atY, float size, float align, const std::vector<int>& codes) const
        {
            if (std::fabs(y - atY) > size)
                return 0.0f;
            const float scale = size / lib::kBasePx, advance = lib::kAdvance * scale;
            const float n = static_cast<float>(codes.size());
            const float penX = atX - align * n * advance, penY = atY + 0.5f * lib::kCapHeight * scale;
            const float i = std::floor((x - penX) / advance);
            float ink = 0.0f;
            for (int k = -1; k <= 1; ++k)
            {
                const float j = i + static_cast<float>(k);
                if (j >= 0.0f && j < n)
                    ink = std::max(ink, glyph(x, y, penX + j * advance, penY, scale, codes[static_cast<std::size_t>(j)]));
            }
            return ink;
        }

        // number()'s characters, as text.sh makes them (rounded by the fraction, as it rounds).
        static std::vector<int> numberCodes(float value, int decimals)
        {
            float m = 1.0f;
            for (int k = 0; k < decimals; ++k)
                m *= 10.0f;
            const float a = std::fabs(value) * m;
            const float whole = std::floor(a);
            const int r = static_cast<int>(std::min(whole + (a - whole >= 0.5f ? 1.0f : 0.0f), 9999999.0f));
            std::string digits = std::to_string(r);
            while (static_cast<int>(digits.size()) < decimals + 1)
                digits.insert(digits.begin(), '0');
            std::vector<int> codes;
            if (value < 0.0f && r > 0)
                codes.push_back(lib::glyphCode(0x2212));
            for (std::size_t k = 0; k < digits.size(); ++k)
            {
                if (decimals > 0 && k == digits.size() - static_cast<std::size_t>(decimals))
                    codes.push_back('.');
                codes.push_back(digits[k]);
            }
            return codes;
        }
    };

    std::vector<int> codesOf(const std::string& ascii)
    {
        std::vector<int> out;
        for (const char c : ascii)
            out.push_back(static_cast<unsigned char>(c));
        return out;
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
        const uint64_t compiledBefore = funkgui::shaders::detail::metalCompileCount();
        const auto t0 = std::chrono::steady_clock::now();   // a code nobody checked: set() runs Metal's compiler here
        const bool taken = b.ok() && funkgui::shaders::set(pg::shaderId(b.binary), b.binary.data(),
                                                           static_cast<uint32_t>(b.binary.size()), why);
        std::printf("shaders::set() of a code not checked before: %.2f ms\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        P.eq("prompt.example_taken", taken ? 1 : 0, 1);
        // The count BUILD reads moves with such a set(): one compile of Metal's (none off macOS).
        P.eq("prompt.set_compiles_once", static_cast<int64_t>(funkgui::shaders::detail::metalCompileCount() - compiledBefore),
             kMetal ? 1 : 0);
        if (!why.empty())
            std::printf("FunkGui: %s\n", why.c_str());
        // The text and shapes example (the library's): it builds, the renderer takes it, and it is still.
        const std::string gauge = promptExample(pg::ChatAgent::systemPrompt(), "gauge.sc");
        const pg::ShaderBuild gb = pg::buildShader("gauge.sc", gauge, { "rpm" }, t);
        if (!gb.ok())
            std::printf("the prompt's gauge:\n%s", gb.error.c_str());
        std::string gwhy;
        P.eq("prompt.gauge_builds", !gauge.empty() && gb.ok() && has(gauge, "#include \"funk/text.sh\"")
                                        && funkgui::shaders::check(gb.binary.data(), static_cast<uint32_t>(gb.binary.size()), gwhy) ? 1 : 0, 1);
        if (!gwhy.empty())
            std::printf("FunkGui: %s\n", gwhy.c_str());
        P.eq("prompt.gauge_still", pg::shaderAnimates(gauge) ? 0 : 1, 1);
        // The dial the pointer drags (THE INTERFACE's pointer): it builds with its pointer inputs, and it is still.
        const std::string dial = promptExample(pg::ChatAgent::systemPrompt(), "dial.sc");
        const pg::ShaderBuild db = pg::buildShader("dial.sc", dial, { "cutoff", "pointer.hover", "pointer.down" }, t);
        if (!db.ok())
            std::printf("the prompt's dial:\n%s", db.error.c_str());
        std::string dwhy;
        P.eq("prompt.dial_builds", !dial.empty() && db.ok() && has(dial, "in_pointer_hover")
                                       && funkgui::shaders::check(db.binary.data(), static_cast<uint32_t>(db.binary.size()), dwhy) ? 1 : 0, 1);
        P.eq("prompt.dial_still", pg::shaderAnimates(dial) ? 0 : 1, 1);
        // Its .gui line reads as the prompt writes it: the pointer's inputs and y= on the shader.
        const std::string prompt = pg::ChatAgent::systemPrompt();
        const std::size_t line = prompt.find("shader \"dial.sc\" inputs=");
        P.eq("prompt.dial_line", line != std::string::npos
                                     && pg::readGuiShaders({ "panel.gui", prompt.substr(line, prompt.find('\n', line) - line) + "\n" },
                                                           pg::product::kIsInstrument)
                                            .size() == 1 ? 1 : 0, 1);
    }

    // ---- ANIMATES ------------------------------------------------------------------------------------------------------
    {
        const auto animates = [](const std::string& body) { return pg::shaderAnimates(body) ? 1 : 0; };
        P.eq("animates.time", animates("void main() { gl_FragColor = vec4(vec3(fract(u_time)), 1.0); }"), 1);
        P.eq("animates.view_size", animates("void main() { float t = u_viewSize.w; gl_FragColor = vec4(t); }"), 1);
        P.eq("animates.line_comment", animates("// u_time is not read\nvoid main() { gl_FragColor = vec4(1.0); }"), 0);
        P.eq("animates.block_comment", animates("/* u_time,\n u_viewSize */ void main() { gl_FragColor = vec4(1.0); }"), 0);
        P.eq("animates.comment_continued", animates("// a comment that goes on \\\n u_time\nvoid main() {}"), 0);
        P.eq("animates.longer_names", animates("float my_u_time = 1.0; float u_timeScale = 2.0; float u_viewSize2;"), 0);
        P.eq("animates.after_comment", animates("/* still? */ float t = 0.5 * u_time; // no"), 1);
        P.eq("animates.probe_shaders", animates(kStill) == 0 && animates(kMoving) == 1 ? 1 : 0, 1);
        P.eq("animates.prompt_example", animates(promptExample(pg::ChatAgent::systemPrompt())), 1);
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
    // still.sc's and moving.sc's ids, as the compile gave them: not registered before the Panel's two frames, and
    // registered after them, so the count below spans their shaders::set().
    const auto registered = [&] {
        const pg::CompileStatus c = port.status();
        int n = 0;
        for (const pg::PanelShader& sh : c.panel.shaders)
            n += sh.id != 0 && funkgui::shaders::has(sh.id) ? 1 : 0;
        return n;
    };
    const int registeredBefore = registered();
    const uint64_t compiledBefore = funkgui::shaders::detail::metalCompileCount();
    const auto registering = std::chrono::steady_clock::now();
    host.tick(2);                                        // the Panel registers them (shaders::set, the message thread)
    const double registerMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - registering).count();
    const uint64_t compiledBySet = funkgui::shaders::detail::metalCompileCount() - compiledBefore;
    const int registeredBySet = registered();
    host.settle(60);
    const pg::CompileStatus s = port.status();
    std::printf("notes:\n%s", s.notes.c_str());
    P.eq("build.plays", s.ok ? 1 : 0, 1);
    P.eq("build.three", static_cast<int64_t>(s.panel.shaders.size()), 3);
    const bool built = s.panel.shaders.size() == 3 && !s.panel.shaders[0].binary.empty() && !s.panel.shaders[1].binary.empty();
    P.eq("build.binaries", built ? 1 : 0, 1);
    P.eq("build.ids", built && s.panel.shaders[0].id == pg::shaderId(s.panel.shaders[0].binary) && s.panel.shaders[0].id != 0 ? 1 : 0, 1);
    P.eq("build.no_note_for_good_ones", has(s.notes, "still.sc") || has(s.notes, "moving.sc") ? 0 : 1, 1);
    std::printf("Metal compiles in the Panel's shaders::set(): %llu (its two frames %.2f ms)\n",
                static_cast<unsigned long long>(compiledBySet), registerMs);
    P.eq("build.unregistered_before", registeredBefore, 0);
    P.eq("build.registered_in_two_frames", registeredBySet, 2);
    P.eq("build.set_compiles_nothing", static_cast<int64_t>(compiledBySet), 0);   // checked while the program compiled
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
            host.tick(20);                               // past the quarter second a new program moves it for
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
            // A shader that reads u_time moves every frame: the panel asks for each (Panel.h "Unchanged frames").
            host.settle(60);
            host.draw();
            int asked = 0;
            for (int k = 0; k < 6; ++k)
            {
                host.tick(1, 1.0f / 12.0f);
                asked += panel.needsDraw() ? 1 : 0;
                host.draw();
            }
            P.eq("draw.animated_asks", asked, 6);
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
        // Where no GPU picture is drawn (Linux), SoftRaster's, saying why, bgfx never started for it; the rows that look at
        // the GPU's pixels skipped, saying so.
        const std::string noGpu = probe::noGpuPictures(), skipWhy = probe::noGpuPicturesSkip();
        const auto skipAll = [&](std::initializer_list<const char*> keys) {
            for (const char* k : keys)
                probe::skip(P, k, skipWhy);
        };
        if (noGpu.empty())
        {
            P.eq("picture.gpu", g.gpu && again.gpu ? 1 : 0, 1);
            P.eq("picture.held", !heldBefore && funkgui::gpuPictureContextHeld() ? 1 : 0, 1);
        }
        else
        {
            P.eq("picture.soft_said", !g.gpu && !again.gpu && g.why == noGpu ? 1 : 0, 1);
            P.eq("picture.not_held", funkgui::gpuPictureContextHeld() ? 0 : 1, 1);
            skipAll({ "picture.gpu", "picture.held", "picture.soft_placeholder", "picture.gpu_not_placeholder", "picture.gpu_differs",
                      "picture.gpu_shader_colours", "picture.rest_over_2_share", "picture.rest_over_4" });
        }
        P.eq("picture.size", g.image.w == soft.w && g.image.h == soft.h && soft.w > 0 ? 1 : 0, 1);
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
        if (noGpu.empty())
            P.eq("picture.gpu_png", g.image.w > 0 && funkgui::writePng(g.image, probe::outPath("gui.shaders.gpu.png").c_str()) ? 1 : 0, 1);
        else
            skipAll({ "picture.gpu_png" });           // no gui.shaders.gpu.png: it would be SoftRaster's

        // The hold: kept while pictures come, let go once they stop (the processor's timer calls picturesIdle).
        pg::picturesIdle();
        if (noGpu.empty())
            P.eq("picture.hold_kept", funkgui::gpuPictureContextHeld() ? 1 : 0, 1);
        else
            skipAll({ "picture.hold_kept" });
        pg::picturesIdle(0.0);
        P.eq("picture.hold_released", funkgui::gpuPictureContextHeld() ? 0 : 1, 1);

        // view_panel's picture: on the GPU (bgfx started again), and through the fallback.
        const pg::PanelPicture onGpu = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        std::printf("view_panel: %s\n", onGpu.about.c_str());
        if (noGpu.empty())
            P.eq("picture.panel_gpu", !onGpu.png.empty() && onGpu.gpu && has(onGpu.about, "Drawn by the GPU as the user's screen draws it, shaders included")
                                          && funkgui::gpuPictureContextHeld() ? 1 : 0, 1);
        else
            P.eq("picture.panel_soft_said", !onGpu.png.empty() && !onGpu.gpu && has(onGpu.about, "Drawn without the GPU (" + noGpu + "): each shader shows as a striped placeholder")
                                                && !funkgui::gpuPictureContextHeld() ? 1 : 0, 1);
        P.eq("picture.panel_png", writeBytes(probe::outPath("gui.shaders.picture.png"), onGpu.png) ? 1 : 0, 1);
        pg::picture::forceSoftRasterForTests(true);
        const pg::PanelPicture onSoft = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        pg::picture::forceSoftRasterForTests(false);
        std::printf("view_panel, the GPU forced off: %s\n", onSoft.about.c_str());
        P.eq("picture.fallback_png", !onSoft.png.empty() && !onSoft.gpu && onSoft.width == onGpu.width && onSoft.height == onGpu.height ? 1 : 0, 1);
        P.eq("picture.fallback_note", has(onSoft.about, "Drawn without the GPU (forced by a test): each shader shows as a striped placeholder") ? 1 : 0, 1);
        if (noGpu.empty())
            P.eq("picture.fallback_differs", onSoft.png != onGpu.png ? 1 : 0, 1);
        else
            skipAll({ "picture.fallback_differs" });   // both SoftRaster's here
        writeBytes(probe::outPath("gui.shaders.picture-soft.png"), onSoft.png);
        pg::picturesIdle(0.0);
    }

    // ---- DRAW ----------------------------------------------------------------------------------------------------------
    // A panel whose only shader is still: quiet while nothing changes, and a frame when the parameter it reads moves
    // with no input (a host's automation): its inputs are what it draws (Panel.h "Unchanged frames"), and what asks:
    // the parameter's knob is on the page not shown and its dial hidden in the PANEL view, and a hidden item's key is
    // not watched. (A .gui file that leaves a parameter out shows its knob last: the knobs are named, on page B.)
    {
        const char* const still = "panel { tabs {\n"
                                  "    page \"A\" { shader \"still.sc\" inputs=\"cutoff\" width=200 height=120 }\n"
                                  "    page \"B\" { knob cutoff; knob level }\n"
                                  "} }\n";
        load(processor, { { pg::kProgramName, program() }, { "panel.gui", still }, { "still.sc", kStill } });
        processor.settle();
        host.tick(2);
        host.settle();
        // The editor's check while typing landed (a change of its own, late on a loaded machine).
        for (int k = 0; k < 600 && (panel.codeAssist().checking() || port.compiling()); ++k)
        {
            host.tick(1, 1.0f / 12.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        host.settle();
        P.eq("draw.still_alone", shaderShowing(panel, "still.sc") != nullptr && !panel.guiControl(0).shown() ? 1 : 0, 1);
        host.draw();
        int asked = 0;
        for (int k = 0; k < 11; ++k)
        {
            host.tick(1, 1.0f / 12.0f);
            asked += panel.needsDraw() ? 1 : 0;
        }
        P.eq("draw.still_quiet", asked, 0);
        host.draw();                                     // the safety net's second starts again: the move must ask
        juce::AudioProcessorParameter& cutoff = *processor.getParameters()[0];
        cutoff.setValueNotifyingHost(cutoff.getValue() < 0.5f ? 0.75f : 0.25f);
        host.tick(1, 1.0f / 12.0f);
        P.eq("draw.still_input", panel.needsDraw() ? 1 : 0, 1);
        // The move keeps the full rate (the shader moving: what its inputs' change set is kept until the Panel reads
        // it), a quarter second after it too, and then the panel is calm.
        const pg::ui::GuiShaderItem* sh = shaderShowing(panel, "still.sc");
        P.eq("draw.input_full_rate", sh != nullptr && !sh->settled() && panel.wantsFullRate() ? 1 : 0, 1);
        host.tick(9);                                    // 0.15 s at 60 Hz
        P.eq("draw.input_held", sh != nullptr && !sh->settled() && panel.wantsFullRate() ? 1 : 0, 1);
        host.tick(9);
        P.eq("draw.input_calm_after", sh != nullptr && sh->settled() && !panel.wantsFullRate() ? 1 : 0, 1);
    }

    // ---- POINTER -------------------------------------------------------------------------------------------------------
    // A shader that reads the pointer over it (pointer.x, .hover, .down) and drags cutoff with y=: its inputs follow
    // the pointer, it asks for frames on the pointer's events and not between them, and the drag writes its param.
    {
        const char* const touch = "// touch.sc: lit by the pointer over it\n"
                                  "void main()\n"
                                  "{\n"
                                  "    gl_FragColor = vec4(0.2 + 0.6 * in_pointer_hover, 0.2 + 0.6 * in_pointer_down, in_cutoff, 1.0);\n"
                                  "}\n";
        const char* const gui = "panel { tabs {\n"
                                "    page \"A\" { shader \"touch.sc\" inputs=\"cutoff pointer.x pointer.hover pointer.down\" y=cutoff "
                                "drag=relative width=160 height=90 }\n"
                                "    page \"B\" { knob cutoff; knob level }\n"
                                "} }\n";
        load(processor, { { pg::kProgramName, program() }, { "panel.gui", gui }, { "touch.sc", touch } });
        processor.settle();
        host.tick(2);
        host.settle();
        for (int k = 0; k < 600 && (panel.codeAssist().checking() || port.compiling()); ++k)
        {
            host.tick(1, 1.0f / 12.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const pg::ui::GuiShaderItem* sh = shaderShowing(panel, "touch.sc");
        P.eq("pointer.shown", sh != nullptr && sh->takesPointer() ? 1 : 0, 1);
        if (sh != nullptr)
        {
            const funkgui::Rect r = sh->cell();
            host.move(5.0f, 5.0f);
            host.settle();
            P.eq("pointer.rest", sh->inputs()[2] == 0.0f && sh->inputs()[3] == 0.0f ? 1 : 0, 1);
            const auto frames = [&](int ticks) {
                int asked = 0;
                for (int k = 0; k < ticks; ++k)
                {
                    host.tick(1, 1.0f / 12.0f);
                    asked += panel.needsDraw() ? 1 : 0;
                }
                return asked;
            };
            host.draw();
            P.eq("pointer.quiet_away", frames(11), 0);
            // The pointer comes: a frame for it, its inputs the place and hover.
            host.move(r.x + 0.25f * r.w, r.y + 0.75f * r.h);
            host.tick(1, 1.0f / 12.0f);
            host.tick(1, 1.0f / 12.0f);
            P.near("pointer.input_x", static_cast<double>(sh->inputs()[1]), 0.25, 0.01);
            P.eq("pointer.input_hover", sh->inputs()[2] == 1.0f ? 1 : 0, 1);
            P.eq("pointer.draws_on_move", panel.needsDraw() || !sh->settled() ? 1 : 0, 1);
            host.draw();
            // Held still over it: calm again once its inputs rest (a quarter second), no frame asked between events.
            host.tick(6, 1.0f / 12.0f);
            host.draw();
            P.eq("pointer.quiet_between", frames(11), 0);
            const auto colour = [&]() -> std::array<double, 3> {
                const pg::picture::Drawn d = pg::picture::draw(host.draw());
                if (!d.gpu)
                    return { -1.0, -1.0, -1.0 };
                const Px c = pixelsOf(d.image, r, 2.0f, 4.0f);
                return meanColour(d.image, c.x0, c.x1, c.y0, c.y1);
            };
            const std::array<double, 3> hovered = colour();
            // A press: down high, a drag up 50 px (relative) raises cutoff a quarter, one host gesture.
            const float before = processor.ports()[0]->value01();
            host.press(r.centreX(), r.centreY());
            host.moveTo(r.centreX(), r.centreY() - 50.0f, 5);
            host.tick(1, 1.0f / 12.0f);
            P.eq("pointer.input_down", sh->inputs()[3] == 1.0f ? 1 : 0, 1);
            P.near("pointer.drag_writes", static_cast<double>(processor.ports()[0]->value01() - before), 0.25, 0.01);
            const std::array<double, 3> pressed = colour();
            host.release();
            host.tick(2, 1.0f / 12.0f);
            P.eq("pointer.input_up", sh->inputs()[3] == 0.0f ? 1 : 0, 1);
            host.move(5.0f, 5.0f);
            host.tick(2, 1.0f / 12.0f);
            const std::array<double, 3> away = colour();
            // The pixels: the GPU's picture of the shader lit by hover and by the press (red, then green, up 0.6 * 255).
            if (probe::noGpuPictures().empty())
            {
                std::printf("pointer: the shader's colour away (%.0f, %.0f, %.0f), hovered (%.0f, %.0f, %.0f), pressed (%.0f, %.0f, %.0f)\n",
                            away[0], away[1], away[2], hovered[0], hovered[1], hovered[2], pressed[0], pressed[1], pressed[2]);
                P.ge("pointer.pixels_hover", hovered[0] - away[0], 100.0);
                P.ge("pointer.pixels_down", pressed[1] - hovered[1], 100.0);
            }
            else
            {
                probe::skip(P, "pointer.pixels_hover", probe::noGpuPicturesSkip());
                probe::skip(P, "pointer.pixels_down", probe::noGpuPicturesSkip());
            }
        }
    }

    // ---- CACHE ---------------------------------------------------------------------------------------------------------
    // The process's cache and the disk's (Shaders.h, "The cache"). The disk cache's folders are the probe's own (under
    // CTest the sandbox's): FOLDER's the processor's, the others in it.
    {
        const std::filesystem::path home = pg::shaderCacheFolder();
        const char* sandboxVar = std::getenv((std::string(pg::product::kEnvPrefix) + "PREFS_DIR").c_str());
        P.eq("cache.folder_sandbox", sandboxVar != nullptr && home == std::filesystem::path(sandboxVar) / "shaders" ? 1 : 0, 1);
        P.ge("cache.folder_written", folder(home).entries, 2);   // BUILD's still.sc and moving.sc

        // A second instance builds nothing: the same panel, from the process's memory.
        const pg::Sources two{ { pg::kProgramName, program() },
                               { "panel.gui", "panel { row { shader \"still.sc\" inputs=\"cutoff\"; shader \"moving.sc\" "
                                              "inputs=\"out\" } row { knob cutoff; knob level } }\n" },
                               { "still.sc", kStill }, { "moving.sc", kMoving } };
        pg::CompileStatus first;
        {
            pg::Processor one;
            load(one, two);
            one.settle();
            first = one.editorPort().status();
        }
        const bool firstBuilt = first.panel.shaders.size() == 2 && first.panel.shaders[0].id != 0 && first.panel.shaders[1].id != 0;
        P.eq("cache.first_built", firstBuilt ? 1 : 0, 1);
        {
            const uint64_t runs = pg::shadercRuns();
            const pg::ShaderCache::Counts before = pg::shaderCache().counts();
            pg::Processor second;
            load(second, two);
            second.settle();
            const pg::CompileStatus c = second.editorPort().status();
            const pg::ShaderCache::Counts after = pg::shaderCache().counts();
            P.eq("cache.second_instance_runs_no_shaderc", static_cast<int64_t>(pg::shadercRuns() - runs), 0);
            P.eq("cache.second_instance_from_memory", static_cast<int64_t>(after.memory + after.waited - before.memory - before.waited) >= 2 ? 1 : 0, 1);
            P.eq("cache.second_instance_same", firstBuilt && c.panel == first.panel ? 1 : 0, 1);
        }
        // A restart (the memory let go, as a new process has none) reads the disk.
        {
            pg::shaderCache().forget();
            const uint64_t runs = pg::shadercRuns();
            const pg::ShaderCache::Counts before = pg::shaderCache().counts();
            pg::Processor again;
            load(again, two);
            again.settle();
            const pg::CompileStatus c = again.editorPort().status();
            const pg::ShaderCache::Counts after = pg::shaderCache().counts();
            P.eq("cache.restart_runs_no_shaderc", static_cast<int64_t>(pg::shadercRuns() - runs), 0);
            P.eq("cache.restart_from_disk", static_cast<int64_t>(after.disk - before.disk), 2);
            P.eq("cache.restart_same", firstBuilt && c.panel == first.panel ? 1 : 0, 1);
        }

        const pg::ShaderTools plain = tools();
        const std::filesystem::path root = home.parent_path() / "shader-cache-probe";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        const auto fresh = [&](const std::filesystem::path& dir) {
            pg::ShaderTools x = plain;
            x.cacheDir = dir;
            return x;
        };

        // A changed shaderc (here a copy, its time moved on) builds again; the entry of the one before is not read.
        {
            const std::filesystem::path bin = root / "bin";
            std::filesystem::create_directories(bin, ec);
            const std::filesystem::path copy = bin / std::filesystem::path(plain.shaderc).filename();
            std::filesystem::copy_file(plain.shaderc, copy, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::permissions(copy, std::filesystem::perms::owner_all | std::filesystem::perms::group_read
                                                   | std::filesystem::perms::group_exec | std::filesystem::perms::others_read
                                                   | std::filesystem::perms::others_exec, ec);
            pg::ShaderTools x = fresh(root / "changed");
            x.shaderc = copy.string();
            const std::string body = saltedStill(1);
            pg::ShaderCache a, b, c;
            const pg::ShaderBuild one = a.build("still.sc", body, { "cutoff" }, x);
            const pg::ShaderBuild two2 = b.build("still.sc", body, { "cutoff" }, x);
            std::filesystem::last_write_time(copy, std::filesystem::last_write_time(copy, ec) + std::chrono::hours(1), ec);
            const pg::ShaderBuild three = c.build("still.sc", body, { "cutoff" }, x);
            P.eq("cache.changed_first_built", one.ok() && a.counts().built == 1 ? 1 : 0, 1);
            P.eq("cache.changed_same_reads_disk", two2.ok() && b.counts().disk == 1 && b.counts().built == 0 ? 1 : 0, 1);
            P.eq("cache.changed_builds_again", three.ok() && c.counts().built == 1 && c.counts().disk == 0 ? 1 : 0, 1);
            P.eq("cache.changed_same_binary", one.binary == three.binary ? 1 : 0, 1);
            P.eq("cache.changed_two_entries", folder(root / "changed").entries, 2);
            std::filesystem::remove_all(bin, ec);
        }

        // A corrupt entry is ignored (built again, the right binary) and written again whole; so is another key's entry
        // under its name.
        {
            const std::filesystem::path dir = root / "corrupt";
            const pg::ShaderTools x = fresh(dir);
            const std::string body = saltedStill(2);
            const pg::ShaderBuild good = pg::ShaderCache().build("still.sc", body, { "cutoff" }, x);
            const std::filesystem::path entry = onlyEntry(dir);
            const std::string whole = bytesOf(entry);
            P.eq("cache.corrupt_entry_written", good.ok() && !entry.empty() && !whole.empty() ? 1 : 0, 1);
            const auto spoilt = [&](const std::string& bytes) {
                {
                    std::ofstream out(entry, std::ios::binary | std::ios::trunc);
                    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
                pg::ShaderCache c;
                const pg::ShaderBuild b = c.build("still.sc", body, { "cutoff" }, x);
                pg::ShaderCache d;                       // and written again: the next reads it
                const pg::ShaderBuild e = d.build("still.sc", body, { "cutoff" }, x);
                return c.counts().built == 1 && c.counts().disk == 0 && b.binary == good.binary && d.counts().disk == 1
                    && e.binary == good.binary && bytesOf(entry) == whole;
            };
            std::string flipped = whole;
            flipped.back() = static_cast<char>(flipped.back() ^ 0x5a);
            P.eq("cache.corrupt_byte_ignored", spoilt(flipped) ? 1 : 0, 1);
            P.eq("cache.corrupt_cut_ignored", spoilt(whole.substr(0, whole.size() / 2)) ? 1 : 0, 1);
            P.eq("cache.corrupt_empty_ignored", spoilt(std::string()) ? 1 : 0, 1);
            P.eq("cache.corrupt_garbage_ignored", spoilt(std::string(whole.size(), 'x')) ? 1 : 0, 1);
            // Another shader's entry under this one's name (its key is not this one's).
            const std::filesystem::path otherDir = root / "other";
            pg::ShaderCache().build("still.sc", saltedStill(3), { "cutoff" }, fresh(otherDir));
            P.eq("cache.other_key_ignored", spoilt(bytesOf(onlyEntry(otherDir))) ? 1 : 0, 1);
            P.eq("cache.corrupt_no_temp_left", folder(dir).temps, 0);
        }

        // At once: four threads asking one cache for one new build run shaderc once; four caches (four processes) asking
        // for one, each building it, leave one whole entry and no temporary file.
        {
            const pg::ShaderTools x = fresh(root / "together");
            const std::string body = saltedStill(4);
            pg::ShaderCache one;
            const uint64_t runs = pg::shadercRuns();
            std::vector<pg::ShaderBuild> got(4);
            {
                std::vector<std::thread> threads;
                for (std::size_t k = 0; k < 4; ++k)
                    threads.emplace_back([&, k] { got[k] = one.build("still.sc", body, { "cutoff" }, x); });
                for (std::thread& th : threads)
                    th.join();
            }
            const pg::ShaderCache::Counts n = one.counts();
            P.eq("cache.together_one_shaderc", static_cast<int64_t>(pg::shadercRuns() - runs), 1);
            P.eq("cache.together_shared", n.built == 1 && n.memory + n.waited == 3 && got[0].ok() && got[1].binary == got[0].binary
                                              && got[2].binary == got[0].binary && got[3].binary == got[0].binary ? 1 : 0, 1);
            bool whole = true;
            for (int round = 0; round < 3; ++round)
            {
                const std::string b = saltedStill(10 + round);
                std::vector<pg::ShaderCache> caches(4);
                std::vector<pg::ShaderBuild> each(4);
                {
                    std::vector<std::thread> threads;
                    for (std::size_t k = 0; k < 4; ++k)
                        threads.emplace_back([&, k] { each[k] = caches[k].build("still.sc", b, { "cutoff" }, x); });
                    for (std::thread& th : threads)
                        th.join();
                }
                pg::ShaderCache after;
                const pg::ShaderBuild read = after.build("still.sc", b, { "cutoff" }, x);
                whole = whole && after.counts().disk == 1 && read.ok() && read.binary == each[0].binary;
            }
            P.eq("cache.processes_whole_entry", whole ? 1 : 0, 1);
            P.eq("cache.processes_no_temp_left", folder(root / "together").temps, 0);
        }

        // A running shaderc holds none of the process's other files: one written and closed while builds start is
        // renamed and removed at once. On Windows a file open as a shaderc started was inherited by it, and could be
        // neither until that shaderc exited (the temporary file above was left so); a descriptor holds no name
        // elsewhere. A try that fails is made again for 10 ms, which a scanner's look at a new file is over in and a
        // shaderc is not.
        {
            const std::filesystem::path heldDir = root / "held";
            std::filesystem::create_directories(heldDir, ec);
            const pg::ShaderTools heldTools = fresh(heldDir / "cache");
            const auto spin = [](int ms) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                while (std::chrono::steady_clock::now() < until)
                    std::this_thread::yield();
            };
            const auto soon = [&](const auto& tryIt) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
                for (;;)
                {
                    if (tryIt())
                        return true;
                    if (std::chrono::steady_clock::now() >= until)
                        return false;
                    std::this_thread::yield();
                }
            };
            constexpr int kHeldBuilds = 6;
            std::atomic<bool> buildsDone{ false };
            std::atomic<int> buildsOk{ 0 };
            std::thread builder([&] {
                for (int k = 0; k < kHeldBuilds; ++k)
                    if (pg::ShaderCache().build("still.sc", saltedStill(500 + k), { "cutoff" }, heldTools).ok())
                        buildsOk.fetch_add(1);
                buildsDone.store(true);
            });
            int tried = 0, stuck = 0;
            while (!buildsDone.load())
            {
                const std::filesystem::path from = heldDir / ("f" + std::to_string(tried) + ".txt");
                const std::filesystem::path to = heldDir / ("f" + std::to_string(tried) + ".moved");
                {
                    std::ofstream file(from, std::ios::binary | std::ios::trunc);
                    file << "held";
                    spin(2);                             // open while a shaderc may start
                }
                const bool moved = soon([&] {
                    std::error_code e;
                    std::filesystem::rename(from, to, e);
                    return !e;
                });
                const bool removed = moved && soon([&] {
                    std::error_code e;
                    return std::filesystem::remove(to, e) && !e;
                });
                stuck += removed ? 0 : 1;
                ++tried;
            }
            builder.join();
            std::printf("held: %d files written while %d shaders were built; %d not renamed and removed\n", tried,
                        kHeldBuilds, stuck);
            P.eq("cache.held_builds", buildsOk.load(), kHeldBuilds);
            P.ge("cache.held_files_tried", tried, kHeldBuilds);
            P.eq("cache.held_none_stuck", stuck, 0);
        }

        // The cap: the entries used least lately go first, down to three quarters of it; the newest stays.
        {
            const std::filesystem::path dir = root / "cap";
            pg::ShaderTools x = fresh(dir);
            pg::ShaderCache().build("still.sc", saltedStill(100), { "cutoff" }, x);
            const std::uintmax_t one = folder(dir).bytes;
            x.cacheBytes = one * 4;                      // four entries' room
            std::filesystem::path newest;
            for (int k = 1; k < 10; ++k)
            {
                pg::ShaderCache().build("still.sc", saltedStill(100 + k), { "cutoff" }, x);
                // Their times a second apart (a file system keeps whole seconds at worst), the newest last.
                std::error_code e;
                for (auto it = std::filesystem::directory_iterator(dir, e); !e && it != std::filesystem::directory_iterator(); it.increment(e))
                    if (it->path().extension() == ".shader")
                        std::filesystem::last_write_time(it->path(), it->last_write_time(e) - std::chrono::seconds(1), e);
            }
            pg::ShaderCache last;
            last.build("still.sc", saltedStill(109), { "cutoff" }, x);
            const Folder f = folder(dir);
            std::printf("cache: an entry %llu bytes; after ten builds under a cap of four, %d entries, %llu bytes\n",
                        static_cast<unsigned long long>(one), f.entries, static_cast<unsigned long long>(f.bytes));
            P.le("cache.cap_bytes", static_cast<double>(f.bytes), static_cast<double>(x.cacheBytes));
            P.in("cache.cap_entries", f.entries, 2, 4);
            P.eq("cache.cap_newest_kept", static_cast<int64_t>(last.counts().disk), 1);
            pg::ShaderCache firstAgain;
            firstAgain.build("still.sc", saltedStill(100), { "cutoff" }, x);
            P.eq("cache.cap_oldest_gone", static_cast<int64_t>(firstAgain.counts().built), 1);
        }

        // The folder's size a running total: the process scans a folder at its first write there, not at every write;
        // that scan removes a writer's temporary file left long ago and keeps one being written.
        {
            const std::filesystem::path dir = root / "scans";
            const pg::ShaderTools x = fresh(dir);
            std::filesystem::create_directories(dir, ec);
            const std::filesystem::path stale = dir / "0123456789abcdef.left.tmp";
            const std::filesystem::path recent = dir / "fedcba9876543210.writing.tmp";
            writeBytes(stale.string(), "half an entry");
            writeBytes(recent.string(), "half an entry");
            const auto hourAgo = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
            std::filesystem::last_write_time(stale, hourAgo, ec);
            const uint64_t scans = pg::shaderCacheScans();
            pg::ShaderCache c;
            for (int k = 0; k < 5; ++k)
                c.build("still.sc", saltedStill(300 + k), { "cutoff" }, x);
            P.eq("cache.scans_written", folder(dir).entries, 5);
            P.eq("cache.scans_first_write_only", static_cast<int64_t>(pg::shaderCacheScans() - scans), 1);
            P.eq("cache.scans_stale_temp_gone", std::filesystem::exists(stale, ec) ? 0 : 1, 1);
            P.eq("cache.scans_recent_temp_kept", std::filesystem::exists(recent, ec) ? 1 : 0, 1);
        }

        // RUN guesses a shader's inputs from the .gui file before the program says which it has: a guess that is wrong
        // (an input the program lacks) costs a build, never the result.
        {
            pg::Processor guess;
            const pg::Sources wrong{ { pg::kProgramName, program() },
                                     { "panel.gui", "panel { row { shader \"still.sc\" inputs=\"cutoff nosuch\" } row { knob "
                                                    "cutoff; knob level } }\n" },
                                     { "still.sc", saltedStill(200) } };
            const uint64_t runs = pg::shadercRuns();
            const pg::ShaderCache::Counts before = pg::shaderCache().counts();
            load(guess, wrong);
            guess.settle();
            const pg::ShaderCache::Counts after = pg::shaderCache().counts();
            const uint64_t ran = pg::shadercRuns() - runs;
            const pg::CompileStatus c = guess.editorPort().status();
            const pg::ShaderBuild direct = pg::buildShader("still.sc", saltedStill(200), { "cutoff" }, plain);
            P.eq("cache.guess_wrong_right_result", c.panel.shaders.size() == 1 && direct.ok() && c.panel.shaders[0].binary == direct.binary
                                                       && c.panel.shaders[0].inputs == std::vector<std::string>{ "cutoff" } ? 1 : 0, 1);
            // The guess's build and the program's: one more than a right guess.
            P.eq("cache.guess_wrong_two_builds", static_cast<int64_t>(after.built - before.built), 2);
            P.eq("cache.guess_wrong_two_shaderc", static_cast<int64_t>(ran), 2);
            P.eq("cache.guess_reads_inputs", static_cast<int64_t>(pg::readGuiShaders(wrong[1], kInstrument).size()), 1);
            const std::vector<pg::PanelShader> guessed = pg::readGuiShaders(wrong[1], kInstrument);
            P.eq("cache.guess_takes_names", !guessed.empty() && guessed[0].inputs == std::vector<std::string>{ "cutoff", "nosuch" } ? 1 : 0, 1);
        }

        // RUN's early builds (Compiler::build): started before FMinus compiles, so that the builds after the compile
        // find them made. A compile with four new shaders runs shaderc four times (a right guess costs nothing more),
        // and the builds after the compile take all four from memory. And they run while FMinus compiles: a program of
        // 100 ms or more with those four shaders (built by then, so no shaderc runs and its limit on runs at once plays
        // no part) and a renderer's check that takes 150 ms for a code it has not seen (as Metal's compile may; here a
        // wait). Two orders of events say so, where a bound on the whole RUN's time would also count each moment a busy
        // machine keeps one of its threads from a core. Every check begins before the compile can have ended, which it
        // cannot were the early builds made after the compile or not at all. And the RUN's thread, which compiles, is
        // past both the compile and the early builds (the builds after the compile find the checks done) in less than
        // the last check's end and the compile's time together, which it cannot be were the early builds made before
        // the compile: the compile would begin only once the last check had ended. A busy machine can still keep a
        // thread from a core for longer than the compile in one of the two windows, so the compile is measured up to
        // three times and the first attempt in which both orders hold is the one reported: a wrong order fails every
        // attempt whatever the machine does, so one good attempt proves the right one.
        {
            pg::Engine engine;
            engine.prepare(48000.0, 256);
            pg::Compiler compiler(engine);
            compiler.setFreezing(false);
            compiler.setSelfTesting(false);
            pg::ShaderTools x = fresh(root / "early");
            compiler.setShaderTools(x);
            const int salt = 31;                         // TIME's salts are never 30 or 31 more than a multiple of 32
            const uint64_t runs = pg::shadercRuns();
            const pg::ShaderCache::Counts before = pg::shaderCache().counts();
            const pg::CompileStatus light = compiler.compileNow(timedFiles(salt, true, false));
            const pg::ShaderCache::Counts after = pg::shaderCache().counts();
            bool four = light.ok && light.panel.shaders.size() == 4;
            for (const pg::PanelShader& sh : light.panel.shaders)
                four = four && !sh.binary.empty();
            P.eq("cache.early_compiles", four ? 1 : 0, 1);
            P.eq("cache.early_builds_each_once", static_cast<int64_t>(after.built - before.built), 4);
            P.eq("cache.early_shaderc_four", static_cast<int64_t>(pg::shadercRuns() - runs), 4);
            P.ge("cache.early_made_before_needed",
                 static_cast<double>(after.memory + after.waited - before.memory - before.waited), 4.0);

            const std::chrono::milliseconds checkTime(150);
            const double checkMs = static_cast<double>(checkTime.count());
            std::mutex seenMutex;
            std::set<std::vector<uint8_t>> seen;
            std::chrono::steady_clock::time_point t0{};
            // Under seenMutex, ms after t0: when the first and the last of the four checks began, when the last ended,
            // and when the builds after the compile first asked for a check (a code checked already: at once).
            double firstCheck = -1.0, lastCheck = -1.0, lastEnded = -1.0, afterCompile = -1.0;
            x.check = [&](const std::vector<uint8_t>& binary, std::string&) {
                const auto msNow = [&t0] {
                    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                };
                {
                    const std::lock_guard<std::mutex> lock(seenMutex);
                    if (!seen.insert(binary).second)
                    {
                        if (afterCompile < 0.0)
                            afterCompile = msNow();
                        return true;
                    }
                    lastCheck = msNow();
                    if (firstCheck < 0.0)
                        firstCheck = lastCheck;
                }
                std::this_thread::sleep_for(checkTime);
                const std::lock_guard<std::mutex> lock(seenMutex);
                lastEnded = std::max(lastEnded, msNow());
                return true;
            };
            compiler.setShaderTools(x);
            // Each attempt a measurement of its own: the checks see four codes new to them (`seen` and the times
            // cleared), FMinus compiles the program again (compileNow keeps no compiled program to give back), and the
            // shaders come from memory (no shaderc run: its limit on runs at once plays no part in any attempt).
            pg::CompileStatus h;
            uint64_t shadercRan = 0;
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                {
                    const std::lock_guard<std::mutex> lock(seenMutex);
                    seen.clear();
                    firstCheck = lastCheck = lastEnded = afterCompile = -1.0;
                    t0 = std::chrono::steady_clock::now();
                }
                const uint64_t ranBefore = pg::shadercRuns();
                h = compiler.compileNow(timedFiles(salt, true, true));
                shadercRan = pg::shadercRuns() - ranBefore;
                const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                const std::lock_guard<std::mutex> lock(seenMutex);
                const bool beside = lastCheck >= 0.0 && lastCheck <= h.ms;
                const bool done = afterCompile >= 0.0 && lastEnded >= 0.0;
                const bool notBefore = afterCompile <= lastEnded + h.ms;
                std::printf("early: a compile of %.1f ms with four checks of %.0f ms at once took %.1f ms (their sum %.1f; "
                            "shaderc ran %d times); the checks began %.1f to %.1f ms in and ended by %.1f, the builds after "
                            "the compile asked %.1f ms in\n",
                            h.ms, checkMs, total, h.ms + checkMs, static_cast<int>(shadercRan), firstCheck, lastCheck,
                            lastEnded, afterCompile);
                if (beside && done && notBefore)
                    break;
                if (attempt < 2)
                    std::printf("early: %s: the compile again\n",
                                !beside ? "a check began after the compile could have ended (a thread kept from a core?)"
                                : !done ? "the builds after the compile did not find the checks done"
                                        : "the RUN's thread asked later than the last check's end and the compile's time "
                                          "together (a thread kept from a core?)");
            }
            P.eq("cache.early_heavy_compiles", h.ok && h.panel.shaders.size() == 4 && seen.size() == 4 && shadercRan == 0 ? 1 : 0, 1);
            P.le("cache.early_beside_the_compile", lastCheck, h.ms);
            P.eq("cache.early_checks_done", afterCompile >= 0.0 && lastEnded >= 0.0 ? 1 : 0, 1);
            P.le("cache.early_not_before_the_compile", afterCompile, lastEnded + h.ms);
        }

        // A build that failed for a reason of the moment is never kept: a shaderc that does not run (a copy without its
        // permission to execute) gives its note and leaves nothing on disk, and once it runs (the permission back, its
        // path, size and time the same: the same key) the same cache builds it. (Windows has no such permission.)
#if !defined(_WIN32)
        {
            const std::filesystem::path bin = root / "noexec";
            std::filesystem::create_directories(bin, ec);
            const std::filesystem::path copy = bin / std::filesystem::path(plain.shaderc).filename();
            std::filesystem::copy_file(plain.shaderc, copy, std::filesystem::copy_options::overwrite_existing, ec);
            using std::filesystem::perms;
            std::filesystem::permissions(copy, perms::owner_read | perms::owner_write | perms::group_read | perms::others_read, ec);
            const std::filesystem::file_time_type time = std::filesystem::last_write_time(copy, ec);
            pg::ShaderTools x = fresh(root / "passing");
            x.shaderc = copy.string();
            const std::string body = saltedStill(400);
            pg::ShaderCache c;
            const pg::ShaderBuild failed = c.build("still.sc", body, { "cutoff" }, x);
            const int onDisk = folder(root / "passing").entries;
            std::filesystem::permissions(copy, perms::owner_all | perms::group_read | perms::group_exec | perms::others_read
                                                   | perms::others_exec, ec);
            std::filesystem::last_write_time(copy, time, ec);
            const pg::ShaderBuild working = c.build("still.sc", body, { "cutoff" }, x);
            const pg::ShaderCache::Counts n = c.counts();
            std::printf("passing: without its permission to execute: %s; with it: %s (built %d, from memory %d, from disk %d)\n",
                        failed.ok() ? "built" : failed.error.c_str(), working.ok() ? "built" : working.error.c_str(),
                        static_cast<int>(n.built), static_cast<int>(n.memory), static_cast<int>(n.disk));
            P.eq("cache.passing_failure_said", !failed.ok() && has(failed.error, "shaderc did not run") ? 1 : 0, 1);
            P.eq("cache.passing_failure_not_on_disk", onDisk, 0);
            P.eq("cache.passing_failure_not_kept", working.ok() && n.built == 2 && n.memory == 0 && n.disk == 0 ? 1 : 0, 1);
            std::filesystem::remove_all(bin, ec);
        }
#endif

        // A shader that does not compile is kept in memory (shaderc does not run for it again in the process) but never
        // on disk: a session after this one tries it again.
        {
            const std::filesystem::path dir = root / "broken";
            pg::ShaderCache c;
            const pg::ShaderBuild one = c.build("broken.sc", kBroken, {}, fresh(dir));
            const uint64_t runs = pg::shadercRuns();
            const pg::ShaderBuild again = c.build("broken.sc", kBroken, {}, fresh(dir));
            P.eq("cache.broken_note", !one.ok() && has(one.error, "broken.sc:5:1: note: the shader does not compile:") ? 1 : 0, 1);
            P.eq("cache.broken_kept_in_memory", !again.ok() && again.error == one.error && c.counts().memory == 1
                                                    && pg::shadercRuns() == runs ? 1 : 0, 1);
            const Folder f = folder(dir);
            P.eq("cache.broken_not_on_disk", f.entries + f.temps, 0);
        }
        std::filesystem::remove_all(root, ec);
    }

    // ---- TIME ----------------------------------------------------------------------------------------------------------
    // A RUN's compile (Compiler::compileNow: RUN's build, on the calling thread) of a program alone, and with a panel of
    // four shaders new to the process (shaderc and the renderer's check, as the processor has them; a disk cache of the
    // probe's own); then the same four again on the same compiler, on a second one (another instance), and after a
    // restart (the memory let go: the disk's builds, FunkGui's checks still remembered). And the four shaders' part
    // alone, at the same moment: one after another as RUN built them before (buildShader, then the check, in turn), and
    // at once (buildAll, on a cache of no builds and no disk). Printed, the median of five rounds; never checked.
    {
        pg::Engine engine;
        engine.prepare(48000.0, 256);
        pg::ShaderTools checked = tools();
        checked.check = [](const std::vector<uint8_t>& binary, std::string& why) {
            return funkgui::shaders::check(binary.data(), static_cast<uint32_t>(binary.size()), why);
        };
        const std::filesystem::path disk = pg::shaderCacheFolder().parent_path() / "shader-cache-time";
        checked.cacheDir = disk;
        bool compiled = true;
        const auto timed = [&compiled](pg::Compiler& c, const pg::Sources& them) {
            const auto t0 = std::chrono::steady_clock::now();
            const pg::CompileStatus r = c.compileNow(them);
            compiled = compiled && r.ok && r.notes.find(".sc:") == std::string::npos;
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        };
        const auto msSince = [](std::chrono::steady_clock::time_point t0) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        };
        const int salt0 = static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
        for (const bool heavy : { false, true })
        {
            std::vector<double> alone, fresh, again, second, restart;
            for (int round = 0; round < 5; ++round)
            {
                pg::Compiler a(engine), b(engine);
                for (pg::Compiler* one : { &a, &b })
                {
                    one->setFreezing(false);
                    one->setSelfTesting(false);
                    one->setShaderTools(checked);
                }
                const pg::Sources four = timedFiles(salt0 * 32 + round * 2 + (heavy ? 1 : 0), true, heavy);
                alone.push_back(timed(a, timedFiles(0, false, heavy)));
                fresh.push_back(timed(a, four));
                again.push_back(timed(a, four));
                second.push_back(timed(b, four));
                pg::shaderCache().forget();
                restart.push_back(timed(b, four));
            }
            std::printf("time: a RUN's compile, the median of 5 (ms), %s: the program alone %.1f; with four new shaders %.1f; "
                        "the same four again %.1f; on a second instance %.1f; after a restart %.1f\n",
                        heavy ? "a program of 100 ms or more" : "a small program", median(alone), median(fresh),
                        median(again), median(second), median(restart));
        }
        std::vector<double> inTurn, atOnce;
        pg::ShaderTools noDisk = checked;
        noDisk.cacheDir.clear();
        for (int round = 0; round < 5; ++round)
        {
            const auto jobsOf = [](const pg::Sources& them) {
                std::vector<pg::ShaderJob> jobs;
                for (const pg::PanelShader& sh : pg::readGuiShaders(them[1], kInstrument))
                    for (const pg::SourceFile& f : them)
                        if (f.name == sh.file)
                            jobs.push_back({ sh.file, f.text, sh.inputs, {} });
                return jobs;
            };
            const std::vector<pg::ShaderJob> one = jobsOf(timedFiles(salt0 * 32 + 20 + round * 2, true, false));
            auto t0 = std::chrono::steady_clock::now();
            for (const pg::ShaderJob& j : one)
            {
                const pg::ShaderBuild b = pg::buildShader(j.file, j.body, j.inputs, noDisk);
                std::string why;
                compiled = compiled && b.ok() && noDisk.check(b.binary, why);
            }
            inTurn.push_back(msSince(t0));
            const std::vector<pg::ShaderJob> two = jobsOf(timedFiles(salt0 * 32 + 21 + round * 2, true, false));
            pg::ShaderCache none;
            t0 = std::chrono::steady_clock::now();
            for (const pg::ShaderBuild& b : none.buildAll(two, noDisk))
                compiled = compiled && b.ok() && !b.refused;
            atOnce.push_back(msSince(t0));
        }
        std::printf("time: four new shaders built and checked, the median of 5 (ms): one after another %.1f; at once %.1f "
                    "(shaderc %d at once at most here)\n",
                    median(inTurn), median(atOnce),
                    std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, pg::kShadercAtOnce));
        P.eq("time.compiles", compiled ? 1 : 0, 1);
        std::error_code ec;
        std::filesystem::remove_all(disk, ec);
    }
    // ---- SIXTEEN -------------------------------------------------------------------------------------------------------
    {
        std::string params, names;
        for (int k = 1; k <= pg::kMaxShaderInputs + 1; ++k)
        {
            params += "    param float p" + std::to_string(k) + " [[fminus::range(0, 1)]] [[fminus::default(" + std::to_string(k * 0.05)
                    + ")]];\n";
            names += (k == 1 ? "" : " ") + std::string("p") + std::to_string(k);
        }
        const std::string source = kInstrument
            ? "module Voice\n{\n" + params + "    void start(float n, float v) {}\n"
                  "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
            : "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        const std::string body = "// sixteen.sc: the sixteenth input, by its number and by its name.\n"
                                 "void main()\n{\n    gl_FragColor = vec4(vec3(in15), in_p16);\n}\n";
        pg::Processor q;
        q.prepareToPlay(48000.0, 256);
        pg::ui::Panel qp(q.ports(), q.editorPort());
        funkgui::HeadlessHost qh(qp, 0, 2.0f);
        qh.settle();
        {
            const funkgui::Point at = qp.centreOfView(true);
            qh.click(at.x, at.y);
            qh.settle();
        }
        load(q, { { pg::kProgramName, source },
                  { "panel.gui", "panel { row { shader \"sixteen.sc\" inputs=\"" + names + "\" width=200 height=120 } }\n" },
                  { "sixteen.sc", body } });
        q.settle();
        qh.tick(20);
        const pg::CompileStatus st = q.editorPort().status();
        std::printf("SIXTEEN notes:\n%s", st.notes.c_str());
        P.eq("sixteen.plays", st.ok ? 1 : 0, 1);
        P.eq("sixteen.past_noted", has(st.notes, "a shader takes " + std::to_string(pg::kMaxShaderInputs) + " inputs at most: p"
                                                     + std::to_string(pg::kMaxShaderInputs + 1) + " is left out") ? 1 : 0, 1);
        P.eq("sixteen.built", st.panel.shaders.size() == 1 && !st.panel.shaders[0].binary.empty() && !has(st.notes, "sixteen.sc:")
                                  && static_cast<int>(st.panel.shaders[0].inputs.size()) == pg::kMaxShaderInputs ? 1 : 0, 1);
        const pg::ui::GuiShaderItem* item = shaderShowing(qp, "sixteen.sc");
        P.eq("sixteen.shown", item != nullptr ? 1 : 0, 1);
        if (item != nullptr)
        {
            std::printf("SIXTEEN inputs: %.3f ... %.3f\n", static_cast<double>(item->inputs()[0]),
                        static_cast<double>(item->inputs()[pg::kMaxShaderInputs - 1]));
            P.near("sixteen.first_input", static_cast<double>(item->inputs()[0]), 0.05, 1e-4);
            P.near("sixteen.last_input", static_cast<double>(item->inputs()[pg::kMaxShaderInputs - 1]), 0.05 * pg::kMaxShaderInputs, 1e-4);
        }
        else
        {
            P.eq("sixteen.first_input", 0, 1);
            P.eq("sixteen.last_input", 0, 1);
        }
    }
    // ---- TEXT ----------------------------------------------------------------------------------------------------------
    // Text and includes (Shaders.h; fminus/ShaderLib.h; editor/ShaderFont.h).
    {
        // The atlas: ShaderLib.h's constants are the bake's, and every glyph fits its cell.
        const pg::ui::ShaderFontAtlas& atlas = pg::ui::shaderFontAtlas();
        std::printf("TEXT atlas: %s, %d glyphs, base %.3f, spread %.3f, advance %.6f, capitals %.6f, all in their cells: %s\n",
                    atlas.ok ? "made" : atlas.why.c_str(), atlas.glyphs, static_cast<double>(atlas.basePx),
                    static_cast<double>(atlas.spread), static_cast<double>(atlas.advance),
                    static_cast<double>(atlas.capHeight), atlas.allInCells ? "yes" : "no");
        P.eq("text.atlas", atlas.ok && atlas.texels.size() == static_cast<std::size_t>(lib::kAtlasW) * lib::kAtlasH ? 1 : 0, 1);
        P.eq("text.atlas_glyphs", atlas.glyphs, lib::kCodes - 33);
        P.eq("text.atlas_in_cells", atlas.allInCells ? 1 : 0, 1);
        P.near("text.atlas_base", static_cast<double>(atlas.basePx), static_cast<double>(lib::kBasePx), 1e-6);
        P.near("text.atlas_spread", static_cast<double>(atlas.spread), static_cast<double>(lib::kSpread), 1e-6);
        P.near("text.atlas_advance", static_cast<double>(atlas.advance), static_cast<double>(lib::kAdvance), 1e-3);
        P.near("text.atlas_capitals", static_cast<double>(atlas.capHeight), static_cast<double>(lib::kCapHeight), 1e-3);

        // The pre-pass: strings made codes, line for line; comments and #include lines left as they are.
        std::string err;
        const std::string rpm = pg::shaderText("t.sc", "vec4 s = \"RPM\"; // \"not\"\n#include \"funk/text.sh\"\n/* \"no\" */\n", err);
        P.eq("text.literal", rpm == "vec4 s = vec4(5066834.0, 0.0, 0.0, 0.0); // \"not\"\n#include \"funk/text.sh\"\n/* \"no\" */\n"
                                 && err.empty() ? 1 : 0, 1);
        const std::string escapes = pg::shaderText("t.sc", "a = \"\\u00B0F\"; b = \"\\\"\\\\\"; c = \"\";\n", err);
        P.eq("text.escapes", escapes == "a = vec4(18047.0, 0.0, 0.0, 0.0); b = vec4(23586.0, 0.0, 0.0, 0.0); c = vec4(0.0, 0.0, 0.0, 0.0);\n"
                                 && err.empty() ? 1 : 0, 1);
        const std::string longer = pg::shaderText("t.sc", "m = \"ABCDEFGHIJKLM\";\n", err);
        std::string zeros;
        for (int k = 0; k < 11; ++k)
            zeros += ", 0.0";
        P.eq("text.mat4", longer.rfind("m = mat4(4407873.0, ", 0) == 0 && has(longer, ", 77.0" + zeros + ");\n") && err.empty() ? 1 : 0, 1);
        pg::shaderText("t.sc", "x\n  y = \"" + std::string(49, 'A') + "\";\n", err);
        P.eq("text.too_long", has(err, "t.sc:2:7: note: the shader does not compile: a string holds 48 characters at most (this one 49)") ? 1 : 0, 1);
        pg::shaderText("t.sc", "y = \"caf\\u00E9\";\n", err);
        P.eq("text.no_glyph", has(err, "t.sc:1:5: note: the shader does not compile: the font has no U+00E9") ? 1 : 0, 1);
        pg::shaderText("t.sc", "y = 1.0;\ny = \"open;\n", err);
        P.eq("text.unended", has(err, "t.sc:2:5: note: the shader does not compile: a string has no closing quote on its line") ? 1 : 0, 1);
        pg::shaderText("t.sc", "y = \"\\n\";\n", err);
        P.eq("text.bad_escape", has(err, "t.sc:1:5: note: the shader does not compile: a string's backslash escapes") ? 1 : 0, 1);
        // A vector of one value a cast (Direct3D's compiler takes no float3(x)), at any depth; one of more values, a
        // comment, a name that only ends in vec3 and a declaration left; every line kept.
        const std::string ones = pg::shaderText(
            "t.sc", "vec3 c = vec3(w * in_out); // vec3(x)\nvec4 d = vec4(vec3(f(a, b)), 1.0); ivec2 i = ivec2 (k);\n"
                    "myvec3(x); vec3 g(vec3 p) { return vec2(vec4(\"AB\")).xyy; }\n/* vec2(y) */ bvec4 m = bvec4(\n  t);\n",
            err);
        std::printf("text.vector_of_one:\n%s", ones.c_str());
        P.eq("text.vector_of_one",
             ones == "vec3 c = ((vec3)(w * in_out)); // vec3(x)\nvec4 d = vec4(((vec3)(f(a, b))), 1.0); ivec2 i = ((ivec2) (k));\n"
                     "myvec3(x); vec3 g(vec3 p) { return ((vec2)(((vec4)(vec4(16961.0, 0.0, 0.0, 0.0))))).xyy; }\n"
                     "/* vec2(y) */ bvec4 m = ((bvec4)(\n  t));\n"
                 && err.empty() ? 1 : 0, 1);

        // The includes: the library's and the program's .sh files, in turn theirs, each once; one there is none of a note.
        const pg::Sources headers{ { "b.sh", "#include \"funk/text.sh\"\n#include \"c.sh\"\n" }, { "c.sh", "float c1() { return 1.0; }\n" },
                                   { "d.sh", "" } };
        const std::vector<std::string> reached = pg::shaderIncludes(
            "a.sc", "#include \"funk/text.sh\"\n#include <funk/shapes.sh>\n#include \"b.sh\"\n// #include \"d.sh\"\n#include <bgfx_shader.sh>\n",
            headers, err);
        P.eq("includes.reached", reached == std::vector<std::string>{ "funk/text.sh", "funk/shapes.sh", "b.sh", "c.sh" } && err.empty() ? 1 : 0, 1);
        pg::shaderIncludes("a.sc", "void f() {}\n  #include \"nofile.sh\"\n", headers, err);
        P.eq("includes.missing", has(err, "a.sc:2:12: note: the shader does not compile: there is no file nofile.sh to #include") ? 1 : 0, 1);
        pg::shaderIncludes("a.sc", "#include \"e.sh\"\n", { { "e.sh", "\n#include \"gone.sh\"\n" } }, err);
        P.eq("includes.missing_in_header", has(err, "e.sh:2:10: note: the shader does not compile: there is no file gone.sh") ? 1 : 0, 1);
        P.eq("includes.headers", static_cast<int64_t>(pg::shaderHeaders({ { "program.fm", "" }, { "x.sh", "" }, { "panel.gui", "" },
                                                                          { "y.sc", "" }, { "z.sh", "" } }).size()), 2);
        // A clock read in a .sh file the shader includes animates it; the library reads none.
        const pg::Sources clock{ { "tick.sh", "float tick() { return fract(u_time); }\n" } };
        P.eq("includes.animates", pg::shaderAnimates("a.sc", "#include \"tick.sh\"\nvoid main() {}\n", clock) ? 1 : 0, 1);
        P.eq("includes.library_still", !pg::shaderAnimates("a.sc", "#include \"funk/text.sh\"\n#include \"funk/shapes.sh\"\nvoid main() {}\n", {})
                                           && !pg::shaderAnimates("a.sc", "void main() { float t = text(v_px, v_px, 1.0, 0.0, \"u_time\"); }\n", {})
                                           ? 1 : 0, 1);
        int clocks = 0;
        for (const lib::LibraryFile& f : lib::library())
            clocks += pg::shaderAnimates(f.text) ? 1 : 0;
        P.eq("includes.library_no_clock", clocks, 0);

        // The panel: text and numbers drawn white on black (text.sc, ink.sh of the program's own, which includes the
        // library too), a gauge of shapes (dash.sc: its outer scale counted from -135, its inner one from 135), and an
        // odometer past 2^23 (odo.sc: 8999001 in seven segments and in the font, each over the same digits drawn one by
        // one, digit7's and text()'s).
        const char* const kTextSc = "// text.sc: the font, numbers and a long string, white on black.\n"
                                    "#include \"funk/text.sh\"\n"
                                    "#include \"ink.sh\"\n"
                                    "void main()\n{\n"
                                    "    float ink = text(v_px, vec2(8.0, 14.0), 14.0, 0.0, \"RPM 0-9 \\u00B0F\");\n"
                                    "    ink = max(ink, text(v_px, vec2(0.5 * u_size.x, 38.0), 20.0, 0.5, \"Hg\"));\n"
                                    "    ink = max(ink, number(v_px, vec2(u_size.x - 8.0, 64.0), 16.0, 1.0, in_level * 8000.0, 0.0));\n"
                                    "    ink = max(ink, number(v_px, vec2(8.0, 64.0), 12.0, 0.0, -in_cutoff * 10.0, 2.0));\n"
                                    "    ink = max(ink, label(v_px));\n"
                                    "    gl_FragColor = vec4(vec3_splat(ink), 1.0);\n}\n";
        const auto inkSh = [](const std::string& words) {
            return "// ink.sh: a helper of the program's own, which includes the library too.\n#include \"funk/text.sh\"\n\n"
                   "float label(vec2 p)\n{\n    return text(p, vec2(8.0, 92.0), 9.0, 0.0, \"" + words + "\");\n}\n";
        };
        const char* const kDashSc = "// dash.sc: a gauge of the library's shapes, its title and its value.\n"
                                    "#include \"funk/text.sh\"\n#include \"funk/shapes.sh\"\n"
                                    "void main()\n{\n"
                                    "    vec2 c = vec2(0.5 * u_size.x, 0.56 * u_size.y);\n"
                                    "    float r = 0.4 * u_size.y;\n"
                                    "    vec3 col = ramp(vec3(0.08, 0.08, 0.1), vec3(0.03, 0.03, 0.04), around(v_px, c, 1.3 * r));\n"
                                    "    col = mix(col, vec3(0.16, 0.16, 0.18), box(v_px, vec2(u_size.x - 27.0, 17.0), vec2(42.0, 24.0), 5.0));\n"
                                    "    col = mix(col, vec3(0.3, 0.09, 0.06), number7(v_px, vec2(u_size.x - 12.0, 17.0), 16.0, 8888888.0, 2.0));\n"
                                    "    col = mix(col, vec3(1.0, 0.25, 0.1), number7(v_px, vec2(u_size.x - 12.0, 17.0), 16.0, in_cutoff * 100.0, 2.0));\n"
                                    "    col = mix(col, vec3(0.35, 0.35, 0.4), ring(v_px, c, r + 4.0, 1.5));\n"
                                    "    col = mix(col, vec3(0.75, 0.75, 0.75), ticks(v_px, c, 0.82 * r, r, 1.5, -135.0, 135.0, 9.0));\n"
                                    "    col = mix(col, vec3(0.5, 0.5, 0.55), ticks(v_px, c, 0.56 * r, 0.74 * r, 1.5, 135.0, -135.0, 5.0));\n"
                                    "    col = mix(col, vec3(0.9, 0.2, 0.1), arc(v_px, c, r - 2.0, 4.0, 90.0, 135.0));\n"
                                    "    col = mix(col, vec3(0.25, 0.25, 0.28), segment(v_px, vec2(10.0, 17.0), vec2(34.0, 17.0), 3.0));\n"
                                    "    col = mix(col, vec3(0.2, 0.6, 1.0), segment(v_px, vec2(10.0, 17.0), vec2(10.0 + 24.0 * in_level, 17.0), 3.0));\n"
                                    "    vec2 tip = c + dirOf(-135.0 + 270.0 * in_level) * 0.9 * r;\n"
                                    "    col += vec3(1.0, 0.45, 0.1) * 0.35 * glow(sdLine(v_px, c, tip), 4.0);\n"
                                    "    col = mix(col, vec3(1.0, 0.55, 0.1), needle(v_px, c, -135.0 + 270.0 * in_level, 0.9 * r, 3.0));\n"
                                    "    col = mix(col, vec3(0.12, 0.12, 0.13), circle(v_px, c, 5.0));\n"
                                    "    col = mix(col, vec3(0.6, 0.6, 0.62), text(v_px, c + vec2(0.0, 0.3 * r), 9.0, 0.5, \"RPM\"));\n"
                                    "    col = mix(col, vec3(1.0, 1.0, 1.0), number(v_px, c + vec2(0.0, 0.58 * r), 12.0, 0.5, in_level * 8000.0, 0.0));\n"
                                    "    col = mix(col, ramp3(vec3(0.2, 0.9, 0.3), vec3(1.0, 0.8, 0.1), vec3(1.0, 0.2, 0.1), along(v_px, c - vec2(r, 0.0), c + vec2(r, 0.0))),\n"
                                    "              box(v_px, vec2(c.x, u_size.y - 6.0), vec2(2.0 * r, 4.0), 2.0));\n"
                                    "    gl_FragColor = vec4(col, 1.0);\n}\n";
        const char* const kOdoSc = "// odo.sc: 8999001 (odd, past 2^23) in seven segments and in the font, each over the same digits.\n"
                                   "#include \"funk/text.sh\"\n#include \"funk/shapes.sh\"\n"
                                   "void main()\n{\n"
                                   "    float v = 8999001.0 + floor(in_level);\n"
                                   "    float pitch = 0.62 * 20.0;\n"
                                   "    float ink = number7(v_px, vec2(100.0, 20.0), 20.0, v, 7.0);\n"
                                   "    ink = max(ink, digit7(v_px, vec2(100.0 - 0.5 * pitch, 50.0), 20.0, 1.0));\n"
                                   "    ink = max(ink, digit7(v_px, vec2(100.0 - 1.5 * pitch, 50.0), 20.0, 0.0));\n"
                                   "    ink = max(ink, digit7(v_px, vec2(100.0 - 2.5 * pitch, 50.0), 20.0, 0.0));\n"
                                   "    ink = max(ink, digit7(v_px, vec2(100.0 - 3.5 * pitch, 50.0), 20.0, 9.0));\n"
                                   "    ink = max(ink, digit7(v_px, vec2(100.0 - 4.5 * pitch, 50.0), 20.0, 9.0));\n"
                                   "    ink = max(ink, digit7(v_px, vec2(100.0 - 5.5 * pitch, 50.0), 20.0, 9.0));\n"
                                   "    ink = max(ink, digit7(v_px, vec2(100.0 - 6.5 * pitch, 50.0), 20.0, 8.0));\n"
                                   "    ink = max(ink, number(v_px, vec2(8.0, 78.0), 12.0, 0.0, v, 0.0));\n"
                                   "    ink = max(ink, text(v_px, vec2(8.0, 98.0), 12.0, 0.0, \"8999001\"));\n"
                                   "    gl_FragColor = vec4(vec3_splat(ink), 1.0);\n}\n";
        const std::string textGui = "panel { row {\n"
                                    "    shader \"text.sc\" inputs=\"cutoff level\" width=240 height=120\n"
                                    "    shader \"dash.sc\" inputs=\"cutoff level\" width=200 height=120\n"
                                    "    shader \"odo.sc\" inputs=\"level\" width=200 height=120\n"
                                    "} row { knob cutoff; knob level } }\n";
        const std::string firstWords = "A STRING LONGER THAN TWELVE";
        const pg::Sources textFiles{ { pg::kProgramName, program() }, { "panel.gui", textGui }, { "text.sc", kTextSc },
                                     { "ink.sh", inkSh(firstWords) }, { "dash.sc", kDashSc }, { "other.sh", "// no shader includes this\n" },
                                     { "odo.sc", kOdoSc } };
        load(processor, textFiles);
        processor.settle();
        host.tick(2);
        host.settle(30);
        const pg::CompileStatus ts = port.status();
        std::printf("TEXT notes:\n%s", ts.notes.c_str());
        const bool textBuilt = ts.ok && ts.panel.shaders.size() == 3 && !ts.panel.shaders[0].binary.empty() && !ts.panel.shaders[1].binary.empty()
                               && !ts.panel.shaders[2].binary.empty();
        P.eq("text.built", textBuilt && !has(ts.notes, "text.sc") && !has(ts.notes, "ink.sh") && !has(ts.notes, "dash.sc")
                               && !has(ts.notes, "odo.sc") ? 1 : 0, 1);
        const uint32_t textId = textBuilt ? ts.panel.shaders[0].id : 0u;
        const pg::ui::GuiShaderItem* textItem = shaderShowing(panel, "text.sc");
        const pg::ui::GuiShaderItem* dashItem = shaderShowing(panel, "dash.sc");
        P.eq("text.registered", textItem != nullptr && dashItem != nullptr && textItem->program() == textId && textId != 0
                                    && funkgui::shaders::hasTexture(lib::kTextureId) ? 1 : 0, 1);
        P.eq("text.still", textItem != nullptr && textItem->settled() ? 1 : 0, 1);

        // On the GPU, against the reference: each pixel of text.sc's cell what text.sh's arithmetic gives there.
        constexpr float kDpi = 2.0f;
        const TextRef ref{ atlas.texels, 1.0f / kDpi };
        const auto compare = [&](const char* name, const char* png) {
            host.settle(10);
            const funkgui::PrimList& frame = host.draw();
            const pg::picture::Drawn g = pg::picture::draw(frame);
            if (png != nullptr && g.image.w > 0)
                funkgui::writePng(g.image, probe::outPath(png).c_str());
            if (!g.gpu && !probe::noGpuPictures().empty())
            {
                for (const char* row : { ".gpu", ".inked", ".inked_as_reference", ".mean_difference", ".over_16_share" })
                    probe::skip(P, std::string(name) + row, probe::noGpuPicturesSkip());
                return;
            }
            if (textItem == nullptr || !g.gpu)
            {
                P.eq((std::string(name) + ".gpu").c_str(), 0, 1);
                return;
            }
            const funkgui::Rect cell = textItem->cell();
            const float cutoff = textItem->inputs()[0], level = textItem->inputs()[1];
            const std::vector<int> first = [] {
                std::vector<int> v = codesOf("RPM 0-9 ");
                v.push_back(lib::glyphCode(0x00B0));
                v.push_back('F');
                return v;
            }();
            const Px box = pixelsOf(g.image, cell, kDpi, 0.0f);
            int worst = 0, inked = 0, inkedRef = 0, n = 0, over16 = 0;
            double sum = 0.0;
            for (int y = box.y0; y < box.y1; ++y)
                for (int x = box.x0; x < box.x1; ++x, ++n)
                {
                    const float px = (static_cast<float>(x) + 0.5f) / kDpi - cell.x, py = (static_cast<float>(y) + 0.5f) / kDpi - cell.y;
                    float ink = ref.text(px, py, 8.0f, 14.0f, 14.0f, 0.0f, first);
                    ink = std::max(ink, ref.text(px, py, 0.5f * cell.w, 38.0f, 20.0f, 0.5f, codesOf("Hg")));
                    ink = std::max(ink, ref.text(px, py, cell.w - 8.0f, 64.0f, 16.0f, 1.0f, TextRef::numberCodes(level * 8000.0f, 0)));
                    ink = std::max(ink, ref.text(px, py, 8.0f, 64.0f, 12.0f, 0.0f, TextRef::numberCodes(-cutoff * 10.0f, 2)));
                    ink = std::max(ink, ref.text(px, py, 8.0f, 92.0f, 9.0f, 0.0f, codesOf(firstWords)));
                    const int want = static_cast<int>(std::lround(ink * 255.0f));
                    const int got = at(g.image, x, y)[0];
                    const int d = std::abs(got - want);
                    worst = std::max(worst, d);
                    over16 += d > 16 ? 1 : 0;
                    sum += d;
                    inked += got > 128 ? 1 : 0;
                    inkedRef += want > 128 ? 1 : 0;
                }
            std::printf("%s: text.sc's cell %d x %d px on the GPU against the reference: the mean difference %.3f, the "
                        "largest %d, %d over 16; %d px inked (the reference %d); cutoff %.3f, level %.3f\n",
                        name, box.x1 - box.x0, box.y1 - box.y0, sum / std::max(n, 1), worst, over16, inked, inkedRef,
                        static_cast<double>(cutoff), static_cast<double>(level));
            P.eq((std::string(name) + ".gpu").c_str(), 1, 1);
            P.ge((std::string(name) + ".inked").c_str(), static_cast<double>(inkedRef), 400.0);
            P.near((std::string(name) + ".inked_as_reference").c_str(), static_cast<double>(inked), static_cast<double>(inkedRef),
                   std::max(20.0, 0.03 * inkedRef));
            P.le((std::string(name) + ".mean_difference").c_str(), sum / std::max(n, 1), 1.5);
            P.le((std::string(name) + ".over_16_share").c_str(), static_cast<double>(over16) / std::max(n, 1), 0.01);
        };
        compare("text.draw", "gui.shaders.text.png");

        // Past 2^23 (odo.sc): number7's and number()'s 8999001 the same pixels as the digits drawn one by one under
        // each (a float's x + 0.5 would round the tie to even, an 8999002); dash.sc's ticks as wide on both sides of
        // the first and last marks, either way round (the outer scale from -135, the inner one from 135).
        {
            const funkgui::PrimList& frame = host.draw();
            const pg::picture::Drawn g = pg::picture::draw(frame);
            const pg::ui::GuiShaderItem* odoItem = shaderShowing(panel, "odo.sc");
            const auto sameAsBelow = [&](const std::string& name, float top, float height, float below) {
                if (!g.gpu && !probe::noGpuPictures().empty())
                {
                    probe::skip(P, name, probe::noGpuPicturesSkip());
                    probe::skip(P, name + "_inked", probe::noGpuPicturesSkip());
                    return;
                }
                if (odoItem == nullptr || !g.gpu)
                {
                    P.eq(name.c_str(), 0, 1);
                    return;
                }
                funkgui::Rect band = odoItem->cell();
                band.y += top;
                band.h = height;
                const Px box = pixelsOf(g.image, band, kDpi, 0.0f);
                const int shift = static_cast<int>(std::lround(below * kDpi));
                int differ = 0, inked = 0, worst = 0;
                for (int y = box.y0; y < box.y1 && y + shift < g.image.h; ++y)
                    for (int x = box.x0; x < box.x1; ++x)
                    {
                        const int a = at(g.image, x, y)[0], b = at(g.image, x, y + shift)[0];
                        worst = std::max(worst, std::abs(a - b));
                        differ += std::abs(a - b) > 16 ? 1 : 0;
                        inked += a > 128 ? 1 : 0;
                    }
                std::printf("%s: %d px inked, %d differ from the digits %.0f px under them by more than 16 (the most %d)\n",
                            name.c_str(), inked, differ, static_cast<double>(below), worst);
                P.eq(name.c_str(), differ, 0);
                P.ge((name + "_inked").c_str(), static_cast<double>(inked), 200.0);
            };
            sameAsBelow("text.number7_large", 6.0f, 28.0f, 30.0f);
            sameAsBelow("text.number_large", 68.0f, 18.0f, 20.0f);

            // A mark's ink (its red over the background's, near it) on each side of its centre line: the less over the more.
            const auto evenness = [&](const char* name, float angle, float r0, float r1) {
                if (!g.gpu && !probe::noGpuPictures().empty())
                {
                    probe::skip(P, name, probe::noGpuPicturesSkip());
                    return;
                }
                if (dashItem == nullptr || !g.gpu)
                {
                    P.eq(name, 0, 1);
                    return;
                }
                const funkgui::Rect cell = dashItem->cell();
                const float cx = 0.5f * cell.w, cy = 0.56f * cell.h, rad = 0.4f * cell.h;
                const float a = angle * 3.14159265f / 180.0f;
                const float dx = std::sin(a), dy = -std::cos(a);     // along the mark (y down)
                const Px box = pixelsOf(g.image, cell, kDpi, 0.0f);
                std::vector<std::pair<float, int>> samples;          // across the mark (px), red
                double background = 0.0;
                int counted = 0;
                for (int y = box.y0; y < box.y1; ++y)
                    for (int x = box.x0; x < box.x1; ++x)
                    {
                        const float px = (static_cast<float>(x) + 0.5f) / kDpi - cell.x - cx;
                        const float py = (static_cast<float>(y) + 0.5f) / kDpi - cell.y - cy;
                        const float along = px * dx + py * dy, across = px * -dy + py * dx;
                        if (along < r0 * rad + 1.0f || along > r1 * rad - 1.0f)
                            continue;
                        const int red = at(g.image, x, y)[0];
                        if (std::fabs(across) > 2.5f && std::fabs(across) < 4.0f)
                        {
                            background += red;
                            ++counted;
                        }
                        else if (std::fabs(across) <= 1.5f)
                            samples.emplace_back(across, red);
                    }
                background /= std::max(counted, 1);
                double side[2] = { 0.0, 0.0 };
                for (const auto& [across, red] : samples)
                    side[across < 0.0f ? 0 : 1] += std::max(0.0, red - background);
                const double even = std::max(side[0], side[1]) > 0.0 ? std::min(side[0], side[1]) / std::max(side[0], side[1]) : 0.0;
                std::printf("%s: the mark at %.0f deg, its ink on the two sides of its centre line %.0f and %.0f (the "
                            "background %.1f): %.3f\n", name, static_cast<double>(angle), side[0], side[1], background, even);
                P.ge(name, even, 0.8);
            };
            evenness("text.ticks_first_even", -135.0f, 0.82f, 1.0f);
            evenness("text.ticks_reverse_first_even", 135.0f, 0.56f, 0.74f);
            evenness("text.ticks_reverse_last_even", -135.0f, 0.56f, 0.74f);
        }
        // The numbers follow their inputs: the level at 1/8000 of its range (a "1"), the cutoff at 0.9 ("-9.00").
        juce::AudioProcessorParameter& cutoffParam = *processor.getParameters()[0];
        juce::AudioProcessorParameter& levelParam = *processor.getParameters()[1];
        cutoffParam.setValueNotifyingHost(0.9f);
        levelParam.setValueNotifyingHost(1.0f / 8000.0f);
        host.tick(20);
        P.near("text.inputs_follow", textItem != nullptr ? static_cast<double>(textItem->inputs()[1]) : 0.0, 1.0 / 8000.0, 1e-5);
        compare("text.numbers_moved", "gui.shaders.text.moved.png");
        cutoffParam.setValueNotifyingHost(0.25f);
        levelParam.setValueNotifyingHost(0.5f);
        host.tick(20);

        // The cache's key holds every file a shader includes: ink.sh changed, text.sc builds again (another program); a
        // .sh no shader includes changed, nothing builds.
        {
            const uint64_t runs = pg::shadercRuns();
            pg::Sources other = textFiles;
            other[5].text = "// still no shader includes this, changed\n";
            load(processor, other);
            processor.settle();
            const pg::CompileStatus o = port.status();
            P.eq("includes.unrelated_same", o.panel.shaders.size() == 3 && o.panel.shaders[0].id == textId ? 1 : 0, 1);
            P.eq("includes.unrelated_no_build", static_cast<int64_t>(pg::shadercRuns() - runs), 0);
            pg::Sources changed = textFiles;
            changed[3].text = inkSh("ANOTHER STRING, AS LONG");
            load(processor, changed);
            processor.settle();
            const pg::CompileStatus c = port.status();
            P.eq("includes.header_rebuilds", c.panel.shaders.size() == 3 && c.panel.shaders[0].id != textId && c.panel.shaders[0].id != 0
                                                 && c.panel.shaders[1].id == ts.panel.shaders[1].id
                                                 && c.panel.shaders[2].id == ts.panel.shaders[2].id ? 1 : 0, 1);
            P.eq("includes.header_one_build", static_cast<int64_t>(pg::shadercRuns() - runs), 1);
            // A .sh file that does not compile: the note on its own line.
            pg::Sources broken = textFiles;
            broken[3].text = "#include \"funk/text.sh\"\n\nfloat label(vec2 p)\n{\n    return notDefinedHere(p);\n}\n";
            load(processor, broken);
            processor.settle();
            const pg::CompileStatus b = port.status();
            std::printf("a broken ink.sh: %s", b.notes.c_str());
            P.eq("includes.header_note", has(b.notes, "ink.sh:5:1: note: the shader does not compile:") && has(b.notes, "notDefinedHere")
                                             && b.ok ? 1 : 0, 1);
            // ... and text.sc, which includes it, says so: its placeholder, its screen reader's value and view_panel's
            // element line give ink.sh's note, at its line.
            host.tick(2);
            const pg::ui::GuiShaderItem* brokenText = shaderShowing(panel, "text.sc");
            char line[512] = {};
            if (brokenText != nullptr)
                brokenText->specLine(line, sizeof line);
            std::printf("text.sc with ink.sh broken: %s\n", line);
            bool spoken = false;
            for (const funkgui::A11yItem& a : host.accessibility())
                spoken = spoken || (a.title == "Shader text.sc" && has(a.value, "ink.sh:5: the shader does not compile:"));
            P.eq("includes.header_why", brokenText != nullptr && brokenText->program() == 0
                                            && brokenText->why().rfind("ink.sh:5: the shader does not compile:", 0) == 0
                                            && has(brokenText->why(), "notDefinedHere") && has(line, "shader text.sc   ink.sh:5: ")
                                            && spoken ? 1 : 0, 1);
            // An #include of a file there is none of, and a string too long: notes on their lines, nothing built.
            pg::Sources missing = textFiles;
            missing[2].text = std::string(kTextSc) + "#include \"nothere.sh\"\n";
            load(processor, missing);
            processor.settle();
            const pg::CompileStatus m = port.status();
            P.eq("includes.missing_note", has(m.notes, "text.sc:13:10: note: the shader does not compile: there is no file nothere.sh")
                                              && m.panel.shaders.size() == 3 && m.panel.shaders[0].binary.empty() ? 1 : 0, 1);
        }

        // Every renderer's profile: the library and both shaders compile for another renderer than this platform's too,
        // Vulkan's SPIR-V on macOS and Windows, Metal's on Linux (where SPIR-V is the panel's own); Direct3D 11's needs
        // Windows' compiler.
        {
            pg::ShaderTools other = tools();
#if defined(__linux__)
            other.platform = "osx";
            other.profile = "metal";
            const char* const key = "text.metal";
#else
            other.platform = "linux";
            other.profile = "spirv";
            const char* const key = "text.spirv";
#endif
            const pg::Sources heads = pg::shaderHeaders(textFiles);
            const pg::ShaderBuild a = pg::buildShader("text.sc", kTextSc, { "cutoff", "level" }, other, heads);
            const pg::ShaderBuild b = pg::buildShader("dash.sc", kDashSc, { "cutoff", "level" }, other, heads);
            const pg::ShaderBuild o = pg::buildShader("odo.sc", kOdoSc, { "level" }, other, heads);
            if (!a.ok() || !b.ok() || !o.ok())
                std::printf("%s:\n%s%s%s", other.profile.c_str(), a.error.c_str(), b.error.c_str(), o.error.c_str());
            P.eq(key, a.ok() && b.ok() && o.ok() && a.binary != ts.panel.shaders[0].binary ? 1 : 0, 1);
        }
        load(processor, textFiles);
        processor.settle();
        host.tick(2);
        host.settle(30);
    }

    return P.finish();
}
