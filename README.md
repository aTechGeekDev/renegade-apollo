# renegade-apollo

Workspace for completing the **[Renegade Project](https://renegade-project.tech) / Windows-on-ARM**
port on the **Xiaomi Mi 10T / Mi 10T Pro / Redmi K30S Ultra** (`apollo`, Snapdragon 865 / SM8250),
reusing the verified hardware work from the sibling [`ios-apollo`](https://github.com/aTechGeekDev/ios-apollo)
project.

> [!IMPORTANT]
> **Renegade already has an apollo port.** The upstream firmware
> [`edk2-porting/edk2-msm`](https://github.com/edk2-porting/edk2-msm) ships a complete device
> package at `Platform/Xiaomi/sm8250/apollo.*` (ACPI tables, memory map, build config). So this
> is **not** a from-scratch port. This repo is the workspace for **finishing** that port:
> extending its ACPI and assembling the Windows ARM64 driver pack, using the `ios-apollo`
> hardware map — then sending improvements upstream.

> [!WARNING]
> Experimental. During bring-up **nothing is flashed permanently** — the UEFI is started with a
> temporary `fastboot boot`, as in `ios-apollo`.

---

## Where things stand

The upstream apollo port is at **early bring-up**: its device ACPI covers the **8 CPU cores, UFS
storage, and power/idle states (PEP/LPI)**; the SoC-common SM8250 layer provides USB and core
peripherals; the display comes from the bootloader framebuffer (1080×2400). That is enough to
**boot Windows 11 to the desktop with display, storage and USB**.

The device-specific long tail is thin or missing — and that is exactly what `ios-apollo` verified
on this phone:

| Peripheral | apollo chip (from ios-apollo) | Upstream Renegade status | Where our work helps |
| --- | --- | --- | --- |
| CPU / storage / power | SM8250, UFS | ✅ In ACPI | — |
| Display (framebuffer) | 1080×2400 panel | ✅ Bootloader handoff | panel/backlight detail later |
| USB | SM8250 | ✅ SoC-common | — |
| Touch | Novatek NT36675 (SPI) | ❌ to verify / add | ✅ bus + firmware known |
| Wi-Fi + BT | QCA6390 | ❌ to verify / add | ✅ bring-up known |
| Audio | WCD938x + 2× CS35L41 | ❌ | ✅ routing + tuning known |
| Haptics | AW8697 (I2C) | ❌ | ✅ |
| Charger / battery | PM8150B | ❌ | ✅ |
| GPU accel | Adreno 650 | driver-pack | ✅ firmware known |
| Sensors | QC sensor hub (SLPI) | ❌ | partial |

See [`docs/hardware-apollo.md`](docs/hardware-apollo.md) for the full chip/firmware map and
[`docs/porting-plan.md`](docs/porting-plan.md) for the plan.

---

## Plan

1. **Build the stock apollo UEFI** from `edk2-msm` (`./build.sh -d apollo -b`) and boot Windows 11
   on the phone — baseline.
2. **Extend the apollo ACPI** with the device-specific peripherals above, mining `ios-apollo`'s
   `NOTES.md` for register-level detail (bases, GPIO, clocks, buses).
3. **Assemble the Windows ARM64 driver pack** — base [`WOA-Project/windows_silicon_qcom_kona`](https://github.com/WOA-Project/windows_silicon_qcom_kona)
   (SD865 "Kona") plus device specifics; firmware blobs via the `ios-apollo` puller scripts.
4. **Send improvements upstream** to `edk2-porting/edk2-msm`.

---

## Layout

| Path | What's inside |
| --- | --- |
| `docs/hardware-apollo.md` | apollo hardware map reused from `ios-apollo` |
| `docs/porting-plan.md` | the completion plan and the cloud-vs-PC split |
| `AcpiTables/` | WIP ACPI additions before they go upstream |
| `Platform/` | scratch / notes on the upstream apolloPkg |

---

## License

MIT, see [`LICENSE`](LICENSE). Windows, its drivers and any Qualcomm/Xiaomi firmware are **not**
distributed here; they are obtained by the user from their own device and from Microsoft.
