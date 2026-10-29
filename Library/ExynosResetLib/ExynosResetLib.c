/** @file
  ResetSystemLib for Samsung Exynos4.

  Uses the PMU software reset register (S5P_SWRESET, PMU base + 0x0400),
  exactly what the kernel's exynos reboot handler uses.  Any reset flavour
  (cold/warm/platform-specific) becomes the same SoC-wide software reset;
  shutdown is not supported (the platform would need to cut power through
  the PMIC over I2C).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/TimerLib.h>

#define EXYNOS_PMU_SWRESET  0x0400

/**
  Wait for pending writes to drain, then trigger the SoC software reset.
**/
STATIC
VOID
DoSwReset (
  VOID
  )
{
  UINT32  PmuBase = PcdGet32 (PcdPmuBaseAddress);

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
  DoSwReset ();
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  DoSwReset ();
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
  (void)DataSize;
  (void)ResetData;
  DoSwReset ();
}
