# Extending the apollo ACPI for Windows-on-ARM

The upstream Renegade apollo DSDT (`edk2-msm Platform/Xiaomi/sm8250/AcpiTables/apollo/`)
currently describes only **CPU, UFS storage and power/idle states (PEP/LPI)**. Everything
Windows needs beyond booting-with-display-and-storage has to be added as ACPI device nodes.

This doc records the **real apollo wiring** (from the verified `ios-apollo` bring-up) for each
peripheral, the ACPI include file it belongs in (Qualcomm's standard names, already stubbed as
commented `Include(...)` lines in `dsdt_common.asl` / `Dsdt.asl`), and the Windows driver that
consumes it. Draft ASL lives in [`../AcpiTables/apollo/`](../AcpiTables/apollo/).

## How the ACPI is structured here

`Dsdt.asl` opens `Scope(\_SB_)` and `Include`s `dsdt_common.asl`, which pulls in `ufs.asl` and
`Pep_lpi.asl`. The pattern for every device is the one in `ufs.asl`: a `Device` with `_HID`
(a Qualcomm `QCOM####` id that binds the Windows driver), `_STA`, and a `_CRS` of
`Memory32Fixed` register windows + GIC `Interrupt`s. Leaf devices on a bus (touch on SPI,
haptics on I2C) instead carry `SpiSerialBusV2`/`I2CSerialBusV2` + `GpioInt`/`GpioIo` resources
that **reference** a bus controller and the TLMM GPIO controller — so those controllers must be
declared first.

Two layers of values:
- **SoC-fixed (SM8250)** — QUP/TLMM/SPMI/PCIe/USB/LPASS register bases and GIC interrupts. Same
  on every SM8250 phone; take them from an SM8250 reference DSDT (a WOA SM8250 port) or
  `sm8250.dtsi`. Not apollo-specific.
- **apollo-specific** — which bus instance a chip sits on, its GPIOs, address, firmware. These
  are the hard part, and they are what `ios-apollo` nails down (table below).

## Peripheral map (apollo-specific wiring, verified in ios-apollo)

| Peripheral | Chip | Bus / wiring (apollo) | ACPI include | Windows driver family | Source (ios-apollo) | Status |
| --- | --- | --- | --- | --- | --- | --- |
| **Touch** | Novatek NT36675 | QUP0 **SE4** SPI @ `0x994000`, 10 MHz, pins gpio8-11; **reset gpio38** (act-low), **irq gpio39** (rising) | `buses.asl` (SE4) + `qcgpio.asl` + `touch.asl` | HID-over-SPI / Novatek | patches 0082-0094 | **draft ASL** (`touch.asl`) |
| **Bluetooth** | QCA6390 | **QUP SE6 UART**, 3 Mbaud | `buses.asl` (SE6 UART) + `wcnss_bt.asl` | Qualcomm BT (UART) | 0136, 0145 | spec |
| **Wi-Fi** | QCA6390 | **PCIe** (MHI), SM8250 PCIe0 | `pcie.asl` | Qualcomm/ath WLAN (Kona) | 0126-0135 | spec |
| **Charger / battery** | PM8150B | **SPMI**, regs @ `0x4800`/`0x48c0` (fuel-gauge/charger) | `spmi.asl` + `pmic_core.asl` + `pmic_batt.asl` | Qualcomm PMIC + charger | 0182 | spec |
| **Haptics** | Awinic AW8697 | I2C (instance TBD), enable gpio32 | `buses.asl` (I2C) + leaf | vendor/Qualcomm haptics | 0146, 0147 | spec (bus TBD) |
| **Audio** | WCD938x + 2× CS35L41 | LPASS SoundWire + ADSP (mem `0x8bb00000`) | `audio.asl` | Qualcomm audio (LPASS/ADSP) | 0138, 0139 | spec (complex) |
| **Buttons** | pwr / vol± | PMIC + TLMM | `cust_arraybutton.asl` | PEP button array | README (ios-apollo) | spec |
| **USB** | SM8250 | SoC-common | (SoC layer) | Qualcomm USB | — | already via SoC |
| **Display** | 1080×2400 panel | bootloader framebuffer handoff | (PcdMipiFrameBuffer in `apollo.dsc`) | SimpleFB → QC display | 0125, 0152 | present (fb) |

> The Novatek register constants (`SWRST 0x3f0fe`, `EVENT_BUF 0x22d00`, trim `0x1f64e`,
> chip-id `72 66 03`) are protocol values for the OS touch driver, not ACPI — kept in
> `touch.asl` comments for whoever writes/ports that driver.

## Values still needed from an SM8250 reference (SoC-fixed, not apollo-specific)

These block turning the specs above into compiling ASL and are **not** in `ios-apollo` (it used
polled/bare-metal access, Apple device-tree addressing, or didn't touch the block):

- GIC SPI interrupt numbers for QUP0 **SE4** (touch SPI) and **SE6** (BT UART).
- QUP0 wrapper + per-SE register bases for the `buses.asl` controller nodes.
- SM8250 **PCIe0** controller window + interrupts (Wi-Fi).
- **SPMI** controller base + the PM8150B peripheral layout (charger).
- **TLMM** GPIO controller node (`qcgpio.asl`) — three tiles at `0x0F100000` / `0x0F500000` /
  `0x0F900000`, 0x40000 each (tile bases **are** recorded in ios-apollo).

Best source: a WOA SM8250 DSDT (e.g. another `edk2-msm` SM8250 port once one gains peripherals,
or a DuoWOA/SurfaceDuo-style SM8250 reference) cross-checked against `sm8250.dtsi`.

## Workflow

1. Draft each node here (`AcpiTables/apollo/*.asl`), apollo values from the table.
2. Fill the SoC-fixed blanks from the SM8250 reference.
3. Wire the includes into `dsdt_common.asl` (uncomment / add `Include("...")`).
4. Compile with `iasl` (apollo uses `USE_IASL=true`); fix macro/syntax.
5. Build the apollo UEFI (`build.sh -d apollo`) and test on-device, one peripheral at a time.
6. PR to `edk2-porting/edk2-msm`.

---

## Status: resolved & iasl-validated (2026-11)

The SoC-fixed blanks are now filled from `sm8250.dtsi`, and the node set **compiles cleanly**
(`iasl`: 0 errors, 0 warnings; the 14 remarks are the same cosmetic ones upstream `ufs.asl`
produces). Written to [`../AcpiTables/apollo/`](../AcpiTables/apollo/); wire into the upstream
DSDT per [`install-runbook.md`](install-runbook.md) §3.

| Node | File | _HID | Resources (resolved) | Source |
| --- | --- | --- | --- | --- |
| TLMM GPIO | `qcgpio.asl` | `QCOM0217` | mem `0x0F100000`/`0xC00000`, IRQ **240** (GIC 208+32), 180 pins | sm8250.dtsi `pinctrl@f100000` |
| SPI (touch) | `buses.asl` SPI5 | `QCOM021E` | mem `0x994000`/`0x4000`, IRQ **638** (GIC 606+32) | sm8250.dtsi `spi@994000` + ios-apollo |
| UART (BT) | `buses.asl` UAR6 | `QCOM0236` | mem `0x998000`/`0x4000`, IRQ **639** (GIC 607+32) | sm8250.dtsi `serial@998000` = SE6 |
| Touch | `touch.asl` | NVT (TODO) | SPI→SPI5 10 MHz, GpioInt gpio39, GpioIo gpio38 | ios-apollo 0082-0094 |
| Bluetooth | `wcnss_bt.asl` | `QCOM02B5` | UART→UAR6, 3 Mbaud, HW flow | ios-apollo 0136-0145 |
| Buttons | `cust_arraybutton.asl` | `ACPI0011` | GpioInt→PM01 (pwr/vol) | sdm845 ref + PM8150 |

**ACPI convention `Interrupt{N}` = `GIC_SPI (N-32)`** — the rule used throughout (confirmed
against the existing sdm845 nodes).

### Still open (need the phone or a PMIC node)
- **Touch driver**: ACPI ready, but the Novatek NT36675 needs a non-Kona driver (see
  `driver-pack.md`); and the touch `_HID` must match whatever driver is chosen.
- **PMIC node** (`\_SB.PM01`, `pmic_core.asl`): the buttons reference it; charger/battery
  (PM8150B over SPMI `0x0C440000`) build on it. Next ASL to write.
- **Wi-Fi** (QCA6390 over PCIe0 `0x01C00000`) and **audio** (LPASS/SoundWire) — larger nodes.
- All values above still need **on-device validation** (one peripheral per boot).
