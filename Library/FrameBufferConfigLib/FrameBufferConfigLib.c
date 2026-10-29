/** @file
  Runtime framebuffer discovery for Samsung Exynos4 (FIMD0).

  No DEBUG() calls in here: this library is used by the framebuffer serial
  port library itself, so printing would recurse.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FrameBufferConfigLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>

// Exynos4 FIMD0 register offsets (see Linux arch/arm/plat-s5p/regs-fb-s5p.h)
#define S3C_VIDCON0     0x0000
#define S3C_VIDCON1     0x0004
#define S3C_VIDTCON2    0x0018
#define S3C_WINCON(x)   (0x0020 + ((x) * 4))
#define VIDADDR_START(x) (0x00A0 + ((x) * 8)) // win0/win1: +8 per window
#define VIDADDR_END(x)   (0x00D0 + ((x) * 8))

// The generic window layout used by windows 0/1 has 8-byte stride; windows
// 2..4 pack their address registers differently.  Handle both.
#define WIN_START_REG(w) (((w) < 2) ? (0x00A0 + ((w) * 8)) : (0x00B0 + (((w) - 2) * 8)))
#define WIN_END_REG(w)   (((w) < 2) ? (0x00D0 + ((w) * 8)) : (0x00E0 + (((w) - 2) * 8)))

#define WINCON_ENWIN     (1u << 0)
#define WINCON_BPPMODE   (0xFu << 2)

#define VIDCON0_ENVID    (1u << 1)
#define VIDCON0_ENVID_F  (1u << 0)

#define EXYNOS_MAX_WINDOWS 5

STATIC EXYNOS_FRAMEBUFFER_CONFIG  mConfig;
STATIC BOOLEAN                    mTried    = FALSE;
STATIC BOOLEAN                    mValid    = FALSE;

/**
  Translate a FIMD BPPMODE value into bytes per pixel in memory.
**/
STATIC
UINT32
BppModeToBytes (
  IN UINT32  BppMode
  )
{
  switch (BppMode) {
    case 0x5: // 16BPP_565
    case 0x7: // 16BPP_1555
    case 0xE: // 16BPP_A4444 (win1+)
      return 2;
    case 0x8: // 18BPP_666
    case 0x9: // 18BPP_A1665
    case 0xB: // 24BPP_888
      return 3;
    case 0xA: // 19BPP_A1666
    case 0xC: // 24BPP_A1887
    case 0xD: // 25BPP_A1888 / 28BPP_A4888
      return 4;
    default:
      return 0;
  }
}

STATIC
VOID
Discover (
  VOID
  )
{
  UINT32  Fimd   = PcdGet32 (PcdFimdBaseAddress);
  UINT32  Vidcon = MmioRead32 (Fimd + S3C_VIDCON0);
  UINT32  Win;
  UINT32  Width  = PcdGet32 (PcdFrameBufferWidth);
  UINT32  Height = PcdGet32 (PcdFrameBufferHeight);
  UINT32  Bytes  = PcdGet32 (PcdFrameBufferPixelBpp) / 8;

  // Panel timing programmed by the bootloader: LINEVAL/HOZVAL are "size - 1".
  if ((Vidcon & (VIDCON0_ENVID | VIDCON0_ENVID_F)) != 0) {
    UINT32  Vidtcon2 = MmioRead32 (Fimd + S3C_VIDTCON2);
    UINT32  W        = (Vidtcon2 & 0x7FF) + 1;
    UINT32  H        = ((Vidtcon2 >> 11) & 0x7FF) + 1;

    // Sanity: the panel on this family is 1280x800 (or portrait 800x1280).
    if ((W >= 320) && (W <= 2048) && (H >= 320) && (H <= 2048)) {
      Width  = W;
      Height = H;
    }
  }

  for (Win = 0; Win < EXYNOS_MAX_WINDOWS; Win++) {
    UINT32  Wincon = MmioRead32 (Fimd + S3C_WINCON (Win));
    UINT32  Start;
    UINT32  End;
    UINT32  Diff;
    UINT32  BppMode;
    UINT32  Cand;

    if ((Wincon & WINCON_ENWIN) == 0) {
      continue;
    }

    Start = MmioRead32 (Fimd + WIN_START_REG (Win));
    End   = MmioRead32 (Fimd + WIN_END_REG (Win));
    if ((Start == 0) || (Start == 0xFFFFFFFF) || (End <= Start)) {
      continue;
    }

    BppMode = (Wincon & WINCON_BPPMODE) >> 2;
    Diff    = End - Start;

    // Preferred: derive bytes-per-pixel from the buffer geometry.  The end
    // address is programmed as start + (buffer lines * stride), so look for
    // a stride that tiles the buffer (the bootloader may have allocated more
    // than one frame: accept 1..4 frames worth).
    Bytes = 0;
    for (Cand = 4; Cand >= 2; Cand--) {
      UINT32  Stride = Width * Cand;
      UINT32  Lines;
      UINT32  Frame;
      BOOLEAN Match = FALSE;

      if ((Stride == 0) || (Diff < Stride)) {
        continue;
      }

      // Lines the hardware would scan for this candidate stride.  The buffer
      // end address is start + frames * height * stride (give or take a line),
      // so accept 1..4 stacked frames.
      Lines = Diff / Stride;
      for (Frame = 1; Frame <= 4; Frame++) {
        UINT32  Expect = Frame * Height;

        if ((Lines + 2 >= Expect) && (Lines <= Expect + 2)) {
          Match = TRUE;
          break;
        }
      }

      if (!Match) {
        continue;
      }

      // Prefer the candidate that agrees with the FIMD pixel format.
      if ((BppModeToBytes (BppMode) == Cand) || (BppModeToBytes (BppMode) == 0)) {
        Bytes = Cand;
        break;
      }

      if (Bytes == 0) {
        Bytes = Cand;
      }
    }

    if (Bytes == 0) {
      Bytes = BppModeToBytes (BppMode);
    }

    if (Bytes < 2) {
      Bytes = 4;
    }

    mConfig.Base          = Start;
    mConfig.Width         = Width;
    mConfig.Height        = Height;
    mConfig.BytesPerPixel = Bytes;
    mValid                = TRUE;
    return;
  }
}

RETURN_STATUS
EFIAPI
ExynosGetFrameBufferConfig (
  OUT EXYNOS_FRAMEBUFFER_CONFIG  *Config
  )
{
  if (Config == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  if (!mTried) {
    mTried = TRUE;
    Discover ();
  }

  if (!mValid) {
    return RETURN_UNSUPPORTED;
  }

  CopyMem (Config, &mConfig, sizeof (mConfig));
  return RETURN_SUCCESS;
}

VOID
EFIAPI
ReconfigFb (
  IN UINTN  Bpp
  )
{
  // The bootloader's FIMD/DSIM/panel setup is deliberately left untouched:
  // just find out what it did.
  (void)Bpp;
  if (!mTried) {
    mTried = TRUE;
    Discover ();
  }
}

VOID
EFIAPI
PaintScreen (
  IN UINTN  BgColor
  )
{
  EXYNOS_FRAMEBUFFER_CONFIG  Config;
  UINT8                      *Pixels;
  UINTN                      Count;
  UINTN                      Index;

  if (ExynosGetFrameBufferConfig (&Config) != RETURN_SUCCESS) {
    return;
  }

  Pixels = (UINT8 *)(UINTN)Config.Base;
  Count  = (UINTN)Config.Width * Config.Height;

  for (Index = 0; Index < Count; Index++) {
    UINTN  Shift;
    UINTN  Byte;

    for (Byte = 0; Byte < Config.BytesPerPixel; Byte++) {
      Shift  = Byte * 8;
      *Pixels++ = (UINT8)((BgColor >> Shift) & 0xFF);
    }
  }
}
