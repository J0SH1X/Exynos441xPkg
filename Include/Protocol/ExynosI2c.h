/** @file
  Exynos4 I2C protocol.

  The Exynos4210 has eight s3c2410-style I2C controllers (bus 0..7) at
  0x13860000 + Bus * 0x10000, all clocked by ACLK_100 (100 MHz).  On the
  GT-N7000 the buses above 7 are software bit-banged ("i2c-gpio" platform
  devices in mach-u1.c) and are not covered here.

  Modelled on HtcLeoPkg's HtcLeoI2C protocol (Linux-style i2c_msg transfers),
  with the bus number passed per call so one protocol instance covers all
  eight controllers.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_I2C_PROTOCOL_H__
#define __EXYNOS_I2C_PROTOCOL_H__

#define EXYNOS_I2C_PROTOCOL_GUID                                               \
  {                                                                            \
    0x9d3f4a71, 0x5c20, 0x4b8e,                                                \
    {                                                                          \
      0xa1, 0x77, 0x6e, 0x24, 0x41, 0x10, 0x44, 0x02                           \
    }                                                                          \
  }

//
// Hardware s3c2410-style controllers: bus 0..7.
//
#define EXYNOS_I2C_MAX_BUS  8

//
// Linux-style transfer flags.
//
#define I2C_M_RD  0x0001

#pragma pack(1)
typedef struct {
  UINT16  Addr;   // 7-bit slave address
  UINT16  Flags;  // I2C_M_RD for reads
  UINT16  Len;    // payload length in bytes
  UINT8   *Buf;
} EXYNOS_I2C_MSG;
#pragma pack()

typedef struct _EXYNOS_I2C_PROTOCOL EXYNOS_I2C_PROTOCOL;

/**
  Write Count bytes to a slave.

  @param[in] Bus   Controller number, 0..EXYNOS_I2C_MAX_BUS-1.
  @param[in] Chip  7-bit slave address.
  @param[in] Buf   Payload.
  @param[in] Count Payload length in bytes.

  @return Number of bytes written, or -1 on error/NACK/timeout.
**/
typedef
INTN
(EFIAPI *EXYNOS_I2C_WRITE)(
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN UINT16               Chip,
  IN UINT8                *Buf,
  IN UINTN                Count
  );

/**
  Write Reg, then read Count bytes from a slave (repeated start).

  @param[in] Bus   Controller number, 0..EXYNOS_I2C_MAX_BUS-1.
  @param[in] Chip  7-bit slave address.
  @param[in] Reg   Register offset sent before the read.
  @param[in] Buf   Destination buffer.
  @param[in] Count Number of bytes to read.

  @return Number of bytes read, or -1 on error/NACK/timeout.
**/
typedef
INTN
(EFIAPI *EXYNOS_I2C_READ)(
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN UINT16               Chip,
  IN UINT8                Reg,
  IN UINT8                *Buf,
  IN UINTN                Count
  );

/**
  Execute a sequence of messages (Linux i2c_transfer semantics).

  @param[in] Bus  Controller number, 0..EXYNOS_I2C_MAX_BUS-1.
  @param[in] Msgs Message array.
  @param[in] Num  Number of messages.

  @return Number of messages transferred, or -1 on error.
**/
typedef
INTN
(EFIAPI *EXYNOS_I2C_XFER)(
  IN EXYNOS_I2C_PROTOCOL  *This,
  IN UINT32               Bus,
  IN EXYNOS_I2C_MSG       *Msgs,
  IN INTN                 Num
  );

struct _EXYNOS_I2C_PROTOCOL {
  EXYNOS_I2C_WRITE  Write;
  EXYNOS_I2C_READ   Read;
  EXYNOS_I2C_XFER   Xfer;
};

extern EFI_GUID gExynosI2cProtocolGuid;

#endif
