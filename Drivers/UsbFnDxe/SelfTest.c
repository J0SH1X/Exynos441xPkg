/** @file
  Bring-up self test for UsbFnDxe (USBFN_SELFTEST): acts as its own
  EFI_USBFN_IO_PROTOCOL client.

  Presents a vendor-class device (USBFN_VID:USBFN_PID) with one bulk OUT
  (0x01) and one bulk IN (0x81) endpoint and echoes every OUT transfer back
  on IN, so enumeration and both data directions can be checked from a host
  (lsusb, then any bulk loopback tool) before a real class driver exists.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbFnDxe.h"

#if USBFN_SELFTEST

#define ST_BUFFER_SIZE  4096

STATIC EFI_USB_DEVICE_DESCRIPTOR  mStDevice = {
  sizeof (EFI_USB_DEVICE_DESCRIPTOR), USB_DESC_TYPE_DEVICE,
  0x0200, 0xFF, 0x00, 0x00, USBFN_EP0_MPS,
  USBFN_VID, USBFN_PID, 0x0100,
  1, 2, 3, 1
};

STATIC EFI_USB_CONFIG_DESCRIPTOR  mStConfig = {
  sizeof (EFI_USB_CONFIG_DESCRIPTOR), USB_DESC_TYPE_CONFIG,
  0,                // TotalLength: filled in by ConfigureEnableEndpoints
  1, 1, 0, 0x80, 250
};

STATIC EFI_USB_INTERFACE_DESCRIPTOR  mStInterface = {
  sizeof (EFI_USB_INTERFACE_DESCRIPTOR), USB_DESC_TYPE_INTERFACE,
  0, 0, 2, 0xFF, 0x00, 0x00, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  mStEpIn = {
  sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), USB_DESC_TYPE_ENDPOINT,
  0x81, USB_ENDPOINT_BULK, 512, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  mStEpOut = {
  sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), USB_DESC_TYPE_ENDPOINT,
  0x01, USB_ENDPOINT_BULK, 512, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  *mStEndpoints[] = { &mStEpIn, &mStEpOut };
STATIC EFI_USB_INTERFACE_INFO       mStIntfInfo     = { &mStInterface, mStEndpoints };
STATIC EFI_USB_INTERFACE_INFO       *mStIntfTable[] = { &mStIntfInfo };
STATIC EFI_USB_CONFIG_INFO          mStConfigInfo   = { &mStConfig, mStIntfTable };
STATIC EFI_USB_CONFIG_INFO          *mStConfigTable[] = { &mStConfigInfo };
STATIC EFI_USB_DEVICE_INFO          mStDeviceInfo   = { &mStDevice, mStConfigTable };

STATIC EFI_USBFN_IO_PROTOCOL  *mStIo;
STATIC EFI_EVENT              mStTimer;
STATIC BOOLEAN                mStRxPosted;
STATIC UINT8                  mStConfigValue; // last configuration seen
STATIC UINTN                  mStEchoed;
STATIC UINT8                  mStRx[ST_BUFFER_SIZE];
STATIC UINT8                  mStTx[ST_BUFFER_SIZE];
STATIC UINT8                  mStEp0[64];

UINT8
UsbFnCurrentConfiguration (
  VOID
  );

VOID
UsbFnServiceCounts (
  OUT UINTN  *Irq,
  OUT UINTN  *Poll
  );

STATIC
VOID
StPostRx (
  VOID
  )
{
  UINTN  Size;

  Size = sizeof (mStRx);
  if (!EFI_ERROR (mStIo->Transfer (mStIo, 1, EfiUsbEndpointDirectionDeviceRx,
                                   &Size, mStRx))) {
    mStRxPosted = TRUE;
  }
}

/**
  String descriptors are the class driver's job; answer them here.
**/
STATIC
VOID
StSetup (
  IN EFI_USB_DEVICE_REQUEST  *Req
  )
{
  CONST CHAR16  *Str;
  UINTN         Size;
  UINTN         Index;
  BOOLEAN       Stall;

  if ((Req->Request != USB_REQ_GET_DESCRIPTOR) ||
      ((Req->Value >> 8) != USB_DESC_TYPE_STRING)) {
    DEBUG ((DEBUG_INFO, "UsbFn test: unhandled SETUP %02x %02x %04x, stall\n",
            Req->RequestType, Req->Request, Req->Value));
    Stall = TRUE;
    mStIo->SetEndpointStallState (mStIo, 0, EfiUsbEndpointDirectionDeviceTx, &Stall);
    return;
  }

  switch (Req->Value & 0xFF) {
    case 0:  Str = NULL;                 break;   // language ids
    case 1:  Str = L"Samsung";           break;
    case 2:  Str = L"Galaxy Note UsbFn"; break;
    default: Str = L"GT-N7000";          break;
  }

  mStEp0[1] = USB_DESC_TYPE_STRING;
  if (Str == NULL) {
    mStEp0[2] = 0x09;   // en-US
    mStEp0[3] = 0x04;
    Size      = 4;
  } else {
    Size = 2;
    for (Index = 0; (Str[Index] != 0) && (Size + 2 <= sizeof (mStEp0)); Index++) {
      mStEp0[Size++] = (UINT8)Str[Index];
      mStEp0[Size++] = (UINT8)(Str[Index] >> 8);
    }
  }
  mStEp0[0] = (UINT8)Size;

  Size = MIN (Size, (UINTN)Req->Length);
  mStIo->Transfer (mStIo, 0, EfiUsbEndpointDirectionDeviceTx, &Size, mStEp0);
}

STATIC
VOID
EFIAPI
StTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_USBFN_MESSAGE          Msg;
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
  UINTN                      Size;
  UINTN                      TxSize;
  UINTN                      Irq;
  UINTN                      Poll;

  for ( ; ; ) {
    Size = sizeof (Payload);
    if (EFI_ERROR (mStIo->EventHandler (mStIo, &Msg, &Size, &Payload)) ||
        (Msg == EfiUsbMsgNone)) {
      break;
    }

    switch (Msg) {
      case EfiUsbMsgBusEventReset:
        mStRxPosted = FALSE;
        DEBUG ((DEBUG_INFO, "UsbFn test: bus reset\n"));
        break;

      case EfiUsbMsgBusEventSpeed:
        DEBUG ((DEBUG_INFO, "UsbFn test: %a speed\n",
                (Payload.ubs == UsbBusSpeedHigh) ? "high" : "full"));
        break;

      case EfiUsbMsgSetupPacket:
        StSetup (&Payload.udr);
        break;

      case EfiUsbMsgEndpointStatusChangedRx:
        if (Payload.utr.EndpointIndex == 1) {
          mStRxPosted = FALSE;
          TxSize      = Payload.utr.BytesTransferred;
          CopyMem (mStTx, mStRx, TxSize);
          mStIo->Transfer (mStIo, 1, EfiUsbEndpointDirectionDeviceTx, &TxSize, mStTx);
        }
        break;

      case EfiUsbMsgEndpointStatusChangedTx:
        if (Payload.utr.EndpointIndex == 1) {
          if ((++mStEchoed % 64) == 1) {
            UsbFnServiceCounts (&Irq, &Poll);
            DEBUG ((DEBUG_INFO, "UsbFn test: echoed %u transfers (irq %u, poll %u)\n",
                    mStEchoed, Irq, Poll));
          }
        }
        break;

      default:
        break;
    }
  }

  if (UsbFnCurrentConfiguration () != mStConfigValue) {
    mStConfigValue = UsbFnCurrentConfiguration ();
    UsbFnServiceCounts (&Irq, &Poll);
    DEBUG ((DEBUG_INFO, "UsbFn test: configuration %u, loopback on EP1 (irq %u, poll %u)\n",
            mStConfigValue, Irq, Poll));
  }

  //
  // Keep one receive armed whenever configured; it may overlap the echo of
  // the previous one (the IN and OUT sides are independent).
  //
  if (!mStRxPosted && (mStConfigValue != 0)) {
    StPostRx ();
  }
}

EFI_STATUS
UsbFnSelfTestStart (
  IN EFI_USBFN_IO_PROTOCOL  *UsbFnIo
  )
{
  EFI_STATUS  Status;

  mStIo = UsbFnIo;

  Status = mStIo->ConfigureEnableEndpoints (mStIo, &mStDeviceInfo);
  if (!EFI_ERROR (Status)) {
    Status = mStIo->StartController (mStIo);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbFn test: start failed - %r\n", Status));
    return Status;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             StTick, NULL, &mStTimer);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mStTimer, TimerPeriodic, 10000);   // 1 ms
  }

  DEBUG ((DEBUG_INFO, "UsbFn test: started, waiting for a host (%04x:%04x)\n",
          USBFN_VID, USBFN_PID));
  return Status;
}

#endif // USBFN_SELFTEST
