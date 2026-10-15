# cmake/Deps.cmake: the dependencies, pinned and checked (modelled on FSynth's cmake/FsynDeps.cmake, simplified).
#
#   JUCE 8.0.4            from the machine cache ${FP_DEPS_DIR}/JUCE-8.0.4 when it is there, else GitHub
#   bgfx.cmake            from ${FP_DEPS_DIR}/bgfx.cmake-<tag> when it is there, else GitHub (FunkGui's GPU layer);
#                         its commit and those of its submodules bgfx, bx and bimg are asserted
#   shaderc               the prebuilt ${FP_DEPS_DIR}/tools/shaderc-<tag>/shaderc when its stamp names the pinned
#                         bgfx.cmake commit, else built from bgfx.cmake (slow: about 2,000 CPU-seconds)
#   FunkGui v0.16.0       FetchContent at the tag from FP_FUNKGUI_REPO: this machine's local repository
#                         (~/audio/libraries/FunkGui) when it exists, else GitHub; the tag's commit is asserted
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
set(FP_FUNKGUI_TAG       v0.16.0)
set(FP_FUNKGUI_SHA       e1982bd5e393aa7191ef2a32478c078441c456f3)   # the tag's commit (rev-parse v0.16.0^{commit})
set(FP_FUNKGUI_VERSION   0.16.0)

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
else()
  set(BGFX_BUILD_TOOLS ON)
  set(BGFX_BUILD_TOOLS_SHADER ON)
  set(BGFX_BUILD_TOOLS_TEXTURE OFF)
  set(BGFX_BUILD_TOOLS_GEOMETRY OFF)
  set(BGFX_BUILD_TOOLS_BIN2C OFF)
  unset(FUNKGUI_SHADERC)
  set(_fp_shaderc "built from bgfx.cmake")
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

file(WRITE "${CMAKE_BINARY_DIR}/deps.txt" "${_fp_deps_txt}")
message(STATUS "${PROJECT_NAME}: JUCE ${FP_JUCE_TAG}, bgfx.cmake ${FP_BGFX_TAG} (shaderc: ${_fp_shaderc}), "
               "FunkGui ${FP_FUNKGUI_TAG} from ${FP_FUNKGUI_REPO}")
