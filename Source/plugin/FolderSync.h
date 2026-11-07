// Source/plugin/FolderSync.h: the program's files mirrored to a folder on disk, for VS Code or any editor, synced both
// ways (JUCE-free; message thread only). The processor holds one (EditorPort::folder); the probes drive one over a
// draft of their own.
//
// The folder holds a file a draft file, by its name, and what the program's #includes read that is not one of them,
// as read-only copies: host.fm, the file the plugin provides, and a library file named by an #include but not added
// (the plugin compiles only what it has: LIBRARY adds it). A copy as written is what both sides agreed on: a draft
// file of its name (a project with its own host.fm or library file) is the plugin's change, written over it, writable;
// a draft file that becomes a copy is written over quietly when the folder's is as both last agreed (else it is kept
// in .conflicts/ first). A note (ABOUT.md) says so, and .vscode/settings.json makes VS Code open the copies read-only
// (written once each, when missing). The sync's own files start with a dot: the
// manifest (.fmsync: what each file was when both sides last agreed, a hash each, and the copies written), the lock
// (.fmsync-lock), the temporary files a write renames over its file, and .conflicts/ (below).
//
// Each pass (the processor's timer, 20 Hz) compares, file by file, the draft, the folder and what they last agreed on
// (the base):
//   - both the same: they agree (the base moves);
//   - the folder unchanged since: the plugin changed it (typing, Claude, a project opened, a state loaded): written to
//     the folder once the draft has rested (kRestSeconds, at most kMaxWaitSeconds), a deleted file removed, a new
//     one written, each write a temporary file renamed over the old (never a half-written file);
//   - the draft unchanged since: the folder changed it (a save): its text lands in the draft, every file of one
//     save in one write, version-guarded (Plugin::setDraftIf: only while the draft is still what the pass read, else
//     the next pass decides again), so an open editor merges it as one undo step; a new file is appended, a file
//     renamed (the program too) keeps its place (its tab, still shown if it was), a file deleted goes (never the
//     program, the first: it is written back); with run on save, the files then run;
//   - both changed: a conflict. The file is held (neither side written) until the user chooses (resolve), and the
//     side not kept is kept in the folder's .conflicts/ ("program.folder-20261106-140211.fm"): nothing is lost.
// The folder's changes are taken only once its listing (each file's name, size, time and identity) has held still for
// kSettleSeconds, so an editor's save in steps (a temporary file renamed over, the old one moved aside first, a write
// in place) is read once whole; and a write of the plugin's goes only right after a look that found the file as last
// seen, so a save the sync has not taken yet is never overwritten. The sync's own writes come back as the text both
// sides agree on: nothing to take. Names the folder takes: a draft file's, else a valid name (Sources.h) with an
// extension the editor knows (.fm, .gui, .cir, .sc, .sh); anything else is not the program's (an editor's swap and
// backup files: .x.swp, x~, #x#, 4913, x___jb_tmp___), and a new file with a known extension but a name the plugin
// cannot take, or past kMaxFiles, is left there, with a note. The text taken is the editor's (TextEditor::sanitise:
// CR LF as LF, printable ASCII and tabs), as Claude's writes are.
//
// One folder, one instance: start() locks it (.fmsync-lock: flock, released when the process ends), and a folder
// another instance holds is passed over for "<name> 2", "<name> 3", ...; the manifest lets a folder edited while no
// instance synced it be taken back three-way when one starts again (an edit in the folder lands, an edit on both
// sides is a conflict). The folder is the instance's: a project opened, a state loaded or an example started change
// the draft, and the folder follows as for any edit of the plugin's (a save in the folder the sync has not taken by
// then is a conflict, never lost).
#pragma once

#include "plugin/EditorPort.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace playground
{
    class FolderWatch;

    class FolderSync
    {
    public:
        // What the sync reaches of its instance: the processor's draft, a version-guarded write of it, and RUN.
        struct Plugin
        {
            std::function<Draft()> draft;
            // `next` becomes the draft only while the draft is still `now` (false: nothing set)
            std::function<bool(const Draft& now, const Draft& next)> setDraftIf;
            std::function<void(const Sources& files)> run;
        };

        static constexpr double kSettleSeconds = 0.15;   // the folder's listing still this long before it is read
        static constexpr double kRestSeconds = 0.3;      // the draft still this long before it is written out
        static constexpr double kMaxWaitSeconds = 1.5;   // ... or at most this long after it first changed
        static constexpr double kPollSeconds = 0.5;      // the folder looked at, with no watcher
        static constexpr double kWatchedSeconds = 3.0;   // ... and with one (in case it misses an event)
        static constexpr double kEventSeconds = 30.0;    // an event's note shows this long
        static constexpr std::size_t kMaxBytes = 1 << 20;   // a larger file in the folder is not taken
        static constexpr const char* kManifest = ".fmsync";
        static constexpr const char* kLock = ".fmsync-lock";
        static constexpr const char* kConflicts = ".conflicts";
        static constexpr const char* kNote = "ABOUT.md";

        explicit FolderSync(Plugin plugin);
        ~FolderSync();
        FolderSync(const FolderSync&) = delete;
        FolderSync& operator=(const FolderSync&) = delete;

        // Mirror into root/name (made when missing; `name` tidied for a folder's name): a folder another instance
        // holds is passed over for "name 2", "name 3", ... The first pass runs at `now`. False, with why, when no
        // folder could be made or locked. A folder synced before is let go first, as it is (stop(false)).
        bool start(const std::filesystem::path& root, const std::string& name, double now, std::string& why);
        // Stop: what the plugin changed and the folder did not is written first (unless `flush` is false: start()'s,
        // which leaves the folder it leaves as it was, a state loaded with another folder's name having brought files
        // that are not that folder's); the folder stays as it is.
        void stop(bool flush = true);
        bool on() const noexcept { return on_; }
        const std::string& name() const noexcept { return name_; }
        const std::filesystem::path& folder() const noexcept { return folder_; }
        bool watching() const noexcept;

        // A pass at `now` (seconds, any clock that only goes forward). `force`: look at the folder now (else when the
        // watcher says something changed, or every kPollSeconds, kWatchedSeconds while watched).
        void pass(double now, bool force = false);

        // A conflict settled: `keepPlugin`, the draft's text is written to the folder (the folder's file kept whole in
        // .conflicts/); else the folder's lands in the draft (the draft's kept there). False, with why, when `file`
        // is in no conflict, the draft moved meanwhile (try again), the folder's file could not be kept aside, or the
        // folder's is to be kept but cannot be taken (over kMaxBytes, unreadable: never taken as a deletion).
        bool resolve(const std::string& file, bool keepPlugin, double now, std::string& why);

        void setRunOnSave(bool on);
        bool runOnSave() const noexcept { return runOnSave_; }

        // What it shows the user (EditorPort::FolderState), its path as the caller gives it.
        FolderState state(const std::string& shownPath) const;
        uint64_t revision() const noexcept { return revision_; }

        // The tidy folder name for `name` ("" when nothing is left of it).
        static std::string tidyFolderName(const std::string& name);
        // A text's hash, as the manifest keeps it (FNV-1a, 64 bits).
        static uint64_t hashOf(std::string_view text) noexcept;

    private:
        struct Stat
        {
            int64_t size = -1;
            int64_t time = 0;
            // The file's identity (its inode; 0 on Windows): a save that renames a new file into place (most editors'
            // safe save) is a change even when its size is the old one's and its time the same, as within one tick of
            // the kernel's file clock on Linux before 6.13 (1 to 10 ms there).
            uint64_t id = 0;
            friend bool operator==(const Stat&, const Stat&) = default;
        };
        struct Seen                                   // a file of the folder as last read
        {
            Stat stat;
            uint64_t hash = 0;                        // of its text as the editor takes it
            bool large = false;                       // past kMaxBytes: never taken
        };
        struct Event
        {
            std::string text;
            double at = 0.0;
        };

        bool lock(const std::filesystem::path& dir);
        void unlock();
        void loadManifest();
        void saveManifest();
        void writeExtras();                           // the note and VS Code's settings, when missing
        // The folder's listing: the files a pass may act on, by name (the ones it ignores left out).
        std::map<std::string, Stat> list(const Draft& d) const;
        bool look(double now, const Draft& d);        // a listing taken; true: it has held still kSettleSeconds
        bool ours(const std::string& name, const Draft& d) const;
        // A file's text from the folder, as the editor takes it (false: gone, unreadable or too large)
        bool readText(const std::string& name, std::string& text);
        const Seen* seenOf(const std::string& name);   // read when its stat moved
        bool writeFile(const std::string& name, const std::string& text, bool readOnly);
        bool removeFile(const std::string& name);
        // The side not kept, in .conflicts/ (false: it could not be kept there): a text, or the folder's file whole.
        std::filesystem::path asideFor(const std::string& name, const char* side);
        bool keepAside(const std::string& name, const std::string& text, const char* side);
        bool keepFolderFileAside(const std::string& name);
        // host.fm and the library files the #includes read, read only (`provided`: each one's text, by name); what
        // holds that stops one goes to `conditions`
        void syncCopies(const std::map<std::string, std::string>& provided, const Draft& d, double now,
                        std::vector<std::string>& conditions);
        // A look's decisions, file by file (`rested`: the plugin's changes may be written; `writeOnly`: only they are,
        // stop()'s). False: the draft moved under a write of the folder's, nothing taken (the next pass decides again).
        bool decide(double now, const Draft& d, bool rested, bool writeOnly);
        void note(const std::string& text, double now);
        void bump() noexcept { ++revision_; }

        Plugin plugin_;
        std::unique_ptr<FolderWatch> watch_;
        bool on_ = false;
        std::string name_;
        std::filesystem::path folder_;
        int lockFd_ = -1;
        bool runOnSave_ = false;

        std::map<std::string, uint64_t> base_;        // what both sides last agreed on (the manifest's)
        std::map<std::string, uint64_t> copies_;      // the read-only copies written, their texts' hashes
        std::map<std::string, Seen> seen_;            // the folder's files as last read
        std::map<std::string, Stat> listing_;         // the last look's
        double listedAt_ = -1.0e9;                    // when the listing last changed
        double nextLook_ = 0.0;
        bool lookedOnce_ = false;
        uint64_t draftKey_ = 0;
        uint64_t decidedKey_ = 0;                     // the draft the last decide() reconciled
        double draftAt_ = -1.0e9;                     // when the draft last changed
        double pendingSince_ = -1.0;                  // the draft away from the last decided since (-1: not)
        bool manifestDirty_ = false;

        std::set<std::string> conflicts_;
        std::vector<std::string> conditions_;         // what holds now ("my file.fm: not a name the plugin takes")
        std::vector<Event> events_;                   // what happened, the newest first
        std::string landed_;
        uint64_t landedCount_ = 0;
        std::string error_;
        uint64_t revision_ = 1;
    };
}
