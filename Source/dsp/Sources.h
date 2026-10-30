// Source/dsp/Sources.h: a program's files, the editor's tabs (JUCE-free).
//
// The first file is the program the plugin plays (its main module, Patch.h); the others are files it may include by
// name, `#include "filters.fm"`, as may they each other. FMinus reads a file where its first #include is and never
// again (as if every file began with #pragma once), so two files may both include a third. A name is what the
// messages call the file ("filters.fm:3:12: error: ...").
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace playground
{
    struct SourceFile
    {
        std::string name;
        std::string text;

        friend bool operator==(const SourceFile&, const SourceFile&) = default;
    };

    using Sources = std::vector<SourceFile>;

    inline constexpr std::size_t kMaxFiles = 12;          // the tabs the editor has room for (they narrow to fit)
    inline constexpr std::size_t kMaxNameLength = 24;
    inline constexpr const char* kProgramName = "program.fm";

    // Whether `name` may name a file: 1 to kMaxNameLength letters, digits, '_', '-' and '.', not starting with '.'.
    inline bool validFileName(std::string_view name)
    {
        if (name.empty() || name.size() > kMaxNameLength || name.front() == '.')
            return false;
        for (const char c : name)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'
                         || c == '-' || c == '.';
            if (!ok)
                return false;
        }
        return true;
    }

    // Whether `name` is a circuit netlist (a SPICE-style .cir file, any case: fminus/Circuits.h): the program's
    // #include reads the FMinus module FMinus's circuit compiler generates from it, never its text. Here, apart from
    // Circuits.h, since the language service and the editor (no FMinus) tell the files apart too.
    inline bool isCircuitFile(std::string_view name)
    {
        if (name.size() < 5)
            return false;
        const std::string_view ext = name.substr(name.size() - 4);
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return ext[0] == '.' && lower(ext[1]) == 'c' && lower(ext[2]) == 'i' && lower(ext[3]) == 'r';
    }

    // The file named `name`, or nullptr.
    inline const SourceFile* findFile(const Sources& files, std::string_view name)
    {
        for (const SourceFile& f : files)
            if (f.name == name)
                return &f;
        return nullptr;
    }

    // A name no file has: "file2.fm", "file3.fm", ...
    inline std::string freshFileName(const Sources& files)
    {
        for (int n = 2;; ++n)
        {
            std::string name = "file" + std::to_string(n) + ".fm";
            if (findFile(files, name) == nullptr)
                return name;
        }
    }

    // A program of one file.
    inline Sources singleFile(std::string text, std::string name = kProgramName)
    {
        return { SourceFile{ std::move(name), std::move(text) } };
    }

    // `files` as the editor can show them (a session's, which may be damaged or crafted): at most kMaxFiles, each name
    // valid and none twice (the program's, when it is not, kProgramName; another's a fresh one), the texts as they
    // are. Empty stays empty.
    inline Sources tidyFiles(Sources files)
    {
        if (files.size() > kMaxFiles)
            files.resize(kMaxFiles);
        Sources out;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            SourceFile f = std::move(files[i]);
            if (!validFileName(f.name) || findFile(out, f.name) != nullptr)
                f.name = i == 0 ? std::string(kProgramName) : freshFileName(out);
            out.push_back(std::move(f));
        }
        return out;
    }
}
