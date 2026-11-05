//
// Bluetooth — Qualcomm QCA6390 over UART — Xiaomi apollo (SM8250)
// Depends on UAR6 (buses.asl). Windows driver: qcbtfmuart8250.inf (Kona).
//
// Wiring (verified in ios-apollo, xnu 0136-0145):
//   transport : QUP0 SE6 UART -> \_SB.UAR6 (reg 0x998000, GIC 639)
//   init 115200 baud, then 3 Mbaud operational, hardware flow control
//   firmware  : htbtfw20.tlv, htnv20.bin  (QCA6390)
//
Device (BTH0)
{
    Name (_HID, "QCOM02B5")          // Qualcomm BT (UART transport)
    Name (_UID, 0)
    Name (_DEP, Package () { \_SB_.UAR6 })

    Method (_CRS, 0x0, NotSerialized)
    {
        Name (PBUF, ResourceTemplate ()
        {
            UARTSerialBusV2 (
                3000000,                // operational baud (init at 115200)
                DataBitsEight,
                StopBitsOne,
                0xC0,                   // RTS/CTS in use
                LittleEndian,
                ParityTypeNone,
                FlowControlHardware,
                0x20, 0x20,
                "\\_SB.UAR6",
                0, ResourceConsumer, , )
        })
        Return (PBUF)
    }

    Method (_STA) { Return (0xF) }
}
