/** @file
  Boot a downstream ARM Linux zImage (Samsung GT-N7000 kernel) from UEFI.

    LinuxBoot.efi [-c "cmdline"] [-a "text"] [\path\zImage]

  Searches every FAT file system for the zImage (default \zImage), then
  boots it the way the Samsung bootloader would have: zImage at 0x40008000
  (zreladdr), ATAGs at 0x40000100, r0 = 0, r1 = machine type, r2 = ATAGs.
  The ATAGs are rebuilt from the bootloader's own list that BootShim/PrePi
  preserved (ExynosBootParams.h): memory banks, serial, revision and the
  command line, which carries settings the kernel depends on
  (s3cfb.bootloaderfb, lcdtype, sec_log, ...).  -c replaces the command
  line, -a appends to it.

  Loosely after HtcLeoPkg's HtcLeoMenuApp Booter.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/SimpleFileSystem.h>

#include <ExynosBootParams.h>

#define KERNEL_ADDR        0x40008000U   // zreladdr (mach-exynos/Makefile.boot)
#define ATAGS_REGION       0x40000000U   // first 32 KiB of DRAM, below the kernel
#define ATAGS_REGION_SIZE  0x8000U
#define ATAGS_ADDR         0x40000100U
#define ATAGS_MAX          (ATAGS_REGION_SIZE - (ATAGS_ADDR - ATAGS_REGION))
#define CMDLINE_MAX        1024

#define ZIMAGE_MAGIC       0x016F2818U
#define ZIMAGE_MAGIC_OFF   0x24
#define ZIMAGE_START_OFF   0x28
#define ZIMAGE_END_OFF     0x2C

#define MACH_SMDKC210      2838          // what the Samsung bootloader passes

//
// PL310 L2 cache controller (off under UEFI today; handled if a bootloader
// ever leaves it on).
//
#define PL310_BASE         0x10502000U
#define PL310_CTRL         (PL310_BASE + 0x100)
#define PL310_CACHE_SYNC   (PL310_BASE + 0x730)
#define PL310_CLEAN_INV_WAY (PL310_BASE + 0x7FC)

VOID
LinuxJump (
  IN UINT32  Entry,
  IN UINT32  MachType,
  IN UINT32  Atags
  );

STATIC
EFI_FILE_INFO *
GetFileInfo (
  IN EFI_FILE_PROTOCOL  *File
  )
{
  EFI_FILE_INFO  *Info;
  UINTN          Size;

  Size = 0;
  if (File->GetInfo (File, &gEfiFileInfoGuid, &Size, NULL) != EFI_BUFFER_TOO_SMALL) {
    return NULL;
  }
  Info = AllocatePool (Size);
  if ((Info != NULL) && EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid, &Size, Info))) {
    FreePool (Info);
    Info = NULL;
  }
  return Info;
}

/**
  TRUE if [A, A+ASize) and [B, B+BSize) overlap.
**/
STATIC
BOOLEAN
Overlaps (
  IN UINTN  A,
  IN UINTN  ASize,
  IN UINTN  B,
  IN UINTN  BSize
  )
{
  return (A < B + BSize) && (B < A + ASize);
}

/**
  Find Path on any FAT volume and read it into a pool buffer.
**/
STATIC
EFI_STATUS
LoadFromAnyVolume (
  IN  CONST CHAR16  *Path,
  OUT UINT8         **Buffer,
  OUT UINTN         *Size
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  CHAR16                           *Where;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
                                        (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    if (EFI_ERROR (Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0))) {
      Root->Close (Root);
      continue;
    }

    Info = GetFileInfo (File);
    if ((Info == NULL) || ((Info->Attribute & EFI_FILE_DIRECTORY) != 0)) {
      Status = EFI_NOT_FOUND;
    } else {
      *Size   = (UINTN)Info->FileSize;
      *Buffer = AllocatePool (*Size);
      if (*Buffer == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
      } else {
        Status = File->Read (File, Size, *Buffer);
      }
    }
    if (Info != NULL) {
      FreePool (Info);
    }
    File->Close (File);
    Root->Close (Root);

    if (!EFI_ERROR (Status)) {
      Where = ConvertDevicePathToText (DevicePathFromHandle (Handles[Index]), TRUE, TRUE);
      Print (L"LinuxBoot: %s from %s (%u bytes)\n", Path,
             (Where != NULL) ? Where : L"?", *Size);
      if (Where != NULL) {
        FreePool (Where);
      }
      break;
    }
  }

  FreePool (Handles);
  return Status;
}

/**
  Kernel ATAGs: the bootloader's list without its command line and initrd,
  then the (possibly replaced or extended) command line.

  @return Bytes written to Out, 0 if it did not fit.
**/
STATIC
UINTN
BuildAtags (
  OUT UINT32        *Out,
  IN  UINTN         OutSize,
  IN  CONST CHAR8   *Replace OPTIONAL,
  IN  CONST CHAR8   *Append OPTIONAL,
  OUT CHAR8         *Cmdline,
  OUT UINT32        *MachType
  )
{
  EXYNOS_BOOT_PARAMS  *Bp;
  ATAG_HEADER         *Tag;
  UINT32              *Ptr;
  UINTN               Words;

  Bp         = BOOT_PARAMS;
  Ptr        = Out;
  Cmdline[0] = '\0';
  *MachType  = MACH_SMDKC210;

  if ((Bp->Magic == BOOT_PARAMS_MAGIC) && (Bp->AtagsSize != 0)) {
    *MachType = Bp->MachType;
    for (Tag = (ATAG_HEADER *)Bp->Atags; Tag->Size != 0; Tag = ATAG_NEXT (Tag)) {
      if (Tag->Tag == ATAG_CMDLINE) {
        AsciiStrnCpyS (Cmdline, CMDLINE_MAX, ((ATAG_CMDLINE_DATA *)Tag)->Cmdline,
                       (Tag->Size - 2) * 4);
        continue;
      }
      if ((Tag->Tag == ATAG_INITRD2) || (Tag->Tag == ATAG_RAMDISK)) {
        continue;   // the zImage carries its own initramfs
      }
      if ((UINTN)(Ptr - Out + Tag->Size) * 4 > OutSize) {
        return 0;
      }
      CopyMem (Ptr, Tag, Tag->Size * 4);
      Ptr += Tag->Size;
    }
  } else {
    //
    // No bootloader list: minimal CORE + 1 GiB at 0x40000000.
    //
    *Ptr++ = 2;
    *Ptr++ = ATAG_CORE;
    *Ptr++ = 4;
    *Ptr++ = ATAG_MEM;
    *Ptr++ = 0x40000000;
    *Ptr++ = 0x40000000;
  }

  if (Replace != NULL) {
    AsciiStrCpyS (Cmdline, CMDLINE_MAX, Replace);
  }
  if ((Append != NULL) && (Append[0] != '\0')) {
    if (Cmdline[0] != '\0') {
      AsciiStrCatS (Cmdline, CMDLINE_MAX, " ");
    }
    AsciiStrCatS (Cmdline, CMDLINE_MAX, Append);
  }

  //
  // CMDLINE: NUL included, rounded up to whole words; then ATAG_NONE.
  //
  Words = (AsciiStrLen (Cmdline) + 4) / 4;
  if ((UINTN)(Ptr - Out + 2 + Words + 2) * 4 > OutSize) {
    return 0;
  }
  *Ptr++ = 2 + (UINT32)Words;
  *Ptr++ = ATAG_CMDLINE;
  ZeroMem (Ptr, Words * 4);
  CopyMem (Ptr, Cmdline, AsciiStrLen (Cmdline));
  Ptr += Words;
  *Ptr++ = 0;
  *Ptr++ = ATAG_NONE;

  return (UINTN)(Ptr - Out) * 4;
}

STATIC
VOID
FlushL2 (
  VOID
  )
{
  if ((MmioRead32 (PL310_CTRL) & 1) == 0) {
    return;
  }
  MmioWrite32 (PL310_CLEAN_INV_WAY, 0xFFFF);
  while (MmioRead32 (PL310_CLEAN_INV_WAY) & 0xFFFF) {
  }
  MmioWrite32 (PL310_CACHE_SYNC, 0);
  MmioWrite32 (PL310_CTRL, 0);
}

/**
  ExitBootServices, retrying once the memory map changed underneath us.
**/
STATIC
EFI_STATUS
LeaveBootServices (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS             Status;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  Try;

  MapSize = 0;
  gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
  MapSize += 8 * DescSize;   // room for the allocation below
  Map = AllocatePool (MapSize);
  if (Map == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Try = 0; Try < 4; Try++) {
    UINTN  Size = MapSize;

    Status = gBS->GetMemoryMap (&Size, Map, &MapKey, &DescSize, &DescVersion);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gBS->ExitBootServices (ImageHandle, MapKey);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }
  }
  return Status;
}

EFI_STATUS
EFIAPI
LinuxBootMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  EFI_SHELL_PARAMETERS_PROTOCOL  *Params;
  CONST CHAR16                   *Path;
  CHAR8                          *Replace;
  CHAR8                          *Append;
  UINT8                          *Image;
  UINTN                          FileSize;
  UINTN                          KernelSize;
  UINT32                         Start;
  UINT32                         End;
  UINT32                         *Atags;
  UINTN                          AtagsSize;
  CHAR8                          *Cmdline;
  UINT32                         MachType;
  EFI_PHYSICAL_ADDRESS           Region;
  UINTN                          Index;
  EFI_LOADED_IMAGE_PROTOCOL      *Self;

  Path    = L"\\zImage";
  Replace = NULL;
  Append  = NULL;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiShellParametersProtocolGuid,
                                       (VOID **)&Params))) {
    for (Index = 1; Index < Params->Argc; Index++) {
      if ((StrCmp (Params->Argv[Index], L"-c") == 0) && (Index + 1 < Params->Argc)) {
        Replace = AllocatePool (StrLen (Params->Argv[++Index]) + 1);
        UnicodeStrToAsciiStrS (Params->Argv[Index], Replace, StrLen (Params->Argv[Index]) + 1);
      } else if ((StrCmp (Params->Argv[Index], L"-a") == 0) && (Index + 1 < Params->Argc)) {
        Append = AllocatePool (StrLen (Params->Argv[++Index]) + 1);
        UnicodeStrToAsciiStrS (Params->Argv[Index], Append, StrLen (Params->Argv[Index]) + 1);
      } else {
        Path = Params->Argv[Index];
      }
    }
  }

  Status = LoadFromAnyVolume (Path, &Image, &FileSize);
  if (EFI_ERROR (Status)) {
    Print (L"LinuxBoot: %s not found on any FAT volume - %r\n", Path, Status);
    return Status;
  }

  //
  // zImage header: magic at 0x24, start/end of the image at 0x28/0x2C.  A
  // padded partition dump is longer than the image itself.
  //
  if ((FileSize < 0x30) || (*(UINT32 *)(Image + ZIMAGE_MAGIC_OFF) != ZIMAGE_MAGIC)) {
    Print (L"LinuxBoot: %s is not an ARM zImage\n", Path);
    return EFI_UNSUPPORTED;
  }
  Start      = *(UINT32 *)(Image + ZIMAGE_START_OFF);
  End        = *(UINT32 *)(Image + ZIMAGE_END_OFF);
  KernelSize = ((End > Start) && (End - Start <= FileSize)) ? (End - Start) : FileSize;
  if (KERNEL_ADDR + KernelSize > BOOT_PARAMS_BASE) {
    Print (L"LinuxBoot: zImage of %u bytes would overrun the preserved boot params\n",
           KernelSize);
    return EFI_BAD_BUFFER_SIZE;
  }

  Atags   = AllocatePool (ATAGS_MAX);
  Cmdline = AllocatePool (CMDLINE_MAX);
  if ((Atags == NULL) || (Cmdline == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }
  AtagsSize = BuildAtags (Atags, ATAGS_MAX, Replace, Append, Cmdline, &MachType);
  if (AtagsSize == 0) {
    Print (L"LinuxBoot: ATAG list does not fit\n");
    return EFI_BUFFER_TOO_SMALL;
  }

  Print (L"LinuxBoot: kernel %u bytes -> 0x%x, machine type %u, ATAGs %u bytes -> 0x%x\n",
         KernelSize, KERNEL_ADDR, MachType, AtagsSize, ATAGS_ADDR);
  Print (L"LinuxBoot: cmdline \"%a\"\n", Cmdline);

  //
  // Claim the destinations so nothing else is placed there before we leave;
  // if UEFI already uses them, the copy after ExitBootServices still works
  // as long as it is not our own buffers (CopyMem handles overlap).
  //
  Region = ATAGS_REGION;
  if (EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiLoaderData,
                                     EFI_SIZE_TO_PAGES (ATAGS_REGION_SIZE), &Region))) {
    Print (L"LinuxBoot: note: 0x%x is in use by UEFI\n", ATAGS_REGION);
  }
  Region = KERNEL_ADDR;
  if (EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiLoaderData,
                                     EFI_SIZE_TO_PAGES (KernelSize), &Region))) {
    Print (L"LinuxBoot: note: 0x%x is in use by UEFI\n", KERNEL_ADDR);
  }

  //
  // This app keeps running after the copy: its own image must not be in the
  // way.  (Its stack and pool come from the top of RAM, like the image.)
  //
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid,
                                       (VOID **)&Self)) &&
      (Overlaps ((UINTN)Self->ImageBase, (UINTN)Self->ImageSize, KERNEL_ADDR, KernelSize) ||
       Overlaps ((UINTN)Self->ImageBase, (UINTN)Self->ImageSize, ATAGS_REGION, ATAGS_REGION_SIZE))) {
    Print (L"LinuxBoot: this app was loaded at 0x%p, inside the kernel's place\n",
           Self->ImageBase);
    return EFI_ACCESS_DENIED;
  }

  Print (L"LinuxBoot: starting kernel\n");
  gBS->Stall (100000);   // let the USB log ship the lines above

  Status = LeaveBootServices (ImageHandle);
  if (EFI_ERROR (Status)) {
    Print (L"LinuxBoot: ExitBootServices failed - %r\n", Status);
    return Status;
  }

  //
  // Boot services are gone: no allocation, no console from here on.
  //
  CopyMem ((VOID *)(UINTN)ATAGS_ADDR, Atags, AtagsSize);
  CopyMem ((VOID *)(UINTN)KERNEL_ADDR, Image, KernelSize);
  FlushL2 ();
  LinuxJump (KERNEL_ADDR, MachType, ATAGS_ADDR);

  return EFI_ABORTED;   // not reached
}
