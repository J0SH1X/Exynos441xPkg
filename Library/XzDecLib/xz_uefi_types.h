/** @file
  C99 types XZ Embedded expects, mapped onto edk2's base types.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef XZ_UEFI_TYPES_H
#define XZ_UEFI_TYPES_H

#include <Base.h>

typedef UINT8    uint8_t;
typedef UINT16   uint16_t;
typedef UINT32   uint32_t;
typedef UINT64   uint64_t;
typedef UINTN    size_t;
typedef BOOLEAN  bool;

#define true   TRUE
#define false  FALSE

#endif
