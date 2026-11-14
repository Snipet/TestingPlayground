// Source/plugin/Git.cpp: see Git.h. CLAUDE.md, "Git", gives the rules each run keeps.
#include "plugin/Git.h"

#include "plugin/FileReplace.h"

#include "Product.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>

namespace playground::git
{
    namespace
    {
        namespace fs = std::filesystem;

        // The variables that would point git at another repository, index or object store than the folder's (a host
        // started from a git hook, a terminal's leftovers), and the dates a commit would otherwise take: never passed on.
        const std::vector<std::string> kUnset{ "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                                               "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE",
                                               "GIT_PREFIX", "GIT_AUTHOR_DATE", "GIT_COMMITTER_DATE", "GIT_EDITOR",
                                               "LANGUAGE", "LANG" };

        bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

        std::string trimmed(std::string s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
            std::size_t from = 0;
            while (from < s.size() && std::isspace(static_cast<unsigned char>(s[from])))
                ++from;
            return s.substr(from);
        }

        // An empty folder of the plugin's that core.hooksPath names, so that no hook runs (the repository's, the user's
        // own core.hooksPath's, a template's): a private one made once a process in the temporary folder, made again
        // if it went or something was put in it. "" when none could be made.
        std::string hooksFolder()
        {
            static std::mutex mutex;
            static std::string made;
            const std::lock_guard<std::mutex> lock(mutex);
            std::error_code ec;
            if (const fs::path m = files::pathOf(made); !made.empty() && fs::is_directory(m, ec) && fs::is_empty(m, ec) && !ec)
                return made;
            made.clear();
            const fs::path tmp = fs::temp_directory_path(ec);
            if (ec)
                return {};
            std::random_device rd;
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                char name[64];
                std::snprintf(name, sizeof name, "%sgit-hooks-%08x%08x", product::kEnvPrefix, static_cast<unsigned>(rd()),
                              static_cast<unsigned>(rd()));
                const fs::path folder = tmp / name;
                if (fs::create_directory(folder, ec) && !ec)
                {
#if !defined(_WIN32)
                    fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
                    made = files::utf8Of(folder);                // run::run reads it as UTF-8
                    return made;
                }
            }
            return {};
        }

        // Git's last lines on stderr, the hints left out, as they are: at most four.
        std::string lastLines(const std::string& err)
        {
            std::vector<std::string> lines;
            std::istringstream in(err);
            std::string line;
            while (std::getline(in, line))
            {
                line = trimmed(line);
                if (!line.empty() && line.rfind("hint:", 0) != 0)
                    lines.push_back(line);
            }
            std::string out;
            for (std::size_t i = lines.size() > 4 ? lines.size() - 4 : 0; i < lines.size(); ++i)
                out += (out.empty() ? "" : "\n") + lines[i];
            return out;
        }

        Outcome outcomeOf(const run::Result& r, const std::string& what, int timeoutMs)
        {
            Outcome o;
            o.ended = r.ended;
            o.code = r.code;
            o.out = r.out;
            o.outCut = r.outCut;
            o.ok = r.ok();
            if (o.ok)
                return o;
            const std::string said = lastLines(r.err);
            if (r.ended == run::Ended::exited && !said.empty())
                o.why = said;
            else
            {
                o.why = "git " + what + " " + run::describe(r, timeoutMs);
                if (!said.empty() && r.ended != run::Ended::notStarted)
                    o.why += ":\n" + said;
            }
            return o;
        }

        Outcome refused(const std::string& why)
        {
            Outcome o;
            o.why = why;
            return o;
        }

        // git with the fixed options and environment, and `env`'s variables over them (commitAll's dates).
        Outcome callWith(const Context& c, const std::vector<std::string>& args, bool network,
                         const std::vector<std::pair<std::string, std::string>>& env)
        {
            if (c.git.empty())
                return refused(notFoundWords());
            const std::string hooks = hooksFolder();
            if (hooks.empty())
                return refused("Git was not run: no empty folder for its hooks could be made in the temporary folder.");
            run::Request r;
            r.args = { c.git,
                       "-c", "protocol.ext.allow=never",
                       "-c", "core.hooksPath=" + hooks,
                       "-c", "core.fsmonitor=false",
                       "-c", "gc.auto=0",
                       "-c", "maintenance.auto=false" };
            if (!c.folder.empty())
            {
                r.args.push_back("-C");
                r.args.push_back(c.folder);
            }
            r.args.insert(r.args.end(), args.begin(), args.end());
            r.env = { { "GIT_TERMINAL_PROMPT", "0" },
                      { "GIT_ALLOW_PROTOCOL", c.allowFile ? "https:ssh:file" : "https:ssh" },
                      { "GIT_MERGE_AUTOEDIT", "no" },
                      { "LC_ALL", "C" } };
            // ssh fails at once rather than wait for a passphrase, unless the user's environment chose an ssh.
            const char* sshCommand = std::getenv("GIT_SSH_COMMAND");
            const char* ssh = std::getenv("GIT_SSH");
            if ((sshCommand == nullptr || *sshCommand == '\0') && (ssh == nullptr || *ssh == '\0'))
                r.env.push_back({ "GIT_SSH_COMMAND", "ssh -o BatchMode=yes" });
            r.unset = kUnset;
            for (const auto& kv : env)
            {
                r.unset.erase(std::remove(r.unset.begin(), r.unset.end(), kv.first), r.unset.end());
                r.env.push_back(kv);
            }
            r.timeoutMs = network ? c.networkMs : c.localMs;
            r.cancel = c.cancel;
            r.outLimit = c.outLimit;
            return outcomeOf(run::run(r), args.empty() ? std::string() : args.front(), r.timeoutMs);
        }

        // A remote's name (origin): as a branch's, with no '/'.
        std::string remoteRefusal(const std::string& remote)
        {
            if (remote.empty() || remote.find('/') != std::string::npos)
                return "The remote's name \"" + remote + "\" cannot be used.";
            return branchRefusal(remote).empty() ? std::string() : "The remote's name \"" + remote + "\" cannot be used.";
        }

        std::string trackingRef(const std::string& remote, const std::string& branch)
        {
            return "refs/remotes/" + remote + "/" + branch;
        }

        // Whether `ref` names a commit here.
        bool resolves(const Context& c, const std::string& ref, std::string* sha = nullptr)
        {
            const Outcome o = call(c, { "rev-parse", "--verify", "-q", ref + "^{commit}" });
            if (o.ok && sha != nullptr)
                *sha = trimmed(o.out);
            return o.ok;
        }

        // `version`'s first line: "git version 2.43.0" (Windows: "git version 2.43.0.windows.1"). "" when it is none.
        std::string versionOf(const std::string& out)
        {
            const std::string line = out.substr(0, out.find('\n'));
            const std::string lead = "git version ";
            if (line.rfind(lead, 0) != 0)
                return {};
            return trimmed(line.substr(lead.size()));
        }

        Found tryGit(const std::string& path)
        {
            Found f;
            run::Request r;
            r.args = { path, "--version" };
            r.env = { { "LC_ALL", "C" }, { "GIT_TERMINAL_PROMPT", "0" } };
            r.timeoutMs = 10000;
            r.outLimit = 4096;
            const run::Result res = run::run(r);
            if (res.ended == run::Ended::notStarted)
            {
                f.why = path + " could not be run (" + res.why + ").";
                return f;
            }
            const std::string version = res.ok() ? versionOf(res.out) : std::string();
            if (version.empty())
            {
                f.why = path + " is not git (its --version " + (res.ok() ? "says something else" : run::describe(res, r.timeoutMs))
                      + ").";
                return f;
            }
            f.path = res.ended == run::Ended::exited ? run::resolve(path) : path;
            f.version = version;
            return f;
        }

#if defined(__APPLE__)
        // Whether the command line tools are there: /usr/bin/git is only a shim that offers to install them otherwise.
        bool commandLineTools()
        {
            run::Request r;
            r.args = { "/usr/bin/xcode-select", "-p" };
            r.timeoutMs = 10000;
            return run::run(r).ok();
        }
#endif
    }

    std::string notFoundWords()
    {
        return "Git was not found on this computer. Install it (git-scm.com; on a Mac, Apple's command line tools or "
               "Homebrew), or set where it is in Settings.";
    }

    Found find(const std::string& given)
    {
        if (!given.empty())
            return tryGit(given);
        const std::string var = std::string(product::kEnvPrefix) + "GIT";
#if defined(_WIN32)
        // The variable as Windows holds it (wide), given on as UTF-8 (FileReplace.h's pathOf, utf8Of).
        const std::wstring wvar(var.begin(), var.end());
        if (const wchar_t* v = _wgetenv(wvar.c_str()); v != nullptr && *v != L'\0')
            return tryGit(files::utf8Of(fs::path(v)));
#else
        if (const char* v = std::getenv(var.c_str()); v != nullptr && *v != '\0')
            return tryGit(v);
#endif
        std::vector<std::string> places;
#if defined(__APPLE__)
        places = { "/opt/homebrew/bin/git", "/usr/local/bin/git" };
        std::error_code ec;
        for (const std::string& p : places)
            if (fs::is_regular_file(p, ec))
                if (Found f = tryGit(p); f.found())
                    return f;
        if (fs::is_regular_file("/usr/bin/git", ec) && commandLineTools())
            if (Found f = tryGit("/usr/bin/git"); f.found())
                return f;
#elif defined(_WIN32)
        if (const std::string onPath = run::resolve("git.exe"); !onPath.empty())
            if (Found f = tryGit(onPath); f.found())
                return f;
        if (const wchar_t* pf = _wgetenv(L"ProgramFiles"); pf != nullptr && *pf != L'\0')
        {
            // The variable as Windows holds it (wide), the path given on as UTF-8, as run::run reads it.
            const fs::path p = fs::path(pf) / L"Git" / L"cmd" / L"git.exe";
            std::error_code ec;
            if (fs::is_regular_file(p, ec))
                if (Found f = tryGit(files::utf8Of(p)); f.found())
                    return f;
        }
#else
        if (const std::string onPath = run::resolve("git"); !onPath.empty())
            if (Found f = tryGit(onPath); f.found())
                return f;
#endif
        Found none;
        none.why = notFoundWords();
        return none;
    }

    std::string addressRefusal(const std::string& address, bool allowFile)
    {
        if (address.empty())
            return "Give the repository's address.";
        if (address.front() == '-')
            return "A repository's address cannot start with '-'.";
        for (const char ch : address)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c == 0x7f)
                return "A repository's address cannot hold a control character.";
            if (c == ' ')
                return "A repository's address cannot hold a space.";
        }
        // git reads <transport>::<address> as "run that transport's helper" (ext:: runs a command).
        if (has(address, "::"))
            return "That kind of address is not taken: give an https://, ssh:// or user@host:path address.";
        const std::size_t scheme = address.find("://");
        if (scheme != std::string::npos)
        {
            std::string name = address.substr(0, scheme);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string rest = address.substr(scheme + 3);
            if (name == "https" || name == "ssh" || (allowFile && name == "file"))
            {
                if (rest.empty() || (name != "file" && (rest.front() == '/' || rest.front() == '-')))
                    return "That address names no host.";
                return {};
            }
            return "Only https:// and ssh:// addresses (or user@host:path) are taken, not " + name + "://.";
        }
        // scp's form: user@host:path, the host before any '/'.
        const std::size_t at = address.find('@');
        const std::size_t colon = address.find(':');
        const std::size_t slash = address.find('/');
        if (at != std::string::npos && at > 0 && colon != std::string::npos && colon > at + 1 && colon + 1 < address.size()
            && (slash == std::string::npos || slash > colon) && address[at + 1] != '-' && address[at + 1] != '[')
            return {};
        return "Give an https://, ssh:// or user@host:path address (a folder on this computer is not taken).";
    }

    std::string branchRefusal(const std::string& b)
    {
        const std::string bad = "The branch's name \"" + b + "\" cannot be used.";
        if (b.empty() || b.front() == '-' || b.front() == '.' || b.front() == '/' || b.back() == '/' || b.back() == '.'
            || has(b, "..") || has(b, "//") || has(b, "/.") || has(b, "@{") || b == "@"
            || (b.size() >= 5 && b.compare(b.size() - 5, 5, ".lock") == 0) || b.size() > 200)
            return bad;
        for (const char ch : b)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) || c == '.' || c == '_' || c == '/' || c == '-'))
                return bad;
        }
        return {};
    }

    Outcome call(const Context& c, const std::vector<std::string>& args, bool network)
    {
        return callWith(c, args, network, {});
    }

    Outcome init(const Context& c, const std::string& branch)
    {
        if (const std::string why = branchRefusal(branch); !why.empty())
            return refused(why);
        if (c.git.empty())
            return refused(notFoundWords());   // nothing made
        std::error_code ec;
        fs::create_directories(files::pathOf(c.folder), ec);
        if (Outcome o = call(c, { "init", "-q" }); !o.ok)
            return o;
        // `init -b` needs git 2.28; HEAD pointed at the branch does the same on any.
        return call(c, { "symbolic-ref", "HEAD", "refs/heads/" + branch });
    }

    Outcome commitAll(const Context& c, const std::string& message, std::int64_t whenMs,
                      const std::vector<std::pair<std::string, std::string>>& trailers, std::string* sha, bool allowEmpty)
    {
        std::string text = trimmed(message);
        if (text.empty())
            return refused("A commit needs a message.");
        std::string lines;
        for (const auto& [key, value] : trailers)
        {
            const bool keyOk = !key.empty() && std::all_of(key.begin(), key.end(), [](char ch) {
                return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-';
            });
            if (!keyOk || value.empty() || value.find_first_of("\r\n") != std::string::npos)
                return refused("A commit's trailer \"" + key + "\" cannot be used.");
            lines += "\n" + key + ": " + value;
        }
        if (!lines.empty())
            text += "\n" + lines;
        if (Outcome o = call(c, { "add", "-A" }); !o.ok)
            return o;
        std::vector<std::pair<std::string, std::string>> env;
        if (whenMs > 0)
        {
            const std::string date = "@" + std::to_string(whenMs / 1000) + " +0000";
            env = { { "GIT_AUTHOR_DATE", date }, { "GIT_COMMITTER_DATE", date } };
        }
        std::vector<std::string> args{ "commit", "-q", "--no-verify", "--cleanup=whitespace" };
        if (allowEmpty)
            args.push_back("--allow-empty");
        args.push_back("-m");
        args.push_back(text);
        Outcome o = callWith(c, args, false, env);
        if (!o.ok)
        {
            // Git's words for it (their last lines, which `why` holds): "fatal: no email was given and auto-detection
            // is disabled", "fatal: empty ident name ... not allowed", "fatal: unable to auto-detect email address".
            if (has(o.why, "Please tell me who you are") || has(o.why, "empty ident") || has(o.why, "unable to auto-detect")
                || has(o.why, "auto-detection is disabled") || has(o.why, "no name was given") || has(o.why, "no email was given"))
                o.why = "Git does not know who you are, so it cannot commit. Set it once with these two commands:\n"
                        "git config --global user.name \"Your Name\"\n"
                        "git config --global user.email \"you@example.com\"";
            return o;
        }
        if (sha != nullptr)
            resolves(c, "HEAD", sha);
        return o;
    }

    Outcome addRemote(const Context& c, const std::string& name, const std::string& address)
    {
        if (const std::string why = remoteRefusal(name); !why.empty())
            return refused(why);
        if (const std::string why = addressRefusal(address, c.allowFile); !why.empty())
            return refused(why);
        return call(c, { "remote", "add", name, "--", address });
    }

    Outcome remoteHas(const Context& c, const std::string& remote, const std::string& branch, bool& hasIt, std::string* sha)
    {
        hasIt = false;
        if (const std::string why = remoteRefusal(remote); !why.empty())
            return refused(why);
        if (const std::string why = branchRefusal(branch); !why.empty())
            return refused(why);
        const std::string ref = "refs/heads/" + branch;
        Outcome o = call(c, { "ls-remote", "--heads", "--", remote, ref }, true);
        if (!o.ok)
            return o;
        std::istringstream in(o.out);
        std::string line;
        while (std::getline(in, line))
        {
            const std::size_t tab = line.find('\t');
            if (tab != std::string::npos && trimmed(line.substr(tab + 1)) == ref)
            {
                hasIt = true;
                if (sha != nullptr)
                    *sha = line.substr(0, tab);
            }
        }
        return o;
    }

    Outcome fetch(const Context& c, const std::string& remote, const std::string& branch)
    {
        if (const std::string why = remoteRefusal(remote); !why.empty())
            return refused(why);
        if (const std::string why = branchRefusal(branch); !why.empty())
            return refused(why);
        return call(c, { "fetch", "-q", "--no-tags", "--", remote, "+refs/heads/" + branch + ":" + trackingRef(remote, branch) },
                    true);
    }

    Outcome push(const Context& c, const std::string& remote, const std::string& branch, bool setUpstream)
    {
        if (const std::string why = remoteRefusal(remote); !why.empty())
            return refused(why);
        if (const std::string why = branchRefusal(branch); !why.empty())
            return refused(why);
        // An explicit refspec with no '+', and no --force of any kind: a remote that moved refuses it.
        std::vector<std::string> args{ "push", "--porcelain" };
        if (setUpstream)
            args.push_back("-u");
        args.push_back("--");
        args.push_back(remote);
        args.push_back("refs/heads/" + branch + ":refs/heads/" + branch);
        Outcome o = call(c, args, true);
        if (!o.ok && (has(o.out, "[rejected]") || has(o.why, "[rejected]"))
            && (has(o.out, "non-fast-forward") || has(o.out, "fetch first") || has(o.why, "non-fast-forward")
                || has(o.why, "fetch first")))
        {
            o.behind = true;
            o.why = "The remote has commits this one does not have: pull first. Nothing was pushed.";
        }
        return o;
    }

    Outcome counts(const Context& c, const std::string& remote, const std::string& branch, Counts& n)
    {
        n = {};
        if (const std::string why = remoteRefusal(remote); !why.empty())
            return refused(why);
        if (const std::string why = branchRefusal(branch); !why.empty())
            return refused(why);
        const std::string theirs = trackingRef(remote, branch);
        const bool mine = resolves(c, "HEAD");
        const bool remoteHasIt = resolves(c, theirs);
        const auto count = [&](const std::string& range, int& into) {
            Outcome o = call(c, { "rev-list", "--count", range });
            if (o.ok)
                into = std::atoi(trimmed(o.out).c_str());
            return o;
        };
        if (mine && remoteHasIt)
        {
            Outcome o = call(c, { "rev-list", "--left-right", "--count", "HEAD..." + theirs });
            if (o.ok && std::sscanf(o.out.c_str(), "%d %d", &n.ahead, &n.behind) != 2)
            {
                o.ok = false;
                o.why = "git rev-list said \"" + trimmed(o.out) + "\"";
            }
            return o;
        }
        if (mine)
            return count("HEAD", n.ahead);
        if (remoteHasIt)
            return count(theirs, n.behind);
        Outcome none;
        none.ok = true;
        none.ended = run::Ended::exited;
        none.code = 0;
        return none;
    }

    Outcome fastForward(const Context& c, const std::string& remote, const std::string& branch)
    {
        if (const std::string why = remoteRefusal(remote); !why.empty())
            return refused(why);
        if (const std::string why = branchRefusal(branch); !why.empty())
            return refused(why);
        return call(c, { "merge", "-q", "--ff-only", trackingRef(remote, branch) });
    }

    Outcome clone(const Context& c, const std::string& address, const std::string& folder, const std::string& branch)
    {
        if (const std::string why = addressRefusal(address, c.allowFile); !why.empty())
            return refused(why);
        if (!branch.empty())
            if (const std::string why = branchRefusal(branch); !why.empty())
                return refused(why);
        if (folder.empty())
            return refused("A clone needs a folder.");
        Context at = c;
        at.folder.clear();
        std::vector<std::string> args{ "clone", "-q", "--no-tags" };
        if (!branch.empty())
        {
            args.push_back("--branch");
            args.push_back(branch);
        }
        args.push_back("--");
        args.push_back(address);
        args.push_back(folder);
        return call(at, args, true);
    }

    Outcome log(const Context& c, int n, std::vector<Commit>& commits)
    {
        commits.clear();
        if (!resolves(c, "HEAD"))
        {
            Outcome none = call(c, { "rev-parse", "--git-dir" });   // a repository with no commit yet: ok, nothing
            return none;
        }
        return log(c, n, commits, "HEAD");
    }

    Outcome log(const Context& c, int n, std::vector<Commit>& commits, const std::string& range)
    {
        commits.clear();
        if (range.empty() || range.front() == '-')
            return refused("A log needs a range.");
        Outcome o = call(c, { "log", "-n", std::to_string(std::max(1, n)), "--format=%H%x1f%at%x1f%s%x1e", range, "--" });
        if (!o.ok)
            return o;
        std::size_t from = 0;
        for (;;)
        {
            const std::size_t end = o.out.find('\x1e', from);
            if (end == std::string::npos)
                break;
            const std::string record = trimmed(o.out.substr(from, end - from));
            from = end + 1;
            const std::size_t a = record.find('\x1f');
            const std::size_t b = a == std::string::npos ? a : record.find('\x1f', a + 1);
            if (b == std::string::npos)
                continue;
            Commit commit;
            commit.sha = record.substr(0, a);
            commit.timeMs = std::atoll(record.substr(a + 1, b - a - 1).c_str()) * 1000;
            commit.subject = record.substr(b + 1);
            commits.push_back(std::move(commit));
        }
        return o;
    }

    Outcome show(const Context& c, const std::string& rev, const std::string& path, std::string& bytes)
    {
        bytes.clear();
        if (rev.empty() || rev.front() == '-' || path.empty() || path.front() == '-' || path.find(':') != std::string::npos)
            return refused("A file \"" + path + "\" at \"" + rev + "\" cannot be read.");
        Outcome o = call(c, { "cat-file", "blob", rev + ":" + path });
        if (o.ok && o.outCut)
        {
            o.ok = false;
            o.why = path + " is larger than " + std::to_string(c.outLimit >> 20) + " MB.";
            return o;
        }
        if (o.ok)
            bytes = std::move(o.out);
        o.out.clear();
        return o;
    }

    Outcome files(const Context& c, const std::string& rev, std::vector<std::string>& paths)
    {
        paths.clear();
        if (rev.empty() || rev.front() == '-')
            return refused("A tree needs a commit.");
        Outcome o = call(c, { "ls-tree", "-r", "-z", "--name-only", rev, "--" });
        if (!o.ok)
            return o;
        std::size_t from = 0;
        while (from < o.out.size())
        {
            const std::size_t end = o.out.find('\0', from);
            const std::string path = o.out.substr(from, (end == std::string::npos ? o.out.size() : end) - from);
            if (!path.empty())
                paths.push_back(path);
            if (end == std::string::npos)
                break;
            from = end + 1;
        }
        return o;
    }

    std::string resolve(const Context& c, const std::string& rev)
    {
        std::string sha;
        if (rev.empty() || rev.front() == '-' || !resolves(c, rev, &sha))
            return {};
        return sha;
    }

    std::string currentBranch(const Context& c)
    {
        const Outcome o = call(c, { "symbolic-ref", "-q", "--short", "HEAD" });
        return o.ok ? trimmed(o.out) : std::string();
    }

    Outcome head(const Context& c, std::string& sha)
    {
        sha.clear();
        if (resolves(c, "HEAD", &sha))
        {
            Outcome o;
            o.ok = true;
            o.ended = run::Ended::exited;
            o.code = 0;
            return o;
        }
        return call(c, { "rev-parse", "--git-dir" });
    }
}
