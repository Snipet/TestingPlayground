# products/synth.cmake: one product's identity, written in one place. The synth: a module Voice, one instance a
# note, sixteen at once.
# CMakeLists.txt reads every products/<key>.cmake and builds the shared sources once for each (fp_add_product); the
# values reach juce_add_plugin, funkgui_configure_product, the MCP server and the generated Product.h
# (cmake/Product.h.in), which the C++ code reads instead of JUCE's JucePlugin_* macros. The product's own inputs are
# named by its key (this file's name): Source/fminus/examples/synth/ (its EXAMPLES) and Source/mcp/icon-synth.png (its
# Claude Desktop extension's icon). Nothing else may spell these values out. scripts/plugin.py reads and rewrites
# them: keep one `set(PRODUCT_<KEY> "<value>")` per line.
#
# Whatever a user has installed depends on these: hosts find the plugin by its codes, Claude Desktop its extension by
# the name, the user's projects, preferences and sockets live in folders named after it. Change none lightly.
#
#   NAME         target and bundle name: PascalCase letters and digits; also the projects' folder
#                (~/Library/Audio/Presets/<COMPANY>/<NAME>/), FunkGui's preferences folder
#                (~/Library/Application Support/<NAME>/) and the MCP sockets' (Funk-<NAME>-mcp)
#   KIND         effect | instrument
#   VERSION      the bundles' version
#   COMPANY      shown by hosts
#   MFR_CODE     the AU manufacturer code: the same in all of the user's plugins (FSynth's)
#   CODE         this plugin's own code. Hosts identify an AU by the pair, so it must differ from every other plugin's,
#                or one plugin hides the other. 4 letters or digits, at least one upper-case letter.
#   BUNDLE_ID    reverse-DNS
#   ENV_PREFIX   FunkGui's environment prefix: <prefix>UI_THEME=1, <prefix>GPU_LOG=1, ...
#   OBJC_PREFIX  FunkGui's Objective-C class-name root: unique per product, since a host loads both into one process
set(PRODUCT_NAME        "TestingPlaygroundSynth")
set(PRODUCT_KIND        "instrument")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_COMPANY     "Funk")
set(PRODUCT_MFR_CODE    "Funk")
set(PRODUCT_CODE        "Tsyn")
set(PRODUCT_BUNDLE_ID   "com.funk.testingplaygroundsynth")
set(PRODUCT_ENV_PREFIX  "TSYN_")
set(PRODUCT_OBJC_PREFIX "Tsyn")
