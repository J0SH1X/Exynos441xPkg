/** @file
  Exynos4 I2C DXE driver.

  Polling master for the eight s3c2410-style I2C controllers at
  0x13860000 + Bus * 0x10000.  Register flow follows u-boot's
  s3c24x0_i2c.c (drivers/i2c/s3c24x0_i2c.c): poll IICCON's interrupt
  pend bit with a timeout instead of taking the interrupt, handle the
  repeated start between messages exactly like s3c24x0_do_msg().

  All controllers are clocked by ACLK_100 (100 MHz, kernel clock.c:
  i2c0..7 parent clk_aclk_100); the divider is computed for 100 kHz.
  The controller's clock gate (IP_PERIL bit 6+Bus) is enabled on demand.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/ExynosI2c.h>

#define I2C_BASE(Bus)  (0x13860000 + ((Bus) * 0x10000))

// Register offsets (struct s3c24x0_i2c, u-boot)
#define IICCON_OFFSET   0x00
#define IICSTAT_OFFSET  0x04
#define IICADD_OFFSET   0x08
#define IICDS_OFFSET    0x0C

// IICCON bits (plat/regs-iic.h)
#define IICCON_ACKEN   (1u << 7)  // ACK enable
#define IICCON_TXDIV   (1u << 6)  // prescaler: 16 (0) / 512 (1)
#define IICCON_IRQEN   (1u << 5)  // interrupt enable
#define IICCON_IRQPEND (1u << 4)  // pending: byte transfer finished
#define IICCON_SCALE   0x0F       // divisor - 1

// IICSTAT bits
#define IICSTAT_MODE_RX   (2u << 6)  // master receive
#define IICSTAT_MODE_TX   (3u << 6)  // master transmit
#define IICSTAT_START     (1u << 5)  // generate (repeated) start / stop
#define IICSTAT_TXRXEN    (1u << 4)  // Tx/Rx enable
#define IICSTAT_BSY       (1u << 5)  // read: bus busy (write: START/STOP)
#define IICSTAT_NACK      (1u << 0)  // last bit received was NACK

//
// ACLK_100 feeding i2c0..7, target bus speed, and per-byte timeout.
//
#define I2C_INPUT_CLOCK_HZ  100000000
#define I2C_BUS_SPEED_HZ    100000
#define I2C_TIMEOUT_US      10000

// CMU clock gate for i2c0..7: IP_PERIL bit (6 + Bus) (kernel clock.c)
#define CMU_BASE            0x10030000
#define CLKGATE_IP_PERIL    (CMU_BASE + 0x0C950)

/**
  Enable the controller's clock gate (IP_PERIL bit 6+Bus).
**/
STATIC
VOID
I2cEnableClock (
  IN UINT32  Bus
  )
{
  UINT32  Reg;

  Reg = MmioRead32 (CLKGATE_IP_PERIL);
  Reg |= (1u << (6 + Bus));
  MmioWrite32 (CLKGATE_IP_PERIL, Reg);
}

/**
  Program the prescaler/divider for I2C_BUS_SPEED_HZ and go idle.

  Mirrors u-boot's i2c_ch_init().
**/
STATIC
VOID
I2cInit (
  IN UINT32  Base
  )
{
  UINT32  Con;
  UINT32  Div;
  UINT32  Pres;

  Pres = 16;
  if ((I2C_INPUT_CLOCK_HZ / Pres / (16 + 1)) > I2C_BUS_SPEED_HZ) {
    Pres = 512;
  }

  Div = 0;
  while ((I2C_INPUT_CLOCK_HZ / Pres / (Div + 1)) > I2C_BUS_SPEED_HZ) {
    Div++;
  }

  Con  = (Div & IICCON_SCALE) | IICCON_ACKEN | IICCON_IRQEN;
  Con |= (Pres == 512) ? IICCON_TXDIV : 0;
  MmioWrite32 (Base + IICCON_OFFSET, Con);

  MmioWrite32 (Base + IICSTAT_OFFSET, 0);
  MmioWrite32 (Base + IICADD_OFFSET, 0);
  MmioWrite32 (Base + IICSTAT_OFFSET, IICSTAT_MODE_TX | IICSTAT_TXRXEN);
}

/**
  Wait for the current byte transfer to finish.

  @return 0 on success, 1 on NACK, 2 on timeout.
**/
STATIC
INTN
I2cWaitXfer (
  IN UINT32  Base
  )
{
  UINTN  Tries;

  for (Tries = 0; Tries < I2C_TIMEOUT_US; Tries++) {
    if (MmioRead32 (Base + IICCON_OFFSET) & IICCON_IRQPEND) {
      return (MmioRead32 (Base + IICSTAT_OFFSET) & IICSTAT_NACK) ? 1 : 0;
    }

    MicroSecondDelay (1);
  }

  return 2;
}

/**
  Clear the interrupt pend bit: acknowledges the finished byte and clocks
  the next one.
**/
STATIC
VOID
I2cNextByte (
  IN UINT32  Base
  )
{
  MmioWrite32 (Base + IICCON_OFFSET,
               MmioRead32 (Base + IICCON_OFFSET) & ~IICCON_IRQPEND);
}

/**
  Transfer one message; emits a (repeated) start for it.  Caller sends STOP.
**/
STATIC
INTN
I2cDoMsg (
  IN UINT32           Base,
  IN UINT16           Chip,
  IN EXYNOS_I2C_MSG   *Msg,
  IN BOOLEAN          Seq
  )
{
  BOOLEAN  IsRead;
  INTN     Ret;
  UINTN    Index;

  IsRead = (Msg->Flags & I2C_M_RD) != 0;

  // (Re-)enable ACK for this message: a previous read NACKed its last byte
  MmioWrite32 (Base + IICCON_OFFSET,
               MmioRead32 (Base + IICCON_OFFSET) | IICCON_ACKEN);

  // Get the slave address going.  On a repeated start the previous byte's
  // pend bit is still set and holds SCL low; clear it only after START is
  // armed (kernel s3c24xx_i2c_message_start + irq out_ack), otherwise the
  // controller clocks out IICDS as data instead of restarting.
  MmioWrite32 (Base + IICSTAT_OFFSET,
               IICSTAT_TXRXEN | (IsRead ? IICSTAT_MODE_RX : IICSTAT_MODE_TX));
  MmioWrite32 (Base + IICDS_OFFSET, (Chip << 1) | (IsRead ? 1 : 0));
  MicroSecondDelay (1);
  MmioWrite32 (Base + IICSTAT_OFFSET,
               IICSTAT_TXRXEN | IICSTAT_START |
               (IsRead ? IICSTAT_MODE_RX : IICSTAT_MODE_TX));
  if (Seq) {
    I2cNextByte (Base);
  }

  Ret = I2cWaitXfer (Base);
  if (Ret != 0) {
    return Ret;
  }

  if (IsRead) {
    for (Index = 0; (Ret == 0) && (Index < Msg->Len); Index++) {
      if (Index == (UINTN)(Msg->Len - 1)) {
        // NACK the final byte so the slave stops driving the bus
        MmioWrite32 (Base + IICCON_OFFSET,
                     MmioRead32 (Base + IICCON_OFFSET) & ~IICCON_ACKEN);
      }

      I2cNextByte (Base);
      Ret       = I2cWaitXfer (Base);
      Msg->Buf[Index] = MmioRead32 (Base + IICDS_OFFSET);
    }

    if (Ret == 1) {
      Ret = 0;  // NACK after the last read byte is the normal terminator
    }
  } else {
    for (Index = 0; (Ret == 0) && (Index < Msg->Len); Index++) {
      MmioWrite32 (Base + IICDS_OFFSET, Msg->Buf[Index]);
      I2cNextByte (Base);
      Ret = I2cWaitXfer (Base);
    }
  }

  return Ret;
}

STATIC
INTN
EFIAPI
I2cXfer (
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN EXYNOS_I2C_MSG       *Msgs,
  IN INTN                 Num
  )
{
  UINT32  Base;
  INTN    Ret;
  INTN    Index;
  UINTN   Tries;

  if ((Bus >= EXYNOS_I2C_MAX_BUS) || (Msgs == NULL) || (Num <= 0)) {
    return -1;
  }

  Base = I2C_BASE (Bus);
  I2cEnableClock (Bus);
  I2cInit (Base);

  // Wait for the bus to be free
  for (Tries = 0; MmioRead32 (Base + IICSTAT_OFFSET) & IICSTAT_BSY; Tries++) {
    if (Tries >= I2C_TIMEOUT_US) {
      DEBUG ((DEBUG_ERROR, "I2c%u: bus busy, IICSTAT=0x%x\n",
              Bus, MmioRead32 (Base + IICSTAT_OFFSET)));
      return -1;
    }

    MicroSecondDelay (1);
  }

  for (Ret = 0, Index = 0; (Ret == 0) && (Index < Num); Index++) {
    Ret = I2cDoMsg (Base, Msgs[Index].Addr, &Msgs[Index], Index != 0);
  }

  // STOP: same mode as the last message, START bit cleared
  MmioWrite32 (Base + IICSTAT_OFFSET,
               IICSTAT_TXRXEN |
               ((Msgs[Index - 1].Flags & I2C_M_RD) ? IICSTAT_MODE_RX : IICSTAT_MODE_TX));
  I2cNextByte (Base);

  if (Ret != 0) {
    DEBUG ((DEBUG_ERROR, "I2c%u: chip 0x%x msg %d %a, IICSTAT=0x%x IICCON=0x%x\n",
            Bus, Msgs[Index - 1].Addr, Index - 1,
            (Ret == 1) ? "NACK" : "timeout",
            MmioRead32 (Base + IICSTAT_OFFSET), MmioRead32 (Base + IICCON_OFFSET)));
  }

  return (Ret == 0) ? Num : -1;
}

STATIC
INTN
EFIAPI
I2cWrite (
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN UINT16               Chip,
  IN UINT8                *Buf,
  IN UINTN                Count
  )
{
  EXYNOS_I2C_MSG  Msg;

  if ((Buf == NULL) || (Count == 0)) {
    return -1;
  }

  Msg.Addr   = Chip;
  Msg.Flags  = 0;
  Msg.Len    = (UINT16)Count;
  Msg.Buf    = Buf;

  return (I2cXfer (This, Bus, &Msg, 1) == 1) ? (INTN)Count : -1;
}

STATIC
INTN
EFIAPI
I2cRead (
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN UINT16               Chip,
  IN UINT8                Reg,
  IN UINT8                *Buf,
  IN UINTN                Count
  )
{
  EXYNOS_I2C_MSG  Msg[2];

  if ((Buf == NULL) || (Count == 0)) {
    return -1;
  }

  Msg[0].Addr  = Chip;
  Msg[0].Flags = 0;
  Msg[0].Len   = 1;
  Msg[0].Buf   = &Reg;

  Msg[1].Addr  = Chip;
  Msg[1].Flags = I2C_M_RD;
  Msg[1].Len   = (UINT16)Count;
  Msg[1].Buf   = Buf;

  return (I2cXfer (This, Bus, Msg, 2) == 2) ? (INTN)Count : -1;
}

STATIC EXYNOS_I2C_PROTOCOL mExynosI2c = {
  I2cWrite,
  I2cRead,
  I2cXfer,
};

EFI_STATUS
EFIAPI
I2cDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gExynosI2cProtocolGuid,
                  &mExynosI2c,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_INFO, "%a: Exynos I2C protocol installed\n", __FUNCTION__));
  return Status;
}
