/** @file
  Exynos441x eMMC + microSD storage driver.

  Exposes each card as an EFI_BLOCK_IO_PROTOCOL on top of the two host
  controllers wired on the Galaxy Note 1 (GT-N7000, Exynos4210):

    - eMMC    -> MSHCI (DesignWare "dwmci") at 0x12550000, CD_PERMANENT,
                 8-bit data bus, sclk_dwmci from MPLL.
    - microSD -> HSMMC2 (standard SDHCI) at 0x12530000, external card-detect
                 on GPX3(4) (inverted), 4-bit data bus, sclk_mmc2.

  The microSD slot is only initialized when a card is actually present: the
  driver reads the card-detect GPIO first and skips the whole HSMMC2 bring-up
  if the slot is empty.  The eMMC is always present, so it is always brought up.

  Both hosts use PIO (no DMA) for data transfer.  This mirrors HtcLeoPkg's
  SdCardDxe (which ported LK's mmc stack) but is written fresh for Exynos4:
  a shared SD/MMC command/init layer drives either host through a small
  ops vtable (command, set clock, read/write data block).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __SD_CARD_DXE_H__
#define __SD_CARD_DXE_H__

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>

//
// Host controller register bases (map-exynos4.h).
//
#define EXYNOS4_PA_HSMMC2  0x12530000  // microSD, standard SDHCI
#define EXYNOS4_PA_MSHCI   0x12550000  // eMMC, DesignWare MSHCI

//
// sclk_dwmci as left by the bootloader (kernel probe logs 80 MHz).  Used as
// the CIU source clock for the CLKDIV calculation; dividers round up, so an
// actual 40 MHz source only makes the bus slower, never out of spec.
//
#define EXYNOS4_MSHCI_SCLK_HZ  80000000

//
// microSD card-detect: GPX3(4), inverted (0 = card present).
//
#define SD_CD_GPIO_BANK  EXYNOS_GPIO_BANK_GPX3
#define SD_CD_GPIO_PIN   4

#define BLOCK_SIZE  512

//
// Card types the init layer distinguishes.
//
typedef enum {
  CardTypeNone = 0,
  CardTypeMmc,   // eMMC (CMD1 op-cond path)
  CardTypeSd,    // microSD (ACMD41 op-cond path)
} CARD_TYPE;

//
// Minimal card state after identification.
//
typedef struct {
  CARD_TYPE  Type;
  UINT16     Rca;          // relative card address (SD/MMC, after CMD3)
  BOOLEAN    HighCapacity; // SDHC / block-addressed
  UINT32     BlockSize;    // typically 512
  UINT64     BlockCount;   // total capacity in BlockSize units
} CARD_STATE;

//
// Host ops: a thin indirection so the shared init/IO layer can drive either
// the SDHCI (microSD) or MSHCI (eMMC) host without knowing its registers.
//
typedef struct _MMC_HOST MMC_HOST;

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_SEND_CMD)(
  IN MMC_HOST  *Host,
  IN UINT32    Cmd,      // raw command index
  IN UINT32    Arg,
  IN UINT32    Flags,    // HOST_CMD_* below
  OUT UINT32   *Resp     // 4 words, may be NULL
  );

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_SET_CLOCK)(
  IN MMC_HOST  *Host,
  IN UINT32    ClockHz
  );

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_PREPARE_DATA)(
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount
  );

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_SET_BUS_WIDTH)(
  IN MMC_HOST  *Host,
  IN UINT32    Width
  );

typedef
EFI_STATUS
(EFIAPI *MMC_HOST_XFER_BLOCK)(
  IN MMC_HOST  *Host,
  IN BOOLEAN   Read,
  IN UINT32    BlockCount,
  OUT VOID     *Buffer
  );

//
// Host command flags (shared by both backends).
//
#define HOST_CMD_RESP_NONE   0x0000
#define HOST_CMD_RESP_SHORT  0x0001
#define HOST_CMD_RESP_LONG   0x0002
#define HOST_CMD_RESP_BUSY   0x0004  // response followed by busy on DAT0
#define HOST_CMD_DATA        0x0010  // this command transfers data
#define HOST_CMD_WRITE       0x0020  // data transfer is card-ward (default read)
#define HOST_CMD_NO_CRC      0x0040  // response CRC invalid by spec (R3: CMD1,
                                    // ACMD41) — do not set the CRC check bit

struct _MMC_HOST {
  UINT32                 Base;       // MMIO base of the host controller
  UINT32                 MaxClockHz; // max bus clock the host can drive
  UINT32                 BaseClockHz; // SDHCI: sclk_mmc feeding the divider
  CARD_STATE             Card;
  MMC_HOST_SEND_CMD      SendCmd;
  MMC_HOST_SET_CLOCK     SetClock;
  MMC_HOST_PREPARE_DATA  PrepareData;
  MMC_HOST_XFER_BLOCK    XferBlock;
  MMC_HOST_SET_BUS_WIDTH SetBusWidth; // optional (NULL: bus stays 1-bit)
};

//
// Shared SD/MMC command layer (MmcCore.c).
//
EFI_STATUS
MmcInitCard (
  IN MMC_HOST  *Host,
  IN BOOLEAN   IsEmmc
  );

EFI_STATUS
MmcReadBlocks (
  IN MMC_HOST  *Host,
  IN EFI_LBA   Lba,
  IN UINTN     Blocks,
  OUT VOID     *Buffer
  );

EFI_STATUS
MmcWriteBlocks (
  IN MMC_HOST  *Host,
  IN EFI_LBA   Lba,
  IN UINTN     Blocks,
  IN VOID      *Buffer
  );

//
// Host backends.
//
EFI_STATUS
SdhciInit (
  IN MMC_HOST  *Host,
  IN UINT32    Base,
  IN UINT32    MaxClockHz,
  IN UINT32    BaseClockHz
  );

EFI_STATUS
MshciInit (
  IN MMC_HOST  *Host,
  IN UINT32    Base,
  IN UINT32    MaxClockHz
  );

#endif // __SD_CARD_DXE_H__
