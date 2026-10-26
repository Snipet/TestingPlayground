# cmake/LintSignature.cmake: the signature rule, as the CTest test lint.signature (cmake -DFP_BUNDLES=<bundle>;... -P
# <this>). Each built bundle's code signature holds, its resources sealed (codesign --verify --strict): a host loads
# no other (Live's plugin scanner refuses a VST3 whose seal does not hold, and lists it nowhere). CMakeLists.txt signs a
# bundle again when its first build leaves the seal broken.
if(NOT FP_BUNDLES)
  message(FATAL_ERROR "LintSignature.cmake: pass -DFP_BUNDLES=<bundle>;...")
endif()
find_program(_codesign codesign REQUIRED)
set(_bad "")
foreach(_b IN LISTS FP_BUNDLES)
  if(NOT EXISTS "${_b}")
    message(FATAL_ERROR "lint.signature: ${_b} is not built (build the plugin first)")
  endif()
  execute_process(COMMAND ${_codesign} --verify --strict "${_b}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    string(STRIP "${_err}" _err)
    list(APPEND _bad "${_err}")
  endif()
endforeach()
if(_bad)
  list(JOIN _bad "\n  " _msg)
  message(FATAL_ERROR "lint.signature:\n  ${_msg}")
endif()
message(STATUS "lint.signature: every bundle's signature holds")
