// PROBE name=plugin.live timeout=120
//
// plugin.live: the live tools (plugin/Live.cpp) over a real processor, Claude Desktop's calls made through the agent
// (ChatAgent::mcpCall) and through the instance's socket (McpLink), in folders of the probe's own. A host thread plays
// the processor as an audio thread would, in real time (48 kHz, blocks of 256), with a playhead at 100 BPM in 7/8:
//   LISTEN  listen while the host plays: it says it is live and cannot be reproduced; the host's blocks, its tempo,
//           signature and transport, the rate, the largest block, not bypassed; an effect's: a 1 kHz sine at -12 dBFS
//           through a gain of -6 dB: the input's RMS (-15.1 dBFS), the gain in to out (-6 dB), the output's peak at
//           1 kHz, the display's field (0.501); an instrument's: three note-ons (A4, C5, E5) counted with their range
//           and velocity, the keys held, three voices at once, none stolen or dropped, the spectrum's strongest peak at
//           one of the notes; the CPU per block; the text and the JSON; the meters' readers raised while it listens
//           and back after
//   BYPASS  the host's bypass: listen says so
//   SILENT  a host that calls nothing: listen says the host is not calling the plugin, and measures nothing
//   LINK    through the socket: a listen of 5 s, get_status answered meanwhile (before the listen's reply), then a cancel
//           that stops the listen at once (its reply says it stopped early), the readers back to none
//   SINCE   get_status's dial lines in their units (a frequency in kHz, a scaled percentage with its number, an int by
//           its option's name) and its token; with since: the editor's drag of a dial, one line in its units; the
//           host's automation; a typed edit, and its diff against the text Claude Desktop last read; RUN; a version and
//           a preset saved and the preset applied (its dials no move); the host's bypass and its end, its transport
//           starting, a new sample rate; the next call with the new token: nothing; another instance's token: the whole
//           state; another tool's result starts with the hint while changes wait, and not when none do
#include "Probe.h"

#include "Audio.h"

#include "mcp/McpProtocol.h"
#include "plugin/McpLink.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/params/ParamPort.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kIn = 0.25118864315095801;              // -12 dBFS

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : fields)
            o->setProperty(k, v);
        return juce::var(o);
    }

    std::string textOf(const pg::ChatAgent::McpResult& r)
    {
        std::string all;
        if (const juce::Array<juce::var>* c = r.content.getArray())
            for (const juce::var& b : *c)
                all += b["text"].toString().toStdString();
        return all;
    }

    // The tool's own text: its last block (a line saying what changed may come first).
    std::string resultOf(const pg::ChatAgent::McpResult& r)
    {
        const juce::Array<juce::var>* c = r.content.getArray();
        return c != nullptr && !c->isEmpty() ? c->getLast()["text"].toString().toStdString() : std::string();
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    int count(const std::string& s, const std::string& part)
    {
        int n = 0;
        for (std::size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1))
            ++n;
        return n;
    }

    std::string tokenOf(const std::string& status)
    {
        const std::size_t at = status.rfind("Token: ");
        return at == std::string::npos ? std::string() : status.substr(at + 7, status.find(' ', at + 7) - at - 7);
    }

    class Head final : public juce::AudioPlayHead
    {
    public:
        std::atomic<bool> playing{ true };
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo pos;
            pos.setBpm(100.0);
            pos.setPpqPosition(4.5);
            pos.setPpqPositionOfLastBarStart(4.0);
            pos.setTimeSignature(TimeSignature{ 7, 8 });
            pos.setIsPlaying(playing.load());
            return pos;
        }
    };

    // A host that plays the processor in real time on a thread of its own: an effect's input a 1 kHz sine at -12 dBFS;
    // an instrument's three note-ons, 40, 50 and 60 blocks after a tool starts listening.
    struct Host
    {
        pg::Processor& p;
        std::atomic<bool> stop{ false }, bypass{ false };
        std::atomic<int> blocks{ 0 };
        std::atomic<int> notesAt{ -1 };                          // the block a listener was first seen at
        std::thread thread;

        explicit Host(pg::Processor& proc) : p(proc) {}
        ~Host() { end(); }
        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;

        void start() { thread = std::thread([this] { run(); }); }
        void end()
        {
            stop.store(true);
            if (thread.joinable())
                thread.join();
        }

        void run()
        {
            juce::AudioBuffer<float> buffer(2, kBlock);
            auto next = std::chrono::steady_clock::now();
            std::size_t pos = 0;
            for (int b = 0; !stop.load(); ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    const float v = kInstrument ? 0.0f
                                                : static_cast<float>(kIn * std::sin(2.0 * 3.14159265358979323846 * 1000.0
                                                                                    * static_cast<double>(pos + static_cast<std::size_t>(i)) / kFs));
                    buffer.setSample(0, i, v);
                    buffer.setSample(1, i, v);
                }
                juce::MidiBuffer midi;
                if constexpr (kInstrument)
                {
                    if (notesAt.load() < 0 && p.meterReaders() > 0)
                        notesAt.store(b);
                    const int at = notesAt.load();
                    if (at >= 0)
                        for (const auto& [k, note] : { std::pair{ 40, 69 }, std::pair{ 50, 72 }, std::pair{ 60, 76 } })
                            if (b == at + k)
                                midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 8);
                }
                if (bypass.load())
                    p.processBlockBypassed(buffer, midi);
                else
                    p.processBlock(buffer, midi);
                pos += kBlock;
                blocks.store(b + 1);
                next += std::chrono::microseconds(static_cast<int64_t>(1.0e6 * kBlock / kFs));
                std::this_thread::sleep_until(next);
            }
        }
    };

    // The program listen hears: an effect's gain (-6 dB) with a readout of its factor; an instrument's sine voice.
    std::string listenProgram()
    {
        if constexpr (kInstrument)
            return "#pragma panel number lvl\n"
                   "module Voice\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.25)]];\n"
                   "    float rate, ph, step, lvl;\n    bool held;\n"
                   "    void init(float r) { rate = r; }\n"
                   "    void start(float note, float velocity) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; }\n"
                   "    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            const float s = level[i] * sinf(6.28318531f * ph);\n"
                   "            ph += step;\n            if (ph >= 1) ph -= 1;\n"
                   "            left[i] += s;\n            right[i] += s;\n        }\n"
                   "        lvl = level[0];\n        return held;\n    }\n};\n";
        else
            return "#pragma panel number k\n"
                   "module Effect\n{\n    param float trim [[fminus::range(-24, 12)]] [[fminus::default(-6)]];\n    float k;\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            const float g = expf(0.115129255f * trim[i]);\n"
                   "            left[i] *= g;\n            right[i] *= g;\n            k = g;\n        }\n    }\n};\n";
    }

    // The program get_status's dial lines read: a frequency on a log taper in Hz, a percentage scaled from 0..1, an int
    // with its options' names (panel.gui).
    pg::Sources unitsProgram()
    {
        const std::string params = "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                                   "    param float mix [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param int wave [[fminus::range(0, 2)]] [[fminus::default(1)]];\n";
        const std::string program =
            kInstrument ? "module Voice\n{\n" + params
                              + "    void start(float note, float velocity) {}\n"
                                "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                        : "module Effect\n{\n" + params
                              + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                                "        for (int i = 0; i < n; ++i)\n        {\n            left[i] *= mix[i];\n"
                                "            right[i] *= mix[i];\n        }\n    }\n};\n";
        const std::string gui = "panel {\n    row {\n        knob cutoff taper=log unit=\"Hz\"\n"
                                "        knob mix scale=100 unit=\"%\" decimals=0\n"
                                "        buttons wave \"SAW\" \"SQUARE\" \"TRI\"\n    }\n}\n";
        return { { "program.fm", program }, { "panel.gui", gui } };
    }

    bool load(pg::Processor& p, const pg::Sources& files)
    {
        pg::Draft d;
        d.files = files;
        p.editorPort().setDraft(d);
        p.editorPort().run(files);
        p.settle();
        return p.editorPort().status().ok;
    }

    int slotOf(pg::Processor& p, const char* name)
    {
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (std::string(p.control(c).name) == name)
                return c;
        return -1;
    }

    // A block on this thread (the SINCE part plays its own blocks), then the message thread's poll.
    void block(pg::Processor& p, bool bypassed = false)
    {
        juce::AudioBuffer<float> buffer(2, kBlock);
        buffer.clear();
        juce::MidiBuffer midi;
        if (bypassed)
            p.processBlockBypassed(buffer, midi);
        else
            p.processBlock(buffer, midi);
        p.poll();
    }
}

PROBE(plugin, live)
{
    const funkgui::HeadlessGuiScope gui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Folders of the probe's own: the sockets (a path short enough for one), the projects.
    const std::string dir = "/tmp/" + std::string(pg::product::kEnvPrefix) + "live-" + std::to_string(getpid());
    setenv((std::string(pg::product::kEnvPrefix) + "MCP_DIR").c_str(), dir.c_str(), 1);
    const juce::File projects = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getChildFile(juce::String(pg::product::kEnvPrefix) + "live-projects-" + juce::String(getpid()));
    projects.deleteRecursively();
    pg::projectstore::setDirectoryForTests(projects);

    Head head;
    auto owned = std::make_unique<pg::Processor>();
    pg::Processor& p = *owned;
    p.setPlayHead(&head);
    p.setRateAndBufferSizeDetails(kFs, kBlock);
    p.prepareToPlay(kFs, kBlock);
    P.eq("listen.program_compiles", load(p, pg::singleFile(listenProgram())) ? 1 : 0, 1);
    pg::ChatAgent& agent = p.chatAgent();
    const auto listen = [&](double seconds, bool json) {
        juce::var args = object({ { "seconds", seconds } });
        if (json)
            args.getDynamicObject()->setProperty("format", "json");
        return resultOf(agent.mcpCall("listen", args));
    };

    // ---- LISTEN --------------------------------------------------------------------------------------------------------
    {
        Host host(p);
        host.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        P.eq("listen.no_reader_before", p.meterReaders(), 0);
        const std::string j = listen(1.0, true);
        const int readersAfter = p.meterReaders();
        const std::string t = listen(0.5, false);
        host.end();
        std::printf("listen, json:\n%s\nlisten, text:\n%s", j.c_str(), t.c_str());
        const juce::var v = juce::JSON::parse(juce::String::fromUTF8(j.c_str()));
        const juce::var h = v["host"];
        P.eq("listen.json_live", static_cast<bool>(v["live"]) ? 1 : 0, 1);
        P.ge("listen.blocks", static_cast<double>(h["blocks"]), 120.0);
        P.eq("listen.host", static_cast<bool>(h["known"]) && static_cast<bool>(h["playing"]) && static_cast<int>(h["beats_per_bar"]) == 7
                                    && static_cast<int>(h["beat_unit"]) == 8 && !static_cast<bool>(h["bypassed"]) ? 1 : 0, 1);
        P.near("listen.host_bpm", static_cast<double>(h["bpm"]), 100.0, 1e-6);
        P.near("listen.host_rate", static_cast<double>(h["rate"]), kFs, 1e-6);
        P.eq("listen.host_largest_block", static_cast<int>(h["largest_block"]), kBlock);
        const juce::var lv = v["levels"];
        const juce::var peaks = v["spectrum"]["out"]["peaks"];
        const double top = peaks.size() > 0 ? static_cast<double>(peaks[0]["hz"]) : 0.0;
        if constexpr (kInstrument)
        {
            const juce::var midi = v["midi"], voices = v["voices"];
            P.eq("listen.note_ons", static_cast<int>(midi["note_ons"]), 3);
            P.eq("listen.note_range", static_cast<int>(midi["lowest"]) == 69 && static_cast<int>(midi["highest"]) == 76 ? 1 : 0, 1);
            P.eq("listen.velocity", static_cast<int>(midi["velocity_min"]) == 100 && static_cast<int>(midi["velocity_max"]) == 100 ? 1 : 0, 1);
            P.eq("listen.held", midi["held"].size(), 3);
            P.eq("listen.voices_at_once", static_cast<int>(voices["most"]), 3);
            P.eq("listen.none_stolen_or_dropped", static_cast<int>(voices["steals"]) == 0 && static_cast<int>(voices["dropped"]) == 0 ? 1 : 0, 1);
            const bool atANote = std::fabs(top - 440.0) < 9.0 || std::fabs(top - 523.25) < 10.0 || std::fabs(top - 659.26) < 13.0;
            P.eq("listen.peak_at_a_note", atANote ? 1 : 0, 1);
            P.eq("listen.text_no_new_notes", has(t, "MIDI: 0 note-ons") ? 1 : 0, 1);
            P.eq("listen.text_held", has(t, "held now: A4 C5 E5") ? 1 : 0, 1);
        }
        else
        {
            P.near("listen.in_rms", static_cast<double>(lv["in"]["rms_p50_dbfs"]), -15.05, 0.3);
            P.near("listen.gain", static_cast<double>(lv["gain_db"]["p50"]), -6.0, 0.2);
            P.near("listen.out_peak", static_cast<double>(lv["out"]["peak_dbfs"]), -18.0, 0.3);
            P.near("listen.peak_1khz", top, 1000.0, 10.0);
            P.eq("listen.text_gain", has(t, "Gain in to out: -6.0 dB") ? 1 : 0, 1);
        }
        const juce::var shown = v["displays"];
        P.eq("listen.display", shown.size() == 1 ? 1 : 0, 1);
        if (shown.size() == 1)
            P.near("listen.display_now", static_cast<double>(shown[0]["now"]), kInstrument ? 0.25 : 0.50118723, 1e-3);
        P.ge("listen.cpu", static_cast<double>(v["cpu_percent"]["max"]), 0.0);
        P.eq("listen.cpu_said", v["cpu_percent"].isObject() && has(t, "CPU per block: p50 ") ? 1 : 0, 1);
        P.eq("listen.text_live_first", t.rfind("Live: ", 0) == 0 && has(t.substr(0, t.find('\n')), "cannot be reproduced") ? 1 : 0, 1);
        P.eq("listen.text_host", has(t, "Host: 100.0 BPM, playing at bar 2") && has(t, "7/8; 48 kHz, blocks up to 256 samples; not bypassed") ? 1 : 0, 1);
        P.eq("listen.text_plays_the_editor", has(t, "What plays is the editor's files.") ? 1 : 0, 1);
        P.eq("listen.readers_back", readersAfter == 0 && p.meterReaders() == 0 ? 1 : 0, 1);
    }

    // ---- BYPASS and SILENT ---------------------------------------------------------------------------------------------
    {
        Host host(p);
        host.bypass.store(true);
        host.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string t = listen(0.3, false);
        host.end();
        P.eq("bypass.said", has(t, kInstrument ? "bypassed (silent)" : "bypassed (the input passes)") ? 1 : 0, 1);
        const std::string none = listen(0.3, false);
        std::printf("listen, no host:\n%s", none.c_str());
        P.eq("silent.not_called", has(none, "0 blocks: the host is not calling the plugin") && has(none, "nothing was measured") ? 1 : 0, 1);
    }

    // ---- LINK ----------------------------------------------------------------------------------------------------------
    {
        const pg::McpLink* link = p.mcpLink();
        const int fd = link != nullptr ? pg::mcp::connectTo(link->socketPath()) : -1;
        P.eq("link.connected", fd >= 0 ? 1 : 0, 1);
        if (fd >= 0)
        {
            Host host(p);
            host.start();
            pg::mcp::LineReader r(fd);
            const auto send = [&](int id, const char* method, const juce::var& params) {
                pg::mcp::writeLine(fd, juce::JSON::toString(object({ { "id", id }, { "method", method }, { "params", params } }), true).toStdString());
            };
            const auto t0 = std::chrono::steady_clock::now();
            send(1, "call", object({ { "name", "listen" }, { "arguments", object({ { "seconds", 5 } }) } }));
            send(2, "call", object({ { "name", "get_status" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
            std::string line;
            const int got = r.next(line, 2000);
            const double first = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const juce::var reply = got == 1 ? juce::JSON::parse(juce::String::fromUTF8(line.c_str())) : juce::var();
            P.eq("link.status_first", got == 1 && static_cast<int>(reply["id"]) == 2 ? 1 : 0, 1);
            P.le("link.status_quick_s", first, 1.0);
            for (int k = 0; k < 100 && p.meterReaders() == 0; ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            P.eq("link.listening_meanwhile", p.meterReaders(), 1);
            send(3, "cancel", object({ { "id", 1 } }));
            const auto c0 = std::chrono::steady_clock::now();
            bool cancelled = false, stopped = false;
            for (int k = 0; k < 2 && r.next(line, 3000) == 1; ++k)
            {
                const juce::var v = juce::JSON::parse(juce::String::fromUTF8(line.c_str()));
                if (static_cast<int>(v["id"]) == 3)
                    cancelled = static_cast<bool>(v["result"]["cancelled"]);
                if (static_cast<int>(v["id"]) == 1)
                    for (const juce::var& b : *v["result"]["content"].getArray())
                        stopped = stopped || b["text"].toString().contains("(stopped early)");
            }
            const double stopS = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
            host.end();
            P.eq("link.cancelled", cancelled ? 1 : 0, 1);
            P.eq("link.listen_stopped_early", stopped ? 1 : 0, 1);
            P.le("link.stopped_quick_s", stopS, 1.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            P.eq("link.readers_back", p.meterReaders(), 0);
            close(fd);
        }
    }

    // ---- SINCE ---------------------------------------------------------------------------------------------------------
    {
        pg::Processor q;
        Head stopped;
        stopped.playing.store(false);
        q.setPlayHead(&stopped);
        q.setRateAndBufferSizeDetails(kFs, kBlock);
        q.prepareToPlay(kFs, kBlock);
        P.eq("since.program_compiles", load(q, unitsProgram()) ? 1 : 0, 1);
        block(q);
        pg::ChatAgent& a = q.chatAgent();
        const juce::var none(new juce::DynamicObject());
        const std::string status = textOf(a.mcpCall("get_status", none));
        std::printf("get_status:\n%s", status.c_str());
        P.eq("since.dial_hz", has(status, "cutoff: float [20, 20000] Hz default 1.00 kHz, now 1.00 kHz\n") ? 1 : 0, 1);
        P.eq("since.dial_scaled", has(status, "mix: float [0, 1] default 50 % (0.5), now 50 % (0.5)\n") ? 1 : 0, 1);
        P.eq("since.dial_options", has(status, "wave: int [0, 2] (SAW SQUARE TRI) default SQUARE, now SQUARE\n") ? 1 : 0, 1);
        const std::string token = tokenOf(status);
        P.eq("since.token", token.empty() ? 0 : 1, 1);
        const std::string read = textOf(a.mcpCall("read_file", object({ { "name", "program.fm" } })));
        P.eq("since.no_hint_without_changes", read.rfind("module ", 0) == 0 ? 1 : 0, 1);

        // The changes.
        const int cutoff = slotOf(q, "cutoff"), mix = slotOf(q, "mix");
        funkgui::ParamPort& knob = *q.ports()[static_cast<std::size_t>(cutoff)];
        knob.beginGesture();
        knob.setValue01(0.6f);
        q.poll();
        knob.setValue01(0.7f);
        q.poll();
        knob.endGesture();
        q.poll();
        q.getParameters()[mix]->setValueNotifyingHost(0.3f);   // the host's automation: no gesture
        q.poll();
        pg::Draft d = q.editorPort().draft();
        d.files[0].text += "// typed\n";
        q.editorPort().setDraft(d);
        q.editorPort().run(d.files);
        q.settle();
        pg::SavedVersion saved;
        std::string why;
        P.eq("since.version_saved", q.editorPort().saveVersion("Live probe", saved, why) ? 1 : 0, 1);
        P.eq("since.preset_saved", q.editorPort().savePreset("Bright", why) ? 1 : 0, 1);
        q.getParameters()[mix]->setValueNotifyingHost(0.8f);
        q.poll();
        P.eq("since.preset_applied", q.editorPort().applyPreset("Live probe", "Bright", why) ? 1 : 0, 1);
        q.poll();
        block(q, true);
        block(q, true);
        block(q);
        stopped.playing.store(true);
        block(q);
        q.prepareToPlay(44100.0, kBlock);
        block(q);

        const std::string hinted = textOf(a.mcpCall("read_file", object({ { "name", "panel.gui" } })));
        std::printf("read_file's result after the changes: %s\n", hinted.substr(0, hinted.find('\n')).c_str());
        P.eq("since.hint", hinted.rfind("(The user changed ", 0) == 0 && has(hinted, "get_status with since shows them.)") ? 1 : 0, 1);
        const std::string since = textOf(a.mcpCall("get_status", object({ { "since", juce::String(token) } })));
        std::printf("get_status since:\n%s", since.c_str());
        P.eq("since.drag_one_line", count(since, "you moved cutoff 1.00 kHz -> 2.52 kHz\n"), 1);
        P.eq("since.host_moved", has(since, "the host moved mix 50 % -> ") && has(since, "-> 80 %\n") ? 1 : 0, 1);
        P.eq("since.preset_no_move", has(since, "80 % -> 30 %") ? 0 : 1, 1);
        P.eq("since.edited", has(since, "you edited program.fm\n") ? 1 : 0, 1);
        P.eq("since.ran", has(since, "you ran it: it compiled, 3 dials\n") ? 1 : 0, 1);
        P.eq("since.version", has(since, "you saved version 1 of \"Live probe\"\n") ? 1 : 0, 1);
        P.eq("since.presets", has(since, "you saved preset \"Bright\"\n") && has(since, "you applied preset \"Bright\" of \"Live probe\"\n") ? 1 : 0, 1);
        P.eq("since.bypass", has(since, "the host bypassed the plugin, until ") ? 1 : 0, 1);
        P.eq("since.transport", has(since, "the host started playing, at 100.0 BPM, bar 2\n") ? 1 : 0, 1);
        P.eq("since.rate", has(since, "the host changed the sample rate: 48 kHz -> 44.1 kHz\n") ? 1 : 0, 1);
        P.eq("since.diff", has(since, "program.fm (+1 -0), against the text you last saw:\n") && has(since, "\n+// typed\n") ? 1 : 0, 1);
        P.eq("since.no_whole_state", has(since, "Dials:\n") ? 0 : 1, 1);
        const std::string again = textOf(a.mcpCall("get_status", object({ { "since", juce::String(tokenOf(since)) } })));
        P.eq("since.then_nothing", has(again, "Nothing changed since your last look.\n") && has(again, "The files are as you last saw them.\n") ? 1 : 0, 1);
        const std::string other = textOf(a.mcpCall("get_status", object({ { "since", "abc-1" } })));
        P.eq("since.unknown_token_whole_state", has(other, "That token is not this instance's") && has(other, "Dials:\n") ? 1 : 0, 1);
    }

    owned.reset();
    pg::projectstore::setDirectoryForTests({});
    projects.deleteRecursively();
    juce::File(juce::String(dir)).deleteRecursively();
    return P.finish();
}
