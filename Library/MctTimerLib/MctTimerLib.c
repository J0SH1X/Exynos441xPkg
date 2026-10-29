/** @file
  TimerLib on the Exynos4 Multi-Core Timer (MCT) free-running counter.

  The MCT's 64-bit global counter (G_CNT) is clocked directly by the 24 MHz
  crystal (the kernel uses the same source: clk_get("xtal")).  We make sure
  the counter is started (TCON.START) with the required write-status
  handshake, then implement the whole TimerLib API on top of it.

  No DEBUG() calls: this library is linked into SEC/PrePi where the
  framebuffer serial port already lives and printing could recurse.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>

// Exynos4 MCT register offsets (Linux arch/arm/mach-exynos/include/mach/regs-mct.h)
#define MCT_G_CNT_L      0x100
#define MCT_G_CNT_U      0x104
#define MCT_G_CNT_WSTAT  0x110
#define MCT_G_TCON       0x240
#define MCT_G_WSTAT      0x24C

#define MCT_G_TCON_START      (1u << 8)
#define MCT_G_WSTAT_TCON      (1u << 16)  // G_TCON write status
#define MCT_G_CNT_WSTAT_L     (1u << 0)
#define MCT_G_CNT_WSTAT_U     (1u << 1)

#define MCT_BASE PcdGet32 (PcdMctBaseAddress)
#define MCT_HZ   PcdGet32 (PcdMctFrequencyHz)

/**
  Make sure the free-running counter is started.  TCON writes must be
  acknowledged through G_WSTAT (write-one-to-clear), like the kernel's
  exynos4_mct_write() does.
**/
STATIC
VOID
MctEnsureStarted (
  VOID
  )
{
  UINT32  Tcon = MmioRead32 (MCT_BASE + MCT_G_TCON);

  if ((Tcon & MCT_G_TCON_START) != 0) {
    return;
  }

  MmioWrite32 (MCT_BASE + MCT_G_TCON, Tcon | MCT_G_TCON_START);

  // Wait for the write to be applied (bounded; the kernel allows 0x1000).
  {
    UINT32  Tries = 0x10000;
    while (Tries-- != 0) {
      UINT32  Wstat = MmioRead32 (MCT_BASE + MCT_G_WSTAT);
      if ((Wstat & MCT_G_WSTAT_TCON) != 0) {
        MmioWrite32 (MCT_BASE + MCT_G_WSTAT, MCT_G_WSTAT_TCON);
        return;
      }
    }
  }
}

/**
  Read the 64-bit counter with the lo/hi/hi consistency loop (the counter
  can increment between the two 32-bit reads).
**/
STATIC
UINT64
MctReadCounter (
  VOID
  )
{
  UINT32  Hi;
  UINT32  Hi2;
  UINT32  Lo;

  Hi2 = MmioRead32 (MCT_BASE + MCT_G_CNT_U);
  do {
    Hi = Hi2;
    Lo = MmioRead32 (MCT_BASE + MCT_G_CNT_L);
    Hi2 = MmioRead32 (MCT_BASE + MCT_G_CNT_U);
  } while (Hi != Hi2);

  return ((UINT64)Hi << 32) | Lo;
}

/**
  Retrieve the current value of a 64-bit free running performance counter.
**/
UINT64
EFIAPI
GetPerformanceCounter (
  VOID
  )
{
  return MctReadCounter ();
}

/**
  Retrieves the start and end values of the performance counter.
**/
UINT64
EFIAPI
GetPerformanceCounterProperties (
  OUT UINT64  *StartValue  OPTIONAL,
  OUT UINT64  *EndValue    OPTIONAL
  )
{
  if (StartValue != NULL) {
    *StartValue = 0;
  }

  if (EndValue != NULL) {
    *EndValue = MAX_UINT64;
  }

  return MCT_HZ;
}

/**
  Converts the current performance counter value to time in nanoseconds.
**/
UINT64
EFIAPI
GetTimeInNanoSecond (
  IN UINT64  Ticks
  )
{
  UINT64  Seconds;
  UINT32  Remainder32;
  UINT64  Remainder;

  // ns = Ticks * 1e9 / Frequency, split so the multiply cannot overflow:
  // Remainder is < Frequency (24e6), so Remainder * 1e9 still fits in u64.
  Seconds   = DivU64x32Remainder (Ticks, MCT_HZ, &Remainder32);
  Remainder = Remainder32;
  return Seconds * 1000000000ULL +
         DivU64x32 (Remainder * 1000000000ULL, MCT_HZ);
}

/**
  Stalls the CPU for at least the given number of microseconds.
**/
UINTN
EFIAPI
MicroSecondDelay (
  IN UINTN  MicroSeconds
  )
{
  UINT64  Ticks;
  UINT64  Start;
  UINT64  Now;

  MctEnsureStarted ();

  Ticks = MultU64x32 ((UINT64)MicroSeconds, MCT_HZ / 1000000);
  Start = MctReadCounter ();
  do {
    Now = MctReadCounter ();
  } while ((Now - Start) < Ticks);

  return MicroSeconds;
}

/**
  Stalls the CPU for at least the given number of nanoseconds.
**/
UINTN
EFIAPI
NanoSecondDelay (
  IN UINTN  NanoSeconds
  )
{
  UINT64  Ticks;
  UINT64  Start;
  UINT64  Now;

  MctEnsureStarted ();

  // Ticks = ns * 24MHz / 1e9 = ns * 24 / 1000 (keep it overflow-free)
  Ticks = DivU64x32 (MultU64x32 ((UINT64)NanoSeconds, MCT_HZ), 1000000000);
  if (Ticks == 0) {
    Ticks = 1;
  }

  Start = MctReadCounter ();
  do {
    Now = MctReadCounter ();
  } while ((Now - Start) < Ticks);

  return NanoSeconds;
}
