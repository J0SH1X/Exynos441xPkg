/** @file
  Minimal DSDT for the GT-N7000: just the device DBG2 points at.

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
  }
}
