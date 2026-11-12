// PROBE name=plugin.git timeout=180
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
#include "Probe.h"

#include "plugin/Git.h"
#include "plugin/Run.h"

#include "Product.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
        for (const char* section : { "commit", "push", "pull", "clone", "address", "hooks", "nogit" })
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
