/** @file
  OS log ring: a RAM log for code running after ExitBootServices (Windows
  drivers, the MCT HAL extension).  There is no debugger or UART on the
  phone, so they append text here and it is read back after a warm reset,
  which keeps DRAM (usbdbg.py oslog).

  512 KiB at 0x5E900000-0x5E980000, just below the Samsung sec_log
  (0x5E9FFFF8): that area provably survives a reset, while something in
  the boot chain clobbers the low 16 MiB (a ring at 0x40E00000 came back
  zeroed).  Carved out of system memory by Exynos441xPkgMem.c so the OS
  never uses it.  Unlike the boot log ring, firmware never clears it: it
  accumulates over boots until a writer finds no valid magic or the host
  clears it.

  Same layout as EXYNOS_LOG_RING: Head counts bytes ever written, byte N is
  at Data[N % OS_LOG_DATA_SIZE].  The Windows-side writer is
  ExynosWinDrivers/common/oslog.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_OS_LOG_H__
#define __EXYNOS_OS_LOG_H__

#define OS_LOG_BASE   0x5E900000
#define OS_LOG_SIZE   0x00080000
#define OS_LOG_MAGIC  SIGNATURE_32 ('O', 'S', 'L', 'G')

#endif
