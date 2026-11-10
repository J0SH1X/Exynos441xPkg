/** @file
  TouchDxe: GT-N7000 touchscreen (Atmel mXT540E) and S Pen (Wacom G5SP)
  behind one EFI_ABSOLUTE_POINTER_PROTOCOL.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef TOUCH_DXE_H_
#define TOUCH_DXE_H_

#include <Uefi.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>
#include <Protocol/HardwareInterrupt.h>

//
// The pointer's range is the pen's (the finer of the two), in screen
// orientation: X along the 800-pixel side, Y along the 1280-pixel side.
// Finger coordinates are scaled up to it.
//
#define TOUCH_MAX_X  7119
#define TOUCH_MAX_Y  11391

typedef enum {
  TouchSourceFinger,
  TouchSourcePen
} TOUCH_SOURCE;

extern EXYNOS_I2C_PROTOCOL              *gTouchI2c;
extern EXYNOS_GPIO_PROTOCOL             *gTouchGpio;
extern EFI_HARDWARE_INTERRUPT_PROTOCOL  *gTouchInterrupt;

/**
  Report a contact.  X/Y are in TOUCH_MAX_X/Y units, Z is pressure 0..255,
  Buttons are EFI_ABSP_TouchActive / EFI_ABS_AltActive.  InRange FALSE
  means the source has gone (finger lifted, pen out of range).
**/
VOID
TouchReport (
  IN TOUCH_SOURCE  Source,
  IN BOOLEAN       InRange,
  IN UINT32        X,
  IN UINT32        Y,
  IN UINT32        Z,
  IN UINT32        Buttons
  );

EFI_STATUS
MxtTouchInit (
  VOID
  );

EFI_STATUS
WacomPenInit (
  VOID
  );

#endif
