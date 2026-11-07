// Source/dsp/Export.cpp: see Export.h.
#include "dsp/Export.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace playground::measure
{
    namespace
    {
        constexpr uint32_t kRound[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };

        uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        // A float's bytes as the hash and the WAV take them: little-endian.
        void putFloat(uint8_t* p, float f)
        {
            const uint32_t u = std::bit_cast<uint32_t>(f);
            p[0] = static_cast<uint8_t>(u);
            p[1] = static_cast<uint8_t>(u >> 8);
            p[2] = static_cast<uint8_t>(u >> 16);
            p[3] = static_cast<uint8_t>(u >> 24);
        }

        void put16(std::vector<uint8_t>& o, uint32_t v)
        {
            o.push_back(static_cast<uint8_t>(v));
            o.push_back(static_cast<uint8_t>(v >> 8));
        }

        void put32(std::vector<uint8_t>& o, uint32_t v)
        {
            put16(o, v & 0xffffu);
            put16(o, v >> 16);
        }

        void putTag(std::vector<uint8_t>& o, const char* tag) { o.insert(o.end(), tag, tag + 4); }

        uint32_t get16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }
        uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

        float sampleOf(const std::vector<float>& x, std::size_t i) { return i < x.size() ? x[i] : 0.0f; }

        constexpr std::size_t kChunk = std::size_t{ 1 } << 16;   // bytes handed to a sink at once
        constexpr char kHex[] = "0123456789abcdef";

        // A sink fed through a buffer of kChunk bytes.
        class Buffered
        {
        public:
            explicit Buffered(const Sink& sink) : sink_(sink) { buf_.reserve(kChunk); }
            void add(const void* p, std::size_t n)
            {
                const auto* b = static_cast<const uint8_t*>(p);
                while (ok_ && n > 0)
                {
                    const std::size_t take = std::min(n, kChunk - buf_.size());
                    buf_.insert(buf_.end(), b, b + take);
                    b += take;
                    n -= take;
                    total_ += take;
                    if (buf_.size() == kChunk)
                        flush();
                }
            }
            void add(const std::string& s) { add(s.data(), s.size()); }
            bool finish()
            {
                flush();
                return ok_;
            }
            std::size_t total() const { return total_; }

        private:
            void flush()
            {
                if (ok_ && !buf_.empty())
                    ok_ = sink_(buf_.data(), buf_.size());
                buf_.clear();
            }
            const Sink& sink_;
            std::vector<uint8_t> buf_;
            std::size_t total_ = 0;
            bool ok_ = true;
        };

        std::string number(double v, const char* format)
        {
            if (std::isnan(v))
                return "nan";
            if (std::isinf(v))
                return v > 0.0 ? "inf" : "-inf";
            char b[48];
            std::snprintf(b, sizeof b, format, v);
            return b;
        }
    }

    // ---- SHA-256 ------------------------------------------------------------------------------------------------------
    Sha256::Sha256()
        : h_{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }
    {
    }

    void Sha256::block(const uint8_t* p)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16
                 | static_cast<uint32_t>(p[4 * i + 2]) << 8 | static_cast<uint32_t>(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    void Sha256::add(const void* data, std::size_t bytes)
    {
        if (done_)
            return;
        const auto* p = static_cast<const uint8_t*>(data);
        length_ += bytes;
        if (used_ > 0)
        {
            const std::size_t take = std::min(bytes, sizeof buf_ - used_);
            std::memcpy(buf_ + used_, p, take);
            used_ += take;
            p += take;
            bytes -= take;
            if (used_ < sizeof buf_)
                return;
            block(buf_);
            used_ = 0;
        }
        for (; bytes >= sizeof buf_; p += sizeof buf_, bytes -= sizeof buf_)
            block(p);
        std::memcpy(buf_, p, bytes);
        used_ = bytes;
    }

    void Sha256::addStereo(const std::vector<float>& l, const std::vector<float>& r, std::size_t most)
    {
        // Eight frames a block (64 bytes), straight into the compression when the buffer is empty.
        const std::vector<float>& right = r.empty() ? l : r;
        uint8_t frames[64];
        std::size_t i = 0;
        const std::size_t n = std::min(l.size(), most);
        while (i < n)
        {
            const std::size_t take = std::min<std::size_t>(8, n - i);
            for (std::size_t k = 0; k < take; ++k)
            {
                putFloat(frames + 8 * k, l[i + k]);
                putFloat(frames + 8 * k + 4, sampleOf(right, i + k));
            }
            add(frames, 8 * take);
            i += take;
        }
    }

    std::string Sha256::hex()
    {
        if (done_)
            return digest_;
        const uint64_t bits = length_ * 8u;
        const uint8_t one = 0x80;
        add(&one, 1);
        const uint8_t zero = 0;
        while (used_ != 56)
            add(&zero, 1);
        uint8_t len[8];
        for (int k = 0; k < 8; ++k)
            len[k] = static_cast<uint8_t>(bits >> (56 - 8 * k));
        add(len, 8);
        done_ = true;
        digest_.clear();
        for (const uint32_t v : h_)
            for (int s = 28; s >= 0; s -= 4)
                digest_ += kHex[(v >> s) & 15u];
        return digest_;
    }

    std::string renderHash(const std::vector<float>& l, const std::vector<float>& r, std::size_t most)
    {
        Sha256 h;
        h.addStereo(l, r, most);
        return h.hex();
    }

    std::string hashLine(const std::string& hex, const std::string& what)
    {
        return "Render hash: " + hex.substr(0, kHashShown) + " (SHA-256 of the float32 samples, L R interleaved, of " + what + ").";
    }

    // ---- WAV --------------------------------------------------------------------------------------------------------
    namespace
    {
        // The header: RIFF, fmt (and a float's fact), then data's tag and size.
        std::vector<uint8_t> wavHeader(std::size_t frames, double rate, WavFormat format)
        {
            const bool fl = format == WavFormat::float32;
            const uint32_t bytesPer = fl ? 4u : 3u, align = 2u * bytesPer;
            const auto sr = static_cast<uint32_t>(std::clamp(std::llround(rate), 1LL, 4294967295LL));
            const auto data = static_cast<uint32_t>(frames * align);
            std::vector<uint8_t> o;
            putTag(o, "RIFF");
            put32(o, static_cast<uint32_t>(wavBytes(frames, format) - 8));
            putTag(o, "WAVE");
            putTag(o, "fmt ");
            put32(o, fl ? 18u : 16u);
            put16(o, fl ? 3u : 1u);                      // IEEE float, or PCM
            put16(o, 2u);
            put32(o, sr);
            put32(o, sr * align);
            put16(o, align);
            put16(o, bytesPer * 8u);
            if (fl)
            {
                put16(o, 0u);                            // cbSize
                putTag(o, "fact");
                put32(o, 4u);
                put32(o, static_cast<uint32_t>(frames));
            }
            putTag(o, "data");
            put32(o, data);
            return o;
        }
    }

    std::size_t wavBytes(std::size_t frames, WavFormat format)
    {
        // RIFF, WAVE (12); fmt (8 + 16, a float's 8 + 18 and fact's 12); data (8): the header; then the frames.
        return format == WavFormat::float32 ? 12 + 26 + 12 + 8 + frames * 8 : 12 + 24 + 8 + frames * 6;
    }

    WavWritten writeWav(const Sink& sink, const std::vector<float>& l, const std::vector<float>& r, double rate, WavFormat format)
    {
        WavWritten w;
        const std::size_t frames = l.size();
        if (wavBytes(frames, format) > 0xffffffffull)   // past RIFF's 4 GB: never from a render (Measure.h's caps)
            return w;
        Buffered out(sink);
        const std::vector<uint8_t> header = wavHeader(frames, rate, format);
        out.add(header.data(), header.size());
        const std::vector<float>& right = r.empty() ? l : r;
        uint8_t frame[8];
        for (std::size_t i = 0; i < frames; ++i)
        {
            const float s[2] = { l[i], sampleOf(right, i) };
            if (format == WavFormat::float32)
            {
                putFloat(frame, s[0]);
                putFloat(frame + 4, s[1]);
                out.add(frame, 8);
                continue;
            }
            for (int c = 0; c < 2; ++c)
            {
                // Times 2^23, to nearest (a NaN as 0: the output guard leaves none), held within 24 bits.
                const double x = std::isfinite(s[c]) ? std::nearbyint(static_cast<double>(s[c]) * 8388608.0) : 0.0;
                const double held = std::clamp(x, -8388608.0, 8388607.0);
                w.clipped += held != x ? 1 : 0;
                const auto v = static_cast<uint32_t>(static_cast<int32_t>(held));
                frame[3 * c] = static_cast<uint8_t>(v);
                frame[3 * c + 1] = static_cast<uint8_t>(v >> 8);
                frame[3 * c + 2] = static_cast<uint8_t>(v >> 16);
            }
            out.add(frame, 6);
        }
        w.ok = out.finish();
        w.bytes = out.total();
        return w;
    }

    bool readWav(std::span<const uint8_t> b, Wav& out, std::string& why)
    {
        out = {};
        if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        {
            why = "not a RIFF WAVE file";
            return false;
        }
        int format = 0;
        std::size_t at = 12;
        bool fmt = false;
        while (at + 8 <= b.size())
        {
            const uint8_t* c = b.data() + at;
            const std::size_t size = get32(c + 4);
            if (at + 8 + size > b.size())
            {
                why = "a chunk runs past the file's end";
                return false;
            }
            if (std::memcmp(c, "fmt ", 4) == 0 && size >= 16)
            {
                format = static_cast<int>(get16(c + 8));
                out.channels = static_cast<int>(get16(c + 10));
                out.rate = get32(c + 12);
                out.bits = static_cast<int>(get16(c + 22));
                fmt = true;
            }
            else if (std::memcmp(c, "data", 4) == 0)
            {
                if (!fmt || (out.channels != 1 && out.channels != 2))
                {
                    why = "no fmt chunk before the data, or not one or two channels";
                    return false;
                }
                out.isFloat = format == 3;
                if (!((format == 1 && (out.bits == 16 || out.bits == 24 || out.bits == 32)) || (format == 3 && out.bits == 32)))
                {
                    why = "format " + std::to_string(format) + " at " + std::to_string(out.bits) + " bits is not read here";
                    return false;
                }
                const std::size_t width = static_cast<std::size_t>(out.bits / 8), frames = size / (width * static_cast<std::size_t>(out.channels));
                out.l.resize(frames);
                out.r.resize(frames);
                const uint8_t* p = c + 8;
                for (std::size_t i = 0; i < frames; ++i)
                    for (int ch = 0; ch < out.channels; ++ch, p += width)
                    {
                        float v = 0.0f;
                        if (out.isFloat)
                            v = std::bit_cast<float>(get32(p));
                        else if (out.bits == 16)
                            v = static_cast<float>(static_cast<int16_t>(get16(p))) / 32768.0f;
                        else if (out.bits == 24)
                        {
                            const uint32_t u = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16;
                            v = static_cast<float>(static_cast<double>(static_cast<int32_t>(u << 8) >> 8) / 8388608.0);
                        }
                        else
                            v = static_cast<float>(static_cast<double>(static_cast<int32_t>(get32(p))) / 2147483648.0);
                        (ch == 0 ? out.l : out.r)[i] = v;
                    }
                if (out.channels == 1)
                    out.r = out.l;
                return true;
            }
            at += 8 + size + (size & 1u);
        }
        why = "no data chunk";
        return false;
    }

    // ---- CSV --------------------------------------------------------------------------------------------------------
    std::size_t writeCsv(const Sink& sink, const FieldLog& log, double rate, std::span<const std::size_t> rows, int digits)
    {
        Buffered out(sink);
        std::string line = "time_s,sample";
        for (const std::string& c : log.columns)
            line += "," + c;
        out.add(line + "\n");
        const std::size_t cols = log.columns.size();
        const std::string realFmt = digits > 0 ? "%." + std::to_string(digits) + "g" : "%.9g";
        const std::string doubleFmt = digits > 0 ? realFmt : "%.17g";
        const auto row = [&](std::size_t k) {
            line = number(static_cast<double>(log.at[k]) / rate, "%.6f") + "," + std::to_string(log.at[k]);
            for (std::size_t c = 0; c < cols; ++c)
            {
                line += ',';
                const std::size_t i = k * cols + c;
                if (i < log.none.size() && log.none[i] != 0)
                    continue;
                const double v = i < log.values.size() ? log.values[i] : 0.0;
                const FieldLog::Kind kind = c < log.kinds.size() ? log.kinds[c] : FieldLog::Kind::real;
                line += number(v, kind == FieldLog::Kind::whole && std::isfinite(v) && v == std::floor(v) ? "%.0f"
                                  : kind == FieldLog::Kind::real64                                  ? doubleFmt.c_str()
                                                                                                    : realFmt.c_str());
            }
            out.add(line + "\n");
        };
        if (rows.empty())
            for (std::size_t k = 0; k < log.rows(); ++k)
                row(k);
        else
            for (const std::size_t k : rows)
                if (k < log.rows())
                    row(k);
        return out.finish() ? out.total() : 0;
    }

    std::vector<std::size_t> evenRows(std::size_t rows, std::size_t count)
    {
        std::vector<std::size_t> out;
        if (rows == 0 || count == 0)
            return out;
        if (count >= rows)
        {
            for (std::size_t k = 0; k < rows; ++k)
                out.push_back(k);
            return out;
        }
        for (std::size_t k = 0; k < count; ++k)
        {
            const std::size_t r = count > 1 ? k * (rows - 1) / (count - 1) : 0;
            if (out.empty() || out.back() != r)
                out.push_back(r);
        }
        return out;
    }
}
