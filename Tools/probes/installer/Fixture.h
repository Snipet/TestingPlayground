// Tools/probes/installer/Fixture.h: what the installer's probes share (installer.engine, installer.gui): a payload of
// two made-up products in a folder of the probe's own, and a System that never touches what a probe must not.
//
//   root()        the probe's root, inside its CTest sandbox (<build>/sandbox/installer/<probe>/), emptied first; its
//                 name has spaces and both quotes (Windows: spaces and a single quote, the one its names may hold), so
//                 every path the engine writes or quotes has them too
//   writePayload  "Tone" (an effect) and "ToneSynth" (an instrument), company "Funk Test": each a .component (signed ad
//                 hoc around a copy of /usr/bin/true, so the CDHash path is the one compared), a .vst3 and a .app
//                 (unsigned: compared byte for byte) and a .mcpb (a zip with its manifest.json). Windows: a .vst3 (its
//                 version in Contents/Resources/moduleinfo.json), the app's folder (its <Name>.exe a copy of System32's
//                 whoami.exe whose resources are a version resource of the version asked: the engine reads it as it
//                 reads a real build's) and a .mcpb
//   FakeSystem    the copy is real (ditto, robocopy: inside the root only); the Trash is Places::trash (a folder of the
//                 root); the AU registrar, the running apps and the administrator prompt are stand-ins: the prompt runs
//                 the engine's script's text with /bin/sh -c (Windows: PowerShell, -EncodedCommand) as this user (as
//                 `do shell script` does, but inside the root, no administrator, no prompt), or answers "cancelled". A
//                 gate holds copies from a given one on, for a picture of a run half done.
//   Windows'      junction() (cmd's mklink /J: no privilege needed, as a symbolic link needs), isLink(), denyWrite() and
//                 allowWrite() (icacls: a folder this user may not add to, then may again), userSid()
//
// The probes use Places::under(root()) only: nothing reaches ~/Library, /Library, /Applications, %LOCALAPPDATA% or
// Program Files.
#pragma once

#include "installer/Engine.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <funkgui/core/Config.h>

#if defined(_WIN32)
#include <cstdint>
#include <windows.h>
#include <sddl.h>
#undef near                                          // windows.h's 16-bit leftovers: the probes' P.near
#undef far
#endif

namespace fixture
{
    namespace fs = std::filesystem;
    namespace pi = playground::installer;

    inline constexpr const char* kEffect = "Tone";
    inline constexpr const char* kSynth = "ToneSynth";
    inline constexpr const char* kCompany = "Funk Test";

#if defined(_WIN32)
    inline fs::path systemExe(const wchar_t* name)
    {
        wchar_t b[MAX_PATH];
        const UINT n = GetSystemDirectoryW(b, MAX_PATH);
        return fs::path(std::wstring(b, n)) / name;
    }

    inline std::string text(const fs::path& p)      // a path as UTF-8 (the probes' code page: Utf8.manifest)
    {
        return p.string();
    }

    // This user's SID, as icacls names it ("*S-1-5-21-...").
    inline std::string userSid()
    {
        std::string out;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return out;
        DWORD n = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &n);
        std::vector<BYTE> buf(n);
        LPSTR sid = nullptr;
        if (n > 0 && GetTokenInformation(token, TokenUser, buf.data(), n, &n)
            && ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid))
        {
            out = std::string("*") + sid;
            LocalFree(sid);
        }
        CloseHandle(token);
        return out;
    }

    inline int icacls(const std::vector<std::string>& args)
    {
        std::vector<std::string> argv{ text(systemExe(L"icacls.exe")) };
        argv.insert(argv.end(), args.begin(), args.end());
        return pi::runProgram(argv, nullptr, nullptr);
    }

    // This user may no longer add a file or a folder to `dir` (a deny of its own, on that folder alone), then may again.
    inline bool denyWrite(const fs::path& dir) { return icacls({ text(dir), "/deny", userSid() + ":(WD,AD)" }) == 0; }
    inline bool allowWrite(const fs::path& dir) { return icacls({ text(dir), "/remove:d", userSid() }) == 0; }

    // This process's token in words: elevated or not, and its backup and restore privileges on or off (with them on, a
    // folder's denial does not bind what opens it for backup, as CreateDirectoryW does).
    inline std::string tokenWords()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return "its token unread";
        TOKEN_ELEVATION elevation{};
        DWORD n = 0;
        const bool elevated =
            GetTokenInformation(token, TokenElevation, &elevation, static_cast<DWORD>(sizeof elevation), &n) != FALSE
            && elevation.TokenIsElevated != 0;
        std::string out = elevated ? "an elevated administrator's process" : "a process not elevated";
        n = 0;
        GetTokenInformation(token, TokenPrivileges, nullptr, 0, &n);
        std::vector<BYTE> buf(n);
        if (n > 0 && GetTokenInformation(token, TokenPrivileges, buf.data(), n, &n) != FALSE)
        {
            const auto* privileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(buf.data());
            const LUID_AND_ATTRIBUTES* each = privileges->Privileges;
            for (DWORD i = 0; i < privileges->PrivilegeCount; ++i)
            {
                LUID luid = each[i].Luid;
                wchar_t name[64] = {};
                DWORD length = 64;
                if (LookupPrivilegeNameW(nullptr, &luid, name, &length) == FALSE)
                    continue;
                const std::wstring w(name, length);
                if (w == L"SeBackupPrivilege" || w == L"SeRestorePrivilege")
                    out += std::string(w == L"SeBackupPrivilege" ? ", backup " : ", restore ")
                         + ((each[i].Attributes & SE_PRIVILEGE_ENABLED) != 0 ? "on" : "off");
            }
        }
        CloseHandle(token);
        return out;
    }

    // Why `dir`, denied to this user (denyWrite), takes a new folder here all the same; "" when it refuses it, as for an
    // installer a user runs. A new folder is what the engine makes there first (its staging folder, CreateDirectoryW).
    // Where it is not refused (a CI runner's administrator), the rows that need the refusal are skipped, saying so.
    inline std::string denialPassed(const fs::path& dir)
    {
        const fs::path p = dir / ".probe-denied";
        if (CreateDirectoryW(p.c_str(), nullptr) == FALSE)
            return {};
        RemoveDirectoryW(p.c_str());
        return "a folder denied to this user takes a new folder all the same here (" + tokenWords() + ")";
    }

    // A junction at `link` to `target` (which need not be there).
    inline bool junction(const fs::path& link, const fs::path& target)
    {
        return pi::runProgram({ text(systemExe(L"cmd.exe")), "/c", "mklink", "/J", text(link), text(target) }, nullptr, nullptr)
               == 0;
    }

    inline bool isLink(const fs::path& p)
    {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    // A version resource (VS_VERSIONINFO) whose ProductVersion and FileVersion are `version`, in US English.
    inline std::vector<unsigned char> versionResource(const std::string& version)
    {
        struct Node
        {
            std::vector<unsigned char> b;
            void u16(unsigned v)
            {
                b.push_back(static_cast<unsigned char>(v & 0xFF));
                b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
            }
            void u32(uint32_t v)
            {
                u16(v & 0xFFFF);
                u16(v >> 16);
            }
            void text(const std::string& s)
            {
                for (const char c : s)
                    u16(static_cast<unsigned char>(c));
                u16(0);
            }
            void pad()
            {
                while (b.size() % 4 != 0)
                    b.push_back(0);
            }
        };
        // wLength, wValueLength, wType, the key, its padding, the value, then each child at a 32-bit boundary.
        const auto node = [](const std::string& key, unsigned type, unsigned valueLength, const std::vector<unsigned char>& value,
                             const std::vector<std::vector<unsigned char>>& children) {
            Node n;
            n.u16(0);
            n.u16(valueLength);
            n.u16(type);
            n.text(key);
            n.pad();
            n.b.insert(n.b.end(), value.begin(), value.end());
            for (const auto& c : children)
            {
                n.pad();
                n.b.insert(n.b.end(), c.begin(), c.end());
            }
            const std::size_t len = n.b.size();
            n.b[0] = static_cast<unsigned char>(len & 0xFF);
            n.b[1] = static_cast<unsigned char>((len >> 8) & 0xFF);
            return n.b;
        };
        const auto string = [&](const std::string& key, const std::string& value) {
            Node v;
            v.text(value);
            return node(key, 1, static_cast<unsigned>(value.size() + 1), v.b, {});
        };
        unsigned parts[4] = { 0, 0, 0, 0 };
        std::size_t at = 0;
        for (int i = 0; i < 4 && at <= version.size(); ++i)
        {
            const std::size_t dot = version.find('.', at);
            parts[i] = static_cast<unsigned>(std::strtoul(version.substr(at, dot - at).c_str(), nullptr, 10));
            if (dot == std::string::npos)
                break;
            at = dot + 1;
        }
        Node fixed;
        fixed.u32(0xFEEF04BDu);                      // VS_FIXEDFILEINFO: the signature, the struct's version
        fixed.u32(0x00010000u);
        for (int k = 0; k < 2; ++k)                  // the file's version, then the product's
        {
            fixed.u32((parts[0] << 16) | parts[1]);
            fixed.u32((parts[2] << 16) | parts[3]);
        }
        fixed.u32(0x3Fu);                            // the flags' mask, the flags
        fixed.u32(0u);
        fixed.u32(0x00040004u);                      // VOS_NT_WINDOWS32
        fixed.u32(1u);                               // VFT_APP
        fixed.u32(0u);
        fixed.u32(0u);
        fixed.u32(0u);
        const auto table = node("040904b0", 1, 0, {}, { string("FileVersion", version), string("ProductVersion", version) });
        const auto strings = node("StringFileInfo", 1, 0, {}, { table });
        Node translation;
        translation.u16(0x0409);
        translation.u16(0x04b0);
        const auto var = node("Translation", 0, 4, translation.b, {});
        const auto vars = node("VarFileInfo", 1, 0, {}, { var });
        return node("VS_VERSION_INFO", 0, static_cast<unsigned>(fixed.b.size()), fixed.b, { strings, vars });
    }

    // An executable whose version resource says `version`: a copy of System32's whoami.exe (never run), its resources
    // all replaced by that one.
    inline bool writeExe(const fs::path& exe, const std::string& version)
    {
        std::error_code ec;
        fs::create_directories(exe.parent_path(), ec);
        fs::copy_file(systemExe(L"whoami.exe"), exe, fs::copy_options::overwrite_existing, ec);
        if (ec)
            return false;
        SetFileAttributesW(exe.c_str(), FILE_ATTRIBUTE_NORMAL);
        const HANDLE h = BeginUpdateResourceW(exe.c_str(), TRUE);
        if (h == nullptr)
            return false;
        std::vector<unsigned char> res = versionResource(version);
        const bool put = UpdateResourceW(h, MAKEINTRESOURCEW(16), MAKEINTRESOURCEW(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                                         res.data(), static_cast<DWORD>(res.size())) != FALSE;
        return EndUpdateResourceW(h, put ? FALSE : TRUE) != FALSE && put;
    }
#endif

    // The probe's root: <sandbox>/root with spaces 'and' "quotes" (Windows: 'and' quotes), emptied.
    inline fs::path root(const char* probe)
    {
        const std::string var = std::string(funkgui::config::kEnvPrefix) + "PREFS_DIR";
        const char* sb = std::getenv(var.c_str());
        fs::path base = sb != nullptr && fs::path(sb).is_absolute() ? fs::path(sb)
                                                                   : fs::temp_directory_path() / (std::string("installer-") + probe);
#if defined(_WIN32)
        const fs::path r = base / "root with spaces 'and' quotes";
        if (fs::exists(r))
            icacls({ text(r), "/reset", "/T", "/C", "/Q" });   // a deny a stopped run left, taken back
#else
        const fs::path r = base / "root with spaces 'and' \"quotes\"";
#endif
        std::error_code ec;
        // Folders a probe made read-only are made writable again, so that the next run can empty its own root.
        for (fs::recursive_directory_iterator it(r, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) && !it->is_symlink(ec))
                fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        fs::permissions(r, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(r, ec);
        fs::create_directories(r, ec);
        return r;
    }

    inline void writeText(const fs::path& p, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
    }

    inline std::string readText(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    inline void setVersion(const fs::path& bundle, const std::string& version)
    {
#if defined(_WIN32)
        // A VST3: its moduleinfo.json; an app's folder: its exe's version resource.
        if (bundle.extension() == ".vst3")
            writeText(bundle / "Contents" / "Resources" / "moduleinfo.json",
                      "{ \"Name\": \"" + bundle.stem().string() + "\", \"Version\": \"" + version + "\" }\n");
        else
            writeExe(bundle / (bundle.filename().string() + ".exe"), version);
        return;
#endif
        const std::string name = bundle.stem().string();
        writeText(bundle / "Contents" / "Info.plist",
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                  "<plist version=\"1.0\"><dict>"
                  "<key>CFBundleExecutable</key><string>" + name + "</string>"
                  "<key>CFBundleIdentifier</key><string>com.funk.test." + name + bundle.extension().string().substr(1) + "</string>"
                  "<key>CFBundleShortVersionString</key><string>" + version + "</string>"
                  "<key>CFBundleVersion</key><string>" + version + "</string>"
                  "<key>CFBundlePackageType</key><string>BNDL</string>"
                  "</dict></plist>\n");
    }

    inline bool sign(const fs::path& bundle)
    {
#if defined(_WIN32)
        static_cast<void>(bundle);
        return true;                                     // nothing is signed on Windows
#else
        return pi::runProgram({ "/usr/bin/codesign", "--force", "--sign", "-", bundle.string() }, nullptr, nullptr) == 0;
#endif
    }

    // A bundle: its Info.plist, its executable (a copy of /usr/bin/true when `signed_`, then signed ad hoc), a resource.
    // Windows: a VST3 (moduleinfo.json, its binary) or an app's folder (its exe with the version), and the resource,
    // Contents/Resources/data.txt in both.
    inline void writeBundle(const fs::path& b, const std::string& version, const std::string& data, bool signed_)
    {
#if defined(_WIN32)
        static_cast<void>(signed_);
        if (b.extension() == ".vst3")
            writeText(b / "Contents" / "x86_64-win" / (b.stem().string() + ".vst3"), "a module: " + b.stem().string() + "\n");
        setVersion(b, version);
        writeText(b / "Contents" / "Resources" / "data.txt", data);
        return;
#endif
        const std::string name = b.stem().string();
        setVersion(b, version);
        const fs::path exe = b / "Contents" / "MacOS" / name;
        if (signed_)
        {
            std::error_code ec;
            fs::create_directories(exe.parent_path(), ec);
            fs::copy_file("/usr/bin/true", exe, fs::copy_options::overwrite_existing, ec);
        }
        else
            writeText(exe, "#!/bin/sh\n# " + name + "\n");
        writeText(b / "Contents" / "Resources" / "data.txt", data);
        if (signed_)
            sign(b);
    }

    inline void writeMcpb(const fs::path& file, const std::string& version, const std::string& data)
    {
        juce::ZipFile::Builder zip;
        const juce::String manifest = "{ \"manifest_version\": \"0.3\", \"name\": \"" + juce::String(file.stem().string())
                                      + "\", \"version\": \"" + juce::String(version) + "\" }\n";
        zip.addEntry(new juce::MemoryInputStream(manifest.toRawUTF8(), manifest.getNumBytesAsUTF8(), true), 9, "manifest.json",
                     juce::Time(2026, 0, 1, 0, 0));
        zip.addEntry(new juce::MemoryInputStream(data.data(), data.size(), true), 9, "server/data", juce::Time(2026, 0, 1, 0, 0));
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        fs::remove(file, ec);
#if defined(_WIN32)
        juce::FileOutputStream out(juce::File(juce::String(file.wstring().c_str())));
#else
        juce::FileOutputStream out(juce::File(juce::String::fromUTF8(file.c_str())));
#endif
        zip.writeToStream(out, nullptr);
    }

    // The two products' payload; `data` tells builds apart (the same version, other contents).
    inline void writePayload(const fs::path& folder, const std::string& version = "1.2.0", const std::string& ext = "0.3.1",
                             const std::string& data = "build 1")
    {
        std::error_code ec;
        fs::create_directories(folder, ec);
        writeText(folder / "payload.json",
                  std::string("{ \"format\": \"funk-playground-payload\", \"version\": 1, \"project\": \"Tone\", \"products\": [\n")
                      + "  { \"key\": \"effect\", \"name\": \"" + kEffect + "\", \"kind\": \"effect\", \"company\": \"" + kCompany
                      + "\", \"bundleId\": \"com.funk.test.tone\", \"version\": \"" + version + "\" },\n"
                      + "  { \"key\": \"synth\", \"name\": \"" + kSynth + "\", \"kind\": \"instrument\", \"company\": \""
                      + kCompany + "\", \"bundleId\": \"com.funk.test.tonesynth\", \"version\": \"" + version + "\" } ] }\n");
        for (const char* name : { kEffect, kSynth })
        {
#if !defined(_WIN32)
            writeBundle(folder / (std::string(name) + ".component"), version, data, true);
#endif
            writeBundle(folder / (std::string(name) + ".vst3"), version, data, false);
            writeBundle(folder / (std::string(name) + pi::partExtension(pi::Part::app)), version, data, false);
            writeMcpb(folder / (std::string(name) + ".mcpb"), ext, data);
        }
    }

    // Every path under `dir` (relative) whose name starts with `prefix`.
    inline std::vector<std::string> named(const fs::path& dir, const std::string& prefix)
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().filename().string().rfind(prefix, 0) == 0)
                out.push_back(fs::relative(it->path(), dir, ec).string());
        return out;
    }

    class FakeSystem final : public pi::System
    {
    public:
        std::vector<fs::path> apps;                  // what runningApps() answers
        int registrarRestarts = 0;
        int adminRuns = 0;
        std::string lastScript, lastPrompt;
        bool cancelPrompt = false;                   // the user cancels the administrator prompt
        std::atomic<int> copiesStarted{ 0 };

        bool copy(const fs::path& from, const fs::path& to, std::string& error) override
        {
            const int n = copiesStarted.fetch_add(1) + 1;
            {
                std::unique_lock<std::mutex> lock(gateMutex_);
                gateCv_.wait(lock, [&] { return gateAt_ <= 0 || n < gateAt_; });
            }
            return pi::copyItem(from, to, error);
        }
        std::vector<fs::path> runningApps() override { return apps; }
        bool restartAuRegistrar() override
        {
            ++registrarRestarts;
            return true;
        }
        bool moveToTrash(const fs::path& path, std::string& error) override
        {
            error = "a probe never uses the user's Trash (" + path.string() + ")";
            return false;
        }
        Admin runAsAdministrator(const std::string& script, const std::string& prompt, std::string& error,
                                 int& status) override
        {
            ++adminRuns;
            lastScript = script;
            lastPrompt = prompt;
            status = -1;
            if (cancelPrompt)
                return Admin::cancelled;
            std::string err;
#if defined(_WIN32)
            status = pi::runProgram({ pi::powershellExe().string(), "-NoLogo", "-NoProfile", "-NonInteractive",
                                      "-ExecutionPolicy", "Bypass", "-EncodedCommand", pi::psEncoded(script) },
                                    nullptr, &err);
#else
            status = pi::runProgram({ "/bin/sh", "-c", script }, nullptr, &err);
#endif
            if (status == 0)
                return Admin::ran;
            error = err;
            return Admin::failed;
        }

        // Copies from the `at`-th (1-based) on wait until release(); 0: none waits.
        void holdFrom(int at)
        {
            const std::lock_guard<std::mutex> lock(gateMutex_);
            gateAt_ = at;
        }
        void release()
        {
            {
                const std::lock_guard<std::mutex> lock(gateMutex_);
                gateAt_ = 0;
            }
            gateCv_.notify_all();
        }

    private:
        std::mutex gateMutex_;
        std::condition_variable gateCv_;
        int gateAt_ = 0;
    };
}
