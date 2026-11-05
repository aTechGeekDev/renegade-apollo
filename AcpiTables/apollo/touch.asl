//
// Novatek NT36675 touchscreen — Xiaomi apollo (Mi 10T / Mi 10T Pro)
// Draft ACPI node for the Renegade / Windows-on-ARM apollo port.
//
// All wiring below is from the VERIFIED ios-apollo bring-up (xnu patches
// 0082-0094), whose values come from the Android DT in MiCode
// Xiaomi_Kernel_OpenSource (branch apollo-q-oss,
// drivers/input/touchscreen/nt36672c):
//
//   Bus      : QUPv3 QUP0 SE4  (pinctrl function "qup4"), SPI, 10 MHz
//   SE base  : 0x00994000  (size 0x4000)        [GENI SE4]
//   SPI pins : gpio8-11  (MISO, MOSI, SCLK, CS) — north TLMM tile, func 1
//   reset    : gpio38, active-low   [DT novatek,reset-gpio = <&tlmm 38 0>]
//   irq      : gpio39, rising edge  [DT novatek,irq-gpio   = <&tlmm 39 0x2001>]
//
// Novatek protocol constants (consumed by the OS touch driver, NOT by ACPI):
//   SWRST_N8  0x3f0fe   SPI_RD_FAST 0x3f310   TRIM 0x1f64e   EVENT_BUF 0x22d00
//   chip-id trim bytes  .. .. .. 72 66 03  (identifies NT36675)
//
// DEPENDENCIES (must exist as ACPI devices before this leaf resolves):
//   * SPI controller for QUP0 SE4   -> add in buses.asl, ACPI path \_SB.SPI4
//   * TLMM GPIO controller          -> add in qcgpio.asl, ACPI path \_SB.GIO0
//   Those controllers are SoC-fixed SM8250 nodes; take their register bases
//   and GIC interrupt numbers from an SM8250 reference DSDT / sm8250.dtsi.
//
// TODO before this is flashable (do not treat as final):
//   [ ] _HID the Windows Novatek / HID-over-SPI driver actually binds to
//   [ ] GIC SPI interrupt number for QUP0 SE4 (ios-apollo polled, so it is
//       not recorded here) — from sm8250.dtsi qupv3 SE4
//   [ ] confirm SpiSerialBusV2 / GpioInt macro argument order with `iasl`
//   [ ] on-device validation (the firmware download the NT36675 needs still
//       has to be provided by the OS driver, as on Android)
//

Device (NVTT)
{
    Name (_HID, "NVT0675")          // TODO: confirm (placeholder)
    Name (_CID, "PNP0C50")          // generic HID-over-SPI
    Name (_UID, 0)
    Method (_STA) { Return (0xF) }

    Method (_CRS, 0x0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            // SPI master: QUP0 SE4, 10 MHz, mode 0, CS0, 8-bit, 4-wire.
            SpiSerialBusV2 (0, PolarityLow, FourWireMode, 8,
                ControllerInitiated, 10000000, ClockPolarityLow,
                ClockPhaseFirst, "\\_SB.SPI4", 0, ResourceConsumer, , )

            // Touch interrupt: TLMM gpio39, rising edge, wake-capable.
            GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullUp, 0,
                "\\_SB.GIO0", 0, ResourceConsumer, , ) { 39 }

            // Reset line: TLMM gpio38, active-low, output.
            GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                "\\_SB.GIO0", 0, ResourceConsumer, , ) { 38 }
        })
        Return (RBUF)
    }

    // Panel geometry (verified in ios-apollo): 1080 x 2400.
    Name (_DSD, Package ()
    {
        ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package ()
        {
            Package () { "novatek,swrst-n8-addr",   0x3f0fe },
            Package () { "novatek,spi-rd-fast-addr", 0x3f310 },
            Package () { "touchscreen-size-x",       1080 },
            Package () { "touchscreen-size-y",       2400 }
        }
    })
}
