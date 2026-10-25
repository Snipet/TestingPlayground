// Source/plugin/PresetStore.h: the presets on this Mac: a program (the editor's files, the tab shown) and its dials'
// values, saved under a name, one file each in ~/Library/Audio/Presets/Funk/<this plugin's name>/<name>.fmpreset, the
// effect's apart from the synth's (one's programs do not play in the other). A file is JSON:
//
//   { "format": "funk-playground-preset", "version": 1, "plugin": "TestingPlaygroundSynth", "kind": "instrument",
//     "name": "Glass Bloom", "active": 0, "files": [ { "name": "program.fm", "text": "..." }, ... ],
//     "dials": [ { "name": "cutoff", "value": 900 }, ... ] }
//
// The dials by parameter name, in the program's own units, so a preset's values land in whichever slots its program's
// parameters take. A preset's name is its file's (a file renamed in the Finder is a preset renamed); names are told
// apart case aside, as the Mac's disks tell files apart.
//
// Any thread; the functions do their own I/O and hold no state but the folder override.
#pragma once

#include "dsp/Sources.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace juce
{
    class File;
}

namespace testingplaygroundsynth::presetstore
{
    inline constexpr std::size_t kMaxName = 40;          // characters
    inline constexpr const char* kExtension = ".fmpreset";

    // The folder (tests point it elsewhere; an empty path restores the default). Under a probe's preferences sandbox
    // (<ENV_PREFIX>PREFS_DIR set) it is that folder's presets/. It is made when a preset is first saved.
    juce::File directory();
    void setDirectoryForTests(const juce::File& dir);

    struct Dial
    {
        std::string name;
        float value = 0.0f;
    };

    struct Preset
    {
        std::string name;
        Sources files;
        int active = 0;                                  // the tab shown
        std::vector<Dial> dials;
    };

    struct Entry
    {
        std::string name;
        int64_t modifiedMs = 0;                          // its file's last change, ms since 1970
    };

    // `name` as a preset may have it: printable ASCII but / \ : * ? " < > | and a leading '.', its spaces collapsed and
    // trimmed, at most kMaxName characters; "" when nothing is left.
    std::string tidyName(std::string_view name);

    std::vector<Entry> list();                           // by name, case aside
    bool exists(std::string_view name);
    // Writes `p` under p.name (tidied), over a preset of that name (case aside): false, with why, when it cannot.
    bool save(const Preset& p, std::string* why = nullptr);
    // The preset of that name: nothing, with why, when there is none, it is not a preset of this plugin's, or its
    // file is damaged. Its name is its file's.
    std::optional<Preset> load(std::string_view name, std::string* why = nullptr);
    bool remove(std::string_view name, std::string* why = nullptr);
}
