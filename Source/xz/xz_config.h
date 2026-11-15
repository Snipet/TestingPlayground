/* SPDX-License-Identifier: 0BSD */
/* Source/xz/xz_config.h: the userspace glue XZ Embedded's xz_private.h includes by this name (its own
 * userspace/xz_config.h, 0BSD, cut to what this build uses), found on fp_xz's include path ahead of anything else of
 * the name: Source/xz/embedded holds none. The options are XzOptions.h's. Compiled into the decoder's C files alone. */
#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H

#include "XzOptions.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "xz.h"

#define kmalloc(size, flags) malloc(size)
#define kfree(ptr) free(ptr)
#define vmalloc(size) malloc(size)
#define vfree(ptr) free(ptr)

#define memeq(a, b, size) (memcmp(a, b, size) == 0)
#define memzero(buf, size) memset(buf, 0, size)

#ifndef min
#	define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#define min_t(type, x, y) min(x, y)

#ifndef fallthrough
#	if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311
#		define fallthrough [[fallthrough]]
#	elif defined(__GNUC__) || defined(__clang__)
#		define fallthrough __attribute__((__fallthrough__))
#	else
#		define fallthrough do {} while (0)
#	endif
#endif

#ifndef __always_inline
#	if defined(__GNUC__) || defined(__clang__)
#		define __always_inline inline __attribute__((__always_inline__))
#	else
#		define __always_inline inline
#	endif
#endif

static inline uint32_t get_unaligned_le32(const uint8_t *buf)
{
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static inline uint32_t get_unaligned_be32(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static inline void put_unaligned_le32(uint32_t val, uint8_t *buf)
{
	buf[0] = (uint8_t)val;
	buf[1] = (uint8_t)(val >> 8);
	buf[2] = (uint8_t)(val >> 16);
	buf[3] = (uint8_t)(val >> 24);
}

static inline void put_unaligned_be32(uint32_t val, uint8_t *buf)
{
	buf[0] = (uint8_t)(val >> 24);
	buf[1] = (uint8_t)(val >> 16);
	buf[2] = (uint8_t)(val >> 8);
	buf[3] = (uint8_t)val;
}

#define get_le32 get_unaligned_le32
#define get_be32 get_unaligned_be32

#endif
