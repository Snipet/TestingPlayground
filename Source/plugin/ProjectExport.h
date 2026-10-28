// Source/plugin/ProjectExport.h: a project (ProjectStore.h) out of this Mac and back in, in two forms:
//
//   source   a .zip of its code, to read, edit and keep anywhere:
//              <Name>/README.md            what it is (its info), its files, its presets, how to open it again
//              <Name>/project.json         the manifest: the info, every version's number, date, note, tab and dials
//                                          (and where its files are), the presets
//              <Name>/<file>               the newest version's files (program.fm first)
//              <Name>/versions/v<N>/<file> each older version's, when every version goes
//   package  a .fmpack: the project file's JSON (ProjectStore.h: versions, presets, info), compressed, in one file:
//              bytes 0-3   "FMPK"
//              byte 4      the package format (1)
//              byte 5      how the rest is compressed: 0 stored, 1 zlib (deflate), 2 xz (LZMA2)
//              bytes 6-7   0
//              bytes 8-15  the JSON's size in bytes, little-endian
//              bytes 16-19 its CRC-32, little-endian
//              byte 20...  the JSON, compressed
//            On macOS it is xz, by Apple's Compression framework (COMPRESSION_LZMA writes the .xz format, so `tail -c +21
//            Name.fmpack | xz -d` prints the JSON): a project's versions are mostly the same text, which LZMA's window
//            takes as repeats. Elsewhere zlib (JUCE's), and a package in xz is refused there with a why.
//
// Either imports as a new project (ProjectStore::add: "<name> 2" when the name is taken), its versions, presets and info
// with it; the kind is checked (a synth's export does not open in the effect). Any thread; no state.
#pragma once

#include "plugin/ProjectStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace juce
{
    class File;
}

namespace playground::projectexport
{
    inline constexpr const char* kSourceExtension = ".zip";
    inline constexpr const char* kPackageExtension = ".fmpack";
    inline constexpr int kPackageFormat = 1;
    inline constexpr int kHeaderBytes = 20;

    enum class Method : uint8_t { stored = 0, zlib = 1, xz = 2 };

    struct Result
    {
        int64_t bytes = 0;                               // the file written
        int64_t raw = 0;                                 // what it holds, uncompressed
        Method method = Method::stored;                  // a package's
        int versions = 0;                                // how many went
    };

    // The project written to `to` (made or replaced): every version, or with !allVersions the newest only (numbered as
    // it is). False, with why, when it cannot be.
    bool writeSource(const projectstore::Project& p, bool allVersions, const juce::File& to, Result& out, std::string* why = nullptr);
    bool writePackage(const projectstore::Project& p, bool allVersions, const juce::File& to, Result& out, std::string* why = nullptr);

    // The project in a .zip or a .fmpack this plugin (or the other playground: refused, with why) exported.
    std::optional<projectstore::Project> read(const juce::File& from, std::string* why = nullptr);

    // The package's body, for probes: the bytes after the header, and back.
    std::vector<uint8_t> compress(const std::string& raw, Method& method);
    std::optional<std::string> decompress(const uint8_t* data, std::size_t size, Method method, std::size_t rawSize,
                                          std::string* why = nullptr);
    uint32_t crc32(const void* data, std::size_t size);
}
