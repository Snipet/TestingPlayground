// Source/plugin/FactoryStore.cpp: see FactoryStore.h.
#include "plugin/FactoryStore.h"
#include "plugin/FileReplace.h"

#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"
#include "plugin/FactoryKeys.h"
#include "plugin/ProjectExport.h"
#include "plugin/Verify.h"

#include "dsp/Sources.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include "fminus/Xdg.h"
#endif

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace playground::factorystore
{
    namespace fs = std::filesystem;

    namespace
    {
        // ---- the website's formats (scripts/factory.ts) ------------------------------------------------------------
        constexpr const char* kIndexFormat = "funk-playground-factory";
        constexpr int kIndexVersion = 1;
        constexpr const char* kProjectFormat = "funk-playground-project";
        constexpr int kProjectVersion = 1;
        constexpr const char* kProgram = "program.fm";
        constexpr std::size_t kMaxRawBytes = std::size_t{ 64 } << 20;   // what a project's file may say it holds
        constexpr std::size_t kMaxCategory = 24;                         // ProjectStore.h's kMaxTag
        constexpr int64_t kMaxSafeInteger = (int64_t{ 1 } << 53) - 1;    // JavaScript's Number.MAX_SAFE_INTEGER
        constexpr std::size_t kMaxErrorBody = 4096;                      // what of an error's body is read
        constexpr auto kStaleTemp = std::chrono::minutes(10);            // a temporary file older is a crash's

        // An index's entry and the index, as openIndex checked them.
        struct Entry
        {
            std::string name, category, needs, sha256;
            std::size_t bytes = 0;
        };
        struct Index
        {
            std::string kind, plugin;
            int64_t sequence = 0;
            std::vector<Entry> projects;                 // in the index's order
        };

        // Why a set was not taken.
        struct Refusal
        {
            Step step = Step::none;
            std::string reason;
        };

        void refuse(Refusal& r, Step step, std::string reason)
        {
            r.step = step;
            r.reason = std::move(reason);
        }

        // The same, as the empty answer of a check that returns an optional.
        std::nullopt_t refused(Refusal& r, Step step, std::string reason)
        {
            refuse(r, step, std::move(reason));
            return std::nullopt;
        }

        // ---- JSON as JavaScript sees it --------------------------------------------------------------------------------

        bool validUtf8(const std::string& s)
        {
            return juce::CharPointer_UTF8::isValidString(s.data(), static_cast<int>(std::min<std::size_t>(s.size(), 0x7fffffff)))
                && s.size() <= 0x7fffffff;
        }

        // The JSON text parsed; false when it does not parse (or is not UTF-8, which JSON is).
        bool parseJson(const std::string& text, juce::var& out)
        {
            if (!validUtf8(text))
                return false;
            return juce::JSON::parse(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())), out).wasOk();
        }

        bool isString(const juce::var& v) { return v.isString(); }
        std::string text(const juce::var& v) { return v.toString().toStdString(); }
        bool stringIs(const juce::var& v, const char* s) { return v.isString() && v.toString() == juce::String::fromUTF8(s); }

        // String(value) in JavaScript, for the reasons that quote one.
        std::string jsString(const juce::var& v)
        {
            if (v.isVoid() || v.isUndefined())
                return "undefined";
            if (v.isBool())
                return static_cast<bool>(v) ? "true" : "false";
            if (v.isArray() || v.isObject())
                return v.isArray() ? "" : "[object Object]";
            return text(v);
        }

        // Number.isSafeInteger(value): a number with no fraction, within 2^53 - 1 either way.
        bool safeInteger(const juce::var& v, int64_t& out)
        {
            if (v.isInt() || v.isInt64())
            {
                out = static_cast<int64_t>(static_cast<juce::int64>(v));
                return out >= -kMaxSafeInteger && out <= kMaxSafeInteger;
            }
            if (v.isDouble())
            {
                const double d = static_cast<double>(v);
                if (!std::isfinite(d) || d != std::floor(d) || std::abs(d) > static_cast<double>(kMaxSafeInteger))
                    return false;
                out = static_cast<int64_t>(d);
                return true;
            }
            return false;
        }

        bool numberIs(const juce::var& v, int64_t want)
        {
            int64_t n = 0;
            return safeInteger(v, n) && n == want;
        }

        bool hasOwn(const juce::var& v, const char* name)
        {
            const juce::DynamicObject* o = v.getDynamicObject();
            return o != nullptr && o->hasProperty(juce::Identifier(name));
        }

        // A string's length as JavaScript counts it (UTF-16 code units).
        std::size_t jsLength(const std::string& s)
        {
            std::size_t n = 0;
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                const auto c = static_cast<unsigned char>(s[i]);
                if ((c & 0xC0) == 0x80)
                    continue;                            // a continuation byte
                n += c >= 0xF0 ? 2 : 1;                  // four bytes: outside the BMP, a surrogate pair
            }
            return n;
        }

        std::string lowerAscii(std::string s)
        {
            for (char& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        }

        // Base64 as Node's Buffer.from(text, 'base64') reads it: both alphabets, anything else skipped, ending at '='.
        std::vector<uint8_t> fromBase64(const std::string& s)
        {
            std::vector<uint8_t> out;
            uint32_t acc = 0;
            int bits = 0;
            for (const char ch : s)
            {
                int v = -1;
                if (ch >= 'A' && ch <= 'Z')
                    v = ch - 'A';
                else if (ch >= 'a' && ch <= 'z')
                    v = ch - 'a' + 26;
                else if (ch >= '0' && ch <= '9')
                    v = ch - '0' + 52;
                else if (ch == '+' || ch == '-')
                    v = 62;
                else if (ch == '/' || ch == '_')
                    v = 63;
                else if (ch == '=')
                    break;
                if (v < 0)
                    continue;
                acc = (acc << 6) | static_cast<uint32_t>(v);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
                }
            }
            return out;
        }

        // A version "1.2.3" (one to four digits each, as the index's isVersion takes it) as its three numbers.
        bool versionOf(const std::string& s, std::array<int, 3>& out)
        {
            std::size_t at = 0;
            for (int part = 0; part < 3; ++part)
            {
                const std::size_t start = at;
                int n = 0;
                while (at < s.size() && s[at] >= '0' && s[at] <= '9' && at - start < 4)
                    n = n * 10 + (s[at++] - '0');
                if (at == start)
                    return false;
                out[static_cast<std::size_t>(part)] = n;
                if (part < 2)
                {
                    if (at >= s.size() || s[at] != '.')
                        return false;
                    ++at;
                }
            }
            return at == s.size();
        }

        // Whether this plugin (`ours`) is at least the version an entry needs.
        bool newEnough(const std::string& ours, const std::string& needs)
        {
            std::array<int, 3> a{}, b{};
            if (!versionOf(ours, a) || !versionOf(needs, b))
                return false;
            return a >= b;
        }

        // ---- the checks (scripts/factory.ts: openIndex, checkFile, unpack, checkProjectJson) ---------------------------

        std::optional<Index> openIndex(const std::string& indexText, const Config& c, Refusal& r)
        {
            if (indexText.size() > kMaxIndexBytes)
                return refused(r, Step::size, "the index is larger than a plugin takes");
            juce::var outer;
            if (!parseJson(indexText, outer))
                return refused(r, Step::signature, "the index does not parse");
            if (!outer.isObject() || !isString(outer["key"]) || !isString(outer["signature"]) || !isString(outer["body"]))
                return refused(r, Step::signature, "the index is not a key, a signature and a body");
            const std::string keyId = text(outer["key"]);
            const Config::Key* key = nullptr;
            for (const Config::Key& k : c.keys)
                if (k.id == keyId)
                    key = &k;
            if (key == nullptr)
                return refused(r, Step::signature, "the index is signed with a key the plugins do not have (" + keyId + ")");
            const std::vector<uint8_t> signature = fromBase64(text(outer["signature"]));
            if (signature.size() != verify::kSignatureBytes)
                return refused(r, Step::signature, "the signature is not 64 bytes");
            const std::string bodyText = text(outer["body"]);
            const auto* bodyBytes = reinterpret_cast<const uint8_t*>(bodyText.data());
            if (!verify::p256(key->point, std::span<const uint8_t>(bodyBytes, bodyText.size()), signature))
                return refused(r, Step::signature, "the signature does not verify");

            juce::var body;
            if (!parseJson(bodyText, body))
                return refused(r, Step::index, "the index's body does not parse");
            if (!body.isObject() || !stringIs(body["format"], kIndexFormat) || !numberIs(body["version"], kIndexVersion))
                return refused(r, Step::index, "the body is no factory index of version 1");
            if (!stringIs(body["kind"], c.kind.c_str()))
                return refused(r, Step::index, "the index is for kind " + jsString(body["kind"]) + ", not " + c.key + "'s");
            if (!stringIs(body["plugin"], c.plugin.c_str()))
                return refused(r, Step::index, "the index is for " + jsString(body["plugin"]) + ", not " + c.plugin);
            Index index;
            index.kind = c.kind;
            index.plugin = c.plugin;
            if (!safeInteger(body["sequence"], index.sequence) || index.sequence < 1)
                return refused(r, Step::index, "the index has no sequence");
            if (!isString(body["published"]))
                return refused(r, Step::index, "the index has no date");
            const juce::Array<juce::var>* projects = body["projects"].getArray();
            if (projects == nullptr || static_cast<std::size_t>(projects->size()) > kMaxProjects)
                return refused(r, Step::index, "the index lists no projects, or too many");
            std::set<std::string> names;
            std::size_t total = 0;
            for (const juce::var& p : *projects)
            {
                if (!p.isObject() || !isString(p["name"]) || !isString(p["category"]))
                    return refused(r, Step::index, "an entry has no name");
                Entry e;
                e.name = text(p["name"]);
                e.category = text(p["category"]);
                if (e.name.empty() || projectstore::tidyName(e.name) != e.name)
                    return refused(r, Step::index, "\"" + e.name + "\" is a name the plugin would change");
                if (!names.insert(lowerAscii(e.name)).second)
                    return refused(r, Step::index, "the index lists " + e.name + " twice");
                if (jsLength(e.category) > kMaxCategory)
                    return refused(r, Step::index, e.name + ": its category is too long");
                std::array<int, 3> version{};
                if (!isString(p["needs"]) || !versionOf(text(p["needs"]), version))
                    return refused(r, Step::index, e.name + ": it does not say which plugin version it needs");
                e.needs = text(p["needs"]);
                e.sha256 = isString(p["sha256"]) ? text(p["sha256"]) : std::string();
                const bool hex = e.sha256.size() == 64
                              && std::all_of(e.sha256.begin(), e.sha256.end(),
                                             [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
                if (!hex)
                    return refused(r, Step::index, e.name + ": no SHA-256");
                int64_t bytes = 0;
                if (!safeInteger(p["bytes"], bytes) || bytes < 20 || bytes > static_cast<int64_t>(kMaxFileBytes))
                    return refused(r, Step::index, e.name + ": its file is larger than a plugin takes (or no size)");
                e.bytes = static_cast<std::size_t>(bytes);
                total += e.bytes;
                index.projects.push_back(std::move(e));
            }
            if (total > kMaxSetBytes)
                return refused(r, Step::index, "the set is larger than a plugin takes");
            return index;
        }

        // A project's file to its JSON (unpack): the package's header, then its body. The xz method is refused here, on
        // every platform, before anything is inflated.
        std::optional<std::string> unpack(const std::string& file, Refusal& r)
        {
            const auto* b = reinterpret_cast<const uint8_t*>(file.data());
            if (file.size() < static_cast<std::size_t>(projectexport::kHeaderBytes) || file.compare(0, 4, "FMPK") != 0)
                return refused(r, Step::project, "it is no project package (no FMPK)");
            if (b[4] != 1)
                return refused(r, Step::project, "it is a package of format " + std::to_string(b[4]) + ", and the plugins read 1");
            const uint8_t method = b[5];
            if (method == 2)
                return refused(r, Step::project, "it is compressed with xz, which the plugins read on macOS alone: use zlib");
            if (method != 0 && method != 1)
                return refused(r, Step::project, "it is compressed in a way (" + std::to_string(method) + ") the plugins do not read");
            uint64_t size = 0;
            for (int i = 0; i < 8; ++i)
                size |= static_cast<uint64_t>(b[8 + i]) << (8 * i);
            uint32_t crc = 0;
            for (int i = 0; i < 4; ++i)
                crc |= static_cast<uint32_t>(b[16 + i]) << (8 * i);
            if (size > kMaxRawBytes)
                return refused(r, Step::project, "it says it holds more than a project may");
            const auto* data = b + projectexport::kHeaderBytes;
            const std::size_t dataSize = file.size() - static_cast<std::size_t>(projectexport::kHeaderBytes);
            const std::optional<std::string> raw = projectexport::decompress(
                data, dataSize, method == 0 ? projectexport::Method::stored : projectexport::Method::zlib, static_cast<std::size_t>(size));
            if (!raw)
                return refused(r, Step::project, method == 0 ? "its JSON is not the size its header gives"
                                                             : "its compressed part does not read to the size its header gives");
            if (projectexport::crc32(raw->data(), raw->size()) != crc)
                return refused(r, Step::project, "its JSON does not match its checksum");
            return raw;
        }

        // A factory project's JSON (checkProjectJson, but for the private-data scan, which is the publisher's alone:
        // spec section 5), then the project ProjectStore makes of it, named by its factory name.
        std::optional<projectstore::Project> projectOf(const std::string& json, const Entry& entry, const Index& index, Refusal& r)
        {
            const auto no = [&](std::string why) {
                refuse(r, Step::project, std::move(why));
                return std::nullopt;
            };
            juce::var root;
            if (!parseJson(json, root))
                return no("its JSON does not parse");
            if (!root.isObject())
                return no("its JSON is no object");
            if (!stringIs(root["format"], kProjectFormat) || !numberIs(root["version"], kProjectVersion))
                return no("it is no project file of format 1");
            if (!stringIs(root["kind"], index.kind.c_str()))
                return no("it is a project of kind " + jsString(root["kind"]) + ", not " + index.kind);
            if (!stringIs(root["plugin"], index.plugin.c_str()))
                return no("it is a project of " + jsString(root["plugin"]) + ", not " + index.plugin);
            if (!stringIs(root["name"], entry.name.c_str()))
                return no("its name inside is \"" + jsString(root["name"]) + "\"");
            if (projectstore::tidyName(entry.name) != entry.name || entry.name.empty())
                return no("its name is one the plugin would change");
            if (hasOwn(root, "id"))
                return no("it holds a project id, which names chats on one machine: a factory project has none");
            if (hasOwn(root, "sampleFiles"))
                return no("it holds samples: a factory project is text alone");
            const juce::Array<juce::var>* versions = root["versions"].getArray();
            if (versions == nullptr || versions->size() != 1 || !(*versions)[0].isObject())
                return no("it is not one version");
            const juce::var version = (*versions)[0];
            if (!numberIs(version["number"], 1))
                return no("its version is not numbered 1");
            if (hasOwn(version, "midi"))
                return no("it holds MIDI mappings (someone's controller's)");
            if (hasOwn(version, "samples"))
                return no("its version plays samples: a factory project is text alone");
            const juce::Array<juce::var>* files = version["files"].getArray();
            if (files == nullptr || files->isEmpty())
                return no("its version names no file");
            std::set<std::string> names;
            for (const juce::var& f : *files)
            {
                if (!f.isObject() || !isString(f["name"]) || !isString(f["text"]))
                    return no("a file of its version has no name or no text");
                const std::string name = text(f["name"]);
                if (!validFileName(name))
                    return no("\"" + name + "\" is no name a tab may have");
                if (!names.insert(lowerAscii(name)).second)
                    return no("it has " + name + " twice");
            }
            if (names.count(kProgram) == 0)
                return no(std::string("it has no ") + kProgram);
            if (hasOwn(root, "presets") && !root["presets"].isArray())
                return no("its presets are no list");
            if (hasOwn(root, "info") && !root["info"].isObject())
                return no("its info is no object");

            std::string why;
            std::optional<projectstore::Project> p = projectstore::fromJson(json, entry.name, &why);
            if (!p)
                return no(why.empty() ? std::string("it does not read as a project") : why);
            if (p->versions.size() != 1 || p->latest().files.size() != names.size())
                return no("its version's files do not read as a program's");
            p->name = factoryName(entry.name);
            return p;
        }

        // A project's file against its entry (checkFile): its size, its hash, then what it holds. `capped`: a download
        // read no further than one byte past the entry's size, whose real size past that is not known.
        std::optional<projectstore::Project> checkFile(const Entry& e, const std::string& file, const Index& index, bool capped,
                                                       Refusal& r)
        {
            if (file.size() != e.bytes)
            {
                if (capped && file.size() > e.bytes)
                    refuse(r, Step::file, e.name + ": its file is larger than the " + std::to_string(e.bytes) + " bytes the index says");
                else
                    refuse(r, Step::file, e.name + ": its file is " + std::to_string(file.size()) + " bytes, and the index says "
                                              + std::to_string(e.bytes));
                return std::nullopt;
            }
            if (verify::hex(verify::sha256(file.data(), file.size())) != e.sha256)
            {
                refuse(r, Step::file, e.name + ": its file does not match its SHA-256");
                return std::nullopt;
            }
            Refusal inner;
            std::optional<std::string> json = unpack(file, inner);
            std::optional<projectstore::Project> p = json ? projectOf(*json, e, index, inner) : std::nullopt;
            if (!p)
                refuse(r, inner.step, e.name + ": " + inner.reason);
            return p;
        }

        // ---- files ---------------------------------------------------------------------------------------------------

        fs::path pathOf(const std::string& utf8)
        {
            return fs::path(std::u8string(utf8.begin(), utf8.end()));
        }

        std::optional<std::string> readFile(const fs::path& p, std::size_t limit)
        {
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(p, ec);
            if (ec || size > limit)
                return std::nullopt;
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return std::nullopt;
            std::string out(static_cast<std::size_t>(size), '\0');
            in.read(out.data(), static_cast<std::streamsize>(out.size()));
            if (static_cast<std::size_t>(in.gcount()) != out.size())
                return std::nullopt;
            return out;
        }

        // `bytes` written to `p`: beside it under a name of this write's own, then renamed over it (one step on every
        // platform: files::replace, which on Windows waits out a reader of the old file for up to half a second).
        bool writeFile(const fs::path& p, const std::string& bytes)
        {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            const std::string tag = juce::Uuid().toString().substring(0, 12).toStdString();
            const fs::path temp = p.parent_path() / pathOf(p.filename().string() + "." + tag + ".tmp");
            bool written = false;
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.close();
                written = static_cast<bool>(out);
            }
            if (written)
                (void)files::replace(temp, p, ec);   // FileReplace.h: a reader on Windows waited out
            if (!written || ec)
            {
                std::error_code e;
                fs::remove(temp, e);
                return false;
            }
            return true;
        }

        // The folder's lock (.lock), held while a set's index and state are written or read: flock on its own open file
        // (which another open of it holds apart, this process's too), on Windows an open that shares nothing
        // (FolderSync.cpp's two ways). Tried again every few milliseconds, for a few seconds at most, or until `cancel`.
        class FolderLock
        {
        public:
            FolderLock(const fs::path& dir, const std::atomic<bool>& cancel)
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                const fs::path p = dir / ".lock";
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!cancel.load() && std::chrono::steady_clock::now() < until)
                {
#if defined(_WIN32)
                    int fd = -1;
                    if (_wsopen_s(&fd, p.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0 && fd >= 0)
                    {
                        fd_ = fd;
                        return;
                    }
#else
                    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
                    if (fd >= 0)
                    {
                        if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
                        {
                            fd_ = fd;
                            return;
                        }
                        ::close(fd);
                    }
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
            ~FolderLock()
            {
                if (fd_ < 0)
                    return;
#if defined(_WIN32)
                _close(fd_);
#else
                ::flock(fd_, LOCK_UN);
                ::close(fd_);
#endif
            }
            FolderLock(const FolderLock&) = delete;
            FolderLock& operator=(const FolderLock&) = delete;
            bool held() const noexcept { return fd_ >= 0; }

        private:
            int fd_ = -1;
        };

        // state.json: the highest sequence taken and when the site last answered.
        struct DiskState
        {
            int64_t sequence = 0;
            int64_t checkedMs = 0;
        };

        DiskState readState(const fs::path& folder)
        {
            DiskState s;
            juce::var v;
            if (const std::optional<std::string> t = readFile(folder / "state.json", 4096); t && parseJson(*t, v) && v.isObject())
            {
                (void)safeInteger(v["sequence"], s.sequence);
                (void)safeInteger(v["checked"], s.checkedMs);
                s.sequence = std::max<int64_t>(0, s.sequence);
            }
            return s;
        }

        bool writeState(const fs::path& folder, const DiskState& s)
        {
            return writeFile(folder / "state.json",
                             "{\"sequence\":" + std::to_string(s.sequence) + ",\"checked\":" + std::to_string(s.checkedMs) + "}\n");
        }

        std::string answerWord(Answer a) { return a == Answer::yes ? "yes" : a == Answer::no ? "no" : ""; }

        Answer readAnswer(const fs::path& file)
        {
            juce::var v;
            if (const std::optional<std::string> t = readFile(file, 4096); t && parseJson(*t, v) && v.isObject())
            {
                if (stringIs(v["answer"], "yes"))
                    return Answer::yes;
                if (stringIs(v["answer"], "no"))
                    return Answer::no;
            }
            return Answer::notAsked;
        }

        std::string hostOf(const std::string& site) { return claude::hostName(site); }

        // ---- the process's store -----------------------------------------------------------------------------------
        std::mutex overrideMutex;
        juce::File overrideDir;
        std::mutex processMutex;
        std::weak_ptr<Store> theStore;
#if defined(FP_PROBES)
        std::optional<Config> testConfig;
#endif
    }

    // ---- the configuration -------------------------------------------------------------------------------------------

    juce::File directory()
    {
        {
            const std::lock_guard<std::mutex> lock(overrideMutex);
            if (overrideDir != juce::File())
                return overrideDir;
        }
        // A probe's sandbox (CTest sets <ENV_PREFIX>PREFS_DIR for each, cmake/Probes.cmake): never the user's folder.
        const std::string sandbox = std::string(product::kEnvPrefix) + "PREFS_DIR";
        if (const char* dir = std::getenv(sandbox.c_str()); dir != nullptr && *dir != '\0')
            return juce::File(juce::String::fromUTF8(dir)).getChildFile("factory");
#if defined(_WIN32)
        // %LOCALAPPDATA%\Funk\Playground: a cache of the website's, this machine's alone (not the roaming profile).
        return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Funk").getChildFile("Playground");
#elif defined(__APPLE__)
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Application Support")
            .getChildFile("Funk")
            .getChildFile("Playground");
#else
        // Linux: $XDG_DATA_HOME (else ~/.local/share)/Funk/Playground (fminus/Xdg.h), beside the chats.
        return juce::File(juce::String::fromUTF8(xdg::dataHome().c_str())).getChildFile("Funk").getChildFile("Playground");
#endif
    }

    void setDirectoryForTests(const juce::File& dir)
    {
        const std::lock_guard<std::mutex> lock(overrideMutex);
        overrideDir = dir;
    }

    std::string siteFrom(const char* environment)
    {
        if (environment == nullptr || *environment == '\0')
            return kSite;
        std::string s(environment);
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        const bool https = s.rfind("https://", 0) == 0;
        const bool http = s.rfind("http://", 0) == 0;
        if (!https && !http)
            return kSite;
        const std::string rest = s.substr(https ? 8 : 7);
        if (rest.empty() || rest.find_first_of("/?#@ \\") != std::string::npos)
            return kSite;                                // an origin alone: no path, query, user or space
        std::string host = rest.substr(0, rest.find(':'));
        if (const std::size_t colon = rest.find(':'); colon != std::string::npos)
        {
            const std::string port = rest.substr(colon + 1);
            if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return kSite;
        }
        for (char& c : host)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (http && host != "127.0.0.1" && host != "localhost")
            return kSite;                                // plain HTTP only to this machine (wrangler dev)
        return s;
    }

    Config processConfig()
    {
        Config c;
        const juce::File root = directory();
        c.key = product::kKey;
        c.kind = product::kKind;
        c.plugin = product::kName;
        c.version = product::kVersion;
        c.folder = root.getChildFile("Factory").getChildFile(c.key).getFullPathName().toStdString();
        c.answerFile = root.getChildFile("factory.json").getFullPathName().toStdString();
        c.site = siteFrom(std::getenv((std::string(product::kEnvPrefix) + "FACTORY_SITE").c_str()));
        for (const factory::PublicKey& k : factory::kPublicKeys)
            c.keys.push_back({ std::string(k.id), k.point });
        c.transport = claude::httpsTransport();
        c.now = [] { return static_cast<int64_t>(juce::Time::currentTimeMillis()); };
        const char* off = std::getenv((std::string(product::kEnvPrefix) + "FACTORY").c_str());
        c.off = off != nullptr && juce::String(off).trim().equalsIgnoreCase("off");
        c.userAgent = std::string(product::kName) + "/" + product::kVersion;
        return c;
    }

    // ---- the store ---------------------------------------------------------------------------------------------------

    struct Store::Impl
    {
        Config config;
        bool publishes = false;
        fs::path folder, files, answerFile;

        mutable std::mutex m;
        std::condition_variable cv;
        bool stop = false;
        bool loadWanted = false, fetchWanted = false, forced = false, answerDirty = false;
        bool busy = false;
        bool editorSeen = false;
        bool autoFetched = false;                        // an editor's open has fetched once in this store
        Answer answer = Answer::notAsked;
        State state;                                     // status computed on reading
        factory::Snapshot set;
        std::string setIndex;                            // the bytes of the index `set` came from
        std::atomic<bool> cancel{ false };
        std::atomic<int> requests{ 0 };
        std::atomic<uint64_t> generation{ 0 };
        std::thread thread;

        explicit Impl(Config c, bool p) : config(std::move(c)), publishes(p)
        {
            folder = pathOf(config.folder);
            files = folder / "p";
            answerFile = pathOf(config.answerFile);
            if (!config.now)
                config.now = [] { return static_cast<int64_t>(juce::Time::currentTimeMillis()); };
        }

        // ---- what others see ----------------------------------------------------------------------------------------

        Status statusLocked() const
        {
            if (set != nullptr)
                return Status::ready;
            if (state.fetching)
                return Status::fetching;
            if (config.off)
                return Status::off;
            if (answer == Answer::notAsked)
                return Status::notAsked;
            if (answer == Answer::no)
                return Status::declined;
            if (state.failure != Failure::none)
                return Status::failed;
            return Status::none;
        }

        void publish(factory::Snapshot s, std::string fromIndex)
        {
            {
                const std::lock_guard<std::mutex> lock(m);
                set = s;
                setIndex = std::move(fromIndex);
                state.sequence = s != nullptr ? s->sequence : 0;
                state.projects = s != nullptr ? static_cast<int>(s->projects.size()) : 0;
                state.generation = generation.fetch_add(1) + 1;
            }
            if (publishes)
                factory::publish(this, std::move(s));
        }

        void fail(Failure f, const Refusal& r)
        {
            const std::lock_guard<std::mutex> lock(m);
            state.failure = f;
            state.step = r.step;
            state.reason = r.reason;
        }

        void succeed(int64_t checkedMs)
        {
            const std::lock_guard<std::mutex> lock(m);
            state.failure = Failure::none;
            state.step = Step::none;
            state.reason.clear();
            if (checkedMs > 0)
                state.checkedMs = checkedMs;
        }

        // ---- a set from its index and its files ---------------------------------------------------------------------

        // The entries this plugin takes: those that need no newer version than this one.
        std::vector<const Entry*> taken(const Index& index) const
        {
            std::vector<const Entry*> out;
            for (const Entry& e : index.projects)
                if (newEnough(config.version, e.needs))
                    out.push_back(&e);
            return out;
        }

        static factory::Snapshot setOf(std::vector<projectstore::Project> projects, int64_t sequence)
        {
            auto s = std::make_shared<factory::Set>();
            s->projects = std::move(projects);
            std::sort(s->projects.begin(), s->projects.end(), [](const projectstore::Project& a, const projectstore::Project& b) {
                const int c = juce::String(a.name).compareIgnoreCase(juce::String(b.name));
                return c != 0 ? c < 0 : a.name < b.name;
            });
            s->fetched = true;
            s->sequence = sequence;
            return s;
        }

        fs::path fileOf(const Entry& e) const { return files / pathOf(e.sha256 + ".fmpack"); }

        // Temporary files a crash left behind (a write's own name never outlives it otherwise).
        void tidyTemps() const
        {
            std::error_code ec;
            const auto old = fs::file_time_type::clock::now() - kStaleTemp;
            for (const fs::path& dir : { folder, files })
                for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                    if (it->path().extension() == ".tmp")
                    {
                        std::error_code e;
                        if (const auto when = fs::last_write_time(it->path(), e); !e && when < old)
                            fs::remove(it->path(), e);
                    }
        }

        // ---- reading the set on disk --------------------------------------------------------------------------------

        void load()
        {
            {
                const std::lock_guard<std::mutex> lock(m);
                if (!answerDirty)
                    answer = readAnswer(answerFile);
            }
            Refusal r;
            factory::Snapshot s;
            std::string indexText;
            DiskState disk;
            bool none = false;
            std::error_code ec;
            if (!fs::is_directory(folder, ec))
                none = true;                             // nothing yet: no folder made for it
            else
            {
                const FolderLock lock(folder, cancel);   // not while another store writes a set
                disk = readState(folder);
                if (!fs::exists(folder / "index.json", ec))
                    none = true;
                else if (std::optional<std::string> t = readFile(folder / "index.json", kMaxIndexBytes + 1); !t)
                    refuse(r, Step::size, "the index on this computer does not read, or is larger than a plugin takes");
                else
                {
                    indexText = std::move(*t);
                    s = checkLocal(indexText, disk, r);
                }
                if (lock.held())
                    tidyTemps();
            }
            {
                const std::lock_guard<std::mutex> lock(m);
                state.loaded = true;
                state.checkedMs = disk.checkedMs;
            }
            if (s != nullptr)
                publish(std::move(s), std::move(indexText));
            else if (!none)
                fail(Failure::check, r);
        }

        factory::Snapshot checkLocal(const std::string& indexText, const DiskState& disk, Refusal& r) const
        {
            const std::optional<Index> index = openIndex(indexText, config, r);
            if (!index)
                return nullptr;
            if (index->sequence < disk.sequence)
            {
                refuse(r, Step::sequence, "the index is older (" + std::to_string(index->sequence) + ") than one taken before ("
                                              + std::to_string(disk.sequence) + ")");
                return nullptr;
            }
            std::vector<projectstore::Project> projects;
            for (const Entry* e : taken(*index))
            {
                const std::optional<std::string> file = readFile(fileOf(*e), kMaxFileBytes + 1);
                if (!file)
                {
                    refuse(r, Step::file, e->name + ": its file is not there");
                    return nullptr;
                }
                std::optional<projectstore::Project> p = checkFile(*e, *file, *index, false, r);
                if (!p)
                    return nullptr;
                projects.push_back(std::move(*p));
            }
            return setOf(std::move(projects), index->sequence);
        }

        // ---- fetching ------------------------------------------------------------------------------------------------

        enum class Got { ok, stopped, failed };

        // GET `path` from the site, the body no longer than `limit` bytes (one past it is read, so that a larger one is
        // known to be): ok with the body; failed with why (a connection's failure, or an answer that is not a 200).
        Got get(const std::string& path, std::size_t limit, std::string& body, int& status, Refusal& r)
        {
            body.clear();
            const claude::Headers headers = { { "User-Agent", config.userAgent } };
            const auto onChunk = [&](std::string_view chunk) {
                const std::size_t room = limit + 1 - std::min(body.size(), limit + 1);
                body.append(chunk.data(), std::min(chunk.size(), room));
                return body.size() <= limit;             // one byte past the limit: no more is read
            };
            requests.fetch_add(1);
            const claude::HttpResult res = config.transport->getLimited(config.site, path, headers, onChunk, kMaxErrorBody, cancel);
            status = res.status;
            if (cancel.load())
                return Got::stopped;
            if (res.status == 0)
                refuse(r, Step::connection, "no connection to " + hostOf(config.site));
            else if (res.status >= 300 && res.status < 400)
                refuse(r, Step::address, "the site sent the request elsewhere (" + std::to_string(res.status)
                                             + "), and the plugin follows no redirect");
            else if (res.status != 200)
                refuse(r, Step::connection, "the site answered " + std::to_string(res.status));
            else
                return Got::ok;
            return Got::failed;
        }

        void fetch()
        {
            {
                const std::lock_guard<std::mutex> lock(m);
                state.fetching = true;
            }
            Refusal r;
            const auto finish = [&](std::optional<Failure> f) {
                if (f)
                    fail(*f, r);
                const std::lock_guard<std::mutex> lock(m);
                state.fetching = false;
            };
            std::string indexText;
            int status = 0;
            const std::string key = config.key;
            switch (get("/factory/v1/" + key + "/index.json", kMaxIndexBytes, indexText, status, r))
            {
                case Got::stopped: return finish(std::nullopt);
                case Got::failed: return finish(r.step == Step::address ? Failure::check : Failure::connection);
                case Got::ok: break;
            }
            const int64_t now = config.now();
            std::string held;
            {
                const std::lock_guard<std::mutex> lock(m);
                if (set != nullptr)
                    held = setIndex;
            }
            if (!held.empty() && held == indexText)
            {
                // The index this computer has: a check, and nothing more.
                {
                    const FolderLock lock(folder, cancel);
                    if (lock.held())
                    {
                        DiskState disk = readState(folder);
                        disk.checkedMs = std::max(disk.checkedMs, now);
                        (void)writeState(folder, disk);
                    }
                }
                succeed(now);
                return finish(std::nullopt);
            }
            const std::optional<Index> index = openIndex(indexText, config, r);
            if (!index)
                return finish(Failure::check);
            {
                int64_t highest = readState(folder).sequence;
                const std::lock_guard<std::mutex> lock(m);
                if (set != nullptr)
                    highest = std::max(highest, set->sequence);
                if (index->sequence < highest)
                    refuse(r, Step::sequence, "the index is older (" + std::to_string(index->sequence) + ") than one taken before ("
                                                  + std::to_string(highest) + ")");
            }
            if (r.step == Step::sequence)
                return finish(Failure::check);

            // Each file: the folder's when it is there and passes, else fetched; each checked, a project made of it.
            std::vector<projectstore::Project> projects;
            std::map<std::string, std::string> kept;    // every file of the set, by its hash: rewritten at the commit if
                                                         // another store took one away meanwhile
            std::vector<fs::path> added;                 // the files this fetch wrote
            const auto dropAdded = [&] {
                std::string inUse;
                {
                    const std::lock_guard<std::mutex> lock(m);
                    if (set != nullptr)
                        inUse = setIndex;
                }
                std::set<std::string> named;
                for (const Entry& e : entriesOfIndex(inUse))
                    named.insert(e.sha256);
                for (const fs::path& p : added)
                    if (named.count(p.stem().string()) == 0)
                    {
                        std::error_code ec;
                        fs::remove(p, ec);
                    }
            };
            for (const Entry* e : taken(*index))
            {
                std::optional<projectstore::Project> p;
                std::string file;
                if (std::optional<std::string> there = readFile(fileOf(*e), kMaxFileBytes + 1); there && there->size() == e->bytes)
                {
                    Refusal local;
                    p = checkFile(*e, *there, *index, false, local);
                    if (p)
                        file = std::move(*there);
                }
                if (!p)
                {
                    std::error_code ec;
                    fs::remove(fileOf(*e), ec);          // a file that does not pass is no use to anyone
                    switch (get("/projects/v1/p/" + e->sha256 + ".fmpack", e->bytes, file, status, r))
                    {
                        case Got::stopped: return finish(std::nullopt);
                        case Got::failed:
                            if (status == 404)
                            {
                                refuse(r, Step::file, e->name + ": its file is not there");
                                dropAdded();
                                return finish(Failure::check);
                            }
                            if (r.step == Step::address)
                                dropAdded();
                            return finish(r.step == Step::address ? Failure::check : Failure::connection);
                        case Got::ok: break;
                    }
                    p = checkFile(*e, file, *index, true, r);
                    if (!p)
                    {
                        dropAdded();
                        return finish(Failure::check);
                    }
                    if (writeFile(fileOf(*e), file))
                        added.push_back(fileOf(*e));
                }
                projects.push_back(std::move(*p));
                kept[e->sha256] = std::move(file);
            }

            // Every check passed: the files (any another store took away meanwhile), the index, the state, under the
            // folder's lock, unless another store took a newer set meanwhile.
            bool newer = false, written = false;
            {
                const FolderLock lock(folder, cancel);
                if (lock.held())
                {
                    const DiskState disk = readState(folder);
                    Refusal ignored;
                    int64_t onDisk = disk.sequence;
                    if (std::optional<std::string> t = readFile(folder / "index.json", kMaxIndexBytes + 1))
                        if (std::optional<Index> other = openIndex(*t, config, ignored))
                            onDisk = std::max(onDisk, other->sequence);
                    newer = onDisk > index->sequence;
                    if (!newer)
                    {
                        written = true;
                        for (const Entry* e : taken(*index))
                        {
                            std::error_code ec;
                            if (fs::file_size(fileOf(*e), ec) != e->bytes || ec)
                                written = written && writeFile(fileOf(*e), kept[e->sha256]);
                        }
                        written = written && writeFile(folder / "index.json", indexText)
                               && writeState(folder, { index->sequence, std::max(disk.checkedMs, now) });
                        if (written)
                            prune(*index);
                    }
                }
            }
            if (cancel.load())
                return finish(std::nullopt);
            if (newer)
            {
                finish(std::nullopt);
                load();                                  // another store's newer set, as it is on disk
                return;
            }
            if (!written)
            {
                refuse(r, Step::disk, "the factory projects could not be written to this computer");
                return finish(Failure::connection);
            }
            publish(setOf(std::move(projects), index->sequence), indexText);
            succeed(now);
            finish(std::nullopt);
        }

        // The entries of an index this store has already checked (its set's), for what they name.
        std::vector<Entry> entriesOfIndex(const std::string& indexText) const
        {
            Refusal ignored;
            if (std::optional<Index> index = openIndex(indexText, config, ignored))
                return index->projects;
            return {};
        }

        // The files no longer named by the index just written (a withdrawn or changed project's), and stale temporaries.
        void prune(const Index& index) const
        {
            std::set<std::string> named;
            for (const Entry& e : index.projects)
                named.insert(e.sha256 + ".fmpack");
            std::error_code ec;
            for (fs::directory_iterator it(files, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == ".fmpack" && named.count(it->path().filename().string()) == 0)
                {
                    std::error_code e;
                    fs::remove(it->path(), e);
                }
            tidyTemps();
        }

        // ---- the thread ------------------------------------------------------------------------------------------------

        void considerFetch(bool force)
        {
            Answer a;
            bool allowed = false;
            {
                std::unique_lock<std::mutex> lock(m);
                if (!answerDirty)
                {
                    lock.unlock();
                    const Answer onDisk = readAnswer(answerFile);   // the other product may have asked meanwhile
                    lock.lock();
                    if (!answerDirty)
                        answer = onDisk;
                }
                a = answer;
                if (config.off || a != Answer::yes)
                    return;
                if (force)
                    allowed = true;
                else if (editorSeen && !autoFetched)
                {
                    const bool fresh = set != nullptr && config.now() - state.checkedMs <= kRecheckMs;
                    allowed = !fresh;
                    autoFetched = allowed;
                }
            }
            if (allowed)
                fetch();
        }

        void run()
        {
            for (;;)
            {
                bool doLoad = false, doFetch = false, doForce = false, doAnswer = false;
                Answer a = Answer::notAsked;
                {
                    std::unique_lock<std::mutex> lock(m);
                    cv.wait(lock, [&] { return stop || loadWanted || fetchWanted || answerDirty; });
                    if (stop)
                        return;
                    doLoad = std::exchange(loadWanted, false);
                    doFetch = std::exchange(fetchWanted, false);
                    doForce = std::exchange(forced, false);
                    doAnswer = answerDirty;
                    a = answer;
                    busy = true;
                }
                if (doAnswer)
                {
                    const std::string word = answerWord(a);
                    (void)writeFile(answerFile, word.empty() ? std::string("{}\n") : "{\"answer\":\"" + word + "\"}\n");
                    const std::lock_guard<std::mutex> lock(m);
                    if (answer == a)
                        answerDirty = false;
                }
                bool loaded = false;
                {
                    const std::lock_guard<std::mutex> lock(m);
                    loaded = state.loaded;
                }
                if (doLoad || (doFetch && !loaded))
                    load();
                if (doFetch && !cancel.load())
                    considerFetch(doForce);
                const std::lock_guard<std::mutex> lock(m);
                busy = false;
                cancel.store(false);
                cv.notify_all();
            }
        }
    };

    Store::Store(Config config, bool publishes) : s_(std::make_unique<Impl>(std::move(config), publishes))
    {
        s_->thread = std::thread([impl = s_.get()] { impl->run(); });
    }

    Store::~Store()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->stop = true;
            s_->cancel.store(true);
        }
        s_->cv.notify_all();
        if (s_->thread.joinable())
            s_->thread.join();
        if (s_->publishes)
            factory::publish(s_.get(), nullptr);
    }

    void Store::start()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->loadWanted = true;
        }
        s_->cv.notify_all();
    }

    Answer Store::answer() const
    {
        const std::lock_guard<std::mutex> lock(s_->m);
        return s_->answer;
    }

    void Store::setAnswer(Answer a)
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->answer = a;
            s_->answerDirty = true;
        }
        s_->cv.notify_all();
    }

    void Store::editorOpened()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->editorSeen = true;
            s_->fetchWanted = true;
        }
        s_->cv.notify_all();
    }

    void Store::refreshNow()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->fetchWanted = true;
            s_->forced = true;
        }
        s_->cv.notify_all();
    }

    State Store::state() const
    {
        const std::lock_guard<std::mutex> lock(s_->m);
        State st = s_->state;
        st.answer = s_->answer;
        st.status = s_->statusLocked();
        return st;
    }

    factory::Snapshot Store::set() const
    {
        const std::lock_guard<std::mutex> lock(s_->m);
        return s_->set;
    }

    uint64_t Store::generation() const noexcept { return s_->generation.load(); }

    bool Store::idle() const
    {
        const std::lock_guard<std::mutex> lock(s_->m);
        return !s_->busy && !s_->loadWanted && !s_->fetchWanted && !s_->answerDirty;
    }

    int Store::requests() const noexcept { return s_->requests.load(); }

    void Store::cancel()
    {
        const std::lock_guard<std::mutex> lock(s_->m);
        if (s_->busy)
            s_->cancel.store(true);                      // cleared again when the thread's work ends
    }

    // ---- the process's -------------------------------------------------------------------------------------------------

    std::shared_ptr<Store> attach()
    {
        const std::lock_guard<std::mutex> lock(processMutex);
        std::shared_ptr<Store> s = theStore.lock();
        if (!s)
        {
#if defined(FP_PROBES)
            s = std::make_shared<Store>(testConfig ? *testConfig : processConfig(), true);
#else
            s = std::make_shared<Store>(processConfig(), true);
#endif
            s->start();
            theStore = s;
        }
        return s;
    }

    std::shared_ptr<Store> process()
    {
        const std::lock_guard<std::mutex> lock(processMutex);
        return theStore.lock();
    }

#if defined(FP_PROBES)
    void useConfigForTests(const Config* config)
    {
        const std::lock_guard<std::mutex> lock(processMutex);
        if (config != nullptr)
            testConfig = *config;
        else
            testConfig.reset();
    }
#endif
}
