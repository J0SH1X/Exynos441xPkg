/** @file
  XZ Embedded configuration for UEFI (Exynos441xPkg XzDecLib).

  xz_private.h includes this instead of the Linux headers outside the
  kernel; it maps the few libc/kernel helpers the decoder uses onto
  BaseMemoryLib / MemoryAllocationLib.  Only the LZMA2 stream decoder is
  built (no BCJ filters: the archives this is for do not use them).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H

//
// This XZ Embedded version verifies CRC32 only and rejects any other check
// type, but xz's default is CRC64 (the PhilZ ramdisk archives use it).
// Accept them and skip the check field; corrupt input still fails in the
// LZMA2 decoder or in the caller's parsing.
//
#define XZ_DEC_ANY_CHECK

#include "xz_uefi_types.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "xz.h"

#define kmalloc(size, flags)  AllocatePool (size)
#define kfree(ptr)            do { if ((ptr) != NULL) FreePool (ptr); } while (0)
#define vmalloc(size)         AllocatePool (size)
#define vfree(ptr)            do { if ((ptr) != NULL) FreePool (ptr); } while (0)

#define memcpy(d, s, n)       CopyMem ((d), (s), (n))
#define memmove(d, s, n)      CopyMem ((d), (s), (n))
#define memset(d, c, n)       SetMem ((d), (n), (UINT8)(c))
#define memcmp(a, b, n)       CompareMem ((a), (b), (n))
#define memeq(a, b, n)        (CompareMem ((a), (b), (n)) == 0)
#define memzero(d, n)         ZeroMem ((d), (n))

#undef min
#define min(x, y)             ((x) < (y) ? (x) : (y))
#define min_t(type, x, y)     min ((type)(x), (type)(y))

#ifndef __always_inline
#  define __always_inline     inline __attribute__((__always_inline__))
#endif

/*
 * Little-endian, unaligned-safe (the input buffer has no alignment
 * guarantee and ARM would fault on a misaligned word load).
 */
static inline uint32_t get_le32 (const uint8_t *buf)
{
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

#endif
