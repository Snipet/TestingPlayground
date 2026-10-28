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
// buildShader() runs bgfx's shaderc on it (Compiler's thread: about 60 ms; never the audio or the message thread) into
// the binary FunkGui's ShaderLibrary takes. shaderc's errors come back as a note's text on the file's own line.
#pragma once

#include <cstdint>
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
        std::string platform = "osx";         // shaderc's --platform and -p for the renderer (ShaderLibrary's target())
        std::string profile = "metal";
        std::string prelude;                  // funkgui::shaders::fragmentPrelude()
        std::string varyingDef;               // funkgui::shaders::varyingDef()
        std::string bgfxShaderSh;             // funkgui::shaders::bgfxShaderSh()
        bool usable() const noexcept { return !shaderc.empty() && !prelude.empty() && !varyingDef.empty(); }
    };

    struct ShaderBuild
    {
        std::vector<uint8_t> binary;          // shaderc's output, when it compiled
        std::string error;                    // why not: "glow.sc:4: note: the shader does not compile: ..." lines
        bool ok() const noexcept { return !binary.empty(); }
    };

    // The fragment shader shaderc compiles: FunkGui's head, the plugin's names for `inputs` (up to eight names), and
    // the file's text. bodyLine: the line of the result where the file's first line is.
    std::string shaderSource(const std::string& body, const std::vector<std::string>& inputs, const ShaderTools& tools,
                             int* bodyLine = nullptr);

    // shaderc on shaderSource(body, inputs), in a folder of its own under the system's temporary one (removed after).
    // `file` names the shader in the error's lines.
    ShaderBuild buildShader(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                            const ShaderTools& tools);

    // The builds of the sources seen last (a program run again with the same shader does not run shaderc again), the
    // 32 newest. Thread-safe.
    class ShaderCache
    {
    public:
        ShaderBuild build(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                          const ShaderTools& tools);

    private:
        std::mutex mutex_;
        std::map<std::string, std::pair<uint64_t, ShaderBuild>> builds_;   // by the source, with when it was used
        uint64_t clock_ = 0;
    };

    // A shader binary's id for FunkGui's ShaderLibrary: its bytes' hash, never 0 (the same shader, the same id).
    uint32_t shaderId(const std::vector<uint8_t>& binary);
}
