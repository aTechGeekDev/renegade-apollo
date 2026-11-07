# Handoff: renegade-apollo (Windows on Mi 10T / apollo, SM8250)

## Current state (latest; read this first)

Branch `claude/busy-johnson-ghrd5y`, everything pushed. Windows 11 24H2 runs to the desktop on apollo from
Mu-Silicium (`mu/apolloPkg`, alioth binaries), booted tethered with `fastboot boot Mu-apollo.img`; test-signing on;
WOA-Project Kona pack installed. The decisive change was **no PEP** (`--no-pep`): Qualcomm's qcpep cannot run on
SM8250 (uefi/apollo/README.md, "Decision: run without Qualcomm's PEP"), so every Qualcomm driver that needs PEP
(UART, SPI, PCIe, audio, GPU, Wi-Fi) is out, and devices are driven by **our own drivers that touch the hardware
directly**, ported from the user's ios-apollo repo (`~/ios-apollo` in the session; xnu/patches/*).

### Build the UEFI (user's Kali VM)
```
cd ~/renegade-apollo && git pull
LOGO=~/logo.img bash mu/install-apollo.sh        # defaults below
cd ~/Mu-Silicium && python3 build_uefi.py -d apollo   (in its venv; CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-)
fastboot boot Mu-apollo.img
```
`install-apollo.sh` defaults: `DROP=PRTC,PMAP`, `--no-thermal`, `NO_PEP=1`, `TOUCH=1` (\_SB.TCH0, APLO0001),
`BT=1` (\_SB.BTH0, APLO0002), `FB_SHIFT=0` (full screen; 256 only for reading bugcheck text under the camera hole),
`LOGO=<dump of the phone's logo partition>` -> `make-boot-logo.py` crops the Mi logo into SiliciumPkg's BootLogo.bmp
(local checkout only; `LOGO=silicium` restores). Logo dump: `adb shell dd if=/dev/block/by-name/logo of=/tmp/logo.img`
then `adb pull` (from recovery). **Status: the TOUCH/BT/LOGO/FB_SHIFT=0 rebuild has not been reported back yet.**

### Own Windows drivers (`windrivers/`)
Built by `.github/workflows/drivers.yml` (windows-latest, WDK NuGet 10.0.26100.2454, KMDF 1.15) on every push to
`windrivers/**`; published to branch **`driver-builds`** (raw: `https://raw.githubusercontent.com/aTechGeekDev/renegade-apollo/driver-builds/<file>`)
with SHA256SUMS. Each build stamps `DriverVer 1.0.<run>.0` so pnputil replaces older installs (exit 259 = it kept the
installed one). Installers test-sign with the self-made cert `CN=renegade-apollo test signing` and add it to
LocalMachine Root/TrustedPublisher with **certutil** (Import-Certificate is refused there on this install).
The user transfers zips to the phone and runs them from an Administrator PowerShell; progress of every driver is
written as REG_SZ values under `HKLM\SYSTEM\CurrentControlSet\Services\<driver>\Parameters` (no debugger needed;
each installer has `-Status` and `-Remove`).

| Driver | What | Status on the phone |
| --- | --- | --- |
| `apollocpufreq` | legacy driver: EPSS domains 0x18591000/2000/3000, top LUT row <= `MaxMHz` | built; **not yet run/reported**. No thermal zones exist -> advise a cap (`-MaxMHz 1800`) until a governor with a TSENS limit (ios-apollo 0153) is ported |
| `apollotouch` | NT36675 on QUP0 SE4 SPI 0x990000 (GCC clocks, gpio8-11, reset gpio38, IRQ gpio39 polled), firmware download from `System32\drivers\novatek_nt36675_j3s_fw01.bin` (installer converts Xiaomi's apollo-q-oss ihex, SHA-256 checked; never committed), HID via **VHF** (10-finger touchscreen) | **WORKS** (user: "touch working"). Was offset only because of FB_SHIFT=256 (fixed by FB_SHIFT=0; `TopRows` param exists for shifted builds). Long press registered as a tap -> fixed in 1.0.9.0 (safety-poll frames no longer lift contacts; held contacts re-sent every 25 ms); **1.0.9.0 not yet confirmed** |
| `apollobt` | QCA6390 Bluetooth: RPMh votes (cmd-db 0x80860000; smpa6/smpa5/smpc8/ldoa16), BT_EN gpio21, SE6 UART 0x998000 (gpio16-19) polled, ROM check, phone's firmware `htbtfw20.tlv` + `htnv20.bin` (from its `bluetooth` partition; ios-apollo `tools/pull-bt-fw.sh`), UART to 921600, IBS; then a child PDO `apollobt\UART_H4` with compatible ID `MS_BTHX_BTHMINI` -> inbox BthMini (IOCTL_BTHX_*, bthxddi.h) | built (1.0.10.0, zip sent); **never run**. Params: `Steps` 1-5 (1-4 diagnostic only), `Baud`, `Address` (REG_BINARY 6). If Windows hangs after install: boot a UEFI built with `BT=0` |

### Wi-Fi (researched, not started)
Qualcomm's Kona `qcwlan8250` binds QCMS (subsystem/PEP stack): unusable. **Lead:** WOA-Project/Qualcomm-Reference-Drivers
`8180_CLS/*/qcwlan6800sc8180.cab` is an ARM64 NDIS driver for **PCI\VEN_17CB&DEV_1101&SUBSYS_010817CB** (QCA6390)
with its firmware, no PEP dependency. Needed first, all untested: chip power + WLAN_EN gpio20 (ios-apollo
`sweet_rpmh_wifi_power`, 0126 sweet_wifi.c), PCIe0 PHY/link (ios-apollo 0126 sweet_pcie*), ECAM mode for pci.sys
(SM8250 = DWC cfg_1_9_0, which has PARF_ECAM_BASE + outbound blocker, see Linux pcie-qcom.c) + PCI0/MCFG in ACPI,
an identity SMMU context for stream 0x1c01, interrupts (MSI via GIC 0x17a10040 SPI 768+, or INTx) - the main unknown;
apollo's subsystem ID must match the INF (else edit + re-catalog). Plan offered to the user: staged driver that only
powers the chip, brings the link up and reports IDs. Meanwhile: USB Ethernet / USB tethering (USB host works).
Also found: alioth's `qcfakepep` (stub PEP, github.com/18934020489Xx/alioth-woa-drivers) reportedly unlocks the
audio chain; WOA-Kona/Drivers has MIT GPIO/SPMI/PMIC drivers. Silicium's status lists alioth Windows BT/speakers/sensors working.

### Open items / what the user is waiting on
1. Results: rebuilt UEFI (logo, full screen, TCH0+BTH0), touch 1.0.9.0 long press, apollobt install output, apollocpufreq + winsat.
2. User asked about Windows Update: advised OK with driver updates blocked (ExcludeWUDriversInQualityUpdate=1,
   SearchOrderConfig=0), restore point, full battery, no max-clock cpufreq, skip feature updates, re-`fastboot boot` on each restart.
3. ios-apollo ports still possible: CPU governor + TSENS thermal limit (0153, safety), WLED brightness (0151/0154),
   battery PM8150B (0182, unproven), buttons (ask user if they work), PMIC RTC (0181), torch.
4. Firmware flashing (permanent boot) stays last, per the user.

Rules the user set: short copy-pasteable steps, say what is proven vs guessed, ask before turning the debugger on,
confirm destructive steps; never commit device dumps or vendor firmware (touch fw, BT fw, logo, Wi-Fi fw).

---

Written 2026-11-07 by the session that continued after the original one hit its limit. Branch:
`claude/busy-johnson-ghrd5y` (all work below is pushed there). Read this, `git pull`, then
`uefi/apollo/README.md` (sections "Root cause ... PEPH" onward) and `mu/README.md`.

## Where things stand (one paragraph)
Windows 11 24H2 (26100) ARM64 boots its kernel on apollo from both firmwares, but stops before any device starts.
The cause on edk2-msm firmware is **proven with the kernel debugger**: `qcpep8250.sys` (PEP driver) writes `0x3A` to the
LMh block at `0x18350800` inside its AddDevice; that MMIO write never completes, so CPU 5 freezes while Windows' boot
thread holds the PnP lock. A workaround (patched + test-signed `qcpep8250.sys`) is written and pushed but **not yet run
on the phone**. The user is waiting to try it.

## Commits this session (oldest first)
| Commit | What |
| --- | --- |
| `6bf421a` | `-DENABLE_PUS3` in apollo build flags (README step 1, sed checked against upstream `apollo.dsc`); removed committed `__pycache__` |
| `3916081` | `FB_SHIFT` (default 256 rows) in `bisect-build.sh` and `mu/install-apollo.sh`: framebuffer below the punch hole so bugcheck / UEFI text is readable; `FB_SHIFT=0` = full screen |
| `21fce7e` | README: the qcpep AddDevice hang, with the debugger evidence |
| `a849e8a` | Mu apolloPkg: Qualcomm charger drivers restored (DXE.inc/APRIORI.inc now = alioth's). Without them Mu UEFI died with `Synchronous Exception at 0xAA0003F594000415` (a jump through a pointer holding code bytes) |
| `55da523` | `uefi/apollo/patch-qcpep-lmh.py` + `uefi/apollo/sign-install-qcpep.ps1` (the workaround) |

## Debugger results (image E = edk2-msm + PEPH fix + PUS3 + shifted FB)
- PEPH fix works: `qcpep` no longer `CM_PROB_FAILED_ADD`. All 8 CPUs up (`!cpuinfo`).
- No devnode ever started; `storufs` has no device. WDF IFR log for qcpep: only `WdfDevStatePnpInit`.
- `!pnptriage`: `nt!PiEngineLock` exclusive + `IopDeviceTreeLock` held by thread `Phase1Initialization`, RUNNING on CPU 5.
  CPU 5 never answers the freeze IPI (no context); all other CPUs freeze normally.
- `dps` over that thread's stack: `IopInitializeBootDrivers -> PipCallDriverAddDevice -> Wdf FxDriver::AddDevice ->
  qcpep8250 +0x213810/+0x1c6630 -> +0x2116c4 -> +0x1dbfe8 (call at +0x1dc308) -> +0x1df300 (call at +0x1df358) ->
  +0x5c50 -> +0x5b88 (call at +0x5fa4) -> +0x3398` = `dsb sy; str w1,[x0]` to the mapping of **0x18350800**.
- `+0x5c50` maps 0x18350800 and 0x18358800 (0x2C each, `MmMapIoSpaceEx`) unconditionally, then writes 0x3A/0.
  These are LMh (Limits Management Hardware) cluster blocks (mainline sm8150 DT has `lmh@18350800`/`lmh@18358800`;
  apollo's stock Linux DT does not describe them). qcpep has `LMH_DCVSH_*` strings around this code.
- Other qcpep threads: a WDF work item waits (no timeout) on an event set only in SelfManagedIoInit; qcpep's own
  worker waits on a semaphore. Both are consequences, not causes.

Static analysis was done on `qcpep8250.sys` 1.0.2140.0 from `WOA-Project/windows_silicon_qcom_kona`
(SHA-256 `39fafcd6...4e01`). The phone's stack offsets match this build exactly.

## Hypothesis (not proven)
The block needs firmware setup that edk2-msm's sm8250 build never does. Mu-Silicium's Kona firmware runs Qualcomm DXE
drivers edk2-msm lacks: **LimitsDxe** (prime suspect), CPRDxe, PdcDxe, TsensDxe, AdcDxe, GpiDxe, PILDxe, SPSSDxe,
FeatureEnablerDxe. The same qcpep boots on pipa/alioth under Mu.

## Mu-Silicium status
With the charger drivers back, `Mu-apollo.img` (DEBUG build) passes UEFI and the Windows kernel starts (debugger
connected). Windows then froze completely after a few seconds of spinner: **the debugger could not break in at all**
(every CPU stuck, unlike edk2-msm where only CPU 5 was). Not examined; could be the same LMh write or something new.
A breakpoint plan was given but not run (user refused more debugging): `sxe ld:qcpep8250.sys`, then
`bp qcpep8250+0x5c50` (before the write), `+0x5fa8` (after the 0x18350800 write), `+0x5fb4` (after 0x18358800).

## The workaround (pushed, untested on the phone)
- `patch-qcpep-lmh.py`: only accepts the 1.0.2140.0 SHA-256; turns `+0x5520` (reads LMh +4), `+0x5b20` (writes +8) and
  `+0x5b88` (writes +0) into `ret` after checking the original instructions; all callers ignore the results. Strips
  the now-invalid embedded signature (it was the last thing in the file), recomputes the PE checksum (the routine
  reproduces the original's `0x230864`). Verified here by disassembling the output; refuses a second run.
- `sign-install-qcpep.ps1` (**never executed: no PowerShell in the cloud container**): self-signed code-signing cert
  (`CN=renegade-apollo test signing`, CurrentUser\My), `Set-AuthenticodeSignature` SHA256, then replaces every
  `qcpep8250.sys` in `W:\Windows\System32\DriverStore\FileRepository\qcpep.*\` (the INF uses `%13%`, run from driver
  store) and `System32\drivers` if present, keeping `.orig`; `-Restore` undoes it; `-EspDrive S: -DebugOff` sets BCD
  `testsigning on`, `debug off`.
- Risks: (1) the script is untested; (2) assumes test-signing mode accepts a self-signed cert for a boot-start driver
  (a past note says winload rejected `qcppx` with `0xc0430007` while other test-signed boot drivers loaded); (3) the
  next blocker after this hang is unknown.

## Instructions the user has (in order)
1. Kali: `git pull`; download `qcpep8250.sys` from the WOA repo (or their `C:\kona-pack`); `python3
   uefi/apollo/patch-qcpep-lmh.py qcpep8250.sys qcpep8250-lmhfix.sys`.
2. Phone in OrangeFox mass storage; PC: Windows partition `W:`, ESP `S:`.
3. Admin PowerShell: `sign-install-qcpep.ps1 -Sys .\qcpep8250-lmhfix.sys -WindowsDrive W: -EspDrive S: -DebugOff`.
4. `fastboot boot Mu-apollo.img`; if Mu fails, `fastboot boot ~/boot-apollo-E.img`. Report a photo.

## If it still does not boot
- Driver-signature error at boot: signing assumption failed. Options: different signing route (signtool with a test
  cert), or `-Restore`.
- Same freeze: on edk2-msm there may be more LMh/other unclocked accesses after AddDevice. Without the debugger, only
  screen photos are available; the user strongly prefers no debugging. Ask before re-enabling it.
- Longer-term proper fix: run LimitsDxe (and possibly CPRDxe/PdcDxe/TsensDxe) before Windows. Mu already does; for
  edk2-msm, borrow Mu's Kona binaries into `apollo.fdf.inc` (riskier), or make the Mu port the main path.

## Working notes
- `ios-apollo` (public, read-only clone at `/home/user/atechgeekdev/ios-apollo` in this container) boots XNU from
  Xiaomi ABL via a shim, no UEFI, and never touches the LMh blocks ("only hardware LMh"). Nothing to reuse for this hang.
- User context: Kali for building (`~/Mu-Silicium`, `~/edk2-msm`, `~/renegade-apollo`, `~/Silicium-ACPI`), a Windows PC
  for WinDbg/DISM/bcdedit. The user is frustrated with long debug loops and wants boots that need no intervention:
  keep instructions short and copy-pasteable, and be explicit about what is proven and what is guessed.
- `bisect-build.sh` defaults `GEN=~/make-apollo-dsdt.py`; pass `GEN=~/renegade-apollo/uefi/apollo/make-apollo-dsdt.py`.
