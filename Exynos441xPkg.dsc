#
#  Exynos441xPkg platform description - Samsung Galaxy Note 1 (GT-N7000)
#  and friends (Exynos4210/4212/4410/4412 family).
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = Exynos441x
  PLATFORM_GUID                  = 1d0d9a4b-40b4-4a4e-9d41-3c9a4e5f4412
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = ARM
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Exynos441xPkg/Exynos441xPkg.fdf

  # DEBUG() goes to the screen by default: a phone has no UART attached.
  # build.sh can flip these to use the Exynos UART0 instead.
  DEFINE USE_UART_FOR_SERIAL_OUTPUT = 0
  DEFINE USE_SCREEN_FOR_SERIAL_OUTPUT = 1

!include Exynos441xPkg/CommonDsc.dsc.inc

################################################################################
#
# Components Section - list of all EDK II Modules needed by this Platform
#
################################################################################
[Components.common]
  #
  # SEC phase: the PrePi that GenFv's reset vector branches into.
  #
  Exynos441xPkg/PrePi/PeiUniCore.inf

  #
  # DXE Core
  #
  MdeModulePkg/Core/Dxe/DxeMain.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
      NULL|MdeModulePkg/Library/DxeCrc32GuidedSectionExtractLib/DxeCrc32GuidedSectionExtractLib.inf
  }

  #
  # Architectural Protocols
  #
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf

  #
  # Timer: MCT global comparator 0 -> GIC INTID 89.  Gives the DXE Core a
  # real tick so timed events (boot timeout, Stall, SetTimer) work.
  #
  Exynos441xPkg/Drivers/MctTimerDxe/MctTimerDxe.inf

  #
  # Interrupt controller (GIC-400 at 0x1048/0x10490000).
  #
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf

  #
  # PCD runtime driver for the dynamic PCDs (video mode / boot timeout)
  #
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  #
  # Consoles: SimpleFbDxe publishes the bootloader's framebuffer as a GOP,
  # ConSplitter wires it into ConIn/ConOut, GraphicsConsole draws text.
  #
  Exynos441xPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
  Exynos441xPkg/Drivers/LogoDxe/LogoDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  #
  # Variables (RAM only for now)
  #
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

  #
  # Status code plumbing + BDS
  #
  MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
  MdeModulePkg/Universal/DriverHealthManagerDxe/DriverHealthManagerDxe.inf
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }

  #
  # Clock controller (CMU gates + special-clock rates) for the drivers below.
  #
  Exynos441xPkg/Drivers/ClockDxe/ClockDxe.inf

  #
  # Keypad / GPIO / I2C (Galaxy Note 1 buttons: power + vol up/down + home).
  #
  Exynos441xPkg/Drivers/GpioDxe/GpioDxe.inf
  Exynos441xPkg/Drivers/I2CDxe/I2CDxe.inf
  Exynos441xPkg/Drivers/GenericKeypadDeviceDxe/GenericKeypadDeviceDxe.inf
  Exynos441xPkg/Drivers/KeypadDxe/KeypadDxe.inf

  #
  # Storage: internal eMMC (MSHCI) + microSD (HSMMC2, card-detect gated),
  # then the disk stack (partition + FAT) on top of BlockIo.
  #
  Exynos441xPkg/Drivers/SdCardDxe/SdCardDxe.inf
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf

  #
  # USB peripheral (device mode only): EFI_USBFN_IO_PROTOCOL for KDNET.
  #
  Exynos441xPkg/Drivers/UsbFnDxe/UsbFnDxe.inf

  #
  # UEFI Shell.
  #
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
      HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
      PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0xFF
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
      gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|8000
  }
