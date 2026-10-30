/** @file
  Exynos441x eMMC + microSD storage driver entry point.

  Publishes an EFI_BLOCK_IO_PROTOCOL for the internal eMMC (MSHCI) and, only
  when a card is physically present, for the microSD slot (HSMMC2).  The
  microSD path is gated on the GPX3(4) card-detect GPIO so an empty slot never
  brings up the host controller.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdCardDxe.h"

//
// Per-card BlockIo instance: a BlockIo protocol bound to one MMC_HOST.
//
typedef struct {
  EFI_BLOCK_IO_PROTOCOL  BlkIo;
  EFI_BLOCK_IO_MEDIA     Media;
  MMC_HOST               *Host;
} SD_BLOCK_IO;

//
// One SD_BLOCK_IO + host state per slot (eMMC + microSD).
//
STATIC SD_BLOCK_IO  mEmmcBlkIo;
STATIC SD_BLOCK_IO  mSdBlkIo;
STATIC MMC_HOST     mEmmcHost;
STATIC MMC_HOST     mSdHost;

//
// Vendor device paths (vendor node + end node) so the eMMC and microSD
// BlockIo handles are distinguishable.  Distinct GUIDs per slot.
//
typedef struct {
  VENDOR_DEVICE_PATH  Vendor;
  EFI_DEVICE_PATH     End;
} STORAGE_DEVICE_PATH;

STATIC STORAGE_DEVICE_PATH mEmmcDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)), (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) },
    },
    { 0x6b5f2cf0, 0x8a3d, 0x4c1e, { 0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC STORAGE_DEVICE_PATH mSdDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)), (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) },
    },
    { 0x6b5f2cf0, 0x8a3d, 0x4c1e, { 0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x02 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

//
// BlockIo callbacks: forward to the shared MmcCore layer on this card's host.
//

STATIC
EFI_STATUS
EFIAPI
BlkReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
BlkReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  SD_BLOCK_IO *BlkIo = BASE_CR (This, SD_BLOCK_IO, BlkIo);
  UINTN       Blocks;

  if ((BufferSize == 0) || (Buffer == NULL)) {
    return EFI_SUCCESS;
  }
  if ((BufferSize % BlkIo->Media.BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Blocks = BufferSize / BlkIo->Media.BlockSize;
  return MmcReadBlocks (BlkIo->Host, Lba, Blocks, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
BlkWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  SD_BLOCK_IO *BlkIo = BASE_CR (This, SD_BLOCK_IO, BlkIo);
  UINTN       Blocks;

  if ((BufferSize == 0) || (Buffer == NULL)) {
    return EFI_SUCCESS;
  }
  if ((BufferSize % BlkIo->Media.BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Blocks = BufferSize / BlkIo->Media.BlockSize;
  return MmcWriteBlocks (BlkIo->Host, Lba, Blocks, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
BlkFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

//
// Bring a card up on an already-initialised host and publish its BlockIo.
//
STATIC
EFI_STATUS
SetupBlockIo (
  IN  SD_BLOCK_IO        *BlkIo,
  IN  MMC_HOST           *Host,
  IN  BOOLEAN            IsEmmc,
  IN  STORAGE_DEVICE_PATH *DevicePath,
  OUT EFI_HANDLE         *Handle
  )
{
  EFI_STATUS  Status;

  Status = MmcInitCard (Host, IsEmmc);
  if (EFI_ERROR (Status)) {
    SD_DEBUG ((DEBUG_ERROR, "SdCard: card init failed on 0x%x - %r\n",
            Host->Base, Status));
    return Status;
  }

  ZeroMem (&BlkIo->Media, sizeof (BlkIo->Media));
  BlkIo->Media.MediaPresent   = TRUE;
  BlkIo->Media.RemovableMedia = IsEmmc ? FALSE : TRUE;
  BlkIo->Media.BlockSize      = Host->Card.BlockSize;
  BlkIo->Media.LastBlock      = Host->Card.BlockCount - 1;
  BlkIo->Media.ReadOnly       = FALSE;

  BlkIo->BlkIo.Revision     = EFI_BLOCK_IO_INTERFACE_REVISION;
  BlkIo->BlkIo.Media        = &BlkIo->Media;
  BlkIo->BlkIo.Reset        = BlkReset;
  BlkIo->BlkIo.ReadBlocks   = BlkReadBlocks;
  BlkIo->BlkIo.WriteBlocks  = BlkWriteBlocks;
  BlkIo->BlkIo.FlushBlocks  = BlkFlushBlocks;
  BlkIo->Host               = Host;

  return gBS->InstallMultipleProtocolInterfaces (
                 Handle,
                 &gEfiBlockIoProtocolGuid, &BlkIo->BlkIo,
                 &gEfiDevicePathProtocolGuid, DevicePath,
                 NULL
                 );
}

//
// GPIO drive-strength registers (no protocol op for these): 2 bits per pin.
//
#define EXYNOS4_GPK0DRV  0x1100004C
#define EXYNOS4_GPK1DRV  0x1100006C
#define EXYNOS4_GPK2DRV  0x1100008C

//
// GPIO_eMMC_EN (GPIO_XMMC0_CDn on Q1): the moviNAND VCC enable.
//
#define EMMC_EN_GPIO  EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPK0, 2)

/**
  Power-cycle the eMMC and route its pins to the MSHCI controller.

  Mirrors exynos4_setup_mshci_init_card() + exynos4_setup_mshci_cfg_gpio(8):
  eMMC_EN low with all bus pins as inputs (card unpowered, wedged state from
  the bootloader cleared), then CLK/CMD/DAT[0:3] = GPK0 func 3, DAT[4:7] =
  GPK1[3:6] func 4, eMMC_EN high.  Without the mux the CIU drives pins that
  are still owned by SDHCI0 and the card never sees CMD1.
**/
STATIC
VOID
EmmcBoardInit (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  UINTN  Pin;

  SD_DEBUG ((DEBUG_INFO, "SdCard: GPK0 CON=0x%x DAT=0x%x GPK1 CON=0x%x (bootloader)\n",
          MmioRead32 (0x11000040), MmioRead32 (0x11000044), MmioRead32 (0x11000060)));

  Gpio->Config (Gpio, EMMC_EN_GPIO, EXYNOS_GPIO_MODE_OUTPUT, EXYNOS_GPIO_PULL_NONE);
  Gpio->Set (Gpio, EMMC_EN_GPIO, 0);
  for (Pin = 0; Pin <= 6; Pin++) {
    if (Pin != 2) {
      Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPK0, Pin),
                    EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);
    }
  }
  for (Pin = 3; Pin <= 6; Pin++) {
    Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPK1, Pin),
                  EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_NONE);
  }
  gBS->Stall (100000);  // 100 ms unpowered

  for (Pin = 0; Pin <= 6; Pin++) {
    if (Pin != 2) {
      Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPK0, Pin),
                    3, EXYNOS_GPIO_PULL_NONE);
    }
  }
  for (Pin = 3; Pin <= 6; Pin++) {
    Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPK1, Pin),
                  4, EXYNOS_GPIO_PULL_NONE);
  }
  MmioWrite32 (EXYNOS4_GPK0DRV, 0x2AAA);
  MmioWrite32 (EXYNOS4_GPK1DRV, 0x2AAA);

  Gpio->Set (Gpio, EMMC_EN_GPIO, 1);
  gBS->Stall (30000);  // Q1 set_power waits 20 ms + 10 ms pull-up settle

  SD_DEBUG ((DEBUG_INFO, "SdCard: eMMC pins muxed, GPK0 CON=0x%x DAT=0x%x GPK1 CON=0x%x\n",
          MmioRead32 (0x11000040), MmioRead32 (0x11000044), MmioRead32 (0x11000060)));
}

/**
  Route HSMMC2 pins: CLK/CMD = GPK2[0:1], DAT[0:3] = GPK2[3:6], all func 2
  (exynos4_setup_sdhci2_cfg_gpio, U1/Q1 drive strength LV3).
**/
STATIC
VOID
SdBoardInit (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  UINTN  Pin;

  for (Pin = 0; Pin <= 6; Pin++) {
    if (Pin != 2) {
      Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPK2, Pin),
                    2, EXYNOS_GPIO_PULL_NONE);
    }
  }
  MmioWrite32 (EXYNOS4_GPK2DRV, 0x3FFF);
}

/**
  Driver entry: bring up the eMMC always, and the microSD slot only if a card
  is detected on GPX3(4).

  @param[in] ImageHandle  Driver image handle.
  @param[in] SystemTable  EFI system table.

  @retval EFI_SUCCESS  At least the eMMC BlockIo was published.
**/
EFI_STATUS
EFIAPI
SdCardInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EXYNOS_GPIO_PROTOCOL  *Gpio;
  BOOLEAN               CardPresent;
  EFI_HANDLE            EmmcHandle = NULL;
  EFI_HANDLE            SdHandle   = NULL;

  //
  // Blank the panel so the camera log starts clean.  Direct framebuffer fill
  // via FrameBufferConfigLib (same call PlatformBm makes); the display
  // hardware itself is never touched.
  //
  PaintScreen (0);

  SD_DEBUG ((DEBUG_INFO, "SdCard: probe start\n"));

  Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  if (EFI_ERROR (Status)) {
    SD_PAUSE ((DEBUG_ERROR, "SdCard: GPIO protocol unavailable - %r\n", Status));
    return Status;
  }

  //
  // eMMC (MSHCI) is always present: power it, mux it, publish BlockIo.
  //
  EmmcBoardInit (Gpio);

  Status = MshciInit (&mEmmcHost, EXYNOS4_PA_MSHCI, EXYNOS4_MSHCI_SCLK_HZ);
  if (EFI_ERROR (Status)) {
    SD_DEBUG ((DEBUG_ERROR, "SdCard: eMMC host init failed - %r\n", Status));
  } else {
    Status = SetupBlockIo (&mEmmcBlkIo, &mEmmcHost, TRUE,
                           &mEmmcDevicePath, &EmmcHandle);
    if (EFI_ERROR (Status)) {
      SD_DEBUG ((DEBUG_ERROR, "SdCard: eMMC BlockIo publish failed - %r\n", Status));
    } else {
      SD_DEBUG ((DEBUG_INFO, "SdCard: eMMC BlockIo published (last LBA %lu)\n",
              mEmmcBlkIo.Media.LastBlock));
    }
  }

  //
  // One 5 s hold here, after the eMMC phase and before the microSD slot is
  // touched: the last eMMC line (success, CMD timeout or CIU register dump)
  // stays readable for a photograph.
  //
  gBS->Stall (5000000);

  //
  // microSD (HSMMC2): only touch the host if a card is detected.  GPX3(4) is
  // inverted (0 = card present).  The pin is left in whatever state the
  // bootloader chose (EINT function / pull-down reads as a phantom card), so
  // make it a plain input first.  Pull-up: an empty slot then reads 1 even
  // without the board's external pull-up; an inserted card shorts it to GND.
  //
  Gpio->Config (Gpio, EXYNOS_GPIO (SD_CD_GPIO_BANK, SD_CD_GPIO_PIN),
                EXYNOS_GPIO_MODE_INPUT, EXYNOS_GPIO_PULL_UP);
  gBS->Stall (10000);

  CardPresent = (Gpio->Get (Gpio, EXYNOS_GPIO (SD_CD_GPIO_BANK, SD_CD_GPIO_PIN)) == 0);
  SD_DEBUG ((DEBUG_INFO, "SdCard: GPX3 CON=0x%x DAT=0x%x PUD=0x%x -> CD %a\n",
          MmioRead32 (0x11000C60), MmioRead32 (0x11000C64), MmioRead32 (0x11000C68),
          CardPresent ? "present" : "empty"));
  if (!CardPresent) {
    SD_PAUSE ((DEBUG_INFO, "SdCard: no microSD card detected, slot skipped\n"));
    return EFI_SUCCESS;
  }

  SD_DEBUG ((DEBUG_INFO, "SdCard: microSD card detected, bringing up HSMMC2\n"));

  SdBoardInit (Gpio);

  Status = SdhciInit (&mSdHost, EXYNOS4_PA_HSMMC2, 25000000);
  if (EFI_ERROR (Status)) {
    SD_PAUSE ((DEBUG_ERROR, "SdCard: microSD host init failed - %r\n", Status));
    return EFI_SUCCESS;
  }

  Status = SetupBlockIo (&mSdBlkIo, &mSdHost, FALSE,
                         &mSdDevicePath, &SdHandle);
  if (EFI_ERROR (Status)) {
    SD_PAUSE ((DEBUG_ERROR, "SdCard: microSD BlockIo publish failed - %r\n", Status));
  } else {
    SD_PAUSE ((DEBUG_INFO, "SdCard: microSD BlockIo published (last LBA %lu)\n",
            mSdBlkIo.Media.LastBlock));
  }

  return EFI_SUCCESS;
}
