//
// GENI QUPv3 serial engines used on apollo (SM8250).
// All in wrapper geniqup@9c0000 (Qualcomm "QUP0"). Bases + GIC interrupts
// from sm8250.dtsi; the touch SE is the one ios-apollo verified (0x994000).
//
//   SE5  spi@994000    GIC_SPI 606 -> ACPI 638   Novatek NT36675 touch (SPI)
//   SE6  serial@998000 GIC_SPI 607 -> ACPI 639   QCA6390 Bluetooth (UART)
//
// (Only the SEs apollo actually uses are declared here; add more as needed.)

// ---- SE5 : SPI master for the touchscreen -------------------------------
Device (SPI5)
{
    Name (_HID, "QCOM021E")          // GENI SPI
    Name (_UID, 5)
    Method (_STA) { Return (0xF) }

    Method (_CRS, 0x0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0x00994000, 0x00004000)
            Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , ) { 638 }
        })
        Return (RBUF)
    }
}

// ---- SE6 : UART for Bluetooth (QCA6390) ---------------------------------
Device (UAR6)
{
    Name (_HID, "QCOM0236")          // GENI UART
    Name (_UID, 6)
    Method (_STA) { Return (0xF) }

    Method (_CRS, 0x0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0x00998000, 0x00004000)
            Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , ) { 639 }
        })
        Return (RBUF)
    }
}
