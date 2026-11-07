# Route around qcpep, not through it

**Short answer: no Qualcomm PEP binary can be "fixed" for SM8250 in any way that will reliably deliver Wi-Fi, GPU, audio or HDMI, and only one of those four goals needs a real PEP at all.** No project has run any qcpep build on SM8250 under Windows, as of 2026-11-07. The WOA "qcpep8250" you patched is a renamed SM8150/SC8180X binary. qcpep8280 hangs on SC8280XP-only thermal sensors. Each patch so far has only exposed the next hard-coded address from another chip. The four goals depend on PEP in four different ways:

- **Wi-Fi** does not need a PEP. The only usable Windows driver for apollo's QCA6390 is a self-contained PCIe NDIS miniport. What it needs is PCIe brought up by our own code.
- **Audio** needs PEP's interface handshake, which a stub can fake. But Qualcomm's Kona audio stack targets a codec apollo does not have, so built-in speakers really mean porting ios-apollo's own ADSP stack.
- **GPU acceleration** is the one goal that truly needs a real PEP that switches on clocks and power rails. No SM8250 Windows port has it. Treat it as a research track, not a daily-driver dependency.
- **HDMI over USB-C** is closed in hardware: Xiaomi wired apollo's port as USB 2.0 with the DisplayPort PHY disabled. A DisplayLink USB adapter is the only wired external display.

The practical path to a daily driver:

1. **This week, no new drivers:** USB Ethernet, a USB-C DAC and a DisplayLink adapter.
2. **Next:** finish `apollobt` for Bluetooth audio.
3. **Then:** a staged PCIe probe driver toward native Wi-Fi.
4. **On the side:** a time-boxed, cheap PEP experiment (your own logging stub PEP, or one boot of qcpep7280). Run it for what it teaches, not because anything depends on it.

## Patching qcpep8250 is open-ended, and a stub PEP powers nothing

### What has been tried, and what has failed

The WOA Kona pack's `qcpep8250.sys` is byte-identical to the Hana (SM8150) pack's `qcpep8150.sys`: same git blob, same DriverVer 1.0.2140.0 ([kona](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/main/Drivers/SOC/PowerManagement), [hana](https://github.com/WOA-Project/windows_silicon_qcom_hana/tree/main/Drivers/SOC/PowerManagement)). Its SHA-256 matches the `ORIGINAL_SHA256` in `uefi/apollo/patch-qcpep-lmh.py`, so this is the exact binary you have been fighting.

**Proven on apollo** (`uefi/apollo/README.md`), the failures came in this order:

1. The PEPH string mismatch (fixed).
2. A CPU hang writing SM8150's LMh block at `0x18350800` (patched to `ret` in three functions).
3. Bugcheck `0x14B`, because the display RSC reports 2 TCS where the table demands 4 (patched).
4. `SOC_CRITICAL_DEVICE_REMOVED`, tag `qpe0`, which is unresolved.

**Reported, not reproduced by us:** the Poco F3 (alioth) project K40_Win11 independently found more SC8180X-only accesses waiting behind those. They are another LMh write at `+0x6288` (`0x18350bc0`) and OSM reads at `+0x6388` ([K40 dead-ends](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)). The same project ran qcpep8280. It hung with `0x101 CLOCK_WATCHDOG_TIMEOUT` on two thermal-sensor (TSENS) controllers that exist only on SC8280XP. With the hidden registry flag `EnableStubbedPepForPresilicon=1` forced through kd, it got past AddDevice, PrepareHardware and D0Entry, then died with `0x10D WDF_VIOLATION`: a NULL spinlock handle in SelfManagedIoInit ([K40 phase17h2](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/phase17h2-qcpep8280-kd-result-20261022.md)). Every SM8250 Windows port that reaches a desktop does so with no PEP bound: pipa, alioth on Silicium, K40_Win11 and apollo.

### The hidden danger is in the clock tables, not the next crash

The deeper problem is not the next crash address. qcpep is a whole SoC power stack: its own clock driver, interconnect (ICB) arbitration, RPMh rail clients, TSENS, LMh and processor power management. It resolves the clock names in PEP0's ACPI tables against **built-in, SoC-specific tables**. **Verified by static scan:** qcpep8250 contains all 150 clock names, 8 GDSC names and 38 ICB IDs that pipa's DSDT references. Those tables belong to SC8180X ([kona binaries](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/main/Drivers/SOC/PowerManagement)).

**Inferred, not verified:** SM8250's clock controller (GCC) register layout differs from SC8180X's; Linux has separate `gcc-sc8180x.c` and `gcc-sm8250.c`. So even if AddDevice finally survives, the first real "turn this device on" request could write the wrong clock registers. That is the danger of route A below. The prize, real power management, is exactly the part most likely to be wrong.

### A stub PEP provides the handshake only

Qualcomm's PEP does two separable jobs.

**Job 1: a private interface layer.** It publishes device interface `{55beca03-…}` and answers QueryInterface for `{a0e1dd36-…}` (one function) and `{99f8bbdb-…}` (three functions). Without it, qcsmmu fails AddDevice with Code 31 and qcsubsys fails loading the ADSP with `0xC000003B`. That is the chain K40 traced with ETW and disassembly ([K40 breakthroughs 6–8](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/dead-ends-and-breakthroughs.md)). Windows' own IOMMU interface, the alternative that would let qcsmmu skip PEP, only exists when Hyper-V owns the SMMU. That is impossible on Android-shipped phones, and slbounce states it cannot be used on Android devices ([slbounce](https://github.com/TravMurav/slbounce)). A stub can fake job 1.

**Job 2: a real PoFx power plug-in.** When a device starts or changes power state, PoFx tells the PEP to turn on the device's clocks, power domains (GDSCs), RPMh rails and bus bandwidth. Microsoft's DPM documentation says that when a PEP claims a device, "the device and all its components must be turned on, and clocks to the device must ungated" ([MicrosoftDocs DPM notifications](https://github.com/MicrosoftDocs/windows-driver-docs/blob/staging/windows-driver-docs-pr/kernel/dpm-notifications.md)). A stub cannot fake job 2. Devices then work only if the firmware left them powered.

### Do not install alioth's qcfakepep as published

alioth's `qcfakepep` (2026-10-28) is the stub everyone is pointing at. It is **unverified**: its repository has only a test plan with "expected" outcomes and no results ([alioth-woa-drivers DEPLOY.md](https://github.com/18934020489Xx/alioth-woa-drivers/blob/3e5e052b2de21fa63e7303a6e34b65f7796cd008/DEPLOY.md)). Reading its source shows three problems ([qcfakepep.c](https://github.com/18934020489Xx/alioth-woa-drivers/blob/main/qcfakepep/qcfakepep.c)):

- It answers only `{a0e1dd36}` and does not implement `{99f8bbdb}`.
- It registers no PoFx plug-in.
- In `DriverEntry` it unconditionally maps physical `0x1_8591_0000` instead of the CPU-frequency block at `0x1859_1000`, although its own comment gives the right address. On a phone with 6–12 GB of RAM, that address is ordinary DRAM. **Do not install qcfakepep on apollo as published.**

### What a PEP route gives each of your four goals

The table below combines the verified dependency findings with inferences from them.

| Goal | Does a stub PEP (job 1) help? | Is a real PEP (job 2) required? | Is PEP the actual blocker? |
| --- | --- | --- | --- |
| Wi-Fi | Only optionally (it might let qcsmmu handle PCIe DMA mapping, unverified) | No | **No.** The blocker is PCIe bring-up |
| Audio (speakers) | Yes, for Qualcomm's chain to start at all | Probably, for DSP rail votes | **Partly.** The Kona audio drivers target the wrong codec anyway |
| GPU acceleration | No | **Yes**: GX rail, GPU and display clocks, bus votes | **Yes**, plus several more Qualcomm drivers |
| HDMI over USB-C | No | — | **No.** The hardware is configured without DP |

### Five PEP routes, ranked

Effort and risk figures in this table are inferences from the code and the documented failures; no route has been tested on SM8250.

| Route | Effort | Risk | What it can unlock | Verdict |
| --- | --- | --- | --- | --- |
| **B. Your own stub PEP**: publish `{55beca03}`, answer both interfaces with success, no MMIO, log every request to the registry | 1–3 days, roughly 300–600 lines | Low for the driver itself. Starting PEP0 releases all of the roughly 32 devices that `_DEP` on it, so Qualcomm UART/SPI drivers could fight `apollotouch`/`apollobt` | qcsmmu AddDevice, the qcsubsys interface check, and a log of what downstream drivers ask a PEP to do | Best value. Its main product is information |
| **C. qcpep7280 (SC7280)**: try stub mode first, then normal mode with kd | 1–2 boots to learn | Medium: SC7280 clock tables, possible SoC-ID check | If normal mode survives: real power management for the devices it knows | Best Qualcomm-binary bet. Never tried by anyone |
| **D. qcpep8280 stub mode** plus a patch for the NULL spinlock | Days of reverse engineering | Medium: the next presilicon bug is unknown | Same as B | Only if C fails the same way |
| **A. Keep patching qcpep8250** | Open-ended: each boot finds one more address | High: wrong clock tables | In practice, probably only the interface layer | Stop |
| **E. Full own PEP**, from Microsoft's MIT PEP sample or ms-iot's imx6pep (about 6.5k lines) | Weeks per device class, months for broad coverage | Medium–high | Whatever it implements; needed for Qualcomm's GPU driver | Only targeted, after B shows what is needed |

Why qcpep7280 stands out (**verified by static address scan**): every SoC block it hard-codes and the scan found exists at the same address on SM8250. That covers TSENS0/1, the three CPU-frequency (EPSS) domains at `0x18591000/2000/3000`, the APPS RSC and AOSS QMP. It has no display-RSC table and no SM8150 LMh addresses, and it carries the same stub-mode switch as qcpep8280 ([Qualcomm-Reference-Drivers](https://github.com/WOA-Project/Qualcomm-Reference-Drivers); [Linux sm8250.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi) vs [kodiak.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/kodiak.dtsi)).

The scanner has limits. It missed the `0x18350800` address that kd proved qcpep8250 uses, so "no hit" is not proof of absence. Nobody has checked whether qcpep7280 refuses non-SC7280 chips. To try it: PEP0 gets `_HID QCOM0A17` and PEPH `ACPI\VEN_QCOM&DEV_0A17`. Put the stub flag into the INF copy before installing, because the INF's AddReg resets it to 0 on every install.

## Wi-Fi needs a PCIe bring-up, not a PEP

### The driver to use

**Proven by inspecting the package:** exactly one public Windows driver binds apollo's QCA6390 (`PCI\VEN_17CB&DEV_1101`). It is Qualcomm's `qcwlan6800sc8180` from the SC8180X reference set (latest 200.0.117.0, driver 1.0.2160.0). It is an ARM64 NDIS miniport that imports only NDIS, ntoskrnl, NETIO, HAL and WDF, and carries its own MHI host, QMI client and IPC router. It has no dependency on PEP, QCMS or qcppx ([Qualcomm-Reference-Drivers 8180_CLS](https://github.com/WOA-Project/Qualcomm-Reference-Drivers/tree/master/8180_CLS)). Its INF matches only `SUBSYS_010817CB`. apollo's subsystem ID is unknown, so the INF may need one added line and a re-catalog.

The Kona pack's `qcwlan8250` is a dead end even with a perfect PEP. It binds `QCMS\VEN_QCOM&DEV_252B` and ships `wlanmdsp.mbn` for the integrated WLAN block of SM8150-era chips. Linux's `sm8150.dtsi` has `wifi@18800000`, and `sm8250.dtsi` does not ([sm8150.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8150.dtsi), [sm8250.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi)).

### Inbox pci.sys can do the PCI part

Qualcomm's own Kona ACPI design uses Windows' inbox `pci.sys` ([Kona MCFG](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Silicon/Qualcomm/Kona/Decompiled/MCFG.dsl), [IORT](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Silicon/Qualcomm/Kona/Decompiled/IORT.dsl)):

- PCI0 is a standard `PNP0A08` host bridge.
- MCFG puts ECAM for segment 0 at `0x60000000`.
- IORT maps PCIe requester IDs to SMMU streams `0x1C00`+.
- MADT declares a GIC MSI frame at `0x17A10000`.

`qcppx8250` is not a PCI bus driver. It is a PEP plug-in (`PoFxRegisterPlugin`) with no link-training code, which only gates PCI0 through `_DEP` ([Kona PCIe drivers](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/main/Drivers/SOC/PCIe)). Qualcomm's reference UEFI trains the PCIe link before Windows runs; Silicium's UEFI has no PCIe driver, and `make-apollo-dsdt.py` currently deletes PCI0 and PCI1.

### The hardware sequence is proven on apollo itself

**Proven on this exact phone under XNU:** ios-apollo patches 0126–0135 give working QCA6390 Wi-Fi with scan, WPA2 join and Safari traffic. The sequence is RPMh rails, WLAN_EN on gpio20, the PCIe0 PHY and a Gen2 x1 link, its own SMMU context for stream `0x1c01`, and polled MHI ([ios-apollo NOTES.md](https://github.com/aTechGeekDev/ios-apollo/blob/main/NOTES.md), [0126 patch](https://github.com/aTechGeekDev/ios-apollo/blob/main/xnu/patches/0126-apollo-wifi-pcie-mhi.patch)). The radio and link are not in question. The Windows glue is.

### Four unknowns, all answerable by one probe driver

1. **Does SM8250's controller honour Linux's `PARF_ECAM_BASE` and outbound-blocker registers** so pci.sys can scan bus 0 safely? Linux applies them to the `cfg_1_9_0` family that includes SM8250, but only enables ECAM when the config window spans two buses ([pcie-qcom.c](https://github.com/torvalds/linux/blob/master/drivers/pci/controller/dwc/pcie-qcom.c)).
2. **Will the hypervisor allow a bypass or identity SMMU context for stream `0x1C01`?** ios-apollo was allowed to program a context.
3. **Which SPIs does the MSI frame actually raise?** The MADT frame says base 832, count 128; ios-apollo's notes say "SPI 768..".
4. **What are apollo's QCA6390 subsystem ID and board ID?**

### Route A, the native path

These steps are inferred from the evidence above and none has been tested:

1. A staged probe driver in your existing `windrivers/` style. It reuses `apollobt`'s RPMh code (adding smpa4, ldoa9 and ldoa5), ports `sweet_pcie.c` and reports IDs and register read-backs to the registry.
2. ECAM and iATU configuration.
3. Restoring PCI0 in the DSDT behind a build switch like `BT=0`, with its `_DEP` pointing at your driver and the root port's empty power resources reporting "on".
4. The SMMU context.
5. `qcwlan6800sc8180`, with `bd_j3sgl.elf` placed as the board file.

The research notes put success at **40–60% over several weeks of on-device iteration**; that figure is a judgement, not a measurement. Once the register sequence works from Windows, it can move into a Mu-Silicium DXE so pci.sys sees the device exactly as on Qualcomm reference hardware.

The fallback, a full custom Wi-Fi driver ported from ios-apollo, is about 8,000+ lines plus Windows Wi-Fi stack plumbing, which means months. As with all firmware, never commit `wlanfw20.mbn`, `m3.bin`, `bd_j3sgl.elf` or `amss20.bin`.

### Interim networking

USB Ethernet is proven on SM8250 Windows: alioth runs an ASIX AX88179 adapter, behind a powered hub ([K40 driver-strategy](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/driver-strategy-20260422.md)). USB host already works on apollo (`docs/handoff.md`). USB tethering from another phone should bind Windows' inbox RNDIS or UsbNcm drivers; this is a claim from Microsoft's class list, untested on apollo ([supported USB classes](https://learn.microsoft.com/windows-hardware/drivers/usbcon/supported-usb-classes)). A pocket travel router in client mode, cabled into the Ethernet adapter, gives you de-facto wireless. **Do not buy a USB Wi-Fi dongle.** Community reports through October 2025 find essentially none with Windows ARM64 drivers ([TP-Link community](https://community.tp-link.com/en/home/forum/topic/846508), [Parallels forum](https://forum.parallels.com/threads/anybody-get-a-usb-wifi-adapter-to-work.368139/)).

## Audio needs the ADSP, and Kona's audio stack targets the wrong codec

### Why the Kona audio stack cannot work on apollo

Qualcomm's Windows audio is a chain of about eight drivers: SMMU, SCM, PIL loader, GLINK/IPC router, FastRPC, ADSP subsystem, SLIMbus/ADCM/codec device and miniport. Each link sits behind PEP0's interface ([Kona pack](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Audio)). **Verified in the binaries:** the Kona "8250" audio drivers are internally 8180 builds (`qcauddev8180.pdb`, `qcslimbus8180.pdb`). The codec DSP firmware `qcwdsp8250.mbn` is for a **WCD9340 on SLIMbus**. The ADSP image `qcadsp8250.mbn` is an `ADSP.HT.5.0.1 … SM8150` build signed with Qualcomm **test** keys ([qcauddev_ext8250.inf](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Extensions/Audio/Device/qcauddev_ext8250.inf), [qcadsp8250.mbn](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Extensions/Subsystems/Audio/qcadsp8250.mbn)).

apollo has different hardware: a WCD938x on SoundWire, and two Cirrus CS35L41 amplifiers on I2C, fed by the tertiary TDM interface (ios-apollo patch 0139). **Strong inference:** even with a working PEP, the Kona stack has no driver for apollo's codec. Its ADSP image will not pass Xiaomi's TrustZone authentication; only apollo's own `adsp.mdt` will. K40 spent six months on the alioth Qualcomm audio chain (phases 1–18a) without producing sound ([K40 notes](https://github.com/zhishixuebao-0791/K40_Win11/tree/6316c56d97827e84cac94e3eb11308efdb43e397/Alioth-Engineering/notes)).

### The unresolved Silicium "Speakers ✅" claim

Silicium's status table has marked alioth Windows **Speakers ✅** since a 2026-08-06 commit, with Microphone ❌ ([Mu-Silicium Status.md](https://github.com/Project-Silicium/Mu-Silicium/blob/8471e47/Status.md)). The rewritten alioth DSDT places `CS35L41` nodes under the audio device. Its audio HIDs (`QCOM2523`, `QCOM2534`, `QCOM25E0`, `"CS35L41"`) match no Kona-pack INF, and no driver pack is public ([Silicium-ACPI alioth DSDT](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc11/Platforms/Xiaomi/alioth/Decompiled/DSDT.dsl)).

This is the most valuable unknown in the whole project. alioth's speaker hardware is nearly identical to apollo's: the same amplifier model, reset GPIO 114, IRQs 113/112 and tertiary TDM. The only difference is that the second amplifier sits at I2C address 0x41 on alioth and 0x42 on apollo ([alioth Linux DT](https://github.com/mainlining/linux/blob/022f7f8/arch/arm64/boot/dts/qcom/sm8250-xiaomi-alioth.dts)). **Ask N1kroks/Silicium which driver pack produces that ✅ before spending a single boot on the Qualcomm audio chain.**

### What our own drivers can and cannot do

The amplifiers can be configured from the CPU over I2C without the ADSP; ios-apollo's `sweet_cs35l41.c` does exactly that. But the amplifiers can't play sound without it. **Proven under XNU on apollo:** the TDM interface and the audio clock block (LPASS) belong to the ADSP. Reading an LPASS clock register from the CPU "ends the kernel with an external abort". Sound flows from the CPU to the ADSP and on to TDM, with sample buffers on SMMU stream `0x1801` (ios-apollo 0139).

ios-apollo heard its first test tone on boot 63, had full system audio on boot 64 and had working microphones on boot 93 (2026-10-31) ([ios-apollo NOTES.md](https://github.com/aTechGeekDev/ios-apollo/blob/1f75f381e31da96de537653fbac2252d2eb80418/NOTES.md)). A Windows port of that stack has these parts:

- ADSP loading through TrustZone with apollo's own firmware
- SMP2P/GLINK/QRTR/APR messaging
- AFE/ADM/ASM setup on the ADSP
- the SMMU context
- the CS35L41 setup
- a WaveRT or ACX miniport

The notes estimate **5–10k lines**. The risk is high: TrustZone or SMMU mistakes hang or reset the phone, and without Cirrus' speaker-protection firmware the speakers need ios-apollo's gain caps. The licence needs a check too, because the hakios-derived code is APSL 2.0. One feasibility question is unanswered: whether a Windows KMDF driver can issue the TrustZone loader SMC calls directly, without Qualcomm's `qcscm`. Answer that first.

### Audio that works now

USB audio is immediate and needs no new drivers. Windows' inbox `usbaudio2.sys` binds any class-compliant UAC2 device ([MicrosoftDocs USB Audio 2.0](https://github.com/MicrosoftDocs/windows-driver-docs/blob/main/windows-driver-docs-pr/audio/usb-2-0-audio-drivers.md)). That covers a USB-C DAC dongle, a USB headset or a USB speaker. A **passive** USB-C-to-3.5 mm adapter will not work, because it relies on the phone's own analog path.

Bluetooth A2DP, AVRCP and HFP are inbox profiles in Windows 11 ([MicrosoftDocs Bluetooth support](https://github.com/MicrosoftDocs/windows-driver-docs/blob/main/windows-driver-docs-pr/bluetooth/general-bluetooth-support-in-windows.md)). They come for free once `apollobt` gives BthMini a working HCI. That driver is already built (1.0.10.0) and has never been run. ios-apollo found that A2DP stutters at 115200 baud and is smooth at 921600, which `apollobt` already targets.

## GPU acceleration is the only goal that truly needs a real PEP

### What Qualcomm's GPU driver needs

The Kona pack ships one combined display and GPU driver, `qcdxkm8250.sys`. Like the PEP, it is internally `qcdxkm8180.sys`, and it is signed with the "Windows On Andromeda" chain, not WHQL ([qcdxkm8250.sys](https://github.com/WOA-Project/windows_silicon_qcom_kona/blob/22221a09e02cb7054792427af60299b1c34d3c3e/Drivers/Graphics/DXKM/qcdxkm8250.sys)). In a real Kona DSDT, the GPU device `_DEP`s on MMU0, MMU1, IMM0, IMM1, PEP0, PMIC, PILC, RPEN, TREE and SCM0. Its clocks and power are not in the GPU driver at all. They sit in PEP0's `G0MD` table: a display power domain (`mdss_core_gdsc`), 94 `CLOCK` entries, 126 `BUSARB` bandwidth votes and PMIC regulator votes. Executing those tables is qcpep's job ([pipa DSDT](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl)). A stub that returns "success" never turns the GPU rail on. The display only works today because UEFI left the display controller running.

**Verified:** no SM8250 device in Mu-Silicium's table has a working Windows GPU. The only ✅ entries are SM8150 and SM7125 phones, whose SoCs have a working Qualcomm Windows driver set ([Mu-Silicium Status.md](https://github.com/Project-Silicium/Mu-Silicium/blob/8471e47f5248ebbffc2e6196b683204163efe155/Status.md)). qcfakepep's README claims it "lifts the PEP dependency from the GPU stack". Its code does no power work at all, so for real GPU operation that claim is almost certainly wrong ([qcfakepep.c](https://github.com/18934020489Xx/alioth-woa-drivers/blob/3e5e052b2de21fa63e7303a6e34b65f7796cd008/qcfakepep/qcfakepep.c)).

### What the Qualcomm route would take

Getting `qcdx8250` running on apollo would take all of these:

1. A PEP that really powers the GPU and display: qcpep7280 in normal mode, or route E.
2. Six more Qualcomm devices restored to the DSDT, each with its own hang risk. qcsmmu8250 hung at the logo on alioth.
3. A J3S panel description. Silicium's vayu XML (an NT36672C video-mode panel with DSC 1.1 in 540×20 slices) is a close template, and Xiaomi's `dsi-panel-j3s-37-02-0a-dsc-video.dtsi` has every timing and init command ([vayu DSDT](https://github.com/Project-Silicium/Silicium-ACPI/blob/f50bc115dccdea12e44efb550fd783d1daafc739/Platforms/Xiaomi/vayu/Decompiled/DSDT.dsl), [J3S dtsi](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/apollo-q-oss/arch/arm64/boot/dts/venor/qcom/dsi-panel-j3s-37-02-0a-dsc-video.dtsi)).
4. Luck that an 8180-branch driver handles the A650 and its secure microcode path on a production Xiaomi TrustZone.

**Inference:** this is months of work with low odds. Nobody has published a working result on any SM8250 phone.

### Alternatives without Qualcomm's driver

The hardware itself is not the obstacle. ios-apollo brought up the A650 under XNU (GMU, GX rail, SMMU, SQE microcode, TrustZone zap shader) and ran Mesa Turnip on it. But writing a Windows render driver means a WDDM kernel driver plus a D3D user-mode driver and shader compiler, which is person-years of work.

A **display-only WDDM driver** built on Microsoft's KMDOD sample is achievable in your CI. The sample targets exactly a UEFI-framebuffer device ([KMDOD README](https://github.com/microsoft/Windows-driver-samples/blob/main/video/KMDOD/README.md)). Combined with ios-apollo's WLED code, it would give brightness control and perhaps rotation and tear-free flips, but no 3D. The research notes estimate 2–6 weeks. The KMDOD README warns that the sample bugchecks if put to sleep.

Until then, Windows renders with WARP on the CPU. Your 1800 MHz cap, needed because there are no thermal zones yet, also caps WARP. Turning off animations and transparency is the free win.

## HDMI is closed in hardware; DisplayLink is the only wired screen

### Why USB-C video cannot work

**Verified in Xiaomi's kernel source and in a live apollo device tree:**

- The primary USB controller runs at `maximum-speed = "high-speed"` with a no-op PHY in place of the USB3/DP combo PHY.
- The combo PHY (`usb_qmp_dp_phy`) is `disabled`.
- The DP controller (`sde_dp`) is `disabled`.
- The display pipeline lists no DP connector.
- The live boot arguments carry `androidboot.dp=0x0`.

The same configuration covers Xiaomi's whole 2020 SM8250 line ([MiCode apollo-q-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/apollo-q-oss), [edk2-msm apollo.dtb](https://github.com/edk2-porting/edk2-msm)). Physical routing of the SuperSpeed pins is not proven by a schematic, but nothing in Xiaomi's firmware supports DP alt mode. On other phones, Windows DP output would need the PEP-bound Qualcomm display driver plus the Type-C/PD stack on the ADSP, so it is moot twice over. USB-C-to-HDMI cables, "multiport" USB-C hubs and USB-C monitors will not work on apollo under any OS.

### The route that works: DisplayLink

The working route is a **DisplayLink (Synaptics DL-3xxx/5xxx/6xxx) USB graphics adapter**. Synaptics' Windows package has supported ARM64 since 11.3. The current 12.2 M2 (June 2026) lists ARM64 and fixes "image distortion on select ARM64-based systems" ([Synaptics 12.2 M2 notes](https://drivers.synaptics.com/sites/default/files/release_notes/2026-07/DisplayLink%20USB%20Graphics%20Software%20for%20Windows12.2%20M2-Release%20Notes.txt)). It is an indirect display driver that renders through the system's adapter and falls back to WARP ([Microsoft IDD overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview)), so it should run without a GPU driver.

**Nobody has reported it on a WOA phone, so treat it as a test.** Expect a usable 1080p office screen over USB 2.0, with stutter on video and scrolling. The CPU will likely be as much of a bottleneck as the bus.

### Setup and what to avoid

Power role matters, by inference: no Qualcomm PD stack runs under Windows on apollo. A dock that tries to charge the phone may flip it into USB device mode or fail to negotiate. Start with a small DisplayLink adapter behind a **self-powered hub that does not back-feed the phone**.

Avoid non-DisplayLink "USB display" chips (Silicon Motion, MacroSilicon) and legacy DL-1x5 adapters; none has a current ARM64 Windows driver ([Plugable SMI](https://plugable.com/pages/siliconmotion), [DisplayLink OS support](https://support.displaylink.com/knowledgebase/articles/543910)). Miracast needs a Wi-Fi driver with Wi-Fi Direct. Remote Desktop over USB Ethernet works today on Windows 11 Pro.

## Recommended order: fallbacks this week, Wi-Fi next, PEP as a side bet

The order follows two rules: do what needs no debugger first, and do work whose groundwork carries over to later features. Your registry-status pattern for drivers (`docs/handoff.md`) avoids debugger loops. PEP experiments are the opposite: every boot needs kd and reveals one more address. Effort figures are estimates from the research notes, not measurements.

| # | Work | Unlocks | Effort | Proven vs guessed |
| --- | --- | --- | --- | --- |
| 1 | Buy and test: an AX88179 or RTL8153 USB Ethernet adapter, a powered USB-C hub, a UAC2 USB-C DAC, and a DisplayLink adapter with the 12.2 M2 ARM64 driver | Networking, audio and an external screen now | Hours | Ethernet proven on alioth; UAC2 inbox documented; DisplayLink on a no-GPU phone untested |
| 2 | Run `apollobt` (already built) | Bluetooth peripherals and A2DP audio | Days | BT audio proven on apollo under XNU; never run under Windows |
| 3 | Port the TSENS thermal governor (ios-apollo 0153) | Removes the 1800 MHz cap, which helps WARP and DisplayLink | Days | Listed as an open item in the handoff |
| 4 | Wi-Fi probe driver: rails, WLAN_EN, PCIe link; report the IDs, the ECAM register read-back and the MSI SPI range | Answers all four Wi-Fi unknowns | 1–3 sessions | Hardware sequence proven on apollo; Windows behaviour unknown |
| 5 | ECAM, then PCI0 in ACPI, then the SMMU context for 0x1C01, then `qcwlan6800sc8180` | Native Wi-Fi | Several weeks; 40–60% success (judgement) | Guessed |
| 6 | Side bet, time-boxed to about two boots each: your own logging stub PEP (no EPSS code, both interfaces), then qcpep7280 in stub mode | A record of what Qualcomm drivers ask for; maybe qcsmmu for PCIe DMA mapping; a first read on qcpep7280 | Days | Untested by anyone on SM8250 |
| 7 | Ask Silicium which pack makes alioth "Speakers ✅" | Possibly a shortcut to speakers | One message | Unknown |
| 8 | Own ADSP stage-1 driver ("DSP up and linked"), then TDM, CS35L41 and a miniport | Built-in speakers and earpiece; later sensors and microphones | Weeks to months | Proven under XNU on apollo; the Windows SMC question is open |
| 9 | KMDOD display-only driver plus WLED | Brightness, a WDDM display | 2–6 weeks | Sample exists; untested here |
| 10 | GPU via Qualcomm `qcdx` | 3D acceleration | Months; low odds | Unproven on any SM8250 device |

Steps 4–5 and step 8 share their hardest groundwork: RPMh votes (already in `apollobt`), programming SMMU contexts without qcsmmu, and TrustZone calls. Each one lowers the risk of the next. If the stub PEP in step 6 does get qcsmmu running, it could replace the hand-made SMMU context for both Wi-Fi and audio. That is the strongest reason to run the PEP experiment at all.

## Conclusion

"Fix qcpep" turns out to be the wrong unit of work. The four goals hang on four different things: a PCIe link for Wi-Fi, the ADSP for audio, a full power stack for the GPU, and missing wiring for HDMI. The PEP is the critical path only for the goal that is least reachable anyway. Every Windows port of this SoC has stalled at the same PEP wall, which means the evidence base for SM8250 Windows features is thinner than it looks. Each claim of progress (qcfakepep's README, Silicium's ✅) needs to be read as a hypothesis until someone publishes a log. apollo's real advantage is different: ios-apollo has already proven every hardware sequence for Wi-Fi, audio and the GPU on this exact phone. Porting proven sequences into small, registry-reporting Windows drivers has a known shape. Patching another chip's power driver does not.

The most consequential near-term fact is not about PEP at all. A stub PEP that merely satisfies qcsmmu could turn the SMMU, the shared hard problem behind both Wi-Fi DMA and ADSP audio, from hand-written code into a Qualcomm-maintained driver. If two boots of a logging stub show qcsmmu starting cleanly, the order above should shift to lean on it. If they don't, nothing is lost: the native Wi-Fi and audio plans never depended on it.
