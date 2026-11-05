# renegade-apollo

**Goal:** a [Renegade Project](https://renegade-project.tech) / Windows-on-ARM (UEFI) device port
for the **Xiaomi Mi 10T / Mi 10T Pro / Redmi K30S Ultra** (`apollo`, Snapdragon 865 / SM8250).

This repo is the device-specific half of a Windows-on-ARM port: an **EDK2/UEFI firmware**
and **ACPI tables** for `apollo`, plus the driver-pack and install notes needed to boot
**ARM64 Windows 11** on the phone via `fastboot boot`.

> [!WARNING]
> Experimental bring-up, not a daily driver. A Windows-on-ARM port replaces the normal boot
> chain and can brick a device if a wrong image is flashed permanently. During bring-up
> **nothing is flashed** — the UEFI is started with a temporary `fastboot boot`, exactly like
> the sibling [`ios-apollo`](https://github.com/aTechGeekDev/ios-apollo) project.

---

## Why this repo exists and what it reuses

The hardware bring-up work from [`ios-apollo`](https://github.com/aTechGeekDev/ios-apollo)
(iOS 16 / XNU on the same phone) does **not** port over as code — XNU kernel patches and iOS
dylib shims are a different world from Windows ACPI + `.sys` drivers. What *does* carry over,
and what makes this port realistic, is the **verified hardware map** of `apollo`: every chip,
its firmware blob, the bus it sits on, panel timings, PMIC layout. That map is in
[`docs/hardware-apollo.md`](docs/hardware-apollo.md) and feeds directly into the ACPI tables
and the choice of Windows drivers.

The plan and milestones are in [`docs/porting-plan.md`](docs/porting-plan.md).

---

## Status

| Area | Status |
| --- | --- |
| Hardware reference (from `ios-apollo`) | ✅ Captured (`docs/hardware-apollo.md`) |
| EDK2 / UEFI platform package for apollo | ❌ Not started |
| ACPI tables (DSDT/SSDT) | ❌ Not started |
| Windows ARM64 driver pack (SM8250) | ❌ Not started |
| First `fastboot boot` of UEFI to a UEFI shell | ❌ Not started |
| Windows 11 installer boot | ❌ Not started |

---

## Layout

| Path | What's inside |
| --- | --- |
| `docs/hardware-apollo.md` | The apollo hardware map reused from `ios-apollo` |
| `docs/porting-plan.md` | The EDK2 → ACPI → driver-pack roadmap and milestones |
| `Platform/` | The EDK2/UEFI platform package for apollo (to be added) |
| `AcpiTables/` | Device-specific ACPI tables (to be added) |

---

## License

MIT, see [`LICENSE`](LICENSE). Windows, its drivers and any Qualcomm/Xiaomi firmware are **not**
distributed here; they are obtained by the user from their own device and from Microsoft.
