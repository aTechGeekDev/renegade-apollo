# AcpiTables/

Device-specific ACPI tables (DSDT/SSDT) describing apollo's peripherals go here.

Build them from `../docs/hardware-apollo.md` and the register-level details in
`ios-apollo/NOTES.md`. Start minimal (UART + display + storage + USB), then add peripherals
one at a time — see `../docs/porting-plan.md` Part 2.
