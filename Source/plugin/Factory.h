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
#pragma once

#include "plugin/ProjectStore.h"

#include <cstddef>
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
    // The product's factory projects, by name (case aside), each named factoryName(<its own name>); read from what was
    // compiled in once, on first use (any thread), and kept for the process. One that does not read (a damaged JSON,
    // the other kind's, no program) is left out, and problems() says why.
    const std::vector<projectstore::Project>& projects();
    const std::vector<std::string>& problems();
    // The factory project of that name (its factory name or its own, case aside), or null.
    const projectstore::Project* find(std::string_view name);
    std::size_t bytes();                                 // the texts compiled in: what each plugin binary grows by

    // A factory project from its parts: its own name, its project.json and its version's files (name, text), as the
    // folder holds them. Nothing, with why, when it is not one: not a project of this plugin's kind, not one version,
    // a file it names missing, a name ProjectStore would change. The compiled-in ones and the folder's below take it.
    std::optional<projectstore::Project> fromParts(std::string_view own, const std::string& json, const Sources& files,
                                                   std::string* why = nullptr);
    // A factory project's folder on disk, as scripts/import-factory.py writes it (probes).
    std::optional<projectstore::Project> fromFolder(const juce::File& folder, std::string* why = nullptr);
}
