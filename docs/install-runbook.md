# Install runbook — Windows-on-ARM on apollo

End-to-end steps to get Windows 11 ARM64 booting on the Mi 10T. Steps marked **[PC]** run on
your Linux/Windows computer; **[PHONE]** need the device in fastboot/recovery. None of this can
be finished in a cloud container — it is the hardware last mile.

> [!CAUTION]
> This repartitions/erases `userdata`. Back up first. An unlocked bootloader is required.
> A wrong permanent flash can brick the phone — during bring-up prefer temporary `fastboot boot`.

## 0. Prerequisites [PC]
- `adb`, `fastboot`, `git`, build tools, and (for the ACPI) `iasl` + `aarch64-linux-gnu` cross-gcc.
- An unlocked bootloader on the Mi 10T (`apollo`), and OrangeFox recovery (temporary boot).
- A Windows 11 **ARM64** image (`install.wim` from a UUP build) + Windows ARM64 ADK tools.

## 1. Back up the phone [PHONE]
From OrangeFox recovery, pull and keep OUT of git:
`persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot`, `dtbo`, `vbmeta`.
(Same list ios-apollo uses — these are never overwritten by a Windows install.)

## 2. Build the Renegade UEFI for apollo [PC]
```sh
git clone --recursive https://github.com/edk2-porting/edk2-msm
cd edk2-msm
./build.sh -d apollo            # -> UEFI .fd
./build.sh -d apollo -z         # or: flashable installer zip (tools/Installer)
```
(Needs `iasl`, cross-gcc, submodules. In the cloud container the `freetype`/`boringssl`
submodules are network-blocked; on your PC they clone normally.)

## 3. Apply the apollo ACPI additions [PC]
This repo's [`AcpiTables/apollo/`](../AcpiTables/apollo/) adds GPIO, SPI/UART buses, touch,
Bluetooth and buttons (all `iasl`-validated). To fold them into the upstream DSDT:
1. Copy `qcgpio.asl buses.asl touch.asl wcnss_bt.asl cust_arraybutton.asl` into
   `Platform/Xiaomi/sm8250/AcpiTables/apollo/` in your edk2-msm clone.
2. In that folder's `dsdt_common.asl`, inside the `\_SB_` scope, add:
   ```
   Include("qcgpio.asl")
   Include("buses.asl")
   Include("touch.asl")
   Include("wcnss_bt.asl")
   ```
   and in `Dsdt.asl` uncomment / add `Include("cust_arraybutton.asl")`.
3. Rebuild (step 2). `build.sh` compiles the DSDT with `iasl` (apollo `USE_IASL=true`).

## 4. Pull the firmware blobs [PHONE]
Use the ios-apollo pullers (same blobs Windows needs — see `driver-pack.md`):
`pull-gpu-fw.sh`, `pull-wifi-fw.sh`, `pull-bt-fw.sh`, `pull-audio-fw.sh`, `pull-mic-haptic-fw.sh`;
touch FW `novatek_nt36675_j3s_fw01.bin` from MiCode `apollo-q-oss`.

## 5. Build the Windows image + inject drivers [PC]
1. `dism /Apply-Image` the ARM64 `install.wim` to the target partition.
2. Inject the Kona driver pack:
   `dism /Image:<mnt> /Add-Driver /Driver:windows_silicon_qcom_kona /Recurse /ForceUnsigned`
   plus the **Novatek touch driver** (the one gap — see `driver-pack.md`).
3. Place the Qualcomm/QCA/Novatek firmware where the drivers expect it.
4. `bcdboot` the Windows partition; set the UEFI as the boot payload.
(The WOA-Project deployment tooling / Renegade wiki automate most of 5 — check the current wiki,
which this container can't reach.)

## 6. Boot [PHONE]
```sh
fastboot boot uefi.img          # temporary: nothing permanent flashed yet
```
First boot runs Windows setup. Once stable, flash the installer zip (step 2) to make it permanent.

## What to expect (per `acpi-extension.md` + `ios-apollo` reality)
- **Works early:** display (framebuffer), USB, UFS storage, CPU, power states.
- **Added by this repo's ACPI:** touch, Bluetooth, buttons, the SPI/UART/GPIO controllers they
  need (driver for Novatek touch still to source).
- **Spec'd, not yet wired:** Wi-Fi (PCIe), audio (LPASS/SoundWire), charger (SPMI/PMIC), haptics.
- **Out of scope:** modem/cellular, cameras.
Bring up one peripheral at a time; read the UEFI/Windows logs between boots.
