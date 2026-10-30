/** @file
  USB log + debug channel (EFI_USBFN_IO_PROTOCOL client).

  Vendor device 1209:0001 with two interfaces, so a log reader and a debug
  tool can hold them at the same time:

  - Interface 0, bulk IN 0x81: the boot log ring (ExynosLogRing.h), every
    DEBUG() byte since PrePi entry, then new output as it appears.  The read
    position survives bus resets (up to one ring of history).
  - Interface 1, bulk OUT 0x02 / IN 0x82: commands (USBDBG_*): ping, memory
    read, 32-bit write, reset into normal / download / recovery mode.
    Host side: Tools/usbdbg.py.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/Usb.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/UsbFunctionIo.h>

#include <ExynosLogRing.h>

#define USBLOG_VID         0x1209
#define USBLOG_PID         0x0001
#define USBLOG_CHUNK       4096
#define USBLOG_POLL_100NS  (10 * 10000)   // 10 ms

//
// Debug channel wire format (little endian).  Request: one 16-byte header.
// Reply: a 16-byte header, then Length bytes of data (USBDBG_READ only).
//
#define USBDBG_REQ_MAGIC   SIGNATURE_32 ('U', 'D', 'B', 'G')
#define USBDBG_RSP_MAGIC   SIGNATURE_32 ('U', 'D', 'B', 'R')
#define USBDBG_MAX_READ    4096

#define USBDBG_PING        1   // -> "PONG"
#define USBDBG_READ        2   // Arg0 address, Arg1 length (<= 4096)
#define USBDBG_WRITE32     3   // Arg0 address, Arg1 value
#define USBDBG_RESET       4   // Arg0: 0 normal, 1 download, 4 recovery

#define USBDBG_OK          0
#define USBDBG_EBADCMD     1
#define USBDBG_EBADARG     2

typedef struct {
  UINT32  Magic;
  UINT32  Command;
  UINT32  Arg0;
  UINT32  Arg1;
} USBDBG_HEADER;

typedef struct {
  UINT32  Magic;
  UINT32  Status;
  UINT32  Length;
  UINT32  Reserved;
} USBDBG_REPLY;

STATIC EFI_USB_DEVICE_DESCRIPTOR  mDevice = {
  sizeof (EFI_USB_DEVICE_DESCRIPTOR), USB_DESC_TYPE_DEVICE,
  0x0200, 0xFF, 0x00, 0x00, 64,
  USBLOG_VID, USBLOG_PID, 0x0100,
  1, 2, 3, 1
};

STATIC EFI_USB_CONFIG_DESCRIPTOR  mConfig = {
  sizeof (EFI_USB_CONFIG_DESCRIPTOR), USB_DESC_TYPE_CONFIG,
  0, 2, 1, 0, 0x80, 250
};

STATIC EFI_USB_INTERFACE_DESCRIPTOR  mInterface = {
  sizeof (EFI_USB_INTERFACE_DESCRIPTOR), USB_DESC_TYPE_INTERFACE,
  0, 0, 1, 0xFF, 0x00, 0x00, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  mEpIn = {
  sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), USB_DESC_TYPE_ENDPOINT,
  0x81, USB_ENDPOINT_BULK, 512, 0
};

STATIC EFI_USB_INTERFACE_DESCRIPTOR  mDbgInterface = {
  sizeof (EFI_USB_INTERFACE_DESCRIPTOR), USB_DESC_TYPE_INTERFACE,
  1, 0, 2, 0xFF, 0x00, 0x00, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  mDbgEpOut = {
  sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), USB_DESC_TYPE_ENDPOINT,
  0x02, USB_ENDPOINT_BULK, 512, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  mDbgEpIn = {
  sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), USB_DESC_TYPE_ENDPOINT,
  0x82, USB_ENDPOINT_BULK, 512, 0
};

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  *mEndpoints[]    = { &mEpIn };
STATIC EFI_USB_ENDPOINT_DESCRIPTOR  *mDbgEndpoints[] = { &mDbgEpOut, &mDbgEpIn };
STATIC EFI_USB_INTERFACE_INFO       mIntfInfo        = { &mInterface, mEndpoints };
STATIC EFI_USB_INTERFACE_INFO       mDbgIntfInfo     = { &mDbgInterface, mDbgEndpoints };
STATIC EFI_USB_INTERFACE_INFO       *mIntfTable[]    = { &mIntfInfo, &mDbgIntfInfo };
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

//
// Debug channel state.
//
#define DBG_EP  2
STATIC BOOLEAN                mDbgRxPosted;
STATIC BOOLEAN                mDbgTxBusy;
STATIC UINT8                  mDbgRx[512];
STATIC UINT8                  mDbgTx[sizeof (USBDBG_REPLY) + USBDBG_MAX_READ];
STATIC INT32                  mPendingReset = -1;   // reboot mode once the reply is out

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

/**
  Keep one command receive armed on EP 0x02 (EFI_NOT_READY until the host
  configured the device: retried on the next poll).
**/
STATIC
VOID
DbgPostRx (
  VOID
  )
{
  UINTN  Size;

  if (mDbgRxPosted) {
    return;
  }
  Size = sizeof (mDbgRx);
  if (!EFI_ERROR (mIo->Transfer (mIo, DBG_EP, EfiUsbEndpointDirectionDeviceRx,
                                 &Size, mDbgRx))) {
    mDbgRxPosted = TRUE;
  }
}

/**
  Reboot now; the bootloader picks the mode up from PMU INFORM3
  (ExynosResetLib).
**/
STATIC
VOID
DbgReset (
  IN UINT32  Mode
  )
{
  CONST CHAR16  *Name;

  Name = (Mode == 1) ? L"download" : (Mode == 4) ? L"recovery" : NULL;
  DEBUG ((DEBUG_INFO, "UsbDbg: reset (%s)\n", (Name != NULL) ? Name : L"normal"));
  if (Name != NULL) {
    gRT->ResetSystem (EfiResetPlatformSpecific, EFI_SUCCESS, StrSize (Name), (VOID *)Name);
  }
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}

/**
  Execute one request from mDbgRx and send its reply on EP 0x82.
**/
STATIC
VOID
DbgHandle (
  IN UINTN  Length
  )
{
  USBDBG_HEADER  Req;
  USBDBG_REPLY   *Rsp;
  UINT8          *Data;
  UINT32         Index;
  UINTN          Size;

  Rsp  = (USBDBG_REPLY *)mDbgTx;
  Data = mDbgTx + sizeof (USBDBG_REPLY);
  ZeroMem (Rsp, sizeof (*Rsp));
  Rsp->Magic  = USBDBG_RSP_MAGIC;
  Rsp->Status = USBDBG_OK;

  CopyMem (&Req, mDbgRx, MIN (Length, sizeof (Req)));
  if ((Length < sizeof (Req)) || (Req.Magic != USBDBG_REQ_MAGIC)) {
    Rsp->Status = USBDBG_EBADCMD;
  } else {
    switch (Req.Command) {
      case USBDBG_PING:
        CopyMem (Data, "PONG", 4);
        Rsp->Length = 4;
        break;

      case USBDBG_READ:
        //
        // Aligned reads go word by word so MMIO registers see 32-bit
        // accesses.  An unmapped address aborts like any other bad access.
        //
        if ((Req.Arg1 == 0) || (Req.Arg1 > USBDBG_MAX_READ)) {
          Rsp->Status = USBDBG_EBADARG;
          break;
        }
        if (((Req.Arg0 | Req.Arg1) & 3) == 0) {
          for (Index = 0; Index < Req.Arg1; Index += 4) {
            *(UINT32 *)(Data + Index) = MmioRead32 (Req.Arg0 + Index);
          }
        } else {
          for (Index = 0; Index < Req.Arg1; Index++) {
            Data[Index] = MmioRead8 (Req.Arg0 + Index);
          }
        }
        Rsp->Length = Req.Arg1;
        break;

      case USBDBG_WRITE32:
        if ((Req.Arg0 & 3) != 0) {
          Rsp->Status = USBDBG_EBADARG;
          break;
        }
        MmioWrite32 (Req.Arg0, Req.Arg1);
        break;

      case USBDBG_RESET:
        mPendingReset = (INT32)Req.Arg0;   // after the reply went out
        break;

      default:
        Rsp->Status = USBDBG_EBADCMD;
        break;
    }
  }

  Size = sizeof (USBDBG_REPLY) + Rsp->Length;
  if (!EFI_ERROR (mIo->Transfer (mIo, DBG_EP, EfiUsbEndpointDirectionDeviceTx,
                                 &Size, mDbgTx))) {
    mDbgTxBusy = TRUE;
  } else if (mPendingReset >= 0) {
    DbgReset ((UINT32)mPendingReset);
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
        mTxBusy      = FALSE;
        mDbgTxBusy   = FALSE;
        mDbgRxPosted = FALSE;
        break;

      case EfiUsbMsgSetupPacket:
        UsbLogSetup (&Payload.udr);
        break;

      case EfiUsbMsgEndpointStatusChangedTx:
        if (Payload.utr.EndpointIndex == 1) {
          mReadPos = mTxEnd;
          mTxBusy  = FALSE;
        } else if (Payload.utr.EndpointIndex == DBG_EP) {
          mDbgTxBusy = FALSE;
          if (mPendingReset >= 0) {
            DbgReset ((UINT32)mPendingReset);
          }
        }
        break;

      case EfiUsbMsgEndpointStatusChangedRx:
        if (Payload.utr.EndpointIndex == DBG_EP) {
          mDbgRxPosted = FALSE;
          if (!mDbgTxBusy) {
            DbgHandle (Payload.utr.BytesTransferred);
          }
        }
        break;

      default:
        break;
    }
  }

  DbgPostRx ();
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
