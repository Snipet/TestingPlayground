# Two plugins (AU, VST3, Standalone; macOS arm64) from one source tree, on JUCE 8.0.4 and FunkGui, with FSynth's DSP
# core vendored in Source/fsdsp/core. The products are products/<key>.cmake: the effect (products/effect.cmake) and
# the synth (products/synth.cmake). One configure builds every product: the shared sources are compiled once for
# each, against that product's generated Product.h. Use the presets (CMakePresets.json):
#
#   cmake --workflow --preset dev-verify     configure, build (both plugins + probes) and run every test, in build-dev
#   scripts/test.sh [effect|synth]           build and run the probes only (both products, or the one named)
#
# Layout (README.md has the whole story):
#   products/<key>.cmake         a product's identity: name, kind (effect | instrument), codes, bundle id, prefixes
#   Source/fsdsp/core/**         the vendored DSP core (SEED.tsv; scripts/sync-dsp-core.sh), JUCE-free
#   Source/dsp/**                the algorithms, JUCE-free                         -> <Name>_dsp (static library)
#   Source/fminus/*.cpp          a program, compiled for the plugin (Patch.h)      -> <Name>_fminus, which _dsp links
#   Source/fminus/examples/<key> a product's EXAMPLES, the first the default      -> generated/<key>/Examples.h
#   Source/fminus/library/       the LIBRARY, every product's                      -> generated/<key>/Library.h
#   Source/plugin/*.cpp          the JUCE AudioProcessor                           -> the plugin and the probes
#   Source/editor/**/*.cpp       the FunkGui Panel, minus gpu/                     -> the plugin and the probes
#   Source/editor/gpu/*.cpp      the EditorHost factory (GPU)                      -> the plugin only
#   Source/mcp/                  the MCP server; icon-<key>.png its extension's    -> <Name>MCP and <Name>.mcpb
#   Assets/logo/<kind>/          the app icon (Tools/logo/logo.swift writes it)   -> each bundle's Icon.icns
#   Tools/probes/**/*.cpp        probes on FunkGui's Harness, self-registered      -> <Name>_probes, tests <key>:<probe>
# Every list is a CONFIGURE_DEPENDS glob: a new source file needs no CMake edit. Nothing outside products/ (and the
# docs) names a product: the C++ namespace is `playground` in both, and every product-specific value comes from the
# product's file, through Product.h or the variables below.
cmake_minimum_required(VERSION 3.30)                     # CMP0168: FetchContent populates without a sub-build

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS")

project(Playground LANGUAGES C CXX)

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

# The products: every products/<key>.cmake, in key order, unless FP_PRODUCTS names some (-DFP_PRODUCTS=synth configures
# the synth alone). Empty means every one, so a new products/<key>.cmake is picked up; the presets set it empty.
file(GLOB _fp_product_files CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/products/*.cmake)
set(_fp_all_products "")
foreach(_f IN LISTS _fp_product_files)
  get_filename_component(_k ${_f} NAME_WE)
  list(APPEND _fp_all_products ${_k})
endforeach()
list(SORT _fp_all_products)
set(FP_PRODUCTS "" CACHE STRING "The products to build: keys of products/<key>.cmake; empty: every one")
if(FP_PRODUCTS)
  set(_fp_products ${FP_PRODUCTS})
else()
  set(_fp_products ${_fp_all_products})
endif()
if(NOT _fp_products)
  message(FATAL_ERROR "no product: products/ has no <key>.cmake")
endif()
set(FP_BUILT_PRODUCTS "${_fp_products}" CACHE INTERNAL "The configured products (scripts/install.sh reads it)")
foreach(_k IN LISTS _fp_products)
  if(NOT _k MATCHES "^[a-z][a-z0-9]*$")
    message(FATAL_ERROR "products/${_k}.cmake: a product's key (its file's name) is lower-case letters and digits")
  endif()
  if(NOT _k IN_LIST _fp_all_products)
    message(FATAL_ERROR "FP_PRODUCTS names '${_k}', but there is no products/${_k}.cmake")
  endif()
endforeach()

# The C++ namespace of Source/dsp, fminus, plugin, editor and mcp, every product's (the sources spell it out; the
# generated headers take it from here). Neutral, so that a product's name can change without touching the code.
set(FP_NAMESPACE playground)
file(STRINGS "${PROJECT_SOURCE_DIR}/Source/plugin/Processor.h" _ns REGEX "^namespace ${FP_NAMESPACE}$")
if(NOT _ns)
  message(FATAL_ERROR "CMakeLists.txt: FP_NAMESPACE is '${FP_NAMESPACE}', but Source/plugin/Processor.h has no "
                      "'namespace ${FP_NAMESPACE}' line")
endif()

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(Flags)
include(Deps)

# ---- the shared inputs -----------------------------------------------------------------------------------------------
set(FP_SOURCE_ROOT ${PROJECT_SOURCE_DIR}/Source)
file(GLOB_RECURSE FP_DSP_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/fsdsp/*.cpp ${FP_SOURCE_ROOT}/dsp/*.cpp)
file(GLOB FP_PLUGIN_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/plugin/*.cpp)
list(FILTER FP_PLUGIN_SOURCES EXCLUDE REGEX "/CreateEditor[^/]*\\.cpp$")
set(FP_CREATE_EDITOR_GENERIC ${FP_SOURCE_ROOT}/plugin/CreateEditorGeneric.cpp)        # the probes' editor factory
file(GLOB_RECURSE FP_EDITOR_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/*.cpp)
list(FILTER FP_EDITOR_SOURCES EXCLUDE REGEX "/Source/editor/gpu/")
file(GLOB FP_EDITOR_GPU_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/gpu/*.cpp ${FP_SOURCE_ROOT}/editor/gpu/*.mm)
file(GLOB FP_FMINUS_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/fminus/*.cpp)
# The Standalone app (Source/plugin/standalone/, its Standalone target only): our sources' warnings, and ARC for its
# Objective-C++.
file(GLOB FP_STANDALONE_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/plugin/standalone/*.cpp
                                                  ${FP_SOURCE_ROOT}/plugin/standalone/*.mm)
fp_warn_sources(${FP_STANDALONE_SOURCES})
foreach(_f IN LISTS FP_STANDALONE_SOURCES)
  if(_f MATCHES "\\.mm$")
    set_property(SOURCE ${_f} DIRECTORY ${PROJECT_SOURCE_DIR} APPEND PROPERTY COMPILE_OPTIONS -fobjc-arc)
  endif()
endforeach()

# The editor's LIBRARY (Source/fminus/library/<n>-<category>/*.fm, in folder and then file-name order), compiled into
# every product: building blocks a program takes as tabs of its own and #includes (the LIBRARY menu, Claude's
# add_library_file). Each file's first line is "// <its file name>: what it is"; a folder's name, its number and
# dashes dropped, is its category. Read once here; each product's Library.h is written from these rows.
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

# The extension's own version (every product's .mcpb): Claude Desktop updates an installed extension only to a newer
# one, so this goes up with every change to the server, its manifest or what the plugin tells it (the tools, the
# guide's preamble).
set(FP_MCP_VERSION "0.3.1")

# The probes (cmake/Probes.cmake): `probes` builds every product's.
enable_testing()
include(Probes)
add_custom_target(probes)

# ---- one product -----------------------------------------------------------------------------------------------------
# fp_add_product(<key>): products/<key>.cmake's identity, checked, and everything built from it: the generated headers
# in <build>/generated/<key>/, the static libraries <Name>_fminus and <Name>_dsp, the plugin <Name> (AU, VST3,
# Standalone), the MCP server <Name>MCP and its extension <Name>.mcpb, the probes <Name>_probes and their tests, and
# the target <key>, which builds all of them. The PRODUCT_* values live in this function's scope only.
function(fp_add_product key)
  set(_def products/${key}.cmake)
  foreach(_k NAME KIND VERSION COMPANY MFR_CODE CODE BUNDLE_ID ENV_PREFIX OBJC_PREFIX)
    unset(PRODUCT_${_k})
  endforeach()
  include(${PROJECT_SOURCE_DIR}/${_def})
  foreach(_k NAME KIND VERSION COMPANY MFR_CODE CODE BUNDLE_ID ENV_PREFIX OBJC_PREFIX)
    if("${PRODUCT_${_k}}" STREQUAL "")
      message(FATAL_ERROR "${_def}: no PRODUCT_${_k}")
    endif()
  endforeach()
  if(NOT PRODUCT_NAME MATCHES "^[A-Z][A-Za-z0-9]*$")
    message(FATAL_ERROR "${_def}: PRODUCT_NAME '${PRODUCT_NAME}' must be PascalCase letters and digits")
  endif()
  if(PRODUCT_KIND STREQUAL "instrument")
    set(PRODUCT_IS_INSTRUMENT true)
  elseif(PRODUCT_KIND STREQUAL "effect")
    set(PRODUCT_IS_INSTRUMENT false)
  else()
    message(FATAL_ERROR "${_def}: PRODUCT_KIND '${PRODUCT_KIND}' must be effect or instrument")
  endif()
  if(NOT PRODUCT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "${_def}: PRODUCT_VERSION '${PRODUCT_VERSION}' must be <major>.<minor>.<patch>")
  endif()
  foreach(_code PRODUCT_MFR_CODE PRODUCT_CODE)
    if(NOT ${_code} MATCHES "^[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]$" OR NOT ${_code} MATCHES "[A-Z]")
      message(FATAL_ERROR "${_def}: ${_code} '${${_code}}' must be 4 letters or digits, one upper-case")
    endif()
  endforeach()
  # What a host or a user tells the products apart by differs between them: a host loads both into one process (the
  # Objective-C classes are process-wide), finds a plugin by its codes, and each keeps its files in its own folders.
  foreach(_k NAME CODE BUNDLE_ID ENV_PREFIX OBJC_PREFIX)
    get_property(_taken GLOBAL PROPERTY FP_PRODUCT_${_k}S)
    if("${PRODUCT_${_k}}" IN_LIST _taken)
      message(FATAL_ERROR "${_def}: PRODUCT_${_k} '${PRODUCT_${_k}}' is another product's too: each needs its own")
    endif()
    set_property(GLOBAL APPEND PROPERTY FP_PRODUCT_${_k}S "${PRODUCT_${_k}}")
  endforeach()

  # ---- the generated headers: <build>/generated/<key>/ -----------------------------------------------------------
  set(FP_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated/${key})
  configure_file(${PROJECT_SOURCE_DIR}/cmake/Product.h.in ${FP_GENERATED_DIR}/Product.h @ONLY)

  # The editor's EXAMPLES (Source/fminus/examples/<key>/*.fm, in file-name order), compiled into the plugin: the first
  # is the program a new instance plays. Each file's first line names it, "// <Title>: what it is". Adding or editing
  # one reconfigures, so the copies follow.
  file(GLOB _examples CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/Source/fminus/examples/${key}/*.fm)
  list(SORT _examples)
  if(NOT _examples)
    message(FATAL_ERROR "Source/fminus/examples/${key}/ has no .fm file: the product needs a default program")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_examples})
  set(FP_FMINUS_EXAMPLE_ROWS "")
  foreach(_f IN LISTS _examples)
    file(READ ${_f} _text)
    if(_text MATCHES "\\)fminus\"")
      message(FATAL_ERROR "${_f}: the text )fminus\" would end the raw string it is compiled into")
    endif()
    if(NOT _text MATCHES "^// ([^:\n]+):")
      message(FATAL_ERROR "${_f}: the first line must be '// <Title>: ...', the title the EXAMPLES menu shows")
    endif()
    string(APPEND FP_FMINUS_EXAMPLE_ROWS "        Example{ \"${CMAKE_MATCH_1}\", R\"fminus(${_text})fminus\" },\n")
  endforeach()
  file(CONFIGURE OUTPUT ${FP_GENERATED_DIR}/Examples.h @ONLY CONTENT [=[// Generated by CMakeLists.txt from Source/fminus/examples/@key@/*.fm: the editor's EXAMPLES, as they were when the
// plugin was built. The first is the program a new instance plays.
#pragma once

namespace @FP_NAMESPACE@
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

  file(CONFIGURE OUTPUT ${FP_GENERATED_DIR}/Library.h @ONLY CONTENT [=[// Generated by CMakeLists.txt from Source/fminus/library/*/*.fm: the editor's LIBRARY, as it was when the plugin
// was built.
#pragma once

#include <string_view>

namespace @FP_NAMESPACE@
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

  # ---- FMinus: the JIT compiler, through its public API (fminus/FMinus.h), and the program (Patch.h), JUCE-free -----
  # FMinus::FMinus is FMinus's public API: fminus/FMinus.h needs C++17 and has no LLVM in it, so every file of the
  # plugin may include it with the plugin's own C++20 and flags. LLVM is linked into the plugin, but its headers never
  # reach it.
  add_library(${PRODUCT_NAME}_fminus STATIC ${FP_FMINUS_SOURCES})
  set_target_properties(${PRODUCT_NAME}_fminus PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  target_include_directories(${PRODUCT_NAME}_fminus PUBLIC ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR})
  target_link_libraries(${PRODUCT_NAME}_fminus PUBLIC fp_flags FMinus::FMinus PRIVATE fp_warnings fp_lto)

  # ---- the DSP library: Source/fsdsp + Source/dsp, JUCE-free (no JUCE include path reaches it) ---------------------
  add_library(${PRODUCT_NAME}_dsp STATIC ${FP_DSP_SOURCES})
  set_target_properties(${PRODUCT_NAME}_dsp PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  target_include_directories(${PRODUCT_NAME}_dsp PUBLIC ${FP_SOURCE_ROOT})
  target_link_libraries(${PRODUCT_NAME}_dsp PUBLIC fp_flags ${PRODUCT_NAME}_fminus PRIVATE fp_warnings fp_lto)

  # ---- the plugin ----------------------------------------------------------------------------------------------------
  if(PRODUCT_KIND STREQUAL "instrument")
    set(_fp_plugin_kind IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE VST3_CATEGORIES Instrument Synth
                        AU_MAIN_TYPE kAudioUnitType_MusicDevice MICROPHONE_PERMISSION_ENABLED FALSE)
  else()
    set(_fp_plugin_kind IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE VST3_CATEGORIES Fx
                        AU_MAIN_TYPE kAudioUnitType_Effect MICROPHONE_PERMISSION_ENABLED TRUE
                        MICROPHONE_PERMISSION_TEXT "${PRODUCT_NAME} processes the audio input.")
  endif()
  # The app icon, the kind's (Assets/logo/<kind>/, written by Tools/logo/logo.swift): JUCE makes each bundle's
  # Resources/Icon.icns (the Info.plist's CFBundleIconFile) from ICON_BIG at configure time, with only the sizes it is
  # given; ours replaces it below, every size drawn for itself (16 and 32 px set on their own grids).
  set(_fp_logo ${PROJECT_SOURCE_DIR}/Assets/logo/${PRODUCT_KIND})
  if(NOT EXISTS ${_fp_logo}/icon.icns OR NOT EXISTS ${_fp_logo}/installer-1024.png)
    message(FATAL_ERROR "Assets/logo/${PRODUCT_KIND}/ has no icon.icns: run `swift Tools/logo/logo.swift`")
  endif()
  juce_add_plugin(${PRODUCT_NAME}
      ICON_BIG                 ${_fp_logo}/installer-1024.png
      COMPANY_NAME             "${PRODUCT_COMPANY}"
      PLUGIN_MANUFACTURER_CODE ${PRODUCT_MFR_CODE}
      PLUGIN_CODE              ${PRODUCT_CODE}
      FORMATS                  AU VST3 Standalone
      PRODUCT_NAME             "${PRODUCT_NAME}"
      VERSION                  ${PRODUCT_VERSION}
      BUNDLE_ID                ${PRODUCT_BUNDLE_ID}
      NEEDS_MIDI_OUTPUT        FALSE
      IS_MIDI_EFFECT           FALSE
      ${_fp_plugin_kind}
      COPY_PLUGIN_AFTER_BUILD  FALSE)                    # scripts/install.sh copies them, deliberately
  get_target_property(_fp_icns ${PRODUCT_NAME} JUCE_ICON_FILE)
  configure_file(${_fp_logo}/icon.icns ${_fp_icns} COPYONLY)

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
    target_link_libraries(${PRODUCT_NAME} PRIVATE compression)  # ProjectExport.cpp: a package's xz (libcompression)
  endif()
  funkgui_configure_product(${PRODUCT_NAME} PRODUCT ${PRODUCT_NAME} OBJC_PREFIX ${PRODUCT_OBJC_PREFIX}
                            ENV_PREFIX ${PRODUCT_ENV_PREFIX} PREFS_FOLDER ${PRODUCT_NAME})
  funkgui_compile_shaders(${PRODUCT_NAME})
  funkgui_add_font(${PRODUCT_NAME})                       # the font's and bgfx's licences into each bundle

  # The Standalone is our own app (Source/plugin/standalone/): JUCE's custom standalone hook leaves JUCE's out (its
  # drawn title bar and Options button) for macOS's window, the editor's top bar under its buttons, and a menu bar. The
  # AU and the VST3 are unchanged.
  if(TARGET ${PRODUCT_NAME}_Standalone)
    target_sources(${PRODUCT_NAME}_Standalone PRIVATE ${FP_STANDALONE_SOURCES})
    target_compile_definitions(${PRODUCT_NAME}_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
    target_link_libraries(${PRODUCT_NAME}_Standalone PRIVATE fp_flags)
  endif()

  # Each bundle exports its entry points and nothing else: the AU its factories, the VST3 its three functions, what
  # JUCE exports. Everything else stays private, LLVM above all (linked statically, with FMinus): exported, its weak
  # symbols would be merged across every plugin in the host's process that exports them too (dyld coalesces weak
  # definitions process-wide), and two plugins would share LLVM's state; so would the two products, whose C++ is one
  # namespace. The test lint.exports checks the built bundles.
  set(_exports ${CMAKE_BINARY_DIR}/exports/${key})
  file(CONFIGURE OUTPUT ${_exports}-au.txt CONTENT "_JuceAUFactory\n_${PRODUCT_NAME}AUFactory\n")
  file(CONFIGURE OUTPUT ${_exports}-vst3.txt CONTENT "_GetPluginFactory\n_bundleEntry\n_bundleExit\n")
  foreach(_fmt AU VST3)
    if(TARGET ${PRODUCT_NAME}_${_fmt})
      string(TOLOWER ${_fmt} _list)
      set(_list ${_exports}-${_list}.txt)
      target_link_options(${PRODUCT_NAME}_${_fmt} PRIVATE "LINKER:-exported_symbols_list,${_list}")
      set_property(TARGET ${PRODUCT_NAME}_${_fmt} APPEND PROPERTY LINK_DEPENDS ${_list})
      set_property(GLOBAL APPEND PROPERTY FP_EXPORT_BUNDLES "$<TARGET_FILE:${PRODUCT_NAME}_${_fmt}>=${_list}")
    endif()
  endforeach()

  # shaderc, the pinned bgfx's, in each bundle's Contents/Resources: the plugin builds a .gui file's shaders with it,
  # on the compiler's thread (fminus/Shaders.h; its licence is bgfx's, which funkgui_add_font puts beside it). A
  # resource of each bundle, as FunkGui's font is, never a copy after the build: that would break the bundle's seal. A
  # shaderc built here (no prebuilt one) is copied before the link instead, once it is built.
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
      set_property(GLOBAL APPEND PROPERTY FP_SIGNED_BUNDLES "$<TARGET_BUNDLE_DIR:${PRODUCT_NAME}_${_fmt}>")
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

  # ---- the MCP server (Source/mcp): Claude Desktop starts it; it reaches the plugin's instances (plugin/McpLink.h) --
  # <Name>MCP at the top of the build tree, and <Name>.mcpb beside it: a Claude Desktop extension (manifest.json, the
  # product's icon and the server), installed by opening it. juce_core only: the server holds no plugin code.
  set(FP_MCP ${PRODUCT_NAME}MCP)
  juce_add_console_app(${FP_MCP} PRODUCT_NAME "${FP_MCP}" VERSION ${PRODUCT_VERSION})
  target_sources(${FP_MCP} PRIVATE ${FP_SOURCE_ROOT}/mcp/Main.cpp)
  fp_warn_sources(${FP_SOURCE_ROOT}/mcp/Main.cpp)
  target_include_directories(${FP_MCP} PRIVATE ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR})
  target_compile_definitions(${FP_MCP} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(${FP_MCP} PRIVATE fp_flags juce::juce_core juce::juce_recommended_config_flags)
  set_target_properties(${FP_MCP} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  string(TOLOWER ${PRODUCT_NAME} FP_MCP_ID)
  set(_icon ${FP_SOURCE_ROOT}/mcp/icon-${key}.png)
  if(NOT EXISTS ${_icon})
    message(FATAL_ERROR "Source/mcp/icon-${key}.png is missing: the ${key} product's extension needs its icon")
  endif()
  if(PRODUCT_KIND STREQUAL "instrument")
    set(FP_MCP_WHAT "synth")
    set(FP_MCP_NAME "Synth")
    set(FP_MCP_ASK "a sound")
    set(FP_MCP_TRACK "an instrument track")
    set(FP_MCP_EXAMPLES "“Make a warm pad with a slow filter sweep.” “Turn it into a plucky bass.” “Make it brighter, and explain what each dial does.”")
  else()
    set(FP_MCP_WHAT "effect")
    set(FP_MCP_NAME "Effect")
    set(FP_MCP_ASK "an effect")
    set(FP_MCP_TRACK "an audio track")
    set(FP_MCP_EXAMPLES "“Make a tape delay with a little wow.” “Make the reverb darker and longer.” “Explain what this effect does to my sound.”")
  endif()
  set(_mcpb ${CMAKE_BINARY_DIR}/mcpb/${key})
  file(CONFIGURE OUTPUT ${_mcpb}/manifest.json @ONLY CONTENT [=[{
  "manifest_version": "0.3",
  "name": "@FP_MCP_ID@",
  "display_name": "@PRODUCT_NAME@",
  "version": "@FP_MCP_VERSION@",
  "description": "Ask Claude for @FP_MCP_ASK@ in words: it builds it in @PRODUCT_NAME@, listens to it and sets its dials.",
  "long_description": "Open @PRODUCT_NAME@ in your DAW (on @FP_MCP_TRACK@) or as its own app, then talk to Claude about the sound you want. Claude writes the @FP_MCP_WHAT@'s program, runs it, measures and looks at the result, fixes what is off and leaves the dials at good settings; you hear it right away, and the plugin's window shows what Claude did. Try: @FP_MCP_EXAMPLES@\n\nWith more than one open, say which you mean by its track or project (each also shows its name, such as \"@FP_MCP_NAME@ 2\", while Claude is connected). Claude Desktop's + menu also has ready-made requests: Make a sound, Improve the sound, Explain the sound, Fix what I hear.\n\nNothing leaves your Mac except your conversation with Claude: the extension talks to the plugin through a private local connection.",
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
  # The extension, packed again whenever the server, its manifest or its icon changes.
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${PRODUCT_NAME}.mcpb
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_mcpb}/server
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${FP_MCP}> ${_mcpb}/server/${FP_MCP}
      COMMAND ${CMAKE_COMMAND} -E copy ${_icon} ${_mcpb}/icon.png
      COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/${PRODUCT_NAME}.mcpb
      COMMAND ${CMAKE_COMMAND} -E chdir ${_mcpb} ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/${PRODUCT_NAME}.mcpb
              --format=zip manifest.json icon.png server/${FP_MCP}
      DEPENDS ${FP_MCP} ${_mcpb}/manifest.json ${_icon}
      VERBATIM)
  add_custom_target(${FP_MCP}_mcpb ALL DEPENDS ${CMAKE_BINARY_DIR}/${PRODUCT_NAME}.mcpb)

  # ---- the probes and their tests (cmake/Probes.cmake) ---------------------------------------------------------------
  fp_add_probes(${key})

  # `<key>`: this product alone (its plugin's formats, its extension and its probes); the presets dev-<key> build it.
  add_custom_target(${key})
  add_dependencies(${key} ${PRODUCT_NAME}_All ${FP_MCP}_mcpb ${PRODUCT_NAME}_probes)

  message(STATUS "${key}: ${PRODUCT_NAME} ${PRODUCT_VERSION} (${PRODUCT_KIND}, ${PRODUCT_MFR_CODE}/${PRODUCT_CODE}, "
                 "${PRODUCT_BUNDLE_ID})")
endfunction()

foreach(_k IN LISTS _fp_products)
  fp_add_product(${_k})
endforeach()

# ---- the lints, once for every product's bundles (cmake/Probes.cmake) -----------------------------------------------
fp_add_lints()

message(STATUS "${PROJECT_NAME}: ${_fp_products}; ${CMAKE_BUILD_TYPE}, LTO ${FP_LTO}, Werror ${FP_WERROR}")
