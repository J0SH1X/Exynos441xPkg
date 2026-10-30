/** @file
  Samsung Exynos4210 (GT-N7000) defines shared by the ACPI tables.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __EXYNOS_ACPI_PLATFORM_H__
#define __EXYNOS_ACPI_PLATFORM_H__

#include <IndustryStandard/Acpi.h>

#define EFI_ACPI_OEM_ID             {'S','A','M','S','N','G'}
#define EFI_ACPI_OEM_TABLE_ID       SIGNATURE_64 ('E','X','Y','4','2','1','0',' ')
#define EFI_ACPI_OEM_REVISION       0x00000001
#define EFI_ACPI_CREATOR_ID         SIGNATURE_32 ('E','D','K','2')
#define EFI_ACPI_CREATOR_REVISION   0x00000001

#define EXYNOS_ACPI_HEADER(Signature, Type, Revision)  \
  {                                                    \
    Signature,                                         \
    sizeof (Type),                                     \
    Revision,                                          \
    0,                     /* checksum: AcpiTableDxe */ \
    EFI_ACPI_OEM_ID,                                   \
    EFI_ACPI_OEM_TABLE_ID,                             \
    EFI_ACPI_OEM_REVISION,                             \
    EFI_ACPI_CREATOR_ID,                               \
    EFI_ACPI_CREATOR_REVISION                          \
  }

//
// GIC-400-style GIC on the 4210: distributor, then one CPU interface per
// core 0x8000 apart (kernel cpu-exynos4.c gic_bank_offset).
//
#define EXYNOS_GICD_BASE         0x10490000
#define EXYNOS_GICC_BASE         0x10480000

//
// DWC2 USB device controller (UsbFnDxe): registers + FIFO windows, SPI 71.
//
#define EXYNOS_USB_DEVICE_BASE   0x12480000
#define EXYNOS_USB_DEVICE_SIZE   0x00020000

#endif
