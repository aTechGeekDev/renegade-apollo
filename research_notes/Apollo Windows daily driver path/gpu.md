# GPU acceleration (Adreno 650) and a real display driver for Windows 11 ARM64 on Xiaomi Mi 10T (apollo, SM8250)

Researched 2026-11-07. Labels: **[Verified]** = checked directly in primary data this session (driver binaries/INFs,
DSDTs, kernel source, repo notes, read locally from git clones); **[Claim]** = stated by a source, not checked;
**[Inference]** = my reasoning. Network limits this session: learn.microsoft.com and lists.freedesktop.org did not
resolve (blocked), so Microsoft docs are cited only through GitHub (Windows-driver-samples) or search snippets.
General web search found almost nothing on SM8250 Windows GPU work; the primary evidence is in GitHub repositories.

Repos read (shallow clones, commit in brackets):
- WOA-Project/windows_silicon_qcom_kona [22221a0, 2026-01-04] = the Kona pack installed on apollo
- Project-Silicium/Silicium-ACPI [f50bc11, 2026-10-29]; Project-Silicium/Mu-Silicium [8471e47, 2026-11-05]
- zhishixuebao-0791/K40_Win11 (Alioth-Engineering) [6316c56, 2026-10-23]
- 18934020489Xx/alioth-woa-drivers [3e5e052, 2026-10-28]
- MiCode/Xiaomi_Kernel_OpenSource branch apollo-q-oss [acee5ab] (panel dtsi)
- microsoft/Windows-driver-samples (KMDOD README)
- Local: aTechGeekDev/ios-apollo NOTES.md [1f75f38, 2026-11-05]; renegade-apollo uefi/apollo/make-apollo-dsdt.py

## 1. Which Qualcomm Windows display/GPU driver exists for SM8250, and what does it depend on?

### Takeaway
The Kona pack ships one combined display + GPU WDDM driver, `qcdx8250.inf` / `qcdxkm8250.sys` (27.20.2140.0,
Oct 2024). It is internally `qcdxkm8180.sys` (the SC8180X driver, like `qcpep8250` = `qcpep8180`), signed with the
"Windows On Andromeda" test-style certificate (not WHQL). It binds `ACPI\VEN_QCOM&DEV_253A` (\_SB.GPU0) and depends
on a working PEP for all clocks/GDSCs/bus votes (via PEP0's ACPI power tables and PoFx), on qcsmmu (MMU0/MMU1),
qciommu, PIL/SCM/TrEE (secure microcode/zap), PMIC (WLED backlight), plus a panel XML returned by GPU0.\_ROM.
apollo's current DSDT deletes GPU0 and all of these dependencies, so nothing can bind today.

### Cited Findings
- [Verified] Kona pack `Drivers/Graphics/DXKM/` contains `qcdx8250.inf` (DriverVer `10/30/2024, 27.20.2140.0000`),
  kernel drivers `qcdxkm8250.sys` (2,912,864 B), `qcdxkmti8250.sys`, `qcdxkmrs38250.sys` (per-OS-version variants),
  D3D11/D3D12 UMDs (`qcdx11arm64xum8250.dll`, `qcdx12arm64xum8250.dll`), Vulkan ICD (`qcvkarm64xum.dll`,
  `qcvk_icd_arm64x.json`), OpenCL (`OpenCL_adreno.dll`), WSL UMD (`libqcdx12arm64wslum.so`), and firmware files
  `qcvss8250.mbn` and `qcdxkmsuc8250.mbn`. Also `Drivers/Graphics/QDCM/qdcmlib8250.inf` (display colour component
  `SWC\VEN_QCOM&DEV_253A&SWC_QDCM8250`). — [Kona pack DXKM folder](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Graphics/DXKM)
- [Verified] INF hardware IDs: `ACPI\VEN_QCOM&DEV_253A` (generic), `...&SUBSYS_MTP08250`, and REV-specific entries;
  device strings "Adreno 640 GPU" (generic/MTP), "Adreno 635" (REV_0D13/0912), "Adreno 645" (REV_0D01/0D11/0900/0910),
  "Adreno 650" (REV_0D15/0913). Service `QCDX`, demand start, `ErrorControl = IGNORE`, binary from the driver store
  (`%13%`). The INF sets `HKLM\...\GraphicsDrivers\DisableVersionMismatchCheck=1` and, for MTP only, `Miniport\DISPLAY\Config`
  `PanelCfg1BrightnessMin/MaxLuminance`. — [qcdx8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Graphics/DXKM/qcdx8250.inf)
- [Verified] `qcdxkm8250.sys` strings: PDB path `Z:\b\WP\direct3d_kmd\rel\10.5.1\kmd\driver\miniport\ARM64\Release\qcdxkm8180.pdb`,
  OriginalFilename `qcdxkm8180.sys`, FileVersion 27.20.2140.0000, PE timestamp 2024-10-30. It embeds a list of
  firmware names for other chips (`qcvss8150/8280/7180/...mbn`, `qcdxkmsuc8150/8280/7180/850.mbn`) — i.e. a
  multi-SoC SC8180X-branch driver, renamed to "8250" like the pack's qcpep. Signer: "Windows On Andromeda KMCI
  Codesigning" / "Windows On Andromeda Production PCA" (same chain as `qcsmmu8250.sys`). — [qcdxkm8250.sys](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Graphics/DXKM/qcdxkm8250.sys)
- [Verified] The two `.mbn` files are not GPU firmware: `qcvss8250.mbn` is an ARM ELF with
  `QC_IMAGE_VERSION_STRING=VIDEO.IR.1.2-00040-PROD-1` (Venus video-codec firmware; the driver has `VedPilLoad`/
  `IOCTL_PIL_TZ_LOAD_IMAGE` strings for it), `qcdxkmsuc8250.mbn` is a Hexagon (QDSP6) ELF. No a650 SQE/GMU/zap file
  ships; the driver has `LoadMicrocode`, `SecureUcodeLoad`, `SecureUcodeAuthenticate`, `EnableFallbackToDisableSecureMode`,
  `DisableUCodeBootstrap`, `GmuPwrStart/GmuPwrRequestPowerOn` strings. — same binary (local strings dump)
- [Verified] Power/clock model strings in `qcdxkm8250.sys`: `PoFxRegisterComponentPerfStates`,
  `PoFxIssueComponentPerfStateChange`, `ICBID_MASTER_GFX3D`, `ICBID_SLAVE_GFX3D_CFG`, `ICBID_SLAVE_EBI1`,
  `GPU_RPMH_CPRF`, `FOOTSWITCH_OVERRIDE`, `\Device\QCOM_PEPDriver`, callback `PEPLTVRGFXRestrictionCB`; TrEE/SCM
  interface use (`IOCTL_SCM_SECURE_APP_*`, `hTZInterface`, `CryptoInitTrEE`). Backlight via PMIC WLED
  (`BacklightPmicWled*`, `BacklightPmicBankSelect`, `PMIPowerPmicModel`). — same binary
- [Verified] ACPI methods the driver looks up (4-char strings): `_ROM`, `ROM_`, `GPU0`, `PGRT`, `PBRT`, `PBRC`, `BLCP`,
  `DITH`, `PIGC`, `PLGC`, `PPCC`, `PWGM`, `GCTL`, ...; panel-XML key names (`DSIInitSequence`, `DSIDSCEnable`,
  `DSIDSCSliceWidth`, `DSIDynamicRefreshRates`, `BacklightPmicControlType`, ...), plus `Miniport\PanelOverride`,
  `edidoverride.xml`, `PanelEdid`. — same binary
- [Verified] A real Kona-style GPU0 (Silicium pipa DSDT): `_HID "QCOM053A"`, `_DEP` = `MMU0, MMU1, IMM0, IMM1, PEP0,
  PMIC, PILC, RPEN, TREE, SCM0`; `_CRS` has MDP/DP-PHY/VSYNC IRQ/GFX/GPU-CC/PDC/GMU regions (alioth's copy shows
  `0x0AE00000` MDSS, `0x088E0000` DP PHY, `0x03D00000` GFX, `0x03D90000`/`0x03DE0000` etc.) and a `RESI` table naming
  them (MDP_REGS, DP_PHY_REGS, VSYNC_INTERRUPT, GFX_REGS, GPU_PDC_*, GPU_CC...). Methods: `PMCL`, `_ROM` (panel XML,
  several variants PCFG/PCF1..5), `PIGC`, `PPCC`, `PBRT`, `BLCP`. — [pipa DSDT.dsl, line ~67831](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl)
- [Verified] The display's clocks/GDSC/bus votes live in PEP0, not in the GPU driver: pipa's `\_SB.PEP0.G0MD` has a
  `"DEVICE", "\\_SB.GPU0"` package with COMPONENT/FSTATE/PSTATE entries: `FOOTSWITCH mdss_core_gdsc`, 94 `CLOCK`
  entries (`disp_cc_mdss_mdp_clk`, `_pclk0_clk`, `_byte0_clk`, `_esc0_clk`, `gcc_disp_hf_axi_clk`, ...), 126 `BUSARB`
  (`ICBID_MASTER_MDP0`→`ICBID_SLAVE_EBI1`), `PMICVREGVOTE` (`PPP_RESOURCE_ID_LDO1_C`, `LDO3_C`, `LDO5_A`), `TLMMGPIO`;
  GPU core resources (`gpu_cc_cxo_clk`, `gpu_cc_cx_gmu_clk`, `gcc_gpu_memnoc_gfx_clk`) follow. Executing these
  tables is qcpep's job. — [pipa DSDT.dsl, lines ~3708-7300](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl)
- [Verified] SMMU driver: `qcsmmu8250.inf` binds `ACPI\VEN_QCOM&DEV_2509&REV_0001/0002` and loads stream tables from
  `SMMC.bin`; IOMMU driver `qciommu8250.inf` binds `ACPI\QCOM259B`; other GPU0 dependencies in the pack:
  `qcpil8250` (QCOM251B), `qcscm8250` (QCOM250B), `qcrpen8250` (QCOM2533), `QcTrEE8250` (QCOM2576), `qcpmic8250`
  (QCOM252E), qcpep (QCOM2519 + many sub-IDs). — Kona pack INFs under [Drivers/SOC](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/SOC)
- [Verified] K40/alioth reverse engineering: `{55beca03-ba2c-4fbc-9efc-ee8eca56f0c7}` is a device interface created
  by qcpep (both qcpep8280 and qcpep8250 call `WdfDeviceCreateDeviceInterface` with it); "the other Qualcomm drivers
  (smmu, subsys, gpu, i2c/spi/uart, camera, scm...) are consumers"; qcsmmu fails AddDevice (Code 31) when the interface
  is absent. qcsmmu8280 additionally asks PEP for QueryInterface `{a0e1dd36-...}` (one function). —
  [K40 dead-ends-and-breakthroughs.md, items 8 and 13](https://github.com/zhishixuebao-0791/K40_Win11/blob/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)
- [Verified] On ARM64 Windows without Hyper-V, `IoGetIommuInterface` returns `0xC00000BB` even with a correct IORT
  (`nt!HalpGetIommuInterface` only checks `HalpHvIommu`); Hyper-V/EL2 needs Secure Launch, which QHEE only allows on
  factory-Windows devices (K40 cites the slbounce README: "Can this be used on android devices? No."). So the kernel
  IOMMU path that lets qcsmmu skip PEP is closed on Android phones. — [K40 phase18a IORT plan/result](https://github.com/zhishixuebao-0791/K40_Win11/blob/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes/acpi-hid-phase18a-iort-plan-20261022.md)
- [Verified] apollo's DSDT generator removes GPU0, MMU0/1, IMM0/1, TREE, PILC (and PEP0 is neutralised with
  `--no-pep`), and renames QCOM05xx→QCOM25xx to match the Kona pack (so pipa's GPU0 `QCOM053A` would become
  `QCOM253A` = the qcdx INF ID). — renegade-apollo `uefi/apollo/make-apollo-dsdt.py` (DROP list, lines 27-33; docstring step 2)
- [Verified] Hardware facts from Linux (via K40's IORT work): adreno_smmu `0x03da0000/0x10000`, global IRQs SPI 672/673,
  8 context IRQs SPI 678-685; apps_smmu `0x15000000`. — [K40 phase18a build result](https://github.com/zhishixuebao-0791/K40_Win11/blob/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes/acpi-hid-phase18a-build-result-20261023.md)
- [Verified] What the A650 needs at the hardware level (proven under XNU on apollo by ios-apollo): GPU CC/CX GDSC,
  GMU firmware `a650_gmu.bin` + HFI + RPMh (cmd-db gfx/cx/mx levels), GX power, GPU SMMU context (stream 0, mask
  0x400/0x401), SQE microcode `a650_sqe.fw` (version 0x093 from the phone; Linux wants >=0x095), and the zap shader
  `a650_zap.mdt/.b0x` authenticated by TrustZone (PAS image 13, `PAS_MEM_SETUP` at `0x8681a000`) to leave secure
  mode; then the 3D core drew (boot 26, 2026-10-24) and Turnip/Metal ran on it later. — [ios-apollo NOTES.md (boots 12-26, firmware entry)](https://github.com/aTechGeekDev/ios-apollo/blob/1f75f381e31da96de537653fbac2252d2eb80418/NOTES.md)

### Inferences
- The Kona "8250" display driver is an SC8180X-branch KMD that also carries A650/Kona IDs in its INF; whether its
  kernel code really supports the A650 + SM8250 DPU (MDSS 6.0) has never been demonstrated publicly. Given qcpep8250
  (same renaming) hard-codes SM8150/8180 LMh addresses, a similar risk exists for chip-specific code in qcdxkm.
- Without a functioning PEP, qcdx gets no clocks, GDSCs, RPMh/ICB votes or PoFx P-state handling: those are executed
  by qcpep from PEP0's tables. A stub PEP that answers "success" without touching hardware does not turn on
  `mdss_core_gdsc`, `disp_cc_*` clocks or the GX rail. The display only works today because UEFI left MDSS running.
- The secure microcode/zap path (`SecureUcodeLoad`, SCM/TrEE) means qcscm/QcTrEE (and probably qcpil/RPEN) must also
  run; TrEE/SCM on Kona also consume the PEP interface (K40 list of consumers).

### Gaps
- No disassembly of qcdxkm8250 was done: exact PEP interfaces it requires (beyond `\Device\QCOM_PEPDriver` and the
  `{55beca03}` class), where the A650 SQE/GMU/zap images come from (embedded vs. loaded), and whether it checks the
  chip ID / refuses on SM8250 are unknown.
- Meaning of `REV_0D15/0913` (Adreno 650) in the INF (which ACPI value supplies REV) not confirmed.

## 2. Has any SM8250 Windows port achieved GPU acceleration? What blocked them? Does qcfakepep remove the PEP blocker?

### Takeaway
No. As of 2026-11-05, Mu-Silicium lists Windows GPU as not working on every SM8250 device it supports (Poco F3
alioth, Xiaomi Pad 6 pipa); the only Silicium devices with Windows GPU working are SM8150/SM7125/SM7325 phones whose
SoCs have a working Qualcomm Windows BSP. The blocker everywhere on SM8250 is the PEP (qcpep8250 = SM8150 LMh code,
qcpep8280 = SC8280XP TSENS code; both hang), with SMMU and signing as follow-on blockers. qcfakepep is two weeks old,
its own deploy doc lists only *expected* results, and by design it only satisfies the interface handshake; it cannot
provide the clock/power work the display/GPU driver needs. The "lifts the PEP dependency from the GPU stack" claim
is unverified and, for real GPU operation, very likely wrong.

### Cited Findings
- [Verified] Mu-Silicium Status.md (commit 2026-11-05): "Xiaomi Poco F3 / Redmi K40 / Mi 11X" — Windows column:
  GPU ❌, Display ✅, Touchscreen ✅, Bluetooth ✅, Speakers ✅, USB host ✅; Linux column: GPU ✅. "Xiaomi Pad 6": Windows
  GPU ❌ (also touch/BT/speakers ❌). OnePlus 8T (SM8250) GPU ❌; Galaxy S20 5G (SM8250) "WinPE Only", GPU ❌. Of 47
  device entries with a Windows GPU row, only Xiaomi Pad 5 (SM8150), Mi 9 (SM8150), Poco X3 Pro (SM8150), Redmi Note 9 Pro
  family (SM7125) show ✅ and Mi 11 Lite NE (SM7325) ⚠️ "visual artifacts". — [Mu-Silicium Status.md](https://github.com/Project-Silicium/Mu-Silicium/blob/8471e47f5248ebbffc2e6196b683204163efe155/Status.md)
- [Verified] Silicium's alioth DSDT (updated 2026-08-06 "Alioth: Update ACPI") gives GPU0 `_HID "QCOM2536"`, which in
  the Kona pack is the camera JPEG encoder ID (`qccamjpege8250.inf: ACPI\QCOM2536`), not the display driver's
  `DEV_253A`; GPU0 still `_DEP`s on PEP0/MMU/IMM/PILC/RPEN/TREE/SCM0. Its `_ROM` holds a panel XML for
  "AMS667xx01 — Samsung DSI Command Mode Panel with DSC (1080x2400 24bpp)". — [alioth DSDT.dsl, line ~14830](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl); [Kona qccamjpege8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Camera/JPEGEncoder)
- [Verified] K40 / Alioth-Engineering (Poco F3) findings: qcpep8250 (internally qcpep8180) freezes a CPU writing
  SM8150 LMh at `0x18350800/0x18358800`; qcpep8280 hangs on SC8280XP-only TSENS controllers (`0x0c224000/0x0c251000`);
  without PEP there is no `{55beca03}` interface → qcsmmu Code 31 → ADSP fails. Kona-pack drivers signed by "Windows On
  Andromeda KMCI Codesigning" are refused (CI 3004 / Code 52) unless test signing is on; qcsmmu8250 under test signing
  without a PEP hangs boot at the logo; the WHQL Surface qcsmmu8280 + 8250 INF config loads but Code 31.
  `EnableStubbedPepForPresilicon=1` in qcpep8280 answers `{a0e1dd36}` with success and skips the TSENS init (marked
  "待实测" = untested). The repo's last commit is 2026-10-23 and its notes never reach the GPU. — [K40 dead-ends-and-breakthroughs.md](https://github.com/zhishixuebao-0791/K40_Win11/blob/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)
- [Claim] alioth-woa-drivers README: qcfakepep "reproduces that semantics with **zero MMIO access**. Unlocks the whole
  audio chain (SMMU → qcsubsys → ADSP) and lifts the PEP dependency from the GPU stack." — [alioth-woa-drivers README](https://github.com/18934020489Xx/alioth-woa-drivers/blob/3e5e052b2de21fa63e7303a6e34b65f7796cd008/README.md)
- [Verified] qcfakepep.c (389 lines) only: publishes device interface `{55beca03}` and answers `IRP_MN_QUERY_INTERFACE`
  for `{a0e1dd36}` v1 with a 0x28-byte structure whose function returns success; everything else is passed down.
  No PoFx/PEP (`PEP_*`) platform-extension registration, no clock/footswitch/bus handling. — [qcfakepep.c](https://github.com/18934020489Xx/alioth-woa-drivers/blob/3e5e052b2de21fa63e7303a6e34b65f7796cd008/qcfakepep/qcfakepep.c)
- [Verified] Repo history: all ten commits on 2026-10-28 (single evening); DEPLOY.md gives "预期" (expected) outcomes,
  e.g. "预期 MMU0/MMU1 从 Code 31 → OK" and "worst case: devices stay Code 31 (harmless)"; no test log is published.
  — [alioth-woa-drivers DEPLOY.md](https://github.com/18934020489Xx/alioth-woa-drivers/blob/3e5e052b2de21fa63e7303a6e34b65f7796cd008/DEPLOY.md)
- [Verified] On SM8150 devices where Windows GPU works, the DSDT GPU0 has the same structure and the same PEP0 `_DEP`
  (vayu: `_HID "QCOM053A"`, `_DEP` MMU0, MMU1, IMM0, IMM1, PEP0, PMIC, PILC, RPEN, TREE, SCM0; methods `ENGS`, `TMDT`,
  `PMCL`, `_ROM`). — [vayu DSDT.dsl, line ~66436](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/vayu/Decompiled/DSDT.dsl)
- [Claim] General web: a Linus Tech Tips thread considered Windows on SD865 theoretical, with GPU drivers based on a
  prototype leak; no source found showing a working Adreno 650 Windows driver. — [LTT forum](https://linustechtips.com/topic/1424934-windows-1011-on-a-qualcomm-snapdragon-865/)
- [Verified] GitHub issue search (REST) found no Mu-Silicium or windows_silicon_qcom_kona issue reporting a working
  SM8250 Windows GPU; the Kona pack README says "Currently non functional chipset features ... consider 'everything'
  for now" and supports officially only the MTP. — [Kona README](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/README.md)

### Inferences
- On working SM8150 ports the GPU stack works because a real qcpep (8150 BSP) runs PEP0's power tables. On SM8250
  the missing piece is exactly that power layer, so the realistic "Qualcomm driver" route is: a PEP that both
  publishes the interfaces *and* implements the GPU0/display power components (GDSC on, clocks at required rates,
  ICB/RPMh votes) — either a patched qcpep8250 (LMh/OSM writes removed; renegade-apollo already has an untested patch
  for three LMh functions, K40 found two more: `+0x6288`, `+0x6388`) or a custom PEP (a Windows PEP platform
  extension, `PEP_*` notifications) that implements the subset of PEP0's tables qcdx uses.
- qcfakepep may let MMU0/MMU1 (qcsmmu) and maybe qcsubsys start; even then qcdx would start with unpowered hardware
  except what UEFI left on (MDSS, DSI, panel). GX/GMU power-up needs RPMh and GPU CC programming that only a PEP
  (or our own driver) would do. Expect either a qcdx start failure (Code 43/TDR) or a display-only start at best.
- apollo deliberately runs `--no-pep`; adding a stub PEP is reversible but GPU0 also needs MMU/IMM/TREE/SCM0/PILC/RPEN
  devices back in the DSDT (all currently dropped), each with its own driver risk (qcsmmu8250 hang reported on alioth).

### Gaps
- No public report of anyone installing qcdx8250 on an SM8250 phone and the resulting status code. Renegade/WOA
  Telegram groups (t.me/project_aloha_issues, t.me/DuoWOA, named in the Kona README) were not searchable.
- Whether Mi 10 (umi), OnePlus 8 (instantnoodle), Lenovo Legion / Samsung SM8250 Windows ports exist beyond the
  Silicium entries: found nothing public (only Silicium's OnePlus 8T and Galaxy S20 rows, both GPU ❌).

## 3. How do SM8150/SC8180X Windows devices configure the display, and can it be adapted to apollo's J3S panel?

### Takeaway
Qualcomm's Windows display driver reads the panel from an XML blob returned by `\_SB.GPU0._ROM` (plus backlight/colour
methods PBRT/BLCP/PIGC/PPCC). The best template is Silicium's vayu (Poco X3 Pro, SM8150, Windows GPU ✅): a Novatek
NT36672C video-mode LCD, 1080x2400, DSC 1.1 with 540x20 slices, WLED backlight — nearly the same class of panel as
apollo's J3S (Novatek TDDI, video mode, DSC 1.1, 540x20 slices, WLED 12-bit). All required J3S data (porches, DSC,
on/off sequences, reset timing, physical size, dfps list) is in Xiaomi's `dsi-panel-j3s-37-02-0a-dsc-video.dtsi`.
Writing the XML is easy; it only helps once the driver stack (Q1/Q2) runs.

### Cited Findings
- [Verified] Panel XML format (alioth `_ROM`, 5449 bytes): `<PanelName>`, `<PanelDescription>`, groups `EDID
  Configuration`, `Detailed Timing`, `Active Timing` (HorizontalActive/FrontPorch/BackPorch/SyncPulse, Vertical...),
  `Display Interface` (InterfaceType 9, InterfaceColorFormat 3), `DSI Interface` (DSIBitClockFrequency, DSIChannelId,
  DSIVirtualId, DSIColorFormat 36, DSITrafficMode, DSILanes 4, DSIRefreshRate 0x780000 = 120.0 in 16.16, DSITE*,
  DSIDSCEnable/Major/Minor/Scr/ProfileID/SliceWidth/SliceHeight/SlicePerPacket, DSIControllerMapping, DSILP11AtInit),
  `<DSIInitSequence>` / `<DSITermSequence>` as hex lines "datatype payload..." (e.g. `05 11 00` then `FF 0A`),
  `Backlight Configuration` (BacklightType, BacklightSteps, Brightness*Luminance, AdaptiveBrightnessFeature).
  `_ROM(offset, length)` returns at most 0x1000 bytes per call. `BLCP` builds a DCS 0x51 brightness packet. —
  [alioth DSDT.dsl, lines ~15030-15810](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl)
- [Verified] vayu panel XML (6566 bytes): `<PanelName>TM_NT36672C</PanelName>`, "J20S NT36672C Tianma Single DSI Vid
  Mode LCD Panel with DSC (1080x2400 24bpp)"; HFP 67 / HBP 40 / HSW 12, VFP 33 / VBP 30 / VSW 2; DSIBitClockFrequency
  850129200; DSILanes 4; DSITrafficMode 2; DSIRefreshRate 0x780000; DSC 1.1, ProfileID 4, slice 540x20; init sequence
  starts `15 FF 24 / 15 FB 01 / 15 4D 02`; backlight: `BacklightType 1`, `BacklightPmicControlType 2`,
  `BacklightPMICBankSelect 7`, `BacklightPMICPWMFrequency 800000`, `BacklightBitWidth 12`, `PMIPowerPmicModel 0x1E`,
  `CABLEnable True`, `DisplayResetInfo 0 5000 10000 10000 0`. — [vayu DSDT.dsl, `PCFG` at line ~77753](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/vayu/Decompiled/DSDT.dsl)
- [Verified] apollo J3S panel (Xiaomi apollo-q-oss): node `qcom,mdss_dsi_j3s_37_02_0a_dsc_video`, name "xiaomi 37 02 0a
  video mode dsc dsi panel", `dsi_video_mode`, `burst_mode`, 24 bpp, 4 lanes, `lp11-init`, `rgb_swap_rgb`, DSI ctrl/phy
  0, `brightness-max-level 4095`, reset sequence `<1 2>, <0 10>, <1 5>, <0 1>, <1 10>`, physical 69.5 x 154.4 mm
  (695/1544 in 0.1 mm), dfps `144 120 90 60 50 48 30` via `dfps_immediate_porch_mode_vfp`; one timing: 144 Hz,
  1080x2400, HSW 12 / HBP 43 / HFP 40, VSW 2 / VBP 26 / VFP 28; DSC `0x11`, scr 0, slice 540x20, 2 slices/pkt,
  8 bpc, 8 bpp, block prediction; on-command (~95 lines of `15 ...` DCS short writes, page selects `FF E0/F0/D0/C0/23/10`),
  displayon `29`, displayoff `28`; `mi,is-tddi-flag`. No PHY timings/clock rate in the dtsi (computed by the driver). —
  [dsi-panel-j3s-37-02-0a-dsc-video.dtsi](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/apollo-q-oss/arch/arm64/boot/dts/venor/qcom/dsi-panel-j3s-37-02-0a-dsc-video.dtsi)
- [Verified] The Kona pack INF itself only writes `PanelCfg1BrightnessMin/MaxLuminance` (MTP section); the driver
  also has `Miniport\PanelOverride` and `edidoverride.xml` strings (a registry/file override path besides ACPI). —
  [qcdx8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Graphics/DXKM/qcdx8250.inf)
- [Verified] apollo's ios-apollo display work never re-initialised DSI: it scans out from the UEFI/ABL-configured DPU,
  finding "the source pipe whose SRC0_ADDR is the boot framebuffer" and flipping between three frames (xnu 0156,
  `sweet_display.c`), with GPU blits into the flip frames. — [ios-apollo NOTES.md (0156 entry, ~line 617)](https://github.com/aTechGeekDev/ios-apollo/blob/1f75f381e31da96de537653fbac2252d2eb80418/NOTES.md)

### Inferences
- Converting J3S to the Qualcomm XML: 1080x2400, HFP 40/HBP 43/HSW 12, VFP 28/VBP 26/VSW 2, DSIRefreshRate for 144 Hz
  = 0x900000 (or 60 Hz 0x3C0000 with matching VFP; dfps by VFP like Xiaomi), DSITrafficMode = burst (vayu's 2 is
  probably burst; check against the driver's enum), DSC 1.1 profile/slices as vayu, init sequence = Xiaomi's on-command
  with the 6-byte dtsi header reduced to "datatype + payload" (e.g. `15 01 00 00 00 00 02 FF E0` → `15 FF E0`), then
  `05 11` + delay + `05 29`. Bit clock must be computed: for DSC 3:1 (24→8 bpp) at 144 Hz, pixel clock ≈ (1080+95) x
  (2400+56) x 144 ≈ 415 MHz, compressed 8 bpp over 4 lanes ≈ 830 Mbps per lane before overhead — same order as vayu's
  850 MHz at 120 Hz. Verify against the live register values (DSI PLL) left by ABL.
- Backlight: apollo is LCD + WLED (4095 max = 12-bit, ios-apollo 0151/0154 drives WLED); vayu's PMIC WLED keys are the
  model, but the PMIC model/bank (PM8150L on SM8250 vs. vayu's PMI) must be adapted, and the qcpmic/WLED path again
  goes through Qualcomm's PMIC drivers (SPMI) that need their own bring-up.
- Because the panel is already lit by ABL/UEFI, a driver that keeps the existing DSI/DPU state ("seamless" takeover,
  as ios-apollo does) avoids most panel risk; Qualcomm's driver will instead power-cycle and re-init the panel
  through PEP and the XML, so every value must be right.

### Gaps
- PHY timings and DSI PLL settings for J3S are not in the dtsi; would need reading from registers or computing per
  Linux `dsi_phy_7nm` / downstream `dsi_pll_7nm`.
- The exact enum meaning of `DSITrafficMode`, `DSIColorFormat`, `InterfaceType` values was inferred from examples, not
  documented.

## 4. Alternatives: render-only GPU, display-only driver, Mesa/Dozen/Freedreno, WARP baseline

### Takeaway
No open-source Adreno driver exists for Windows; Mesa's Windows drivers (Dozen = Vulkan on D3D12, GLon12, CLon12) sit
on top of an existing D3D12 driver (or WARP) and add no hardware acceleration by themselves. The realistic
non-Qualcomm options are (a) a WDDM **display-only** driver (KMDOD) for the UEFI framebuffer/DPU — gives proper
WDDM modes, brightness, maybe page flipping, but no 3D; (b) a custom **render-only** WDDM driver for Adreno — the
hardware bring-up is proven by ios-apollo, but a WDDM KMD + D3D UMD (or Mesa Turnip/Freedreno port + D3D mapping) is a
very large project. WARP (Microsoft Basic Render Driver) is what apollo uses now.

### Cited Findings
- [Verified] Microsoft KMDOD sample: "implements most of the device driver interfaces (DDIs) that a display-only miniport
  driver should provide to the Windows Display Driver Model (WDDM)"; "can be installed ... on top of a graphics device
  that supports access to frame buffer memory through the Unified Extensible Firmware Interface (UEFI)"; "does not
  support the *sleep* power state. If it is placed in the sleep state, the driver will cause a system bugcheck". —
  [Windows-driver-samples video/KMDOD/README.md](https://github.com/microsoft/Windows-driver-samples/blob/main/video/KMDOD/README.md)
- [Claim] Rob Clark (freedreno) on mesa-dev, April 2021: Mesa has a "kernel driver" abstraction in `src/freedreno/drm`,
  so a Windows port might be possible for someone who understands the Windows kernel-mode driver interface, which "is
  not publicly documented anywhere (or at least the adreno specifics are not)" (via search snippet; page itself blocked
  here). — [mesa-dev 2021-04](https://lists.freedesktop.org/archives/mesa-dev/2021-April/225116.html)
- [Claim] Dozen (Vulkan over Direct3D 12) was merged into Mesa 22.1; Microsoft's GLon12 reached OpenGL 4.3; Microsoft's
  Mesa D3D12 work targets running GL/CL "off D3D12 drivers ... for Windows on Arm where GL/CL drivers may not be
  available". — [Phoronix: Dozen merged](https://www.phoronix.com/news/Vulkan-On-Direct3D-12-Dzn-Merge); [Phoronix: GL 4.3 on D3D12](https://www.phoronix.com/news/OpenGL-4.3-D3D12-Mesa); [Microsoft DirectX blog 2020](https://devblogs.microsoft.com/directx/in-the-works-opencl-and-opengl-mapping-layers-to-directx/)
- [Claim] WARP is a "high speed, fully conformant software rasterizer", used when no hardware driver is available
  (search snippet of Microsoft docs; docs host blocked here); no ARM64-specific benchmark found. — [DirectX WARP (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp)
- [Verified] ios-apollo ran Mesa Turnip (Vulkan, "device Turnip Adreno (TM) 650") on apollo's A650 under XNU through
  its own kernel interface (`SweetGPU` user client; BoAlloc/BoMap/Submit/WaitFence), with CoreAnimation on Metal-over-
  Turnip ("the acceleration works"); it needed ~90 kernel patches (0096-0185) for GPU + display. — [ios-apollo NOTES.md](https://github.com/aTechGeekDev/ios-apollo/blob/1f75f381e31da96de537653fbac2252d2eb80418/NOTES.md)

### Inferences
- A render-only WDDM driver (Windows allows render-only adapters alongside a display-only one) could reuse ios-apollo's
  hardware sequence (GDSC → GMU/HFI → GX → SMMU → SQE → zap via TZ SCM). But WDDM requires the KMD to implement
  memory management (GPU VA, residency, paging via DxgKrnl), scheduling/preemption/TDR, and a D3D11/D3D12 UMD with a
  DXBC/DXIL shader compiler for Adreno. Qualcomm's UMDs are tied to their KMD's private interface, so they cannot be
  reused. A Mesa port (Turnip + Dozen-inverse, i.e. a D3D12-on-Vulkan UMD such as vkd3d-proton on Windows) does not
  exist for WDDM. Effort: person-years. Not a daily-driver path.
- A display-only KMDOD is the most achievable "real display driver": it can be built in the existing `windrivers/`
  CI (WDK), would replace the Basic Display Adapter, and could add brightness (WLED, already prototyped in ios-apollo)
  and maybe vsync-locked flips using the DPU as ios-apollo does. It does not accelerate DWM (DWM would still render
  with WARP), so UI smoothness gains would be small; benefits are mostly correct brightness control, rotation, maybe
  lower CPU from tear-free flips. KMDOD must not be put to sleep (bugcheck) — matters for Modern Standby.
- For apps, Microsoft's OpenCL/OpenGL/Vulkan compatibility layers run on WARP in software; they offer compatibility,
  not speed.

### Gaps
- No measured WARP numbers on SM8250 (Kryo 585) under Windows; the user could measure (e.g. dxdiag/3DMark Night Raid,
  or a WebGL test in Edge) as the baseline.
- No public WDDM render-only open-source Adreno effort found as of Nov 2026.

## 5. HDMI / DisplayPort output

### Takeaway
On apollo it does not matter: Xiaomi wires the USB-C port as USB 2.0 only and disables the QMP USB3/DP PHY and the
DP controller, so no DP-alt-mode output exists in hardware configuration (covered in detail in the sibling
`hdmi.md`). On other SM8250 devices, DP would come from the same qcdx driver (GPU0 `_CRS` includes `DP_PHY_REGS`), so it
shares all blockers above.

### Cited Findings
- [Verified, by sibling research] apollo-q-oss `xiaomi-sm8250-common.dtsi`: `usb-phy = <&usb2_phy0>, <&usb_nop_phy>;
  maximum-speed = "high-speed"; &usb_qmp_dp_phy { status = "disabled"; }`. — research_notes/Apollo Windows daily driver
  path/hdmi.md; [Xiaomi_Kernel_OpenSource apollo-q-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/apollo-q-oss)
- [Verified] Kona GPU0 resources include `DP_PHY_REGS` (alioth `_CRS` second region `0x088E0000`, RESI entry
  "DP_PHY_REGS", "DISPLAY"), i.e. DP is part of the qcdx display device. — [alioth DSDT.dsl](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl)
- [Verified] The qcdx driver also contains HDCP UMD (`qchdcpumd8250.dll`, `ContentProtectionDriverName`) and EDID
  handling (`EDPEDIDRead`, `DynamicEDIDEnabled`). — [qcdx8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Graphics/DXKM/qcdx8250.inf)

### Inferences
- External display on apollo is only possible via USB display adapters (DisplayLink-class, CPU-rendered) — see hdmi.md.

### Gaps
- None beyond hdmi.md.

## 6. Effort and risk per route (with evidence)

### Takeaway
Ranked by realism for a daily driver: (A) stay on the UEFI framebuffer + WARP (works now); (B) own display-only
KMDOD on the existing framebuffer/DPU (weeks, low-moderate risk, no 3D); (C) Qualcomm qcdx8250 via a working PEP
(months, high risk, unproven on any SM8250 device, many dependent drivers); (D) own render-only Adreno WDDM driver
(person-years). Stub PEP (qcfakepep) alone is not a GPU route.

### Cited Findings
- [Verified] Route A baseline: Windows boots to the desktop with only the UEFI framebuffer (Microsoft Basic Display
  Adapter), no PEP. — renegade-apollo docs/handoff.md "Current state"; uefi/apollo/README.md "Decision: run without
  Qualcomm's PEP"
- [Verified] Route C dependencies and blockers: PEP (qcpep8250 LMh hang, qcpep8280 TSENS hang), SMMU (qcsmmu needs PEP
  interface; qcsmmu8250 hang at logo on alioth with test signing and no PEP), Andromeda signature (test signing
  required — apollo already has it on), HID remap (QCOM05xx→25xx done by make-apollo-dsdt.py), GPU0 + MMU/IMM/TREE/SCM0/PILC/RPEN
  devices currently dropped from apollo's DSDT. — [K40 dead-ends](https://github.com/zhishixuebao-0791/K40_Win11/blob/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md); renegade-apollo `uefi/apollo/make-apollo-dsdt.py`
- [Verified] Route D hardware feasibility: ios-apollo brought up GMU, HFI, GX, SMMU, SQE, zap/TZ (`smc 0x42000201/202/205`)
  and Turnip on apollo's A650 between 2026-10-24 and 2026-11-05. — [ios-apollo NOTES.md](https://github.com/aTechGeekDev/ios-apollo/blob/1f75f381e31da96de537653fbac2252d2eb80418/NOTES.md)
- [Verified] Route B building block: Microsoft's KMDOD sample targets exactly a UEFI-framebuffer device. —
  [KMDOD README](https://github.com/microsoft/Windows-driver-samples/blob/main/video/KMDOD/README.md)

### Inferences
- **A. Framebuffer + WARP (status quo).** Effort 0. Risk 0. Cost: all rendering on CPU (heat/battery; the handoff
  already caps CPU at 1800 MHz for lack of thermal zones, which also caps WARP), fixed 1080x2400 portrait, no
  brightness control via Windows, no vsync. Small wins: set DWM-friendly settings (animations/transparency off).
- **B. Own display-only driver (KMDOD) + WLED brightness.** Effort: ~2-6 weeks for someone already building KMDF drivers
  in `windrivers/` (sample code exists; scanout address = UEFI framebuffer; optional DPU page-flip registers from
  ios-apollo 0156). Risk: low-moderate (a display driver bug = black screen/bugcheck at boot; recoverable by booting
  without it/safe mode); must avoid sleep. Gain: WDDM display device, brightness (WLED via SPMI/PMIC, ported from
  ios-apollo 0151/0154), possibly rotation and tear-free flips. No GPU acceleration.
- **C. Qualcomm qcdx8250.** Prerequisites in order: (1) a PEP that does real power work for GPU0/MDSS (patched
  qcpep8250 with all 8180-specific MMIO removed, or a custom PEP implementing PEP0's G0MD/GPU components — the
  latter is itself a large project: PoFx PEP platform extension, clock/GDSC/RPMh/ICB programming for display and GPU);
  (2) MMU0/MMU1 back with qcsmmu (8250 or 8280 binary), IMM0/1 qciommu, SCM0/TREE/PILC/RPEN; (3) GPU0 back as
  `QCOM253A` with a J3S panel XML (template: vayu) and apollo's GPIO/regulator data; (4) hope the 8180-branch
  qcdxkm handles A650/MDSS 6.0 and that TZ accepts its secure microcode path on a production Xiaomi device. Each
  step risks boot hangs (as seen with qcpep and qcsmmu). Nobody has published a working result on any SM8250 phone
  after ~2.5 years of Silicium/WOA effort. Effort: months; probability of full success: low.
- **D. Own render-only WDDM Adreno driver.** Hardware part is proven (ios-apollo), but WDDM KMD + D3D UMD/compiler is
  orders of magnitude more work than anything in this project; not recommended.
- A pragmatic stance for "daily driver": accept WARP, do B if brightness/WDDM features matter, and keep C as a
  research track that only becomes interesting if someone (Silicium/K40/WOA) publishes a working SM8250 PEP.

### Gaps
- No quantitative WARP vs. hardware comparison on this SoC; no public qcdx8250 install attempt on SM8250 to calibrate
  route C's first failure point.
