/** @file

  Copyright (c) 2011-2017, ARM Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiPei.h>

#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugAgentLib.h>
#include <Library/PrePiLib.h>
#include <Library/PrintLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/TimerLib.h>
#include <Library/PerformanceLib.h>
#include <Library/FrameBufferConfigLib.h>

#include <Ppi/GuidedSectionExtraction.h>
#include <Ppi/ArmMpCoreInfo.h>
#include <Ppi/SecPerformance.h>

#include "PrePi.h"

#include <ExynosBootParams.h>
#include <ExynosLogRing.h>

STATIC
VOID
BootParamsLog (
  VOID
  );

UINT64  mSystemMemoryEnd = FixedPcdGet64 (PcdSystemMemoryBase) +
                           FixedPcdGet64 (PcdSystemMemorySize) - 1;

/*UINTN Width = FixedPcdGet32(PcdFrameBufferWidth);
UINTN Height = FixedPcdGet32(PcdFrameBufferHeight);
UINTN Bpp = FixedPcdGet32(PcdFrameBufferPixelBpp);
UINTN FbAddr = FixedPcdGet32(PcdFrameBufferAddress);*/

VOID
EnableCounter()
{
  /* enable cp10 and cp11 */
	UINT32 val;
	__asm__ volatile("mrc	p15, 0, %0, c1, c0, 2" : "=r" (val));
	val |= (3<<22)|(3<<20);
	__asm__ volatile("mcr	p15, 0, %0, c1, c0, 2" :: "r" (val));

  ArmInstructionSynchronizationBarrier();
  ArmDataMemoryBarrier();

	/* set enable bit in fpexc */
	__asm__ volatile("mrc  p10, 7, %0, c8, c0, 0" : "=r" (val));
	val |= (1<<30);
	__asm__ volatile("mcr  p10, 7, %0, c8, c0, 0" :: "r" (val));

  /* enable the cycle count register */
	UINT32 en;
	__asm__ volatile("mrc	p15, 0, %0, c9, c12, 0" : "=r" (en));
	en &= ~(1<<3); /* cycle count every cycle */
	en |= 1; /* enable all performance counters */
	__asm__ volatile("mcr	p15, 0, %0, c9, c12, 0" :: "r" (en));

	/* enable cycle counter */
	en = (1<<31);
	__asm__ volatile("mcr	p15, 0, %0, c9, c12, 1" :: "r" (en));
}

/**
  SEC main routine.
  @param[in]  UefiMemoryBase  Start of the PI/UEFI memory region
  @param[in]  StacksBase      Start of the stack
  @param[in]  StartTimeStamp  Timer value at start of execution
**/
STATIC
VOID
PrePiMain (
  IN  UINTN   UefiMemoryBase,
  IN  UINTN   StacksBase,
  IN  UINT64  StartTimeStamp
  )
{
  EFI_HOB_HANDOFF_INFO_TABLE  *HobList;
  EFI_STATUS                  Status;
  CHAR8                       Buffer[100];
  UINTN                       CharCount;
  UINTN                       StacksSize;
  FIRMWARE_SEC_PERFORMANCE    Performance;

  // Initialize the architecture specific bits
  ArchInitialize ();

  // Find the framebuffer the Samsung bootloader programmed (FIMD registers).
  // Nothing is reconfigured: we only read back what the BL set up.
  ReconfigFb(FixedPcdGet32(PcdFrameBufferPixelBpp));

  // Enable the cycle counter (code from PrimeG2Pkg)
  EnableCounter();

  // Initialize the Serial Port
  SerialPortInitialize ();

  CharCount = AsciiSPrint (
                Buffer,
                sizeof (Buffer),
                "Exynos441xPkg UEFI (version %s built at %a on %a)\n\r",
                (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString),
                __TIME__,
                __DATE__
                );
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  BootParamsLog ();

  DEBUG((
        EFI_D_INFO | EFI_D_LOAD,
        "UEFI Memory Base = 0x%p, Stack Base = 0x%p\n",
        UefiMemoryBase,
        StacksBase
    ));

  // Initialize the Debug Agent for Source Level Debugging
  InitializeDebugAgent (DEBUG_AGENT_INIT_POSTMEM_SEC, NULL, NULL);
  SaveAndSetDebugTimerInterrupt (TRUE);

  // Declare the PI/UEFI memory region
  HobList = HobConstructor (
              (VOID *)UefiMemoryBase,
              FixedPcdGet32 (PcdSystemMemoryUefiRegionSize),
              (VOID *)UefiMemoryBase,
              (VOID *)StacksBase // The top of the UEFI Memory is reserved for the stacks
              );
  PrePeiSetHobList (HobList);

  // Initialize MMU and Memory HOBs (Resource Descriptor HOBs)
  Status = MemoryPeim (UefiMemoryBase, FixedPcdGet32 (PcdSystemMemoryUefiRegionSize));
  ASSERT_EFI_ERROR (Status);

  // Create the Stacks HOB
  StacksSize = PcdGet32 (PcdCPUCorePrimaryStackSize);

  BuildStackHob (StacksBase, StacksSize);

  // TODO: Call CpuPei as a library
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));

  // Store timer value logged at the beginning of firmware image execution
  Performance.ResetEnd = GetTimeInNanoSecond (StartTimeStamp);

  // Build SEC Performance Data Hob
  BuildGuidDataHob (&gEfiFirmwarePerformanceGuid, &Performance, sizeof (Performance));

  // Set the Boot Mode
  SetBootMode (ArmPlatformGetBootMode ());

  // Initialize Platform HOBs (CpuHob and FvHob)
  Status = PlatformPeim ();
  ASSERT_EFI_ERROR (Status);

  // Now, the HOB List has been initialized, we can register performance information
  PERF_START (NULL, "PEI", NULL, StartTimeStamp);

  // SEC phase needs to run library constructors by hand.
  ProcessLibraryConstructorList ();

  // Assume the FV that contains the SEC (our code) also contains a compressed FV.
  Status = DecompressFirstFv ();
  ASSERT_EFI_ERROR (Status);

  // Load the DXE Core and transfer control to it
  Status = LoadDxeCoreFromFv (NULL, 0);
  ASSERT_EFI_ERROR (Status);
}

/**
  Copy the bootloader's ATAG list behind the r1/r2 the BootShim saved
  (ExynosBootParams.h), before anything can reuse that memory.  Leaves
  AtagsSize 0 unless the list starts with ATAG_CORE, ends with ATAG_NONE
  and fits.
**/
STATIC
VOID
BootParamsCapture (
  VOID
  )
{
  EXYNOS_BOOT_PARAMS  *Bp;
  ATAG_HEADER         *Tag;
  UINT64              MemBase;
  UINT64              MemTop;
  UINTN               Size;

  Bp = BOOT_PARAMS;
  if (Bp->Magic != BOOT_PARAMS_MAGIC) {
    return;
  }
  Bp->AtagsSize = 0;

  MemBase = PcdGet64 (PcdSystemMemoryBase);
  MemTop  = MemBase + PcdGet64 (PcdSystemMemorySize);
  if ((Bp->AtagsBase < MemBase) || (Bp->AtagsBase + 8 > MemTop) ||
      ((Bp->AtagsBase & 3) != 0)) {
    return;
  }

  Tag = (ATAG_HEADER *)(UINTN)Bp->AtagsBase;
  if (Tag->Tag != ATAG_CORE) {
    return;
  }

  Size = 0;
  for ( ; ; ) {
    if (Tag->Size == 0) {
      Size += sizeof (ATAG_HEADER);   // ATAG_NONE
      break;
    }
    if ((Tag->Size < 2) ||
        (Size + Tag->Size * 4 + sizeof (ATAG_HEADER) > BOOT_PARAMS_ATAGS_MAX) ||
        (Bp->AtagsBase + Size + Tag->Size * 4 + 8 > MemTop)) {
      return;
    }
    Size += Tag->Size * 4;
    Tag   = ATAG_NEXT (Tag);
  }

  CopyMem (Bp->Atags, (VOID *)(UINTN)Bp->AtagsBase, Size);
  Bp->AtagsSize = (UINT32)Size;
}

/**
  Log what the bootloader passed (machine type, parsed ATAGs).
**/
STATIC
VOID
BootParamsLog (
  VOID
  )
{
  EXYNOS_BOOT_PARAMS  *Bp;
  ATAG_HEADER         *Tag;
  CHAR8               Chunk[121];
  CONST CHAR8         *Cmd;
  UINTN               Len;

  Bp = BOOT_PARAMS;
  if (Bp->Magic != BOOT_PARAMS_MAGIC) {
    DEBUG ((DEBUG_ERROR, "Bootloader params: none (BootShim did not save r1/r2)\n"));
    return;
  }

  DEBUG ((DEBUG_INFO, "Bootloader: machine type %u, ATAGs at 0x%x, %u bytes preserved\n",
          Bp->MachType, Bp->AtagsBase, Bp->AtagsSize));
  if (Bp->AtagsSize == 0) {
    DEBUG ((DEBUG_ERROR, "Bootloader: no valid ATAG list at 0x%x\n", Bp->AtagsBase));
    return;
  }

  for (Tag = (ATAG_HEADER *)Bp->Atags; Tag->Size != 0; Tag = ATAG_NEXT (Tag)) {
    switch (Tag->Tag) {
      case ATAG_CORE:
        DEBUG ((DEBUG_INFO, "  ATAG_CORE\n"));
        break;
      case ATAG_MEM:
        DEBUG ((DEBUG_INFO, "  ATAG_MEM     0x%08x bytes @ 0x%08x\n",
                ((ATAG_MEM_DATA *)Tag)->Size, ((ATAG_MEM_DATA *)Tag)->Start));
        break;
      case ATAG_CMDLINE:
        //
        // Longer than one DEBUG() line can hold: print in pieces.
        //
        Cmd = ((ATAG_CMDLINE_DATA *)Tag)->Cmdline;
        Len = AsciiStrnLenS (Cmd, (Tag->Size - 2) * 4);
        DEBUG ((DEBUG_INFO, "  ATAG_CMDLINE (%u chars):\n", Len));
        while (Len > 0) {
          AsciiStrnCpyS (Chunk, sizeof (Chunk), Cmd, MIN (Len, sizeof (Chunk) - 1));
          DEBUG ((DEBUG_INFO, "    %a\n", Chunk));
          Cmd += MIN (Len, sizeof (Chunk) - 1);
          Len -= MIN (Len, sizeof (Chunk) - 1);
        }
        break;
      default:
        //
        // Payload words only (Size includes the 2-word header), up to 3.
        //
        DEBUG ((DEBUG_INFO, "  ATAG 0x%08x  %u words:", Tag->Tag, Tag->Size));
        for (Len = 2; (Len < Tag->Size) && (Len < 5); Len++) {
          DEBUG ((DEBUG_INFO, " %08x", ((UINT32 *)Tag)[Len]));
        }
        DEBUG ((DEBUG_INFO, "\n"));
        break;
    }
  }
}

VOID
CEntryPoint (
  IN  UINTN  MpId,
  IN  UINTN  UefiMemoryBase,
  IN  UINTN  StacksBase
  )
{
  UINT64  StartTimeStamp;

  PaintScreen (0);

  //
  // Fresh boot log before the first DEBUG() (UsbLogDxe ships it to a host).
  //
  LogRingReset ();

  //
  // The bootloader's ATAG list, before anything can reuse its memory.
  //
  BootParamsCapture ();

  // Initialize the platform specific controllers
  ArmPlatformInitialize (MpId);

  StartTimeStamp = 0;

  // Data Cache enabled on Primary core when MMU is enabled.
  ArmDisableDataCache ();
  // Invalidate instruction cache
  ArmInvalidateInstructionCache ();
  // Enable Instruction Caches on all cores.
  ArmEnableInstructionCache ();

  // NOTE: upstream parks secondary cores here with ArmCallWFE().  The
  // Samsung bootloader enters the payload on a core that does not pass
  // the ArmPlatformIsPrimaryCore() test, so parking would stall the only
  // running CPU.  SMP bring-up comes later.

  PrePiMain (UefiMemoryBase, StacksBase, StartTimeStamp);

  // DXE Core should always load and never return
  ASSERT (FALSE);
}