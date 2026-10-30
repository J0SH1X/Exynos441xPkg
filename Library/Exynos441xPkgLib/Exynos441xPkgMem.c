/** @file
  Memory map + resource HOBs for Exynos441xPkg.

  Derived from the QSD8250 (HtcLeoPkg) implementation: builds the system
  memory resource HOB, carves out the bootloader's framebuffer and the
  firmware device itself (both live inside DRAM and must never be handed
  out as free memory), and describes the MMU regions used by PrePi.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferConfigLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

#include <ExynosLogRing.h>

// The total number of descriptors, including the final "end-of-table" descriptor.
#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS 12

#define DDR_ATTRIBUTES_CACHED    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define DDR_ATTRIBUTES_UNCACHED  ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED

// Exynos4 address space: boot ROM + peripherals live below the DRAM base.
#define EXYNOS_LOW_DEVICE_BASE  0x00000000
#define EXYNOS_LOW_DEVICE_SIZE  0x40000000

/**
  Remove [Base, Base+Size) from the SYSTEM_MEMORY resource HOB list and
  publish it as EFI_RESOURCE_MEMORY_RESERVED instead.

  PrePi already built a single full-coverage SYSTEM_MEMORY descriptor; this
  splits it around the reserved range (prefix stays, suffix becomes a new
  descriptor if there is one).

  @param[in] Base  Start of the range to carve out.
  @param[in] Size  Size of the range in bytes.
**/
STATIC
VOID
CarveReservedRange (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttributes;
  EFI_PEI_HOB_POINTERS         NextHob;
  UINT64                       ResourceTop;
  EFI_PHYSICAL_ADDRESS         End = Base + Size;

  if ((Size == 0) || (End <= Base)) {
    return;
  }

  ResourceAttributes = (
                        EFI_RESOURCE_ATTRIBUTE_PRESENT |
                        EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
                        EFI_RESOURCE_ATTRIBUTE_TESTED
                        );

  NextHob.Raw = GetHobList ();
  while ((NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, NextHob.Raw)) != NULL) {
    if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
        (Base >= NextHob.ResourceDescriptor->PhysicalStart) &&
        (End <= (NextHob.ResourceDescriptor->PhysicalStart + NextHob.ResourceDescriptor->ResourceLength)))
    {
      ResourceTop = NextHob.ResourceDescriptor->PhysicalStart +
                    NextHob.ResourceDescriptor->ResourceLength;

      // Descriptor starting exactly at the range: shrink it.
      if (Base == NextHob.ResourceDescriptor->PhysicalStart) {
        NextHob.ResourceDescriptor->PhysicalStart = End;
        NextHob.ResourceDescriptor->ResourceLength = ResourceTop - End;
      } else {
        NextHob.ResourceDescriptor->ResourceLength = Base - NextHob.ResourceDescriptor->PhysicalStart;
      }

      // Tail beyond the range becomes its own SYSTEM_MEMORY descriptor.
      if (End < ResourceTop) {
        BuildResourceDescriptorHob (
          EFI_RESOURCE_SYSTEM_MEMORY,
          ResourceAttributes,
          End,
          ResourceTop - End
          );
      }

      BuildResourceDescriptorHob (
        EFI_RESOURCE_MEMORY_RESERVED,
        EFI_RESOURCE_ATTRIBUTE_PRESENT,
        Base,
        Size
        );
      return;
    }

    NextHob.Raw = GET_NEXT_HOB (NextHob);
  }
}

/**
  Return the Virtual Memory Map of your platform.
**/
VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *VirtualMemoryTable;
  UINTN                         Index = 0;
  EXYNOS_FRAMEBUFFER_CONFIG     FbConfig;
  BOOLEAN                       HaveFb = FALSE;
  EFI_PHYSICAL_ADDRESS          FbBase = 0;
  UINT64                        FbSize = 0;

  ASSERT (VirtualMemoryMap != NULL);

  if (ExynosGetFrameBufferConfig (&FbConfig) == RETURN_SUCCESS) {
    FbBase = FbConfig.Base;
    FbSize = (UINT64)FbConfig.Width * FbConfig.Height * FbConfig.BytesPerPixel;
    HaveFb = (FbBase >= PcdGet64 (PcdSystemMemoryBase)) &&
             ((FbBase + FbSize) <= (PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize)));
  }

  //
  // Publish the system memory resource HOB (MemoryPeim skips its own build
  // if a full-coverage SYSTEM_MEMORY HOB already exists), then remove the
  // ranges that must not be handed out as free memory: the bootloader's
  // framebuffer (we print on it) and the firmware device (we run from it).
  //
  {
    EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttributes;

    ResourceAttributes = (
                          EFI_RESOURCE_ATTRIBUTE_PRESENT |
                          EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                          EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
                          EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
                          EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
                          EFI_RESOURCE_ATTRIBUTE_TESTED
                          );

    BuildResourceDescriptorHob (
      EFI_RESOURCE_SYSTEM_MEMORY,
      ResourceAttributes,
      PcdGet64 (PcdSystemMemoryBase),
      PcdGet64 (PcdSystemMemorySize)
      );
  }

  CarveReservedRange (
    (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdFdBaseAddress),
    (UINT64)PcdGet32 (PcdFdSize)
    );

  if (HaveFb) {
    CarveReservedRange (FbBase, FbSize);
  }

  //
  // Boot log ring (ExynosLogRing.h), written since PrePi entry.
  //
  CarveReservedRange (LOG_RING_BASE, LOG_RING_SIZE);

  VirtualMemoryTable = AllocatePages (
                         EFI_SIZE_TO_PAGES (sizeof (ARM_MEMORY_REGION_DESCRIPTOR) * MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS)
                         );
  if (VirtualMemoryTable == NULL) {
    return;
  }

  // System DRAM (cached)
  VirtualMemoryTable[Index].PhysicalBase = PcdGet64 (PcdSystemMemoryBase);
  VirtualMemoryTable[Index].VirtualBase  = VirtualMemoryTable[Index].PhysicalBase;
  VirtualMemoryTable[Index].Length       = PcdGet64 (PcdSystemMemorySize);
  VirtualMemoryTable[Index].Attributes   = DDR_ATTRIBUTES_CACHED;
  Index++;

  // Low memory: boot ROM + SoC peripherals (0x0 .. DRAM base)
  VirtualMemoryTable[Index].PhysicalBase = EXYNOS_LOW_DEVICE_BASE;
  VirtualMemoryTable[Index].VirtualBase  = EXYNOS_LOW_DEVICE_BASE;
  VirtualMemoryTable[Index].Length       = EXYNOS_LOW_DEVICE_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  Index++;

  // Framebuffer: uncached so DEBUG output appears immediately.
  if (HaveFb) {
    VirtualMemoryTable[Index].PhysicalBase = FbBase;
    VirtualMemoryTable[Index].VirtualBase  = FbBase;
    VirtualMemoryTable[Index].Length       = FbSize;
    VirtualMemoryTable[Index].Attributes   = DDR_ATTRIBUTES_UNCACHED;
    Index++;
  }

  // End of Table
  VirtualMemoryTable[Index].PhysicalBase = 0;
  VirtualMemoryTable[Index].VirtualBase  = 0;
  VirtualMemoryTable[Index].Length       = 0;
  VirtualMemoryTable[Index].Attributes   = (ARM_MEMORY_REGION_ATTRIBUTES)0;
  Index++;

  ASSERT ((Index + 1) <= MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);
  *VirtualMemoryMap = VirtualMemoryTable;
}
