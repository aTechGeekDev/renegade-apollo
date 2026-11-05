//
// Hardware buttons — Xiaomi apollo (power, volume up/down)
// Power + volume are on the PM8150 PMIC. References \_SB.PM01 (the PMIC GPIO
// controller) which must be declared by the PMIC ACPI (pmic_core.asl) — that
// node is still TODO for apollo, so these pin indices are the Qualcomm
// reference layout (PON KPDPWR = power, PON RESIN = volume-down) and the
// volume-up PMIC GPIO index is to confirm on PM8150.
//
Device (BTNS)
{
    Name (_HID, "ACPI0011")          // GPIO Button Array
    Name (_UID, 0)

    Method (_CRS, 0x0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            // Power button   (PON KPDPWR)
            GpioInt (Edge, ActiveBoth, ExclusiveAndWake, PullDown, 0,
                "\\_SB.PM01", , ) { 0 }
            // Volume Up       (PMIC GPIO - confirm index on PM8150)
            GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0,
                "\\_SB.PM01", , ) { 133 }
            // Volume Down     (PON RESIN)
            GpioInt (Edge, ActiveBoth, Exclusive, PullDown, 0,
                "\\_SB.PM01", , ) { 1 }
        })
        Return (RBUF)
    }

    Name (_DSD, Package ()
    {
        ToUUID ("FA6BD625-9CE8-470D-A2C7-B3CA36C4282E"),
        Package ()
        {
            Package (5) { 0, 1, 0, 0x01, 0x0D },   // Control collection
            Package (5) { 1, 0, 1, 0x01, 0x81 },   // Power -> Sleep
            Package (5) { 1, 1, 1, 0x0C, 0xE9 },   // Volume Increment
            Package (5) { 1, 2, 1, 0x0C, 0xEA }    // Volume Decrement
        }
    })
}
