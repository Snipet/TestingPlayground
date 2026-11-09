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
//           one of the notes; a racing wheel's fields off rest (steering, throttle, gear, buttons; the brake at rest
//           left out); the CPU per block; the text and the JSON; the meters' readers raised while it listens and back
//           after
//   MONO    an instrument's: a #pragma mono program's voices are one at a time (of 1: mono), where LISTEN's are of 16
//   BYPASS  the host's bypass: listen says so
//   SILENT  a host that calls nothing: listen says the host is not calling the plugin, and measures nothing
//   LINK    through the socket: a listen of 5 s, get_status answered meanwhile (before the listen's reply), then a cancel
//           that stops the listen at once (its reply says it stopped early), the readers back to none
//   SINCE   get_status's dial lines in their units (a frequency in kHz, a scaled percentage with its number, an int by
//           its option's name) and its token; with since, each in the third person ("the user moved", never "you"): the
//           editor's drag of a dial, one line in its units; the host's automation, its two moves of a dial with a run
//           between them a line each, on either side of the run's; a typed edit, and its diff against the file at the
//           token; RUN; a version and a preset
//           saved and the preset applied (its dials no move); the host's bypass and its end, its transport starting, a
//           new sample rate; the next call with the new token: nothing; another instance's token: the whole state;
//           another tool's result starts with the hint while changes wait (the first three named with their values,
//           and how many more), and not when none do; each token handed out
//           its own, two tokens from before an edit both shown it (one conversation's look moves nothing of another's),
//           Claude Desktop's own write no change but in the diff; a host move just before a preset that leaves that
//           dial alone still a move; a bypass ending after a token: that token's reader reads the end, an older
//           token's one line, never "still"
//   NET     a dial moved three times (by the user, the host, the user, back where it started): one line, its net
//           change, at its first move's place; a dial moved, a preset applied that sets it, the dial moved again: a line
//           on either side of the preset's, the second from the preset's value; files "changed" with a new file of one
//           line: "1 line"
//   LAYOUT  a netlist redrawn (its *@ lines alone): listen says the editor's files play, get_status since says "the
//           user redrew pedal.cir", and with files "changed" that only its drawing changed (no *@ line in the diff); a
//           netlist line edited beside it: that line's diff alone
//   REOPEN  the plugin opened again from its session: a token from before still says which files changed since (one
//           edited, with read_file's versions, one redrawn, one added, one closed, one as it was), and after a second
//           opening too; files "none" leaves it out; a token of no record is not this instance's
//   CANCEL  two cancels before a listen starts: the first one's listen still stops at once
//   NEWCHAT New chat while a call of Claude Desktop's runs: the chat stays busy until it ends
//   STOPPED the journal's words for a run whose program stopped as it played (runWords): what plays now, as get_status
//           says it (nothing; the program before again; the program before on)
#include "Probe.h"

#include "Audio.h"

#include "mcp/McpProtocol.h"
#include "plugin/McpLink.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "fminus/CirDoc.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/params/ParamPort.h>

#include <algorithm>
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
namespace cir = playground::cir;

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
                    if (at >= 0 && b == at + 45)                     // a racing wheel: half right, half throttle, 2nd, cross
                        for (const auto& [cc, value] : { std::pair{ 16, 96 }, std::pair{ 17, 64 }, std::pair{ 20, 2 }, std::pair{ 21, 1 } })
                            midi.addEvent(juce::MidiMessage::controllerEvent(1, cc, value), 16);
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
#if defined(_WIN32)
    const std::string dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "live-" + juce::String(getpid()))
                                .getFullPathName()
                                .replaceCharacter('\\', '/')
                                .toStdString();
#else
    const std::string dir = "/tmp/" + std::string(pg::product::kEnvPrefix) + "live-" + std::to_string(getpid());
#endif
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
            P.eq("listen.voices_of_sixteen", static_cast<int>(voices["of"]) == pg::Patch::kVoices && voices["mono"].isVoid()
                                                 && has(t, " at once (of 16), ") ? 1 : 0, 1);
            P.eq("listen.none_stolen_or_dropped", static_cast<int>(voices["steals"]) == 0 && static_cast<int>(voices["dropped"]) == 0 ? 1 : 0, 1);
            const bool atANote = std::fabs(top - 440.0) < 9.0 || std::fabs(top - 523.25) < 10.0 || std::fabs(top - 659.26) < 13.0;
            P.eq("listen.peak_at_a_note", atANote ? 1 : 0, 1);
            P.eq("listen.text_no_new_notes", has(t, "MIDI: 0 note-ons") ? 1 : 0, 1);
            P.eq("listen.text_held", has(t, "held now: A4 C5 E5") ? 1 : 0, 1);
            const juce::var rw = v["racing_wheel"];
            P.eq("listen.racing_json", std::fabs(static_cast<double>(rw["steering"]["now"]) - 0.5) < 1e-3
                                           && static_cast<double>(rw["steering"]["min"]) == 0.0 && static_cast<int>(rw["gear"]["now"]) == 2
                                           && static_cast<int>(rw["buttons"]["now"]) == 1 && rw["brake"].isVoid() ? 1 : 0, 1);
            P.eq("listen.racing_text", has(t, "; racing wheel: steering +0.50, throttle 0.50, gear 2, buttons 0x1.") ? 1 : 0, 1);
        }
        else
        {
            P.near("listen.in_rms", static_cast<double>(lv["in"]["rms_p50_dbfs"]), -15.05, 0.3);
            P.near("listen.gain", static_cast<double>(lv["gain_db"]["p50"]), -6.0, 0.2);
            P.near("listen.out_peak", static_cast<double>(lv["out"]["peak_dbfs"]), -18.0, 0.3);
            P.near("listen.peak_1khz", top, 1000.0, 10.0);
            P.eq("listen.text_gain", has(t, "Gain in to out: -6.0 dB") ? 1 : 0, 1);
            P.eq("listen.no_racing", v["racing_wheel"].isVoid() && !has(t, "racing wheel") ? 1 : 0, 1);
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

    // ---- MONO: a mono program's voices, one at a time -------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        P.eq("mono.compiles", load(p, pg::singleFile("#pragma mono\n" + listenProgram())) ? 1 : 0, 1);
        Host host(p);
        host.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string j = listen(0.8, true);
        host.end();
        const juce::var voices = juce::JSON::parse(juce::String::fromUTF8(j.c_str()))["voices"];
        std::printf("listen, a mono program's voices: %s\n", juce::JSON::toString(voices, true).toRawUTF8());
        P.eq("mono.one_voice", static_cast<int>(voices["of"]) == 1 && voices["mono"].toString() == "always"
                                   && static_cast<int>(voices["most"]) >= 1 ? 1 : 0, 1);
        const std::string t = listen(0.2, false);
        P.eq("mono.text", has(t, " at once (of 1: mono), ") ? 1 : 0, 1);
        P.eq("mono.back", load(p, pg::singleFile(listenProgram())) ? 1 : 0, 1);
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
            pg::mcp::closeFd(fd);
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
        // The dials at the program's defaults: a run keeps a dial's value when the program before had one of its name
        // in that slot (the effect's default program, Init, has a mix at 100 %).
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (const pg::Control k = q.control(c); k.used)
                q.getParameters()[c]->setValueNotifyingHost(pg::normOf(k, k.def));
        q.poll();
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
        P.eq("since.no_hint_without_changes", read.rfind("program.fm: ", 0) == 0 && has(read, "\nmodule ") ? 1 : 0, 1);

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
        // the host moved a dial too: not "The user changed"
        P.eq("since.hint", hinted.rfind("(", 0) == 0 && has(hinted.substr(0, hinted.find('\n')), " things changed since the last MCP call here: ")
                               && has(hinted, "get_status with since shows them.)") ? 1 : 0, 1);
        // It names the first three, oldest first, with their values and without their times, and how many more there are.
        const std::string hintLine = hinted.substr(0, hinted.find('\n'));
        P.eq("since.hint_names_three", has(hintLine, " since the last MCP call here: the user moved cutoff 1.00 kHz -> 2.52 kHz; the host moved mix 50 % -> 30 %")
                                           && has(hintLine, "; the user edited program.fm; +") && has(hintLine, " more. get_status with since shows them.)") ? 1 : 0, 1);
        const std::string since = textOf(a.mcpCall("get_status", object({ { "since", juce::String(token) } })));
        std::printf("get_status since:\n%s", since.c_str());
        P.eq("since.drag_one_line", count(since, "the user moved cutoff 1.00 kHz -> 2.52 kHz\n"), 1);
        // the host's two moves of mix (30 %, then 80 %), a run between them (which may set dials): a line each, on either
        // side of the run's, never one net change that would read as if the run came after 80 %
        const std::size_t mixFirst = since.find("the host moved mix 50 % -> 30 %\n"), ranAt = since.find("the user ran it: "),
                          mixThen = since.find("the host moved mix 30 % -> 80 %\n");
        P.eq("since.host_moved", mixFirst != std::string::npos && ranAt != std::string::npos && mixThen != std::string::npos
                                     && mixFirst < ranAt && ranAt < mixThen && count(since, "moved mix ") == 2 ? 1 : 0, 1);
        P.eq("since.net_said", has(since, "oldest first (a dial's moves as one line, its net change):\n") ? 1 : 0, 1);
        P.eq("since.preset_no_move", has(since, "80 % -> 30 %") ? 0 : 1, 1);
        P.eq("since.edited", has(since, "the user edited program.fm\n") ? 1 : 0, 1);
        P.eq("since.ran", has(since, "the user ran it: it compiled, 3 dials\n") ? 1 : 0, 1);
        P.eq("since.version", has(since, "the user saved version 1 of \"Live probe\"\n") ? 1 : 0, 1);
        P.eq("since.presets", has(since, "the user saved preset \"Bright\"\n") && has(since, "the user applied preset \"Bright\" of \"Live probe\"\n") ? 1 : 0, 1);
        P.eq("since.no_you", has(since, " you ") ? 0 : 1, 1);
        P.eq("since.bypass", has(since, "the host bypassed the plugin, until ") ? 1 : 0, 1);
        P.eq("since.transport", has(since, "the host started playing, at 100.0 BPM, bar 2\n") ? 1 : 0, 1);
        P.eq("since.rate", has(since, "the host changed the sample rate: 48 kHz -> 44.1 kHz\n") ? 1 : 0, 1);
        P.eq("since.diff", has(since, "The files against their text at that token") && has(since, "program.fm (+1 -0):\n")
                               && has(since, "\n+// typed\n") ? 1 : 0, 1);
        P.eq("since.no_whole_state", has(since, "Dials:\n") ? 0 : 1, 1);
        const std::string again = textOf(a.mcpCall("get_status", object({ { "since", juce::String(tokenOf(since)) } })));
        P.eq("since.then_nothing", has(again, "Nothing changed since your last look.\n") && has(again, "The files are as they were at that token.\n") ? 1 : 0, 1);
        const std::string other = textOf(a.mcpCall("get_status", object({ { "since", "abc-1" } })));
        P.eq("since.unknown_token_whole_state", has(other, "That token is not this instance's") && has(other, "Dials:\n") ? 1 : 0, 1);

        // Each token's files are its own: two conversations of one server hold a token each from before an edit; the
        // first to ask moves nothing of the other's. What Claude Desktop wrote itself is no change, but in the diff.
        const std::string tokenA = tokenOf(textOf(a.mcpCall("get_status", none)));
        const std::string tokenB = tokenOf(textOf(a.mcpCall("get_status", none)));
        P.eq("since.tokens_own", !tokenA.empty() && tokenA != tokenB ? 1 : 0, 1);
        d = q.editorPort().draft();
        d.files[0].text += "// typed again\n";
        q.editorPort().setDraft(d);
        const std::string sinceB = textOf(a.mcpCall("get_status", object({ { "since", juce::String(tokenB) } })));
        const std::string sinceA = textOf(a.mcpCall("get_status", object({ { "since", juce::String(tokenA) } })));
        P.eq("since.per_token", has(sinceB, "\n+// typed again\n") && count(sinceA, "the user edited program.fm\n") == 1
                                    && has(sinceA, "\n+// typed again\n") ? 1 : 0, 1);
        d = q.editorPort().draft();
        const pg::ChatAgent::McpResult wrote = a.mcpCall("write_file", object({ { "name", "program.fm" },
                                                                                { "content", juce::String(d.files[0].text + "// Desktop's own\n") } }));
        const std::string own = textOf(a.mcpCall("get_status", object({ { "since", juce::String(tokenOf(sinceA)) } })));
        P.eq("since.own_edit_in_the_diff", !wrote.isError && has(own, "Nothing changed since your last look.\n")
                                               && has(own, "edits made through MCP since are in it too") && has(own, "\n+// Desktop's own\n") ? 1 : 0, 1);

        // A move just before a preset that leaves that dial alone is a move still: the preset's own dials are seen.
        P.eq("since.preset_cutoff_only", pg::projectstore::savePreset("Live probe", "Cutoff only", { { "cutoff", 5000.0f } }, saved.number, &why) ? 1 : 0, 1);
        const std::string beforePreset = tokenOf(own);
        q.getParameters()[mix]->setValueNotifyingHost(0.55f);   // the host's, with no poll before the preset
        P.eq("since.preset_applied_again", q.editorPort().applyPreset("Live probe", "Cutoff only", why) ? 1 : 0, 1);
        q.poll();
        const std::string preset = textOf(a.mcpCall("get_status", object({ { "since", juce::String(beforePreset) } })));
        std::printf("get_status since, a host move then a preset:\n%s", preset.c_str());
        P.eq("since.move_before_preset", has(preset, "the host moved mix ") && has(preset, "-> 55 %\n") && !has(preset, "moved cutoff")
                                             && has(preset, "the user applied preset \"Cutoff only\"") ? 1 : 0, 1);

        // A bypass that ends after a token was handed out: that token's reader reads its end, an older one's one line.
        const std::string beforeBypass = tokenOf(preset);
        block(q, true);
        const std::string whileBypassed = tokenOf(textOf(a.mcpCall("get_status", none)));
        block(q);
        const std::string bypassOld = textOf(a.mcpCall("get_status", object({ { "since", juce::String(beforeBypass) } })));
        const std::string bypassNew = textOf(a.mcpCall("get_status", object({ { "since", juce::String(whileBypassed) } })));
        P.eq("since.bypass_ended", has(bypassOld, "the host bypassed the plugin, until ") && !has(bypassOld, "it still is")
                                       && !has(bypassOld, "un-bypassed") ? 1 : 0, 1);
        P.eq("since.unbypass_after_token", has(bypassNew, "the host un-bypassed the plugin\n") && !has(bypassNew, "bypassed the plugin,") ? 1 : 0, 1);

        // ---- NET -------------------------------------------------------------------------------------------------------
        // A dial's moves as one line, its net change: the user's, the host's, the user's again, back where it started (a
        // token handed out between them, so the journal keeps three), at the first move's place, before the move of mix
        // the host made in between.
        {
            const std::string beforeNet = tokenOf(bypassNew);
            const float cutoffWas = q.getParameters()[cutoff]->getValue();
            knob.beginGesture();
            knob.setValue01(0.2f);
            q.poll();
            knob.endGesture();
            q.poll();
            tokenOf(textOf(a.mcpCall("get_status", none)));
            q.getParameters()[mix]->setValueNotifyingHost(0.4f);
            q.poll();
            q.getParameters()[cutoff]->setValueNotifyingHost(0.3f);
            q.poll();
            tokenOf(textOf(a.mcpCall("get_status", none)));
            knob.beginGesture();
            knob.setValue01(cutoffWas);
            q.poll();
            knob.endGesture();
            q.poll();
            const std::string net = textOf(a.mcpCall("get_status", object({ { "since", juce::String(beforeNet) } })));
            std::printf("get_status since, cutoff moved three times:\n%s", net.c_str());
            const std::size_t cutoffAt = net.find("the user and the host moved cutoff and back to ");
            const std::size_t mixAt = net.find("the host moved mix 55 % -> 40 %\n");
            P.eq("net.one_line", count(net, "cutoff"), 1);
            P.eq("net.back_where_it_was", cutoffAt != std::string::npos && has(net.substr(cutoffAt), " kHz (3 times)\n") ? 1 : 0, 1);
            P.eq("net.first_move_place", cutoffAt != std::string::npos && mixAt != std::string::npos && cutoffAt < mixAt ? 1 : 0, 1);

            // A preset between two moves of a dial sets it with no line of its own: a line on either side of the
            // preset's, the second from the preset's value (5 kHz), so that read oldest first the last says where it is.
            const std::string beforeBetween = tokenOf(net);
            knob.beginGesture();
            knob.setValue01(0.25f);
            q.poll();
            knob.endGesture();
            q.poll();
            P.eq("net.preset_between_applied", q.editorPort().applyPreset("Live probe", "Cutoff only", why) ? 1 : 0, 1);
            q.poll();
            knob.beginGesture();
            knob.setValue01(0.35f);
            q.poll();
            knob.endGesture();
            q.poll();
            const std::string between = textOf(a.mcpCall("get_status", object({ { "since", juce::String(beforeBetween) } })));
            std::printf("get_status since, cutoff moved, a preset, cutoff moved again:\n%s", between.c_str());
            const std::size_t firstAt = between.find("the user moved cutoff "), presetAt = between.find("the user applied preset \"Cutoff only\"");
            const std::size_t thenAt = presetAt == std::string::npos ? presetAt : between.find("the user moved cutoff 5.00 kHz -> ", presetAt);
            P.eq("net.preset_between", count(between, "moved cutoff ") == 2 && firstAt != std::string::npos && presetAt != std::string::npos
                                           && thenAt != std::string::npos && firstAt < presetAt && !has(between, "(2 times)") ? 1 : 0, 1);

            // files "changed" against a token this journal knows: a new file of one line is "1 line"
            const std::string beforeMemo = tokenOf(between);
            const pg::Draft withoutMemo = q.editorPort().draft();
            pg::Draft memo = withoutMemo;
            memo.files.push_back({ "memo.fm", "// memo\n" });
            q.editorPort().setDraft(memo);
            const std::string memoSince = textOf(a.mcpCall("get_status", object({ { "since", juce::String(beforeMemo) }, { "files", "changed" } })));
            P.eq("net.one_line_file", has(memoSince, "memo.fm: new since then, 1 line (read_file shows it).\n") ? 1 : 0, 1);
            q.editorPort().setDraft(withoutMemo);
        }

        // ---- LAYOUT ----------------------------------------------------------------------------------------------------
        // A netlist redrawn (its *@ lines alone, fminus/CirDoc.h) is no edit: listen says the editor's files play, and
        // since says the user redrew it.
        {
            pg::Draft dl = q.editorPort().draft();
            dl.files.push_back({ "pedal.cir", cir::written(pg::findLibraryFile("diodetone.cir")->source) });
            q.editorPort().setDraft(dl);
            q.editorPort().run(dl.files);
            q.settle();
            const std::string beforeRedraw = tokenOf(textOf(a.mcpCall("get_status", none)));
            dl = q.editorPort().draft();
            const std::string from = "*@ part R1 6,0";
            const std::size_t at = dl.files.back().text.find(from);
            if (at != std::string::npos)
                dl.files.back().text.replace(at, from.size(), "*@ part R1 6,-6");
            q.editorPort().setDraft(dl);
            const std::string heard = textOf(a.mcpCall("listen", object({ { "seconds", 0.1 } })));
            const std::string redrew = textOf(a.mcpCall("get_status", object({ { "since", juce::String(beforeRedraw) } })));
            std::printf("get_status since, a netlist redrawn:\n%s", redrew.c_str());
            P.eq("layout.listen_plays_draft", at != std::string::npos && has(heard, "What plays is the editor's files.") ? 1 : 0, 1);
            P.eq("layout.since_redrew", has(redrew, "the user redrew pedal.cir\n") && !has(redrew, "the user edited pedal.cir") ? 1 : 0, 1);
            // files "changed": a drawing's *@ lines are no diff (one line says so), a netlist line changed beside them is
            const std::string changed = textOf(a.mcpCall("get_status", object({ { "since", juce::String(beforeRedraw) }, { "files", "changed" } })));
            P.eq("layout.changed_drawing_only", has(changed, "pedal.cir: only its drawing (its *@ lines) changed.\n")
                                                    && !has(changed, "\n+*@") && !has(changed, "\n-*@") ? 1 : 0, 1);
            dl = q.editorPort().draft();
            const std::string r1 = "\nR1 ";
            const std::size_t line = dl.files.back().text.find(r1);
            const std::size_t end = line == std::string::npos ? line : dl.files.back().text.find('\n', line + 1);
            if (end != std::string::npos)
                dl.files.back().text.insert(end, "  ; a comment typed");
            q.editorPort().setDraft(dl);
            const std::string both = textOf(a.mcpCall("get_status", object({ { "since", juce::String(beforeRedraw) }, { "files", "changed" } })));
            std::printf("get_status since, files changed, a netlist line edited and the drawing moved:\n%s", both.c_str());
            P.eq("layout.changed_netlist_lines", end != std::string::npos && has(both, "pedal.cir (+1 -1):\n") && has(both, "; a comment typed")
                                                     && !has(both, "\n+*@") && !has(both, "\n-*@") && !has(both, "only its drawing") ? 1 : 0, 1);
        }

        // ---- REOPEN ----------------------------------------------------------------------------------------------------
        // The plugin opened again (a new processor from the session's state): a token from before is not the new
        // journal's, but the session kept its files' hashes, so get_status says which changed since, after the whole
        // state (an edit with read_file's versions, a netlist redrawn, a file added, one closed, one as it was); and
        // so does the next opening's; files "none" says nothing of them; a token of no record is not this instance's.
        {
            pg::Draft dq = q.editorPort().draft();
            dq.files.push_back({ "extra.fm", "// extra\n" });
            q.editorPort().setDraft(dq);
            const std::string beforeClose = tokenOf(textOf(a.mcpCall("get_status", none)));
            juce::MemoryBlock session;
            q.getStateInformation(session);
            pg::Processor r;
            r.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
            pg::ChatAgent& ra = r.chatAgent();
            pg::Draft dr = r.editorPort().draft();
            for (pg::SourceFile& f : dr.files)
            {
                if (f.name == "program.fm")
                    f.text += "// after the reopen\n";
                if (const std::size_t at = f.text.find("*@ part R1 6,-6"); f.name == "pedal.cir" && at != std::string::npos)
                    f.text.replace(at, 15, "*@ part R1 6,-12");
            }
            dr.files.erase(std::remove_if(dr.files.begin(), dr.files.end(), [](const pg::SourceFile& f) { return f.name == "extra.fm"; }),
                           dr.files.end());
            dr.files.push_back({ "notes.fm", "// notes\n" });
            r.editorPort().setDraft(dr);
            const std::string reopened = textOf(ra.mcpCall("get_status", object({ { "since", juce::String(beforeClose) } })));
            std::printf("get_status since a token from before the plugin was opened again:\n%s", reopened.c_str());
            const std::string readNow = resultOf(ra.mcpCall("read_file", object({ { "name", "program.fm" } })));
            // read_file's first line, "program.fm: 42 lines, version 1a2b3c4d.": "42 lines, version 1a2b3c4d"
            const std::string readHead = readNow.substr(0, readNow.find('\n'));
            const std::size_t colon = readHead.find(": ");
            const std::string linesNow = colon == std::string::npos || readHead.size() < colon + 3 || readHead.find(", version ") == std::string::npos
                                             ? std::string()
                                             : readHead.substr(colon + 2, readHead.size() - colon - 3);
            P.eq("reopen.said", has(reopened, "That token is from before the plugin was opened again") && has(reopened, "Dials:\n") ? 1 : 0, 1);
            P.eq("reopen.edited", !linesNow.empty() && has(reopened, "program.fm changed: now " + linesNow + " (then ") ? 1 : 0, 1);
            P.eq("reopen.redrawn", has(reopened, "pedal.cir: only its drawing (its *@ lines) changed.\n") ? 1 : 0, 1);
            P.eq("reopen.added", has(reopened, "notes.fm: new since then, 1 line.\n") ? 1 : 0, 1);
            P.eq("reopen.closed", has(reopened, "extra.fm is gone (closed or renamed).\n") ? 1 : 0, 1);
            P.eq("reopen.as_they_were", has(reopened, "As they were: panel.gui.\n") ? 1 : 0, 1);
            juce::MemoryBlock sessionAgain;
            r.getStateInformation(sessionAgain);
            pg::Processor t;
            t.setStateInformation(sessionAgain.getData(), static_cast<int>(sessionAgain.getSize()));
            const std::string twice = textOf(t.chatAgent().mcpCall("get_status", object({ { "since", juce::String(beforeClose) } })));
            P.eq("reopen.twice", has(twice, "That token is from before the plugin was opened again") && has(twice, "notes.fm: new since then") ? 1 : 0, 1);
            const std::string quiet = textOf(ra.mcpCall("get_status", object({ { "since", juce::String(beforeClose) }, { "files", "none" } })));
            P.eq("reopen.files_none", has(quiet, "That token is from before") && !has(quiet, "notes.fm: new") ? 1 : 0, 1);
            const std::string stranger = textOf(ra.mcpCall("get_status", object({ { "since", "abc-1" } })));
            P.eq("reopen.unknown_token", has(stranger, "That token is not this instance's") && !has(stranger, "since then") ? 1 : 0, 1);
        }

        // Cancels that come before a listen starts, two of them: each stops its own listen at once.
        a.mcpCancel(9001);
        a.mcpCancel(9002);
        const auto cancelledAt = std::chrono::steady_clock::now();
        const std::string early = resultOf(a.mcpCall("listen", object({ { "seconds", 3 } }), 9001));
        const double earlyS = std::chrono::duration<double>(std::chrono::steady_clock::now() - cancelledAt).count();
        P.eq("cancel.before_start_kept", has(early, "(stopped early)") ? 1 : 0, 1);
        P.le("cancel.before_start_quick_s", earlyS, 1.0);

        // New chat while a call of Claude Desktop's runs (view_panel, drawn at the next poll): the chat stays busy
        // while the call works, so no turn of its own can start beside it.
        // Its start waited for by the clock, not by a count of sleeps: a busy machine sleeps longer than it is asked.
        std::atomic<bool> drawn{ false };
        std::string viewed;
        std::thread desktop([&] {
            viewed = resultOf(a.mcpCall("view_panel", none, 9003));
            drawn.store(true);
        });
        const auto startBy = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!a.mcpWorking() && !drawn.load() && std::chrono::steady_clock::now() < startBy)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool working = a.mcpWorking();
        a.newChat();
        // New chat's stop also ends the call's wait for its picture (Processor::chatPicture: the thread that draws it
        // may be the one waiting for the stop), so the call may end before the chat's state is read, on a busy machine
        // most of all; the chat is idle then, rightly. What must never be is a call still at work beside a chat that is
        // not busy: its state read first, then the call's, which can only end.
        const bool busyAfterNew = a.chatState().busy;
        const bool workingAfter = a.mcpWorking();
        while (!drawn.load())
        {
            q.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        desktop.join();
        std::printf("newchat: the call working %d, the chat busy after New chat %d, the call working then %d; view_panel: "
                    "%s\n", working ? 1 : 0, busyAfterNew ? 1 : 0, workingAfter ? 1 : 0, viewed.substr(0, 200).c_str());
        P.eq("newchat.busy_while_desktop_works", working && (busyAfterNew || !workingAfter) ? 1 : 0, 1);
        P.eq("newchat.idle_after", !a.chatState().busy && !a.mcpWorking() ? 1 : 0, 1);
    }

    // ---- STOPPED ---------------------------------------------------------------------------------------------------
    {
        pg::CompileStatus s;
        s.revision = 1;
        s.ok = false;
        s.stopped = true;
        s.request = s.stoppedProgram = 7;
        s.errors = "program.fm:3:10: error: process() ran past its budget: a loop that never ends?\n";
        s.program = 7;                                   // nothing plays
        const std::string nothing = pg::runWords(s);
        s.program = 8;                                   // the program before, compiled again
        s.restored = pg::singleFile("module Effect {}\n");
        const std::string again = pg::runWords(s);
        s.program = 6;                                   // the program before, which never went
        s.restored.clear();
        const std::string on = pg::runWords(s);
        const std::string said = "ran it: it stopped (process() ran past its budget: a loop that never ends?); ";
        P.eq("stopped.nothing_plays", nothing == said + "nothing plays" ? 1 : 0, 1);
        P.eq("stopped.plays_again", again == said + "the program before plays again" ? 1 : 0, 1);
        P.eq("stopped.plays_on", on == said + "the program before plays on" ? 1 : 0, 1);
        std::printf("stopped: %s | %s | %s\n", nothing.c_str(), again.c_str(), on.c_str());
    }

    owned.reset();
    pg::projectstore::setDirectoryForTests({});
    projects.deleteRecursively();
    juce::File(juce::String(dir)).deleteRecursively();
    return P.finish();
}
