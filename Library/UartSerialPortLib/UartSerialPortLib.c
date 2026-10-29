/** @file
  SerialPortLib on the Samsung S3C-compatible UART used by Exynos4.

  The bootloader normally leaves UART0 configured; this library re-initialises
  it defensively so DEBUG output on a serial dongle works even if the BL
  switched consoles.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>

// Samsung S3C UART register offsets
#define ULCON   0x00   // line control
#define UCON    0x04   // control
#define UFCON   0x08   // FIFO control
#define UMCON   0x0C   // modem control
#define UTRSTAT 0x10   // tx/rx status
#define UTXH    0x20   // tx hold
#define UBRDIV  0x28   // divisor integer
#define UDIVSLOT 0x2C  // divisor slot

#define UTRSTAT_TX_EMPTY (1 << 2)

#define UART_BASE PcdGet32(PcdSerialRegisterBase)

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  UINT32  Base = UART_BASE;

  if (Base == 0) {
    return RETURN_SUCCESS;
  }

  // 8N1, no interrupts, level mode, no FIFO, no loopback, divisor computed
  // for 115200 from whatever baud clock the bootloader left running
  // (we intentionally do not touch the UART clock gate/divisors: if the BL
  // did not set the clock up there is nothing this tiny lib can do anyway).
  MmioWrite32 (Base + ULCON, 0x3);
  MmioWrite32 (Base + UCON, 0x5);      // tx edge / rx level, pclk, polling
  MmioWrite32 (Base + UFCON, 0x0);     // FIFOs off
  MmioWrite32 (Base + UMCON, 0x0);

  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8   *Buffer,
  IN UINTN   NumberOfBytes
  )
{
  UINT32  Base = UART_BASE;
  UINTN   Index;

  if ((Base == 0) || (Buffer == NULL)) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    UINT32  Timeout = 0x100000;

    // Wait until the TX buffer can take a byte
    while (((MmioRead32 (Base + UTRSTAT) & UTRSTAT_TX_EMPTY) == 0) &&
           (Timeout-- != 0))
    {
    }

    MmioWrite32 (Base + UTXH, Buffer[Index]);
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return FALSE;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64            *BaudRate,
  IN OUT UINT32            *ReceiveFifoDepth,
  IN OUT UINT32            *Timeout,
  IN OUT EFI_PARITY_TYPE   *Parity,
  IN OUT UINT8             *DataBits,
  IN OUT EFI_STOP_BITS_TYPE *StopBits
  )
{
  return RETURN_UNSUPPORTED;
}
