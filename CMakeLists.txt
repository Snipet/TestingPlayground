# Two plugins (AU, VST3, Standalone on macOS arm64; VST3 and Standalone on Windows x64 and Linux x86-64) from one
# source tree, on JUCE 8.0.4 and FunkGui, with FSynth's DSP core vendored in Source/fsdsp/core. The products are
# products/<key>.cmake: the effect (products/effect.cmake) and the synth (products/synth.cmake). One configure builds
# every product: the shared sources are compiled once for each, against that product's generated Product.h. Use the
# presets (CMakePresets.json):
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
#   Source/fminus/library/       the LIBRARY, every product's (.fm, .cir)          -> generated/<key>/Library.h
#   factory/<key>/<name>/        a product's factory projects (read-only)          -> generated/<key>/FactoryProjects.h
#   Source/plugin/*.cpp          the JUCE AudioProcessor                           -> the plugin and the probes
#   Source/editor/**/*.cpp       the FunkGui Panel, minus gpu/                     -> the plugin and the probes
#   Source/editor/gpu/*.cpp      the EditorHost factory (GPU)                      -> the plugin only
#   Source/mcp/                  the MCP server; icon-<key>.png its extension's    -> <Name>MCP and <Name>.mcpb
#   Source/sandbox/              the GUI sandbox's helper, with the editor's .gui  -> <Name>GuiSandbox (in the .mcpb)
#                                code and no plugin code (FP_SANDBOX_SHARED)
#   Source/render/               the headless renderer: the DSP library and the    -> <Name>Render (in the .mcpb)
#                                tools' request reading (FP_RENDER_SHARED)
#   Assets/logo/<kind>/          the app icon (Tools/logo/logo.swift writes it)   -> each bundle's Icon.icns
#   Source/installer/            the installer, one for every product (its payload: their bundles and extensions)
#                                                                                  -> "<Project> Installer.app" (.exe)
#   Tools/probes/**/*.cpp        probes on FunkGui's Harness, self-registered      -> <Name>_probes, tests <key>:<probe>
#   Tools/probes/installer/      the installer's probes                            -> Installer_probes, tests installer.*
#
# Windows (x64, clang-cl; the windows-* presets): the VST3, the Standalone, the MCP server and its extension, the GUI
# sandbox's helper, the installer (its payload\ beside its exe) and the probes. The editor draws on the GPU there too
# (FunkGui v0.24.0: bgfx on Direct3D 11), or, with FP_WITH_GPU off, on the CPU (Source/editor/nogpu/: FunkGui's
# SoftRaster in a JUCE component); no AU, no signing. Platform files are named for it: *Win.cpp and *None.cpp beside
# the macOS *.mm and IOKit sources (Source/plugin/standalone/, Source/installer/: EngineWin.cpp, SystemWin.cpp).
#
# Linux (x86-64, clang and lld; the linux-* presets): the VST3, the Standalone, the MCP server and its extension, the
# GUI sandbox's helper, the headless renderer and the probes; no AU, no installer app. The editor draws on the GPU
# (FunkGui: bgfx on Vulkan, in an X11 child window of JUCE's peer, XWayland on a Wayland desktop), or with FP_WITH_GPU
# off on the CPU as on Windows. The Standalone takes Windows' *Win.cpp window and its own *Linux.cpp racing wheel.
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

# The platforms: macOS (arm64, Apple clang), Windows (x64, clang-cl: FMinus's compiler there) and Linux (x86-64, clang).
set(FP_WINDOWS OFF)
set(FP_LINUX OFF)
if(APPLE)
  if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "${PROJECT_NAME} builds for arm64 only on macOS: configure with a preset (CMakePresets.json), "
                        "which sets CMAKE_OSX_ARCHITECTURES=arm64.")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "${PROJECT_NAME} needs Apple clang (Xcode or its command-line tools)")
  endif()
  set(FP_ARCH arm64)
elseif(WIN32)
  set(FP_WINDOWS ON)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "${PROJECT_NAME} builds on Windows with clang-cl (FMinus's compiler there): configure with a "
                        "windows-* preset from an \"x64 Native Tools Command Prompt for VS\" (CMakePresets.json sets "
                        "CMAKE_C_COMPILER and CMAKE_CXX_COMPILER to clang-cl), with Visual Studio's \"C++ Clang tools "
                        "for Windows\" installed. This is ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}).")
  endif()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM|arm|aarch)")
    message(FATAL_ERROR "${PROJECT_NAME} builds for x64 on Windows: open \"x64 Native Tools Command Prompt for VS\"")
  endif()
  set(FP_ARCH x86_64)
  # The C runtime of FMinus's LLVM (FMinus's own default there): every target links the same one.
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
  # Large translation units (JUCE's modules, Panel.cpp) and UTF-8 sources.
  add_compile_options($<$<COMPILE_LANGUAGE:CXX,C>:/bigobj> $<$<COMPILE_LANGUAGE:CXX,C>:/utf-8>)
  # windows.h without min/max macros and its rarely used parts, as JUCE's own headers define them (the same values, so
  # JUCE's definitions are no redefinitions).
  add_compile_definitions(NOMINMAX= WIN32_LEAN_AND_MEAN=1 _CRT_SECURE_NO_WARNINGS)
  # RelWithDebInfo inlines fully, as clang's -O2 does on macOS: CMake's default for an MSVC-like compiler, /Ob1, inlines
  # only what is marked inline (the Engine, SoftRaster and the probes' analyses ran several times slower). For every
  # target below, FMinus's and FunkGui's included.
  foreach(_lang C CXX)
    string(REPLACE "/Ob1" "/Ob2" CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}")
  endforeach()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(FP_LINUX ON)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "${PROJECT_NAME} builds on Linux with clang (FMinus's compiler): configure with a linux-* "
                        "preset (CMakePresets.json sets CMAKE_C_COMPILER and CMAKE_CXX_COMPILER to clang and clang++, "
                        "and links with lld). This is ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}).")
  endif()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "${PROJECT_NAME} builds for x86-64 on Linux (this is ${CMAKE_SYSTEM_PROCESSOR})")
  endif()
  set(FP_ARCH x86_64)
  # Every static library here ends up in a shared object (the VST3's .so): position-independent code throughout,
  # FMinus's, FunkGui's and bgfx's included.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
  message(FATAL_ERROR "${PROJECT_NAME} builds on macOS (arm64), Windows (x64) and Linux (x86-64).")
endif()
# FunkGui's GPU layer (bgfx: Metal on macOS, Direct3D 11 on Windows, Vulkan on X11 on Linux) and the editor host on it;
# OFF: the CPU host (Source/editor/nogpu/: SoftRaster, custom shaders as placeholders), which needs neither bgfx nor
# shaderc.
option(FP_WITH_GPU "FunkGui's GPU layer (bgfx) and FunkGui's EditorHost; OFF: the editor on the CPU" ON)
# The user's home: the machine caches (~/audio/...) are found under it.
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{HOME}" FP_HOME)
else()
  file(TO_CMAKE_PATH "$ENV{USERPROFILE}" FP_HOME)
endif()

# ---- options ---------------------------------------------------------------------------------------------------------
# Windows starts with -Werror and LTO off: its headers and its linker (lld-link for LTO) are newer ground.
if(FP_WINDOWS)
  set(_fp_strict OFF)
else()
  set(_fp_strict ON)
endif()
option(FP_WERROR "-Werror on our own sources (never on JUCE's, FunkGui's or bgfx's)" ${_fp_strict})
option(FP_LTO    "-flto in Release builds"                                         ${_fp_strict})
set(FP_DEPS_DIR "${FP_HOME}/audio/.deps" CACHE PATH "Read-only machine cache of JUCE, bgfx.cmake and shaderc")
# fp_local_checkout(<var> <name>): this machine's git checkout of a library of ours, ~/audio/libraries/<name> or else
# ~/audio/<name> (where a Windows machine may keep them), or "" when there is none.
function(fp_local_checkout var name)
  set(${var} "" PARENT_SCOPE)
  foreach(_d "${FP_HOME}/audio/libraries/${name}" "${FP_HOME}/audio/${name}")
    if(EXISTS "${_d}/.git")
      set(${var} "${_d}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()
fp_local_checkout(_fp_funkgui_local FunkGui)
if(_fp_funkgui_local)
  set(_fp_funkgui_repo "${_fp_funkgui_local}")
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
# Windows' system libraries the plugin, the server and the helper call: Winsock (the MCP sockets), CNG (the chat's key
# file), the shell's folders.
set(FP_WINDOWS_LIBS ws2_32 bcrypt shell32 ole32)
# Linux's: OpenSSL's libcrypto (the chat's key file: AES, PBKDF2 and HMAC, as CommonCrypto and CNG give them) and
# libcurl (the chat's HTTPS: ClaudeApi.cpp's transport; JUCE's own Linux client speaks plain HTTP alone, so JUCE_USE_CURL
# stays 0 everywhere and no JUCE target links curl).
set(FP_LINUX_LIBS "")
if(FP_LINUX)
  find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
  find_package(CURL REQUIRED)
  set(FP_LINUX_LIBS OpenSSL::Crypto CURL::libcurl)
endif()
if(FP_WITH_GPU)
  set(_fp_editor_on "GPU")
else()
  set(_fp_editor_on "CPU (FunkGui's SoftRaster)")
endif()
include(Flags)
include(Deps)

# ---- the shared inputs -----------------------------------------------------------------------------------------------
set(FP_SOURCE_ROOT ${PROJECT_SOURCE_DIR}/Source)
file(GLOB_RECURSE FP_DSP_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/fsdsp/*.cpp ${FP_SOURCE_ROOT}/dsp/*.cpp)
file(GLOB FP_PLUGIN_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/plugin/*.cpp)
list(FILTER FP_PLUGIN_SOURCES EXCLUDE REGEX "/CreateEditor[^/]*\\.cpp$")
set(FP_CREATE_EDITOR_GENERIC ${FP_SOURCE_ROOT}/plugin/CreateEditorGeneric.cpp)        # the probes' editor factory
file(GLOB_RECURSE FP_EDITOR_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/*.cpp)
list(FILTER FP_EDITOR_SOURCES EXCLUDE REGEX "/Source/editor/(gpu|nogpu)/")
# The editor's host: FunkGui's EditorHost on the GPU (gpu/), or, with FP_WITH_GPU off, the CPU host
# (nogpu/: SoftEditorHost, its factory, and FP_NOGPU_SHARED: what FunkGui::gpu would define that the shared code
# calls, which the probes and the sandbox's helper take too).
set(FP_NOGPU_SHARED "")
if(FP_WITH_GPU)
  file(GLOB FP_EDITOR_GPU_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/gpu/*.cpp ${FP_SOURCE_ROOT}/editor/gpu/*.mm)
else()
  file(GLOB FP_EDITOR_GPU_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/editor/nogpu/*.cpp)
  set(FP_NOGPU_SHARED ${FP_SOURCE_ROOT}/editor/nogpu/ShaderLibraryNone.cpp)
  list(FILTER FP_EDITOR_GPU_SOURCES EXCLUDE REGEX "/ShaderLibraryNone\\.cpp$")
endif()
file(GLOB FP_FMINUS_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/fminus/*.cpp)
# The GUI sandbox's helper (Source/sandbox/): its own sources, and the editor's .gui code and the .gui and program
# reading it shares with the plugin, by name: nothing that compiles a program (Patch.cpp, FMinus, LLVM), plays one
# (Source/dsp) or is the plugin's (Source/plugin). A file added to this list must keep that so.
file(GLOB FP_SANDBOX_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/sandbox/*.cpp)
set(FP_SANDBOX_SHARED "")
foreach(_f editor/GuiView editor/GuiItems editor/GuiLayout editor/Looks editor/DisplayItem editor/Items editor/PictureDraw
           editor/Theme editor/ShaderFont
           fminus/GuiSpec fminus/PanelSpec fminus/Lang fminus/LangConditionals fminus/HostFile fminus/Shaders fminus/ShaderLib)
  list(APPEND FP_SANDBOX_SHARED ${FP_SOURCE_ROOT}/${_f}.cpp)
endforeach()
# The headless renderer (Source/render/): its own sources, the DSP library and, by name, the plugin's reading of the
# tools' JSON (plugin/ChatScenario.cpp: juce_core and the DSP library alone; ChatCaptured.cpp holds what reaches the
# plugin). A file added to this list must keep that so: no processor, no editor, no FunkGui.
file(GLOB FP_RENDER_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/render/*.cpp)
set(FP_RENDER_SHARED ${FP_SOURCE_ROOT}/plugin/ChatScenario.cpp)
# The Standalone app (Source/plugin/standalone/, its Standalone target only): our sources' warnings, and ARC for its
# Objective-C++.
file(GLOB FP_STANDALONE_SOURCES CONFIGURE_DEPENDS ${FP_SOURCE_ROOT}/plugin/standalone/*.cpp
                                                  ${FP_SOURCE_ROOT}/plugin/standalone/*.mm)
# Each platform's own: macOS the window in Objective-C++ and the racing wheel over IOKit; Windows and Linux *Win.cpp
# (JUCE's window with a menu bar: nothing in it is Windows'); Windows *None.cpp (no racing wheel), Linux *Linux.cpp (the
# racing wheel over the kernel's evdev and sysfs: plugin/WheelEvdev.h).
if(APPLE)
  list(FILTER FP_STANDALONE_SOURCES EXCLUDE REGEX "(Win|None|Linux)\\.cpp$")
elseif(FP_LINUX)
  list(FILTER FP_STANDALONE_SOURCES EXCLUDE REGEX "(\\.mm|/RacingWheel\\.cpp|/RacingWheelNone\\.cpp)$")
else()
  list(FILTER FP_STANDALONE_SOURCES EXCLUDE REGEX "(\\.mm|/RacingWheel\\.cpp|Linux\\.cpp)$")
endif()
fp_warn_sources(${FP_STANDALONE_SOURCES})
foreach(_f IN LISTS FP_STANDALONE_SOURCES)
  if(_f MATCHES "\\.mm$")
    set_property(SOURCE ${_f} DIRECTORY ${PROJECT_SOURCE_DIR} APPEND PROPERTY COMPILE_OPTIONS -fobjc-arc)
  endif()
endforeach()

# The editor's LIBRARY (Source/fminus/library/<n>-<category>/*.fm and *.cir, in folder and then file-name order),
# compiled into every product: building blocks a program takes as tabs of its own and #includes (the LIBRARY menu,
# Claude's add_library_file): FMinus files, and circuit netlists (.cir, fminus/Circuits.h). Each file's first line is
# "// <its file name>: what it is" ("* <its file name>: what it is" in a netlist, whose comments are SPICE's); a
# folder's name, its number and dashes dropped, is its category. Read once here; each product's Library.h is written
# from these rows.
file(GLOB FP_FMINUS_LIBRARY CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/Source/fminus/library/*/*.fm
                                              ${PROJECT_SOURCE_DIR}/Source/fminus/library/*/*.cir)
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
  if(NOT _name MATCHES "^[A-Za-z0-9_][A-Za-z0-9_.-]*\\.(fm|cir)$")
    message(FATAL_ERROR "${_f}: a library file's name is a tab's name (letters, digits, _, - and .), ending .fm or .cir")
  endif()
  if(_name MATCHES "\\.cir$")
    set(_comment "\\*")
    set(_said "*")
  else()
    set(_comment "//")
    set(_said "//")
  endif()
  if(NOT _text MATCHES "^${_comment} ([^:\n]+): ([^\n\"\\\\]+)\n")
    message(FATAL_ERROR "${_f}: the first line must be '${_said} ${_name}: what it is' (no quotes or backslashes)")
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
set(FP_MCP_VERSION "0.16.0")

# The probes (cmake/Probes.cmake): `probes` builds every product's.
enable_testing()
include(Probes)
add_custom_target(probes)

# ---- one product -----------------------------------------------------------------------------------------------------
# fp_add_product(<key>): products/<key>.cmake's identity, checked, and everything built from it: the generated headers
# in <build>/generated/<key>/, the static libraries <Name>_fminus and <Name>_dsp, the plugin <Name> (AU, VST3,
# Standalone), the MCP server <Name>MCP, the headless renderer <Name>Render and the extension <Name>.mcpb, the probes
# <Name>_probes and their tests, and the target <key>, which builds all of them. The PRODUCT_* values live in this function's scope only.
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
    if(NOT APPLE)                                          # "RUN (or Cmd-Return)": a Windows or Linux keyboard's Ctrl
      string(REPLACE "Cmd-" "Ctrl-" _text "${_text}")
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

  file(CONFIGURE OUTPUT ${FP_GENERATED_DIR}/Library.h @ONLY CONTENT [=[// Generated by CMakeLists.txt from Source/fminus/library/*/*.fm and *.cir: the editor's LIBRARY, as it was when
// the plugin was built.
#pragma once

#include <string_view>

namespace @FP_NAMESPACE@
{
    struct LibraryFile
    {
        const char* name;                   // the tab's name: "svf.fm" (a netlist's "screamer.cir")
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

  # The factory projects (factory/<key>/<name>/, plugin/Factory.h), compiled into the plugin read-only: a folder a
  # project, its name the project's (as ProjectStore's tidyName leaves a name), holding project.json (a project file's
  # JSON, ProjectStore.h, of one version, its files' texts left out) and each file that version names, by its name,
  # beside it. scripts/import-factory.py writes them; a hand-made one keeps the same shape. Adding or editing one
  # reconfigures. FP_FACTORY_BYTES (the texts compiled in) is printed, what each plugin binary grows by, and the largest
  # text: each text is one string literal, which may pass the 65,536 bytes -Wpedantic warns about (the header lets it).
  file(GLOB _factory CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/factory/${key}/*/project.json)
  list(SORT _factory)
  set(FP_FACTORY_FILES "")
  set(FP_FACTORY_ROWS "")
  set(_fp_bytes 0)
  set(_fp_n 0)
  set(_fp_largest 0)
  set(_fp_largest_at "")
  foreach(_j IN LISTS _factory)
    get_filename_component(_dir "${_j}" DIRECTORY)
    get_filename_component(_name "${_dir}" NAME)
    string(LENGTH "${_name}" _len)
    if(_name MATCHES "[/\\\\:*?\"<>|]" OR _name MATCHES "^[. ]" OR _name MATCHES " $" OR _name MATCHES "  "
       OR _len GREATER 40 OR NOT _name MATCHES "^[ -~]+$")
      message(FATAL_ERROR "${_dir}: a factory project's folder is its name: printable ASCII, no / \\ : * ? \" < > |, "
                          "no leading dot or space, single spaces, at most 40 characters")
    endif()
    file(READ "${_j}" _json)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_j}")
    string(JSON _nv ERROR_VARIABLE _err LENGTH "${_json}" versions)
    if(_err OR NOT _nv EQUAL 1)
      message(FATAL_ERROR "${_j}: a factory project keeps one version (versions: one object, its files named)")
    endif()
    string(JSON _nf ERROR_VARIABLE _err LENGTH "${_json}" versions 0 files)
    if(_err OR _nf LESS 1)
      message(FATAL_ERROR "${_j}: its version names no file")
    endif()
    if(_json MATCHES "\\)factory\"")
      message(FATAL_ERROR "${_j}: the text )factory\" would end the raw string it is compiled into")
    endif()
    string(LENGTH "${_json}" _len)
    math(EXPR _fp_bytes "${_fp_bytes} + ${_len}")
    if(_len GREATER _fp_largest)
      set(_fp_largest ${_len})
      set(_fp_largest_at "${_name}/project.json")
    endif()
    set(_rows "")
    math(EXPR _last "${_nf} - 1")
    foreach(_i RANGE ${_last})
      string(JSON _fname GET "${_json}" versions 0 files ${_i} name)
      if(NOT _fname MATCHES "^[A-Za-z0-9_-][A-Za-z0-9_.-]*$" OR _fname STREQUAL "project.json")
        message(FATAL_ERROR "${_j}: '${_fname}' is no file name a tab may have (letters, digits, _, - and .)")
      endif()
      if(NOT EXISTS "${_dir}/${_fname}")
        message(FATAL_ERROR "${_j}: its version names ${_fname}, which is not beside it")
      endif()
      file(READ "${_dir}/${_fname}" _text)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dir}/${_fname}")
      if(_text MATCHES "\\)factory\"")
        message(FATAL_ERROR "${_dir}/${_fname}: the text )factory\" would end the raw string it is compiled into")
      endif()
      string(LENGTH "${_text}" _len)
      math(EXPR _fp_bytes "${_fp_bytes} + ${_len}")
      if(_len GREATER _fp_largest)
        set(_fp_largest ${_len})
        set(_fp_largest_at "${_name}/${_fname}")
      endif()
      string(APPEND _rows "        FactoryFile{ \"${_fname}\", R\"factory(${_text})factory\" },\n")
    endforeach()
    string(APPEND FP_FACTORY_FILES "    inline constexpr FactoryFile kFactoryFiles${_fp_n}[] = {\n${_rows}    };\n")
    string(APPEND FP_FACTORY_ROWS
           "        FactoryProject{ \"${_name}\", R\"factory(${_json})factory\", kFactoryFiles${_fp_n}, ${_nf} },\n")
    math(EXPR _fp_n "${_fp_n} + 1")
  endforeach()
  set(FP_FACTORY_COUNT ${_fp_n})
  set(_fp_largest_said "")
  if(_fp_n GREATER 0)
    set(_fp_largest_said ", the largest text ${_fp_largest} bytes (${_fp_largest_at})")
  endif()
  message(STATUS "${PRODUCT_NAME}: ${_fp_n} factory projects (factory/${key}/), ${_fp_bytes} bytes compiled in${_fp_largest_said}")
  file(CONFIGURE OUTPUT ${FP_GENERATED_DIR}/FactoryProjects.h @ONLY CONTENT [=[// Generated by CMakeLists.txt from factory/@key@/*/: the factory projects (plugin/Factory.h), as they were when
// the plugin was built: each one's project.json (a project file's JSON, its version's files' texts left out) and
// those files' texts, in its version's order. Each text is one raw string literal, which may be longer than the
// 65,536 bytes a compiler must take (-Wpedantic's -Woverlength-strings), which clang and clang-cl compile as well.
#pragma once

#include <cstddef>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

namespace @FP_NAMESPACE@
{
    struct FactoryFile
    {
        const char* name;                   // the tab's name: "program.fm"
        const char* text;
    };

    struct FactoryProject
    {
        const char* name;                   // its folder's: the project's own name
        const char* json;                   // its project.json
        const FactoryFile* files;           // its version's files, in its order
        int fileCount;
    };

@FP_FACTORY_FILES@
    inline constexpr std::size_t kFactoryCount = @FP_FACTORY_COUNT@;
    inline constexpr std::size_t kFactoryBytes = @_fp_bytes@;   // the texts compiled in
    inline constexpr FactoryProject kFactory[] = {
@FP_FACTORY_ROWS@        FactoryProject{ nullptr, nullptr, nullptr, 0 },   // the end (an empty set is no empty array)
    };
}

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
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
  if(APPLE)
    set(_fp_formats AU VST3 Standalone)
  else()
    set(_fp_formats VST3 Standalone)                     # Windows, Linux: no AU (Windows: JUCE makes the .ico)
  endif()
  juce_add_plugin(${PRODUCT_NAME}
      ICON_BIG                 ${_fp_logo}/installer-1024.png
      COMPANY_NAME             "${PRODUCT_COMPANY}"
      PLUGIN_MANUFACTURER_CODE ${PRODUCT_MFR_CODE}
      PLUGIN_CODE              ${PRODUCT_CODE}
      FORMATS                  ${_fp_formats}
      PRODUCT_NAME             "${PRODUCT_NAME}"
      VERSION                  ${PRODUCT_VERSION}
      BUNDLE_ID                ${PRODUCT_BUNDLE_ID}
      NEEDS_MIDI_OUTPUT        FALSE
      IS_MIDI_EFFECT           FALSE
      ${_fp_plugin_kind}
      COPY_PLUGIN_AFTER_BUILD  FALSE)                    # scripts/install.sh copies them, deliberately
  if(APPLE)
    get_target_property(_fp_icns ${PRODUCT_NAME} JUCE_ICON_FILE)
    configure_file(${_fp_logo}/icon.icns ${_fp_icns} COPYONLY)
  endif()

  set(_fp_plugin_own ${FP_PLUGIN_SOURCES} ${FP_EDITOR_SOURCES} ${FP_EDITOR_GPU_SOURCES} ${FP_NOGPU_SHARED})
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
  target_link_libraries(${PRODUCT_NAME} PRIVATE FunkGui::core)
  if(FP_WITH_GPU)
    target_link_libraries(${PRODUCT_NAME} PRIVATE FunkGui::gpu)
  else()
    target_compile_definitions(${PRODUCT_NAME} PRIVATE FP_SOFT_EDITOR=1)   # the Standalone's App.cpp asks the CPU host
  endif()
  if(APPLE)
    target_link_libraries(${PRODUCT_NAME} PRIVATE "-framework ImageIO" "-framework CoreGraphics")   # ChatImages.cpp
    target_link_libraries(${PRODUCT_NAME} PRIVATE "-framework CoreServices")   # FolderWatch.cpp: FSEvents
    target_link_libraries(${PRODUCT_NAME} PRIVATE compression)  # ProjectExport.cpp: a package's xz (libcompression)
  elseif(FP_WINDOWS)
    target_link_libraries(${PRODUCT_NAME} PRIVATE ${FP_WINDOWS_LIBS})
  elseif(FP_LINUX)
    target_link_libraries(${PRODUCT_NAME} PRIVATE ${FP_LINUX_LIBS})
  endif()
  funkgui_configure_product(${PRODUCT_NAME} PRODUCT ${PRODUCT_NAME} OBJC_PREFIX ${PRODUCT_OBJC_PREFIX}
                            ENV_PREFIX ${PRODUCT_ENV_PREFIX} PREFS_FOLDER ${PRODUCT_NAME})
  funkgui_compile_shaders(${PRODUCT_NAME})
  funkgui_add_font(${PRODUCT_NAME})                       # the font's and bgfx's licences into each bundle
  if(NOT FP_WITH_GPU)
    # Without FunkGui's GPU layer funkgui_add_font copies nothing; the bundled face's licence (OFL) still goes beside
    # each binary that embeds the face: the VST3's Contents/Resources/licences, the Standalone's licences/.
    get_property(_fp_fg_root GLOBAL PROPERTY FUNKGUI_SOURCE_DIR)
    foreach(_fmt VST3 Standalone)
      if(TARGET ${PRODUCT_NAME}_${_fmt})
        if(_fmt STREQUAL "VST3")
          set(_fp_lic_dir "$<TARGET_FILE_DIR:${PRODUCT_NAME}_${_fmt}>/../Resources/licences")
        else()
          set(_fp_lic_dir "$<TARGET_FILE_DIR:${PRODUCT_NAME}_${_fmt}>/licences")
        endif()
        add_custom_command(TARGET ${PRODUCT_NAME}_${_fmt} PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_fp_lic_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_fp_fg_root}/fonts/JetBrainsMono-LICENSE.txt" "${_fp_lic_dir}"
            VERBATIM)
      endif()
    endforeach()
  endif()

  # The Standalone is our own app (Source/plugin/standalone/): JUCE's custom standalone hook leaves JUCE's out (its
  # drawn title bar and Options button) for macOS's window, the editor's top bar under its buttons, and a menu bar. The
  # AU and the VST3 are unchanged.
  if(TARGET ${PRODUCT_NAME}_Standalone)
    target_sources(${PRODUCT_NAME}_Standalone PRIVATE ${FP_STANDALONE_SOURCES})
    target_compile_definitions(${PRODUCT_NAME}_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
    target_link_libraries(${PRODUCT_NAME}_Standalone PRIVATE fp_flags)
    if(APPLE)
      # The racing wheel (standalone/RacingWheel.cpp): IOKit's HID manager, the Standalone's alone.
      target_link_libraries(${PRODUCT_NAME}_Standalone PRIVATE "-framework IOKit" "-framework CoreFoundation")
    endif()
    if(NOT FP_WITH_GPU)
      target_compile_definitions(${PRODUCT_NAME}_Standalone PRIVATE FP_SOFT_EDITOR=1)
    endif()
  endif()

  # Each bundle exports its entry points and nothing else: the AU its factories, the VST3 its three functions, what
  # JUCE exports. Everything else stays private, LLVM above all (linked statically, with FMinus): exported, its weak
  # symbols would be merged across every plugin in the host's process that exports them too (dyld coalesces weak
  # definitions process-wide), and two plugins would share LLVM's state; so would the two products, whose C++ is one
  # namespace. The test lint.exports checks the built bundles.
  # Windows: a DLL exports what is marked dllexport and nothing else (LLVM's archives mark nothing), so there is no
  # list there, and nothing is signed (a VST3 on Windows needs no signature).
  if(APPLE)
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
  endif()   # APPLE: exports, shaderc, signatures

  # Linux: the VST3 exports its entry points and nothing else, as on macOS, through a version script (JUCE's three
  # functions); every other symbol is local, so none of LLVM's is bound to another loaded object's definition, nor
  # another's to ours. lint.exports checks the built .so.
  if(FP_LINUX AND TARGET ${PRODUCT_NAME}_VST3)
    set(_exports ${CMAKE_BINARY_DIR}/exports/${key})
    file(CONFIGURE OUTPUT ${_exports}-vst3.map
         CONTENT "{\n  global: GetPluginFactory; ModuleEntry; ModuleExit;\n  local: *;\n};\n")
    file(CONFIGURE OUTPUT ${_exports}-vst3.txt CONTENT "GetPluginFactory\nModuleEntry\nModuleExit\n")
    target_link_options(${PRODUCT_NAME}_VST3 PRIVATE "LINKER:--version-script=${_exports}-vst3.map")
    set_property(TARGET ${PRODUCT_NAME}_VST3 APPEND PROPERTY LINK_DEPENDS ${_exports}-vst3.map)
    set_property(GLOBAL APPEND PROPERTY FP_EXPORT_BUNDLES "$<TARGET_FILE:${PRODUCT_NAME}_VST3>=${_exports}-vst3.txt")
  endif()

  # Windows and Linux with the GPU layer: shaderc (shaderc.exe on Windows), the pinned bgfx's, where the plugin looks
  # for it (Processor.cpp's shaderTools): the VST3's Contents/Resources, beside its Contents/x86_64-win (x86_64-linux),
  # as on macOS, and the Standalone's own folder, beside its executable. Copied before the link, once it is built or
  # found.
  if((FP_WINDOWS OR FP_LINUX) AND FP_WITH_GPU)
    if(FUNKGUI_SHADERC)
      set(_fp_sc "${FUNKGUI_SHADERC}")
    else()
      set(_fp_sc "$<TARGET_FILE:shaderc>")
    endif()
    foreach(_fmt VST3 Standalone)
      if(TARGET ${PRODUCT_NAME}_${_fmt})
        if(_fmt STREQUAL "VST3")
          set(_fp_sc_dir "$<TARGET_FILE_DIR:${PRODUCT_NAME}_${_fmt}>/../Resources")
        else()
          set(_fp_sc_dir "$<TARGET_FILE_DIR:${PRODUCT_NAME}_${_fmt}>")
        endif()
        if(NOT FUNKGUI_SHADERC)
          add_dependencies(${PRODUCT_NAME}_${_fmt} shaderc)
        endif()
        add_custom_command(TARGET ${PRODUCT_NAME}_${_fmt} PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_fp_sc_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_fp_sc}" "${_fp_sc_dir}/shaderc${CMAKE_EXECUTABLE_SUFFIX}"
            VERBATIM)
      endif()
    endforeach()
  endif()

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
  if(FP_WINDOWS)
    target_link_libraries(${FP_MCP} PRIVATE ${FP_WINDOWS_LIBS})
  endif()
  set_target_properties(${FP_MCP} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  # ---- the headless renderer (Source/render): <Name>Render, beside the server in the extension -----------------------
  # A program rendered and measured from the command line with the plugin's compiler and offline render (the DSP
  # library, FMinus's JIT inside it) and the tools' JSON (FP_RENDER_SHARED): juce_core, no plugin, no editor. The
  # extension carries it (server/<Name>Render), so an installed extension has it beside its server.
  set(FP_RENDER ${PRODUCT_NAME}Render)
  juce_add_console_app(${FP_RENDER} PRODUCT_NAME "${FP_RENDER}" VERSION ${PRODUCT_VERSION})
  set(_fp_render_own ${FP_RENDER_SOURCES} ${FP_RENDER_SHARED})
  target_sources(${FP_RENDER} PRIVATE ${_fp_render_own})
  fp_warn_sources(${_fp_render_own})
  target_include_directories(${FP_RENDER} PRIVATE ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR})
  target_compile_definitions(${FP_RENDER} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(${FP_RENDER} PRIVATE ${PRODUCT_NAME}_dsp fp_flags juce::juce_core
                                             juce::juce_recommended_config_flags)
  if(FP_LTO)
    target_link_libraries(${FP_RENDER} PRIVATE juce::juce_recommended_lto_flags)
  endif()
  if(APPLE)
    target_link_options(${FP_RENDER} PRIVATE "LINKER:-dead_strip")
  elseif(FP_WINDOWS)
    target_link_libraries(${FP_RENDER} PRIVATE ${FP_WINDOWS_LIBS})
  endif()
  set_target_properties(${FP_RENDER} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  # ---- the GUI sandbox's helper (Source/sandbox): <Name>GuiSandbox, beside the server, which starts it ---------------
  # The editor's .gui code and FunkGui, headless on JUCE's message thread; no FMinus, no LLVM, no audio, no plugin
  # code (FP_SANDBOX_SHARED above). The .mcpb carries it beside the server.
  set(FP_SANDBOX ${PRODUCT_NAME}GuiSandbox)
  juce_add_console_app(${FP_SANDBOX} PRODUCT_NAME "${FP_SANDBOX}" VERSION ${PRODUCT_VERSION})
  set(_fp_sandbox_own ${FP_SANDBOX_SOURCES} ${FP_SANDBOX_SHARED} ${FP_NOGPU_SHARED})
  target_sources(${FP_SANDBOX} PRIVATE ${_fp_sandbox_own})
  fp_warn_sources(${_fp_sandbox_own})
  target_include_directories(${FP_SANDBOX} PRIVATE ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR})
  target_compile_definitions(${FP_SANDBOX} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_link_libraries(${FP_SANDBOX} PRIVATE fp_flags fp_lto FunkGui::core juce::juce_recommended_config_flags)
  if(FP_WITH_GPU)
    target_link_libraries(${FP_SANDBOX} PRIVATE FunkGui::gpu)
  endif()
  if(FP_LTO)
    target_link_libraries(${FP_SANDBOX} PRIVATE juce::juce_recommended_lto_flags)
  endif()
  if(APPLE)
    target_link_options(${FP_SANDBOX} PRIVATE "LINKER:-dead_strip")
  elseif(FP_WINDOWS)
    target_link_libraries(${FP_SANDBOX} PRIVATE ${FP_WINDOWS_LIBS})
  endif()
  funkgui_configure_product(${FP_SANDBOX} PRODUCT ${PRODUCT_NAME} OBJC_PREFIX ${PRODUCT_OBJC_PREFIX}
                            ENV_PREFIX ${PRODUCT_ENV_PREFIX} PREFS_FOLDER ${PRODUCT_NAME})
  funkgui_compile_shaders(${FP_SANDBOX})                  # FunkGui::gpu's own shaders (the UI's, the pictures')
  set_target_properties(${FP_SANDBOX} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
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
  # The server, the helper and the renderer as the extension holds them (server/<name>, .exe on Windows), and the
  # platform.
  set(FP_MCP_EXE ${FP_MCP}${CMAKE_EXECUTABLE_SUFFIX})
  set(FP_SANDBOX_EXE ${FP_SANDBOX}${CMAKE_EXECUTABLE_SUFFIX})
  set(FP_RENDER_EXE ${FP_RENDER}${CMAKE_EXECUTABLE_SUFFIX})
  if(FP_WINDOWS)
    set(FP_MCP_PLATFORM "win32")
    set(FP_MCP_MACHINE "computer")
  elseif(FP_LINUX)
    set(FP_MCP_PLATFORM "linux")
    set(FP_MCP_MACHINE "computer")
  else()
    set(FP_MCP_PLATFORM "darwin")
    set(FP_MCP_MACHINE "Mac")
  endif()
  set(_mcpb ${CMAKE_BINARY_DIR}/mcpb/${key})
  file(CONFIGURE OUTPUT ${_mcpb}/manifest.json @ONLY CONTENT [=[{
  "manifest_version": "0.3",
  "name": "@FP_MCP_ID@",
  "display_name": "@PRODUCT_NAME@",
  "version": "@FP_MCP_VERSION@",
  "description": "Ask Claude for @FP_MCP_ASK@ in words: it builds it in @PRODUCT_NAME@, listens to it and sets its dials.",
  "long_description": "Open @PRODUCT_NAME@ in your DAW (on @FP_MCP_TRACK@) or as its own app, then talk to Claude about the sound you want. Claude writes the @FP_MCP_WHAT@'s program, runs it, measures and looks at the result, fixes what is off and leaves the dials at good settings; you hear it right away, and the plugin's window shows what Claude did. Try: @FP_MCP_EXAMPLES@\n\nWith more than one open, say which you mean by its track or project (each also shows its name, such as \"@FP_MCP_NAME@ 2\", while Claude is connected). Claude Desktop's + menu also has ready-made requests: Make a sound, Improve the sound, Explain the sound, Fix what I hear.\n\nClaude can also design the plugin's panel with you, even with the plugin closed: it sketches the interface in a sandbox, sees it drawn exactly as the plugin draws it, and puts it into the plugin only when you ask. The extension carries a second program for that, @FP_SANDBOX@ (beside the server), which draws panels and nothing else.\n\nA third, @FP_RENDER@ (beside the server too), renders and measures a program from the command line, as Claude's tools do, for whoever writes programs in a terminal or an editor; Claude Desktop never runs it.\n\nNothing leaves your @FP_MCP_MACHINE@ except your conversation with Claude: the extension talks to the plugin through a private local connection.",
  "author": { "name": "@PRODUCT_COMPANY@" },
  "icon": "icon.png",
  "server": {
    "type": "binary",
    "entry_point": "server/@FP_MCP_EXE@",
    "mcp_config": { "command": "${__dirname}/server/@FP_MCP_EXE@", "args": [], "env": {} }
  },
  "tools": [
    { "name": "list_instances", "description": "Find the open @FP_MCP_WHAT@s" },
    { "name": "playground_guide", "description": "Read the guide to the @FP_MCP_WHAT@ and its language" },
    { "name": "gui_sandbox", "description": "Design the @FP_MCP_WHAT@'s panel and see it drawn, with or without the plugin open" }
  ],
  "tools_generated": true,
  "prompts_generated": true,
  "compatibility": { "platforms": ["@FP_MCP_PLATFORM@"] }
}
]=])
  # The licences of what the sandbox's helper holds, as each bundle carries them (funkgui_add_font): bgfx's, bx's,
  # bimg's and bgfx.cmake's (FunkGui's GPU) and the bundled font's (OFL), in licences/ of the extension.
  get_property(_fp_funkgui_root GLOBAL PROPERTY FUNKGUI_SOURCE_DIR)
  get_property(_fp_licence_files GLOBAL PROPERTY FUNKGUI_LICENCE_FILES)
  set(_fp_licence_files "${_fp_funkgui_root}/fonts/JetBrainsMono-LICENSE.txt" ${_fp_licence_files})
  set(_fp_licences "")
  foreach(_l IN LISTS _fp_licence_files)
    if(NOT EXISTS "${_l}")
      message(FATAL_ERROR "${_l} is missing: the extension carries it beside the sandbox's helper")
    endif()
    get_filename_component(_n "${_l}" NAME)
    list(APPEND _fp_licences licences/${_n})
  endforeach()
  # The extension, packed again whenever the server, the sandbox's helper, the renderer, its manifest, its icon or a
  # licence changes: the server finds the helper beside itself (server/<Name>GuiSandbox), and the renderer is there too.
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${PRODUCT_NAME}.mcpb
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_mcpb}/server ${_mcpb}/licences
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${FP_MCP}> ${_mcpb}/server/${FP_MCP_EXE}
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${FP_SANDBOX}> ${_mcpb}/server/${FP_SANDBOX_EXE}
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${FP_RENDER}> ${_mcpb}/server/${FP_RENDER_EXE}
      COMMAND ${CMAKE_COMMAND} -E copy ${_fp_licence_files} ${_mcpb}/licences/
      COMMAND ${CMAKE_COMMAND} -E copy ${_icon} ${_mcpb}/icon.png
      COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/${PRODUCT_NAME}.mcpb
      COMMAND ${CMAKE_COMMAND} -E chdir ${_mcpb} ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/${PRODUCT_NAME}.mcpb
              --format=zip manifest.json icon.png server/${FP_MCP_EXE} server/${FP_SANDBOX_EXE} server/${FP_RENDER_EXE}
              ${_fp_licences}
      DEPENDS ${FP_MCP} ${FP_SANDBOX} ${FP_RENDER} ${_mcpb}/manifest.json ${_icon} ${_fp_licence_files}
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

# ---- the installer: one app for every product, the products inside it (cmake/Installer.cmake) -----------------------
# macOS: "<Project> Installer.app" with its payload inside; Windows: "<Project> Installer.exe" with payload\ beside it.
# Linux has no installer app (its Engine has no Linux body).
if(NOT FP_LINUX)
  include(Installer)
  fp_add_installer()
endif()

# ---- the lints, once for every product's bundles (cmake/Probes.cmake) -----------------------------------------------
fp_add_lints()

# ---- wheel-dump: a developer's tool, outside `all` and never installed ----------------------------------------------
# Tools/wheel-dump/: a connected Logitech G29's HID elements and their live values, beside what plugin/WheelMap.h's
# table makes of them (README, "A racing wheel"). `cmake --build build-dev --target wheel-dump`, then
# build-dev/wheel-dump.
if(APPLE)
  add_executable(wheel-dump EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/Tools/wheel-dump/main.cpp
                                             ${FP_SOURCE_ROOT}/plugin/WheelMap.cpp)
  target_include_directories(wheel-dump PRIVATE ${FP_SOURCE_ROOT})
  target_link_libraries(wheel-dump PRIVATE fp_flags fp_warnings "-framework IOKit" "-framework CoreFoundation")
endif()

message(STATUS "${PROJECT_NAME}: ${_fp_products}; ${CMAKE_BUILD_TYPE}, LTO ${FP_LTO}, Werror ${FP_WERROR}, "
               "${FP_ARCH}, editor on the ${_fp_editor_on}")
