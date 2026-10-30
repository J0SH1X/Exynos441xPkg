/** @file
  ResetSystemLib for Samsung Exynos4.

  Uses the PMU software reset register (S5P_SWRESET, PMU base + 0x0400),
  exactly what the kernel's exynos reboot handler uses.  Any reset flavour
  (cold/warm/platform-specific) becomes the same SoC-wide software reset;
  shutdown is not supported (the platform would need to cut power through
  the PMIC over I2C).

  Like the kernel's sec_reboot(), the Samsung bootloader's reboot mode goes
  into PMU INFORM3 first: ResetPlatformSpecific() with ResetData L"download"
  (or L"recovery") reboots straight into that mode.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/TimerLib.h>

#define EXYNOS_PMU_SWRESET  0x0400
#define EXYNOS_PMU_INFORM2  0x0808
#define EXYNOS_PMU_INFORM3  0x080C

//
// sec-reboot.c: the INFORM2 magic skips the LP charging screen, INFORM3
// carries the reboot mode for the bootloader.
//
#define INFORM2_NO_LPM          0x12345678
#define REBOOT_MODE_PREFIX      0x12345670
#define REBOOT_MODE_NONE        0
#define REBOOT_MODE_DOWNLOAD    1
#define REBOOT_MODE_RECOVERY    4

/**
  Record the bootloader reboot mode, then trigger the SoC software reset.
**/
STATIC
VOID
DoSwReset (
  IN UINT32  Mode
  )
{
  UINT32  PmuBase = PcdGet32 (PcdPmuBaseAddress);

  MmioWrite32 (PmuBase + EXYNOS_PMU_INFORM2, INFORM2_NO_LPM);
  MmioWrite32 (PmuBase + EXYNOS_PMU_INFORM3, REBOOT_MODE_PREFIX | Mode);

  // The PMU register writes are not buffered on this path in practice, but
  // a short delay keeps the write observable before the reset lands.
  MmioWrite32 (PmuBase + EXYNOS_PMU_SWRESET, 0x1);

  // Should never get here.  Spin so no code runs on a half-reset SoC.
  for (;;) {
    MicroSecondDelay (10);
  }
}

VOID
EFIAPI
ResetCold (
  VOID
  )
{
  DoSwReset (REBOOT_MODE_NONE);
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  DoSwReset (REBOOT_MODE_NONE);
}

VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "%a: shutdown is not supported yet\n", __FUNCTION__));
  for (;;) {
    MicroSecondDelay (1000);
  }
}

VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  CONST CHAR16  *Mode;
  UINT32        RebootMode;

  //
  // ResetData starts with a NUL-terminated string (UEFI ResetSystem).
  //
  Mode       = (CONST CHAR16 *)ResetData;
  RebootMode = REBOOT_MODE_NONE;
  if ((Mode != NULL) && (DataSize >= sizeof (L"download"))) {
    if (StrnCmp (Mode, L"download", 8) == 0) {
      RebootMode = REBOOT_MODE_DOWNLOAD;
    } else if (StrnCmp (Mode, L"recovery", 8) == 0) {
      RebootMode = REBOOT_MODE_RECOVERY;
    }
  }

  DoSwReset (RebootMode);
}
