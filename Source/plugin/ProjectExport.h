// Source/plugin/ProjectExport.h: a project (ProjectStore.h) out of this Mac and back in, in two forms:
//
//   source   a .zip of its code, to read, edit and keep anywhere:
//              <Name>/README.md            what it is (its info), its files, its presets, how to open it again
//              <Name>/project.json         the manifest: the info, every version's number, date, note, tab and dials
//                                          (and where its files are), the presets
//              <Name>/<file>               the newest version's files (program.fm first)
//              <Name>/versions/v<N>/<file> each older version's, when every version goes
//              <Name>/samples/<file>       the samples' files (ProjectStore.h) the versions that go name, each once,
//                                          under the name the newest of them gives it, stored as they are
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
//   bundle   several projects at once (PROJECTS' Export with more than one chosen): one .zip of stored entries, each
//            project's own export as above (all .fmpack or all source .zip) under its own name, and a manifest:
//              bundle.json         { "format": "funk-playground-bundle", "version": 1, "plugin": "<product name>",
//                                    "kind": "instrument", "projects": [ { "name": "Glass Bloom",
//                                    "file": "Glass Bloom.fmpack" }, ... ] }
//              <Name>.fmpack       (or <Name>.zip) each project's export, named by its own name (a factory project's
//                                  without "Factory: "), made unique case aside ("Pad.fmpack", "Pad 2.fmpack": the
//                                  Mac's default disk tells no case apart)
//            Unzipped by hand it is a folder of ordinary exports, each importable alone. readAll() gives its projects
//            (each read as its file would be, the other kind's refused one by one); read() refuses a bundle, saying so.
//            At most kMaxBundleProjects projects; the caller (Processor's export job) keeps it under kMaxBundleBytes of
//            project files.
//
// Either imports as a new project (ProjectStore::add: "<name> 2" when the name is taken, a new id), its versions,
// presets and info with it; the kind is checked (a synth's export does not open in the effect). Neither holds the
// project's chats nor its id (ProjectStore.h): the chats stay on this Mac (ChatStore.h), and the id, which names their
// folder here, is left out, so a file sent elsewhere carries nothing of them (the source zip's README says so; a factory
// project made from a user's file must leave it out too). Any thread; no state.
#pragma once

#include "plugin/ProjectStore.h"

#include <cstdint>
#include <functional>
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

    // The project written to `to` (made, or with `replace` replaced; without it a file there is left as it is and the
    // write refused: place()): every version, or with !allVersions the newest only (numbered as it is). False, with why,
    // when it cannot be. sourceBytes and packageBytes make the same file's bytes in memory.
    bool writeSource(const projectstore::Project& p, bool allVersions, const juce::File& to, Result& out, std::string* why = nullptr,
                     bool replace = true);
    bool writePackage(const projectstore::Project& p, bool allVersions, const juce::File& to, Result& out, std::string* why = nullptr,
                      bool replace = true);
    bool sourceBytes(const projectstore::Project& p, bool allVersions, std::vector<uint8_t>& bytes, Result& out,
                     std::string* why = nullptr);
    bool packageBytes(const projectstore::Project& p, bool allVersions, std::vector<uint8_t>& bytes, Result& out,
                      std::string* why = nullptr);

    // `temp` moved to `to` in one step: over a file there only when `replace`; else one there (made meanwhile) is left as
    // it is and the move refused, with why (renameat2's RENAME_NOREPLACE on Linux, renamex_np's RENAME_EXCL on macOS,
    // MoveFileExW without replacing on Windows; a hard link where the file system takes neither).
    bool place(const juce::File& temp, const juce::File& to, bool replace, std::string* why = nullptr);

    // The project in a .zip or a .fmpack this plugin (or the other playground: refused, with why) exported. A bundle is
    // refused (readAll reads it).
    std::optional<projectstore::Project> read(const juce::File& from, std::string* why = nullptr);

    // The bundle (above).
    inline constexpr const char* kBundleFormat = "funk-playground-bundle";
    inline constexpr const char* kBundleManifest = "bundle.json";
    inline constexpr int kBundleVersion = 1;
    inline constexpr int kMaxBundleProjects = 256;
    inline constexpr int64_t kMaxBundleBytes = int64_t{ 2 } << 30;   // the project files a bundle is made from, in all
    // The file each project named takes in a bundle (its own name, made unique case aside), in their order.
    std::vector<std::string> bundleNames(const std::vector<std::string>& names, bool package);
    struct BundleProgress
    {
        std::function<bool()> cancelled;                 // asked before each project and before the file is placed
        std::function<void(const std::string& name, bool ok, const std::string& why)> step;   // after each project
    };
    struct BundleResult
    {
        Result file;                                     // its bytes, what its exports hold, the versions in all
        int projects = 0;                                // in it
        std::vector<std::string> failed;                 // "<name>: <why>", each left out
        bool cancelled = false;
    };
    using Loader = std::function<std::optional<projectstore::Project>(const std::string& name, std::string* why)>;
    // The projects `names` (each read by `load`, one at a time, on the caller's thread) written as a bundle to `to`,
    // through a temporary beside it, then place()d (`replace` as there); a project that cannot be read or exported is
    // left out (BundleResult::failed). False, with why, when none went, it was cancelled (no file left) or the file
    // cannot be written.
    bool writeBundle(const std::vector<std::string>& names, const Loader& load, bool package, bool allVersions, const juce::File& to,
                     bool replace, const BundleProgress& progress, BundleResult& out, std::string* why = nullptr);
    bool isBundle(const juce::File& from);
    // What a file brings in: a bundle's projects, each read or refused with why (its manifest's name and file), or a
    // single export's one project. Nothing, with why, when the file is neither.
    struct Item
    {
        std::string name;
        std::string file;
        std::optional<projectstore::Project> project;
        std::string why;
    };
    std::optional<std::vector<Item>> readAll(const juce::File& from, std::string* why = nullptr);

    // The package's body, for probes: the bytes after the header, and back.
    std::vector<uint8_t> compress(const std::string& raw, Method& method);
    std::optional<std::string> decompress(const uint8_t* data, std::size_t size, Method method, std::size_t rawSize,
                                          std::string* why = nullptr);
    uint32_t crc32(const void* data, std::size_t size);
}
