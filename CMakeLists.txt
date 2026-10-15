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

# ---- sources ---------------------------------------------------------------------------------------------------------
set(FP_SOURCE_ROOT ${PROJECT_SOURCE_DIR}/Source)
file(GLOB_RECURSE FP_DSP_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/fsdsp/*.cpp ${FP_SOURCE_ROOT}/dsp/*.cpp)
file(GLOB FP_PLUGIN_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/plugin/*.cpp)
list(FILTER FP_PLUGIN_SOURCES EXCLUDE REGEX "/CreateEditor[^/]*\\.cpp$")
set(FP_CREATE_EDITOR_GENERIC ${FP_SOURCE_ROOT}/plugin/CreateEditorGeneric.cpp)        # the probes' editor factory
file(GLOB_RECURSE FP_EDITOR_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/*.cpp)
list(FILTER FP_EDITOR_SOURCES EXCLUDE REGEX "/Source/editor/gpu/")
file(GLOB FP_EDITOR_GPU_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/gpu/*.cpp ${FP_SOURCE_ROOT}/editor/gpu/*.mm)

# ---- the DSP library: Source/fsdsp + Source/dsp, JUCE-free (no JUCE include path reaches it) -------------------------
add_library(${PRODUCT_NAME}_dsp STATIC ${FP_DSP_SOURCES})
set_target_properties(${PRODUCT_NAME}_dsp PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(${PRODUCT_NAME}_dsp PUBLIC ${FP_SOURCE_ROOT})
target_link_libraries(${PRODUCT_NAME}_dsp PUBLIC fp_flags PRIVATE fp_warnings fp_lto)

# ---- the plugin ------------------------------------------------------------------------------------------------------
if(PRODUCT_IS_INSTRUMENT)
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
funkgui_configure_product(${PRODUCT_NAME} PRODUCT ${PRODUCT_NAME} OBJC_PREFIX ${PRODUCT_OBJC_PREFIX}
                          ENV_PREFIX ${PRODUCT_ENV_PREFIX} PREFS_FOLDER ${PRODUCT_NAME})
funkgui_compile_shaders(${PRODUCT_NAME})
funkgui_add_font(${PRODUCT_NAME})                         # the font's and bgfx's licences into each bundle

# codesign refuses a bundle with Finder detritus left by an earlier build; Ninja runs PRE_BUILD as PRE_LINK.
foreach(_fmt AU VST3)
  if(TARGET ${PRODUCT_NAME}_${_fmt})
    add_custom_command(TARGET ${PRODUCT_NAME}_${_fmt} PRE_BUILD
        COMMAND /bin/sh -c "xattr -cr \"$<TARGET_BUNDLE_DIR:${PRODUCT_NAME}_${_fmt}>\" 2>/dev/null || true"
        VERBATIM)
  endif()
endforeach()

# ---- the probes (cmake/Probes.cmake) ---------------------------------------------------------------------------------
enable_testing()
include(Probes)

message(STATUS "${PRODUCT_NAME} ${PROJECT_VERSION} (${PRODUCT_KIND}, ${PRODUCT_MFR_CODE}/${PRODUCT_CODE}): "
               "${CMAKE_BUILD_TYPE}, LTO ${FP_LTO}, Werror ${FP_WERROR}")
