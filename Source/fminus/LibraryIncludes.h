// Source/fminus/LibraryIncludes.h: a program's files with the library files its #includes name (JUCE-free, FMinus-free;
// the product's generated Library.h). An example written against the library (one that does #include "screamer.cir" or
// "osc.fm") compiles only with those files beside it, as LIBRARY adds them: the editor's EXAMPLES (Panel::loadExample),
// the examples in Claude's instructions (ChatPrompt.cpp) and the probes that play the examples take its files from here.
#pragma once

#include "Library.h"
#include "dsp/Sources.h"
#include "fminus/Lang.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace playground
{
    // `files`, then each library file an #include of theirs names and no file of theirs has (and what those include in
    // turn), in the order first named: what the program needs to compile as it is. A name the library lacks is left to
    // FMinus ("'x.fm' file not found"), and host.fm to the plugin, which provides it.
    inline Sources withLibraryIncludes(Sources files)
    {
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            const std::string text = files[i].text;          // a copy: push_back below may move the files
            for (const std::string_view line : lang::splitLines(text))
            {
                std::string name;
                int from = 0, to = 0;
                if (!lang::includeOn(line, name, from, to) || findFile(files, name) != nullptr)
                    continue;
                if (const LibraryFile* f = findLibraryFile(name))
                    files.push_back({ f->name, f->source });
            }
        }
        return files;
    }
}
