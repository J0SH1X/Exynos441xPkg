/** @file
  Boot a downstream ARM Linux zImage (Samsung GT-N7000 kernel) from UEFI.

    LinuxBoot.efi [-r | -n] [-c "cmdline"] [-a "text"] [-i \path\initrd] [\path\zImage]

  Searches every FAT file system for the zImage (default \zImage), then
  boots it the way the Samsung bootloader would have: zImage at 0x40008000
  (zreladdr), ATAGs at 0x40000100, r0 = 0, r1 = machine type, r2 = ATAGs.
  The ATAGs are rebuilt from the bootloader's own list that BootShim/PrePi
  preserved (ExynosBootParams.h): memory banks, serial, revision and the
  command line, which carries settings the kernel depends on
  (s3cfb.bootloaderfb, lcdtype, sec_log, ...).  -c replaces the command
  line, -a appends to it.  -i passes an initrd (ATAG_INITRD2) that the
  kernel unpacks over its built-in initramfs.

  Dual-ramdisk kernels (Samsung/CF-Root style, e.g. PhilZ) are detected from
  the image itself: a "BOOT_IMAGE_OFFSETS" line after the zImage locates
  xz'd tar archives for the boot and recovery ramdisks.  Their init would
  read them back from the KERNEL partition, which holds UEFI here, so the
  chosen archive is unpacked into a cpio initrd instead.  A real boot archive
  means a normal boot; a placeholder one (1 block) means a recovery kernel,
  booted with "bootmode=2" like the Samsung bootloader does.  -r / -n force
  recovery / normal; -i and -c override the automatic initrd / bootmode.

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
#include <Library/PrintLib.h>
#include <Library/XzDecLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/SimpleFileSystem.h>

#include <ExynosBootParams.h>

#define KERNEL_ADDR        0x40008000U   // zreladdr (mach-exynos/Makefile.boot)
#define INITRD_ADDR        0x42000000U   // well above the decompressed kernel
#define INITRD_MAX         0x04000000U
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

//
// ---------------------------------------------------------------------------
// Dual-ramdisk zImages: BOOT_IMAGE_OFFSETS + xz'd tar -> cpio initrd.
// ---------------------------------------------------------------------------
//

typedef struct {
  BOOLEAN  Found;
  UINTN    BootOffset;      // in 512-byte blocks from the image start
  UINTN    BootLen;
  UINTN    RecoveryOffset;
  UINTN    RecoveryLen;
} BOOT_IMAGE_OFFSETS;

/**
  Find and parse "BOOT_IMAGE_OFFSETS<sep>boot_offset=N;boot_len=N;
  recovery_offset=N;recovery_len=N;" right after the zImage.
**/
STATIC
VOID
FindBootImageOffsets (
  IN  CONST UINT8         *Image,
  IN  UINTN               FileSize,
  IN  UINTN               KernelSize,
  OUT BOOT_IMAGE_OFFSETS  *Offsets
  )
{
  STATIC CONST CHAR8  Marker[] = "BOOT_IMAGE_OFFSETS";
  CONST CHAR8         *P;
  CONST CHAR8         *End;
  UINTN               Limit;
  UINTN               Pos;
  UINTN               *Field;
  UINTN               Value;

  ZeroMem (Offsets, sizeof (*Offsets));

  //
  // The marker sits in the first block after the zImage; search 64 KiB.
  //
  Limit = MIN (FileSize, KernelSize + SIZE_64KB);
  for (Pos = KernelSize; Pos + sizeof (Marker) < Limit; Pos++) {
    if (CompareMem (Image + Pos, Marker, sizeof (Marker) - 1) == 0) {
      break;
    }
  }
  if (Pos + sizeof (Marker) >= Limit) {
    return;
  }

  P   = (CONST CHAR8 *)Image + Pos + sizeof (Marker) - 1;
  End = (CONST CHAR8 *)Image + Limit;
  while ((P < End) && ((*P < ' ') || (*P > '~'))) {
    P++;                                    // separator byte(s)
  }

  while ((P < End) && (*P >= ' ') && (*P <= '~')) {
    if (AsciiStrnCmp (P, "boot_offset=", 12) == 0) {
      Field = &Offsets->BootOffset;     P += 12;
    } else if (AsciiStrnCmp (P, "boot_len=", 9) == 0) {
      Field = &Offsets->BootLen;        P += 9;
    } else if (AsciiStrnCmp (P, "recovery_offset=", 16) == 0) {
      Field = &Offsets->RecoveryOffset; P += 16;
    } else if (AsciiStrnCmp (P, "recovery_len=", 13) == 0) {
      Field = &Offsets->RecoveryLen;    P += 13;
    } else {
      Field = NULL;
    }
    for (Value = 0; (P < End) && (*P >= '0') && (*P <= '9'); P++) {
      Value = Value * 10 + (*P - '0');
    }
    if (Field != NULL) {
      *Field = Value;
    }
    while ((P < End) && (*P != ';') && (*P >= ' ') && (*P <= '~')) {
      P++;
    }
    if ((P < End) && (*P == ';')) {
      P++;
    }
  }

  Offsets->Found = (Offsets->RecoveryLen != 0) || (Offsets->BootLen != 0);
}

STATIC
UINTN
TarOctal (
  IN CONST UINT8  *Field,
  IN UINTN        Length
  )
{
  UINTN  Value;
  UINTN  Index;

  Value = 0;
  for (Index = 0; Index < Length; Index++) {
    if ((Field[Index] >= '0') && (Field[Index] <= '7')) {
      Value = Value * 8 + (Field[Index] - '0');
    } else if (Value != 0) {
      break;
    }
  }
  return Value;
}

/**
  Append one newc entry; Out has room (the caller sized it).
**/
STATIC
UINTN
CpioEntry (
  OUT UINT8        *Out,
  IN  UINT32       Ino,
  IN  UINT32       Mode,
  IN  UINT32       Uid,
  IN  UINT32       Gid,
  IN  UINT32       Mtime,
  IN  CONST CHAR8  *Name,
  IN  CONST VOID   *Data,
  IN  UINTN        DataSize
  )
{
  UINTN  Used;
  UINTN  NameSize;

  NameSize = AsciiStrLen (Name) + 1;
  Used     = AsciiSPrint ((CHAR8 *)Out, 111,
                          "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X",
                          Ino, Mode, Uid, Gid, (Mode & 0170000) == 0040000 ? 2 : 1,
                          Mtime, (UINT32)DataSize, 0, 0, 0, 0, (UINT32)NameSize, 0);
  CopyMem (Out + Used, Name, NameSize);
  Used += NameSize;
  while ((Used & 3) != 0) {
    Out[Used++] = 0;
  }
  if (DataSize != 0) {
    CopyMem (Out + Used, Data, DataSize);
    Used += DataSize;
    while ((Used & 3) != 0) {
      Out[Used++] = 0;
    }
  }
  return Used;
}

/**
  ustar (with GNU long names) -> uncompressed newc cpio, the format the
  kernel unpacks as an initramfs.  Directories, regular files and symlinks
  keep their mode and owner; other entry types are skipped.
**/
STATIC
EFI_STATUS
TarToCpio (
  IN  CONST UINT8  *Tar,
  IN  UINTN        TarSize,
  OUT UINT8        **Cpio,
  OUT UINTN        *CpioSize,
  OUT UINTN        *Entries
  )
{
  STATIC CHAR8  Name[512];
  STATIC CHAR8  LongName[512];
  CONST UINT8   *Hdr;
  UINT8         *Out;
  UINTN         Pos;
  UINTN         Used;
  UINTN         Size;
  UINT32        Mode;
  UINT32        Ino;
  CHAR8         Type;
  CONST CHAR8   *N;

  //
  // Every tar entry spends >= 512 bytes on its header, a cpio entry < 400
  // plus the same data, so TarSize + slack always fits.
  //
  Out = AllocatePool (TarSize + SIZE_4KB);
  if (Out == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Used        = 0;
  Ino         = 0;
  LongName[0] = '\0';
  for (Pos = 0; Pos + 512 <= TarSize; Pos += 512 + ALIGN_VALUE (Size, 512)) {
    Hdr  = Tar + Pos;
    Size = 0;
    if (IsZeroBuffer (Hdr, 512)) {
      break;                                        // end-of-archive blocks
    }
    Size = TarOctal (Hdr + 124, 12);
    Type = (CHAR8)Hdr[156];
    if (Pos + 512 + Size > TarSize) {
      FreePool (Out);
      return EFI_COMPROMISED_DATA;
    }

    if (Type == 'L') {                              // GNU long name for the next entry
      AsciiStrnCpyS (LongName, sizeof (LongName), (CONST CHAR8 *)Hdr + 512,
                     MIN (Size, sizeof (LongName) - 1));
      continue;
    }

    if (LongName[0] != '\0') {
      AsciiStrCpyS (Name, sizeof (Name), LongName);
      LongName[0] = '\0';
    } else if ((CompareMem (Hdr + 257, "ustar", 5) == 0) && (Hdr[345] != 0)) {
      AsciiStrnCpyS (Name, sizeof (Name), (CONST CHAR8 *)Hdr + 345, 155);   // prefix
      AsciiStrCatS (Name, sizeof (Name), "/");
      AsciiStrnCatS (Name, sizeof (Name), (CONST CHAR8 *)Hdr, 100);
    } else {
      AsciiStrnCpyS (Name, sizeof (Name), (CONST CHAR8 *)Hdr, 100);
    }

    N = Name;
    while ((N[0] == '.') && (N[1] == '/')) {
      N += 2;
    }
    while (N[0] == '/') {
      N++;
    }
    Size = (Type == '5') ? 0 : Size;
    if (N[0] == '\0') {
      continue;
    }
    if (AsciiStrLen (N) > 0 && N[AsciiStrLen (N) - 1] == '/') {
      ((CHAR8 *)N)[AsciiStrLen (N) - 1] = '\0';
    }

    Mode = (UINT32)TarOctal (Hdr + 100, 8) & 07777;
    switch (Type) {
      case '0':
      case '\0':
        Used += CpioEntry (Out + Used, ++Ino, 0100000 | Mode, (UINT32)TarOctal (Hdr + 108, 8),
                           (UINT32)TarOctal (Hdr + 116, 8), (UINT32)TarOctal (Hdr + 136, 12),
                           N, Hdr + 512, Size);
        break;
      case '5':
        Used += CpioEntry (Out + Used, ++Ino, 0040000 | Mode, (UINT32)TarOctal (Hdr + 108, 8),
                           (UINT32)TarOctal (Hdr + 116, 8), (UINT32)TarOctal (Hdr + 136, 12),
                           N, NULL, 0);
        break;
      case '2':
        Used += CpioEntry (Out + Used, ++Ino, 0120777, (UINT32)TarOctal (Hdr + 108, 8),
                           (UINT32)TarOctal (Hdr + 116, 8), (UINT32)TarOctal (Hdr + 136, 12),
                           N, Hdr + 157, AsciiStrnLenS ((CONST CHAR8 *)Hdr + 157, 100));
        break;
      default:
        continue;                                   // hard links, devices, ...
    }
  }

  Used     += CpioEntry (Out + Used, 0, 0, 0, 0, 0, "TRAILER!!!", NULL, 0);
  *Cpio     = Out;
  *CpioSize = Used;
  *Entries  = Ino;
  return EFI_SUCCESS;
}

/**
  Unpack one of the image's appended archives into a cpio initrd.
**/
STATIC
EFI_STATUS
DualRamdiskInitrd (
  IN  CONST UINT8  *Image,
  IN  UINTN        FileSize,
  IN  UINTN        OffsetBlocks,
  IN  UINTN        LenBlocks,
  OUT UINT8        **Initrd,
  OUT UINTN        *InitrdSize
  )
{
  EFI_STATUS  Status;
  VOID        *Tar;
  UINTN       TarSize;
  UINTN       Entries;

  if ((OffsetBlocks * 512 + LenBlocks * 512 > FileSize) || (LenBlocks == 0)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = XzDecompress (Image + OffsetBlocks * 512, LenBlocks * 512, &Tar, &TarSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = TarToCpio (Tar, TarSize, Initrd, InitrdSize, &Entries);
  FreePool (Tar);
  if (!EFI_ERROR (Status)) {
    Print (L"LinuxBoot: ramdisk %u bytes xz -> %u bytes tar -> %u bytes cpio (%u entries)\n",
           LenBlocks * 512, TarSize, *InitrdSize, Entries);
  }
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
  IN  UINTN         InitrdSize,
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

  if (InitrdSize != 0) {
    if ((UINTN)(Ptr - Out + 4) * 4 > OutSize) {
      return 0;
    }
    *Ptr++ = 4;
    *Ptr++ = ATAG_INITRD2;
    *Ptr++ = INITRD_ADDR;
    *Ptr++ = (UINT32)InitrdSize;
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
  CONST CHAR16                   *InitrdPath;
  BOOT_IMAGE_OFFSETS             Offsets;
  INTN                           Force;          // 1 recovery, -1 normal, 0 auto
  BOOLEAN                        Recovery;
  CHAR8                          *AutoAppend;
  UINT8                          *Initrd;
  UINTN                          InitrdSize;
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
  Replace    = NULL;
  Append     = NULL;
  InitrdPath = NULL;
  Initrd     = NULL;
  InitrdSize = 0;
  Force      = 0;
  AutoAppend = NULL;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiShellParametersProtocolGuid,
                                       (VOID **)&Params))) {
    for (Index = 1; Index < Params->Argc; Index++) {
      if ((StrCmp (Params->Argv[Index], L"-c") == 0) && (Index + 1 < Params->Argc)) {
        Replace = AllocatePool (StrLen (Params->Argv[++Index]) + 1);
        UnicodeStrToAsciiStrS (Params->Argv[Index], Replace, StrLen (Params->Argv[Index]) + 1);
      } else if (StrCmp (Params->Argv[Index], L"-r") == 0) {
        Force = 1;
      } else if (StrCmp (Params->Argv[Index], L"-n") == 0) {
        Force = -1;
      } else if ((StrCmp (Params->Argv[Index], L"-i") == 0) && (Index + 1 < Params->Argc)) {
        InitrdPath = Params->Argv[++Index];
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

  //
  // Dual-ramdisk image?  Pick the archive, unpack it, set bootmode.
  //
  FindBootImageOffsets (Image, FileSize, KernelSize, &Offsets);
  if (Offsets.Found) {
    Recovery = (Force > 0) || ((Force == 0) && (Offsets.BootLen <= 1) && (Offsets.RecoveryLen > 1));
    Print (L"LinuxBoot: dual-ramdisk image (boot %u, recovery %u blocks): %s boot\n",
           Offsets.BootLen, Offsets.RecoveryLen, Recovery ? L"recovery" : L"normal");

    if (InitrdPath == NULL) {
      Status = DualRamdiskInitrd (Image, FileSize,
                                  Recovery ? Offsets.RecoveryOffset : Offsets.BootOffset,
                                  Recovery ? Offsets.RecoveryLen : Offsets.BootLen,
                                  &Initrd, &InitrdSize);
      if (EFI_ERROR (Status)) {
        Print (L"LinuxBoot: could not unpack the %s ramdisk - %r\n",
               Recovery ? L"recovery" : L"boot", Status);
        return Status;
      }
    }

    //
    // The Samsung bootloader adds bootmode=2 for recovery; -c means the
    // caller wrote the whole command line.
    //
    if (Recovery && (Replace == NULL)) {
      AutoAppend = AllocatePool (CMDLINE_MAX);
      if (AutoAppend == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      AsciiStrCpyS (AutoAppend, CMDLINE_MAX, "bootmode=2");
      if (Append != NULL) {
        AsciiStrCatS (AutoAppend, CMDLINE_MAX, " ");
        AsciiStrCatS (AutoAppend, CMDLINE_MAX, Append);
      }
      Append = AutoAppend;
    }
  } else if (Force > 0) {
    Print (L"LinuxBoot: -r given but %s has no BOOT_IMAGE_OFFSETS; booting it as is\n", Path);
  }

  if (InitrdPath != NULL) {
    Status = LoadFromAnyVolume (InitrdPath, &Initrd, &InitrdSize);
    if (EFI_ERROR (Status)) {
      Print (L"LinuxBoot: initrd %s not found on any FAT volume - %r\n", InitrdPath, Status);
      return Status;
    }
  }
  if ((Initrd != NULL) && ((InitrdSize == 0) || (InitrdSize > INITRD_MAX))) {
    Print (L"LinuxBoot: initrd of %u bytes is not usable\n", InitrdSize);
    return EFI_BAD_BUFFER_SIZE;
  }

  Atags   = AllocatePool (ATAGS_MAX);
  Cmdline = AllocatePool (CMDLINE_MAX);
  if ((Atags == NULL) || (Cmdline == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }
  AtagsSize = BuildAtags (Atags, ATAGS_MAX, Replace, Append, InitrdSize, Cmdline, &MachType);
  if (AtagsSize == 0) {
    Print (L"LinuxBoot: ATAG list does not fit\n");
    return EFI_BUFFER_TOO_SMALL;
  }

  Print (L"LinuxBoot: kernel %u bytes -> 0x%x, machine type %u, ATAGs %u bytes -> 0x%x\n",
         KernelSize, KERNEL_ADDR, MachType, AtagsSize, ATAGS_ADDR);
  Print (L"LinuxBoot: cmdline \"%a\"\n", Cmdline);
  if (InitrdSize != 0) {
    Print (L"LinuxBoot: initrd %u bytes -> 0x%x\n", InitrdSize, INITRD_ADDR);
  }

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
  Region = INITRD_ADDR;
  if ((InitrdSize != 0) &&
      EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiLoaderData,
                                     EFI_SIZE_TO_PAGES (InitrdSize), &Region))) {
    Print (L"LinuxBoot: note: 0x%x is in use by UEFI\n", INITRD_ADDR);
  }

  //
  // This app keeps running after the copy: its own image must not be in the
  // way.  (Its stack and pool come from the top of RAM, like the image.)
  //
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid,
                                       (VOID **)&Self)) &&
      (Overlaps ((UINTN)Self->ImageBase, (UINTN)Self->ImageSize, KERNEL_ADDR, KernelSize) ||
       Overlaps ((UINTN)Self->ImageBase, (UINTN)Self->ImageSize, ATAGS_REGION, ATAGS_REGION_SIZE) ||
       Overlaps ((UINTN)Self->ImageBase, (UINTN)Self->ImageSize, INITRD_ADDR, InitrdSize))) {
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
  if (InitrdSize != 0) {
    CopyMem ((VOID *)(UINTN)INITRD_ADDR, Initrd, InitrdSize);
  }
  CopyMem ((VOID *)(UINTN)KERNEL_ADDR, Image, KernelSize);
  FlushL2 ();
  LinuxJump (KERNEL_ADDR, MachType, ATAGS_ADDR);

  return EFI_ABORTED;   // not reached
}
