/** @file
  Standard SDHCI host backend (Exynos4 HSMMC) for microSD.

  Drives the SDHCI-standard host controller at EXYNOS4_PA_HSMMC2 in PIO
  mode.  Register offsets follow the SDHCI spec (drivers/mmc/host/sdhci.h);
  the Exynos4 extras (CONTROL2/3 base-clock select) follow
  arch/arm/plat-samsung/include/plat/regs-sdhci.h.

  The card-detect decision (whether to even bring this host up) is made by
  the driver entry point, not here.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdCardDxe.h"

//
// SDHCI register offsets (spec).
//
#define SDHCI_BLOCK_SIZE       0x04
#define SDHCI_BLOCK_COUNT      0x06
#define SDHCI_ARGUMENT         0x08
#define SDHCI_TRANSFER_MODE    0x0C
#define SDHCI_COMMAND          0x0E
#define SDHCI_RESPONSE         0x10
#define SDHCI_BUFFER_DATA_PORT 0x20
#define SDHCI_PRESENT_STATE    0x24
#define SDHCI_HOST_CONTROL     0x28
#define SDHCI_POWER_CONTROL    0x29
#define SDHCI_CLOCK_CONTROL    0x2C
#define SDHCI_TIMEOUT_CONTROL  0x2E
#define SDHCI_SOFTWARE_RESET   0x2F
#define SDHCI_INT_STATUS       0x30
#define SDHCI_INT_ENABLE       0x34
#define SDHCI_SIGNAL_ENABLE    0x38
#define SDHCI_HOST_CONTROL2    0x3E
#define SDHCI_CAPABILITIES     0x40
#define SDHCI_HOST_VERSION     0xFE

//
// PRESENT_STATE bits.
//
#define SDHCI_CMD_INHIBIT   (1U << 0)
#define SDHCI_DAT_INHIBIT   (1U << 1)
#define SDHCI_DAT_LVL_MASK  (0x0FU << 20)
#define SDHCI_DAT_LVL_SHIFT 20

//
// CLOCK_CONTROL bits.
//
#define SDHCI_CLK_INT_EN     0x0001
#define SDHCI_CLK_INT_STABLE 0x0002
#define SDHCI_CLK_CARD_EN    0x0004
#define SDHCI_DIV_MASK       0xFF00

//
// SOFTWARE_RESET bits.
//
#define SDHCI_RST_ALL  0x01
#define SDHCI_RST_CMD  0x02
#define SDHCI_RST_DATA 0x04

//
// INT_STATUS bits (only the ones this driver waits on).
//
#define SDHCI_INT_CMD_DONE   (1U << 0)
#define SDHCI_INT_XFER_DONE  (1U << 1)
#define SDHCI_INT_DMA        (1U << 3)
#define SDHCI_INT_BUF_WR      (1U << 4)
#define SDHCI_INT_BUF_RD      (1U << 5)
#define SDHCI_INT_ERR_MASK   0xFFFF0000U

//
// COMMAND register construction.
//
#define SDHCI_CMD_RESP_NONE   0x00
#define SDHCI_CMD_RESP_LONG   0x01
#define SDHCI_CMD_RESP_SHORT  0x02
#define SDHCI_CMD_RESP_BUSY   0x03
#define SDHCI_CMD_CRC         0x08
#define SDHCI_CMD_INDEX       0x10
#define SDHCI_CMD_DATA        0x20

//
// TRANSFER_MODE bits.
//
#define SDHCI_TRNS_READ    0x10
#define SDHCI_TRNS_MULTI   0x20

//
// HOST_CONTROL bits.
//
#define SDHCI_CTRL_4BITBUS  0x02
#define SDHCI_CTRL_8BITBUS  0x20

//
// Exynos4 CONTROL2 (base-clock select).
//
#define S3C_SDHCI_CONTROL2         0x80
#define S3C_CTRL2_SELBASECLK_MASK  (0x3U << 4)
#define S3C_CTRL2_SELBASECLK_SHIFT 4
#define S3C_CTRL2_SELBASECLK_SCLK  2   // sclk_mmc from the CMU (clk_type DIV_EXTERNAL)

//
// Wait for a bit in PRESENT_STATE before issuing a command/transfer.
//
STATIC
EFI_STATUS
WaitInhibitClear (
  IN MMC_HOST  *Host,
  IN UINT32    Mask
  )
{
  UINTN  Retry;

  for (Retry = 0; Retry < 2000; Retry++) {
    if ((MmioRead32 (Host->Base + SDHCI_PRESENT_STATE) & Mask) == 0) {
      return EFI_SUCCESS;
    }
    gBS->Stall (1000);
  }
  return EFI_TIMEOUT;
}

//
// Poll SDHCI_INT_STATUS for completion (command or transfer), clear it, and
// surface any error bits.
//
STATIC
EFI_STATUS
WaitInt (
  IN MMC_HOST  *Host,
  IN UINT32    DoneMask
  )
{
  UINTN  Retry;
  UINT32 Int;

  for (Retry = 0; Retry < 5000; Retry++) {
    Int = MmioRead32 (Host->Base + SDHCI_INT_STATUS);
    if (Int & SDHCI_INT_ERR_MASK) {
      MmioWrite32 (Host->Base + SDHCI_INT_STATUS, Int);
      SD_DEBUG ((DEBUG_ERROR, "Sdhci: INT error 0x%x\n", Int));
      return EFI_DEVICE_ERROR;
    }
    if (Int & DoneMask) {
      MmioWrite32 (Host->Base + SDHCI_INT_STATUS, Int & DoneMask);
      return EFI_SUCCESS;
    }
    gBS->Stall (1000);
  }
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
EFIAPI
SdhciSendCmd (
  IN MMC_HOST  *Host,
  IN UINT32    Cmd,
  IN UINT32    Arg,
  IN UINT32    Flags,
  OUT UINT32   *Resp
  )
{
  EFI_STATUS  Status;
  UINT32      CmdReg;
  UINT32      RespCtl;

  Status = WaitInhibitClear (Host, SDHCI_CMD_INHIBIT);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((Flags & HOST_CMD_DATA) &&
      EFI_ERROR (WaitInhibitClear (Host, SDHCI_DAT_INHIBIT))) {
    return EFI_TIMEOUT;
  }

  //
  // ARGUMENT, then COMMAND (write triggers execution).
  //
  MmioWrite32 (Host->Base + SDHCI_ARGUMENT, Arg);

  RespCtl = SDHCI_CMD_INDEX;
  if (!(Flags & HOST_CMD_NO_CRC)) {
    RespCtl |= SDHCI_CMD_CRC;
  }
  if (Flags & HOST_CMD_RESP_LONG) {
    RespCtl |= SDHCI_CMD_RESP_LONG;
  } else if (Flags & HOST_CMD_RESP_BUSY) {
    RespCtl |= SDHCI_CMD_RESP_BUSY;
  } else if (Flags & HOST_CMD_RESP_SHORT) {
    RespCtl |= SDHCI_CMD_RESP_SHORT;
  }

  CmdReg = (Cmd << 8) | RespCtl;
  if (Flags & HOST_CMD_DATA) {
    CmdReg |= SDHCI_CMD_DATA;
  }

  MmioWrite16 (Host->Base + SDHCI_COMMAND, (UINT16)CmdReg);

  Status = WaitInt (Host, SDHCI_INT_CMD_DONE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Resp != NULL) {
    Resp[0] = MmioRead32 (Host->Base + SDHCI_RESPONSE + 0x0);
    Resp[1] = MmioRead32 (Host->Base + SDHCI_RESPONSE + 0x4);
    Resp[2] = MmioRead32 (Host->Base + SDHCI_RESPONSE + 0x8);
    Resp[3] = MmioRead32 (Host->Base + SDHCI_RESPONSE + 0xC);

    //
    // SDHCI stores R2 without its CRC byte (bits 127:8 in RESPONSE[119:0]);
    // shift back up so Resp[3] holds bits 127:96 like the MSHCI backend.
    //
    if (Flags & HOST_CMD_RESP_LONG) {
      Resp[3] = (Resp[3] << 8) | (Resp[2] >> 24);
      Resp[2] = (Resp[2] << 8) | (Resp[1] >> 24);
      Resp[1] = (Resp[1] << 8) | (Resp[0] >> 24);
      Resp[0] = Resp[0] << 8;
    }
  }

  return EFI_SUCCESS;
}

//
// Program the internal divider + Exynos4 base-clock select, then enable the
// SD clock.  MaxClockHz is the fastest the card was asked to run; we derive
// the divider from the host's own base clock via CAPABILITIES.
//
STATIC
EFI_STATUS
EFIAPI
SdhciSetClock (
  IN MMC_HOST  *Host,
  IN UINT32    ClockHz
  )
{
  UINT32  BaseClock;
  UINT32  Div;
  UINT16  ClkReg;
  UINTN   Retry;

  //
  // Disable the SD clock before reprogramming.
  //
  MmioWrite16 (Host->Base + SDHCI_CLOCK_CONTROL, 0);

  if (ClockHz == 0) {
    return EFI_SUCCESS;
  }

  //
  // CAPABILITIES[7:0] = base clock in MHz.
  //
  BaseClock = (MmioRead32 (Host->Base + SDHCI_CAPABILITIES) & 0xFF) * 1000000U;
  if (BaseClock == 0) {
    BaseClock = 50000000U;  // Exynos4 sclk_mmc default guess
  }

  //
  // SDHCI 2.0 divider: SDCLK = base / (2 * Div) with Div a power of two
  // (0x01..0x80), 0 = base.  Round up so we never exceed the target.
  //
  Div = 0;
  if (BaseClock > ClockHz) {
    for (Div = 1; (Div < 0x80) && ((BaseClock / (2 * Div)) > ClockHz); Div <<= 1) {
    }
  }

  ClkReg = (UINT16)((Div << 8) | SDHCI_CLK_INT_EN);
  MmioWrite16 (Host->Base + SDHCI_CLOCK_CONTROL, ClkReg);

  //
  // Wait for the internal clock to stabilise.
  //
  for (Retry = 0; Retry < 1000; Retry++) {
    ClkReg = MmioRead16 (Host->Base + SDHCI_CLOCK_CONTROL);
    if (ClkReg & SDHCI_CLK_INT_STABLE) {
      break;
    }
    gBS->Stall (1000);
  }
  if (!(ClkReg & SDHCI_CLK_INT_STABLE)) {
    return EFI_TIMEOUT;
  }

  //
  // Enable the card-side clock.
  //
  MmioWrite16 (Host->Base + SDHCI_CLOCK_CONTROL,
               (UINT16)(ClkReg | SDHCI_CLK_CARD_EN));

  return EFI_SUCCESS;
}

//
// Program block size/count and transfer mode ahead of the data command (both
// are latched when the COMMAND register is written).
//
STATIC
EFI_STATUS
EFIAPI
SdhciPrepareData (
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount
  )
{
  UINT16  Mode;

  MmioWrite16 (Host->Base + SDHCI_BLOCK_SIZE, (UINT16)BLOCK_SIZE);
  MmioWrite16 (Host->Base + SDHCI_BLOCK_COUNT, (UINT16)BlockCount);

  Mode = 0;
  if (Read) {
    Mode |= SDHCI_TRNS_READ;
  }
  if (BlockCount > 1) {
    Mode |= SDHCI_TRNS_MULTI;
  }
  MmioWrite16 (Host->Base + SDHCI_TRANSFER_MODE, Mode);

  return EFI_SUCCESS;
}

//
// PIO one or more 512-byte blocks through the SDHCI buffer port.
//
STATIC
EFI_STATUS
EFIAPI
SdhciXferBlock (
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount,
  OUT VOID     *Buffer
  )
{
  UINT32  *Ptr;
  UINT32  Words;
  UINT32  Block;

  Ptr = Buffer;

  for (Block = 0; Block < BlockCount; Block++) {
    if (Read) {
      EFI_STATUS Status = WaitInt (Host, SDHCI_INT_BUF_RD);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      for (Words = 0; Words < (BLOCK_SIZE / 4); Words++) {
        *Ptr++ = MmioRead32 (Host->Base + SDHCI_BUFFER_DATA_PORT);
      }
    } else {
      EFI_STATUS Status = WaitInt (Host, SDHCI_INT_BUF_WR);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      for (Words = 0; Words < (BLOCK_SIZE / 4); Words++) {
        MmioWrite32 (Host->Base + SDHCI_BUFFER_DATA_PORT, *Ptr++);
      }
    }
  }

  //
  // Wait for the whole transfer to complete.
  //
  return WaitInt (Host, SDHCI_INT_XFER_DONE);
}

/**
  Reset and prepare an SDHCI host for card identification.

  Leaves the host running at a low identification clock; the caller raises it
  via SetClock once the card is in the transfer state.

  @param[out] Host       Initialised host (ops + Card zeroed).
  @param[in]  Base       MMIO base of the SDHCI controller.
  @param[in]  MaxClockHz Fastest bus clock to use in the transfer state.

  @retval EFI_SUCCESS  Host ready.
**/
EFI_STATUS
SdhciInit (
  IN MMC_HOST  *Host,
  IN UINT32    Base,
  IN UINT32    MaxClockHz
  )
{
  EFI_STATUS  Status;
  UINTN       Retry;

  ZeroMem (Host, sizeof (*Host));
  Host->Base       = Base;
  Host->MaxClockHz = MaxClockHz;
  Host->SendCmd    = SdhciSendCmd;
  Host->SetClock   = SdhciSetClock;
  Host->PrepareData = SdhciPrepareData;
  Host->XferBlock  = SdhciXferBlock;

  //
  // SOFTWARE_RESET: all.
  //
  MmioWrite8 (Host->Base + SDHCI_SOFTWARE_RESET, SDHCI_RST_ALL);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (!(MmioRead8 (Host->Base + SDHCI_SOFTWARE_RESET) & SDHCI_RST_ALL)) {
      break;
    }
    gBS->Stall (1000);
  }
  if (MmioRead8 (Host->Base + SDHCI_SOFTWARE_RESET) & SDHCI_RST_ALL) {
    return EFI_TIMEOUT;
  }

  //
  // Polled: enable every status bit (INT_ENABLE gates whether INT_STATUS
  // latches at all) but signal none of them to the GIC.
  //
  MmioWrite32 (Host->Base + SDHCI_INT_ENABLE,  0xFFFFFFFFU);
  MmioWrite32 (Host->Base + SDHCI_SIGNAL_ENABLE, 0);
  MmioWrite32 (Host->Base + SDHCI_INT_STATUS, 0xFFFFFFFFU);

  //
  // Card clock from sclk_mmc (kernel S3C_SDHCI_CLK_DIV_EXTERNAL).
  //
  MmioAndThenOr32 (Host->Base + S3C_SDHCI_CONTROL2,
                   ~S3C_CTRL2_SELBASECLK_MASK,
                   S3C_CTRL2_SELBASECLK_SCLK << S3C_CTRL2_SELBASECLK_SHIFT);

  //
  // Power on the bus (3.3V).
  //
  MmioWrite8 (Host->Base + SDHCI_POWER_CONTROL, 0x0E);

  //
  // Bus width starts at 1-bit; MmcCore widens to 4-bit for SD later.
  //
  MmioWrite8 (Host->Base + SDHCI_HOST_CONTROL, 0);

  //
  // Identification clock (~400 kHz).
  //
  Status = SdhciSetClock (Host, 400000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SD_DEBUG ((DEBUG_INFO, "Sdhci: host at 0x%x initialised, VER=0x%x CAPS=0x%x CTRL2=0x%x PRES=0x%x\n",
          Base,
          MmioRead16 (Host->Base + SDHCI_HOST_VERSION),
          MmioRead32 (Host->Base + SDHCI_CAPABILITIES),
          MmioRead32 (Host->Base + S3C_SDHCI_CONTROL2),
          MmioRead32 (Host->Base + SDHCI_PRESENT_STATE)));
  return EFI_SUCCESS;
}
