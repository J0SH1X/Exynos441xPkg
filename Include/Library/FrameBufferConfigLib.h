/** @file
  Framebuffer discovery for Samsung Exynos4x1x.

  The Samsung bootloader already initialised FIMD + MIPI-DSI + the panel and
  pointed a FIMD window at a framebuffer in DRAM.  Instead of reprogramming
  the display pipeline we only *read back* the FIMD window registers to find
  out where that framebuffer is and in which format the hardware scans it,
  then paint into it.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _FRAME_BUFFER_CONFIG_LIB_H_
#define _FRAME_BUFFER_CONFIG_LIB_H_

#include <Base.h>

typedef struct {
  UINT32  Base;           ///< Physical start address of the scan-out buffer
  UINT32  Width;          ///< Active horizontal resolution (pixels)
  UINT32  Height;         ///< Active vertical resolution (lines)
  UINT32  BytesPerPixel;  ///< Memory bytes per pixel (2, 3 or 4)
} EXYNOS_FRAMEBUFFER_CONFIG;

/**
  Discover (once, lazily) the framebuffer the bootloader set up.

  @param[out] Config  Filled in on success.

  @retval RETURN_SUCCESS            Configuration is valid.
  @retval RETURN_INVALID_PARAMETER  Config was NULL.
  @retval RETURN_UNSUPPORTED        No enabled FIMD window could be found.
**/
RETURN_STATUS
EFIAPI
ExynosGetFrameBufferConfig (
  OUT EXYNOS_FRAMEBUFFER_CONFIG  *Config
  );

/**
  Called by PrePi with the (fallback) bpp from the PCD.  We never touch the
  hardware - the bootloader's configuration is left exactly as it is - this
  just triggers framebuffer discovery.

  @param[in] Bpp  Fallback bits per pixel (unused, kept for API compat).
**/
VOID
EFIAPI
ReconfigFb (
  IN UINTN  Bpp
  );

/**
  Fill the whole (discovered) framebuffer with a solid colour.

  @param[in] BgColor  Colour, low bytes first, up to BytesPerPixel wide.
**/
VOID
EFIAPI
PaintScreen (
  IN UINTN  BgColor
  );

#endif // _FRAME_BUFFER_CONFIG_LIB_H_
