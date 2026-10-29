// PROBE name=installer.engine timeout=240
//
// installer.engine: the installer's engine (Source/installer/Engine.h) against a root of the probe's own (Fixture.h:
// Places::under(<sandbox>/root with spaces 'and' "quotes")), with a made-up payload of two products. Nothing outside
// that root is written: ditto copies inside it, the Trash is a folder of it, the AU registrar and the administrator
// prompt are stand-ins (the prompt runs the engine's script with /bin/sh as this user, in the root).
//   PAYLOAD   payload.json and the parts beside it: two products, four parts each, the bundles' versions from their
//             Info.plists, the extensions' from their manifests; a missing payload is an error in words
//   CLEAN     every part "Just for me" into an empty root: each where Engine.h says (the app in /Applications, which is
//             writable here), each bundle whole (its version, its signature where it had one), no staging folder left,
//             the AU registrar restarted once, the log written
//   AGAIN     the same plan again: every part "already installed", nothing copied, the registrar left alone; a payload
//             whose VST3 is another build of the same version (the bundle's bytes) and whose AU is re-signed (its
//             CDHash): only those two are replaced, their old copies in the Trash
//   VERSIONS  an older and a newer version installed are told apart (upgrade, downgrade, each with both versions);
//             versions compare number by number
//   FAILURE   the VST3's folder made unwritable after the plan: the AU before it installed, the VST3 failed with words
//             that say why, the parts after it not run; the old VST3 copy whole (its version, its bytes), nothing in
//             its folder but it; the report says what was and was not changed
//   UNINSTALL every copy moved to the Trash (Places::trash): gone from its folder, there by its own name; the registrar
//             restarted; a path that is not one of the parts the engine knows is refused
//   LINKS     a folder on the way that is a link to a folder outside (the VST3 folder) and a target that is a link (an
//             Audio Unit) are refused with words naming the link; nothing is written where they point; a scan does not
//             look into the linked folder
//   ADMIN     "All users": the plan wants the administrator; its script quotes every path it names (each source, target
//             and folder appears only between single quotes, and the root has spaces and both quotes), and run (as
//             this user, by the stand-in) puts every part in the system's folders, replacing an older copy (to the
//             Trash); a cancelled prompt changes nothing and says so; the uninstall's script quotes its paths too
//   NOTES     a running Standalone of a product whose app changes, and a running host, are named in the plan's notes
#include "Probe.h"

#include "Fixture.h"

#include "installer/Engine.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace
{
    namespace fs = std::filesystem;
    namespace pi = playground::installer;

    std::vector<pi::Selection> everything(const pi::Payload& pay)
    {
        std::vector<pi::Selection> s;
        for (const pi::Product& pr : pay.products)
            for (const pi::Part part : pi::kParts)
                s.push_back({ pr.key, part });
        return s;
    }

    int countAction(const pi::Plan& plan, pi::Action a)
    {
        return static_cast<int>(std::count_if(plan.steps.begin(), plan.steps.end(), [&](const pi::Step& s) { return s.action == a; }));
    }

    const pi::Step* stepOf(const pi::Plan& plan, const std::string& key, pi::Part part)
    {
        for (const pi::Step& s : plan.steps)
            if (s.key == key && s.part == part)
                return &s;
        return nullptr;
    }

    bool under(const fs::path& p, const fs::path& root)
    {
        const fs::path rel = p.lexically_normal().lexically_relative(root.lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    }

    bool contains(const std::string& s, const std::string& what)
    {
        return s.find(what) != std::string::npos;
    }

    // The script with every quoted occurrence of `p` taken out: what is left must not name `p` (it is never unquoted).
    bool quotedOnly(const std::string& script, const std::string& p)
    {
        const std::string q = pi::shellQuote(p);
        if (!contains(script, q))
            return false;
        std::string rest = script;
        for (std::size_t at = rest.find(q); at != std::string::npos; at = rest.find(q))
            rest.erase(at, q.size());
        return !contains(rest, p);
    }
}

PROBE(installer, engine)
{
    const funkgui::HeadlessGuiScope gui;               // juce's message thread, for the zip reader
    const fs::path root = fixture::root("installer.engine");
    const fs::path payloadDir = root / "payload";
    fixture::writePayload(payloadDir);
    const pi::Places places = pi::Places::under(root);
    std::error_code ec;
    fs::create_directories(places.root / "Applications", ec);
    fs::create_directories(places.home, ec);
    fixture::FakeSystem sys;

    // ---- PAYLOAD -----------------------------------------------------------------------------------------------------
    {
        const pi::Payload pay = pi::Payload::read(payloadDir);
        P.eq("payload.read", pay.error.empty() ? 1 : 0, 1);
        P.eq("payload.products", static_cast<int64_t>(pay.products.size()), 2);
        P.eq("payload.project", pay.project == "Tone" ? 1 : 0, 1);
        int present = 0, versions = 0;
        for (const pi::Product& pr : pay.products)
            for (const pi::Part part : pi::kParts)
            {
                present += pr.offer(part).present ? 1 : 0;
                versions += pr.offer(part).version == (part == pi::Part::extension ? "0.3.1" : "1.2.0") ? 1 : 0;
            }
        P.eq("payload.parts_present", present, 8);
        P.eq("payload.versions_read", versions, 8);
        P.eq("payload.kinds", pay.products.size() == 2 && !pay.products[0].instrument() && pay.products[1].instrument() ? 1 : 0, 1);
        const pi::Payload none = pi::Payload::read(root / "no payload here");
        P.eq("payload.missing_is_an_error", !none.error.empty() && none.products.empty() ? 1 : 0, 1);
        std::printf("missing payload: %s\n", none.error.c_str());
    }

    // ---- CLEAN -------------------------------------------------------------------------------------------------------
    pi::Engine engine(pi::Payload::read(payloadDir), places, sys);
    {
        const pi::Plan plan = engine.plan(everything(engine.payload()), pi::Scope::user);
        P.eq("clean.steps", static_cast<int64_t>(plan.steps.size()), 8);
        P.eq("clean.all_install", countAction(plan, pi::Action::install), 8);
        P.eq("clean.no_admin", plan.admin() ? 1 : 0, 0);
        int inside = 0;
        for (const pi::Step& s : plan.steps)
            inside += under(s.target, root) ? 1 : 0;
        P.eq("clean.targets_inside_the_root", inside, 8);
        const pi::Step* au = stepOf(plan, "effect", pi::Part::au);
        const pi::Step* app = stepOf(plan, "effect", pi::Part::app);
        const pi::Step* ext = stepOf(plan, "synth", pi::Part::extension);
        P.eq("clean.au_in_home_components",
             au != nullptr && au->target == places.home / "Library/Audio/Plug-Ins/Components/Tone.component" ? 1 : 0, 1);
        P.eq("clean.app_in_applications", app != nullptr && app->target == places.root / "Applications/Tone.app" ? 1 : 0, 1);
        P.eq("clean.extension_in_app_support", ext != nullptr
             && ext->target == places.home / "Library/Application Support/Funk Test/ToneSynth/ToneSynth.mcpb" ? 1 : 0, 1);

        const pi::Report r = engine.install(plan, {});
        P.eq("clean.ok", r.ok() ? 1 : 0, 1);
        P.eq("clean.done", r.count(pi::State::done), 8);
        int whole = 0, versions = 0;
        for (const pi::Step& s : plan.steps)
        {
            whole += pi::Engine::sameContents(s.target, s.source) ? 1 : 0;
            const std::string v = s.part == pi::Part::extension ? pi::Engine::extensionVersion(s.target)
                                                                : pi::Engine::bundleVersion(s.target);
            versions += v == s.to ? 1 : 0;
        }
        P.eq("clean.bundles_whole", whole, 8);
        P.eq("clean.versions", versions, 8);
        P.eq("clean.au_signature_holds",
             pi::runProgram({ "/usr/bin/codesign", "--verify", "--strict", au->target.string() }, nullptr, nullptr), 0);
        P.eq("clean.no_staging_left", static_cast<int64_t>(fixture::named(root / "home", ".installing").size()
                                                           + fixture::named(root / "system", ".installing").size()), 0);
        P.eq("clean.registrar_restarted", sys.registrarRestarts, 1);
        P.eq("clean.report_says_so", r.registrarRestarted ? 1 : 0, 1);
        P.eq("clean.log_written", contains(fixture::readText(places.log), "Tone.component") ? 1 : 0, 1);
        P.eq("clean.installed_found", static_cast<int64_t>(engine.installed().size()), 8);
    }

    // ---- AGAIN -------------------------------------------------------------------------------------------------------
    {
        const pi::Plan plan = engine.plan(everything(engine.payload()), pi::Scope::user);
        P.eq("again.all_already_installed", countAction(plan, pi::Action::same), 8);
        P.eq("again.nothing_changes", plan.changing(), 0);
        const int copies = sys.copiesStarted.load();
        const pi::Report r = engine.install(plan, {});
        P.eq("again.skipped", r.count(pi::State::skipped), 8);
        P.eq("again.nothing_copied", sys.copiesStarted.load() - copies, 0);
        P.eq("again.registrar_left_alone", sys.registrarRestarts, 1);
        P.eq("again.says_already_installed", contains(r.lines[0], "already installed") ? 1 : 0, 1);

        // Another build of the same version: the effect's VST3 (bytes) and its AU (re-signed: the CDHash).
        fixture::writeText(payloadDir / "Tone.vst3/Contents/Resources/data.txt", "build 2");
        fixture::writeText(payloadDir / "Tone.component/Contents/Resources/data.txt", "build 2");
        fixture::sign(payloadDir / "Tone.component");
        pi::Engine again(pi::Payload::read(payloadDir), places, sys);
        const pi::Plan plan2 = again.plan(everything(again.payload()), pi::Scope::user);
        P.eq("again.rebuilds", countAction(plan2, pi::Action::rebuild), 2);
        P.eq("again.rebuild_is_the_vst3", stepOf(plan2, "effect", pi::Part::vst3)->action == pi::Action::rebuild ? 1 : 0, 1);
        P.eq("again.rebuild_is_the_au", stepOf(plan2, "effect", pi::Part::au)->action == pi::Action::rebuild ? 1 : 0, 1);
        P.eq("again.the_rest_same", countAction(plan2, pi::Action::same), 6);
        const int before = sys.copiesStarted.load();
        const pi::Report r2 = again.install(plan2, {});
        P.eq("again.replaced_two", r2.count(pi::State::done), 2);
        P.eq("again.copied_two", sys.copiesStarted.load() - before, 2);
        const fs::path vst3Data = places.home / "Library/Audio/Plug-Ins/VST3/Tone.vst3/Contents/Resources/data.txt";
        P.eq("again.new_build_in_place", fixture::readText(vst3Data) == "build 2" ? 1 : 0, 1);
        P.eq("again.old_copies_in_trash", fs::exists(places.trash / "Tone.vst3") && fs::exists(places.trash / "Tone.component") ? 1 : 0, 1);
        P.eq("again.old_copy_whole", fixture::readText(places.trash / "Tone.vst3/Contents/Resources/data.txt") == "build 1" ? 1 : 0, 1);
        P.eq("again.registrar_restarted_for_the_au", sys.registrarRestarts, 2);
    }

    // ---- VERSIONS ----------------------------------------------------------------------------------------------------
    {
        fixture::setVersion(places.home / "Library/Audio/Plug-Ins/Components/ToneSynth.component", "1.1.0");
        fixture::setVersion(places.home / "Library/Audio/Plug-Ins/VST3/ToneSynth.vst3", "2.0.0");
        const pi::Plan plan = engine.plan(everything(engine.payload()), pi::Scope::user);
        const pi::Step* older = stepOf(plan, "synth", pi::Part::au);
        const pi::Step* newer = stepOf(plan, "synth", pi::Part::vst3);
        P.eq("versions.older_is_upgrade", older->action == pi::Action::upgrade && older->from == "1.1.0" && older->to == "1.2.0" ? 1 : 0, 1);
        P.eq("versions.newer_is_downgrade", newer->action == pi::Action::downgrade && newer->from == "2.0.0" && newer->to == "1.2.0" ? 1 : 0, 1);
        P.eq("versions.cmp_numbers", pi::Engine::compareVersions("0.10.0", "0.9.1"), 1);
        P.eq("versions.cmp_equal_padded", pi::Engine::compareVersions("1.2", "1.2.0"), 0);
        P.eq("versions.cmp_less", pi::Engine::compareVersions("1.1.9", "1.2.0"), -1);
        P.eq("versions.cmp_text", pi::Engine::compareVersions("1.2.0b", "1.2.0a"), 1);
    }

    // ---- FAILURE -----------------------------------------------------------------------------------------------------
    {
        // The synth's AU (older) and VST3 (newer) are to be replaced; its VST3 folder becomes unwritable after the plan.
        const pi::Plan plan = engine.plan({ { "synth", pi::Part::au }, { "synth", pi::Part::vst3 }, { "synth", pi::Part::app } },
                                          pi::Scope::user);
        P.eq("failure.plan_changes", plan.changing(), 2);       // the app is the same
        fixture::setVersion(places.home / "Library/Audio/Plug-Ins/Components/ToneSynth.component", "1.1.0");
        const fs::path vst3Dir = places.home / "Library/Audio/Plug-Ins/VST3";
        const fs::path old = vst3Dir / "ToneSynth.vst3";
        const std::string oldPlist = fixture::readText(old / "Contents/Info.plist");
        fs::permissions(vst3Dir, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove, ec);
        const pi::Report r = engine.install(plan, {});
        fs::permissions(vst3Dir, fs::perms::owner_write, fs::perm_options::add, ec);
        const pi::Step* au = stepOf(plan, "synth", pi::Part::au);
        const std::size_t auAt = static_cast<std::size_t>(au - plan.steps.data());
        const std::size_t vstAt = static_cast<std::size_t>(stepOf(plan, "synth", pi::Part::vst3) - plan.steps.data());
        P.eq("failure.not_ok", r.ok() ? 1 : 0, 0);
        P.eq("failure.au_before_it_done", r.states[auAt] == pi::State::done ? 1 : 0, 1);
        P.eq("failure.vst3_failed", r.states[vstAt] == pi::State::failed ? 1 : 0, 1);
        P.eq("failure.says_why", contains(r.lines[vstAt], "may not write in ~/Library/Audio/Plug-Ins/VST3") ? 1 : 0, 1);
        P.eq("failure.old_copy_whole", fixture::readText(old / "Contents/Info.plist") == oldPlist
                                       && fixture::readText(old / "Contents/Resources/data.txt") == "build 1" ? 1 : 0, 1);
        P.eq("failure.old_version_kept", pi::Engine::bundleVersion(old) == "2.0.0" ? 1 : 0, 1);
        P.eq("failure.folder_holds_only_them",
             static_cast<int64_t>(std::distance(fs::directory_iterator(vst3Dir), fs::directory_iterator{})), 2);
        P.eq("failure.error_names_what_changed", contains(r.error, "ToneSynth's VST3 was not installed")
                                                 && contains(r.error, "Installed: ToneSynth's Audio Unit") ? 1 : 0, 1);
        std::printf("failure: %s\n", r.error.c_str());
    }

    // ---- LINKS -------------------------------------------------------------------------------------------------------
    {
        const fs::path outside = root / "outside";
        fs::create_directories(outside / "VST3", ec);
        fs::create_directories(outside / "Components", ec);
        // The VST3 folder: a link to a folder outside (the copies there are moved aside first, to the probe's spare).
        const fs::path vst3Dir = places.home / "Library/Audio/Plug-Ins/VST3";
        fs::rename(vst3Dir, root / "spare-vst3", ec);
        fs::create_directory_symlink(outside / "VST3", vst3Dir, ec);
        // The effect's AU: a link to a bundle outside.
        const fs::path au = places.home / "Library/Audio/Plug-Ins/Components/Tone.component";
        fs::rename(au, root / "spare-au.component", ec);
        fs::create_directory_symlink(outside / "Components" / "Tone.component", au, ec);
        const pi::Plan plan = engine.plan(everything(engine.payload()), pi::Scope::user);
        const pi::Step* v = stepOf(plan, "effect", pi::Part::vst3);
        const pi::Step* a = stepOf(plan, "effect", pi::Part::au);
        P.eq("links.folder_link_refused", v->action == pi::Action::blocked && contains(v->why, "is a link") ? 1 : 0, 1);
        P.eq("links.target_link_refused", a->action == pi::Action::blocked && contains(a->why, "is a link") ? 1 : 0, 1);
        std::printf("links: %s\nlinks: %s\n", v->why.c_str(), a->why.c_str());
        const pi::Report r = engine.install(plan, {});
        P.eq("links.nothing_written_outside",
             static_cast<int64_t>(std::distance(fs::recursive_directory_iterator(outside), fs::recursive_directory_iterator{})), 2);
        P.eq("links.link_left_as_it_was", fs::is_symlink(au) && fs::is_symlink(vst3Dir) ? 1 : 0, 1);
        P.eq("links.blocked_skipped", r.count(pi::State::failed), 0);
        int seenInside = 0;
        for (const pi::Copy& c : engine.installed())
            seenInside += under(c.path, outside) ? 1 : 0;
        P.eq("links.scan_does_not_follow", seenInside, 0);
        // An uninstall of a path the engine does not know: refused.
        pi::Copy stranger;
        stranger.key = "effect";
        stranger.part = pi::Part::vst3;
        stranger.path = outside / "VST3" / "Tone.vst3";
        const pi::Report u = engine.uninstall({ stranger }, {});
        P.eq("links.unknown_path_refused", u.states[0] == pi::State::skipped && contains(u.lines[0], "left as it was") ? 1 : 0, 1);
        // Back as they were.
        fs::remove(au, ec);
        fs::rename(root / "spare-au.component", au, ec);
        fs::remove(vst3Dir, ec);
        fs::rename(root / "spare-vst3", vst3Dir, ec);
    }

    // ---- UNINSTALL ---------------------------------------------------------------------------------------------------
    {
        const std::vector<pi::Copy> copies = engine.installed();
        P.eq("uninstall.found", static_cast<int64_t>(copies.size()), 8);
        const int restarts = sys.registrarRestarts;
        fs::remove_all(places.trash, ec);                // the probe's own stand-in Trash, emptied for the count
        const pi::Report r = engine.uninstall(copies, {});
        P.eq("uninstall.ok", r.ok() ? 1 : 0, 1);
        P.eq("uninstall.done", r.count(pi::State::done), 8);
        int gone = 0, inTrash = 0;
        for (const pi::Copy& c : copies)
        {
            gone += fs::exists(fs::symlink_status(c.path)) ? 0 : 1;
            inTrash += fs::exists(places.trash / c.path.filename()) ? 1 : 0;
        }
        P.eq("uninstall.gone", gone, 8);
        P.eq("uninstall.in_trash", inTrash, 8);
        P.eq("uninstall.registrar_restarted", sys.registrarRestarts - restarts, 1);
        P.eq("uninstall.none_left", static_cast<int64_t>(engine.installed().size()), 0);
    }

    // ---- ADMIN -------------------------------------------------------------------------------------------------------
    {
        // An older copy for all users already, to be replaced.
        const fs::path sysAu = places.root / "Library/Audio/Plug-Ins/Components/Tone.component";
        fixture::writeBundle(sysAu, "1.0.0", "old", false);
        const pi::Plan plan = engine.plan(everything(engine.payload()), pi::Scope::system);
        P.eq("admin.wants_the_administrator", plan.admin() ? 1 : 0, 1);
        P.eq("admin.older_upgrades", stepOf(plan, "effect", pi::Part::au)->action == pi::Action::upgrade ? 1 : 0, 1);
        int systemPlaces = 0;
        for (const pi::Step& s : plan.steps)
            systemPlaces += under(s.target, places.root) ? 1 : 0;
        P.eq("admin.targets_in_the_system", systemPlaces, 8);
        const std::string script = engine.adminScript(plan, root / "status \"file\"");
        int quoted = 0;
        for (const pi::Step& s : plan.steps)
            quoted += (quotedOnly(script, s.source.string()) ? 1 : 0) + (quotedOnly(script, s.target.string()) ? 1 : 0)
                      + (quotedOnly(script, s.folder.string()) ? 1 : 0);
        P.eq("admin.every_path_quoted", quoted, 24);
        P.eq("admin.root_has_quotes", contains(root.string(), "'") && contains(root.string(), "\"") && contains(root.string(), " ") ? 1 : 0, 1);
        P.eq("admin.no_rm", contains(script, "rm ") ? 1 : 0, 0);
        P.eq("admin.status_quoted", quotedOnly(script, (root / "status \"file\"").string()) ? 1 : 0, 1);

        // The prompt cancelled: nothing changes.
        sys.cancelPrompt = true;
        const pi::Report cancelled = engine.install(plan, {});
        sys.cancelPrompt = false;
        P.eq("admin.cancel_reported", cancelled.cancelled ? 1 : 0, 1);
        P.eq("admin.cancel_changes_nothing", cancelled.count(pi::State::done), 0);
        P.eq("admin.cancel_old_copy_kept", pi::Engine::bundleVersion(sysAu) == "1.0.0" ? 1 : 0, 1);
        P.eq("admin.cancel_says_so", contains(cancelled.error, "password was not given") ? 1 : 0, 1);
        std::printf("cancelled: %s\n", cancelled.error.c_str());

        // Run (as this user, by the stand-in: the root is the probe's).
        const int runs = sys.adminRuns;
        std::vector<pi::State> seen;
        const pi::Report r = engine.install(plan, [&](int, pi::State s, const std::string&) { seen.push_back(s); });
        P.eq("admin.one_prompt", sys.adminRuns - runs, 1);
        P.eq("admin.prompt_names_the_installer", contains(sys.lastPrompt, "Tone Installer") ? 1 : 0, 1);
        P.eq("admin.ok", r.ok() ? 1 : 0, 1);
        if (!r.ok())
            std::printf("admin: %s\n", r.error.c_str());
        P.eq("admin.done", r.count(pi::State::done), 8);
        P.eq("admin.progress_reported", std::count(seen.begin(), seen.end(), pi::State::working) >= 1 ? 1 : 0, 1);
        int whole = 0;
        for (const pi::Step& s : plan.steps)
            whole += pi::Engine::sameContents(s.target, s.source) ? 1 : 0;
        P.eq("admin.bundles_whole", whole, 8);
        int oldInTrash = 0;                              // by a free name: the Trash has a Tone.component already
        for (const auto& e : fs::directory_iterator(places.trash))
            oldInTrash += e.path().extension() == ".component"
                          && fixture::readText(e.path() / "Contents/Resources/data.txt") == "old" ? 1 : 0;
        P.eq("admin.old_copy_in_trash", oldInTrash, 1);
        P.eq("admin.no_staging_left", static_cast<int64_t>(fixture::named(places.root, ".installing").size()), 0);

        // A step of the script that fails (a folder it may not write in, as this user): said in words, the step after
        // it not run, the old copy whole.
        fixture::setVersion(sysAu, "1.0.0");
        fixture::setVersion(places.root / "Library/Audio/Plug-Ins/VST3/Tone.vst3", "1.0.0");
        const pi::Plan again = engine.plan({ { "effect", pi::Part::au }, { "effect", pi::Part::vst3 } }, pi::Scope::system);
        const fs::path comps = sysAu.parent_path();
        fs::permissions(comps, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove, ec);
        const pi::Report failed = engine.install(again, {});
        fs::permissions(comps, fs::perms::owner_write, fs::perm_options::add, ec);
        std::printf("admin failure: %s\n", failed.error.c_str());
        P.eq("admin.failure_reported", failed.ok() ? 1 : 0, 0);
        P.eq("admin.failure_in_words", contains(failed.error, "Tone's Audio Unit was not installed: it may not write in "
                                                              "/Library/Audio/Plug-Ins/Components") ? 1 : 0, 1);
        P.eq("admin.failure_next_not_run", failed.states.size() == 2 && failed.states[1] == pi::State::notRun ? 1 : 0, 1);
        P.eq("admin.failure_old_copy_kept", pi::Engine::bundleVersion(sysAu) == "1.0.0" ? 1 : 0, 1);

        // The uninstall's script: every path quoted.
        const std::vector<pi::Copy> copies = engine.installed();
        const std::string un = engine.adminUninstallScript(copies, root / "status");
        int q = 0;
        for (const pi::Copy& c : copies)
            q += quotedOnly(un, c.path.string()) ? 1 : 0;
        P.eq("admin.uninstall_paths_quoted", q, static_cast<int64_t>(copies.size()));
        P.eq("admin.uninstall_no_rm", contains(un, "rm ") ? 1 : 0, 0);
        std::printf("script (its first step):\n");
        const std::size_t from = script.find("# 0: ");
        std::printf("%s\n", script.substr(from, std::min<std::size_t>(5000, script.find("# 1: ") - from)).c_str());
    }

    // ---- NOTES -------------------------------------------------------------------------------------------------------
    {
        sys.apps = { fs::path("/Applications/Tone.app"), fs::path("/Applications/Logic Pro.app"), fs::path("/Applications/Safari.app") };
        fixture::setVersion(places.root / "Applications/Tone.app", "1.0.0");
        fixture::setVersion(places.root / "Library/Audio/Plug-Ins/VST3/ToneSynth.vst3", "1.0.0");
        const pi::Plan plan = engine.plan(everything(engine.payload()), pi::Scope::system);
        std::string notes;
        for (const std::string& n : plan.notes)
            notes += n + "\n";
        std::printf("notes:\n%s", notes.c_str());
        P.eq("notes.standalone_named", contains(notes, "Tone is open") ? 1 : 0, 1);
        P.eq("notes.host_named", contains(notes, "Logic Pro is open: restart your DAW") ? 1 : 0, 1);
        P.eq("notes.other_apps_not_named", contains(notes, "Safari") ? 1 : 0, 0);
        sys.apps.clear();
    }
    return P.finish();
}
