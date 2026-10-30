/** @file
  Exynos4 clock controller (CMU) protocol.

  Owns the CMU registers so device drivers never poke them directly: gate a
  peripheral clock on/off, and get or set the rate of a special clock (mux +
  ratio + pre-ratio).  Clock ids and register layout follow the kernel's
  clock-exynos4.c for the Exynos4210 (GT-N7000).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_CLOCK_H__
#define __EXYNOS_CLOCK_H__

#define EXYNOS_CLOCK_PROTOCOL_GUID                                             \
  {                                                                            \
    0x9d3f4a71, 0x5c20, 0x4b8e,                                                \
    {                                                                          \
      0xa1, 0x77, 0x6e, 0x24, 0x41, 0x10, 0x44, 0x03                           \
    }                                                                          \
  }

typedef enum {
  //
  // Special clocks (rate control + source gate).
  //
  ExynosClkSclkMmc2 = 0,     // sclk_mmc2: HSMMC2 (microSD)
  ExynosClkSclkDwmci,        // sclk_dwmci (dout_mmc4): MSHCI (eMMC)

  //
  // Bus clock gates (CLKGATE_IP_*), enable/disable only.
  //
  ExynosClkGateSdmmc2,
  ExynosClkGateDwmci,
  ExynosClkGateUsbOtg,         // DWC2 USB device controller ("usbotg" gate)
  ExynosClkGateI2c0,           // i2c0..i2c7: consecutive ids, add the bus number
  ExynosClkGateI2c1,
  ExynosClkGateI2c2,
  ExynosClkGateI2c3,
  ExynosClkGateI2c4,
  ExynosClkGateI2c5,
  ExynosClkGateI2c6,
  ExynosClkGateI2c7,

  ExynosClkMax
} EXYNOS_CLOCK_ID;

typedef struct _EXYNOS_CLOCK_PROTOCOL EXYNOS_CLOCK_PROTOCOL;

/**
  Ungate a clock.

  @retval EFI_SUCCESS           Clock running.
  @retval EFI_INVALID_PARAMETER Unknown clock id.
**/
typedef
EFI_STATUS
(EFIAPI *EXYNOS_CLOCK_ENABLE)(
  IN EXYNOS_CLOCK_PROTOCOL  *This,
  IN EXYNOS_CLOCK_ID        Id
  );

/**
  Gate a clock.

  @retval EFI_SUCCESS           Clock stopped.
  @retval EFI_INVALID_PARAMETER Unknown clock id.
**/
typedef
EFI_STATUS
(EFIAPI *EXYNOS_CLOCK_DISABLE)(
  IN EXYNOS_CLOCK_PROTOCOL  *This,
  IN EXYNOS_CLOCK_ID        Id
  );

/**
  Current rate of a special clock.

  @param[out] RateHz  Rate in Hz.

  @retval EFI_SUCCESS           Rate returned.
  @retval EFI_UNSUPPORTED       Gate-only clock, or a mux source this driver
                                cannot compute.
  @retval EFI_INVALID_PARAMETER Unknown clock id.
**/
typedef
EFI_STATUS
(EFIAPI *EXYNOS_CLOCK_GET_RATE)(
  IN  EXYNOS_CLOCK_PROTOCOL  *This,
  IN  EXYNOS_CLOCK_ID        Id,
  OUT UINT32                 *RateHz
  );

/**
  Program a special clock from MPLL to the fastest rate not above RateHz.

  @param[in]  RateHz    Requested upper bound in Hz.
  @param[out] ActualHz  Rate actually programmed (optional).

  @retval EFI_SUCCESS           Clock reprogrammed.
  @retval EFI_UNSUPPORTED       Gate-only clock.
  @retval EFI_INVALID_PARAMETER Unknown clock id or RateHz out of range.
**/
typedef
EFI_STATUS
(EFIAPI *EXYNOS_CLOCK_SET_RATE)(
  IN  EXYNOS_CLOCK_PROTOCOL  *This,
  IN  EXYNOS_CLOCK_ID        Id,
  IN  UINT32                 RateHz,
  OUT UINT32                 *ActualHz OPTIONAL
  );

struct _EXYNOS_CLOCK_PROTOCOL {
  EXYNOS_CLOCK_ENABLE    Enable;
  EXYNOS_CLOCK_DISABLE   Disable;
  EXYNOS_CLOCK_GET_RATE  GetRate;
  EXYNOS_CLOCK_SET_RATE  SetRate;
};

extern EFI_GUID gExynosClockProtocolGuid;

#endif
