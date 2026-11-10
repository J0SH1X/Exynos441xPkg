/** @file
  Touch test: paints where EFI_ABSOLUTE_POINTER_PROTOCOL says the finger or
  pen is (green while touching, yellow with the S Pen's side button held, a
  red dot where it lifted) and prints the raw state, including pen hover.
  Any key (volume/power/home) exits.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/GraphicsOutput.h>

#define DOT_SIZE  12

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mBlack = { 0x00, 0x00, 0x00, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mGreen = { 0x00, 0xFF, 0x00, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mRed   = { 0x00, 0x00, 0xFF, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mYellow = { 0x00, 0xFF, 0xFF, 0 };

STATIC
VOID
DrawDot (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN UINTN                          X,
  IN UINTN                          Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color
  )
{
  UINTN  Width;
  UINTN  Height;

  Width  = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;
  X      = (X >= DOT_SIZE / 2) ? X - DOT_SIZE / 2 : 0;
  Y      = (Y >= DOT_SIZE / 2) ? Y - DOT_SIZE / 2 : 0;
  if ((X >= Width) || (Y >= Height)) {
    return;
  }

  Gop->Blt (Gop, Color, EfiBltVideoFill, 0, 0, X, Y,
            MIN (DOT_SIZE, Width - X), MIN (DOT_SIZE, Height - Y), 0);
}

EFI_STATUS
EFIAPI
TouchTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_ABSOLUTE_POINTER_PROTOCOL  *Pointer;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EFI_ABSOLUTE_POINTER_STATE     State;
  EFI_EVENT                      Events[2];
  EFI_INPUT_KEY                  Key;
  UINTN                          Index;
  UINTN                          X;
  UINTN                          Y;
  UINTN                          LastX;
  UINTN                          LastY;
  BOOLEAN                        Touching;
  EFI_STATUS                     Status;

  Status = gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&Pointer);
  if (EFI_ERROR (Status)) {
    Print (L"No absolute pointer (touchscreen): %r\n", Status);
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Print (L"No GOP: %r\n", Status);
    return Status;
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  Gop->Blt (Gop, &mBlack, EfiBltVideoFill, 0, 0, 0, 0,
            Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution, 0);
  Print (L"Touch test: range %lu x %lu, any key exits\n",
         Pointer->Mode->AbsoluteMaxX + 1, Pointer->Mode->AbsoluteMaxY + 1);

  Pointer->Reset (Pointer, FALSE);
  gST->ConIn->Reset (gST->ConIn, FALSE);

  Events[0] = Pointer->WaitForInput;
  Events[1] = gST->ConIn->WaitForKey;
  Touching  = FALSE;
  LastX     = 0;
  LastY     = 0;

  for ( ; ; ) {
    gBS->WaitForEvent (2, Events, &Index);
    if (Index == 1) {
      gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      break;
    }

    if (EFI_ERROR (Pointer->GetState (Pointer, &State))) {
      continue;
    }

    if ((State.ActiveButtons & EFI_ABSP_TouchActive) != 0) {
      X = (UINTN)(State.CurrentX * Gop->Mode->Info->HorizontalResolution /
                  (Pointer->Mode->AbsoluteMaxX + 1));
      Y = (UINTN)(State.CurrentY * Gop->Mode->Info->VerticalResolution /
                  (Pointer->Mode->AbsoluteMaxY + 1));
      DrawDot (Gop, X, Y, ((State.ActiveButtons & EFI_ABS_AltActive) != 0) ? &mYellow : &mGreen);
      LastX    = X;
      LastY    = Y;
      Touching = TRUE;
    } else if (Touching) {
      DrawDot (Gop, LastX, LastY, &mRed);
      Touching = FALSE;
    }

    gST->ConOut->SetCursorPosition (gST->ConOut, 0, 1);
    Print (L"x %5lu  y %5lu  z %3lu  %s%s   \n", State.CurrentX, State.CurrentY,
           State.CurrentZ, Touching ? L"down" : L"up  ",
           ((State.ActiveButtons & EFI_ABS_AltActive) != 0) ? L" side" : L"     ");
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  return EFI_SUCCESS;
}
