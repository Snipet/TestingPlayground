// Source/dsp/Assets.h: the program's samples (JUCE-free): audio files the user added (the explorer's ASSETS, or
// Claude's write_file with a path), each kept as it was added (a WAV, AIFF or FLAC file's bytes, what a session and a
// project keep) beside its sound, decoded once when it was added (plugin/AssetFiles.h: JUCE's readers, on the message
// thread or Claude's worker), never on the audio thread. A program reads one as a table: `#pragma sample kick
// "kick.wav"` fills its field `Sample kick;` (fminus/Patch.h, "Samples"; the library's sample.fm) with the sound at the
// rate it plays at, resampled here (windowed sinc) when the file's own rate is another, off the audio thread, once a
// rate (Asset::at keeps each one made), and handed to the program's instances as a pointer the Patch holds while it
// lives: the audio thread only reads it.
//
// The sound is planar: channel c's frame i at data[c * frames + i], one or two channels, at most kMaxSeconds. What a
// program may hold is capped (kMaxFileBytes a file as added, kMaxTotalBytes all of a program's, kMaxAssets of them;
// a sound's rate, length and size decoded), and refusal() says in words why a file does not fit: a file's header is
// checked before anything is read or allocated. A set of samples is immutable once made (`Assets`, shared): adding,
// removing or renaming one makes a new set, so a compile, a render and the audio thread each keep the one they started
// with.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace playground::assets
{
    inline constexpr std::size_t kMaxFileBytes = std::size_t{ 8 } << 20;    // a file as added: 8 MB
    inline constexpr std::size_t kMaxTotalBytes = std::size_t{ 32 } << 20;  // a program's in all (a session's, a
                                                                            // version's): 32 MB; a project's files:
                                                                            // projectstore::kMaxSampleBytes
    inline constexpr std::size_t kMaxAssets = 16;                           // a program's samples
    inline constexpr double kMaxSeconds = 60.0;                             // a sample's length
    inline constexpr int kMaxChannels = 2;
    inline constexpr double kMinRate = 1000.0, kMaxRate = 768000.0;         // a file's rate: 1 kHz to 768 kHz
    inline constexpr std::size_t kMaxSoundBytes = std::size_t{ 64 } << 20;  // a sound decoded, as floats: 64 MB (60 s
                                                                            // of stereo at 96 kHz takes 44 MB)

    // Whether `name` is an audio file's name: .wav, .wave, .aif, .aiff or .flac, any case.
    bool isAudioName(std::string_view name);
    // Whether it may name a sample: a valid file name (Sources.h's validFileName) that is an audio file's.
    bool validName(std::string_view name);
    // A file's name (its path's last part, "Kick 808 (hard).WAV") as a sample's: what validFileName refuses made '-'
    // (one for a run of them, none at either end of its stem), its extension in small letters, its stem cut so that the
    // whole fits kMaxNameLength; "" when nothing is left or it is no audio file's. `taken`: a name that is, made
    // "<stem>-2<ext>", "<stem>-3<ext>", ...
    std::string tidyName(std::string_view fileName, const std::vector<std::string>& taken = {});

    // A sound, planar: channel c's frame i at data[c * frames + i].
    struct Audio
    {
        double rate = 0.0;                          // its frames a second
        int channels = 0;                           // 1 or 2
        int frames = 0;                             // each channel's
        std::vector<float> data;
        double seconds() const noexcept { return rate > 0.0 ? static_cast<double>(frames) / rate : 0.0; }
    };

    // `from` at `rate`: windowed sinc (a Kaiser window over kZeros zero crossings each side, its cutoff kCutoff of the
    // lower rate's Nyquist frequency), as many frames as its length takes at that rate (rounded), each a weighted sum
    // of the input's around its time; the input is silent outside its frames. `from` itself when its rate is `rate`.
    // Off the audio thread (it allocates): resamples() counts the calls.
    inline constexpr int kZeros = 48;
    inline constexpr double kCutoff = 0.95;
    Audio resample(const Audio& from, double rate);
    uint64_t resamples() noexcept;

    // FNV-1a's 64 bits of the bytes, as 16 hex digits: how a sample is known in a project (its versions name each by
    // name and hash; the project keeps each file once).
    std::string hashOf(std::string_view bytes);

    // One sample: its name ("kick.wav"), the file as it was added, its hash, the sound decoded (at the file's own rate),
    // and the sound at each rate a program asked for (at(): made once a rate, kept, at most kRates of them). A renamed
    // copy shares all of it. Any thread.
    class Asset
    {
    public:
        static constexpr std::size_t kRates = 4;

        Asset(std::string name, std::string bytes, Audio audio);
        Asset(std::string name, const Asset& as);   // `as` under another name

        const std::string& name() const noexcept { return name_; }
        const std::string& bytes() const noexcept;
        const std::string& hash() const noexcept;
        const Audio& audio() const noexcept;
        // The sound at `rate` (resample()'s), made the first time a rate is asked for: off the audio thread.
        std::shared_ptr<const Audio> at(double rate) const;
        // "0.42 s, mono, 44.1 kHz, 37 KB"
        std::string describe() const;

    private:
        struct Body;
        std::string name_;
        std::shared_ptr<Body> body_;
    };

    struct Set
    {
        std::vector<std::shared_ptr<const Asset>> items;   // in the order they were added

        std::shared_ptr<const Asset> find(std::string_view name) const;   // by its exact name, or null
        std::size_t bytes() const noexcept;                // the files' as added, in all
        bool empty() const noexcept { return items.empty(); }
    };

    // Why a file of `bytes` named `name` cannot be added to `to` (in place of one of its name): its name, its size, the
    // set's size or count; "" when it can.
    std::string refusal(const Set* to, std::string_view name, std::size_t bytes);
    // Why a sound of `frames` frames (each channel's) of `channels` at `rate` cannot be a sample: no frames, more than
    // kMaxChannels, a rate outside kMinRate to kMaxRate, longer than kMaxSeconds, more than kMaxSoundBytes decoded; ""
    // when it can. What a file's header claims, checked before anything is read or allocated (AssetFiles.h).
    std::string refusal(std::string_view name, double rate, int64_t channels, int64_t frames);
    // The same of a decoded sound.
    std::string refusal(std::string_view name, const Audio& audio);
    // "9.4 MB", "37 KB", "512 bytes"
    std::string sizeText(std::size_t bytes);

    // A set with `asset` in it (in place of one of its name, where that one was), without the sample `name`, or with
    // `from` renamed `to` (a copy sharing its sound): new sets, `set` as it was. `set` may be null (none).
    std::shared_ptr<const Set> with(const std::shared_ptr<const Set>& set, std::shared_ptr<const Asset> asset);
    std::shared_ptr<const Set> without(const std::shared_ptr<const Set>& set, std::string_view name);
    std::shared_ptr<const Set> renamed(const std::shared_ptr<const Set>& set, std::string_view from, std::string_view to);

    // Whether two sets hold the same samples: the same names with the same files, in any order. Null is empty.
    bool same(const Set* a, const Set* b);
}

namespace playground
{
    // A program's samples (assets::Set), shared and never changed once made; null: none.
    using Assets = std::shared_ptr<const assets::Set>;
}
