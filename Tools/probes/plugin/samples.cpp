// PROBE name=plugin.samples timeout=300
//
// plugin.samples: the program's samples (dsp/Assets.h, plugin/AssetFiles.h; Patch.h, "Samples"; ProjectStore.h):
//   DECODE    a 32-bit float WAV read as its floats bit for bit, a 16-bit and a 24-bit one as their integers over 2^15
//             and 2^23 exactly (stereo, planar); an AIFF and a FLAC the same; a file of another kind refused in words
//   PLAY      a #pragma sample field holds the file at its rate (frames, channels, rate), and a program that plays it
//             through the Engine gives its floats back bit for bit; the frozen copy plays it too
//   NOTES     a sample the set lacks, a field that is not there and one of another type: notes, the program plays
//             (frames 0)
//   RESAMPLE  44.1 kHz to 48: a 1 kHz sine within -115 dB of the ideal one, a 15 kHz one within -110 dB, its length the
//             same in seconds; 96 kHz to 48: a 30 kHz sine (past the new Nyquist) 105 dB down, a 1 kHz one within
//             -115 dB (away from the ends, which the kernel reaches past); a Patch reset at another rate holds that
//             rate's sound, and one back at a rate made before makes nothing again
//   CAPS      the refusals in words: a file past 8 MB, the 17th sample, past 32 MB in all, three channels, past 60 s, a
//             name that is no audio file's; a project's files past 64 MB refuse a save
//   AUDIO     the audio thread never decodes or resamples: a processor plays a 44.1 kHz sample at 48 and 96 kHz
//             (prepareToPlay resamples, off the audio thread) and processBlock adds no decode and no resample
//   SESSION   a state keeps the samples, as added; a processor that loads it plays them; a sample removed plays on in
//             the program that has it until its next run
//   PROJECT   a version keeps its samples (two versions, one file), its note says what changed (new, changed, gone),
//             saving it as it is adds none, opening it makes them the plugin's, the edited mark follows them; a
//             .fmpack and a .zip carry them out and back
//   TOOLS     write_file's path adds one (its name tidied), get_status lists it (and which the program reads),
//             rename_file and delete_file act on it, content for an audio file's name refused
//   EXAMPLE   sample.fm's one-shot: the synth's voice plays the hit at note 60 bit for bit and an octave up in half
//             the time; the effect plays it over its input
#include "Probe.h"

#include "Rig.h"

#include "dsp/Assets.h"
#include "fminus/LibraryIncludes.h"
#include "fminus/Patch.h"
#include "plugin/AssetFiles.h"
#include "plugin/Processor.h"
#include "plugin/ProjectExport.h"
#include "plugin/ProjectStore.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <tuple>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr int kBlock = 256;

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    void put(std::string& s, uint32_t v, int bytes)
    {
        for (int i = 0; i < bytes; ++i)
            s += static_cast<char>((v >> (8 * i)) & 0xffu);
    }

    // A WAV file's bytes: `frames` frames of `channels`, interleaved, 16- or 24-bit integers (`ints`) or 32-bit floats.
    std::string wav(int channels, int rate, int bits, const std::vector<int32_t>& ints, const std::vector<float>& floats)
    {
        const bool real = !floats.empty();
        const std::size_t count = real ? floats.size() : ints.size();
        const auto data = static_cast<uint32_t>(count * static_cast<std::size_t>(bits / 8));
        std::string s = "RIFF";
        put(s, 36 + data, 4);
        s += "WAVEfmt ";
        put(s, 16, 4);
        put(s, real ? 3u : 1u, 2);
        put(s, static_cast<uint32_t>(channels), 2);
        put(s, static_cast<uint32_t>(rate), 4);
        put(s, static_cast<uint32_t>(rate * channels * bits / 8), 4);
        put(s, static_cast<uint32_t>(channels * bits / 8), 2);
        put(s, static_cast<uint32_t>(bits), 2);
        s += "data";
        put(s, data, 4);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (real)
            {
                uint32_t u = 0;
                std::memcpy(&u, &floats[i], 4);
                put(s, u, 4);
            }
            else
                put(s, static_cast<uint32_t>(ints[i]), bits / 8);
        }
        return s;
    }

    // A mono 32-bit float WAV of `x` at `rate`.
    std::string floatWav(const std::vector<float>& x, int rate) { return wav(1, rate, 32, {}, x); }

    // A file written by one of JUCE's writers (AIFF, FLAC), stereo, 24-bit: the integers given (each channel's), as
    // its writers take them (left-justified in 32 bits).
    std::string written(juce::AudioFormat& format, const std::vector<int32_t>& l, const std::vector<int32_t>& r, int rate)
    {
        juce::MemoryBlock block;
        {
            std::vector<int> a(l.size()), b(r.size());
            for (std::size_t i = 0; i < l.size(); ++i)
            {
                a[i] = static_cast<int>(static_cast<uint32_t>(l[i]) << 8);
                b[i] = static_cast<int>(static_cast<uint32_t>(r[i]) << 8);
            }
            const int* channels[] = { a.data(), b.data(), nullptr };
            std::unique_ptr<juce::AudioFormatWriter> w(
                format.createWriterFor(new juce::MemoryOutputStream(block, false), rate, 2, 24, {}, 0));
            if (w == nullptr || !w->write(channels, static_cast<int>(a.size())))
                return {};
        }
        return std::string(static_cast<const char*>(block.getData()), block.getSize());
    }

    std::shared_ptr<const pg::assets::Asset> asset(const std::string& name, const std::string& bytes)
    {
        std::string why;
        return pg::assetfiles::make(nullptr, name, bytes, why);
    }

    pg::Assets setOf(std::initializer_list<std::shared_ptr<const pg::assets::Asset>> items)
    {
        auto s = std::make_shared<pg::assets::Set>();
        for (const auto& a : items)
            if (a != nullptr)
                s->items.push_back(a);
        return s;
    }

    // A hit: a decaying 180 Hz sine with noise in its first 5 ms, `frames` at 48 kHz, its peak 0.9 (deterministic).
    std::vector<float> hit(int frames, double rate = 48000.0)
    {
        std::vector<float> x(static_cast<std::size_t>(frames));
        uint32_t seed = 22222;
        for (int i = 0; i < frames; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const double t = i / rate;
            const double noise = t < 0.005 ? 0.3 * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0) : 0.0;
            x[static_cast<std::size_t>(i)] = static_cast<float>(0.9 * std::exp(-t * 18.0) * (std::sin(2 * std::numbers::pi * 180.0 * t) + noise) / 1.3);
        }
        return x;
    }

    // A program whose main module plays its Sample field s from its first sample (the effect: in place of its input;
    // the synth: a voice from its note-on), channel 0 left and channel 1 right, at one sample a sample.
    std::string player(const std::string& file, const std::string& extra = {})
    {
        const std::string head = "#include \"sample.fm\"\n#pragma sample s \"" + file + "\"\n" + extra;
        if constexpr (kInstrument)
            return head + "module Voice\n{\n    Sample s;\n    int pos;\n"
                          "    void start(float note, float velocity) { pos = 0; }\n"
                          "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                          "        for (int i = 0; i < n; ++i)\n        {\n"
                          "            left[i] += s.at(0, pos);\n            right[i] += s.at(1, pos);\n            ++pos;\n        }\n"
                          "        return pos < s.frames;\n    }\n};\n";
        else   // again and again, 50 ms apart
            return head + "module Effect\n{\n    Sample s;\n    int pos;\n"
                          "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                          "        for (int i = 0; i < n; ++i)\n        {\n"
                          "            left[i] = s.at(0, pos);\n            right[i] = s.at(1, pos);\n"
                          "            pos = pos + 1 < s.frames + 2400 ? pos + 1 : 0;\n        }\n"
                          "    }\n};\n";
    }

    pg::Sources withLibrary(const std::string& program) { return pg::withLibraryIncludes(pg::singleFile(program)); }

    // The program rendered `n` samples in a Rig at `rate` (the synth: note 60 at sample 0, held).
    probe::Stereo render(probe::Rig& rig, std::size_t n)
    {
        std::vector<probe::Note> notes;
        if constexpr (kInstrument)
            notes.push_back({ 0, pg::EventKind::noteOn, 60, 1.0f });
        return rig.run(probe::Stereo(n), kBlock, notes);
    }

    // How many of a's first `n` samples differ from b's (bits).
    int differing(const std::vector<float>& a, const float* b, std::size_t n)
    {
        int d = 0;
        for (std::size_t i = 0; i < n && i < a.size(); ++i)
            d += std::memcmp(&a[i], b + i, sizeof(float)) != 0 ? 1 : 0;
        return d;
    }

    // The largest |got - sin(2 pi f t + phase)| over the frames from `from` to `to`, in dB.
    double sineErrorDb(const pg::assets::Audio& a, double f, std::size_t from, std::size_t to)
    {
        double worst = 0.0;
        for (std::size_t i = from; i < to && i < static_cast<std::size_t>(a.frames); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(a.data[i]) - 0.5 * std::sin(2 * std::numbers::pi * f * static_cast<double>(i) / a.rate)));
        return 20.0 * std::log10(std::max(worst, 1.0e-12));
    }

    pg::assets::Audio sine(double f, double rate, double seconds, double amp = 0.5)
    {
        pg::assets::Audio a;
        a.rate = rate;
        a.channels = 1;
        a.frames = static_cast<int>(std::llround(seconds * rate));
        a.data.resize(static_cast<std::size_t>(a.frames));
        for (int i = 0; i < a.frames; ++i)
            a.data[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2 * std::numbers::pi * f * i / rate));
        return a;
    }

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

    // The editor's files run, and taken at once (prepareToPlay takes a program waiting, without a fade).
    void run(pg::Processor& p, const pg::Sources& files, double rate = 48000.0)
    {
        p.editorPort().setDraft({ files, 0 });
        p.editorPort().run(files);
        p.settle();
        p.setRateAndBufferSizeDetails(rate, kBlock);
        p.prepareToPlay(rate, kBlock);
    }

    double peakOf(const std::vector<float>& x)
    {
        double peak = 0.0;
        for (const float v : x)
            peak = std::max(peak, static_cast<double>(std::fabs(v)));
        return peak;
    }

    // `blocks` host blocks (the synth: note 60 at the first's start), the left channel's output.
    std::vector<float> play(pg::Processor& p, int blocks, double rate)
    {
        p.setRateAndBufferSizeDetails(rate, kBlock);
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buffer(2, kBlock);
            buffer.clear();
            juce::MidiBuffer midi;
            if (kInstrument && b == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
            p.processBlock(buffer, midi);
            out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock);
        }
        return out;
    }

    juce::File writeFile(const juce::File& dir, const std::string& name, const std::string& bytes)
    {
        const juce::File f = dir.getChildFile(juce::String::fromUTF8(name.c_str()));
        f.replaceWithData(bytes.data(), bytes.size());
        return f;
    }
}

PROBE(plugin, samples)
{
    const funkgui::HeadlessGuiScope gui;
    namespace ps = pg::projectstore;
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile(juce::String(pg::product::kEnvPrefix) + "samples-probe-" + juce::String(juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    dir.createDirectory();
    ps::setDirectoryForTests(dir.getChildFile("projects"));

    // ---- DECODE ----------------------------------------------------------------------------------------------------
    // Floats with every kind of bits: tiny, huge, negative zero, the extremes of a sample's usual range and past it.
    const std::vector<float> floats = { 0.0f, -0.0f, 1.0f, -1.0f, 0.333333343f, -0.7071067811f, 1.0e-30f, 3.5f, -2.25f,
                                        0.1f, 1.17549435e-38f, 0.999999940f, -0.5f, 0.25f, 0.125f, 6.1035156e-05f };
    {
        std::string why;
        const std::optional<pg::assets::Audio> a = pg::assetfiles::decode(floatWav(floats, 48000), "f.wav", why);
        P.eq("decode.float.read", a.has_value() ? 1 : 0, 1);
        if (a)
        {
            P.eq("decode.float.shape", a->channels == 1 && a->frames == static_cast<int>(floats.size()) && a->rate == 48000.0 ? 1 : 0, 1);
            P.eq("decode.float.bits", differing(floats, a->data.data(), floats.size()), 0);
        }
        // 16-bit and 24-bit stereo: each integer over 2^15 or 2^23, interleaved in the file, planar here.
        const std::vector<int32_t> i16 = { 0, 0, 32767, -32768, 1, -1, 12345, -23456, 16384, -16384 };
        const std::vector<int32_t> i24 = { 0, 0, 8388607, -8388608, 1, -1, 1234567, -2345678, 4194304, -4194304 };
        for (const auto& [bits, ints, scale] : { std::tuple{ 16, i16, 32768.0 }, std::tuple{ 24, i24, 8388608.0 } })
        {
            const std::optional<pg::assets::Audio> b = pg::assetfiles::decode(wav(2, 44100, bits, ints, {}), "i.wav", why);
            int wrong = b && b->channels == 2 && b->frames == 5 && b->rate == 44100.0 ? 0 : 1;
            for (int i = 0; b && wrong == 0 && i < 5; ++i)
                for (int c = 0; c < 2; ++c)
                    wrong += b->data[static_cast<std::size_t>(c * 5 + i)]
                                     != static_cast<float>(ints[static_cast<std::size_t>(2 * i + c)] / scale) ? 1 : 0;
            P.eq(bits == 16 ? "decode.int16.exact" : "decode.int24.exact", wrong, 0);
        }
        // AIFF and FLAC (JUCE's writers), 24-bit stereo: back as written.
        std::vector<int32_t> li(300), ri(300);
        std::vector<float> l(300), r(300);
        for (std::size_t i = 0; i < l.size(); ++i)
        {
            li[i] = static_cast<int32_t>(std::lround(8388607.0 * 0.6 * std::sin(0.05 * static_cast<double>(i))));
            ri[i] = static_cast<int32_t>(std::lround(8388607.0 * 0.3 * std::cos(0.11 * static_cast<double>(i))));
            l[i] = static_cast<float>(li[i] / 8388608.0);
            r[i] = static_cast<float>(ri[i] / 8388608.0);
        }
        juce::AiffAudioFormat aiffFormat;
        juce::FlacAudioFormat flacFormat;
        for (const auto& [name, bytes] : { std::pair{ std::string("x.aiff"), written(aiffFormat, li, ri, 48000) },
                                           std::pair{ std::string("x.flac"), written(flacFormat, li, ri, 48000) } })
        {
            const std::optional<pg::assets::Audio> c = pg::assetfiles::decode(bytes, name, why);
            const bool shape = c && c->channels == 2 && c->frames == 300;
            P.eq(name == "x.aiff" ? "decode.aiff.exact" : "decode.flac.exact",
                 shape ? differing(l, c->data.data(), 300) + differing(r, c->data.data() + 300, 300) : -1, 0);
        }
        const bool text = !pg::assetfiles::decode("not audio at all, only words", "notes.wav", why).has_value();
        P.eq("decode.refused_words", text && has(why, "notes.wav is not a WAV, AIFF or FLAC file this plugin can read") ? 1 : 0, 1);
        P.eq("decode.names", pg::assets::tidyName("/some/where/Kick 808 (hard).WAV") == "Kick-808-hard.wav"
                                   && pg::assets::tidyName("snare.wav", { "snare.wav" }) == "snare-2.wav"
                                   && pg::assets::tidyName("a.mp3").empty() && pg::assets::isAudioName("x.AIF")
                                   && !pg::assets::isAudioName("x.fm") ? 1 : 0, 1);
    }

    // ---- PLAY ------------------------------------------------------------------------------------------------------
    const std::vector<float> kick = hit(2400);   // 50 ms at 48 kHz
    const std::shared_ptr<const pg::assets::Asset> kickAsset = asset("kick.wav", floatWav(kick, 48000));
    P.eq("play.made", kickAsset != nullptr ? 1 : 0, 1);
    const pg::Assets kit = setOf({ kickAsset });
    {
        const pg::Patch::Result r = pg::Patch::compile(withLibrary(player("kick.wav")), 48000.0f, false, nullptr, true, true, kit);
        P.eq("play.compiles", r.patch != nullptr && r.notes.empty() ? 1 : 0, 1);
        const pg::assets::Audio* held = r.patch != nullptr ? r.patch->sample("s") : nullptr;
        P.eq("play.field", held != nullptr && held->frames == 2400 && held->channels == 1 && held->rate == 48000.0 ? 1 : 0, 1);
        P.eq("play.recipe", r.patch != nullptr && r.patch->recipe().samples.size() == 1
                                && r.patch->recipe().samples[0].first == "kick.wav"
                                && r.patch->recipe().samples[0].second == kickAsset->hash() ? 1 : 0, 1);
        probe::Rig rig(48000.0, kBlock);
        std::string errors;
        P.eq("play.loads", rig.load(withLibrary(player("kick.wav")), &errors, true, true, kit) ? 1 : 0, 1);
        const probe::Stereo out = render(rig, 4096);
        P.eq("play.bits_left", differing(kick, out.l.data(), kick.size()), 0);
        P.eq("play.bits_right", differing(kick, out.r.data(), kick.size()), 0);   // a mono sample's channel 1 is its 0
        double after = 0.0;
        for (std::size_t i = kick.size(); i < out.l.size(); ++i)
            after = std::max(after, static_cast<double>(std::fabs(out.l[i])));
        P.le("play.silent_after", after, 0.0);
        // The frozen copy plays it through the same instances: the same sound (not the same bits: Math::Fast).
        probe::Rig frozen(48000.0, kBlock);
        (void)frozen.load(withLibrary(player("kick.wav")), &errors, true, true, kit);
        (void)render(frozen, kBlock);
        std::string why;
        P.eq("play.frozen", frozen.freeze(&why) ? 1 : 0, 1);
    }

    // ---- NOTES -----------------------------------------------------------------------------------------------------
    {
        const pg::Patch::Result missing = pg::Patch::compile(withLibrary(player("snare.wav")), 48000.0f, false, nullptr, true, true, kit);
        P.eq("notes.missing", missing.patch != nullptr && has(missing.notes, "have no snare.wav, so s stays empty (frames 0)")
                                  && missing.patch->sample("s") == nullptr ? 1 : 0, 1);
        const pg::Patch::Result none = pg::Patch::compile(withLibrary(player("kick.wav", "#pragma sample nothere \"kick.wav\"\n")), 48000.0f,
                                                          false, nullptr, true, true, kit);
        P.eq("notes.no_field", none.patch != nullptr && has(none.notes, "has a field nothere") ? 1 : 0, 1);
        const pg::Patch::Result typed = pg::Patch::compile(withLibrary(player("kick.wav", "#pragma sample pos \"kick.wav\"\n")), 48000.0f,
                                                           false, nullptr, true, true, kit);
        P.eq("notes.wrong_type", typed.patch != nullptr && has(typed.notes, "pos is of type int: a sample's field is a Sample") ? 1 : 0, 1);
        const pg::Patch::Result bare = pg::Patch::compile(withLibrary(player("kick.wav", "#pragma sample\n")), 48000.0f,
                                                          false, nullptr, true, true, kit);
        P.eq("notes.bare", bare.patch != nullptr && has(bare.notes, "it names a field and a sample") ? 1 : 0, 1);
    }

    // ---- RESAMPLE --------------------------------------------------------------------------------------------------
    {
        const uint64_t before = pg::assets::resamples();
        const pg::assets::Audio up1 = pg::assets::resample(sine(1000.0, 44100.0, 1.0), 48000.0);
        const pg::assets::Audio up15 = pg::assets::resample(sine(15000.0, 44100.0, 1.0), 48000.0);
        P.eq("resample.length", up1.frames == 48000 && up1.rate == 48000.0 ? 1 : 0, 1);
        const std::size_t edge = 2000;   // the kernel's reach past the ends, and more
        const double e1 = sineErrorDb(up1, 1000.0, edge, 48000 - edge), e15 = sineErrorDb(up15, 15000.0, edge, 48000 - edge);
        P.le("resample.up_1k_error_db", e1, -115.0);
        P.le("resample.up_15k_error_db", e15, -110.0);
        const pg::assets::Audio down1 = pg::assets::resample(sine(1000.0, 96000.0, 1.0), 48000.0);
        P.le("resample.down_1k_error_db", sineErrorDb(down1, 1000.0, edge, 48000 - edge), -115.0);
        const pg::assets::Audio down30 = pg::assets::resample(sine(30000.0, 96000.0, 1.0), 48000.0);
        double peak = 0.0;
        for (std::size_t i = edge; i < 48000 - edge; ++i)
            peak = std::max(peak, static_cast<double>(std::fabs(down30.data[i])));
        P.le("resample.down_30k_db", 20.0 * std::log10(std::max(peak, 1.0e-12) / 0.5), -105.0);
        P.eq("resample.counted", static_cast<int>(pg::assets::resamples() - before), 4);
        // A Patch at another rate than its sample's: the sound resampled when it compiles, and again at a reset's rate.
        const std::shared_ptr<const pg::assets::Asset> at44 = asset("k44.wav", floatWav(hit(4410, 44100.0), 44100));
        pg::Patch::Result r = pg::Patch::compile(withLibrary(player("k44.wav")), 48000.0f, false, nullptr, true, true, setOf({ at44 }));
        const pg::assets::Audio* a = r.patch != nullptr ? r.patch->sample("s") : nullptr;
        P.eq("resample.patch_48k", a != nullptr && a->rate == 48000.0 && a->frames == 4800 ? 1 : 0, 1);
        if (r.patch)
        {
            r.patch->reset(96000.0f);
            const pg::assets::Audio* b = r.patch->sample("s");
            P.eq("resample.reset_96k", b != nullptr && b->rate == 96000.0 && b->frames == 9600 ? 1 : 0, 1);
            const uint64_t made = pg::assets::resamples();
            r.patch->reset(48000.0f);                    // a rate made before: the asset's own copy
            P.eq("resample.kept", pg::assets::resamples() == made && r.patch->sample("s")->frames == 4800 ? 1 : 0, 1);
        }
    }

    // ---- CAPS ------------------------------------------------------------------------------------------------------
    {
        namespace as = pg::assets;
        P.eq("caps.file", has(as::refusal(nullptr, "big.wav", (std::size_t{ 9 } << 20) + 1),
                              "big.wav is 9.0 MB: a sample may be 8.0 MB at most") ? 1 : 0, 1);
        P.eq("caps.name", has(as::refusal(nullptr, "kick.mp3", 100), "kick.mp3 is not a WAV, AIFF or FLAC file") ? 1 : 0, 1);
        // Sixteen small ones, then a seventeenth refused; one of their names replaces it instead.
        auto many = std::make_shared<as::Set>();
        for (int k = 0; k < 16; ++k)
            many->items.push_back(std::make_shared<const as::Asset>("s" + std::to_string(k) + ".wav", std::string(100, 'x'), as::Audio{}));
        P.eq("caps.count", has(as::refusal(many.get(), "s16.wav", 100), "a program has 16 samples at most: remove one before adding s16.wav")
                               && as::refusal(many.get(), "s3.wav", 100).empty() ? 1 : 0, 1);
        auto heavy = std::make_shared<as::Set>();
        for (int k = 0; k < 4; ++k)
            heavy->items.push_back(std::make_shared<const as::Asset>("h" + std::to_string(k) + ".wav", std::string(std::size_t{ 7 } << 20, 'x'), as::Audio{}));
        P.eq("caps.total", has(as::refusal(heavy.get(), "h4.wav", std::size_t{ 7 } << 20),
                               "with h4.wav the samples would take 35.0 MB: a program's may take 32.0 MB at most") ? 1 : 0, 1);
        std::string why;
        const std::vector<int32_t> three(3 * 100, 1000);
        P.eq("caps.channels", !pg::assetfiles::decode(wav(3, 48000, 16, three, {}), "tri.wav", why).has_value()
                                  && has(why, "tri.wav has 3 channels: a sample has one or two") ? 1 : 0, 1);
        const std::vector<int32_t> longOne(static_cast<std::size_t>(8000 * 61), 0);
        P.eq("caps.length", !pg::assetfiles::decode(wav(1, 8000, 16, longOne, {}), "long.wav", why).has_value()
                                && has(why, "long.wav is 61.0 s long: a sample may be 60 s at most") ? 1 : 0, 1);
        // From disk: the size refused before the file is read.
        const juce::File big = writeFile(dir, "big.wav", std::string((std::size_t{ 8 } << 20) + 10, '\0'));
        const uint64_t decodes = pg::assetfiles::decodes();
        P.eq("caps.file_disk", pg::assetfiles::read(nullptr, big.getFullPathName().toStdString(), "", why) == nullptr
                                   && has(why, "big.wav is 8.0 MB: a sample may be 8.0 MB at most")
                                   && pg::assetfiles::decodes() == decodes ? 1 : 0, 1);
        // A project's files past 64 MB (what saveVersion refuses a save with): ten of 7 MB, 70 MB; nine, 63 MB, fit.
        ps::Project weighty;
        weighty.name = "Heavy";
        for (int k = 0; k < 10; ++k)
            weighty.samples.push_back({ std::to_string(k), std::string(std::size_t{ 7 } << 20, 'x') });
        const std::string over = ps::samplesRefusal(weighty);
        weighty.samples.pop_back();
        P.eq("caps.project", has(over, "\"Heavy\"'s versions would keep 70.0 MB of samples: a project keeps 64.0 MB at most")
                                 && ps::samplesRefusal(weighty).empty() ? 1 : 0, 1);
    }

    // ---- AUDIO: the audio thread never decodes ------------------------------------------------------------------------
    const juce::File kick44 = writeFile(dir, "Kick 44.wav", floatWav(hit(4410, 44100.0), 44100));
    {
        pg::Processor p;
        p.setRateAndBufferSizeDetails(48000.0, kBlock);
        p.prepareToPlay(48000.0, kBlock);
        std::string why;
        P.eq("audio.added", p.editorPort().addAssets({ kick44.getFullPathName().toStdString() }, why) == 1 && why.empty() ? 1 : 0, 1);
        const std::vector<pg::AssetEntry> listed = p.editorPort().assets();
        P.eq("audio.listed", listed.size() == 1 && listed[0].name == "Kick-44.wav" && has(listed[0].detail, "0.10 s, mono, 44.1 kHz")
                                 && !listed[0].used ? 1 : 0, 1);
        run(p, withLibrary(player("Kick-44.wav")));
        P.eq("audio.used", !p.editorPort().assets().empty() && p.editorPort().assets()[0].used ? 1 : 0, 1);
        const uint64_t decodes = pg::assetfiles::decodes(), resamples = pg::assets::resamples();
        const std::vector<float> at48 = play(p, 40, 48000.0);
        P.eq("audio.no_work_48k", pg::assetfiles::decodes() == decodes && pg::assets::resamples() == resamples ? 1 : 0, 1);
        P.ge("audio.plays", peakOf(at48), 0.5);
        // Another rate: prepareToPlay resamples (the host's thread, the audio stopped); the blocks after do nothing more.
        p.prepareToPlay(96000.0, kBlock);
        const uint64_t prepared = pg::assets::resamples();
        P.eq("audio.prepare_resamples", prepared == resamples + 1 ? 1 : 0, 1);
        (void)play(p, 40, 96000.0);
        P.eq("audio.no_work_96k", pg::assetfiles::decodes() == decodes && pg::assets::resamples() == prepared ? 1 : 0, 1);
    }

    // ---- SESSION -----------------------------------------------------------------------------------------------------
    {
        pg::Processor a;
        a.setRateAndBufferSizeDetails(48000.0, kBlock);
        a.prepareToPlay(48000.0, kBlock);
        const juce::File k48 = writeFile(dir, "kick.wav", floatWav(kick, 48000));
        std::string why;
        (void)a.editorPort().addAssets({ k48.getFullPathName().toStdString() }, why);
        run(a, withLibrary(player("kick.wav")));
        juce::MemoryBlock state;
        a.getStateInformation(state);
        pg::Processor b;
        b.setRateAndBufferSizeDetails(48000.0, kBlock);
        b.prepareToPlay(48000.0, kBlock);
        b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        b.settle();
        b.prepareToPlay(48000.0, kBlock);
        const std::vector<pg::AssetEntry> kept = b.editorPort().assets();
        P.eq("session.kept", kept.size() == 1 && kept[0].name == "kick.wav" && kept[0].used ? 1 : 0, 1);
        const std::vector<float> out = play(b, 12, 48000.0);
        P.eq("session.plays_bits", differing(kick, out.data(), kick.size()), 0);
        // Removed, the sample plays on in the program that has it, until its next run (silent then).
        P.eq("session.removed", b.editorPort().removeAsset("kick.wav", why) && b.editorPort().assets().empty() ? 1 : 0, 1);
        P.ge("session.plays_on", peakOf(play(b, 12, 48000.0)), 0.5);
        run(b, withLibrary(player("kick.wav")));
        P.le("session.silent_after_run", peakOf(play(b, 12, 48000.0)), 0.0);
        // A state from before samples (none in it) has none.
        b.editorPort().addAssets({ k48.getFullPathName().toStdString() }, why);
        pg::Processor c;
        juce::MemoryBlock plain;
        c.getStateInformation(plain);
        b.setStateInformation(plain.getData(), static_cast<int>(plain.getSize()));
        P.eq("session.none", b.editorPort().assets().empty() ? 1 : 0, 1);
    }

    // ---- PROJECT -----------------------------------------------------------------------------------------------------
    {
        pg::Processor p;
        p.setRateAndBufferSizeDetails(48000.0, kBlock);
        p.prepareToPlay(48000.0, kBlock);
        pg::EditorPort& port = p.editorPort();
        const juce::File k48 = writeFile(dir, "kick.wav", floatWav(kick, 48000));
        const juce::File snare = writeFile(dir, "snare.wav", floatWav(hit(1200), 48000));
        std::string why;
        (void)port.addAssets({ k48.getFullPathName().toStdString() }, why);
        run(p, withLibrary(player("kick.wav")));
        pg::SavedVersion saved;
        P.eq("project.saved", port.saveVersion("Drum Kit", saved, why) && saved.number == 1 ? 1 : 0, 1);
        P.eq("project.not_edited", !port.project().edited ? 1 : 0, 1);
        P.eq("project.unchanged", port.saveVersion("Drum Kit", saved, why) && saved.unchanged ? 1 : 0, 1);
        (void)port.addAssets({ snare.getFullPathName().toStdString() }, why);
        P.eq("project.edited_by_sample", port.project().edited ? 1 : 0, 1);
        P.eq("project.saved2", port.saveVersion("Drum Kit", saved, why) && saved.number == 2 ? 1 : 0, 1);
        std::optional<ps::Project> pr = ps::load("Drum Kit", &why);
        P.eq("project.kept", pr && pr->versions.size() == 2 && pr->versions[0].samples.size() == 1
                                 && pr->versions[1].samples.size() == 2 && pr->samples.size() == 2 ? 1 : 0, 1);   // kick once
        P.eq("project.note_new", pr && has(pr->versions[1].note, "snare.wav new") ? 1 : 0, 1);
        // The kick replaced by another file of its name, the snare gone.
        (void)dir.getChildFile("other").createDirectory();
        const juce::File other = writeFile(dir.getChildFile("other"), "kick.wav", floatWav(hit(1800), 48000));
        (void)port.addAssets({ other.getFullPathName().toStdString() }, why);
        (void)port.removeAsset("snare.wav", why);
        (void)port.saveVersion("Drum Kit", saved, why);
        pr = ps::load("Drum Kit", &why);
        P.eq("project.note_changed", pr && has(pr->latest().note, "kick.wav changed") && has(pr->latest().note, "snare.wav gone") ? 1 : 0, 1);
        P.eq("project.files_each_once", pr && pr->samples.size() == 3 ? 1 : 0, 1);
        // Opening version 1 makes its samples the plugin's.
        P.eq("project.open", port.openProject("Drum Kit", 1, why) ? 1 : 0, 1);
        p.settle();
        p.prepareToPlay(48000.0, kBlock);
        const std::vector<pg::AssetEntry> v1 = port.assets();
        P.eq("project.open_samples", v1.size() == 1 && v1[0].name == "kick.wav" ? 1 : 0, 1);
        const std::vector<float> opened = play(p, 12, 48000.0);
        P.eq("project.open_plays_bits", differing(kick, opened.data(), kick.size()), 0);
        // A version deleted takes the files no other version names with it.
        P.eq("project.delete_version", port.deleteVersion("Drum Kit", 3, why) && ps::load("Drum Kit")->samples.size() == 2 ? 1 : 0, 1);
        // Out and back in: a package and a zip, every version.
        for (const bool package : { true, false })
        {
            const juce::File out = dir.getChildFile(package ? "kit.fmpack" : "kit.zip");
            pg::Exported ex;
            std::string name;
            const bool written = port.exportProject("Drum Kit", package, true, out.getFullPathName().toStdString(), ex, why);
            const bool imported = written && port.importProject(out.getFullPathName().toStdString(), name, why);
            const std::optional<ps::Project> back = imported ? ps::load(name) : std::nullopt;
            bool same = back && back->versions.size() == 2 && back->samples.size() == 2;
            for (std::size_t v = 0; same && v < 2; ++v)
                same = back->versions[v].samples == ps::load("Drum Kit")->versions[v].samples;
            for (std::size_t k = 0; same && k < back->samples.size(); ++k)
                same = pg::assets::hashOf(back->samples[k].bytes) == back->samples[k].hash;
            P.eq(package ? "project.package_round_trip" : "project.zip_round_trip", same ? 1 : 0, 1);
            if (!package)
            {
                // The zip holds the files as they are, in samples/.
                juce::ZipFile zip(out);
                P.eq("project.zip_files", zip.getIndexOfFileName("Drum Kit/samples/kick.wav") >= 0
                                              && zip.getIndexOfFileName("Drum Kit/samples/snare.wav") >= 0 ? 1 : 0, 1);
            }
            if (imported)
                (void)ps::remove(name);
        }
        (void)ps::remove("Drum Kit");
    }

    // ---- TOOLS -------------------------------------------------------------------------------------------------------
    {
        pg::Processor p;
        p.setRateAndBufferSizeDetails(48000.0, kBlock);
        p.prepareToPlay(48000.0, kBlock);
        const juce::File k48 = writeFile(dir, "Big Kick.wav", floatWav(kick, 48000));
        const std::string added = textOf(p.chatAgent().mcpCall("write_file", object({ { "name", "" }, { "path", k48.getFullPathName() } })));
        P.eq("tools.added", has(added, "Added the sample Big-Kick.wav (0.05 s, mono, 48 kHz") ? 1 : 0, 1);
        const std::string named = textOf(p.chatAgent().mcpCall("write_file", object({ { "name", "kick.wav" }, { "path", k48.getFullPathName() } })));
        P.eq("tools.added_named", has(named, "Added the sample kick.wav") && p.editorPort().assets().size() == 2 ? 1 : 0, 1);
        const std::string content = textOf(p.chatAgent().mcpCall("write_file", object({ { "name", "x.wav" }, { "content", "RIFF" } })));
        P.eq("tools.content_refused", has(content, "x.wav names an audio file: give its path") ? 1 : 0, 1);
        const std::string bad = textOf(p.chatAgent().mcpCall("write_file", object({ { "name", "" }, { "path", dir.getChildFile("none.wav").getFullPathName() } })));
        P.eq("tools.missing_file", has(bad, "there is no file at") ? 1 : 0, 1);
        run(p, withLibrary(player("kick.wav")));
        const std::string status = textOf(p.chatAgent().mcpCall("get_status", juce::var(new juce::DynamicObject())));
        P.eq("tools.status", has(status, "Samples: Big-Kick.wav (0.05 s, mono, 48 kHz, ") && has(status, "kick.wav (0.05 s, mono, 48 kHz, 9 KB; the program reads it)") ? 1 : 0, 1);
        const std::string renamed = textOf(p.chatAgent().mcpCall("rename_file", object({ { "name", "Big-Kick.wav" }, { "new_name", "boom.wav" } })));
        P.eq("tools.renamed", has(renamed, "Renamed the sample Big-Kick.wav to boom.wav") && p.editorPort().assets()[0].name == "boom.wav" ? 1 : 0, 1);
        const std::string deleted = textOf(p.chatAgent().mcpCall("delete_file", object({ { "name", "boom.wav" } })));
        P.eq("tools.deleted", has(deleted, "Deleted the sample boom.wav") && p.editorPort().assets().size() == 1 ? 1 : 0, 1);
        // A line a #pragma sample names that the samples lack.
        run(p, withLibrary(player("hat.wav")));
        const std::string lacking = textOf(p.chatAgent().mcpCall("get_status", juce::var(new juce::DynamicObject())));
        P.eq("tools.status_missing", has(lacking, "named by #pragma sample but not there: hat.wav") ? 1 : 0, 1);
    }

    // ---- EXAMPLE: sample.fm's one-shot ---------------------------------------------------------------------------------
    {
        probe::Rig rig(48000.0, kBlock);
        std::string errors;
        if constexpr (kInstrument)
        {
            const std::string voice = "#include \"sample.fm\"\n#pragma sample kick \"kick.wav\"\n"
                                      "module Voice\n{\n    Sample kick;\n    float pos;\n    float step;\n    float gain;\n"
                                      "    void start(float note, float velocity)\n    {\n        pos = 0;\n"
                                      "        step = expf(0.0577622650f * (note - 60));\n        gain = velocity;\n    }\n"
                                      "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                                      "        for (int i = 0; i < n; ++i)\n        {\n"
                                      "            left[i] += gain * kick.read(0, pos);\n            right[i] += gain * kick.read(1, pos);\n"
                                      "            pos += step;\n        }\n        return pos < kick.frames;\n    }\n};\n";
            P.eq("example.loads", rig.load(withLibrary(voice), &errors, true, true, kit) ? 1 : 0, 1);
            const probe::Stereo at60 = rig.run(probe::Stereo(4096), kBlock, { { 0, pg::EventKind::noteOn, 60, 1.0f } });
            P.eq("example.note60_bits", differing(kick, at60.l.data(), kick.size()), 0);
            probe::Rig up(48000.0, kBlock);
            (void)up.load(withLibrary(voice), &errors, true, true, kit);
            const probe::Stereo at72 = up.run(probe::Stereo(4096), kBlock, { { 0, pg::EventKind::noteOn, 72, 1.0f } });
            std::size_t last = 0;
            for (std::size_t i = 0; i < at72.l.size(); ++i)
                if (at72.l[i] != 0.0f)
                    last = i;
            P.in("example.octave_up_length", static_cast<double>(last), 1180.0, 1205.0);   // half the hit's 2400
        }
        else
        {
            const std::string effect = "#include \"sample.fm\"\n#pragma sample hit \"kick.wav\"\n"
                                       "module Effect\n{\n    Sample hit;\n    int pos;\n"
                                       "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                                       "        for (int i = 0; i < n; ++i)\n        {\n"
                                       "            left[i] += hit.at(0, pos);\n            right[i] += hit.at(1, pos);\n"
                                       "            ++pos;\n        }\n    }\n};\n";
            P.eq("example.loads", rig.load(withLibrary(effect), &errors, true, true, kit) ? 1 : 0, 1);
            const probe::Stereo out = rig.run(probe::Stereo(4096), kBlock);
            P.eq("example.over_silence_bits", differing(kick, out.l.data(), kick.size()), 0);
        }
    }

    ps::setDirectoryForTests({});
    dir.deleteRecursively();
    return P.finish();
}
