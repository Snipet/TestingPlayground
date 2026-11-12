// Source/plugin/ProjectStore.h: the projects on this Mac. A project is a program (the editor's files, the tab shown)
// and its dials' values, under a name, kept as versions (every save adds one, numbered 1, 2, 3, ...: an older one
// opens again), and its presets: dial settings for its program, no code. One file each, in
// ~/Library/Audio/Presets/Funk/<this plugin's name>/<name>.fmproject (Windows: Documents\Funk\<name>\; Linux:
// $XDG_DATA_HOME, else ~/.local/share, /Funk/<name>/), the effect's apart from the synth's (one's programs do not play
// in the other). A file is JSON:
//
//   { "format": "funk-playground-project", "version": 1, "plugin": "<product name>", "kind": "instrument",
//     "name": "Glass Bloom", "id": "8c1f4e2a-...",
//     "versions": [ { "number": 1, "saved": 1761580000000, "note": "first save", "active": 0,
//                     "files": [ { "name": "program.fm", "text": "..." }, ... ],
//                     "dials": [ { "name": "cutoff", "value": 900 }, { "name": "drive", "value": 2, "label": "PITCH" }, ... ],
//                     "circuits": [ { "key": "0123456789abcdef", "text": "fminus-circuit-choice 1 ..." } ],
//                     "midi": [ { "dial": "cutoff", "channel": 1, "cc": 74 }, { "dial": "drive", "channel": 1, "cc": 2,
//                                 "fine": true } ] },
//                   ... ],                                                                     oldest first
//     "presets":  [ { "name": "Bright", "saved": 1761580100000, "from": 1, "category": "Leads", "dials": [ ... ] }, ... ],
//                                                                                                       by name
//     "info":     { "description": "...", "author": "...", "category": "Pad", "tags": [ "warm" ], "genres": [ ... ] } }
//
// A version's "circuits" (optional: an older project has none, and an older build reads past it) are the circuit
// self-test's answers for its netlists (fminus/Circuits.h: a netlist's key and fminus::circuit::Choice::text()), so a
// version opened plays its refined configuration from its first block instead of a provisional one until the test is
// run again; saving the same program again with answers its latest version lacks adds them to that version.
//
// A version's "midi" (optional, written only when there are some: an older project has none, and an older build reads
// past it) are MIDI learn's mappings when it was saved (plugin/MidiLearn.h: a dial by name, a channel, a control, 14
// bits or 7): opening the version makes them the mappings, and a version with none leaves the mappings as they are.
// Mappings other than the latest's are a change: saving adds a version ("MIDI map" in its note), unless they are ones a
// version with none left as they were (VersionMidi::inherited) and the latest has none either.
//
// A version's "samples" (optional, written only when there are some: an older project has none, and an older build
// reads past them) are the samples it plays (dsp/Assets.h: the explorer's ASSETS, what its #pragma sample lines read),
// each by its name and its file's hash; the files are the project's "sampleFiles", each once whichever versions name
// it (its hash and its bytes as base64), so a hundred versions of one drum kit keep one copy of each hit. A save keeps
// only the files its kept versions name; a project's files are kMaxSampleBytes at most in all (a save past it is
// refused, in words). Opening a version makes its samples the plugin's (none: none); samples other than the latest's
// are a change ("kick.wav new", "snare.wav changed", "hat.wav gone" in the note).
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
// A preset's "category" (optional: added later, read past by an older build; none when it has none) is what the top
// bar's list of the presets groups it by (editor/PresetPicker.h); setPresetCategory sets it, and a preset saved over
// one of its name keeps it.
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
// The factory projects (Factory.h: the set fetched from the website once there is one, else the one compiled into the
// plugin; each call reads the set current then) are served here too, by their factory name (EditorPort.h's
// factoryName: "Factory: Warm Pad", which tidyName never leaves, so no file here can have it): load(), exists(),
// presetsOf() and presetsCached() read them, factoryList() lists them (list() and listCached() are this Mac's alone),
// and every write (saveVersion, savePreset, removePreset, removeVersion, remove, setInfo) refuses one, with why. add()
// of a project named so takes its own name.
//
// Any thread; the functions do their own I/O. They hold no state but the folder override and a cache of the listing
// and of each project's presets (a file read again only when its size or its time changes; what this process writes
// or reads whole, it keeps there as it is), and the Reader below.
#pragma once

#include "plugin/MidiLearn.h"

#include "dsp/Assets.h"
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
        friend bool operator==(const Dial&, const Dial&) = default;
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

    // A sample a version plays (above): its name and its file's hash (assets::hashOf); the file is the project's.
    struct SampleRef
    {
        std::string name;
        std::string hash;
        friend bool operator==(const SampleRef&, const SampleRef&) = default;
    };
    // A sample file a project keeps (above), once: its hash and its bytes as added.
    struct SampleFile
    {
        std::string hash;
        std::string bytes;
    };
    inline constexpr std::size_t kMaxSampleBytes = std::size_t{ 64 } << 20;   // a project's sample files, in all: 64 MB
    // The samples of a set as a version names them, by name.
    std::vector<SampleRef> refsOf(const assets::Set* samples);

    struct Version
    {
        int number = 0;
        int64_t savedMs = 0;                             // ms since 1970
        std::string note;                                // what changed since the version before
        Sources files;
        int active = 0;                                  // the tab shown
        std::vector<Dial> dials;
        std::vector<Answer> circuits;                    // the self-test's answers for its netlists (may be none)
        std::vector<midilearn::Mapping> midi;            // MIDI learn's mappings (may be none)
        std::vector<SampleRef> samples;                  // the samples it plays (may be none), by name
    };

    struct Preset
    {
        std::string name;
        int64_t savedMs = 0;
        int from = 0;                                    // the version it was saved with (0: none)
        std::vector<Dial> dials;
        std::string category = {};                       // "" for none (above)
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
        std::vector<SampleFile> samples;                 // the files its versions' samples name, each once

        const Version& latest() const { return versions.back(); }
        const Version* version(int number) const;        // none when it is not kept
        const Preset* preset(std::string_view wanted) const;   // case aside
        const SampleFile* sample(std::string_view hash) const;   // none when it keeps no file of that hash
    };

    struct Entry
    {
        std::string name;
        int64_t savedMs = 0;                             // its latest version's
        int latest = 0;                                  // its latest version's number
        int versions = 0;                                // how many it keeps
        std::vector<std::string> presets;                // their names, by name
        std::string category;                            // its info's: what PROJECTS groups it by ("" for none)
        std::vector<std::string> tags;                   // its info's
        std::string description;                         // its info's (PROJECTS' search reads it)
    };

    // `name` as a project or a preset may have it: printable ASCII but / \ : * ? " < > | and a leading '.', its spaces
    // collapsed and trimmed, at most kMaxName characters; "" when nothing is left.
    std::string tidyName(std::string_view name);
    bool sameName(std::string_view a, std::string_view b);   // case aside

    std::vector<Entry> list();                           // by name, case aside
    std::vector<Entry> factoryList();                    // the factory projects (Factory.h), by name, case aside
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
        std::string category = {};                       // "" for none
        std::vector<Dial> dials = {};                    // what the top bar's cycle matches the dials against
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
    // A project's versions (each its number, when and what changed, oldest first), its presets and its info, from the same
    // cache, for PROJECTS and EXPORT on the message thread: as presetsCached, a file the cache does not hold as it is now
    // goes to `unread` and what the cache last held of it is given, `read` false for one never read (nothing yet); none,
    // with why, for no such project or a damaged one. A factory project's from what is compiled in.
    struct VersionSummary
    {
        int number = 0;
        int64_t savedMs = 0;
        std::string note;
        friend bool operator==(const VersionSummary&, const VersionSummary&) = default;
    };
    struct Detail
    {
        bool read = false;
        std::vector<VersionSummary> versions;
        std::vector<PresetSummary> presets;
        Info info;
    };
    std::optional<Detail> detailCached(std::string_view name, std::vector<std::string>& unread, std::string* why = nullptr);

    // Reads the project files listCached and presetsCached could not serve into the cache, on a thread of its own,
    // one at a time, each path asked once until it is read; revision() counts the files it read into the cache, so a
    // caller lists again when it moves. A read keeps what it read only if the file is still as a look before it found
    // it (a file that changed meanwhile is asked for by the next listing; a write's own entry is never replaced by an
    // older read). The process has one (reader()), made when first asked for; it goes, its thread joined, with its last
    // holder. readerParses() counts the files it read whole (of parses()), listings() the listings, cached or not
    // (probes).
    struct VersionAsk;
    class Reader
    {
    public:
        Reader();
        ~Reader();
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        void request(const std::vector<std::string>& paths);
        // The versions versionCached could not serve (below): each file read once for every version of it asked
        // meanwhile, the listing's entry kept as request()'s read keeps it, the versions into their table.
        void requestVersions(const std::vector<VersionAsk>& asks);
        uint64_t revision() const noexcept;
        bool idle() const;                               // nothing asked waits or is being read (probes)

    private:
        struct State;
        std::unique_ptr<State> s_;
    };
    std::shared_ptr<Reader> reader();
    int readerParses();
    int listings();

    // ---- One version's content, off the message thread (editor/DiffCard.h, the A/B card's sides) -------------------
    // A version as a caller that must not wait while a project's file is read whole sees it: its files, its dials (with
    // their values' names), its samples by name and hash (never their bytes), its note and when it was saved. From a
    // table of its own, the newest kKeptVersions asked (by the file's path, its time and size, and the number asked), so
    // the card that shows two sides, and asks for them every frame, finds them there; a file the table does not hold as
    // it is now goes to `unread`, for the Reader's requestVersions, and `read` is false until it has (its revision then
    // moves, as for a listing). `number` 0 is the newest. `missing`: no such version in the file (or the file is no
    // project of this plugin's). None, with why, when there is no project of that name. A factory project's at once,
    // from the factory set (Factory.h).
    inline constexpr std::size_t kKeptVersions = 6;
    struct VersionContent
    {
        int number = 0;                                  // the version's own number (the newest's, for 0 asked)
        int64_t savedMs = 0;
        std::string note;
        Sources files;
        int active = 0;
        std::vector<Dial> dials;
        std::vector<SampleRef> samples;
    };
    struct VersionRead
    {
        bool read = false;                               // false: the Reader reads it now
        bool missing = false;
        VersionContent content;
    };
    struct VersionAsk
    {
        std::string path;                                // the project's file
        int number = 0;                                  // as asked (0: the newest)
    };
    std::optional<VersionRead> versionCached(std::string_view name, int number, std::vector<VersionAsk>& unread,
                                             std::string* why = nullptr);
    std::size_t versionsKept();                          // how many the table holds now (probes)
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
    // MIDI learn's mappings for a new version (Version::midi). `inherited`: they came from before the version open now,
    // which had none (opening it left them), and are as they were when it opened: while the project's latest version
    // has none either, they are no change of their own (saving that version as it was adds no version), though a
    // version saved for another change keeps them.
    struct VersionMidi
    {
        std::vector<midilearn::Mapping> mappings;
        bool inherited = false;
    };
    // A new version of the project `name` (tidied; a new project when there is none), its note what changed since the
    // latest, with the self-test's answers for its netlists (`circuits`: at most kMaxAnswers kept; when the files, the
    // tab, the dials, MIDI learn's mappings and the samples are the latest's, answers it lacks are added to it and no
    // version is), and the samples it plays (`samples`: their files kept in the project, above). Nothing, with why, when
    // it cannot be written (or the project's sample files would pass kMaxSampleBytes). `lead` goes before the note's
    // words, the change after it in brackets ("Claude: brighter (program.fm +12 -3)"): who saved it and why, as noteLead
    // tidies it.
    std::optional<Saved> saveVersion(std::string_view name, const Sources& files, int active, const std::vector<Dial>& dials,
                                     const std::vector<Answer>& circuits = {}, const VersionMidi& midi = {},
                                     const assets::Set* samples = nullptr, std::string* why = nullptr,
                                     std::string_view lead = {});
    // A version note's words as a project keeps them: printable, one line, its spaces collapsed, at most kMaxLead
    // characters (cut at a word, with "..."); "" when nothing is left.
    inline constexpr std::size_t kMaxLead = 160;
    std::string noteLead(std::string_view words);
    // A preset of the project (over one of that name, case aside, which takes the new name's case and keeps its
    // category).
    bool savePreset(std::string_view project, std::string_view preset, const std::vector<Dial>& dials, int from,
                    std::string* why = nullptr);
    bool removePreset(std::string_view project, std::string_view preset, std::string* why = nullptr);
    // A preset's category alone (tidied as a project's is; "" for none): one read, one write. Nothing, with why, when
    // the project or the preset is not there (a factory project's: read-only).
    bool setPresetCategory(std::string_view project, std::string_view preset, std::string_view category,
                           std::string* why = nullptr);
    std::string tidyCategory(std::string_view category);   // as tidyInfo tidies a project's
    bool removeVersion(std::string_view project, int number, std::string* why = nullptr);   // never its only one
    bool remove(std::string_view name, std::string* why = nullptr);

    // The info as a project keeps it: printable text, cut to its lengths (a tag to kMaxTag, at most kMaxTags of them,
    // each once case aside, as the genres), the spaces collapsed.
    Info tidyInfo(Info info);
    std::vector<std::string> splitTags(std::string_view commaSeparated);   // "warm, analog,, Warm" -> warm, analog
    bool setInfo(std::string_view project, const Info& info, std::string* why = nullptr);
    // Its category alone (tidied as tidyInfo does; "" for none), the rest of its info as it is: one read, one write.
    bool setCategory(std::string_view project, std::string_view category, std::string* why = nullptr);

    // How PROJECTS shows the projects (editor/ProjectBrowser.h, editor/ProjectGroups.h): the projects starred (by name,
    // a factory one's as factoryName spells it), the groups folded (ProjectGroups' keys), the search typed, the tag
    // filtered on and the order. The folder's own, so every instance of this plugin on the Mac (and a probe, in its
    // folder) shares it: one small file there, kViewFile (hidden: no project's name starts with a '.'), written whole
    // (beside, then moved over) and read again only when its time or its size changes. None, or one that does not
    // read: the default (nothing starred or folded, no filter, by name). remove() unstars the project it deletes.
    inline constexpr const char* kViewFile = ".projects-view.json";
    inline constexpr std::size_t kMaxFavourites = 500, kMaxFolded = 100, kMaxSearch = 60;
    struct View
    {
        std::vector<std::string> favourites;             // case aside, each once
        std::vector<std::string> folded;
        std::string search;                              // printable, at most kMaxSearch characters
        std::string tag;                                 // "" for any
        bool byDate = false;                             // the newest save first, else by name
        friend bool operator==(const View&, const View&) = default;
    };
    View view();
    bool setView(const View& v, std::string* why = nullptr);

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
    // ("program.fm +12 -3": its lines added and taken away, whatever their order), each sample given added, changed or
    // gone ("kick.wav new"), how many dials moved, and, given MIDI learn's mappings, "MIDI map" when they are not
    // `before`'s.
    std::string describeChange(const Version* before, const Sources& files, const std::vector<Dial>& dials,
                               const std::vector<midilearn::Mapping>* midi = nullptr,
                               const std::vector<SampleRef>* samples = nullptr);
    // Why a project's sample files do not fit (kMaxSampleBytes), in words; "" when they do.
    std::string samplesRefusal(const Project& p);
}
