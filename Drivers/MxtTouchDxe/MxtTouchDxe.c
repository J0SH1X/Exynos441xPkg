/** @file
  Atmel maXTouch mXT540E touchscreen (GT-N7000): EFI_ABSOLUTE_POINTER_PROTOCOL.

  Wiring (Linux mach-u1.c, gpio-q1.h): I2C bus 3 (GPA1[2:3]), address 0x4C;
  CHG on GPX0(4) = EINT4 = SPI 20; power through an LDO switched by GPL0(3).

  The Linux driver (drivers/input/touchscreen/mxt540e.c) writes its config
  and backs it up to the controller's NVM on every boot, so the chip comes
  up configured; this driver only powers it, reads the object table and
  decodes T9 (multi-touch) messages for the first finger.

  CHG is active low and stays low while messages are queued.  The EINT is
  level-low: the handler masks it and signals an event, the event drains the
  message queue over I2C at TPL_CALLBACK, then unmasks it again.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>
#include <Protocol/HardwareInterrupt.h>

#define MXT_I2C_BUS   3
#define MXT_I2C_ADDR  0x4C

#define MXT_LDO_GPIO  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPL0, 3)
#define MXT_CHG_GPIO  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPX0, 4)
#define MXT_SDA_GPIO  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPA1, 2)
#define MXT_SCL_GPIO  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPA1, 3)
#define GPIO_FUNC_I2C   3
#define GPIO_FUNC_EINT  0xF

//
// EINT 0-7 (GPX0) controls; CON is 4 bits per pin, 0 = low level.
//
#define EINT_BASE       0x11000000
#define EINT40_CON      (EINT_BASE + 0xE00)
#define EINT40_MASK     (EINT_BASE + 0xF00)
#define EINT40_PEND     (EINT_BASE + 0xF40)
#define MXT_EINT_BIT    BIT4
#define MXT_EINT_SHIFT  16
#define MXT_GIC_INTID   52        // EINT4 = SPI 20

#define MXT_POWER_ON_MS  130      // MXT540E_HW_RESET_TIME

//
// Object types and T9 message bits.
//
#define MXT_GEN_MESSAGEPROCESSOR_T5    5
#define MXT_TOUCH_MULTITOUCHSCREEN_T9  9
#define MXT_T9_DETECT   0x80
#define MXT_T9_RELEASE  0x20
#define MXT_REPORT_NONE 0xFF

#define MXT_INFO_SIZE          7
#define MXT_OBJECT_TABLE       7
#define MXT_OBJECT_ENTRY_SIZE  6
#define MXT_MAX_MSG_SIZE       16

STATIC EXYNOS_I2C_PROTOCOL              *mI2c;
STATIC EXYNOS_GPIO_PROTOCOL             *mGpio;
STATIC EFI_HARDWARE_INTERRUPT_PROTOCOL  *mInterrupt;

STATIC EFI_EVENT  mDrainEvent;

STATIC UINT16  mMsgAddr;          // T5
STATIC UINT8   mMsgSize;
STATIC UINT8   mT9FirstReport;    // report ID of T9 instance 0, finger 0
STATIC UINT16  mXRange;
STATIC UINT16  mYRange;
STATIC UINT8   mXDropBits;
STATIC UINT8   mYDropBits;

STATIC EFI_ABSOLUTE_POINTER_MODE   mMode;
STATIC EFI_ABSOLUTE_POINTER_STATE  mState;
STATIC BOOLEAN                     mStateChanged;

STATIC EFI_ABSOLUTE_POINTER_PROTOCOL  mAbsolutePointer;

/**
  mXT registers are addressed with a 16-bit little-endian offset.
**/
STATIC
EFI_STATUS
MxtRead (
  IN  UINT16  Reg,
  OUT UINT8   *Buf,
  IN  UINT16  Len
  )
{
  UINT8           Addr[2];
  EXYNOS_I2C_MSG  Msgs[2];

  Addr[0]       = (UINT8)Reg;
  Addr[1]       = (UINT8)(Reg >> 8);
  Msgs[0].Addr  = MXT_I2C_ADDR;
  Msgs[0].Flags = 0;
  Msgs[0].Len   = sizeof (Addr);
  Msgs[0].Buf   = Addr;
  Msgs[1].Addr  = MXT_I2C_ADDR;
  Msgs[1].Flags = I2C_M_RD;
  Msgs[1].Len   = Len;
  Msgs[1].Buf   = Buf;

  return (mI2c->Xfer (mI2c, MXT_I2C_BUS, Msgs, 2) == 2) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
VOID
MxtHandleMessage (
  IN UINT8  *Msg
  )
{
  UINT32  X;
  UINT32  Y;

  //
  // First finger only: an absolute pointer has one contact.
  //
  if (Msg[0] != mT9FirstReport) {
    return;
  }

  if ((Msg[1] & MXT_T9_RELEASE) != 0) {
    mState.ActiveButtons &= ~EFI_ABSP_TouchActive;
    mState.CurrentZ       = 0;
  } else if ((Msg[1] & MXT_T9_DETECT) != 0) {
    X = (((UINT32)Msg[2] << 4) | (Msg[4] >> 4)) >> mXDropBits;
    Y = (((UINT32)Msg[3] << 4) | (Msg[4] & 0xF)) >> mYDropBits;
    mState.CurrentX       = MIN (X, mXRange);
    mState.CurrentY       = MIN (Y, mYRange);
    mState.CurrentZ       = Msg[6];      // amplitude
    mState.ActiveButtons |= EFI_ABSP_TouchActive;
  } else {
    return;
  }

  mStateChanged = TRUE;
}

/**
  Drain the message queue (CHG goes high once it is empty), then let the
  EINT fire again.
**/
STATIC
VOID
EFIAPI
MxtDrain (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT8  Msg[MXT_MAX_MSG_SIZE];
  UINTN  Count;

  for (Count = 0; Count < 64; Count++) {
    if (mGpio->Get (mGpio, MXT_CHG_GPIO) != 0) {
      break;
    }
    if (EFI_ERROR (MxtRead (mMsgAddr, Msg, mMsgSize)) || (Msg[0] == MXT_REPORT_NONE)) {
      break;
    }
    MxtHandleMessage (Msg);
  }

  MmioWrite32 (EINT40_PEND, MXT_EINT_BIT);
  MmioAnd32 (EINT40_MASK, ~(UINT32)MXT_EINT_BIT);
}

STATIC
VOID
EFIAPI
MxtInterruptHandler (
  IN HARDWARE_INTERRUPT_SOURCE  Source,
  IN EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  //
  // Level-low: keep it masked until the queue has been read.
  //
  MmioOr32 (EINT40_MASK, MXT_EINT_BIT);
  MmioWrite32 (EINT40_PEND, MXT_EINT_BIT);
  mInterrupt->EndOfInterrupt (mInterrupt, Source);
  gBS->SignalEvent (mDrainEvent);
}

STATIC
EFI_STATUS
EFIAPI
MxtReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  EFI_TPL  OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ZeroMem (&mState, sizeof (mState));
  mStateChanged = FALSE;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MxtGetState (
  IN     EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN OUT EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mStateChanged) {
    CopyMem (State, &mState, sizeof (*State));
    mStateChanged = FALSE;
    Status        = EFI_SUCCESS;
  } else {
    Status = EFI_NOT_READY;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
VOID
EFIAPI
MxtWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mStateChanged) {
    gBS->SignalEvent (Event);
  }
}

/**
  Read the info block and object table: find T5 and T9, and work out the
  coordinate format the way the Linux driver does (a range below 1024 is
  reported in 12-bit units, i.e. 2 extra bits).
**/
STATIC
EFI_STATUS
MxtProbe (
  VOID
  )
{
  UINT8       Info[MXT_INFO_SIZE];
  UINT8       *Table;
  UINT8       *Obj;
  UINT8       T9[22];
  UINT16      T9Addr;
  UINT8       ReportId;
  UINTN       Index;
  EFI_STATUS  Status;

  Status = MxtRead (0, Info, sizeof (Info));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "mXT: no answer at 0x%02x on I2C%u\n", MXT_I2C_ADDR, MXT_I2C_BUS));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "mXT: family 0x%02x variant 0x%02x version 0x%02x build 0x%02x, %u objects\n",
          Info[0], Info[1], Info[2], Info[3], Info[6]));

  Table = AllocatePool (Info[6] * MXT_OBJECT_ENTRY_SIZE);
  if (Table == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = MxtRead (MXT_OBJECT_TABLE, Table, Info[6] * MXT_OBJECT_ENTRY_SIZE);
  if (EFI_ERROR (Status)) {
    FreePool (Table);
    return Status;
  }

  T9Addr   = 0;
  ReportId = 1;
  for (Index = 0; Index < Info[6]; Index++) {
    Obj = Table + Index * MXT_OBJECT_ENTRY_SIZE;
    switch (Obj[0]) {
      case MXT_GEN_MESSAGEPROCESSOR_T5:
        mMsgAddr = Obj[1] | ((UINT16)Obj[2] << 8);
        mMsgSize = MIN (Obj[3] + 1, MXT_MAX_MSG_SIZE);
        break;
      case MXT_TOUCH_MULTITOUCHSCREEN_T9:
        T9Addr         = Obj[1] | ((UINT16)Obj[2] << 8);
        mT9FirstReport = ReportId;
        break;
    }
    // Obj[4] = instances - 1, Obj[5] = report IDs per instance.
    ReportId += Obj[5] * (Obj[4] + 1);
  }
  FreePool (Table);

  if ((mMsgAddr == 0) || (T9Addr == 0)) {
    DEBUG ((DEBUG_ERROR, "mXT: no T5/T9 object\n"));
    return EFI_UNSUPPORTED;
  }

  Status = MxtRead (T9Addr, T9, sizeof (T9));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // T9: [9] ORIENT (bit 0 swaps X/Y), [18-19] XRANGE, [20-21] YRANGE.
  //
  mXRange = T9[18] | ((UINT16)T9[19] << 8);
  mYRange = T9[20] | ((UINT16)T9[21] << 8);
  if ((T9[9] & BIT0) != 0) {
    mXDropBits = ((T9[21] & 0xC) == 0) ? 2 : 0;
    mYDropBits = ((T9[19] & 0xC) == 0) ? 2 : 0;
  } else {
    mXDropBits = ((T9[19] & 0xC) == 0) ? 2 : 0;
    mYDropBits = ((T9[21] & 0xC) == 0) ? 2 : 0;
  }

  DEBUG ((DEBUG_INFO, "mXT: T5 @0x%x (%u bytes), T9 @0x%x report %u, ctrl 0x%02x, range %ux%u\n",
          mMsgAddr, mMsgSize, T9Addr, mT9FirstReport, T9[0], mXRange + 1, mYRange + 1));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MxtTouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&mI2c);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&mGpio);
  }
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gHardwareInterruptProtocolGuid, NULL, (VOID **)&mInterrupt);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Power (mxt540e_power_on()): I2C3 pins, LDO on, wait for the chip, CHG
  // as EINT.  The bootloader may have left the LDO on already.
  //
  mGpio->Config (mGpio, MXT_SDA_GPIO, GPIO_FUNC_I2C, EXYNOS_GPIO_PULL_UP);
  mGpio->Config (mGpio, MXT_SCL_GPIO, GPIO_FUNC_I2C, EXYNOS_GPIO_PULL_UP);
  mGpio->Config (mGpio, MXT_LDO_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  if (mGpio->Get (mGpio, MXT_LDO_GPIO) == 0) {
    mGpio->Set (mGpio, MXT_LDO_GPIO, 1);
    gBS->Stall (MXT_POWER_ON_MS * 1000);
  }
  mGpio->Config (mGpio, MXT_CHG_GPIO, GPIO_FUNC_EINT, EXYNOS_GPIO_PULL_NONE);

  Status = MxtProbe ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mMode.AbsoluteMinX = 0;
  mMode.AbsoluteMinY = 0;
  mMode.AbsoluteMinZ = 0;
  mMode.AbsoluteMaxX = mXRange;
  mMode.AbsoluteMaxY = mYRange;
  mMode.AbsoluteMaxZ = 255;
  mMode.Attributes   = EFI_ABSP_SupportsPressureAsZ;

  mAbsolutePointer.Reset    = MxtReset;
  mAbsolutePointer.GetState = MxtGetState;
  mAbsolutePointer.Mode     = &mMode;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, MxtWaitForInput, NULL,
                             &mAbsolutePointer.WaitForInput);
  if (!EFI_ERROR (Status)) {
    Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, MxtDrain, NULL, &mDrainEvent);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // EINT4 level-low, masked until the GIC side is ready.
  //
  MmioOr32 (EINT40_MASK, MXT_EINT_BIT);
  MmioAndThenOr32 (EINT40_CON, ~(0x7u << MXT_EINT_SHIFT), 0x0u << MXT_EINT_SHIFT);
  MmioWrite32 (EINT40_PEND, MXT_EINT_BIT);

  Status = mInterrupt->RegisterInterruptSource (mInterrupt, MXT_GIC_INTID, MxtInterruptHandler);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (&Handle,
                                                   &gEfiAbsolutePointerProtocolGuid, &mAbsolutePointer,
                                                   NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Read whatever queued up since power-on (this also unmasks the EINT).
  //
  gBS->SignalEvent (mDrainEvent);
  return EFI_SUCCESS;
}
