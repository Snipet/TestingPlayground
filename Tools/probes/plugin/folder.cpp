// PROBE name=plugin.folder timeout=180
//
// plugin.folder: the program's files mirrored to a folder and synced both ways (plugin/FolderSync.h), in a temporary
// folder of the probe's own, the folder's editor played by the probe's own writes. A FolderSync over a draft of the
// probe's (its version guard and RUN counted), passed at a clock of the probe's (every pass looks: no watcher needed),
// then the processor's through its EditorPort:
//   START    a folder of the name given, made, holding the draft's files; host.fm, the plugin's, read only; the note
//            (ABOUT.md), VS Code's settings and the manifest; nothing written into the draft
//   OUT      an edit of the plugin's waits for the draft to rest, then is written whole (no temporary file left), and
//            never comes back as an edit of the folder's; typing that never rests is written within kMaxWaitSeconds of
//            its first change, by the timer's own passes (no look forced)
//   IN       a save in the folder waits for the folder to hold still, then lands in the draft in one write (two files
//            saved together, still one); with run on save it runs
//   ATOMIC   a save that renames a temporary file over the file lands; a save in JetBrains' steps (the file moved
//            aside, the new one renamed in) lands once, and the draft never loses the file meanwhile; a save written
//            in two parts lands whole, never the half
//   SWAP     an editor's swap, backup and lock files (.x.swp, x~, #x#, .#x, 4913, x.crswap) are never taken
//   FOLDER   a new file there is appended to the draft; renamed, it keeps its place (its tab still shown); deleted,
//            it goes; the program deleted there is written back (a note says so), renamed there it is renamed in the
//            draft; a name the plugin cannot take and a file past 16 are left there with a note; a .txt is nobody's,
//            a shader's .sh the program's
//   PLUGIN   a file added, renamed and deleted in the draft is so in the folder
//   CONFLICT both sides changed: the file is held (neither side written), listed; keeping the plugin's writes it there
//            and keeps the folder's in .conflicts/; keeping the folder's lands it in the draft and keeps the plugin's
//            there; a conflict both sides end the same by hand is no longer one; one whose folder side grew past 1 MB
//            is no deletion: keeping the folder's refused, keeping the plugin's keeps the large file aside whole
//   GUARD    a write of the folder's the version guard refuses (the draft moved) is taken again at the next pass,
//            nothing lost
//   INCLUDES a library file an #include names but the draft lacks: a read-only copy; host.fm changed in the folder is
//            written again (the text kept in .conflicts/) and never taken; the library file then added in the plugin:
//            the folder's own, writable
//   MANY     a second sync of the same name takes "<name> 2"; once the first stops, the name is free again
//   RESTART  an edit made in the folder while nothing synced lands when it starts again (the manifest's base); an edit
//            on both sides meanwhile is a conflict
//   PROJECT  the draft replaced whole (a project opened): the folder follows (a file gone, a file new); a save in the
//            folder the sync had not taken by then is a conflict, never overwritten
//   CRLF     a save with CR LF lands as LF and is not written back
//   SWITCH   a project with its own host.fm and library file opened over the read-only copies: written there,
//            writable, no conflict; the project before opened again: the copies written again quietly (nothing kept
//            aside, no note)
//   GONE     the folder deleted: made again with the draft's files, the draft untouched
//   WATCH    (macOS) FSEvents: a save lands well before the look kept for a missed event (kWatchedSeconds)
//   PORT     the processor: off at first; on, a folder under the projects' Folders/ (the path shown from ~), the
//            project open's name when there is one; a save there lands in the draft (draftRevision moves) and, with run
//            on save, compiles; the session keeps it on, its name and run on save, and a processor that loads it syncs
//            with the same folder; a state of another folder's name moves it there, the folder it left as it was (not
//            given the state's files); open in VS Code and Show in Finder go to the launcher with the folder's path, and
//            under a probe nothing is launched without one; off, the folder stays as it was
//   LAUNCH   (POSIX) how Linux opens the folder (plugin/Launch.h): xdg-open's arguments are its name and the folder's
//            path as it is; a program started from a list gets a folder named with $(...), backticks, quotes and a
//            semicolon as one argument, unchanged, and no shell runs any of it; the call returns at once while the
//            program runs, and leaves no child of this process; a program not found, or one whose exec fails, is a
//            false with why
#include "Probe.h"

#include "fminus/Patch.h"
#include "plugin/EditorPort.h"
#include "plugin/FolderSync.h"
#include "plugin/Launch.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pg = playground;
namespace fs = std::filesystem;

namespace
{
    // The plugin's side, the probe's: a draft, the version guard (refuse: the next write refused, as when the draft
    // moved), RUN and every text the folder wrote into the draft counted.
    struct Fake
    {
        pg::Draft draft;
        int writes = 0, refused = 0, runs = 0;
        bool refuse = false;
        std::vector<std::string> landed;             // each write's first file's text

        pg::FolderSync::Plugin plugin()
        {
            return { [this] { return draft; },
                     [this](const pg::Draft& now, const pg::Draft& next) {
                         if (refuse || now != draft)
                         {
                             refuse = false;
                             ++refused;
                             return false;
                         }
                         draft = next;
                         ++writes;
                         for (const pg::SourceFile& f : next.files)
                             landed.push_back(f.text);
                         return true;
                     },
                     [this](const pg::Sources&) { ++runs; } };
        }
        const std::string* text(const std::string& name) const
        {
            const pg::SourceFile* f = pg::findFile(draft.files, name);
            return f != nullptr ? &f->text : nullptr;
        }
        int index(const std::string& name) const
        {
            for (std::size_t i = 0; i < draft.files.size(); ++i)
                if (draft.files[i].name == name)
                    return static_cast<int>(i);
            return -1;
        }
        void set(const std::string& name, const std::string& text)
        {
            for (pg::SourceFile& f : draft.files)
                if (f.name == name)
                {
                    f.text = text;
                    return;
                }
            draft.files.push_back({ name, text });
        }
    };

    std::string readFile(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    void writeFile(const fs::path& p, const std::string& text)   // in place, as VS Code saves
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
    }
    void writeAtomic(const fs::path& dir, const std::string& name, const std::string& text)
    {
        const fs::path temp = dir / (".~" + name + ".tmp");
        writeFile(temp, text);
        fs::rename(temp, dir / name);
    }
    bool there(const fs::path& p)
    {
        std::error_code ec;
        return fs::exists(p, ec);
    }
    bool readOnly(const fs::path& p)
    {
        std::error_code ec;
        const fs::perms m = fs::status(p, ec).permissions();
        return !ec && (m & fs::perms::owner_write) == fs::perms::none;
    }
    // Every file in .conflicts/ whose text is `text`.
    bool keptAside(const fs::path& dir, const std::string& text)
    {
        std::error_code ec;
        for (fs::directory_iterator it(dir / pg::FolderSync::kConflicts, ec), end; !ec && it != end; it.increment(ec))
            if (readFile(it->path()) == text)
                return true;
        return false;
    }
    bool tempLeft(const fs::path& dir)
    {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string n = it->path().filename().string();
            if (n.rfind(".fmsync-", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".tmp")
                return true;
        }
        return false;
    }
    bool noted(const pg::FolderState& s, const std::string& part)
    {
        for (const std::string& n : s.notes)
            if (n.find(part) != std::string::npos)
                return true;
        return false;
    }

    // Passes every 50 ms of the probe's clock for `seconds`, each one looking at the folder.
    void passFor(pg::FolderSync& s, double& t, double seconds)
    {
        for (const double end = t + seconds; t < end; t += 0.05)
            s.pass(t, true);
    }

    std::string program(const std::string& body)
    {
        if constexpr (pg::product::kIsInstrument)
            return "// Folder: a program\n#include \"lib.fm\"\nmodule Voice\n{\n    " + body
                 + "\n    void start(float note, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// Folder: a program\n#include \"lib.fm\"\nmodule Effect\n{\n    " + body
                 + "\n    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }
    const char* kLib = "float twice(float x) { return x * 2.0f; }\n";
}

PROBE(plugin, folder)
{
    const funkgui::HeadlessGuiScope gui;
    const juce::File tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile(juce::String(pg::product::kEnvPrefix) + "folder-probe-"
                                                 + juce::String(juce::Time::currentTimeMillis()));
    tempDir.deleteRecursively();
    const fs::path root = fs::path(tempDir.getFullPathName().toStdString()) / "Folders";

    Fake fake;
    fake.draft.files = { { "program.fm", program("float a = 1.0f;") }, { "lib.fm", kLib } };
    double t = 1000.0;
    std::string why;

    // ---- START ---------------------------------------------------------------------------------------------------------
    pg::FolderSync sync(fake.plugin());
    P.eq("start.made", sync.start(root, "Synth 1", t, why) && sync.on() && sync.name() == "Synth 1" ? 1 : 0, 1);
    const fs::path dir = root / "Synth 1";
    passFor(sync, t, 0.5);
    P.eq("start.files", readFile(dir / "program.fm") == fake.draft.files[0].text && readFile(dir / "lib.fm") == kLib ? 1 : 0, 1);
    P.eq("start.host_copy", readFile(dir / "host.fm") == std::string(pg::Patch::hostSource()) ? 1 : 0, 1);
    P.eq("start.host_read_only", readOnly(dir / "host.fm") ? 1 : 0, 1);
    P.eq("start.note", readFile(dir / pg::FolderSync::kNote).find("synced both ways") != std::string::npos ? 1 : 0, 1);
    P.eq("start.vscode", readFile(dir / ".vscode" / "settings.json").find("files.readonlyFromPermissions") != std::string::npos
                             ? 1 : 0, 1);
    P.eq("start.manifest", readFile(dir / pg::FolderSync::kManifest).rfind("fmsync 1", 0) == 0 ? 1 : 0, 1);
    P.eq("start.no_draft_write", fake.writes, 0);
    P.eq("start.state", sync.state("x").on && sync.state("x").conflicts.empty() ? 1 : 0, 1);

    // ---- OUT -----------------------------------------------------------------------------------------------------------
    const std::string typed = program("float a = 2.0f;");
    fake.set("program.fm", typed);
    passFor(sync, t, 0.1);
    P.eq("out.rests", readFile(dir / "program.fm") != typed ? 1 : 0, 1);
    passFor(sync, t, 0.6);
    P.eq("out.written", readFile(dir / "program.fm") == typed ? 1 : 0, 1);
    P.eq("out.no_temp", tempLeft(dir) ? 0 : 1, 1);
    passFor(sync, t, 0.5);
    P.eq("out.own_write_not_read", fake.writes, 0);
    {
        // Typing that never rests: a change every 100 ms for 2.5 s, the passes the timer's own (no look forced, the
        // next look kWatchedSeconds off while watched): written within kMaxWaitSeconds of the first change.
        const double from = t;
        double firstWrite = -1.0;
        for (int k = 0; k < 25; ++k)
        {
            fake.set("program.fm", program("float a = " + std::to_string(k) + ".5f;"));
            for (const double end = t + 0.1; t < end; t += 0.05)
            {
                sync.pass(t);
                if (firstWrite < 0.0 && readFile(dir / "program.fm") != typed)
                    firstWrite = t - from;
            }
        }
        std::printf("out: typing first written after %.2f s\n", firstWrite);
        P.in("out.max_wait_s", firstWrite, 0.0, pg::FolderSync::kMaxWaitSeconds + 0.1);
        passFor(sync, t, 0.6);
        P.eq("out.rested_last", readFile(dir / "program.fm") == *fake.text("program.fm") ? 1 : 0, 1);
        P.eq("out.typing_not_read", fake.writes, 0);
    }

    // ---- IN ------------------------------------------------------------------------------------------------------------
    const std::string saved = program("float a = 3.0f;");
    writeFile(dir / "program.fm", saved);
    passFor(sync, t, 0.05);
    P.eq("in.settles", *fake.text("program.fm") != saved ? 1 : 0, 1);
    passFor(sync, t, 0.5);
    P.eq("in.lands", *fake.text("program.fm") == saved ? 1 : 0, 1);
    P.eq("in.one_write", fake.writes, 1);
    P.eq("in.landed", sync.state("").landed == "program.fm" && sync.state("").landedCount == 1 ? 1 : 0, 1);
    P.eq("in.no_run", fake.runs, 0);
    passFor(sync, t, 1.0);
    P.eq("in.not_written_back", fake.writes == 1 && readFile(dir / "program.fm") == saved ? 1 : 0, 1);
    sync.setRunOnSave(true);
    writeFile(dir / "program.fm", program("float a = 4.0f;"));
    writeFile(dir / "lib.fm", "float twice(float x) { return x + x; }\n");
    passFor(sync, t, 0.5);
    P.eq("in.two_files_one_write", fake.writes, 2);
    P.eq("in.run_on_save", fake.runs, 1);
    sync.setRunOnSave(false);

    // ---- ATOMIC --------------------------------------------------------------------------------------------------------
    {
        const std::string atomic = program("float a = 5.0f;");
        writeAtomic(dir, "program.fm", atomic);
        passFor(sync, t, 0.5);
        P.eq("atomic.rename_over", *fake.text("program.fm") == atomic ? 1 : 0, 1);
        // JetBrains' safe write: the new text beside, the old moved aside, the new renamed in, the old deleted.
        const std::string jb = program("float a = 6.0f;");
        const int writes = fake.writes;
        writeFile(dir / "program.fm___jb_tmp___", jb);
        fs::rename(dir / "program.fm", dir / "program.fm___jb_old___");
        passFor(sync, t, 0.05);
        const bool kept = fake.text("program.fm") != nullptr;
        fs::rename(dir / "program.fm___jb_tmp___", dir / "program.fm");
        fs::remove(dir / "program.fm___jb_old___");
        passFor(sync, t, 0.5);
        P.eq("atomic.mid_save_kept", kept ? 1 : 0, 1);
        P.eq("atomic.jetbrains", *fake.text("program.fm") == jb && fake.writes == writes + 1 ? 1 : 0, 1);
        P.eq("atomic.no_stray", static_cast<int64_t>(fake.draft.files.size()), 2);
        // A save in two parts, the second within the folder's settle: only the whole lands.
        const std::string whole = program("float a = 7.0f; float b = 8.0f;");
        const std::size_t before = fake.landed.size();
        writeFile(dir / "program.fm", whole.substr(0, whole.size() / 2));
        passFor(sync, t, 0.1);
        writeFile(dir / "program.fm", whole);
        passFor(sync, t, 0.5);
        bool half = false;
        for (std::size_t k = before; k < fake.landed.size(); ++k)
            half = half || fake.landed[k] == whole.substr(0, whole.size() / 2);
        P.eq("atomic.whole_only", !half && *fake.text("program.fm") == whole ? 1 : 0, 1);
    }

    // ---- SWAP ----------------------------------------------------------------------------------------------------------
    {
        const int writes = fake.writes;
        for (const char* n : { ".program.fm.swp", "program.fm~", "#program.fm#", ".#program.fm", "4913", "program.fm.crswap" })
            writeFile(dir / n, "junk");
        passFor(sync, t, 0.5);
        P.eq("swap.ignored", fake.writes == writes && fake.draft.files.size() == 2u ? 1 : 0, 1);
        for (const char* n : { ".program.fm.swp", "program.fm~", "#program.fm#", ".#program.fm", "4913", "program.fm.crswap" })
            fs::remove(dir / n);
    }

    // ---- FOLDER --------------------------------------------------------------------------------------------------------
    {
        writeFile(dir / "extra.fm", "// extra\n");
        passFor(sync, t, 0.5);
        P.eq("folder.add", fake.index("extra.fm"), 2);
        fake.draft.active = 2;                              // its tab shown
        fs::rename(dir / "extra.fm", dir / "more.fm");
        passFor(sync, t, 0.5);
        P.eq("folder.rename_keeps_place", fake.index("more.fm") == 2 && fake.index("extra.fm") < 0 ? 1 : 0, 1);
        P.eq("folder.rename_keeps_tab", fake.draft.active, 2);
        fs::remove(dir / "more.fm");
        passFor(sync, t, 0.5);
        P.eq("folder.delete", fake.index("more.fm") < 0 && fake.draft.files.size() == 2u ? 1 : 0, 1);
        const std::string programText = *fake.text("program.fm");
        fs::remove(dir / "program.fm");
        passFor(sync, t, 0.5);
        P.eq("folder.program_stays", fake.index("program.fm") == 0 && readFile(dir / "program.fm") == programText ? 1 : 0, 1);
        P.eq("folder.program_noted", noted(sync.state(""), "the program") ? 1 : 0, 1);
        // Renamed there, the program is renamed in the draft (in its place), as the plugin itself allows; and back.
        fs::rename(dir / "program.fm", dir / "main.fm");
        passFor(sync, t, 0.5);
        P.eq("folder.program_renamed", fake.index("main.fm") == 0 && fake.index("program.fm") < 0 ? 1 : 0, 1);
        fs::rename(dir / "main.fm", dir / "program.fm");
        passFor(sync, t, 0.5);
        P.eq("folder.program_renamed_back", fake.index("program.fm") == 0 && *fake.text("program.fm") == programText ? 1 : 0, 1);
        const int writes = fake.writes;
        writeFile(dir / "my file.fm", "// a space\n");
        writeFile(dir / "notes.txt", "mine\n");
        passFor(sync, t, 0.5);
        P.eq("folder.bad_name", fake.index("my file.fm") < 0 && noted(sync.state(""), "not a valid name") ? 1 : 0, 1);
        P.eq("folder.other_ext", fake.index("notes.txt") < 0 && !noted(sync.state(""), "notes.txt") && fake.writes == writes
                                     ? 1 : 0, 1);
        fs::remove(dir / "my file.fm");
        passFor(sync, t, 0.5);
        P.eq("folder.note_goes", noted(sync.state(""), "not a valid name") ? 0 : 1, 1);
        // A shader's .sh file is the program's, as a .sc is.
        writeFile(dir / "gauge.sh", "// shared shader functions\n");
        passFor(sync, t, 0.5);
        P.eq("folder.shader_header", fake.index("gauge.sh") == 2 ? 1 : 0, 1);
        fs::remove(dir / "gauge.sh");
        passFor(sync, t, 0.5);
        P.eq("folder.shader_header_goes", fake.index("gauge.sh") < 0 && fake.draft.files.size() == 2u ? 1 : 0, 1);
    }

    // ---- PLUGIN --------------------------------------------------------------------------------------------------------
    {
        fake.set("p2.fm", "// p2\n");
        passFor(sync, t, 0.6);
        P.eq("plugin.add", readFile(dir / "p2.fm") == "// p2\n" ? 1 : 0, 1);
        fake.draft.files.back().name = "p3.fm";
        passFor(sync, t, 0.6);
        P.eq("plugin.rename", !there(dir / "p2.fm") && readFile(dir / "p3.fm") == "// p2\n" ? 1 : 0, 1);
        fake.draft.files.pop_back();
        passFor(sync, t, 0.6);
        P.eq("plugin.delete", !there(dir / "p3.fm") ? 1 : 0, 1);
    }

    // ---- CONFLICT ------------------------------------------------------------------------------------------------------
    {
        const std::string mine = program("float a = 10.0f;"), theirs = program("float a = 11.0f;");
        fake.set("program.fm", mine);
        writeFile(dir / "program.fm", theirs);
        passFor(sync, t, 0.8);
        const pg::FolderState s = sync.state("");
        P.eq("conflict.listed", s.conflicts.size() == 1u && s.conflicts[0] == "program.fm" ? 1 : 0, 1);
        P.eq("conflict.held", *fake.text("program.fm") == mine && readFile(dir / "program.fm") == theirs ? 1 : 0, 1);
        P.eq("conflict.keep_plugin", sync.resolve("program.fm", true, t, why) && readFile(dir / "program.fm") == mine ? 1 : 0, 1);
        P.eq("conflict.folder_kept_aside", keptAside(dir, theirs) ? 1 : 0, 1);
        P.eq("conflict.cleared", sync.state("").conflicts.empty() ? 1 : 0, 1);
        passFor(sync, t, 0.5);
        const std::string mine2 = program("float a = 12.0f;"), theirs2 = program("float a = 13.0f;");
        fake.set("lib.fm", "// mine\n" + std::string(kLib));
        writeFile(dir / "lib.fm", "// theirs\n" + std::string(kLib));
        passFor(sync, t, 0.8);
        const int writes = fake.writes;
        P.eq("conflict.keep_folder", sync.resolve("lib.fm", false, t, why) && *fake.text("lib.fm") == "// theirs\n" + std::string(kLib)
                                         && fake.writes == writes + 1 ? 1 : 0, 1);
        P.eq("conflict.plugin_kept_aside", keptAside(dir, "// mine\n" + std::string(kLib)) ? 1 : 0, 1);
        P.eq("conflict.none_left", sync.resolve("lib.fm", true, t, why) ? 0 : 1, 1);
        // Both sides changed, then made the same by hand: no longer a conflict.
        fake.set("program.fm", mine2);
        writeFile(dir / "program.fm", theirs2);
        passFor(sync, t, 0.8);
        P.eq("conflict.again", static_cast<int64_t>(sync.state("").conflicts.size()), 1);
        writeFile(dir / "program.fm", mine2);
        passFor(sync, t, 0.5);
        P.eq("conflict.ended_by_hand", sync.state("").conflicts.empty() ? 1 : 0, 1);
        // A conflict whose folder side then grows past 1 MB: no deletion. Keeping the folder's is refused (the draft
        // as it was); keeping the plugin's keeps the large file aside whole before writing over it.
        const std::string mineLib = "// mine again\n" + std::string(kLib);
        fake.set("lib.fm", mineLib);
        writeFile(dir / "lib.fm", "// theirs again\n" + std::string(kLib));
        passFor(sync, t, 0.8);
        const std::string large = "// large\n" + std::string(pg::FolderSync::kMaxBytes, '/') + "\n";
        writeFile(dir / "lib.fm", large);
        passFor(sync, t, 0.5);
        const pg::Draft held = fake.draft;
        P.eq("conflict.large_listed", sync.state("").conflicts.size() == 1u && noted(sync.state(""), "over 1 MB") ? 1 : 0, 1);
        P.eq("conflict.large_keep_folder_refused", !sync.resolve("lib.fm", false, t, why) && fake.draft == held
                                                       && sync.state("").conflicts.size() == 1u ? 1 : 0, 1);
        P.eq("conflict.large_keep_plugin", sync.resolve("lib.fm", true, t, why) && readFile(dir / "lib.fm") == mineLib
                                               && keptAside(dir, large) ? 1 : 0, 1);
        passFor(sync, t, 0.5);
    }

    // ---- GUARD ---------------------------------------------------------------------------------------------------------
    {
        const std::string g = program("float a = 14.0f;");
        writeFile(dir / "program.fm", g);
        fake.refuse = true;
        const int refused = fake.refused;
        passFor(sync, t, 0.5);
        P.eq("guard.refused_then_lands", fake.refused == refused + 1 && *fake.text("program.fm") == g ? 1 : 0, 1);
    }

    // ---- INCLUDES ------------------------------------------------------------------------------------------------------
    {
        const std::string lib = pg::kLibrary[0].name;
        const std::string withLib = "#include \"" + lib + "\"\n" + *fake.text("program.fm");
        fake.set("program.fm", withLib);
        passFor(sync, t, 0.8);
        P.eq("includes.library_copy", readFile(dir / lib) == pg::kLibrary[0].source && readOnly(dir / lib) ? 1 : 0, 1);
        P.eq("includes.copy_not_taken", fake.index(lib) < 0 ? 1 : 0, 1);
        std::error_code ec;
        fs::permissions(dir / "host.fm", fs::perms::owner_write, fs::perm_options::add, ec);
        writeFile(dir / "host.fm", "// my host\n");
        passFor(sync, t, 0.5);
        P.eq("includes.host_restored", readFile(dir / "host.fm") == std::string(pg::Patch::hostSource()) && readOnly(dir / "host.fm")
                                           ? 1 : 0, 1);
        P.eq("includes.host_kept_aside", keptAside(dir, "// my host\n") ? 1 : 0, 1);
        P.eq("includes.host_not_taken", fake.index("host.fm") < 0 ? 1 : 0, 1);
        fake.set(lib, pg::kLibrary[0].source);              // LIBRARY adds it
        passFor(sync, t, 0.8);
        P.eq("includes.added_writable", !readOnly(dir / lib) ? 1 : 0, 1);
        const std::string edited = std::string(pg::kLibrary[0].source) + "// edited\n";
        writeFile(dir / lib, edited);
        passFor(sync, t, 0.5);
        P.eq("includes.added_edits_land", *fake.text(lib) == edited ? 1 : 0, 1);
    }

    // ---- MANY ----------------------------------------------------------------------------------------------------------
    {
        Fake other;
        other.draft.files = { { "program.fm", program("float b = 1.0f;") } };
        pg::FolderSync second(other.plugin());
        P.eq("many.second_folder", second.start(root, "Synth 1", t, why) && second.name() == "Synth 1 2" ? 1 : 0, 1);
        double t2 = t;
        passFor(second, t2, 0.5);
        P.eq("many.apart", readFile(root / "Synth 1 2" / "program.fm") == other.draft.files[0].text
                               && readFile(dir / "program.fm") == *fake.text("program.fm") ? 1 : 0, 1);
        second.stop();
    }

    // ---- RESTART -------------------------------------------------------------------------------------------------------
    {
        sync.stop();
        P.eq("restart.off", sync.on() ? 0 : 1, 1);
        Fake third;
        pg::FolderSync taker(third.plugin());
        third.draft.files = { { "program.fm", "// x\n" } };
        P.eq("restart.name_free", taker.start(root, "Synth 1", t, why) && taker.name() == "Synth 1" ? 1 : 0, 1);
        taker.stop();
        // `taker` wrote its own files there meanwhile: started again on the first draft, the folder follows it back.
        const std::string away = program("float a = 15.0f;");
        P.eq("restart.on", sync.start(root, "Synth 1", t, why) ? 1 : 0, 1);
        passFor(sync, t, 0.8);
        P.eq("restart.follows_plugin", readFile(dir / "program.fm") == *fake.text("program.fm") ? 1 : 0, 1);
        sync.stop();
        writeFile(dir / "program.fm", away);                // edited while nothing synced
        sync.start(root, "Synth 1", t, why);
        passFor(sync, t, 0.5);
        P.eq("restart.edit_lands", *fake.text("program.fm") == away ? 1 : 0, 1);
        sync.stop();
        writeFile(dir / "program.fm", program("float a = 16.0f;"));
        fake.set("program.fm", program("float a = 17.0f;"));
        sync.start(root, "Synth 1", t, why);
        passFor(sync, t, 0.8);
        P.eq("restart.both_conflict", sync.state("").conflicts.size() == 1u && *fake.text("program.fm") == program("float a = 17.0f;")
                                          && readFile(dir / "program.fm") == program("float a = 16.0f;") ? 1 : 0, 1);
        (void)sync.resolve("program.fm", true, t, why);
        passFor(sync, t, 0.5);
    }

    // ---- PROJECT -------------------------------------------------------------------------------------------------------
    {
        const std::string libName = pg::kLibrary[0].name;
        const std::string pending = "// saved just before\n" + *fake.text(libName);
        writeFile(dir / libName, pending);                  // a save the sync has not taken yet...
        fake.draft.files = { { "program.fm", program("float c = 1.0f;") }, { "b.fm", "// b\n" } };   // ...a project opened
        fake.draft.active = 0;
        passFor(sync, t, 0.8);
        P.eq("project.follows", readFile(dir / "program.fm") == fake.draft.files[0].text && readFile(dir / "b.fm") == "// b\n"
                                    && !there(dir / "lib.fm") ? 1 : 0, 1);
        P.eq("project.save_not_lost", readFile(dir / libName) == pending && sync.state("").conflicts.size() == 1u ? 1 : 0, 1);
        (void)sync.resolve(libName, true, t, why);
        passFor(sync, t, 0.5);
        P.eq("project.resolved", !there(dir / libName) && keptAside(dir, pending) ? 1 : 0, 1);
    }

    // ---- CRLF ----------------------------------------------------------------------------------------------------------
    {
        writeFile(dir / "b.fm", "// one\r\n// two\r\n");
        passFor(sync, t, 1.0);
        P.eq("crlf.lf", *fake.text("b.fm") == "// one\n// two\n" ? 1 : 0, 1);
        P.eq("crlf.not_bounced", readFile(dir / "b.fm") == "// one\r\n// two\r\n" ? 1 : 0, 1);
    }

    // ---- SWITCH (a project with its own file of a copy's name) ---------------------------------------------------------
    {
        const pg::Draft before = fake.draft;
        const std::string lib = pg::kLibrary[0].name, stockLib = pg::kLibrary[0].source;
        const std::string stockHost(pg::Patch::hostSource());
        // Project A names the library file without having it: the folder's copies, read only.
        pg::Draft a;
        a.files = { { "program.fm", "#include \"" + lib + "\"\n" + program("float s = 1.0f;") } };
        fake.draft = a;
        passFor(sync, t, 0.8);
        P.eq("switch.copies", readFile(dir / lib) == stockLib && readOnly(dir / lib) && readOnly(dir / "host.fm") ? 1 : 0, 1);
        // Project B has its own, edited: the plugin's change over the copies as written, no conflict.
        const std::string ownLib = "// mine\n" + stockLib, ownHost = stockHost + "// mine\n";
        pg::Draft b;
        b.files = { a.files[0], { lib, ownLib }, { "host.fm", ownHost } };
        const int writes = fake.writes;
        fake.draft = b;
        passFor(sync, t, 0.8);
        P.eq("switch.own_no_conflict", sync.state("").conflicts.empty() ? 1 : 0, 1);
        P.eq("switch.own_written", readFile(dir / lib) == ownLib && !readOnly(dir / lib) && readFile(dir / "host.fm") == ownHost
                                       && !readOnly(dir / "host.fm") ? 1 : 0, 1);
        P.eq("switch.own_draft_untouched", fake.draft == b && fake.writes == writes ? 1 : 0, 1);
        // Back to A: the copies written again quietly (nothing kept aside, no note), read only.
        const auto aside = [&dir] {
            std::error_code ec;
            int n = 0;
            for (fs::directory_iterator it(dir / pg::FolderSync::kConflicts, ec), end; !ec && it != end; it.increment(ec))
                ++n;
            return n;
        };
        const int asideBefore = aside();
        fake.draft = a;
        passFor(sync, t, 0.8);
        P.eq("switch.back_copies", readFile(dir / lib) == stockLib && readOnly(dir / lib) && readFile(dir / "host.fm") == stockHost
                                       && readOnly(dir / "host.fm") ? 1 : 0, 1);
        P.eq("switch.back_quiet", aside() == asideBefore && !noted(sync.state(""), "written again") ? 1 : 0, 1);
        fake.draft = before;
        passFor(sync, t, 0.8);
        P.eq("switch.restored", readFile(dir / "program.fm") == before.files[0].text && !there(dir / lib)
                                    && sync.state("").conflicts.empty() ? 1 : 0, 1);
    }

    // ---- ROOM (16 files at most) ---------------------------------------------------------------------------------------
    {
        while (fake.draft.files.size() < pg::kMaxFiles)
            fake.draft.files.push_back({ "f" + std::to_string(fake.draft.files.size()) + ".fm", "// f\n" });
        passFor(sync, t, 0.8);
        writeFile(dir / "one-more.fm", "// 17\n");
        passFor(sync, t, 0.5);
        P.eq("folder.no_room", fake.index("one-more.fm") < 0 && noted(sync.state(""), "no room") ? 1 : 0, 1);
        fs::remove(dir / "one-more.fm");
        fake.draft.files.resize(2);
        passFor(sync, t, 0.8);
    }

    // ---- GONE ----------------------------------------------------------------------------------------------------------
    {
        const pg::Draft before = fake.draft;
        std::error_code ec;
        fs::remove_all(dir, ec);
        passFor(sync, t, 0.8);
        P.eq("gone.made_again", readFile(dir / "program.fm") == before.files[0].text && readFile(dir / "b.fm") == *fake.text("b.fm")
                                    ? 1 : 0, 1);
        P.eq("gone.draft_untouched", fake.draft == before ? 1 : 0, 1);
    }

    // ---- WATCH ---------------------------------------------------------------------------------------------------------
#if defined(__APPLE__)
    {
        P.eq("watch.watching", sync.watching() ? 1 : 0, 1);
        const auto clock = [] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        double now = clock();
        for (int k = 0; k < 10; ++k, now = clock())          // a look, then a while with no look due
        {
            sync.pass(now + 1000.0, k == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const std::string watched = program("float w = 1.0f;");
        writeFile(dir / "program.fm", watched);
        const double from = clock();
        double landedAfter = -1.0;
        while (clock() - from < 2.5 && landedAfter < 0.0)
        {
            sync.pass(clock() + 1000.0);
            if (*fake.text("program.fm") == watched)
                landedAfter = clock() - from;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::printf("watch: landed after %.3f s\n", landedAfter);
        P.in("watch.prompt_s", landedAfter, 0.0, 1.5);
    }
#endif
    sync.stop();

#if !defined(_WIN32)
    // ---- LAUNCH --------------------------------------------------------------------------------------------------------
    {
        // A folder named as a shell would run it: had a shell come between, it would make the markers (in this
        // process's folder, the program's too) and the program would get some other path.
        const std::string marker = std::string(pg::product::kEnvPrefix) + "launch-marker-" + std::to_string(::getpid());
        const std::string odd = "a$(touch " + marker + ")`touch " + marker + "-2` \"q\" 'r'; echo $HOME";
        const fs::path launchDir = fs::path(tempDir.getFullPathName().toStdString()) / "Launch";
        const fs::path folder = launchDir / odd;
        std::error_code ec;
        fs::create_directories(folder, ec);
        const fs::path here = fs::current_path(ec);
        P.eq("launch.opener_args", pg::launch::openerArgs(folder.string()) == std::vector<std::string>{ "xdg-open", folder.string() }
                                       ? 1 : 0, 1);
        const fs::path opened = folder / "opened";
        why.clear();
        const bool started = pg::launch::detached({ "touch", opened.string() }, why);
        bool arrived = false;
        for (int i = 0; i < 500 && !arrived; ++i)
        {
            arrived = there(opened);
            if (!arrived)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::printf("launch: started %d (%s), arrived %d\n", started ? 1 : 0, why.c_str(), arrived ? 1 : 0);
        P.eq("launch.argument_whole", started && arrived ? 1 : 0, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        P.eq("launch.no_shell", !there(here / marker) && !there(here / (marker + "-2")) && !there(launchDir / marker) ? 1 : 0, 1);
        // A program that runs on: the call is back long before it ends, and neither it nor the intermediate child is
        // left this process's (nothing to wait for, no zombie).
        const auto from = std::chrono::steady_clock::now();
        why.clear();
        const bool slow = pg::launch::detached({ "sleep", "1" }, why);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
        std::printf("launch: sleep started %d in %.3f s\n", slow ? 1 : 0, took);
        P.eq("launch.returns_at_once", slow && took < 0.5 ? 1 : 0, 1);
        errno = 0;
        P.eq("launch.no_child_left", ::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD ? 1 : 0, 1);
        why.clear();
        P.eq("launch.not_found", !pg::launch::detached({ std::string(pg::product::kEnvPrefix) + "no-such-program" }, why)
                                     && why.find("not found") != std::string::npos ? 1 : 0, 1);
        // An executable file that is no program (no #!, no binary format): found, but its exec fails, and says so.
        const fs::path junk = launchDir / "not-a-program";
        writeFile(junk, "this is no program\n");
        fs::permissions(junk, fs::perms::owner_all, fs::perm_options::replace, ec);
        why.clear();
        P.eq("launch.exec_fails", !pg::launch::detached({ junk.string() }, why) && why.find("could not start") != std::string::npos
                                      ? 1 : 0, 1);
        std::printf("launch: exec failure says \"%s\"\n", why.c_str());
    }
#endif

    // ---- PORT ----------------------------------------------------------------------------------------------------------
    {
        namespace ps = pg::projectstore;
        ps::setDirectoryForTests(tempDir.getChildFile("projects"));
        pg::Processor a;
        pg::EditorPort& port = a.editorPort();
        P.eq("port.off_at_first", port.folder().on ? 0 : 1, 1);
        pg::Draft d;
        d.files = { { "program.fm", program("param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];") },
                    { "lib.fm", kLib } };
        port.setDraft(d);
        port.run(d.files);
        a.settle();
        pg::SavedVersion savedVersion;
        P.eq("port.project_saved", port.saveVersion("Glass Bloom", savedVersion, why) ? 1 : 0, 1);
        P.eq("port.on", port.setFolderSync(true, why) && port.folder().on ? 1 : 0, 1);
        const pg::FolderState s = port.folder();
        P.eq("port.project_name", s.name == "Glass Bloom" ? 1 : 0, 1);
        const fs::path pdir = fs::path(tempDir.getChildFile("projects").getChildFile("Folders").getFullPathName().toStdString())
                            / "Glass Bloom";
        const std::string shownEnd = "/Folders/Glass Bloom";
        P.eq("port.path_shown", s.path.size() > shownEnd.size() && s.path.substr(s.path.size() - shownEnd.size()) == shownEnd
                                    && (s.path[0] == '~' || s.path[0] == '/') ? 1 : 0, 1);
        const auto pollFor = [&a](double seconds) {
            const auto from = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count() < seconds)
            {
                a.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        };
        pollFor(0.6);
        P.eq("port.files_out", readFile(pdir / "program.fm") == d.files[0].text ? 1 : 0, 1);
        const uint64_t rev = port.draftRevision();
        port.setFolderRunOnSave(true);
        const std::string edited = program("param float level [[fminus::range(0, 1)]] [[fminus::default(0.25)]];");
        writeFile(pdir / "program.fm", edited);
        pollFor(1.0);
        a.settle();
        P.eq("port.edit_lands", port.draft().files[0].text == edited && port.draftRevision() > rev ? 1 : 0, 1);
        P.eq("port.run_on_save", port.running().size() == 2u && port.running()[0].text == edited ? 1 : 0, 1);
        P.eq("port.landed_count", static_cast<int64_t>(port.folder().landedCount), 1);
        // The launcher: VS Code and the Finder, with the folder's path.
        std::vector<std::pair<pg::FolderApp, std::string>> launched;
        a.setFolderLauncher([&launched](pg::FolderApp app, const std::string& path, std::string&) {
            launched.emplace_back(app, path);
            return true;
        });
        P.eq("port.vscode", port.openFolder(pg::FolderApp::vscode, why) && launched.size() == 1u
                                && launched[0].first == pg::FolderApp::vscode && fs::path(launched[0].second) == pdir ? 1 : 0, 1);
        P.eq("port.finder", port.openFolder(pg::FolderApp::files, why) && launched.size() == 2u
                                && launched[1].first == pg::FolderApp::files ? 1 : 0, 1);
        a.setFolderLauncher(nullptr);
        why.clear();
        P.eq("port.nothing_launched", !port.openFolder(pg::FolderApp::vscode, why) && why.find("probe") != std::string::npos ? 1 : 0, 1);
        // The session keeps it: a processor that loads it syncs with the same folder once this one let it go.
        juce::MemoryBlock state;
        a.getStateInformation(state);
        P.eq("port.off", port.setFolderSync(false, why) && !port.folder().on && there(pdir / "program.fm") ? 1 : 0, 1);
        pg::Processor b;
        b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        b.poll();
        const pg::FolderState sb = b.editorPort().folder();
        P.eq("port.session_resumes", sb.on && sb.name == "Glass Bloom" && sb.runOnSave ? 1 : 0, 1);
        // A state with another folder's name: the folder left is left as it was (never given the state's files).
        {
            pg::Processor c;
            pg::Draft other;
            other.files = { { "program.fm", program("float other = 1.0f;") } };
            c.editorPort().setDraft(other);
            pg::SavedVersion v;
            (void)c.editorPort().saveVersion("Other", v, why);
            (void)c.editorPort().setFolderSync(true, why);
            juce::MemoryBlock otherState;
            c.getStateInformation(otherState);
            (void)c.editorPort().setFolderSync(false, why);
            const std::string kept = readFile(pdir / "program.fm");
            b.setStateInformation(otherState.getData(), static_cast<int>(otherState.getSize()));
            b.poll();
            P.eq("port.state_switches", b.editorPort().folder().name == "Other" ? 1 : 0, 1);
            P.eq("port.left_as_it_was", readFile(pdir / "program.fm") == kept && kept == edited ? 1 : 0, 1);
        }
        juce::MemoryBlock none;
        b.editorPort().setFolderSync(false, why);
        b.getStateInformation(none);
        P.eq("port.session_off", none.toString().contains("FOLDER") ? 0 : 1, 1);
        ps::setDirectoryForTests({});
    }

    tempDir.deleteRecursively();
    return P.finish();
}
