# Qualcomm PEP (qcpep) on SM8250 / apollo: routes to a working PEP and what each unlocks

Research date: 2026-11-07. Scope: Windows 11 ARM64 on Xiaomi Mi 10T (apollo, SM8250 "Kona") booted from Mu-Silicium.
Ground truth for what apollo has already tried: `docs/handoff.md` ("Current state"), `uefi/apollo/README.md`
("Root cause of INACCESSIBLE_BOOT_DEVICE: PEP0.PEPH" through "Decision: run without Qualcomm's PEP"),
`uefi/apollo/patch-qcpep-lmh.py`. Those local files are cited by path, without links.

Evidence labels used below: **[verified]** = I read it in repo code, binaries, logs or docs during this session;
**[reported]** = another project's written claim, kd log or note that I read but could not reproduce;
**[inference]** = my own conclusion.

Clones and scripts from this session (static analysis only; nothing was executed):
`/tmp/claude-0/-home-user-renegade-apollo/1af7fa84-c9a2-52c9-827f-f37178cec63b/scratchpad/pep/` and
`/tmp/claude-0/-home-user-renegade-apollo/1af7fa84-c9a2-52c9-827f-f37178cec63b/scratchpad/pepscripts/`
(`scan.py` looks for hard-coded SoC addresses, `guids.py` and `deps.py` look for PEP interface GUIDs and PoFx imports).

## 1. Has anyone got a real Qualcomm PEP (any qcpep build) working on SM8250 under Windows?

### Takeaway
No. I found no project with any Qualcomm qcpep running on SM8250, SM8250-AB or SM8250-AC under Windows (as of 2026-11-07).
Both public builds tested on SM8250 die on hard-coded hardware from other SoCs:
- "qcpep8250" (really the SM8150/SC8180X build) on apollo and on alioth.
- qcpep8280 on alioth.

Every SM8250 Windows port that reaches the desktop does it with no PEP bound: pipa, alioth (Silicium), K40_Win11, and apollo.
The newest attempt is a stub PEP, alioth's `qcfakepep` (2026-10-28). It is unverified: there are no published results.

### Cited Findings
- **[verified] The WOA "qcpep8250.sys" is a renamed SM8150 build.** WOA-Project's Kona pack `qcpep8250.sys` and the Hana (SM8150) pack `qcpep8150.sys` are byte-identical: same git blob `63b9c3b80f941138f695f0f27e21a52676e3362e`. Both INFs say DriverVer `10/30/2024, 1.0.2140.0000`.
  - Sources: [kona qcpep8250.sys](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/main/Drivers/SOC/PowerManagement); [hana qcpep8150.sys](https://github.com/WOA-Project/windows_silicon_qcom_hana/tree/main/Drivers/SOC/PowerManagement).
  - The SHA-256 of the kona file (`39fafcd6…`) matches `ORIGINAL_SHA256` in apollo's `uefi/apollo/patch-qcpep-lmh.py`.
- **[reported] K40_Win11 (alioth) independently reached the same conclusion:** "qcpep8250.sys (internal name qcpep8180.sys) … is hard-coded for 8150/8180 hardware". Its kd log shows AddDevice writing to `0x18350800/0x18358800`, the LMh blocks of SM8150/SC8180X, which do not exist on SM8250 (LMh there is part of EPSS `0x18590000`). — [K40 dead-ends-and-breakthroughs.md, breakthrough 11](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)
- **[reported] K40 found more hard-coded SC8180X accesses in qcpep8250:**

  | Function | What it touches |
  | --- | --- |
  | `+0x5520`, `+0x5b20`, `+0x5b88` | LMh |
  | `+0x6288` | LMh `0x18350bc0` bit 0 |
  | `+0x6388` | reads the OSM L-values at `0x18323704`/`0x18325f04` |
  | `+0x1dd730` | maps GIC `0x17a00108/0x17a00188` (these exist on SM8250) |
  | `+0x137978` | references the OSM table at `+0x177f90`; whether it accesses it was "not yet checked" |

  These are the same two extra addresses that apollo's static audit found. — [same file](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)
- **[verified, apollo] Where qcpep8250 1.0.2140.0 fails on apollo, in order:**
  1. The PEPH string mismatch (fixed in the generator).
  2. A hang on the LMh write to `0x18350800`. Patched to `ret` at `+0x5520`/`+0x5b20`/`+0x5b88`.
  3. `0x14B` because the display RSC `0x0AF20000` reports 2 TCS where the table minimum is 4. Minimum patched to 2.
  4. `SOC_CRITICAL_DEVICE_REMOVED` (tag `qpe0`).

  Source: `uefi/apollo/README.md`.
- **[reported] qcpep8280 on alioth: 0x101 CLOCK_WATCHDOG_TIMEOUT during AddDevice.**
  - The TSENS init (`0x2b8bc0` → `0x140b18`) loops over 4 controllers hard-coded at `.rdata 0x1a6a70`: `0x0c222000/0x0c263000`, `0x0c223000/0x0c265000` (exist on SM8250), and `0x0c224000/0x0c251000`, `0x0c225000/0x0c252000` (SC8280XP only). The third controller hangs.
  - There is no registry or ACPI switch.
  - Source: [K40 phase17h2-qcpep8280-kd-result-20261022.md](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/phase17h2-qcpep8280-kd-result-20261022.md)
- **[reported] qcpep8280 "stub" mode got further, then crashed.**
  - qcpep8280 has a registry flag `Services\qcpep\Parameters\EnableStubbedPepForPresilicon`. It is read in AddDevice and short-circuits about 15 functions, including the TSENS init and the `{a0e1dd36}` interface function, which then always returns success.
  - The INF's AddReg resets the flag to 0 on every (re)install, so K40 forced it to 1 with kd for one boot.
  - Result: PEP0 passed AddDevice, PrepareHardware and D0Entry, then hit **BugCheck 0x10D WDF_VIOLATION in SelfManagedIoInit** (`qcpep8280+0x2ab624`; a global spinlock handle at `0x2243a0` is NULL).
  - Further analysis was not continued.
  - Source: [same file](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/phase17h2-qcpep8280-kd-result-20261022.md)
- **[verified] K40_Win11's last commit** is 2026-10-23: "Phase18a test: IORT loaded but kernel IOMMU needs Hyper-V (dead end)". There is no later PEP result. — [K40_Win11 repo](https://github.com/zhishixuebao-0791/K40_Win11)
- **[verified] alioth-woa-drivers (`qcfakepep` + `aliothcpufreq`)** consists only of commits dated 2026-10-28. Its README says "Status: experimental" and claims it "Unlocks the whole audio chain (SMMU → qcsubsys → ADSP)". No logs or results are published. Its DEPLOY.md is a test plan with expected outcomes, not results. — [alioth-woa-drivers](https://github.com/18934020489Xx/alioth-woa-drivers)
- **[verified] Silicium's alioth ACPI deliberately avoids any qcpep binding.** It was rewritten 2026-08-06 ("Alioth: Update ACPI", N1kroks; DSDT shrank from 436 KB to 90 KB) with QCOM25xx HIDs. PEP0 is `_HID "QCOM2517"` with PEPH `"ACPI\\VEN_QCOM&DEV_2517"`, which no WOA qcpep INF matches (Kona INF: `ACPI\QCOM2519`). UFS0 has no `_DEP` on PEP0.
  - Sources: [Silicium-ACPI alioth DSDT](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl); [kona qcpep.wd8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/main/Drivers/SOC/PowerManagement/qcpep.wd8250.inf).
  - `qcfakepep.inf` matches exactly `ACPI\VEN_QCOM&DEV_2517` and `DEV_0519`.
- **[verified] Mu-Silicium Status.md (repo HEAD 2026-11-05):**
  - **alioth (Active), Windows:** storage, buttons, accelerometer, compass, gyroscope, battery, USB host, device and PD, slow charging, CPU, touch, Bluetooth, speakers and display are ✅. WLAN, GPU, microphone, camera, mobile data and vibration are ❌.
  - **pipa (Inactive), Windows:** only storage, buttons, battery, USB device, CPU and display are ✅.
  - Source: [Mu-Silicium Status.md](https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md)
- **[verified] qcpep8250's INF writes AeoB-format resource blobs** into the device hardware key, for example `SystemLibResources\LVDD` naming RPMh ARC resources such as `/arc/client/rail_cx`, `/arc/client/rail_mx`, `/arc/client/display/rail_cx` and `/arc/client/rail_xo`. — [qcpep.wd8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/main/Drivers/SOC/PowerManagement/qcpep.wd8250.inf)

### Inferences
- Silicium's alioth Windows list (BT, speakers, sensors ✅ with no qcpep binding) suggests those features come from non-PEP paths: Silicium/own drivers, or a stub. I could not find which drivers Silicium uses for alioth speakers or sensors. See Gaps.
- No project has reached a PEP-dependent Qualcomm device (SMMU, ADSP, GPU) on SM8250 Windows with any PEP, real or stub, as far as public evidence goes.

### Gaps
- Renegade Project wiki and forum, edk2-porting/edk2-msm and WOA-Project Telegram: nothing found showing qcpep on SM8250. Web search returned nothing relevant. The Renegade forum is not indexed. Treat "nobody did it" as "no public evidence", not proof.
- I could not reach learn.microsoft.com from this container (not in the allowed domains); Microsoft docs were read from the MicrosoftDocs GitHub repo instead.
- Which driver package Silicium/N1kroks uses for alioth Windows speakers, BT and sensors (QCOM25xx HIDs) is undocumented in Silicium-ACPI, Mu-Silicium or the Guides repo.

## 2. What PEP provides to downstream Qualcomm drivers, and which parts need real power management

### Takeaway
Qualcomm's PEP has two separate jobs:
1. **A private "SoC interface" layer.** It publishes device interface `{55beca03-ba2c-4fbc-9efc-ee8eca56f0c7}` and answers QueryInterface for `{a0e1dd36-…}` (0x28 bytes, one function) and `{99f8bbdb-…}` (0x38 bytes, three functions). This is what makes SMMU, ADSP and GPU drivers fail AddDevice or start when PEP is absent. A stub can fake it.
2. **A real PoFx PEP plug-in (DPM, PPM, ACPI).** It accepts devices and, on F-state and D-state transitions and `PoFxPowerControl` requests, turns on the clocks, GDSCs, RPMh rails and interconnect (ICB) bandwidth listed in PEP0's ACPI config packages. A stub cannot fake this. Devices then only work if firmware left their clocks and rails on, or if another driver powers them.

### Cited Findings
- **[reported] How qcsmmu8280 uses the PEP interfaces:**
  - `{a0e1dd36}` (0x28 bytes, version 1) is queried synchronously in AddDevice; failure makes AddDevice return 0xC0000001 (Code 31).
  - `{99f8bbdb}` (0x38 bytes, 3 functions) is queried asynchronously after a `{55beca03}` arrival notification.
  - Both interfaces are only provided by PEP (qcpep8280 `0x46c68`, `0x4682c`/`0x469f4`).
  - Source: [K40 phase17h2 note, "方向1 结论"](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/phase17h2-qcpep8280-kd-result-20261022.md)
- **[reported] How qcpep8280 implements `{a0e1dd36}`:** `WdfDeviceAddQueryInterface` with {Size 0x28, Ver 1, Context NULL, no-op Ref/Deref, function `0x20230`}. `0x20230` returns 0xC0000022 at IRQL>0, returns 0 in stub mode, and otherwise issues a named power request to a device node (`0x2478d0(arg, &0x223ba8, 1,1,0,0)`, containing the string "DEVICE"). — [same](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/phase17h2-qcpep8280-kd-result-20261022.md)
- **[reported] The kernel IOMMU path does not exist on these phones.** qcsmmu8280 skips the PEP query only when `IoGetIommuInterface` works. On ARM64 Windows that needs Hyper-V owning the SMMU (`nt!HalpHvIommu`). Adding a Kona IORT did not help (`IoGetIommuInterface` → 0xC00000BB). Hyper-V/EL2 is not available on Android phones; slbounce's README says it cannot be used on Android devices. So the PEP interface is mandatory for qcsmmu.
  - Sources: [K40 dead-ends 13/14](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md); [slbounce](https://github.com/TravMurav/slbounce)
- **[reported] qcsubsys needs `{55beca03}`.** After finding the ADSP firmware, qcsubsys queries device interface class `{55beca03}` (Procmon boot log) and fails if it is absent (ADSP 0xC000003B). The dependency chain is: no PEP0 driver → no `{55beca03}` → SMMU Code 31 → ADSP fails. — [K40 breakthroughs 6 and 8](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)
- **[verified, my static scan of 54 WOA Kona `.sys` files]** Columns: which PEP interface GUIDs are embedded, and which PoFx imports the driver uses by name. WDF drivers can also use PoFx through the WDF function table, which this scan cannot see.

  | Driver | PEP GUIDs embedded | PoFx imports by name |
  | --- | --- | --- |
  | qcsmmu8250 | 55be, a0e1, 99f8 | PoFxRegisterDevice |
  | qcsubsys8250 | 55be, a0e1 | PoFxPowerControl |
  | qcqdss8250 | 55be, a0e1 | PoFxRegisterDevice, PoFxPowerControl |
  | qcdxkm8250 (GPU) | 55be, 99f8 | none |
  | qcuart8250 | none | PoFxRegisterDevice, PoFxActivateComponent, PoFxPowerControl |
  | qcauddev8250 | none | PoFxRegisterDevice, PoFxActivateComponent |
  | qcpmictcc8250 | none | PoFxRegisterDevice, PoFxActivateComponent |
  | qcppx8250 (PCIe) | none | PoFxRegisterPlugin: it is itself a PoFx plug-in |
  | qcwlan8250, qcpil8250, qcglink8250, qcspi8250, qci2c8250 | none | none of these |

  Source: [WOA Kona drivers](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/main/Drivers)
- **[verified] What PoFx sends a PEP (DPM notifications).**
  - `PEP_DPM_PREPARE_DEVICE`: "If the PEP claims ownership of the device, the device and all its components must be turned on, and clocks to the device must ungated."
  - `PEP_DPM_REGISTER_DEVICE` and `PEP_DPM_NOTIFY_COMPONENT_IDLE_STATE` (0x13) are the other key notifications.
  - `PEP_DPM_POWER_CONTROL_REQUEST` (0x0E) "Informs the PEP that a driver has called the PoFxPowerControl API to send a control code directly to the PEP".
  - Source: [MicrosoftDocs dpm-notifications.md](https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/kernel/dpm-notifications.md)
- **[verified] PEPs have three interfaces:** DPM, PPM (processor idle/perf) and, since Windows 10, ACPI runtime methods ("Platform Extension"). — [Windows-driver-samples pofx/PEP README](https://github.com/microsoft/Windows-driver-samples/tree/main/pofx/PEP)
- **[verified] Example SM8250 PEP0 config** (Silicium alioth DSDT `G4MD`, GPU0 component 4, F0 EXIT): `"BUSARB" {3, "ICBID_MASTER_APPSS_PROC", "ICBID_SLAVE_DISPLAY_CFG", 0x047868C0, 0}`, then `"CLOCK" {"gcc_disp_xo_clk", 1}`, and so on. This is per-device clock, ICB and rail sequencing that qcpep executes. — [Silicium-ACPI alioth DSDT](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl)
- **[reported] Which ACPI methods qcpep evaluates by IOCTL:** qcpep8250 evaluates only `PEPH`, `SOCI`, `THTZ` and `GEPT` that way. The xxMD config methods are read through the ACPI interface table, mechanism not fully confirmed. Emptying all 27 xxMD methods did not stop the hang, so the hang is in core init, not in device config. — [K40 phase17e note](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/acpi-hid-phase17e-build-result-20261021.md); [dead-end 10](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)
- **[verified, Linux as hardware reference] Power the DSPs need on SM8250:**
  - ADSP: proxy power domains `lcx` and `lmx`, AOSS QMP `load_state = "adsp"`.
  - CDSP: `cx`, `load_state = "cdsp"`.
  - Source: [Linux qcom_q6v5_pas.c](https://github.com/torvalds/linux/blob/master/drivers/remoteproc/qcom_q6v5_pas.c) (`sm8250_adsp_resource`, `sm8250_cdsp_resource`)

### Inferences
Needs per downstream driver:

| Driver or feature | Stub interface enough? | Real power management needed? |
| --- | --- | --- |
| qcsmmu | Yes for AddDevice (`{a0e1dd36}` returning success; `{99f8bbdb}` is optional/asynchronous). | Probably not if firmware leaves the SMMU on. MMU1 is the APPS SMMU at `0x15000000`; it is always on. |
| qcsubsys/qcpil (ADSP/CDSP/SLPI) | Gets the interface check past. | Booting the DSP needs LCX/LMX (ADSP) or CX (CDSP) votes plus AOSS QMP `load_state`, per Linux. If qcsubsys relies on PEP (via `{a0e1dd36}` "DEVICE" requests or PoFxPowerControl) to place those votes, a stub that returns success without voting may let the load start, but the DSP can then fail or crash. Untested. |
| GPU (qcdxkm) | No. | GX rail, GPU_CC/GMU clocks and bus votes come from PEP DPM per `G4MD`-style configs, and it also needs `{99f8bbdb}`, which qcfakepep does not implement. |
| UART (qcuart) | No. | Uses PoFxPowerControl; works only where firmware left the QUP clocks on. apollo already bypasses this with its own drivers. |
| PCIe (qcppx, PCI0/1/2 `_DEP` on PEP0) | No. | Likely needs PEP for GDSC, pipe clocks and refgen. apollo's planned route already does PCIe by hand. |
| Wi-Fi | — | qcwlan8250 has no PEP GUIDs itself, but its stack (QCMS/subsystem, or PCIe for QCA6390) is PEP-gated indirectly. |
| Display | — | qcdx needs a PEP. Today's GOP framebuffer works without one. |

### Gaps
- Exact semantics of the `{99f8bbdb}` 3-function interface and of the `{a0e1dd36}` function arguments. Only qcpep8280's implementation was partially reverse-engineered by K40.
- Whether qcsubsys8250/8280 sends PoFxPowerControl codes that must succeed for subsystem boot. No public RE.

## 3. How far a stub PEP (qcfakepep-style) gets other SM8250 ports

### Takeaway
On paper, a stub gets qcsmmu's AddDevice past its PEP query, and gives qcsubsys the `{55beca03}` interface it looks for. In practice nobody has published a test. qcfakepep as published has three problems:
- **A real bug:** its merged EPSS code maps physical `0x1_8591_0000` instead of `0x1859_1000`. On 6 to 12 GB phones that is DRAM, so it would read and write random RAM.
- **A missing interface:** `{99f8bbdb}`.
- **No PoFx plug-in registration:** no DPM, so it gives no power management at all.

### Cited Findings
- **[verified] What qcfakepep is and does:**
  - A pure WDM function driver for PEP0.
  - In AddDevice it calls `IoRegisterDeviceInterface(PDO, {55beca03})`.
  - On IRP_MN_QUERY_INTERFACE it serves only `{a0e1dd36}` (version 1, size ≥ 0x28), with a function `NTSTATUS PowerRequest(PVOID)` returning STATUS_SUCCESS.
  - Everything else is passed down. No PoFxRegisterPlugin.
  - Source: [qcfakepep.c](https://github.com/18934020489Xx/alioth-woa-drivers/blob/main/qcfakepep/qcfakepep.c)
- **[verified] The EPSS address bug:**
  - Both `qcfakepep.c` and `aliothcpufreq.c` `#define EPSS_D0_PHYS 0x0000000185910000ULL` and `EPSS_D1_PHYS 0x0000000185920000ULL`, while their own comments say `0x18591000`/`0x18592000`.
  - `qcfakepep.c` scans the "LUT" at +0x100 and writes +0x320 in `DriverEntry`, unconditionally.
  - SM8250's real cpufreq-hw is at `0x18591000` (also used by apollo's `windrivers/apollocpufreq/apollocpufreq.c`).
  - The comment "Domain 1 (CPU4-7)" is also wrong: SM8250 has 3 EPSS domains.
  - Sources: [qcfakepep.c](https://github.com/18934020489Xx/alioth-woa-drivers/blob/main/qcfakepep/qcfakepep.c); [aliothcpufreq.c](https://github.com/18934020489Xx/alioth-woa-drivers/blob/main/aliothcpufreq/aliothcpufreq.c); [Linux sm8250.dtsi `cpufreq@18591000`](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi)
- **[reported] What K40 had working before any PEP experiment:**
  - Hardware IDs adjusted (QCOM05xx → QCOM25xx/0Axx/06xx).
  - Surface WHQL 8280 binaries used with 8250 configs to get past Andromeda signing.
  - SSDD and PILC working.
  - qcsubsys ADSP moved from 0xC0000034 to 0xC000003B, blocked by the missing `{55beca03}`.
  - MMU1 stuck at Code 31 waiting on PEP.
  - Source: [K40 dead-ends-and-breakthroughs.md](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)
- **[verified] 32 devices in each DSDT `_DEP` on PEP0** (they will not start until PEP0 starts):
  - **pipa:** ABD ADSP AGR0 BTH0 CAMP CDSP GSI I2C5 I2C8 IC10 IPA LLC MMU1 MON0 PCI0 PCI1 PDSR QDSS QWLN QWPP SCSS SDC2 SPI4 SPSS UARD UCP0 UFS0 UR14 UR18 UR20 URS1 VFE0.
  - **alioth (Silicium, current):** ABD ADSP AGR0 BTH0 I2C2 I2C4 IC12 IC16 IC17 LLC MJCT MMU0 MMU1 MON0 NSP0 PCI0 PCI1 PCI2 PDSR QDSS SCM0 SCSS SDC2 SPI3 SPI5 SPSS TSC1 UAR7 UARD UCP0 URS0 URS1. UFS0 is not in the alioth list.
  - Source: [Silicium-ACPI pipa / alioth DSDT](https://github.com/Project-Silicium/Silicium-ACPI/tree/main/Platforms/Xiaomi)
- **[reported] K40's own verdict:** "only the PEP (or a substitute) can provide `{a0e1dd36}`" on K40. — [dead-end 14](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)

### Inferences
- **Starting PEP0 on apollo (any stub) releases every device with `_DEP` on PEP0 at once.** On apollo's DSDT that likely includes URS0/URS1 (USB), SDC2, the I2C/SPI/UART nodes and PCI0/1. Today `--no-pep` strips PEP0 from every `_DEP`, so those devices already enumerate with whatever drivers apollo uses.
  - Two consequences of a stub: Qualcomm function drivers for those nodes (qcspi, qci2c, qcuart) could now bind and contend with apollo's own SPI (touch) and UART (BT) drivers.
  - Qualcomm drivers that touch unclocked blocks can hang the CPU (a hang, not a fault, as with LMh).
  - Bring up a stub with Qualcomm bus drivers removed or disabled first.
- **The realistic best case for a stub on apollo:** MMU1 (qcsmmu) starts, then qcsubsys proceeds to ADSP loading. Audio then still needs: the ADSP actually booting (LCX/LMX votes, AOSS `load_state`), PIL/SCM/TZ authentication of Xiaomi-signed firmware, GLINK/IPC router, PDSR, the audio miniport and codec drivers, and Kona configuration for these.
  - K40 had parts of this chain working (PILC, SSDD), so it is plausible but multi-stage.
- **A safe stub for apollo should differ from qcfakepep:**
  - Drop the EPSS code (apollocpufreq already does frequency).
  - Implement `{99f8bbdb}` returning success.
  - Optionally register as a minimal PoFx PEP accepting no devices.
  - Log every QueryInterface and power request to the registry (apollo's pattern) to learn what downstream drivers ask for.

### Gaps
- No published boot result of qcfakepep (or any stub) on any SM8250 device: which devices then start and which still fail is unknown.
- Whether returning STATUS_SUCCESS from the `{a0e1dd36}` function without acting breaks qcsmmu later (for example, expecting a clock or bus vote before SMMU register access). Unknown.

## 4. PEP config contents, and whether a custom or open PEP could drive SM8250 power resources

### Takeaway
Qualcomm's PEP is a whole SoC power stack, not a thin driver:
- It contains its own clock driver, keyed by clock name.
- It contains ICB (interconnect) arbitration, RPMh/ARC rail clients, an AOP QMP channel, TSENS, LMh/limits and PPM.
- PEP0's ACPI packages only sequence named resources.

An own PEP that really drives SM8250 resources would need ports of Linux `gcc-sm8250`/`dispcc`/`gpucc`, `rpmh`/`cmd-db`, `rpmhpd`, the interconnect BCM votes and `qcom_aoss`, written into a PoFx DPM plug-in. Open-source bases exist (Microsoft's MIT PEP sample; i.MX6 PEP with full DPM/PPM/ACPI, about 6.5k lines). The SoC tables are the big job.

### Cited Findings
- **[verified, my scan] Size and content of pipa's DSDT PEP0 configuration:** 27 xxMD methods (APMD…SDMD, G0MD–G9MD, plus CTPM, INTR, PPPM, STND, PGCT, PMCF). They reference 150 distinct `gcc_/cam_cc_/disp_cc_/gpu_cc_/video_cc_…_clk` names, 8 GDSC/VDD footswitch names, 38 `ICBID_*` endpoints and rail paths.
  - Silicium's current alioth DSDT keeps 11 xxMD methods (APMD BPMD FPMD G0MD G4MD G5MD LPMD MPMD OPMD PEMD SDMD) plus INTR, PMCF, PPPM, STND.
  - Source: [Silicium-ACPI pipa](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl), [alioth](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl)
- **[verified, my scan] The clock names live inside the qcpep binaries:**

  | Build | Clock names (of pipa's 150) | GDSC names (of 8) | ICB IDs (of 38) |
  | --- | --- | --- | --- |
  | qcpep8250 (=8150/8180 build) | 150 | 8 | 38 |
  | qcpep7280 1.0.4056.4400 | 125 | 7 | 29 |
  | qcpep8280 1.0.4195.6500 | 144 | 6 | 38 |

  So qcpep resolves names to its own built-in, SoC-specific clock and ICB tables. — binaries from [WOA kona](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/main/Drivers/SOC/PowerManagement) and [Qualcomm-Reference-Drivers](https://github.com/WOA-Project/Qualcomm-Reference-Drivers)
- **[verified] The rail and RPMh side is name-based.** qcpep's INF resources name RPMh ARC clients by path (`/arc/client/rail_cx` …), which resolve via cmd-db at run time. apollo's own `apollobt` already places RPMh votes via cmd-db at `0x80860000`. — [qcpep.wd8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/main/Drivers/SOC/PowerManagement/qcpep.wd8250.inf); `docs/handoff.md`
- **[verified] Open PEP bases:**
  - [microsoft/Windows-driver-samples pofx/PEP](https://github.com/microsoft/Windows-driver-samples/tree/main/pofx/PEP): `PoFxRegisterPlugin` in `common/pep.c`, an ACPI-method PEP sample (repo licence MIT).
  - [ms-iot/imx-iotcore driver/power/imx6pep](https://github.com/ms-iot/imx-iotcore/tree/public_preview/driver/power/imx6pep) (MIT, last commit 2019-11-08): about 6,569 lines. It handles PEP_DPM_PREPARE/REGISTER_DEVICE, NOTIFY_COMPONENT_IDLE_STATE, POWER_CONTROL_REQUEST, component perf states, SoC subsystem accounting, the full PEP_NOTIFY_PPM_* idle/perf set and PEP_NOTIFY_ACPI_*. It drives the i.MX6 clock tree directly (`mx6clktreehelper`).
- **[reported] SM8250 ADSP power, as Linux does it:** LCX/LMX proxy votes and a QMP `load_state` message. — [Linux qcom_q6v5_pas.c](https://github.com/torvalds/linux/blob/master/drivers/remoteproc/qcom_q6v5_pas.c)

### Inferences
- **A "real" own PEP does not need to interpret Qualcomm's xxMD packages.** It can accept only selected devices and hard-code their power-up in C: the ADSP rails (LCX/LMX ARC votes plus AOSS QMP), GPU GX/CX, the QUP SE clocks for UART/SPI/I2C, and the PCIe GDSC and clocks. This mirrors what apollo already does per driver.
- **Effort scale.** A generic interpreter for the 150-clock config with GCC/dispcc/gpucc/camcc tables and ICB BCM votes means porting several thousand lines of Linux SM8250 clock and interconnect data. That is months of work. **[inference]**
- **Hidden risk in any non-native qcpep.** qcpep's internal GCC register tables for SC8180X (the "8250" build), SC7280 or SC8280XP will not match SM8250's GCC layout; Linux has separate `gcc-sc8180x.c`, `gcc-sc7280.c` and `gcc-sm8250.c` drivers. Even if AddDevice survives, the first DPM clock enable could write the wrong GCC registers.
  - **This is unverified.** Check it by watching qcpep writes to the GCC region (`0x00100000`) with kd, or by RE of qcpep's clock table.

### Gaps
- No public open-source Qualcomm Windows PEP, and no WOA-Project PEP source, was found.
- I did not check Raspberry Pi Windows (ms-iot rpi-iotcore) for a PEP.
- The interpretation format of the xxMD packages (version and fields) is undocumented.

## 5. Other qcpep binaries closer to SM8250, and their SoC hard-coding

### Takeaway
WOA-Project/Qualcomm-Reference-Drivers has qcpep for 7180, 7280, 8180, 8280, 8380 and 850/8998, but **no 8150 or 8250 build**. By static address scan, **qcpep7280 (SC7280/"Kodiak") is much the closest to SM8250**. It hard-codes:
- **the same** two TSENS controllers, EPSS domains (`0x18591000/2000/3000`), APPS RSC DRV2 and AOSS QMP as SM8250 (all confirmed identical in Linux `sm8250.dtsi` vs `kodiak.dtsi`);
- only one RSC (no display RSC, so no TCS check like the one apollo patched);
- no SM8150 LMh addresses;
- the same `EnableStubbedPepForPresilicon` switch as 8280;
- the same three PEP interface GUIDs.

Nobody has tried it on SM8250 (no public record).

### Cited Findings
- **[verified] Repository inventory** (repo HEAD 2026-03-21), counts of qcpep cab versions:

  | Folder | Builds |
  | --- | --- |
  | 8180_CLS (qcpep.wd8180) | 107 |
  | 7180_CLS | 29 |
  | 7280_CLS + 7280_WINDOWS_CLS | 4 + 14 |
  | 8280_QRD | 64 |
  | Surface 8280 | 21 / 21 / 10 |
  | 8380_CRD, 8380PA_CRD, 8380_RUN, Surface 8380 | several |
  | 1000_CLS (wd8180) | 1 |
  | 850/8998 OEM | several |

  No 8150, 8250, 8350 or 8450. — [Qualcomm-Reference-Drivers](https://github.com/WOA-Project/Qualcomm-Reference-Drivers)
- **[verified, my scan `scan.py`, data tables only]** Three binaries scanned: qcpep8250.sys 1.0.2140.0, qcpep8280.sys 1.0.4195.6500 (8280_QRD 200.0.68.0) and qcpep7280.sys 1.0.4056.4400 (7280_WINDOWS_CLS 200.0.15.0).

  | Hard-coded block | qcpep8250 | qcpep8280 | qcpep7280 |
  | --- | --- | --- | --- |
  | OSM table `0x18321000/0x18323000/0x18325800` | yes (`.rdata+0x177f90`) | yes (`+0x1ac810`) | yes (`+0x1af5f0`) |
  | TSENS0/1 `0x0c222000/0x0c263000`, `0x0c223000/0x0c265000` | yes | yes | yes |
  | TSENS2/3 (SC8280XP only) | no | yes | no |
  | APPS RSC DRV2 `0x18220000` | yes | yes | yes |
  | Display RSC `0x0af20000` | yes (second RSC entry, TCS minimum 4) | no | no |
  | AOSS QMP `0x0c300000` | yes | yes | yes |
  | EPSS `0x18591000/2000/3000` | no | no | yes (`.data+0x1bdc08..28`) |

  - The OSM table is shared by all three, so it is probably a generic table that is not always accessed. K40 also concluded the OSM table was "not the root cause", then later said this was premature.
  - qcpep7280 EPSS table: `{0x18591000, 0x1000, 4, 0x40000000}`, `{0x18592000, 0x1000, 4, 0x80000000}`, `{0x18593000, 0x1000, 4, 0x00080000}`, then `0x0c300000`.
  - qcpep7280 RSC table: count 1, APPS DRV2 only (TCS 8, NCPT 0x10). qcpep8250's table has a second entry with `0x0af20000` and minimum 4.
- **[verified, Linux reference] Address match between SM8250 and SC7280 (`kodiak.dtsi`):** both have `tsens0@c263000`, `tsens1@c265000`, `aoss_qmp@c300000`, `apps_rsc@18200000`, `cpufreq@18591000` and `pdc@b220000`. — [sm8250.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi), [kodiak.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/kodiak.dtsi)
- **[verified] Strings and GUIDs in all three binaries:** each contains `{55beca03}`, `{a0e1dd36}`, `{99f8bbdb}`, `PEPH`, `SOCI`, `THTZ` and `GEPT`. 7280 and 8280 contain `EnableStubbedPepForPresilicon`; 8250 does not (K40 also found it only in 8280).
- **[verified] INF and signature details:**
  - qcpep.wd7280.inf matches PEP `ACPI\QCOM0A17` (TSENS QCOM0A37…).
  - qcpep.wd8280.inf matches `ACPI\QCOM0617`.
  - The qcpep7280 catalog is Microsoft WHQL-signed (Microsoft Windows Third Party Component CA 2012).
  - The 8280_QRD catalog also carries "Windows On Andromeda KMCI Codesigning" strings.
  - Irrelevant on apollo, which runs with test-signing on.
- **[reported] Apollo's hard-coded-hardware story so far matches this table:** qcpep8250 hit SM8150 LMh (absent on SM8250) and the display-RSC TCS minimum; qcpep8280 hit TSENS2/3 (absent). — `uefi/apollo/README.md`; [K40 phase17h2](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/phase17h2-qcpep8280-kd-result-20261022.md)

### Inferences
- **Why qcpep7280 is the most promising unpatched Qualcomm PEP for SM8250's AddDevice and early init:** every SoC block it hard-codes that I could find exists at the same address on SM8250, and SC7280 also has the same 4+3+1 CPU/EPSS topology. Its known weak points:
  - SC7280-specific clock and ICB tables (125/150 clock names; camera IPE1/FD and others missing).
  - SC7280 AeoB INF resources (PMIC PM7325 rather than PM8150).
  - Possible chip-ID checks (`SOCI`).
  - Unknown SC7280-only blocks reached later.
- **My scanner misses some addresses.** It finds 32-bit literals in data plus MOVZ/MOVK pairs, and it did not find `0x18350800`, which kd proved qcpep8250 uses. So "no hit" is not proof of absence.
- **Practical test:** stub mode on a clean install (flag set in the INF copy before install, so AddReg writes 1), PEP0 `_HID QCOM0A17` with `PEPH "ACPI\\VEN_QCOM&DEV_0A17"`, kd attached. Check whether stub mode in the 7280 build has the same NULL-spinlock 0x10D bug that K40 hit in 8280.

### Gaps
- I did not disassemble qcpep7280's AddDevice, its use of `SOCI`, or its DPM clock path. Whether it checks the SoC ID (SMEM/`SOCI`) and refuses SM8250 is unknown.
- qcpep7180 and 8380 were not scanned. 7180 (SC7180) has OSM-style CPU frequency, unlike SM8250's EPSS, so it is expected to be farther away. **[inference]**
- I found no qcpep8350 or 8450 build anywhere.

## 6. Routes, what each unlocks, and effort/risk

### Takeaway
Routes ranked by expected value for apollo **[inference throughout, based on the findings above]**:
- **(B) Own minimal stub PEP:** small, safe, quick to learn from. Not qcfakepep as-is.
- **(C) qcpep7280 trial, ideally in stub mode first:** cheap one-boot experiment, best odds among Qualcomm binaries.
- **(D) qcpep8280 stub mode with a NULL-spinlock patch:** moderate.
- **(A) Continuing to patch qcpep8250 (SC8180X build):** open-ended, wrong GCC tables likely.
- **(E) Full own PEP:** a long-term project that only pays off for GPU, PCIe or audio via Qualcomm drivers. Per-device own drivers (apollo's current strategy) give more per hour.

### Cited Findings
- **[verified] Apollo's current baseline:** Windows 11 24H2 desktop with no PEP; own drivers for touch, cpufreq and BT; Wi-Fi planned via PCIe by hand. — `docs/handoff.md`
- **[verified] Next unresolved failure with qcpep8250 after two patches:** SOC_CRITICAL_DEVICE_REMOVED `qpe0`. — `uefi/apollo/README.md`
- **[reported] 8280 stub-mode results and blockers:** passes AddDevice/PrepareHardware/D0Entry, then 0x10D NULL spinlock in SelfManagedIoInit. The INF AddReg resets the flag. — [K40 phase17h2](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/phase17h2-qcpep8280-kd-result-20261022.md)
- **[verified] qcfakepep's implementation, missing interface and EPSS bug:** — [qcfakepep.c](https://github.com/18934020489Xx/alioth-woa-drivers/blob/main/qcfakepep/qcfakepep.c)

### Inferences
Route table (effort assumes this project's current tooling: KDNET works, test-signing, CI driver builds):

| Route | What it is | Effort | Risk | What it can unlock | Verdict |
| --- | --- | --- | --- | --- | --- |
| **A. Patch qcpep8250 1.0.2140.0 further** | Keep patching the SC8180X build: next `qpe0`, then OSM `0x18323704/0x18325f04` (`+0x6388`), LMh `0x18350bc0` (`+0x6288`), unknown others | Open-ended: each boot finds one more; days to weeks | High: SC8180X GCC/clock tables would make DPM write wrong registers; LMh/limits disabled | In theory everything qcpep does; in practice probably only the interface layer before DPM breaks things | Stop unless the others fail |
| **B. Own stub PEP (WDM/KMDF)** | Publish `{55beca03}`; answer `{a0e1dd36}` (0x28) and `{99f8bbdb}` (0x38) with success; no MMIO; log requests; optionally a PoFx plug-in that accepts nothing | 1–3 days to build (≈300–600 lines, template in qcfakepep minus EPSS) | Low for the stub itself. Medium for the side effects: PEP0 starting releases all `_DEP` children, so disable Qualcomm bus drivers that would fight apollo's own SPI/UART drivers | qcsmmu AddDevice (MMU1), qcsubsys interface check, a path toward ADSP or SLPI loading (audio, sensors) if the DSP boots without votes; QDSS. Not GPU (needs real GX/GPU_CC power), not Qualcomm UART/PCIe (PoFx power) | **Do first.** Cheap, and its request log tells what a real PEP must do |
| **C. qcpep7280 (SC7280)** | PEP0 `_HID QCOM0A17` and PEPH `DEV_0A17`; install `qcpep.wd7280.inf`; try stub mode first (flag in the INF), then normal mode with kd | 1–2 boots to learn; patches if needed | Medium: SC7280 clock/ICB tables (wrong GCC writes possible in DPM), possible SoC-ID check, PPM tuned for SC7280, PM7325 AeoB resources | If normal mode survives: real PoFx DPM/PPM (CPU idle states, thermal via TSENS which matches SM8250), the full interface layer. Stub mode: same as B but with vendor-correct interface semantics | **Best Qualcomm-binary bet**; try after B or in parallel |
| **D. qcpep8280 stub mode** | 8280_QRD or Surface WHQL build with `EnableStubbedPepForPresilicon=1` (edit the INF AddReg) plus a patch for the NULL spinlock in SelfManagedIoInit | Days (RE plus patch) | Medium: the next presilicon-path bug is unknown | Same as B (stub semantics) | Only if C's stub mode has the same bug and B is insufficient |
| **E. Full own PEP (PoFx DPM, optionally PPM)** | Start from imx6pep (MIT) or the MS sample; accept selected devices; per-device power-up in C: RPMh ARC/VRM votes via cmd-db (code exists in apollobt), AOSS QMP `load_state`, GCC/GPU_CC/DISP_CC clocks and GDSCs from Linux SM8250 tables, ICB/BCM bandwidth | Months for broad coverage; weeks for one device class (e.g. ADSP: LCX/LMX plus QMP) | Medium–high: correctness of clock and rail sequencing; thermal/LMh ownership; PPM optional (apollocpufreq already does frequency) | Whatever devices it implements; needed for Qualcomm's own GPU (qcdx) and possibly audio and Wi-Fi stacks | Only targeted (ADSP rails) after B shows exactly what fails |

What each unlocks for the other Qualcomm drivers:
- **Wi-Fi:** none of the PEP routes is required if apollo follows the handoff's plan (QCA6390 over PCIe by hand, plus the `qcwlan6800sc8180` NDIS driver). A PEP only matters for Qualcomm's qcppx/PCI0 path or the qcwlan8250 subsystem stack.
- **GPU:** needs C (normal mode) or E. Stubs are insufficient.
- **Audio and sensors (ADSP/SLPI):** B, C or D are prerequisites (interface layer). Likely also needs rail votes (E-lite) plus Xiaomi-signed firmware squashed from the modem partition (as K40 did) and Kona subsystem configuration.
- **Display:** stays on the GOP framebuffer without PEP. qcdx needs C or E plus a panel configuration.

### Gaps
- No empirical data for B, C or D on SM8250. All effort and risk figures above are estimates from the code and the failures documented by apollo and K40.
- Whether apollo's own drivers and the Qualcomm drivers released by a started PEP0 can coexist is untested.
