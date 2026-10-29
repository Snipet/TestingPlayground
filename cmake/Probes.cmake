# cmake/Probes.cmake: the probe executables, their self-registered CTest tests and the lints (modelled on FSynth's
# cmake/FsynProbes.cmake, simplified: no goldens, no Mode scopes).
#
# A probe is one file under Tools/probes/<layer>/ whose first matching line registers it:
#
#   // PROBE name=<layer>.<name> timeout=<seconds>
#
# and whose body is PROBE(<layer>, <name>) { ... } (Tools/probes/common/Probe.h). Every probe runs for every product:
# fp_add_probes(<key>), which fp_add_product calls with that product's variables, compiles them all into one
# executable, <Name>_probes (a JUCE console app: the processor and the editor's Panel run headless in it, as that
# product's), and each registered probe becomes the CTest test <key>:<layer>.<name> (effect:dsp.swap,
# synth:dsp.swap), labelled `probe`, `<layer>` and `<key>`. Files without the line (rigs, helpers) register nothing.
# Tools/probes/common/ holds the main() and the helpers.
#
# A probe test passes on exit 0 (FunkGui's Harness: 1 = a spec row failed, 4 = a harness error). Probes use spec rows
# only (P.near, P.le, P.ge, P.in, P.eq), judged at once with their tolerance; there are no golden files, so a golden
# row (P.num, P.hash, ...) would report golden_missing (exit 3) and fail. A product's outputs (WAVs, PNGs) go to
# <build>/probe-out/<key>/, its results JSON to <build>/probe-results/<key>/, and the FunkGui preferences of a probe
# to <build>/sandbox/<key>/<probe>/ (never the user's ~/Library).
#
# fp_add_lints(), once after every product: lint.exports and lint.signature over every product's bundles, and
# lint.layers over the sources, labelled `lint`.
include_guard(GLOBAL)

set(FP_PROBE_ROOT ${PROJECT_SOURCE_DIR}/Tools/probes)
file(GLOB_RECURSE FP_PROBE_SOURCES CONFIGURE_DEPENDS ${FP_PROBE_ROOT}/*.cpp)

# The registered probes, read once: FP_PROBE_NAMES (<layer>.<name>, in file order) and FP_PROBE_TIMEOUT_<name>.
set(FP_PROBE_NAMES "")
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
  if(_name IN_LIST FP_PROBE_NAMES)
    message(FATAL_ERROR "${_f}: probe ${_name} is registered twice")
  endif()
  list(APPEND FP_PROBE_NAMES ${_name})
  set(FP_PROBE_TIMEOUT_${_name} ${_timeout})
endforeach()
list(LENGTH FP_PROBE_NAMES _fp_n)
message(STATUS "${PROJECT_NAME}: ${_fp_n} probes, each run for every product: ${FP_PROBE_NAMES}")

# fp_add_probes(<key>): the product's probe executable and its tests (from the caller's PRODUCT_* and FP_* variables).
function(fp_add_probes key)
  set(FP_PROBES ${PRODUCT_NAME}_probes)
  juce_add_console_app(${FP_PROBES} PRODUCT_NAME "${FP_PROBES}" VERSION ${PRODUCT_VERSION})
  set(_fp_probe_own ${FP_PLUGIN_SOURCES} ${FP_EDITOR_SOURCES} ${FP_CREATE_EDITOR_GENERIC} ${FP_PROBE_SOURCES})
  target_sources(${FP_PROBES} PRIVATE ${_fp_probe_own})
  fp_warn_sources(${_fp_probe_own})
  target_include_directories(${FP_PROBES} PRIVATE ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR} ${FP_PROBE_ROOT}/common)
  target_compile_definitions(${FP_PROBES} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
                                                 PROBE_DEFAULT_OUT_DIR="${CMAKE_BINARY_DIR}/probe-out/${key}")
  target_link_libraries(${FP_PROBES} PRIVATE
      ${PRODUCT_NAME}_dsp fp_flags FunkGui::core FunkGui::gpu FunkGui::harness
      juce::juce_audio_processors juce::juce_recommended_config_flags)
  if(APPLE)
    target_link_libraries(${FP_PROBES} PRIVATE "-framework ImageIO" "-framework CoreGraphics")   # ChatImages.cpp
    target_link_libraries(${FP_PROBES} PRIVATE compression)   # ProjectExport.cpp
  endif()
  if(FP_LTO)
    target_link_options(${FP_PROBES} PRIVATE $<$<CONFIG:Release>:-flto>)  # the DSP library holds bitcode there
  endif()
  funkgui_configure_product(${FP_PROBES} PRODUCT ${PRODUCT_NAME} OBJC_PREFIX ${PRODUCT_OBJC_PREFIX}
                            ENV_PREFIX ${PRODUCT_ENV_PREFIX} PREFS_FOLDER ${PRODUCT_NAME})
  funkgui_compile_shaders(${FP_PROBES})                   # FunkGui::gpu's embedded shaders (the shaders' registry)
  # At the top of the build tree (build-dev/<Name>_probes), not in JUCE's artefacts folder.
  set_target_properties(${FP_PROBES} PROPERTIES EXCLUDE_FROM_ALL TRUE RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_dependencies(probes ${FP_PROBES})                    # `probes` builds every product's (the presets name it)
  add_dependencies(${FP_PROBES} ${FP_MCP} ${FP_SANDBOX})  # plugin.mcp and plugin.sandbox run the product's MCP server,
                                                          # and the sandbox's helper beside it

  set(_p ${PRODUCT_ENV_PREFIX})
  foreach(_name IN LISTS FP_PROBE_NAMES)
    string(REGEX MATCH "^[^.]+" _layer ${_name})
    set(_test ${key}:${_name})
    set(_sb ${CMAKE_BINARY_DIR}/sandbox/${key}/${_name})
    add_test(NAME ${_test}
             COMMAND $<TARGET_FILE:${FP_PROBES}> ${_name}
                     --golden-root ${PROJECT_SOURCE_DIR}/tests/golden --arch arm64
                     --bless-to ${CMAKE_BINARY_DIR}/golden-candidates/${key}
                     --results ${CMAKE_BINARY_DIR}/probe-results/${key})
    set_tests_properties(${_test} PROPERTIES
        LABELS "probe;${_layer};${key}"
        TIMEOUT ${FP_PROBE_TIMEOUT_${_name}}
        ENVIRONMENT "PROBE_OUT_DIR=${CMAKE_BINARY_DIR}/probe-out/${key};${_p}PREFS_DIR=${_sb};${_p}SHADERC=${FP_SHADERC};${_p}MCP_DIR=${_sb}/mcp;${_p}MCP_SERVER=$<TARGET_FILE:${FP_MCP}>")
  endforeach()
endfunction()

# fp_add_lints(): the lints, once, over what every product built.
function(fp_add_lints)
  # ---- the export lint: each built bundle exports its entry points only (CMakeLists.txt says why) ------------------
  get_property(_fp_bundles GLOBAL PROPERTY FP_EXPORT_BUNDLES)
  add_test(NAME lint.exports COMMAND ${CMAKE_COMMAND} "-DFP_BUNDLES=${_fp_bundles}"
                                     -P ${PROJECT_SOURCE_DIR}/cmake/LintExports.cmake)
  set_tests_properties(lint.exports PROPERTIES LABELS "lint" TIMEOUT 120)

  # ---- the signature lint: each built bundle's signature holds (a host refuses one whose seal does not) -----------
  get_property(_fp_signed GLOBAL PROPERTY FP_SIGNED_BUNDLES)
  add_test(NAME lint.signature COMMAND ${CMAKE_COMMAND} "-DFP_BUNDLES=${_fp_signed}"
                                       -P ${PROJECT_SOURCE_DIR}/cmake/LintSignature.cmake)
  set_tests_properties(lint.signature PROPERTIES LABELS "lint" TIMEOUT 120)

  # ---- the layer lint: Source/dsp and Source/fsdsp are JUCE-free and FunkGui-free ------------------------------------
  add_test(NAME lint.layers COMMAND ${CMAKE_COMMAND} -DFP_SOURCE_DIR=${PROJECT_SOURCE_DIR}
                                    -P ${PROJECT_SOURCE_DIR}/cmake/LintLayers.cmake)
  set_tests_properties(lint.layers PROPERTIES LABELS "lint" TIMEOUT 60)
endfunction()
