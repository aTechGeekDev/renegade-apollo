//
// Novatek NT36675 touchscreen — Xiaomi apollo (Mi 10T / Mi 10T Pro)
// Windows-on-ARM ACPI leaf. Depends on SPI5 + GIO0 (buses.asl, qcgpio.asl).
//
// Wiring (verified in ios-apollo, xnu 0082-0094; DT from MiCode apollo-q-oss):
//   bus    : QUP0 SE5 SPI  -> \_SB.SPI5  (reg 0x994000, GIC 638), 10 MHz
//   reset  : TLMM gpio38, active-low   (novatek,reset-gpio = <&tlmm 38 0>)
//   irq    : TLMM gpio39, rising edge  (novatek,irq-gpio   = <&tlmm 39 0x2001>)
//
// NOTE on the driver: the Kona pack's touch INF (qcImproveTouch8250.inf) is
// NOT Novatek. apollo needs a Novatek NT36675 function driver (HID-over-SPI
// client). The _HID below is a placeholder until that driver's ID is known.
// Novatek protocol constants (for that driver, not ACPI):
//   SWRST 0x3f0fe  SPI_RD_FAST 0x3f310  TRIM 0x1f64e  EVENT_BUF 0x22d00
//   chip-id trim   .. .. .. 72 66 03
//
Device (NVTT)
{
    Name (_HID, "NVT0675")           // TODO: real Novatek HIDSPI id
    Name (_CID, "PNP0C51")           // HID-over-SPI class
    Name (_UID, 0)
    Method (_STA) { Return (0xF) }

    Method (_CRS, 0x0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            // QUP0 SE5 SPI: 10 MHz, mode 0, CS0, 8-bit, 4-wire
            SpiSerialBusV2 (0, PolarityLow, FourWireMode, 8,
                ControllerInitiated, 10000000, ClockPolarityLow,
                ClockPhaseFirst, "\\_SB.SPI5", 0, ResourceConsumer, , )

            // Touch interrupt: TLMM gpio39, rising edge, wake-capable
            GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullUp, 0,
                "\\_SB.GIO0", 0, ResourceConsumer, , ) { 39 }

            // Reset: TLMM gpio38, active-low output
            GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                "\\_SB.GIO0", 0, ResourceConsumer, , ) { 38 }
        })
        Return (RBUF)
    }

    Name (_DSD, Package ()
    {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
            Package () { "novatek,swrst-n8-addr",    0x3f0fe },
            Package () { "novatek,spi-rd-fast-addr",  0x3f310 },
            Package () { "touchscreen-size-x",        1080 },
            Package () { "touchscreen-size-y",        2400 }
        }
    })
}
