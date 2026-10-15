# cmake/Product.cmake: this plugin's identity, written in one place. CMakeLists.txt includes it before project(); the
# values reach juce_add_plugin, funkgui_configure_product and the generated Product.h (cmake/Product.h.in), which the
# C++ code reads instead of JUCE's JucePlugin_* macros. scripts/plugin.py rewrites the values (clone, rename): keep one
# `set(PRODUCT_<KEY> "<value>")` per line.
#
#   NAME         target and bundle name: PascalCase letters and digits
#   KIND         effect | instrument
#   VERSION      the bundles' version
#   COMPANY      shown by hosts
#   MFR_CODE     the AU manufacturer code: the same in all of the user's plugins (FSynth's)
#   CODE         this plugin's own code. Hosts identify an AU by the pair, so it must differ from every other plugin's,
#                or one plugin hides the other. 4 letters or digits, at least one upper-case letter.
#   BUNDLE_ID    reverse-DNS
#   NAMESPACE    the C++ namespace of Source/dsp, plugin and editor (the sources spell it out; configure checks)
#   ENV_PREFIX   FunkGui's environment prefix: <prefix>UI_THEME=1, <prefix>GPU_LOG=1, ...
#   OBJC_PREFIX  FunkGui's Objective-C class-name root (unique per plugin, so two FunkGui plugins share a host)
set(PRODUCT_NAME        "TestingPlayground")
set(PRODUCT_KIND        "effect")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_COMPANY     "Funk")
set(PRODUCT_MFR_CODE    "Funk")
set(PRODUCT_CODE        "Tstn")
set(PRODUCT_BUNDLE_ID   "com.funk.testingplayground")
set(PRODUCT_NAMESPACE   "testingplayground")
set(PRODUCT_ENV_PREFIX  "TSTN_")
set(PRODUCT_OBJC_PREFIX "Tstn")
