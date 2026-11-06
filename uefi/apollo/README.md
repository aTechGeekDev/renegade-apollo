# edk2-msm changes for apollo (Windows boot)

Changes to [`edk2-porting/edk2-msm`](https://github.com/edk2-porting/edk2-msm) found while bringing up
Windows 11 24H2 ARM64 on a Mi 10T Pro (apollo, 8 GB / 128 GB). Build with
`./build.sh --device apollo -A -f` (`-A` is required, or the DSDT is not recompiled).

## 1. Drop SimpleInit from apollo's boot order
`Platform/Xiaomi/sm8250/apollo.dsc`: remove `-DENABLE_SIMPLE_INIT` from `CC_FLAGS`.

SimpleInit exits with status 1 on apollo (`Error: Image at 000C973F000 start failed: 00000001`), so the
firmware never reached the Windows boot entry and sat on the logo. Without the flag, the BDS boots the
auto-detected ESP (`\EFI\Boot\bootaa64.efi`), and the side buttons open the EDK2 Boot Manager menu.

## 2. Corrected memory map
`Platform/Xiaomi/sm8250/Library/apollo/PlatformMemoryMapLib/PlatformMemoryMapLib.c` → replace with
[`PlatformMemoryMapLib.c`](PlatformMemoryMapLib.c) from this folder.

The upstream map was copied from sibling SM8250 devices and gave Windows memory that is protected or
absent on apollo, which caused `MEMORY_MANAGEMENT` (0x1A) during boot:

| Upstream "free RAM" | Actually on apollo |
| --- | --- |
| `0x80600000-0x80700000` | `xbl_aop_region` (starts at 0x80600000, size 0x260000) |
| `0x8BF00000-0x92700000` | `pil_adsp`, `pil_spss`, `cdsp_secure_heap` (TrustZone) |
| `0xB0400000-0xB1400000` | `disp_rdump_region` (no-map) |
| `0xBBB00000-0xBCC00000` | not RAM (gap between DDR banks) |
| `0xFFC00000-0xFFC20000` | not System RAM (no-map, dynamic) |

Measured on the device from OrangeFox recovery:
- DDR banks (`/memory`): `0x80000000+0x3BB00000`, `0xC0000000+0xC0000000`, `0x180000000+0x100000000`
- `/proc/iomem` System RAM and `/proc/device-tree/reserved-memory` (all `no-map` carve-outs).

The new map was checked programmatically: every `Conv` region is inside Linux System RAM, no `Conv`
region overlaps a `no-map` carve-out, and every carve-out is fully `Reserv`. RAM is still capped at
`0x200000000` (about 5.5 GB usable); bank 3 up to `0x280000000` can be exposed once boot is stable.

## 3. Fill in the ACPI placeholders at boot (sm8250)
[`0001-sm8250-patch-acpi-placeholders.patch`](0001-sm8250-patch-acpi-placeholders.patch) (apply with
`git apply` in the edk2-msm root, made against `e195262`).

sm8250's `MsPlatformDevicesLib` had `PlatformUpdateAcpiTables()` commented out and `AslUpdateLib`
disabled, so Windows read the DSDT placeholders as-is, including
`SOSI = 0xdeadbeefffffffff` (SoCInfo shared-memory base used by Qualcomm's ChipInfoLib). A Qualcomm driver
mapped that address and Windows stopped with `MEMORY_MANAGEMENT` (0x1A, parameter 2 containing `DEADBEEF`).
The patch ports sm8150's implementation (minus the RFS-protection call): SOSI comes from SMEM item 137,
chip ID / version / family / serial from ChipInfo, platform subtype from PlatformInfo.
`TCMA` stays `0xDEADBEEF` without a `TGCM` region, the same as every sm8150 device.

## 4. A full Windows DSDT for apollo (fixes INACCESSIBLE_BOOT_DEVICE)
Every SM8250 device in edk2-msm ships a 4 KB stub DSDT (CPUs + UFS0, no PEP), so Windows' `storufs`
cannot power the UFS and stops with `INACCESSIBLE_BOOT_DEVICE` (0x7B). (Samsung r8q's large table is a
byte-identical copy of an SM8150 one.)

[`make-apollo-dsdt.py`](make-apollo-dsdt.py) builds apollo's table from
[Silicium-ACPI](https://github.com/Project-Silicium/Silicium-ACPI)'s Xiaomi **pipa** (SM8250-AC, Kona)
DSDT. The table itself is not committed here (Silicium-ACPI has no licence file); generate it locally:

```sh
git clone --depth 1 https://github.com/Project-Silicium/Silicium-ACPI
python3 make-apollo-dsdt.py Silicium-ACPI/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl Dsdt.asl
```

What it changes, and how it was checked:
- **IDs:** `QCOM05xx` -> `QCOM25xx`, the IDs in `WOA-Project/windows_silicon_qcom_kona` (58 ids; every one
  maps to the matching driver, e.g. PEP0->qcpep, GIO0->qcgpio, SPMI->qcspmi).
- **UFS power rails** remapped to apollo's measured ones (`/sys/class/regulator` in recovery):
  VCC `LDO10_A` 2.904 V -> `LDO17_A` 2.496 V, ref clock/VCCQ `LDO3_C` -> `LDO6_A` 1.2 V.
- **Devices removed for first boot:** Bluetooth (would set `LDO11_C` 3.312 V > apollo max 3.304 V and
  `LDO2_C` 1.304 V vs apollo's fixed 1.08 V), SD card, audio, cameras, pipa's touch/battery/sensors,
  modem/DSP/sensor loaders (pipa memory carve-outs), SMMUs, PCIe, `TREE` (reads `TCMA = 0xDEADBEEF`).
  Kept: CPUs, PEP0, UFS0, GPIO, SPMI, PMIC (+GPIO/apps/ext/RTC), ABD, SCM0, BAMs, GPI, USB, buttons.
- **Rail audit of the result:** every PMIC vote of a kept device is inside apollo's own regulator limits;
  no `Memory32Fixed` inside RAM; compiles with `iasl` (0 errors). Every line the repair step changed was
  checked to reference only removed objects (one circular PEP0<->AGR0 reference kept via `External`).

### Bisecting a boot crash
With this table storage comes up, and the next boot stops with `DRIVER_IRQL_NOT_LESS_OR_EQUAL` in
**ACPI.sys** (a NULL read). The crash is in Microsoft's ACPI driver, not a Qualcomm one, so the
suspects are the objects ACPI.sys drives itself:
- `PRTC` (`ACPI000E`, Time & Alarm): ACPI.sys reads the clock through `_GRT`, a GenericSerialBus field
  on `ABD`. edk2-msm's stub had no PRTC, and Windows then used the UEFI clock.
- the thermal zones (`TZ0`..`TZ99`), which ACPI.sys owns.

Ruled out: the MADT and GTDT are field-for-field identical to Silicium's Kona ones, and FADT flags match.
Every `_DEP` target is a device, and the `External` is wrapped by iasl in `If (Zero)`.

The generator takes `--drop NAME,...` and `--no-thermal` for this. [`bisect-build.sh`](bisect-build.sh)
builds three images in one go, each removing more than the one before it:

| Image | Removes (on top of the list above) |
| --- | --- |
| `boot-apollo-A.img` | `PRTC` |
| `boot-apollo-B.img` | `PRTC` + all thermal zones |
| `boot-apollo-C.img` | B + `PEXT BTNS URS1 QGP0-2 BAM* RPEN`; keeps UFS, PEP, GPIO, SPMI, PMIC, USB + Type-C |

## 5. Windows image side (not firmware)
- Drivers: `WOA-Project/windows_silicon_qcom_kona`, injected with `dism /Add-Driver /Recurse /ForceUnsigned`.
- BCD: `testsigning on`, `nointegritychecks on`, `recoveryenabled no`.
- `qcppx` (PCIe, `qcppx8250.sys`) is disabled (`Services\qcppx\Start = 4`, originally 0): winload
  rejected it with `0xc0430007` while every other test-signed boot driver loaded. Wi-Fi needs it later.
