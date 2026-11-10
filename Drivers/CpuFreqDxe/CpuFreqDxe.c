/** @file
  Raise the Cortex-A9 clock (both cores share it) to 1.2 GHz early in DXE,
  for UEFI and everything booted from it (the OS keeps the setting).  The
  bootloader leaves 500 or 800 MHz depending on how the phone was started.

  Linux's cpufreq-4210.c tables for the Exynos4210, 1200 MHz: APLL M/P/S
  150/3/1, CLK_DIV_CPU0 core 0 m0 3 m1 7 periph 3 atb 4 dbg 1 apll 7,
  CLK_DIV_CPU1 copy 5, VDD_ARM up to 1.350 V (the highest ASV group, and
  Samsung's BUCK1 cap on this board).  1.4 GHz would need this chip's ASV
  group (worst case 1.400 V).

  Raising the frequency follows exynos4210_set_frequency(): voltage first,
  then the dividers, then the APLL - just S if only S changes, otherwise
  set_apll(): core on MPLL while the APLL relocks, then back.

  VDD_ARM is MAX8997 BUCK1 (I2C5, 0x66).  Which of its eight DVS registers
  is live depends on the BUCK1_EN_A/B and BUCK2_EN GPIOs, so all eight get
  the new level.  If the PMIC cannot be programmed and read back, the clock
  stays where it is.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>

//
// CMU_CPU (regs-clock.h, CMU base 0x10030000 + 0x14000).
//
#define APLL_LOCK          0x10044000
#define APLL_CON0          0x10044100
#define CLK_SRC_CPU        0x10044200
#define CLK_MUX_STAT_CPU   0x10044400
#define CLK_DIV_CPU0       0x10044500
#define CLK_DIV_CPU1       0x10044504
#define CLK_DIV_STAT_CPU0  0x10044600
#define CLK_DIV_STAT_CPU1  0x10044604

#define APLL_PMS_MASK      ((0x3FFu << 16) | (0x3Fu << 8) | 0x7u)
#define APLL_M(c)          (((c) >> 16) & 0x3FF)
#define APLL_P(c)          (((c) >> 8) & 0x3F)
#define APLL_S(c)          ((c) & 0x7)
#define APLL_LOCKED        (1u << 29)
#define APLL_LOCKTIME      0x1C20           // 300 us
#define MUX_CORE_SEL       (1u << 16)       // 0: MOUTAPLL, 1: SCLKMPLL
#define MUX_CORE_STAT(s)   (((s) >> 16) & 0x7)
#define MUX_STAT_APLL      1
#define MUX_STAT_MPLL      2
#define XTAL_HZ            24000000u

//
// 1200 MHz.
//
#define TARGET_APLL_PMS    ((150u << 16) | (3u << 8) | 1u)
#define TARGET_DIV_CPU0    ((0u << 0) | (3u << 4) | (7u << 8) | (3u << 12) | (4u << 16) | \
                            (1u << 20) | (7u << 24))
#define TARGET_DIV_COPY    5u
#define TARGET_VDD_ARM_MV  1350u

//
// MAX8997 (max8997-private.h): BUCK1 DVS levels, 650 mV + 25 mV steps.
//
#define MAX8997_I2C_BUS        5
#define MAX8997_I2C_ADDR       0x66
#define MAX8997_REG_BUCK1DVS1  0x19
#define MAX8997_BUCK1_DVS_REGS 8
#define BUCK1_CODE(mV)         (((mV) - 650) / 25)
#define BUCK1_MV(code)         (650 + ((code) & 0x3F) * 25)

//
// I2C5 pins: GPB(6)/GPB(7), function 3 (s3c_i2c5_cfg_gpio, 4210).
//
#define I2C5_FUNC  3

STATIC
UINT32
ArmClockHz (
  VOID
  )
{
  UINT32  Con;
  UINT32  Div;

  if ((MmioRead32 (CLK_SRC_CPU) & (1u << 16)) != 0) {
    return 0;     // core on MPLL: not what the bootloader leaves
  }
  Con = MmioRead32 (APLL_CON0);
  Div = MmioRead32 (CLK_DIV_CPU0);
  if (APLL_P (Con) == 0) {
    return 0;
  }
  return (UINT32)(((UINT64)XTAL_HZ * APLL_M (Con)) / (APLL_P (Con) << (APLL_S (Con) ? APLL_S (Con) - 1 : 0)))
         / ((Div & 0x7) + 1) / (((Div >> 28) & 0x7) + 1);
}

STATIC
EFI_STATUS
SetVddArm (
  IN EXYNOS_I2C_PROTOCOL  *I2c,
  IN UINT32               MilliVolts
  )
{
  UINT8  Buf[2];
  UINT8  Level[MAX8997_BUCK1_DVS_REGS];
  UINTN  Index;

  if (I2c->Read (I2c, MAX8997_I2C_BUS, MAX8997_I2C_ADDR, MAX8997_REG_BUCK1DVS1,
                 Level, sizeof (Level)) != sizeof (Level)) {
    return EFI_DEVICE_ERROR;
  }
  DEBUG ((DEBUG_INFO, "CpuFreq: VDD_ARM DVS levels %u %u %u %u %u %u %u %u mV\n",
          BUCK1_MV (Level[0]), BUCK1_MV (Level[1]), BUCK1_MV (Level[2]), BUCK1_MV (Level[3]),
          BUCK1_MV (Level[4]), BUCK1_MV (Level[5]), BUCK1_MV (Level[6]), BUCK1_MV (Level[7])));

  for (Index = 0; Index < MAX8997_BUCK1_DVS_REGS; Index++) {
    Buf[0] = (UINT8)(MAX8997_REG_BUCK1DVS1 + Index);
    Buf[1] = (UINT8)BUCK1_CODE (MilliVolts);
    if (I2c->Write (I2c, MAX8997_I2C_BUS, MAX8997_I2C_ADDR, Buf, 2) != 2) {
      return EFI_DEVICE_ERROR;
    }
  }

  //
  // The buck ramps at 10 mV/us (buck_ramp_delay); a few hundred mV take
  // tens of microseconds.
  //
  gBS->Stall (1000);

  if (I2c->Read (I2c, MAX8997_I2C_BUS, MAX8997_I2C_ADDR, MAX8997_REG_BUCK1DVS1,
                 Level, sizeof (Level)) != sizeof (Level)) {
    return EFI_DEVICE_ERROR;
  }
  for (Index = 0; Index < MAX8997_BUCK1_DVS_REGS; Index++) {
    if ((Level[Index] & 0x3F) != BUCK1_CODE (MilliVolts)) {
      return EFI_DEVICE_ERROR;
    }
  }
  return EFI_SUCCESS;
}

/**
  set_clkdiv(): CPU0 dividers, then CPU1, each waiting for its STAT bits.
**/
STATIC
VOID
SetCpuDividers (
  VOID
  )
{
  UINTN  Retry;

  MmioWrite32 (CLK_DIV_CPU0, TARGET_DIV_CPU0);
  for (Retry = 0; (Retry < 100000) && ((MmioRead32 (CLK_DIV_STAT_CPU0) & 0x1111111) != 0); Retry++) {
  }

  MmioAndThenOr32 (CLK_DIV_CPU1, ~0x77u, TARGET_DIV_COPY);
  for (Retry = 0; (Retry < 100000) && ((MmioRead32 (CLK_DIV_STAT_CPU1) & 0x11) != 0); Retry++) {
  }
}

/**
  set_apll(): run the core from MPLL while the APLL relocks to new M/P/S.
**/
STATIC
EFI_STATUS
SetApll (
  IN UINT32  Pms
  )
{
  UINTN  Retry;

  MmioOr32 (CLK_SRC_CPU, MUX_CORE_SEL);
  for (Retry = 0; MUX_CORE_STAT (MmioRead32 (CLK_MUX_STAT_CPU)) != MUX_STAT_MPLL; Retry++) {
    if (Retry > 100000) {
      return EFI_TIMEOUT;
    }
  }

  MmioWrite32 (APLL_LOCK, APLL_LOCKTIME);
  MmioAndThenOr32 (APLL_CON0, ~APLL_PMS_MASK, Pms);
  for (Retry = 0; (MmioRead32 (APLL_CON0) & APLL_LOCKED) == 0; Retry++) {
    if (Retry > 1000) {
      // Stay on MPLL (800 MHz) rather than switch to an unlocked PLL.
      return EFI_TIMEOUT;
    }
    gBS->Stall (10);
  }

  MmioAnd32 (CLK_SRC_CPU, ~MUX_CORE_SEL);
  for (Retry = 0; MUX_CORE_STAT (MmioRead32 (CLK_MUX_STAT_CPU)) != MUX_STAT_APLL; Retry++) {
    if (Retry > 100000) {
      return EFI_TIMEOUT;
    }
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
CpuFreqDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EXYNOS_I2C_PROTOCOL   *I2c;
  EXYNOS_GPIO_PROTOCOL  *Gpio;
  UINT32                Con;
  EFI_STATUS            Status;

  Con = MmioRead32 (APLL_CON0);
  DEBUG ((DEBUG_INFO, "CpuFreq: ARMCLK %u MHz (APLL %u/%u/%u, DIV_CPU0 0x%08x)\n",
          ArmClockHz () / 1000000, APLL_M (Con), APLL_P (Con), APLL_S (Con),
          MmioRead32 (CLK_DIV_CPU0)));

  if ((Con & APLL_PMS_MASK) == TARGET_APLL_PMS) {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&I2c);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB, 6), I2C5_FUNC, EXYNOS_GPIO_PULL_UP);
  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB, 7), I2C5_FUNC, EXYNOS_GPIO_PULL_UP);

  Status = SetVddArm (I2c, TARGET_VDD_ARM_MV);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CpuFreq: VDD_ARM to %u mV failed - %r, clock left alone\n",
            TARGET_VDD_ARM_MV, Status));
    return Status;
  }

  //
  // Raising the frequency: dividers first, then the APLL (just S when M and
  // P already match; a relock otherwise, as from the bootloader's 500/800).
  //
  SetCpuDividers ();
  if ((Con & ~0x7u & APLL_PMS_MASK) == (TARGET_APLL_PMS & ~0x7u)) {
    MmioAndThenOr32 (APLL_CON0, ~0x7u, APLL_S (TARGET_APLL_PMS));
  } else {
    Status = SetApll (TARGET_APLL_PMS);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "CpuFreq: APLL relock failed - %r (CON0 0x%08x, MUX_STAT 0x%x)\n",
              Status, MmioRead32 (APLL_CON0), MmioRead32 (CLK_MUX_STAT_CPU)));
      return Status;
    }
  }

  DEBUG ((DEBUG_INFO, "CpuFreq: ARMCLK %u MHz at %u mV\n", ArmClockHz () / 1000000,
          TARGET_VDD_ARM_MV));
  return EFI_SUCCESS;
}
