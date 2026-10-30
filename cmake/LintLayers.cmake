# cmake/LintLayers.cmake: the layer rules, as the CTest test lint.layers (cmake -DFP_SOURCE_DIR=<root> -P <this>).
#
# Source/dsp/**, Source/fsdsp/** and Source/fminus/** are plain C++: std and the SIMD intrinsics only (and FMinus's
# public API, fminus/FMinus.h). They include nothing from JUCE, FunkGui, Source/plugin or Source/editor, so the
# algorithms build and run in any host (a probe, a benchmark, another plugin) and the audio code cannot reach the GUI.
# The DSP library has no JUCE include path, so such an include already fails to compile; this test names the line.
#
# FMinus's internals ("frontend/...", "ir/...", "jit/...", "support/...", "runtime/...", its circuit compiler's
# "circuit/...") and LLVM's headers appear nowhere in Source/: FMinus's public headers, fminus/FMinus.h and
# fminus/Circuit.h (the circuit compiler's: Source/fminus/Circuits.cpp), are the way in (FMinus::FMinus gives no other
# include path, so such an include already fails to compile; this test names the line).
if(NOT FP_SOURCE_DIR)
  message(FATAL_ERROR "LintLayers.cmake: pass -DFP_SOURCE_DIR=<project root>")
endif()
file(GLOB_RECURSE _files ${FP_SOURCE_DIR}/Source/dsp/* ${FP_SOURCE_DIR}/Source/fsdsp/* ${FP_SOURCE_DIR}/Source/fminus/*)
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
  message(FATAL_ERROR "lint.layers: Source/dsp, Source/fsdsp and Source/fminus must not include JUCE, FunkGui, plugin/ "
                      "or editor/:\n  ${_msg}")
endif()

# Source/sandbox/** (the GUI sandbox's helper, CMakeLists.txt's FP_SANDBOX_SHARED) draws a .gui file with the editor's
# .gui code and reads programs without compiling them: it includes nothing of the plugin (Source/plugin), of the audio
# side (dsp/Engine.h, Compiler.h, Measure.h) or of what compiles a program (fminus/Patch.h, FMinus and its circuit
# compiler, fminus/Circuit.h, and the netlists' generator, fminus/Circuits.h), nor the editor's Panel, which holds all
# of those.
file(GLOB_RECURSE _sandbox ${FP_SOURCE_DIR}/Source/sandbox/*)
set(_inPlugin "")
foreach(_f IN LISTS _sandbox)
  if(NOT _f MATCHES "\\.(h|hpp|cpp|cc|mm|inl)$")
    continue()
  endif()
  file(STRINGS ${_f} _lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]((\\.\\./)*plugin/|fminus/Patch\\.h|fminus/FMinus\\.h|fminus/Circuits?\\.h|dsp/(Engine|Compiler|Measure)\\.h|editor/Panel\\.h)")
  foreach(_l IN LISTS _lines)
    file(RELATIVE_PATH _rel ${FP_SOURCE_DIR} ${_f})
    string(STRIP "${_l}" _l)
    list(APPEND _inPlugin "${_rel}: ${_l}")
  endforeach()
endforeach()
if(_inPlugin)
  list(JOIN _inPlugin "\n  " _msg)
  message(FATAL_ERROR "lint.layers: Source/sandbox holds no plugin, audio or compiler code (the GUI sandbox's helper "
                      "links none):\n  ${_msg}")
endif()

file(GLOB_RECURSE _all ${FP_SOURCE_DIR}/Source/*)
set(_inside "")
foreach(_f IN LISTS _all)
  if(NOT _f MATCHES "\\.(h|hpp|cpp|cc|mm|inl|def)$")
    continue()
  endif()
  file(STRINGS ${_f} _lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](llvm/|llvm-c/|frontend/|ir/|jit/|support/|runtime/|circuit/)")
  foreach(_l IN LISTS _lines)
    file(RELATIVE_PATH _rel ${FP_SOURCE_DIR} ${_f})
    string(STRIP "${_l}" _l)
    list(APPEND _inside "${_rel}: ${_l}")
  endforeach()
endforeach()
if(_inside)
  list(JOIN _inside "\n  " _msg)
  message(FATAL_ERROR "lint.layers: FMinus's internals, its circuit compiler's and LLVM stay inside FMinus (include "
                      "fminus/FMinus.h and fminus/Circuit.h, its public API):\n  ${_msg}")
endif()
list(LENGTH _files _n)
list(LENGTH _sandbox _ns)
message(STATUS "lint.layers: ${_n} files under Source/dsp, Source/fsdsp and Source/fminus, no JUCE/FunkGui/plugin/"
               "editor include; ${_ns} under Source/sandbox, no plugin, audio or compiler include; FMinus through "
               "fminus/FMinus.h and fminus/Circuit.h only")
