cmake_minimum_required(VERSION 3.22)

# ──────────────────────────────────────────────────────────────────────────
# Rename points when forking this template:
#   1. project(...) name below
#   2. juce_add_plugin(...) target name and metadata (COMPANY_NAME,
#      PLUGIN_MANUFACTURER_CODE, PLUGIN_CODE, PRODUCT_NAME, BUNDLE_ID)
#   3. The target name in every target_sources / target_include_directories
#      / target_compile_definitions / target_link_libraries / target_compile_options
# ──────────────────────────────────────────────────────────────────────────

# Force a build type before anything else. Unix Makefiles is single-config,
# so without this CMAKE_BUILD_TYPE is empty and NDEBUG is *not* defined for
# our plugin TUs. visage exposes a LeakChecker member that exists only when
# NDEBUG is undefined — mismatched debug/release between visage's library
# and our plugin causes an ODR layout drift on visage::Canvas (extra member
# in debug), which silently corrupts state_.current_region reads from
# inlined template methods like Canvas::circle, leading to crashes inside
# ShapeBatcher::batchIndex on the first shape that isn't pre-instantiated
# in the visage static library (e.g. Circle).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# OBJCXX is required: visage's macOS backend uses Metal via Objective-C++.
project(TestingPlayground VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Apple Silicon only.
if(NOT APPLE)
    message(FATAL_ERROR "${PROJECT_NAME} targets Apple Silicon only.")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    message(FATAL_ERROR "${PROJECT_NAME} requires arm64 (Apple M-series).")
endif()

set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE)

# Pinned to a known-good commit. Bump deliberately — visage has no
# stable release tags, and its API moves.
FetchContent_Declare(
    visage
    GIT_REPOSITORY https://github.com/VitalAudio/visage.git
    GIT_TAG        b0b2ee89abb3f1173dad4d80460197eb96afa724)

FetchContent_MakeAvailable(JUCE visage)

juce_add_plugin(TestingPlayground
    COMPANY_NAME            "Funk"
    PLUGIN_MANUFACTURER_CODE Funk
    PLUGIN_CODE             Tstn
    FORMATS                 AU VST3 Standalone
    PRODUCT_NAME            "TestingPlayground"
    BUNDLE_ID               com.funk.testingplayground
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        FALSE
    NEEDS_MIDI_OUTPUT       FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE)

# Strip extended attributes left by a previous codesign run before JUCE's
# own ad-hoc signing step, preventing "detritus not allowed" errors on
# incremental rebuilds.
add_custom_command(TARGET TestingPlayground_AU PRE_BUILD
    COMMAND sh -c "xattr -cr '${CMAKE_BINARY_DIR}/TestingPlayground_artefacts/$<CONFIG>/AU/TestingPlayground.component' 2>/dev/null || true"
    VERBATIM)
add_custom_command(TARGET TestingPlayground_VST3 PRE_BUILD
    COMMAND sh -c "xattr -cr '${CMAKE_BINARY_DIR}/TestingPlayground_artefacts/$<CONFIG>/VST3/TestingPlayground.vst3' 2>/dev/null || true"
    VERBATIM)

# Embedded resources for the GUI: per-platform-compiled shaders and the
# Roboto font used for knob value text. visage_embed_shaders runs visage's
# bundled shaderc to transpile every .sc file in shaders/ into the
# platform's native format (Metal on macOS) and packs them as C++
# byte arrays exposed under namespace resources::shaders. Same idea for
# fonts via add_embedded_resources.
file(GLOB PLUGIN_SHADER_SOURCES shaders/[vf]s_*.sc)
file(GLOB PLUGIN_FONT_SOURCES   fonts/*.ttf)
visage_embed_shaders(EmbeddedShaderResources "plugin_shaders.h" "resources::shaders" "${PLUGIN_SHADER_SOURCES}")
add_embedded_resources(EmbeddedFontResources "plugin_fonts.h" "resources::fonts" "${PLUGIN_FONT_SOURCES}")

target_sources(TestingPlayground PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/StandaloneApp.cpp
    Source/gui/Knob.cpp
    Source/gui/GraphEditor.cpp)

# visage doesn't expose its module subdirs as public includes, so the
# headers we use (e.g. "visage_app/application_window.h") need explicit
# search paths.
target_include_directories(TestingPlayground PRIVATE
    Source
    ${visage_SOURCE_DIR}
    ${visage_SOURCE_DIR}/visage_app
    ${visage_SOURCE_DIR}/visage_graphics
    ${visage_SOURCE_DIR}/visage_ui
    ${visage_SOURCE_DIR}/visage_utils
    ${visage_SOURCE_DIR}/visage_widgets
    ${visage_SOURCE_DIR}/visage_windowing
    ${visage_SOURCE_DIR}/visage_file_embed)

target_compile_definitions(TestingPlayground PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # Suppress JUCE's default StandaloneFilterApp so our Source/StandaloneApp.cpp
    # provides juce_CreateApplication() instead. Lets us use a plain
    # DocumentWindow with the native macOS title bar and no JUCE banner.
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)

target_link_libraries(TestingPlayground PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    visage
    EmbeddedShaderResources
    EmbeddedFontResources
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Apple Silicon tuning. Applied only to our target, never to visage/JUCE.
# We intentionally do NOT pass -ffast-math — JUCE itself contains
# constexpr uses of std::numeric_limits<>::infinity(), which become UB
# under -ffinite-math-only and trigger compile errors.
target_compile_options(TestingPlayground PRIVATE
    -mcpu=apple-m1
    -fno-math-errno
    -fno-trapping-math)
