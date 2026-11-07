// PROBE name=plugin.linux timeout=120
//
// plugin.linux: Linux's own pieces (built and run on Linux only: cmake/Probes.cmake leaves it out elsewhere), each in
// folders of the probe's own and with made-up devices: nothing of the user's is read or written (the environment is
// changed for a row and put back).
//   FOLDERS  the XDG base directories (fminus/Xdg.h): the projects in $XDG_DATA_HOME/Funk/<name>, the chats and keys in
//            $XDG_DATA_HOME/Funk/Playground, the shaders' cache in $XDG_CACHE_HOME/Funk/<name>/Shaders, each under the
//            home's defaults (~/.local/share, ~/.cache) when unset or relative; $XDG_RUNTIME_DIR taken only when it is
//            a folder of this user's that no one else may enter (0755 or a link: none); a shader build's temporary
//            folder in $TMPDIR when set, else $XDG_RUNTIME_DIR, else the system's; working them out writes nothing
//   MCP      the instances' sockets' folder (mcp/McpProtocol.h, with no <prefix>MCP_DIR): $XDG_RUNTIME_DIR's
//            Funk-<name>-mcp (made, 0700), else $TMPDIR's funk-<uid>/ (made 0700) holding it; a funk-<uid> that is a link
//            or that others may write into refused (no folder); a socket there listens and takes a connection (AF_UNIX),
//            its path within sun_path's 108 bytes
//   STANDALONE  the probe as the installed Standalone (hard-linked, else copied, into an app folder beside a
//            shaderc), started by name through a link in PATH from another folder, as `TestingPlayground` from a
//            terminal: JUCE's currentExecutableFile is then argv[0] against the working folder (the case reproduced),
//            and bundledShaderc(standalone) still finds the shaderc beside the program (/proc/self/exe)
//   SECRETS  the chat's keys (plugin/ChatStore.h) with a keyring of the probe's own (secrets::Store): saved there, no
//            file; read back; there per its kind and origin (another provider's server is none); forgotten in both
//            places; no keyring to ask: the file (FPK1), which moves into the keyring the next time the key is read
//            with one there; a keyring that refuses a key: the file; has() asked once for two looks in a row; a
//            keyring that fails to read: why says so; a key the keyring refused while it held an older one: the newer
//            (the file) read, then moved in, the Anthropic key and another provider's; a keyring left locked (its
//            prompt dismissed: a lookup finds nothing and says nothing, a clear removes nothing and says nothing): the
//            key kept and said locked, a forget false with why and the key still kept, then forgotten once unlocked;
//            the agent (a processor's): a turn's key read while the keyring's prompt waits, the chat's state asked
//            meanwhile and answered (the read holds no lock of the agent's), the turn then sent with that key; a
//            removal the locked keyring refuses said as an error (Anthropic's, Gemini's), the key still shown kept,
//            then removed and said so. The desktop's (libsecret): with no session bus to reach, not
//            available and saying why, and the key goes to its file (the container's case); with a bus address that
//            leads nowhere (libsecret loaded), each call returns its error, nothing hangs or crashes, and the key goes
//            to its file. The machine's key: /etc/machine-id's text, else D-Bus's, else "host:" and the host's name
//   WHEEL    the racing wheel over evdev and sysfs (plugin/WheelEvdev.h's Hub) with a made-up sysfs tree and nodes of
//            the probe's own (a socket a node: the probe writes the kernel's events into it and reads what the hub
//            writes): the G29 found among other devices (a keyboard of Logitech's, another vendor's; a G27 in
//            compatibility mode none, a G29 in it found); hid-input's codes made HID usages (buttons 1 to 16 and 17
//            on, the axes, the hat); on, the range file already 900 left alone (read-only, so a write would fail), the
//            spring as FF_AUTOCENTER 0x6000 on the node; its events through the driver to the same messages a Map
//            makes of the same values; the PS button the start key's note; the shift lights in RPM1 to RPM5's
//            brightness, bit i RPM<i+1>; SYN_DROPPED's events dropped until SYN_REPORT, then the state read again; off:
//            the spring 0, the lights 0; unplugged (end of the node): the rest messages, no longer present; plugged in
//            again (another input device): read again; a range of 540 written 900; a range of 540 it may not write
//            said on the card and no spring sent; lights it may not write said on the card; a node it may not open
//            listed and said, read once it may; a G29 in compatibility mode switched through alternate_modes
//            ("native"); the hub gone: the spring off
//   INSTALL  the Linux package's install script (scripts/linux-install.sh) over a made-up package of this product, for a
//            made-up home whose path holds spaces, quotes, a backtick and $(...): every part in its place (the VST3
//            in ~/.vst3, the app's folder in ~/.local/lib/<company>/<name> with its server/ and extension, the links in
//            ~/.local/bin, the menu entry whose Exec reads back as the app's path, the icon), nothing run from a path's
//            text, Claude Code's command printed for the server; again: the old copies in the Trash (their info files'
//            paths, percent-encoded), the new in place; a staging copy a stopped run left goes to the Trash; a dry run
//            changes nothing; a VST3 place that is a link, and a launcher that is not the script's, left alone and
//            said; uninstall: every part to the Trash, the links removed, a second one finds nothing; --help
#include "Probe.h"

#if defined(__linux__)
#include "FakeClaude.h"

#include "fminus/Shaders.h"
#include "fminus/Xdg.h"
#include "mcp/McpProtocol.h"
#include "plugin/ChatAgent.h"
#include "plugin/ChatStore.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"
#include "plugin/Providers.h"
#include "plugin/SecretStore.h"
#include "plugin/WheelDriver.h"
#include "plugin/WheelEvdev.h"
#include "plugin/WheelMap.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <fcntl.h>
#include <linux/input.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace fs = std::filesystem;
namespace wh = playground::wheel;
namespace ev = playground::wheel::evdev;

namespace
{
    void same(funkgui::test::Probe& P, const std::string& name, const std::string& got, const std::string& want)
    {
        if (got != want)
            std::printf("%s: got \"%s\", wanted \"%s\"\n", name.c_str(), got.c_str(), want.c_str());
        P.eq(name, got == want ? 1 : 0, 1);
    }

    void yes(funkgui::test::Probe& P, const std::string& name, bool ok, const std::string& about = {})
    {
        if (!ok && !about.empty())
            std::printf("%s: %s\n", name.c_str(), about.c_str());
        P.eq(name, ok ? 1 : 0, 1);
    }

    // The environment's variables changed for a row, put back as they were when it goes.
    struct Env
    {
        std::map<std::string, std::optional<std::string>> saved;
        void set(const std::string& k, const std::optional<std::string>& v)
        {
            if (saved.find(k) == saved.end())
            {
                const char* now = std::getenv(k.c_str());
                saved[k] = now != nullptr ? std::optional<std::string>(now) : std::nullopt;
            }
            if (v)
                ::setenv(k.c_str(), v->c_str(), 1);
            else
                ::unsetenv(k.c_str());
        }
        ~Env()
        {
            for (const auto& [k, v] : saved)
                if (v)
                    ::setenv(k.c_str(), v->c_str(), 1);
                else
                    ::unsetenv(k.c_str());
        }
    };

    std::string readText(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void writeText(const fs::path& p, const std::string& text)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
    }

    int modeOf(const fs::path& p)
    {
        struct stat st{};
        return ::lstat(p.c_str(), &st) == 0 ? static_cast<int>(st.st_mode & 0777) : -1;
    }

    // Every path under `root` (relative, sorted), for "nothing changed".
    std::string listing(const fs::path& root)
    {
        std::set<std::string> all;
        std::error_code ec;
        if (fs::exists(root, ec))
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::none, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                all.insert(fs::relative(it->path(), root, ec).string());
        std::string s;
        for (const std::string& p : all)
            s += p + "\n";
        return s;
    }

    // ---- SECRETS: a keyring of the probe's own -----------------------------------------------------------------------
    // `locked`: as libsecret over a keyring the user leaves locked (the prompt to unlock it dismissed): its search
    // (has) still finds the item, its lookup finds nothing and says nothing, its store fails, its clear removes nothing
    // and says nothing. `hold`: lookup waits until it is let go (a prompt the user has not answered yet), `inLookup`
    // saying so. Lookups may run on the agent's worker while the probe's thread looks: the maps are the probe's alone
    // then (it changes them only while no turn runs).
    struct FakeStore final : pg::secrets::Store
    {
        std::map<std::string, std::string> kept;             // kind \n origin -> secret
        std::map<std::string, std::string> labels;
        bool up = true, refuse = false, failRead = false, locked = false;
        std::atomic<bool> hold{ false }, inLookup{ false };
        std::atomic<int> hasCalls{ 0 };
        static std::string id(const pg::secrets::Item& i) { return i.kind + "\n" + i.origin; }
        std::string name() const override { return "the probe's keyring"; }
        bool available(std::string* why) override
        {
            if (!up && why != nullptr)
                *why = "down";
            return up;
        }
        int has(const pg::secrets::Item& i, std::string*) override
        {
            ++hasCalls;
            return kept.count(id(i)) != 0 ? 1 : 0;
        }
        std::optional<std::string> lookup(const pg::secrets::Item& i, std::string* why) override
        {
            inLookup = true;
            for (int waited = 0; hold.load() && waited < 3000; ++waited)   // 30 s at most: a probe never hangs
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            inLookup = false;
            if (failRead)
            {
                if (why != nullptr)
                    *why = "locked";
                return std::nullopt;
            }
            if (why != nullptr)
                why->clear();
            const auto it = kept.find(id(i));
            return it != kept.end() && !locked ? std::optional<std::string>(it->second) : std::nullopt;
        }
        bool store(const pg::secrets::Item& i, const std::string& label, std::string_view secret, std::string* why) override
        {
            if (refuse || locked)
            {
                if (why != nullptr)
                    *why = "refused";
                return false;
            }
            kept[id(i)] = std::string(secret);
            labels[id(i)] = label;
            return true;
        }
        bool clear(const pg::secrets::Item& i, std::string*) override
        {
            if (!locked)
                kept.erase(id(i));
            return true;
        }
    };

    // ---- WHEEL: the made-up kernel ------------------------------------------------------------------------------------
    struct Kernel final : ev::System
    {
        fs::path dev;                                        // the made-up /dev
        std::map<std::string, int> peers;                    // a node's path -> the probe's end of its socket
        std::set<std::string> denied;                        // nodes open() refuses (EACCES)
        std::set<int> watched;
        std::vector<std::string> log;                        // what the hub did, the timers' and the sends
        std::map<uint16_t, std::array<int32_t, 3>> absState{ { ABS_X, { 32768, 0, 65535 } },
                                                             { ABS_Y, { 255, 0, 255 } },
                                                             { ABS_Z, { 255, 0, 255 } },
                                                             { ABS_RZ, { 255, 0, 255 } },
                                                             { ABS_HAT0X, { 0, -1, 1 } },
                                                             { ABS_HAT0Y, { 0, -1, 1 } } };
        std::set<uint16_t> keysDown;
        bool armed[3] = { false, false, false };
        double clock = 50.0;
        int asked = -1;
        std::vector<std::vector<wh::Message>> sent;

        ~Kernel() override
        {
            for (const auto& [node, fd] : peers)
                ::close(fd);
        }

        int open(const std::string& node) override
        {
            if (denied.count(node) != 0)
            {
                errno = EACCES;
                return -1;
            }
            if (!fs::exists(node))
            {
                errno = ENOENT;
                return -1;
            }
            int sv[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) != 0)
                return -1;
            if (const auto it = peers.find(node); it != peers.end())
                ::close(it->second);
            peers[node] = sv[1];
            return sv[0];
        }
        bool abs(int, uint16_t code, int32_t& value, int32_t& min, int32_t& max) override
        {
            const auto it = absState.find(code);
            if (it == absState.end())
                return false;
            value = it->second[0];
            min = it->second[1];
            max = it->second[2];
            return true;
        }
        bool keys(int, std::vector<uint8_t>& bits) override
        {
            bits.assign(KEY_MAX / 8 + 1, 0);
            for (const uint16_t k : keysDown)
                bits[k / 8] = static_cast<uint8_t>(bits[k / 8] | (1u << (k % 8)));
            return true;
        }
        void watch(int fd) override { watched.insert(fd); }
        void unwatch(int fd) override { watched.erase(fd); }
        void arm(wh::Timer t, double) override { armed[static_cast<int>(t)] = true; }
        void disarm(wh::Timer t) override { armed[static_cast<int>(t)] = false; }
        double now() override { return clock; }
        void send(const std::vector<wh::Message>& m) override { sent.push_back(m); }
        void changed() override {}
        int lights() override { return asked; }

        // The kernel's events into a node.
        void events(const std::string& node, const std::vector<input_event>& es)
        {
            const int fd = peers.at(node);
            for (const input_event& e : es)
                (void)::write(fd, &e, sizeof e);
        }
        // What the hub wrote into a node (its EV_FF events), as "ff 97:24576 ...".
        std::string written(const std::string& node)
        {
            std::string s;
            input_event e{};
            while (::read(peers.at(node), &e, sizeof e) == static_cast<ssize_t>(sizeof e))
                s += (s.empty() ? "" : " ") + std::string(e.type == EV_FF ? "ff " : "? ") + std::to_string(e.code) + ":"
                   + std::to_string(e.value);
            return s;
        }
        // Each watched node's events read by the hub (the loop's part).
        void pump(ev::Hub& hub)
        {
            const std::set<int> now = watched;
            for (const int fd : now)
                hub.readable(fd);
        }
        void fire(ev::Hub& hub, wh::Timer t)
        {
            armed[static_cast<int>(t)] = false;
            hub.driver().fire(t);
        }
        std::string takeSent()
        {
            std::string s;
            for (const auto& batch : sent)
                for (const wh::Message& m : batch)
                    s += std::string(s.empty() ? "" : " ")
                       + (m.status == wh::Message::kCc ? "" : m.status == wh::Message::kNoteOn ? "on" : "off")
                       + std::to_string(m.number) + ":" + std::to_string(m.value);
            sent.clear();
            return s;
        }
    };

    input_event inputEvent(uint16_t type, uint16_t code, int32_t value, long micros = 1000)
    {
        input_event e{};
        e.input_event_sec = micros / 1000000;
        e.input_event_usec = micros % 1000000;
        e.type = type;
        e.code = code;
        e.value = value;
        return e;
    }

    std::string text(const std::vector<wh::Message>& ms)
    {
        std::string s;
        for (const wh::Message& m : ms)
            s += std::string(s.empty() ? "" : " ")
               + (m.status == wh::Message::kCc ? "" : m.status == wh::Message::kNoteOn ? "on" : "off")
               + std::to_string(m.number) + ":" + std::to_string(m.value);
        return s;
    }

    // A made-up sysfs input device: class/input/<event>/device -> devices/<input> (a link, as the kernel's), its ids,
    // and its HID device's folder with hid-lg4ff's files.
    fs::path makeDevice(const fs::path& sys, const fs::path& dev, const std::string& event, const std::string& input,
                        int vendor, int product, int version, bool lg4ff)
    {
        const fs::path in = sys / "devices" / input;
        char hex[8];
        std::snprintf(hex, sizeof hex, "%04x", vendor);
        writeText(in / "id/vendor", std::string(hex) + "\n");
        std::snprintf(hex, sizeof hex, "%04x", product);
        writeText(in / "id/product", std::string(hex) + "\n");
        std::snprintf(hex, sizeof hex, "%04x", version);
        writeText(in / "id/version", std::string(hex) + "\n");
        const fs::path hid = in / "device";
        fs::create_directories(hid);
        if (lg4ff)
        {
            writeText(hid / "range", "900\n");
            writeText(hid / "alternate_modes", "");
            for (int i = 1; i <= 5; ++i)
                writeText(hid / "leds" / ("0003:046D:C24F.0001::RPM" + std::to_string(i)) / "brightness", "0\n");
        }
        fs::create_directories(sys / "class/input" / event);
        std::error_code ec;
        fs::remove(sys / "class/input" / event / "device", ec);
        fs::create_directory_symlink(fs::path("../../../devices") / input, sys / "class/input" / event / "device");
        writeText(dev / "input" / event, "");
        return hid;
    }

    void unplug(const fs::path& sys, const fs::path& dev, const std::string& event)
    {
        std::error_code ec;
        fs::remove_all(sys / "class/input" / event, ec);
        fs::remove(dev / "input" / event, ec);
    }

    std::string leds(const fs::path& hid)
    {
        std::string s;
        for (int i = 1; i <= 5; ++i)
        {
            std::string b = readText(hid / "leds" / ("0003:046D:C24F.0001::RPM" + std::to_string(i)) / "brightness");
            while (!b.empty() && b.back() == '\n')
                b.pop_back();
            s += b;
        }
        return s;
    }

    // ---- INSTALL: the script run as a child with an environment of the probe's ---------------------------------------
    struct Run
    {
        int code = -1;
        std::string out;
    };

    Run runScript(const fs::path& script, const std::vector<std::string>& args, const std::map<std::string, std::string>& env,
                  const fs::path& cwd, const fs::path& outFile)
    {
        std::vector<std::string> argStore{ "bash", script.string() };
        argStore.insert(argStore.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (std::string& a : argStore)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        std::vector<std::string> envStore;
        for (const auto& [k, v] : env)
            envStore.push_back(k + "=" + v);
        std::vector<char*> envp;
        for (std::string& e : envStore)
            envp.push_back(e.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, outFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
        posix_spawn_file_actions_addclosefrom_np(&fa, 3);
        pid_t pid = -1;
        Run r;
        if (posix_spawn(&pid, "/bin/bash", &fa, nullptr, argv.data(), envp.data()) == 0)
        {
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            r.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
        posix_spawn_file_actions_destroy(&fa);
        r.out = readText(outFile);
        return r;
    }

    // A desktop entry's Exec value read back (one quoted argument): the string escapes, then the quoting.
    std::string execArgument(const std::string& value)
    {
        std::string s;                                       // the string escape: \\ -> \ (and \s \n \t \r)
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '\\' && i + 1 < value.size())
            {
                const char n = value[++i];
                s += n == 's' ? ' ' : n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
            }
            else
                s += value[i];
        }
        std::string out;                                     // the quoting: "...", \" \` \$ \\ inside; %% -> %
        if (s.size() < 2 || s.front() != '"' || s.back() != '"')
            return "(not quoted) " + s;
        for (std::size_t i = 1; i + 1 < s.size(); ++i)
        {
            if (s[i] == '\\' && i + 2 < s.size())
                out += s[++i];
            else if (s[i] == '%' && i + 2 < s.size() && s[i + 1] == '%')
                out += s[i++];
            else
                out += s[i];
        }
        return out;
    }

    std::string urlDecode(const std::string& s)
    {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i)
            if (s[i] == '%' && i + 2 < s.size())
            {
                out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            }
            else
                out += s[i];
        return out;
    }

    // The Trash's entries: their info files' paths (decoded), by the file's name.
    std::map<std::string, std::string> trashed(const fs::path& trash)
    {
        std::map<std::string, std::string> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(trash / "info", ec))
        {
            const std::string t = readText(e.path());
            const std::size_t at = t.find("Path=");
            if (at == std::string::npos)
                continue;
            const std::size_t end = t.find('\n', at);
            std::string name = e.path().filename().string();
            name = name.substr(0, name.size() - std::string(".trashinfo").size());
            out[name] = urlDecode(t.substr(at + 5, end - at - 5));
        }
        return out;
    }
}

PROBE(plugin, linux)
{
    namespace store = pg::chatstore;
    const std::string name = pg::product::kName;
    const std::string company = pg::product::kCompany;
    const std::string prefix = pg::product::kEnvPrefix;
    // The probe started again as the installed Standalone is, by name through PATH (STANDALONE, below): what it finds,
    // into the file named, and nothing else.
    if (const char* self = std::getenv((prefix + "LINUX_PROBE_SELF").c_str()); self != nullptr && *self != '\0')
    {
        writeText(self, juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().toStdString()
                            + "\n" + pg::bundledShaderc(true) + "\n");
        return P.finish();
    }
    const fs::path out = fs::path(probe::outPath("linux"));
    std::error_code ec;
    fs::remove_all(out, ec);
    fs::create_directories(out);
    // Short paths for sockets (sun_path: 108 bytes), under the system's temporary folder.
    std::string shortRoot = (fs::temp_directory_path(ec) / "tp-linux-XXXXXX").string();
    if (::mkdtemp(shortRoot.data()) == nullptr)
    {
        P.harnessError("no temporary folder");
        return P.finish();
    }
    const fs::path tmp = shortRoot;

    // ---- FOLDERS ------------------------------------------------------------------------------------------------------
    {
        Env env;
        const fs::path home = out / "home";
        fs::create_directories(home);
        env.set("HOME", home.string());
        env.set(prefix + "PREFS_DIR", std::nullopt);         // the stores' own defaults, not the probe's sandbox
        env.set("XDG_DATA_HOME", std::nullopt);
        env.set("XDG_CACHE_HOME", std::nullopt);
        same(P, "folders.projects.default", pg::projectstore::directory().getFullPathName().toStdString(),
             (home / ".local/share" / company / name).string());
        same(P, "folders.chats.default", store::directory().getFullPathName().toStdString(),
             (home / ".local/share/Funk/Playground").string());
        same(P, "folders.cache.default", pg::shaderCacheFolder().string(), (home / ".cache" / company / name / "Shaders").string());
        env.set("XDG_DATA_HOME", (out / "data").string());
        env.set("XDG_CACHE_HOME", (out / "cache").string());
        same(P, "folders.projects.xdg", pg::projectstore::directory().getFullPathName().toStdString(),
             (out / "data" / company / name).string());
        same(P, "folders.chats.xdg", store::directory().getFullPathName().toStdString(), (out / "data/Funk/Playground").string());
        same(P, "folders.cache.xdg", pg::shaderCacheFolder().string(), (out / "cache" / company / name / "Shaders").string());
        env.set("XDG_DATA_HOME", "relative/data");
        env.set("XDG_CACHE_HOME", "relative/cache");
        same(P, "folders.projects.relative_ignored", pg::projectstore::directory().getFullPathName().toStdString(),
             (home / ".local/share" / company / name).string());
        same(P, "folders.cache.relative_ignored", pg::shaderCacheFolder().string(), (home / ".cache" / company / name / "Shaders").string());
        same(P, "folders.nothing_written", listing(home) + listing(out / "data") + listing(out / "cache"), "");

        // $XDG_RUNTIME_DIR, and a shader build's temporary folder.
        const fs::path run = tmp / "run";
        fs::create_directories(run);
        ::chmod(run.c_str(), 0700);
        env.set("XDG_RUNTIME_DIR", run.string());
        same(P, "folders.runtime.private", pg::xdg::runtimeDir(), run.string());
        env.set("TMPDIR", std::nullopt);
        same(P, "folders.temp.runtime", pg::shaderTempBase(ec).string(), run.string());
        env.set("TMPDIR", (tmp / "t").string());
        fs::create_directories(tmp / "t");
        same(P, "folders.temp.tmpdir", pg::shaderTempBase(ec).string(), (tmp / "t").string());
        ::chmod(run.c_str(), 0755);
        same(P, "folders.runtime.open_refused", pg::xdg::runtimeDir(), "");
        env.set("TMPDIR", std::nullopt);
        yes(P, "folders.temp.system", pg::shaderTempBase(ec) == fs::temp_directory_path(ec));
        ::chmod(run.c_str(), 0700);
        fs::create_directory_symlink(run, tmp / "runlink", ec);
        env.set("XDG_RUNTIME_DIR", (tmp / "runlink").string());
        same(P, "folders.runtime.link_refused", pg::xdg::runtimeDir(), "");
    }

    // ---- MCP --------------------------------------------------------------------------------------------------------------
    {
        Env env;
        env.set(prefix + "MCP_DIR", std::nullopt);
        const fs::path run = tmp / "mrun";
        fs::create_directories(run);
        ::chmod(run.c_str(), 0700);
        env.set("XDG_RUNTIME_DIR", run.string());
        const std::string folder = pg::mcp::folder();
        same(P, "mcp.folder.runtime", folder, (run / ("Funk-" + name + "-mcp")).string());
        P.eq("mcp.folder.private", modeOf(folder), 0700);
        const std::string sock = folder + "/1.sock";
        yes(P, "mcp.socket.fits", pg::mcp::fits(sock), sock);
        const int l = pg::mcp::listenAt(sock);
        const int c = l >= 0 ? pg::mcp::connectTo(sock) : -1;
        const int a = c >= 0 ? pg::mcp::acceptOn(l) : -1;
        P.eq("mcp.socket.connects", l >= 0 && c >= 0 && a >= 0 ? 1 : 0, 1);
        if (c >= 0 && a >= 0)
        {
            const char hello[] = "{\"id\":1}\n";
            char got[16] = {};
            (void)pg::mcp::sendSome(c, hello, sizeof hello - 1);
            const long long n = pg::mcp::recvSome(a, got, sizeof got);
            same(P, "mcp.socket.carries", std::string(got, static_cast<std::size_t>(n > 0 ? n : 0)), "{\"id\":1}\n");
        }
        for (const int fd : { a, c, l })
            if (fd >= 0)
                pg::mcp::closeFd(fd);
        P.eq("mcp.socket.mode", modeOf(sock), 0600);
        pg::mcp::removePath(sock);

        env.set("XDG_RUNTIME_DIR", std::nullopt);
        const fs::path t = tmp / "mt";
        fs::create_directories(t);
        env.set("TMPDIR", t.string());
        const fs::path own = t / ("funk-" + std::to_string(::getuid()));
        same(P, "mcp.folder.tmpdir", pg::mcp::folder(), (own / ("Funk-" + name + "-mcp")).string());
        P.eq("mcp.folder.tmpdir_own_private", modeOf(own), 0700);
        ::chmod(own.c_str(), 0777);
        same(P, "mcp.folder.writable_refused", pg::mcp::folder(), "");
        ::chmod(own.c_str(), 0700);
        fs::remove_all(own, ec);
        fs::create_directories(t / "elsewhere");
        fs::create_directory_symlink(t / "elsewhere", own, ec);
        same(P, "mcp.folder.link_refused", pg::mcp::folder(), "");
        P.eq("mcp.folder.link_not_followed", fs::exists(t / "elsewhere" / ("Funk-" + name + "-mcp")) ? 1 : 0, 0);
    }

    // ---- STANDALONE ------------------------------------------------------------------------------------------------------
    {
        // The probe as the installed Standalone (a file of the app's folder, beside its shaderc), started by name through
        // the package's link in PATH from another folder: JUCE's currentExecutableFile is then argv[0] against the
        // working folder, and the shaderc beside the program is found all the same.
        const fs::path prog = out / "prog";
        const fs::path app = prog / "app", bin = prog / "bin", cwd = prog / "cwd";
        for (const fs::path& d : { app, bin, cwd })
            fs::create_directories(d);
        const fs::path probeExe = fs::read_symlink("/proc/self/exe", ec);
        ec.clear();
        fs::create_hard_link(probeExe, app / name, ec);      // its own file there: a link would resolve back
        if (ec)                                              // another file system (PROBE_OUT_DIR elsewhere): a copy
        {
            ec.clear();
            fs::copy_file(probeExe, app / name, ec);
            ::chmod((app / name).c_str(), 0755);
        }
        yes(P, "standalone.setup", !ec, "cannot put " + probeExe.string() + " into " + app.string() + ": " + ec.message());
        if (!ec)
        {
            writeText(app / "shaderc", "#!/bin/sh\n");
            ::chmod((app / "shaderc").c_str(), 0755);
            fs::create_symlink(app / name, bin / name, ec);
            writeText(prog / "run.sh", "cd \"$1\" || exit 9\nexec \"$2\" plugin.linux\n");
            const fs::path said = prog / "said.txt";
            const Run r = runScript(prog / "run.sh", { cwd.string(), name },
                                    { { "PATH", bin.string() + ":/usr/bin:/bin" }, { "HOME", (prog / "home").string() },
                                      { "LANG", "C.UTF-8" }, { "PROBE_OUT_DIR", (prog / "out").string() },
                                      { prefix + "LINUX_PROBE_SELF", said.string() } },
                                    prog, prog / "child.log");
            const std::string text = readText(said);
            const std::size_t nl = text.find('\n');
            if (nl == std::string::npos)
                std::printf("standalone: the child (exit %d) said nothing:\n%s\n", r.code, r.out.c_str());
            // The child sees its folders as the kernel has them (getcwd, /proc/self/exe): without links.
            same(P, "standalone.by_name.reproduced", text.substr(0, nl), (fs::canonical(cwd, ec) / name).string());
            same(P, "standalone.by_name.shaderc", nl != std::string::npos ? text.substr(nl + 1, text.find('\n', nl + 1) - nl - 1) : "",
                 (fs::canonical(app, ec) / "shaderc").string());
        }
        fs::remove_all(prog, ec);
    }

    // ---- SECRETS ----------------------------------------------------------------------------------------------------------
    {
        Env env;
        const juce::File dir = juce::File(juce::String((out / "chat").string())).getChildFile("Playground");
        store::setDirectoryForTests(dir);
        const juce::File keyFile = dir.getChildFile("anthropic-key.bin");
        const std::string key = fake::fakeKey();
        FakeStore ring;
        store::setSecretStoreForTests(&ring, true);
        same(P, "secrets.place", store::keyPlace(), "the probe's keyring");
        std::string why;
        yes(P, "secrets.save", store::saveKey(key, &why), why);
        same(P, "secrets.kept", ring.kept["anthropic\nhttps://api.anthropic.com"], key);
        same(P, "secrets.label", ring.labels["anthropic\nhttps://api.anthropic.com"], "Funk Playground: Anthropic API key");
        P.eq("secrets.no_file", keyFile.exists() ? 1 : 0, 0);
        P.eq("secrets.has", store::hasKey() ? 1 : 0, 1);
        same(P, "secrets.loads", store::loadKey(&why).value_or(""), key);
        ring.hasCalls = 0;
        store::setSecretStoreForTests(&ring, true);          // the cache emptied
        (void)store::hasKey();
        (void)store::hasKey();
        P.eq("secrets.has_asked_once", ring.hasCalls.load(), 1);
        yes(P, "secrets.forget", store::forgetKey());
        P.eq("secrets.forgotten", static_cast<int64_t>(ring.kept.size()), 0);
        P.eq("secrets.forgotten_has", store::hasKey() ? 1 : 0, 0);

        // Another provider's, bound to its server.
        const std::string openai = fake::fakeOpenAiKey();
        yes(P, "secrets.provider.save", store::saveProviderKey("openai", "https://api.openai.com", openai, &why), why);
        same(P, "secrets.provider.kept", ring.kept["openai\nhttps://api.openai.com"], openai);
        same(P, "secrets.provider.loads", store::loadProviderKey("openai", "https://api.openai.com", &why).value_or(""), openai);
        P.eq("secrets.provider.other_server", store::loadProviderKey("openai", "https://example.test").has_value() ? 1 : 0, 0);
        P.eq("secrets.provider.no_file", static_cast<int64_t>(dir.findChildFiles(juce::File::findFiles, false, "key-*").size()), 0);
        store::forgetProviderKey("openai", "https://api.openai.com");
        P.eq("secrets.provider.forgotten", ring.kept.count("openai\nhttps://api.openai.com") != 0 ? 1 : 0, 0);

        // No keyring to ask: the file; then one there: moved in.
        ring.up = false;
        same(P, "secrets.down.place", store::keyPlace(), "");
        yes(P, "secrets.down.save", store::saveKey(key, &why), why);
        P.eq("secrets.down.file", keyFile.existsAsFile() ? 1 : 0, 1);
        P.eq("secrets.down.not_kept", static_cast<int64_t>(ring.kept.size()), 0);
        same(P, "secrets.down.loads", store::loadKey(&why).value_or(""), key);
        ring.up = true;
        same(P, "secrets.up.loads", store::loadKey(&why).value_or(""), key);
        same(P, "secrets.up.moved_in", ring.kept["anthropic\nhttps://api.anthropic.com"], key);
        P.eq("secrets.up.file_gone", keyFile.exists() ? 1 : 0, 0);

        // A keyring that refuses: the file. One that fails to read: why says so.
        ring.kept.clear();
        ring.refuse = true;
        yes(P, "secrets.refused.save", store::saveKey(key, &why), why);
        P.eq("secrets.refused.file", keyFile.existsAsFile() ? 1 : 0, 1);
        ring.refuse = false;
        store::forgetKey();
        ring.failRead = true;
        why.clear();
        P.eq("secrets.failing.loads", store::loadKey(&why).has_value() ? 1 : 0, 0);
        yes(P, "secrets.failing.why", why.find("the probe's keyring: locked") != std::string::npos, why);
        ring.failRead = false;

        // A key saved while the keyring refused it (left locked), the keyring holding an older one: the newer, from its
        // file, then moved in once the keyring takes it. The same for another provider's.
        const std::string anthropicId = "anthropic\nhttps://api.anthropic.com", openaiId = "openai\nhttps://api.openai.com";
        const std::string older = key.substr(0, key.size() - 6) + "OLDKEY";
        const std::string olderAi = openai.substr(0, openai.size() - 6) + "OLDKEY";
        yes(P, "secrets.newer.save_old", store::saveKey(older, &why) && store::saveProviderKey("openai", "https://api.openai.com", olderAi, &why), why);
        ring.refuse = true;
        yes(P, "secrets.newer.save_new", store::saveKey(key, &why) && keyFile.existsAsFile()
                                             && store::saveProviderKey("openai", "https://api.openai.com", openai, &why), why);
        same(P, "secrets.newer.ring_old", ring.kept[anthropicId], older);
        same(P, "secrets.newer.loads_new_refused", store::loadKey(&why).value_or(""), key);
        ring.refuse = false;
        store::setSecretStoreForTests(&ring, true);          // as another process: nothing cached
        same(P, "secrets.newer.loads_new", store::loadKey(&why).value_or(""), key);
        same(P, "secrets.newer.moved_in", ring.kept[anthropicId], key);
        P.eq("secrets.newer.file_gone", keyFile.exists() ? 1 : 0, 0);
        same(P, "secrets.newer.provider_loads_new", store::loadProviderKey("openai", "https://api.openai.com", &why).value_or(""), openai);
        same(P, "secrets.newer.provider_moved_in", ring.kept[openaiId], openai);
        P.eq("secrets.newer.provider_file_gone", static_cast<int64_t>(dir.findChildFiles(juce::File::findFiles, false, "key-*").size()), 0);

        // A keyring left locked (the user dismissed its prompt): the key kept but not read, and said so; not removed by
        // a forget, which says so (libsecret's clear removes no locked item and says nothing); removed once unlocked.
        ring.locked = true;
        store::setSecretStoreForTests(&ring, true);
        why.clear();
        P.eq("secrets.locked.loads_none", store::loadKey(&why).has_value() ? 1 : 0, 0);
        yes(P, "secrets.locked.why", why.find("the probe's keyring is locked") != std::string::npos, why);
        P.eq("secrets.locked.has", store::hasKey() ? 1 : 0, 1);
        std::string forgetWhy;
        P.eq("secrets.locked.forget_fails", store::forgetKey(&forgetWhy) ? 1 : 0, 0);
        yes(P, "secrets.locked.forget_why", forgetWhy.find("the probe's keyring still holds it") != std::string::npos, forgetWhy);
        same(P, "secrets.locked.still_kept", ring.kept[anthropicId], key);
        P.eq("secrets.locked.still_has", store::hasKey() ? 1 : 0, 1);
        forgetWhy.clear();
        P.eq("secrets.locked.provider_forget_fails", store::forgetProviderKey("openai", "https://api.openai.com", &forgetWhy) ? 1 : 0, 0);
        yes(P, "secrets.locked.provider_forget_why", forgetWhy.find("still holds it") != std::string::npos, forgetWhy);
        P.eq("secrets.locked.provider_still_has", store::hasProviderKey("openai", "https://api.openai.com") ? 1 : 0, 1);
        ring.locked = false;
        yes(P, "secrets.unlocked.forget", store::forgetKey(&forgetWhy) && store::forgetProviderKey("openai", "https://api.openai.com", &forgetWhy), forgetWhy);
        P.eq("secrets.unlocked.forgotten", store::hasKey() || store::hasProviderKey("openai", "https://api.openai.com") ? 1 : 0, 0);
        P.eq("secrets.unlocked.ring_empty", static_cast<int64_t>(ring.kept.size()), 0);

        // The agent: a turn reads the key without the agent's lock (the keyring's prompt may wait for the user while the
        // editor asks for the chat's state); a removal the keyring refuses is said as such, the key still shown kept.
        {
            const auto lastOf = [](const pg::ChatState& st, pg::ChatEntry::Kind kind) {
                for (auto it = st.entries.rbegin(); it != st.entries.rend(); ++it)
                    if (it->kind == kind)
                        return it->text;
                return std::string();
            };
            pg::Processor processor;
            pg::ChatAgent& agent = processor.chatAgent();
            auto script = std::make_shared<fake::Script>();
            script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "high" } } });
            script->add(fake::Response::ok(fake::Sse().start().text("Hello.").end()));
            agent.setTransport(std::make_unique<fake::Transport>(script));
            yes(P, "secrets.agent.saved", store::saveKey(key, &why), why);   // kept; the agent has not read it
            ring.hold = true;                                // its prompt not answered yet
            agent.send("hello");
            for (int i = 0; i < 3000 && !ring.inLookup.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            P.eq("secrets.agent.reading", ring.inLookup.load() ? 1 : 0, 1);
            auto state = std::async(std::launch::async, [&agent] { return agent.chatState(); });
            P.eq("secrets.agent.state_while_reading", state.wait_for(std::chrono::seconds(5)) == std::future_status::ready ? 1 : 0, 1);
            ring.hold = false;
            state.wait();
            agent.waitIdle();
            same(P, "secrets.agent.answered", lastOf(agent.chatState(), pg::ChatEntry::Kind::assistant), "Hello.");
            same(P, "secrets.agent.sent_with_key", script->lastHeader("POST /v1/messages", "x-api-key"), key);

            ring.locked = true;
            agent.forgetKey();
            const std::string said = lastOf(agent.chatState(), pg::ChatEntry::Kind::error);
            yes(P, "secrets.agent.not_removed_said", said.find("could not be removed") != std::string::npos
                                                         && said.find("still holds it") != std::string::npos, said);
            P.eq("secrets.agent.still_shown", agent.chatState().hasKey ? 1 : 0, 1);
            const pg::ChatProvider gemini{ "gemini", "", "" };
            const std::string geminiOrigin = pg::providers::originOf(pg::providers::baseOf(gemini));
            ring.locked = false;
            yes(P, "secrets.agent.gemini_saved", store::saveProviderKey("gemini", geminiOrigin, fake::fakeGeminiKey(), &why), why);
            ring.locked = true;
            agent.forgetProviderKey(gemini);
            const std::string saidGemini = lastOf(agent.chatState(), pg::ChatEntry::Kind::error);
            yes(P, "secrets.agent.provider_not_removed_said", saidGemini.find("for " + pg::providers::nameOf(gemini) + " could not be removed") != std::string::npos,
                saidGemini);
            ring.locked = false;
            agent.forgetKey();
            agent.forgetProviderKey(gemini);
            const std::string removed = lastOf(agent.chatState(), pg::ChatEntry::Kind::note);
            yes(P, "secrets.agent.removed_said", removed.find("removed from") != std::string::npos, removed);
            P.eq("secrets.agent.gone", agent.chatState().hasKey || store::hasProviderKey("gemini", geminiOrigin) ? 1 : 0, 0);
        }

        // The desktop's, with no session bus to reach: none, and the file. (libsecret's own file backend, which
        // SECRET_BACKEND=file chooses, is never reached from here.)
        pg::secrets::Store* desk = pg::secrets::desktop();
        yes(P, "secrets.desktop.exists", desk != nullptr);
        env.set("SECRET_BACKEND", std::nullopt);
        env.set("DBUS_SESSION_BUS_ADDRESS", std::nullopt);
        const fs::path run = tmp / "srun";
        fs::create_directories(run);
        ::chmod(run.c_str(), 0700);
        env.set("XDG_RUNTIME_DIR", run.string());
        std::string busWhy;
        P.eq("secrets.desktop.no_bus", pg::secrets::sessionBus(&busWhy) ? 1 : 0, 0);
        yes(P, "secrets.desktop.no_bus_why", busWhy.find("no session bus") != std::string::npos, busWhy);
        P.eq("secrets.desktop.unavailable", desk != nullptr && desk->available(nullptr) ? 1 : 0, 0);
        store::setSecretStoreForTests(desk, true);
        same(P, "secrets.desktop.place", store::keyPlace(), "");
        yes(P, "secrets.desktop.file", store::saveKey(key, &why) && keyFile.existsAsFile(), why);
        same(P, "secrets.desktop.file_loads", store::loadKey(&why).value_or(""), key);
        store::forgetKey();

        // libsecret's calls against a bus address that leads nowhere: each says why, and the key still goes to its file.
        std::string libWhy;
        const bool lib = pg::secrets::libsecretLoaded(&libWhy);
        P.note("libsecret", lib ? "\"loaded\"" : "\"not installed\"");
        if (lib && desk != nullptr)
        {
            env.set("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + (tmp / "no-bus").string());
            P.eq("secrets.dead_bus.available", desk->available(nullptr) ? 1 : 0, 1);
            const pg::secrets::Item probeItem{ "probe", "https://probe.invalid" };
            std::string w1, w2, w3, w4;
            P.eq("secrets.dead_bus.has", desk->has(probeItem, &w1), -1);
            yes(P, "secrets.dead_bus.has_why", !w1.empty());
            P.eq("secrets.dead_bus.lookup", desk->lookup(probeItem, &w2).has_value() ? 1 : 0, 0);
            yes(P, "secrets.dead_bus.lookup_why", !w2.empty());
            P.eq("secrets.dead_bus.store", desk->store(probeItem, "probe", "nothing", &w3) ? 1 : 0, 0);
            yes(P, "secrets.dead_bus.store_why", !w3.empty());
            P.eq("secrets.dead_bus.clear", desk->clear(probeItem, &w4) ? 1 : 0, 0);
            yes(P, "secrets.dead_bus.clear_why", !w4.empty());
            std::printf("secrets.dead_bus: %s\n", w2.c_str());
            yes(P, "secrets.dead_bus.file", store::saveKey(key, &why) && keyFile.existsAsFile(), why);
            same(P, "secrets.dead_bus.file_loads", store::loadKey(&why).value_or(""), key);
            store::forgetKey();
        }
        else
            yes(P, "secrets.libsecret.why", !libWhy.empty(), "not loaded, and no reason");
        store::setSecretStoreForTests(nullptr, false);
        store::setDirectoryForTests(juce::File());

        // The machine's key.
        std::string want;
        for (const char* path : { "/etc/machine-id", "/var/lib/dbus/machine-id" })
            if (want.empty())
                want = juce::File(path).loadFileAsString().trim().toStdString();
        const std::string machine = store::machineKey();
        if (!want.empty())
            same(P, "secrets.machine_key", machine, want);
        else
            yes(P, "secrets.machine_key_host", machine.rfind("host:", 0) == 0 && machine.size() > 5, machine);
    }

    // ---- WHEEL --------------------------------------------------------------------------------------------------------------
    {
        const fs::path sys = out / "sys", dev = out / "dev";
        fs::create_directories(sys / "class/input");
        fs::create_directories(dev / "input");
        makeDevice(sys, dev, "event3", "input20", 0x046d, 0xc31c, 0x6400, false);   // a Logitech keyboard
        makeDevice(sys, dev, "event4", "input21", 0x1234, 0xc24f, 0x0100, false);   // another vendor's
        makeDevice(sys, dev, "event6", "input23", 0x046d, 0xc294, 0x1230, false);   // a G27 in compatibility mode
        const fs::path hid = makeDevice(sys, dev, "event5", "input22", 0x046d, 0xc24f, 0x0100, true);
        const std::string node = (dev / "input/event5").string();
        const ev::Paths paths{ sys.string(), dev.string() };

        // SCAN and the mapping.
        {
            const auto found = ev::scan(paths);
            std::string s;
            for (const ev::Found& f : found)
                s += f.event + "/" + f.input + " ";
            same(P, "wheel.scan.g29_alone", s, "event5/input22 ");
            if (!found.empty())
            {
                same(P, "wheel.scan.node", found[0].node, node);
                P.eq("wheel.scan.kind", found[0].kind == wh::Kind::g29 ? 1 : 0, 1);
            }
            makeDevice(sys, dev, "event7", "input24", 0x046d, 0xc294, 0x1350, true);
            const auto two = ev::scan(paths);
            P.eq("wheel.scan.compat_found", two.size() == 2 && two[1].kind == wh::Kind::compat ? 1 : 0, 1);
            unplug(sys, dev, "event7");
            P.eq("wheel.map.button1", ev::buttonOf(BTN_TRIGGER), 1);
            P.eq("wheel.map.button16", ev::buttonOf(BTN_DEAD), 16);
            P.eq("wheel.map.button17", ev::buttonOf(BTN_TRIGGER_HAPPY1), 17);
            P.eq("wheel.map.button25", ev::buttonOf(BTN_TRIGGER_HAPPY9), 25);
            P.eq("wheel.map.gamepad1", ev::buttonOf(BTN_SOUTH), 1);
            P.eq("wheel.map.not_a_button", ev::buttonOf(KEY_A), 0);
            P.eq("wheel.map.x", ev::usageOf(ABS_X), 0x30);
            P.eq("wheel.map.rz", ev::usageOf(ABS_RZ), 0x35);
            P.eq("wheel.map.hat_not_an_axis", ev::usageOf(ABS_HAT0X), 0);
            std::string hats;
            for (const auto& [x, y] : std::vector<std::pair<int, int>>{ { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, 0 } })
                hats += std::to_string(ev::hatOf(x, y));
            same(P, "wheel.map.hat", hats, "012345678");
        }

        Kernel k;
        k.dev = dev;
        fs::permissions(hid / "range", fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
        {
            ev::Hub hub(k, paths, true);
            hub.rescan();
            P.eq("wheel.on.present", hub.driver().present() ? 1 : 0, 1);
            P.eq("wheel.on.watched", static_cast<int64_t>(k.watched.size()), 1);
            same(P, "wheel.on.status", hub.driver().status(), "");
            same(P, "wheel.on.range_left", readText(hid / "range"), "900\n");
            same(P, "wheel.on.spring", k.written(node), "ff 97:24576");
            const std::string rest = k.takeSent();
            yes(P, "wheel.on.rest_first", !rest.empty(), rest);

            // Events: the steering full right, the throttle down, cross, the d-pad right; then the PS button.
            k.events(node, { inputEvent(EV_ABS, ABS_X, 65535), inputEvent(EV_ABS, ABS_Z, 0), inputEvent(EV_KEY, BTN_TRIGGER, 1),
                             inputEvent(EV_ABS, ABS_HAT0X, 1), inputEvent(EV_SYN, SYN_REPORT, 0) });
            k.pump(hub);
            P.eq("wheel.events.flush_armed", k.armed[static_cast<int>(wh::Timer::flush)] ? 1 : 0, 1);
            k.fire(hub, wh::Timer::flush);
            wh::Map ref;
            {
                std::vector<wh::Message> m;
                ref.flush(m);
                ref.input(wh::kDesktop, 0x30, 65535, 0, 65535);
                ref.input(wh::kDesktop, 0x32, 0, 0, 255);
                ref.input(wh::kButtonPage, 1, 1, 0, 1);
                ref.input(wh::kDesktop, wh::kHat, 2, 0, 7);
                m.clear();
                ref.flush(m);
                same(P, "wheel.events.as_map", k.takeSent(), text(m));
            }
            k.events(node, { inputEvent(EV_KEY, BTN_TRIGGER_HAPPY9, 1, 2000), inputEvent(EV_SYN, SYN_REPORT, 0, 2000) });
            k.pump(hub);
            k.clock += 1.0;
            k.fire(hub, wh::Timer::flush);
            const std::string start = k.takeSent();
            yes(P, "wheel.events.start_key", start.find("on36:100") != std::string::npos, start);

            // The shift lights.
            k.asked = 0x05;
            k.fire(hub, wh::Timer::lights);
            same(P, "wheel.lights.sent", leds(hid), "10100");
            k.asked = 0x1F;
            k.fire(hub, wh::Timer::lights);
            same(P, "wheel.lights.all", leds(hid), "11111");

            // SYN_DROPPED: dropped until SYN_REPORT, then the state again: as the kernel holds it (what was reported,
            // and the throttle now half way).
            k.absState[ABS_X] = { 65535, 0, 65535 };
            k.absState[ABS_HAT0X] = { 1, -1, 1 };
            k.keysDown = { BTN_TRIGGER, BTN_TRIGGER_HAPPY9 };
            k.absState[ABS_Z] = { 128, 0, 255 };
            k.events(node, { inputEvent(EV_SYN, SYN_DROPPED, 0, 3000), inputEvent(EV_ABS, ABS_Z, 5, 3000),
                             inputEvent(EV_SYN, SYN_REPORT, 0, 3000) });
            k.pump(hub);
            k.clock += 1.0;
            k.fire(hub, wh::Timer::flush);
            {
                std::vector<wh::Message> m;
                ref.input(wh::kButtonPage, 25, 1, 0, 1);
                ref.flush(m);
                ref.input(wh::kDesktop, 0x32, 128, 0, 255);
                m.clear();
                ref.flush(m);
                same(P, "wheel.dropped.resynced", k.takeSent(), text(m));
            }

            // Off: the spring 0, the lights off.
            hub.driver().setOn(false);
            same(P, "wheel.off.spring", k.written(node), "ff 97:0");
            same(P, "wheel.off.lights", leds(hid), "00000");
            hub.driver().setOn(true);
            same(P, "wheel.on_again.spring", k.written(node), "ff 97:24576");
            k.takeSent();

            // Unplugged: the node's end, then the kernel's list without it.
            ::close(k.peers.at(node));
            k.peers.erase(node);
            k.pump(hub);
            const std::string gone = k.takeSent();
            yes(P, "wheel.unplug.rest", !gone.empty(), "nothing sent");
            P.eq("wheel.unplug.not_present", hub.driver().present() ? 1 : 0, 0);
            P.eq("wheel.unplug.unwatched", static_cast<int64_t>(k.watched.size()), 0);
            unplug(sys, dev, "event5");
            hub.rescan();
            P.eq("wheel.unplug.rescan", hub.driver().present() ? 1 : 0, 0);

            // Plugged in again (another input device), its range 540: written 900.
            const fs::path hid2 = makeDevice(sys, dev, "event5", "input25", 0x046d, 0xc24f, 0x0100, true);
            writeText(hid2 / "range", "540\n");
            hub.rescan();
            P.eq("wheel.replug.present", hub.driver().present() ? 1 : 0, 1);
            same(P, "wheel.replug.range_written", readText(hid2 / "range"), "900");
            same(P, "wheel.replug.spring", k.written(node), "ff 97:24576");

            // Lights it may not write: said on the card.
            for (int i = 1; i <= 5; ++i)
                fs::permissions(hid2 / "leds" / ("0003:046D:C24F.0001::RPM" + std::to_string(i)) / "brightness", fs::perms::owner_read);
            k.asked = 0x03;
            k.fire(hub, wh::Timer::lights);
            same(P, "wheel.lights.root_said", hub.driver().status(), "its shift lights were not set (it is root's: see the README for a udev rule)");
            k.asked = -1;
        }
        same(P, "wheel.quit.spring_off", k.written(node), "ff 97:0");

        // A range of 540 it may not write: said, and no spring.
        {
            writeText(fs::path(sys / "devices/input25/device/range"), "540\n");
            fs::permissions(sys / "devices/input25/device/range", fs::perms::owner_read);
            ev::Hub hub(k, paths, true);
            hub.rescan();
            same(P, "wheel.range.root_said", hub.driver().status(),
                 "its range and spring were not set (it is root's: see the README for a udev rule)");
            same(P, "wheel.range.no_spring", k.written(node), "");
            fs::permissions(sys / "devices/input25/device/range", fs::perms::owner_read | fs::perms::owner_write);
        }
        k.written(node);

        // A node it may not open: listed and said; read once it may.
        {
            k.denied.insert(node);
            ev::Hub hub(k, paths, true);
            hub.rescan();
            P.eq("wheel.denied.present", hub.driver().present() ? 1 : 0, 1);
            same(P, "wheel.denied.said", hub.driver().status(),
                 "the system keeps it from this app: its device needs your read and write access (see the README)");
            k.denied.clear();
            hub.rescan();
            same(P, "wheel.denied.then_read", k.written(node), "ff 97:24576");
            same(P, "wheel.denied.then_status", hub.driver().status(), "");
        }
        k.written(node);
        unplug(sys, dev, "event5");

        // A G29 in compatibility mode (the kernel told not to switch it): "native" to alternate_modes.
        {
            const fs::path hid3 = makeDevice(sys, dev, "event8", "input26", 0x046d, 0xc294, 0x1350, true);
            ev::Hub hub(k, paths, true);
            hub.rescan();
            same(P, "wheel.switch.native", readText(hid3 / "alternate_modes"), "native");
            same(P, "wheel.switch.status", hub.driver().status(), "switching to its full mode");
            P.eq("wheel.switch.timer", k.armed[static_cast<int>(wh::Timer::switched)] ? 1 : 0, 1);
        }
    }

    // ---- INSTALL ------------------------------------------------------------------------------------------------------
    {
        const fs::path script = fs::path(PROBE_SOURCE_DIR) / "scripts/linux-install.sh";
        const fs::path pkg = out / "pkg";
        const fs::path prod = pkg / "products" / name;
        const std::string id = pg::product::kBundleId;
        std::string mcpId = name;
        for (char& c : mcpId)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        fs::create_directories(prod);
        fs::copy_file(script, pkg / "install.sh", fs::copy_options::overwrite_existing, ec);
        writeText(prod / (name + ".vst3/Contents/x86_64-linux/" + name + ".so"), "one");
        writeText(prod / ("app/" + name), "#!/bin/sh\necho app\n");
        writeText(prod / "app/shaderc", "#!/bin/sh\n");
        writeText(prod / ("server/" + name + "MCP"), "#!/bin/sh\n");
        writeText(prod / ("server/" + name + "Render"), "#!/bin/sh\n");
        writeText(prod / ("server/" + name + "GuiSandbox"), "#!/bin/sh\n");
        for (const std::string& f : { "app/" + name, std::string("app/shaderc"), "server/" + name + "MCP", "server/" + name + "Render",
                                     "server/" + name + "GuiSandbox" })
            ::chmod((prod / f).c_str(), 0755);
        writeText(prod / (name + ".mcpb"), "PK");
        writeText(prod / "icons/256.png", "png");
        writeText(prod / "product.env", "ID=" + id + "\nVERSION=" + pg::product::kVersion + "\nCOMPANY=" + company + "\nKIND="
                                            + pg::product::kKind + "\nMCP=" + mcpId + "\nTITLE=Audio effect\n");
        // A home whose path a shell would trip over: spaces, quotes, a backtick, $(...).
        const fs::path home = out / "h o m e" / "it's \"q\" `touch BT` $(touch MARK)";
        fs::create_directories(home);
        const fs::path work = out / "cwd";
        fs::create_directories(work);
        std::map<std::string, std::string> env{ { "HOME", home.string() },
                                                { "PATH", "/usr/local/bin:/usr/bin:/bin" },
                                                { "LANG", "C.UTF-8" } };
        const fs::path log = out / "install.log";
        const fs::path vst3 = home / ".vst3" / (name + ".vst3");
        const fs::path lib = home / ".local/lib" / company / name;
        const fs::path desktop = home / ".local/share/applications" / (id + ".desktop");
        const fs::path trash = home / ".local/share/Trash";

        const Run first = runScript(pkg / "install.sh", {}, env, work, log);
        P.eq("install.exit", first.code, 0);
        if (first.code != 0)
            std::printf("%s\n", first.out.c_str());
        same(P, "install.vst3", readText(vst3 / ("Contents/x86_64-linux/" + name + ".so")), "one");
        P.eq("install.app", ::access((lib / name).c_str(), X_OK) == 0 ? 1 : 0, 1);
        P.eq("install.shaderc", fs::exists(lib / "shaderc") ? 1 : 0, 1);
        P.eq("install.server", ::access((lib / "server" / (name + "MCP")).c_str(), X_OK) == 0 ? 1 : 0, 1);
        P.eq("install.extension", fs::exists(lib / (name + ".mcpb")) ? 1 : 0, 1);
        same(P, "install.link", fs::read_symlink(home / ".local/bin" / name, ec).string(), (lib / name).string());
        same(P, "install.render_link", fs::read_symlink(home / ".local/bin" / (name + "Render"), ec).string(),
             (lib / "server" / (name + "Render")).string());
        const std::string entry = readText(desktop);
        std::string exec;
        if (const std::size_t at = entry.find("\nExec="); at != std::string::npos)
            exec = entry.substr(at + 6, entry.find('\n', at + 1) - at - 6);
        same(P, "install.desktop.exec", execArgument(exec), (lib / name).string());
        yes(P, "install.desktop.marked", entry.find("\nX-Funk-Product=" + name + "\n") != std::string::npos);
        yes(P, "install.desktop.icon", entry.find("\nIcon=" + id + "\n") != std::string::npos);
        P.eq("install.icon", fs::exists(home / ".local/share/icons/hicolor/256x256/apps" / (id + ".png")) ? 1 : 0, 1);
        P.eq("install.nothing_ran", fs::exists(work / "MARK") || fs::exists(work / "BT") || fs::exists(home / "MARK") ? 1 : 0, 0);
        yes(P, "install.claude_code", first.out.find("claude mcp add --scope user " + mcpId + " -- ") != std::string::npos, first.out);
        P.eq("install.no_staging_left", listing(home).find(".installing-") != std::string::npos ? 1 : 0, 0);

        // Again, with a new build and a stopped run's staging copy beside the VST3: the old copies to the Trash.
        writeText(prod / (name + ".vst3/Contents/x86_64-linux/" + name + ".so"), "two");
        writeText(home / ".vst3" / ("." + name + ".vst3.installing-999") / "x", "left");
        const Run again = runScript(pkg / "install.sh", {}, env, work, log);
        P.eq("again.exit", again.code, 0);
        same(P, "again.vst3", readText(vst3 / ("Contents/x86_64-linux/" + name + ".so")), "two");
        const auto t = trashed(trash);
        int vstInTrash = 0;
        for (const auto& [file, was] : t)
            if (was == vst3.string() && readText(trash / "files" / file / ("Contents/x86_64-linux/" + name + ".so")) == "one")
                ++vstInTrash;
        P.eq("again.old_vst3_in_trash", vstInTrash, 1);
        int libInTrash = 0, leftInTrash = 0;
        for (const auto& [file, was] : t)
        {
            libInTrash += was == lib.string() ? 1 : 0;
            leftInTrash += was == (home / ".vst3" / ("." + name + ".vst3.installing-999")).string() ? 1 : 0;
        }
        P.eq("again.old_app_in_trash", libInTrash, 1);
        P.eq("again.staging_left_to_trash", leftInTrash, 1);

        // A dry run changes nothing.
        const std::string before = listing(home);
        const Run dry = runScript(pkg / "install.sh", { "--dry-run" }, env, work, log);
        P.eq("dry.exit", dry.code, 0);
        same(P, "dry.unchanged", listing(home), before);
        yes(P, "dry.says", dry.out.find("would replace " + vst3.string()) != std::string::npos, dry.out);

        // A VST3 place that is a link, and a launcher that is not the script's: left alone, said.
        const fs::path elsewhere = out / "elsewhere.vst3";
        fs::create_directories(elsewhere);
        fs::rename(vst3, out / "moved.vst3", ec);
        fs::create_directory_symlink(elsewhere, vst3, ec);
        fs::remove(home / ".local/bin" / name, ec);
        writeText(home / ".local/bin" / name, "#!/bin/sh\necho mine\n");
        const Run links = runScript(pkg / "install.sh", {}, env, work, log);
        P.eq("links.exit", links.code, 0);
        same(P, "links.vst3_link_kept", fs::read_symlink(vst3, ec).string(), elsewhere.string());
        P.eq("links.nothing_through_it", fs::is_empty(elsewhere, ec) ? 1 : 0, 1);
        yes(P, "links.vst3_said", links.out.find("is a link: left alone") != std::string::npos, links.out);
        same(P, "links.launcher_kept", readText(home / ".local/bin" / name), "#!/bin/sh\necho mine\n");
        yes(P, "links.launcher_said", links.out.find("is not a link this script made") != std::string::npos, links.out);
        fs::remove(vst3, ec);
        fs::rename(out / "moved.vst3", vst3, ec);
        fs::remove(home / ".local/bin" / name, ec);

        // Uninstall: every part to the Trash, the links removed; then nothing to find.
        const Run un = runScript(pkg / "install.sh", { "--uninstall" }, env, work, log);
        P.eq("uninstall.exit", un.code, 0);
        P.eq("uninstall.vst3_gone", fs::exists(vst3) ? 1 : 0, 0);
        P.eq("uninstall.app_gone", fs::exists(lib) ? 1 : 0, 0);
        P.eq("uninstall.desktop_gone", fs::exists(desktop) ? 1 : 0, 0);
        P.eq("uninstall.render_link_gone", fs::is_symlink(home / ".local/bin" / (name + "Render")) ? 1 : 0, 0);
        int inTrash = 0;
        for (const auto& [file, was] : trashed(trash))
            inTrash += was == vst3.string() || was == lib.string() || was == desktop.string() ? 1 : 0;
        P.ge("uninstall.in_trash", inTrash, 5);                // the two replaced copies, then the three parts
        const Run un2 = runScript(pkg / "install.sh", { "--uninstall" }, env, work, log);
        P.eq("uninstall.again_exit", un2.code, 0);
        yes(P, "uninstall.again_nothing", un2.out.find("nothing installed by this script found") != std::string::npos, un2.out);
        const Run help = runScript(pkg / "install.sh", { "--help" }, env, work, log);
        P.eq("install.help", help.code == 0 && help.out.find("--uninstall") != std::string::npos ? 1 : 0, 1);
    }

    fs::remove_all(tmp, ec);
    return P.finish();
}
#else
PROBE(plugin, linux) { return P.finish(); }
#endif
