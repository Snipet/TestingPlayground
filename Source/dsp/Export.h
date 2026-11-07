// Source/dsp/Export.h: a render as data (JUCE-free): its hash, a WAV of it and a CSV of the program's fields over it.
// The chat's measuring tools put the hash in their reports (measure_output, view_sound, compare and trace: two renders
// with the same hash are the same bits), and trace's `export` writes the files (plugin/ChatTrace.cpp, which picks the
// folder and writes through a Sink).
//
// The hash: SHA-256 (FIPS 180-4) of the samples as float32, little-endian, left and right interleaved: the bytes of a
// float32 WAV's data chunk, so `shasum -a 256` of a raw dump, Python's hashlib over numpy's `float32` interleaved array,
// or a build elsewhere that writes the same samples gives the same digest. The reports show its first kHashShown hex
// digits; a report of several renders (an effect's test signals) hashes them in turn, one stream.
//
// The WAV: RIFF, stereo, at the render's rate (rounded to a whole Hz), float32 (format 3, IEEE float, with its fact
// chunk) or 24-bit PCM (format 1, which Python's `wave` module reads too): each sample times 2^23, rounded to nearest
// and held within the format's range (a sample past full scale is clipped, and counted), never dithered: the same file
// run after run. The CSV: a header (time_s, sample, then each field's path), then a row each time a field log took one
// (FieldLog: trace's fields after its calls of 16 samples), a float to 9 significant digits, a double to 17 (each reads
// back to the bits it was), an int or a bool whole, an empty cell where no voice of the choice sounds, nan or inf where
// a value was not finite.
//
// Off the audio thread only. Deterministic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace playground::measure
{
    // ---- the render hash ---------------------------------------------------------------------------------------------
    inline constexpr std::size_t kHashShown = 16;    // hex digits a report shows (64 bits)

    // SHA-256 over bytes fed in any pieces; hex() the digest in lower-case hex, 64 digits.
    class Sha256
    {
    public:
        Sha256();
        void add(const void* data, std::size_t bytes);
        // The samples as the hash takes them: float32, little-endian, l[i] then r[i] (r shorter: its missing samples
        // are 0; r empty: l mono, each sample twice).
        void addStereo(const std::vector<float>& l, const std::vector<float>& r);
        std::string hex();                           // finishes it: add nothing after

    private:
        void block(const uint8_t* p);
        uint32_t h_[8];
        uint8_t buf_[64];
        std::size_t used_ = 0;
        uint64_t length_ = 0;                        // bytes
        bool done_ = false;
        std::string digest_;
    };

    // The render hash of one render: SHA-256 of its float32 samples, L R interleaved (above), 64 hex digits.
    std::string renderHash(const std::vector<float>& l, const std::vector<float>& r);
    // A report's line of it: "Render hash: 9f86d081884c7d65 (SHA-256 of the float32 samples, L R interleaved, of <what>)."
    std::string hashLine(const std::string& hex, const std::string& what);

    // ---- files -------------------------------------------------------------------------------------------------------
    // Where a file's bytes go, in order: false stops the writing (a disk full, a closed stream).
    using Sink = std::function<bool(const void* data, std::size_t bytes)>;

    enum class WavFormat : uint8_t { float32, pcm24 };

    struct WavWritten
    {
        bool ok = false;                             // every byte went (the sink never said false)
        std::size_t bytes = 0;                       // the file's
        std::size_t clipped = 0;                     // 24-bit: samples (of either channel) held at full scale
    };
    // A file's size before it is written: the header and frames x the format's bytes a frame.
    std::size_t wavBytes(std::size_t frames, WavFormat format);
    // The samples as a stereo WAV (above): frames = l.size() (r shorter: 0 past its end; empty: l on both).
    WavWritten writeWav(const Sink& sink, const std::vector<float>& l, const std::vector<float>& r, double rate, WavFormat format);

    // A WAV read back (format 1 at 16, 24 or 32 bits, or format 3 at 32; one or two channels): for the probes, and a
    // check that what was written reads as it should.
    struct Wav
    {
        uint32_t rate = 0;
        int channels = 0;
        int bits = 0;
        bool isFloat = false;
        std::vector<float> l, r;                     // r = l for one channel
    };
    bool readWav(std::span<const uint8_t> bytes, Wav& out, std::string& why);

    // Fields logged over a render (trace's export: Debug.h's TraceRequest::every), a row each time it took one.
    struct FieldLog
    {
        enum class Kind : uint8_t { real, real64, whole };   // a float, a double, an int or a bool
        std::vector<std::string> columns;            // the fields' paths
        std::vector<Kind> kinds;
        std::vector<std::size_t> at;                 // each row's sample: the fields as they were after it
        std::vector<double> values;                  // rows x columns (with `none`: unused)
        std::vector<uint8_t> none;                   // rows x columns: 1 where no voice of the choice sounded
        std::size_t rows() const { return at.size(); }
    };
    // The CSV (above) of the rows given (empty: every row), each value to `digits` significant digits when not 0 (the
    // inline form's, shorter; a whole number stays whole); its bytes written, or 0 when the sink said false.
    std::size_t writeCsv(const Sink& sink, const FieldLog& log, double rate, std::span<const std::size_t> rows = {}, int digits = 0);
    // `count` rows spread evenly over a log of `rows` (its first and its last among them; all of them when fewer).
    std::vector<std::size_t> evenRows(std::size_t rows, std::size_t count);
}
