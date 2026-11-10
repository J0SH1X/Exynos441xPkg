/** @file
  PlatformBootManagerLib for Exynos441xPkg.

  Registers the framebuffer GOP and the text consoles, adds the UEFI Shell
  from FVMAIN as a boot option, shows the platform splash and then boots the
  Shell directly.  Diagnostics go through DEBUG() only: ConOut does not
  exist yet at BeforeConsole time and Print() into it is what killed the
  Shell before.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BootLogoLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FrameBufferConfigLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/KeypadDevice.h>
#include <Protocol/LoadedImage.h>

//
// Title registered for the Shell boot option.  Also used to find the option
// again after RefreshAllBootOption() so we can boot it directly instead of
// trusting BootOrder.
//
#define SHELL_APP_TITLE  L"UEFI Shell"

//
// Implemented in UefiBootManagerLib's BmConsole.c but not exported through
// UefiBootManagerLib.h.  HtcLeoPkg declares it the same way.
//
VOID
EFIAPI
EfiBootManagerConnectAllConsoles (
  VOID
  );

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
  EFI_BOOT_MANAGER_LOAD_OPTION     *Options;
  UINTN                            OptionCount;
  INT32                            OptionNumber;

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

  //
  // Register the option once.  Neither OvmfPkg's nor HtcLeoPkg's
  // PlatformBootManagerLib probes the file with LoadImage() first, and doing
  // so here just made the option silently disappear: the FV definitely holds
  // the Shell (its FILE_GUID is in FVMAIN.Fv), so a failing probe means the
  // probe is wrong, not the file.  What HtcLeoPkg does do is look the option
  // up first (PlatformRegisterBootOption) so a reboot does not stack a second
  // "UEFI Shell" entry onto BootOrder; same idea, inlined here.
  //
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

  Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
  OptionNumber = EfiBootManagerFindLoadOption (&NewOption, Options, OptionCount);
  if (OptionNumber >= 0) {
    DEBUG ((DEBUG_INFO, "%a: '%s' already registered as option %d\n",
            __FUNCTION__, Description, (INT32)OptionNumber));
    EfiBootManagerFreeLoadOptions (Options, OptionCount);
    EfiBootManagerFreeLoadOption (&NewOption);
    return (UINTN)OptionNumber;
  }

  Status = EfiBootManagerAddLoadOptionVariable (&NewOption, 0);
  DEBUG ((DEBUG_INFO, "%a: added %s: %r\n", __FUNCTION__, Description, Status));
  OptionNumber = (INT32)NewOption.OptionNumber;

  EfiBootManagerFreeLoadOptions (Options, OptionCount);
  EfiBootManagerFreeLoadOption (&NewOption);
  return EFI_ERROR (Status) ? 0 : (UINTN)OptionNumber;
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
  // End of the platform's DXE phase.  Until this is signalled the security
  // handler defers every image that does not come from a firmware volume
  // ("3rd party image ... deferred to load before EndOfDxe"), so nothing on a
  // FAT volume, LinuxBoot.efi included, could be started from the Shell.
  //
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  //
  // Register the consoles here, not in AfterConsole.  BdsEntry() calls
  // EfiBootManagerConnectAllDefaultConsoles() between BeforeConsole and
  // AfterConsole, and that is what reads the ConOut variable and fills in
  // gST->ConOut.  Registering later leaves gST->ConOut NULL, which kills
  // Print() and leaves the Shell with nothing to draw on -- all output seen
  // before this point is DEBUG going straight to the framebuffer through
  // SerialPortLib, which never uses gST->ConOut.
  //
  // The GOP handle is what gets registered because SimpleFbDxe installs its
  // device path alongside the protocol; connecting that path is what makes
  // GraphicsConsoleDxe bind and install SimpleTextOut in the first place.
  //
  FilterAndProcess (&gEfiGraphicsOutputProtocolGuid, AddOutput);
  FilterAndProcess (&gEfiSimpleTextInProtocolGuid, AddInput);

  //
  // The keypad handle (GenericKeypadDeviceDxe's KEYPAD_DEVICE_PROTOCOL
  // instance) carries a device path, so adding it to ConIn makes
  // ConnectAllDefaultConsoles() connect it -- and KeypadDxe's driver
  // binding then produces SimpleTextIn on it.  Same wiring as HtcLeoPkg.
  //
  FilterAndProcess (&gEFIDroidKeypadDeviceProtocolGuid, AddInput);

  // The Shell has to exist before BDS sorts BootOrder.
  AddBootOptionForFile (SHELL_APP_TITLE, &gUefiShellFileGuid);
}

/**
  Boot the option registered under Description, directly.

  This deliberately does not consult BootOrder.  RefreshAllBootOption()
  discovers every bootable thing on every filesystem, including
  EFI\BOOT\BOOTARM.EFI on any FAT volume (an SD card, a USB stick), and
  those get sorted ahead of the Shell.  Picking the option by description
  and calling EfiBootManagerBoot() on it sidesteps that ordering entirely.

  @return TRUE if the option was found and EfiBootManagerBoot() returned.
**/
STATIC
BOOLEAN
BootOptionByDescription (
  IN CONST CHAR16  *Description
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         Count;
  UINTN                         Index;
  BOOLEAN                       Found;

  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  if (Options == NULL) {
    return FALSE;
  }

  Found = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (StrCmp (Options[Index].Description, Description) == 0) {
      EfiBootManagerBoot (&Options[Index]);
      //
      // If we get here the image loaded, started and gave control back,
      // which is a different bug from never being found.
      //
      Found = TRUE;
      break;
    }
  }

  EfiBootManagerFreeLoadOptions (Options, Count);
  return Found;
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

  //
  // Bind every driver to every handle.  Nothing connects the GOP handle to
  // GraphicsConsoleDxe otherwise, so no SimpleTextOut is ever installed and
  // gST->ConOut stays NULL -- which is what kills the Shell on its first
  // Print().  This is the step HtcLeoPkg does (PlatformBm.c:411) that we were
  // missing; consoles were registered in BeforeConsole but never bound.
  //
  EfiBootManagerConnectAll ();

  //
  // Consoles were registered in BeforeConsole and BdsEntry() connects the
  // default ones between the two callbacks; re-connecting repairs anything
  // ConnectAll() re-bound.  Leo calls these same two (PlatformBm.c:411).
  //
  EfiBootManagerConnectAllConsoles ();

  //
  // BmUpdateSystemTableConsole() only accepts a console whose device path
  // LocateDevicePath() can resolve.  If that fails, gST->ConOut stays NULL and
  // the Shell dies on its first Print().  Grab any SimpleTextOut directly so
  // the Shell always has something to draw on.
  //
  if (gST->ConOut == NULL) {
    EFI_STATUS                      S;
    EFI_HANDLE                      *Handles;
    UINTN                           NoHandles;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *TextOut;

    S = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextOutProtocolGuid,
                                 NULL, &NoHandles, &Handles);
    if (!EFI_ERROR (S) && (NoHandles > 0)) {
      S = gBS->HandleProtocol (Handles[0], &gEfiSimpleTextOutProtocolGuid,
                               (VOID **)&TextOut);
      if (!EFI_ERROR (S)) {
        gST->ConsoleOutHandle = Handles[0];
        gST->ConOut            = TextOut;
      }
      gBS->FreePool (Handles);
    }
  }

  //
  // Show the splash.  Must come after ConnectAll(): BootLogoEnableLogo()
  // looks the GOP up on gST->ConsoleOutHandle, and that handle only carries
  // GOP once ConSplitter has adopted our framebuffer's GOP.
  //
  Status = BootLogoEnableLogo ();


  // Re-enumerate boot options from all file systems / firmware volumes.
  EfiBootManagerRefreshAllBootOption ();

  //
  // Boot the Shell immediately.  Do not hand control back to BDS: whatever
  // else RefreshAllBootOption() found (BOOTARM.EFI on an SD card, for
  // instance) would be picked ahead of the Shell by BootOrder.
  //
  if (BootOptionByDescription (SHELL_APP_TITLE)) {
  }

  //
  // The platform watchdog would fire after ~30 s of BDS running with no
  // boot attempt; disable it explicitly so a slow DEBUG build cannot reset
  // the phone mid-boot.
  //
  Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  DEBUG ((DEBUG_INFO, "%a: watchdog disarm: %r\n", __FUNCTION__, Status));
}
