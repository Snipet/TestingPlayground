# cmake/Flags.cmake: the compile flags of our own code (modelled on FSynth's cmake/FsynArch.cmake, simplified).
#
#   fp_flags        INTERFACE: the CPU (macOS: -mcpu=apple-m1; Windows and Linux: x86-64 with AVX2 and FMA, which
#                   fsdsp's x86 SIMD needs) and the floating-point contract: -fno-math-errno -fno-trapping-math
#                   -ffp-contract=off (clang-cl takes them through /clang:). Never -ffast-math (JUCE's constexpr
#                   infinity() breaks under -ffinite-math-only, and the DSP core relies on IEEE arithmetic). Intended
#                   FMAs go through fsdsp::simd::fma.
#   fp_lto          INTERFACE: -flto in Release when FP_LTO is on (the DSP library; the plugin takes JUCE's LTO target).
#   fp_warnings     INTERFACE: FP_WARNING_FLAGS, for targets made only of our sources (the DSP library).
#   FP_WARNING_FLAGS  JUCE 8.0.4's clang warning list with -Wextra first and -Wno-float-equal last (exact float
#                   compares are intended in DSP code and probes), then -Werror when FP_WERROR is on. clang-cl reads
#                   -Wall as cl's /Wall (clang's -Weverything), so there it is /W4 (clang's -Wall -Wextra), and -Werror
#                   is /WX; the other -W flags it takes as clang does.
#   fp_warn_sources(<file>...)  FP_WARNING_FLAGS on single files, for targets that also compile JUCE's module sources
#                   and FunkGui's (the plugin, the probes): third-party code never gets our -Werror.
include_guard(GLOBAL)

add_library(fp_flags INTERFACE)
if(FP_WINDOWS)
  target_compile_options(fp_flags INTERFACE /arch:AVX2 /clang:-mavx2 /clang:-mfma /clang:-fno-math-errno
                                            /clang:-fno-trapping-math /clang:-ffp-contract=off)
elseif(FP_LINUX)
  target_compile_options(fp_flags INTERFACE -mavx2 -mfma -fno-math-errno -fno-trapping-math -ffp-contract=off)
else()
  target_compile_options(fp_flags INTERFACE -mcpu=apple-m1 -fno-math-errno -fno-trapping-math -ffp-contract=off)
endif()

add_library(fp_lto INTERFACE)
if(FP_LTO)
  target_compile_options(fp_lto INTERFACE $<$<CONFIG:Release>:-flto>)
  target_link_options(fp_lto INTERFACE $<$<CONFIG:Release>:-flto>)
endif()

if(FP_WINDOWS)
  set(FP_WARNING_FLAGS /W4)
else()
  set(FP_WARNING_FLAGS -Wextra -Wall)
endif()
list(APPEND FP_WARNING_FLAGS
    -Wshadow-all -Wshorten-64-to-32 -Wstrict-aliasing -Wuninitialized -Wunused-parameter -Wconversion
    -Wsign-compare -Wint-conversion -Wconditional-uninitialized -Wconstant-conversion -Wsign-conversion
    -Wbool-conversion -Wextra-semi -Wunreachable-code -Wcast-align -Wshift-sign-overflow -Wmissing-prototypes
    -Wnullable-to-nonnull-conversion -Wno-ignored-qualifiers -Wswitch-enum -Wpedantic -Wdeprecated
    -Wmissing-field-initializers -Wzero-as-null-pointer-constant -Wunused-private-field -Woverloaded-virtual -Wreorder
    -Winconsistent-missing-destructor-override -Wno-float-equal)
if(FP_WINDOWS)
  # Windows' own idioms in its headers' macros and in the calls our sources make to them (the Win32 API's
  # LONG / DWORD conversions, its non-standard extensions) are not ours to fix.
  list(APPEND FP_WARNING_FLAGS -Wno-language-extension-token -Wno-nonportable-system-include-path
                               -Wno-microsoft-enum-value -Wno-unknown-pragmas)
endif()
if(FP_WERROR)
  if(FP_WINDOWS)
    list(APPEND FP_WARNING_FLAGS /WX)
  else()
    list(APPEND FP_WARNING_FLAGS -Werror)
  endif()
endif()

add_library(fp_warnings INTERFACE)
target_compile_options(fp_warnings INTERFACE ${FP_WARNING_FLAGS})

function(fp_warn_sources)
  if(ARGN)
    set_source_files_properties(${ARGN} DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTIES COMPILE_OPTIONS "${FP_WARNING_FLAGS}")
  endif()
endfunction()
