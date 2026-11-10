/** @file
  TouchDxe: the GT-N7000's touchscreen (MxtTouch.c) and S Pen (WacomPen.c)
  as one EFI_ABSOLUTE_POINTER_PROTOCOL.

  An absolute pointer has a single contact.  While the pen is in range it
  owns the pointer and finger input is ignored (resting the hand on the
  screen while writing); otherwise the first finger does.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "TouchDxe.h"

EXYNOS_I2C_PROTOCOL              *gTouchI2c;
EXYNOS_GPIO_PROTOCOL             *gTouchGpio;
EFI_HARDWARE_INTERRUPT_PROTOCOL  *gTouchInterrupt;

STATIC EFI_ABSOLUTE_POINTER_MODE   mMode;
STATIC EFI_ABSOLUTE_POINTER_STATE  mState;
STATIC BOOLEAN                     mStateChanged;
STATIC BOOLEAN                     mPenInRange;

STATIC EFI_ABSOLUTE_POINTER_PROTOCOL  mAbsolutePointer;

VOID
TouchReport (
  IN TOUCH_SOURCE  Source,
  IN BOOLEAN       InRange,
  IN UINT32        X,
  IN UINT32        Y,
  IN UINT32        Z,
  IN UINT32        Buttons
  )
{
  EFI_TPL  OldTpl;

  if (Source == TouchSourcePen) {
    mPenInRange = InRange;
  } else if (mPenInRange) {
    return;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (InRange) {
    mState.CurrentX = MIN (X, TOUCH_MAX_X);
    mState.CurrentY = MIN (Y, TOUCH_MAX_Y);
    mState.CurrentZ = Z;
  } else {
    mState.CurrentZ = 0;
    Buttons         = 0;
  }
  mState.ActiveButtons = Buttons;
  mStateChanged        = TRUE;
  gBS->RestoreTPL (OldTpl);
}

STATIC
EFI_STATUS
EFIAPI
TouchReset (
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
TouchGetState (
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
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mStateChanged) {
    gBS->SignalEvent (Event);
  }
}

EFI_STATUS
EFIAPI
TouchDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;
  EFI_STATUS  TouchStatus;
  EFI_STATUS  PenStatus;
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&gTouchI2c);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&gTouchGpio);
  }
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gHardwareInterruptProtocolGuid, NULL, (VOID **)&gTouchInterrupt);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mMode.AbsoluteMaxX = TOUCH_MAX_X;
  mMode.AbsoluteMaxY = TOUCH_MAX_Y;
  mMode.AbsoluteMaxZ = 255;
  mMode.Attributes   = EFI_ABSP_SupportsAltActive | EFI_ABSP_SupportsPressureAsZ;

  mAbsolutePointer.Reset    = TouchReset;
  mAbsolutePointer.GetState = TouchGetState;
  mAbsolutePointer.Mode     = &mMode;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, TouchWaitForInput, NULL,
                             &mAbsolutePointer.WaitForInput);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Either one alone is still a pointer.
  //
  TouchStatus = MxtTouchInit ();
  PenStatus   = WacomPenInit ();
  if (EFI_ERROR (TouchStatus) && EFI_ERROR (PenStatus)) {
    return TouchStatus;
  }

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (&Handle,
                                                 &gEfiAbsolutePointerProtocolGuid, &mAbsolutePointer,
                                                 NULL);
}
