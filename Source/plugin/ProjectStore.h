// Source/plugin/ProjectStore.h: the projects on this Mac. A project is a program (the editor's files, the tab shown)
// and its dials' values, under a name, kept as versions (every save adds one, numbered 1, 2, 3, ...: an older one
// opens again), and its presets: dial settings for its program, no code. One file each, in
// ~/Library/Audio/Presets/Funk/<this plugin's name>/<name>.fmproject, the effect's apart from the synth's (one's
// programs do not play in the other). A file is JSON:
//
//   { "format": "funk-playground-project", "version": 1, "plugin": "<product name>", "kind": "instrument",
//     "name": "Glass Bloom",
//     "versions": [ { "number": 1, "saved": 1761580000000, "note": "first save", "active": 0,
//                     "files": [ { "name": "program.fm", "text": "..." }, ... ],
//                     "dials": [ { "name": "cutoff", "value": 900 }, ... ],
//                     "circuits": [ { "key": "0123456789abcdef", "text": "fminus-circuit-choice 1 ..." } ] },
//                   ... ],                                                                     oldest first
//     "presets":  [ { "name": "Bright", "saved": 1761580100000, "from": 1, "dials": [ ... ] }, ... ],  by name
//     "info":     { "description": "...", "author": "...", "category": "Pad", "tags": [ "warm" ], "genres": [ ... ] } }
//
// A version's "circuits" (optional: an older project has none, and an older build reads past it) are the circuit
// self-test's answers for its netlists (fminus/Circuits.h: a netlist's key and fminus::circuit::Choice::text()), so a
// version opened plays its refined configuration from its first block instead of a provisional one until the test is
// run again; saving the same program again with answers its latest version lacks adds them to that version.
//
// The dials by parameter name, in the program's own units, so a version's or a preset's values land in whichever
// slots its program's parameters take, and a preset saved with one version applies to the next. A version's note says
// what changed since the one before ("program.fm +12 -3, lib.fm new, 2 dials"). A project keeps its last kMaxVersions
// versions; the numbers go on counting. A project's name is its file's (a file renamed in the Finder is a project
// renamed); names are told apart case aside, as the Mac's disks tell files apart.
//
// A project's info (Info) describes it to others: what an export (ProjectExport.h) carries besides the program, and
// what a project imported brings.
//
// The presets of the playground before projects (<name>.fmpreset: a program and its dials) become projects of one
// version, at the time the preset was saved, the first time the folder is read; the old files then move into the
// folder's "Presets before projects" (never deleted).
//
// Any thread; the functions do their own I/O. They hold no state but the folder override and a cache of the listing
// (a file read again only when its size or its time changes).
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

namespace playground::projectstore
{
    inline constexpr std::size_t kMaxName = 40;          // characters
    inline constexpr int kMaxVersions = 100;             // a project's, the newest
    inline constexpr const char* kExtension = ".fmproject";
    inline constexpr const char* kLegacyExtension = ".fmpreset";
    inline constexpr const char* kMigratedFolder = "Presets before projects";

    // The folder (tests point it elsewhere; an empty path restores the default). Under a probe's preferences sandbox
    // (<ENV_PREFIX>PREFS_DIR set) it is that folder's presets/. It is made when a project is first saved.
    juce::File directory();
    void setDirectoryForTests(const juce::File& dir);

    struct Dial
    {
        std::string name;
        float value = 0.0f;
    };

    // A circuit self-test's answer kept with a version (above): its netlist's key (16 hex digits) and the answer's
    // line (Choice::text()).
    struct Answer
    {
        std::string key;
        std::string text;
        friend bool operator==(const Answer&, const Answer&) = default;
    };
    inline constexpr std::size_t kMaxAnswers = 16;       // a version's, at most

    struct Version
    {
        int number = 0;
        int64_t savedMs = 0;                             // ms since 1970
        std::string note;                                // what changed since the version before
        Sources files;
        int active = 0;                                  // the tab shown
        std::vector<Dial> dials;
        std::vector<Answer> circuits;                    // the self-test's answers for its netlists (may be none)
    };

    struct Preset
    {
        std::string name;
        int64_t savedMs = 0;
        int from = 0;                                    // the version it was saved with (0: none)
        std::vector<Dial> dials;
    };

    inline constexpr int kMaxTags = 16;
    inline constexpr std::size_t kMaxTag = 24;           // characters
    inline constexpr std::size_t kMaxDescription = 600;
    inline constexpr std::size_t kMaxAuthor = 60;

    struct Info
    {
        std::string description;                         // what it is, in a few sentences
        std::string author;
        std::string category;                            // one ("Bass", "Reverb"): editor/ExportCard.h offers them
        std::vector<std::string> tags;                   // free words, case aside unique
        std::vector<std::string> genres;
        bool empty() const noexcept
        {
            return description.empty() && author.empty() && category.empty() && tags.empty() && genres.empty();
        }
        friend bool operator==(const Info&, const Info&) = default;
    };

    struct Project
    {
        std::string name;
        std::vector<Version> versions;                   // oldest first, never empty
        std::vector<Preset> presets;                     // by name, case aside
        Info info;

        const Version& latest() const { return versions.back(); }
        const Version* version(int number) const;        // none when it is not kept
        const Preset* preset(std::string_view wanted) const;   // case aside
    };

    struct Entry
    {
        std::string name;
        int64_t savedMs = 0;                             // its latest version's
        int latest = 0;                                  // its latest version's number
        int versions = 0;                                // how many it keeps
        std::vector<std::string> presets;                // their names, by name
    };

    // `name` as a project or a preset may have it: printable ASCII but / \ : * ? " < > | and a leading '.', its spaces
    // collapsed and trimmed, at most kMaxName characters; "" when nothing is left.
    std::string tidyName(std::string_view name);
    bool sameName(std::string_view a, std::string_view b);   // case aside

    std::vector<Entry> list();                           // by name, case aside
    bool exists(std::string_view name);
    // The project of that name: nothing, with why, when there is none, it is not a project of this plugin's, or its
    // file is damaged. Its name is its file's.
    std::optional<Project> load(std::string_view name, std::string* why = nullptr);

    struct Saved
    {
        int number = 0;                                  // the version's
        bool unchanged = false;                          // the files, the tab and the dials were the latest's: none added
        bool created = false;                            // a new project
    };
    // A new version of the project `name` (tidied; a new project when there is none), its note what changed since the
    // latest, with the self-test's answers for its netlists (`circuits`: at most kMaxAnswers kept; when the files, the
    // tab and the dials are the latest's, answers it lacks are added to it and no version is). Nothing, with why, when
    // it cannot be written.
    std::optional<Saved> saveVersion(std::string_view name, const Sources& files, int active, const std::vector<Dial>& dials,
                                     const std::vector<Answer>& circuits = {}, std::string* why = nullptr);
    // A preset of the project (over one of that name, case aside, which takes the new name's case).
    bool savePreset(std::string_view project, std::string_view preset, const std::vector<Dial>& dials, int from,
                    std::string* why = nullptr);
    bool removePreset(std::string_view project, std::string_view preset, std::string* why = nullptr);
    bool removeVersion(std::string_view project, int number, std::string* why = nullptr);   // never its only one
    bool remove(std::string_view name, std::string* why = nullptr);

    // The info as a project keeps it: printable text, cut to its lengths (a tag to kMaxTag, at most kMaxTags of them,
    // each once case aside, as the genres), the spaces collapsed.
    Info tidyInfo(Info info);
    std::vector<std::string> splitTags(std::string_view commaSeparated);   // "warm, analog,, Warm" -> warm, analog
    bool setInfo(std::string_view project, const Info& info, std::string* why = nullptr);

    // A project as the JSON of its file, and back (`name` the project's when the JSON is a file's; nothing, with why, when
    // it is not a project of this plugin's or it is damaged). For ProjectExport.
    std::string toJson(const Project& p);
    std::optional<Project> fromJson(const std::string& json, std::string_view name, std::string* why = nullptr);
    // A project added under its name, or, when one has it, "<name> 2", "<name> 3", ...: the name it took, or nothing,
    // with why, when it cannot be written.
    std::optional<std::string> add(Project p, std::string* why = nullptr);

    // What changed from `before` (none: "first save") to the files and dials given: each file added, removed or edited
    // ("program.fm +12 -3": its lines added and taken away, whatever their order), and how many dials moved.
    std::string describeChange(const Version* before, const Sources& files, const std::vector<Dial>& dials);
}
