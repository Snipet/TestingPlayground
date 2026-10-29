// Source/installer/Engine.h: the installer's engine, without a GUI: what the payload offers, what is installed now, the
// plan, the install, the uninstall and their log. The installer app (Panel.h, App.cpp) drives it; the probes
// (Tools/probes/installer/) drive it against a folder of their own. It knows no product: the payload names them
// (payload.json, written by the build: cmake/Installer.cmake).
//
// The payload (<Installer>.app/Contents/Resources/payload/): payload.json (the project, and each product's key, name,
// kind, company and bundle id) and, per product, <Name>.component, <Name>.vst3, <Name>.app and <Name>.mcpb. A part's
// version is read from the bundle's Info.plist (CFBundleShortVersionString), an extension's from its manifest.json
// (the extension's own version, FP_MCP_VERSION).
//
// Where a part goes (Places: this Mac's folders, or a probe's root standing in for them):
//
//   part        "Just for me" (Scope::user)                     "All users" (Scope::system)
//   Audio Unit  ~/Library/Audio/Plug-Ins/Components             /Library/Audio/Plug-Ins/Components
//   VST3        ~/Library/Audio/Plug-Ins/VST3                   /Library/Audio/Plug-Ins/VST3
//   App         /Applications when this user may write there,   /Applications
//               else ~/Applications (scripts/install.sh's rule)
//   extension   ~/Library/Application Support/<Company>/<Name>  /Library/Application Support/<Company>/<Name> (the
//                                                               .pkg's place)
//
// What is installed now is looked for in both scopes' folders (and both app folders), with each copy's version.
//
// The plan says, for each part chosen, what happens: install (nothing there), upgrade (an older version there),
// downgrade (a newer one there: said so, and the part's switch starts off), rebuild (the same version, other contents:
// another build), same (already installed: skipped), or blocked (why, in plain words: a link where the part goes, a
// folder that is a link or no folder, a copy that belongs to another user). Contents are compared by the code
// signature's CDHash (the main executable's code and every sealed resource), or byte for byte where there is none.
//
// Installing a part: ditto (--noqtn: it keeps the signature, the extended attributes and the ACLs; the quarantine flag
// is dropped, as a .pkg's installed files carry none: the installer itself passed Gatekeeper with its payload sealed
// in its signature) into a staging folder made beside the target (<folder>/.installing-XXXXXX), then one atomic
// rename into place (renamex_np's RENAME_SWAP over an old copy), so a failure at any point leaves the old copy whole
// where it was. The old copy then goes to the Trash. Nothing is ever deleted: a replaced copy, an uninstalled one and
// a half-made staging copy all go to the user's Trash (Places::trash in a probe), and only an empty staging folder is
// removed (rmdir). After an Audio Unit changes, the AU registrar restarts (killall -9 AudioComponentRegistrar, as the
// .pkg's postinstall restarts it) so hosts rescan. A failed step stops the run: the steps before it are done, the
// ones after it not run, and the report says which.
//
// "All users" writes where only an administrator may: the engine writes the whole run as one shell script (every path
// quoted, adminScript) and asks macOS to run it as root through its own administrator prompt (osascript's `do shell
// script ... with administrator privileges`): the app never sees the password. The script reports each step to a
// status file the engine reads as it runs. The AU registrar is restarted by the app afterwards, never by the script.
//
// Safety: the engine writes only into the folders above. A path is followed only below Places::home and Places::root
// (each resolved first: /var is /private/var), and every folder on the way to a target must be a real folder, not a
// link: a target, or a folder above it, that is a symbolic link is refused, whatever it points at. A copy another user
// owns is not replaced "Just for me" (the .pkg's are root's). Running apps are warned of: the Standalone that a part
// replaces, and the hosts that load the plugins ("restart your DAW").
//
// Threads: an Engine is used by one thread at a time; install() and uninstall() call `progress` on their own thread
// (the caller's, and for "All users" a poller of the status file too), never at once.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace playground::installer
{
    namespace fs = std::filesystem;

    enum class Part : uint8_t { au, vst3, app, extension };
    inline constexpr Part kParts[] = { Part::au, Part::vst3, Part::app, Part::extension };

    const char* partName(Part part);                 // "Audio Unit", "VST3", "App", "Claude Desktop extension"
    const char* partExtension(Part part);            // ".component", ".vst3", ".app", ".mcpb"

    enum class Scope : uint8_t { user, system };     // "Just for me", "All users"

    // The folders: this Mac's, or a probe's stand-ins.
    struct Places
    {
        fs::path home;                               // the user's home: ~/Library/..., ~/Applications
        fs::path root;                               // the system's root: /Library/..., /Applications
        fs::path trash;                              // where old copies go; empty: the user's own Trash (Finder's
                                                     // Put Back works), through System::moveToTrash
        fs::path log;                                // the log appended to (what went where); empty: none

        static Places thisMac();                     // $HOME (else the password database's), "/", the Trash,
                                                     // ~/Library/Logs/<logName>.log
        static Places thisMac(const std::string& logName);
        static Places under(const fs::path& testRoot);   // <root>/home, <root>/system, <root>/Trash, <root>/install.log
    };

    // A part the payload offers.
    struct Offer
    {
        Part part = Part::au;
        fs::path source;                             // in the payload
        std::string version;                         // its Info.plist's, or the extension's manifest's
        bool present = false;                        // false: this payload lacks it
    };

    // A product of the payload.
    struct Product
    {
        std::string key, name, kind, company, bundleId;
        std::string version;                         // its app's (else its first bundle's) version
        std::vector<Offer> offers;                   // one a part, in kParts' order

        bool instrument() const { return kind == "instrument"; }
        const Offer& offer(Part part) const { return offers[static_cast<std::size_t>(part)]; }
    };

    struct Payload
    {
        fs::path folder;
        std::string project;                         // the project's name (payload.json's)
        std::vector<Product> products;
        std::string error;                           // why it could not be read, in plain words; empty when it was

        // payload.json and the parts beside it. A missing or unreadable payload gives no products and the error.
        static Payload read(const fs::path& folder);
        const Product* find(const std::string& key) const;
    };

    // A copy of a part found installed.
    struct Copy
    {
        std::string key;                             // the product's
        Part part = Part::au;
        Scope scope = Scope::user;                   // the folder's: the user's (home) or the system's (root)
        fs::path path;
        std::string version;                         // empty when it could not be read
        bool link = false;                           // a symbolic link: never followed, never replaced or moved
        bool mine = true;                            // this user owns it
        bool trashable = true;                       // this user may move it (no administrator needed)
    };

    enum class Action : uint8_t { install, upgrade, downgrade, rebuild, replace, same, blocked };

    struct Step
    {
        std::string key, product;                    // the product's key and name
        Part part = Part::au;
        Action action = Action::install;
        fs::path source, folder, target;
        std::string from, to;                        // the version there now (when there is one), and the payload's
        std::string why;                             // blocked: why, in plain words

        bool changes() const { return action != Action::same && action != Action::blocked; }
    };

    struct Selection
    {
        std::string key;
        Part part = Part::au;
    };

    struct Plan
    {
        Scope scope = Scope::user;
        std::vector<Step> steps;                     // the parts chosen, in the payload's order
        std::vector<std::string> notes;              // running apps and hosts, copies the other scope also has

        int changing() const;                        // steps that change something
        int blocked() const;
        bool admin() const { return scope == Scope::system && changing() > 0; }
    };

    // What a step came to.
    enum class State : uint8_t { waiting, working, done, skipped, failed, notRun };

    struct Report
    {
        std::vector<State> states;                   // each step's, by its index in the plan (or the copies' list)
        std::vector<std::string> lines;              // each step's line: where it went, or what went wrong
        std::vector<std::string> log;                // the run's log, line by line (also appended to Places::log)
        std::string error;                           // the failure, in plain words (with what was and was not
                                                     // changed); empty when none
        bool cancelled = false;                      // stopped: the administrator prompt cancelled, or cancel set
        bool registrarRestarted = false;

        bool ok() const { return error.empty() && !cancelled; }
        int count(State s) const;
    };

    // Each step's progress: its index, its state and a line about it. Called on the running thread.
    using ProgressFn = std::function<void(int step, State state, const std::string& line)>;

    // The system's tools, behind an interface so that a probe replaces each one it must not use for real (the AU
    // registrar, the administrator prompt, the Trash, the processes) and keeps ditto (dittoCopy).
    class System
    {
    public:
        virtual ~System() = default;

        // ditto --noqtn <from> <to>; false with the reason (its own words) in `error`.
        virtual bool copy(const fs::path& from, const fs::path& to, std::string& error) = 0;
        // The bundles of the apps running now (pgrep -lf: each process's executable inside <bundle>/Contents/MacOS).
        virtual std::vector<fs::path> runningApps() = 0;
        // killall -9 AudioComponentRegistrar: hosts see the Audio Units as they are now.
        virtual bool restartAuRegistrar() = 0;
        // The user's Trash (Finder's Put Back works). False with the reason in `error`.
        virtual bool moveToTrash(const fs::path& path, std::string& error) = 0;
        // /bin/sh <script>, as root, through macOS's administrator prompt showing `prompt`; blocks until it is over.
        enum class Admin : uint8_t { ran, failed, cancelled };
        virtual Admin runAsAdministrator(const fs::path& script, const std::string& prompt, std::string& error) = 0;
    };

    // The tools of this Mac (System.cpp): ditto, pgrep, killall, NSFileManager's trash, osascript.
    class MacSystem final : public System
    {
    public:
        bool copy(const fs::path& from, const fs::path& to, std::string& error) override;
        std::vector<fs::path> runningApps() override;
        bool restartAuRegistrar() override;
        bool moveToTrash(const fs::path& path, std::string& error) override;
        Admin runAsAdministrator(const fs::path& script, const std::string& prompt, std::string& error) override;
    };

    // ditto --noqtn <from> <to>, as MacSystem::copy runs it (a probe's System copies with it too).
    bool dittoCopy(const fs::path& from, const fs::path& to, std::string& error);

    // Runs a program (no shell) and waits: its exit status (-1 when it could not start), its output and its errors.
    int runProgram(const std::vector<std::string>& argv, std::string* out, std::string* err);

    // A path quoted for /bin/sh: between single quotes, each ' written '\''.
    std::string shellQuote(const std::string& s);

    class Engine
    {
    public:
        Engine(Payload payload, Places places, System& system);

        const Payload& payload() const noexcept { return payload_; }
        const Places& places() const noexcept { return places_; }
        System& system() noexcept { return system_; }

        // The folder a part goes to in this scope (above), and the file or bundle's name there.
        fs::path folder(const Product& product, Part part, Scope scope) const;
        static std::string itemName(const Product& product, Part part);
        // Every folder a part may be found in, each with the scope it stands for (the user's first).
        std::vector<std::pair<fs::path, Scope>> knownFolders(const Product& product, Part part) const;

        std::vector<Copy> installed() const;                                 // every product's, every part's
        std::vector<Copy> installed(const Product& product, Part part) const;

        // The plan for these parts in this scope (the selection's order is the payload's, whatever it is given in).
        Plan plan(const std::vector<Selection>& chosen, Scope scope) const;

        // Runs the plan's steps that change something (the others are skipped at once). Stops at the first failure,
        // or before the next step once `cancel` is set. The AU registrar restarts when an Audio Unit changed.
        Report install(const Plan& plan, const ProgressFn& progress, const std::atomic<bool>* cancel = nullptr);

        // Moves these copies to the Trash (an administrator's prompt for those this user may not move). Links are
        // refused. The AU registrar restarts when an Audio Unit went.
        Report uninstall(const std::vector<Copy>& copies, const ProgressFn& progress,
                         const std::atomic<bool>* cancel = nullptr);

        // The scripts "All users" runs as root: every path quoted. Each step reports to `status` ("STEP <i> start",
        // "STEP <i> done", "STEP <i> fail <why>"); the first failure ends the script.
        // The uninstall's script holds the copies `only` names (indices; empty: every copy that is not a link).
        std::string adminScript(const Plan& plan, const fs::path& status) const;
        std::string adminUninstallScript(const std::vector<Copy>& copies, const fs::path& status,
                                         const std::vector<int>& only = {}) const;

        // A path as the user reads it: ~ for the home.
        std::string shown(const fs::path& p) const;

        // -1, 0 or 1: dotted numbers compared number by number ("0.10.0" after "0.9.1"); text by text.
        static int compareVersions(const std::string& a, const std::string& b);
        static std::string bundleVersion(const fs::path& bundle);           // CFBundleShortVersionString; "" if none
        static std::string extensionVersion(const fs::path& mcpb);          // its manifest.json's "version"; "" if none
        // Whether two copies of a part hold the same contents (the CDHash, else every file's bytes).
        static bool sameContents(const fs::path& a, const fs::path& b);

    private:
        // Why `folder` may not be written to (a link or a file on the way, outside home and root), or "" when it may.
        std::string unsafe(const fs::path& folder) const;
        bool mayMove(const fs::path& path) const;                            // this user may rename it away
        bool trash(const fs::path& path, std::string& where, std::string& error);
        fs::path trashName(const fs::path& item) const;                      // a free name in Places::trash
        fs::path trashFolder() const;                                        // the script's: Places::trash, or ~/.Trash
        void log(Report& report, const std::string& line);
        bool installStep(const Step& step, std::string& line, std::string& error);
        // Writes the script `makeScript` makes for its status file, runs it through the administrator prompt, and
        // reports its steps (`indices` of `steps`) as the status file tells them.
        Report runAdmin(const std::function<std::string(const fs::path&)>& makeScript, int steps,
                        const std::vector<int>& indices, const ProgressFn& progress, Report report, const std::string& what);

        Payload payload_;
        Places places_;
        System& system_;
        fs::path realHome_, realRoot_;               // home and root, resolved
    };
}
