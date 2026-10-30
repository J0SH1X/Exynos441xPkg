/** @file
  Exynos4210 clock controller driver.

  Installs gExynosClockProtocolGuid.  Special clocks are mux -> ratio ->
  pre-ratio chains (dout_mmcN / sclk_mmcN in the kernel's clock-exynos4.c);
  gates are single bits in CLKGATE_IP_*.  Only MPLL and the 24 MHz crystals
  are computed as mux parents, which covers everything the board uses.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosClock.h>

#define CMU_BASE              0x10030000
#define CLKSRC_FSYS           (CMU_BASE + 0xC240)
#define CLKSRC_MASK_FSYS      (CMU_BASE + 0xC340)
#define CLKDIV_FSYS2          (CMU_BASE + 0xC548)
#define CLKDIV_FSYS3          (CMU_BASE + 0xC54C)
#define CLKDIV_STAT_FSYS2     (CMU_BASE + 0xC648)
#define CLKDIV_STAT_FSYS3     (CMU_BASE + 0xC64C)
#define CLKGATE_IP_FSYS       (CMU_BASE + 0xC940)
#define CLKGATE_IP_PERIL      (CMU_BASE + 0xC950)

//
// Exynos4210 MPLL_CON0 lives in CMU_DMC (4x12 moved it to 0x10108).
//
#define MPLL_CON0             (CMU_BASE + 0x14108)

#define XTAL_HZ               24000000U

//
// clkset_group mux values (exynos4_clkset_group_list).
//
#define MUX_XXTI              0
#define MUX_XUSBXTI           1
#define MUX_MPLL              6

typedef struct {
  //
  // Special clock: Src/Div/Stat == 0 for gate-only entries.
  //
  UINT32  SrcReg;
  UINT8   SrcShift;
  UINT32  MaskReg;
  UINT8   MaskBit;
  UINT32  DivReg;
  UINT8   RatioShift;     // 4-bit ratio (dout_mmcN)
  UINT8   PreShift;       // 8-bit pre-ratio (sclk_mmcN)
  UINT32  StatReg;

  //
  // Gate.
  //
  UINT32  GateReg;
  UINT8   GateBit;
} EXYNOS_CLOCK_DESC;

STATIC CONST EXYNOS_CLOCK_DESC  mClocks[ExynosClkMax] = {
  [ExynosClkSclkMmc2]   = { CLKSRC_FSYS, 8,  CLKSRC_MASK_FSYS, 8,
                            CLKDIV_FSYS2, 0, 8, CLKDIV_STAT_FSYS2, 0, 0 },
  [ExynosClkSclkDwmci]  = { CLKSRC_FSYS, 16, CLKSRC_MASK_FSYS, 16,
                            CLKDIV_FSYS3, 0, 8, CLKDIV_STAT_FSYS3, 0, 0 },
  [ExynosClkGateSdmmc2] = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_FSYS, 7 },
  [ExynosClkGateDwmci]  = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_FSYS, 9 },
  [ExynosClkGateUsbOtg] = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_FSYS, 13 },
  [ExynosClkGateI2c0]   = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_PERIL, 6 },
  [ExynosClkGateI2c1]   = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_PERIL, 7 },
  [ExynosClkGateI2c2]   = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_PERIL, 8 },
  [ExynosClkGateI2c3]   = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_PERIL, 9 },
  [ExynosClkGateI2c4]   = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_PERIL, 10 },
  [ExynosClkGateI2c5]   = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_PERIL, 11 },
  [ExynosClkGateI2c6]   = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_PERIL, 12 },
  [ExynosClkGateI2c7]   = { 0, 0, 0, 0, 0, 0, 0, 0, CLKGATE_IP_PERIL, 13 },
};

/**
  MPLL output.  Exynos4210 MPLL is a PLL4508: Fout = Fin * M / (P << (S - 1))
  (kernel s5p_get_pll45xx, pll_4508).
**/
STATIC
UINT32
MpllRate (
  VOID
  )
{
  UINT32  Con;
  UINT32  M;
  UINT32  P;
  UINT32  S;

  Con = MmioRead32 (MPLL_CON0);
  M   = (Con >> 16) & 0x3FF;
  P   = (Con >> 8) & 0x3F;
  S   = Con & 0x7;
  if (P == 0) {
    return 0;
  }

  return (UINT32)(((UINT64)XTAL_HZ * M) / (P << (S ? S - 1 : 0)));
}

STATIC
UINT32
MuxParentRate (
  IN UINT32  Mux
  )
{
  switch (Mux) {
    case MUX_XXTI:
    case MUX_XUSBXTI:
      return XTAL_HZ;
    case MUX_MPLL:
      return MpllRate ();
    default:
      return 0;   // EPLL/VPLL/HDMI/USBPHY parents are not used here
  }
}

STATIC
EFI_STATUS
EFIAPI
ClockEnable (
  IN EXYNOS_CLOCK_PROTOCOL  *This,
  IN EXYNOS_CLOCK_ID        Id
  )
{
  CONST EXYNOS_CLOCK_DESC  *Clk;

  if (Id >= ExynosClkMax) {
    return EFI_INVALID_PARAMETER;
  }

  Clk = &mClocks[Id];
  if (Clk->GateReg != 0) {
    MmioOr32 (Clk->GateReg, 1U << Clk->GateBit);
  }
  if (Clk->MaskReg != 0) {
    MmioOr32 (Clk->MaskReg, 1U << Clk->MaskBit);
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ClockDisable (
  IN EXYNOS_CLOCK_PROTOCOL  *This,
  IN EXYNOS_CLOCK_ID        Id
  )
{
  CONST EXYNOS_CLOCK_DESC  *Clk;

  if (Id >= ExynosClkMax) {
    return EFI_INVALID_PARAMETER;
  }

  Clk = &mClocks[Id];
  if (Clk->GateReg != 0) {
    MmioAnd32 (Clk->GateReg, ~(1U << Clk->GateBit));
  }
  if (Clk->MaskReg != 0) {
    MmioAnd32 (Clk->MaskReg, ~(1U << Clk->MaskBit));
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ClockGetRate (
  IN  EXYNOS_CLOCK_PROTOCOL  *This,
  IN  EXYNOS_CLOCK_ID        Id,
  OUT UINT32                 *RateHz
  )
{
  CONST EXYNOS_CLOCK_DESC  *Clk;
  UINT32                   Parent;
  UINT32                   Div;

  if ((Id >= ExynosClkMax) || (RateHz == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Clk = &mClocks[Id];
  if (Clk->SrcReg == 0) {
    return EFI_UNSUPPORTED;
  }

  Parent = MuxParentRate ((MmioRead32 (Clk->SrcReg) >> Clk->SrcShift) & 0xF);
  if (Parent == 0) {
    return EFI_UNSUPPORTED;
  }

  Div     = MmioRead32 (Clk->DivReg);
  *RateHz = Parent / (((Div >> Clk->RatioShift) & 0xF) + 1)
                   / (((Div >> Clk->PreShift) & 0xFF) + 1);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ClockSetRate (
  IN  EXYNOS_CLOCK_PROTOCOL  *This,
  IN  EXYNOS_CLOCK_ID        Id,
  IN  UINT32                 RateHz,
  OUT UINT32                 *ActualHz OPTIONAL
  )
{
  CONST EXYNOS_CLOCK_DESC  *Clk;
  UINT32                   Mpll;
  UINT32                   Total;
  UINT32                   Ratio;
  UINT32                   Pre;
  UINTN                    Retry;

  if ((Id >= ExynosClkMax) || (RateHz == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Clk = &mClocks[Id];
  if (Clk->SrcReg == 0) {
    return EFI_UNSUPPORTED;
  }

  Mpll = MpllRate ();
  if (Mpll == 0) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Smallest total divider giving <= RateHz, split into ratio (1..16) and
  // pre-ratio (1..256).
  //
  Total = (Mpll + RateHz - 1) / RateHz;
  Ratio = MIN (Total, 16U);
  Pre   = (Total + Ratio - 1) / Ratio;
  if (Pre > 256) {
    return EFI_INVALID_PARAMETER;
  }

  MmioAndThenOr32 (Clk->SrcReg, ~(0xFU << Clk->SrcShift), MUX_MPLL << Clk->SrcShift);
  MmioOr32 (Clk->MaskReg, 1U << Clk->MaskBit);
  MmioAndThenOr32 (Clk->DivReg,
                   ~((0xFU << Clk->RatioShift) | (0xFFU << Clk->PreShift)),
                   ((Ratio - 1) << Clk->RatioShift) | ((Pre - 1) << Clk->PreShift));

  //
  // Wait for both dividers to settle (STAT bit per divider field).
  //
  for (Retry = 0; Retry < 1000; Retry++) {
    if (!(MmioRead32 (Clk->StatReg) &
          ((1U << Clk->RatioShift) | (1U << Clk->PreShift)))) {
      break;
    }
    gBS->Stall (100);
  }

  if (ActualHz != NULL) {
    *ActualHz = Mpll / (Ratio * Pre);
  }
  return EFI_SUCCESS;
}

STATIC EXYNOS_CLOCK_PROTOCOL  mExynosClock = {
  ClockEnable,
  ClockDisable,
  ClockGetRate,
  ClockSetRate,
};

EFI_STATUS
EFIAPI
ClockDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->InstallMultipleProtocolInterfaces (
                &ImageHandle,
                &gExynosClockProtocolGuid,
                &mExynosClock,
                NULL
                );
}
