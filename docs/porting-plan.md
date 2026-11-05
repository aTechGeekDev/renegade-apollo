# Plan: completing the Renegade / Windows-on-ARM port on apollo

The base already exists upstream in [`edk2-porting/edk2-msm`](https://github.com/edk2-porting/edk2-msm)
(`Platform/Xiaomi/sm8250/apollo.*`). The job is to build it, extend it, and pack the drivers.
The hardware map ([`hardware-apollo.md`](hardware-apollo.md)) feeds steps 2 and 3.

```
Xiaomi bootloader (unlocked)
  └─ fastboot boot uefi.img         temporary, nothing flashed (bring-up)
     └─ EDK2 / UEFI firmware        apollo package (exists upstream)  ← Step 1
        └─ ACPI tables              CPU+UFS+PEP today; extend peripherals ← Step 2
           └─ Windows Boot Manager
              └─ Windows 11 ARM64 + driver pack (SD865 "Kona") ← Step 3
```

## Step 1 — Build the stock apollo UEFI (baseline)

- Upstream build: `./build.sh -d apollo -b` (generates a `fastboot boot` image). Toolchain
  CLANG38; apollo uses `USE_IASL=true` + `SPLIT_DSDT=true` (ASL compiled with `iasl`, no wine).
- Submodules: `Common/edk2`, `Common/edk2-platforms`, `Platform/EFI_Binaries`,
  `GPLDrivers/Library/SimpleInit`, `tools/Installer`.
- **Milestone 1:** `fastboot boot` the image and reach the UEFI / Windows boot on the phone
  (user-side test). This proves the baseline before we change anything.

## Step 2 — Extend the apollo ACPI

The device DSDT currently covers CPU (8 cores), UFS storage, and PEP/LPI power states. Add the
device-specific peripherals from `hardware-apollo.md`, one at a time:

- Touch (Novatek NT36675 over SPI), haptics (AW8697 over I2C), charger/battery (PM8150B),
  audio routing (WCD938x + CS35L41), sensors (SLPI), panel/backlight detail.
- **Mine `ios-apollo/NOTES.md`** for the register-level facts (peripheral bases, GPIO pins,
  clock/regulator order, bus instances) — every value there is from a real boot of this exact
  phone, which is what makes the ACPI correct rather than guessed.
- Validate each addition by compiling the DSDT with `iasl` before a test boot.

## Step 3 — Windows ARM64 driver pack

- Base the pack on the SD865 "Kona" set: [`WOA-Project/windows_silicon_qcom_kona`](https://github.com/WOA-Project/windows_silicon_qcom_kona),
  [`WOA-Project/Qualcomm-Reference-Drivers`](https://github.com/WOA-Project/Qualcomm-Reference-Drivers).
- Add device specifics: panel, Novatek touch, CS35L41 amps, AW8697 haptics.
- Firmware blobs come from the `ios-apollo` puller scripts (`tools/pull-*.sh`) — same blobs,
  loaded the Windows way.
- Bring-up order (roughly as in `ios-apollo`): display → touch → USB → storage → Wi-Fi/BT →
  audio/haptics → GPU → sensors. Modem and cameras are out of scope.

## Step 4 — Upstream

- Send the ACPI additions and any fixes to `edk2-porting/edk2-msm` as PRs against the apollo
  package, so the Mi 10T port improves for everyone.

## Resolved earlier questions

- **Renegade source:** `edk2-porting/edk2-msm` (site `renegade-project.tech` is network-blocked
  here, but the code is on GitHub).
- **SM8250 base to fork:** apollo already exists; siblings `alioth`, `elish`, `lmi`, `pipa` and
  OnePlus `instantnoodle` are the reference SM8250 device packages to copy patterns from.

## Cloud container vs. your PC

| Here (cloud) | On your PC (with the phone) |
| --- | --- |
| Build the UEFI image (`build.sh`) | `fastboot boot uefi.img` to test |
| Write/extend ACPI; mine `NOTES.md` | Pull firmware blobs off the phone |
| Assemble/script the driver pack | Install the Windows image, flash, debug |
