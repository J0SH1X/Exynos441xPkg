/** @file
  Minimal DSDT for the GT-N7000: the USB port, the clock controller, the
  microSD host and the touchscreen with the I2C and GPIO controllers it
  hangs off.

  _HIDs EXYN00xx are ours; the Windows drivers live in ExynosWinDrivers.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

DefinitionBlock ("DSDT.aml", "DSDT", 2, "SAMSNG", "EXY4210 ", 1)
{
  Scope (\_SB)
  {
    //
    // DWC2 USB device controller (peripheral mode only), KDNET debug port.
    //
    Device (UFN0)
    {
      Name (_HID, "EXYN0001")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12480000, 0x00020000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 103 }   // SPI 71
      })
    }

    //
    // I2C3 (s3c2410-style controller), touchscreen bus.  Pins and clock gate
    // are left set up by UEFI.
    //
    Device (I2C3)
    {
      Name (_HID, "EXYN0002")
      Name (_UID, 3)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x13890000, 0x00000100)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 93 }    // SPI 61
      })
    }

    //
    // GPIO part 2 (0x11000000): the GPX0 bank and its EINT0-7 controls.
    // Each EINT0-7 has its own SPI (16-23), so one interrupt per pin.
    //
    Device (GPIO)
    {
      Name (_HID, "EXYN0003")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x11000000, 0x00001000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 48, 49, 50, 51, 52, 53, 54, 55 }
      })
    }

    //
    // Clock controller (CMU), served to other drivers by exynosclk.
    //
    Device (CLK0)
    {
      Name (_HID, "EXYN0005")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x10030000, 0x00020000)
      })
    }

    //
    // HSMMC2: microSD (standard SDHCI with Exynos extras).  UEFI leaves the
    // slot powered and muxed; the card clock comes from CLK0.  No _DEP on
    // CLK0: Windows 8.1 then holds the device until CLK0 provides an
    // operation region, which it never does.  The SD driver falls back to
    // UEFI's 50 MHz sclk_mmc2 if the clock driver is not up yet.
    //
    // No interrupt: exynosstor (8.1, Storport virtual miniport) polls, and
    // Storport removes the adapter when it cannot connect a declared
    // interrupt (SPI 75 = GSI 107) the miniport has no handler for.
    //
    Device (SDC2)
    {
      Name (_HID, "EXYN0006")
      Name (_UID, 2)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x12530000, 0x00000100)
      })
    }

    //
    // Atmel mXT540E touchscreen: I2C3 address 0x4C, CHG on GPX0(4).
    //
    Device (TSC0)
    {
      Name (_HID, "EXYN0004")
      Name (_UID, 0)
      Method (_STA) { Return (0xF) }
      Name (_CRS, ResourceTemplate () {
        I2cSerialBus (0x4C, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C3", 0, ResourceConsumer)
        GpioInt (Level, ActiveLow, Exclusive, PullNone, 0, "\\_SB.GPIO") { 4 }
      })
    }
  }
}
