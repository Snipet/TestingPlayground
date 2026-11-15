// Source/plugin/Factory.h: the factory projects, the plugin's own sounds as the website serves them, each a project of
// one version (the program's files and its dials) with its presets and its info, read-only.
//
// The binary holds none (the website repository's docs/factory-projects.md, decision D4): the set is the one the
// process's FactoryStore (plugin/FactoryStore.h) fetched, checked and kept on this computer, and none until it has one.
// An empty set is a normal state: the FACTORY group then says why, in one row (the store's State).
//
// A set's projects come from the shape scripts/import-factory.py writes and the website's publisher reads: a folder a
// project, named as the project is, holding project.json (a project file's JSON, ProjectStore.h, of one version whose
// files' texts are left out) and each of those files beside it, by its name (program.fm, panel.gui, ...). fromParts
// reads one from its parts (the store, from a downloaded package), fromFolder from such a folder (the probes, and the
// probe plugin.factory, which compiles and plays every project of a folder given to it: README, "Factory projects").
//
// The store (ProjectStore.h) serves them by their factory name (EditorPort.h's factoryName: "Factory: Warm Pad"),
// which no project on this computer can have: load(), exists() and the presets read them, and every write refuses them
// (a save makes the user's own copy instead: Processor::saveVersion). A user project of the same own name stays the
// user's.
//
// The set can change while the plugin runs (a set fetched, read from disk, replaced by a newer one). So it is handed
// out as an immutable snapshot (current()) with a generation number that moves with it, never as a reference that
// lives for the process: a caller holds the snapshot while it uses a project of it, and a list made from one (the
// editor's FACTORY) is made again when generation() moves.
#pragma once

#include "plugin/ProjectStore.h"

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
    // A factory set: its projects, by name (case aside), each named factoryName(<its own name>). Immutable once made: a
    // new set is a new Set, so a reader holding one never sees it change.
    struct Set
    {
        std::vector<projectstore::Project> projects;
        int64_t sequence = 0;                            // its index's
        // The project of that name (its factory name or its own, case aside) in this set, or null.
        const projectstore::Project* find(std::string_view name) const;
    };
    using Snapshot = std::shared_ptr<const Set>;

    // The factory set the plugin shows now: the set the process's FactoryStore has (FactoryStore.h), else an empty
    // one (never null). Any thread; it never waits on the store (a lock held for a pointer's copy). Hold the snapshot
    // for as long as a project of it is used: a later call may give another one.
    Snapshot current();
    // A number that moves whenever current() would give another set (a set fetched, read from disk, or gone with the
    // last processor), so that a list made from it (the editor's FACTORY, PROJECTS) is made again when it moves.
    uint64_t generation();
    // The factory project of that name in current(), or null: the pointer keeps its snapshot alive.
    std::shared_ptr<const projectstore::Project> find(std::string_view name);
    // A set of these projects (each named factoryName(<own>), as fromParts names them), by name, case aside.
    Snapshot setOf(std::vector<projectstore::Project> projects, int64_t sequence = 0);

    // The process's FactoryStore (alone) makes `set` the one current() gives, or with null takes back the one it made
    // (`owner` says whose: one store's set is never taken back by another). Either moves generation(). This is the one
    // way a set reaches current() in a plugin: through the store's checks, with the built-in keys.
    void publish(const void* owner, Snapshot set);

#if defined(FP_PROBES)
    // Probes only (the probe executables alone define FP_PROBES, cmake/Probes.cmake; a plugin has no such call): makes
    // `set` the one current() gives, as a store's would be (Tools/probes/common/FactorySet.h: the made-up projects of
    // Tools/probes/fixtures/factory-set/<key>/), or with null takes it back. Either moves generation(). A store that
    // publishes later replaces it, and takes back only its own.
    void publishForTests(Snapshot set);
#endif

    // A factory project from its parts: its own name, its project.json and its version's files (name, text), as the
    // folder holds them. Nothing, with why, when it is not one: not a project of this plugin's kind, not one version,
    // a file it names missing, a name ProjectStore would change. The folder's below takes it.
    std::optional<projectstore::Project> fromParts(std::string_view own, const std::string& json, const Sources& files,
                                                   std::string* why = nullptr);
    // A factory project's folder on disk, as scripts/import-factory.py writes it (the probes; plugin.factory over a
    // staging folder).
    std::optional<projectstore::Project> fromFolder(const juce::File& folder, std::string* why = nullptr);
}
