/** @file
  Exynos4210 USB peripheral driver (DWC2 USB device controller).

  Publishes EFI_USBFN_IO_PROTOCOL.  Peripheral mode only: the core is forced
  into device mode; there is no host role, no ID pin and no VBUS drive.

  Interrupt driven: the controller interrupt services the FIFOs and the EP0
  standard requests and queues UsbFn messages; EventHandler() only dequeues
  them.  Data moves through the FIFOs in slave (PIO) mode.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __USBFN_DXE_H__
#define __USBFN_DXE_H__

#include <Uefi.h>
#include <IndustryStandard/Usb.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/ExynosClock.h>
#include <Protocol/ExynosGpio.h>
#include <Protocol/ExynosI2c.h>
#include <Protocol/HardwareInterrupt.h>
#include <Protocol/UsbFunctionIo.h>

//
// Controller base and interrupt (map-exynos4.h EXYNOS4_PA_HSOTG,
// irqs-exynos4.h IRQ_USB_HSOTG = SPI 71).
//
#define DWC2_BASE  0x12480000
#define DWC2_IRQ   (32 + 71)

//
// DWC2 global registers.
//
#define GAHBCFG    0x008
#define GUSBCFG    0x00C
#define GRSTCTL    0x010
#define GINTSTS    0x014
#define GINTMSK    0x018
#define GRXSTSP    0x020
#define GRXFSIZ    0x024
#define GNPTXFSIZ  0x028
#define GSNPSID    0x040
#define GHWCFG2    0x048
#define GHWCFG3    0x04C
#define GHWCFG4    0x050
#define DIEPTXF(n) (0x104 + ((n) - 1) * 4)

#define GAHBCFG_GLBLINTRMSK    (1U << 0)
#define GAHBCFG_TXFEMPLVL      (1U << 7)   // TxFE when the FIFO is completely empty

#define GUSBCFG_FORCEDEVMODE   (1U << 30)
#define GUSBCFG_PHYIF16        (1U << 3)
#define GUSBCFG_TRDTIM(x)      ((x) << 10)
#define GUSBCFG_TOUTCAL(x)     (x)

#define GRSTCTL_CSFTRST        (1U << 0)
#define GRSTCTL_RXFFLSH        (1U << 4)
#define GRSTCTL_TXFFLSH        (1U << 5)
#define GRSTCTL_TXFNUM(x)      ((x) << 6)
#define GRSTCTL_TXFNUM_ALL     GRSTCTL_TXFNUM (0x10)
#define GRSTCTL_AHBIDLE        (1U << 31)

#define GINT_RXFLVL            (1U << 4)
#define GINT_USBSUSP           (1U << 11)
#define GINT_USBRST            (1U << 12)
#define GINT_ENUMDONE          (1U << 13)
#define GINT_IEPINT            (1U << 18)
#define GINT_OEPINT            (1U << 19)
#define GINT_WKUPINT           (1U << 31)
#define GINT_DEVICE_MASK       (GINT_RXFLVL | GINT_USBSUSP | GINT_USBRST | \
                                GINT_ENUMDONE | GINT_IEPINT | GINT_OEPINT | \
                                GINT_WKUPINT)

#define GRXSTS_EPNUM(x)        ((x) & 0xF)
#define GRXSTS_BCNT(x)         (((x) >> 4) & 0x7FF)
#define GRXSTS_PKTSTS(x)       (((x) >> 17) & 0xF)
#define PKTSTS_OUT_DATA        2
#define PKTSTS_OUT_DONE        3
#define PKTSTS_SETUP_DONE      4
#define PKTSTS_SETUP_DATA      6

//
// DWC2 device registers.
//
#define DCFG        0x800
#define DCTL        0x804
#define DSTS        0x808
#define DIEPMSK     0x810
#define DOEPMSK     0x814
#define DAINT       0x818
#define DAINTMSK    0x81C
#define DIEPEMPMSK  0x834
#define DIEPCTL(n)  (0x900 + (n) * 0x20)
#define DIEPINT(n)  (0x908 + (n) * 0x20)
#define DIEPTSIZ(n) (0x910 + (n) * 0x20)
#define DTXFSTS(n)  (0x918 + (n) * 0x20)
#define DOEPCTL(n)  (0xB00 + (n) * 0x20)
#define DOEPINT(n)  (0xB08 + (n) * 0x20)
#define DOEPTSIZ(n) (0xB10 + (n) * 0x20)
#define PCGCCTL     0xE00
#define DFIFO(n)    (0x1000 + (n) * 0x1000)

#define DCFG_DEVADDR_MASK      (0x7FU << 4)
#define DCFG_DEVADDR(x)        ((x) << 4)

#define DCTL_SFTDISCON         (1U << 1)
#define DCTL_CGNPINNAK         (1U << 8)
#define DCTL_CGOUTNAK          (1U << 10)

#define DSTS_ENUMSPD(x)        (((x) >> 1) & 0x3)
#define ENUMSPD_HS             0

#define DEPCTL_MPS_MASK        0x7FF
#define DEPCTL_USBACTEP        (1U << 15)
#define DEPCTL_EPTYPE(x)       ((x) << 18)
#define DEPCTL_STALL           (1U << 21)
#define DEPCTL_TXFNUM(x)       ((x) << 22)
#define DEPCTL_CNAK            (1U << 26)
#define DEPCTL_SNAK            (1U << 27)
#define DEPCTL_SETD0PID        (1U << 28)
#define DEPCTL_EPDIS           (1U << 30)
#define DEPCTL_EPENA           (1U << 31)

#define DEPINT_XFERCOMPL       (1U << 0)
#define DEPINT_EPDISBLD        (1U << 1)
#define DOEPINT_SETUP          (1U << 3)
#define DIEPINT_TXFE           (1U << 7)

#define DEPTSIZ_PKTCNT(x)      ((x) << 19)
#define DOEPTSIZ0_SUPCNT(x)    ((x) << 29)

//
// FIFO layout, in 32-bit words: one shared RX FIFO, then one TX FIFO per IN
// endpoint (dedicated-FIFO mode).
//
#define RX_FIFO_WORDS    512
#define EP0_TXF_WORDS    64
#define EPN_TXF_WORDS    256

//
// Bring-up self test (SelfTest.c): the driver starts itself and runs a bulk
// loopback device.  Off: UsbLogDxe (and later KDNET) is the client.
//
#ifndef USBFN_SELFTEST
#define USBFN_SELFTEST         0
#endif

#define USBFN_MAX_EPS          4      // EP0 + bulk endpoint numbers 1..3
#define USBFN_EP0_MPS          64
#define USBFN_MAX_TRANSFER     (64 * 1024)
#define USBFN_EVENT_QUEUE      32

//
// Identity reported by GetVendorIdProductId / GetDeviceInfo.  pid.codes test
// VID/PID: development only, the class driver may override them in its own
// device descriptor.
//
#define USBFN_VID              0x1209
#define USBFN_PID              0x0001

//
// Port routing on the GT-N7000: micro-USB D+/D- go through the MAX8997 MUIC
// and GPIO_USB_SEL (GPL0(6), low = AP, high = modem).
//
#define USB_SEL_GPIO           EXYNOS_GPIO (EXYNOS_GPIO_BANK_GPL0, 6)
#define MUIC_I2C_BUS           5
#define MUIC_I2C_ADDR          0x25   // 0x4A >> 1
#define MUIC_REG_CTRL1         0x0C
#define MUIC_CTRL1_SW_MASK     0x3F
#define MUIC_CTRL1_SW_USB      0x09   // COMN1SW = COMP2SW = USB

//
// One transfer per endpoint and direction.
//
typedef struct {
  BOOLEAN    Active;
  BOOLEAN    Internal;    // EP0 data stage owned by the driver, not reported
  BOOLEAN    SendZlp;     // EP0 IN: terminate a max-packet-multiple reply
  UINT8      *Buffer;
  UINTN      Length;
  UINTN      Done;        // bytes moved through the FIFO so far
  UINTN      Queued;      // EP0 IN: bytes of the packet in flight
} USBFN_XFER;

typedef struct {
  EFI_USBFN_MESSAGE          Message;
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
} USBFN_EVENT;

typedef struct {
  UINT32                    Signature;
  EFI_USBFN_IO_PROTOCOL     UsbFnIo;
  EFI_HANDLE                Handle;
  EFI_HARDWARE_INTERRUPT_PROTOCOL *Interrupt;
  EFI_EVENT                 ExitBootServices;

  BOOLEAN                   Started;
  EFI_USB_BUS_SPEED         Speed;
  UINT8                     Configuration;

  EFI_USB_DEVICE_INFO       *DeviceInfo;
  UINT8                     *ConfigBlob;      // full configuration descriptor
  UINT16                    ConfigBlobLength;

  USBFN_XFER                In[USBFN_MAX_EPS];
  USBFN_XFER                Out[USBFN_MAX_EPS];
  UINT8                     EpType[USBFN_MAX_EPS];   // USB_ENDPOINT_* per number
  UINT16                    EpMps[USBFN_MAX_EPS];

  UINT32                    SetupWords[2];    // SETUP packet from the RX FIFO
  UINT8                     Ep0Buffer[256];   // driver-answered EP0 replies

  USBFN_EVENT               Queue[USBFN_EVENT_QUEUE];
  UINTN                     QueueHead;
  UINTN                     QueueTail;
  UINTN                     QueueDropped;

  UINTN                     IrqCount;         // controller interrupts taken
  UINTN                     PollCount;        // serviced from EventHandler
} USBFN_DEV;

#define USBFN_SIGNATURE    SIGNATURE_32 ('U', 'F', 'N', 'D')
#define USBFN_FROM_THIS(a) CR (a, USBFN_DEV, UsbFnIo, USBFN_SIGNATURE)

//
// Board.c
//
EFI_STATUS
UsbBoardInit (
  VOID
  );

//
// Dwc2.c
//
EFI_STATUS
Dwc2CoreInit (
  VOID
  );

VOID
Dwc2Connect (
  IN BOOLEAN  Connect
  );

VOID
Dwc2BusReset (
  IN USBFN_DEV  *Dev
  );

VOID
Dwc2Ep0ArmOut (
  VOID
  );

VOID
Dwc2SetAddress (
  IN UINT8  Address
  );

VOID
Dwc2ActivateEndpoints (
  IN USBFN_DEV  *Dev
  );

VOID
Dwc2StartIn (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep
  );

VOID
Dwc2StartOut (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep
  );

VOID
Dwc2FillTxFifo (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep
  );

VOID
Dwc2SetStall (
  IN UINT8    Ep,
  IN BOOLEAN  In,
  IN BOOLEAN  Stall
  );

BOOLEAN
Dwc2GetStall (
  IN UINT8    Ep,
  IN BOOLEAN  In
  );

VOID
Dwc2AbortEp (
  IN UINT8    Ep,
  IN BOOLEAN  In
  );

//
// UsbFnDxe.c: interrupt service.
//
VOID
UsbFnQueueEvent (
  IN USBFN_DEV                        *Dev,
  IN EFI_USBFN_MESSAGE                Message,
  IN CONST EFI_USBFN_MESSAGE_PAYLOAD  *Payload OPTIONAL
  );

VOID
UsbFnServiceInterrupt (
  IN USBFN_DEV  *Dev
  );

#endif // __USBFN_DXE_H__
