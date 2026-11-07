// Source/editor/nogpu/ShaderLibraryNone.cpp: funkgui/gpu/ShaderLibrary.h's functions in a build without FunkGui's GPU
// layer (FP_WITH_GPU off): there is no renderer to give a shader to, so none is registered (SoftRaster draws each
// shader draw as its striped placeholder, as every probe PNG does), no texture is kept, and the texts a shader is
// built with are empty. Such a build bundles no shaderc either (Processor.cpp's shaderTools), so a .gui file's shader
// says it draws nothing. CMakeLists.txt compiles this into the plugin, the probes and the sandbox's helper in place of
// FunkGui::gpu's ShaderLibrary.cpp.
#include <funkgui/gpu/ShaderLibrary.h>

namespace funkgui::shaders
{
    Target target() { return {}; }

    bool set(uint32_t, const void*, uint32_t, std::string& why)
    {
        why = "this build has no GPU renderer: shaders draw as placeholders";
        return false;
    }

    bool check(const void*, uint32_t, std::string& why)
    {
        why = "this build has no GPU renderer: shaders draw as placeholders";
        return false;
    }

    void remove(uint32_t) {}

    bool has(uint32_t) { return false; }

    const char* varyingDef() { return ""; }

    const char* fragmentPrelude() { return ""; }

    const char* bgfxShaderSh() { return ""; }

    bool setTexture(uint32_t, int, int, TextureFormat, const void*, int, TextureSampling) { return false; }

    void removeTexture(uint32_t) {}

    bool hasTexture(uint32_t) { return false; }

    bool bindTexture(uint32_t, std::string_view, uint32_t) { return false; }

    void unbindTexture(uint32_t, std::string_view) {}
}
