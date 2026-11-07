# Audio on apollo (Mi 10T, SM8250) under Windows 11 ARM64: routes, dependency chains, what other SM8250 ports achieved

Researched 2026-11-07. Method: git clones of the repos named below (pinned to the commit that was read) plus local reads of
`/home/user/renegade-apollo` and `/home/user/atechgeekdev/ios-apollo`. Many web hosts were unreachable from this session
(learn.microsoft.com, neowin.net, api.github.com return DNS failure or 403), so Microsoft documentation was read from its
GitHub source (MicrosoftDocs/windows-driver-docs). The general web search found nothing useful for alioth/apollo Windows audio.
Labels used below: **[verified]** = I read it in the file or binary myself; **[claim]** = a project states it, not checked;
**[inference]** = my reasoning.

Pinned sources:
- Kona pack: `WOA-Project/windows_silicon_qcom_kona` @ `22221a09` (2026-01-04)
- Alioth-Engineering: `zhishixuebao-0791/K40_Win11` @ `6316c56d` (2026-10-23)
- qcfakepep: `18934020489Xx/alioth-woa-drivers` @ `3e5e052b` (2026-10-28)
- Silicium status: `Project-Silicium/Mu-Silicium` @ `8471e47` (2026-11-05); alioth ACPI `Project-Silicium/Silicium-ACPI` @ `f50bc11` (2026-10-29)
- alioth Linux DT: `mainlining/linux` branch `nikroks/alioth` @ `022f7f8` (2025-06-08)
- Qualcomm reference drivers mirror: `WOA-Project/Qualcomm-Reference-Drivers` @ `ad6e472` (2026-03-21)

## Q1. Qualcomm's Windows audio stack on Kona/SM8150/SC8180X, its dependency chain, and what the Kona pack has for 8250

### Takeaway
Qualcomm's Windows audio is a chain of about eight drivers: SMMU, SCM, PIL/subsystem loader, GLINK/IPC router, FastRPC, ADSP, the SLIMbus/ADCM/AUDD device, and the miniport. **Every link depends on PEP0's interface.** The Kona pack's "8250" audio pieces are really **SM8150/SC8180X builds**. Its ADSP image is an `ADSP.HT.5.0.1 … SM8150` build signed with Qualcomm **test** keys. Its codec driver and WDSP image target a **WCD9340 (Tavil) codec on SLIMbus**, not apollo's WCD9380 on SoundWire with bolero macros. As shipped, the Kona audio stack cannot drive apollo's audio hardware even with a working PEP.

### Cited Findings
- **[verified] The Kona pack has these audio packages:** `qcsubsys8250` (CombinedSubsystem), `qcpil8250` (HexagonLoader), `qcglink8250`, `qcadsprpc8250`/`qcadsprpcd8250` with `libadsprpc.dll`, `qcslimbus8250`, `qcadcm8250`, `qcauddev8250`, `qcaudminiport8250` with `qcaudminiportti8250.sys` and ProxyAPO, `AudioService8250`, `QCListenSM`, the ACDB extension (`Extensions/Audio/ACDB/MTP|QRD`, `Codec_cal.acdb` etc.), `QcUSBAudio8250` (`ACPI\QCOM2590`), `qcsmmu8250`, `qcscm8250` and `qcpep.wd/wp8250`. It also ships firmware `Extensions/Subsystems/Audio/qcadsp8250.mbn` (10,983,556 bytes), the ADSP FastRPC `.so` modules and `Extensions/Audio/Device/qcwdsp8250.mbn`. The pack contains **no** WCD938x, SoundWire or CS35L41 driver. — [Kona pack tree](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/22221a09e02cb7054792427af60299b1c34d3c3e)
- **[verified] The PnP chain, read from the INF hardware IDs, is:**
  - ACPI `QCOM251D` (ADSP, `qcsubsys8250` plus the `qcsubsys_ext_adsp8250` extension)
  - → `ADSP\QCOM2510` (`qcslimbus8250`), whose INF creates the child `SLM1\QCOM2524`
  - → `qcadcm8250`
  - → `ADCM\QCOM2525` (`qcauddev8250`, "Aqstic Audio Device") and `AUDD\QCOM2537` (MBHC headset detection)
  - → `AUDD\QCOM252C` (`qcaudminiport8250`, which needs `KS.Registration`, `WDMAUDIO.Registration` and `MsApoFxProxy.Registration`)
  - The PIL is ACPI `QCOM251B` (`qcpil8250`). `AudioService8250` binds `QCOM25D2` (AUDS).
  - — INFs under [Drivers/Audio](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Audio) and [Extensions/Subsystems/Audio/qcsubsys_ext_adsp8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Extensions/Subsystems/Audio/qcsubsys_ext_adsp8250.inf)
- **[verified] The ADSP extension sets these registry values:**
  - `AdspImagePath = %13%\qcadsp8250.mbn`
  - `SubsystemLoad\ADSP MemoryReservation 0x1a00000`, alignment 1 MiB
  - FastRPC path mappings to `map_SHARED_LIBS_855.adsp.prodQ.txt`. Note the "855" name.
  - `qcpilEXT8250.inf` puts the ADSP at `MemoryAddress 0x8be00000`. apollo's Android DT reserves `pil_adsp_region@8bb00000`, 0x2500000 bytes (`ios-apollo` xnu patch 0139, sweet_adsp.c header).
  - — [qcsubsys_ext_adsp8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Extensions/Subsystems/Audio/qcsubsys_ext_adsp8250.inf), [qcpilEXT8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Extensions/HexagonLoader/qcpilEXT8250.inf); local `/home/user/atechgeekdev/ios-apollo/xnu/patches/0139-apollo-audio-mi10t-port.patch`
- **[verified] `qcadsp8250.mbn` is an SM8150 ADSP build with test signing.** Its embedded build paths read `ADSP.HT.5.0.1-00194-SM8150-1_20200702_221805`, and its certificate chain reads "Generated Test Root CA1 / Generated Test Attestation CA1". — `strings` on [qcadsp8250.mbn](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Extensions/Subsystems/Audio/qcadsp8250.mbn)
- **[verified] The audio drivers in the "8250" pack are internally 8180 builds:**
  - PDB paths: `qcauddev8180.pdb`, `qcadcm8180.pdb`, `qcslimbus8180.pdb`.
  - `qcauddev` contains `AdieCodecDriverKMDF.c` (an AP-side analog codec driver) and the string `\Device\Slimbus`.
  - Its INF writes `AUDD\CodecInitializationParameters` (`WcdInstanceOffset`, `IsWDSPAvailable=1`, `SwrSleep`, `mclk_speed`) and `SpeakerProtectionParameters`.
  - — `strings` on [qcauddev8250.sys](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Audio/Device/qcauddev8250.sys), [qcadcm8250.sys](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Audio/ADCM/qcadcm8250.sys), [qcslimbus8250.sys](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Audio/Slimbus/qcslimbus8250.sys), [qcauddev8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Audio/Device/qcauddev8250.inf)
- **[verified] `qcwdsp8250.mbn` is the firmware of the WCD9340 codec's own DSP.** It contains `dog_hal_9340_wcd.c` and build tag `WDSP.9340.2.0-00045-W9340AAAAAAAZQ`. `qcauddev_ext8250.inf` installs it as `WdspImageName`. — [qcauddev_ext8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Extensions/Audio/Device/qcauddev_ext8250.inf)
- **[verified] apollo's audio hardware is a different design.** Its DT has a WCD938x codec (reset GPIO 32) with bolero RX/TX/VA/WSA macros and an FSA4480. It has two CS35L41 amps on QUP0 SE3 I2C (0x98c000) at 0x40 (speaker) and 0x42 (earpiece "RCV"), IRQs GPIO 113/112, shared reset GPIO 114. The amps are fed by the tertiary TDM interface (8 slots × 32 bits, AFE port 0x9020). — local `/home/user/atechgeekdev/ios-apollo/NOTES.md` (DT upload notes) and `xnu/patches/0139-apollo-audio-mi10t-port.patch` (sweet_cs35l41.c, sweet_sound.c headers)
- **[verified] Newer Qualcomm Windows SoCs ship no AP-side codec driver.** For 7280/8280/8380 the Qualcomm/OEM driver mirror ships only `qcadcm`, `qcslimbus` and `qcaudminiport(_Base/_apo/extensions)`; no `qcauddev`. Only 8180-era sets have `qcauddev`. — `WOA-Project/Qualcomm-Reference-Drivers` [tree](https://github.com/WOA-Project/Qualcomm-Reference-Drivers/tree/ad6e472) (package names only)
- **[verified] alioth's dependency chain was found with ETW and disassembly.**
  - PEP0 provides device interface `{55beca03-ba2c-4fbc-9efc-ee8eca56f0c7}` and QueryInterface `{a0e1dd36-…}`.
  - Without PEP0: SMMU (`QCOM2509`/`0509`) fails with Code 31 `0xC0000001`, then qcsubsys ADSP fails with `0xC000003B`.
  - ACPI `_DEP` of ADSP in the alioth DSDT: `PEP0, PILC, GLNK, IPC0, RPEN (ReSet Protection ENabler), SSDD, ARPC`. ADCM additionally depends on `MMU0, IMM0`.
  - — [dead-ends-and-breakthroughs.md](https://github.com/zhishixuebao-0791/K40_Win11/blob/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md) (breakthroughs 6–8), [Silicium-ACPI alioth DSDT](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc11/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl) (Device ADSP, ADCM)
- **[claim, alioth team] Windows' kernel IOMMU path is unavailable on these phones.** `IoGetIommuInterface` only works when Hyper-V owns the SMMU (`nt!HalpHvIommu`). On Android-shipped phones QHEE holds EL2, and slbounce explicitly does not support Android devices. So qcsmmu cannot take that path, and PEP (or a replacement for it) must provide the interface. — same file, dead ends 13–14
- **[verified] apollo's current DSDT drops every audio node.** `make-apollo-dsdt.py` drops `AUDS, PILC, SCSS, ADSP, SSDD, MMU0, MMU1, IMM0, IPC0, GLNK, ARPC, ARPD, CFSA, …` and runs with `--no-pep`. — local `/home/user/renegade-apollo/uefi/apollo/make-apollo-dsdt.py` lines 27–35; `uefi/apollo/README.md` "Decision: run without Qualcomm's PEP"

### Inferences
- **[inference, strong] Even with a working PEP (or qcfakepep), the Kona `qcauddev`/`qcslimbus` pair expects a WCD934x behind a SLIMbus controller.** SM8250 has no such codec: WCD938x is reached over SoundWire through LPASS's TX/RX macros. The analog paths (earpiece driver, headset, microphones) therefore have no matching driver in any public Windows package.
- **[inference, strong] The Kona `qcadsp8250.mbn` cannot be used on apollo**, for two reasons:
  1. It is test-signed, while apollo's TZ authenticates PIL images with Xiaomi's production chain.
  2. It is an SM8150 build.
  apollo's own `adsp.mdt/.bNN` (on the modem partition; `ios-apollo/tools/pull-audio-fw.sh` extracts them) is the only ADSP image that will authenticate. ios-apollo proved that this image authenticates and runs on apollo (Q2). Whether Qualcomm's Windows `qcadcm`/miniport can talk to the **Android** ADSP firmware's APR services (ASM/ADM/AFE) is unknown. Both use APR over GLINK, but the Windows stack's ACDB/topology IDs and AFE port setup are Windows-specific.
- **[inference] The 8280/8380 Windows layout is closer to SM8250 hardware** (WCD938x + SoundWire + LPASS macros), but there the codec control appears to sit inside Windows-specific ADSP firmware (no AP-side codec driver ships). That firmware would have to be an SM8250 build signed by Xiaomi, which does not exist.

### Gaps
- I did not disassemble `qcadcm`/`qcauddev` to see whether a TDM-only speaker path (no codec) can be configured from the registry.
- I did not check whether `qcslimbus` on Kona is a real controller or a virtual bus. The Silicium alioth DSDT gives SLM1 MMIO 0x03AC0000 and IRQ 0xC3.
- The exact Windows topology/ACDB format compatibility with Android ACDB files is unknown.

## Q2. What the Poco F3 (alioth) Windows projects achieved, how, and how much is device-specific

### Takeaway
There are three alioth sources, and they disagree.
- **K40_Win11 / Alioth-Engineering** (most detailed public log): **no audio as of 2026-10-23.** The chain stops at SMMU/ADSP because no PEP runs.
- **alioth-woa-drivers' `qcfakepep`** (2026-10-28): a stub PEP whose README *claims* it "unlocks the whole audio chain (SMMU → qcsubsys → ADSP)". The repo contains only a verification plan, no result.
- **Project Silicium's status table**: since 2026-08-06 it marks alioth **Windows: Speakers ✅, Bluetooth ✅, Touch ✅, Battery ✅, sensors ✅, Microphone ❌**. The driver pack behind that row is not public.

alioth's speaker hardware is almost identical to apollo's: two CS35L41 on QUP SE3 I2C, reset GPIO 114, tertiary TDM through the ADSP. Any alioth speaker route is therefore close to directly applicable.

### Cited Findings
- **[verified] Silicium's alioth Windows row changed on 2026-08-06.** Commit `fb69cf4` "Kona: Update" (Vitalii Skorkin = N1kroks, 2026-08-06) flipped the alioth Windows column to Touch/BT/Speakers/Battery/USB Host/PD/accelerometer/compass/gyro ✅ and Charging "Slow" ⚠️. WLAN, GPU, Camera, Vibration and Microphone stay ❌. In the same commit, aliothPkg enabled `CSRT/IORT/MCFG` in `ACPI.inc`, the memory map gained `QTEE`/`TZApps` and changed `HYP` to MEM_RES, and `AcpiTableUpdate.c` set `PRP0 = 1`. Before this, from 2025-03-01 to 2026-08-06, the row had Speakers/BT/Touch ❌, and the earlier note "Needs Windows Drivers: github.com/N1kroks/SM8250-Drivers" had been removed in `f5c5985` (2025-04-30). — [Mu-Silicium Status.md](https://github.com/Project-Silicium/Mu-Silicium/blob/8471e47/Status.md) (section "Xiaomi Poco F3 / Redmi K40 / Mi 11X"); history via `git log` on Status.md
- **[verified] The matching DSDT was rewritten the same day.** Silicium-ACPI commit `8509c58` "Alioth: Update ACPI" (same author, 2026-08-06) rewrote the alioth DSDT (436 KB → 90 KB AML). It now has:
  - `PEP0` `_HID QCOM2517`, `PILC` `QCOM25E0`, `ADSP` `QCOM251B` (with `_DEP` on PEP0/PILC/GLNK/IPC0/RPEN/SSDD/ARPC), child `SLM1`, child `ADCM` (CHLD `ADCM\QCOM2523`), and under that `AUDD` (CHLD `AUDD\QCOM2534`, `AUDD\QCOM2529`) with GPIO 32 (codec reset) and an `MBHC` child.
  - Two `CS35L41` devices (`_HID "CS35L41"`) in `Scope(\_SB.ADSP.ADCM.AUDD)`: `I2C4` addresses 0x41/0x40 at 400 kHz, GpioInt 0x70/0x71.
  - None of these audio HIDs (QCOM2523, QCOM2534, QCOM25E0, "CS35L41") appears in any Kona-pack INF (checked all INFs). `QCOM251B` is the PIL HID in the Kona pack, not the ADSP's. `QCOM2529` is a rear camera sensor in the Kona pack.
  - — [Silicium-ACPI alioth DSDT](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc11/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl); Kona INFs as above
- **[claim] What Silicium's ✅ means is undocumented.** The status table gives no description, driver link or date for "Speakers ✅". I found no public alioth Windows driver pack: `N1kroks/SM8250-Drivers` asks for credentials (private or deleted), and the probed `WOA-Kona/*` names do not exist except `WOA-Kona/Drivers`, which holds only MIT GPIO/SPMI/PMIC drivers (@ `3dcdec0`, 2025-07-16). — [WOA-Kona/Drivers](https://github.com/WOA-Kona/Drivers)
- **[verified] K40_Win11 / Alioth-Engineering status as of 2026-10-23:**
  - ADSP is stuck: `0xC0000034` → `0xC000003B` after writing the 8280-style software key (ImagePath/RPEC/Interfaces) and PILC `SubsystemLoad\ADSP`. Procmon root-caused it to the missing SMMU interface, and ETW/disassembly then traced that to **no PEP** (PEP provides `{55beca03}`).
  - SMMU: the Kona `qcsmmu8250` is Andromeda-signed and gets CI 3004/Code 52 under normal CI. They swapped in the WHQL Surface `qcsmmu8280.sys` binary while keeping the 8250 INF stream table, which gave Code 31.
  - Test-signing then worked (Phase17a; Secure Boot off).
  - `qcpep8250` hangs writing SM8150 LMh registers `0x18350800/0x18358800`. `qcpep8280` hangs on SC8280XP-only TSENS controllers (`0x0c224000/0x0c251000`).
  - Found but not yet tested: `HKLM\SYSTEM\…\Services\qcpep\Parameters\EnableStubbedPepForPresilicon=1` makes `qcpep8280` answer `{a0e1dd36}` with success and skip TSENS.
  - Firmware: they extracted alioth's own `adsp/cdsp/slpi` from `modem_x` via TWRP and merged them with `pil_squasher.py`. They judge that the Kona `qcadsp8250.mbn` ("MTP reference signing") will not authenticate, but this is "inference, not tested".
  - — [dead-ends-and-breakthroughs.md](https://github.com/zhishixuebao-0791/K40_Win11/blob/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)
- **[verified] Their audio experiments install packages but contain no "audio works" result.**
  - Experiment directories: `audio-adsp-phase1b` (signed qcsubsys8250), `audio-alias-phase1` (ADSPRPC/AudioService), `audio-deps-phase2a` (qcpil8250, qcrpen8250 "ResetProtectionEnabler", qcscm8250, qcsmmu8250 with `SMMC.bin`), `audio-fsa4480-minimal`.
  - FSA4480 (`ACPI\FSA04480`, I2C5 0x42) enumerates but sits at Code 51 (waiting on dependencies).
  - — [Alioth-Engineering/experiments](https://github.com/zhishixuebao-0791/K40_Win11/tree/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/experiments), notes `alioth-audio-gap-20260423_151444.md`, `dsdt-audio-targets-20260424.md`
- **[verified code / claim effect] `qcfakepep`:**
  - Pure WDM, binds `ACPI\VEN_QCOM&DEV_0519` and `DEV_2517`.
  - Publishes `{55beca03}` and answers QueryInterface `{a0e1dd36}` (size 0x28, version 1, one function) with unconditional `STATUS_SUCCESS`, with zero MMIO.
  - The README claims it "unlocks the whole audio chain (SMMU → qcsubsys → ADSP)". DEPLOY.md only lists *expected* results ("MMU0/MMU1 from Code 31 → OK", then "restore qcsubsys one by one … watch ADSP status code") and records no outcome.
  - Single commit, 2026-10-28; status "experimental".
  - — [qcfakepep.c](https://github.com/18934020489Xx/alioth-woa-drivers/blob/3e5e052b2de21fa63e7303a6e34b65f7796cd008/qcfakepep/qcfakepep.c), [README.md](https://github.com/18934020489Xx/alioth-woa-drivers/blob/3e5e052b2de21fa63e7303a6e34b65f7796cd008/README.md), [DEPLOY.md](https://github.com/18934020489Xx/alioth-woa-drivers/blob/3e5e052b2de21fa63e7303a6e34b65f7796cd008/DEPLOY.md)
- **[verified] alioth's audio hardware matches apollo's closely.**
  - Linux DT: two `cirrus,cs35l41` on `&i2c3` at 0x40 ("RCV") and 0x41 ("LCV"), IRQ GPIO 113/112, reset GPIO 114, boost 4000 mA / 1000 nH / 15 µF; `fcs,fsa4480` on `&i2c15` at 0x42.
  - The speaker DAI link is `TERTIARY_TDM_RX_0` through `q6afedai` (the ADSP's AFE), pins `mi2s2_sck/ws/data0/data1`.
  - apollo: amps at 0x40/**0x42**, the same reset GPIO 114, IRQs 113/112, the same boost values, tertiary TDM.
  - Silicium's alioth Linux column also shows Speakers ✅ / Microphone ❌.
  - — [sm8250-xiaomi-alioth.dts](https://github.com/mainlining/linux/blob/022f7f8/arch/arm64/boot/dts/qcom/sm8250-xiaomi-alioth.dts) (lines ~739–1150); ios-apollo NOTES.md

### Inferences
- **[inference] What the August 2026 Silicium alioth setup probably runs.** Sensors (SLPI), battery/charging (PMIC GLINK) and speakers (ADSP AFE TDM) all need DSP subsystems loaded. The setup is therefore most likely running a PEP solution plus a Qualcomm subsystem stack that is **not** the stock Kona pack, because its HIDs do not match Kona INFs. It is probably a newer Qualcomm 8250 Windows driver set, or SC8180X/8280 drivers re-targeted to these HIDs. Placing the CS35L41 nodes under AUDD suggests a Qualcomm audio driver that controls the amps itself, as `qcauddev` does for WSA amps. Unverified.
- **[inference] Speakers are the most portable piece**, because both phones use the same amp model, TDM interface and reset GPIO. The differences are I2C address 0x41 vs 0x42 and IRQ assignment, which are DSDT/registry-level changes. The microphones need WCD938x/SoundWire, which is ❌ on alioth even in Linux.
- **[inference] The two alioth groups have not converged.** K40_Win11's PEP conclusions (qcpep8250 and qcpep8280 both hang on SM8250) contradict Silicium's ✅ table, unless Silicium uses a different PEP binary or the presilicon-stub registry switch. Ask N1kroks/Silicium (Discord/Telegram) which driver pack makes "Speakers ✅" before investing.

### Gaps
- No public evidence (logs, screenshots, driver list) behind Silicium's alioth "Speakers ✅". The repo/package is not found, and web search returned nothing.
- No published qcfakepep test result. K40_Win11's last pushed note is phase18a (2026-10-23).
- Which binary binds `QCOM2534`, `QCOM2529`, `QCOM2523`, `QCOM25E0` and `CS35L41` in Silicium's alioth ACPI is unknown.

## Q3. Windows driver paths for CS35L41 and WCD938x; can the amps be driven without the ADSP?

### Takeaway
- **CS35L41 control (I2C) needs no ADSP.** ios-apollo drives both amps from the AP over GENI I2C. A Windows KMDF driver doing the same is straightforward; apollotouch already shows that pattern.
- **The amps' audio data cannot reach them without the ADSP.** The TDM interface, its clocks and LPASS are owned by the ADSP. ios-apollo found that touching LPASS clock registers from the AP causes an external abort, and its speaker path runs through ADSP AFE/ADM/ASM over GLINK/APR.
- No public Windows driver exists for WCD938x. Cirrus' Windows CS35L41 drivers exist only inside OEM laptop packages (HDA side-codec over Intel/AMD I2C/SPI) and do not fit a Qualcomm TDM design.

### Cited Findings
- **[verified] ios-apollo's CS35L41 driver is a self-contained AP-side I2C driver.** It programs the two CS35L41 rev B2 over QUP0 SE3 (0x98c000, GPIO 119/120, polled FIFO, 400 kHz) at 0x40/0x42 with reset GPIO 114. It follows Linux `cs35l41.c`/`cs35l41-lib.c`: OTP boot and trims, B2 errata, boost 1 µH / 15 µF / 4 A, ASP as TDM (DSP_A), PLL from the 12.288 MHz bit clock, 48 kHz, DAC fed from ASPRX1, bypassing the Cirrus DSP and its speaker-protection firmware. Analog gain is 12 of Android's 18, and volume is capped 20 dB below Android by default. — local `ios-apollo/xnu/patches/0139-apollo-audio-mi10t-port.patch` (sweet_cs35l41.c header, ~line 3300)
- **[verified] The audio data path goes through the ADSP:**
  1. ADSP loaded by TZ PAS: SCM 0x42000201/202/205, image at 0x8bb00000.
  2. SMP2P items 443/429, IPCC client 3, AOP QMP "adsp", RPMh LCX/LMX level 384.
  3. GLINK `apr_audio_svc` + QRTR.
  4. AFE: TERT_TDM_RX_0 port 0x9020 in group 0x9120, bit clock TER_TDM_IBIT, TDM pins GPIO 133/135/134 function 1 and 137 function 3.
  5. ADM COPP, ASM stream.
  6. Sample buffers via SMMU stream 0x1801.
  ios-apollo first heard a test tone on 2026-10-28 (boot 63) and full iOS audio in boot 64. — ios-apollo NOTES.md "BOOT 63", "BOOT 64"; 0139 sweet_adsp.c/sweet_sound.c headers
- **[verified] ios-apollo states plainly that LPASS belongs to the ADSP:** "Reading an LPASS clock register from here ends the kernel with an external abort: the DSP owns that block, and talks about it only this way." — 0139 sweet_adsp.c original header (removed lines quoted in the patch)
- **[verified] The microphones on apollo also go through the DSP.** WCD938x via the TX macro and SoundWire TX master (v1.5.1, device ID 0xd01170223), with LPASS clocks voted through the DSP, codec reset GPIO 32, AFE port 0xB037, ACDB-derived topology 0x10010315. They worked in boot 93 (2026-10-31). — ios-apollo NOTES.md "BOOT 93", boot 80 entry
- **[claim] Cirrus' Windows support for CS35L41 is OEM-bundled.** Linux `cs35l41_hda` (since 5.17) serves laptops with CS35L41 behind HDA codecs on I2C/SPI (ACPI `CSC3551`). Linux patch threads say many OEMs hard-coded CSC3551 configuration in their proprietary Windows software, and one HP quirk set was "taken from HP's proprietary Windows driver". A user reports a "Cirrus Logic Amplifiers" driver on Intel Serial-IO I2C under Windows. — [Phoronix: Linux 5.17 CS35L41 HDA](https://www.phoronix.com/news/Linux-5.17-Cirrus-CS35L41); [LKML: CSC3551 without _DSD](https://lkml.iu.edu/hypermail/linux/kernel/2307.1/06856.html); [LKML: HP models without _DSD](https://lkml.iu.edu/hypermail/linux/kernel/2401.0/01102.html); [Launchpad #1958019 comment](https://bugs.launchpad.net/ubuntu/+source/alsa-driver/+bug/1958019/comments/528) (read as search snippets; the pages could not be fetched)
- **[verified] No Cirrus/CS35L41 package exists in the Qualcomm/OEM WoA driver mirror** (Surface, Lenovo, HP, Samsung, ASUS, ECS folders). Qualcomm WoA laptops use Qualcomm's own audio stack. — [Qualcomm-Reference-Drivers](https://github.com/WOA-Project/Qualcomm-Reference-Drivers/tree/ad6e472) (filename scan for cs35/cirrus/amp)
- **[verified] The Silicium alioth DSDT uses `_HID "CS35L41"`**, not Cirrus' laptop ID `CSC3551`, so a Cirrus laptop INF would not match it anyway. — Silicium-ACPI alioth DSDT

### Inferences
- **[inference] There is no "amps without ADSP" route on SM8250.** The amps only take serial audio (TDM/I2S, ASP). The only SoC serial-audio interface wired to them is LPASS tertiary TDM, which only the ADSP can clock and feed. A pure-AP Windows driver can power, configure and mute the amps (useful to keep them safe and silent), but cannot play sound. The CS35L41's internal tone/DSP features need firmware and still produce no OS audio.
- **[inference] The realistic built-in-speaker route on Windows is an own driver stack modelled on ios-apollo (port of hakios-apollo):**
  1. ADSP PIL via SCM PAS with apollo's own `adsp.mdt`.
  2. SMP2P/GLINK/QRTR/APR in a KMDF driver.
  3. AFE TDM port + ADM + ASM.
  4. SMMU context for stream 0x1801. Linux `arm-smmu` / ios-apollo shows how; this is the same SMMU problem Wi-Fi has.
  5. CS35L41 I2C setup.
  6. A WaveRT or ACX miniport (or the sysvad-style sample) feeding ASM buffers.
  Everything here is SoC-generic SM8250 plus apollo's numbers already captured in 0139. The license of hakios-derived code is APSL 2.0 (per 0139 headers); check compatibility before copying code into the Windows drivers.
- **[inference] WCD938x (earpiece-free analog paths, headset, microphones) is a second, larger project.** It needs a SoundWire master and the WCD938x register sequences; ios-apollo's `sweet_mic.c` (2646 lines) is the reference. The earpiece itself is the second CS35L41 at 0x42 (not WCD938x), so earpiece output comes with the speaker work.

### Gaps
- I could not read Cirrus Logic's own Windows driver documentation (no public page found; hosts blocked).
- Whether Windows 11 has any inbox SoundWire class support usable on Qualcomm was not established. The doc search was aborted, and no SoundWire page names were found in windows-driver-docs.

## Q4. Practical fallbacks: USB audio and Bluetooth audio

### Takeaway
- **USB digital audio is the quickest working audio and needs zero new drivers.** USB host already works on apollo under Windows. Windows' inbox `usbaudio.sys` (UAC1) and `usbaudio2.sys` (UAC2, since 1703) bind any class-compliant USB-C DAC dongle, USB headset or USB speaker.
- **Passive USB-C-to-3.5 mm adapters will not work.** They rely on the phone's FSA4480 + WCD938x analog path.
- **Bluetooth A2DP/HFP are inbox Windows 11 profiles** and come for free once `apollobt` gives BthMini a working HCI. A2DP throughput needs the UART above 115200 baud; ios-apollo needed 921600.

### Cited Findings
- **[verified, Microsoft docs] UAC2:** "Starting with Windows 10, release 1703, a USB Audio 2.0 driver is shipped with Windows … The driver is a WaveRT audio port class miniport … named usbaudio2.sys." It binds compatible IDs `USB\Class_01&SubClass_0x&Prot_20`. — [usb-2-0-audio-drivers.md](https://github.com/MicrosoftDocs/windows-driver-docs/blob/main/windows-driver-docs-pr/audio/usb-2-0-audio-drivers.md)
- **[verified, Microsoft docs] UAC1:** `Usbaudio.sys` is the AVStream class driver for USB Audio 1.0 devices. — [usb-audio-class-system-driver--usbaudio-sys-.md](https://github.com/MicrosoftDocs/windows-driver-docs/blob/main/windows-driver-docs-pr/audio/usb-audio-class-system-driver--usbaudio-sys-.md)
- **[verified, Microsoft docs] Bluetooth:** Windows 11 22H2 has in-box A2DP 1.3.2, AVRCP 1.6.2 and HFP 1.7.2. A2DP sideband *offload* (Windows 11 22000+) is optional and needs Microsoft-defined HCI commands plus an audio DSP driver; plain A2DP does not. — [general-bluetooth-support-in-windows.md](https://github.com/MicrosoftDocs/windows-driver-docs/blob/main/windows-driver-docs-pr/bluetooth/general-bluetooth-support-in-windows.md), [audio-sideband-a2dp-offload.md](https://github.com/MicrosoftDocs/windows-driver-docs/blob/main/windows-driver-docs-pr/audio/audio-sideband-a2dp-offload.md)
- **[verified] USB host works on apollo's Windows, and USB tethering/Ethernet is suggested as the Wi-Fi stopgap.** `apollobt` (QCA6390 over SE6 UART with an `MS_BTHX_BTHMINI` child) is built (1.0.10.0) but never run. — local `/home/user/renegade-apollo/docs/handoff.md` "Current state"
- **[verified] ios-apollo's Bluetooth audio history.** A2DP audio stuttered at 115200 baud and was smooth at 921600. Headphone pairing, A2DP, HFP and AVRCP connected on apollo under iOS. — ios-apollo NOTES.md boots 76–78, "BT STAGE 4"
- **[verified] The Kona pack's `QcUSBAudio8250` (`ACPI\QCOM2590`) is Qualcomm's own USB audio driver.** Its DriverVer is 01/04/2018 1.0.7900. It binds an ACPI device, not USB class IDs, so it is not needed for class-compliant dongles. — [QcUSBAudio8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/USB/USBAudio/QcUSBAudio8250.inf)
- **[verified] apollo has an FSA4480 USB-C analog switch**, and alioth's FSA4480 sits at Code 51 under Windows. — ios-apollo NOTES.md (DT upload); K40_Win11 notes (`dsdt-audio-targets-20260424.md`)

### Inferences
- **[inference] The USB-C port is one port**, so a USB audio device competes with charging and USB Ethernet. A powered USB-C hub with PD pass-through, audio and Ethernet would address that; whether Windows' USB PD/role handling on apollo allows charging through a hub is not established.
- **[inference] Bluetooth is the best "wireless daily" audio.** HFP voice over HCI/SCO depends on QCA6390 routing SCO over HCI rather than its PCM pins, which is not looked at for apollo. A2DP-only is likely first.

### Gaps
- Not tested which UAC2 dongles enumerate on apollo's DWC3 host under this Windows build. USB host is reported working generically.
- Microsoft's announced low-latency UAC2/ASIO driver for WoA could not be read (neowin.net blocked); it does not change the inbox UAC1/UAC2 conclusion.

## Q5. Effort and risk per route

### Takeaway
Ranked by value for effort: (1) USB audio now; (2) Bluetooth A2DP once apollobt works; (3) own ADSP + TDM + CS35L41 speaker/earpiece stack ported from ios-apollo (weeks to months, high complexity, but every hardware step is proven on this phone); (4) Qualcomm stack via stub PEP (blocked by a PEP chain with no published success and a codec/ADSP mismatch); (5) microphones/headset via WCD938x (largest effort).

### Cited Findings
- **[verified] ios-apollo needed many staged boots to bring audio up, even reusing hakios-apollo's working drivers.**
  - Stages A–C (DSP up, GLINK/APR, tone through the amps) came in one boot, boot 63, 2026-10-28.
  - iOS audio (stage D) came in boot 64.
  - Stutter fixes followed (boots 77–80, 0146–0147).
  - The mic was usable in boot 93 (2026-10-31).
  - — ios-apollo NOTES.md
- **[verified] The alioth Qualcomm-stack effort has run for about six months** (notes from 2026-04-22 to 2026-10-23, phases 1–18a). Problems met: signing (Andromeda-signed Kona drivers → CI 3004/Code 52; WHQL 8280 binaries as substitutes), registry/extension-INF shape for 8280 drivers, PEP hangs, and no Hyper-V IOMMU. None of it produced audio. — [K40_Win11 notes](https://github.com/zhishixuebao-0791/K40_Win11/tree/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes)
- **[verified] apollo's own-driver pattern already works for three drivers:** KMDF drivers touching hardware directly (apollotouch over SPI + VHF, apollocpufreq), CI-built and test-signed, with progress reported through the registry. — `/home/user/renegade-apollo/docs/handoff.md`

### Inferences
Effort figures below are estimates. Risks rated high come from SCM/TZ and SMMU misprogramming.

| Route | Depends on | Effort (est.) | Risk | Evidence level |
|---|---|---|---|---|
| A. USB-C DAC/headset/speaker (UAC1/2 inbox) | USB host (works) | Hours: buy or test a dongle | Very low | Inbox driver documented; USB host works on apollo |
| B. Bluetooth A2DP/HFP (inbox profiles) | apollobt working; UART ≥ 921600 for A2DP | Days, after apollobt passes | Low–medium (BT driver hang risk already planned for with `BT=0`) | Profiles inbox; apollo BT audio proven on iOS |
| C. Own speaker + earpiece stack: KMDF ADSP PIL (SCM PAS, apollo `adsp.mdt`), SMP2P/GLINK/QRTR/APR, AFE TDM/ADM/ASM, SMMU stream 0x1801, CS35L41 over I2C, WaveRT/ACX miniport | AOP QMP + RPMh votes (apollobt has RPMh code), SCM calls from a Windows driver (may need `qcscm` or direct SMC; Windows may restrict SMC from drivers, untested), SMMU context bank programming without qcsmmu | Large: about 5–10k lines of C to port from 0139 (sweet_adsp.c ~2200, sweet_sound.c ~1550, sweet_cs35l41.c ~2000 lines) plus a Windows audio miniport | High: TZ/SMMU faults can hang or reset; speaker damage risk without Cirrus speaker protection (ios-apollo mitigates with gain caps and a "guard") | Every hardware step proven on apollo under XNU |
| D. Qualcomm stack via qcfakepep (or qcpep8280 `EnableStubbedPepForPresilicon`) + SMMU/PIL/subsys/GLINK/ADCM/AUDD/miniport | Stub PEP works; signing workarounds; alioth's own ADSP firmware; a codec driver matching WCD938x/SoundWire **and** a TDM speaker endpoint; matching ACDB | Unbounded: alioth spent 6 months without audio | High: PEP is boot-critical (apollo removed it to boot at all); the codec mismatch may make it impossible | Only claims; Silicium's ✅ without a public pack |
| E. Microphones / USB-C analog headset via WCD938x + SoundWire + FSA4480 | Route C done; SoundWire master; WCD938x sequences; ACDB-derived topologies | Large, after C | Medium | Proven on apollo under XNU (boot 93) |

- **[inference] Recommendation for the report writer:** treat A (and B when BT lands) as the daily-driver audio answer. Ask Silicium/N1kroks what binds the alioth HIDs before spending time on D. Start C only as a staged "DSP up and linked" driver first, mirroring ios-apollo's `apollo_adsp_step` design. That first stage also benefits Wi-Fi/sensors (shared SMP2P/GLINK/QMP/SMMU groundwork).
- **[inference] Shared groundwork with other features:**
  - The SCM/PAS + SMP2P + GLINK + QMP + RPMh layer from route C is also what loading the CDSP/SLPI (sensors) would need.
  - The SMMU identity/context work is shared with the PCIe Wi-Fi plan in handoff.md.
  - Battery/charging via PMIC GLINK also rides on the ADSP on SM8250 (inference from Silicium listing battery ✅ together with ADSP-dependent features; the handoff lists battery PM8150B as a separate, unproven ios-apollo port).

### Gaps
- Whether a Windows KMDF driver can issue the TZ PAS SMC calls directly (without `qcscm`) on this firmware was not researched. It is a key feasibility question for route C.
- Effort numbers are estimates from line counts and ios-apollo's boot history, not measured.
