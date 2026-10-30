/** @file
  Decompress a .xz stream (XZ Embedded, LZMA2, no BCJ filters).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __XZ_DEC_LIB_H__
#define __XZ_DEC_LIB_H__

/**
  Decompress one .xz stream into a newly allocated pool buffer.  Bytes after
  the end of the stream (block padding) are ignored.

  @param[in]  Input       The .xz data.
  @param[in]  InputSize   Its size in bytes.
  @param[out] Output      Decompressed data; the caller frees it with FreePool.
  @param[out] OutputSize  Its size in bytes.

  @retval EFI_SUCCESS           Decompressed.
  @retval EFI_COMPROMISED_DATA  Not a valid / supported .xz stream.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.
**/
EFI_STATUS
EFIAPI
XzDecompress (
  IN  CONST VOID  *Input,
  IN  UINTN       InputSize,
  OUT VOID        **Output,
  OUT UINTN       *OutputSize
  );

#endif
