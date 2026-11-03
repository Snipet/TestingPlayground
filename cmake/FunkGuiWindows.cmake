# cmake/FunkGuiWindows.cmake: what FunkGui v0.23.1 lacks on Windows, carried here until FunkGui has it (included by
# cmake/Deps.cmake once FunkGui is available, on Windows only). FunkGui::core is an INTERFACE library whose sources
# compile inside each consumer's targets, so a source of FunkGui's that cannot compile on Windows is swapped in its
# INTERFACE_SOURCES for one of ours:
#
#   src/core/CLocale.cpp   POSIX's locale_t   -> Source/editor/nogpu/funkgui/CLocaleWin.cpp (the C runtime's _l calls)
#
# and a source that includes FunkGui's test harness (funkgui/test/Harness.h, which includes <unistd.h>: today
# src/canvas/Fingerprint.cpp) gets FP_POSIX_SHIM_DIR on its own include path, a <unistd.h> of the C runtime's calls
# (Source/editor/nogpu/funkgui/posix/). Only those sources: on a whole target's path, a library that tests for
# <unistd.h> would take Windows for POSIX. cmake/Probes.cmake gives the probes the same folder.
#
# Everything else of FunkGui::core compiles there as it is. FunkGui's GPU layer (FunkGui::gpu: bgfx, the EditorHost)
# has no Windows backend at all; the editor runs on Source/editor/nogpu/SoftEditorHost instead.
include_guard(GLOBAL)

get_target_property(_fp_fg_sources FunkGuiCore INTERFACE_SOURCES)
set(_fp_fg_swaps "src/core/CLocale.cpp=${PROJECT_SOURCE_DIR}/Source/editor/nogpu/funkgui/CLocaleWin.cpp")
foreach(_swap IN LISTS _fp_fg_swaps)
  string(REPLACE "=" ";" _swap "${_swap}")
  list(GET _swap 0 _theirs)
  list(GET _swap 1 _ours)
  set(_found OFF)
  set(_kept "")
  foreach(_s IN LISTS _fp_fg_sources)
    if(_s MATCHES "/${_theirs}$")
      set(_found ON)
    else()
      list(APPEND _kept "${_s}")
    endif()
  endforeach()
  if(NOT _found)
    message(FATAL_ERROR "${PROJECT_NAME}: FunkGui::core has no ${_theirs} to swap for Windows: has FunkGui moved it "
                        "(cmake/FunkGuiWindows.cmake)?")
  endif()
  list(APPEND _kept "${_ours}")
  set(_fp_fg_sources ${_kept})
endforeach()
set_target_properties(FunkGuiCore PROPERTIES INTERFACE_SOURCES "${_fp_fg_sources}")

set(FP_POSIX_SHIM_DIR "${PROJECT_SOURCE_DIR}/Source/editor/nogpu/funkgui/posix")
set(_fp_fg_harnessed "")
foreach(_s IN LISTS _fp_fg_sources)
  if(_s MATCHES "\\.cpp$" AND EXISTS "${_s}")
    file(STRINGS "${_s}" _hit REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]funkgui/test/Harness\\.h[>\"]")
    if(_hit)
      # FunkGui's sources compile inside this directory's targets: the property is set in its scope. A compile
      # option (/I), not INCLUDE_DIRECTORIES: funkgui_configure_product sets that property of each of FunkGui's sources
      # for every target it configures (its include/funkgui/<module>), replacing whatever was there.
      set_property(SOURCE "${_s}" DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY COMPILE_OPTIONS
                   "/I${FP_POSIX_SHIM_DIR}")
      get_filename_component(_n "${_s}" NAME)
      list(APPEND _fp_fg_harnessed ${_n})
    endif()
  endif()
endforeach()
message(STATUS "${PROJECT_NAME}: FunkGui::core on Windows: CLocale.cpp swapped for Source/editor/nogpu/funkgui/CLocaleWin.cpp; "
               "<unistd.h> from ${FP_POSIX_SHIM_DIR} for ${_fp_fg_harnessed}")
