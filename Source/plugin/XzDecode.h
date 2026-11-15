// Source/plugin/XzDecode.h: one .xz stream decoded in memory, by XZ Embedded (Source/xz/: the XZ Utils project's small
// decoder, vendored unmodified, 0BSD; its options Source/xz/XzOptions.h). ProjectExport.cpp reads a package in xz
// (macOS writes them) with it on Windows and Linux; macOS reads them with Apple's Compression framework, and a probe
// (plugin.export) decodes what Apple's encoder wrote with both.
//
// The input is untrusted: a stream cut short, changed, longer or shorter than `size`, with a filter or an integrity
// check this build lacks, is refused with why, never read or written outside the two buffers. Single-call mode
// (XZ_SINGLE): `out` is the dictionary, so nothing is allocated but the decoder's own state; a stream whose dictionary
// is larger than `size` is fine (it never reaches past what it wrote). After the stream only stream padding may follow
// (zero bytes, a multiple of four), as xz's format allows. Any thread; the CRC tables are made once a process.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace playground::xzdecode
{
    // The stream in `in`, decoded into `out`, which it must fill exactly (`size` bytes, the header's). False with why in
    // a package's words ("it is damaged (...)", "it uses ...") when it cannot be.
    bool decode(const uint8_t* in, std::size_t inSize, uint8_t* out, std::size_t size, std::string* why = nullptr);
}
