/** @file
  Exynos4 GPIO DXE driver.

  Installs gExynosGpioProtocolGuid over the Exynos4 GPIO register banks.
  Register layout follows the kernel's s3c_gpio_bank (drivers/gpio/gpio-
  exynos4.c): every bank is a CON/DAT/PULL triplet (0x00/0x04/0x08) spaced
  0x20 bytes apart inside one of the three GPIO controllers.

  CON is 4 bits per pin (s3c_gpio_setcfg_s3c64xx_4bit): 0 = input,
  1 = output, >= 2 = alternate function.  PULL is 2 bits per pin with the
  Exynos4 encoding (pull-up = 3, pull-down = 1, disabled = 0).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosGpio.h>

//
// The three GPIO controllers (map-exynos4.h).
//
#define EXYNOS4_PA_GPIO1  0x11400000
#define EXYNOS4_PA_GPIO2  0x11000000
#define EXYNOS4_PA_GPIO3  0x03860000

#define GPIO_CON_OFFSET    0x00
#define GPIO_DAT_OFFSET    0x04
#define GPIO_PULL_OFFSET   0x08

//
// One entry per ExynosGpioBank, in enum order.  Base is the full MMIO
// address of the bank, Ngpio its pin count (kernel exynos4_gpio_common_
// 4bit + exynos4210_gpio_4bit tables).
//
typedef struct {
  UINT32  Base;
  UINT8   Ngpio;
} EXYNOS_GPIO_BANK_DESC;

STATIC CONST EXYNOS_GPIO_BANK_DESC mGpioBanks[EXYNOS_GPIO_BANK_MAX] = {
  [EXYNOS_GPIO_BANK_GPA0] = { EXYNOS4_PA_GPIO1 + 0x000, 8 },
  [EXYNOS_GPIO_BANK_GPA1] = { EXYNOS4_PA_GPIO1 + 0x020, 6 },
  [EXYNOS_GPIO_BANK_GPB]  = { EXYNOS4_PA_GPIO1 + 0x040, 8 },
  [EXYNOS_GPIO_BANK_GPC0] = { EXYNOS4_PA_GPIO1 + 0x060, 5 },
  [EXYNOS_GPIO_BANK_GPC1] = { EXYNOS4_PA_GPIO1 + 0x080, 5 },
  [EXYNOS_GPIO_BANK_GPD0] = { EXYNOS4_PA_GPIO1 + 0x0A0, 4 },
  [EXYNOS_GPIO_BANK_GPD1] = { EXYNOS4_PA_GPIO1 + 0x0C0, 4 },
  [EXYNOS_GPIO_BANK_GPE0] = { EXYNOS4_PA_GPIO1 + 0x0E0, 5 },
  [EXYNOS_GPIO_BANK_GPE1] = { EXYNOS4_PA_GPIO1 + 0x100, 8 },
  [EXYNOS_GPIO_BANK_GPE2] = { EXYNOS4_PA_GPIO1 + 0x120, 6 },
  [EXYNOS_GPIO_BANK_GPE3] = { EXYNOS4_PA_GPIO1 + 0x140, 8 },
  [EXYNOS_GPIO_BANK_GPE4] = { EXYNOS4_PA_GPIO1 + 0x160, 8 },
  [EXYNOS_GPIO_BANK_GPF0] = { EXYNOS4_PA_GPIO1 + 0x180, 8 },
  [EXYNOS_GPIO_BANK_GPF1] = { EXYNOS4_PA_GPIO1 + 0x1A0, 8 },
  [EXYNOS_GPIO_BANK_GPF2] = { EXYNOS4_PA_GPIO1 + 0x1C0, 8 },
  [EXYNOS_GPIO_BANK_GPF3] = { EXYNOS4_PA_GPIO1 + 0x1E0, 6 },
  [EXYNOS_GPIO_BANK_GPJ0] = { EXYNOS4_PA_GPIO2 + 0x000, 8 },
  [EXYNOS_GPIO_BANK_GPJ1] = { EXYNOS4_PA_GPIO2 + 0x020, 5 },
  [EXYNOS_GPIO_BANK_GPK0] = { EXYNOS4_PA_GPIO2 + 0x040, 7 },
  [EXYNOS_GPIO_BANK_GPK1] = { EXYNOS4_PA_GPIO2 + 0x060, 7 },
  [EXYNOS_GPIO_BANK_GPK2] = { EXYNOS4_PA_GPIO2 + 0x080, 7 },
  [EXYNOS_GPIO_BANK_GPK3] = { EXYNOS4_PA_GPIO2 + 0x0A0, 7 },
  [EXYNOS_GPIO_BANK_GPL0] = { EXYNOS4_PA_GPIO2 + 0x0C0, 8 },
  [EXYNOS_GPIO_BANK_GPL1] = { EXYNOS4_PA_GPIO2 + 0x0E0, 3 },
  [EXYNOS_GPIO_BANK_GPL2] = { EXYNOS4_PA_GPIO2 + 0x100, 8 },
  [EXYNOS_GPIO_BANK_GPY0] = { EXYNOS4_PA_GPIO2 + 0x120, 6 },
  [EXYNOS_GPIO_BANK_GPY1] = { EXYNOS4_PA_GPIO2 + 0x140, 4 },
  [EXYNOS_GPIO_BANK_GPY2] = { EXYNOS4_PA_GPIO2 + 0x160, 6 },
  [EXYNOS_GPIO_BANK_GPY3] = { EXYNOS4_PA_GPIO2 + 0x180, 8 },
  [EXYNOS_GPIO_BANK_GPY4] = { EXYNOS4_PA_GPIO2 + 0x1A0, 8 },
  [EXYNOS_GPIO_BANK_GPY5] = { EXYNOS4_PA_GPIO2 + 0x1C0, 8 },
  [EXYNOS_GPIO_BANK_GPY6] = { EXYNOS4_PA_GPIO2 + 0x1E0, 8 },
  [EXYNOS_GPIO_BANK_GPX0] = { EXYNOS4_PA_GPIO2 + 0xC00, 8 },
  [EXYNOS_GPIO_BANK_GPX1] = { EXYNOS4_PA_GPIO2 + 0xC20, 8 },
  [EXYNOS_GPIO_BANK_GPX2] = { EXYNOS4_PA_GPIO2 + 0xC40, 8 },
  [EXYNOS_GPIO_BANK_GPX3] = { EXYNOS4_PA_GPIO2 + 0xC60, 8 },
  [EXYNOS_GPIO_BANK_GPZ]  = { EXYNOS4_PA_GPIO3 + 0x000, 7 },
};

/**
  Look a pin up in the bank table.

  @param[in]  Pin   Encoded pin.
  @param[out] Base  MMIO base of the bank, on success.
  @param[out] Shift CON/PULL field shift for the pin, on success.

  @retval TRUE  Pin is valid; Base/Shift describe it.
  @retval FALSE Pin out of range.
**/
STATIC
BOOLEAN
LookupPin (
  IN  UINTN   Pin,
  OUT UINT32  *Base,
  OUT UINTN   *Shift
  )
{
  UINTN                      Bank;
  UINTN                      Offset;
  CONST EXYNOS_GPIO_BANK_DESC *Desc;

  Bank   = EXYNOS_GPIO_BANK (Pin);
  Offset = EXYNOS_GPIO_OFFSET (Pin);

  if (Bank >= EXYNOS_GPIO_BANK_MAX) {
    return FALSE;
  }

  Desc = &mGpioBanks[Bank];
  if (Offset >= Desc->Ngpio) {
    return FALSE;
  }

  *Base  = Desc->Base;
  *Shift = Offset;
  return TRUE;
}

STATIC
UINTN
EFIAPI
GpioGet (
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin
  )
{
  UINT32  Base;
  UINTN   Shift;

  if (!LookupPin (Pin, &Base, &Shift)) {
    DEBUG ((DEBUG_ERROR, "%a: bad pin 0x%x\n", __FUNCTION__, (UINT32)Pin));
    return 0;
  }

  return (MmioRead32 (Base + GPIO_DAT_OFFSET) >> Shift) & 1;
}

STATIC
EFI_STATUS
EFIAPI
GpioSet (
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin,
  IN UINTN                 Value
  )
{
  UINT32  Base;
  UINTN   Shift;
  UINT32  Dat;

  if (!LookupPin (Pin, &Base, &Shift)) {
    return EFI_INVALID_PARAMETER;
  }

  Dat = MmioRead32 (Base + GPIO_DAT_OFFSET);
  if (Value) {
    Dat |= (1U << Shift);
  } else {
    Dat &= ~(1U << Shift);
  }
  MmioWrite32 (Base + GPIO_DAT_OFFSET, Dat);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GpioConfig (
  IN EXYNOS_GPIO_PROTOCOL  *This,
  IN UINTN                 Pin,
  IN UINTN                 Function,
  IN UINTN                 Pull
  )
{
  UINT32  Base;
  UINTN   Shift;
  UINT32  Reg;
  UINTN   ConShift;

  if (!LookupPin (Pin, &Base, &Shift)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // CON: 4 bits per pin.
  //
  ConShift = Shift * 4;
  Reg      = MmioRead32 (Base + GPIO_CON_OFFSET);
  Reg     &= ~(0xFU << ConShift);
  Reg     |= ((UINT32)(Function & 0xF) << ConShift);
  MmioWrite32 (Base + GPIO_CON_OFFSET, Reg);

  //
  // PULL: 2 bits per pin, Exynos4 encoding (pull-up = 3).
  //
  Reg = MmioRead32 (Base + GPIO_PULL_OFFSET);
  Reg &= ~(0x3U << (Shift * 2));
  Reg |= ((UINT32)(Pull & 0x3) << (Shift * 2));
  MmioWrite32 (Base + GPIO_PULL_OFFSET, Reg);

  return EFI_SUCCESS;
}

STATIC EXYNOS_GPIO_PROTOCOL mExynosGpio = {
  GpioGet,
  GpioSet,
  GpioConfig,
};

EFI_STATUS
EFIAPI
GpioDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gExynosGpioProtocolGuid,
                  &mExynosGpio,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_INFO, "%a: Exynos GPIO protocol installed\n", __FUNCTION__));
  return Status;
}
