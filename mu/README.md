# apollo on Mu-Silicium

Windows on SM8250 is booted with [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) (alioth and
pipa: Windows boot and internal storage working); edk2-msm's SM8250 devices never got past stub DSDTs and
apollo stops there at `INACCESSIBLE_BOOT_DEVICE` (see `../uefi/apollo/README.md`). This folder adds apollo
to a Mu-Silicium checkout.

| Piece | Source | apollo changes |
| --- | --- | --- |
| `Platforms/Xiaomi/apolloPkg` | Silicium `aliothPkg` (Poco F3) | `SOC_TYPE 0` (SM8250), apollo SMBIOS (alioth's DXE list unchanged: leaving out the Qualcomm charger drivers crashed UEFI with a Synchronous Exception) |
| Firmware binaries | Silicium `Binaries/alioth` (Xiaomi's shared Kona XBL; it carries apollo's `Panel_J3S` config) | none yet; apollo's own `xbl` can replace them later (UEFIReader) |
| `Resources/Configs/apollo.toml` | same as pipa/alioth | none |
| `Resources/DTBs/apollo.dtb` | apollo stock DT from edk2-msm (`board-id 0x2a`) | none |
| ACPI | `make-apollo-dsdt.py` on pipa's DSDT (`--drop PRTC --no-thermal`) + Silicium Kona APIC/DBG2/FACP/GTDT/PPTT + common SSDT | as tested with edk2-msm |

```sh
bash install-apollo.sh                      # MU=~/Mu-Silicium EDK=~/edk2-msm by default
cd ~/Mu-Silicium && python3 build_uefi.py -d apollo
fastboot boot Mu-apollo.img
```
`install-apollo.sh` moves the framebuffer 256 rows down by default (`FB_SHIFT=256`: "Display Reserved" starts
at `0x9C10E000`, height 2144) so bugcheck codes and UEFI exception text clear the punch-hole camera. Re-run it
with `FB_SHIFT=0` for a full-screen build.

The Windows install and its WOA-Project Kona drivers stay as they are: the DSDT uses the same QCOM25xx IDs.

Boot logo (Xiaomi instead of Silicium): dump the phone's `logo` partition from recovery and pass it as LOGO.
The image stays local; make-boot-logo.py crops the Mi logo out of the first full-screen splash.

    adb shell dd if=/dev/block/by-name/logo of=/tmp/logo.img
    adb pull /tmp/logo.img ~/logo.img
    LOGO=~/logo.img bash install-apollo.sh       (LOGO=silicium restores the original)
