// Source/plugin/Factory.h: the factory projects, which come with the plugin: its own sounds, each a project of one
// version (the program's files and its dials) with its presets and its info, read-only.
//
// In the source tree they are factory/<key>/<name>/ (the product's key: factory/synth/, factory/effect/), a folder a
// project, named as the project is: project.json, a project file's JSON (ProjectStore.h) of one version whose files'
// texts are left out, and each of those files beside it, by its name (program.fm, panel.gui, ...), so that a program
// reads (and diffs) as the file it is. CMakeLists.txt compiles every one into the plugin (FactoryProjects.h, the
// texts as they were when it was built); scripts/import-factory.py writes them from a projects folder.
//
// The store (ProjectStore.h) serves them by their factory name (EditorPort.h's factoryName: "Factory: Warm Pad"),
// which no project on this Mac can have: load(), exists() and the presets read them, and every write refuses them (a
// save makes the user's own copy instead: Processor::saveVersion). A user project of the same own name stays the
// user's. The probe plugin.factory compiles and plays every one in both products, its presets too.
//
// The set can change while the plugin runs: the website's (the website repository's docs/factory-projects.md, section
// 6), which plugin/FactoryStore.h fetches, checks and keeps, takes the place of the compiled-in one once the process's
// store has one. So the set is handed out as an immutable snapshot (current()) with a generation number that moves
// with it, never as a reference that lives for the process: a caller holds the snapshot while it uses a project of it,
// and a list made from one (the editor's FACTORY) is made again when generation() moves.
#pragma once

#include "plugin/ProjectStore.h"

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

namespace playground::factory
{
    // A factory set: its projects, by name (case aside), each named factoryName(<its own name>), and what was left out
    // of it with why (the compiled-in set's: a project that does not read). Immutable once made: a new set is a new
    // Set, so a reader holding one never sees it change.
    struct Set
    {
        std::vector<projectstore::Project> projects;
        std::vector<std::string> problems;
        bool fetched = false;                            // from the website (FactoryStore.h), else compiled in
        int64_t sequence = 0;                            // a fetched set's index's
        // The project of that name (its factory name or its own, case aside) in this set, or null.
        const projectstore::Project* find(std::string_view name) const;
    };
    using Snapshot = std::shared_ptr<const Set>;

    // The factory set the plugin shows now: the set fetched from the website when the process's FactoryStore has one
    // (FactoryStore.h), else the one compiled in. Any thread; it never waits on the store (a lock held for a pointer's
    // copy). Hold the snapshot for as long as a project of it is used: a later call may give another one.
    Snapshot current();
    // A number that moves whenever current() would give another set (a set fetched, read from disk, or gone with the
    // last processor), so that a list made from it (the editor's FACTORY, PROJECTS) is made again when it moves.
    uint64_t generation();
    // The set compiled in (read once, on first use, and kept for the process) and what it left out.
    Snapshot builtIn();
    const std::vector<std::string>& problems();
    // The factory project of that name in current(), or null: the pointer keeps its snapshot alive.
    std::shared_ptr<const projectstore::Project> find(std::string_view name);
    std::size_t bytes();                                 // the texts compiled in: what each plugin binary grows by

    // The process's FactoryStore (alone) makes `set` the one current() gives, or with null takes back the one it made
    // (`owner` says whose: one store's set is never taken back by another). Either moves generation().
    void publish(const void* owner, Snapshot set);

    // A factory project from its parts: its own name, its project.json and its version's files (name, text), as the
    // folder holds them. Nothing, with why, when it is not one: not a project of this plugin's kind, not one version,
    // a file it names missing, a name ProjectStore would change. The compiled-in ones and the folder's below take it.
    std::optional<projectstore::Project> fromParts(std::string_view own, const std::string& json, const Sources& files,
                                                   std::string* why = nullptr);
    // A factory project's folder on disk, as scripts/import-factory.py writes it (probes).
    std::optional<projectstore::Project> fromFolder(const juce::File& folder, std::string* why = nullptr);
}
