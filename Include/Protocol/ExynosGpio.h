/** @file
  Exynos4 GPIO controller protocol.

  Thin wrapper over the Exynos4 GPIO banks (CON/DAT/PULL registers), shaped
  like HtcLeoPkg's TLMM_GPIO protocol: Get reads a pin, Set drives an output
  pin, Config selects input/output/special-function and the pull resistor.

  Pins are encoded as EXYNOS_GPIO(Bank, Pin) with Bank taken from the
  ExynosGpioBank enum below.  The bank order and register offsets match the
  kernel's s3c_gpio_chip tables in drivers/gpio/gpio-exynos4.c for the
  Exynos4210 (GT-N7000).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_GPIO_H__
#define __EXYNOS_GPIO_H__

#define EXYNOS_GPIO_PROTOCOL_GUID                                              \
  {                                                                            \
    0x9d3f4a71, 0x5c20, 0x4b8e,                                                \
    {                                                                          \
      0xa1, 0x77, 0x6e, 0x24, 0x41, 0x10, 0x44, 0x01                           \
    }                                                                          \
  }

//
// Pin encoding: (Bank << 8) | PinInBank.  Bank indexes ExynosGpioBank below.
//
typedef UINTN EXYNOS_GPIO_PIN;
#define EXYNOS_GPIO(Bank, Pin)  (((Bank) << 8) | (Pin))
#define EXYNOS_GPIO_BANK(Pin)   (((Pin) >> 8) & 0xFF)
#define EXYNOS_GPIO_OFFSET(Pin) ((Pin) & 0xFF)

//
// GPIO controller banks, in the order of the s3c_gpio_chip tables in the
// kernel's gpio-exynos4.c (Exynos4210).  Each bank is one CON/DAT/PULL
// register triplet, 0x20 bytes apart.
//
typedef enum {
  EXYNOS_GPIO_BANK_GPA0 = 0,   // 0x11400000 + 0x000, 8 pins
  EXYNOS_GPIO_BANK_GPA1,       // 0x11400000 + 0x020, 6 pins
  EXYNOS_GPIO_BANK_GPB,        // 0x11400000 + 0x040, 8 pins
  EXYNOS_GPIO_BANK_GPC0,       // 0x11400000 + 0x060, 5 pins
  EXYNOS_GPIO_BANK_GPC1,       // 0x11400000 + 0x080, 5 pins
  EXYNOS_GPIO_BANK_GPD0,       // 0x11400000 + 0x0A0, 4 pins
  EXYNOS_GPIO_BANK_GPD1,       // 0x11400000 + 0x0C0, 4 pins
  EXYNOS_GPIO_BANK_GPE0,       // 0x11400000 + 0x0E0, 5 pins (4210 only)
  EXYNOS_GPIO_BANK_GPE1,       // 0x11400000 + 0x100, 8 pins
  EXYNOS_GPIO_BANK_GPE2,       // 0x11400000 + 0x120, 6 pins
  EXYNOS_GPIO_BANK_GPE3,       // 0x11400000 + 0x140, 8 pins
  EXYNOS_GPIO_BANK_GPE4,       // 0x11400000 + 0x160, 8 pins
  EXYNOS_GPIO_BANK_GPF0,       // 0x11400000 + 0x180, 8 pins
  EXYNOS_GPIO_BANK_GPF1,       // 0x11400000 + 0x1A0, 8 pins
  EXYNOS_GPIO_BANK_GPF2,       // 0x11400000 + 0x1C0, 8 pins
  EXYNOS_GPIO_BANK_GPF3,       // 0x11400000 + 0x1E0, 6 pins
  EXYNOS_GPIO_BANK_GPJ0,       // 0x11000000 + 0x000, 8 pins
  EXYNOS_GPIO_BANK_GPJ1,       // 0x11000000 + 0x020, 5 pins
  EXYNOS_GPIO_BANK_GPK0,       // 0x11000000 + 0x040, 7 pins
  EXYNOS_GPIO_BANK_GPK1,       // 0x11000000 + 0x060, 7 pins
  EXYNOS_GPIO_BANK_GPK2,       // 0x11000000 + 0x080, 7 pins
  EXYNOS_GPIO_BANK_GPK3,       // 0x11000000 + 0x0A0, 7 pins
  EXYNOS_GPIO_BANK_GPL0,       // 0x11000000 + 0x0C0, 8 pins
  EXYNOS_GPIO_BANK_GPL1,       // 0x11000000 + 0x0E0, 3 pins
  EXYNOS_GPIO_BANK_GPL2,       // 0x11000000 + 0x100, 8 pins
  EXYNOS_GPIO_BANK_GPY0,       // 0x11000000 + 0x120, 6 pins
  EXYNOS_GPIO_BANK_GPY1,       // 0x11000000 + 0x140, 4 pins
  EXYNOS_GPIO_BANK_GPY2,       // 0x11000000 + 0x160, 6 pins
  EXYNOS_GPIO_BANK_GPY3,       // 0x11000000 + 0x180, 8 pins
  EXYNOS_GPIO_BANK_GPY4,       // 0x11000000 + 0x1A0, 8 pins
  EXYNOS_GPIO_BANK_GPY5,       // 0x11000000 + 0x1C0, 8 pins
  EXYNOS_GPIO_BANK_GPY6,       // 0x11000000 + 0x1E0, 8 pins
  EXYNOS_GPIO_BANK_GPX0,       // 0x11000000 + 0xC00, 8 pins (EINT)
  EXYNOS_GPIO_BANK_GPX1,       // 0x11000000 + 0xC20, 8 pins (EINT)
  EXYNOS_GPIO_BANK_GPX2,       // 0x11000000 + 0xC40, 8 pins (EINT)
  EXYNOS_GPIO_BANK_GPX3,       // 0x11000000 + 0xC60, 8 pins (EINT)
  EXYNOS_GPIO_BANK_GPZ,        // 0x03860000 + 0x000, 7 pins

  EXYNOS_GPIO_BANK_MAX
} ExynosGpioBank;

//
// Config() Function values: the raw 4-bit CON field encoding from the
// Exynos4 manual (0 = input, 1 = output, anything else is the SoC's
// alternate-function select for that pin).
//
#define EXYNOS_GPIO_MODE_INPUT    0
#define EXYNOS_GPIO_MODE_OUTPUT   1

//
// Config() Pull values: the 2-bit PULL field encoding of s3c_gpio_setpull_
// exynos4() in the kernel (pull-up is 3, not 2 like on earlier Samsung SoCs).
//
#define EXYNOS_GPIO_PULL_NONE  0
#define EXYNOS_GPIO_PULL_DOWN  1
#define EXYNOS_GPIO_PULL_UP    3

typedef struct _EXYNOS_GPIO EXYNOS_GPIO_PROTOCOL;

/**
  Read the current level of a pin.

  @param[in] Pin   EXYNOS_GPIO(Bank, PinInBank).

  @return 0 or 1.
**/
typedef
UINTN
(EFIAPI *EXYNOS_GPIO_GET)(
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin
  );

/**
  Drive a pin configured as output.

  @param[in] Pin   EXYNOS_GPIO(Bank, PinInBank).
  @param[in] Value 0 or 1.

  @retval EFI_SUCCESS           Pin driven.
  @retval EFI_INVALID_PARAMETER Pin out of range.
**/
typedef
EFI_STATUS
(EFIAPI *EXYNOS_GPIO_SET)(
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin,
  IN UINTN                 Value
  );

/**
  Configure the function (input/output/alternate) and pull resistor of a pin.

  @param[in] Pin      EXYNOS_GPIO(Bank, PinInBank).
  @param[in] Function EXYNOS_GPIO_MODE_INPUT / EXYNOS_GPIO_MODE_OUTPUT, or a
                      raw alternate-function value.
  @param[in] Pull     EXYNOS_GPIO_PULL_NONE / _DOWN / _UP.

  @retval EFI_SUCCESS           Pin configured.
  @retval EFI_INVALID_PARAMETER Pin out of range.
**/
typedef
EFI_STATUS
(EFIAPI *EXYNOS_GPIO_CONFIG)(
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin,
  IN UINTN                 Function,
  IN UINTN                 Pull
  );

//
// Protocol interface structure.
//
struct _EXYNOS_GPIO {
  EXYNOS_GPIO_GET     Get;
  EXYNOS_GPIO_SET     Set;
  EXYNOS_GPIO_CONFIG  Config;
};

extern EFI_GUID gExynosGpioProtocolGuid;

#endif
