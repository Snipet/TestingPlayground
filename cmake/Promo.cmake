# cmake/Promo.cmake: the promotional videos' renderer, <Name>Promo (Tools/promo/Main.cpp; scripts/promo.py drives it).
#
# fp_add_promo(<key>), which fp_add_product calls (macOS with the GPU only): a JUCE console app built as the probes are (the processor and the
# editor's Panel headless, as that product's), at the top of the build tree. It is no part of a build or of the gate
# (EXCLUDE_FROM_ALL): `cmake --build build-dev --target promo` builds every product's, and scripts/promo.py builds it
# before it renders. A change to what it calls (the Panel's probe hooks, EditorPort, Picture.h) builds it once by hand.
include_guard(GLOBAL)

set(FP_PROMO_ROOT ${PROJECT_SOURCE_DIR}/Tools/promo)
file(GLOB FP_PROMO_SOURCES CONFIGURE_DEPENDS ${FP_PROMO_ROOT}/*.cpp)

function(fp_add_promo key)
  if(NOT APPLE OR NOT FP_WITH_GPU)
    return()                                              # macOS's: POSIX pipes to ffmpeg, the GPU's pictures
  endif()
  set(FP_PROMO ${PRODUCT_NAME}Promo)
  juce_add_console_app(${FP_PROMO} PRODUCT_NAME "${FP_PROMO}" VERSION ${PRODUCT_VERSION})
  set(_own ${FP_PLUGIN_SOURCES} ${FP_EDITOR_SOURCES} ${FP_CREATE_EDITOR_GENERIC} ${FP_PROMO_SOURCES})
  target_sources(${FP_PROMO} PRIVATE ${_own})
  fp_warn_sources(${_own})
  target_include_directories(${FP_PROMO} PRIVATE ${FP_SOURCE_ROOT} ${FP_GENERATED_DIR} ${FP_BUILD_COMMIT_DIR})
  add_dependencies(${FP_PROMO} fp_build_commit)
  target_compile_definitions(${FP_PROMO} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
                                                PROMO_SHADERC="${FP_SHADERC}")
  target_link_libraries(${FP_PROMO} PRIVATE
      ${PRODUCT_NAME}_dsp fp_flags fp_xz FunkGui::core FunkGui::gpu
      juce::juce_audio_processors juce::juce_audio_formats juce::juce_recommended_config_flags)
  # As the probes link (cmake/Probes.cmake): ChatImages.cpp, FolderWatch.cpp, ProjectExport.cpp, Verify.cpp.
  target_link_libraries(${FP_PROMO} PRIVATE "-framework ImageIO" "-framework CoreGraphics" "-framework CoreServices"
                                            compression "-framework Security" "-framework CoreFoundation")
  if(FP_LTO)
    target_link_options(${FP_PROMO} PRIVATE $<$<CONFIG:Release>:-flto>)
  endif()
  funkgui_configure_product(${FP_PROMO} PRODUCT ${PRODUCT_NAME} OBJC_PREFIX ${PRODUCT_OBJC_PREFIX}
                            ENV_PREFIX ${PRODUCT_ENV_PREFIX} PREFS_FOLDER ${PRODUCT_NAME})
  funkgui_compile_shaders(${FP_PROMO})
  set_target_properties(${FP_PROMO} PROPERTIES EXCLUDE_FROM_ALL TRUE RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_dependencies(promo ${FP_PROMO})
endfunction()
