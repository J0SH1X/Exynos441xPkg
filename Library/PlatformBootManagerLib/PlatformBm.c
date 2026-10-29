/** @file
  Minimal PlatformBootManagerLib for Exynos441xPkg (milestone 1).

  Sets up text consoles, registers the UEFI Shell as a boot option, then
  lets BDS boot normally.  Note the boot timeout PCD is 0 until the MCT
  timer driver lands (milestone 2): without a timer architectural protocol
  a BDS wait loop would block on a timer event that can never fire.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>

#include <Guid/EventGroup.h>

/**
  Locate all handles carrying ProtocolGuid, pass each to Process.
**/
STATIC
VOID
FilterAndProcess (
  IN EFI_GUID  *ProtocolGuid,
  IN VOID      (*Process)(EFI_HANDLE Handle)
  )
{
  EFI_STATUS   Status;
  EFI_HANDLE   *Handles;
  UINTN        NoHandles;
  UINTN        Idx;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol, ProtocolGuid, NULL, &NoHandles, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Idx = 0; Idx < NoHandles; Idx++) {
    Process (Handles[Idx]);
  }

  gBS->FreePool (Handles);
}

STATIC
VOID
EFIAPI
AddOutput (
  IN EFI_HANDLE  Handle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  DevicePath = DevicePathFromHandle (Handle);
  if (DevicePath == NULL) {
    return;
  }

  EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, DevicePath, NULL);
}

STATIC
VOID
EFIAPI
AddInput (
  IN EFI_HANDLE  Handle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  DevicePath = DevicePathFromHandle (Handle);
  if (DevicePath == NULL) {
    return;
  }

  EfiBootManagerUpdateConsoleVariable (ConIn, DevicePath, NULL);
}

/**
  Add a boot option pointing at a file inside our own firmware volume.

  @return The boot option number, or 0 if the file was not found.
**/
STATIC
UINTN
AddBootOptionForFile (
  IN CONST CHAR16  *Description,
  IN EFI_GUID      *FileGuid
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL         *DevicePath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH FileNode;
  EFI_BOOT_MANAGER_LOAD_OPTION     NewOption;
  EFI_HANDLE                       ImageHandle = NULL;

  // The FV that contains this driver (BdsDxe) also contains the Shell.
  Status = gBS->HandleProtocol (
                  gImageHandle, &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
  DevicePath = AppendDevicePathNode (
                 DevicePathFromHandle (LoadedImage->DeviceHandle),
                 (EFI_DEVICE_PATH_PROTOCOL *)&FileNode
                 );
  if (DevicePath == NULL) {
    return 0;
  }

  // Only register the option if the file really is there: LoadImage fails
  // cleanly when the FV file does not exist.
  Status = gBS->LoadImage (FALSE, NULL, DevicePath, NULL, 0, &ImageHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: %s not present: %r\n", __FUNCTION__, Description,
            Status));
    FreePool (DevicePath);
    return 0;
  }

  gBS->UnloadImage (ImageHandle);

  Status = EfiBootManagerInitializeLoadOption (
             &NewOption,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             (CHAR16 *)Description,
             DevicePath,
             NULL,
             0
             );
  FreePool (DevicePath);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  Status = EfiBootManagerAddLoadOptionVariable (&NewOption, 0);
  DEBUG ((DEBUG_INFO, "%a: added %s: %r\n", __FUNCTION__, Description, Status));
  {
    UINTN OptionNumber = NewOption.OptionNumber;

    EfiBootManagerFreeLoadOption (&NewOption);
    return EFI_ERROR (Status) ? 0 : OptionNumber;
  }
}

/**
  Callback invoked each second while BDS counts down the boot timeout.
**/
VOID
EFIAPI
PlatformBootManagerWaitCallback (
  IN UINT16  TimeoutRemain
  )
{
  DEBUG ((DEBUG_INFO, "Boot in %u second(s)...\n", TimeoutRemain));
}

/**
  Called when BDS could not find anything to boot.
**/
VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "%a: no boot option succeeded\n", __FUNCTION__));
  //
  // Nothing else to do on this platform (no recovery media yet): spin so the
  // message stays on the screen instead of rebooting into a loop.
  //
  for (;;) {
  }
}

/**
  Platform boot manager before the console is initialized.
**/
VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  //
  // Nothing to do: SimpleFbDxe publishes the bootloader's framebuffer as a
  // GOP before BDS runs, and DEBUG() output goes through SerialPortLib
  // (the framebuffer console) from PrePi onwards.
  //
}

/**
  Platform boot manager after the console is initialized.
**/
VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT16      Timeout;

  // Wire up every SimpleTextOutput/TextInput we can find.
  FilterAndProcess (&gEfiSimpleTextOutProtocolGuid, AddOutput);
  FilterAndProcess (&gEfiSimpleTextInProtocolGuid, AddInput);

  EfiBootManagerConnectAllDefaultConsoles ();

  // Make sure the UEFI Shell is a boot option.
  AddBootOptionForFile (L"UEFI Shell", &gUefiShellFileGuid);

  // Re-enumerate boot options from all file systems / firmware volumes.
  EfiBootManagerRefreshAllBootOption ();

  Timeout = PcdGet16 (PcdPlatformBootTimeOut);
  DEBUG ((DEBUG_INFO, "%a: boot timeout = %u\n", __FUNCTION__, Timeout));

  //
  // The platform watchdog would fire after ~30 s of BDS running with no
  // boot attempt; disable it explicitly so a slow DEBUG build cannot reset
  // the phone mid-boot (MCT TimerDxe comes in milestone 2).
  //
  Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  DEBUG ((DEBUG_INFO, "%a: watchdog disarm: %r\n", __FUNCTION__, Status));
}
