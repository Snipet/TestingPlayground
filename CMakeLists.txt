# The plugin (AU, VST3, Standalone; macOS arm64) on JUCE 8.0.4 and FunkGui, with FSynth's DSP core vendored in
# Source/fsdsp/core. Use the presets (CMakePresets.json):
#
#   cmake --workflow --preset dev-verify     configure, build (plugin + probes) and run every test, in build-dev
#   scripts/test.sh                          build and run the probes only
#
# Layout (README.md has the whole story):
#   cmake/Product.cmake      the identity: name, kind (effect | instrument), codes, bundle id, namespace
#   Source/fsdsp/core/**     the vendored DSP core (SEED.tsv; scripts/sync-dsp-core.sh), JUCE-free
#   Source/dsp/**            this plugin's algorithms, JUCE-free                    -> <name>_dsp (static library)
#   Source/fminus/*.cpp      a program, compiled for the plugin (Patch.h), JUCE-free -> <name>_fminus, which _dsp links
#   Source/fminus/examples/  the editor's EXAMPLES, the first the default program   -> generated/Examples.h
#   Source/plugin/*.cpp      the JUCE AudioProcessor                                -> the plugin and the probes
#   Source/editor/**/*.cpp   the FunkGui Panel, minus gpu/                          -> the plugin and the probes
#   Source/editor/gpu/*.cpp  the EditorHost factory (GPU)                           -> the plugin only
#   Tools/probes/**/*.cpp    probes on FunkGui's Harness, self-registered in CTest  -> <name>_probes
# Every list is a CONFIGURE_DEPENDS glob: a new source file needs no CMake edit.
cmake_minimum_required(VERSION 3.30)                     # CMP0168: FetchContent populates without a sub-build

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/Product.cmake)
project(${PRODUCT_NAME} VERSION ${PRODUCT_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Release unless told otherwise: an empty build type means no optimisation at all.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT APPLE)
  message(FATAL_ERROR "${PROJECT_NAME} builds on macOS only (arm64).")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  message(FATAL_ERROR "${PROJECT_NAME} builds for arm64 only: configure with a preset (CMakePresets.json), which sets "
                      "CMAKE_OSX_ARCHITECTURES=arm64.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
  message(FATAL_ERROR "${PROJECT_NAME} needs Apple clang (Xcode or its command-line tools)")
endif()

# ---- options ---------------------------------------------------------------------------------------------------------
option(FP_WERROR "-Werror on our own sources (never on JUCE's, FunkGui's or bgfx's)" ON)
option(FP_LTO    "-flto in Release builds"                                         ON)
set(FP_DEPS_DIR "$ENV{HOME}/audio/.deps" CACHE PATH "Read-only machine cache of JUCE, bgfx.cmake and shaderc")
if(EXISTS "$ENV{HOME}/audio/libraries/FunkGui/.git")
  set(_fp_funkgui_repo "$ENV{HOME}/audio/libraries/FunkGui")
else()
  set(_fp_funkgui_repo "https://github.com/Snipet/FunkGui.git")
endif()
set(FP_FUNKGUI_REPO "${_fp_funkgui_repo}" CACHE STRING
    "FunkGui's repository: the local one when this machine has it, else GitHub (the tag is cmake/Deps.cmake's)")

# ---- the product (cmake/Product.cmake) -------------------------------------------------------------------------------
if(NOT PRODUCT_NAME MATCHES "^[A-Z][A-Za-z0-9]*$")
  message(FATAL_ERROR "cmake/Product.cmake: PRODUCT_NAME '${PRODUCT_NAME}' must be PascalCase letters and digits")
endif()
if(PRODUCT_KIND STREQUAL "instrument")
  set(PRODUCT_IS_INSTRUMENT true)
elseif(PRODUCT_KIND STREQUAL "effect")
  set(PRODUCT_IS_INSTRUMENT false)
else()
  message(FATAL_ERROR "cmake/Product.cmake: PRODUCT_KIND '${PRODUCT_KIND}' must be effect or instrument")
endif()
foreach(_code PRODUCT_MFR_CODE PRODUCT_CODE)
  if(NOT ${_code} MATCHES "^[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]$" OR NOT ${_code} MATCHES "[A-Z]")
    message(FATAL_ERROR "cmake/Product.cmake: ${_code} '${${_code}}' must be 4 letters or digits, one upper-case")
  endif()
endforeach()
if(NOT PRODUCT_NAMESPACE MATCHES "^[a-z_][a-z0-9_]*$")
  message(FATAL_ERROR "cmake/Product.cmake: PRODUCT_NAMESPACE '${PRODUCT_NAMESPACE}' must be a lower-case identifier")
endif()
# The sources spell the namespace out; a hand edit of one side only would fail later with a confusing error.
file(STRINGS "${PROJECT_SOURCE_DIR}/Source/plugin/Processor.h" _ns REGEX "^namespace ${PRODUCT_NAMESPACE}$")
if(NOT _ns)
  message(FATAL_ERROR "cmake/Product.cmake: PRODUCT_NAMESPACE is '${PRODUCT_NAMESPACE}', but Source/plugin/"
                      "Processor.h has no 'namespace ${PRODUCT_NAMESPACE}' line. Rename with scripts/plugin.py rename.")
endif()

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(Flags)
include(Deps)

set(FP_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
configure_file(${PROJECT_SOURCE_DIR}/cmake/Product.h.in ${FP_GENERATED_DIR}/Product.h @ONLY)

# The editor's EXAMPLES (Source/fminus/examples/*.fm, in file-name order), compiled into the plugin: the first is the
# program a new instance plays. Each file's first line names it, "// <Title>: what it is". Adding or editing one
# reconfigures, so the copies follow.
file(GLOB FP_FMINUS_EXAMPLES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/Source/fminus/examples/*.fm)
list(SORT FP_FMINUS_EXAMPLES)
if(NOT FP_FMINUS_EXAMPLES)
  message(FATAL_ERROR "Source/fminus/examples/ has no .fm file: the plugin needs a default program")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FP_FMINUS_EXAMPLES})
set(FP_FMINUS_EXAMPLE_ROWS "")
foreach(_f IN LISTS FP_FMINUS_EXAMPLES)
  file(READ ${_f} _text)
  if(_text MATCHES "\\)fminus\"")
    message(FATAL_ERROR "${_f}: the text )fminus\" would end the raw string it is compiled into")
  endif()
  if(NOT _text MATCHES "^// ([^:\n]+):")
    message(FATAL_ERROR "${_f}: the first line must be '// <Title>: ...', the title the EXAMPLES menu shows")
  endif()
  string(APPEND FP_FMINUS_EXAMPLE_ROWS "        Example{ \"${CMAKE_MATCH_1}\", R\"fminus(${_text})fminus\" },\n")
endforeach()
file(CONFIGURE OUTPUT ${FP_GENERATED_DIR}/Examples.h @ONLY CONTENT [=[// Generated by CMakeLists.txt from Source/fminus/examples/*.fm: the editor's EXAMPLES, as they were when the plugin
// was built. The first is the program a new instance plays.
#pragma once

namespace @PRODUCT_NAMESPACE@
{
    struct Example
    {
        const char* title;
        const char* source;
    };

    inline constexpr Example kExamples[] = {
@FP_FMINUS_EXAMPLE_ROWS@    };
}
]=])

# The editor's LIBRARY (Source/fminus/library/<n>-<category>/*.fm, in folder and then file-name order), compiled into
# the plugin: building blocks a program takes as tabs of its own and #includes (the LIBRARY menu, Claude's
# add_library_file). Each file's first line is "// <its file name>: what it is"; a folder's name, its number and
# dashes dropped, is its category.
file(GLOB FP_FMINUS_LIBRARY CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/Source/fminus/library/*/*.fm)
list(SORT FP_FMINUS_LIBRARY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FP_FMINUS_LIBRARY})
set(FP_FMINUS_LIBRARY_ROWS "")
foreach(_f IN LISTS FP_FMINUS_LIBRARY)
  file(READ ${_f} _text)
  get_filename_component(_name ${_f} NAME)
  get_filename_component(_folder ${_f} DIRECTORY)
  get_filename_component(_folder ${_folder} NAME)
  string(REGEX REPLACE "^[0-9]+-" "" _category "${_folder}")
  string(REPLACE "-" " " _category "${_category}")
  if(_text MATCHES "\\)fminus\"")
    message(FATAL_ERROR "${_f}: the text )fminus\" would end the raw string it is compiled into")
  endif()
  if(NOT _name MATCHES "^[A-Za-z0-9_][A-Za-z0-9_.-]*\\.fm$")
    message(FATAL_ERROR "${_f}: a library file's name is a tab's name (letters, digits, _, - and .), ending .fm")
  endif()
  if(NOT _text MATCHES "^// ([^:\n]+): ([^\n\"\\\\]+)\n")
    message(FATAL_ERROR "${_f}: the first line must be '// ${_name}: what it is' (no quotes or backslashes)")
  endif()
  if(NOT CMAKE_MATCH_1 STREQUAL _name)
    message(FATAL_ERROR "${_f}: the first line names ${CMAKE_MATCH_1}, not ${_name}")
  endif()
  string(APPEND FP_FMINUS_LIBRARY_ROWS
         "        LibraryFile{ \"${_name}\", \"${_category}\", \"${CMAKE_MATCH_2}\", R\"fminus(${_text})fminus\" },\n")
endforeach()
file(CONFIGURE OUTPUT ${FP_GENERATED_DIR}/Library.h @ONLY CONTENT [=[// Generated by CMakeLists.txt from Source/fminus/library/*/*.fm: the editor's LIBRARY, as it was when the plugin
// was built.
#pragma once

#include <string_view>

namespace @PRODUCT_NAMESPACE@
{
    struct LibraryFile
    {
        const char* name;                   // the tab's name: "svf.fm"
        const char* category;               // its folder's: "filters"
        const char* summary;                // its first line's, after the name
        const char* source;
    };

    inline constexpr LibraryFile kLibrary[] = {
@FP_FMINUS_LIBRARY_ROWS@    };

    // The library's file of that name, or null.
    inline const LibraryFile* findLibraryFile(std::string_view name)
    {
        for (const LibraryFile& f : kLibrary)
            if (name == f.name)
                return &f;
        return nullptr;
    }
}
]=])

# ---- sources ---------------------------------------------------------------------------------------------------------
set(FP_SOURCE_ROOT ${PROJECT_SOURCE_DIR}/Source)
file(GLOB_RECURSE FP_DSP_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/fsdsp/*.cpp ${FP_SOURCE_ROOT}/dsp/*.cpp)
file(GLOB FP_PLUGIN_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/plugin/*.cpp)
list(FILTER FP_PLUGIN_SOURCES EXCLUDE REGEX "/CreateEditor[^/]*\\.cpp$")
set(FP_CREATE_EDITOR_GENERIC ${FP_SOURCE_ROOT}/plugin/CreateEditorGeneric.cpp)        # the probes' editor factory
file(GLOB_RECURSE FP_EDITOR_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/*.cpp)
list(FILTER FP_EDITOR_SOURCES EXCLUDE REGEX "/Source/editor/gpu/")
file(GLOB FP_EDITOR_GPU_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/gpu/*.cpp ${FP_SOURCE_ROOT}/editor/gpu/*.mm)

file(GLOB FP_FMINUS_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/fminus/*.cpp)

# ---- FMinus: the JIT compiler, through its public API (fminus/FMinus.h), and the program (Patch.h), JUCE-free ---------
# FMinus::FMinus is FMinus's public API: fminus/FMinus.h needs C++17 and has no LLVM in it, so every file of the plugin
# may include it with the plugin's own C++20 and flags. LLVM is linked into the plugin, but its headers never reach it.
add_library(${PRODUCT_NAME}_fminus STATIC ${FP_FMINUS_SOURCES})
set_target_properties(${PRODUCT_NAME}_fminus PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(${PRODUCT_NAME}_fminus PUBLIC ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR})
target_link_libraries(${PRODUCT_NAME}_fminus PUBLIC fp_flags FMinus::FMinus PRIVATE fp_warnings fp_lto)

# ---- the DSP library: Source/fsdsp + Source/dsp, JUCE-free (no JUCE include path reaches it) -------------------------
add_library(${PRODUCT_NAME}_dsp STATIC ${FP_DSP_SOURCES})
set_target_properties(${PRODUCT_NAME}_dsp PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(${PRODUCT_NAME}_dsp PUBLIC ${FP_SOURCE_ROOT})
target_link_libraries(${PRODUCT_NAME}_dsp PUBLIC fp_flags ${PRODUCT_NAME}_fminus PRIVATE fp_warnings fp_lto)

# ---- the plugin ------------------------------------------------------------------------------------------------------
if(PRODUCT_KIND STREQUAL "instrument")
  set(_fp_plugin_kind IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE VST3_CATEGORIES Instrument Synth
                      AU_MAIN_TYPE kAudioUnitType_MusicDevice MICROPHONE_PERMISSION_ENABLED FALSE)
else()
  set(_fp_plugin_kind IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE VST3_CATEGORIES Fx
                      AU_MAIN_TYPE kAudioUnitType_Effect MICROPHONE_PERMISSION_ENABLED TRUE
                      MICROPHONE_PERMISSION_TEXT "${PRODUCT_NAME} processes the audio input.")
endif()
juce_add_plugin(${PRODUCT_NAME}
    COMPANY_NAME             "${PRODUCT_COMPANY}"
    PLUGIN_MANUFACTURER_CODE ${PRODUCT_MFR_CODE}
    PLUGIN_CODE              ${PRODUCT_CODE}
    FORMATS                  AU VST3 Standalone
    PRODUCT_NAME             "${PRODUCT_NAME}"
    VERSION                  ${PROJECT_VERSION}
    BUNDLE_ID                ${PRODUCT_BUNDLE_ID}
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    ${_fp_plugin_kind}
    COPY_PLUGIN_AFTER_BUILD  FALSE)                      # scripts/install.sh copies them, deliberately

set(_fp_plugin_own ${FP_PLUGIN_SOURCES} ${FP_EDITOR_SOURCES} ${FP_EDITOR_GPU_SOURCES})
target_sources(${PRODUCT_NAME} PRIVATE ${_fp_plugin_own})
fp_warn_sources(${_fp_plugin_own})
target_include_directories(${PRODUCT_NAME} PRIVATE ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR})
# PUBLIC, so the format wrappers compile JUCE with the same settings as the shared code.
target_compile_definitions(${PRODUCT_NAME} PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(${PRODUCT_NAME}
    PRIVATE ${PRODUCT_NAME}_dsp fp_flags juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags)
if(FP_LTO)
  target_link_libraries(${PRODUCT_NAME} PUBLIC juce::juce_recommended_lto_flags)
endif()

# FunkGui, PRIVATE (a PUBLIC link would compile FunkGui again into every format wrapper).
target_link_libraries(${PRODUCT_NAME} PRIVATE FunkGui::core FunkGui::gpu)
if(APPLE)
  target_link_libraries(${PRODUCT_NAME} PRIVATE "-framework ImageIO" "-framework CoreGraphics")   # ChatImages.cpp
  target_link_libraries(${PRODUCT_NAME} PRIVATE compression)   # ProjectExport.cpp: a package's xz (libcompression)
endif()
funkgui_configure_product(${PRODUCT_NAME} PRODUCT ${PRODUCT_NAME} OBJC_PREFIX ${PRODUCT_OBJC_PREFIX}
                          ENV_PREFIX ${PRODUCT_ENV_PREFIX} PREFS_FOLDER ${PRODUCT_NAME})
funkgui_compile_shaders(${PRODUCT_NAME})
funkgui_add_font(${PRODUCT_NAME})                         # the font's and bgfx's licences into each bundle

# Each bundle exports its entry points and nothing else: the AU its factories, the VST3 its three functions, what JUCE
# exports. Everything else stays private, LLVM above all (linked statically, with FMinus): exported, its weak symbols
# would be merged across every plugin in the host's process that exports them too (dyld coalesces weak definitions
# process-wide), and two plugins would share LLVM's state. The test lint.exports checks the built bundles.
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/exports-au.txt CONTENT "_JuceAUFactory\n_${PRODUCT_NAME}AUFactory\n")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/exports-vst3.txt CONTENT "_GetPluginFactory\n_bundleEntry\n_bundleExit\n")
foreach(_fmt AU VST3)
  if(TARGET ${PRODUCT_NAME}_${_fmt})
    string(TOLOWER ${_fmt} _list)
    set(_list ${CMAKE_BINARY_DIR}/exports-${_list}.txt)
    target_link_options(${PRODUCT_NAME}_${_fmt} PRIVATE "LINKER:-exported_symbols_list,${_list}")
    set_property(TARGET ${PRODUCT_NAME}_${_fmt} APPEND PROPERTY LINK_DEPENDS ${_list})
  endif()
endforeach()

# shaderc, the pinned bgfx's, in each bundle's Contents/Resources: the plugin builds a .gui file's shaders with it, on
# the compiler's thread (fminus/Shaders.h; its licence is bgfx's, which funkgui_add_font puts beside it). A resource
# of each bundle, as FunkGui's font is, never a copy after the build: that would break the bundle's seal. A shaderc
# built here (no prebuilt one) is copied before the link instead, once it is built.
foreach(_fmt AU VST3 Standalone)
  if(TARGET ${PRODUCT_NAME}_${_fmt})
    if(FUNKGUI_SHADERC)
      target_sources(${PRODUCT_NAME}_${_fmt} PRIVATE "${FUNKGUI_SHADERC}")
      set_source_files_properties("${FUNKGUI_SHADERC}" TARGET_DIRECTORY ${PRODUCT_NAME}_${_fmt}
                                  PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    else()
      add_dependencies(${PRODUCT_NAME}_${_fmt} shaderc)
      add_custom_command(TARGET ${PRODUCT_NAME}_${_fmt} PRE_LINK
          COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${PRODUCT_NAME}_${_fmt}>/Resources"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:shaderc>"
                  "$<TARGET_BUNDLE_CONTENT_DIR:${PRODUCT_NAME}_${_fmt}>/Resources/shaderc"
          VERBATIM)
    endif()
  endif()
endforeach()

# Each bundle's signature, last: on a bundle's first build JUCE signs the VST3 before its manifest step writes
# Contents/Resources/moduleinfo.json, so the seal no longer holds, and hosts refuse such a bundle (Live's scanner: "a
# sealed resource is missing or invalid"). Signed again, ad hoc, whenever it does not verify; lint.signature checks.
foreach(_fmt AU VST3 Standalone)
  if(TARGET ${PRODUCT_NAME}_${_fmt})
    add_custom_command(TARGET ${PRODUCT_NAME}_${_fmt} POST_BUILD
        COMMAND /bin/sh -c "codesign --verify --strict \"$1\" 2>/dev/null || codesign --force --sign - \"$1\"" sign
                "$<TARGET_BUNDLE_DIR:${PRODUCT_NAME}_${_fmt}>"
        VERBATIM)
  endif()
endforeach()

# codesign refuses a bundle with Finder detritus left by an earlier build; Ninja runs PRE_BUILD as PRE_LINK.
foreach(_fmt AU VST3)
  if(TARGET ${PRODUCT_NAME}_${_fmt})
    add_custom_command(TARGET ${PRODUCT_NAME}_${_fmt} PRE_BUILD
        COMMAND /bin/sh -c "xattr -cr \"$<TARGET_BUNDLE_DIR:${PRODUCT_NAME}_${_fmt}>\" 2>/dev/null || true"
        VERBATIM)
  endif()
endforeach()

# ---- the MCP server (Source/mcp): Claude Desktop starts it; it reaches the plugin's instances (plugin/McpLink.h) ------
# <Name>MCP at the top of the build tree, and <Name>.mcpb beside it: a Claude Desktop extension (manifest.json and the
# server), installed by opening it. juce_core only: the server holds no plugin code.
set(FP_MCP ${PRODUCT_NAME}MCP)
juce_add_console_app(${FP_MCP} PRODUCT_NAME "${FP_MCP}")
target_sources(${FP_MCP} PRIVATE ${FP_SOURCE_ROOT}/mcp/Main.cpp)
fp_warn_sources(${FP_SOURCE_ROOT}/mcp/Main.cpp)
target_include_directories(${FP_MCP} PRIVATE ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR})
target_compile_definitions(${FP_MCP} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(${FP_MCP} PRIVATE fp_flags juce::juce_core juce::juce_recommended_config_flags)
set_target_properties(${FP_MCP} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
string(TOLOWER ${PRODUCT_NAME} FP_MCP_ID)
# The extension's own version: Claude Desktop updates an installed extension only to a newer one, so this goes up with
# every change to the server, its manifest or what the plugin tells it (the tools, the guide's preamble).
set(FP_MCP_VERSION "0.2.0")
if(PRODUCT_KIND STREQUAL "instrument")
  set(FP_MCP_WHAT "synth")
  set(FP_MCP_NAME "Synth")
  set(FP_MCP_ASK "a sound")
else()
  set(FP_MCP_WHAT "effect")
  set(FP_MCP_NAME "Effect")
  set(FP_MCP_ASK "an effect")
endif()
if(PRODUCT_KIND STREQUAL "instrument")
  set(FP_MCP_TRACK "an instrument track")
  set(FP_MCP_EXAMPLES "“Make a warm pad with a slow filter sweep.” “Turn it into a plucky bass.” “Make it brighter, and explain what each dial does.”")
else()
  set(FP_MCP_TRACK "an audio track")
  set(FP_MCP_EXAMPLES "“Make a tape delay with a little wow.” “Make the reverb darker and longer.” “Explain what this effect does to my sound.”")
endif()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/mcpb/manifest.json @ONLY CONTENT [=[{
  "manifest_version": "0.3",
  "name": "@FP_MCP_ID@",
  "display_name": "@PRODUCT_NAME@",
  "version": "@FP_MCP_VERSION@",
  "description": "Ask Claude for @FP_MCP_ASK@ in words: it builds it in @PRODUCT_NAME@, listens to it and sets its dials.",
  "long_description": "Open @PRODUCT_NAME@ in your DAW (on @FP_MCP_TRACK@) or as its own app, then talk to Claude about the sound you want. Claude writes the @FP_MCP_WHAT@'s program, runs it, measures and looks at the result, fixes what is off and leaves the dials at good settings; you hear it right away, and the plugin's window shows what Claude did. Try: @FP_MCP_EXAMPLES@\n\nWith more than one open, say which you mean by its track or project (each also shows its name, such as \"@FP_MCP_NAME@ 2\", while Claude is connected). Claude Desktop's + menu also has ready-made requests: Make a sound, Improve the sound, Explain the sound.\n\nNothing leaves your Mac except your conversation with Claude: the extension talks to the plugin through a private local connection.",
  "author": { "name": "@PRODUCT_COMPANY@" },
  "icon": "icon.png",
  "server": {
    "type": "binary",
    "entry_point": "server/@FP_MCP@",
    "mcp_config": { "command": "${__dirname}/server/@FP_MCP@", "args": [], "env": {} }
  },
  "tools": [
    { "name": "list_instances", "description": "Find the open @FP_MCP_WHAT@s" },
    { "name": "playground_guide", "description": "Read the guide to the @FP_MCP_WHAT@ and its language" }
  ],
  "tools_generated": true,
  "prompts_generated": true,
  "compatibility": { "platforms": ["darwin"] }
}
]=])
add_custom_command(TARGET ${FP_MCP} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/mcpb/server
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${FP_MCP}> ${CMAKE_BINARY_DIR}/mcpb/server/${FP_MCP}
    COMMAND ${CMAKE_COMMAND} -E copy ${FP_SOURCE_ROOT}/mcp/icon.png ${CMAKE_BINARY_DIR}/mcpb/icon.png
    COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR}/mcpb ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/${PRODUCT_NAME}.mcpb
            --format=zip manifest.json icon.png server/${FP_MCP}
    VERBATIM)

# ---- the probes (cmake/Probes.cmake) ---------------------------------------------------------------------------------
enable_testing()
include(Probes)

message(STATUS "${PRODUCT_NAME} ${PROJECT_VERSION} (${PRODUCT_KIND}, ${PRODUCT_MFR_CODE}/${PRODUCT_CODE}): "
               "${CMAKE_BUILD_TYPE}, LTO ${FP_LTO}, Werror ${FP_WERROR}")
