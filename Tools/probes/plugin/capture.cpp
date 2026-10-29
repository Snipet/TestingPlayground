// PROBE name=plugin.capture timeout=180
//
// plugin.capture: the recorder (plugin/Capture.h, the capture tool) as a host drives the processor, as plugin.process
// does: at 48 kHz, prepared for 256, in blocks of 256, 64, 333, 0, 1061 (over the announced: played in parts), 1, 512
// and 128 in turn, its default program playing, the freezer off (so two processors compare bit for bit). An effect gets
// a sine on the left and noise on the right; an instrument a phrase of notes (MIDI, and the panel's keyboard once), the
// sustain pedal, the mod wheel and the pitch wheel; a dial written by the host every tenth block; the host playing.
//   LAST      armed through the tool (mode last, 1 s) half a second in; stop (through the tool) keeps the last second
//             in whole blocks: their sizes the host's, their dials the values written, their transport the playhead's,
//             one program, the Engine's clock at the take's start; the input (an effect's) and the output bit for bit
//             what the probe gave and got; the events those sent, at their samples (the keyboard's at its block's
//             start); and the output a processor's that never records, bit for bit. status while it records says so
//   NEXT      mode next, a second: nothing recorded while nothing plays (status: waiting); it starts at the block of the
//             first note-on (an effect: the input's first sound), fills its second once and ends by itself;
//             the take bit for bit what came from that block on, the voices sounding at its start counted
//   TRANSPORT mode next, the host starting to play: it starts at that block
//   PREPARE   the host preparing again at the same settings before it starts: it goes on; once it has started: it ends
//             there, its take whole up to there, the Engine's clock counted from the last prepare
//   OVERFLOW  blocks of one sample: next ends when its blocks fill (flagged), what it kept bit for bit; last keeps fewer
//             than its seconds (flagged), bit for bit; an instrument's block of 40,000 events ends next before that
//             block (flagged); the processing never waits, its output a processor's that never records, bit for bit
//   FREEZE    a frozen copy playing (Compiler::freezeNow) stops the block after arm, one offered while armed is refused,
//             and after forget one plays again
//   SIZE      20 s at 48 kHz: what an armed recording holds (at most 25 MB), and the live status and stop each within
//             a quarter of a second (they copy what was published), the take its 20 s
//   FORGET    forget frees: a recording the audio thread never took at once, one it holds after its next block
//   TOOL      arm's words say to tell the user and that it stays in memory; status and stop are live (mcpLive), at most
//             25 lines, and say they are live; the tool is listed for MCP, read-only, titled in words; stop with nothing
//             armed, and an unknown action, are errors
//   PRIVACY   the processor's state holds nothing of a recording
#include "Probe.h"

#include "plugin/ChatAgent.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pg = playground;
namespace cap = playground::capture;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kPrepared = 256;
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr int kPattern[] = { 256, 64, 333, 0, 1061, 1, 512, 128 };

    // The host's playhead: playing at 120 BPM in 4/4 from where the probe says.
    class Head final : public juce::AudioPlayHead
    {
    public:
        bool known = true, playing = true;
        double ppq = 0.0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            if (!known)
                return {};
            PositionInfo pos;
            pos.setBpm(120.0);
            pos.setPpqPosition(ppq);
            pos.setPpqPositionOfLastBarStart(std::floor(ppq / 4.0) * 4.0);
            pos.setTimeSignature(TimeSignature{ 4, 4 });
            pos.setIsPlaying(playing);
            return pos;
        }
    };

    // An event as the processor hands it on (its toEvent), at its sample in the run.
    struct Sent
    {
        uint64_t sample = 0;
        pg::EventKind kind = pg::EventKind::noteOn;
        uint8_t note = 0;
        float velocity = 0.0f;
    };

    Sent sentOf(uint64_t sample, const juce::MidiMessage& m)
    {
        Sent s;
        s.sample = sample;
        if (m.isNoteOn())
        {
            s.kind = pg::EventKind::noteOn;
            s.note = static_cast<uint8_t>(m.getNoteNumber());
            s.velocity = static_cast<float>(m.getVelocity()) / 127.0f;
        }
        else if (m.isNoteOff())
        {
            s.kind = pg::EventKind::noteOff;
            s.note = static_cast<uint8_t>(m.getNoteNumber());
        }
        else if (m.isPitchWheel())
        {
            s.kind = pg::EventKind::pitchBend;
            s.velocity = std::clamp(static_cast<float>(m.getPitchWheelValue() - 8192) / 8191.0f, -1.0f, 1.0f);
        }
        else
        {
            s.kind = pg::EventKind::controller;
            s.note = static_cast<uint8_t>(m.getControllerNumber());
            s.velocity = static_cast<float>(m.getControllerValue()) / 127.0f;
        }
        return s;
    }

    // Two processors driven alike, one that records (`rec`) and one that never does (`ref`); what the probe gave and
    // got, block by block.
    struct Rig
    {
        pg::Processor rec, ref;
        Head head;
        std::vector<int> sizes;
        std::vector<uint64_t> starts;
        std::vector<std::array<float, pg::kMaxControls>> dials;
        std::vector<double> ppqs;
        std::vector<bool> playing;
        std::array<std::vector<float>, 2> in, out, outRef;
        std::vector<Sent> events;
        uint64_t pos = 0;
        bool sameAsRef = true;

        Rig()
        {
            for (pg::Processor* p : { &rec, &ref })
            {
                p->compiler().setFreezing(false);
                p->setRateAndBufferSizeDetails(kFs, kPrepared);
                p->prepareToPlay(kFs, kPrepared);
                p->setPlayHead(&head);
            }
        }
        ~Rig()
        {
            rec.setPlayHead(nullptr);
            ref.setPlayHead(nullptr);
        }

        // One host block of n samples: `x(c, sample)` the input, `midi` its events (offsets in the block), `ui` a note on
        // the panel's keyboard before it, `dial` a host write to P1 before it (< 0: none).
        template <class Input>
        void block(int n, Input&& x, const std::vector<std::pair<int, juce::MidiMessage>>& midi, int ui = -1, float dial = -1.0f)
        {
            if (dial >= 0.0f)
                for (pg::Processor* p : { &rec, &ref })
                    p->getParameters()[0]->setValueNotifyingHost(dial);
            head.ppq = static_cast<double>(pos) * 2.0 / kFs;
            if (ui >= 0)
            {
                rec.editorPort().playNote(ui, 0.75f);
                ref.editorPort().playNote(ui, 0.75f);
                events.push_back({ pos, pg::EventKind::noteOn, static_cast<uint8_t>(ui), 0.75f });
            }
            sizes.push_back(n);
            starts.push_back(pos);
            ppqs.push_back(head.ppq);
            playing.push_back(head.known && head.playing);
            std::array<float, pg::kMaxControls> d{};
            for (int c = 0; c < pg::kMaxControls; ++c)
                d[static_cast<std::size_t>(c)] = rec.getParameters()[c]->getValue();
            dials.push_back(d);
            juce::MidiBuffer mb;
            for (const auto& [at, m] : midi)
            {
                mb.addEvent(m, at);
                events.push_back(sentOf(pos + static_cast<uint64_t>(at), m));
            }
            juce::AudioBuffer<float> a(2, std::max(n, 1)), b(2, std::max(n, 1));
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const float v = x(c, pos + static_cast<uint64_t>(i));
                    a.setSample(c, i, v);
                    b.setSample(c, i, v);
                    in[static_cast<std::size_t>(c)].push_back(v);
                }
            juce::AudioBuffer<float> va(a.getArrayOfWritePointers(), 2, n), vb(b.getArrayOfWritePointers(), 2, n);
            rec.processBlock(va, mb);
            ref.processBlock(vb, mb);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const float y = va.getSample(c, i), z = vb.getSample(c, i);
                    out[static_cast<std::size_t>(c)].push_back(y);
                    outRef[static_cast<std::size_t>(c)].push_back(z);
                    sameAsRef = sameAsRef && std::memcmp(&y, &z, sizeof y) == 0;
                }
            pos += static_cast<uint64_t>(n);
        }

        // The index of the block that starts at `sample`, or -1.
        int blockAt(uint64_t sample) const
        {
            for (std::size_t k = 0; k < starts.size(); ++k)
                if (starts[k] == sample && (k + 1 == starts.size() || starts[k + 1] != sample || sizes[k] > 0))
                    return static_cast<int>(k);
            return -1;
        }
    };

    bool sameFloats(const std::vector<float>& a, const float* b, std::size_t n)
    {
        return a.size() == n && (n == 0 || std::memcmp(a.data(), b, n * sizeof(float)) == 0);
    }

    // The take against what the rig gave and got from its block `first` on: sizes, dials, transport, samples, events.
    struct Match
    {
        bool sizes = true, dials = true, transport = true, input = true, output = true, events = true, program = true;
        bool all() const { return sizes && dials && transport && input && output && events && program; }
    };

    Match matchTake(const cap::Take& t, const Rig& r, std::size_t first)
    {
        Match m;
        const uint64_t base = r.starts[first];
        m.sizes = t.blocks.size() + first <= r.sizes.size() && !t.blocks.empty();
        for (std::size_t k = 0; m.sizes && k < t.blocks.size(); ++k)
        {
            const cap::Block& b = t.blocks[k];
            const std::size_t j = first + k;
            m.sizes = m.sizes && static_cast<int>(b.size) == r.sizes[j] && b.sample == r.starts[j] - base;
            m.dials = m.dials && std::memcmp(b.dials.data(), r.dials[j].data(), sizeof(float) * pg::kMaxControls) == 0;
            m.transport = m.transport && b.has(cap::Block::kKnown) == r.head.known && b.has(cap::Block::kPlaying) == r.playing[j]
                       && b.beat == (r.head.known ? r.ppqs[j] : 0.0) && b.bpm == 120.0 && b.beatsPerBar == 4 && b.beatUnit == 4
                       && !b.has(cap::Block::kBypassed) && !b.has(cap::Block::kFrozen);
            m.program = m.program && b.program != 0 && b.program == t.blocks.front().program;
        }
        if (!m.sizes)
            return m;
        const std::size_t n = static_cast<std::size_t>(t.blocks.back().sample + t.blocks.back().size);
        for (std::size_t c = 0; c < 2; ++c)
        {
            m.output = m.output && base + n <= r.out[c].size() && sameFloats(t.out[c], r.out[c].data() + base, n);
            if (t.input)
                m.input = m.input && sameFloats(t.in[c], r.in[c].data() + base, n);
        }
        std::vector<Sent> want;
        for (const Sent& s : r.events)
            if (s.sample >= base && s.sample - base < n + (t.blocks.back().size == 0 ? 1 : 0))
                want.push_back(s);
        m.events = want.size() == t.events.size();
        for (std::size_t k = 0; m.events && k < want.size(); ++k)
        {
            const cap::Note& x = t.events[k];
            m.events = x.sample == want[k].sample - base && x.kind == want[k].kind && x.note == want[k].note
                    && x.velocity == want[k].velocity;
        }
        return m;
    }

    // The first block of the rig's run from `from` on whose samples to the end are at most `seconds`.
    std::size_t lastSeconds(const Rig& r, std::size_t from, double seconds)
    {
        const auto keep = static_cast<uint64_t>(std::llround(seconds * kFs));
        std::size_t k = from;
        while (k < r.starts.size() && r.pos - r.starts[k] > keep)
            ++k;
        return k;
    }

    float sine(int c, uint64_t i)
    {
        if (c == 0)
            return static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(i) / kFs));
        uint64_t h = (i + 1) * 6364136223846793005ull + 1442695040888963407ull;   // noise, the same every run
        h ^= h >> 33;
        return static_cast<float>(static_cast<double>(h % 20001u) / 10000.0 - 1.0) * 0.1f;
    }

    float silence(int, uint64_t) { return 0.0f; }

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : fields)
            o->setProperty(k, v);
        return juce::var(o);
    }

    std::string textOf(const pg::ChatAgent::McpResult& r)
    {
        std::string out;
        if (const juce::Array<juce::var>* c = r.content.getArray())
            for (const juce::var& b : *c)
                if (b["type"].toString() == "text")
                    out += b["text"].toString().toStdString();
        return out;
    }

    int lines(const std::string& t)
    {
        return static_cast<int>(std::count(t.begin(), t.end(), '\n')) + (t.empty() || t.back() == '\n' ? 0 : 1);
    }

    bool has(const std::string& t, const char* part) { return t.find(part) != std::string::npos; }

    pg::ChatAgent::McpResult tool(pg::Processor& p, juce::var args)
    {
        return p.chatAgent().mcpCall("capture", args);
    }

    // The phrase an instrument plays from block k (its events in the block of n samples).
    std::vector<std::pair<int, juce::MidiMessage>> phrase(std::size_t k, int n)
    {
        std::vector<std::pair<int, juce::MidiMessage>> m;
        if (!kInstrument || n == 0)
            return m;
        const int note = 48 + static_cast<int>((k * 7) % 25);
        if (k % 6 == 0)
            m.push_back({ n / 3, juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(40 + (k * 13) % 80)) });
        if (k % 6 == 3)
            m.push_back({ 0, juce::MidiMessage::noteOff(1, 48 + static_cast<int>(((k - 3) * 7) % 25)) });
        if (k % 40 == 5)
            m.push_back({ n - 1, juce::MidiMessage::controllerEvent(1, 64, 127) });
        if (k % 40 == 25)
            m.push_back({ 0, juce::MidiMessage::controllerEvent(1, 64, 0) });
        if (k % 9 == 4)
            m.push_back({ n / 2, juce::MidiMessage::pitchWheel(1, static_cast<int>((k * 997) % 16384)) });
        if (k % 11 == 2)
            m.push_back({ std::max(0, n - 2), juce::MidiMessage::controllerEvent(1, 1, static_cast<int>((k * 5) % 128)) });
        return m;
    }

    int patternAt(std::size_t k) { return kPattern[k % std::size(kPattern)]; }
}

PROBE(plugin, capture)
{
    const funkgui::HeadlessGuiScope gui;

    // ---- TOOL (part): listed for MCP, read-only, titled; stop with nothing armed and an unknown action -----------------
    {
        bool listed = false;
        const juce::var tools = pg::ChatAgent::mcpTools();
        if (const juce::Array<juce::var>* t = tools.getArray())
            for (const juce::var& x : *t)
                if (x["name"].toString() == "capture")
                    listed = static_cast<bool>(x["annotations"]["readOnlyHint"]) && x["title"].toString() == "Record what you play"
                          && x["description"].toString().contains("Tell the user");
        P.eq("tool.listed", listed ? 1 : 0, 1);
        P.eq("tool.live", pg::ChatAgent::mcpLive("capture", object({ { "action", "stop" } }))
                              && pg::ChatAgent::mcpLive("capture", object({ { "action", "status" } }))
                              && !pg::ChatAgent::mcpLive("capture", object({ { "action", "arm" } }))
                              && !pg::ChatAgent::mcpLive("capture", object({ { "action", "forget" } })) ? 1 : 0, 1);
        Rig r;
        const pg::ChatAgent::McpResult s = tool(r.rec, object({ { "action", "stop" } }));
        const pg::ChatAgent::McpResult u = tool(r.rec, object({ { "action", "play" } }));
        std::printf("stop with nothing: %s\nunknown: %s\n", textOf(s).c_str(), textOf(u).c_str());
        P.eq("tool.stop_nothing_error", s.isError ? 1 : 0, 1);
        P.eq("tool.unknown_error", u.isError ? 1 : 0, 1);
    }

    // ---- LAST -----------------------------------------------------------------------------------------------------------
    std::size_t takeBytes = 0;
    {
        Rig r;
        std::size_t armedAt = 0;
        std::string statusText, armText, stopText;
        for (std::size_t k = 0; r.pos < static_cast<uint64_t>(2.5 * kFs); ++k)
        {
            if (r.pos >= static_cast<uint64_t>(0.5 * kFs) && armedAt == 0)
            {
                const pg::ChatAgent::McpResult a = tool(r.rec, object({ { "action", "arm" }, { "seconds", 1 } }));
                armText = textOf(a);
                P.eq("last.armed", a.isError ? 0 : 1, 1);
                armedAt = k;
            }
            if (armedAt != 0 && k == armedAt + 60 && statusText.empty())
                statusText = textOf(tool(r.rec, object({ { "action", "status" } })));
            const int n = patternAt(k);
            r.block(n, kInstrument ? silence : sine, phrase(k, n), kInstrument && k == armedAt + 30 ? 77 : -1,
                    k % 10 == 7 ? static_cast<float>((k * 37) % 100) / 100.0f : -1.0f);
        }
        const pg::ChatAgent::McpResult st = tool(r.rec, object({ { "action", "stop" } }));
        stopText = textOf(st);
        std::printf("arm: %s\nstatus while recording:\n%s\nstop:\n%s", armText.c_str(), statusText.c_str(), stopText.c_str());
        const cap::Reply taken = r.rec.recorder().act(cap::Action::take, {});
        P.eq("last.kept", taken.state == cap::State::kept && taken.take != nullptr ? 1 : 0, 1);
        if (taken.take != nullptr)
        {
            const cap::Take& t = *taken.take;
            const std::size_t first = lastSeconds(r, armedAt, 1.0);
            const Match m = matchTake(t, r, first);
            std::printf("last: kept %zu blocks (%.4f s) from block %zu, the probe's from %zu; %zu events; clock %llu\n",
                        t.blocks.size(), t.seconds(), armedAt, first, t.events.size(), static_cast<unsigned long long>(t.clock));
            P.eq("last.whole_blocks", t.blocks.size() == r.sizes.size() - first ? 1 : 0, 1);
            P.le("last.seconds", t.seconds(), 1.0);
            P.ge("last.seconds_min", t.seconds(), 1.0 - 1061.0 / kFs);
            P.eq("last.sizes", m.sizes ? 1 : 0, 1);
            P.eq("last.dials", m.dials ? 1 : 0, 1);
            P.eq("last.transport", m.transport ? 1 : 0, 1);
            P.eq("last.program", m.program ? 1 : 0, 1);
            P.eq("last.output_bits", m.output ? 1 : 0, 1);
            P.eq("last.input_bits", m.input ? 1 : 0, 1);
            P.eq("last.events", m.events ? 1 : 0, 1);
            if constexpr (kInstrument)
                P.ge("last.events_some", static_cast<double>(t.events.size()), 20.0);
            P.eq("last.clock", t.clock == r.starts[first] ? 1 : 0, 1);
            P.eq("last.rate_part", t.rate == kFs && t.part == kPrepared ? 1 : 0, 1);
            P.eq("last.end", t.end == cap::End::stopped && t.start == cap::Start::arm ? 1 : 0, 1);
            P.eq("last.programs_files", t.programs.size() == 1 && !t.programs.front().second.empty() ? 1 : 0, 1);
            takeBytes = t.bytes;
        }
        P.eq("last.changes_nothing", r.sameAsRef ? 1 : 0, 1);
        P.eq("tool.arm_words", has(armText, "Tell the user") && has(armText, "memory") ? 1 : 0, 1);
        P.eq("tool.status_live", has(statusText, "Recording now (live") && lines(statusText) <= 25 ? 1 : 0, 1);
        P.eq("tool.stop_words", has(stopText, "(live") && has(stopText, "Alerts:") && has(stopText, "CPU:")
                                    && has(stopText, kInstrument ? "Notes: " : "In: ") && has(stopText, "Dials moved: ")
                                    && lines(stopText) <= 25 && !st.isError ? 1 : 0, 1);
        std::printf("stop: %d lines\n", lines(stopText));

        // FORGET: the ring the audio thread held goes after its next block; the take stays until forget.
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> b(2, 64);
        b.clear();
        r.rec.processBlock(b, midi);
        r.rec.poll();
        P.eq("forget.ring_handed_back", r.rec.recorder().rings(), 0);
        P.eq("forget.take_kept", r.rec.recorder().bytes() == takeBytes && takeBytes > 0 ? 1 : 0, 1);
        const pg::ChatAgent::McpResult f = tool(r.rec, object({ { "action", "forget" } }));
        std::printf("forget: %s", textOf(f).c_str());
        P.eq("forget.freed", r.rec.recorder().bytes() == 0 && r.rec.recorder().rings() == 0 ? 1 : 0, 1);
        const pg::ChatAgent::McpResult after = tool(r.rec, object({ { "action", "status" } }));
        P.eq("forget.off", has(textOf(after), "The recorder is off") ? 1 : 0, 1);

        // PRIVACY: the state holds nothing of it (a second of audio is 384 KB).
        juce::MemoryBlock state;
        r.rec.getStateInformation(state);
        P.le("privacy.state_bytes", static_cast<double>(state.getSize()), 100000.0);
    }

    // ---- NEXT -----------------------------------------------------------------------------------------------------------
    {
        Rig r;
        r.head.known = false;                                   // no transport: only a note (a sound) starts it
        const pg::ChatAgent::McpResult a = tool(r.rec, object({ { "action", "arm" }, { "mode", "next" }, { "seconds", 1 } }));
        P.eq("next.armed", a.isError ? 0 : 1, 1);
        std::size_t startBlock = 0;
        uint64_t sound = ~uint64_t{ 0 };                          // an effect's input starts there
        std::string waiting;
        for (std::size_t k = 0; r.pos < static_cast<uint64_t>(1.6 * kFs); ++k)
        {
            const int n = patternAt(k);
            const bool before = r.pos < static_cast<uint64_t>(0.4 * kFs);
            if (!before && startBlock == 0 && n > 0)
            {
                startBlock = k;
                sound = r.pos + static_cast<uint64_t>(n / 2);
            }
            if (k == 20)
                waiting = textOf(tool(r.rec, object({ { "action", "status" } })));
            std::vector<std::pair<int, juce::MidiMessage>> midi;
            if (kInstrument && before && k % 7 == 3 && n > 0)
                midi.push_back({ 0, juce::MidiMessage::controllerEvent(1, 1, 30) });   // no note: nothing starts
            if (kInstrument && k == startBlock && startBlock != 0)
                midi.push_back({ n / 2, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)) });
            else if (kInstrument && !before)
                midi = phrase(k, n);
            r.block(n, [&](int c, uint64_t i) { return !kInstrument && i >= sound ? sine(c, i) : 0.0f; }, midi);
        }
        std::printf("next, waiting: %s", waiting.c_str());
        P.eq("next.waiting", has(waiting, "waiting for") ? 1 : 0, 1);
        r.rec.poll();
        const pg::ChatAgent::McpResult st = tool(r.rec, object({ { "action", "status" } }));
        std::printf("next, status after:\n%s", textOf(st).c_str());
        const cap::Reply status = r.rec.recorder().act(cap::Action::status, {});
        P.eq("next.ended_itself", status.state == cap::State::ended && status.take != nullptr && status.take->end == cap::End::full ? 1 : 0, 1);
        P.eq("next.own_code_after", r.rec.engine().ownCode() ? 0 : 1, 1);
        tool(r.rec, object({ { "action", "stop" } }));
        const cap::Reply taken = r.rec.recorder().act(cap::Action::take, {});
        if (taken.take != nullptr)
        {
            const cap::Take& t = *taken.take;
            const Match m = matchTake(t, r, startBlock);
            std::printf("next: %zu blocks (%.4f s) from block %zu (sample %llu), started by %d\n", t.blocks.size(), t.seconds(),
                        startBlock, static_cast<unsigned long long>(r.starts[startBlock]), static_cast<int>(t.start));
            P.eq("next.start", t.start == (kInstrument ? cap::Start::note : cap::Start::sound) ? 1 : 0, 1);
            P.eq("next.sizes", m.sizes ? 1 : 0, 1);
            P.eq("next.bits", m.output && m.input && m.dials && m.events ? 1 : 0, 1);
            P.le("next.seconds", t.seconds(), 1.0);
            P.ge("next.seconds_min", t.seconds(), 1.0 - 1061.0 / kFs);
            P.eq("next.clock", t.clock == r.starts[startBlock] ? 1 : 0, 1);
            P.eq("next.voices_at_start", t.voicesAtStart, 0);
        }
        else
            P.eq("next.kept", 0, 1);
        P.eq("next.changes_nothing", r.sameAsRef ? 1 : 0, 1);
    }

    // ---- TRANSPORT ------------------------------------------------------------------------------------------------------
    {
        Rig r;
        r.head.playing = false;
        tool(r.rec, object({ { "action", "arm" }, { "mode", "next" }, { "seconds", 1 } }));
        std::size_t startBlock = 0;
        for (std::size_t k = 0; k < 120; ++k)
        {
            if (k == 50)
            {
                r.head.playing = true;
                startBlock = k;
            }
            r.block(patternAt(k), silence, {});
        }
        const cap::Reply s = r.rec.recorder().act(cap::Action::stop, {});
        P.eq("transport.start", s.take != nullptr && s.take->start == cap::Start::transport ? 1 : 0, 1);
        if (s.take != nullptr)
            P.eq("transport.from_its_block", matchTake(*s.take, r, startBlock).sizes ? 1 : 0, 1);
    }

    // ---- PREPARE: the host prepares again ------------------------------------------------------------------------------
    {
        Rig r;
        r.head.known = false;
        const auto prepare = [&] {
            for (pg::Processor* p : { &r.rec, &r.ref })
                p->prepareToPlay(kFs, kPrepared);
        };
        r.rec.recorder().act(cap::Action::arm, { cap::Mode::next, 1.0, false });
        for (std::size_t k = 0; k < 5; ++k)
            r.block(256, silence, {});
        prepare();                                              // before it starts, the same settings: it goes on
        const uint64_t preparedAt = r.pos;
        const std::size_t startBlock = r.sizes.size();
        for (std::size_t k = 0; k < 40; ++k)
        {
            std::vector<std::pair<int, juce::MidiMessage>> midi;
            if (kInstrument && k == 0)
                midi.push_back({ 10, juce::MidiMessage::noteOn(1, 62, static_cast<juce::uint8>(90)) });
            r.block(256, [&](int c, uint64_t i) { return kInstrument ? 0.0f : sine(c, i); }, midi);
        }
        const cap::Reply going = r.rec.recorder().act(cap::Action::status, {});
        P.eq("prepare.same_goes_on", going.state == cap::State::recording ? 1 : 0, 1);
        prepare();                                              // once it has started: it ends there
        r.block(256, silence, {});
        const cap::Reply ended = r.rec.recorder().act(cap::Action::status, {});
        P.eq("prepare.ends", ended.state == cap::State::ended && ended.take != nullptr && ended.take->end == cap::End::prepared ? 1 : 0, 1);
        const cap::Reply s = r.rec.recorder().act(cap::Action::stop, {});
        P.eq("prepare.take_to_there", s.take != nullptr && s.take->blocks.size() == 40 && s.take->end == cap::End::prepared
                                          && s.take->clock == r.starts[startBlock] - preparedAt
                                          && matchTake(*s.take, r, startBlock).all() ? 1 : 0, 1);
    }

    // ---- OVERFLOW -------------------------------------------------------------------------------------------------------
    {
        // Blocks of one sample: next fills its blocks' ring first.
        Rig r;
        r.head.known = false;
        r.rec.recorder().act(cap::Action::arm, { cap::Mode::next, 1.0, false });
        std::size_t startBlock = 0;
        for (std::size_t k = 0; k < 2200; ++k)
        {
            std::vector<std::pair<int, juce::MidiMessage>> midi;
            if (k == 10)
            {
                startBlock = k;
                if (kInstrument)
                    midi.push_back({ 0, juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(90)) });
            }
            r.block(1, [&](int c, uint64_t i) { return k >= 10 ? sine(c, i) : 0.0f; }, midi);
        }
        const cap::Reply s = r.rec.recorder().act(cap::Action::stop, {});
        const bool ok = s.take != nullptr;
        std::printf("overflow, next: %zu blocks kept, trimmed %d, end %d\n", ok ? s.take->blocks.size() : 0, ok ? s.take->trimmed : -1,
                    ok ? static_cast<int>(s.take->end) : -1);
        P.eq("overflow.next_blocks_flagged", ok && (s.take->trimmed & cap::blocksLimit) != 0 && s.take->end == cap::End::full ? 1 : 0, 1);
        P.eq("overflow.next_bits", ok && matchTake(*s.take, r, startBlock).all() ? 1 : 0, 1);
        P.le("overflow.next_short", ok ? s.take->seconds() : 1.0, 0.1);
        P.eq("overflow.next_changes_nothing", r.sameAsRef ? 1 : 0, 1);
    }
    {
        // last with blocks of one sample: it keeps fewer than its second, flagged, bit for bit.
        Rig r;
        r.rec.recorder().act(cap::Action::arm, { cap::Mode::last, 1.0, false });
        for (std::size_t k = 0; k < 3000; ++k)
            r.block(1, sine, {});
        const cap::Reply s = r.rec.recorder().act(cap::Action::stop, {});
        const bool ok = s.take != nullptr && !s.take->blocks.empty();
        const std::size_t first = ok ? r.sizes.size() - s.take->blocks.size() : 0;
        P.eq("overflow.last_flagged", ok && (s.take->trimmed & cap::blocksLimit) != 0 ? 1 : 0, 1);
        P.eq("overflow.last_bits", ok && matchTake(*s.take, r, first).all() ? 1 : 0, 1);
        P.eq("overflow.last_changes_nothing", r.sameAsRef ? 1 : 0, 1);
    }
    if constexpr (kInstrument)
    {
        // A block of 40,000 events: next ends before it, flagged; the block plays all the same.
        Rig r;
        r.head.known = false;
        r.rec.recorder().act(cap::Action::arm, { cap::Mode::next, 2.0, false });
        for (std::size_t k = 0; k < 12; ++k)
        {
            std::vector<std::pair<int, juce::MidiMessage>> midi;
            if (k == 2)
                midi.push_back({ 0, juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(90)) });
            if (k == 5)
                for (int e = 0; e < 40000; ++e)
                    midi.push_back({ e % 256, juce::MidiMessage::controllerEvent(1, 1, e % 128) });
            r.block(256, silence, midi);
        }
        const cap::Reply s = r.rec.recorder().act(cap::Action::stop, {});
        const bool ok = s.take != nullptr;
        P.eq("overflow.events_flagged", ok && (s.take->trimmed & cap::eventsLimit) != 0 && s.take->blocks.size() == 3 ? 1 : 0, 1);
        P.eq("overflow.events_bits", ok && matchTake(*s.take, r, 2).all() ? 1 : 0, 1);
        P.eq("overflow.events_changes_nothing", r.sameAsRef ? 1 : 0, 1);
    }

    // ---- FREEZE ---------------------------------------------------------------------------------------------------------
    {
        Rig r;
        const auto blocks = [&](int k) {
            for (int i = 0; i < k; ++i)
                r.block(256, kInstrument ? silence : sine, {});
        };
        blocks(4);
        const bool offered = r.rec.compiler().freezeNow();
        blocks(1);
        const bool before = r.rec.engine().frozen();
        tool(r.rec, object({ { "action", "arm" } }));
        blocks(1);
        const bool armed = r.rec.engine().frozen();
        const bool offeredArmed = r.rec.compiler().freezeNow();
        blocks(2);
        const bool armedAfterOffer = r.rec.engine().frozen();
        const cap::Reply st = r.rec.recorder().act(cap::Action::status, {});
        bool flags = st.take != nullptr && !st.take->blocks.empty();
        if (flags)
            for (const cap::Block& b : st.take->blocks)
                flags = flags && !b.has(cap::Block::kFrozen);
        tool(r.rec, object({ { "action", "forget" } }));
        r.rec.poll();
        blocks(1);
        const bool again = r.rec.compiler().freezeNow();
        blocks(1);
        const bool after = r.rec.engine().frozen();
        std::printf("freeze: offered %d, plays %d; armed: plays %d, offered %d, plays %d; after forget: offered %d, plays %d\n",
                    offered, before, armed, offeredArmed, armedAfterOffer, again, after);
        P.eq("freeze.plays_before", offered && before ? 1 : 0, 1);
        P.eq("freeze.dropped_when_armed", armed ? 0 : 1, 1);
        P.eq("freeze.refused_while_armed", armedAfterOffer ? 0 : 1, 1);
        P.eq("freeze.take_own_code", flags ? 1 : 0, 1);
        P.eq("freeze.plays_after_forget", again && after ? 1 : 0, 1);
    }

    // ---- SIZE: 20 s at 48 kHz in blocks of 512: what it holds, how long the live calls take ----------------------------
    {
        pg::Processor p;
        p.compiler().setFreezing(false);
        p.setRateAndBufferSizeDetails(kFs, 512);
        p.prepareToPlay(kFs, 512);
        const cap::Reply armed = p.recorder().act(cap::Action::arm, { cap::Mode::last, 20.0, false });
        juce::AudioBuffer<float> b(2, 512);
        juce::MidiBuffer midi;
        for (uint64_t pos = 0; pos < static_cast<uint64_t>(21.0 * kFs); pos += 512)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    b.setSample(c, i, kInstrument ? 0.0f : sine(c, pos + static_cast<uint64_t>(i)));
            midi.clear();
            if (kInstrument && pos % 24576 == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60 + static_cast<int>((pos / 24576) % 12), static_cast<juce::uint8>(100)), 0);
            p.processBlock(b, midi);
        }
        const auto ms = [](auto t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        auto t0 = std::chrono::steady_clock::now();
        const std::string status = textOf(tool(p, object({ { "action", "status" } })));
        const double statusMs = ms(t0);
        t0 = std::chrono::steady_clock::now();
        const cap::Reply stopped = p.recorder().act(cap::Action::stop, {});
        const double stopMs = ms(t0);
        const double heldMb = static_cast<double>(armed.bytes) / 1e6;
        const double takeMb = stopped.take != nullptr ? static_cast<double>(stopped.take->bytes) / 1e6 : 0.0;
        std::printf("size: armed for 20 s it holds %.1f MB; status (its report) took %.1f ms, stop %.1f ms; the take %.2f s, %.1f MB\n",
                    heldMb, statusMs, stopMs, stopped.take != nullptr ? stopped.take->seconds() : 0.0, takeMb);
        P.le("size.held_mb", heldMb, 25.0);
        P.le("size.status_ms", statusMs, 250.0);
        P.le("size.stop_ms", stopMs, 250.0);
        P.ge("size.take_seconds", stopped.take != nullptr ? stopped.take->seconds() : 0.0, 20.0 - 512.0 / kFs);
    }

    // ---- FORGET: one the audio thread never took goes at once -----------------------------------------------------------
    {
        Rig r;
        r.rec.recorder().act(cap::Action::arm, {});
        P.eq("forget.armed_ring", r.rec.recorder().rings(), 1);
        r.rec.recorder().act(cap::Action::forget, {});
        P.eq("forget.untaken_at_once", r.rec.recorder().rings() == 0 && r.rec.recorder().bytes() == 0 ? 1 : 0, 1);
        r.rec.recorder().act(cap::Action::arm, {});
        r.block(256, silence, {});
        r.rec.recorder().act(cap::Action::forget, {});
        const int held = r.rec.recorder().rings();
        r.block(256, silence, {});
        r.rec.poll();
        P.eq("forget.held_then_freed", held == 1 && r.rec.recorder().rings() == 0 ? 1 : 0, 1);
        P.eq("forget.own_code_off", r.rec.engine().ownCode() ? 0 : 1, 1);
    }
    return P.finish();
}
