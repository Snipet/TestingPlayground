// Source/plugin/ProjectTree.h: one version of a project (ProjectStore.h) as a folder of ordinary files, the layout of a
// linked project's git repository (plugin/GitProjects.h), format "funk-playground-git", version 1:
//
//   project.json     the format, plugin, kind, name; the info; the files' order and the tab shown; the dials (name,
//                    value, label); the presets; MIDI learn's mappings; the circuits' answers; the samples as
//                    {name, file}. One item a line, so that two machines' versions that change different things touch
//                    different lines (a merge of them is clean: GitProjects.h). No version's number, time or note: those
//                    are the commit's (its message, its date and its Playground-Version trailer), and two sides that
//                    both saved would otherwise always change the same lines
//   program.fm, ...  the version's files, as named
//   samples/<name>   each sample it plays, its file as it was added
//   README.md        written once at Connect when there is none; never rewritten
//   .gitattributes   "* text=auto eol=lf" and "samples/* binary", written once
//
// The project's id is left out, as exports leave it out: a clone is a project of its own on another machine. Any
// thread; no state. Nothing here runs git: read() takes a getter (a folder's files, or a commit's through git show).
#pragma once

#include "plugin/ProjectStore.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace playground::projecttree
{
    inline constexpr const char* kFormat = "funk-playground-git";
    inline constexpr int kVersion = 1;
    inline constexpr const char* kManifest = "project.json";
    inline constexpr const char* kReadme = "README.md";
    inline constexpr const char* kAttributes = ".gitattributes";
    inline constexpr std::size_t kMaxTextBytes = std::size_t{ 4 } << 20;     // a program's file, read back
    inline constexpr std::size_t kMaxManifestBytes = std::size_t{ 4 } << 20;

    // One version read back from a tree: everything a version is but its number, time and note (the caller's: the
    // commit's), the sample files it names, and the project's name, info and presets as the tree has them.
    struct Tree
    {
        std::string name;
        projectstore::Info info;
        std::vector<projectstore::Preset> presets;
        projectstore::Version version;                   // number 0, savedMs 0, note ""
        std::vector<projectstore::SampleFile> samples;   // each file its refs name, once
    };

    // The files `v`'s tree is made of, relative, '/' between parts: project.json first, then its files in order, then
    // samples/<name> for each sample. README.md and .gitattributes are not among them (written once, never removed).
    std::vector<std::string> filesOf(const projectstore::Version& v);

    // The tree's files and their bytes, in filesOf's order. False, with why, when a sample the version names is not in
    // the project, or a name cannot be a path.
    struct File
    {
        std::string path;
        std::string bytes;
    };
    bool render(const projectstore::Project& p, const projectstore::Version& v, std::vector<File>& out,
                std::string* why = nullptr);

    // `v`'s tree written into `folder` (a real folder, not a link): each file written beside its place and renamed in;
    // README.md and .gitattributes made when they are not there. Then each file `before` lists (what the plugin's own
    // last write of this project wrote, or read back as its tree) that this tree has not is removed, and nothing else:
    // never by a pattern, never a file the plugin did not write, never a link (a path that is one, or passes through
    // one, is refused before anything is written). `written` gets filesOf(v). False, with why, when it cannot be.
    bool write(const projectstore::Project& p, const projectstore::Version& v, const std::string& folder,
               const std::vector<std::string>& before, std::vector<std::string>& written, std::string* why = nullptr);

    // A tree's files, from wherever they are: the bytes at `path`, or none, with why.
    using Getter = std::function<std::optional<std::string>(const std::string& path, std::string* why)>;
    Getter folderGetter(const std::string& folder);
    // The tree read back: none, with why, when it is no project of this plugin's kind (no project.json, another format,
    // the other kind's), or a file it names is missing or cannot be one.
    std::optional<Tree> read(const Getter& get, std::string* why = nullptr);
}
