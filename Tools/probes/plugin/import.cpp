// PROBE name=plugin.import timeout=60
//
// plugin.import: write_file's files in and out (plugin/ChatImport.cpp) through the agent (ChatAgent::mcpCall) over a
// real processor, in folders of the probe's own:
//   FILES   files: two files written as one change of the draft (one new, one replaced; each line saying which, and a
//           version each); a list with a name the editor cannot take, a name twice, or past the editor's 16 files:
//           nothing written; files beside name and content refused
//   FROM    a folder the user named: its program files brought in as one change (program.fm replaced, a new .fm and a
//           .gui added; CR LF read as LF, and said; a character outside ASCII made '?', and said), what is no program
//           file left out and named, the folder sync's read-only copies left out (host.fm as the plugin's, a library
//           file as the library's, another's copy); one file by its path; ~ for the home folder; nothing brought in,
//           all or none, when one file is not text (a NUL byte, bytes that are not UTF-8), is past 256 KB, or has a
//           name the editor cannot take; a relative path, a file that is no program file, a path that is nowhere, and
//           a folder of no program file refused; the folder's files never changed; a folder with a panel's shader and
//           the .sh file it includes: all four of its files brought in (the editor's kinds, Sources.h's isProgramFile)
//   TO      the editor's files written to a new folder of the name given in the plugin's Programs folder (the probe's
//           sandbox under CTest), byte for byte, the path in the result; the same name again: "<name> 2", the first
//           untouched; a name that is a path, or starts with '.', refused; two files whose names differ only in case
//           (one file on this Mac's disk): refused, no folder made; nothing in the plugin changed
//   SAMPLES a WAV by its path added as a sample; path beside content refused; an audio file's name with content
//           refused, naming path; from: a folder's audio file left out, said with path, and one named alone refused;
//           to: the program's samples exported beside its files, byte for byte, and said
#include "Probe.h"

#include "plugin/ChatAgent.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : fields)
            o->setProperty(k, v);
        return juce::var(o);
    }

    std::string resultOf(const pg::ChatAgent::McpResult& r)
    {
        const juce::Array<juce::var>* c = r.content.getArray();
        return c != nullptr && !c->isEmpty() ? c->getLast()["text"].toString().toStdString() : std::string();
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    std::string program(const std::string& note)
    {
        if constexpr (kInstrument)
            return "// " + note + "\nmodule Voice\n{\n    void start(float note, float velocity) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// " + note + "\nmodule Effect\n{\n    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    const pg::SourceFile* fileOf(const pg::Draft& d, const std::string& name) { return pg::findFile(d.files, name); }

    void write(const juce::File& f, const std::string& bytes) { f.replaceWithData(bytes.data(), bytes.size()); }

    // A mono 16-bit WAV at 48 kHz of `frames` frames of a ramp: a sample's file.
    std::string wav16(int frames)
    {
        const auto le = [](std::string& s, uint32_t v, int bytes) {
            for (int i = 0; i < bytes; ++i)
                s += static_cast<char>((v >> (8 * i)) & 0xFF);
        };
        std::string s = "RIFF";
        le(s, static_cast<uint32_t>(36 + frames * 2), 4);
        s += "WAVEfmt ";
        le(s, 16, 4);
        le(s, 1, 2);                                   // PCM
        le(s, 1, 2);                                   // mono
        le(s, 48000, 4);
        le(s, 96000, 4);
        le(s, 2, 2);
        le(s, 16, 2);
        s += "data";
        le(s, static_cast<uint32_t>(frames * 2), 4);
        for (int i = 0; i < frames; ++i)
            le(s, static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(i * 50 - 12000))), 2);
        return s;
    }
}

PROBE(plugin, import)
{
    const funkgui::HeadlessGuiScope gui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile(juce::String(pg::product::kEnvPrefix) + "import-" + juce::String(getpid()));
    tmp.deleteRecursively();
    tmp.createDirectory();
    pg::projectstore::setDirectoryForTests(tmp.getChildFile("projects"));

    auto owned = std::make_unique<pg::Processor>();
    pg::Processor& p = *owned;
    p.setRateAndBufferSizeDetails(48000.0, 256);
    p.prepareToPlay(48000.0, 256);
    pg::ChatAgent& agent = p.chatAgent();
    const auto call = [&](juce::var args, bool* error = nullptr) {
        const pg::ChatAgent::McpResult r = agent.mcpCall("write_file", args);
        if (error != nullptr)
            *error = r.isError;
        const std::string t = resultOf(r);
        std::printf("write_file:\n  %s\n", t.c_str());
        return t;
    };
    const auto reset = [&] {
        pg::Draft d;
        d.files = { { pg::kProgramName, program("before") }, { "keep.fm", "// kept\n" } };
        p.editorPort().setDraft(d);
    };
    const auto files = [](std::initializer_list<std::pair<std::string, std::string>> fs) {
        juce::Array<juce::var> a;
        for (const auto& [n, c] : fs)
            a.add(object({ { "name", juce::String(n) }, { "content", juce::String(c) } }));
        return juce::var(a);
    };

    // ---- FILES --------------------------------------------------------------------------------------------------------
    {
        reset();
        const uint64_t before = p.editorPort().draftRevision();
        bool error = true;
        std::string t = call(object({ { "files", files({ { "keep.fm", "// kept, written\n" }, { "more.fm", "// more\n// two\n" } }) } }), &error);
        const pg::Draft d = p.editorPort().draft();
        P.eq("files.written", !error && fileOf(d, "keep.fm") != nullptr && fileOf(d, "keep.fm")->text == "// kept, written\n"
                                  && fileOf(d, "more.fm") != nullptr && d.files.size() == 3 ? 1 : 0, 1);
        P.eq("files.one_change", static_cast<int64_t>(p.editorPort().draftRevision() - before), int64_t{ 1 });
        P.eq("files.words", has(t, "Wrote 2 files as one change") && has(t, "keep.fm (1 line; replaced)") && has(t, "more.fm (2 lines; new)")
                                && has(t, "Versions: keep.fm ") ? 1 : 0, 1);
        const auto refused = [&](const char* row, juce::var args, const std::string& words) {
            reset();
            bool e = false;
            const std::string r = call(args, &e);
            const pg::Draft now = p.editorPort().draft();
            P.eq(row, e && has(r, words) && now.files.size() == 2 && fileOf(now, "keep.fm")->text == "// kept\n" ? 1 : 0, 1);
        };
        refused("files.bad_name", object({ { "files", files({ { "keep.fm", "x\n" }, { "my song.fm", "y\n" } }) } }), "cannot name a file");
        refused("files.twice", object({ { "files", files({ { "keep.fm", "x\n" }, { "keep.fm", "y\n" } }) } }), "comes twice");
        {
            juce::Array<juce::var> many;
            for (int k = 0; k < 15; ++k)
                many.add(object({ { "name", juce::String("f" + std::to_string(k) + ".fm") }, { "content", "x\n" } }));
            refused("files.past_room", object({ { "files", juce::var(many) } }), "past its 16");
        }
        refused("files.with_name", object({ { "name", "keep.fm" }, { "files", files({ { "keep.fm", "x\n" } }) } }), "takes one of");
    }

    // ---- FROM ---------------------------------------------------------------------------------------------------------
    {
        const juce::File src = tmp.getChildFile("song");
        src.createDirectory();
        write(src.getChildFile("program.fm"), program("imported"));
        write(src.getChildFile("filters.fm"), "// filters\r\nfloat lp(float x) { return x; }\r\n");
        write(src.getChildFile("panel.gui"), "panel {\n    row { }\n}\n// caf\xc3\xa9\n");
        write(src.getChildFile("README.md"), "# a song\n");
        const pg::LibraryFile& lib = pg::kLibrary[0];
        write(src.getChildFile(lib.name), lib.source);
        write(src.getChildFile("host.fm"), std::string(pg::Patch::hostSource()));
        write(src.getChildFile("other.fm"), "// another build's copy\n");
        write(src.getChildFile(".fmsync"), "fmsync 1\ncopy 0123456789abcdef " + std::string(lib.name) + "\ncopy 0123456789abcdef other.fm\n");
        reset();
        bool error = true;
        std::string t = call(object({ { "from", src.getFullPathName() } }), &error);
        pg::Draft d = p.editorPort().draft();
        P.eq("from.folder", !error && fileOf(d, "program.fm")->text == program("imported") && fileOf(d, "keep.fm")->text == "// kept\n"
                                && fileOf(d, "filters.fm") != nullptr && fileOf(d, "panel.gui") != nullptr && d.files.size() == 4 ? 1 : 0, 1);
        P.eq("from.crlf", fileOf(d, "filters.fm") != nullptr && fileOf(d, "filters.fm")->text == "// filters\nfloat lp(float x) { return x; }\n"
                              && has(t, "filters.fm (2 lines; new; CR LF line ends read as LF)") ? 1 : 0, 1);
        P.eq("from.ascii", fileOf(d, "panel.gui") != nullptr && has(fileOf(d, "panel.gui")->text, "// caf?\n")
                               && has(t, "1 character outside ASCII became '?'") ? 1 : 0, 1);
        P.eq("from.words", has(t, "Brought 3 files into the editor from " + src.getFullPathName().toStdString()) && has(t, "program.fm (")
                               && has(t, "; replaced") && has(t, "filters.fm (2 lines; new") ? 1 : 0, 1);
        P.eq("from.left_out", has(t, "README.md") && has(t, "host.fm (the plugin's own)") && has(t, std::string(lib.name) + " (the plugin's own)")
                                  && has(t, "other.fm (the folder sync's read-only copy)") ? 1 : 0, 1);
        P.eq("from.untouched", src.getChildFile("filters.fm").loadFileAsString().contains("\r\n") ? 1 : 0, 1);

        reset();
        t = call(object({ { "from", src.getChildFile("filters.fm").getFullPathName() } }), &error);
        d = p.editorPort().draft();
        P.eq("from.one_file", !error && d.files.size() == 3 && fileOf(d, "filters.fm") != nullptr && has(t, "Brought 1 file") ? 1 : 0, 1);

        const juce::File home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
        if (src.isAChildOf(home))
        {
            reset();
            t = call(object({ { "from", "~/" + src.getChildFile("filters.fm").getRelativePathFrom(home) } }), &error);
            P.eq("from.home", !error && fileOf(p.editorPort().draft(), "filters.fm") != nullptr ? 1 : 0, 1);
        }

        // A panel's shader and the .sh file it includes come in with the program.
        {
            const juce::File glow = tmp.getChildFile("glow-synth");
            glow.createDirectory();
            write(glow.getChildFile("program.fm"), program("glowing"));
            write(glow.getChildFile("panel.gui"), "shader \"glow.sc\"\n");
            write(glow.getChildFile("glow.sc"), "#include \"glow.sh\"\nvoid main() { gl_FragColor = vec4(glow(), 1.0); }\n");
            write(glow.getChildFile("glow.sh"), "vec3 glow() { return vec3(1.0, 0.5, 0.1); }\n");
            reset();
            t = call(object({ { "from", glow.getFullPathName() } }), &error);
            d = p.editorPort().draft();
            std::printf("from a shader's folder: %s\n", t.c_str());
            P.eq("from.shader_include", !error && fileOf(d, "glow.sh") != nullptr && fileOf(d, "glow.sc") != nullptr
                                            && fileOf(d, "panel.gui") != nullptr && d.files.size() == 5 && has(t, "Brought 4 files")
                                            && !has(t, "Only program files") ? 1 : 0, 1);
        }

        const auto refused = [&](const char* row, const juce::String& from, const std::string& words) {
            reset();
            bool e = false;
            const std::string r = call(object({ { "from", from } }), &e);
            const pg::Draft now = p.editorPort().draft();
            P.eq(row, e && has(r, words) && now.files.size() == 2 && fileOf(now, pg::kProgramName)->text == program("before") ? 1 : 0, 1);
        };
        write(src.getChildFile("binary.fm"), std::string("// x\0y\n", 7));
        refused("from.not_text", src.getFullPathName(), "binary.fm is not text");
        src.getChildFile("binary.fm").deleteFile();
        write(src.getChildFile("latin.fm"), "// caf\xe9\n");
        refused("from.not_utf8", src.getFullPathName(), "latin.fm is not text");
        src.getChildFile("latin.fm").deleteFile();
        write(src.getChildFile("large.fm"), std::string(300 * 1024, '/'));
        refused("from.too_large", src.getFullPathName(), "large.fm is 300 KB, past the 256 KB");
        src.getChildFile("large.fm").deleteFile();
        write(src.getChildFile("my song.fm"), "// x\n");
        refused("from.bad_name", src.getFullPathName(), "'my song.fm' cannot name a file in the editor");
        src.getChildFile("my song.fm").deleteFile();
        refused("from.relative", "song/program.fm", "whole (/Users/... or ~/...)");
        refused("from.not_program", src.getChildFile("README.md").getFullPathName(), "README.md is not a program file");
        refused("from.nowhere", src.getChildFile("missing").getFullPathName(), "there is no file or folder");
        const juce::File empty = tmp.getChildFile("empty");
        empty.createDirectory();
        write(empty.getChildFile("notes.txt"), "x\n");
        refused("from.no_program_file", empty.getFullPathName(), "holds no program file");
    }

    // ---- TO -----------------------------------------------------------------------------------------------------------
    {
        reset();
        const pg::Draft d = p.editorPort().draft();
        const uint64_t before = p.editorPort().draftRevision();
        const char* sandbox = std::getenv((std::string(pg::product::kEnvPrefix) + "PREFS_DIR").c_str());
        const juce::File programs = sandbox != nullptr && *sandbox != '\0' ? juce::File(juce::String::fromUTF8(sandbox)).getChildFile("programs")
                                                                           : juce::File();
        if (programs != juce::File())
            programs.deleteRecursively();
        bool error = true;
        std::string t = call(object({ { "to", "My Song" } }), &error);
        const juce::File first = programs.getChildFile("My Song");
        P.eq("to.under_ctest", programs != juce::File() ? 1 : 0, 1);
        P.eq("to.written", !error && first.getChildFile("program.fm").loadFileAsString().toStdString() == d.files[0].text
                               && first.getChildFile("keep.fm").loadFileAsString().toStdString() == "// kept\n" ? 1 : 0, 1);
        P.eq("to.words", has(t, "Exported the editor's 2 files") && has(t, first.getFullPathName().toStdString()) ? 1 : 0, 1);
        P.eq("to.nothing_changed", p.editorPort().draftRevision() == before ? 1 : 0, 1);
        t = call(object({ { "to", "My Song" } }), &error);
        P.eq("to.second", !error && programs.getChildFile("My Song 2").getChildFile("keep.fm").existsAsFile()
                              && has(t, "\"My Song\" was taken, and nothing is replaced") ? 1 : 0, 1);
        t = call(object({ { "to", "../outside" } }), &error);
        P.eq("to.path_refused", error && has(t, "a name, not a path") && !programs.getChildFile("../outside").exists() ? 1 : 0, 1);
        t = call(object({ { "to", ".hidden" } }), &error);
        P.eq("to.dot_refused", error ? 1 : 0, 1);
        pg::Draft cased = d;
        cased.files.push_back({ "filters.fm", "// one\n" });
        cased.files.push_back({ "Filters.fm", "// two\n" });
        p.editorPort().setDraft(cased);
        t = call(object({ { "to", "Cased" } }), &error);
        P.eq("to.case_refused", error && has(t, "filters.fm and Filters.fm differ only in case") && has(t, "Nothing was exported")
                                    && !programs.getChildFile("Cased").exists() ? 1 : 0, 1);
        if (programs != juce::File())
            programs.deleteRecursively();
    }

    // ---- SAMPLES ------------------------------------------------------------------------------------------------------
    {
        reset();
        const juce::File song = tmp.getChildFile("sampled");
        song.createDirectory();
        write(song.getChildFile(pg::kProgramName), program("sampled"));
        const juce::File kick = song.getChildFile("kick.wav");
        write(kick, wav16(480));
        bool error = true;
        std::string t = call(object({ { "path", kick.getFullPathName() } }), &error);
        const std::vector<pg::AssetEntry> held = p.editorPort().assets();
        P.eq("samples.added", !error && has(t, "Added the sample kick.wav") && held.size() == 1 && held[0].name == "kick.wav" ? 1 : 0, 1);
        t = call(object({ { "path", kick.getFullPathName() }, { "content", "x" } }), &error);
        P.eq("samples.path_and_content", error && has(t, "write_file takes one of") ? 1 : 0, 1);
        t = call(object({ { "name", "snare.wav" }, { "content", "x" } }), &error);
        P.eq("samples.audio_name_refused", error && has(t, "snare.wav names an audio file: give its path") ? 1 : 0, 1);
        t = call(object({ { "from", song.getFullPathName() } }), &error);
        P.eq("samples.from_left_out", !error && has(t, "kick.wav (a sample: write_file's path adds it)") ? 1 : 0, 1);
        t = call(object({ { "from", kick.getFullPathName() } }), &error);
        P.eq("samples.from_file_refused", error && has(t, "kick.wav is an audio file: write_file's path adds it as a sample") ? 1 : 0, 1);
        const char* sandbox = std::getenv((std::string(pg::product::kEnvPrefix) + "PREFS_DIR").c_str());
        const juce::File programs = sandbox != nullptr && *sandbox != '\0' ? juce::File(juce::String::fromUTF8(sandbox)).getChildFile("programs")
                                                                           : juce::File();
        t = call(object({ { "to", "With Samples" } }), &error);
        juce::MemoryBlock out;
        const bool read = programs != juce::File() && programs.getChildFile("With Samples").getChildFile("kick.wav").loadFileAsData(out);
        P.eq("samples.exported", !error && read && out.getSize() == static_cast<std::size_t>(kick.getSize())
                                     && std::memcmp(out.getData(), wav16(480).data(), out.getSize()) == 0
                                     && has(t, "and the program's 1 sample") && has(t, "kick.wav (a sample, ") ? 1 : 0, 1);
        if (programs != juce::File())
            programs.deleteRecursively();
    }

    owned.reset();
    tmp.deleteRecursively();
    return P.finish();
}
