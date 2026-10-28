# cmake/Deps.cmake: the dependencies, pinned and checked (modelled on FSynth's cmake/FsynDeps.cmake, simplified).
#
#   JUCE 8.0.4            from the machine cache ${FP_DEPS_DIR}/JUCE-8.0.4 when it is there, else GitHub
#   bgfx.cmake            from ${FP_DEPS_DIR}/bgfx.cmake-<tag> when it is there, else GitHub (FunkGui's GPU layer);
#                         its commit and those of its submodules bgfx, bx and bimg are asserted
#   shaderc               the prebuilt ${FP_DEPS_DIR}/tools/shaderc-<tag>/shaderc when its stamp names the pinned
#                         bgfx.cmake commit, else built from bgfx.cmake (slow: about 2,000 CPU-seconds)
#   FunkGui v0.22.0       FetchContent at the pinned tag from FP_FUNKGUI_REPO: this machine's local repository
#                         (~/audio/libraries/FunkGui) when it exists, else GitHub; the commit is asserted. v0.17.0
#                         brought the TextEditor, v0.18.0 resizable Panels, v0.19.0 custom shaders, v0.20.0 images,
#                         v0.22.0 the TextEditor's code-editor hooks
#   FMinus                FetchContent at the pinned commit from FP_FMINUS_REPO (~/audio/libraries/FMinus, else
#                         GitHub): the JIT compiler, its library and runtime only. Its LLVM, Bison and Flex are the
#                         ones FMinus's own builds put in its deps cache (FP_FMINUS_DEPS_DIR), read, never built here
#
# ~/audio/.deps is a read-only cache written by FCompressor's Scripts/deps.sh: this build reads it and never writes it.
# FETCHCONTENT_SOURCE_DIR_<NAME> bypasses GIT_TAG, which is why every dependency's version and commit are checked after
# population, whatever its source. To move a pin, change the tag, the commit and (FunkGui) the version, or (bgfx) the
# API and the submodule commits, together. <build>/deps.txt records what this configuration used.
include_guard(GLOBAL)
include(FetchContent)
find_package(Git REQUIRED)

# ---- pins ------------------------------------------------------------------------------------------------------------
set(FP_JUCE_TAG          8.0.4)
set(FP_JUCE_SHA          51d11a2be6d5c97ccf12b4e5e827006e19f0555a)
set(FP_BGFX_TAG          v1.153.9385-561)
set(FP_BGFX_SHA          99752df38e40179cf998bb880fe4c16c0b3d60ca)
set(FP_BGFX_API          153)
# bgfx.cmake's submodules (the sources compiled): their commits, as FSynth's FsynDeps.cmake and DEPS.lock have them.
set(FP_BGFX_SUB_SHAS     bgfx=c7684e20da1e385edc439ef39cdb42b8c661016f
                         bx=0b001f5f36579e8aea07efa5af139ca18dad9505
                         bimg=3b4baab0128ac499c5c3bc37202781bf54084049)
set(FP_FUNKGUI_TAG       v0.22.0)
set(FP_FUNKGUI_SHA       0601659efcee5ec8a18b61ca879601b73aede0ee)
set(FP_FUNKGUI_VERSION   0.22.0)
set(FP_FMINUS_SHA        db0062c9203d6c33833e58d223f327ac7538451c)   # FMinus has no tags: a commit of its main
set(FP_FMINUS_LLVM       23.1.3)                                     # the LLVM that commit pins (cmake/FMinusPins.cmake)

# ---- helpers ---------------------------------------------------------------------------------------------------------
# fp_git(<out> <dir> <args>...): read-only git in <dir>; <out> = stripped stdout, <out>_RESULT = exit code.
function(fp_git out dir)
  execute_process(COMMAND ${CMAKE_COMMAND} -E env --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE
                          ${GIT_EXECUTABLE} --no-optional-locks -C "${dir}" ${ARGN}
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(${out} "${_o}" PARENT_SCOPE)
  set(${out}_RESULT "${_rc}" PARENT_SCOPE)
endfunction()

# fp_assert_commit(<dir> <sha> <name>): FATAL when <dir>'s HEAD is not commit <sha>. A <dir> that is not the root of its
# own git checkout (a source tarball) only warns: its version checks still apply.
function(fp_assert_commit dir sha name)
  fp_git(_top "${dir}" rev-parse --show-toplevel)
  if(NOT _top_RESULT EQUAL 0)
    message(WARNING "${PROJECT_NAME}: ${name} at ${dir} is not a git checkout; its commit cannot be checked")
    return()
  endif()
  file(REAL_PATH "${dir}" _d)
  file(REAL_PATH "${_top}" _t)
  if(NOT _d STREQUAL _t)
    message(WARNING "${PROJECT_NAME}: ${name} at ${dir} is not a git checkout; its commit cannot be checked")
    return()
  endif()
  fp_git(_head "${dir}" rev-parse -q --verify "HEAD^{commit}")
  if(NOT _head STREQUAL "${sha}")
    message(FATAL_ERROR "${PROJECT_NAME}: ${name} at ${dir} is at ${_head}, the pin is ${sha}. A stale cache, or a "
                        "populated <build>/_deps/<name>-src from an older pin? Delete that build directory.")
  endif()
endfunction()

# Default JUCE and bgfx.cmake to the machine cache (normal variables, never cached), unless the user gave a
# FETCHCONTENT_SOURCE_DIR_<NAME> of their own.
macro(fp_default_source_dir NAME SUBDIR)
  if(NOT FETCHCONTENT_SOURCE_DIR_${NAME} AND EXISTS "${FP_DEPS_DIR}/${SUBDIR}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_${NAME} "${FP_DEPS_DIR}/${SUBDIR}")
  endif()
endmacro()
fp_default_source_dir(JUCE JUCE-${FP_JUCE_TAG})
fp_default_source_dir(BGFX bgfx.cmake-${FP_BGFX_TAG})

# bgfx is declared here, before FunkGui declares its own, so this declaration wins: SYSTEM (its headers are system
# headers to our -Werror sources) and EXCLUDE_FROM_ALL (only what FunkGui::gpu links is built).
FetchContent_Declare(JUCE    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git  GIT_TAG ${FP_JUCE_TAG} GIT_SHALLOW TRUE)
FetchContent_Declare(bgfx    GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git GIT_TAG ${FP_BGFX_TAG} GIT_SHALLOW TRUE
                             SYSTEM EXCLUDE_FROM_ALL)
FetchContent_Declare(FunkGui GIT_REPOSITORY ${FP_FUNKGUI_REPO}                       GIT_TAG ${FP_FUNKGUI_TAG} GIT_SHALLOW FALSE)

set(_fp_deps_txt "# name\ttag\tcommit\tsource\n")

# ---- JUCE ------------------------------------------------------------------------------------------------------------
FetchContent_MakeAvailable(JUCE)
file(STRINGS "${juce_SOURCE_DIR}/modules/juce_core/system/juce_StandardHeader.h" _v
     REGEX "^#define JUCE_(MAJOR_VERSION|MINOR_VERSION|BUILDNUMBER) +[0-9]+")
set(_jv "")
foreach(_k MAJOR_VERSION MINOR_VERSION BUILDNUMBER)
  string(REGEX MATCH "#define JUCE_${_k} +([0-9]+)" _m "${_v}")
  list(APPEND _jv "${CMAKE_MATCH_1}")
endforeach()
list(JOIN _jv "." _jv)
if(NOT _jv STREQUAL FP_JUCE_TAG)
  message(FATAL_ERROR "${PROJECT_NAME}: JUCE at ${juce_SOURCE_DIR} is ${_jv}, the pin is ${FP_JUCE_TAG}")
endif()
fp_assert_commit("${juce_SOURCE_DIR}" ${FP_JUCE_SHA} JUCE)
string(APPEND _fp_deps_txt "JUCE\t${FP_JUCE_TAG}\t${FP_JUCE_SHA}\t${juce_SOURCE_DIR}\n")
# AppleClang 21 warns -Wnontrivial-memcall inside JUCE 8.0.4's bundled HarfBuzz (and FSynth found it in the VST3 SDK).
# Third-party code, compiled inside our targets: silenced on JUCE's module translation units only, where the compiler
# has the warning. Our sources keep it.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wnontrivial-memcall FP_CXX_HAS_WNONTRIVIAL_MEMCALL)
if(FP_CXX_HAS_WNONTRIVIAL_MEMCALL)
  file(GLOB _fp_juce_module_tus "${juce_SOURCE_DIR}/modules/juce_*/juce_*.cpp" "${juce_SOURCE_DIR}/modules/juce_*/juce_*.mm")
  set_source_files_properties(${_fp_juce_module_tus} DIRECTORY ${PROJECT_SOURCE_DIR}
                              PROPERTIES COMPILE_OPTIONS -Wno-nontrivial-memcall)
endif()

# ---- shaderc, then bgfx ----------------------------------------------------------------------------------------------
set(_sc_dir "${FP_DEPS_DIR}/tools/shaderc-${FP_BGFX_TAG}")
set(_sc_stamp "")
if(EXISTS "${_sc_dir}/shaderc.stamp")
  file(STRINGS "${_sc_dir}/shaderc.stamp" _sc_stamp LIMIT_COUNT 1)
endif()
if(IS_EXECUTABLE "${_sc_dir}/shaderc" AND _sc_stamp STREQUAL "bgfx.cmake ${FP_BGFX_SHA}")
  set(BGFX_BUILD_TOOLS OFF)                               # normal variables: CMP0077
  set(FUNKGUI_SHADERC "${_sc_dir}/shaderc")
  set(_fp_shaderc "${FUNKGUI_SHADERC}")
  set(FP_SHADERC "${FUNKGUI_SHADERC}")                    # the one the plugin carries (CMakeLists.txt)
else()
  set(BGFX_BUILD_TOOLS ON)
  set(BGFX_BUILD_TOOLS_SHADER ON)
  set(BGFX_BUILD_TOOLS_TEXTURE OFF)
  set(BGFX_BUILD_TOOLS_GEOMETRY OFF)
  set(BGFX_BUILD_TOOLS_BIN2C OFF)
  unset(FUNKGUI_SHADERC)
  set(_fp_shaderc "built from bgfx.cmake")
  set(FP_SHADERC "$<TARGET_FILE:shaderc>")
  message(STATUS "${PROJECT_NAME}: no prebuilt shaderc stamped 'bgfx.cmake ${FP_BGFX_SHA}' in ${_sc_dir}: building "
                 "it from bgfx.cmake (about 2,000 CPU-seconds)")
endif()
set(BGFX_BUILD_EXAMPLES OFF)
set(BGFX_INSTALL OFF)
FetchContent_MakeAvailable(bgfx)
foreach(_t bgfx bx bimg bimg_decode bimg_encode)
  if(TARGET ${_t})
    # A plugin binary exports only its entry points.
    set_target_properties(${_t} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  endif()
endforeach()
# metal-cpp's implementation inside bgfx marks its symbols visibility("default") unless this is defined.
target_compile_definitions(bgfx PRIVATE METALCPP_SYMBOL_VISIBILITY_HIDDEN)
file(STRINGS "${bgfx_SOURCE_DIR}/bgfx/include/bgfx/defines.h" _api
     REGEX "^#define BGFX_API_VERSION UINT32_C\\(${FP_BGFX_API}\\)")
if(NOT _api)
  message(FATAL_ERROR "${PROJECT_NAME}: bgfx at ${bgfx_SOURCE_DIR} is not API ${FP_BGFX_API} (bgfx.cmake ${FP_BGFX_TAG})")
endif()
fp_assert_commit("${bgfx_SOURCE_DIR}" ${FP_BGFX_SHA} bgfx.cmake)
string(APPEND _fp_deps_txt "bgfx.cmake\t${FP_BGFX_TAG}\t${FP_BGFX_SHA}\t${bgfx_SOURCE_DIR}\n")
foreach(_kv IN LISTS FP_BGFX_SUB_SHAS)
  string(REPLACE "=" ";" _kv "${_kv}")
  list(GET _kv 0 _sub)
  list(GET _kv 1 _sha)
  fp_assert_commit("${bgfx_SOURCE_DIR}/${_sub}" ${_sha} bgfx.cmake/${_sub})
  string(APPEND _fp_deps_txt "bgfx.cmake/${_sub}\t${FP_BGFX_TAG}\t${_sha}\t${bgfx_SOURCE_DIR}/${_sub}\n")
endforeach()
string(APPEND _fp_deps_txt "shaderc\t${FP_BGFX_TAG}\t${FP_BGFX_SHA}\t${_fp_shaderc}\n")

# ---- FunkGui ---------------------------------------------------------------------------------------------------------
# Its options, as normal variables before it is made available (CMP0077).
set(FUNKGUI_WITH_JUCE ON)
set(FUNKGUI_WITH_BGFX ON)
set(FUNKGUI_WITH_PRESETS OFF)                             # no preset store here (it would link SQLite)
set(FUNKGUI_BUILD_TOOLS OFF)
set(FUNKGUI_BUILD_TESTS OFF)
FetchContent_MakeAvailable(FunkGui)
FetchContent_GetProperties(FunkGui SOURCE_DIR _fp_funkgui_dir)
fp_assert_commit("${_fp_funkgui_dir}" ${FP_FUNKGUI_SHA} FunkGui)
if(NOT FUNKGUI_VERSION STREQUAL FP_FUNKGUI_VERSION)
  message(FATAL_ERROR "${PROJECT_NAME}: FunkGui ${FP_FUNKGUI_TAG} reports version '${FUNKGUI_VERSION}', the pin says "
                      "${FP_FUNKGUI_VERSION}")
endif()
foreach(_t FunkGui::harness FunkGui::core FunkGui::gpu)
  if(NOT TARGET ${_t})
    message(FATAL_ERROR "${PROJECT_NAME}: FunkGui ${FUNKGUI_VERSION} at ${_fp_funkgui_dir} defines no ${_t}")
  endif()
endforeach()
string(APPEND _fp_deps_txt "FunkGui\t${FP_FUNKGUI_TAG}\t${FP_FUNKGUI_SHA}\t${FP_FUNKGUI_REPO}\n")

# ---- FMinus ----------------------------------------------------------------------------------------------------------
# A subproject (SYSTEM: its headers are system headers to our -Werror sources; EXCLUDE_FROM_ALL: only what
# <name>_fminus links is built), without its tests, tools and examples, which it leaves out when it is not the top
# level. -DFETCHCONTENT_SOURCE_DIR_FMINUS=$HOME/audio/libraries/FMinus builds the working tree as it is instead, to try
# a change to FMinus before it is committed; the commit is not checked then.
#
# LLVM comes in FMinus's "system" mode, from the install FMinus's own build made in its deps cache: a Release build of
# this plugin takes the flavor without assertions when there is one (FMinus's release preset makes it), any other the
# one with them (its dev preset's), and FP_FMINUS_LLVM_DIR names one by hand. FMinus's default "cache" mode would build
# LLVM here (half an hour) whenever this tree's settings differ from the ones it was built with, as
# CMAKE_OSX_ARCHITECTURES does.
set(FP_FMINUS_DEPS_DIR "$ENV{HOME}/audio/libraries/FMinus/.deps" CACHE PATH
    "FMinus's deps cache, where its LLVM, Bison and Flex are installed (read only here)")
set(FP_FMINUS_LLVM_DIR "" CACHE PATH "The LLVM install FMinus uses (empty: chosen from FP_FMINUS_DEPS_DIR)")
set(_fp_llvm "${FP_FMINUS_LLVM_DIR}")
if(NOT _fp_llvm)
  file(GLOB _fp_llvm_plain LIST_DIRECTORIES true "${FP_FMINUS_DEPS_DIR}/install/llvm-${FP_FMINUS_LLVM}-Release-[0-9a-f]*")
  file(GLOB _fp_llvm_assert LIST_DIRECTORIES true "${FP_FMINUS_DEPS_DIR}/install/llvm-${FP_FMINUS_LLVM}-Release-assert-*")
  list(FILTER _fp_llvm_plain EXCLUDE REGEX "(-assert-|\\.lock$)")
  list(FILTER _fp_llvm_assert EXCLUDE REGEX "\\.lock$")
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_fp_llvm_order ${_fp_llvm_plain} ${_fp_llvm_assert})
  else()
    set(_fp_llvm_order ${_fp_llvm_assert} ${_fp_llvm_plain})
  endif()
  foreach(_d IN LISTS _fp_llvm_order)
    if(EXISTS "${_d}/fminus-flavor.cmake")                  # FMinus writes it once an install is whole
      set(_fp_llvm "${_d}")
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${_fp_llvm}/lib/cmake/llvm/LLVMConfig.cmake")
  message(FATAL_ERROR "${PROJECT_NAME}: no LLVM ${FP_FMINUS_LLVM} for FMinus in ${FP_FMINUS_DEPS_DIR}/install. Build "
                      "FMinus once (cd ~/audio/libraries/FMinus && cmake --workflow --preset dev), or set "
                      "FP_FMINUS_LLVM_DIR to an LLVM ${FP_FMINUS_LLVM} install.")
endif()
foreach(_tool bison-3.8.2 flex-2.6.4)
  if(NOT EXISTS "${FP_FMINUS_DEPS_DIR}/install/${_tool}/.fminus-installed")
    message(FATAL_ERROR "${PROJECT_NAME}: FMinus's ${_tool} is not in ${FP_FMINUS_DEPS_DIR}/install: build FMinus once "
                        "(cd ~/audio/libraries/FMinus && cmake --workflow --preset dev)")
  endif()
endforeach()

if(EXISTS "$ENV{HOME}/audio/libraries/FMinus/.git")
  set(_fp_fminus_repo "$ENV{HOME}/audio/libraries/FMinus")
else()
  set(_fp_fminus_repo "https://github.com/Snipet/FMinus.git")
endif()
set(FP_FMINUS_REPO "${_fp_fminus_repo}" CACHE STRING
    "FMinus's repository: the local one when this machine has it, else GitHub (the commit is FP_FMINUS_SHA)")
FetchContent_Declare(FMinus GIT_REPOSITORY ${FP_FMINUS_REPO} GIT_TAG ${FP_FMINUS_SHA} GIT_SHALLOW FALSE
                            SYSTEM EXCLUDE_FROM_ALL)

# Its options, as normal variables (CMP0077, and CMP0126 for the cache variables it declares).
set(FMINUS_LLVM_MODE system)
set(LLVM_DIR "${_fp_llvm}/lib/cmake/llvm")
set(FMINUS_DEPS_DIR "${FP_FMINUS_DEPS_DIR}")
set(FMINUS_PARSER_TOOLS_MODE fetch)                       # Bison and Flex from FMINUS_DEPS_DIR, installed
set(FMINUS_BUILD_TESTS OFF)
set(FMINUS_BUILD_TOOLS OFF)
set(FMINUS_BUILD_EXAMPLES OFF)
set(FMINUS_WERROR OFF)                                    # third-party code never gets our -Werror
FetchContent_MakeAvailable(FMinus)
FetchContent_GetProperties(FMinus SOURCE_DIR _fp_fminus_dir)
if(FETCHCONTENT_SOURCE_DIR_FMINUS)
  fp_git(_fp_fminus_head "${_fp_fminus_dir}" rev-parse -q --verify "HEAD^{commit}")
  message(STATUS "${PROJECT_NAME}: FMinus from ${_fp_fminus_dir} as it is (HEAD ${_fp_fminus_head}), not the pin")
  set(_fp_fminus_commit "${_fp_fminus_head}+worktree")
else()
  fp_assert_commit("${_fp_fminus_dir}" ${FP_FMINUS_SHA} FMinus)
  set(_fp_fminus_commit "${FP_FMINUS_SHA}")
endif()
file(STRINGS "${_fp_fminus_dir}/cmake/FMinusPins.cmake" _fp_fminus_llvm_pin REGEX "^set\\(FMINUS_LLVM_VERSION ")
string(REPLACE "." "\\." _fp_llvm_re "${FP_FMINUS_LLVM}")
if(NOT _fp_fminus_llvm_pin MATCHES "[ \"]${_fp_llvm_re}\"?\\)")
  message(FATAL_ERROR "${PROJECT_NAME}: FMinus at ${_fp_fminus_dir} pins '${_fp_fminus_llvm_pin}', but FP_FMINUS_LLVM "
                      "is ${FP_FMINUS_LLVM}: move them together")
endif()
foreach(_t FMinus::FMinus FMinus::Runtime)
  if(NOT TARGET ${_t})
    message(FATAL_ERROR "${PROJECT_NAME}: FMinus at ${_fp_fminus_dir} defines no ${_t}")
  endif()
endforeach()
string(APPEND _fp_deps_txt "FMinus\t-\t${_fp_fminus_commit}\t${_fp_fminus_dir}\n")
string(APPEND _fp_deps_txt "FMinus/LLVM\t${FP_FMINUS_LLVM}\t-\t${_fp_llvm}\n")

file(WRITE "${CMAKE_BINARY_DIR}/deps.txt" "${_fp_deps_txt}")
message(STATUS "${PROJECT_NAME}: JUCE ${FP_JUCE_TAG}, bgfx.cmake ${FP_BGFX_TAG} (shaderc: ${_fp_shaderc}), "
               "FunkGui ${FP_FUNKGUI_TAG} from ${FP_FUNKGUI_REPO}, FMinus ${_fp_fminus_commit} on ${_fp_llvm}")
