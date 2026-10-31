// PROBE name=dsp.meters timeout=120
//
// dsp.meters: what the Engine publishes for the panel's displays (Engine::Meters, Engine::scope), at 48 kHz in blocks
// of 256, a program's #pragma panel displays naming its fields:
//   LEVELS  an effect: a sine at 0.8 in, at half out: the input's peak 0.8 and RMS 0.566 (left and right), the
//           output's 0.4 and 0.283, within 1 %; an instrument: a voice at 0.25 out
//   FALL    after the sound stops the peaks fall 20 dB a second (half a second: 10 dB), and the RMS (its power
//           smoothed over 50 ms) more than 40 dB in half a second
//   TAPS    the fields the displays show, in display order: a float, an int, a bool, as the program wrote them last
//   VOICE   an instrument's fields come from its newest sounding voice (a second note's while both sound, the first's
//           after the second's voice has ended), 0 once none sounds
//   SCOPE   the last samples of the output are the Engine's output, bit for bit (mono: the sides' mean); an effect's
//           input likewise
//   FRAME   the frame counts the blocks (a last, shorter one too)
//   PROGRAM the meters name the program their fields are of (Patch::program): the one that played the last block,
//           not one offered and not yet taken
//   READERS the levels and the scopes are measured only while someone reads them (Engine::addReader; the rows above
//           have a reader, as a panel is): with none the levels read 0 and the scope stands still while the fields
//           and the frame go on; a reader back, the next block's levels are its own (its peak and RMS, not smoothed up
//           from 0), and the scope holds that block, silence before it
//   LISTEN  what a tool that listens reads besides (Meters' second part, Engine::blocks): the samples counted; the host's
//           transport as the Engine follows it (known, 90 BPM, 3/4, playing, the position at the last block's first
//           sample); a block a ring entry, oldest first, each with its length, a finite share above 0 and the metering
//           flag, the next read starting where this one ended; a block whose dial glides flagged moving, a still one
//           not; the output guard's block flagged. An instrument's: three note-ons counted (the last's key and
//           velocity), the keys held as bits and a count, the voices sounding, one held by the pedal after its key
//           went up, none released or fading; a new program starting the held keys again counts no note-on, and the
//           old program's voices fade; a seventeenth note steals one (fading at the block's end); a voice gone
//           non-finite dropped, counted, with its key and its sample, its block flagged. An effect's: no notes, no voices
//   PROBES  the schematic's live probes (Engine.h's "Probes", Patch.h's) on Play this circuit's program for the library's
//           diodetone.cir (fminus/CirTemplates.h: an effect's amp[2] over its input, a synth's saw through its ADSR
//           into the circuit), the probe on element 0 (amp[0], the left channel) or the newest voice:
//             bits     the output with four slots armed is the output with none, bit for bit, in host blocks of 64, 333
//                      and 4096 (a 1 s sine of 0.5 V at 1 kHz in; a synth's note held 0.6 s), and so is a program with
//                      per-call state beside the circuit (a block-rate LFO: a probe never cuts a call)
//             rings    over that second (a synth's A4 held 0.95 s) in blocks of 64 and 333, block by block: the .output
//                      node's ring is the output times .output's scale (tick returns V / scale), the .input node's ring
//                      the input times .input's scale (the synth's input, the saw, written to the right channel by a copy
//                      of the program), bit for bit; each slot's peak, RMS and mean within 1 % of its last whole periods'
//                      samples (the mean within 1 % of the RMS)
//             applied  a set for another program id writes nothing; with nothing armed the module's count, the rings
//                      and the writes stand still, and the meters say none; an unreadable node (screamer.cir's t1, in its
//                      tone block computed as a whole) is refused, its why in the probe table, the other slots read; the
//                      synth's probe follows the newest voice (the one before disarmed), and says when none sounds (-1)
//             safety   across a RUN (another program offered) and a refine (the same files under a new id) with a slot
//                      armed, the Engine writes probe fields only into the program that plays: the retired one's count
//                      of writes stops at the swap, and the new one's stays 0 until a set names its id
#include "Probe.h"

#include "Rig.h"

#include "Library.h"
#include "Product.h"

#include "fminus/CirTemplates.h"
#include "fminus/LibraryIncludes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;

    // The panel's lines and fields for both kinds: x (a float), k (an int), g (a bool) and v (a voice's note).
    const char* kLines = "#pragma panel meter x\n#pragma panel number k\n#pragma panel led g\n#pragma panel number v\n";
}

namespace
{
    // ---- PROBES -------------------------------------------------------------------------------------------------------
    std::string libraryText(const char* name)
    {
        const pg::LibraryFile* f = pg::findLibraryFile(name);
        return f != nullptr ? std::string(f->source) : std::string();
    }

    // Play this circuit's program for `file` (CirTemplates.h), with the library files it #includes.
    pg::Sources played(const char* file, const std::string& program = {})
    {
        const std::string netlist = libraryText(file);
        const std::string text = program.empty() ? pg::cir::hostProgram(file, netlist, kInstrument).text : program;
        return pg::withLibraryIncludes({ { "program.fm", text }, { file, netlist } });
    }

    // Beside the circuit, a block-rate LFO: state a call keeps (a cut call would move it).
    std::string perCallProgram()
    {
        if constexpr (kInstrument)
            return "#include \"diodetone.cir\"\nmodule Voice\n{\n    DiodeTone circuit;\n    float ph;\n    float step;\n"
                   "    float lfo;\n    float rate;\n    bool held;\n"
                   "    void init(float r) { rate = r; circuit.init(r); }\n"
                   "    void start(float note, float velocity) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; "
                   "held = true; ph = 0; circuit.clear(); }\n"
                   "    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        lfo += 0.37f;\n        if (lfo > 6.2831853f) lfo -= 6.2831853f;\n"
                   "        const float g = 0.75f + 0.25f * sinf(lfo);\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            const float y = g * circuit.tick(0.8f * sinf(6.2831853f * ph));\n"
                   "            ph += step;\n            if (ph >= 1) ph -= 1;\n            left[i] += y;\n            right[i] += y;\n"
                   "        }\n        return held;\n    }\n};\n";
        else
            return "#include \"diodetone.cir\"\nmodule Effect\n{\n    DiodeTone amp[2];\n    float lfo;\n"
                   "    void init(float rate) { amp[0].init(rate); amp[1].init(rate); }\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        lfo += 0.37f;\n        if (lfo > 6.2831853f) lfo -= 6.2831853f;\n"
                   "        const float g = 0.75f + 0.25f * sinf(lfo);\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            left[i] = g * amp[0].tick(left[i]);\n            right[i] = g * amp[1].tick(right[i]);\n"
                   "        }\n    }\n};\n";
    }

    int nodeOf(const pg::circuits::Probed& c, const char* name)
    {
        for (std::size_t k = 0; k < c.nodes.size(); ++k)
            if (c.nodes[k].name == name)
                return static_cast<int>(k);
        return -1;
    }

    // The set naming the program that plays, its first circuit field, element 0, the nodes named (-1 when not found).
    pg::ProbeSet setOn(const pg::Engine& e, std::initializer_list<const char*> names)
    {
        pg::ProbeSet set;
        const pg::Patch* p = e.playingPatch();
        if (p == nullptr || p->probed().empty())
            return set;
        set.program = p->program();
        set.circuit = 0;
        set.instance = 0;
        std::size_t k = 0;
        for (const char* n : names)
            if (k < set.nodes.size())
                set.nodes[k++] = static_cast<int16_t>(nodeOf(p->probed()[0], n));
        return set;
    }

    // The input: a 1 s sine of 0.5 V at 1 kHz (an effect's), or a synth's length; its notes.
    probe::Stereo probeInput()
    {
        probe::Stereo in(static_cast<std::size_t>(kFs));
        if constexpr (!kInstrument)
            in.l = in.r = probe::sine(1000.0, 0.5, kFs, in.size());
        return in;
    }
    std::vector<probe::Note> probeNotes()
    {
        if constexpr (kInstrument)
            return { { 0, pg::EventKind::noteOn, 57, 0.9f }, { static_cast<std::size_t>(0.6 * kFs), pg::EventKind::noteOff, 57, 0.0f } };
        return {};
    }

    bool sameBits(const probe::Stereo& a, const probe::Stereo& b)
    {
        return a.size() == b.size() && std::memcmp(a.l.data(), b.l.data(), a.size() * sizeof(float)) == 0
            && std::memcmp(a.r.data(), b.r.data(), a.size() * sizeof(float)) == 0;
    }

    void probes(funkgui::test::Probe& P)
    {
        const char* dt = "diodetone.cir";
        // bits: armed or not, the same output, in blocks of 64, 333 and 4096; and with a call's own state
        for (int which = 0; which < 2; ++which)
        {
            const pg::Sources files = which == 0 ? played(dt) : played(dt, perCallProgram());
            const std::string key = which == 0 ? "probes.bits" : "probes.bits_per_call";
            bool same = true, compiled = true, armedAll = true;
            for (const int block : { 64, 333, 4096 })
            {
                probe::Stereo outs[2];
                for (int armed = 0; armed < 2; ++armed)
                {
                    probe::Rig rig(kFs, 4096);
                    std::string why;
                    if (!rig.load(files, &why))
                    {
                        std::printf("%s", why.c_str());
                        compiled = false;
                        continue;
                    }
                    if (armed == 1)
                    {
                        const pg::ProbeSet set = setOn(rig.engine(), { "b", "in", "a", "c" });
                        armedAll = armedAll && set.nodes[0] >= 0 && set.nodes[1] >= 0 && set.nodes[2] >= 0 && set.nodes[3] >= 0;
                        rig.engine().setProbes(set);
                    }
                    outs[armed] = rig.run(probeInput(), block, probeNotes());
                    if (armed == 1)
                    {
                        pg::Meters m;
                        rig.engine().meters(m);
                        if constexpr (kInstrument)
                            armedAll = armedAll && rig.engine().probeSamples() > 0;   // the note's voice was followed
                        else
                            armedAll = armedAll && m.probes[0].valid && m.probes[3].valid;
                    }
                }
                same = same && sameBits(outs[0], outs[1]);
            }
            P.eq(key + ".compiles", compiled ? 1 : 0, 1);
            P.eq(key + ".armed", armedAll ? 1 : 0, 1);
            P.eq(key + ".same", same ? 1 : 0, 1);
        }

        // rings and stats, block by block, in blocks of 64 and 333
        for (const int block : { 64, 333 })
        {
            const std::string key = "probes.rings_" + std::to_string(block);
            // the synth's input: its copy of the program writes the circuit's input (the saw) to the right channel
            std::string program = pg::cir::hostProgram(dt, libraryText(dt), kInstrument).text;
            if constexpr (kInstrument)
            {
                const std::string from = "            right[i] += y;\n", to = "            right[i] += x;\n";
                const std::size_t at = program.find(from);
                if (at != std::string::npos)
                    program.replace(at, from.size(), to);
            }
            probe::Rig rig(kFs, 512);
            std::string why;
            const bool ok = rig.load(played(dt, program), &why);
            P.eq(key + ".compiles", ok ? 1 : 0, 1);
            if (!ok)
            {
                std::printf("%s", why.c_str());
                continue;
            }
            const pg::ProbeSet set = setOn(rig.engine(), { "b", "in", "a", "c" });
            rig.engine().setProbes(set);
            const probe::Stereo in = probeInput();
            std::vector<probe::Note> notes;                        // the synth's: A4 held throughout
            if constexpr (kInstrument)
                notes.push_back({ 0, pg::EventKind::noteOn, 69, 0.9f });
            constexpr double kScaleOut = 1.0, kScaleIn = 1.0;     // diodetone.cir's: .input scale=1, .output's default
            bool outSame = true, inSame = true, allValid = true;
            std::size_t compared = 0;
            std::vector<float> ring(static_cast<std::size_t>(pg::Engine::kScopeLength)), ringIn(ring.size());
            std::vector<float> outL;
            std::size_t nextNote = 0;
            const std::size_t length = kInstrument ? static_cast<std::size_t>(0.95 * kFs) : in.size();
            // the synth's first block has no voice when it starts (the note comes in it): its probe from the next
            std::size_t first = 0;
            for (std::size_t pos = 0; pos < length;)
            {
                const std::size_t len = std::min<std::size_t>(static_cast<std::size_t>(block), length - pos);
                probe::Stereo part(len);
                std::copy(in.l.begin() + static_cast<std::ptrdiff_t>(pos), in.l.begin() + static_cast<std::ptrdiff_t>(pos + len), part.l.begin());
                std::copy(in.r.begin() + static_cast<std::ptrdiff_t>(pos), in.r.begin() + static_cast<std::ptrdiff_t>(pos + len), part.r.begin());
                std::vector<probe::Note> here;
                while (nextNote < notes.size() && notes[nextNote].at < pos + len)
                {
                    probe::Note n = notes[nextNote++];
                    n.at -= pos;
                    here.push_back(n);
                }
                const probe::Stereo out = rig.run(part, block, here);
                const int n = static_cast<int>(len);
                const int got = rig.engine().probe(0, ring.data(), n);
                const int gotIn = rig.engine().probe(1, ringIn.data(), n);
                if (kInstrument && pos == 0)
                {
                    first = len;
                    pos += len;
                    continue;
                }
                allValid = allValid && got == n && gotIn == n;
                for (int i = 0; i < n && got == n && gotIn == n; ++i)
                {
                    const float wantOut = static_cast<float>(static_cast<double>(out.l[static_cast<std::size_t>(i)]) * kScaleOut);
                    const float wantIn = static_cast<float>(static_cast<double>(kInstrument ? out.r[static_cast<std::size_t>(i)]
                                                                                             : part.l[static_cast<std::size_t>(i)]) * kScaleIn);
                    outSame = outSame && std::memcmp(&ring[static_cast<std::size_t>(i)], &wantOut, sizeof wantOut) == 0;
                    inSame = inSame && std::memcmp(&ringIn[static_cast<std::size_t>(i)], &wantIn, sizeof wantIn) == 0;
                    ++compared;
                }
                outL.insert(outL.end(), out.l.begin(), out.l.end());
                pos += len;
            }
            std::printf("%s: %zu samples compared\n", key.c_str(), compared);
            P.eq(key + ".valid", allValid ? 1 : 0, 1);
            P.eq(key + ".output_ring", outSame && compared == length - first ? 1 : 0, 1);
            P.eq(key + ".input_ring", inSame && compared == length - first ? 1 : 0, 1);
            // the stats against the last whole periods of each slot's ring: 85 of the effect's 1 kHz (4080 samples), 37
            // of the synth's 440 Hz (4036; the peak falls 20 dB a second between the saw's peaks, 0.5 % a period)
            pg::Meters m;
            rig.engine().meters(m);
            P.eq(key + ".meters_set", m.probeSet == set && m.probeProgram == rig.program() ? 1 : 0, 1);
            for (int slot = 0; slot < 4; ++slot)
            {
                constexpr int kWindow = kInstrument ? 4036 : 4080;
                std::vector<float> v(static_cast<std::size_t>(pg::Engine::kScopeLength));
                const int valid = rig.engine().probe(slot, v.data(), pg::Engine::kScopeLength);
                double peak = 0.0, sum = 0.0, squares = 0.0;
                for (int i = pg::Engine::kScopeLength - kWindow; i < pg::Engine::kScopeLength; ++i)
                {
                    const double x = static_cast<double>(v[static_cast<std::size_t>(i)]);
                    peak = std::max(peak, std::fabs(x));
                    sum += x;
                    squares += x * x;
                }
                const double rms = std::sqrt(squares / kWindow), mean = sum / kWindow;
                const pg::ProbeMeter& pm = m.probes[slot];
                const std::string s = key + ".slot" + std::to_string(slot);
                std::printf("%s: peak %.4f (%.4f), rms %.4f (%.4f), mean %+.5f (%+.5f)\n", s.c_str(), static_cast<double>(pm.peak), peak,
                            static_cast<double>(pm.rms), rms, static_cast<double>(pm.mean), mean);
                P.eq(s + ".valid", pm.valid && valid >= kWindow ? 1 : 0, 1);
                P.near(s + ".peak", static_cast<double>(pm.peak), peak, 0.01 * peak);
                P.near(s + ".rms", static_cast<double>(pm.rms), rms, 0.01 * rms);
                P.near(s + ".mean", static_cast<double>(pm.mean), mean, 0.01 * rms);
            }
        }

        // applied: another program's set, none armed, an unreadable node, the synth's newest voice
        {
            probe::Rig rig(kFs, 256);
            const bool ok = rig.load(played(dt));
            P.eq("probes.applied.compiles", ok ? 1 : 0, 1);
            if (ok)
            {
                const pg::Patch* patch = rig.engine().playingPatch();
                std::vector<probe::Note> on;
                if constexpr (kInstrument)
                    on.push_back({ 0, pg::EventKind::noteOn, 57, 0.9f });
                // another program's id: nothing written
                pg::ProbeSet other = setOn(rig.engine(), { "b", "in" });
                other.program += 7;
                rig.engine().setProbes(other);
                const uint64_t w0 = patch->probeWrites();
                const uint32_t s0 = rig.engine().probeSamples();
                rig.run(probe::Stereo(2048), 256, on);
                pg::Meters m;
                rig.engine().meters(m);
                P.eq("probes.applied.other_program_writes", static_cast<int64_t>(patch->probeWrites() - w0), 0);
                P.eq("probes.applied.other_program_rings", static_cast<int64_t>(rig.engine().probeSamples() - s0), 0);
                P.eq("probes.applied.other_program_meters", m.probeProgram == 0 && !m.probes[0].valid && m.probeInstance == -1 ? 1 : 0, 1);
                // armed, then nothing: the count, the rings and the writes stand still
                rig.engine().setProbes(setOn(rig.engine(), { "b" }));
                rig.run(probe::Stereo(1024), 256);
                const uint32_t armedSamples = rig.engine().probeSamples();
                rig.engine().setProbes(pg::ProbeSet{});
                rig.run(probe::Stereo(256), 256);                  // the block that disarms
                // every instance's count (the synth's voice may be on any)
                const auto counts = [&] {
                    uint64_t sum = 0;
                    for (int i = 0; i < patch->instances(); ++i)
                    {
                        uint32_t prn = 0;
                        patch->probeCount(0, 0, i, prn);
                        sum += prn;
                    }
                    return sum;
                };
                bool counted = false;
                for (int i = 0; i < patch->instances(); ++i)
                    counted = patch->probeArmed(0, 0, i) || counted;
                const uint64_t prn0 = counts();
                const uint64_t w1 = patch->probeWrites();
                const uint32_t s1 = rig.engine().probeSamples();
                std::vector<float> before(512);
                rig.run(probe::Stereo(4096), 256);
                const uint64_t prn1 = counts();
                rig.engine().meters(m);
                P.eq("probes.applied.armed_moved", armedSamples > s0 ? 1 : 0, 1);
                P.eq("probes.applied.none_disarmed", counted ? 0 : 1, 1);
                P.eq("probes.applied.none_count_still", static_cast<int64_t>(prn1 - prn0), 0);
                P.eq("probes.applied.none_rings_still", static_cast<int64_t>(rig.engine().probeSamples() - s1), 0);
                P.eq("probes.applied.none_writes", static_cast<int64_t>(patch->probeWrites() - w1), 0);
                P.eq("probes.applied.none_meters", m.probeProgram == 0 && !m.probes[0].valid && m.probeInstance == -1
                                                       && rig.engine().probe(0, before.data(), 512) == 0 ? 1 : 0, 1);
                // the synth: the newest voice followed, the one before disarmed; none sounding: -1
                if constexpr (kInstrument)
                {
                    rig.engine().setProbes(setOn(rig.engine(), { "b", "a" }));
                    std::vector<probe::Note> two = { { 0, pg::EventKind::noteOn, 60, 0.9f }, { 1000, pg::EventKind::noteOn, 64, 0.9f } };
                    rig.run(probe::Stereo(2048), 256, two);
                    rig.engine().meters(m);
                    std::vector<pg::Engine::VoiceInfo> voices;
                    rig.engine().voices(voices);
                    int newest = -1;
                    uint64_t age = 0;
                    for (const pg::Engine::VoiceInfo& v : voices)
                        if (!v.fading && !v.ending && !v.released && (newest < 0 || v.age > age))
                        {
                            newest = v.instance;
                            age = v.age;
                        }
                    int armedCount = 0;
                    for (int i = 0; i < patch->instances(); ++i)
                        armedCount += patch->probeArmed(0, 0, i) ? 1 : 0;
                    std::printf("probes.applied: newest voice on instance %d, followed %d, %d instances armed\n", newest,
                                static_cast<int>(m.probeInstance), armedCount);
                    P.eq("probes.applied.follows_newest", newest >= 0 && m.probeInstance == newest ? 1 : 0, 1);
                    P.eq("probes.applied.one_armed", armedCount == 1 && patch->probeArmed(0, 0, newest) ? 1 : 0, 1);
                    P.eq("probes.applied.newest_valid", m.probes[0].valid && m.probes[1].valid ? 1 : 0, 1);
                    std::vector<probe::Note> off;
                    for (const int key : { 57, 60, 64 })
                        off.push_back({ 0, pg::EventKind::noteOff, key, 0.0f });
                    rig.run(probe::Stereo(static_cast<std::size_t>(kFs)), 256, off);   // the release (300 ms) ends
                    rig.engine().meters(m);
                    armedCount = 0;
                    for (int i = 0; i < patch->instances(); ++i)
                        armedCount += patch->probeArmed(0, 0, i) ? 1 : 0;
                    P.eq("probes.applied.none_sounding", m.voices == 0 && m.probeInstance == -1 && !m.probes[0].valid
                                                             && m.probeProgram == 0 && armedCount == 0 ? 1 : 0, 1);
                }
            }
        }
        // an unreadable node: screamer.cir's t1, inside its tone block (a matched design at 1x)
        {
            probe::Rig rig(kFs, 256);
            const bool ok = rig.load(played("screamer.cir"));
            P.eq("probes.unreadable.compiles", ok ? 1 : 0, 1);
            const pg::Patch* patch = ok ? rig.engine().playingPatch() : nullptr;
            if (patch != nullptr && !patch->probed().empty())
            {
                const pg::circuits::Probed& c = patch->probed()[0];
                const int t1 = nodeOf(c, "t1");
                const std::string why = t1 >= 0 ? c.nodes[static_cast<std::size_t>(t1)].why : std::string();
                std::printf("probes.unreadable: t1 (node %d): %s\n", t1, why.c_str());
                P.eq("probes.unreadable.why", t1 >= 0 && !c.nodes[static_cast<std::size_t>(t1)].readable
                                                  && why.find("inside a filter computed as a whole") != std::string::npos ? 1 : 0, 1);
                rig.engine().setProbes(setOn(rig.engine(), { "t1", "in" }));
                probe::Stereo in(4096);
                if constexpr (!kInstrument)
                    in.l = in.r = probe::sine(1000.0, 0.3, kFs, in.size());
                std::vector<probe::Note> on;
                if constexpr (kInstrument)
                    on.push_back({ 0, pg::EventKind::noteOn, 57, 0.9f });
                rig.run(in, 256, on);
                pg::Meters m;
                rig.engine().meters(m);
                std::vector<float> v(256);
                P.eq("probes.unreadable.refused", !m.probes[0].valid && rig.engine().probe(0, v.data(), 256) == 0 ? 1 : 0, 1);
                P.eq("probes.unreadable.others_read", m.probes[1].valid && rig.engine().probe(1, v.data(), 256) == 256 ? 1 : 0, 1);
            }
        }

        // safety: a RUN, then a refine, with a slot armed: writes only into the program that plays
        {
            probe::Rig rig(kFs, 256);
            const pg::Sources files = played(dt);
            const bool ok = rig.load(files);
            P.eq("probes.safety.compiles", ok ? 1 : 0, 1);
            if (ok)
            {
                std::vector<probe::Note> held;
                if constexpr (kInstrument)
                    held.push_back({ 0, pg::EventKind::noteOn, 57, 0.9f });   // held across: it starts on each new program
                const probe::Stereo in = probeInput();
                const auto chunk = [&](std::size_t n, const std::vector<probe::Note>& notes = {}) {
                    probe::Stereo part(n);
                    std::copy(in.l.begin(), in.l.begin() + static_cast<std::ptrdiff_t>(n), part.l.begin());
                    std::copy(in.r.begin(), in.r.begin() + static_cast<std::ptrdiff_t>(n), part.r.begin());
                    rig.run(part, 64, notes);
                };
                const pg::Patch* a = rig.engine().playingPatch();
                rig.engine().setProbes(setOn(rig.engine(), { "b", "a" }));
                chunk(2048, held);
                const uint64_t wa = a->probeWrites();
                // a RUN: another program (its own id), offered; the set still names the old one's
                pg::Patch::Result rb = pg::Patch::compile(files, static_cast<float>(kFs));
                const bool okB = rb.patch != nullptr;
                const pg::Patch* b = rb.patch.get();
                if (okB)
                {
                    rb.patch->setProgram(rig.program() + 100);
                    rig.engine().offer(std::move(rb.patch));
                }
                chunk(4096);                                                  // the swap and its 20 ms fade
                const bool aStopped = a->probeWrites() == wa;
                const bool bUntouched = okB && b->probeWrites() == 0 && rig.engine().playingPatch() == b;
                if (okB)
                    rig.engine().setProbes(setOn(rig.engine(), { "b", "a" }));   // the editor follows the new id
                chunk(2048);
                const uint64_t wb = okB ? b->probeWrites() : 0;
                P.eq("probes.safety.run_old_stops", aStopped && a->probeWrites() == wa ? 1 : 0, 1);
                P.eq("probes.safety.run_new_untouched", bUntouched ? 1 : 0, 1);
                P.eq("probes.safety.run_new_armed", wb > 0 ? 1 : 0, 1);
                // a refine: the same files under a new id (the retired one is let go by this offer: read before it)
                pg::Patch::Result rc = pg::Patch::compile(files, static_cast<float>(kFs));
                const bool okC = rc.patch != nullptr;
                const pg::Patch* c = rc.patch.get();
                if (okC)
                {
                    rc.patch->setProgram(rig.program() + 200);
                    rig.engine().offer(std::move(rc.patch));
                }
                chunk(4096);
                const bool bStopped = okB && b->probeWrites() == wb;
                const bool cUntouched = okC && c->probeWrites() == 0 && rig.engine().playingPatch() == c;
                if (okC)
                    rig.engine().setProbes(setOn(rig.engine(), { "b", "a" }));
                chunk(2048);
                pg::Meters m;
                rig.engine().meters(m);
                P.eq("probes.safety.refine_old_stops", bStopped && b->probeWrites() == wb ? 1 : 0, 1);
                P.eq("probes.safety.refine_new_untouched", cUntouched ? 1 : 0, 1);
                P.eq("probes.safety.refine_new_armed", okC && c->probeWrites() > 0 && m.probeProgram == rig.program() + 200
                                                           && m.probes[0].valid ? 1 : 0, 1);
            }
        }
    }
}

PROBE(dsp, meters)
{
    std::string source;
    if constexpr (kInstrument)
        source = std::string(kLines) +
                 "module Voice\n{\n    float x;\n    int k;\n    bool g;\n    float v;\n    float ph;\n    bool held;\n"
                 "    void start(float note, float velocity) { v = note; held = true; ph = 0; }\n"
                 "    void stop() { held = false; }\n"
                 "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                 "        for (int i = 0; i < n; ++i)\n        {\n"
                 "            const float s = 0.25f * sinf(6.28318531f * ph);\n            ph += 0.01f;\n"
                 "            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n        }\n"
                 "        x = 0.3f;\n        k = 7;\n        g = true;\n        return held;\n    }\n};\n";
    else
        source = std::string(kLines) +
                 "module Effect\n{\n    float x;\n    int k;\n    bool g;\n    float v;\n"
                 "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                 "        for (int i = 0; i < n; ++i)\n        {\n            left[i] *= 0.5f;\n            right[i] *= 0.5f;\n        }\n"
                 "        x = 0.3f;\n        k = 7;\n        g = true;\n        v = 60;\n    }\n};\n";
    probe::Rig rig(kFs, kBlock);
    rig.engine().addReader();                                    // as a panel does: the levels and the scopes measured
    std::string messages;
    const bool ok = rig.load(source, &messages);
    if (!messages.empty())
        std::printf("%s", messages.c_str());
    P.eq("load.compiles", ok ? 1 : 0, 1);
    P.eq("load.four_displays", static_cast<int64_t>(rig.panel().displays.size()), 4);
    if (!ok)
        return P.finish();

    const std::size_t second = static_cast<std::size_t>(kFs);
    pg::Meters m;
    // LEVELS
    probe::Stereo in(second);
    if constexpr (!kInstrument)
        in.l = in.r = probe::sine(1000.0, 0.8, kFs, second);
    std::vector<probe::Note> notes;
    if constexpr (kInstrument)
        notes.push_back({ 0, pg::EventKind::noteOn, 60, 0.8f });
    const probe::Stereo out = rig.run(in, kBlock, notes);
    P.eq("levels.read", rig.engine().meters(m) ? 1 : 0, 1);
    if constexpr (kInstrument)
    {
        P.near("levels.out_peak", static_cast<double>(m.outPeak[0]), 0.25, 0.0025);
        P.near("levels.out_rms", static_cast<double>(m.outRms[1]), 0.25 / std::sqrt(2.0), 0.0025);
        P.eq("levels.no_input", m.inPeak[0] == 0.0f && m.inRms[1] == 0.0f ? 1 : 0, 1);
    }
    else
    {
        P.near("levels.in_peak", static_cast<double>(m.inPeak[0]), 0.8, 0.008);
        P.near("levels.in_rms", static_cast<double>(m.inRms[1]), 0.8 / std::sqrt(2.0), 0.006);
        P.near("levels.out_peak", static_cast<double>(m.outPeak[1]), 0.4, 0.004);
        P.near("levels.out_rms", static_cast<double>(m.outRms[0]), 0.4 / std::sqrt(2.0), 0.003);
    }
    const uint32_t frames = m.frame;
    P.eq("frame.counts_blocks", static_cast<int64_t>(frames), static_cast<int64_t>((second + kBlock - 1) / kBlock));

    // TAPS
    P.near("taps.float", static_cast<double>(m.taps[0]), 0.3, 1e-6);
    P.near("taps.int", static_cast<double>(m.taps[1]), 7.0, 1e-6);
    P.near("taps.bool", static_cast<double>(m.taps[2]), 1.0, 1e-6);
    P.near("taps.note", static_cast<double>(m.taps[3]), 60.0, 1e-6);

    // SCOPE: the last 512 samples, as the Engine wrote them.
    {
        std::vector<float> scope(512);
        rig.engine().scope(false, scope.data(), 512);
        bool same = true;
        for (std::size_t i = 0; i < 512; ++i)
        {
            const std::size_t at = second - 512 + i;
            same = same && scope[i] == 0.5f * (out.l[at] + out.r[at]);
        }
        P.eq("scope.output_bits", same ? 1 : 0, 1);
        if constexpr (!kInstrument)
        {
            rig.engine().scope(true, scope.data(), 512);
            bool sameIn = true;
            for (std::size_t i = 0; i < 512; ++i)
            {
                const std::size_t at = second - 512 + i;
                sameIn = sameIn && scope[i] == 0.5f * (in.l[at] + in.r[at]);
            }
            P.eq("scope.input_bits", sameIn ? 1 : 0, 1);
        }
    }

    // VOICE (an instrument): a second note while the first sounds, then the second's voice ends.
    if constexpr (kInstrument)
    {
        std::vector<probe::Note> two{ { 0, pg::EventKind::noteOn, 64, 0.8f } };
        rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 8), kBlock, two);
        rig.engine().meters(m);
        P.near("voice.newest", static_cast<double>(m.taps[3]), 64.0, 1e-6);
        std::vector<probe::Note> off{ { 0, pg::EventKind::noteOff, 64, 0.0f } };
        rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 8), kBlock, off);
        rig.engine().meters(m);
        P.near("voice.then_the_one_left", static_cast<double>(m.taps[3]), 60.0, 1e-6);
        std::vector<probe::Note> all{ { 0, pg::EventKind::noteOff, 60, 0.0f } };
        rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 8), kBlock, all);
        rig.engine().meters(m);
        P.near("voice.none_left", static_cast<double>(m.taps[3]), 0.0, 1e-6);
    }

    // FALL: half a second of silence (an instrument's voices have ended above).
    {
        pg::Meters before = m;
        rig.engine().meters(before);
        const probe::Stereo quiet(second / 2);
        rig.run(quiet, kBlock);
        rig.engine().meters(m);
        const double was = static_cast<double>(before.outPeak[0]);
        std::printf("the output's peak: %.4f, half a second later %.4f\n", was, static_cast<double>(m.outPeak[0]));
        if (was > 0.0)
            P.near("fall.peak_10db_in_half_a_second", 20.0 * std::log10(static_cast<double>(m.outPeak[0]) / was), -10.0, 0.3);
        if (before.outRms[0] > 0.0f)
            P.le("fall.rms_40db_in_half_a_second", 20.0 * std::log10(std::max(1e-12, static_cast<double>(m.outRms[0]))
                                                                     / static_cast<double>(before.outRms[0])), -40.0);
    }

    // PROGRAM
    {
        rig.engine().meters(m);
        P.eq("program.the_one_playing", m.program == rig.program() && m.program != 0 ? 1 : 0, 1);
        pg::Patch::Result next = pg::Patch::compile(source, static_cast<float>(kFs));
        P.eq("program.next_compiles", next.patch != nullptr ? 1 : 0, 1);
        if (next.patch)
        {
            const uint64_t id = rig.program() + 100;
            next.patch->setProgram(id);
            rig.engine().offer(std::move(next.patch));
            rig.engine().meters(m);
            P.eq("program.not_one_offered", m.program == rig.program() ? 1 : 0, 1);
            rig.run(probe::Stereo(static_cast<std::size_t>(kBlock)), kBlock);
            rig.engine().meters(m);
            P.eq("program.then_the_one_taken", m.program == id ? 1 : 0, 1);
        }
    }

    // READERS: the sound again (an instrument's a note held), with no one reading the meters, then with a reader back.
    {
        const std::size_t len = static_cast<std::size_t>(kBlock) * 8;
        probe::Stereo loud(len);
        if constexpr (!kInstrument)
            loud.l = loud.r = probe::sine(1000.0, 0.8, kFs, len);
        std::vector<probe::Note> held;
        if constexpr (kInstrument)
            held.push_back({ 0, pg::EventKind::noteOn, 67, 0.8f });
        rig.run(loud, kBlock, held);
        std::vector<float> before(512), after(512);
        rig.engine().scope(false, before.data(), 512);
        rig.engine().removeReader();
        P.eq("readers.none", rig.engine().readers(), 0);
        pg::Meters was;
        rig.engine().meters(was);
        rig.run(loud, kBlock);
        rig.engine().meters(m);
        P.eq("readers.none_levels_0", m.outPeak[0] == 0.0f && m.outRms[1] == 0.0f && m.inPeak[0] == 0.0f ? 1 : 0, 1);
        rig.engine().scope(false, after.data(), 512);
        P.eq("readers.none_scope_still", before == after ? 1 : 0, 1);
        P.eq("readers.none_frames_go_on", static_cast<int64_t>(m.frame - was.frame), 8);
        P.eq("readers.none_fields_go_on", m.taps[1] == 7.0f && m.program == was.program ? 1 : 0, 1);
        rig.engine().addReader();
        probe::Stereo one(static_cast<std::size_t>(kBlock));
        std::copy_n(loud.l.begin(), kBlock, one.l.begin());
        std::copy_n(loud.r.begin(), kBlock, one.r.begin());
        const probe::Stereo block = rig.run(one, kBlock);
        rig.engine().meters(m);
        // The block's own peak and RMS (not smoothed up from 0, which would read a third of it).
        const auto own = [](const std::vector<float>& x, double& peak, double& rms) {
            double e = 0.0;
            peak = 0.0;
            for (const float v : x)
            {
                peak = std::max(peak, std::fabs(static_cast<double>(v)));
                e += static_cast<double>(v) * static_cast<double>(v);
            }
            rms = std::sqrt(e / static_cast<double>(x.size()));
        };
        double peak = 0.0, rms = 0.0;
        own(block.l, peak, rms);
        P.near("readers.back_out_peak", static_cast<double>(m.outPeak[0]), peak, 1e-6);
        P.near("readers.back_out_rms", static_cast<double>(m.outRms[0]), rms, 1e-4 * rms);
        P.ge("readers.back_out_audible", rms, 0.1);
        if constexpr (!kInstrument)
        {
            own(one.l, peak, rms);
            P.near("readers.back_in_peak", static_cast<double>(m.inPeak[0]), peak, 1e-6);
            P.near("readers.back_in_rms", static_cast<double>(m.inRms[0]), rms, 1e-4 * rms);
        }
        rig.engine().scope(false, after.data(), 512);
        bool silentBefore = true, theBlock = true;
        for (std::size_t i = 0; i < 512 - static_cast<std::size_t>(kBlock); ++i)
            silentBefore = silentBefore && after[i] == 0.0f;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kBlock); ++i)
            theBlock = theBlock && after[512 - static_cast<std::size_t>(kBlock) + i] == 0.5f * (block.l[i] + block.r[i]);
        P.eq("readers.back_scope_silence_then_the_block", silentBefore && theBlock ? 1 : 0, 1);
    }

    // LISTEN
    {
        pg::Transport t;
        t.known = true;
        t.bpm = 90.0;
        t.beat = 4.0;
        t.barStart = 3.0;
        t.beatsPerBar = 3;
        t.beatUnit = 4;
        t.playing = true;
        rig.engine().setTransport(t);
        std::vector<pg::BlockStat> stats(static_cast<std::size_t>(pg::Engine::kBlockRing));
        uint64_t from = 0;
        rig.engine().blocks(0, stats.data(), pg::Engine::kBlockRing, from);   // where the ring is now
        pg::Meters before;
        rig.engine().meters(before);
        const std::size_t len = static_cast<std::size_t>(kBlock) * 4;
        std::vector<probe::Note> played;
        if constexpr (kInstrument)
            played = { { 0, pg::EventKind::allNotesOff, 0, 0.0f },
                       { static_cast<std::size_t>(kBlock), pg::EventKind::noteOn, 60, 0.5f },
                       { static_cast<std::size_t>(kBlock), pg::EventKind::noteOn, 64, 0.25f },
                       { static_cast<std::size_t>(kBlock), pg::EventKind::controller, 64, 1.0f },   // the pedal down
                       { static_cast<std::size_t>(kBlock), pg::EventKind::noteOn, 67, 0.75f },
                       { static_cast<std::size_t>(kBlock) * 2, pg::EventKind::noteOff, 64, 0.0f } };
        rig.run(probe::Stereo(len), kBlock, played);
        rig.engine().meters(m);
        P.eq("listen.samples", static_cast<int64_t>(m.samples - before.samples), static_cast<int64_t>(len));
        P.eq("listen.host_known_playing", m.hostKnown && m.host.playing ? 1 : 0, 1);
        P.near("listen.host_bpm", static_cast<double>(m.host.bpm), 90.0, 1e-6);
        P.eq("listen.host_signature", m.host.beatsPerBar == 3 && m.host.beatUnit == 4 ? 1 : 0, 1);
        P.near("listen.host_beat", static_cast<double>(m.host.beat), 4.0 + 3.0 * kBlock * 90.0 / (60.0 * kFs), 1e-4);
        uint64_t next = 0;
        const int got = rig.engine().blocks(from, stats.data(), pg::Engine::kBlockRing, next);
        bool lengths = true, shares = true, metering = true;
        for (int i = 0; i < got; ++i)
        {
            const pg::BlockStat& b = stats[static_cast<std::size_t>(i)];
            lengths = lengths && b.samples == kBlock;
            shares = shares && std::isfinite(b.share) && b.share > 0.0f;
            metering = metering && (b.flags & pg::BlockStat::metering) != 0;
        }
        P.eq("listen.blocks_one_a_block", got, 4);
        P.eq("listen.blocks_next", static_cast<int64_t>(next - from), 4);
        P.eq("listen.blocks_lengths", lengths ? 1 : 0, 1);
        P.eq("listen.blocks_shares", shares ? 1 : 0, 1);
        P.eq("listen.blocks_metering", metering ? 1 : 0, 1);
        uint64_t again = 0;
        P.eq("listen.blocks_read_once", rig.engine().blocks(next, stats.data(), pg::Engine::kBlockRing, again), 0);
        if constexpr (kInstrument)
        {
            P.eq("listen.note_ons", static_cast<int64_t>(m.noteOns - before.noteOns), 3);
            P.eq("listen.last_note", static_cast<int64_t>(m.lastNote), 67);
            P.near("listen.last_velocity", static_cast<double>(m.lastVelocity), 0.75, 1e-6);
            P.eq("listen.held", static_cast<int64_t>(m.held), 2);
            P.eq("listen.held_bits", m.heldKeys[1] == ((1u << (60 - 32)) | 0u) && m.heldKeys[2] == (1u << (67 - 64)) && m.heldKeys[0] == 0
                                         && m.heldKeys[3] == 0 ? 1 : 0, 1);
            P.eq("listen.voices", static_cast<int64_t>(m.voices), 3);
            P.eq("listen.pedal_held", static_cast<int64_t>(m.pedalHeld), 1);
            P.eq("listen.none_released_or_fading", m.released == 0 && m.fading == 0 ? 1 : 0, 1);
            P.eq("listen.notes_flag", (stats[1].flags & pg::BlockStat::notes) != 0 && (stats[2].flags & pg::BlockStat::notes) == 0 ? 1 : 0, 1);
            // A new program: the held keys start again on it (no note-on of the player's), the old one's voices fade.
            pg::Patch::Result next2 = pg::Patch::compile(source, static_cast<float>(kFs));
            if (next2.patch)
            {
                next2.patch->setProgram(rig.program() + 200);
                rig.engine().offer(std::move(next2.patch));
            }
            pg::Meters was = m;
            rig.run(probe::Stereo(static_cast<std::size_t>(kBlock)), kBlock);
            rig.engine().meters(m);
            P.eq("listen.restart_counts_no_note_on", static_cast<int64_t>(m.noteOns - was.noteOns), 0);
            P.eq("listen.swap_fades", m.fading >= 2 && m.voices == 2 ? 1 : 0, 1);
            // Sixteen notes, then a seventeenth at the block's end: it steals one, which is still fading at the end.
            std::vector<probe::Note> many{ { 0, pg::EventKind::controller, 64, 0.0f }, { 0, pg::EventKind::allNotesOff, 0, 0.0f } };
            rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 8), kBlock, many);
            many.clear();
            for (int k = 0; k < 16; ++k)
                many.push_back({ 0, pg::EventKind::noteOn, 30 + k, 0.5f });
            many.push_back({ static_cast<std::size_t>(kBlock) - 8, pg::EventKind::noteOn, 50, 0.5f });
            rig.engine().meters(was);
            rig.run(probe::Stereo(static_cast<std::size_t>(kBlock)), kBlock, many);
            rig.engine().meters(m);
            P.eq("listen.steal_counted", static_cast<int64_t>(m.steals - was.steals), 1);
            P.eq("listen.steal_fading", m.fading == 1 && m.voices == 16 ? 1 : 0, 1);
        }
        else
            P.eq("listen.effect_no_notes", m.noteOns == 0 && m.voices == 0 && m.held == 0 && m.steals == 0 ? 1 : 0, 1);

        // A program with a dial, and a sound that goes bad: an instrument's voice for a key from 100, an effect's
        // output with the dial at its top.
        const std::string bad =
            kInstrument ? "module Voice\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                          "    bool bad;\n    bool held;\n"
                          "    void start(float note, float velocity) { bad = note >= 100; held = true; }\n"
                          "    void stop() { held = false; }\n"
                          "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                          "        for (int i = 0; i < n; ++i)\n        {\n"
                          "            const float s = bad ? sqrtf(-1.0f - level[i]) : 0.1f * level[i];\n"
                          "            left[i] += s;\n            right[i] += s;\n        }\n        return held;\n    }\n};\n"
                        : "module Effect\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                          "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                          "        for (int i = 0; i < n; ++i)\n        {\n"
                          "            left[i] = level[i] > 0.9f ? sqrtf(-1.0f - level[i]) : left[i] * level[i];\n"
                          "            right[i] = left[i];\n        }\n    }\n};\n";
        std::string why;
        const bool loaded = rig.load(bad, &why);
        if (!why.empty())
            std::printf("%s", why.c_str());
        P.eq("listen.bad_program_compiles", loaded ? 1 : 0, 1);
        if (loaded)
        {
            rig.run(probe::Stereo(static_cast<std::size_t>(kBlock)), kBlock);     // its first block snaps the dial
            rig.engine().blocks(0, stats.data(), pg::Engine::kBlockRing, from);
            rig.engine().blocks(from, stats.data(), pg::Engine::kBlockRing, from);   // to the ring's end
            int moved = 0;
            rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 4), kBlock, {}, {},
                    [&](std::size_t pos, std::array<float, pg::kMaxControls>& norm) {
                        if (pos == static_cast<std::size_t>(kBlock))
                        {
                            norm[0] = 0.8f;                      // it glides over the next 20 ms (four blocks of 256)
                            ++moved;
                        }
                    });
            const int n = rig.engine().blocks(from, stats.data(), pg::Engine::kBlockRing, next);
            P.eq("listen.moving_flag", n == 4 && moved == 1 && (stats[0].flags & pg::BlockStat::moving) == 0
                                               && (stats[1].flags & pg::BlockStat::moving) != 0 ? 1 : 0, 1);
            from = next;
            pg::Meters was;
            rig.engine().meters(was);
            std::vector<probe::Note> high;
            if constexpr (kInstrument)
                high.push_back({ 40, pg::EventKind::noteOn, 100, 0.5f });
            else
                rig.norm()[0] = 1.0f;
            rig.engine().takeAlerts();
            rig.run(probe::Stereo(static_cast<std::size_t>(kBlock) * 8), kBlock, high);
            rig.engine().meters(m);
            const int k = rig.engine().blocks(from, stats.data(), pg::Engine::kBlockRing, next);
            bool flagged = false;
            for (int i = 0; i < k; ++i)
                flagged = flagged || (stats[static_cast<std::size_t>(i)].flags & pg::BlockStat::alert) != 0;
            P.eq("listen.alert_flag", flagged && rig.engine().takeAlerts() != 0 ? 1 : 0, 1);
            if constexpr (kInstrument)
            {
                P.eq("listen.dropped", static_cast<int64_t>(m.dropped - was.dropped), 1);
                P.eq("listen.dropped_key", static_cast<int64_t>(m.droppedKey), 100);
                P.eq("listen.dropped_at", static_cast<int64_t>(m.droppedAt), static_cast<int64_t>(was.samples + 40));
            }
        }
    }
    probes(P);
    return P.finish();
}
