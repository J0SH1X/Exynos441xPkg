/** @file
  S Pen digitizer (Wacom G5SP) on the GT-N7000.

  Wiring (Linux mach-u1.c, gpio-q1.h): I2C bus 6 (GPC1[3:4]), address 0x56;
  IRQ on GPE0(0), RESET GPE0(2), SLP GPE1(7).  GPE0 is not an EINT bank:
  its interrupt goes through the GPIO interrupt block of GPIO part 1, all of
  whose groups share SPI 47.

  Every rising edge of IRQ means a 7-byte coordinate packet is ready
  (drivers/input/touchscreen/wacom/wacom_i2c_func.c, wacom_i2c_coord()):
    [0] status: 0x80 in range, 0x40 eraser, 0x20 side button, 0x10 tip
    [1-2] X, [3-4] Y, [5-6] pressure (big endian)
  in the digitizer's own orientation; Q1 subtracts an origin offset, inverts
  X and swaps the axes to get screen coordinates.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "TouchDxe.h"

#define PEN_I2C_BUS   6
#define PEN_I2C_ADDR  0x56

#define PEN_IRQ_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPE0, 0)
#define PEN_RESET_GPIO  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPE0, 2)
#define PEN_SLP_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPE1, 7)
#define PEN_PDCT_GPIO   EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPE1, 6)
#define PEN_SDA_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC1, 3)
#define PEN_SCL_GPIO    EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPC1, 4)
#define GPIO_FUNC_I2C6  4
#define GPIO_FUNC_INT   0xF

//
// GPIO part 1 interrupt block, GPE0's registers (eint_offset 0x1C).
//
#define GPIO1_BASE         0x11400000
#define GPE0_INT_CON       (GPIO1_BASE + 0x700 + 0x1C)
#define GPE0_INT_MASK      (GPIO1_BASE + 0x900 + 0x1C)
#define GPE0_INT_PEND      (GPIO1_BASE + 0xA00 + 0x1C)
#define PEN_INT_BIT        BIT0
#define GPIO_INT_EDGE_RISING  3
#define PEN_GIC_INTID      79      // IRQ_GPIO_XA = SPI 47

#define WACOM_COM_QUERY      0x2A
#define WACOM_QUERY_SIZE     9
#define WACOM_QUERY_DELAY_MS 100   // between query and answer (wacom_i2c_query())
#define WACOM_COORD_SIZE     7
#define WACOM_RESET_MS       200   // wacom_i2c_reset_hw()

#define WACOM_IN_RANGE  0x80
#define WACOM_SIDE      0x20
#define WACOM_TIP       0x10

//
// Q1 (include/linux/wacom_i2c.h, wacom_i2c_coord_table.h).
//
#define WACOM_MAX_COORD_X  11392
#define WACOM_ORIGIN_X     600
#define WACOM_ORIGIN_Y     620

STATIC EFI_EVENT  mPenEvent;

STATIC
EFI_STATUS
PenRead (
  OUT UINT8  *Buf,
  IN  UINT16 Len
  )
{
  EXYNOS_I2C_MSG  Msg;

  Msg.Addr  = PEN_I2C_ADDR;
  Msg.Flags = I2C_M_RD;
  Msg.Len   = Len;
  Msg.Buf   = Buf;

  return (gTouchI2c->Xfer (gTouchI2c, PEN_I2C_BUS, &Msg, 1) == 1) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
VOID
PenHandlePacket (
  IN UINT8  *Data
  )
{
  INT32   RawX;
  INT32   RawY;
  INT32   X;
  INT32   Y;
  UINT32  Pressure;
  UINT32  Buttons;

  if ((Data[0] & WACOM_IN_RANGE) == 0) {
    TouchReport (TouchSourcePen, FALSE, 0, 0, 0, 0);
    return;
  }

  RawX     = ((INT32)Data[1] << 8) | Data[2];
  RawY     = ((INT32)Data[3] << 8) | Data[4];
  Pressure = ((UINT32)Data[5] << 8) | Data[6];

  RawX = MAX (RawX - WACOM_ORIGIN_X, 0);
  RawY = MAX (RawY - WACOM_ORIGIN_Y, 0);
  X    = RawY;                            // xy_switch
  Y    = WACOM_MAX_COORD_X - RawX;        // x_invert
  if ((X > TOUCH_MAX_X) || (Y < 0) || (Y > TOUCH_MAX_Y)) {
    return;
  }

  Buttons = 0;
  if ((Data[0] & WACOM_TIP) != 0) {
    Buttons |= EFI_ABSP_TouchActive;
  }
  if ((Data[0] & WACOM_SIDE) != 0) {
    Buttons |= EFI_ABS_AltActive;
  }

  TouchReport (TouchSourcePen, TRUE, (UINT32)X, (UINT32)Y, MIN (Pressure, 255), Buttons);
}

STATIC
VOID
EFIAPI
PenDrain (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT8  Data[WACOM_COORD_SIZE];

  if (!EFI_ERROR (PenRead (Data, sizeof (Data)))) {
    PenHandlePacket (Data);
  }

  MmioAnd32 (GPE0_INT_MASK, ~(UINT32)PEN_INT_BIT);
}

STATIC
VOID
EFIAPI
PenInterruptHandler (
  IN HARDWARE_INTERRUPT_SOURCE  Source,
  IN EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  //
  // SPI 47 is shared by all GPIO part 1 groups; only GPE0(0) is enabled.
  //
  if ((MmioRead32 (GPE0_INT_PEND) & PEN_INT_BIT) != 0) {
    MmioOr32 (GPE0_INT_MASK, PEN_INT_BIT);
    MmioWrite32 (GPE0_INT_PEND, PEN_INT_BIT);
    gBS->SignalEvent (mPenEvent);
  }
  gTouchInterrupt->EndOfInterrupt (gTouchInterrupt, Source);
}

/**
  Ask the digitizer who it is (also proves it is there).
**/
STATIC
EFI_STATUS
PenQuery (
  VOID
  )
{
  UINT8           Command;
  UINT8           Data[WACOM_QUERY_SIZE];
  EXYNOS_I2C_MSG  Msg;
  UINTN           Try;

  for (Try = 0; Try < 3; Try++) {
    Command   = WACOM_COM_QUERY;
    Msg.Addr  = PEN_I2C_ADDR;
    Msg.Flags = 0;
    Msg.Len   = 1;
    Msg.Buf   = &Command;
    if (gTouchI2c->Xfer (gTouchI2c, PEN_I2C_BUS, &Msg, 1) != 1) {
      continue;
    }
    gBS->Stall (WACOM_QUERY_DELAY_MS * 1000);
    if (!EFI_ERROR (PenRead (Data, sizeof (Data))) && (Data[0] == 0x0F)) {
      DEBUG ((DEBUG_INFO, "Wacom: max %u x %u, pressure %u, firmware 0x%02x%02x\n",
              (Data[1] << 8) | Data[2], (Data[3] << 8) | Data[4],
              (Data[5] << 8) | Data[6], Data[7], Data[8]));
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_ERROR, "Wacom: no answer at 0x%02x on I2C%u\n", PEN_I2C_ADDR, PEN_I2C_BUS));
  return EFI_NOT_FOUND;
}

EFI_STATUS
WacomPenInit (
  VOID
  )
{
  EFI_STATUS  Status;

  //
  // p6_wacom_init_hw(): awake, IRQ as GPIO interrupt.
  //
  gTouchGpio->Config (gTouchGpio, PEN_SDA_GPIO, GPIO_FUNC_I2C6, EXYNOS_GPIO_PULL_UP);
  gTouchGpio->Config (gTouchGpio, PEN_SCL_GPIO, GPIO_FUNC_I2C6, EXYNOS_GPIO_PULL_UP);
  gTouchGpio->Config (gTouchGpio, PEN_SLP_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_UP);
  gTouchGpio->Set (gTouchGpio, PEN_SLP_GPIO, 0);
  gTouchGpio->Config (gTouchGpio, PEN_IRQ_GPIO, GPIO_FUNC_INT, EXYNOS_GPIO_PULL_NONE);

  //
  // wacom_i2c_reset_hw() as the Q1 probe does it: suspend (PDCT input,
  // RESET low), resume (PDCT output high, RESET high).
  //
  gTouchGpio->Config (gTouchGpio, PEN_PDCT_GPIO, EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_UP);
  gTouchGpio->Config (gTouchGpio, PEN_RESET_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  gTouchGpio->Set (gTouchGpio, PEN_RESET_GPIO, 0);
  gBS->Stall (WACOM_RESET_MS * 1000);
  gTouchGpio->Config (gTouchGpio, PEN_PDCT_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_UP);
  gTouchGpio->Set (gTouchGpio, PEN_PDCT_GPIO, 1);
  gTouchGpio->Set (gTouchGpio, PEN_RESET_GPIO, 1);
  gBS->Stall (WACOM_RESET_MS * 1000);

  Status = PenQuery ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, PenDrain, NULL, &mPenEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioOr32 (GPE0_INT_MASK, PEN_INT_BIT);
  MmioAndThenOr32 (GPE0_INT_CON, ~0x7u, GPIO_INT_EDGE_RISING);
  MmioWrite32 (GPE0_INT_PEND, PEN_INT_BIT);

  Status = gTouchInterrupt->RegisterInterruptSource (gTouchInterrupt, PEN_GIC_INTID, PenInterruptHandler);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // A packet may already be waiting (IRQ high, its edge long gone);
  // reading it unmasks the interrupt, otherwise unmask it here.
  //
  if (gTouchGpio->Get (gTouchGpio, PEN_IRQ_GPIO) != 0) {
    gBS->SignalEvent (mPenEvent);
  } else {
    MmioAnd32 (GPE0_INT_MASK, ~(UINT32)PEN_INT_BIT);
  }
  return EFI_SUCCESS;
}
