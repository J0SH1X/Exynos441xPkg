/** @file
  DesignWare MSHCI host backend (Exynos4 dwmci) for the internal eMMC.

  Drives the Samsung/DesignWare "dwmci" controller at EXYNOS4_PA_MSHCI in
  PIO mode.  Register layout follows drivers/mmc/host/mshci.h (CTRL/PWREN/
  CLKDIV/CLKSRC/CLKENA/CTYPE/BLKSIZ/BYTCNT/INTMSK/CMDARG/CMD/RINTSTS/STATUS/
  FIFODAT).  This host is wired CD_PERMANENT with an 8-bit data bus, so the
  card is always present and we always use the full width.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdCardDxe.h"

//
// MSHCI register offsets (mshci.h).
//
#define MSHCI_CTRL     0x00
#define MSHCI_PWREN    0x04
#define MSHCI_CLKDIV   0x08
#define MSHCI_CLKSRC   0x0C
#define MSHCI_CLKENA   0x10
#define MSHCI_TMOUT    0x14
#define MSHCI_CTYPE    0x18
#define MSHCI_BLKSIZ   0x1C
#define MSHCI_BYTCNT   0x20
#define MSHCI_INTMSK   0x24
#define MSHCI_CMDARG   0x28
#define MSHCI_CMD      0x2C
#define MSHCI_RESP0    0x30
#define MSHCI_MINTSTS  0x40
#define MSHCI_RINTSTS  0x44
#define MSHCI_STATUS   0x48
#define MSHCI_USRID    0x68
#define MSHCI_VERID    0x6C
#define MSHCI_HCON     0x70
#define MSHCI_UHS_REG  0x74
#define MSHCI_BMOD     0x80
#define MSHCI_FIFODAT  0x100

//
// CTRL bits.
//
#define MSHCI_CTRL_RESET  (1U << 0)
#define MSHCI_CTRL_FIFO_RESET (1U << 1)
#define MSHCI_CTRL_DMA_RESET  (1U << 2)
#define MSHCI_CTRL_ALL_RESET  (MSHCI_CTRL_RESET | MSHCI_CTRL_FIFO_RESET | \
                               MSHCI_CTRL_DMA_RESET)
#define MSHCI_CTRL_INT_ENABLE (1U << 4)

//
// CMD register bits (mshci.h).
//
#define CMD_RESP_EXP   (1U << 6)
#define CMD_RESP_LEN   (1U << 7)
#define CMD_CHECK_CRC  (1U << 8)
#define CMD_DATA_EXP   (1U << 9)
#define CMD_RW         (1U << 10)  // 1 = write (host->card), 0 = read
#define CMD_TRANSMODE  (1U << 11)  // 1 = stream (MMC_DATA_STREAM), not multi-block
#define CMD_AUTO_STOP  (1U << 12)
#define CMD_WAIT_PRV_DAT (1U << 13)  // CIU waits for previous data transfer
#define CMD_STOP_ABORT (1U << 14)  // CMD12: abort the data transfer in flight
#define CMD_SEND_INIT  (1U << 15)  // 80 init clocks before the command
#define CMD_SEND_CLK_ONLY (1U << 21)  // update-clock: no card command
#define CMD_START      (1U << 31)

//
// Update-clock pseudo-command: start + send-clock-only + wait-previous-data.
// Must be used whenever CLKENA/CLKDIV change, or the CIU never latches the new
// clock and subsequent real commands never complete (kernel's CMD_ONLY_CLK).
//
#define CMD_ONLY_CLK   (CMD_START | CMD_SEND_CLK_ONLY | CMD_WAIT_PRV_DAT)

//
// RINTSTS / MINTSTS bits.
//
#define INTMSK_RE     (1U << 1)
#define INTMSK_CDONE  (1U << 2)
#define INTMSK_DTO    (1U << 3)
#define INTMSK_TXDR   (1U << 4)
#define INTMSK_RXDR   (1U << 5)
#define INTMSK_RCRC   (1U << 6)
#define INTMSK_DCRC   (1U << 7)
#define INTMSK_RTO    (1U << 8)
#define INTMSK_DRTO   (1U << 9)
#define INTMSK_HTO    (1U << 10)
#define INTMSK_HLE    (1U << 12)
#define INTMSK_SBE    (1U << 13)
#define INTMSK_EBE    (1U << 15)

#define INTMSK_ERR    (INTMSK_RE | INTMSK_RCRC | INTMSK_DCRC | INTMSK_RTO | \
                       INTMSK_DRTO | INTMSK_HTO | INTMSK_HLE | \
                       INTMSK_SBE | INTMSK_EBE)

//
// STATUS bits.
//
#define MSHCI_STATUS_FIFO_EMPTY (1U << 2)
#define MSHCI_STATUS_DATA_BUSY  (1U << 9)
#define MSHCI_STATUS_FIFO_COUNT(s) (((s) >> 17) & 0x1FFF)

//
// FIFO depth in 32-bit words (platform data fifo_depth = 0x80).
//
#define MSHCI_FIFO_DEPTH  0x80

STATIC
EFI_STATUS
EFIAPI
MshciSendCmd (
  IN MMC_HOST  *Host,
  IN UINT32    Cmd,
  IN UINT32    Arg,
  IN UINT32    Flags,
  OUT UINT32   *Resp
  )
{
  UINTN  Retry;
  UINT32 CmdReg;
  UINT32 Int;

  //
  // Wait for the command register to be free (CMD_START clear) before
  // issuing.  Note: deliberately NOT gating on STATUS.DATA_BUSY here — the
  // kernel's mshci_send_command doesn't either.  The CIU serialises against
  // data itself; a stale DATA_BUSY left over from the bootloader's last
  // transfer must not block identification commands.
  //
  for (Retry = 0; Retry < 10000; Retry++) {
    if (!(MmioRead32 (Host->Base + MSHCI_CMD) & CMD_START)) {
      break;
    }
    gBS->Stall (1000);
  }
  if (MmioRead32 (Host->Base + MSHCI_CMD) & CMD_START) {
    DEBUG ((DEBUG_ERROR, "Mshci: CMD_START never released\n"));
    return EFI_TIMEOUT;
  }

  //
  // Clear any stale interrupts (and FIFO leftovers from the bootloader).
  //
  MmioWrite32 (Host->Base + MSHCI_RINTSTS, 0xFFFFFFFFU);

  MmioWrite32 (Host->Base + MSHCI_CMDARG, Arg);

  //
  // WAIT_PRV_DAT only on data commands (mainline dw_mmc).  The vendor kernel
  // sets it on every command, but with DAT0 held low by a wedged card the
  // CIU then never issues CMD1 at all: no CDONE, no RTO.  CMD0 gets the 80
  // initialisation clocks the card needs after power-up; CMD12 aborts.
  //
  // CRC check only when the response carries a valid CRC (kernel's
  // MMC_RSP_CRC).  R3 responses (CMD1/ACMD41) have an invalid CRC field by
  // spec; checking it can wedge the CIU instead of completing the command.
  //
  CmdReg = Cmd | CMD_START;
  if (Cmd == 0) {
    CmdReg |= CMD_SEND_INIT;
  } else if (Cmd == 12) {
    CmdReg |= CMD_STOP_ABORT;
  } else if (Flags & HOST_CMD_DATA) {
    CmdReg |= CMD_WAIT_PRV_DAT;
  }
  if (!(Flags & HOST_CMD_NO_CRC)) {
    CmdReg |= CMD_CHECK_CRC;
  }
  if (Flags & HOST_CMD_DATA) {
    CmdReg |= CMD_DATA_EXP;
    if (Flags & HOST_CMD_WRITE) {
      CmdReg |= CMD_RW;  // host->card write (kernel mshci_set_transfer_mode)
    }
  }
  //
  // R1b is a short response; the busy phase is handled by the caller.
  //
  if (Flags & HOST_CMD_RESP_LONG) {
    CmdReg |= CMD_RESP_EXP | CMD_RESP_LEN;
  } else if (Flags & (HOST_CMD_RESP_SHORT | HOST_CMD_RESP_BUSY)) {
    CmdReg |= CMD_RESP_EXP;
  }

  MmioWrite32 (Host->Base + MSHCI_CMD, CmdReg);

  //
  // Poll RINTSTS for command-done or error (~5 s budget).
  //
  for (Retry = 0; Retry < 5000; Retry++) {
    Int = MmioRead32 (Host->Base + MSHCI_RINTSTS);
    if (Int & INTMSK_ERR) {
      MmioWrite32 (Host->Base + MSHCI_RINTSTS, Int);
      DEBUG ((DEBUG_ERROR, "Mshci: cmd %u INT error 0x%x CMD=0x%x STATUS=0x%x\n",
              Cmd, Int, CmdReg, MmioRead32 (Host->Base + MSHCI_STATUS)));
      return EFI_DEVICE_ERROR;
    }
    if (Int & INTMSK_CDONE) {
      MmioWrite32 (Host->Base + MSHCI_RINTSTS, Int & INTMSK_CDONE);
      goto CommandDone;
    }
    gBS->Stall (1000);
  }

  DEBUG ((DEBUG_ERROR, "Mshci: cmd %u no CDONE, RINTSTS=0x%x STATUS=0x%x\n",
          Cmd, MmioRead32 (Host->Base + MSHCI_RINTSTS),
          MmioRead32 (Host->Base + MSHCI_STATUS)));
  return EFI_TIMEOUT;

CommandDone:

  if (Resp != NULL) {
    Resp[0] = MmioRead32 (Host->Base + MSHCI_RESP0);
    Resp[1] = MmioRead32 (Host->Base + MSHCI_RESP0 + 0x4);
    Resp[2] = MmioRead32 (Host->Base + MSHCI_RESP0 + 0x8);
    Resp[3] = MmioRead32 (Host->Base + MSHCI_RESP0 + 0xC);
  }

  return EFI_SUCCESS;
}

//
// MSHCI clock divider: card clock = sclk / (2 * div), div 0 = bypass.  Program CLKDIV,
// pulse the "update clock" command, then enable CLKENA.
//
STATIC
EFI_STATUS
EFIAPI
MshciSetClock (
  IN MMC_HOST  *Host,
  IN UINT32    ClockHz
  )
{
  UINT32  Div;
  UINTN   Retry;

  //
  // Disable clock first.  Wait for any in-flight command to release CMD_START,
  // then gate CLKENA low and push the change through with the update-clock
  // pseudo-command (kernel's mshci_clock_onoff path).
  //
  for (Retry = 0; Retry < 5000; Retry++) {
    if (!(MmioRead32 (Host->Base + MSHCI_CMD) & CMD_START)) {
      break;
    }
    gBS->Stall (1000);
  }

  MmioWrite32 (Host->Base + MSHCI_CLKENA, 0);
  MmioWrite32 (Host->Base + MSHCI_CMD, CMD_ONLY_CLK);
  for (Retry = 0; Retry < 5000; Retry++) {
    if (!(MmioRead32 (Host->Base + MSHCI_CMD) & CMD_START)) {
      break;
    }
    gBS->Stall (1000);
  }

  if (ClockHz == 0) {
    return EFI_SUCCESS;
  }

  //
  // Round up so the card clock never exceeds ClockHz.
  //
  if (ClockHz >= Host->MaxClockHz) {
    Div = 0;
  } else {
    Div = (Host->MaxClockHz + (2 * ClockHz) - 1) / (2 * ClockHz);
  }
  if (Div > 0xFF) {
    Div = 0xFF;
  }

  MmioWrite32 (Host->Base + MSHCI_CLKDIV, Div);

  //
  // Re-enable the clock and push the new divider through with update-clock.
  //
  MmioWrite32 (Host->Base + MSHCI_CLKENA, 1);
  MmioWrite32 (Host->Base + MSHCI_CMD, CMD_ONLY_CLK);

  for (Retry = 0; Retry < 5000; Retry++) {
    if (!(MmioRead32 (Host->Base + MSHCI_CMD) & CMD_START)) {
      return EFI_SUCCESS;
    }
    gBS->Stall (1000);
  }

  DEBUG ((DEBUG_ERROR, "Mshci: clock update never completed\n"));
  return EFI_TIMEOUT;
}

//
// Program block size, byte count, and bus width ahead of the data command.
//
STATIC
EFI_STATUS
EFIAPI
MshciPrepareData (
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount
  )
{
  UINTN  Retry;

  //
  // Drop anything left in the FIFO from an earlier (failed) transfer.
  //
  MmioOr32 (Host->Base + MSHCI_CTRL, MSHCI_CTRL_FIFO_RESET);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (!(MmioRead32 (Host->Base + MSHCI_CTRL) & MSHCI_CTRL_FIFO_RESET)) {
      break;
    }
    gBS->Stall (1000);
  }

  //
  // Bus stays 1-bit (CTYPE = 0): the card is never switched with CMD6, and
  // CTYPE bit 0 means 4-bit, 8-bit is bit 16.
  //
  MmioWrite32 (Host->Base + MSHCI_TMOUT, 0xFFFFFFFFU);
  MmioWrite32 (Host->Base + MSHCI_BLKSIZ, BLOCK_SIZE);
  MmioWrite32 (Host->Base + MSHCI_BYTCNT, BlockCount * BLOCK_SIZE);
  return EFI_SUCCESS;
}

//
// PIO a block through the FIFODAT port (32-bit accesses).
//
STATIC
EFI_STATUS
EFIAPI
MshciXferBlock (
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount,
  OUT VOID     *Buffer
  )
{
  UINT32  *Ptr;
  UINT32  Words;
  UINT32  TotalWords;
  UINT32  Count;
  UINT32  Int;
  UINTN   Retry;

  Ptr        = Buffer;
  TotalWords = (BlockCount * BLOCK_SIZE) / 4;

  //
  // Move words while the FIFO has data (read) or room (write).  Polling the
  // FIFO count rather than RXDR/TXDR: those are watermark-latched and would
  // let us read an empty FIFO once set.
  //
  Words = 0;
  Retry = 0;
  while (Words < TotalWords) {
    Int = MmioRead32 (Host->Base + MSHCI_RINTSTS);
    if (Int & INTMSK_ERR) {
      MmioWrite32 (Host->Base + MSHCI_RINTSTS, Int);
      DEBUG ((DEBUG_ERROR, "Mshci: data error 0x%x after %u words\n", Int, Words));
      return EFI_DEVICE_ERROR;
    }

    Count = MSHCI_STATUS_FIFO_COUNT (MmioRead32 (Host->Base + MSHCI_STATUS));
    if (!Read) {
      Count = MSHCI_FIFO_DEPTH - Count;
    }
    if (Count == 0) {
      if (++Retry >= 5000) {
        DEBUG ((DEBUG_ERROR, "Mshci: FIFO stalled after %u words, RINTSTS=0x%x\n",
                Words, Int));
        return EFI_TIMEOUT;
      }
      gBS->Stall (1000);
      continue;
    }

    Retry = 0;
    for ( ; (Count > 0) && (Words < TotalWords); Count--, Words++) {
      if (Read) {
        *Ptr++ = MmioRead32 (Host->Base + MSHCI_FIFODAT);
      } else {
        MmioWrite32 (Host->Base + MSHCI_FIFODAT, *Ptr++);
      }
    }
  }

  //
  // Wait for data-transfer-complete.
  //
  for (Retry = 0; Retry < 5000; Retry++) {
    Int = MmioRead32 (Host->Base + MSHCI_RINTSTS);
    if (Int & INTMSK_ERR) {
      MmioWrite32 (Host->Base + MSHCI_RINTSTS, Int);
      return EFI_DEVICE_ERROR;
    }
    if (Int & INTMSK_DTO) {
      MmioWrite32 (Host->Base + MSHCI_RINTSTS, Int);
      return EFI_SUCCESS;
    }
    gBS->Stall (1000);
  }

  return EFI_TIMEOUT;
}

/**
  Reset and prepare the MSHCI (eMMC) host for card identification.

  @param[out] Host       Initialised host (ops + Card zeroed).
  @param[in]  Base       MMIO base of the MSHCI controller.
  @param[in]  MaxClockHz Fastest bus clock (sclk_dwmci) to use for transfers.

  @retval EFI_SUCCESS  Host ready.
**/
EFI_STATUS
MshciInit (
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
  Host->SendCmd    = MshciSendCmd;
  Host->SetClock   = MshciSetClock;
  Host->PrepareData = MshciPrepareData;
  Host->XferBlock  = MshciXferBlock;


  //
  // Controller, FIFO and DMA reset (the bootloader leaves IDMAC state and
  // FIFO contents behind).
  //
  MmioWrite32 (Host->Base + MSHCI_CTRL, MSHCI_CTRL_ALL_RESET);
  for (Retry = 0; Retry < 1000; Retry++) {
    if (!(MmioRead32 (Host->Base + MSHCI_CTRL) & MSHCI_CTRL_ALL_RESET)) {
      break;
    }
    gBS->Stall (1000);
  }
  if (MmioRead32 (Host->Base + MSHCI_CTRL) & MSHCI_CTRL_ALL_RESET) {
    DEBUG ((DEBUG_ERROR, "Mshci: reset stuck, CTRL=0x%x\n",
            MmioRead32 (Host->Base + MSHCI_CTRL)));
    return EFI_TIMEOUT;
  }

  //
  // The card rail itself is GPK0(2) (power-cycled by EmmcBoardInit before we
  // get here); PWREN only gates the controller's card power output.
  //
  //
  // PIO only: the bootloader runs the IDMAC; reset it and leave it disabled.
  //
  MmioWrite32 (Host->Base + MSHCI_BMOD, 1);  // BMOD_IDMAC_RESET
  MmioWrite32 (Host->Base + MSHCI_BMOD, 0);

  MmioWrite32 (Host->Base + MSHCI_PWREN, 1);
  MmioWrite32 (Host->Base + MSHCI_CTYPE, 0);  // 1-bit for identification
  MmioWrite32 (Host->Base + MSHCI_TMOUT, 0xFFFFFFFFU);
  MmioWrite32 (Host->Base + MSHCI_INTMSK, 0);
  MmioWrite32 (Host->Base + MSHCI_RINTSTS, 0xFFFFFFFFU);

  //
  // Polling mode: enable the controller's interrupt engine but never route it
  // to the GIC; we read RINTSTS directly.
  //
  MmioWrite32 (Host->Base + MSHCI_CTRL, MSHCI_CTRL_INT_ENABLE);

  //
  // Identification clock (~400 kHz).
  //
  Status = MshciSetClock (Host, 400000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}
