// Source/plugin/ProjectStore.cpp: see ProjectStore.h.
#include "plugin/ProjectStore.h"

#include "plugin/EditorPort.h"
#include "plugin/Factory.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace playground::projectstore
{
    namespace
    {
        std::mutex overrideMutex;
        juce::File overrideDir;                          // empty: the default
        constexpr const char* kFormat = "funk-playground-project";
        constexpr const char* kLegacyFormat = "funk-playground-preset";
        constexpr int kFormatVersion = 1;
        constexpr juce::int64 kMaxBytes = 128 * 1024 * 1024;  // a project larger than this is no project (its sample
                                                              // files, kMaxSampleBytes, are a third more as base64)

        // A file's identity: its device and inode where a look gives them (POSIX), so that two spellings of one name
        // (case aside, on a volume that ignores case) are known for one file; none on Windows (0, 0).
        struct FileId
        {
            uint64_t device = 0, inode = 0;
            bool known() const noexcept { return inode != 0; }
            friend bool operator==(const FileId&, const FileId&) = default;
        };

        // The listing's cache, and its presets' (presetsOf): a file read again only when its size or its time changes.
        // A file that is no project of this plugin's (damaged, the other kind's) is kept as broken, so a listing does not
        // read it whole again until it changes.
        struct Cached
        {
            juce::int64 modified = 0, size = 0;
            FileId id;
            Entry entry;
            std::vector<PresetSummary> presets;
            bool broken = false;
            std::string project, writtenAs;              // the project's id and the name written in its file (idOf)
        };
        std::mutex cacheMutex;
        std::map<std::string, Cached> cache;             // by the file's full path
        std::atomic<int> parsed{ 0 };                    // project files read whole (parses())
        std::atomic<int> readerParsed{ 0 };              // of them, by the Reader's thread (readerParses())
        std::atomic<int> listed{ 0 };                    // listings, cached or not (listings())
        std::mutex readerMutex;
        std::weak_ptr<Reader> theReader;                 // the process's (reader()), while someone holds it

        std::string kindName(bool instrument) { return instrument ? "instrument" : "effect"; }

        // A new project's id: a version 4 UUID from the system's random source, lower case.
        std::string newProjectId()
        {
            std::random_device rd;
            uint8_t b[16];
            for (int i = 0; i < 16; i += 4)
            {
                const uint32_t r = rd();
                for (int k = 0; k < 4; ++k)
                    b[i + k] = static_cast<uint8_t>(r >> (8 * k));
            }
            b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
            b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
            static constexpr char kHex[] = "0123456789abcdef";
            std::string out;
            for (int i = 0; i < 16; ++i)
            {
                if (i == 4 || i == 6 || i == 8 || i == 10)
                    out += '-';
                out += kHex[b[i] >> 4];
                out += kHex[b[i] & 0x0F];
            }
            return out;
        }

        juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

        // The file of the project named so (case aside), or none.
        juce::File fileOf(std::string_view name, const char* extension = kExtension)
        {
            const std::string tidy = tidyName(name);
            if (tidy.empty())
                return {};
            for (const juce::File& f : directory().findChildFiles(juce::File::findFiles, false, juce::String("*") + extension))
                if (sameName(f.getFileNameWithoutExtension().toStdString(), tidy))
                    return f;
            return {};
        }

        juce::var dialsVar(const std::vector<Dial>& dials)
        {
            juce::Array<juce::var> out;
            for (const Dial& d : dials)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("name", str(d.name));
                o->setProperty("value", std::isfinite(d.value) ? static_cast<double>(d.value) : 0.0);
                if (!d.label.empty())
                    o->setProperty("label", str(d.label));
                out.add(juce::var(o));
            }
            return out;
        }

        std::vector<Dial> dialsOf(const juce::var& v)
        {
            std::vector<Dial> out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& d : *a)
                    if (d.isObject())
                    {
                        const double value = static_cast<double>(d["value"]);
                        out.push_back({ d["name"].toString().toStdString(), std::isfinite(value) ? static_cast<float>(value) : 0.0f,
                                        d.hasProperty("label") ? d["label"].toString().toStdString() : std::string() });
                    }
            return out;
        }

        juce::var midiVar(const std::vector<midilearn::Mapping>& midi)
        {
            juce::Array<juce::var> out;
            for (const midilearn::Mapping& m : midi)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("dial", str(m.dial));
                o->setProperty("channel", m.channel);
                o->setProperty("cc", m.cc);
                if (m.fine)
                    o->setProperty("fine", true);
                out.add(juce::var(o));
            }
            return out;
        }

        // A version's MIDI mappings, tidied (MidiLearn.h's tidy: a damaged one left out).
        std::vector<midilearn::Mapping> midiOf(const juce::var& v)
        {
            std::vector<midilearn::Mapping> out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& e : *a)
                    if (e.isObject() && out.size() < static_cast<std::size_t>(4 * kMaxControls))
                        out.push_back({ e["dial"].toString().substring(0, 64).toStdString(), static_cast<int>(e["channel"]),
                                        e.hasProperty("cc") ? static_cast<int>(e["cc"]) : -1, static_cast<bool>(e["fine"]) });
            return midilearn::tidy(std::move(out));
        }

        juce::var answersVar(const std::vector<Answer>& answers)
        {
            juce::Array<juce::var> out;
            for (const Answer& a : answers)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("key", str(a.key));
                o->setProperty("text", str(a.text));
                out.add(juce::var(o));
            }
            return out;
        }

        // A version's answers: each a key and a line, at most kMaxAnswers, a key once (what the store's restore checks
        // is checked there: circuits::restore).
        std::vector<Answer> answersOf(const juce::var& v)
        {
            std::vector<Answer> out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& e : *a)
                {
                    if (!e.isObject() || out.size() >= kMaxAnswers)
                        continue;
                    Answer x{ e["key"].toString().toStdString(), e["text"].toString().toStdString() };
                    if (x.key.empty() || x.key.size() > 64 || x.text.empty() || x.text.size() > 2000
                        || std::any_of(out.begin(), out.end(), [&](const Answer& o) { return o.key == x.key; }))
                        continue;
                    out.push_back(std::move(x));
                }
            return out;
        }

        // `more` over `kept`, by key (a newer answer for a key replaces the older), at most kMaxAnswers.
        std::vector<Answer> mergedAnswers(std::vector<Answer> kept, const std::vector<Answer>& more)
        {
            for (const Answer& a : more)
            {
                const auto it = std::find_if(kept.begin(), kept.end(), [&](const Answer& k) { return k.key == a.key; });
                if (it != kept.end())
                    it->text = a.text;
                else if (kept.size() < kMaxAnswers)
                    kept.push_back(a);
            }
            return kept;
        }

        juce::var filesVar(const Sources& files)
        {
            juce::Array<juce::var> out;
            for (const SourceFile& f : files)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("name", str(f.name));
                o->setProperty("text", str(f.text));
                out.add(juce::var(o));
            }
            return out;
        }

        Sources filesOf(const juce::var& v)
        {
            Sources out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& f : *a)
                    if (f.isObject())
                        out.push_back({ f["name"].toString().toStdString(), f["text"].toString().toStdString() });
            return out;
        }

        // The labels aside: a label follows from its value and the files' options, and a version saved before labels has
        // none (saveVersion gives it them).
        bool sameDials(const std::vector<Dial>& a, const std::vector<Dial>& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Dial& x, const Dial& y) {
                       return x.name == y.name && x.value == y.value;
                   });
        }

        // The labels `from` has that `to` lacks (the same dials, sameDials), given to `to`: whether any was.
        bool addLabels(std::vector<Dial>& to, const std::vector<Dial>& from)
        {
            bool added = false;
            for (std::size_t i = 0; i < to.size() && i < from.size(); ++i)
                if (to[i].label.empty() && !from[i].label.empty())
                {
                    to[i].label = from[i].label;
                    added = true;
                }
            return added;
        }

        void fail(std::string* why, const std::string& what)
        {
            if (why != nullptr)
                *why = what;
        }

        std::vector<std::string> stringsOf(const juce::var& v)
        {
            std::vector<std::string> out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& s : *a)
                    out.push_back(s.toString().toStdString());
            return out;
        }

        juce::var stringsVar(const std::vector<std::string>& v)
        {
            juce::Array<juce::var> out;
            for (const std::string& s : v)
                out.add(str(s));
            return out;
        }

        // The project in a file's JSON: nothing, with why, when it is not one of this plugin's or it is damaged.
        // A version's samples (ProjectStore.h): each a valid sample's name once, with a hash of 16 hex digits.
        juce::var samplesVar(const std::vector<SampleRef>& refs)
        {
            juce::Array<juce::var> out;
            for (const SampleRef& r : refs)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("name", str(r.name));
                o->setProperty("hash", str(r.hash));
                out.add(juce::var(o));
            }
            return out;
        }

        std::vector<SampleRef> samplesOf(const juce::var& v)
        {
            std::vector<SampleRef> out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& e : *a)
                {
                    SampleRef r{ e["name"].toString().toStdString(), e["hash"].toString().toStdString() };
                    const bool hex = r.hash.size() == 16
                                  && std::all_of(r.hash.begin(), r.hash.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
                    if (assets::validName(r.name) && hex && out.size() < assets::kMaxAssets
                        && std::none_of(out.begin(), out.end(), [&](const SampleRef& o) { return o.name == r.name; }))
                        out.push_back(std::move(r));
                }
            return out;
        }

        // The project's sample files: each its hash and its bytes as base64; one whose bytes are not its hash's (damaged)
        // or past a file's cap is left out, and so is what would take the files past kMaxSampleBytes.
        juce::var sampleFilesVar(const std::vector<SampleFile>& files)
        {
            juce::Array<juce::var> out;
            for (const SampleFile& f : files)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("hash", str(f.hash));
                o->setProperty("data", juce::Base64::toBase64(f.bytes.data(), f.bytes.size()));
                out.add(juce::var(o));
            }
            return out;
        }

        std::vector<SampleFile> sampleFilesOf(const juce::var& v)
        {
            std::vector<SampleFile> out;
            std::size_t total = 0;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& e : *a)
                {
                    juce::MemoryOutputStream bytes;
                    if (!juce::Base64::convertFromBase64(bytes, e["data"].toString()))
                        continue;
                    SampleFile f{ e["hash"].toString().toStdString(),
                                  std::string(static_cast<const char*>(bytes.getData()), bytes.getDataSize()) };
                    if (f.bytes.size() > assets::kMaxFileBytes || assets::hashOf(f.bytes) != f.hash
                        || total + f.bytes.size() > kMaxSampleBytes
                        || std::any_of(out.begin(), out.end(), [&](const SampleFile& o) { return o.hash == f.hash; }))
                        continue;
                    total += f.bytes.size();
                    out.push_back(std::move(f));
                }
            return out;
        }

        // The project's sample files that its versions name, each once, in the order first named (`more`: files for the
        // names it lacks, a save's).
        void keepNamedSamples(Project& p, const assets::Set* more = nullptr)
        {
            std::vector<SampleFile> kept;
            for (const Version& v : p.versions)
                for (const SampleRef& r : v.samples)
                {
                    if (std::any_of(kept.begin(), kept.end(), [&](const SampleFile& f) { return f.hash == r.hash; }))
                        continue;
                    if (const SampleFile* f = p.sample(r.hash))
                        kept.push_back(*f);
                    else if (more != nullptr)
                        for (const std::shared_ptr<const assets::Asset>& a : more->items)
                            if (a->hash() == r.hash)
                            {
                                kept.push_back({ a->hash(), a->bytes() });
                                break;
                            }
                }
            p.samples = std::move(kept);
        }

        bool sameSamples(std::vector<SampleRef> a, std::vector<SampleRef> b)
        {
            const auto byName = [](const SampleRef& x, const SampleRef& y) { return x.name < y.name; };
            std::sort(a.begin(), a.end(), byName);
            std::sort(b.begin(), b.end(), byName);
            return a == b;
        }
        void sortPresets(std::vector<Preset>& presets);

        std::optional<Project> parseRoot(const juce::var& root, const std::string& name, const std::string& label, std::string* why)
        {
            if (!root.isObject() || root["format"].toString() != kFormat)
            {
                fail(why, label + " is not a project (or it is damaged)");
                return std::nullopt;
            }
            if (static_cast<int>(root["version"]) > kFormatVersion)
            {
                fail(why, label + " is from a newer version of the plugin");
                return std::nullopt;
            }
            const std::string kind = root["kind"].toString().toStdString();
            if (kind != kindName(product::kIsInstrument))
            {
                fail(why, "\"" + name + "\" is a project of the " + kind + " playground (" + root["plugin"].toString().toStdString()
                              + "): this one plays " + kindName(product::kIsInstrument) + "s");
                return std::nullopt;
            }
            Project p;
            p.name = name;
            // Its id, when it has one that may be one (else none: the next write gives it one), and the name it was
            // written under.
            if (const std::string id = root["id"].toString().toLowerCase().toStdString(); validProjectId(id))
                p.id = id;
            p.writtenAs = root["name"].toString().toStdString();
            if (const juce::Array<juce::var>* versions = root["versions"].getArray())
                for (const juce::var& v : *versions)
                {
                    if (!v.isObject())
                        continue;
                    Version ver;
                    ver.number = static_cast<int>(v["number"]);
                    ver.savedMs = static_cast<int64_t>(static_cast<juce::int64>(v["saved"]));
                    ver.note = v["note"].toString().toStdString();
                    ver.active = static_cast<int>(v["active"]);
                    ver.files = filesOf(v["files"]);
                    ver.dials = dialsOf(v["dials"]);
                    ver.circuits = answersOf(v["circuits"]);
                    ver.midi = midiOf(v["midi"]);
                    ver.samples = samplesOf(v["samples"]);
                    if (!ver.files.empty() && ver.number > 0)
                        p.versions.push_back(std::move(ver));
                }
            std::sort(p.versions.begin(), p.versions.end(), [](const Version& a, const Version& b) { return a.number < b.number; });
            if (p.versions.empty())
            {
                fail(why, "\"" + p.name + "\" has no program");
                return std::nullopt;
            }
            if (const juce::Array<juce::var>* presets = root["presets"].getArray())
                for (const juce::var& v : *presets)
                {
                    if (!v.isObject())
                        continue;
                    Preset pr;
                    pr.name = tidyName(v["name"].toString().toStdString());
                    pr.savedMs = static_cast<int64_t>(static_cast<juce::int64>(v["saved"]));
                    pr.from = static_cast<int>(v["from"]);
                    pr.dials = dialsOf(v["dials"]);
                    pr.category = tidyCategory(v["category"].toString().toStdString());   // none in an older file
                    if (!pr.name.empty() && p.preset(pr.name) == nullptr)
                        p.presets.push_back(std::move(pr));
                }
            if (const juce::var info = root["info"]; info.isObject())
            {
                Info in;
                in.description = info["description"].toString().toStdString();
                in.author = info["author"].toString().toStdString();
                in.category = info["category"].toString().toStdString();
                in.tags = stringsOf(info["tags"]);
                in.genres = stringsOf(info["genres"]);
                p.info = tidyInfo(std::move(in));
            }
            p.samples = sampleFilesOf(root["sampleFiles"]);
            sortPresets(p.presets);                      // by name, case aside, whoever wrote the file (their numbers)
            return p;
        }

        // The project in `f`.
        std::optional<Project> parse(const juce::File& f, std::string* why)
        {
            parsed.fetch_add(1, std::memory_order_relaxed);
            if (f.getSize() > kMaxBytes)
            {
                fail(why, f.getFileName().toStdString() + " is too large to be a project");
                return std::nullopt;
            }
            return parseRoot(juce::JSON::parse(f.loadFileAsString()), f.getFileNameWithoutExtension().toStdString(),
                             f.getFileName().toStdString(), why);
        }

        // A file's time (ms), size and identity from one look at it (a stat), or false when it is no file. On Windows,
        // JUCE's two, and no identity.
        bool look(const juce::File& f, juce::int64& modified, juce::int64& size, FileId& id)
        {
#if defined(_WIN32)
            if (!f.existsAsFile())
                return false;
            modified = f.getLastModificationTime().toMilliseconds();
            size = f.getSize();
            id = {};
            return true;
#else
            struct stat st{};
            if (::stat(f.getFullPathName().toRawUTF8(), &st) != 0 || !S_ISREG(st.st_mode))
                return false;
#if defined(__APPLE__)
            const timespec& t = st.st_mtimespec;
#else
            const timespec& t = st.st_mtim;
#endif
            modified = static_cast<juce::int64>(t.tv_sec) * 1000 + static_cast<juce::int64>(t.tv_nsec / 1000000);
            size = static_cast<juce::int64>(st.st_size);
            id = { static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino) };
            return true;
#endif
        }

        // The folder's project files (*.fmproject, the extension's case aside), each with its time and size. On the Mac
        // read with readdir and one look a file: JUCE's listing asks macOS of every file whether it is hidden, and the
        // time and the size were a look each, about 50 us a file in all on a busy Mac. On Windows JUCE's listing, whose
        // entries hold the time and the size from the folder's own records (as look() reads them, the same
        // conversion), so no look more.
        struct Found
        {
            juce::File file;
            juce::int64 modified = 0, size = 0;
            FileId id;
        };
        std::vector<Found> projectFiles(const juce::File& dir)
        {
            std::vector<Found> out;
#if defined(_WIN32)
            for (const juce::DirectoryEntry& e :
                 juce::RangedDirectoryIterator(dir, false, juce::String("*") + kExtension, juce::File::findFiles))
                out.push_back({ e.getFile(), e.getModificationTime().toMilliseconds(), e.getFileSize(), {} });
#else
            DIR* d = ::opendir(dir.getFullPathName().toRawUTF8());
            if (d == nullptr)
                return out;
            const std::string_view ext = kExtension;
            while (const dirent* e = ::readdir(d))
            {
                const std::string_view name = e->d_name;
                if (name.size() <= ext.size()
                    || !sameName(name.substr(name.size() - ext.size()), ext))
                    continue;
                Found x{ dir.getChildFile(juce::String::fromUTF8(e->d_name)), 0, 0, {} };
                if (look(x.file, x.modified, x.size, x.id))
                    out.push_back(std::move(x));
            }
            ::closedir(d);
#endif
            return out;
        }

        // What the cache keeps of a project read whole: the listing's entry and its presets'.
        Cached cachedOf(const Project& p, juce::int64 modified, juce::int64 size, FileId id)
        {
            Cached c;
            c.modified = modified;
            c.size = size;
            c.id = id;
            c.entry.name = p.name;
            c.project = p.id;
            c.writtenAs = p.writtenAs;
            c.entry.savedMs = p.latest().savedMs;
            c.entry.latest = p.latest().number;
            c.entry.versions = static_cast<int>(p.versions.size());
            for (const Preset& pr : p.presets)
            {
                c.entry.presets.push_back(pr.name);
                c.presets.push_back({ pr.name, pr.savedMs, pr.from, pr.category, pr.dials });
            }
            c.entry.category = p.info.category;
            c.entry.tags = p.info.tags;
            c.entry.description = p.info.description;
            return c;
        }

        // What a read of `f` (a parse of the file as a look before it found it, `modified` and `size`) leaves in the
        // cache: its entry, or `p` none (no project of this plugin's) a broken mark, unless the file has changed since
        // that look: then nothing, and the next listing reads it again. So a read never puts an older file's entry over
        // a newer one's (a write's own, write() below, or another read's). The caller holds cacheMutex.
        void keepRead(const juce::File& f, const std::optional<Project>& p, juce::int64 modified, juce::int64 size)
        {
            juce::int64 m = 0, s = 0;
            FileId id;
            if (!look(f, m, s, id) || m != modified || s != size)
                return;
            Cached c;
            if (p)
                c = cachedOf(*p, m, s, id);
            else
            {
                c.modified = m;
                c.size = s;
                c.id = id;
                c.broken = true;
            }
            cache[f.getFullPathName().toStdString()] = std::move(c);
        }

        // A project's file read whole (a look first, for the cache's time and size) and its entry kept (keepRead).
        std::optional<Project> readAndKeep(const juce::File& f, std::string* why)
        {
            juce::int64 modified = 0, size = 0;
            FileId id;
            const bool looked = look(f, modified, size, id);
            std::optional<Project> p = parse(f, why);
            if (looked)
            {
                const std::lock_guard<std::mutex> lock(cacheMutex);
                keepRead(f, p, modified, size);
            }
            return p;
        }

        juce::var rootOf(const Project& p);

        void sortPresets(std::vector<Preset>& presets)
        {
            std::sort(presets.begin(), presets.end(), [](const Preset& a, const Preset& b) {
                const int c = str(a.name).compareIgnoreCase(str(b.name));
                return c != 0 ? c < 0 : a.name < b.name;
            });
        }

        // A factory project's name (Factory.h): never written, its why saying what a save does instead.
        bool readOnly(std::string_view name, std::string* why)
        {
            if (!isFactoryName(name))
                return false;
            fail(why, "\"" + ownName(name) + "\" is a factory project: it stays as it is (saving it makes a project of your own)");
            return true;
        }

        // Writes `p` under p.name: beside, then moved over, so a project is never half written; one of the name in
        // another case goes. The cache then holds what it wrote (its entry, as the file now is): the listing after a
        // save of this process's reads nothing whole (the explorer's next poll), where it read the file it had just
        // written. A read of the file before (another thread's) checks the file again before it keeps its entry
        // (keepRead), so it never takes this one's place.
        bool write(Project p, std::string* why)
        {
            if (readOnly(p.name, why))
                return false;
            p.name = tidyName(p.name);
            if (p.name.empty())
            {
                fail(why, "a project needs a name");
                return false;
            }
            if (p.id.empty())
                p.id = newProjectId();                   // made when it is first written (ProjectStore.h)
            p.writtenAs = p.name;
            const juce::File dir = directory();
            if (!dir.isDirectory() && !dir.createDirectory())
            {
                fail(why, "cannot make the folder " + dir.getFullPathName().toStdString());
                return false;
            }
            sortPresets(p.presets);
            const juce::String json = str(toJson(p));
            const juce::File target = dir.getChildFile(str(p.name) + kExtension);
            const juce::File temp = dir.getChildFile("." + str(p.name) + ".saving");
            if (!temp.replaceWithText(json, false, false, "\n"))
            {
                fail(why, "cannot write " + temp.getFullPathName().toStdString());
                return false;
            }
            const juce::File old = fileOf(p.name);
            if (old != juce::File() && old.getFileName() != target.getFileName())
                old.deleteFile();
            if (!temp.moveFileTo(target))
            {
                temp.deleteFile();
                fail(why, "cannot replace " + target.getFullPathName().toStdString());
                return false;
            }
            juce::int64 modified = 0, size = 0;
            FileId id;
            const std::lock_guard<std::mutex> lock(cacheMutex);
            if (look(target, modified, size, id))
                cache[target.getFullPathName().toStdString()] = cachedOf(p, modified, size, id);
            return true;
        }

        juce::var rootOf(const Project& p)
        {
            auto* root = new juce::DynamicObject();
            root->setProperty("format", kFormat);
            root->setProperty("version", kFormatVersion);
            root->setProperty("plugin", product::kName);
            root->setProperty("kind", str(kindName(product::kIsInstrument)));
            root->setProperty("name", str(p.name));
            if (!p.id.empty())
                root->setProperty("id", str(p.id));
            juce::Array<juce::var> versions;
            for (const Version& v : p.versions)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("number", v.number);
                o->setProperty("saved", static_cast<juce::int64>(v.savedMs));
                o->setProperty("note", str(v.note));
                o->setProperty("active", v.active);
                o->setProperty("files", filesVar(v.files));
                o->setProperty("dials", dialsVar(v.dials));
                if (!v.circuits.empty())
                    o->setProperty("circuits", answersVar(v.circuits));
                if (!v.midi.empty())
                    o->setProperty("midi", midiVar(v.midi));
                if (!v.samples.empty())
                    o->setProperty("samples", samplesVar(v.samples));
                versions.add(juce::var(o));
            }
            root->setProperty("versions", versions);
            juce::Array<juce::var> presets;
            for (const Preset& pr : p.presets)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("name", str(pr.name));
                o->setProperty("saved", static_cast<juce::int64>(pr.savedMs));
                o->setProperty("from", pr.from);
                if (!pr.category.empty())
                    o->setProperty("category", str(pr.category));
                o->setProperty("dials", dialsVar(pr.dials));
                presets.add(juce::var(o));
            }
            root->setProperty("presets", presets);
            if (!p.info.empty())
            {
                auto* info = new juce::DynamicObject();
                info->setProperty("description", str(p.info.description));
                info->setProperty("author", str(p.info.author));
                info->setProperty("category", str(p.info.category));
                info->setProperty("tags", stringsVar(p.info.tags));
                info->setProperty("genres", stringsVar(p.info.genres));
                root->setProperty("info", juce::var(info));
            }
            if (!p.samples.empty())
                root->setProperty("sampleFiles", sampleFilesVar(p.samples));
            return juce::var(root);
        }

        // The playground's presets before projects: each a project of one version, the old file then moved aside.
        void migrate()
        {
            const juce::File dir = directory();
            if (!dir.isDirectory())
                return;
            for (const juce::File& f : dir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kLegacyExtension))
            {
                const std::string name = f.getFileNameWithoutExtension().toStdString();
                if (tidyName(name) != name || name.empty() || f.getSize() > kMaxBytes)
                    continue;
                const juce::var root = juce::JSON::parse(f.loadFileAsString());
                if (!root.isObject() || root["format"].toString() != kLegacyFormat
                    || root["kind"].toString().toStdString() != kindName(product::kIsInstrument))
                    continue;                            // not this plugin's, or damaged: left as it is
                if (fileOf(name) == juce::File())
                {
                    Project p;
                    p.name = name;
                    Version v;
                    v.number = 1;
                    v.savedMs = f.getLastModificationTime().toMilliseconds();
                    v.note = "the preset saved " + juce::Time(v.savedMs).formatted("%Y-%m-%d %H:%M").toStdString();
                    v.files = filesOf(root["files"]);
                    v.active = std::clamp(static_cast<int>(root["active"]), 0, std::max(0, static_cast<int>(v.files.size()) - 1));
                    v.dials = dialsOf(root["dials"]);
                    if (v.files.empty())
                        continue;
                    p.versions.push_back(std::move(v));
                    if (!write(std::move(p), nullptr))
                        continue;
                }
                const juce::File aside = dir.getChildFile(kMigratedFolder);
                if (!aside.isDirectory() && !aside.createDirectory())
                    continue;
                juce::File to = aside.getChildFile(f.getFileName());
                for (int k = 2; to.exists() && k < 1000; ++k)
                    to = aside.getChildFile(f.getFileNameWithoutExtension() + " " + juce::String(k) + kLegacyExtension);
                f.moveFileTo(to);
            }
        }

        // A project read, changed and written again.
        template <typename Change>
        bool update(std::string_view name, std::string* why, Change&& change)
        {
            if (readOnly(name, why))
                return false;
            std::optional<Project> p = load(name, why);
            if (!p)
                return false;
            if (!change(*p))
                return false;
            return write(std::move(*p), why);
        }

        // Lines `a` has that `b` lacks, as many times as it has them more, whatever their order.
        int linesMore(const std::string& a, const std::string& b)
        {
            std::unordered_map<std::string_view, int> count;
            const auto each = [](const std::string& s, auto&& f) {
                std::size_t at = 0;
                while (at <= s.size())
                {
                    const std::size_t end = std::min(s.find('\n', at), s.size());
                    if (!(end == s.size() && at == s.size()))
                        f(std::string_view(s).substr(at, end - at));
                    at = end + 1;
                }
            };
            each(b, [&](std::string_view line) { ++count[line]; });
            int more = 0;
            each(a, [&](std::string_view line) {
                if (int& c = count[line]; c > 0)
                    --c;
                else
                    ++more;
            });
            return more;
        }
    }

    const Version* Project::version(int number) const
    {
        for (const Version& v : versions)
            if (v.number == number)
                return &v;
        return nullptr;
    }

    const SampleFile* Project::sample(std::string_view hash) const
    {
        for (const SampleFile& f : samples)
            if (f.hash == hash)
                return &f;
        return nullptr;
    }

    std::vector<SampleRef> refsOf(const assets::Set* samples)
    {
        std::vector<SampleRef> out;
        if (samples != nullptr)
            for (const std::shared_ptr<const assets::Asset>& a : samples->items)
                out.push_back({ a->name(), a->hash() });
        return out;
    }

    std::string samplesRefusal(const Project& p)
    {
        std::size_t total = 0;
        for (const SampleFile& f : p.samples)
            total += f.bytes.size();
        if (total <= kMaxSampleBytes)
            return {};
        return "\"" + p.name + "\"'s versions would keep " + assets::sizeText(total) + " of samples: a project keeps "
             + assets::sizeText(kMaxSampleBytes) + " at most (save it as a new project, or delete older versions)";
    }

    const Preset* Project::preset(std::string_view wanted) const
    {
        for (const Preset& p : presets)
            if (sameName(p.name, wanted))
                return &p;
        return nullptr;
    }

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
            return juce::File(juce::String::fromUTF8(dir)).getChildFile("presets");
#if defined(_WIN32)
        // Windows: Documents\Funk\<name> (a user's own files, where a DAW's are).
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(product::kCompany)
            .getChildFile(product::kName);
#else
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile("Library")
            .getChildFile("Audio")
            .getChildFile("Presets")
            .getChildFile(product::kCompany)
            .getChildFile(product::kName);
#endif
    }

    void setDirectoryForTests(const juce::File& dir)
    {
        const std::lock_guard<std::mutex> lock(overrideMutex);
        overrideDir = dir;
    }

    std::string tidyName(std::string_view name)
    {
        std::string out;
        bool space = false;
        for (const char ch : name)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                space = !out.empty();
                continue;
            }
            if (c < 0x21 || c > 0x7E || std::string_view("/\\:*?\"<>|").find(ch) != std::string_view::npos)
                continue;
            if (out.empty() && ch == '.')
                continue;
            if (space)
                out += ' ';
            space = false;
            out += ch;
        }
        if (out.size() > kMaxName)
            out.resize(kMaxName);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    bool sameName(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }

    namespace
    {
        // The folder listed (readdir and a look a file), each project's entry from the cache where it holds the file as
        // it is now. A file it does not hold so: with `unread` none, read whole here (outside the cache's lock, so a
        // listing that reads a large file holds up no other caller) and kept; else not read, but listed as the cache
        // last held it (a file never read: not listed) and its path added to `unread`. The cache then forgets the
        // files that are gone. A broken file is not listed, nor read again while it stays as it is.
        std::vector<Entry> listing(std::vector<std::string>* unread)
        {
            listed.fetch_add(1, std::memory_order_relaxed);
            const std::vector<Found> found = projectFiles(directory());
            std::vector<Entry> out;
            std::vector<const Found*> misses;
            {
                const std::lock_guard<std::mutex> lock(cacheMutex);
                std::map<std::string, Cached> kept;
                for (const Found& fd : found)
                {
                    const std::string name = fd.file.getFileNameWithoutExtension().toStdString();
                    if (tidyName(name) != name || name.empty())
                        continue;
                    // By its path as listed: the entry's name is the file's, so another spelling's would not do here.
                    const std::string path = fd.file.getFullPathName().toStdString();
                    const auto it = cache.find(path);
                    if (it != cache.end() && it->second.modified == fd.modified && it->second.size == fd.size)
                    {
                        Cached c = it->second;
                        c.id = fd.id;
                        if (!c.broken)
                            out.push_back(c.entry);
                        kept.emplace(path, std::move(c));
                    }
                    else if (unread != nullptr)
                    {
                        if (it != cache.end())
                        {
                            if (!it->second.broken)
                                out.push_back(it->second.entry);   // as it was: the Reader brings it up to date
                            kept.emplace(path, it->second);
                        }
                        unread->push_back(path);
                    }
                    else
                        misses.push_back(&fd);
                }
                cache.swap(kept);
            }
            for (const Found* fd : misses)
            {
                std::optional<Project> p = parse(fd->file, nullptr);
                {
                    const std::lock_guard<std::mutex> lock(cacheMutex);
                    keepRead(fd->file, p, fd->modified, fd->size);
                }
                if (p)
                    out.push_back(cachedOf(*p, fd->modified, fd->size, fd->id).entry);
            }
            std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
                const int c = str(a.name).compareIgnoreCase(str(b.name));
                return c != 0 ? c < 0 : a.name < b.name;
            });
            return out;
        }

        // A project's file for its presets: by its name first (one look, where a scan of the folder looks at every
        // project), else, as load() finds it, by the name in another case, once the presets from before projects are
        // projects. None, with why, when there is none.
        bool presetsFile(std::string_view name, juce::File& f, juce::int64& modified, juce::int64& size, FileId& id, std::string* why)
        {
            const std::string tidy = tidyName(name);
            f = tidy.empty() ? juce::File() : directory().getChildFile(str(tidy) + kExtension);
            if (f == juce::File() || !f.existsAsFile())
            {
                migrate();
                f = fileOf(name);
            }
            if (f == juce::File() || !look(f, modified, size, id))
            {
                fail(why, "there is no project named \"" + std::string(name) + "\"");
                return false;
            }
            return true;
        }

        // The cache's entry for that file as it is now, by its path, else by its identity (the path built from the
        // name asked spells it as asked, and a listing keys the file by its name on disk: on a volume that ignores case,
        // Shouty.FMPROJECT asked as Shouty, the same file under another spelling; its identity says so where a look
        // gives one, else its path, case aside: Windows). The caller holds cacheMutex.
        const Cached* heldLocked(const std::string& path, juce::int64 modified, juce::int64 size, const FileId& id)
        {
            if (const auto it = cache.find(path); it != cache.end() && it->second.modified == modified && it->second.size == size)
                return &it->second;
            for (const auto& [key, c] : cache)
                if (c.modified == modified && c.size == size && (id.known() ? c.id == id : sameName(key, path)))
                    return &c;
            return nullptr;
        }
    }

    std::vector<Entry> list()
    {
        migrate();
        return listing(nullptr);
    }

    std::vector<Entry> listCached(std::vector<std::string>& unread)
    {
        migrate();
        return listing(&unread);
    }

    namespace
    {
        // A factory project's presets as the listing has a project's (by name), or nothing, with why.
        std::optional<std::vector<PresetSummary>> factoryPresets(std::string_view name, std::string* why)
        {
            const Project* p = factory::find(name);
            if (p == nullptr)
            {
                fail(why, "there is no factory project named \"" + ownName(name) + "\"");
                return std::nullopt;
            }
            return cachedOf(*p, 0, 0, {}).presets;
        }
    }

    std::vector<Entry> factoryList()
    {
        std::vector<Entry> out;
        for (const Project& p : factory::projects())
            out.push_back(cachedOf(p, 0, 0, {}).entry);
        return out;
    }

    std::optional<std::vector<PresetSummary>> presetsOf(std::string_view name, std::string* why)
    {
        if (isFactoryName(name))
            return factoryPresets(name, why);
        juce::File f;
        juce::int64 modified = 0, size = 0;
        FileId id;
        if (!presetsFile(name, f, modified, size, id, why))
            return std::nullopt;
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            if (const Cached* c = heldLocked(f.getFullPathName().toStdString(), modified, size, id); c != nullptr && !c->broken)
                return c->presets;
        }
        std::optional<Project> p = parse(f, why);
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            keepRead(f, p, modified, size);
        }
        if (!p)
            return std::nullopt;
        return cachedOf(*p, modified, size, id).presets;
    }

    std::optional<std::vector<PresetSummary>> presetsCached(std::string_view name, std::vector<std::string>& unread, std::string* why)
    {
        if (isFactoryName(name))
            return factoryPresets(name, why);
        juce::File f;
        juce::int64 modified = 0, size = 0;
        FileId id;
        if (!presetsFile(name, f, modified, size, id, why))
            return std::nullopt;
        const std::string path = f.getFullPathName().toStdString();
        const std::lock_guard<std::mutex> lock(cacheMutex);
        if (const Cached* c = heldLocked(path, modified, size, id); c != nullptr)
        {
            if (!c->broken)
                return c->presets;
            fail(why, "\"" + std::string(name) + "\" is damaged, or no project of this plugin's");
            return std::nullopt;
        }
        unread.push_back(path);
        if (const auto it = cache.find(path); it != cache.end() && !it->second.broken)
            return it->second.presets;                   // as it was: the Reader brings it up to date
        return std::vector<PresetSummary>();
    }

    int parses() { return parsed.load(std::memory_order_relaxed); }
    int readerParses() { return readerParsed.load(std::memory_order_relaxed); }
    int listings() { return listed.load(std::memory_order_relaxed); }

    bool exists(std::string_view name)
    {
        if (isFactoryName(name))
            return factory::find(name) != nullptr;
        migrate();
        return fileOf(name) != juce::File();
    }

    std::optional<Project> load(std::string_view name, std::string* why)
    {
        if (isFactoryName(name))
        {
            if (const Project* p = factory::find(name))
                return *p;
            fail(why, "there is no factory project named \"" + ownName(name) + "\"");
            return std::nullopt;
        }
        migrate();
        const juce::File f = fileOf(name);
        if (f == juce::File())
        {
            fail(why, "there is no project named \"" + std::string(name) + "\"");
            return std::nullopt;
        }
        return readAndKeep(f, why);                      // its entry kept too: what PROJECTS opens, the poll then has
    }

    std::string noteLead(std::string_view words)
    {
        std::string out;
        for (const char ch : words)
        {
            const auto u = static_cast<unsigned char>(ch);
            const bool space = u <= ' ' || u == 0x7f;
            if (space)
            {
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
            }
            else
                out += ch;                               // UTF-8 kept as it is
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        if (out.size() > kMaxLead)
        {
            std::size_t cut = out.rfind(' ', kMaxLead - 3);
            if (cut == std::string::npos || cut < kMaxLead / 2)
                cut = kMaxLead - 3;
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80)
                --cut;                                   // never inside a UTF-8 character
            out = out.substr(0, cut);
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            out += "...";
        }
        return out;
    }

    std::optional<Saved> saveVersion(std::string_view name, const Sources& files, int active, const std::vector<Dial>& dials,
                                     const std::vector<Answer>& circuits, const VersionMidi& midi, const assets::Set* samples,
                                     std::string* why, std::string_view lead)
    {
        if (readOnly(name, why))
            return std::nullopt;
        const std::vector<midilearn::Mapping> maps = midilearn::tidy(midi.mappings);
        const std::vector<SampleRef> refs = refsOf(samples);
        const std::string tidy = tidyName(name);
        if (tidy.empty())
        {
            fail(why, "a project needs a name");
            return std::nullopt;
        }
        if (files.empty())
        {
            fail(why, "there is no program to save");
            return std::nullopt;
        }
        Saved saved;
        Project p;
        if (exists(tidy))
        {
            std::optional<Project> old = load(tidy, why);
            if (!old)
                return std::nullopt;
            p = std::move(*old);
            Version& last = p.versions.back();
            // Mappings a version with none left as they were make no change while the latest has none either.
            const bool sameMidi = last.midi == maps || (midi.inherited && last.midi.empty());
            if (last.files == files && last.active == active && sameDials(last.dials, dials) && sameMidi
                && sameSamples(last.samples, refs))
            {
                saved.number = last.number;
                saved.unchanged = true;
                // The same program: answers it lacks (a refine since it was saved) join it, as no version is added, and so
                // do its values' names (a version saved before labels).
                std::vector<Answer> merged = mergedAnswers(last.circuits, circuits);
                const bool more = merged != last.circuits;
                last.circuits = std::move(merged);
                const bool named = addLabels(last.dials, dials);
                if (p.name != tidy || more || named)     // the same project, named in another case now, answered, labelled
                {
                    p.name = tidy;
                    if (p.id.empty())
                        p.id = newProjectId();
                    saved.id = p.id;
                    if (!write(std::move(p), why))
                        return std::nullopt;
                }
                else
                    saved.id = p.id;
                return saved;
            }
        }
        else
            saved.created = true;
        p.name = tidy;
        Version v;
        v.number = p.versions.empty() ? 1 : p.latest().number + 1;
        v.savedMs = juce::Time::currentTimeMillis();
        v.midi = maps;
        v.samples = refs;
        v.note = describeChange(p.versions.empty() ? nullptr : &p.latest(), files, dials, &v.midi, &v.samples);
        if (const std::string l = noteLead(lead); !l.empty())
            v.note = l + " (" + v.note + ")";
        v.files = files;
        v.active = active;
        v.dials = dials;
        v.circuits = mergedAnswers({}, circuits);
        p.versions.push_back(std::move(v));
        if (p.versions.size() > static_cast<std::size_t>(kMaxVersions))
            p.versions.erase(p.versions.begin(), p.versions.end() - kMaxVersions);
        keepNamedSamples(p, samples);                    // the new version's files in, those no version names out
        if (const std::string refused = samplesRefusal(p); !refused.empty())
        {
            fail(why, refused);
            return std::nullopt;
        }
        saved.number = p.latest().number;
        if (p.id.empty())
            p.id = newProjectId();                       // made when it is first written (write() would make it too)
        saved.id = p.id;
        if (!write(std::move(p), why))
            return std::nullopt;
        return saved;
    }

    bool savePreset(std::string_view project, std::string_view preset, const std::vector<Dial>& dials, int from, std::string* why)
    {
        const std::string name = tidyName(preset);
        if (name.empty())
        {
            fail(why, "a preset needs a name");
            return false;
        }
        return update(project, why, [&](Project& p) {
            Preset pr{ name, juce::Time::currentTimeMillis(), from, dials };
            for (Preset& old : p.presets)
                if (sameName(old.name, name))
                {
                    pr.category = old.category;          // a preset saved over keeps its group
                    old = std::move(pr);
                    return true;
                }
            p.presets.push_back(std::move(pr));
            return true;
        });
    }

    bool removePreset(std::string_view project, std::string_view preset, std::string* why)
    {
        return update(project, why, [&](Project& p) {
            const auto it = std::find_if(p.presets.begin(), p.presets.end(), [&](const Preset& pr) { return sameName(pr.name, preset); });
            if (it == p.presets.end())
            {
                fail(why, "\"" + p.name + "\" has no preset named \"" + std::string(preset) + "\"");
                return false;
            }
            p.presets.erase(it);
            return true;
        });
    }

    bool setPresetCategory(std::string_view project, std::string_view preset, std::string_view category, std::string* why)
    {
        return update(project, why, [&](Project& p) {
            const auto it = std::find_if(p.presets.begin(), p.presets.end(), [&](const Preset& pr) { return sameName(pr.name, preset); });
            if (it == p.presets.end())
            {
                fail(why, "\"" + p.name + "\" has no preset named \"" + std::string(preset) + "\"");
                return false;
            }
            it->category = tidyCategory(category);
            return true;
        });
    }

    bool removeVersion(std::string_view project, int number, std::string* why)
    {
        return update(project, why, [&](Project& p) {
            const auto it = std::find_if(p.versions.begin(), p.versions.end(), [&](const Version& v) { return v.number == number; });
            if (it == p.versions.end())
            {
                fail(why, "\"" + p.name + "\" keeps no version " + std::to_string(number));
                return false;
            }
            if (p.versions.size() == 1)
            {
                fail(why, "it is the project's only version (delete the project instead)");
                return false;
            }
            p.versions.erase(it);
            keepNamedSamples(p);                         // its sample files no other version names go with it
            return true;
        });
    }

    bool remove(std::string_view name, std::string* why)
    {
        if (readOnly(name, why))
            return false;
        const juce::File f = fileOf(name);
        if (f == juce::File() || !f.deleteFile())
        {
            fail(why, "cannot delete the project \"" + std::string(name) + "\"");
            return false;
        }
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            cache.erase(f.getFullPathName().toStdString());
        }
        // Its star goes with it: a project saved later under its name starts unstarred.
        View v = view();
        const auto starred = [&](const std::string& s) { return sameName(s, name); };
        if (std::any_of(v.favourites.begin(), v.favourites.end(), starred))
        {
            v.favourites.erase(std::remove_if(v.favourites.begin(), v.favourites.end(), starred), v.favourites.end());
            (void)setView(v);
        }
        return true;
    }

    std::vector<std::string> splitTags(std::string_view commaSeparated)
    {
        std::vector<std::string> out;
        std::size_t at = 0;
        while (at <= commaSeparated.size())
        {
            const std::size_t end = std::min(commaSeparated.find(',', at), commaSeparated.size());
            out.push_back(std::string(commaSeparated.substr(at, end - at)));
            at = end + 1;
        }
        return tidyInfo({ {}, {}, {}, std::move(out), {} }).tags;
    }

    Info tidyInfo(Info info)
    {
        // Printable ASCII, the spaces collapsed and trimmed, cut to `most`.
        const auto clean = [](std::string_view s, std::size_t most) {
            std::string out;
            bool space = false;
            for (const char ch : s)
            {
                const auto c = static_cast<unsigned char>(ch);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    space = !out.empty();
                    continue;
                }
                if (space)
                    out += ' ';
                space = false;
                out += c >= 0x20 && c <= 0x7E ? ch : '?';
            }
            if (out.size() > most)
                out.resize(most);
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out;
        };
        const auto list = [&](const std::vector<std::string>& in) {
            std::vector<std::string> out;
            for (const std::string& t : in)
            {
                const std::string w = clean(t, kMaxTag);
                if (!w.empty() && static_cast<int>(out.size()) < kMaxTags
                    && std::none_of(out.begin(), out.end(), [&](const std::string& o) { return sameName(o, w); }))
                    out.push_back(w);
            }
            return out;
        };
        info.description = clean(info.description, kMaxDescription);
        info.author = clean(info.author, kMaxAuthor);
        info.category = clean(info.category, kMaxTag);
        info.tags = list(info.tags);
        info.genres = list(info.genres);
        return info;
    }

    std::string tidyCategory(std::string_view category)
    {
        Info info;
        info.category = std::string(category);
        return tidyInfo(std::move(info)).category;
    }

    bool setInfo(std::string_view project, const Info& info, std::string* why)
    {
        return update(project, why, [&](Project& p) {
            p.info = tidyInfo(info);
            return true;
        });
    }

    bool setCategory(std::string_view project, std::string_view category, std::string* why)
    {
        return update(project, why, [&](Project& p) {
            Info info = p.info;
            info.category = std::string(category);
            p.info = tidyInfo(std::move(info));
            return true;
        });
    }

    namespace
    {
        constexpr const char* kViewFormat = "funk-playground-projects-view";

        // The view's file as last read or written (its path, time and size), so view() reads it only when it changed.
        std::mutex viewMutex;
        struct ViewCache
        {
            std::string path;
            juce::int64 modified = -1, size = -1;
            View view;
        };
        ViewCache viewCache;

        juce::File viewFile() { return directory().getChildFile(kViewFile); }

        // The view as it may be kept: names tidied and each once (case aside), at most so many, the search printable.
        View tidyView(View v)
        {
            const auto unique = [](const std::vector<std::string>& in, std::size_t most, bool names) {
                std::vector<std::string> out;
                for (const std::string& s : in)
                {
                    const std::string t = names ? (isFactoryName(s) ? factoryName(tidyName(ownName(s))) : tidyName(s))
                                                : tidyInfo({ {}, {}, s, {}, {} }).category;
                    if (!t.empty() && out.size() < most && std::none_of(out.begin(), out.end(), [&](const std::string& o) { return sameName(o, t); }))
                        out.push_back(t);
                }
                return out;
            };
            v.favourites = unique(v.favourites, kMaxFavourites, true);
            std::vector<std::string> folded;
            for (const std::string& k : v.folded)        // a group's key ("category:bass"): printable, case aside once
            {
                std::string t;
                for (const char ch : k)
                    if (ch >= 0x20 && ch <= 0x7E && t.size() < kMaxTag + 16)
                        t += ch;
                if (!t.empty() && folded.size() < kMaxFolded
                    && std::none_of(folded.begin(), folded.end(), [&](const std::string& o) { return sameName(o, t); }))
                    folded.push_back(t);
            }
            v.folded = std::move(folded);
            std::string search;
            for (const char ch : v.search)
                if (search.size() < kMaxSearch)
                    search += (ch >= 0x20 && ch <= 0x7E) ? ch : ' ';
            v.search = std::move(search);
            v.tag = tidyInfo({ {}, {}, v.tag, {}, {} }).category;   // a tag as the info keeps one
            return v;
        }

        View viewOf(const juce::var& root)
        {
            View v;
            if (!root.isObject() || root["format"].toString() != kViewFormat)
                return v;
            v.favourites = stringsOf(root["favourites"]);
            v.folded = stringsOf(root["folded"]);
            v.search = root["search"].toString().toStdString();
            v.tag = root["tag"].toString().toStdString();
            v.byDate = root["sort"].toString() == "saved";
            return tidyView(std::move(v));
        }
    }

    View view()
    {
        const juce::File f = viewFile();
        const std::string path = f.getFullPathName().toStdString();
        const bool there = f.existsAsFile();
        const juce::int64 modified = there ? f.getLastModificationTime().toMilliseconds() : 0, size = there ? f.getSize() : 0;
        {
            const std::lock_guard<std::mutex> lock(viewMutex);
            if (viewCache.path == path && viewCache.modified == modified && viewCache.size == size)
                return viewCache.view;
        }
        View v;
        if (there && size < 1024 * 1024)
            v = viewOf(juce::JSON::parse(f.loadFileAsString()));
        const std::lock_guard<std::mutex> lock(viewMutex);
        viewCache = { path, modified, size, v };
        return v;
    }

    bool setView(const View& wanted, std::string* why)
    {
        const View v = tidyView(wanted);
        if (v == view())
            return true;
        const juce::File dir = directory();
        if (!dir.isDirectory() && !dir.createDirectory())
        {
            fail(why, "cannot make the folder " + dir.getFullPathName().toStdString());
            return false;
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("format", kViewFormat);
        root->setProperty("version", 1);
        root->setProperty("favourites", stringsVar(v.favourites));
        root->setProperty("folded", stringsVar(v.folded));
        root->setProperty("search", str(v.search));
        root->setProperty("tag", str(v.tag));
        root->setProperty("sort", v.byDate ? "saved" : "name");
        const juce::File target = viewFile();
        const juce::File temp = dir.getChildFile(juce::String(kViewFile) + ".saving");
        if (!temp.replaceWithText(juce::JSON::toString(juce::var(root)), false, false, "\n") || !temp.moveFileTo(target))
        {
            temp.deleteFile();
            fail(why, "cannot write " + target.getFullPathName().toStdString());
            return false;
        }
        const std::lock_guard<std::mutex> lock(viewMutex);
        viewCache = { target.getFullPathName().toStdString(), target.getLastModificationTime().toMilliseconds(), target.getSize(), v };
        return true;
    }

    std::string toJson(const Project& p)
    {
        Project q = p;
        sortPresets(q.presets);
        return (juce::JSON::toString(rootOf(q), false) + "\n").toStdString();
    }

    std::optional<Project> fromJson(const std::string& json, std::string_view name, std::string* why)
    {
        if (json.size() > static_cast<std::size_t>(kMaxBytes))
        {
            fail(why, "it is too large to be a project");
            return std::nullopt;
        }
        return parseRoot(juce::JSON::parse(str(json)), tidyName(name), "\"" + std::string(name) + "\"", why);
    }

    std::optional<std::string> add(Project p, std::string* why)
    {
        std::string base = tidyName(ownName(p.name));    // a factory project's copy: its own name
        if (base.empty())
            base = "Imported";
        if (p.versions.empty())
        {
            fail(why, "it has no program");
            return std::nullopt;
        }
        std::string name = base;
        for (int k = 2; exists(name); ++k)
        {
            const std::string suffix = " " + std::to_string(k);
            name = base.substr(0, kMaxName - suffix.size()) + suffix;
        }
        p.name = name;
        p.id = newProjectId();                           // a copy of its own: never the id it came with
        p.info = tidyInfo(std::move(p.info));
        if (!write(std::move(p), why))
            return std::nullopt;
        return name;
    }

    bool validProjectId(std::string_view id)
    {
        if (id.size() != 36)
            return false;
        for (std::size_t i = 0; i < id.size(); ++i)
        {
            const char c = id[i];
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        }
        return true;
    }

    std::string idOf(std::string_view name, std::string* why)
    {
        migrate();
        const juce::File f = fileOf(name);
        if (f == juce::File())
        {
            fail(why, "there is no project named \"" + std::string(name) + "\"");
            return {};
        }
        const std::optional<Project> p = readAndKeep(f, why);
        return p ? idOf(*p, why) : std::string();
    }

    std::string idOf(const Project& project, std::string* why)
    {
        const juce::File f = fileOf(project.name);
        if (f == juce::File())
        {
            fail(why, "there is no project named \"" + project.name + "\"");
            return {};
        }
        std::optional<Project> p = project;
        if (!p->id.empty())
        {
            // Another file with the same id: one of the two is a copy made in the Finder (an export or an import never
            // carries an id). The copy is the file whose name is not the one written in it while the other's is; when
            // both are so, or neither, the one made later.
            (void)list();                                // the cache holds every project's id
            const std::string path = f.getFullPathName().toStdString();
            const bool mine = sameName(p->writtenAs, p->name);
            bool copy = false;
            {
                const std::lock_guard<std::mutex> lock(cacheMutex);
                for (const auto& [other, c] : cache)
                {
                    if (other == path || c.broken || c.project != p->id)
                        continue;
                    const bool theirs = sameName(c.writtenAs, c.entry.name);
                    if (mine != theirs)
                        copy = copy || theirs;
                    else
                        copy = copy || f.getCreationTime() > juce::File(juce::String::fromUTF8(other.c_str())).getCreationTime();
                }
            }
            if (!copy)
                return p->id;
            p->id = newProjectId();
        }
        if (p->id.empty())
            p->id = newProjectId();                      // a project from before ids: its first
        const std::string made = p->id;
        if (!write(std::move(*p), why))
            return {};
        return made;
    }

    std::string nameOfId(std::string_view id)
    {
        if (!validProjectId(id))
            return {};
        (void)list();
        std::string found;
        const std::lock_guard<std::mutex> lock(cacheMutex);
        for (const auto& [path, c] : cache)
            if (!c.broken && c.project == id && (found.empty() || sameName(c.writtenAs, c.entry.name)))
                found = c.entry.name;
        return found;
    }

    std::string describeChange(const Version* before, const Sources& files, const std::vector<Dial>& dials,
                               const std::vector<midilearn::Mapping>* midi, const std::vector<SampleRef>* samples)
    {
        if (before == nullptr)
            return "first save";
        std::vector<std::string> parts;
        for (const SourceFile& f : files)
        {
            const auto old = std::find_if(before->files.begin(), before->files.end(), [&](const SourceFile& o) { return o.name == f.name; });
            if (old == before->files.end())
                parts.push_back(f.name + " new");
            else if (old->text != f.text)
            {
                const int added = linesMore(f.text, old->text), taken = linesMore(old->text, f.text);
                parts.push_back(f.name + " +" + std::to_string(added) + " -" + std::to_string(taken));
            }
        }
        for (const SourceFile& o : before->files)
            if (std::none_of(files.begin(), files.end(), [&](const SourceFile& f) { return f.name == o.name; }))
                parts.push_back(o.name + " gone");
        if (samples != nullptr)
        {
            for (const SampleRef& r : *samples)
            {
                const auto old = std::find_if(before->samples.begin(), before->samples.end(),
                                              [&](const SampleRef& o) { return o.name == r.name; });
                if (old == before->samples.end())
                    parts.push_back(r.name + " new");
                else if (old->hash != r.hash)
                    parts.push_back(r.name + " changed");
            }
            for (const SampleRef& o : before->samples)
                if (std::none_of(samples->begin(), samples->end(), [&](const SampleRef& r) { return r.name == o.name; }))
                    parts.push_back(o.name + " gone");
        }
        int moved = 0;
        for (const Dial& d : dials)
        {
            const auto old = std::find_if(before->dials.begin(), before->dials.end(), [&](const Dial& o) { return o.name == d.name; });
            moved += old == before->dials.end() || old->value != d.value ? 1 : 0;
        }
        std::string out;
        const std::size_t shown = std::min<std::size_t>(parts.size(), 3);
        for (std::size_t i = 0; i < shown; ++i)
            out += (i > 0 ? ", " : "") + parts[i];
        if (parts.size() > shown)
            out += ", " + std::to_string(parts.size() - shown) + " more files";
        if (moved > 0)
            out += (out.empty() ? "" : ", ") + std::to_string(moved) + (moved == 1 ? " dial" : " dials");
        if (midi != nullptr && *midi != before->midi)
            out += (out.empty() ? "" : ", ") + std::string("MIDI map");
        return out.empty() ? "the tab shown" : out;
    }
    // ---- the Reader ------------------------------------------------------------------------------------------------
    struct Reader::State
    {
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::string> queue;                   // the paths asked, in order
        std::set<std::string> asked;                     // queued or being read: each asked once until read
        bool stop = false;
        std::atomic<uint64_t> revision{ 0 };
        std::thread thread;

        void run()
        {
            for (;;)
            {
                std::string path;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [this] { return stop || !queue.empty(); });
                    if (stop)
                        return;
                    path = std::move(queue.front());
                    queue.pop_front();
                }
                read(path);
                const std::lock_guard<std::mutex> lock(mutex);
                asked.erase(path);
            }
        }

        // The file read whole into the cache, unless the cache holds it as it is already (another caller read it since
        // it was asked for) or it is gone. A read the cache takes moves the revision under the cache's lock, with it:
        // whoever finds the read in the cache finds the revision moved too (Panel::syncExplorer polls again when it
        // moved while it polled, so its lists never straddle a read).
        void read(const std::string& path)
        {
            const juce::File f(juce::String::fromUTF8(path.c_str()));
            juce::int64 modified = 0, size = 0;
            FileId id;
            if (!look(f, modified, size, id))
                return;
            {
                const std::lock_guard<std::mutex> lock(cacheMutex);
                if (const auto it = cache.find(path); it != cache.end() && it->second.modified == modified && it->second.size == size)
                    return;
            }
            readerParsed.fetch_add(1, std::memory_order_relaxed);
            const std::optional<Project> p = parse(f, nullptr);
            const std::lock_guard<std::mutex> lock(cacheMutex);
            keepRead(f, p, modified, size);
            revision.fetch_add(1, std::memory_order_release);
        }
    };

    Reader::Reader() : s_(std::make_unique<State>())
    {
        s_->thread = std::thread([s = s_.get()] { s->run(); });
    }

    Reader::~Reader()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->mutex);
            s_->stop = true;
        }
        s_->wake.notify_all();
        s_->thread.join();
    }

    void Reader::request(const std::vector<std::string>& paths)
    {
        bool more = false;
        {
            const std::lock_guard<std::mutex> lock(s_->mutex);
            for (const std::string& p : paths)
                if (s_->asked.insert(p).second)
                {
                    s_->queue.push_back(p);
                    more = true;
                }
        }
        if (more)
            s_->wake.notify_one();
    }

    uint64_t Reader::revision() const noexcept { return s_->revision.load(std::memory_order_acquire); }

    bool Reader::idle() const
    {
        const std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->asked.empty();
    }

    std::shared_ptr<Reader> reader()
    {
        const std::lock_guard<std::mutex> lock(readerMutex);
        std::shared_ptr<Reader> r = theReader.lock();
        if (!r)
        {
            r = std::make_shared<Reader>();
            theReader = r;
        }
        return r;
    }
}
