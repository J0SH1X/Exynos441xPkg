/** @file
  Timer Architecture Protocol driver on the Exynos4 Multi-Core Timer (MCT).

  Drives the MCT's global comparator 0 (G_COMP0) against the free-running
  64-bit global counter (G_CNT, 24 MHz).  This is the same clock event the
  Linux kernel uses for CPU0 (exynos4_clockevent_init(), mct-comp), so the
  wiring is known-good on this SoC.

  The comparator is programmed in one-shot mode and re-armed on every tick,
  rather than using the hardware's auto-increment periodic mode.  One-shot
  re-arming keeps the tick rate independent of interrupt latency: a late
  handler simply gets a fresh full period instead of drifting against a
  free-running auto-increment buffer.

  G_COMP0 is a shared peripheral interrupt (SPI 57 = GIC INTID 89), so it
  arrives at whatever core the GIC routes it to; on this uni-core bring-up
  that is CPU0 via the normal ArmGicDxe path.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/Timer.h>
#include <Protocol/HardwareInterrupt.h>

//
// Exynos4 MCT register offsets (Linux arch/arm/mach-exynos/include/mach/regs-mct.h).
//
#define MCT_G_CNT_L       0x100
#define MCT_G_CNT_U       0x104
#define MCT_G_CNT_WSTAT   0x110

#define MCT_G_COMP0_L     0x200
#define MCT_G_COMP0_U     0x204
#define MCT_G_COMP0_INCR  0x208

#define MCT_G_TCON        0x240
#define MCT_G_INT_CSTAT   0x244
#define MCT_G_INT_ENB     0x248
#define MCT_G_WSTAT       0x24C

#define MCT_G_TCON_COMP0_ENABLE  (1u << 0)
#define MCT_G_TCON_COMP0_AUTOINC (1u << 1)
#define MCT_G_TCON_START         (1u << 8)

//
// G_WSTAT write-status bits: each writable register has its own bit, which
// the MCT raises once the write has been applied and which is cleared by
// writing it back (the kernel's exynos4_mct_write() does exactly this).
//
#define MCT_G_WSTAT_COMP0_L     (1u << 0)
#define MCT_G_WSTAT_COMP0_U     (1u << 1)
#define MCT_G_WSTAT_COMP0_INCR  (1u << 2)
#define MCT_G_WSTAT_TCON        (1u << 16)

#define MCT_BASE   PcdGet32 (PcdMctBaseAddress)
#define MCT_FREQ   PcdGet32 (PcdMctFrequencyHz)

// The notification function to call on every timer interrupt.
EFI_TIMER_NOTIFY  mTimerNotifyFunction = (EFI_TIMER_NOTIFY)NULL;

// The current period of the timer interrupt, in 100 ns units.
UINT64  mTimerPeriod = 0;

// Comparator reload, in MCT ticks (24 MHz), the comparator value currently
// programmed, and the number of periods that elapsed since the last handler
// call (covers long handlers that miss ticks).
UINT64  mTimerTicks      = 0;
UINT64  mTimerCompare    = 0;
UINT64  mElapsedPeriod   = 1;

// Cached copy of the Hardware Interrupt protocol instance.
EFI_HARDWARE_INTERRUPT_PROTOCOL  *gInterrupt = NULL;

/**
  Write an MCT register and wait for the write-status handshake.

  Not every MCT register latches immediately; G_COMP0_L/U and G_TCON all
  require waiting for the matching G_WSTAT bit.  Bounded like the kernel's
  exynos4_mct_write(): on a healthy SoC the bit sets within a few reads, and
  we would rather carry on than spin forever if the MCT is absent.

  @param[in] Offset  Register offset from the MCT base.
  @param[in] Value   Value to write.
  @param[in] Wstat   G_WSTAT bit that acknowledges this register.
**/
STATIC
VOID
MctWrite (
  IN UINT32  Offset,
  IN UINT32  Value,
  IN UINT32  Wstat
  )
{
  UINT32  Tries;

  MmioWrite32 (MCT_BASE + Offset, Value);

  for (Tries = 0x1000; Tries != 0; Tries--) {
    if ((MmioRead32 (MCT_BASE + MCT_G_WSTAT) & Wstat) != 0) {
      // Write-one-to-clear the status bit so the next write can be observed.
      MmioWrite32 (MCT_BASE + MCT_G_WSTAT, Wstat);
      return;
    }
  }
}

/**
  Read the 64-bit global counter.  The counter increments between the two
  32-bit reads, so re-read the high word until it is stable.
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
    Hi  = Hi2;
    Lo  = MmioRead32 (MCT_BASE + MCT_G_CNT_L);
    Hi2 = MmioRead32 (MCT_BASE + MCT_G_CNT_U);
  } while (Hi != Hi2);

  return ((UINT64)Hi << 32) | Lo;
}

/**
  Start the free-running counter if the bootloader left it stopped.

  TCON.START gates the whole MCT counter; without it the comparator never
  fires.  The BL normally leaves it running, but we must not assume that.
**/
STATIC
VOID
MctEnsureCounterStarted (
  VOID
  )
{
  UINT32  Tcon;

  Tcon = MmioRead32 (MCT_BASE + MCT_G_TCON);
  if ((Tcon & MCT_G_TCON_START) != 0) {
    return;
  }

  MctWrite (MCT_G_TCON, Tcon | MCT_G_TCON_START, MCT_G_WSTAT_TCON);
}

/**
  Disable comparator 0 and its interrupt.
**/
STATIC
VOID
MctComparatorStop (
  VOID
  )
{
  UINT32  Tcon;

  Tcon = MmioRead32 (MCT_BASE + MCT_G_TCON);
  Tcon &= ~(MCT_G_TCON_COMP0_ENABLE | MCT_G_TCON_COMP0_AUTOINC);

  MctWrite (MCT_G_TCON, Tcon, MCT_G_WSTAT_TCON);
  MmioWrite32 (MCT_BASE + MCT_G_INT_ENB, 0);
}

/**
  Arm comparator 0 to fire MctTicks counts from now.
**/
STATIC
VOID
MctComparatorArm (
  IN UINT64  MctTicks
  )
{
  UINT64  Compare;
  UINT32  Tcon;

  // Reload from "now" so a delayed handler cannot arm a comparator value
  // that has already passed, which would interrupt storm.
  Compare = MctReadCounter () + MctTicks;

  MctWrite (MCT_G_COMP0_L, (UINT32)Compare, MCT_G_WSTAT_COMP0_L);
  MctWrite (MCT_G_COMP0_U, (UINT32)(Compare >> 32), MCT_G_WSTAT_COMP0_U);

  // Remember the value we programmed so the handler can work out how much
  // time actually elapsed when it was late.
  mTimerCompare = Compare;

  MmioWrite32 (MCT_BASE + MCT_G_INT_ENB, 1);

  Tcon = MmioRead32 (MCT_BASE + MCT_G_TCON);
  MctWrite (MCT_G_TCON, Tcon | MCT_G_TCON_COMP0_ENABLE, MCT_G_WSTAT_TCON);
}

/**
  Register the handler NotifyFunction so it is called every time the timer
  interrupt fires.  NULL unregisters it.
**/
EFI_STATUS
EFIAPI
TimerDriverRegisterHandler (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN EFI_TIMER_NOTIFY         NotifyFunction
  )
{
  if ((NotifyFunction == NULL) && (mTimerNotifyFunction == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((NotifyFunction != NULL) && (mTimerNotifyFunction != NULL)) {
    return EFI_ALREADY_STARTED;
  }

  mTimerNotifyFunction = NotifyFunction;

  return EFI_SUCCESS;
}

/**
  Stop the timer when the DXE Core leaves boot services: nothing past ExitBoot
  Services cares about the tick, and the OS will reprogram the MCT itself.
**/
STATIC
VOID
EFIAPI
ExitBootServicesEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  MctComparatorStop ();
}

/**
  Adjust the period of timer interrupts to TimerPeriod, in 100 ns units.
  A TimerPeriod of 0 disables the timer interrupt.
**/
EFI_STATUS
EFIAPI
TimerDriverSetTimerPeriod (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN UINT64                   TimerPeriod
  )
{
  EFI_TPL  OriginalTPL;

  MctComparatorStop ();

  if (TimerPeriod != 0) {
    //
    // Ticks = TimerPeriod[100ns] x Freq[Hz] / 1e7.
    // The product stays well inside a UINT64 for any sane period (1000000
    // units = 0.1 s -> 2.4e12 ticks), so do the multiply first and divide
    // once rather than truncating twice.
    //
    mTimerTicks = DivU64x32 (MultU64x32 (TimerPeriod, MCT_FREQ), 10000000U);
    if (mTimerTicks == 0) {
      mTimerTicks = 1;
    }

    // Raise TPL so the period and the reload stay coherent with the handler.
    OriginalTPL = gBS->RaiseTPL (TPL_HIGH_LEVEL);
    mTimerPeriod   = TimerPeriod;
    mElapsedPeriod = 1;
    gBS->RestoreTPL (OriginalTPL);

    MctComparatorArm (mTimerTicks);
  } else {
    OriginalTPL = gBS->RaiseTPL (TPL_HIGH_LEVEL);
    mTimerPeriod   = TimerPeriod;
    mElapsedPeriod = 1;
    gBS->RestoreTPL (OriginalTPL);
  }

  return EFI_SUCCESS;
}

/**
  Retrieve the period of timer interrupts, in 100 ns units.  0 means the
  timer is currently disabled.
**/
EFI_STATUS
EFIAPI
TimerDriverGetTimerPeriod (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  OUT UINT64                  *TimerPeriod
  )
{
  if (TimerPeriod == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *TimerPeriod = mTimerPeriod;
  return EFI_SUCCESS;
}

/**
  Generate a soft timer interrupt.  The MCT comparator has no software trigger,
  so this is unsupported.
**/
EFI_STATUS
EFIAPI
TimerDriverGenerateSoftInterrupt (
  IN EFI_TIMER_ARCH_PROTOCOL  *This
  )
{
  return EFI_UNSUPPORTED;
}

//
// Interface structure for the Timer Architectural Protocol.
//
EFI_TIMER_ARCH_PROTOCOL  gTimer = {
  TimerDriverRegisterHandler,
  TimerDriverSetTimerPeriod,
  TimerDriverGetTimerPeriod,
  TimerDriverGenerateSoftInterrupt
};

/**
  Interrupt handler: acknowledge the MCT comparator, re-arm it, and let the
  DXE Core know that a tick elapsed.
**/
STATIC
VOID
EFIAPI
TimerInterruptHandler (
  IN  HARDWARE_INTERRUPT_SOURCE  Source,
  IN  EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  EFI_TPL  OriginalTPL;
  UINT64   Current;

  //
  // The DXE Core's tick bookkeeping runs at TPL_HIGH, so raise it here or a
  // nested timer event could be signalled with the core's state inconsistent.
  //
  OriginalTPL = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  // Signal end of interrupt early to help avoid losing subsequent ticks from
  // long duration handlers.
  gInterrupt->EndOfInterrupt (gInterrupt, Source);

  // Clear the comparator interrupt status.  The MCT latches CSTAT until it is
  // written, and the hardware requires it to be cleared twice (the kernel's
  // exynos4_mct_tick_clear() documents and works around this).
  MmioWrite32 (MCT_BASE + MCT_G_INT_CSTAT, 1);
  MmioWrite32 (MCT_BASE + MCT_G_INT_CSTAT, 1);

  if (mTimerNotifyFunction != NULL) {
    //
    // Work out how many periods elapsed.  The comparator fired at
    // mTimerCompare; anything past that means the handler was late and the
    // DXE Core should be told about the missed time rather than have it
    // quietly disappear.  Then re-arm from now (MctComparatorArm reloads from
    // the live counter), so a late handler cannot arm a comparator value that
    // is already in the past.
    //
    Current = MctReadCounter ();
    if (Current > mTimerCompare) {
      mElapsedPeriod = DivU64x32 (Current - mTimerCompare, mTimerTicks) + 1;
    } else {
      mElapsedPeriod = 1;
    }

    MctComparatorArm (mTimerTicks);

    mTimerNotifyFunction (mTimerPeriod * mElapsedPeriod);
  }

  gBS->RestoreTPL (OriginalTPL);
}

/**
  Entry point: hook the MCT comparator into the GIC and publish the Timer
  Architectural Protocol.
**/
EFI_STATUS
EFIAPI
TimerInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT64      TimerPeriod;
  EFI_EVENT   ExitEvent;

  Status = gBS->LocateProtocol (
                  &gHardwareInterruptProtocolGuid,
                  NULL,
                  (VOID **)&gInterrupt
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // The comparator interrupt must be routed before it can be enabled.
  Status = gInterrupt->RegisterInterruptSource (
                         gInterrupt,
                         PcdGet32 (PcdMctGlobalTimerIrq),
                         TimerInterruptHandler
                         );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gInterrupt->EnableInterruptSource (
                         gInterrupt,
                         PcdGet32 (PcdMctGlobalTimerIrq)
                         );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Publish the protocol the DXE Core uses to drive its tick.
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiTimerArchProtocolGuid,
                  &gTimer,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Turn the tick off at ExitBootServices: the OS owns the MCT from there.
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  ExitBootServicesEvent,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &ExitEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MctEnsureCounterStarted ();
  MctComparatorStop ();

  // Default tick rate.  PcdTimerPeriod is in 100 ns units; 10000 = 1 ms,
  // the conventional EDK II platform default.
  TimerPeriod = PcdGet32 (PcdTimerPeriod);
  if (TimerPeriod == 0) {
    TimerPeriod = 10000;
  }

  return TimerDriverSetTimerPeriod (&gTimer, TimerPeriod);
}
