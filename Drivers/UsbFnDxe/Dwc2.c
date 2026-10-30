/** @file
  DWC2 USB device controller: core reset, FIFO layout and endpoint control.

  Device-mode only (GUSBCFG.ForceDevMode); slave mode, no internal DMA.
  Sequences follow the kernel's s3c_udc_otg.c reconfig_usbd()/reset_usbd()
  for the Exynos4210.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UsbFnDxe.h"

#define REG(x)  (DWC2_BASE + (x))

STATIC
BOOLEAN
Dwc2WaitClear (
  IN UINT32  Reg,
  IN UINT32  Mask
  )
{
  UINTN  Retry;

  for (Retry = 0; Retry < 10000; Retry++) {
    if (!(MmioRead32 (REG (Reg)) & Mask)) {
      return TRUE;
    }
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC
VOID
Dwc2FlushFifos (
  VOID
  )
{
  MmioWrite32 (REG (GRSTCTL), GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM_ALL);
  Dwc2WaitClear (GRSTCTL, GRSTCTL_TXFFLSH);
  MmioWrite32 (REG (GRSTCTL), GRSTCTL_RXFFLSH);
  Dwc2WaitClear (GRSTCTL, GRSTCTL_RXFFLSH);
}

/**
  Soft-reset the core into device mode, lay out the FIFOs and leave the
  device soft-disconnected with the controller interrupt unmasked.
**/
EFI_STATUS
Dwc2CoreInit (
  VOID
  )
{
  UINT32  Depth;
  UINT32  Start;
  UINTN   Ep;
  UINTN   Retry;

  for (Retry = 0; Retry < 10000; Retry++) {
    if (MmioRead32 (REG (GRSTCTL)) & GRSTCTL_AHBIDLE) {
      break;
    }
    gBS->Stall (10);
  }

  MmioWrite32 (REG (GRSTCTL), GRSTCTL_CSFTRST);
  if (!Dwc2WaitClear (GRSTCTL, GRSTCTL_CSFTRST)) {
    DEBUG ((DEBUG_ERROR, "UsbFn: core soft reset stuck\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // Exynos4210: 16-bit UTMI+, turnaround 5 (reconfig_usbd).  Forcing device
  // mode takes up to 25 ms to settle.
  //
  MmioWrite32 (REG (GUSBCFG), GUSBCFG_FORCEDEVMODE | GUSBCFG_TRDTIM (5) |
                              GUSBCFG_PHYIF16 | GUSBCFG_TOUTCAL (7));
  gBS->Stall (25000);

  MmioWrite32 (REG (DCTL), DCTL_SFTDISCON);
  MmioWrite32 (REG (DCFG), 0);           // high speed, address 0
  MmioWrite32 (REG (PCGCCTL), 0);

  //
  // RX FIFO, EP0 TX FIFO, then one TX FIFO per IN endpoint.
  //
  Depth = MmioRead32 (REG (GHWCFG3)) >> 16;
  if (RX_FIFO_WORDS + EP0_TXF_WORDS +
      (USBFN_MAX_EPS - 1) * EPN_TXF_WORDS > Depth) {
    DEBUG ((DEBUG_ERROR, "UsbFn: FIFO RAM %u words too small\n", Depth));
    return EFI_DEVICE_ERROR;
  }
  MmioWrite32 (REG (GRXFSIZ), RX_FIFO_WORDS);
  MmioWrite32 (REG (GNPTXFSIZ), (EP0_TXF_WORDS << 16) | RX_FIFO_WORDS);
  Start = RX_FIFO_WORDS + EP0_TXF_WORDS;
  for (Ep = 1; Ep < USBFN_MAX_EPS; Ep++) {
    MmioWrite32 (REG (DIEPTXF (Ep)), (EPN_TXF_WORDS << 16) | Start);
    Start += EPN_TXF_WORDS;
  }
  Dwc2FlushFifos ();

  //
  // Interrupts: device events at the core, transfer-complete and SETUP per
  // endpoint; per-endpoint enables follow in DAINTMSK as endpoints come up.
  //
  MmioWrite32 (REG (DIEPMSK), DEPINT_XFERCOMPL);
  MmioWrite32 (REG (DOEPMSK), DEPINT_XFERCOMPL | DOEPINT_SETUP);
  MmioWrite32 (REG (DAINTMSK), (1U << 0) | (1U << 16));
  MmioWrite32 (REG (DIEPEMPMSK), 0);
  MmioWrite32 (REG (GINTSTS), 0xFFFFFFFF);
  MmioWrite32 (REG (GINTMSK), GINT_DEVICE_MASK);
  MmioWrite32 (REG (GAHBCFG), GAHBCFG_GLBLINTRMSK | GAHBCFG_TXFEMPLVL);

  DEBUG ((DEBUG_INFO, "UsbFn: DWC2 %x, FIFO %u words\n",
          MmioRead32 (REG (GSNPSID)), Depth));
  return EFI_SUCCESS;
}

/**
  Soft-connect (D+ pull-up on) or disconnect.
**/
VOID
Dwc2Connect (
  IN BOOLEAN  Connect
  )
{
  if (Connect) {
    MmioAnd32 (REG (DCTL), ~DCTL_SFTDISCON);
  } else {
    MmioOr32 (REG (DCTL), DCTL_SFTDISCON);
  }
}

/**
  Arm EP0 OUT for up to three back-to-back SETUP packets or one OUT packet
  (data or status stage).
**/
VOID
Dwc2Ep0ArmOut (
  VOID
  )
{
  MmioWrite32 (REG (DOEPTSIZ (0)), DOEPTSIZ0_SUPCNT (3) | DEPTSIZ_PKTCNT (1) |
                                   USBFN_EP0_MPS);
  MmioOr32 (REG (DOEPCTL (0)), DEPCTL_EPENA | DEPCTL_CNAK);
}

/**
  USB bus reset: NAK everything, drop transfers, back to address 0 with only
  EP0 enabled.
**/
VOID
Dwc2BusReset (
  IN USBFN_DEV  *Dev
  )
{
  UINT8  Ep;

  for (Ep = 0; Ep < USBFN_MAX_EPS; Ep++) {
    MmioOr32 (REG (DOEPCTL (Ep)), DEPCTL_SNAK);
    if (Ep != 0) {
      MmioWrite32 (REG (DIEPCTL (Ep)), 0);
      MmioWrite32 (REG (DOEPCTL (Ep)), 0);
    }
    ZeroMem (&Dev->In[Ep], sizeof (Dev->In[Ep]));
    ZeroMem (&Dev->Out[Ep], sizeof (Dev->Out[Ep]));
  }

  Dwc2FlushFifos ();
  MmioAnd32 (REG (DCFG), ~DCFG_DEVADDR_MASK);
  MmioWrite32 (REG (DAINTMSK), (1U << 0) | (1U << 16));
  MmioWrite32 (REG (DIEPEMPMSK), 0);
  Dev->Configuration = 0;

  Dwc2Ep0ArmOut ();
}

/**
  DWC2 takes the new address before the status stage (unlike most cores).
**/
VOID
Dwc2SetAddress (
  IN UINT8  Address
  )
{
  MmioAndThenOr32 (REG (DCFG), ~DCFG_DEVADDR_MASK, DCFG_DEVADDR (Address));
}

/**
  Bring up the bulk endpoints described by the class driver (SET_CONFIGURATION).
**/
VOID
Dwc2ActivateEndpoints (
  IN USBFN_DEV  *Dev
  )
{
  UINT8   Ep;
  UINT32  Ctl;

  for (Ep = 1; Ep < USBFN_MAX_EPS; Ep++) {
    if (Dev->EpMps[Ep] == 0) {
      continue;
    }

    //
    // A repeated SET_CONFIGURATION only resets the data toggles: rewriting
    // DxEPCTL would drop EPENA and silently lose a transfer the class
    // driver has already queued.
    //
    if ((MmioRead32 (REG (DIEPCTL (Ep))) | MmioRead32 (REG (DOEPCTL (Ep)))) &
        DEPCTL_USBACTEP) {
      MmioOr32 (REG (DIEPCTL (Ep)), DEPCTL_SETD0PID);
      MmioOr32 (REG (DOEPCTL (Ep)), DEPCTL_SETD0PID);
      continue;
    }

    Ctl = DEPCTL_USBACTEP | DEPCTL_SETD0PID | DEPCTL_SNAK |
          DEPCTL_EPTYPE (Dev->EpType[Ep]) | Dev->EpMps[Ep];
    MmioWrite32 (REG (DIEPCTL (Ep)), Ctl | DEPCTL_TXFNUM (Ep));
    MmioWrite32 (REG (DOEPCTL (Ep)), Ctl);
    MmioOr32 (REG (DAINTMSK), (1U << Ep) | (1U << (16 + Ep)));
  }
}

/**
  Program the next IN packet(s) of Dev->In[Ep] and start filling its FIFO.
  EP0 goes one max-packet at a time (its XFERSIZE field is 7 bits).
**/
VOID
Dwc2StartIn (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep
  )
{
  USBFN_XFER  *X;
  UINT32      Mps;
  UINT32      Len;
  UINT32      Pkts;

  X   = &Dev->In[Ep];
  Mps = (Ep == 0) ? USBFN_EP0_MPS : Dev->EpMps[Ep];

  Len = (UINT32)(X->Length - X->Done);
  if (Ep == 0) {
    Len = MIN (Len, Mps);
  }
  X->Queued = Len;

  Pkts = (Len == 0) ? 1 : (Len + Mps - 1) / Mps;
  MmioWrite32 (REG (DIEPTSIZ (Ep)), DEPTSIZ_PKTCNT (Pkts) | Len);
  MmioOr32 (REG (DIEPCTL (Ep)), DEPCTL_EPENA | DEPCTL_CNAK);

  if (Len != 0) {
    Dwc2FillTxFifo (Dev, Ep);
  }
}

/**
  Copy as much of the queued IN data as the endpoint's TX FIFO has room for;
  ask for a TxFE interrupt if more remains.
**/
VOID
Dwc2FillTxFifo (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep
  )
{
  USBFN_XFER  *X;
  UINTN       End;
  UINT32      Space;
  UINT32      Word;
  UINTN       Index;
  UINTN       Chunk;

  //
  // EP0: one packet (<= 64 bytes) always fits its 64-word FIFO in one go;
  // Done advances by Queued on XferCompl.  EPn: Done is the FIFO position.
  //
  X     = &Dev->In[Ep];
  End   = (Ep == 0) ? (X->Done + X->Queued) : X->Length;
  Index = X->Done;

  while (Index < End) {
    Space = (MmioRead32 (REG (DTXFSTS (Ep))) & 0xFFFF) * 4;
    Chunk = MIN (End - Index, (UINTN)Space);
    if (Chunk == 0) {
      break;
    }
    while (Chunk > 0) {
      Word = 0;
      CopyMem (&Word, X->Buffer + Index, MIN (Chunk, 4));
      MmioWrite32 (REG (DFIFO (Ep)), Word);
      Index += MIN (Chunk, 4);
      Chunk -= MIN (Chunk, 4);
    }
  }

  if (Ep != 0) {
    X->Done = Index;
  }

  if (Index < End) {
    MmioOr32 (REG (DIEPEMPMSK), 1U << Ep);
  } else {
    MmioAnd32 (REG (DIEPEMPMSK), ~(1U << Ep));
  }
}

/**
  Arm an OUT endpoint for Dev->Out[Ep].  The data arrives through the RX FIFO
  (GINTSTS.RxFLvl) and completes with DOEPINT.XferCompl.
**/
VOID
Dwc2StartOut (
  IN USBFN_DEV  *Dev,
  IN UINT8      Ep
  )
{
  USBFN_XFER  *X;
  UINT32      Mps;
  UINT32      Pkts;

  if (Ep == 0) {
    Dwc2Ep0ArmOut ();
    return;
  }

  X    = &Dev->Out[Ep];
  Mps  = Dev->EpMps[Ep];
  Pkts = (UINT32)((X->Length + Mps - 1) / Mps);
  if (Pkts == 0) {
    Pkts = 1;
  }

  MmioWrite32 (REG (DOEPTSIZ (Ep)), DEPTSIZ_PKTCNT (Pkts) | (Pkts * Mps));
  MmioOr32 (REG (DOEPCTL (Ep)), DEPCTL_EPENA | DEPCTL_CNAK);
}

VOID
Dwc2SetStall (
  IN UINT8    Ep,
  IN BOOLEAN  In,
  IN BOOLEAN  Stall
  )
{
  UINT32  Reg;

  Reg = In ? DIEPCTL (Ep) : DOEPCTL (Ep);
  if (Stall) {
    MmioOr32 (REG (Reg), DEPCTL_STALL);
  } else {
    //
    // Clearing a halt also resets the data toggle (USB 2.0 9.4.5).
    //
    MmioAndThenOr32 (REG (Reg), ~DEPCTL_STALL, (Ep != 0) ? DEPCTL_SETD0PID : 0);
  }
}

BOOLEAN
Dwc2GetStall (
  IN UINT8    Ep,
  IN BOOLEAN  In
  )
{
  return (MmioRead32 (REG (In ? DIEPCTL (Ep) : DOEPCTL (Ep))) & DEPCTL_STALL) != 0;
}

/**
  Stop an in-flight transfer: NAK, disable, and for IN drop the FIFO.
**/
VOID
Dwc2AbortEp (
  IN UINT8    Ep,
  IN BOOLEAN  In
  )
{
  UINT32  Ctl;
  UINT32  Int;

  Ctl = In ? DIEPCTL (Ep) : DOEPCTL (Ep);
  Int = In ? DIEPINT (Ep) : DOEPINT (Ep);

  if (MmioRead32 (REG (Ctl)) & DEPCTL_EPENA) {
    MmioOr32 (REG (Ctl), DEPCTL_SNAK | DEPCTL_EPDIS);
    Dwc2WaitClear (Ctl, DEPCTL_EPENA);
    MmioWrite32 (REG (Int), DEPINT_EPDISBLD);
  }

  if (In) {
    MmioWrite32 (REG (GRSTCTL), GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM (Ep));
    Dwc2WaitClear (GRSTCTL, GRSTCTL_TXFFLSH);
    MmioAnd32 (REG (DIEPEMPMSK), ~(1U << Ep));
  }
}
