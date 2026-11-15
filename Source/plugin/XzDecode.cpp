// Source/plugin/XzDecode.cpp: see XzDecode.h.
#include "plugin/XzDecode.h"

#include "xz/XzOptions.h"
#include "xz/embedded/xz.h"

#include <mutex>

namespace playground::xzdecode
{
    namespace
    {
        constexpr std::size_t kStreamHeader = 12;   // the magic (6 bytes), the flags (2: 0, then the check's ID), CRC32

        void fail(std::string* why, const char* what)
        {
            if (why != nullptr)
                *why = what;
        }

        void initTables()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                xz_crc32_init();
                xz_crc64_init();
            });
        }

        // The check's ID a stream's header names, -1 when it is no header. Only for the why: the decoder checks it.
        int checkOf(const uint8_t* in, std::size_t inSize)
        {
            static constexpr uint8_t kMagic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
            if (inSize < kStreamHeader)
                return -1;
            for (std::size_t i = 0; i < 6; ++i)
                if (in[i] != kMagic[i])
                    return -1;
            return in[6] == 0 ? (in[7] & 0x0F) : -1;
        }

        struct Decoder
        {
            xz_dec* s = xz_dec_init(XZ_SINGLE, 0);
            ~Decoder() { xz_dec_end(s); }
            Decoder() = default;
            Decoder(const Decoder&) = delete;
            Decoder& operator=(const Decoder&) = delete;
        };
    }

    bool decode(const uint8_t* in, std::size_t inSize, uint8_t* out, std::size_t size, std::string* why)
    {
        initTables();
        Decoder d;
        if (d.s == nullptr)
        {
            fail(why, "the plugin ran out of memory reading it");
            return false;
        }
        static uint8_t none = 0;   // a buffer of no bytes still needs an address
        xz_buf b{};
        b.in = inSize > 0 ? in : &none;
        b.in_pos = 0;
        b.in_size = inSize;
        b.out = size > 0 ? out : &none;
        b.out_pos = 0;
        b.out_size = size;
        const xz_ret r = xz_dec_run(d.s, &b);
        switch (r)
        {
            case XZ_STREAM_END:
                break;
            case XZ_OPTIONS_ERROR:
            {
                const int check = checkOf(in, inSize);
                if (check > 1 && check != 4 && check != 10)
                    fail(why, "it uses an xz integrity check this plugin does not know");
                else
                    fail(why, "it uses an xz filter or option this plugin does not read");
                return false;
            }
            case XZ_MEM_ERROR:
                fail(why, "the plugin ran out of memory reading it");
                return false;
            case XZ_FORMAT_ERROR:
                fail(why, "it is damaged (its contents are not xz data)");
                return false;
            case XZ_BUF_ERROR:
                // Single-call: input was left when the output reached `size` (xz_dec_run's own reading).
                fail(why, "it is damaged (its xz data holds more than its header says)");
                return false;
            case XZ_OK:
            case XZ_UNSUPPORTED_CHECK:
            case XZ_MEMLIMIT_ERROR:
            case XZ_DATA_ERROR:
                // Single-call: a check or a structure that does not hold, or the input ran out before the end.
                fail(why, "it is damaged (its xz data is cut short or changed)");
                return false;
        }
        if (b.out_pos != size)
        {
            fail(why, "it is damaged (its contents are not the size its header says)");
            return false;
        }
        // Stream padding alone may follow: zero bytes, a multiple of four.
        const std::size_t rest = inSize - b.in_pos;
        for (std::size_t i = b.in_pos; i < inSize; ++i)
            if (in[i] != 0)
            {
                fail(why, "it is damaged (something follows its xz data)");
                return false;
            }
        if (rest % 4 != 0)
        {
            fail(why, "it is damaged (something follows its xz data)");
            return false;
        }
        return true;
    }
}
