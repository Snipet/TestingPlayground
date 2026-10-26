# cmake/Probes.cmake: the probe executable and its self-registered CTest tests (modelled on FSynth's
# cmake/FsynProbes.cmake, simplified: no goldens, no Mode scopes).
#
# A probe is one file under Tools/probes/<layer>/ whose first matching line registers it:
#
#   // PROBE name=<layer>.<name> timeout=<seconds>
#
# and whose body is PROBE(<layer>, <name>) { ... } (Tools/probes/common/Probe.h). Every probe is compiled into one
# executable, <name>_probes (a JUCE console app: the processor and the editor's Panel run headless in it), and each
# registered probe becomes the CTest test <layer>.<name>, labelled `probe` and `<layer>`. Files without the line (rigs,
# helpers) register nothing. Tools/probes/common/ holds the main() and the helpers.
#
# A probe test passes on exit 0 (FunkGui's Harness: 1 = a spec row failed, 4 = a harness error). Probes use spec rows
# only (P.near, P.le, P.ge, P.in, P.eq), judged at once with their tolerance; there are no golden files, so a golden
# row (P.num, P.hash, ...) would report golden_missing (exit 3) and fail. Outputs (WAVs, PNGs) go to
# <build>/probe-out/, results JSON to <build>/probe-results/, and the FunkGui preferences of a probe to
# <build>/sandbox/<test>/ (never the user's ~/Library).
include_guard(GLOBAL)

set(FP_PROBES ${PRODUCT_NAME}_probes)
set(FP_PROBE_ROOT ${PROJECT_SOURCE_DIR}/Tools/probes)
file(GLOB_RECURSE FP_PROBE_SOURCES CONFIGURE_DEPENDS ${FP_PROBE_ROOT}/*.cpp)

juce_add_console_app(${FP_PROBES} PRODUCT_NAME "${FP_PROBES}")
set(_fp_probe_own ${FP_PLUGIN_SOURCES} ${FP_EDITOR_SOURCES} ${FP_CREATE_EDITOR_GENERIC} ${FP_PROBE_SOURCES})
target_sources(${FP_PROBES} PRIVATE ${_fp_probe_own})
fp_warn_sources(${_fp_probe_own})
target_include_directories(${FP_PROBES} PRIVATE ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR} ${FP_PROBE_ROOT}/common)
target_compile_definitions(${FP_PROBES} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
                                               PROBE_DEFAULT_OUT_DIR="${CMAKE_BINARY_DIR}/probe-out")
target_link_libraries(${FP_PROBES} PRIVATE
    ${PRODUCT_NAME}_dsp fp_flags FunkGui::core FunkGui::gpu FunkGui::harness
    juce::juce_audio_processors juce::juce_recommended_config_flags)
if(FP_LTO)
  target_link_options(${FP_PROBES} PRIVATE $<$<CONFIG:Release>:-flto>)    # the DSP library holds bitcode there
endif()
funkgui_configure_product(${FP_PROBES} PRODUCT ${PRODUCT_NAME} OBJC_PREFIX ${PRODUCT_OBJC_PREFIX}
                          ENV_PREFIX ${PRODUCT_ENV_PREFIX} PREFS_FOLDER ${PRODUCT_NAME})
funkgui_compile_shaders(${FP_PROBES})                     # FunkGui::gpu's embedded shaders (the shaders' registry)
# At the top of the build tree (build-dev/<Name>_probes), not in JUCE's artefacts folder.
set_target_properties(${FP_PROBES} PROPERTIES EXCLUDE_FROM_ALL TRUE RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# `probes` builds the executable (the presets name it; scripts/test.sh builds it).
add_custom_target(probes DEPENDS ${FP_PROBES})

# ---- self-registration -----------------------------------------------------------------------------------------------
set(_fp_names "")
foreach(_f IN LISTS FP_PROBE_SOURCES)
  file(STRINGS ${_f} _hdr LIMIT_COUNT 1 REGEX "^// PROBE ")
  if(NOT _hdr)
    continue()
  endif()
  if(NOT _hdr MATCHES "^// PROBE name=([a-z][a-z0-9_]*)\\.([a-z][a-z0-9_]*) timeout=([0-9]+)$")
    message(FATAL_ERROR "${_f}: malformed probe line '${_hdr}' (wanted '// PROBE name=<layer>.<name> timeout=<s>')")
  endif()
  set(_layer ${CMAKE_MATCH_1})
  set(_name ${CMAKE_MATCH_1}.${CMAKE_MATCH_2})
  set(_timeout ${CMAKE_MATCH_3})
  get_filename_component(_dir ${_f} DIRECTORY)
  get_filename_component(_dir ${_dir} NAME)
  if(NOT _dir STREQUAL _layer)
    message(FATAL_ERROR "${_f}: probe ${_name} belongs in Tools/probes/${_layer}/")
  endif()
  if(_name IN_LIST _fp_names)
    message(FATAL_ERROR "${_f}: probe ${_name} is registered twice")
  endif()
  list(APPEND _fp_names ${_name})
  add_test(NAME ${_name}
           COMMAND $<TARGET_FILE:${FP_PROBES}> ${_name}
                   --golden-root ${PROJECT_SOURCE_DIR}/tests/golden --arch arm64
                   --bless-to ${CMAKE_BINARY_DIR}/golden-candidates --results ${CMAKE_BINARY_DIR}/probe-results)
  set(_sb ${CMAKE_BINARY_DIR}/sandbox/${_name})
  set_tests_properties(${_name} PROPERTIES
      LABELS "probe;${_layer}"
      TIMEOUT ${_timeout}
      ENVIRONMENT "PROBE_OUT_DIR=${CMAKE_BINARY_DIR}/probe-out;${PRODUCT_ENV_PREFIX}PREFS_DIR=${_sb};${PRODUCT_ENV_PREFIX}SHADERC=${FP_SHADERC}")
endforeach()
list(LENGTH _fp_names _fp_n)
message(STATUS "${PRODUCT_NAME}: ${_fp_n} probes: ${_fp_names}")

# ---- the export lint: each built bundle exports its entry points only (CMakeLists.txt says why) ----------------------
set(_fp_bundles "")
foreach(_fmt AU VST3)
  if(TARGET ${PRODUCT_NAME}_${_fmt})
    string(TOLOWER ${_fmt} _list)
    list(APPEND _fp_bundles "$<TARGET_FILE:${PRODUCT_NAME}_${_fmt}>=${CMAKE_BINARY_DIR}/exports-${_list}.txt")
  endif()
endforeach()
add_test(NAME lint.exports COMMAND ${CMAKE_COMMAND} "-DFP_BUNDLES=${_fp_bundles}"
                                   -P ${PROJECT_SOURCE_DIR}/cmake/LintExports.cmake)
set_tests_properties(lint.exports PROPERTIES LABELS "lint" TIMEOUT 120)

# ---- the layer lint: Source/dsp and Source/fsdsp are JUCE-free and FunkGui-free ----------------------------------------
add_test(NAME lint.layers COMMAND ${CMAKE_COMMAND} -DFP_SOURCE_DIR=${PROJECT_SOURCE_DIR}
                                  -P ${PROJECT_SOURCE_DIR}/cmake/LintLayers.cmake)
set_tests_properties(lint.layers PROPERTIES LABELS "lint" TIMEOUT 60)
