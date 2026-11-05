# Porting plan: Renegade / Windows-on-ARM on apollo

A Windows-on-ARM phone port has three parts. The hardware map
([`hardware-apollo.md`](hardware-apollo.md)) feeds parts 2 and 3.

```
Xiaomi bootloader (unlocked)
  └─ fastboot boot uefi.img         temporary, nothing flashed (bring-up)
     └─ EDK2 / UEFI firmware        apollo platform package  ← Part 1
        └─ ACPI tables              describe apollo's hardware ← Part 2
           └─ Windows Boot Manager
              └─ Windows 11 ARM64 + driver pack ← Part 3
```

## Part 1 — EDK2 / UEFI platform package for apollo

- Base it on an existing **SM8250** Windows-on-ARM device (same SoC → most platform code is
  shared). Candidate upstreams to check on GitHub: `WOA-Project` / `Renegade-Project` orgs and
  their SM8250 device packages.
- Structure: `edk2` + `edk2-platforms` as the build base, a device `Platform/.../apolloPkg`
  with `.dsc`/`.fdf`, memory map, SimpleFbDxe (framebuffer handoff from the bootloader),
  and the reset/clock/PEP libraries for SM8250.
- **Milestone 1A:** UEFI builds in CI/container (EDK2 builds fine here — no huge tree needed).
- **Milestone 1B:** `fastboot boot uefi.img` reaches a **UEFI shell / display output** on the
  real phone (user-side test).

## Part 2 — ACPI tables (DSDT/SSDT)

- Describe each peripheral from `hardware-apollo.md`: SPMI/PMIC, UART (BT + debug), SPI (touch),
  SoundWire (codec), I2C (haptics), USB, UFS, display, GPU, Wi-Fi.
- Mine `ios-apollo/NOTES.md` for the register-level facts (bases, GPIO, clocks) — **the single
  richest source we have**, because every value there is from a real boot of this exact phone.
- Start minimal (UART + display + storage + USB) so Windows can boot and be debugged, then add
  peripherals one at a time.
- **Milestone 2:** Windows 11 ARM64 installer boots to setup with keyboard/USB over the cable.

## Part 3 — Windows ARM64 driver pack

- Assemble from the matching **SM8250 WoA driver pack** + device specifics (panel, Novatek
  touch, CS35L41 amps, AW8697 haptics).
- Firmware blobs come from the `ios-apollo` puller scripts (same blobs; see hardware map).
- Bring up in roughly the `ios-apollo` order of difficulty: display → touch → USB → storage →
  Wi-Fi/BT → audio/haptics → GPU → sensors. Modem and cameras are typically out of scope.
- **Milestone 3:** Windows desktop with touch, Wi-Fi and audio.

## Open questions / things to confirm

1. **Renegade source** — the site `renegade-project.tech` is blocked from this container.
   Need the GitHub org/repo for Renegade's UEFI (or confirm we build on `WOA-Project` EDK2
   directly). Once we have the URL we can attach it and base `apolloPkg` on an SM8250 sibling.
2. **Does an SM8250 WoA device package already exist** we can fork? (Mi 10 `umi`, OnePlus 8
   `instNoodle`, etc. are strong candidates — much faster than greenfield.)
3. **Target Windows build** — 11 ARM64 (24H2?) vs 10.

## What can be done in this cloud container vs. on your PC

| Here (cloud) | On your PC (with the phone) |
| --- | --- |
| Write/adapt the EDK2 platform package | `fastboot boot uefi.img` to test |
| Write ACPI tables; mine `NOTES.md` | Pull firmware blobs off the phone |
| Build the UEFI firmware (EDK2 CI) | Deploy/install the Windows image |
| Assemble/script the driver pack | Flash, debug on real hardware |
