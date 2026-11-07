// Source/fminus/Shaders.cpp: see Shaders.h.
#include "fminus/Shaders.h"
#include "fminus/PanelSpec.h"
#include "fminus/ShaderLib.h"

#include "Product.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <random>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace playground
{
    namespace
    {
        constexpr int kTimeoutMs = 10000;            // shaderc on a shader the plugin's size takes about 60 ms

        std::atomic<uint64_t> shadercStarts{ 0 };    // shadercRuns()

        // shaderc's runs at once in the process: kShadercAtOnce, or half the cores when that is fewer (one at least).
        class ShadercGate
        {
        public:
            ShadercGate() : free_(std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, kShadercAtOnce)) {}

            void enter()
            {
                std::unique_lock<std::mutex> lock(mutex_);
                freed_.wait(lock, [this] { return free_ > 0; });
                --free_;
            }

            void leave()
            {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    ++free_;
                }
                freed_.notify_one();
            }

        private:
            std::mutex mutex_;
            std::condition_variable freed_;
            int free_;
        };

        ShadercGate& shadercGate()
        {
            static ShadercGate gate;
            return gate;
        }

        // A run of shaderc through the gate, counted.
        struct ShadercTurn
        {
            ShadercTurn()
            {
                shadercGate().enter();
                shadercStarts.fetch_add(1, std::memory_order_relaxed);
            }
            ~ShadercTurn() { shadercGate().leave(); }
            ShadercTurn(const ShadercTurn&) = delete;
            ShadercTurn& operator=(const ShadercTurn&) = delete;
        };

        // ---- the disk cache (Shaders.h, "The cache") -------------------------------------------------------------------
        // An entry's file, <the key's hash in hex>.shader: kMagic, three 64-bit numbers (little-endian: the key's bytes,
        // the binary's, and the FNV-1a hash of the key and the binary in turn), the key, the binary. A writer writes
        // <hash>.<its own tag>.tmp beside it and renames it over the entry.
        constexpr char kMagic[] = "FPSHADER1\n";
        constexpr std::size_t kMagicBytes = sizeof kMagic - 1;
        constexpr std::size_t kHeadBytes = kMagicBytes + 3 * 8;
        constexpr auto kStaleTemp = std::chrono::minutes(10);   // a .tmp this old was left by a writer that stopped

        uint64_t fnv64(std::string_view bytes, uint64_t h = 14695981039346656037ull)
        {
            for (const char c : bytes)
                h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
            return h;
        }

        std::string hex64(uint64_t v)
        {
            char out[17];
            std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(v));
            return out;
        }

        void putU64(std::string& out, uint64_t v)
        {
            for (int k = 0; k < 8; ++k)
                out += static_cast<char>((v >> (8 * k)) & 0xffu);
        }

        uint64_t getU64(const std::string& in, std::size_t at)
        {
            uint64_t v = 0;
            for (int k = 0; k < 8; ++k)
                v |= static_cast<uint64_t>(static_cast<unsigned char>(in[at + static_cast<std::size_t>(k)])) << (8 * k);
            return v;
        }

        // shaderc as a key's line: its path, size and modification time (a rebuilt or replaced shaderc is another).
        std::string shadercIdentity(const std::string& shaderc)
        {
            std::error_code ec;
            const std::uintmax_t size = std::filesystem::file_size(shaderc, ec);
            if (ec)
                return shaderc + " ?";
            const auto time = std::filesystem::last_write_time(shaderc, ec);
            if (ec)
                return shaderc + " ?";
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
            return shaderc + ' ' + std::to_string(size) + ' ' + std::to_string(static_cast<long long>(ns));
        }

        // Everything a build's bytes come from (Shaders.h, "The cache"): `body` as shaderc reads it, and each file it
        // includes, its name and its text as written beside it.
        std::string keyOf(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                          const ShaderTools& tools, const std::vector<std::pair<std::string, std::string>>& included)
        {
            std::string key = "playground shader 2\n";
            key += tools.platform + ' ' + tools.profile + '\n';
            key += shadercIdentity(tools.shaderc) + '\n';
            key += hex64(fnv64(tools.varyingDef)) + ' ' + hex64(fnv64(tools.bgfxShaderSh)) + '\n';
            key += file + '\n';
            key += shaderSource(body, inputs, tools);
            for (const auto& [name, text] : included)
                key += "#include " + name + ' ' + std::to_string(text.size()) + '\n' + text;
            return key;
        }

        std::string fileText(const std::filesystem::path& p)
        {
            std::ifstream f(p, std::ios::binary);
            return f ? std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()) : std::string();
        }

        // An entry's binary, when the file reads back whole and is `key`'s; else nothing.
        std::optional<std::vector<uint8_t>> readEntry(const std::filesystem::path& entry, const std::string& key)
        {
            const std::string data = fileText(entry);
            if (data.size() < kHeadBytes || data.compare(0, kMagicBytes, kMagic) != 0)
                return std::nullopt;
            const uint64_t keyBytes = getU64(data, kMagicBytes), binaryBytes = getU64(data, kMagicBytes + 8);
            if (keyBytes != key.size() || data.size() - kHeadBytes <= key.size()
                || binaryBytes != data.size() - kHeadBytes - key.size() || data.compare(kHeadBytes, key.size(), key) != 0)
                return std::nullopt;
            const std::string_view binary(data.data() + kHeadBytes + key.size(), data.size() - kHeadBytes - key.size());
            if (fnv64(binary, fnv64(key)) != getU64(data, kMagicBytes + 16))
                return std::nullopt;
            return std::vector<uint8_t>(binary.begin(), binary.end());
        }

        // A name for a writer's temporary file no other writer (thread or process) takes at the same time.
        std::string writerTag()
        {
            static std::atomic<uint64_t> writes{ 0 };
#if defined(_WIN32)
            const uint64_t process = GetCurrentProcessId();
#else
            const uint64_t process = static_cast<uint64_t>(getpid());
#endif
            const uint64_t thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
            const uint64_t now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            return hex64(process) + '-' + hex64(thread ^ now) + '-' + std::to_string(writes.fetch_add(1));
        }

        // The cache within its cap: the entries used least lately removed (never `keep`, the one just written) until it
        // holds three quarters of it; a writer's temporary file left long ago removed. The entries' bytes left.
        std::uintmax_t prune(const std::filesystem::path& dir, std::uintmax_t cap, const std::filesystem::path& keep)
        {
            struct Entry
            {
                std::filesystem::file_time_type used;
                std::uintmax_t bytes;
                std::filesystem::path path;
            };
            std::vector<Entry> entries;
            std::uintmax_t total = 0;
            const auto now = std::filesystem::file_time_type::clock::now();
            std::error_code ec;
            for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
                 it.increment(ec))
            {
                std::error_code e;
                const std::filesystem::path& p = it->path();
                const auto used = it->last_write_time(e);
                if (e)
                    continue;
                if (p.extension() == ".tmp")
                {
                    if (now - used > kStaleTemp)
                        std::filesystem::remove(p, e);
                    continue;
                }
                if (p.extension() != ".shader")
                    continue;
                const std::uintmax_t bytes = it->file_size(e);
                if (e)
                    continue;
                entries.push_back({ used, bytes, p });
                total += bytes;
            }
            if (total <= cap)
                return total;
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
            for (const Entry& x : entries)
            {
                if (total <= cap / 4 * 3)
                    break;
                std::error_code e;
                if (x.path != keep && std::filesystem::remove(x.path, e))
                    total -= x.bytes;
            }
            return total;
        }

        // What the process knows of each cache folder it writes: its entries' bytes as the last scan (prune) found
        // them, with what it has written there since, and its writes since that scan. A write scans the folder only
        // when it is the process's first there, when it takes the total over the cap, or when it is the
        // kRescanWrites-th since the last scan (which finds what other processes wrote, and their temporary files left
        // long ago): a folder at its cap holds thousands of entries, and a scan of them all takes tens of milliseconds,
        // which a RUN would wait for.
        constexpr int kRescanWrites = 64;
        std::atomic<uint64_t> folderScans{ 0 };      // shaderCacheScans()

        struct Folders
        {
            struct Tally
            {
                std::uintmax_t bytes = 0;
                int writes = 0;
                bool scanned = false;
            };
            std::mutex mutex;
            std::map<std::filesystem::path, Tally> tallies;
        };

        Folders& folders()
        {
            static Folders f;
            return f;
        }

        // A write of `added` bytes to `dir` (over an entry of `replaced` bytes, 0 for none) counted, and the folder
        // scanned and brought within `cap` when it is due (above).
        void tally(const std::filesystem::path& dir, std::uintmax_t cap, const std::filesystem::path& entry,
                   std::uintmax_t added, std::uintmax_t replaced)
        {
            Folders& f = folders();
            const std::lock_guard<std::mutex> lock(f.mutex);
            Folders::Tally& t = f.tallies[dir];
            t.bytes += added;
            t.bytes = t.bytes > replaced ? t.bytes - replaced : 0;
            ++t.writes;
            if (t.scanned && t.bytes <= cap && t.writes < kRescanWrites)
                return;
            t.bytes = prune(dir, cap, entry);
            t.scanned = true;
            t.writes = 0;
            folderScans.fetch_add(1, std::memory_order_relaxed);
        }

        // `binary` kept as `key`'s entry: written under a name of the writer's own, then renamed over the entry (one
        // step: a reader sees the old file or the new one whole); nothing kept when the folder cannot be written.
        void writeEntry(const std::filesystem::path& dir, const std::filesystem::path& entry, const std::string& key,
                        const std::vector<uint8_t>& binary, std::uintmax_t cap)
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec)
                return;
            std::string data(kMagic, kMagicBytes);
            putU64(data, key.size());
            putU64(data, binary.size());
            const std::string_view bytes(reinterpret_cast<const char*>(binary.data()), binary.size());
            putU64(data, fnv64(bytes, fnv64(key)));
            data += key;
            data.append(bytes.data(), bytes.size());
            const std::filesystem::path temp = dir / (entry.stem().string() + '.' + writerTag() + ".tmp");
            bool written = false;
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                out.close();
                written = static_cast<bool>(out);
            }
            std::uintmax_t replaced = 0;             // this key's entry already there (another writer's, a corrupt one)
            if (written)
            {
                std::error_code e;
                replaced = std::filesystem::file_size(entry, e);
                if (e)
                    replaced = 0;
                std::filesystem::rename(temp, entry, ec);
            }
            if (!written || ec)
            {
                std::error_code e;
                std::filesystem::remove(temp, e);
                return;
            }
            tally(dir, cap, entry, data.size(), replaced);
        }

        bool identifier(const std::string& s)
        {
            if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
                return false;
            return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        }

        // An input's name as a shader name: letters, digits and _ ("in_cutoff").
        std::string inputName(const std::string& name)
        {
            std::string out;
            for (const char c : name)
                out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            return out;
        }

        bool writeFile(const std::filesystem::path& p, const std::string& text)
        {
            std::ofstream f(p, std::ios::binary);
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            return static_cast<bool>(f);
        }

        std::string readFile(const std::filesystem::path& p)
        {
            std::ifstream f(p, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }

        // Runs argv[0] with its output (both streams) into `log`; its exit status, or -1 when it could not run or took
        // longer than kTimeoutMs (it is killed).
#if defined(_WIN32)
        // One argument as CommandLineToArgvW reads it back: quoted, with backslashes before a quote doubled.
        std::wstring quoted(const std::wstring& a)
        {
            std::wstring out = L"\"";
            std::size_t slashes = 0;
            for (const wchar_t c : a)
            {
                if (c == L'\\')
                    ++slashes;
                else
                {
                    if (c == L'"')
                        out.append(slashes + 1, L'\\');
                    slashes = 0;
                }
                out += c;
            }
            out.append(slashes, L'\\');
            return out + L"\"";
        }

        int run(const std::vector<std::string>& args, const std::filesystem::path& log)
        {
            SECURITY_ATTRIBUTES inherit{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
            HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE)
                return -1;
            HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            std::wstring line;
            for (const std::string& a : args)
                line += (line.empty() ? L"" : L" ") + quoted(std::filesystem::path(a).wstring());
            STARTUPINFOW si{};
            si.cb = sizeof si;
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = in;
            si.hStdOutput = out;
            si.hStdError = out;
            PROCESS_INFORMATION pi{};
            const std::wstring exe = std::filesystem::path(args[0]).wstring();
            const BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                           nullptr, &si, &pi);
            CloseHandle(out);
            if (in != INVALID_HANDLE_VALUE)
                CloseHandle(in);
            if (!ok)
                return -1;
            CloseHandle(pi.hThread);
            int status = -1;
            if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(kTimeoutMs)) == WAIT_OBJECT_0)
            {
                DWORD code = 0;
                if (GetExitCodeProcess(pi.hProcess, &code))
                    status = static_cast<int>(code);
            }
            else
            {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, INFINITE);
            }
            CloseHandle(pi.hProcess);
            return status;
        }

        // A new private folder beside the others in `tmp` (mkdtemp's), its path; "" when none could be made.
        std::string madeTempFolder(const std::filesystem::path& tmp)
        {
            std::random_device rd;
            for (int attempt = 0; attempt < 32; ++attempt)
            {
                char name[32];
                std::snprintf(name, sizeof name, "tp-shader-%08x", static_cast<unsigned>(rd()));
                std::error_code ec;
                if (std::filesystem::create_directory(tmp / name, ec) && !ec)
                    return (tmp / name).string();
            }
            return {};
        }
#else
        int run(const std::vector<std::string>& args, const std::filesystem::path& log)
        {
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            posix_spawn_file_actions_adddup2(&actions, 1, 2);
            posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
            std::vector<char*> argv;
            for (const std::string& a : args)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            pid_t pid = 0;
            const int spawned = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            if (spawned != 0)
                return -1;
            const auto start = std::chrono::steady_clock::now();
            for (;;)
            {
                int status = 0;
                const pid_t done = waitpid(pid, &status, WNOHANG);
                if (done == pid)
                    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                if (done < 0 && errno != EINTR)
                    return -1;
                if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(kTimeoutMs))
                {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    return -1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
#endif

        // A file shaderc reads, as a note cites it: its name and its text (shaderText's).
        struct Cited
        {
            std::string name;
            std::string text;
        };

        // shaderc's log as a note: its first ERROR's message, on the line of the file it is about. shaderc reports
        // lines of its own rewritten listing (bgfx's HLSL), so the line is found in the files instead (the shader's,
        // then the program's .sh files it includes, `cited` in that order): the first one with the name the error
        // names, else the one whose text the listing's marked line has.
        std::string noteOf(const std::vector<Cited>& cited, const std::string& log)
        {
            std::istringstream in(log);
            std::string line, message, marked;
            while (std::getline(in, line))
            {
                if (marked.empty() && line.rfind(">>> ", 0) == 0)
                {
                    const std::size_t colon = line.find(": ");
                    if (colon != std::string::npos && std::atoi(line.c_str() + 4) > 0)
                        marked = line.substr(colon + 2);
                }
                if (message.empty() && line.rfind("ERROR: ", 0) == 0)
                {
                    // ERROR: 0:422: 'undefinedThing' : unknown variable
                    std::size_t p = line.find(':', 7);
                    p = p == std::string::npos ? std::string::npos : line.find(':', p + 1);
                    message = p == std::string::npos ? line.substr(7) : line.substr(p + 1);
                    while (!message.empty() && message.front() == ' ')
                        message.erase(message.begin());
                    while (!message.empty() && (message.back() == ' ' || message.back() == '\r'))
                        message.pop_back();
                }
            }
            if (message.empty())
            {
                // Not glslang's: shaderc's own line (a missing $input, a file it cannot read), or nothing.
                std::istringstream again(log);
                while (std::getline(again, line))
                    if (!line.empty() && line.find("Failed to build shader") == std::string::npos && line != "Code:"
                        && line != "---")
                        message = line;
                if (message.empty())
                    message = "shaderc could not run";
            }
            // The name in quotes, when the error names one.
            std::string name;
            if (const std::size_t q = message.find('\''); q != std::string::npos)
                if (const std::size_t e = message.find('\'', q + 1); e != std::string::npos)
                    name = message.substr(q + 1, e - q - 1);
            std::vector<std::vector<std::string>> lines;
            for (const Cited& c : cited)
            {
                std::vector<std::string>& these = lines.emplace_back();
                std::istringstream b(c.text);
                while (std::getline(b, line))
                    these.push_back(line);
            }
            const auto word = [](const std::string& s, const std::string& w) {
                for (std::size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1))
                {
                    const bool before = p == 0 || !(std::isalnum(static_cast<unsigned char>(s[p - 1])) || s[p - 1] == '_');
                    const std::size_t end = p + w.size();
                    const bool after = end >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_');
                    if (before && after)
                        return true;
                }
                return false;
            };
            std::size_t which = 0;                   // the file cited
            int at = 0;
            if (identifier(name))
                for (std::size_t f = 0; f < lines.size() && at == 0; ++f)
                    for (std::size_t k = 0; k < lines[f].size() && at == 0; ++k)
                        if (word(lines[f][k], name))
                        {
                            which = f;
                            at = static_cast<int>(k) + 1;
                        }
            if (at == 0 && !marked.empty())
            {
                // The listing's line in bgfx's words, back in the shader's: the longest of the files' lines it holds.
                std::string m = marked;
                const auto swap = [&m](const std::string& from, const std::string& to) {
                    for (std::size_t p = m.find(from); p != std::string::npos; p = m.find(from, p + to.size()))
                        m.replace(p, from.size(), to);
                };
                swap("bgfx_FragData0", "gl_FragColor");
                swap("float4", "vec4");
                swap("float3", "vec3");
                swap("float2", "vec2");
                std::size_t best = 0;
                for (std::size_t f = 0; f < lines.size(); ++f)
                    for (std::size_t k = 0; k < lines[f].size(); ++k)
                    {
                        std::string t = lines[f][k];
                        t.erase(0, t.find_first_not_of(" \t"));
                        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r'))
                            t.pop_back();
                        if (t.size() > 3 && t.size() > best && m.find(t) != std::string::npos)
                        {
                            best = t.size();
                            which = f;
                            at = static_cast<int>(k) + 1;
                        }
                    }
            }
            return (cited.empty() ? std::string("shader") : cited[which].name) + ":" + std::to_string(at > 0 ? at : 1)
                 + ":1: note: the shader does not compile: " + message + " (it draws nothing until it does)\n";
        }

        // ---- text and includes (Shaders.h) ---------------------------------------------------------------------------
        // A note of the pre-pass's: the shader does not compile, for `why`, at a line and column of `file`.
        std::string prepassNote(const std::string& file, int line, int column, const std::string& why)
        {
            return file + ":" + std::to_string(line) + ":" + std::to_string(column) + ": note: the shader does not compile: "
                 + why + " (it draws nothing until it does)\n";
        }

        // Walks a shader's text as shaderc's preprocessor reads it: its comments (// to the line's end, a backslash at
        // its end going on; /* */), its #include lines and the rest. `include` hears each #include's file name and
        // place; `string` each string literal's place and inner text (between its quotes, as written), and returns
        // what replaces the literal. The text, each literal replaced; a literal with no end on its line goes to
        // `string` with `ended` false.
        template <typename Include, typename String>
        std::string walk(const std::string& text, Include&& include, String&& string)
        {
            std::string out;
            out.reserve(text.size());
            const std::size_t n = text.size();
            std::size_t i = 0, lineStart = 0;
            int line = 1;
            bool blank = true;                       // only spaces since the line's start (a directive may begin)
            const auto newline = [&](std::size_t at) {
                ++line;
                lineStart = at + 1;
                blank = true;
            };
            while (i < n)
            {
                const char c = text[i];
                if (c == '\n')
                {
                    out += c;
                    newline(i);
                    ++i;
                    continue;
                }
                if (c == '/' && i + 1 < n && text[i + 1] == '/')
                {
                    for (; i < n && text[i] != '\n'; ++i)
                    {
                        out += text[i];
                        if (text[i] == '\\' && i + 1 < n && text[i + 1] == '\n')
                        {
                            out += '\n';
                            newline(i + 1);
                            ++i;
                        }
                    }
                    continue;
                }
                if (c == '/' && i + 1 < n && text[i + 1] == '*')
                {
                    const std::size_t end = text.find("*/", i + 2);
                    const std::size_t stop = end == std::string::npos ? n : end + 2;
                    for (; i < stop; ++i)
                    {
                        out += text[i];
                        if (text[i] == '\n')
                            newline(i);
                    }
                    blank = false;
                    continue;
                }
                if (c == '#' && blank)
                {
                    // A directive: an #include's name heard and its line copied as it is.
                    std::size_t k = i + 1;
                    while (k < n && (text[k] == ' ' || text[k] == '\t'))
                        ++k;
                    if (text.compare(k, 7, "include") == 0 && (k + 7 >= n || !std::isalnum(static_cast<unsigned char>(text[k + 7]))))
                    {
                        std::size_t e = text.find('\n', k);
                        if (e == std::string::npos)
                            e = n;
                        std::size_t q = k + 7;
                        while (q < e && (text[q] == ' ' || text[q] == '\t'))
                            ++q;
                        if (q < e && (text[q] == '"' || text[q] == '<'))
                        {
                            const char close = text[q] == '"' ? '"' : '>';
                            const std::size_t z = text.find(close, q + 1);
                            if (z != std::string::npos && z < e)
                                include(text.substr(q + 1, z - q - 1), line, static_cast<int>(q - lineStart) + 1);
                        }
                        out.append(text, i, e - i);
                        i = e;
                        continue;
                    }
                }
                if (c == '"')
                {
                    std::size_t k = i + 1;
                    while (k < n && text[k] != '"' && text[k] != '\n')
                        k += text[k] == '\\' && k + 1 < n && text[k + 1] != '\n' ? 2 : 1;
                    const bool ended = k < n && text[k] == '"';
                    out += string(std::string_view(text).substr(i + 1, k - i - 1), line, static_cast<int>(i - lineStart) + 1, ended);
                    i = ended ? k + 1 : k;
                    blank = false;
                    continue;
                }
                if (c != ' ' && c != '\t' && c != '\r')
                    blank = false;
                out += c;
                ++i;
            }
            return out;
        }

        // A string literal's glyph codes (ShaderLib.h), from its inner text; false, with why, when one cannot be.
        bool codesOf(std::string_view s, std::vector<int>& codes, std::string& why)
        {
            for (std::size_t i = 0; i < s.size();)
            {
                uint32_t cp = static_cast<unsigned char>(s[i]);
                std::size_t len = 1;
                if (cp == '\\')
                {
                    const char e = i + 1 < s.size() ? s[i + 1] : '\0';
                    if (e == '\\' || e == '"')
                    {
                        cp = static_cast<uint32_t>(e);
                        len = 2;
                    }
                    else if (e == 'u' && i + 6 <= s.size()
                             && std::all_of(s.begin() + static_cast<std::ptrdiff_t>(i + 2), s.begin() + static_cast<std::ptrdiff_t>(i + 6),
                                            [](char h) { return std::isxdigit(static_cast<unsigned char>(h)) != 0; }))
                    {
                        cp = static_cast<uint32_t>(std::strtoul(std::string(s.substr(i + 2, 4)).c_str(), nullptr, 16));
                        len = 6;
                    }
                    else
                    {
                        why = "a string's backslash escapes a backslash, a quote or a code point (a u and four hex digits), "
                              "not this";
                        return false;
                    }
                }
                else if (cp >= 0x80)
                {
                    // UTF-8
                    const int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : -1;
                    if (more < 0 || i + static_cast<std::size_t>(more) >= s.size())
                    {
                        why = "a string holds a byte that is no character";
                        return false;
                    }
                    cp &= more == 3 ? 0x07u : more == 2 ? 0x0Fu : 0x1Fu;
                    for (int k = 1; k <= more; ++k)
                    {
                        const auto b = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
                        if ((b & 0xC0u) != 0x80u)
                        {
                            why = "a string holds a byte that is no character";
                            return false;
                        }
                        cp = (cp << 6) | (b & 0x3Fu);
                    }
                    len = static_cast<std::size_t>(more) + 1;
                }
                const int code = shaderlib::glyphCode(cp);
                if (code < 0)
                {
                    char hex[16];
                    std::snprintf(hex, sizeof hex, "U+%04X", static_cast<unsigned>(cp));
                    why = std::string("the font has no ") + hex + ": a string holds printable ASCII, and these by their "
                          "codes: \\u00B0 for the degree sign; \\u00B1, \\u00D7, \\u00B5, \\u2212 (minus), \\u2026, \\u2264, \\u2265, "
                          "\\u2248, \\u0394, \\u2013, \\u2014, \\u2022, \\u00B7, \\u2190 to \\u2193 (arrows), \\u221E";
                    return false;
                }
                codes.push_back(code);
                i += len;
            }
            if (codes.size() > static_cast<std::size_t>(shaderlib::kMaxString))
            {
                why = "a string holds " + std::to_string(shaderlib::kMaxString) + " characters at most (this one "
                    + std::to_string(codes.size()) + ")";
                return false;
            }
            return true;
        }

        // The codes as a vec4 (up to 12) or a mat4 (up to 48), three a float, the first the lowest byte.
        std::string packed(const std::vector<int>& codes)
        {
            const std::size_t floats = codes.size() <= 12 ? 4 : 16;
            std::vector<uint32_t> v(floats, 0u);
            for (std::size_t k = 0; k < codes.size(); ++k)
                v[k / 3] |= static_cast<uint32_t>(codes[k]) << (8 * (k % 3));
            std::string out = floats == 4 ? "vec4(" : "mat4(";
            for (std::size_t k = 0; k < floats; ++k)
                out += (k == 0 ? "" : ", ") + std::to_string(v[k]) + ".0";
            return out + ")";
        }

        // A file's include guard's name, from its name.
        std::string guardOf(const std::string& name)
        {
            return "FP_INCLUDED_" + hex64(fnv64(name));
        }

        // A shader made ready for shaderc: its text as shaderc reads it, the files it includes as written beside it,
        // and what a note cites (the shader, then the program's .sh files it includes).
        struct Prepared
        {
            std::string body;
            std::vector<std::pair<std::string, std::string>> files;   // each included file's name and text, as written
            std::vector<Cited> cited;
            std::string error;
        };

        Prepared prepare(const std::string& file, const std::string& body, const Sources& headers)
        {
            Prepared out;
            out.body = shaderText(file, body, out.error);
            out.cited.push_back({ file, out.body });
            if (!out.error.empty())
                return out;
            for (const std::string& name : shaderIncludes(file, body, headers, out.error))
            {
                if (const shaderlib::LibraryFile* lib = shaderlib::libraryFile(name))
                {
                    out.files.emplace_back(name, lib->text);
                    continue;
                }
                const SourceFile* h = findFile(headers, name);
                if (h == nullptr)
                    continue;
                std::string error;
                const std::string text = shaderText(name, h->text, error);
                if (!error.empty())
                {
                    out.error = error;
                    return out;
                }
                const std::string guard = guardOf(name);
                out.files.emplace_back(name, "#ifndef " + guard + "\n#define " + guard + "\n" + text
                                                 + (text.empty() || text.back() == '\n' ? "" : "\n") + "#endif\n");
                out.cited.push_back({ name, text });
            }
            return out;
        }
    }

    std::string shaderSource(const std::string& body, const std::vector<std::string>& inputs, const ShaderTools& tools,
                             int* bodyLine)
    {
        std::string s = tools.prelude;
        if (!s.empty() && s.back() != '\n')
            s += '\n';
        s += "// The plugin's names (Source/fminus/Shaders.h).\n"
             "#define u_time (u_viewSize.w)\n"
             "#define u_size (u_rect.zw)\n"
             "#define u_px (u_viewSize.z)\n";
        static constexpr const char* kSwizzle[4] = { "x", "y", "z", "w" };
        for (int k = 0; k < kMaxShaderInputs; ++k)
            s += "#define in" + std::to_string(k) + " (u_params[" + std::to_string(k / 4) + "]." + kSwizzle[k % 4] + ")\n";
        for (std::size_t k = 0; k < inputs.size() && k < static_cast<std::size_t>(kMaxShaderInputs); ++k)
            if (!inputs[k].empty())
                s += "#define in_" + inputName(inputs[k]) + " in" + std::to_string(k) + "\n";
        if (bodyLine != nullptr)
            *bodyLine = 1 + static_cast<int>(std::count(s.begin(), s.end(), '\n'));
        s += body;
        if (s.empty() || s.back() != '\n')
            s += '\n';
        return s;
    }

    namespace
    {
        // buildShader(); `kept` false when the build failed for a reason of the moment (no temporary folder, its files
        // not written, shaderc not run or timed out), which a cache must not keep.
        ShaderBuild buildOnce(const std::string& file, const Prepared& ready, const std::vector<std::string>& inputs,
                              const ShaderTools& tools, bool& kept)
        {
            kept = false;
            ShaderBuild out;
            if (!ready.error.empty())
            {
                out.error = ready.error;             // the pre-pass's note (prepare)
                return out;
            }
            if (!tools.usable())
            {
                out.error = file + ":1:1: note: this plugin cannot build shaders (no shaderc beside it): the shader draws "
                                   "nothing\n";
                return out;
            }
            std::error_code ec;
            const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
#if defined(_WIN32)
            const std::string pattern = ec ? std::string() : madeTempFolder(tmp);
            if (pattern.empty())
#else
            std::string pattern = (tmp / "tp-shader-XXXXXX").string();
            if (ec || mkdtemp(pattern.data()) == nullptr)
#endif
            {
                out.error = file + ":1:1: note: the shader cannot be built: no temporary folder\n";
                return out;
            }
            const std::filesystem::path dir = pattern;
            const std::string source = shaderSource(ready.body, inputs, tools);
            bool written = writeFile(dir / "fs.sc", source) && writeFile(dir / "varying.def.sc", tools.varyingDef)
                        && (tools.bgfxShaderSh.empty() || writeFile(dir / "bgfx_shader.sh", tools.bgfxShaderSh));
            for (const auto& [name, text] : ready.files)
            {
                // Beside fs.sc, for shaderc's -i: the program's by their names, the library's under funk/.
                const std::filesystem::path to = dir / std::filesystem::path(name);
                std::error_code made;
                std::filesystem::create_directories(to.parent_path(), made);
                written = written && !made && writeFile(to, text);
            }
            if (!written)
                out.error = file + ":1:1: note: the shader cannot be built: its files cannot be written\n";
            else
            {
                int status = -1;
                {
                    const ShadercTurn turn;
                    status = run({ tools.shaderc, "-f", (dir / "fs.sc").string(), "-o", (dir / "fs.bin").string(), "--type",
                                   "fragment", "--platform", tools.platform, "-p", tools.profile, "--varyingdef",
                                   (dir / "varying.def.sc").string(), "-i", dir.string() },
                                 dir / "log.txt");
                }
                const std::string bin = status == 0 ? readFile(dir / "fs.bin") : std::string();
                if (!bin.empty())
                    out.binary.assign(bin.begin(), bin.end());
                else
                    out.error = status < 0 && readFile(dir / "log.txt").empty()
                                  ? file + ":1:1: note: the shader cannot be built: shaderc did not run\n"
                                  : noteOf(ready.cited, readFile(dir / "log.txt"));
                kept = status >= 0;                  // a time-out, or a shaderc that never ran: try again next time
            }
            std::filesystem::remove_all(dir, ec);
            return out;
        }
    }

    ShaderBuild buildShader(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                            const ShaderTools& tools, const Sources& headers)
    {
        bool kept = false;
        return buildOnce(file, prepare(file, body, headers), inputs, tools, kept);
    }

    ShaderBuild ShaderCache::build(const std::string& file, const std::string& body, const std::vector<std::string>& inputs,
                                   const ShaderTools& tools, const Sources& headers)
    {
        const Prepared ready = prepare(file, body, headers);
        if (!tools.usable() || !ready.error.empty())
        {
            bool kept = false;
            return buildOnce(file, ready, inputs, tools, kept);   // its note, at once
        }
        const std::string key = keyOf(file, ready.body, inputs, tools, ready.files);
        std::promise<ShaderBuild> promise;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (const auto it = builds_.find(key); it != builds_.end())
            {
                it->second.first = ++clock_;
                ++counts_.memory;
                return it->second.second;
            }
            if (const auto it = making_.find(key); it != making_.end())
            {
                const std::shared_future<ShaderBuild> making = it->second;
                ++counts_.waited;
                lock.unlock();
                return making.get();
            }
            making_.emplace(key, promise.get_future().share());
        }
        ShaderBuild b;
        bool kept = true, fromDisk = false;
        try
        {
            const std::filesystem::path entry =
                tools.cacheDir.empty() ? std::filesystem::path() : tools.cacheDir / (hex64(fnv64(key)) + ".shader");
            if (!entry.empty())
                if (std::optional<std::vector<uint8_t>> binary = readEntry(entry, key))
                {
                    b.binary = std::move(*binary);
                    fromDisk = true;
                    std::error_code ec;                // used now: the last the cap removes
                    std::filesystem::last_write_time(entry, std::filesystem::file_time_type::clock::now(), ec);
                }
            if (!fromDisk)
            {
                b = buildOnce(file, ready, inputs, tools, kept);
                if (b.ok() && !entry.empty())
                    writeEntry(tools.cacheDir, entry, key, b.binary, tools.cacheBytes);
            }
        }
        catch (...)
        {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                making_.erase(key);
            }
            promise.set_exception(std::current_exception());
            throw;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (kept)
            {
                if (builds_.size() >= kMemoryBuilds)
                {
                    auto oldest = builds_.begin();
                    for (auto it = builds_.begin(); it != builds_.end(); ++it)
                        if (it->second.first < oldest->second.first)
                            oldest = it;
                    builds_.erase(oldest);
                }
                builds_[key] = { ++clock_, b };
            }
            making_.erase(key);
            ++(fromDisk ? counts_.disk : counts_.built);
        }
        promise.set_value(b);
        return b;
    }

    std::vector<ShaderBuild> ShaderCache::buildAll(const std::vector<ShaderJob>& jobs, const ShaderTools& tools)
    {
        std::vector<ShaderBuild> out(jobs.size());
        const auto one = [&](std::size_t k) {
            try
            {
                ShaderBuild b = build(jobs[k].file, jobs[k].body, jobs[k].inputs, tools, jobs[k].headers);
                if (b.ok() && tools.check)
                    b.refused = !tools.check(b.binary, b.refusal);
                out[k] = std::move(b);
            }
            catch (const std::exception& e)
            {
                out[k].error = jobs[k].file + ":1:1: note: the shader cannot be built: " + e.what() + "\n";
            }
        };
        std::vector<std::thread> threads;
        for (std::size_t k = 1; k < jobs.size(); ++k)
        {
            try
            {
                threads.emplace_back(one, k);
            }
            catch (const std::system_error&)
            {
                one(k);                                // no thread to be had: here, in turn
            }
        }
        if (!jobs.empty())
            one(0);
        for (std::thread& t : threads)
            t.join();
        return out;
    }

    ShaderCache::Counts ShaderCache::counts() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return counts_;
    }

    void ShaderCache::forget()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        builds_.clear();
    }

    ShaderCache& shaderCache()
    {
        static ShaderCache cache;
        return cache;
    }

    std::filesystem::path shaderCacheFolder()
    {
        // A probe's sandbox (CTest sets <ENV_PREFIX>PREFS_DIR for each, cmake/Probes.cmake): never the user's folder.
        const std::string sandbox = std::string(product::kEnvPrefix) + "PREFS_DIR";
        if (const char* dir = std::getenv(sandbox.c_str()); dir != nullptr && *dir != '\0')
            return std::filesystem::path(dir) / "shaders";
#if defined(_WIN32)
        if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local != nullptr && *local != L'\0')
            return std::filesystem::path(local) / product::kCompany / product::kName / "Shaders";
#elif defined(__APPLE__)
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
            return std::filesystem::path(home) / "Library" / "Caches" / product::kCompany / product::kName / "Shaders";
#else
        if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0')
            return std::filesystem::path(xdg) / product::kCompany / product::kName / "Shaders";
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
            return std::filesystem::path(home) / ".cache" / product::kCompany / product::kName / "Shaders";
#endif
        return {};
    }

    uint64_t shadercRuns()
    {
        return shadercStarts.load(std::memory_order_relaxed);
    }

    uint64_t shaderCacheScans()
    {
        return folderScans.load(std::memory_order_relaxed);
    }

    uint32_t shaderId(const std::vector<uint8_t>& binary)
    {
        uint32_t h = 2166136261u;                    // FNV-1a
        for (const uint8_t b : binary)
            h = (h ^ b) * 16777619u;
        return h == 0 ? 1u : h;
    }

    bool shaderAnimates(const std::string& body)
    {
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
        const std::size_t n = body.size();
        std::size_t i = 0;
        while (i < n)
        {
            const char c = body[i];
            if (c == '/' && i + 1 < n && body[i + 1] == '/')
            {
                // To the line's end; a backslash just before it joins the next line to the comment.
                for (i += 2; i < n && body[i] != '\n'; ++i)
                    if (body[i] == '\\' && i + 1 < n && body[i + 1] == '\n')
                        ++i;
                    else if (body[i] == '\\' && i + 2 < n && body[i + 1] == '\r' && body[i + 2] == '\n')
                        i += 2;
                continue;
            }
            if (c == '/' && i + 1 < n && body[i + 1] == '*')
            {
                const std::size_t end = body.find("*/", i + 2);
                i = end == std::string::npos ? n : end + 2;
                continue;
            }
            if (c == '"')
            {
                // A string (shaderText's), to its end on its line.
                for (++i; i < n && body[i] != '"' && body[i] != '\n'; ++i)
                    if (body[i] == '\\' && i + 1 < n && body[i + 1] != '\n')
                        ++i;
                if (i < n && body[i] == '"')
                    ++i;
                continue;
            }
            const auto digit = [&](std::size_t k) { return k < n && std::isdigit(static_cast<unsigned char>(body[k])) != 0; };
            if (digit(i) || (c == '.' && digit(i + 1)))
            {
                while (i < n && (word(body[i]) || body[i] == '.'))     // a number, its suffix and exponent
                    ++i;
                continue;
            }
            if (word(c))
            {
                const std::size_t from = i;
                while (i < n && word(body[i]))
                    ++i;
                const std::string_view name(body.data() + from, i - from);
                if (name == "u_time" || name == "u_viewSize")
                    return true;
                continue;
            }
            ++i;
        }
        return false;
    }

    bool shaderAnimates(const std::string& file, const std::string& body, const Sources& headers)
    {
        if (shaderAnimates(body))
            return true;
        std::string error;
        for (const std::string& name : shaderIncludes(file, body, headers, error))
            if (const SourceFile* h = findFile(headers, name); h != nullptr && shaderAnimates(h->text))
                return true;
        return false;
    }

    Sources shaderHeaders(const Sources& files)
    {
        Sources out;
        for (const SourceFile& f : files)
            if (f.name.size() > 3 && f.name.compare(f.name.size() - 3, 3, ".sh") == 0)
                out.push_back(f);
        return out;
    }

    std::string shaderText(const std::string& file, const std::string& text, std::string& error)
    {
        error.clear();
        return walk(
            text, [](const std::string&, int, int) {},
            [&](std::string_view inner, int line, int column, bool ended) -> std::string {
                std::vector<int> codes;
                std::string why;
                if (!ended)
                    why = "a string has no closing quote on its line";
                else if (!codesOf(inner, codes, why))
                    codes.clear();
                if (!why.empty())
                {
                    if (error.empty())
                        error = prepassNote(file, line, column, why);
                    return "vec4(0.0, 0.0, 0.0, 0.0)";
                }
                return packed(codes);
            });
    }

    std::vector<std::string> shaderIncludes(const std::string& file, const std::string& body, const Sources& headers,
                                            std::string& error)
    {
        error.clear();
        std::vector<std::string> out;
        // The files to read, in the order they are reached: the shader's, then each of the program's it includes.
        std::vector<std::pair<std::string, const std::string*>> queue{ { file, &body } };
        for (std::size_t q = 0; q < queue.size(); ++q)
        {
            const std::string from = queue[q].first;
            walk(
                *queue[q].second,
                [&](const std::string& name, int line, int column) {
                    if (name == "bgfx_shader.sh" || std::find(out.begin(), out.end(), name) != out.end())
                        return;
                    if (shaderlib::libraryFile(name) != nullptr)
                    {
                        out.push_back(name);
                        return;
                    }
                    if (const SourceFile* h = findFile(headers, name))
                    {
                        out.push_back(name);
                        queue.emplace_back(name, &h->text);
                        return;
                    }
                    if (error.empty())
                        error = prepassNote(from, line, column,
                                            "there is no file " + name + " to #include (the program's .sh files, "
                                            "funk/text.sh and funk/shapes.sh)");
                },
                [](std::string_view, int, int, bool) { return std::string(); });
        }
        return out;
    }
}
