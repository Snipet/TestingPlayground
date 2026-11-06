// Source/fminus/Shaders.h: a .gui file's shaders (GuiSpec.h: shader "glow.sc" inputs="cutoff env out") built for the
// GPU (JUCE-free and FunkGui-free). A shader's file, one of the program's, holds the body of a fragment shader in bgfx's
// language (GLSL with bgfx's vec types and functions): helper functions and a main() that sets gl_FragColor, straight
// RGBA. shaderSource() puts FunkGui's head before it (funkgui/gpu/ShaderLibrary.h: $input v_uv, v_px, bgfx_shader.sh,
// u_viewSize, u_rect, u_params) and the plugin's names:
//
//   u_time      float  seconds, the editor's clock (it moves while the panel draws)
//   u_size      vec2   the element's size in px
//   in0 .. in7  float  its inputs in order; in_<name> the same by the input's name (in_cutoff, in_env, in_out):
//                      a parameter's value 0..1 (its knob's turn), a field's value as the program writes it, in and
//                      out the audio's level (its peak, 0..1)
// and v_uv (0..1 across, y down) and v_px (px from the top left corner), FunkGui's varyings.
//
// buildShader() runs bgfx's shaderc on it (about 40 to 60 ms; never on the audio thread) into the binary FunkGui's
// ShaderLibrary takes; shaderc's errors come back as a note's text on the file's own line. The renderer then checks the
// binary (ShaderTools::check: FunkGui's shaders::check, on macOS Metal's own compiler, milliseconds and more), so that
// the Panel's shaders::set() on the message thread compiles nothing; a binary the renderer refuses is a note too, and
// not handed on. Both run in buildAll(), each shader on a thread of its own but the first, which runs on buildAll's
// caller. The Compiler calls it twice for a compile: first on a thread it starts before FMinus compiles the program,
// for the shaders the .gui file names (readGuiShaders); then, once the compile is done and that thread joined, on the
// thread that compiles (the compiler's own for a RUN; compileNow's caller, often the message thread, for the
// processor's construction and a host's session or preset load), for the shaders the program has, which come from the
// cache by then unless the guess at their inputs was wrong. So shaderc and the check run while FMinus compiles, and
// the thread that compiles waits for what is left. A state load pays for the check whether or not an editor opens
// (FunkGui remembers a code it accepted, so another instance's load of the same shader does not compile it again).
//
// The cache (ShaderCache; shaderCache(), the process's, which every Compiler shares): a build is looked for in memory
// (the 64 used last), then on disk (ShaderTools::cacheDir, shaderCacheFolder(): kept between sessions, capped at
// cacheBytes, the files used least lately removed first), and only then made by shaderc; one that another thread is
// making is waited for, never made twice. A build is keyed by everything its bytes come from: the file's name, the
// source shaderc compiles (shaderSource: FunkGui's head, the plugin's names, the inputs, the text), FunkGui's varyings
// and bgfx_shader.sh, the platform and profile, and shaderc itself (its path, size and modification time: a new shaderc
// builds again). The disk keeps only builds that compiled, each in a file of its own written under a temporary name and
// renamed into place, so two instances or two processes writing one never leave half of it; a file that does not read
// back whole and exactly (its length, its checksum, its key) is ignored and made again. The process keeps each
// folder's size as a running total and scans the folder (for the cap, and for temporary files left long ago) only at
// its first write there, at a write that takes it over the cap and at every 64th (shaderCacheScans): a folder at its
// cap holds thousands of entries. buildAll() runs a panel's builds (four at most) at once, each then checked by the
// renderer, and shaderc runs at most kShadercAtOnce times at once in the process; since the Compiler starts them
// before FMinus compiles the program (above: readGuiShaders, the .gui file's shaders, their inputs taken at their
// word), a RUN waits for the longer of the two, not for their sum.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace playground
{
    // What building a shader needs, from the plugin: FunkGui's texts (ShaderLibrary.h) and shaderc, the pinned bgfx's.
    struct ShaderTools
    {
        std::string shaderc;                  // the executable's path ("" for none: shaders are left out, with a note)
#if defined(_WIN32)
        std::string platform = "windows";     // shaderc's --platform and -p for the renderer (ShaderLibrary's target():
        std::string profile = "s_5_0";        // Direct3D 11 on Windows, Metal on macOS)
#else
        std::string platform = "osx";         // shaderc's --platform and -p for the renderer (ShaderLibrary's target())
        std::string profile = "metal";
#endif
        std::string prelude;                  // funkgui::shaders::fragmentPrelude()
        std::string varyingDef;               // funkgui::shaders::varyingDef()
        std::string bgfxShaderSh;             // funkgui::shaders::bgfxShaderSh()
        // funkgui::shaders::check (any thread): whether the renderer takes a binary, else why; none: not checked here
        // (the Panel's shaders::set() checks it).
        std::function<bool(const std::vector<uint8_t>& binary, std::string& why)> check;
        // The disk cache (ShaderCache, above): its folder (shaderCacheFolder(); empty: none) and its cap in bytes.
        std::filesystem::path cacheDir;
        std::uintmax_t cacheBytes = 16u << 20;
        bool usable() const noexcept { return !shaderc.empty() && !prelude.empty() && !varyingDef.empty(); }
    };

    struct ShaderBuild
    {
        std::vector<uint8_t> binary;          // shaderc's output, when it compiled
        std::string error;                    // why not: "glow.sc:4: note: the shader does not compile: ..." lines
        // buildAll()'s check: the renderer refused the binary (ShaderTools::check), and why ("" when it gave no reason).
        bool refused = false;
        std::string refusal;
        bool ok() const noexcept { return !binary.empty(); }
    };

    // A shader to build: its file's name and text, and its inputs' names (PanelShader's).
    struct ShaderJob
    {
        std::string file;
        std::string body;
        std::vector<std::string> inputs;
    };

    inline constexpr int kShadercAtOnce = 4;  // shaderc's runs at once in a process, at most (fewer on fewer cores)

    // The fragment shader shaderc compiles: FunkGui's head, the plugin's names for `inputs` (up to eight names), and
    // the file's text. bodyLine: the line of the result where the file's first line is.
    std::string shaderSource(const std::string& body, const std::vector<std::string>& inputs, const ShaderTools& tools,
                             int* bodyLine = nullptr);

    // shaderc on shaderSource(body, inputs), in a folder of its own under the system's temporary one (removed after).
    // `file` names the shader in the error's lines.
    ShaderBuild buildShader(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                            const ShaderTools& tools);

    // The builds seen last, in memory and on disk (above). Thread-safe.
    class ShaderCache
    {
    public:
        // A shader's build: the memory's, the disk's (tools.cacheDir), one another thread is making, else shaderc's (a
        // build shaderc could not run for, no temporary folder or a time-out, is never kept).
        ShaderBuild build(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                          const ShaderTools& tools);
        // Several builds at once, each as build() makes it on a thread of its own (the first on the calling one) and then
        // checked by the renderer (tools.check, when given: ShaderBuild::refused): their builds in the jobs' order.
        std::vector<ShaderBuild> buildAll(const std::vector<ShaderJob>& jobs, const ShaderTools& tools);

        // Where this cache's builds came from (probes).
        struct Counts
        {
            uint64_t memory = 0;              // found in memory
            uint64_t waited = 0;              // another thread's build, waited for
            uint64_t disk = 0;                // read from the disk cache
            uint64_t built = 0;               // made by shaderc (buildShader)
        };
        Counts counts() const;
        // Lets go of the memory's builds, as a new process starts with none (the disk cache stays). Probes only.
        void forget();

        static constexpr std::size_t kMemoryBuilds = 64;

    private:
        mutable std::mutex mutex_;
        std::map<std::string, std::pair<uint64_t, ShaderBuild>> builds_;   // by the key, with when it was used
        std::map<std::string, std::shared_future<ShaderBuild>> making_;    // by the key: the builds under way
        uint64_t clock_ = 0;
        Counts counts_;
    };

    // The process's cache, every Compiler's: a second instance of a program builds nothing.
    ShaderCache& shaderCache();

    // The user's folder for the disk cache (ShaderTools::cacheDir): ~/Library/Caches/<Company>/<Name>/Shaders on macOS,
    // %LOCALAPPDATA%\<Company>\<Name>\Shaders on Windows, $XDG_CACHE_HOME (else ~/.cache)/<Company>/<Name>/Shaders
    // elsewhere (Product.h's names); under CTest (<ENV_PREFIX>PREFS_DIR set) that folder's shaders/, never the user's.
    // Empty when there is none.
    std::filesystem::path shaderCacheFolder();

    // The times this process started shaderc (buildShader), for the probes.
    uint64_t shadercRuns();
    // The times this process scanned a disk cache's folder for its size (a write's: the process's first there, one that
    // takes it over its cap, and every 64th), for the probes.
    uint64_t shaderCacheScans();

    // A shader binary's id for FunkGui's ShaderLibrary: its bytes' hash, never 0 (the same shader, the same id).
    uint32_t shaderId(const std::vector<uint8_t>& binary);

    // Whether a shader's file reads the editor's clock: u_time (or u_viewSize, whose w it is) as a word of its code,
    // never in a comment (// to the line's end, a backslash at its end going on, and /* */), nor part of a longer
    // name or a number. Such a shader changes every frame (the Panel draws it at full rate); any other only when an
    // input does.
    bool shaderAnimates(const std::string& body);
}
