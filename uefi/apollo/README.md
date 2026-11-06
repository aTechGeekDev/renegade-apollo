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

## 3. Windows image side (not firmware)
- Drivers: `WOA-Project/windows_silicon_qcom_kona`, injected with `dism /Add-Driver /Recurse /ForceUnsigned`.
- BCD: `testsigning on`, `nointegritychecks on`, `recoveryenabled no`.
- `qcppx` (PCIe, `qcppx8250.sys`) is disabled (`Services\qcppx\Start = 4`, originally 0): winload
  rejected it with `0xc0430007` while every other test-signed boot driver loaded. Wi-Fi needs it later.
