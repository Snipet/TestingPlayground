# cmake/InstallerPayload.cmake: the installer's payload, a build step (cmake/Installer.cmake runs it after the app and
# every product's bundles are built):
#
#   cmake -DAPP=<Installer.app> -DMANIFEST=<payload.json> -DITEMS=<bundle or file>|... -P InstallerPayload.cmake
#
# Each item (the products' .component, .vst3, .app and .mcpb) is cloned with ditto (--clone: on APFS a copy that costs
# neither time nor space; it keeps every signature and attribute) into <APP>/Contents/Resources/payload.new/, beside
# payload.json, which then takes the place of payload/ at once. Then the app is signed again, ad hoc, so that its seal
# covers what is in it now (a host or Gatekeeper checks it whole), and checked. Each item is checked first: a bundle
# whose signature does not hold is refused, as scripts/install.sh refuses it.
foreach(_v APP MANIFEST ITEMS)
  if("${${_v}}" STREQUAL "")
    message(FATAL_ERROR "InstallerPayload.cmake: pass -D${_v}=...")
  endif()
endforeach()
find_program(_ditto ditto REQUIRED)
find_program(_codesign codesign REQUIRED)

set(_res "${APP}/Contents/Resources")
set(_new "${_res}/payload.new")
file(REMOVE_RECURSE "${_new}")
file(MAKE_DIRECTORY "${_new}")
string(REPLACE "|" ";" _items "${ITEMS}")
set(_names "")
foreach(_item IN LISTS _items)
  if(NOT EXISTS "${_item}")
    message(FATAL_ERROR "the installer's payload: ${_item} is not built")
  endif()
  get_filename_component(_name "${_item}" NAME)
  if(IS_DIRECTORY "${_item}")
    execute_process(COMMAND ${_codesign} --verify --strict "${_item}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "the installer's payload: ${_item}: its signature does not hold (${_err})")
    endif()
  endif()
  execute_process(COMMAND ${_ditto} --clone "${_item}" "${_new}/${_name}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "the installer's payload: ditto ${_item}: ${_err}")
  endif()
  list(APPEND _names "${_name}")
endforeach()
file(COPY_FILE "${MANIFEST}" "${_new}/payload.json")
file(REMOVE_RECURSE "${_res}/payload")
file(RENAME "${_new}" "${_res}/payload")

# The app's seal over its payload.
execute_process(COMMAND ${_codesign} --force --sign - "${APP}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "the installer's payload: codesign ${APP}: ${_err}")
endif()
execute_process(COMMAND ${_codesign} --verify --strict "${APP}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "the installer's payload: ${APP}: its signature does not hold (${_err})")
endif()
list(JOIN _names ", " _names)
message(STATUS "the installer's payload: ${_names}")
