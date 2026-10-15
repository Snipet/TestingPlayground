# cmake/LintLayers.cmake: the layer rule, as the CTest test lint.layers (cmake -DFP_SOURCE_DIR=<root> -P <this>).
#
# Source/dsp/** and Source/fsdsp/** are plain C++20: std and the SIMD intrinsics only. They include nothing from JUCE,
# FunkGui, Source/plugin or Source/editor, so the algorithms build and run in any host (a probe, a benchmark, another
# plugin) and the audio code cannot reach the GUI. The DSP library has no JUCE include path, so such an include
# already fails to compile; this test names the line.
if(NOT FP_SOURCE_DIR)
  message(FATAL_ERROR "LintLayers.cmake: pass -DFP_SOURCE_DIR=<project root>")
endif()
file(GLOB_RECURSE _files ${FP_SOURCE_DIR}/Source/dsp/* ${FP_SOURCE_DIR}/Source/fsdsp/*)
set(_bad "")
foreach(_f IN LISTS _files)
  if(NOT _f MATCHES "\\.(h|hpp|cpp|cc|inl|def)$")
    continue()
  endif()
  # <juce_...>, <funkgui/...>, "plugin/..." or "editor/..." (after any ../), or a path through a plugin/ or editor/
  # directory ("../../Source/plugin/Processor.h").
  set(_re "^[ \t]*#[ \t]*include[ \t]*[<\"]")
  string(APPEND _re "((\\.\\./)*(juce_|funkgui/|plugin/|editor/|JuceHeader)|[^>\"]*/(plugin|editor)/)")
  file(STRINGS ${_f} _lines REGEX "${_re}")
  foreach(_l IN LISTS _lines)
    file(RELATIVE_PATH _rel ${FP_SOURCE_DIR} ${_f})
    string(STRIP "${_l}" _l)
    list(APPEND _bad "${_rel}: ${_l}")
  endforeach()
endforeach()
if(_bad)
  list(JOIN _bad "\n  " _msg)
  message(FATAL_ERROR "lint.layers: Source/dsp and Source/fsdsp must not include JUCE, FunkGui, plugin/ or editor/:\n"
                      "  ${_msg}")
endif()
list(LENGTH _files _n)
message(STATUS "lint.layers: ${_n} files under Source/dsp and Source/fsdsp, no JUCE/FunkGui/plugin/editor include")
