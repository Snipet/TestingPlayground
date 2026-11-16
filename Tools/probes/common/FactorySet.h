// Tools/probes/common/FactorySet.h: the probes' factory set. A plugin holds no factory projects (plugin/Factory.h): its
// set is the one its store fetched and checked, and none until then. A probe that wants factory projects to show
// publishes a made-up set through Factory.h's publishForTests, which exists only where FP_PROBES is defined (the probe
// executables): the projects of Tools/probes/fixtures/factory-set/<key>/, in the shape scripts/import-factory.py writes
// and the website's publisher reads (a folder a project: project.json and its version's files beside it).
//
//   probe::factoryFolder(dir)   the projects of <dir>/<key>/, each read with factory::fromFolder (a folder that does
//                               not read is named in `problems`), as a set
//   probe::factorySet()         the made-up set of the tree's fixtures (Tools/probes/fixtures/factory-set/)
//   probe::useFactorySet(on)    makes it current() (on), or takes it back (off: no set, as a plugin with none)
#pragma once

#include "plugin/Factory.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace probe
{
    inline juce::File factoryFixtures()
    {
        return juce::File(PROBE_SOURCE_DIR).getChildFile("Tools/probes/fixtures/factory-set");
    }

    // The projects of <dir>/<key>/ (a staging folder, as the import writes it), by name.
    inline playground::factory::Snapshot factoryFolder(const juce::File& dir, std::vector<std::string>* problems = nullptr)
    {
        std::vector<playground::projectstore::Project> projects;
        juce::Array<juce::File> folders = dir.getChildFile(playground::product::kKey).findChildFiles(juce::File::findDirectories, false);
        folders.sort();
        for (const juce::File& f : folders)
        {
            std::string why;
            if (auto p = playground::factory::fromFolder(f, &why))
                projects.push_back(std::move(*p));
            else if (problems != nullptr)
                problems->push_back(f.getFileName().toStdString() + ": " + why);
        }
        return playground::factory::setOf(std::move(projects));
    }

    inline playground::factory::Snapshot factorySet(std::vector<std::string>* problems = nullptr)
    {
        return factoryFolder(factoryFixtures(), problems);
    }

    inline void useFactorySet(bool on = true)
    {
        playground::factory::publishForTests(on ? factorySet() : nullptr);
    }
}
