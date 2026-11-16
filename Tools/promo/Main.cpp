// Tools/promo/Main.cpp: <Name>Promo, the promotional videos' renderer (scripts/promo.py drives it; README.md
// "Promotional videos"). It plays one of the user's projects offline in a real Processor, from a storyboard of notes and
// dial moves, and draws the editor headless on every video frame, as Claude's view_panel draws it (editor/Picture.h:
// the GPU, shaders as on screen, else SoftRaster): the PANEL view's interface and the CODE view's program, each cropped
// and drawn at the scale that makes it the video's width. Sound and picture come from one loop, so they cannot drift:
// each frame plays its 1/fps of audio through the processor, ticks both Panels on the simulated clock, and draws them.
//
//   <exe> --project-file <p.fmproject> --info                    the project's dials, pages and files, as JSON
//   <exe> --project-file <p.fmproject> --storyboard <s.json> --out <dir> --sizes   the views' sizes in px, as JSON
//   <exe> --project-file <p.fmproject> --storyboard <s.json> --out <dir> [--stills <t,t,...>] [options]
//
// Options: --width (the views' width in px, 1080), --fps (60), --theme (0, GRAPHITE), --ffmpeg, and --code-shown <px>:
// how much of the code view the video shows (from its top; scripts/promo.py's layout), so that its scroll ends with the
// file's last line at the bottom of what is shown.
//
// It never touches the user's folders: the project file is copied into a sandbox of its own (<out>/sandbox: the
// preferences, the projects, the chat's key folder and the MCP sockets' folder all point there, so no Claude Desktop
// sees it), and only <out> is written. Its outputs: audio.wav (32-bit float, stereo), panel.mp4 and code.mp4 (each
// frame of the video, H.264 4:4:4 near-lossless, through ffmpeg on stdin) and meta.json; with --stills, PNGs at those
// times instead of the videos.
//
// The storyboard (JSON; keys it does not know are the compositor's):
//   seconds   the length (default 20); tempo (BPM, default 120: the host's transport, playing from beat 0)
//   preset    a preset of the project to apply; version: a version number (default the newest)
//   page      the interface's page to show (a tabs element's page title)
//   notes     [[beat, note, beats, velocity], ...] (velocity 0..1; beats long)
//   repeat    {"beats": n, "times": k}: the notes again every n beats, k times in all
//   midi      a .mid file (beside the storyboard) instead of notes, its tempo map ignored: its ticks as beats
//   dials     [{"name": "cutoff", "beat": 8, "beats": 8, "to": 0.8}] each a glide from where the dial is to `to`
//             (normalised 0..1, the knob's travel) or `value` (in the dial's own units); smoothstep-shaped
//   code      {"file": "program.fm", "line": 1, "speed": 2.5, "width": 760, "height": 1400}: the CODE view's file,
//             its first line shown, the lines a second it scrolls, the logical width of the editor shown (its left
//             part) and the CODE view's window's height (the editor's room)
//   window    [w, h]: the PANEL view's editor size in logical px (default 1360 x 760), made taller until none of the
//             interface scrolls out of sight
#include "editor/DialLayout.h"
#include "editor/DisplayItem.h"
#include "editor/Panel.h"
#include "editor/Picture.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pg = playground;
namespace fs = std::filesystem;

namespace
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    [[noreturn]] void fail(const std::string& why)
    {
        std::fprintf(stderr, "promo: %s\n", why.c_str());
        std::exit(1);
    }

    std::string argValue(int argc, char** argv, const char* flag, const char* fallback = "")
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], flag) == 0)
                return argv[i + 1];
        return fallback;
    }

    bool hasFlag(int argc, char** argv, const char* flag)
    {
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], flag) == 0)
                return true;
        return false;
    }

    juce::var readJson(const fs::path& path)
    {
        const juce::File f(juce::String::fromUTF8(path.string().c_str()));
        if (!f.existsAsFile())
            fail("no file " + path.string());
        juce::var v;
        const juce::Result r = juce::JSON::parse(f.loadFileAsString(), v);
        if (r.failed())
            fail(path.string() + ": " + r.getErrorMessage().toStdString());
        return v;
    }

    double num(const juce::var& v, const char* key, double fallback)
    {
        const juce::var x = v.getProperty(key, juce::var());
        return x.isDouble() || x.isInt() || x.isInt64() ? static_cast<double>(x) : fallback;
    }

    std::string str(const juce::var& v, const char* key, const std::string& fallback = {})
    {
        const juce::var x = v.getProperty(key, juce::var());
        return x.isString() ? x.toString().toStdString() : fallback;
    }

    // ---- the transport the programs read (host.fm): playing from beat 0 at the storyboard's tempo ----------------
    class Head final : public juce::AudioPlayHead
    {
    public:
        explicit Head(double bpm) : bpm_(bpm) {}
        void at(int64_t sample) noexcept { sample_ = sample; }
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setBpm(bpm_);
            p.setTimeSignature(TimeSignature{ 4, 4 });
            p.setTimeInSamples(sample_);
            p.setTimeInSeconds(static_cast<double>(sample_) / kRate);
            const double ppq = static_cast<double>(sample_) / kRate * bpm_ / 60.0;
            p.setPpqPosition(ppq);
            p.setPpqPositionOfLastBarStart(std::floor(ppq / 4.0) * 4.0);
            p.setIsPlaying(true);
            return p;
        }

    private:
        double bpm_;
        int64_t sample_ = 0;
    };

    // ---- the storyboard, in samples -------------------------------------------------------------------------------
    struct NoteEvent
    {
        int64_t at = 0;
        int note = 60;
        float velocity = 0.0f;                   // 0: a note-off
    };

    struct DialMove
    {
        int slot = -1;
        int64_t from = 0, to = 0;                // samples
        float target = 0.0f;                     // normalised
        float start = -1.0f;                     // where the dial was when the move began (set as it begins)
        bool done = false;                       // reached its target, the gesture ended
    };

    struct Storyboard
    {
        double seconds = 20.0;
        double tempo = 120.0;
        std::string preset, page;
        int version = 0;
        std::vector<NoteEvent> events;           // sorted by time; offs before ons at one time
        std::vector<DialMove> dials;             // slots resolved once the program plays
        juce::var dialSpecs;
        std::string codeFile = pg::kProgramName;
        double codeLine = 1.0, codeSpeed = 2.5, codeWidth = 760.0;
        int codeHeight = 1400;                   // the CODE view's window's height: the editor's room
        int windowW = pg::ui::kDefaultWidth, windowH = pg::ui::kDefaultHeight;
    };

    int64_t beatsToSamples(double beats, double tempo)
    {
        return static_cast<int64_t>(std::llround(beats * 60.0 / tempo * kRate));
    }

    void addNote(Storyboard& s, double beat, int note, double beats, double velocity)
    {
        if (note < 0 || note > 127 || !(beats > 0.0))
            return;
        const float v = static_cast<float>(std::clamp(velocity, 0.01, 1.0));
        s.events.push_back({ beatsToSamples(beat, s.tempo), note, v });
        s.events.push_back({ beatsToSamples(beat + beats, s.tempo), note, 0.0f });
    }

    Storyboard readStoryboard(const fs::path& path)
    {
        Storyboard s;
        const juce::var v = readJson(path);
        s.seconds = num(v, "seconds", 20.0);
        s.tempo = std::clamp(num(v, "tempo", 120.0), 20.0, 400.0);
        s.preset = str(v, "preset");
        s.page = str(v, "page");
        s.version = static_cast<int>(num(v, "version", 0.0));
        if (const juce::var w = v["window"]; w.getArray() != nullptr && w.size() == 2)
        {
            s.windowW = std::max(static_cast<int>(w[0]), pg::ui::kMinWidth);
            s.windowH = std::max(static_cast<int>(w[1]), pg::ui::kMinHeight);
        }

        std::vector<NoteEvent> pattern;
        if (const std::string midi = str(v, "midi"); !midi.empty())
        {
            const fs::path mp = path.parent_path() / midi;
            juce::FileInputStream in(juce::File(juce::String::fromUTF8(mp.string().c_str())));
            juce::MidiFile mf;
            if (!in.openedOk() || !mf.readFrom(in))
                fail("cannot read the MIDI file " + mp.string());
            const double tpq = mf.getTimeFormat() > 0 ? static_cast<double>(mf.getTimeFormat()) : 960.0;
            for (int t = 0; t < mf.getNumTracks(); ++t)
            {
                const juce::MidiMessageSequence* seq = mf.getTrack(t);
                for (int i = 0; i < seq->getNumEvents(); ++i)
                {
                    const juce::MidiMessage& m = seq->getEventPointer(i)->message;
                    const double beat = m.getTimeStamp() / tpq;
                    if (m.isNoteOn())
                        s.events.push_back({ beatsToSamples(beat, s.tempo), m.getNoteNumber(), m.getFloatVelocity() });
                    else if (m.isNoteOff())
                        s.events.push_back({ beatsToSamples(beat, s.tempo), m.getNoteNumber(), 0.0f });
                }
            }
        }
        else if (const juce::Array<juce::var>* notes = v["notes"].getArray(); notes != nullptr)
        {
            const juce::var rep = v["repeat"];
            const double every = num(rep, "beats", 0.0);
            const int times = every > 0.0 ? std::max(1, static_cast<int>(num(rep, "times", 1.0))) : 1;
            for (int k = 0; k < times; ++k)
                for (const juce::var& n : *notes)
                {
                    if (n.getArray() == nullptr || n.size() < 2)
                        continue;
                    addNote(s, static_cast<double>(n[0]) + every * k, static_cast<int>(n[1]),
                            n.size() > 2 ? static_cast<double>(n[2]) : 0.5, n.size() > 3 ? static_cast<double>(n[3]) : 0.8);
                }
        }
        std::stable_sort(s.events.begin(), s.events.end(), [](const NoteEvent& a, const NoteEvent& b) {
            return a.at != b.at ? a.at < b.at : (a.velocity == 0.0f && b.velocity > 0.0f);
        });
        s.dialSpecs = v["dials"];

        const juce::var code = v["code"];
        s.codeFile = str(code, "file", s.codeFile);
        s.codeLine = std::max(1.0, num(code, "line", 1.0));
        s.codeSpeed = num(code, "speed", 2.5);
        s.codeWidth = std::max(200.0, num(code, "width", 760.0));
        s.codeHeight = std::max(pg::ui::kMinHeight, static_cast<int>(num(code, "height", 1400.0)));
        return s;
    }

    // The storyboard's dials, by the program's names, once it plays.
    void resolveDials(Storyboard& s, const pg::Controls& cs)
    {
        const juce::Array<juce::var>* specs = s.dialSpecs.getArray();
        if (specs == nullptr)
            return;
        for (const juce::var& d : *specs)
        {
            const std::string name = str(d, "name");
            int slot = -1;
            for (int c = 0; c < pg::kMaxControls; ++c)
                if (cs[static_cast<std::size_t>(c)].used && juce::String(cs[static_cast<std::size_t>(c)].name).equalsIgnoreCase(juce::String::fromUTF8(name.c_str())))
                    slot = c;
            if (slot < 0)
            {
                std::fprintf(stderr, "promo: warning: no dial \"%s\" (skipped)\n", name.c_str());
                continue;
            }
            const pg::Control& c = cs[static_cast<std::size_t>(slot)];
            float target = 0.0f;
            if (d.hasProperty("value"))
                target = pg::normOf(c, static_cast<float>(static_cast<double>(d["value"])));
            else
                target = static_cast<float>(std::clamp(num(d, "to", 0.5), 0.0, 1.0));
            const double beat = num(d, "beat", 0.0);
            const double beats = std::max(0.0, num(d, "beats", 1.0));
            s.dials.push_back({ slot, beatsToSamples(beat, s.tempo), beatsToSamples(beat + beats, s.tempo), target, -1.0f, false });
        }
    }

    // ---- files ------------------------------------------------------------------------------------------------------
    void writeWav(const fs::path& path, const std::vector<float>& left, const std::vector<float>& right)
    {
        std::ofstream out(path, std::ios::binary);
        const auto u32 = [&](uint32_t x) { out.write(reinterpret_cast<const char*>(&x), 4); };
        const auto u16 = [&](uint16_t x) { out.write(reinterpret_cast<const char*>(&x), 2); };
        const uint32_t frames = static_cast<uint32_t>(left.size());
        const uint32_t bytes = frames * 2u * 4u;
        out.write("RIFF", 4);
        u32(36u + bytes);
        out.write("WAVEfmt ", 8);
        u32(16u);
        u16(3u);                                 // IEEE float
        u16(2u);
        u32(static_cast<uint32_t>(kRate));
        u32(static_cast<uint32_t>(kRate) * 8u);
        u16(8u);
        u16(32u);
        out.write("data", 4);
        u32(bytes);
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            out.write(reinterpret_cast<const char*>(&left[i]), 4);
            out.write(reinterpret_cast<const char*>(&right[i]), 4);
        }
        if (!out)
            fail("cannot write " + path.string());
    }

    void writeText(const fs::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary);
        out << text;
        if (!out)
            fail("cannot write " + path.string());
    }

    // A crop of a drawn frame, in physical px, its size even (H.264's chroma wants it).
    struct Crop
    {
        int x = 0, y = 0, w = 0, h = 0;
    };

    Crop cropOf(const funkgui::Rect& r, float dpi, const funkgui::Image& img)
    {
        Crop c;
        c.x = std::clamp(static_cast<int>(std::lround(r.x * dpi)), 0, std::max(0, img.w - 2));
        c.y = std::clamp(static_cast<int>(std::lround(r.y * dpi)), 0, std::max(0, img.h - 2));
        c.w = std::clamp(static_cast<int>(std::lround(r.w * dpi)), 2, img.w - c.x) & ~1;
        c.h = std::clamp(static_cast<int>(std::lround(r.h * dpi)), 2, img.h - c.y) & ~1;
        return c;
    }

    // An ffmpeg that takes raw RGBA frames on its stdin and writes a near-lossless H.264 4:4:4 file.
    class Encoder
    {
    public:
        Encoder(const std::string& ffmpeg, const fs::path& out, int w, int h, int fps) : w_(w), h_(h)
        {
            const std::string cmd = "'" + ffmpeg + "' -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s "
                                  + std::to_string(w) + "x" + std::to_string(h) + " -r " + std::to_string(fps)
                                  + " -i - -c:v libx264 -preset veryfast -crf 8 -pix_fmt yuv444p '" + out.string() + "'";
            pipe_ = ::popen(cmd.c_str(), "w");
            if (pipe_ == nullptr)
                fail("cannot start ffmpeg: " + cmd);
            row_.resize(static_cast<std::size_t>(w) * 4u);
        }
        ~Encoder() { close(); }
        Encoder(const Encoder&) = delete;
        Encoder& operator=(const Encoder&) = delete;

        void frame(const funkgui::Image& img, const Crop& c)
        {
            for (int y = 0; y < h_; ++y)
            {
                const int sy = std::min(c.y + y, img.h - 1);
                const uint8_t* src = img.rgba.data() + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(img.w)
                                                       + static_cast<std::size_t>(c.x)) * 4u;
                const int n = std::min(w_, img.w - c.x);
                std::memcpy(row_.data(), src, static_cast<std::size_t>(n) * 4u);
                for (std::size_t i = 3; i < row_.size(); i += 4)
                    row_[i] = 255;               // opaque: the editor's ground is
                if (std::fwrite(row_.data(), 1, row_.size(), pipe_) != row_.size())
                    fail("ffmpeg stopped taking frames");
            }
        }

        void close()
        {
            if (pipe_ != nullptr && ::pclose(pipe_) != 0)
                std::fprintf(stderr, "promo: ffmpeg reported an error\n");
            pipe_ = nullptr;
        }

    private:
        int w_, h_;
        FILE* pipe_ = nullptr;
        std::vector<uint8_t> row_;
    };

    // ---- the editor, headless -----------------------------------------------------------------------------------------
    // A Panel and its host, at the scale that makes `region` (read from the Panel once it is laid out) `width` px.
    struct View
    {
        std::unique_ptr<pg::ui::Panel> panel;
        std::optional<funkgui::HeadlessHost> host;
        float dpi = 1.0f;
        int w = 0, h = 0;                        // its window, logical px
        funkgui::Rect region{};
    };

    void choosePage(pg::ui::Panel& panel, const std::string& page)
    {
        if (page.empty())
            return;
        const std::vector<pg::GuiNode>& nodes = panel.panelSpec().gui.nodes;
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].kind != pg::GuiKind::tabs)
                continue;
            for (std::size_t k = 0; k < nodes[i].children.size(); ++k)
                if (nodes[static_cast<std::size_t>(nodes[i].children[k])].text == page)
                {
                    panel.chooseGuiPage(static_cast<int>(i), static_cast<int>(k));
                    return;
                }
        }
        std::fprintf(stderr, "promo: warning: no page \"%s\"\n", page.c_str());
    }

    // A program with no .gui file shows plain dials at the top of the program's card: the video takes their cells, its
    // displays' and its sections' frames, with a margin, rather than the card's empty rest.
    funkgui::Rect plainRegion(pg::ui::Panel& p, const pg::Controls& cs)
    {
        const pg::ui::DialsLayout& L = p.dialsLayout();
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        const auto add = [&](float x, float y, float w, float h) {
            if (!(w > 0.0f) || !(h > 0.0f))
                return;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x + w);
            y1 = std::max(y1, y + h);
        };
        for (std::size_t c = 0; c < cs.size(); ++c)
            if (cs[c].used)
                add(L.dials[c].x, L.dials[c].top, L.dials[c].w, L.dials[c].h);
        for (int d = 0; d < pg::kMaxDisplays; ++d)
            if (const pg::ui::DisplayItem& item = p.displayItem(d); item.shown())
                add(item.cell().x, item.cell().y, item.cell().w, item.cell().h);
        for (const pg::ui::SectionFrame& f : L.sections)
            add(f.r.x, f.r.y, f.r.w, f.r.h);
        const funkgui::Rect card = p.programCard();
        if (!(x1 > x0))
            return card;
        constexpr float m = 24.0f, mTop = 10.0f;    // the top's margin short of the card's status line above
        const float l = std::max(card.x, x0 - m), t = std::max(card.y, y0 - mTop);
        const float r = std::min(card.right(), x1 + m), b = std::min(card.bottom(), y1 + m);
        return { l, t, r - l, b - t };
    }

    // A file's lines as an editor counts them: a last newline ends a line, it does not begin one.
    int linesOf(const funkgui::TextEditor& e)
    {
        const std::string t = e.text();
        return e.lineCount() - (!t.empty() && t.back() == '\n' ? 1 : 0);
    }

    std::string jsonString(const std::string& s)
    {
        return juce::JSON::toString(juce::var(juce::String::fromUTF8(s.c_str())), true).toStdString();
    }

    std::string rectJson(const funkgui::Rect& r)
    {
        std::ostringstream o;
        o << "[" << r.x << ", " << r.y << ", " << r.w << ", " << r.h << "]";
        return o.str();
    }
}

int main(int argc, char** argv)
{
    const fs::path projectFile = argValue(argc, argv, "--project-file");
    if (projectFile.empty() || hasFlag(argc, argv, "--help"))
    {
        std::printf("usage: %s --project-file <p.fmproject> (--info | --storyboard <s.json> --out <dir> [--sizes]"
                    " [--stills t,t,...] [--width 1080] [--fps 60] [--theme 0] [--ffmpeg ffmpeg] [--code-shown px])\n",
                    argv[0]);
        return projectFile.empty() ? 2 : 0;
    }
    const bool info = hasFlag(argc, argv, "--info");
    const bool sizes = hasFlag(argc, argv, "--sizes");
    const fs::path storyPath = argValue(argc, argv, "--storyboard");
    fs::path out = argValue(argc, argv, "--out");
    if (!info && (storyPath.empty() || out.empty()))
        fail("--storyboard and --out are needed (or --info)");
    if (out.empty())
        out = fs::temp_directory_path() / (std::string(pg::product::kName) + "-promo-info");
    const int width = std::max(64, std::atoi(argValue(argc, argv, "--width", "1080").c_str())) & ~1;
    const int fps = std::clamp(std::atoi(argValue(argc, argv, "--fps", "60").c_str()), 1, 240);
    const int theme = std::atoi(argValue(argc, argv, "--theme", "0").c_str());
    const std::string ffmpeg = argValue(argc, argv, "--ffmpeg", "ffmpeg");
    const int codeShown = std::max(0, std::atoi(argValue(argc, argv, "--code-shown", "0").c_str()));
    std::vector<double> stills;
    {
        std::stringstream ss(argValue(argc, argv, "--stills"));
        for (std::string t; std::getline(ss, t, ',');)
            if (!t.empty())
                stills.push_back(std::atof(t.c_str()));
    }

    // The sandbox: the project copied in, every folder the plugin would use pointed there.
    std::error_code ec;
    const fs::path sandbox = out / "sandbox";
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox / "presets", ec);
    fs::create_directories(sandbox / "mcp", ec);
    if (ec)
        fail("cannot make " + sandbox.string() + ": " + ec.message());
    fs::copy_file(projectFile, sandbox / "presets" / projectFile.filename(), fs::copy_options::overwrite_existing, ec);
    if (ec)
        fail("cannot copy " + projectFile.string() + ": " + ec.message());
    const std::string prefix = pg::product::kEnvPrefix;
    ::setenv((prefix + "PREFS_DIR").c_str(), sandbox.c_str(), 1);
    ::setenv((prefix + "MCP_DIR").c_str(), (sandbox / "mcp").c_str(), 1);
    ::setenv((prefix + "SHADERC").c_str(), PROMO_SHADERC, 0);
    for (const char* off : { "FACTORY", "UPDATES", "WHATSNEW" })   // nothing of the network, even with a yes
        ::setenv((prefix + off).c_str(), "off", 1);

    Storyboard story;
    if (!info)
        story = readStoryboard(storyPath);
    const std::string projectName = str(readJson(projectFile), "name");
    if (projectName.empty())
        fail(projectFile.string() + " names no project");

    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    funkgui::UiPreferences::get().setTheme(theme);

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    processor.compiler().setFreezing(false);        // the program's own code throughout: no copy swapped in mid-video
    processor.compiler().setSelfTesting(false);
    Head head(story.tempo);
    processor.setPlayHead(&head);
    processor.prepareToPlay(kRate, kBlock);

    std::string why;
    if (!port.openProject(projectName, story.version, why))
        fail("cannot open \"" + projectName + "\": " + why);
    processor.settle();
    processor.poll();
    if (!story.preset.empty())
    {
        if (!port.applyPreset(projectName, story.preset, why))
            fail("cannot apply the preset \"" + story.preset + "\": " + why);
        processor.settle();
        processor.poll();
    }
    const pg::CompileStatus status = port.status();
    if (!status.ok)
        fail("\"" + projectName + "\" does not compile:\n" + status.errors);
    const pg::Controls controls = port.controls();
    resolveDials(story, controls);

    // Two Panels: the PANEL view and the CODE view, each with the explorer and the chat folded away.
    port.setChatOpen(false);
    port.setExplorerOpen(false);
    port.setCodePanel(false, false);                // the CODE view's editor alone: no panel under it
    port.setCodePanel(true, false);
    View panelView, codeView;
    port.setPanelView(true);
    panelView.panel = std::make_unique<pg::ui::Panel>(processor.ports(), port);
    port.setPanelView(false);
    codeView.panel = std::make_unique<pg::ui::Panel>(processor.ports(), port);
    for (View* v : { &panelView, &codeView })
    {
        v->host.emplace(*v->panel, theme, 1.0f);
        v->w = story.windowW;
        v->h = v == &codeView ? std::max(story.windowH, story.codeHeight) : story.windowH;
        v->host->resize(v->w, v->h);
        v->host->settle(240);
    }
    panelView.panel->setPanelView(true);
    choosePage(*panelView.panel, story.page);
    // The PANEL view as tall as its interface: none of it scrolled out of sight (a keyboard at the bottom, say).
    for (int k = 0; k < 4 && panelView.panel->hasGui(); ++k)
    {
        panelView.host->settle(240);
        const float more = panelView.panel->guiLayout().maxScrollY;
        if (!(more > 0.5f))
            break;
        panelView.h += static_cast<int>(std::ceil(more)) + 2;
        panelView.host->resize(panelView.w, panelView.h);
    }
    codeView.panel->setPanelView(false);
    // The storyboard's file shown (its tab clicked), unfocused (no caret), at its first line.
    {
        pg::ui::Panel& p = *codeView.panel;
        for (int i = 0; i < p.fileCount(); ++i)
            if (p.fileName(i) == story.codeFile)
            {
                const funkgui::Point at = p.centreOfTab(i);
                codeView.host->click(at.x, at.y);
            }
        codeView.host->settle(240);
        p.editor().setFocused(false);
        if (p.activeFile() < 0 || p.fileName(p.activeFile()) != story.codeFile)
            std::fprintf(stderr, "promo: warning: the code view shows %s, not \"%s\"\n",
                         p.activeFile() >= 0 ? p.fileName(p.activeFile()).c_str() : "no file", story.codeFile.c_str());
    }
    panelView.host->settle(240);
    panelView.region = panelView.panel->hasGui() ? panelView.panel->guiLayout().area : plainRegion(*panelView.panel, controls);
    const auto codeRegion = [&] {
        const funkgui::Rect box = codeView.panel->editor().bounds();
        codeView.region = { box.x, box.y, std::min(box.w, static_cast<float>(story.codeWidth)), box.h };
    };
    codeRegion();
    // The code the video shows (--code-shown, px at --width): the CODE view's window made that much editor tall, so
    // the editor's own scroll ends where the video's view of it does, with the file's last line at its bottom.
    if (codeShown > 0)
    {
        const float chrome = static_cast<float>(codeView.h) - codeView.region.h;
        const float wanted = static_cast<float>(codeShown) * codeView.region.w / static_cast<float>(width);
        codeView.h = std::max(pg::ui::kMinHeight, static_cast<int>(std::ceil(wanted + chrome)));
        codeView.host->resize(codeView.w, codeView.h);
        codeView.host->settle(240);
        codeRegion();
        codeView.region.h = std::min(codeView.region.h, wanted);
    }
    if (sizes)
    {
        const auto px = [&](const funkgui::Rect& r) {
            return "[" + std::to_string(width) + ", " + std::to_string(static_cast<int>(std::lround(r.h * static_cast<float>(width) / r.w)) & ~1) + "]";
        };
        std::printf("{\"panel\": %s, \"code\": %s, \"code_lines\": %d}\n", px(panelView.region).c_str(),
                    px(codeView.region).c_str(), linesOf(codeView.panel->editor()));
        fs::remove_all(sandbox, ec);
        return 0;
    }

    pg::ui::Panel& cp = *codeView.panel;
    if (info)
    {
        std::ostringstream o;
        o << "{\n  \"project\": " << jsonString(projectName) << ",\n  \"compiles\": " << (status.ok ? "true" : "false")
          << ",\n  \"notes\": " << jsonString(status.notes) << ",\n  \"gui\": " << (panelView.panel->hasGui() ? "true" : "false")
          << ",\n  \"panel_area\": " << rectJson(panelView.region) << ",\n  \"code_box\": " << rectJson(cp.editor().bounds())
          << ",\n  \"files\": [";
        for (int i = 0; i < cp.fileCount(); ++i)
            o << (i ? ", " : "") << jsonString(cp.fileName(i));
        o << "],\n  \"pages\": [";
        bool first = true;
        for (const pg::GuiNode& n : panelView.panel->panelSpec().gui.nodes)
            if (n.kind == pg::GuiKind::tabs)
                for (int child : n.children)
                {
                    o << (first ? "" : ", ") << jsonString(panelView.panel->panelSpec().gui.nodes[static_cast<std::size_t>(child)].text);
                    first = false;
                }
        o << "],\n  \"presets\": [";
        first = true;
        for (const pg::PresetEntry& e : port.presets(projectName))
        {
            o << (first ? "" : ", ") << jsonString(e.name);
            first = false;
        }
        o << "],\n  \"dials\": [\n";
        first = true;
        for (int c = 0; c < pg::kMaxControls; ++c)
        {
            const pg::Control& k = controls[static_cast<std::size_t>(c)];
            if (!k.used)
                continue;
            const float norm = processor.ports()[static_cast<std::size_t>(c)]->value01();
            o << (first ? "" : ",\n") << "    {\"name\": " << jsonString(k.name) << ", \"min\": " << k.min << ", \"max\": " << k.max
              << ", \"default\": " << k.def << ", \"unit\": " << jsonString(k.unit) << ", \"type\": \""
              << (k.type == pg::ControlType::toggle ? "toggle" : k.type == pg::ControlType::integer ? "integer" : "real")
              << "\", \"now\": " << pg::plainOf(k, norm) << ", \"now_norm\": " << norm << "}";
            first = false;
        }
        o << "\n  ]\n}\n";
        std::fputs(o.str().c_str(), stdout);
        fs::remove_all(sandbox, ec);
        return 0;
    }

    // Each view again at the scale that makes its region the video's width.
    for (View* v : { &panelView, &codeView })
    {
        v->dpi = static_cast<float>(width) / v->region.w;
        v->host.reset();
        v->host.emplace(*v->panel, theme, v->dpi);
        v->host->resize(v->w, v->h);
        v->host->settle(240);
    }
    codeView.panel->editor().setFocused(false);

    // The code's scroll: from its first line, `speed` lines a second, never past its end.
    funkgui::TextEditor& code = codeView.panel->editor();
    const float rowH = code.contentHeight() / static_cast<float>(std::max(1, code.rowCount()));
    const float scroll0 = static_cast<float>(story.codeLine - 1.0) * rowH;
    const float scrollMax = std::max(0.0f, code.contentHeight() - (codeView.region.bottom() - code.textArea().y));

    // ---- the loop -------------------------------------------------------------------------------------------------
    const int64_t total = static_cast<int64_t>(std::llround(story.seconds * kRate));
    const int frames = static_cast<int>(std::llround(story.seconds * fps));
    std::vector<float> left, right;
    left.reserve(static_cast<std::size_t>(total));
    right.reserve(static_cast<std::size_t>(total));
    juce::AudioBuffer<float> buffer(2, kBlock);
    std::size_t nextEvent = 0;
    int64_t now = 0;
    const auto play = [&](int64_t until, bool keep) {
        while (now < until)
        {
            int n = static_cast<int>(std::min<int64_t>(kBlock, until - now));
            juce::MidiBuffer midi;
            while (nextEvent < story.events.size() && story.events[nextEvent].at < now + n)
            {
                const NoteEvent& e = story.events[nextEvent++];
                const int at = static_cast<int>(std::max<int64_t>(0, e.at - now));
                midi.addEvent(e.velocity > 0.0f ? juce::MidiMessage::noteOn(1, e.note, e.velocity)
                                                : juce::MidiMessage::noteOff(1, e.note), at);
            }
            // A move is a gesture on the dial's port (a host parameter's or a panel dial's), as a hand on the knob.
            for (DialMove& d : story.dials)
            {
                if (now < d.from || d.done)
                    continue;
                funkgui::ParamPort* dial = processor.ports()[static_cast<std::size_t>(d.slot)];
                if (d.start < 0.0f)
                {
                    d.start = dial->value01();
                    dial->beginGesture();
                }
                const double span = static_cast<double>(std::max<int64_t>(1, d.to - d.from));
                const double t = std::clamp(static_cast<double>(now - d.from) / span, 0.0, 1.0);
                const double s = t * t * (3.0 - 2.0 * t);
                dial->setValue01(static_cast<float>(d.start + (d.target - d.start) * s));
                if (t >= 1.0)
                {
                    dial->endGesture();
                    d.done = true;
                }
            }
            buffer.setSize(2, n, false, false, true);
            buffer.clear();
            head.at(now);
            processor.processBlock(buffer, midi);
            if (keep)
                for (int i = 0; i < n; ++i)
                {
                    left.push_back(buffer.getSample(0, i));
                    right.push_back(buffer.getSample(1, i));
                }
            now += n;
        }
    };

    std::unique_ptr<Encoder> panelOut, codeOut;
    Crop panelCrop, codeCrop;
    bool gpu = true;
    std::string gpuWhy;
    std::size_t nextStill = 0;
    std::sort(stills.begin(), stills.end());
    if (!stills.empty())
        fs::create_directories(out, ec);

    for (int f = 0; f < frames; ++f)
    {
        play(std::min(total, static_cast<int64_t>(std::llround(static_cast<double>(f + 1) * kRate / fps))), true);
        processor.poll();
        const double t = static_cast<double>(f) / fps;
        code.setScroll(0.0f, std::clamp(scroll0 + static_cast<float>(t * story.codeSpeed) * rowH, 0.0f, scrollMax));
        panelView.host->tick(1, 1.0f / static_cast<float>(fps));
        codeView.host->tick(1, 1.0f / static_cast<float>(fps));

        const bool still = nextStill < stills.size() && t + 0.5 / fps >= stills[nextStill];
        if (!stills.empty() && !still)
            continue;
        const pg::picture::Drawn pd = pg::picture::draw(panelView.host->draw());
        const pg::picture::Drawn cd = pg::picture::draw(codeView.host->draw());
        if (pd.image.w <= 0 || cd.image.w <= 0)
            fail("the editor drew nothing");
        gpu = gpu && pd.gpu && cd.gpu;
        if (!pd.gpu)
            gpuWhy = pd.why;
        if (panelOut == nullptr && codeOut == nullptr && stills.empty())
        {
            fs::create_directories(out, ec);
            panelCrop = cropOf(panelView.region, panelView.dpi, pd.image);
            codeCrop = cropOf(codeView.region, codeView.dpi, cd.image);
            panelOut = std::make_unique<Encoder>(ffmpeg, out / "panel.mp4", panelCrop.w, panelCrop.h, fps);
            codeOut = std::make_unique<Encoder>(ffmpeg, out / "code.mp4", codeCrop.w, codeCrop.h, fps);
        }
        if (still)
        {
            char tag[32];
            std::snprintf(tag, sizeof tag, "%07.3f", stills[nextStill]);
            const Crop pc = cropOf(panelView.region, panelView.dpi, pd.image);
            const Crop cc = cropOf(codeView.region, codeView.dpi, cd.image);
            writeText(out / (std::string("panel_") + tag + ".png"), pg::picture::png(pd.image, pc.x, pc.y, pc.w, pc.h));
            writeText(out / (std::string("code_") + tag + ".png"), pg::picture::png(cd.image, cc.x, cc.y, cc.w, cc.h));
            writeText(out / (std::string("window_") + tag + ".png"), pg::picture::png(pd.image, 0, 0, pd.image.w, pd.image.h));
            ++nextStill;
            continue;
        }
        panelOut->frame(pd.image, panelCrop);
        codeOut->frame(cd.image, codeCrop);
        if (f % fps == 0)
            std::fprintf(stderr, "\rpromo: %s %d / %d s", projectName.c_str(), f / fps, frames / fps);
    }
    play(total, true);
    std::fprintf(stderr, "\n");
    if (panelOut != nullptr)
        panelOut->close();
    if (codeOut != nullptr)
        codeOut->close();
    if (stills.empty())
        writeWav(out / "audio.wav", left, right);

    float peak = 0.0f;
    double sum = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        peak = std::max({ peak, std::abs(left[i]), std::abs(right[i]) });
        sum += static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
    }
    const double rms = left.empty() ? 0.0 : std::sqrt(sum / (2.0 * static_cast<double>(left.size())));
    std::ostringstream o;
    o << "{\n  \"project\": " << jsonString(projectName) << ",\n  \"fps\": " << fps << ",\n  \"frames\": " << frames
      << ",\n  \"seconds\": " << story.seconds << ",\n  \"rate\": " << kRate << ",\n  \"panel\": [" << panelCrop.w << ", "
      << panelCrop.h << "],\n  \"code\": [" << codeCrop.w << ", " << codeCrop.h << "],\n  \"gpu\": " << (gpu ? "true" : "false")
      << ",\n  \"gpu_why\": " << jsonString(gpuWhy) << ",\n  \"peak\": " << peak << ",\n  \"rms\": " << rms
      << ",\n  \"code_lines\": " << linesOf(code) << "\n}\n";
    writeText(out / "meta.json", o.str());
    fs::remove_all(sandbox, ec);
    std::printf("%s", o.str().c_str());
    return 0;
}
