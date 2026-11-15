/* Source/xz/XzOptions.h: how this project builds XZ Embedded (Source/xz/embedded/, vendored unmodified: SEED.tsv).
 * Included by our xz_config.h, which the decoder's xz_private.h includes, and by plugin/XzDecode.cpp before xz.h, so
 * the decoder and its one caller see the same options. Plain C, no definitions: the userspace glue the decoder needs
 * (malloc, memcmp, min, the unaligned reads) is xz_config.h's alone, kept out of C++.
 *
 *   XZ_DEC_SINGLE    single-call mode only: the whole stream in memory, decoded into a buffer of exactly the size the
 *                    package's header gives, which is the LZMA2 dictionary too, so nothing but the decoder's own state
 *                    (about 30 KB) is allocated. XZ_PREALLOC and XZ_DYNALLOC are not compiled.
 *   XZ_USE_CRC64     the CRC64 check (xz's default when it writes a file), verified.
 *   XZ_USE_SHA256    the SHA-256 check, verified.
 *                    With CRC32 (built in) and "none", every check xz's format defines and any encoder writes is
 *                    verified. The IDs it leaves reserved (2, 3, 5 to 9, 11 to 15) are refused (XZ_OPTIONS_ERROR):
 *                    XZ_DEC_ANY_CHECK, which would accept them unchecked, cannot continue in single-call mode, and
 *                    no encoder writes one.
 *   No BCJ filter, no delta, no MicroLZMA, no concatenated streams: a package is one stream of LZMA2 (what Apple's
 *   COMPRESSION_LZMA writes); a stream with another filter is refused (XZ_OPTIONS_ERROR).
 */
#ifndef PLAYGROUND_XZ_OPTIONS_H
#define PLAYGROUND_XZ_OPTIONS_H

#define XZ_DEC_SINGLE
#define XZ_USE_CRC64
#define XZ_USE_SHA256

#endif
