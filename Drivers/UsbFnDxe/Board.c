/** @file
  GT-N7000 board glue for the USB device port: route the micro-USB data lines
  to the AP and bring up USB PHY0.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbFnDxe.h"

//
// PMU / PHY0 (setup-usb-phy.c exynos4_usb_phy0_init, 4210 variant).  The
// controller clock gate is ClockDxe's ExynosClkGateUsbOtg.
//
#define PMU_USBOTG_PHY_CONTROL  0x10020704

#define PHY_BASE                0x125B0000
#define PHYPWR                  (PHY_BASE + 0x00)
#define PHYCLK                  (PHY_BASE + 0x04)
#define RSTCON                  (PHY_BASE + 0x08)

#define PHYPWR_PHY0_NORMAL_MASK 0x29
#define PHYCLK_CLKSEL_MASK      0x3
#define PHYCLK_CLKSEL_24M       0x3   // XusbXTI = 24 MHz (mach-u1.c)
#define PHYCLK_COMMON_ON_N      (1U << 4)
#define RSTCON_PHY0_SWRST_MASK  0x7

/**
  Point the micro-USB port at the AP: GPIO_USB_SEL low and the MAX8997 MUIC
  switches (CTRL1 COMN1SW/COMP2SW) to USB.  Both default elsewhere, and the
  host then never sees the device however correct the controller setup is.
**/
STATIC
EFI_STATUS
UsbRoutePort (
  VOID
  )
{
  EFI_STATUS            Status;
  EXYNOS_GPIO_PROTOCOL  *Gpio;
  EXYNOS_I2C_PROTOCOL   *I2c;
  UINT8                 Ctrl1;
  UINT8                 Buf[2];

  Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&I2c);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Gpio->Config (Gpio, USB_SEL_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  Gpio->Set (Gpio, USB_SEL_GPIO, 0);

  //
  // I2C5 SDA/SCL = GPB(6)/GPB(7) func 3 (s3c_i2c5_cfg_gpio, 4210).
  //
  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB, 6), 3, EXYNOS_GPIO_PULL_UP);
  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB, 7), 3, EXYNOS_GPIO_PULL_UP);

  if (I2c->Read (I2c, MUIC_I2C_BUS, MUIC_I2C_ADDR, MUIC_REG_CTRL1, &Ctrl1, 1) != 1) {
    DEBUG ((DEBUG_ERROR, "UsbFn: MUIC read failed\n"));
    return EFI_DEVICE_ERROR;
  }

  Buf[0] = MUIC_REG_CTRL1;
  Buf[1] = (UINT8)((Ctrl1 & ~MUIC_CTRL1_SW_MASK) | MUIC_CTRL1_SW_USB);
  if (I2c->Write (I2c, MUIC_I2C_BUS, MUIC_I2C_ADDR, Buf, 2) != 2) {
    DEBUG ((DEBUG_ERROR, "UsbFn: MUIC write failed\n"));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "UsbFn: MUIC CTRL1 0x%x -> 0x%x\n", Ctrl1, Buf[1]));
  return EFI_SUCCESS;
}

/**
  Clock, power and reset USB PHY0 for 24 MHz XusbXTI.
**/
STATIC
EFI_STATUS
UsbPhyInit (
  VOID
  )
{
  EFI_STATUS             Status;
  EXYNOS_CLOCK_PROTOCOL  *Clock;

  Status = gBS->LocateProtocol (&gExynosClockProtocolGuid, NULL, (VOID **)&Clock);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Clock->Enable (Clock, ExynosClkGateUsbOtg);
  MmioWrite32 (PMU_USBOTG_PHY_CONTROL, 1);

  MmioAndThenOr32 (PHYCLK, ~(PHYCLK_CLKSEL_MASK | PHYCLK_COMMON_ON_N),
                   PHYCLK_CLKSEL_24M);
  MmioAnd32 (PHYPWR, ~PHYPWR_PHY0_NORMAL_MASK);

  MmioOr32 (RSTCON, RSTCON_PHY0_SWRST_MASK);
  gBS->Stall (10);
  MmioAnd32 (RSTCON, ~RSTCON_PHY0_SWRST_MASK);
  gBS->Stall (1000);  // PHY PLL lock
  return EFI_SUCCESS;
}

EFI_STATUS
UsbBoardInit (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = UsbRoutePort ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return UsbPhyInit ();
}
