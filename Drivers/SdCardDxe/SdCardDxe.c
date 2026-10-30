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
    DEBUG ((DEBUG_ERROR, "SdCard: card init failed on 0x%x - %r\n",
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
  IN EXYNOS_GPIO_PROTOCOL  *Gpio,
  IN UINTN                 OffUs
  )
{
  UINTN  Pin;


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
  gBS->Stall (OffUs);  // card unpowered

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

}

//
// MAX8997 PMIC on I2C5 (mach-u1.c i2c_devs5, 0xcc >> 1).  vtf_2.8v, the
// microSD card VDD and its CMD/DAT pull-ups, is LDO17.  LDOxCTRL: [7:6] = 3
// on, [5:0] = (mV - 800) / 50.
//
#define MAX8997_I2C_BUS       5
#define MAX8997_I2C_ADDR      0x66
#define MAX8997_REG_ID        0x00
#define MAX8997_REG_LDO17CTRL 0x4B
#define MAX8997_LDO_ON_2V8    (0xC0 | ((2800 - 800) / 50))

/**
  Switch on vtf_2.8v.  The bootloader leaves it off, so the slot's CMD and
  DAT lines read low and every response comes back as index/CRC errors.

  @retval EFI_SUCCESS  LDO17 reads back as on at 2.8 V.
**/
STATIC
EFI_STATUS
SdPowerOn (
  IN EXYNOS_GPIO_PROTOCOL  *Gpio
  )
{
  EFI_STATUS           Status;
  EXYNOS_I2C_PROTOCOL  *I2c;
  UINT8                Id;
  UINT8                Before;
  UINT8                After;
  UINT8                Buf[2];

  Status = gBS->LocateProtocol (&gExynosI2cProtocolGuid, NULL, (VOID **)&I2c);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdCard: I2C protocol unavailable - %r\n", Status));
    return Status;
  }

  //
  // I2C5 SDA/SCL = GPB(6)/GPB(7) func 3, pull-up (s3c_i2c5_cfg_gpio, 4210).
  //
  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB, 6), 3, EXYNOS_GPIO_PULL_UP);
  Gpio->Config (Gpio, EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPB, 7), 3, EXYNOS_GPIO_PULL_UP);

  Id = Before = After = 0xFF;
  if ((I2c->Read (I2c, MAX8997_I2C_BUS, MAX8997_I2C_ADDR, MAX8997_REG_ID, &Id, 1) != 1) ||
      (I2c->Read (I2c, MAX8997_I2C_BUS, MAX8997_I2C_ADDR, MAX8997_REG_LDO17CTRL, &Before, 1) != 1)) {
    DEBUG ((DEBUG_ERROR, "SdCard: MAX8997 read failed on i2c%u\n", MAX8997_I2C_BUS));
    return EFI_DEVICE_ERROR;
  }

  Buf[0] = MAX8997_REG_LDO17CTRL;
  Buf[1] = MAX8997_LDO_ON_2V8;
  if (I2c->Write (I2c, MAX8997_I2C_BUS, MAX8997_I2C_ADDR, Buf, 2) != 2) {
    DEBUG ((DEBUG_ERROR, "SdCard: MAX8997 LDO17 write failed\n"));
    return EFI_DEVICE_ERROR;
  }
  I2c->Read (I2c, MAX8997_I2C_BUS, MAX8997_I2C_ADDR, MAX8997_REG_LDO17CTRL, &After, 1);


  gBS->Stall (20000);  // rail ramp + card power-up (SD spec: >= 1 ms)
  return (After == MAX8997_LDO_ON_2V8) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

//
// CMU: sclk_mmc2 = mux(CLKSRC_FSYS[11:8]) / (DIV_FSYS2[3:0]+1) / (DIV_FSYS2[15:8]+1).
//
#define EXYNOS4210_MPLL_CON0     0x10044108
#define EXYNOS4_CLKSRC_FSYS      0x1003C240
#define EXYNOS4_CLKSRC_MASK_FSYS 0x1003C340
#define EXYNOS4_CLKDIV_FSYS2     0x1003C548
#define EXYNOS4_CLKDIV_STAT_FSYS2 0x1003C648
#define CLKSRC_MPLL              6
#define SD_SCLK_TARGET_HZ        50000000U

/**
  Feed HSMMC2 from MPLL at <= 50 MHz.  The bootloader leaves sclk_mmc2 at
  ~8.9 MHz (kernel probe log), and the SDHCI reports no base clock, so the
  rate must be known exactly for the divider to mean anything.

  @return sclk_mmc2 in Hz.
**/
STATIC
UINT32
SdClockInit (
  VOID
  )
{
  UINT32  Con;
  UINT32  Mpll;
  UINT32  Total;
  UINT32  Ratio;
  UINT32  Pre;
  UINTN   Retry;

  Con = MmioRead32 (EXYNOS4210_MPLL_CON0);
  if (((Con >> 8) & 0x3F) == 0) {
    DEBUG ((DEBUG_ERROR, "SdCard: MPLL_CON0=0x%x unusable, sclk_mmc2 left alone\n", Con));
    return 8888888;  // bootloader rate per the kernel's probe log
  }
  //
  // Exynos4210 MPLL is a PLL4508: Fout = Fin * M / (P << (S - 1))
  // (kernel s5p_get_pll45xx, pll_4508).
  //
  Mpll = (UINT32)(((UINT64)24000000 * ((Con >> 16) & 0x3FF)) /
                  ((((Con >> 8) & 0x3F) << ((Con & 0x7) - ((Con & 0x7) ? 1 : 0)))));


  Total = (Mpll + SD_SCLK_TARGET_HZ - 1) / SD_SCLK_TARGET_HZ;
  Ratio = MIN (Total, 16U);
  Pre   = (Total + Ratio - 1) / Ratio;

  MmioAndThenOr32 (EXYNOS4_CLKSRC_FSYS, ~(0xFU << 8), CLKSRC_MPLL << 8);
  MmioOr32 (EXYNOS4_CLKSRC_MASK_FSYS, 1U << 8);
  MmioAndThenOr32 (EXYNOS4_CLKDIV_FSYS2, ~0xFF0FU, ((Pre - 1) << 8) | (Ratio - 1));
  for (Retry = 0; Retry < 1000; Retry++) {
    if (!(MmioRead32 (EXYNOS4_CLKDIV_STAT_FSYS2) & ((1U << 8) | (1U << 0)))) {
      break;
    }
    gBS->Stall (100);
  }

  return Mpll / (Ratio * Pre);
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
  MmioWrite32 (EXYNOS4_GPK2DRV, 0x1555);  // LV3 per pin (CONFIG_MACH_U1)
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
  UINTN                 Attempt;
  EFI_HANDLE            EmmcHandle = NULL;
  EFI_HANDLE            SdHandle   = NULL;

  Status = gBS->LocateProtocol (&gExynosGpioProtocolGuid, NULL, (VOID **)&Gpio);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdCard: GPIO protocol unavailable - %r\n", Status));
    return Status;
  }

  //
  // eMMC (MSHCI) is always present: power it, mux it, publish BlockIo.
  // A failed first attempt is retried after a longer (1 s) power cut.
  //
  for (Attempt = 0; Attempt < 2; Attempt++) {
    EmmcBoardInit (Gpio, (Attempt == 0) ? 100000 : 1000000);

    Status = MshciInit (&mEmmcHost, EXYNOS4_PA_MSHCI, EXYNOS4_MSHCI_SCLK_HZ);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SdCard: eMMC host init failed - %r\n", Status));
      continue;
    }
    Status = SetupBlockIo (&mEmmcBlkIo, &mEmmcHost, TRUE,
                           &mEmmcDevicePath, &EmmcHandle);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SdCard: eMMC attempt %u failed - %r\n", Attempt, Status));
      continue;
    }
    DEBUG ((DEBUG_INFO, "SdCard: eMMC BlockIo published (last LBA %lu)\n",
            mEmmcBlkIo.Media.LastBlock));
    break;
  }


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
  if (!CardPresent) {
    DEBUG ((DEBUG_INFO, "SdCard: no microSD card detected, slot skipped\n"));
    return EFI_SUCCESS;
  }


  Status = SdPowerOn (Gpio);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdCard: microSD power-on failed - %r\n", Status));
    return EFI_SUCCESS;
  }

  SdBoardInit (Gpio);

  Status = SdhciInit (&mSdHost, EXYNOS4_PA_HSMMC2, 25000000, SdClockInit ());
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdCard: microSD host init failed - %r\n", Status));
    return EFI_SUCCESS;
  }

  Status = SetupBlockIo (&mSdBlkIo, &mSdHost, FALSE,
                         &mSdDevicePath, &SdHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdCard: microSD BlockIo publish failed - %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "SdCard: microSD BlockIo published (last LBA %lu)\n",
            mSdBlkIo.Media.LastBlock));
  }

  return EFI_SUCCESS;
}
