// Source/plugin/ProjectStore.h: the projects on this Mac. A project is a program (the editor's files, the tab shown)
// and its dials' values, under a name, kept as versions (every save adds one, numbered 1, 2, 3, ...: an older one
// opens again), and its presets: dial settings for its program, no code. One file each, in
// ~/Library/Audio/Presets/Funk/<this plugin's name>/<name>.fmproject, the effect's apart from the synth's (one's
// programs do not play in the other). A file is JSON:
//
//   { "format": "funk-playground-project", "version": 1, "plugin": "<product name>", "kind": "instrument",
//     "name": "Glass Bloom", "id": "8c1f4e2a-...",
//     "versions": [ { "number": 1, "saved": 1761580000000, "note": "first save", "active": 0,
//                     "files": [ { "name": "program.fm", "text": "..." }, ... ],
//                     "dials": [ { "name": "cutoff", "value": 900 }, { "name": "drive", "value": 2, "label": "PITCH" }, ... ],
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
// slots its program's parameters take, and a preset saved with one version applies to the next. An int's or a bool's
// value that has a name (its switch's, buttons' or menu's text) keeps it too ("label": optional, added later, read past
// by an older build), and lands on that name wherever the program's options put it now (valueOf; Controls.h, "a dial's
// value across a declaration change"), so a preset saved on PITCH applies PITCH after an option is inserted before
// it; a name the options lack lands on the number. Saving a version from before labels again unchanged adds no version
// (a label follows from its value and the files) but gives it its values' names. A version's note says what changed
// since the one before ("program.fm +12 -3, lib.fm new, 2 dials"). A project keeps its last kMaxVersions versions; the
// numbers go on counting. A project's name is its file's (a file renamed in the Finder is a project renamed); names
// are told apart case aside, as the Mac's disks tell files apart.
//
// A project's "id" (optional: a project from before ids has none, and an older build reads past it) is a UUID, made
// when the project is first written (a save, a preset, its info) and kept in its file, so a project renamed in the
// Finder keeps it: the chats kept for the project on this Mac (ChatStore.h, chats/<id>/) are found by it, and a
// session names the project by it too (Processor.cpp), as a renamed project's name is gone. It is this Mac's: an export
// leaves it out (ProjectExport.h), and a project imported or added (add()) gets a new one, as does a copy of a file
// made in the Finder (idOf: the copy is the file whose name is not the "name" written in it). "name" is the name it was
// last written under (`writtenAs`); the project's name is its file's.
//
// A project's info (Info) describes it to others: what an export (ProjectExport.h) carries besides the program, and
// what a project imported brings.
//
// The presets of the playground before projects (<name>.fmpreset: a program and its dials) become projects of one
// version, at the time the preset was saved, the first time the folder is read; the old files then move into the
// folder's "Presets before projects" (never deleted).
//
// Any thread; the functions do their own I/O. They hold no state but the folder override and a cache of the listing
// and of each project's presets (a file read again only when its size or its time changes; what this process writes
// or reads whole, it keeps there as it is), and the Reader below.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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
        std::string label = {};                          // its value's option name, when it has one (above)
    };

    // The plain value dial `d` gives a control `c` of those options: its option's name's value, else its value, within
    // c's range (Controls.h's clampedPlain).
    inline float valueOf(const Dial& d, const Control& c, const std::vector<std::string>& options)
    {
        const float v = optionValue(c, options, d.label);
        return std::isfinite(v) ? v : clampedPlain(c, d.value);
    }

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
        std::string id;                                  // a UUID, lower case ("" in a project from before ids)
        std::string writtenAs;                           // the "name" in its file: the name it was last written under
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
    // A project's presets as the listing keeps them (by name): its file read whole only when its size or its time
    // changed since it was last read (here or by list(), whatever the case its name is spelt in: the same file by its
    // device and inode, on Windows by its path case aside), so a project open, whose presets the explorer asks for
    // every few seconds, costs a look at its file, not a parse of it (an Engine's file is half a MB and more).
    // Nothing, with why, as load(). parses() counts the files read whole, by any function, for the probes.
    struct PresetSummary
    {
        std::string name;
        int64_t savedMs = 0;
        int from = 0;                                    // the version it was saved with (0: none)
        friend bool operator==(const PresetSummary&, const PresetSummary&) = default;
    };
    std::optional<std::vector<PresetSummary>> presetsOf(std::string_view name, std::string* why = nullptr);
    int parses();

    // The listing and a project's presets for a caller that must not wait while a project's file is read whole (the
    // explorer's poll, on the message thread: Processor::projectsPolled). As list() and presetsOf(), from the same
    // cache, but a file the cache does not hold as it is now (another process wrote it since, or none read it yet in
    // this process) is not read here: it is listed as the cache last held it (a file never read: not listed, its
    // presets none) and its path goes to `unread`, for the Reader. A file this process wrote (saveVersion, savePreset,
    // removePreset, removeVersion, setInfo, add, the migration) or read whole (load(), list(), presetsOf()) is held as
    // it is.
    std::vector<Entry> listCached(std::vector<std::string>& unread);
    std::optional<std::vector<PresetSummary>> presetsCached(std::string_view name, std::vector<std::string>& unread,
                                                            std::string* why = nullptr);

    // Reads the project files listCached and presetsCached could not serve into the cache, on a thread of its own,
    // one at a time, each path asked once until it is read; revision() counts the files it read into the cache, so a
    // caller lists again when it moves. A read keeps what it read only if the file is still as a look before it found
    // it (a file that changed meanwhile is asked for by the next listing; a write's own entry is never replaced by an
    // older read). The process has one (reader()), made when first asked for; it goes, its thread joined, with its last
    // holder. readerParses() counts the files it read whole (of parses()), listings() the listings, cached or not
    // (probes).
    class Reader
    {
    public:
        Reader();
        ~Reader();
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        void request(const std::vector<std::string>& paths);
        uint64_t revision() const noexcept;
        bool idle() const;                               // nothing asked waits or is being read (probes)

    private:
        struct State;
        std::unique_ptr<State> s_;
    };
    std::shared_ptr<Reader> reader();
    int readerParses();
    int listings();
    bool exists(std::string_view name);
    // The project of that name: nothing, with why, when there is none, it is not a project of this plugin's, or its
    // file is damaged. Its name is its file's.
    std::optional<Project> load(std::string_view name, std::string* why = nullptr);

    struct Saved
    {
        int number = 0;                                  // the version's
        bool unchanged = false;                          // the files, the tab and the dials were the latest's: none added
        bool created = false;                            // a new project
        std::string id;                                  // the id in its file now ("" for a project from before ids
                                                         // that this save did not write)
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

    // The project's id (above), made and written into its file when it has none, or when another project's file has
    // the same one and this file is the copy (its name is not the one written in it while the other's is, else it was
    // made later): "" when there is no such project, or its file cannot be written. Whether `id` may be one (a UUID's
    // characters: what a session's or a file's id must be before it names a folder). The project whose file holds
    // `id` now ("" for none), from the listing (a project renamed in the Finder since a session named it).
    std::string idOf(std::string_view name, std::string* why = nullptr);
    std::string idOf(const Project& p, std::string* why = nullptr);     // the same, of a project read already (load())
    bool validProjectId(std::string_view id);
    std::string nameOfId(std::string_view id);

    // A project as the JSON of its file, and back (`name` the project's when the JSON is a file's; nothing, with why, when
    // it is not a project of this plugin's or it is damaged). For ProjectExport, which clears the id first.
    std::string toJson(const Project& p);
    std::optional<Project> fromJson(const std::string& json, std::string_view name, std::string* why = nullptr);
    // A project added under its name, or, when one has it, "<name> 2", "<name> 3", ...: the name it took, or nothing,
    // with why, when it cannot be written. It gets an id of its own, whatever id it came with.
    std::optional<std::string> add(Project p, std::string* why = nullptr);

    // What changed from `before` (none: "first save") to the files and dials given: each file added, removed or edited
    // ("program.fm +12 -3": its lines added and taken away, whatever their order), and how many dials moved.
    std::string describeChange(const Version* before, const Sources& files, const std::vector<Dial>& dials);
}
