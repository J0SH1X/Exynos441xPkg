/** @file
  XzDecLib: XzDecompress() on top of XZ Embedded's multi-call decoder.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "xz_config.h"

#include <Uefi.h>
#include <Library/XzDecLib.h>

//
// Largest LZMA2 dictionary accepted (xz -9 uses 64 MiB).
//
#define XZ_DICT_MAX  (64U << 20)

EFI_STATUS
EFIAPI
XzDecompress (
  IN  CONST VOID  *Input,
  IN  UINTN       InputSize,
  OUT VOID        **Output,
  OUT UINTN       *OutputSize
  )
{
  struct xz_dec  *Dec;
  struct xz_buf  Buf;
  enum xz_ret    Ret;
  UINT8          *Out;
  UINT8          *Bigger;
  UINTN          OutSize;

  xz_crc32_init ();
  Dec = xz_dec_init (XZ_DYNALLOC, XZ_DICT_MAX);
  if (Dec == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  OutSize = MAX (InputSize * 4, (UINTN)SIZE_64KB);
  Out     = AllocatePool (OutSize);
  if (Out == NULL) {
    xz_dec_end (Dec);
    return EFI_OUT_OF_RESOURCES;
  }

  Buf.in       = Input;
  Buf.in_pos   = 0;
  Buf.in_size  = InputSize;
  Buf.out      = Out;
  Buf.out_pos  = 0;
  Buf.out_size = OutSize;

  for ( ; ; ) {
    Ret = xz_dec_run (Dec, &Buf);
    if (Ret == XZ_STREAM_END) {
      break;
    }
    if (Ret == XZ_UNSUPPORTED_CHECK) {
      //
      // Reported once after the header with XZ_DEC_ANY_CHECK (CRC64 here):
      // the check is skipped and decoding continues on the next call.
      //
      continue;
    }
    if ((Ret != XZ_OK) || ((Buf.in_pos == Buf.in_size) && (Buf.out_pos < Buf.out_size))) {
      //
      // Corrupt data, an unsupported option, or input exhausted early.
      //
      FreePool (Out);
      xz_dec_end (Dec);
      return EFI_COMPROMISED_DATA;
    }
    if (Buf.out_pos == Buf.out_size) {
      Bigger = ReallocatePool (OutSize, OutSize * 2, Out);
      if (Bigger == NULL) {
        FreePool (Out);
        xz_dec_end (Dec);
        return EFI_OUT_OF_RESOURCES;
      }
      Out          = Bigger;
      OutSize     *= 2;
      Buf.out      = Out;
      Buf.out_size = OutSize;
    }
  }

  xz_dec_end (Dec);
  *Output     = Out;
  *OutputSize = Buf.out_pos;
  return EFI_SUCCESS;
}
