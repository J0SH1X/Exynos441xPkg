/** @file
  Stream the boot log ring (ExynosLogRing.h) to a USB host.

  An EFI_USBFN_IO_PROTOCOL client: presents a vendor-class device with one
  bulk IN endpoint (0x81) and sends every DEBUG() byte since PrePi entry,
  then new output as it appears.  Host side: read EP 0x81 in a loop.  The
  read position survives bus resets, so a host that reconnects continues
  where it left off (up to one ring of history).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/Usb.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/UsbFunctionIo.h>

#include <ExynosLogRing.h>

#define USBLOG_VID         0x1209
#define USBLOG_PID         0x0001
#define USBLOG_CHUNK       4096
#define USBLOG_POLL_100NS  (10 * 10000)   // 10 ms

STATIC EFI_USB_DEVICE_DESCRIPTOR  mDevice = {
  sizeof (EFI_USB_DEVICE_DESCRIPTOR), USB_DESC_TYPE_DEVICE,
  0x0200, 0xFF, 0x00, 0x00, 64,
  USBLOG_VID, USBLOG_PID, 0x0100,
  1, 2, 3, 1
};

STATIC EFI_USB_CONFIG_DESCRIPTOR  mConfig = {
  sizeof (EFI_USB_CONFIG_DESCRIPTOR), USB_DESC_TYPE_CONFIG,
  0, 1, 1, 0, 0x80, 250
};

STATIC EFI_USB_INTERFACE_DESCRIPTOR  mInterface = {
  sizeof (EFI_USB_INTERFACE_DESCRIPTOR), USB_DESC_TYPE_INTERFACE,
  0, 0, 1, 0xFF, 0x00, 0x00, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  mEpIn = {
  sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), USB_DESC_TYPE_ENDPOINT,
  0x81, USB_ENDPOINT_BULK, 512, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  *mEndpoints[]   = { &mEpIn };
STATIC EFI_USB_INTERFACE_INFO       mIntfInfo       = { &mInterface, mEndpoints };
STATIC EFI_USB_INTERFACE_INFO       *mIntfTable[]   = { &mIntfInfo };
STATIC EFI_USB_CONFIG_INFO          mConfigInfo     = { &mConfig, mIntfTable };
STATIC EFI_USB_CONFIG_INFO          *mConfigTable[] = { &mConfigInfo };
STATIC EFI_USB_DEVICE_INFO          mDeviceInfo     = { &mDevice, mConfigTable };

STATIC EFI_USBFN_IO_PROTOCOL  *mIo;
STATIC EFI_EVENT              mPoll;
STATIC BOOLEAN                mTxBusy;
STATIC UINT32                 mReadPos;         // ring position already sent
STATIC UINT32                 mTxEnd;           // ring position after mTx
STATIC UINT8                  mTx[USBLOG_CHUNK];
STATIC UINT8                  mEp0[64];

/**
  GET_DESCRIPTOR(STRING) is the client's job; anything else unknown stalls.
**/
STATIC
VOID
UsbLogSetup (
  IN EFI_USB_DEVICE_REQUEST  *Req
  )
{
  CONST CHAR16  *Str;
  UINTN         Size;
  UINTN         Index;
  BOOLEAN       Stall;

  if ((Req->Request != USB_REQ_GET_DESCRIPTOR) ||
      ((Req->Value >> 8) != USB_DESC_TYPE_STRING)) {
    Stall = TRUE;
    mIo->SetEndpointStallState (mIo, 0, EfiUsbEndpointDirectionDeviceTx, &Stall);
    return;
  }

  switch (Req->Value & 0xFF) {
    case 0:  Str = NULL;                 break;   // language ids
    case 1:  Str = L"Samsung";           break;
    case 2:  Str = L"Galaxy Note UEFI log"; break;
    default: Str = L"GT-N7000";          break;
  }

  mEp0[1] = USB_DESC_TYPE_STRING;
  if (Str == NULL) {
    mEp0[2] = 0x09;   // en-US
    mEp0[3] = 0x04;
    Size    = 4;
  } else {
    Size = 2;
    for (Index = 0; (Str[Index] != 0) && (Size + 2 <= sizeof (mEp0)); Index++) {
      mEp0[Size++] = (UINT8)Str[Index];
      mEp0[Size++] = (UINT8)(Str[Index] >> 8);
    }
  }
  mEp0[0] = (UINT8)Size;

  Size = MIN (Size, (UINTN)Req->Length);
  mIo->Transfer (mIo, 0, EfiUsbEndpointDirectionDeviceTx, &Size, mEp0);
}

/**
  Queue the next piece of unsent log, if any.
**/
STATIC
VOID
UsbLogKick (
  VOID
  )
{
  UINT32  Head;
  UINT32  Pending;
  UINT32  Index;
  UINTN   Size;

  if (mTxBusy) {
    return;
  }

  Head = LOG_RING->Head;
  if (Head - mReadPos > LOG_RING_DATA_SIZE) {
    mReadPos = Head - LOG_RING_DATA_SIZE;   // fell a whole ring behind
  }

  Pending = MIN (Head - mReadPos, (UINT32)USBLOG_CHUNK);
  if (Pending == 0) {
    return;
  }

  //
  // Copy out of the ring: it keeps moving while the transfer is in flight.
  // Stop one short of a full 512-byte multiple so every transfer ends in a
  // short packet and the host read returns without needing a ZLP.
  //
  if ((Pending % 512) == 0) {
    Pending--;
  }
  for (Index = 0; Index < Pending; Index++) {
    mTx[Index] = LOG_RING->Data[(mReadPos + Index) % LOG_RING_DATA_SIZE];
  }

  //
  // EFI_NOT_READY until the host has configured the device: retried on the
  // next poll.
  //
  Size = Pending;
  if (!EFI_ERROR (mIo->Transfer (mIo, 1, EfiUsbEndpointDirectionDeviceTx, &Size, mTx))) {
    mTxEnd  = mReadPos + Pending;
    mTxBusy = TRUE;
  }
}

STATIC
VOID
EFIAPI
UsbLogPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_USBFN_MESSAGE          Msg;
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
  UINTN                      Size;

  for ( ; ; ) {
    Size = sizeof (Payload);
    if (EFI_ERROR (mIo->EventHandler (mIo, &Msg, &Size, &Payload)) ||
        (Msg == EfiUsbMsgNone)) {
      break;
    }

    switch (Msg) {
      case EfiUsbMsgBusEventReset:
        //
        // The driver dropped any transfer in flight: resend that piece.
        //
        mTxBusy = FALSE;
        break;

      case EfiUsbMsgSetupPacket:
        UsbLogSetup (&Payload.udr);
        break;

      case EfiUsbMsgEndpointStatusChangedTx:
        if (Payload.utr.EndpointIndex == 1) {
          mReadPos = mTxEnd;
          mTxBusy  = FALSE;
        }
        break;

      default:
        break;
    }
  }

  UsbLogKick ();
}

STATIC
VOID
EFIAPI
UsbLogStart (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  if (mIo != NULL) {
    return;
  }

  Status = gBS->LocateProtocol (&gEfiUsbFunctionIoProtocolGuid, NULL, (VOID **)&mIo);
  if (EFI_ERROR (Status)) {
    mIo = NULL;
    return;
  }

  Status = mIo->ConfigureEnableEndpoints (mIo, &mDeviceInfo);
  if (!EFI_ERROR (Status)) {
    Status = mIo->StartController (mIo);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbLog: start failed - %r\n", Status));
    return;
  }

  gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, UsbLogPoll, NULL, &mPoll);
  gBS->SetTimer (mPoll, TimerPeriodic, USBLOG_POLL_100NS);
  DEBUG ((DEBUG_INFO, "UsbLog: streaming the boot log on %04x:%04x EP 0x81\n",
          USBLOG_VID, USBLOG_PID));
}

EFI_STATUS
EFIAPI
UsbLogDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UsbLogStart (NULL, NULL);
  return EFI_SUCCESS;
}
