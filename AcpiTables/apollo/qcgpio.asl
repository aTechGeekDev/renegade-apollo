//
// TLMM GPIO controller — Xiaomi apollo (SM8250 / Snapdragon 865)
// Values from sm8250.dtsi (pinctrl@f100000) + ios-apollo (tile bases).
//   register space : 0x0F100000, 0x00C00000  (covers west/south/north tiles
//                    0x0F100000 / 0x0F500000 / 0x0F900000 seen in ios-apollo)
//   summary IRQ    : GIC_SPI 208  -> ACPI 240  (208 + 32)
//   total GPIOs    : 180 (0xB4) on SM8250
//
Device (GIO0)
{
    Name (_HID, "QCOM0217")          // Qualcomm TLMM (Windows GPIO driver)
    Name (_UID, 0)

    Method (_CRS, 0x0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0x0F100000, 0x00C00000)
            // Summary interrupt shared by all banks
            Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 240 }
        })
        Return (RBUF)
    }

    // Number of pins, consumed by the GPIO driver
    Method (OFNI, 0x0, NotSerialized)
    {
        Name (RBUF, Buffer () { 0xB4, 0x00 })   // 180 GPIOs
        Return (RBUF)
    }
}
