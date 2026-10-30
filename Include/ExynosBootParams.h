/** @file
  What the Samsung bootloader handed us, preserved for a later Linux boot.

  The bootloader enters the KERNEL image with the ARM Linux boot ABI:
  r0 = 0, r1 = machine type, r2 = physical address of the ATAG list.  The
  BootShim stores r1/r2 here (after it has turned the D-cache off, so the
  stores reach DRAM), and PrePi copies the ATAG list in behind them before
  anything can reuse that memory.  A booter rebuilds the kernel's ATAGs from
  this copy: the bootloader's command line carries settings the downstream
  kernel needs (s3cfb.bootloaderfb, lcdtype, sec_log, bootmode, ...).

  Fixed 64 KiB at 0x40EF0000, directly below the log ring and carved out of
  system memory by Exynos441xPkgMem.c.  BootShim.S hard-codes the same
  address and magic.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_BOOT_PARAMS_H__
#define __EXYNOS_BOOT_PARAMS_H__

#define BOOT_PARAMS_BASE   0x40EF0000
#define BOOT_PARAMS_SIZE   0x00010000
#define BOOT_PARAMS_MAGIC  SIGNATURE_32 ('B', 'T', 'P', 'A')

typedef struct {
  UINT32  Magic;        // BootShim: BOOT_PARAMS_MAGIC
  UINT32  MachType;     // BootShim: r1
  UINT32  AtagsBase;    // BootShim: r2
  UINT32  AtagsSize;    // PrePi: bytes copied to Atags[], 0 = no valid list
  UINT32  Reserved[4];
  UINT32  Atags[1];     // PrePi: copy of the list, ATAG_NONE terminated
} EXYNOS_BOOT_PARAMS;

#define BOOT_PARAMS            ((EXYNOS_BOOT_PARAMS *)(UINTN)BOOT_PARAMS_BASE)
#define BOOT_PARAMS_ATAGS_MAX  (BOOT_PARAMS_SIZE - OFFSET_OF (EXYNOS_BOOT_PARAMS, Atags))

//
// ARM Linux ATAGs (arch/arm/include/asm/setup.h).  Size is in 32-bit words,
// header included.
//
#define ATAG_NONE       0x00000000
#define ATAG_CORE       0x54410001
#define ATAG_MEM        0x54410002
#define ATAG_VIDEOTEXT  0x54410003
#define ATAG_RAMDISK    0x54410004
#define ATAG_INITRD2    0x54420005
#define ATAG_SERIAL     0x54410006
#define ATAG_REVISION   0x54410007
#define ATAG_VIDEOLFB   0x54410008
#define ATAG_CMDLINE    0x54410009

typedef struct {
  UINT32  Size;
  UINT32  Tag;
} ATAG_HEADER;

typedef struct {
  ATAG_HEADER  Hdr;
  UINT32       Size;
  UINT32       Start;
} ATAG_MEM_DATA;

typedef struct {
  ATAG_HEADER  Hdr;
  CHAR8        Cmdline[1];
} ATAG_CMDLINE_DATA;

#define ATAG_NEXT(h)  ((ATAG_HEADER *)((UINT32 *)(h) + (h)->Size))

#endif
