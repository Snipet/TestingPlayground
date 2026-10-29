// Tools/probes/installer/Fixture.h: what the installer's probes share (installer.engine, installer.gui): a payload of
// two made-up products in a folder of the probe's own, and a System that never touches what a probe must not.
//
//   root()        the probe's root, inside its CTest sandbox (<build>/sandbox/installer/<probe>/), emptied first; its
//                 name has spaces and both quotes, so every path the engine writes or quotes has them too
//   writePayload  "Tone" (an effect) and "ToneSynth" (an instrument), company "Funk Test": each a .component (signed ad
//                 hoc around a copy of /usr/bin/true, so the CDHash path is the one compared), a .vst3 and a .app
//                 (unsigned: compared byte for byte) and a .mcpb (a zip with its manifest.json)
//   FakeSystem    ditto is real (it copies inside the root only); the Trash is Places::trash (a folder of the root);
//                 the AU registrar, the running apps and the administrator prompt are stand-ins: the prompt runs the
//                 engine's script's text with /bin/sh -c as this user (as `do shell script` does, but inside the root,
//                 no administrator, no prompt), or answers "cancelled". A gate holds copies from a given one on, for a
//                 picture of a run half done.
//
// The probes use Places::under(root()) only: nothing reaches ~/Library, /Library or /Applications.
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

namespace fixture
{
    namespace fs = std::filesystem;
    namespace pi = playground::installer;

    inline constexpr const char* kEffect = "Tone";
    inline constexpr const char* kSynth = "ToneSynth";
    inline constexpr const char* kCompany = "Funk Test";

    // The probe's root: <sandbox>/root with spaces 'and' "quotes", emptied.
    inline fs::path root(const char* probe)
    {
        const std::string var = std::string(funkgui::config::kEnvPrefix) + "PREFS_DIR";
        const char* sb = std::getenv(var.c_str());
        fs::path base = sb != nullptr && *sb == '/' ? fs::path(sb) : fs::temp_directory_path() / (std::string("installer-") + probe);
        const fs::path r = base / "root with spaces 'and' \"quotes\"";
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
        return pi::runProgram({ "/usr/bin/codesign", "--force", "--sign", "-", bundle.string() }, nullptr, nullptr) == 0;
    }

    // A bundle: its Info.plist, its executable (a copy of /usr/bin/true when `signed_`, then signed ad hoc), a resource.
    inline void writeBundle(const fs::path& b, const std::string& version, const std::string& data, bool signed_)
    {
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
        juce::FileOutputStream out(juce::File(juce::String::fromUTF8(file.c_str())));
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
            writeBundle(folder / (std::string(name) + ".component"), version, data, true);
            writeBundle(folder / (std::string(name) + ".vst3"), version, data, false);
            writeBundle(folder / (std::string(name) + ".app"), version, data, false);
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
            return pi::dittoCopy(from, to, error);
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
        Admin runAsAdministrator(const std::string& script, const std::string& prompt, std::string& error) override
        {
            ++adminRuns;
            lastScript = script;
            lastPrompt = prompt;
            if (cancelPrompt)
                return Admin::cancelled;
            std::string err;
            const int status = pi::runProgram({ "/bin/sh", "-c", script }, nullptr, &err);
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
