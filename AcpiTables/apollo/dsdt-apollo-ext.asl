//
// Standalone wrapper to syntax-check the apollo peripheral ACPI additions
// with `iasl`. NOT a replacement for the upstream DSDT — in edk2-msm these
// Include() lines are added to dsdt_common.asl / Dsdt.asl inside Scope(\_SB_).
//
//   iasl dsdt-apollo-ext.asl     # run from this directory
//
DefinitionBlock ("apollo-ext.aml", "DSDT", 0x02, "QCOMM ", "APOLLO ", 3)
{
    Scope (\_SB_)
    {
        Include ("qcgpio.asl")          // TLMM GPIO controller  (GIO0)
        Include ("buses.asl")           // QUP SE5 SPI / SE6 UART (SPI5, UAR6)
        Include ("touch.asl")           // Novatek NT36675        (NVTT)
        Include ("wcnss_bt.asl")        // QCA6390 Bluetooth      (BTH0)
        Include ("cust_arraybutton.asl")// power / volume buttons (BTNS)
    }
}
