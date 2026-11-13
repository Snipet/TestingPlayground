// PROBE name=plugin.git timeout=300
//
// plugin.git: the user's git run over a folder (plugin/Git.h) and the runner under it (plugin/Run.h). Everything
// happens in the probe's sandbox (<prefix>PREFS_DIR/git): the remotes are bare repositories there, reached as file://
// addresses (GIT_ALLOW_PROTOCOL gains file only because the probe's Context asks), git's own configuration is files of
// the probe's (GIT_CONFIG_GLOBAL, GIT_CONFIG_SYSTEM, HOME and XDG_CONFIG_HOME pointed into the sandbox), and the
// committer is set by environment: the user's configuration, credentials and remotes are never read, and nothing
// reaches a network. git is <prefix>GIT (cmake/Probes.cmake: CMake's GIT_EXECUTABLE); with none, every row is skipped,
// saying so.
//   RUN      (POSIX: /bin/sh as the child) stdout and stderr captured apart and the exit code; the environment's
//            changes and the working folder; stdout cut at its limit (its first bytes) and stderr (its last), the child
//            drained to its end; a timeout and a cancel each kill it and what it started; a descriptor the process left
//            inheritable never reaches the child; an argument holding $(...) reaches it as it is; a program not found
//            said
//   COMMIT   commits in order, each with its message, its date and its trailer; a message's body kept; nothing to
//            commit refused when empty commits are not asked for; no identity: the words that say how to set it
//   PUSH     the first push makes the remote's branch; what is new goes and nothing else (the remote's count);
//            nothing new is fine; a push from behind is refused as `behind` and the remote keeps its commit
//   PULL     fetch with nothing new: counts 0/0, the fast-forward changes nothing; something new: behind 1, the
//            fast-forward brings its file; both sides moved: counted, and the fast-forward refused, HEAD unchanged
//   CLONE    a clone has the remote's commits and files; into a folder holding a file: refused, the file kept; a
//            file:// address without the probe's leave refused before git runs
//   ADDRESS  addressRefusal: https, ssh and user@host:path taken; ext::, a leading dash, a control character, a space,
//            another scheme, a plain path refused; through the helpers nothing is run (no marker, no remote added);
//            git itself, given an ext:: address directly, refuses it (GIT_ALLOW_PROTOCOL, protocol.ext.allow)
//   HOOKS    (POSIX) a repository's own hooks and the global core.hooksPath's do not run for commit or push, where
//            plain git (the control) runs them
//   NOGIT    find() of a path that is not there, of a file that is no git and of a program that is no git: none
//            found, said; with no git the helpers refuse in words and make nothing; find() of the probe's git: its
//            version
// Then the projects' git (plugin/GitProjects.h), with two store folders standing for two machines (A and B) and a bare
// repository of the probe's as their remote:
//   TREE     a version written as a repository's tree and read back the same (files, tab, dials with their labels,
//            circuits' answers, MIDI map, samples, presets, info); its layout (project.json without id or texts, README,
//            .gitattributes, samples/); a file the user added and an edited README kept, a file the version dropped gone;
//            a file missing: refused
//   CONNECT  a remote with commits refused (no folder, no link); an ext:: address refused; a commit a version, oldest
//            first, with its note, its save's date and its trailer; linked, listed with its remote; twice refused
//   PUSH     a version saved since: listed one ahead; only it pushed; nothing new fine; a remote that moved: refused as
//            behind, the remote's branch as it was (never forced)
//   CLONE    (B) a project of its own (another id), linked; a taken name numbered; the other kind's refused, nothing
//            added, no clone left
//   PULL     a fast-forward: a new version ("Pulled ..."), the link's commit moved; nothing new; not committed again;
//            both sides moved: refused as diverged, nothing changed
//   CHECK    the counts, as the listing shows them
//   LINKS    the file's format; written and read back the same; a rename in the file manager keeps the link; the listing
//            reads no repository (the working folder moved away); a working folder gone: said; no address in the project
//   MERGE    (05c) both sides moved: the pull's merge taken back whole (HEAD, the folder and the versions as they were),
//            the link marked diverged; their side read for Compare with nothing written; Keep mine (theirs a version,
//            mine on top, pushed: the remote's program mine), Take theirs (theirs the newest, mine the version before,
//            a merge commit pushed); with nothing to settle, refused
//   JOB      the Processor's git thread: a pull of the project open and unedited opens the new version; edited, it waits
//            and the typing stays; a git path that is no git: said, nothing else changed; Disconnect: the link gone,
//            the folder kept and named
#include "Probe.h"

#include "plugin/Git.h"
#include "plugin/GitProjects.h"
#include "plugin/Processor.h"
#include "plugin/ProjectExport.h"
#include "plugin/ProjectStore.h"
#include "plugin/Run.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif
#include <unistd.h>

namespace pg = playground;
namespace fs = std::filesystem;

namespace
{
    void writeFile(const fs::path& p, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f << text;
    }

    std::string readFile(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    bool there(const fs::path& p)
    {
        std::error_code ec;
        return fs::exists(p, ec);
    }

    // ---- The projects' git (plugin/GitProjects.h, ProjectExport.h's tree, ProjectStore.h's links, ProcessorGit.cpp) -----
    // Two store folders stand for two machines (A and B); the remote is a bare repository of the probe's.
    std::string sh(const pg::git::Found& found, const fs::path& repo, const std::vector<std::string>& args)
    {
        pg::git::Context c;
        c.git = found.path;
        c.folder = repo.string();
        c.allowFile = true;
        const pg::git::Outcome o = pg::git::call(c, args);
        return o.ok ? o.out : std::string();
    }

    pg::Sources programOf(const std::string& tone)
    {
        const std::string params = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        const std::string program = pg::product::kIsInstrument
            ? "// " + tone + "\nmodule Voice\n{\n" + params
                  + "    bool held;\n    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n) { return held; }\n};\n"
            : "// " + tone + "\nmodule Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        return { { pg::kProgramName, program }, { "lib.fm", "// " + tone + " lib\nfloat half(float x) { return 0.5f * x; }\n" } };
    }

    void projectSections(::funkgui::test::Probe& P, const pg::git::Found& found, const fs::path& root, const std::function<std::string(const fs::path&)>& fileUrl)
    {
        namespace ps = pg::projectstore;
        namespace gp = pg::gitprojects;
        std::error_code ec;
        const fs::path machineA = root / "machine-a", machineB = root / "machine-b";
        const auto onA = [&] { ps::setDirectoryForTests(juce::File(juce::String(machineA.string()))); };
        const auto onB = [&] { ps::setDirectoryForTests(juce::File(juce::String(machineB.string()))); };
        gp::Setup setup;
        setup.git = found.path;
        setup.allowFile = true;
        const fs::path remote = root / "remotes" / "project.git";
        fs::create_directories(remote, ec);
        sh(found, remote, { "init", "--bare", "-q" });
        sh(found, remote, { "symbolic-ref", "HEAD", "refs/heads/main" });
        const std::string url = fileUrl(remote);
        const auto remoteCount = [&] { return std::atoi(sh(found, remote, { "rev-list", "--count", "main" }).c_str()); };
        const std::vector<ps::Dial> dials{ { "level", 0.25f, "" }, { "mode", 2.0f, "PITCH" } };

        // ---- TREE ------------------------------------------------------------------------------------------------------
        onA();
        {
            auto set = std::make_shared<pg::assets::Set>();
            set->items.push_back(std::make_shared<const pg::assets::Asset>("kick.wav", std::string("RIFF\0\0\0\0WAVEfake", 16), pg::assets::Audio{}));
            const std::optional<ps::Saved> s1 = ps::saveVersion("Pad", programOf("one"), 1, dials, { { "0123456789abcdef", "fminus-circuit-choice 1 x" } },
                                                                 { { { "level", 1, 74, false } }, false }, set.get());
            ps::savePreset("Pad", "Bright", { { "level", 0.9f, "" } }, 1);
            ps::Info info;
            info.description = "A pad for the probe.";
            info.category = "Pad";
            ps::setInfo("Pad", info);
            const std::optional<ps::Project> p = ps::load("Pad");
            const fs::path tree = root / "tree";
            std::string why;
            const bool wrote = p && pg::projectexport::writeTree(*p, 1, juce::File(juce::String(tree.string())), &why);
            std::printf("tree: wrote %d (%s)\n", wrote ? 1 : 0, why.c_str());
            const std::optional<ps::Project> back = pg::projectexport::readTree(juce::File(juce::String(tree.string())), &why);
            bool same = wrote && back && back->versions.size() == 1 && s1.has_value();
            if (same)
            {
                const ps::Version& a = p->latest();
                const ps::Version& b = back->latest();
                same = a.files == b.files && a.dials == b.dials && a.active == b.active && a.circuits == b.circuits && a.midi == b.midi
                    && a.samples == b.samples && back->presets.size() == 1 && back->presets[0].name == "Bright"
                    && back->info == p->info && back->sample(a.samples.at(0).hash) != nullptr
                    && back->sample(a.samples.at(0).hash)->bytes == p->sample(a.samples.at(0).hash)->bytes;
            }
            P.eq("tree.reads_back", same ? 1 : 0, 1);
            const std::string manifest = readFile(tree / "project.json");
            P.eq("tree.layout", there(tree / "program.fm") && there(tree / "lib.fm") && there(tree / "samples" / "kick.wav")
                                    && there(tree / "README.md") && there(tree / ".gitattributes")
                                    && manifest.find("\"funk-playground-git\"") != std::string::npos
                                    && manifest.find("\"id\"") == std::string::npos && manifest.find("float half") == std::string::npos
                                    && manifest.find("// one") == std::string::npos
                                    && manifest.find("PITCH") != std::string::npos ? 1 : 0, 1);
            // A file the user put in the repository stays; a file the version no longer has goes; README stays as edited.
            writeFile(tree / "NOTES.txt", "mine\n");
            writeFile(tree / "README.md", "edited\n");
            ps::Project q = *p;
            q.versions.back().files = { { pg::kProgramName, programOf("two").front().text } };
            pg::projectexport::writeTree(q, 1, juce::File(juce::String(tree.string())), &why);
            P.eq("tree.keeps_others", there(tree / "NOTES.txt") && !there(tree / "lib.fm") && readFile(tree / "README.md") == "edited\n"
                                          ? 1 : 0, 1);
            fs::remove_all(tree / "program.fm", ec);
            P.eq("tree.missing_file_refused", !pg::projectexport::readTree(juce::File(juce::String(tree.string())), &why) && !why.empty() ? 1 : 0, 1);
        }

        // ---- CONNECT ---------------------------------------------------------------------------------------------------
        {
            ps::saveVersion("Pad", programOf("two"), 0, dials);
            ps::saveVersion("Pad", programOf("three"), 0, dials);
            // A remote with commits already: refused, no folder left, no link.
            const fs::path full = root / "remotes" / "full.git";
            fs::create_directories(full, ec);
            sh(found, full, { "init", "--bare", "-q" });
            const fs::path seed = root / "seed";
            pg::git::Context sc;
            sc.git = found.path;
            sc.allowFile = true;
            pg::git::clone(sc, fileUrl(full), seed.string());
            writeFile(seed / "x.txt", "x\n");
            sc.folder = seed.string();
            pg::git::commitAll(sc, "seed", 0, {});
            sh(found, seed, { "push", "-q", "origin", "HEAD:main" });
            const gp::Result refused = gp::connect(setup, "Pad", fileUrl(full), "main");
            std::printf("connect: non-empty says \"%s\"\n", refused.why.c_str());
            std::size_t gitFolders = 0;
            for (const auto& e : fs::directory_iterator(machineA / "Git", ec))
                gitFolders += e.is_directory() ? 1 : 0;
            P.eq("connect.non_empty_refused", !refused.ok && refused.why.find("not empty") != std::string::npos && gitFolders == 0
                                                  && ps::links().links.empty() ? 1 : 0, 1);
            const gp::Result bad = gp::connect(setup, "Pad", "ext::sh -c touch% /tmp/x", "main");
            P.eq("connect.address_refused", !bad.ok && ps::links().links.empty() ? 1 : 0, 1);

            const gp::Result r = gp::connect(setup, "Pad", url, "main");
            std::printf("connect: ok %d \"%s\" (%s)\n", r.ok ? 1 : 0, r.line.c_str(), r.why.c_str());
            const std::optional<ps::Project> p = ps::load("Pad");
            const std::optional<ps::Link> k = p ? ps::linkOf(p->id) : std::nullopt;
            P.eq("connect.linked", r.ok && k && k->committed == 3 && k->branch == "main" && k->ahead == 0 && k->behind == 0
                                       && k->log.size() == 3 && remoteCount() == 3 ? 1 : 0, 1);
            const std::string log = sh(found, remote, { "log", "--reverse", "--format=%s%x01%at%x01%(trailers:key=Playground-Version,valueonly)%x02", "main" });
            bool order = p.has_value();
            std::size_t at = 0;
            for (int n = 1; order && n <= 3; ++n)
            {
                const ps::Version* v = p->version(n);
                const std::string want = gp::messageOf(n, v->note) + "\x01" + std::to_string(v->savedMs / 1000) + "\x01" + std::to_string(n);
                const std::size_t end = log.find('\x02', at);
                const std::string got = log.substr(at, end - at);
                order = end != std::string::npos && got.substr(got.find_first_not_of('\n')).rfind(want, 0) == 0;
                at = end + 1;
            }
            std::printf("connect: remote log \"%s\"\n", log.c_str());
            P.eq("connect.a_commit_a_version", order ? 1 : 0, 1);
            const std::vector<ps::Entry> listed = ps::list();
            P.eq("connect.listed", listed.size() == 1 && listed[0].git && listed[0].ahead == 0 && listed[0].behind == 0
                                       && listed[0].remote == url ? 1 : 0, 1);
            const gp::Result twice = gp::connect(setup, "Pad", url, "main");
            P.eq("connect.twice_refused", !twice.ok && twice.why.find("already") != std::string::npos ? 1 : 0, 1);
        }

        // ---- PUSH ------------------------------------------------------------------------------------------------------
        {
            ps::saveVersion("Pad", programOf("four"), 0, dials);
            const std::vector<ps::Entry> listed = ps::list();
            P.eq("project.push.ahead_listed", listed.size() == 1 && listed[0].ahead == 1 ? 1 : 0, 1);
            const gp::Result r = gp::push(setup, "Pad");
            std::printf("push: \"%s\" (%s)\n", r.line.c_str(), r.why.c_str());
            const std::optional<ps::Project> p = ps::load("Pad");
            P.eq("project.push.only_new", r.ok && remoteCount() == 4 && ps::linkOf(p->id)->committed == 4 && ps::list()[0].ahead == 0 ? 1 : 0, 1);
            const gp::Result again = gp::push(setup, "Pad");
            P.eq("project.push.nothing_new", again.ok && remoteCount() == 4 ? 1 : 0, 1);
        }

        // ---- CLONE (machine B) ------------------------------------------------------------------------------------------
        onB();
        {
            const gp::Result r = gp::clone(setup, url, "");
            std::printf("clone: \"%s\" (%s)\n", r.line.c_str(), r.why.c_str());
            const std::optional<ps::Project> p = ps::load(r.project);
            onA();
            const std::optional<ps::Project> a = ps::load("Pad");
            onB();
            P.eq("clone.a_project", r.ok && r.project == "Pad" && p && p->versions.size() == 1 && a && p->latest().files == a->latest().files
                                        && p->id != a->id && ps::linkOf(p->id) && ps::linkOf(p->id)->branch == "main" ? 1 : 0, 1);
            const gp::Result second = gp::clone(setup, url, "");
            P.eq("clone.taken_name", second.ok && second.project == "Pad 2" ? 1 : 0, 1);
            if (second.ok)
                ps::remove("Pad 2");
            // The other kind's project: refused, nothing added, no clone left.
            const fs::path other = root / "remotes" / "other.git";
            fs::create_directories(other, ec);
            sh(found, other, { "init", "--bare", "-q" });
            const std::string otherUrl = fileUrl(other);
            const fs::path work = root / "other-work";
            pg::git::Context oc;
            oc.git = found.path;
            oc.allowFile = true;
            pg::git::clone(oc, otherUrl, work.string());
            std::string json = readFile(fs::path(ps::linkOf(p->id)->folder) / "project.json");
            const std::string kind = pg::product::kIsInstrument ? "\"instrument\"" : "\"effect\"";
            const std::string otherKind = pg::product::kIsInstrument ? "\"effect\"" : "\"instrument\"";
            if (const std::size_t k = json.find(kind); k != std::string::npos)
                json.replace(k, kind.size(), otherKind);
            writeFile(work / "project.json", json);
            for (const pg::SourceFile& f : p->latest().files)
                writeFile(work / f.name, f.text);
            oc.folder = work.string();
            pg::git::commitAll(oc, "other kind", 0, {});
            sh(found, work, { "push", "-q", "origin", "HEAD:main" });
            const std::size_t before = ps::list().size();
            const gp::Result refused = gp::clone(setup, otherUrl, "main");
            std::printf("clone: other kind says \"%s\"\n", refused.why.c_str());
            bool leftover = false;
            for (const auto& e : fs::directory_iterator(machineB / "Git", ec))
                leftover = leftover || e.path().filename().string().rfind(".cloning", 0) == 0;
            P.eq("clone.other_kind_refused", !refused.ok && ps::list().size() == before && !leftover ? 1 : 0, 1);
        }

        // ---- PULL --------------------------------------------------------------------------------------------------------
        {
            // B: a new version, pushed. A: nothing of its own since: a fast-forward into a new version.
            ps::saveVersion("Pad", programOf("from b"), 0, dials);
            const gp::Result pushed = gp::push(setup, "Pad");
            onA();
            const int latestBefore = ps::load("Pad")->latest().number;
            const gp::Result r = gp::pull(setup, "Pad");
            std::printf("pull: \"%s\" (%s)\n", r.line.c_str(), r.why.c_str());
            const std::optional<ps::Project> p = ps::load("Pad");
            P.eq("project.pull.fast_forward", pushed.ok && r.ok && r.version == latestBefore + 1 && p->latest().number == r.version
                                          && p->latest().files == programOf("from b") && p->latest().note.rfind("Pulled ", 0) == 0
                                          && ps::linkOf(p->id)->committed == r.version ? 1 : 0, 1);
            const gp::Result none = gp::pull(setup, "Pad");
            P.eq("project.pull.nothing_new", none.ok && none.nothing && ps::load("Pad")->versions.size() == p->versions.size() ? 1 : 0, 1);
            const int pushedCount = remoteCount();
            P.eq("project.pull.not_recommitted", gp::push(setup, "Pad").ok && remoteCount() == pushedCount ? 1 : 0, 1);

            // Both sides move: A's push is refused as behind (nothing forced), and its pull refused as diverged.
            ps::saveVersion("Pad", programOf("a again"), 0, dials);
            onB();
            gp::pull(setup, "Pad");
            ps::saveVersion("Pad", programOf("b again"), 0, dials);
            gp::push(setup, "Pad");
            onA();
            const std::string remoteHead = sh(found, remote, { "rev-parse", "main" });
            const gp::Result behind = gp::push(setup, "Pad");
            std::printf("push: behind says \"%s\"\n", behind.why.c_str());
            P.eq("project.push.behind_not_forced", !behind.ok && behind.behind && sh(found, remote, { "rev-parse", "main" }) == remoteHead ? 1 : 0, 1);
            const std::size_t versions = ps::load("Pad")->versions.size();
            const gp::Result diverged = gp::pull(setup, "Pad");
            std::printf("pull: diverged says \"%s\"\n", diverged.why.c_str());
            P.eq("project.pull.diverged_refused", !diverged.ok && diverged.diverged && ps::load("Pad")->versions.size() == versions ? 1 : 0, 1);
        }

        // ---- CHECK -------------------------------------------------------------------------------------------------------
        {
            const gp::Result r = gp::check(setup, "Pad");
            std::printf("check: \"%s\"\n", r.line.c_str());
            const std::vector<ps::Entry> listed = ps::list();
            P.eq("check.counts", r.ok && listed.size() == 1 && listed[0].ahead == 1 && listed[0].behind == 1 ? 1 : 0, 1);
        }

        // ---- LINKS -------------------------------------------------------------------------------------------------------
        {
            const ps::Links before = ps::links();
            const juce::File file = ps::directory().getChildFile(ps::kLinksFile);
            const juce::var json = juce::JSON::parse(file.loadFileAsString());
            P.eq("links.file", file.existsAsFile() && json["format"].toString() == "funk-playground-git-links" && json["links"].size() == 1 ? 1 : 0, 1);
            // Round trip: written again whole, read back the same.
            ps::Link k = before.links.at(0);
            k.checkedMs += 1;
            ps::setLink(k);
            ps::Link back = ps::links().links.at(0);
            P.eq("links.round_trip", back == k ? 1 : 0, 1);
            // A rename in the file manager keeps the link (by id).
            fs::rename(machineA / "Pad.fmproject", machineA / "Pad Renamed.fmproject", ec);
            std::vector<ps::Entry> listed = ps::list();
            P.eq("links.rename_kept", listed.size() == 1 && listed[0].name == "Pad Renamed" && listed[0].git
                                          && ps::linkCached("Pad Renamed").has_value() ? 1 : 0, 1);
            // The listing reads no repository: the working folder moved away, the listing is as it was.
            const fs::path folder = k.folder, away = root / "moved-away";
            fs::rename(folder, away, ec);
            listed = ps::list();
            P.eq("links.listing_reads_no_repo", listed.size() == 1 && listed[0].git && listed[0].ahead == 1 ? 1 : 0, 1);
            const gp::Result gone = gp::push(setup, "Pad Renamed");
            P.eq("links.folder_gone_said", !gone.ok && gone.why.find("working folder is gone") != std::string::npos ? 1 : 0, 1);
            fs::rename(away, folder, ec);
            fs::rename(machineA / "Pad Renamed.fmproject", machineA / "Pad.fmproject", ec);
            P.eq("links.no_address_in_project", readFile(machineA / "Pad.fmproject").find(url) == std::string::npos ? 1 : 0, 1);
        }

        // ---- MERGE (package 05c): A and the remote both moved since they last met ---------------------------------------
        {
            // Both sides changed the program's file and the version's lines of project.json: git cannot join them.
            const std::optional<ps::Project> before = ps::load("Pad");
            const std::string headBefore = sh(found, ps::linkOf(before->id)->folder, { "rev-parse", "HEAD" });
            const gp::Result conflict = gp::pull(setup, "Pad");
            std::printf("merge: conflict says \"%s\"\n", conflict.why.c_str());
            const fs::path aFolder = ps::linkOf(before->id)->folder;
            P.eq("merge.conflict_aborted", !conflict.ok && conflict.diverged && !there(aFolder / ".git" / "MERGE_HEAD")
                                               && sh(found, aFolder, { "rev-parse", "HEAD" }) == headBefore
                                               && sh(found, aFolder, { "status", "--porcelain" }).empty()
                                               && ps::load("Pad")->versions.size() == before->versions.size() ? 1 : 0, 1);
            P.eq("merge.marked_diverged", ps::linkOf(before->id)->diverged ? 1 : 0, 1);
            // Their side, read for the Compare card: B's newest program, nothing written to the store.
            const gp::Result t = gp::theirs(setup, "Pad");
            P.eq("merge.theirs_read", t.ok && t.theirsProject && t.theirsProject->latest().files == programOf("b again")
                                          && ps::load("Pad")->versions.size() == before->versions.size()
                                          && !there(aFolder.parent_path() / ".theirs") ? 1 : 0, 1);
            // Keep mine: theirs a version, mine on top, the link settled; then pushed, and the remote's tree is mine.
            const gp::Result keep = gp::keepMine(setup, "Pad");
            std::printf("merge: keep mine \"%s\" (%s)\n", keep.line.c_str(), keep.why.c_str());
            const std::optional<ps::Project> kept = ps::load("Pad");
            const std::size_t n = kept->versions.size();
            P.eq("merge.keep_mine_both_kept", keep.ok && n == before->versions.size() + 2
                                                  && kept->versions[n - 2].files == programOf("b again")
                                                  && kept->versions[n - 2].note.rfind("Theirs, ", 0) == 0
                                                  && kept->latest().files == programOf("a again")
                                                  && kept->latest().note.rfind("Kept mine over ", 0) == 0 ? 1 : 0, 1);
            const gp::Result keepPush = gp::push(setup, "Pad");
            P.eq("merge.keep_mine_pushed", keepPush.ok && !ps::linkOf(kept->id)->diverged
                                               && sh(found, remote, { "show", "main:program.fm" }) == programOf("a again").front().text ? 1 : 0, 1);
            // Take theirs: B moves on (a fast-forward of A's merge), A saves something else.
            onB();
            const gp::Result bPull = gp::pull(setup, "Pad");
            ps::saveVersion("Pad", programOf("b third"), 0, dials);
            gp::push(setup, "Pad");
            onA();
            ps::saveVersion("Pad", programOf("a third"), 0, dials);
            const gp::Result conflict2 = gp::pull(setup, "Pad");
            const int mineNumber = ps::load("Pad")->latest().number;
            const gp::Result take = gp::takeTheirs(setup, "Pad");
            std::printf("merge: take theirs \"%s\" (%s)\n", take.line.c_str(), take.why.c_str());
            const std::optional<ps::Project> taken = ps::load("Pad");
            const ps::Version* mine = taken->version(mineNumber);
            P.eq("merge.take_theirs_both_kept", bPull.ok && conflict2.diverged && take.ok && taken->latest().files == programOf("b third")
                                                    && taken->latest().note.rfind("Took theirs, ", 0) == 0 && mine != nullptr
                                                    && mine->files == programOf("a third") ? 1 : 0, 1);
            const gp::Result takePush = gp::push(setup, "Pad");
            P.eq("merge.take_theirs_pushed", takePush.ok && sh(found, remote, { "show", "main:program.fm" }) == programOf("b third").front().text
                                                 && sh(found, aFolder, { "show", "-s", "--format=%P", "HEAD~0" }).find(' ') != std::string::npos ? 1 : 0, 1);
            // Nothing to settle: refused in words, nothing changed.
            const std::size_t now = ps::load("Pad")->versions.size();
            const gp::Result idle = gp::keepMine(setup, "Pad");
            P.eq("merge.nothing_to_settle", !idle.ok && idle.why.find("nothing to settle") != std::string::npos
                                                && ps::load("Pad")->versions.size() == now ? 1 : 0, 1);
            onB();
            gp::pull(setup, "Pad");
            onA();
        }

        // ---- JOB (the Processor's git thread) ------------------------------------------------------------------------------
        {
            const funkgui::HeadlessGuiScope gui;
            pg::Processor proc;
            proc.setGitFileRemotesForTests(true);
            pg::EditorPort& port = proc.editorPort();
            const auto waitJob = [&](const pg::GitJob& job) {
                const uint64_t ended = port.gitState().ended;
                std::string why;
                if (!port.startGit(job, why))
                    return pg::GitState{};
                const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
                while (port.gitState().ended == ended && std::chrono::steady_clock::now() < until)
                {
                    proc.poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return port.gitState();
            };
            // Settle A first: its version on the remote taken (Take theirs is part 3's), by a fresh clone on B pushed.
            onA();
            const fs::path aFolder = ps::linkOf(ps::load("Pad")->id)->folder;
            sh(found, aFolder, { "reset", "-q", "--hard", "origin/main" });
            {
                ps::Link k = *ps::linkOf(ps::load("Pad")->id);
                k.committed = ps::load("Pad")->latest().number;
                k.ahead = 0;
                ps::setLink(k);
            }
            std::string why;
            P.eq("job.opened", port.openProject("Pad", 0, why) ? 1 : 0, 1);
            proc.settle();
            onB();
            ps::saveVersion("Pad", programOf("job one"), 0, dials);
            gp::push(setup, "Pad");
            onA();
            pg::GitJob pull;
            pull.kind = pg::GitKind::pull;
            pull.project = "Pad";
            const pg::GitState s = waitJob(pull);
            std::printf("job: pull \"%s\" (%s) opened %d\n", s.line.c_str(), s.why.c_str(), s.opened ? 1 : 0);
            proc.settle();
            P.eq("job.pull_opened_unedited", s.ok && s.opened && port.project().version == s.version
                                                 && port.draft().files == programOf("job one") ? 1 : 0, 1);
            // Edited: the new version waits.
            pg::Draft d = port.draft();
            d.files[0].text += "// typed\n";
            port.setDraft(d);
            onB();
            ps::saveVersion("Pad", programOf("job two"), 0, dials);
            gp::push(setup, "Pad");
            onA();
            const pg::GitState s2 = waitJob(pull);
            std::printf("job: pull while edited \"%s\"\n", s2.line.c_str());
            P.eq("job.pull_left_edited", s2.ok && !s2.opened && port.project().version != s2.version
                                             && port.draft().files[0].text.find("// typed") != std::string::npos ? 1 : 0, 1);
            // NOGIT: a git path that is no git: said, nothing else changes.
            const ps::Links linksBefore = ps::links();
            port.setGitPath((root / "no-such-git").string(), why);
            pg::GitJob check;
            check.kind = pg::GitKind::check;
            check.project = "Pad";
            const pg::GitState s3 = waitJob(check);
            std::printf("job: no git says \"%s\"\n", s3.why.c_str());
            ps::Links linksAfter = ps::links();
            linksAfter.git = linksBefore.git;
            P.eq("job.nogit_said", !s3.ok && !s3.why.empty() && s3.looked && s3.gitPath.empty() && linksAfter == linksBefore ? 1 : 0, 1);
            port.setGitPath("", why);
            // Disconnect: the link goes, the folder stays.
            pg::GitJob off;
            off.kind = pg::GitKind::disconnect;
            off.project = "Pad";
            const pg::GitState s4 = waitJob(off);
            P.eq("job.disconnect", s4.ok && ps::links().links.empty() && there(aFolder / ".git")
                                       && s4.line.find(aFolder.string()) != std::string::npos ? 1 : 0, 1);
        }
        ps::setDirectoryForTests(juce::File());
    }
}

PROBE(plugin, git)
{
    const std::string prefix = pg::product::kEnvPrefix;
    const char* sandboxVar = std::getenv((prefix + "PREFS_DIR").c_str());
    if (sandboxVar == nullptr || *sandboxVar == '\0')
    {
        probe::skip(P, "git", "no sandbox (" + prefix + "PREFS_DIR): run the probes through ctest");
        return P.finish();
    }
    const fs::path root = fs::path(sandboxVar) / "git";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // git's configuration: the probe's files alone; the committer by environment.
    const fs::path home = root / "home";
    fs::create_directories(home, ec);
    const fs::path globalHooks = root / "global-hooks";
    const fs::path globalConfig = root / "gitconfig";
    writeFile(globalConfig, "[user]\n\tuseConfigOnly = true\n[core]\n\thooksPath = " + globalHooks.generic_string()
                                + "\n[init]\n\tdefaultBranch = main\n");
    writeFile(root / "gitconfig-system", "");
    setenv("GIT_CONFIG_GLOBAL", globalConfig.string().c_str(), 1);
    setenv("GIT_CONFIG_SYSTEM", (root / "gitconfig-system").string().c_str(), 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("HOME", home.string().c_str(), 1);
    setenv("XDG_CONFIG_HOME", (home / ".config").string().c_str(), 1);
    const auto identity = [](bool on) {
        for (const char* v : { "GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME" })
            on ? setenv(v, "Probe", 1) : unsetenv(v);
        for (const char* v : { "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL" })
            on ? setenv(v, "probe@example.invalid", 1) : unsetenv(v);
    };
    identity(true);

    // ---- RUN -------------------------------------------------------------------------------------------------------------
#if !defined(_WIN32)
    {
        const fs::path runDir = root / "run";
        fs::create_directories(runDir, ec);
        pg::run::Request r;
        r.args = { "/bin/sh", "-c", "printf hello; printf oops >&2; exit 3" };
        pg::run::Result res = pg::run::run(r);
        std::printf("run: ended %d code %d out \"%s\" err \"%s\"\n", static_cast<int>(res.ended), res.code, res.out.c_str(),
                    res.err.c_str());
        P.eq("run.captured", res.ended == pg::run::Ended::exited && res.code == 3 && res.out == "hello" && res.err == "oops" ? 1 : 0, 1);

        setenv("FP_PROBE_RUN_GONE", "here", 1);
        r = {};
        r.args = { "sh", "-c", "printf '%s|%s|%s' \"$FP_PROBE_RUN_SET\" \"${FP_PROBE_RUN_GONE-none}\" \"$(pwd -P)\"" };
        r.env = { { "FP_PROBE_RUN_SET", "set" } };
        r.unset = { "FP_PROBE_RUN_GONE" };
        r.folder = runDir.string();
        res = pg::run::run(r);
        const std::string wanted = "set|none|" + fs::canonical(runDir, ec).string();
        std::printf("run: env and folder \"%s\" (wanted \"%s\")\n", res.out.c_str(), wanted.c_str());
        P.eq("run.env_folder", res.ok() && res.out == wanted ? 1 : 0, 1);

        r = {};
        r.args = { "/bin/sh", "-c", "head -c 200000 /dev/zero; echo done >&2" };
        r.outLimit = 1000;
        res = pg::run::run(r);
        P.eq("run.out_cut", res.ok() && res.out.size() == 1000 && res.outCut && res.err == "done\n" ? 1 : 0, 1);

        r = {};
        r.args = { "/bin/sh", "-c", "i=0; while [ $i -lt 3000 ]; do echo line$i >&2; i=$((i+1)); done; echo last >&2" };
        r.errLimit = 100;
        res = pg::run::run(r);
        std::printf("run: err tail %zu bytes, cut %d, ends \"%s\"\n", res.err.size(), res.errCut ? 1 : 0,
                    res.err.substr(res.err.size() > 12 ? res.err.size() - 12 : 0).c_str());
        P.eq("run.err_tail", res.ok() && res.err.size() == 100 && res.errCut && res.err.size() >= 5
                                 && res.err.compare(res.err.size() - 5, 5, "last\n") == 0 ? 1 : 0, 1);

        // A timeout: the child and what it started (a background job holding the pipes, a marker it would leave).
        const fs::path late = runDir / "late-marker";
        r = {};
        r.args = { "/bin/sh", "-c", "(sleep 1; touch '" + late.string() + "') & sleep 30" };
        r.timeoutMs = 300;
        res = pg::run::run(r);
        std::printf("run: timeout ended %d after %.0f ms\n", static_cast<int>(res.ended), res.ms);
        P.eq("run.timeout", res.ended == pg::run::Ended::timedOut && res.ms < 3000 ? 1 : 0, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        P.eq("run.timeout_group", there(late) ? 0 : 1, 1);

        std::atomic<bool> cancel{ false };
        std::thread canceller([&cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            cancel = true;
        });
        r = {};
        r.args = { "sleep", "30" };
        r.cancel = &cancel;
        res = pg::run::run(r);
        canceller.join();
        std::printf("run: cancel ended %d after %.0f ms\n", static_cast<int>(res.ended), res.ms);
        P.eq("run.cancel", res.ended == pg::run::Ended::cancelled && res.ms < 3000 ? 1 : 0, 1);

        // A descriptor left inheritable (no close-on-exec) at 57: the child holds 1 (the control) and not 57.
        const int opened = ::open(globalConfig.string().c_str(), O_RDONLY);
        const int leaked = opened >= 0 ? ::fcntl(opened, F_DUPFD, 57) : -1;
        r = {};
        r.args = { "/bin/sh", "-c",
                   "for f in 1 " + std::to_string(leaked) + "; do if [ -e /dev/fd/$f ]; then printf open; else printf closed; fi; "
                   "printf ' '; done" };
        res = pg::run::run(r);
        std::printf("run: descriptors 1 and %d in the child: %s\n", leaked, res.out.c_str());
        P.eq("run.no_handle", leaked >= 57 && res.ok() && res.out == "open closed " ? 1 : 0, 1);
        if (leaked >= 0)
            ::close(leaked);
        if (opened >= 0)
            ::close(opened);

        const std::string marker = (runDir / "shell-marker").string();
        const std::string odd = "a $(touch " + marker + ") `touch " + marker + "` ; \"q\" 'r'";
        r = {};
        r.args = { "printf", "%s", odd };
        r.folder = runDir.string();
        res = pg::run::run(r);
        P.eq("run.no_shell", res.ok() && res.out == odd && !there(marker) ? 1 : 0, 1);

        r = {};
        r.args = { prefix + "no-such-program" };
        res = pg::run::run(r);
        std::printf("run: not found says \"%s\"\n", res.why.c_str());
        P.eq("run.not_found", res.ended == pg::run::Ended::notStarted && res.why.find("not found") != std::string::npos ? 1 : 0, 1);
    }
#else
    for (const char* row : { "run.captured", "run.env_folder", "run.out_cut", "run.err_tail", "run.timeout",
                             "run.timeout_group", "run.cancel", "run.no_handle", "run.no_shell", "run.not_found" })
        probe::skip(P, row, "the rows run /bin/sh as their child (POSIX)");
#endif

    // ---- git --------------------------------------------------------------------------------------------------------------
    const pg::git::Found found = pg::git::find();
    std::printf("git: %s %s (%s)\n", found.path.c_str(), found.version.c_str(), found.why.c_str());
    if (!found.found())
    {
        for (const char* section : { "commit", "push", "pull", "clone", "address", "hooks", "nogit", "tree", "connect", "check",
                                     "links", "job" })
            probe::skip(P, section, "no git on this machine (" + prefix + "GIT): " + found.why);
        return P.finish();
    }
    P.eq("nogit.found_probe_git", !found.version.empty() ? 1 : 0, 1);

    const auto context = [&](const fs::path& folder) {
        pg::git::Context c;
        c.git = found.path;
        c.folder = folder.string();
        c.allowFile = true;
        return c;
    };
    const auto fileUrl = [](const fs::path& p) {
        std::string s = p.generic_string();
        return "file://" + std::string(s.front() == '/' ? "" : "/") + s;
    };
    const auto count = [&](const fs::path& repo, const std::string& ref) {
        const pg::git::Outcome o = pg::git::call(context(repo), { "rev-list", "--count", ref });
        return o.ok ? std::atoi(o.out.c_str()) : -1;
    };
    const auto headOf = [&](const fs::path& repo) {
        std::string sha;
        pg::git::head(context(repo), sha);
        return sha;
    };

    // The remote: a bare repository whose HEAD is main.
    const fs::path bare = root / "remotes" / "pad.git";
    fs::create_directories(bare, ec);
    pg::git::call(context(bare), { "init", "--bare", "-q" });
    pg::git::call(context(bare), { "symbolic-ref", "HEAD", "refs/heads/main" });
    const std::string url = fileUrl(bare);

    // ---- COMMIT ------------------------------------------------------------------------------------------------------------
    const fs::path a = root / "a";
    const std::int64_t day = 86400000, t0 = 1700000000000;
    {
        const pg::git::Context c = context(a);
        const pg::git::Outcome init = pg::git::init(c, "main");
        std::printf("commit: init ok %d (%s)\n", init.ok ? 1 : 0, init.why.c_str());
        bool all = init.ok;
        for (int v = 1; v <= 3; ++v)
        {
            writeFile(a / "program.fm", "// version " + std::to_string(v) + "\n");
            std::string sha;
            const pg::git::Outcome o = pg::git::commitAll(c, "Version " + std::to_string(v), t0 + v * day,
                                                          { { "Playground-Version", std::to_string(v) } }, &sha);
            std::printf("commit: v%d ok %d sha %s (%s)\n", v, o.ok ? 1 : 0, sha.c_str(), o.why.c_str());
            all = all && o.ok && sha.size() == 40;
        }
        P.eq("commit.made", all ? 1 : 0, 1);
        std::vector<pg::git::Commit> log;
        const pg::git::Outcome l = pg::git::log(c, 10, log);
        bool order = l.ok && log.size() == 3;
        for (std::size_t i = 0; order && i < log.size(); ++i)
            order = log[i].subject == "Version " + std::to_string(3 - i) && log[i].timeMs == t0 + static_cast<std::int64_t>(3 - i) * day;
        P.eq("commit.order_and_dates", order ? 1 : 0, 1);
        const pg::git::Outcome body = pg::git::call(c, { "log", "-n", "1", "--format=%B%x00%cd", "--date=raw" });
        std::printf("commit: newest body \"%s\"\n", body.out.c_str());
        P.eq("commit.trailer", body.ok && body.out.find("Version 3\n\nPlayground-Version: 3") == 0 ? 1 : 0, 1);
        P.eq("commit.committer_date",
             body.ok && body.out.find(std::to_string((t0 + 3 * day) / 1000) + " +0000") != std::string::npos ? 1 : 0, 1);

        writeFile(a / "program.fm", "// version 4\n");
        pg::git::commitAll(c, "Brighter\n\nThe filter opens further.", t0 + 4 * day, { { "Playground-Version", "4" } });
        std::vector<pg::git::Commit> newest;
        pg::git::log(c, 1, newest);
        const pg::git::Outcome body4 = pg::git::call(c, { "log", "-n", "1", "--format=%b" });
        P.eq("commit.body_kept", newest.size() == 1 && newest[0].subject == "Brighter"
                                     && body4.out.find("The filter opens further.") != std::string::npos ? 1 : 0, 1);

        const std::string before = headOf(a);
        const pg::git::Outcome empty = pg::git::commitAll(c, "Nothing", 0, {}, nullptr, false);
        P.eq("commit.empty_refused", !empty.ok && headOf(a) == before ? 1 : 0, 1);

        identity(false);
        writeFile(a / "other.fm", "// who?\n");
        const pg::git::Outcome nobody = pg::git::commitAll(c, "No one", 0, {});
        identity(true);
        std::printf("commit: no identity says \"%s\"\n", nobody.why.c_str());
        P.eq("commit.identity_words", !nobody.ok && headOf(a) == before && nobody.why.find("user.email") != std::string::npos ? 1 : 0, 1);
        fs::remove(a / "other.fm", ec);
        pg::git::call(c, { "reset", "-q" });
    }

    // ---- PUSH -------------------------------------------------------------------------------------------------------------
    {
        const pg::git::Context c = context(a);
        P.eq("push.remote_added", pg::git::addRemote(c, "origin", url).ok ? 1 : 0, 1);
        bool has = true;
        const pg::git::Outcome lr = pg::git::remoteHas(c, "origin", "main", has);
        P.eq("push.remote_empty", lr.ok && !has ? 1 : 0, 1);
        const pg::git::Outcome first = pg::git::push(c, "origin", "main", true);
        std::printf("push: first ok %d (%s)\n", first.ok ? 1 : 0, first.why.c_str());
        std::string remoteSha;
        pg::git::remoteHas(c, "origin", "main", has, &remoteSha);
        P.eq("push.first", first.ok && has && remoteSha == headOf(a) && count(bare, "main") == 4 ? 1 : 0, 1);

        writeFile(a / "program.fm", "// version 5\n");
        pg::git::commitAll(c, "Version 5", t0 + 5 * day, { { "Playground-Version", "5" } });
        pg::git::fetch(c, "origin", "main");
        pg::git::Counts n;
        pg::git::counts(c, "origin", "main", n);
        std::printf("push: before, ahead %d behind %d\n", n.ahead, n.behind);
        const pg::git::Outcome second = pg::git::push(c, "origin", "main");
        P.eq("push.only_new", n.ahead == 1 && n.behind == 0 && second.ok && count(bare, "main") == 5 && headOf(bare) == headOf(a)
                                  ? 1 : 0, 1);
        const pg::git::Outcome again = pg::git::push(c, "origin", "main");
        P.eq("push.nothing_new", again.ok && count(bare, "main") == 5 ? 1 : 0, 1);
    }

    // ---- CLONE ------------------------------------------------------------------------------------------------------------
    const fs::path b = root / "b";
    {
        pg::git::Context c = context(root);
        const pg::git::Outcome o = pg::git::clone(c, url, b.string(), "main");
        std::printf("clone: ok %d (%s)\n", o.ok ? 1 : 0, o.why.c_str());
        P.eq("clone.made", o.ok && headOf(b) == headOf(a) && readFile(b / "program.fm") == "// version 5\n" ? 1 : 0, 1);
        std::vector<pg::git::Commit> log;
        pg::git::log(context(b), 10, log);
        P.eq("clone.history", log.size() == 5 && log.back().subject == "Version 1" ? 1 : 0, 1);

        const fs::path taken = root / "taken";
        writeFile(taken / "mine.txt", "mine");
        const pg::git::Outcome over = pg::git::clone(c, url, taken.string());
        std::printf("clone: into a folder with a file says \"%s\"\n", over.why.c_str());
        P.eq("clone.into_taken_refused", !over.ok && readFile(taken / "mine.txt") == "mine" && !there(taken / ".git") ? 1 : 0, 1);

        c.allowFile = false;
        const fs::path plain = root / "plain";
        const pg::git::Outcome noFile = pg::git::clone(c, url, plain.string());
        std::printf("clone: file:// without leave says \"%s\"\n", noFile.why.c_str());
        P.eq("clone.file_refused", !noFile.ok && noFile.ended == pg::run::Ended::notStarted && !there(plain) ? 1 : 0, 1);
    }

    // ---- PULL -------------------------------------------------------------------------------------------------------------
    {
        const pg::git::Context cb = context(b), ca = context(a);
        pg::git::Counts n{ 9, 9 };
        const std::string before = headOf(b);
        const bool fetched = pg::git::fetch(cb, "origin", "main").ok;
        pg::git::counts(cb, "origin", "main", n);
        const pg::git::Outcome ff = pg::git::fastForward(cb, "origin", "main");
        P.eq("pull.nothing_new", fetched && n.ahead == 0 && n.behind == 0 && ff.ok && headOf(b) == before ? 1 : 0, 1);

        writeFile(a / "program.fm", "// version 6\n");
        pg::git::commitAll(ca, "Version 6", t0 + 6 * day, { { "Playground-Version", "6" } });
        pg::git::push(ca, "origin", "main");
        pg::git::fetch(cb, "origin", "main");
        pg::git::counts(cb, "origin", "main", n);
        std::printf("pull: something new, ahead %d behind %d\n", n.ahead, n.behind);
        const pg::git::Outcome ff2 = pg::git::fastForward(cb, "origin", "main");
        P.eq("pull.fast_forward", n.behind == 1 && n.ahead == 0 && ff2.ok && headOf(b) == headOf(a)
                                      && readFile(b / "program.fm") == "// version 6\n" ? 1 : 0, 1);

        // Both sides move: B pushes first; A's push is refused as behind, and nothing is forced.
        writeFile(b / "program.fm", "// B's change\n");
        pg::git::commitAll(cb, "From B", t0 + 7 * day, {});
        const bool bPushed = pg::git::push(cb, "origin", "main").ok;
        writeFile(a / "program.fm", "// A's change\n");
        pg::git::commitAll(ca, "From A", t0 + 7 * day + 1000, {});
        const pg::git::Outcome rejected = pg::git::push(ca, "origin", "main");
        std::printf("push: from behind ok %d behind %d says \"%s\"\n", rejected.ok ? 1 : 0, rejected.behind ? 1 : 0,
                    rejected.why.c_str());
        P.eq("push.rejected_behind", bPushed && !rejected.ok && rejected.behind ? 1 : 0, 1);
        P.eq("push.nothing_forced", headOf(bare) == headOf(b) && count(bare, "main") == 7 ? 1 : 0, 1);

        const std::string aBefore = headOf(a);
        pg::git::fetch(ca, "origin", "main");
        pg::git::counts(ca, "origin", "main", n);
        const pg::git::Outcome noFf = pg::git::fastForward(ca, "origin", "main");
        std::printf("pull: diverged ahead %d behind %d, fast-forward says \"%s\"\n", n.ahead, n.behind, noFf.why.c_str());
        P.eq("pull.diverged_counted", n.ahead == 1 && n.behind == 1 ? 1 : 0, 1);
        P.eq("pull.no_ff_refused", !noFf.ok && headOf(a) == aBefore && readFile(a / "program.fm") == "// A's change\n" ? 1 : 0, 1);
    }

    // ---- ADDRESS ----------------------------------------------------------------------------------------------------------
    {
        const fs::path marker = root / "ext-marker";
        const std::string ext = "ext::sh -c touch% " + marker.generic_string();
        struct Case
        {
            const char* key;
            std::string address;
            bool taken;
        };
        const std::vector<Case> cases{
            { "https", "https://github.com/me/pad.git", true },
            { "ssh", "ssh://git@example.com/me/pad.git", true },
            { "scp", "git@github.com:me/pad.git", true },
            { "ext", ext, false },
            { "transport", "fd::17", false },
            { "dash", "-uhttps://example.com/x", false },
            { "dash_scp", "git@-oProxyCommand=x:y", false },
            { "control", "https://example.com/a\nb", false },
            { "tab", "https://example.com/a\tb", false },
            { "space", "https://example.com/a b", false },
            { "scheme", "ftp://example.com/pad.git", false },
            { "http", "http://example.com/pad.git", false },
            { "path", "/home/me/pad.git", false },
            { "file", "file:///tmp/pad.git", false },
            { "empty", "", false },
        };
        for (const Case& k : cases)
        {
            const std::string why = pg::git::addressRefusal(k.address);
            P.eq(std::string("address.") + k.key, why.empty() == k.taken ? 1 : 0, 1);
        }
        P.eq("address.file_with_leave", pg::git::addressRefusal("file:///tmp/pad.git", true).empty() ? 1 : 0, 1);

        const pg::git::Context c = context(a);
        const pg::git::Outcome add = pg::git::addRemote(c, "evil", ext);
        const pg::git::Outcome remotes = pg::git::call(c, { "remote" });
        const pg::git::Outcome cl = pg::git::clone(context(root), ext, (root / "evil").string());
        P.eq("address.helpers_refuse", !add.ok && !cl.ok && remotes.out.find("evil") == std::string::npos && !there(root / "evil")
                                           ? 1 : 0, 1);
        // Past the check: git itself, given the address directly, refuses to run it.
        const pg::git::Outcome direct = pg::git::call(c, { "ls-remote", "--", ext }, true);
        std::printf("address: git given ext:: directly says \"%s\"\n", direct.why.c_str());
        P.eq("address.git_refuses_ext", !direct.ok && !there(marker) ? 1 : 0, 1);
        P.eq("branch.refusals", pg::git::branchRefusal("main").empty() && pg::git::branchRefusal("feature/x-1").empty()
                                    && !pg::git::branchRefusal("-f").empty() && !pg::git::branchRefusal("a..b").empty()
                                    && !pg::git::branchRefusal("a b").empty() && !pg::git::branchRefusal("x.lock").empty()
                                    ? 1 : 0, 1);
    }

    // ---- HOOKS ------------------------------------------------------------------------------------------------------------
#if !defined(_WIN32)
    {
        const fs::path marker = root / "hook-marker";
        const auto plant = [&](const fs::path& dir) {
            for (const char* name : { "pre-commit", "post-commit", "commit-msg", "pre-push" })
            {
                writeFile(dir / name, "#!/bin/sh\necho " + std::string(name) + " >> '" + marker.string() + "'\nexit 1\n");
                fs::permissions(dir / name, fs::perms::owner_all, fs::perm_options::replace, ec);
            }
        };
        plant(globalHooks);              // the probe's global config's core.hooksPath
        plant(a / ".git" / "hooks");     // the repository's own

        // The control: plain git, in a repository of its own, runs the global hooks (its pre-commit refuses).
        const fs::path control = root / "control";
        fs::create_directories(control, ec);
        pg::run::Request r;
        r.args = { found.path, "-C", control.string(), "init", "-q" };
        pg::run::run(r);
        writeFile(control / "x", "x");
        r.args = { found.path, "-C", control.string(), "add", "-A" };
        pg::run::run(r);
        r.args = { found.path, "-C", control.string(), "commit", "-q", "-m", "control" };
        const pg::run::Result plain = pg::run::run(r);
        P.eq("hooks.control_runs", !plain.ok() && there(marker) ? 1 : 0, 1);
        fs::remove(marker, ec);

        const pg::git::Context c = context(a);
        writeFile(a / "program.fm", "// hooked?\n");
        const pg::git::Outcome made = pg::git::commitAll(c, "With hooks planted", 0, {});
        P.eq("hooks.repo_commit", made.ok && !there(marker) ? 1 : 0, 1);

        // The global hooks alone (the repository's taken away): still none runs.
        fs::remove_all(a / ".git" / "hooks", ec);
        writeFile(a / "program.fm", "// hooked again?\n");
        const pg::git::Outcome made2 = pg::git::commitAll(c, "With global hooks", 0, {});
        P.eq("hooks.global_commit", made2.ok && !there(marker) ? 1 : 0, 1);

        // pre-push: a fresh clone, a commit, a push (its hooks planted too).
        const fs::path d = root / "d";
        pg::git::clone(context(root), url, d.string(), "main");
        plant(d / ".git" / "hooks");
        writeFile(d / "program.fm", "// pushed with hooks\n");
        pg::git::commitAll(context(d), "Pushed with hooks", 0, {});
        const pg::git::Outcome pushed = pg::git::push(context(d), "origin", "main");
        std::printf("hooks: push ok %d (%s)\n", pushed.ok ? 1 : 0, pushed.why.c_str());
        P.eq("hooks.push", pushed.ok && !there(marker) ? 1 : 0, 1);
    }
#else
    for (const char* row : { "hooks.control_runs", "hooks.repo_commit", "hooks.global_commit", "hooks.push" })
        probe::skip(P, row, "the planted hooks are shell scripts (POSIX)");
#endif

    projectSections(P, found, root, fileUrl);

    // ---- NOGIT ------------------------------------------------------------------------------------------------------------
    {
        const pg::git::Found missing = pg::git::find((root / "no-such-git").string());
        std::printf("nogit: missing says \"%s\"\n", missing.why.c_str());
        P.eq("nogit.missing", !missing.found() && !missing.why.empty() ? 1 : 0, 1);
        const fs::path text = root / "not-git.txt";
        writeFile(text, "just text\n");
        const pg::git::Found textFound = pg::git::find(text.string());
        P.eq("nogit.text_file", !textFound.found() && !textFound.why.empty() ? 1 : 0, 1);
#if !defined(_WIN32)
        const std::string echo = pg::run::resolve("echo");
        const pg::git::Found echoFound = pg::git::find(echo);
        std::printf("nogit: %s says \"%s\"\n", echo.c_str(), echoFound.why.c_str());
        P.eq("nogit.other_program", !echo.empty() && !echoFound.found() && echoFound.why.find("is not git") != std::string::npos ? 1 : 0, 1);
#else
        probe::skip(P, "nogit.other_program", "echo is no program of its own on Windows");
#endif
        pg::git::Context none;
        none.folder = (root / "never").string();
        const pg::git::Outcome i = pg::git::init(none, "main");
        const pg::git::Outcome cm = pg::git::commitAll(none, "x", 0, {});
        P.eq("nogit.nothing_changed", !i.ok && !cm.ok && i.why == pg::git::notFoundWords() && !there(root / "never") ? 1 : 0, 1);
    }
    return P.finish();
}
