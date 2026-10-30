/** @file
  EFI_USBFN_IO_PROTOCOL for the Exynos4210 DWC2 USB device controller.

  The controller interrupt (GIC 103) drains the RX FIFO, refills TX FIFOs,
  answers the EP0 standard requests the driver owns (SET_ADDRESS,
  GET_DESCRIPTOR device/config, SET/GET_CONFIGURATION, GET_STATUS,
  CLEAR/SET_FEATURE) and queues everything the class driver must see.
  EventHandler() dequeues; it also services the controller itself if an
  event is pending, so a lost interrupt cannot wedge a polling client.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbFnDxe.h"

#define REG(x)  (DWC2_BASE + (x))

STATIC USBFN_DEV  *mDev;

//
// ---------------------------------------------------------------------------
// Event queue (filled at TPL_HIGH_LEVEL by the interrupt handler).
// ---------------------------------------------------------------------------
//

VOID
UsbFnQueueEvent (
  IN USBFN_DEV                        *Dev,
  IN EFI_USBFN_MESSAGE                Message,
  IN CONST EFI_USBFN_MESSAGE_PAYLOAD  *Payload OPTIONAL
  )
{
  UINTN  Next;

  Next = (Dev->QueueHead + 1) % USBFN_EVENT_QUEUE;
  if (Next == Dev->QueueTail) {
    Dev->QueueDropped++;
    return;
  }

  Dev->Queue[Dev->QueueHead].Message = Message;
  if (Payload != NULL) {
    CopyMem (&Dev->Queue[Dev->QueueHead].Payload, Payload, sizeof (*Payload));
  } else {
    ZeroMem (&Dev->Queue[Dev->QueueHead].Payload, sizeof (*Payload));
  }
  Dev->QueueHead = Next;
}

STATIC
VOID
UsbFnQueueTransfer (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep,
  IN BOOLEAN    In
  )
{
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
  USBFN_XFER                 *X;

  X = In ? &Dev->In[Ep] : &Dev->Out[Ep];
  ZeroMem (&Payload, sizeof (Payload));
  Payload.utr.BytesTransferred = X->Done;
  Payload.utr.TransferStatus   = UsbTransferStatusComplete;
  Payload.utr.EndpointIndex    = Ep;
  Payload.utr.Direction        = In ? EfiUsbEndpointDirectionDeviceTx
                                    : EfiUsbEndpointDirectionDeviceRx;
  Payload.utr.Buffer           = X->Buffer;

  UsbFnQueueEvent (Dev, In ? EfiUsbMsgEndpointStatusChangedTx
                           : EfiUsbMsgEndpointStatusChangedRx, &Payload);
}

//
// ---------------------------------------------------------------------------
// EP0: driver-owned standard requests.
// ---------------------------------------------------------------------------
//

STATIC
VOID
Ep0Reply (
  IN USBFN_DEV   *Dev,
  IN CONST VOID  *Data,
  IN UINTN       Length,
  IN UINT16      WLength
  )
{
  USBFN_XFER  *X;

  Length = MIN (MIN (Length, (UINTN)WLength), sizeof (Dev->Ep0Buffer));
  CopyMem (Dev->Ep0Buffer, Data, Length);

  X = &Dev->In[0];
  ZeroMem (X, sizeof (*X));
  X->Active   = TRUE;
  X->Internal = TRUE;
  X->Buffer   = Dev->Ep0Buffer;
  X->Length   = Length;
  X->SendZlp  = (Length < WLength) && (Length != 0) &&
                ((Length % USBFN_EP0_MPS) == 0);
  Dwc2StartIn (Dev, 0);
}

STATIC
VOID
Ep0Status (
  IN USBFN_DEV  *Dev
  )
{
  Ep0Reply (Dev, NULL, 0, 0);
}

STATIC
VOID
Ep0Stall (
  VOID
  )
{
  //
  // The core clears an EP0 stall by itself on the next SETUP.
  //
  Dwc2SetStall (0, TRUE, TRUE);
  Dwc2SetStall (0, FALSE, TRUE);
}

/**
  @retval TRUE   Request answered here.
  @retval FALSE  Hand it to the class driver.
**/
STATIC
BOOLEAN
Ep0Standard (
  IN USBFN_DEV                     *Dev,
  IN CONST EFI_USB_DEVICE_REQUEST  *Req
  )
{
  UINT8   Recipient;
  UINT8   Ep;
  UINT16  Status;

  if ((Req->RequestType & 0x60) != USB_REQ_TYPE_STANDARD) {
    return FALSE;
  }
  Recipient = Req->RequestType & 0x1F;

  switch (Req->Request) {
    case USB_REQ_SET_ADDRESS:
      Dwc2SetAddress ((UINT8)(Req->Value & 0x7F));
      Ep0Status (Dev);
      return TRUE;

    case USB_REQ_GET_DESCRIPTOR:
      switch (Req->Value >> 8) {
        case USB_DESC_TYPE_DEVICE:
          if (Dev->DeviceInfo == NULL) {
            return FALSE;
          }
          Ep0Reply (Dev, Dev->DeviceInfo->DeviceDescriptor,
                    Dev->DeviceInfo->DeviceDescriptor->Length, Req->Length);
          return TRUE;
        case USB_DESC_TYPE_CONFIG:
          if (Dev->ConfigBlob == NULL) {
            return FALSE;
          }
          Ep0Reply (Dev, Dev->ConfigBlob, Dev->ConfigBlobLength, Req->Length);
          return TRUE;
        case 0x06:  // DEVICE_QUALIFIER: not dual-speed capable
        case 0x07:  // OTHER_SPEED_CONFIGURATION
          Ep0Stall ();
          return TRUE;
        default:
          return FALSE;   // strings and class descriptors: class driver
      }

    case USB_REQ_SET_CONFIG:
      Dev->Configuration = (UINT8)Req->Value;
      if (Dev->Configuration != 0) {
        Dwc2ActivateEndpoints (Dev);
      }
      Ep0Status (Dev);
      return TRUE;

    case USB_REQ_GET_CONFIG:
      Ep0Reply (Dev, &Dev->Configuration, 1, Req->Length);
      return TRUE;

    case USB_REQ_GET_STATUS:
      Status = 0;
      if (Recipient == USB_TARGET_ENDPOINT) {
        Ep     = Req->Index & 0xF;
        Status = (Ep < USBFN_MAX_EPS) &&
                 Dwc2GetStall (Ep, (Req->Index & 0x80) != 0) ? 1 : 0;
      }
      Ep0Reply (Dev, &Status, sizeof (Status), Req->Length);
      return TRUE;

    case USB_REQ_CLEAR_FEATURE:
    case USB_REQ_SET_FEATURE:
      if ((Recipient == USB_TARGET_ENDPOINT) && (Req->Value == 0)) {
        Ep = Req->Index & 0xF;
        if (Ep < USBFN_MAX_EPS) {
          Dwc2SetStall (Ep, (Req->Index & 0x80) != 0,
                        Req->Request == USB_REQ_SET_FEATURE);
        }
      }
      Ep0Status (Dev);
      return TRUE;

    default:
      return FALSE;
  }
}

STATIC
VOID
Ep0Setup (
  IN USBFN_DEV  *Dev
  )
{
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;

  //
  // A new SETUP cancels whatever EP0 was doing.
  //
  ZeroMem (&Dev->In[0], sizeof (Dev->In[0]));
  ZeroMem (&Dev->Out[0], sizeof (Dev->Out[0]));
  CopyMem (&Payload.udr, Dev->SetupWords, sizeof (Payload.udr));

  if (!Ep0Standard (Dev, &Payload.udr)) {
    UsbFnQueueEvent (Dev, EfiUsbMsgSetupPacket, &Payload);
  }

  Dwc2Ep0ArmOut ();
}

//
// ---------------------------------------------------------------------------
// Interrupt service.
// ---------------------------------------------------------------------------
//

STATIC
VOID
RxFifoPop (
  IN USBFN_DEV  *Dev
  )
{
  UINT32      Sts;
  UINT8       Ep;
  UINTN       Count;
  UINTN       Index;
  UINT32      Word;
  USBFN_XFER  *X;

  Sts   = MmioRead32 (REG (GRXSTSP));
  Ep    = (UINT8)GRXSTS_EPNUM (Sts);
  Count = GRXSTS_BCNT (Sts);

  switch (GRXSTS_PKTSTS (Sts)) {
    case PKTSTS_SETUP_DATA:
      Dev->SetupWords[0] = MmioRead32 (REG (DFIFO (0)));
      Dev->SetupWords[1] = MmioRead32 (REG (DFIFO (0)));
      break;

    case PKTSTS_OUT_DATA:
      X = (Ep < USBFN_MAX_EPS) ? &Dev->Out[Ep] : NULL;
      for (Index = 0; Index < Count; Index += 4) {
        Word = MmioRead32 (REG (DFIFO (0)));
        if ((X != NULL) && X->Active && (X->Done < X->Length)) {
          CopyMem (X->Buffer + X->Done, &Word,
                   MIN (MIN (Count - Index, (UINTN)4), X->Length - X->Done));
          X->Done += MIN (MIN (Count - Index, (UINTN)4), X->Length - X->Done);
        }
      }
      if (X != NULL) {
        X->Queued = Count;   // size of the last OUT packet
      }
      break;

    default:
      break;  // OUT/SETUP complete markers: handled via DOEPINT
  }
}

STATIC
VOID
OutComplete (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep
  )
{
  USBFN_XFER  *X;

  X = &Dev->Out[Ep];
  if (Ep == 0) {
    //
    // EP0 OUT data stage: keep going while full packets arrive and the
    // class driver asked for more; status-stage ZLPs with no transfer
    // pending are just consumed.
    //
    if (X->Active &&
        ((X->Done >= X->Length) || (X->Queued < USBFN_EP0_MPS))) {
      X->Active = FALSE;
      UsbFnQueueTransfer (Dev, 0, FALSE);
    }
    Dwc2Ep0ArmOut ();
    return;
  }

  if (X->Active) {
    X->Active = FALSE;
    UsbFnQueueTransfer (Dev, Ep, FALSE);
  }
}

STATIC
VOID
InComplete (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep
  )
{
  USBFN_XFER  *X;

  X = &Dev->In[Ep];
  if (!X->Active) {
    return;
  }

  if (Ep == 0) {
    X->Done += X->Queued;
    if (X->Done < X->Length) {
      Dwc2StartIn (Dev, 0);
      return;
    }
    if (X->SendZlp) {
      X->SendZlp = FALSE;
      Dwc2StartIn (Dev, 0);
      return;
    }
  }

  X->Active = FALSE;
  if (!X->Internal) {
    UsbFnQueueTransfer (Dev, Ep, TRUE);
  }
}

VOID
UsbFnServiceInterrupt (
  IN USBFN_DEV  *Dev
  )
{
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
  UINT32                     Sts;
  UINT32                     Daint;
  UINT32                     Int;
  UINT8                      Ep;

  Sts = MmioRead32 (REG (GINTSTS)) & MmioRead32 (REG (GINTMSK));

  if (Sts & GINT_USBRST) {
    MmioWrite32 (REG (GINTSTS), GINT_USBRST);
    Dwc2BusReset (Dev);
    UsbFnQueueEvent (Dev, EfiUsbMsgBusEventReset, NULL);
  }

  if (Sts & GINT_ENUMDONE) {
    MmioWrite32 (REG (GINTSTS), GINT_ENUMDONE);
    Dev->Speed = (DSTS_ENUMSPD (MmioRead32 (REG (DSTS))) == ENUMSPD_HS)
                 ? UsbBusSpeedHigh : UsbBusSpeedFull;
    MmioAnd32 (REG (DIEPCTL (0)), ~DEPCTL_MPS_MASK);   // EP0 MPS 64
    MmioOr32 (REG (DCTL), DCTL_CGNPINNAK);
    ZeroMem (&Payload, sizeof (Payload));
    Payload.ubs = Dev->Speed;
    UsbFnQueueEvent (Dev, EfiUsbMsgBusEventSpeed, &Payload);
  }

  if (Sts & GINT_USBSUSP) {
    MmioWrite32 (REG (GINTSTS), GINT_USBSUSP);
    UsbFnQueueEvent (Dev, EfiUsbMsgBusEventSuspend, NULL);
  }

  if (Sts & GINT_WKUPINT) {
    MmioWrite32 (REG (GINTSTS), GINT_WKUPINT);
    UsbFnQueueEvent (Dev, EfiUsbMsgBusEventResume, NULL);
  }

  //
  // RxFLvl is a level: drain the whole FIFO before looking at the endpoint
  // interrupts, so XferCompl always sees the data that preceded it.
  //
  while (MmioRead32 (REG (GINTSTS)) & GINT_RXFLVL) {
    RxFifoPop (Dev);
  }

  if (Sts & (GINT_OEPINT | GINT_IEPINT)) {
    Daint = MmioRead32 (REG (DAINT)) & MmioRead32 (REG (DAINTMSK));

    for (Ep = 0; Ep < USBFN_MAX_EPS; Ep++) {
      if (Daint & (1U << (16 + Ep))) {
        Int = MmioRead32 (REG (DOEPINT (Ep)));
        MmioWrite32 (REG (DOEPINT (Ep)), Int);
        if ((Ep == 0) && (Int & DOEPINT_SETUP)) {
          Ep0Setup (Dev);
        } else if (Int & DEPINT_XFERCOMPL) {
          OutComplete (Dev, Ep);
        }
      }

      if (Daint & (1U << Ep)) {
        Int = MmioRead32 (REG (DIEPINT (Ep)));
        if (!(MmioRead32 (REG (DIEPEMPMSK)) & (1U << Ep))) {
          Int &= ~DIEPINT_TXFE;
        }
        MmioWrite32 (REG (DIEPINT (Ep)), Int & ~DIEPINT_TXFE);
        if (Int & DIEPINT_TXFE) {
          Dwc2FillTxFifo (Dev, Ep);
        }
        if (Int & DEPINT_XFERCOMPL) {
          InComplete (Dev, Ep);
        }
      }
    }
  }
}

STATIC
VOID
EFIAPI
UsbFnInterruptHandler (
  IN HARDWARE_INTERRUPT_SOURCE  Source,
  IN EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  mDev->IrqCount++;
  UsbFnServiceInterrupt (mDev);
  mDev->Interrupt->EndOfInterrupt (mDev->Interrupt, Source);
}

//
// ---------------------------------------------------------------------------
// EFI_USBFN_IO_PROTOCOL.
// ---------------------------------------------------------------------------
//

STATIC
EFI_STATUS
EFIAPI
UsbFnDetectPort (
  IN  EFI_USBFN_IO_PROTOCOL  *This,
  OUT EFI_USBFN_PORT_TYPE    *PortType
  )
{
  if (PortType == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // No charger detection (the MUIC could do it); report a plain host port.
  //
  *PortType = EfiUsbStandardDownstreamPort;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnConfigureEnableEndpoints (
  IN EFI_USBFN_IO_PROTOCOL  *This,
  IN EFI_USB_DEVICE_INFO    *DeviceInfo
  )
{
  USBFN_DEV                    *Dev;
  EFI_USB_CONFIG_INFO          *Config;
  EFI_USB_INTERFACE_INFO       *Intf;
  EFI_USB_ENDPOINT_DESCRIPTOR  *EpDesc;
  UINTN                        Total;
  UINTN                        I;
  UINTN                        E;
  UINT8                        *Ptr;
  UINT8                        Num;

  Dev = USBFN_FROM_THIS (This);
  if ((DeviceInfo == NULL) || (DeviceInfo->DeviceDescriptor == NULL) ||
      (DeviceInfo->ConfigInfoTable == NULL) ||
      (DeviceInfo->ConfigInfoTable[0] == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Only the first configuration is served.
  //
  Config = DeviceInfo->ConfigInfoTable[0];
  Total  = Config->ConfigDescriptor->Length;
  for (I = 0; I < Config->ConfigDescriptor->NumInterfaces; I++) {
    Intf   = Config->InterfaceInfoTable[I];
    Total += Intf->InterfaceDescriptor->Length;
    for (E = 0; E < Intf->InterfaceDescriptor->NumEndpoints; E++) {
      EpDesc = Intf->EndpointDescriptorTable[E];
      Num    = EpDesc->EndpointAddress & 0xF;
      if ((Num == 0) || (Num >= USBFN_MAX_EPS)) {
        return EFI_UNSUPPORTED;
      }
      Total += EpDesc->Length;
    }
  }

  if (Dev->ConfigBlob != NULL) {
    FreePool (Dev->ConfigBlob);
  }
  Dev->ConfigBlob = AllocatePool (Total);
  if (Dev->ConfigBlob == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (Dev->EpType, sizeof (Dev->EpType));
  ZeroMem (Dev->EpMps, sizeof (Dev->EpMps));

  Ptr = Dev->ConfigBlob;
  CopyMem (Ptr, Config->ConfigDescriptor, Config->ConfigDescriptor->Length);
  Ptr += Config->ConfigDescriptor->Length;
  for (I = 0; I < Config->ConfigDescriptor->NumInterfaces; I++) {
    Intf = Config->InterfaceInfoTable[I];
    CopyMem (Ptr, Intf->InterfaceDescriptor, Intf->InterfaceDescriptor->Length);
    Ptr += Intf->InterfaceDescriptor->Length;
    for (E = 0; E < Intf->InterfaceDescriptor->NumEndpoints; E++) {
      EpDesc = Intf->EndpointDescriptorTable[E];
      CopyMem (Ptr, EpDesc, EpDesc->Length);
      Ptr += EpDesc->Length;

      Num              = EpDesc->EndpointAddress & 0xF;
      Dev->EpType[Num] = EpDesc->Attributes & 0x3;
      Dev->EpMps[Num]  = EpDesc->MaxPacketSize & DEPCTL_MPS_MASK;
    }
  }

  ((EFI_USB_CONFIG_DESCRIPTOR *)Dev->ConfigBlob)->TotalLength = (UINT16)Total;
  Dev->ConfigBlobLength = (UINT16)Total;
  Dev->DeviceInfo       = DeviceInfo;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnGetEndpointMaxPacketSize (
  IN  EFI_USBFN_IO_PROTOCOL  *This,
  IN  EFI_USB_ENDPOINT_TYPE  EndpointType,
  IN  EFI_USB_BUS_SPEED      BusSpeed,
  OUT UINT16                 *MaxPacketSize
  )
{
  if (MaxPacketSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  switch (EndpointType) {
    case UsbEndpointControl:
      *MaxPacketSize = USBFN_EP0_MPS;
      return EFI_SUCCESS;
    case UsbEndpointBulk:
      *MaxPacketSize = (BusSpeed == UsbBusSpeedHigh) ? 512 : 64;
      return EFI_SUCCESS;
    default:
      return EFI_UNSUPPORTED;
  }
}

STATIC
EFI_STATUS
EFIAPI
UsbFnGetDeviceInfo (
  IN     EFI_USBFN_IO_PROTOCOL     *This,
  IN     EFI_USBFN_DEVICE_INFO_ID  Id,
  IN OUT UINTN                     *BufferSize,
  OUT    VOID                      *Buffer OPTIONAL
  )
{
  CONST CHAR16  *Str;
  UINTN         Size;

  if (BufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Id) {
    case EfiUsbDeviceInfoSerialNumber:
      Str = L"GT-N7000";
      break;
    case EfiUsbDeviceInfoManufacturerName:
      Str = L"Samsung";
      break;
    case EfiUsbDeviceInfoProductName:
      Str = L"Galaxy Note";
      break;
    default:
      return EFI_UNSUPPORTED;
  }

  Size = StrSize (Str);
  if ((Buffer == NULL) || (*BufferSize < Size)) {
    *BufferSize = Size;
    return EFI_BUFFER_TOO_SMALL;
  }
  CopyMem (Buffer, Str, Size);
  *BufferSize = Size;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnGetVendorIdProductId (
  IN  EFI_USBFN_IO_PROTOCOL  *This,
  OUT UINT16                 *Vid,
  OUT UINT16                 *Pid
  )
{
  if ((Vid == NULL) || (Pid == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *Vid = USBFN_VID;
  *Pid = USBFN_PID;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnAbortTransfer (
  IN EFI_USBFN_IO_PROTOCOL         *This,
  IN UINT8                         EndpointIndex,
  IN EFI_USBFN_ENDPOINT_DIRECTION  Direction
  )
{
  USBFN_DEV   *Dev;
  BOOLEAN     In;
  EFI_TPL     OldTpl;

  Dev = USBFN_FROM_THIS (This);
  if (EndpointIndex >= USBFN_MAX_EPS) {
    return EFI_INVALID_PARAMETER;
  }

  In     = (Direction == EfiUsbEndpointDirectionDeviceTx);
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (EndpointIndex != 0) {
    Dwc2AbortEp (EndpointIndex, In);
  }
  if (In) {
    Dev->In[EndpointIndex].Active = FALSE;
  } else {
    Dev->Out[EndpointIndex].Active = FALSE;
  }
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnGetEndpointStallState (
  IN  EFI_USBFN_IO_PROTOCOL         *This,
  IN  UINT8                         EndpointIndex,
  IN  EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  OUT BOOLEAN                       *State
  )
{
  if ((EndpointIndex >= USBFN_MAX_EPS) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *State = Dwc2GetStall (EndpointIndex, Direction == EfiUsbEndpointDirectionDeviceTx);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnSetEndpointStallState (
  IN EFI_USBFN_IO_PROTOCOL         *This,
  IN UINT8                         EndpointIndex,
  IN EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN OUT BOOLEAN                   *State
  )
{
  if ((EndpointIndex >= USBFN_MAX_EPS) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (EndpointIndex == 0) {
    if (*State) {
      Ep0Stall ();   // protocol stall of the current control transfer
    }
    return EFI_SUCCESS;
  }

  Dwc2SetStall (EndpointIndex, Direction == EfiUsbEndpointDirectionDeviceTx, *State);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnEventHandler (
  IN     EFI_USBFN_IO_PROTOCOL      *This,
  OUT    EFI_USBFN_MESSAGE          *Message,
  IN OUT UINTN                      *PayloadSize,
  OUT    EFI_USBFN_MESSAGE_PAYLOAD  *Payload
  )
{
  USBFN_DEV  *Dev;
  EFI_TPL    OldTpl;

  Dev = USBFN_FROM_THIS (This);
  if ((Message == NULL) || (PayloadSize == NULL) || (Payload == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  //
  // Interrupts are masked here; service anything pending so a polling
  // client makes progress even if an interrupt was lost.
  //
  if (Dev->Started &&
      (MmioRead32 (REG (GINTSTS)) & MmioRead32 (REG (GINTMSK)))) {
    Dev->PollCount++;
    UsbFnServiceInterrupt (Dev);
  }

  if (Dev->QueueTail == Dev->QueueHead) {
    gBS->RestoreTPL (OldTpl);
    *Message     = EfiUsbMsgNone;
    *PayloadSize = 0;
    return EFI_SUCCESS;
  }

  if (*PayloadSize < sizeof (EFI_USBFN_MESSAGE_PAYLOAD)) {
    gBS->RestoreTPL (OldTpl);
    *PayloadSize = sizeof (EFI_USBFN_MESSAGE_PAYLOAD);
    return EFI_BUFFER_TOO_SMALL;
  }

  *Message = Dev->Queue[Dev->QueueTail].Message;
  CopyMem (Payload, &Dev->Queue[Dev->QueueTail].Payload, sizeof (*Payload));
  *PayloadSize   = sizeof (*Payload);
  Dev->QueueTail = (Dev->QueueTail + 1) % USBFN_EVENT_QUEUE;

  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnTransfer (
  IN     EFI_USBFN_IO_PROTOCOL         *This,
  IN     UINT8                         EndpointIndex,
  IN     EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN OUT UINTN                         *BufferSize,
  IN OUT VOID                          *Buffer
  )
{
  USBFN_DEV   *Dev;
  USBFN_XFER  *X;
  BOOLEAN     In;
  EFI_TPL     OldTpl;

  Dev = USBFN_FROM_THIS (This);
  if ((EndpointIndex >= USBFN_MAX_EPS) || (BufferSize == NULL) ||
      ((*BufferSize != 0) && (Buffer == NULL)) ||
      (*BufferSize > USBFN_MAX_TRANSFER)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((EndpointIndex != 0) && (Dev->EpMps[EndpointIndex] == 0)) {
    return EFI_INVALID_PARAMETER;   // endpoint not in the configuration
  }
  if (!Dev->Started) {
    return EFI_NOT_READY;
  }
  if ((EndpointIndex != 0) && (Dev->Configuration == 0)) {
    return EFI_NOT_READY;   // bulk endpoints exist only once the host configured us
  }

  In     = (Direction == EfiUsbEndpointDirectionDeviceTx);
  X      = In ? &Dev->In[EndpointIndex] : &Dev->Out[EndpointIndex];
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (X->Active) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }

  ZeroMem (X, sizeof (*X));
  X->Active = TRUE;
  X->Buffer = Buffer;
  X->Length = *BufferSize;

  if (In) {
    Dwc2StartIn (Dev, EndpointIndex);
  } else if (EndpointIndex != 0) {
    Dwc2StartOut (Dev, EndpointIndex);
  }
  //
  // EP0 OUT is always armed (Dwc2Ep0ArmOut); the data stage lands in X.
  //

  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnGetMaxTransferSize (
  IN  EFI_USBFN_IO_PROTOCOL  *This,
  OUT UINTN                  *MaxTransferSize
  )
{
  if (MaxTransferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *MaxTransferSize = USBFN_MAX_TRANSFER;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnAllocateTransferBuffer (
  IN  EFI_USBFN_IO_PROTOCOL  *This,
  IN  UINTN                  Size,
  OUT VOID                   **Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // PIO: any memory works.
  //
  *Buffer = AllocatePool (Size);
  return (*Buffer == NULL) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnFreeTransferBuffer (
  IN EFI_USBFN_IO_PROTOCOL  *This,
  IN VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  FreePool (Buffer);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnStartController (
  IN EFI_USBFN_IO_PROTOCOL  *This
  )
{
  USBFN_DEV   *Dev;
  EFI_STATUS  Status;

  Dev = USBFN_FROM_THIS (This);
  if (Dev->Started) {
    return EFI_SUCCESS;
  }

  Status = UsbBoardInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UsbFn: board init failed - %r\n", Status));
    return Status;
  }

  Status = Dwc2CoreInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dev->QueueHead = Dev->QueueTail = 0;
  Dev->Started   = TRUE;
  Dev->Interrupt->EnableInterruptSource (Dev->Interrupt, DWC2_IRQ);
  Dwc2Connect (TRUE);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnStopController (
  IN EFI_USBFN_IO_PROTOCOL  *This
  )
{
  USBFN_DEV  *Dev;

  Dev = USBFN_FROM_THIS (This);
  if (!Dev->Started) {
    return EFI_SUCCESS;
  }

  Dwc2Connect (FALSE);
  Dev->Interrupt->DisableInterruptSource (Dev->Interrupt, DWC2_IRQ);
  MmioWrite32 (REG (GINTMSK), 0);
  Dev->Started = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnSetEndpointPolicy (
  IN EFI_USBFN_IO_PROTOCOL         *This,
  IN UINT8                         EndpointIndex,
  IN EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN EFI_USBFN_POLICY_TYPE         PolicyType,
  IN UINTN                         BufferSize,
  IN VOID                          *Buffer
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
UsbFnGetEndpointPolicy (
  IN     EFI_USBFN_IO_PROTOCOL         *This,
  IN     UINT8                         EndpointIndex,
  IN     EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN     EFI_USBFN_POLICY_TYPE         PolicyType,
  IN OUT UINTN                         *BufferSize,
  IN OUT VOID                          *Buffer
  )
{
  if ((BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  switch (PolicyType) {
    case EfiUsbPolicyMaxTransactionSize:
      if (*BufferSize < sizeof (UINT32)) {
        *BufferSize = sizeof (UINT32);
        return EFI_BUFFER_TOO_SMALL;
      }
      *(UINT32 *)Buffer = USBFN_MAX_TRANSFER;
      *BufferSize       = sizeof (UINT32);
      return EFI_SUCCESS;

    case EfiUsbPolicyZeroLengthTerminationSupport:
    case EfiUsbPolicyZeroLengthTermination:
      if (*BufferSize < sizeof (BOOLEAN)) {
        *BufferSize = sizeof (BOOLEAN);
        return EFI_BUFFER_TOO_SMALL;
      }
      *(BOOLEAN *)Buffer = FALSE;   // no automatic ZLP: the class driver sends one
      *BufferSize        = sizeof (BOOLEAN);
      return EFI_SUCCESS;

    default:
      return EFI_UNSUPPORTED;
  }
}

STATIC CONST EFI_USBFN_IO_PROTOCOL  mUsbFnIoTemplate = {
  EFI_USBFN_IO_PROTOCOL_REVISION,
  UsbFnDetectPort,
  UsbFnConfigureEnableEndpoints,
  UsbFnGetEndpointMaxPacketSize,
  UsbFnGetDeviceInfo,
  UsbFnGetVendorIdProductId,
  UsbFnAbortTransfer,
  UsbFnGetEndpointStallState,
  UsbFnSetEndpointStallState,
  UsbFnEventHandler,
  UsbFnTransfer,
  UsbFnGetMaxTransferSize,
  UsbFnAllocateTransferBuffer,
  UsbFnFreeTransferBuffer,
  UsbFnStartController,
  UsbFnStopController,
  UsbFnSetEndpointPolicy,
  UsbFnGetEndpointPolicy,
};

EFI_STATUS
UsbFnSelfTestStart (
  IN EFI_USBFN_IO_PROTOCOL  *UsbFnIo
  );

/**
  bConfigurationValue the host selected (0 = unconfigured); self test only.
**/
UINT8
UsbFnCurrentConfiguration (
  VOID
  )
{
  return mDev->Configuration;
}

/**
  Interrupt vs EventHandler-serviced counts; self test only.
**/
VOID
UsbFnServiceCounts (
  OUT UINTN  *Irq,
  OUT UINTN  *Poll
  )
{
  *Irq  = mDev->IrqCount;
  *Poll = mDev->PollCount;
}

/**
  Leave the port the way the next OS expects it: no D+ pull-up (the host
  sees a clean disconnect) and no controller interrupts.
**/
STATIC
VOID
EFIAPI
UsbFnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mDev->Started) {
    Dwc2Connect (FALSE);
    MmioWrite32 (REG (GINTMSK), 0);
    MmioWrite32 (REG (GAHBCFG), 0);
    mDev->Started = FALSE;
  }
}

EFI_STATUS
EFIAPI
UsbFnDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  mDev = AllocateZeroPool (sizeof (*mDev));
  if (mDev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  mDev->Signature = USBFN_SIGNATURE;
  CopyMem (&mDev->UsbFnIo, &mUsbFnIoTemplate, sizeof (mUsbFnIoTemplate));

  Status = gBS->LocateProtocol (&gHardwareInterruptProtocolGuid, NULL,
                                (VOID **)&mDev->Interrupt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Routed now, enabled by StartController.
  //
  Status = mDev->Interrupt->RegisterInterruptSource (mDev->Interrupt, DWC2_IRQ,
                                                     UsbFnInterruptHandler);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mDev->Interrupt->DisableInterruptSource (mDev->Interrupt, DWC2_IRQ);

  gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                    UsbFnExitBootServices, NULL, &mDev->ExitBootServices);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mDev->Handle,
                  &gEfiUsbFunctionIoProtocolGuid, &mDev->UsbFnIo,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

#if USBFN_SELFTEST
  UsbFnSelfTestStart (&mDev->UsbFnIo);
#endif
  return EFI_SUCCESS;
}
