# Wi-Fi under Windows 11 ARM64 on Xiaomi Mi 10T (apollo, SM8250, QCA6390 on PCIe0)

Research date: 2026-11-07. Repos were read at these commits: WOA-Project/Qualcomm-Reference-Drivers `ad6e472` (2026-03-21), WOA-Project/windows_silicon_qcom_kona `22221a0` (2026-01-04), Project-Silicium/Mu-Silicium `8471e47` (2026-11-05), Project-Silicium/Silicium-ACPI `f50bc11` (2026-10-29), zhishixuebao-0791/K40_Win11 `6316c56` (2026-10-23), 18934020489Xx/alioth-woa-drivers `3e5e052` (2026-10-28), torvalds/linux `d487237` (2026-11-06), and the local clone of aTechGeekDev/ios-apollo `1f75f38` (2026-11-05). Driver CABs were unpacked with cabextract and read as text (INF), PE import tables (pefile), and `strings`. Nothing was executed.

Labels used below: **[V]** = I checked it myself in the files named. **[C]** = what a source says, not checked. **[I]** = my inference.

Network limits: learn.microsoft.com and patchew.org could not be fetched in this session because they are not on its network allowlist. For Microsoft documents I only had search-result snippets.

## Q1. Which Windows ARM64 drivers support QCA6390 (PCI\VEN_17CB&DEV_1101), and what do they depend on?

### Takeaway
Only one public Windows driver binds QCA6390 over PCI: Qualcomm's `qcwlan6800sc8180` (SC8180X "CLS" reference package, latest 200.0.117.0, driver 1.0.2160.0, dated 06/22/2025). It is a self-contained ARM64 NDIS/WDI miniport. It carries its own MHI host, QMI client and IPC router. It imports only NDIS, ntoskrnl, NETIO, HAL and WDF, with no PEP, QCMS or qcppx dependency. It does need a working PCI stack under it: pci.sys with config space, BAR, MSI or line interrupt, and DMA. Kona's `qcwlan8250` cannot be used at all. It binds `QCMS\VEN_QCOM&DEV_252B` and ships `WLANMDSP.MBN`, both of which belong to the SM8150/SC8180X *integrated* WLAN design, a block SM8250 does not have.

### Cited Findings
- **[V] `qcwlan6800sc8180.inf` (8180_CLS/200.0.117.0)**:
  - Hardware IDs are only `PCI\VEN_17CB&DEV_1101&SUBSYS_010817CB&REV_00` and `...&REV_01`.
  - Descriptions: "Qualcomm QCA639x Wi-Fi 6 Dual Band Simultaneous (DBS) Network Adapter" and "Killer Wi-Fi 6 AX500-DBS".
  - `BusType = 5` (PCI); service `qcwlan` → `qcwlan6800sc81.sys`; class Net; includes `netvwifibus.inf`.
  - `.HW` section needs `PciASPMOptIn.HW`, `VWiFiBus.PnPFilterRegistration.HW`, `PciD3ColdSupported.HW`.
  - `MSI.reg`: `MSISupported=1`, `MessageNumberLimit=16`.
  - Firmware copied to `%12%` (System32\drivers): `wlanfw20.mbn`, `m3.bin`, `bdwlan.elf`, `bdwlan.e11`. Registry values `FWFileName`="wlanfw.mbn", `BDFileName`="bdwlan.elf", `M3Needed=1`.
  - `PnpLockDown=1`. No `_DEP`/PEP/QCMS reference anywhere in the INF. — [WOA-Project/Qualcomm-Reference-Drivers 8180_CLS](https://github.com/WOA-Project/Qualcomm-Reference-Drivers/tree/master/8180_CLS)
- **[V] `qcwlan6800sc81.sys` imports** (ARM64, machine 0xAA64): NDIS.SYS, ntoskrnl.exe, NETIO.SYS, HAL.dll and WDFLDR.SYS only. The calls include `IoGetDmaAdapter`, `IoConnectInterruptEx`, `NdisMMapIoSpace` and `NdisMRegisterScatterGatherDma`. No import from any Qualcomm driver. — same package
- **[V] Strings inside `qcwlan6800sc81.sys`** show a built-in MHI host, QMI client (QCCI) and IPC router. Examples: "MHI EXEC_ENV", "Exec env is AMSS", "QMI request failed", "Notified IPC Router CLOSE". Other strings:
  - "%s - no MSI support." (hints at a non-MSI path).
  - "chip_id/board_id/soc_id", "BDF file: %s", "FWFileName path + nameFromRegKey" (firmware path can be overridden in the registry).
  - "Use Mac Address in BDF / Registry table / random MAC". — same package
- **[V] Firmware in the package**: `wlanfw20.mbn` reports `QC_IMAGE_VERSION_STRING=WLAN.HST.1.0.1.c8-00628.2-QCAHSTSWPLZ_V2_TO_X86-1` (the host-PC build of the QCA6390 hw2.0 firmware). `bdwlan.elf` is byte-identical to `bdwlan.e11` (57,868 bytes). — same package
- **[V] `qcwlan8180.inf` (SC8180X, also the Surface Pro X 8180_CAM package)** binds only `QCMS\VEN_QCOM&DEV_042B[&SUBSYS_...]` with `BusType=15` and firmware `WLANMDSP.MBN`/`bdwlan.bXX`. This is SC8180X's integrated WLAN, not QCA6390. — [Qualcomm-Reference-Drivers 8180_CLS / Surface/8180_CAM](https://github.com/WOA-Project/Qualcomm-Reference-Drivers/tree/master/Surface)
- **[V] `qcwlan8280.inf` (SC8280XP QRD 200.0.68.0)** binds `PCI\VEN_17CB&DEV_1103` (WCN685x / QCA6490 "Wi-Fi 6E ... WiFiCx"), not DEV_1101. It is a PCI driver with the same shape (`BusType=5`, `PciASPMOptIn`, `PciD3ColdSupported`). — [Qualcomm-Reference-Drivers 8280_QRD](https://github.com/WOA-Project/Qualcomm-Reference-Drivers/tree/master/8280_QRD)
- **[V] Kona `qcwlan8250.inf` (1.0.2140.0, 10/30/2024)** binds only `QCMS\VEN_QCOM&DEV_252B&SUBSYS_{QRD,CDP,MTP,HDK,RUMI}x8250` and copies `wlanmdsp.mbn`, `data.msc` and `bdwlan.bin`. — [windows_silicon_qcom_kona Drivers/WLAN](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/main/Drivers/WLAN)
- **[V] The Kona DSDT's WLAN device is the SM8150-style integrated block.** In the pipa DSDT (Qualcomm Kona DSDT as shipped by Silicium), `\_SB.AMSS.QWLN` has:
  - `_DEP` on `PEP0` and `MMU0`;
  - memory `0x18800000` (size 0x800000), `0x0C250000`, `0x8BC00000`;
  - SPIs 0x1BE onward. — [Silicium-ACPI Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl)

  In Linux, `wifi@18800000` (`qcom,wcn3990-wifi`) exists in `sm8150.dtsi` but not in `sm8250.dtsi`. — [linux sm8150.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8150.dtsi), [linux sm8250.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi)
- **[V] `MhiHost.inf`** (Surface 8280_ARC, 18.52.1.3) binds only modem IDs `PCI\VEN_17CB&DEV_0300..0308` (MDM9x35…SDX65). It does not bind DEV_1101. The QCA6390 driver does not need it, because its MHI is built in. — [Qualcomm-Reference-Drivers Surface/8280_ARC_1996](https://github.com/WOA-Project/Qualcomm-Reference-Drivers/tree/master/Surface)
- **[V] Repo inventory**: across all platforms in Qualcomm-Reference-Drivers, the only package that names DEV_1101 is `qcwlan6800sc8180` (77 versions under 8180_CLS). Other WLAN packages target 7180/7280/8280/8380 (WCN6855/WCN785x/WPSS) or 850/8998 (SNOC). — [Qualcomm-Reference-Drivers](https://github.com/WOA-Project/Qualcomm-Reference-Drivers)
- **[V] Apollo's own firmware** sits on the phone's `modem` partition under `image/qca6390/`: `amss20.bin`, `m3.bin`, `regdb.bin`, and board files for several Xiaomi phones (apollo's is `bd_j3sgl.elf`). ios-apollo's `tools/pull-wifi-fw.sh` extracts them read-only. — [ios-apollo tools/pull-wifi-fw.sh](https://github.com/aTechGeekDev/ios-apollo/blob/main/tools/pull-wifi-fw.sh)

### Inferences
- [I] `qcwlan6800sc8180` is the only realistic Qualcomm driver for apollo's radio.
  - Its INF matches only SUBSYS_010817CB, so if apollo's QCA6390 reports another subsystem ID, the INF needs an added line plus re-cataloguing and test-signing (the project already test-signs).
  - Apollo's subsystem ID is unknown. ios-apollo only checks vendor/device `0x110117cb`.
- [I] The board-data naming (`bdwlan.e11`, and the log "chip_id … board_id") suggests the driver loads `bdwlan.e<board_id>` and falls back to `bdwlan.elf`. Replacing `bdwlan.elf` (and/or `bdwlan.eXX` for apollo's board ID) with apollo's `bd_j3sgl.elf` should give the right RF calibration. The SC8180X/Killer board file would probably work but with wrong calibration and wrong regulatory/SAR tables.
- [I] Apollo's Android firmware (`amss20.bin`, mobile WLAN.HST build) and the Windows `wlanfw20.mbn` (…`_TO_X86` build) belong to the same family. The Windows driver should use its own `wlanfw20.mbn`/`m3.bin`, because QMI/WMI versions must match the driver.
- [I] The driver asks for `PciD3ColdSupported`/ASPM, which suggests it expects ACPI power resources around the root port. With no PEP, power resources can be no-ops (as in the Kona DSDT, see Q2), provided the chip is powered before enumeration.

### Gaps
- Apollo's QCA6390 PCI subsystem ID and board ID are unknown. They can be read in the first staged step (config dword 0x2C; the QMI `chip_id/board_id` print).
- I did not disassemble whether the driver falls back to a line interrupt when MSI is unavailable. Only the "no MSI support" string was seen.
- I did not check whether any other OEM (Lenovo, Samsung, HP) package carries a newer or different DEV_1101 driver outside this repo. Windows Update catalog searches were not possible here.

## Q2. Does inbox pci.sys work with SM8250's PCIe controller, or is Qualcomm's qcppx required? How do Qualcomm Windows designs expose PCIe?

### Takeaway
Qualcomm's own Windows design for Kona (SM8250) uses the inbox pci.sys:
- PCI0 is `PNP0A08`/`PNP0A03`, with MCFG ECAM at 0x60000000 for segment 0;
- IORT maps root-complex RIDs to apps-SMMU streams 0x1C00+;
- MADT has a GICv2m-style MSI frame at 0x17A10000.

`qcppx` is **not** a PCI bus driver. It is an ACPI PEP plug-in (`PoFxRegisterPlugin`) that waits for qcpep's interface. It only gates and power-manages PCI0 through `_DEP`. qcppx has no link-training code, so the link and ECAM must already be up before pci.sys runs; Qualcomm's own UEFI does this on its reference designs.

Silicium's Kona UEFI has no PCIe init driver, but it still forces `PRP0=1`. renegade-apollo's DSDT script currently deletes PCI0/PCI1.

Conclusion: pci.sys can work **without PEP**, provided something brings up PCIe0 in ECAM mode and the ACPI/IORT/MADT descriptions match. That "something" can be a UEFI DXE or an early Windows driver that PCI0 `_DEP`s on.

### Cited Findings
- **[V] Kona MCFG (Silicium-ACPI `Silicon/Qualcomm/Kona/MCFG.aml`, OEM "QCOM"/"QCOMEDK2", rev 0x8250)**:
  - segment 0, buses 0–0x0F, ECAM base `0x60000000` (PCIe0 DBI);
  - segment 1 at `0x40000000`, buses 0–0xFF;
  - segment 2 at `0x64000000`, buses 0–0xFF.
  - The stored checksum is 0 ("should be C9"). — [Silicium-ACPI Kona/Decompiled/MCFG.dsl](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Silicon/Qualcomm/Kona/Decompiled/MCFG.dsl)
- **[V] Kona IORT** has three PCI root-complex nodes (segments 0/1/2), each with 6 ID mappings to the SMMU node at offset 0x30 (SMMUv2 base `0x15000000`). Segment 0 output bases are `0x1C00`, `0x1C04`, `0x1C08`, `0x1C10`, `0x1C20`, `0x1C40`. — [Silicium-ACPI Kona/Decompiled/IORT.dsl](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Silicon/Qualcomm/Kona/Decompiled/IORT.dsl)
- **[V] Kona MADT (`APIC.aml`)**, parsed by me:
  - GICD `0x17A00000` (GICv3), GICR `0x17A60000`, **no ITS**;
  - one **GIC MSI Frame**: id 0, base `0x17A10000`, flags 0x1 (SPI override), SPI count 128, SPI base 832. — [Silicium-ACPI Kona/APIC.aml](https://github.com/Project-Silicium/Silicium-ACPI/tree/main/Silicon/Qualcomm/Kona)
- **[V] pipa/Kona DSDT `\_SB.PCI0`**:
  - `_HID PNP0A08`, `_CID PNP0A03`, `_SEG 0`, `_BBN 0`, `_CCA 1`;
  - `_STA` returns 0xF only if `PRP0 == 1`;
  - `_CRS`: Memory32Fixed `0x60200000` length `0x01DF0000`, buses 0–1;
  - `_PRT` INTA–D → GSIV 0xB5–0xB8;
  - standard `_OSC`;
  - child `RP1` with `_PR0/_PR3/_PRR` on power resources `P0RR`/`R0RR`, whose `_ON/_OFF/_RST` methods are **empty** and whose `_STA` returns 0;
  - `_DEP` on `\_SB.PEP0` and `\_SB.QPPX`.
  - `QPPX` is `_HID "QCOM05A2"`, `_CID "QCOMFFE2"`, with `_CRS` 0x01FEC004 (4 bytes), a `_QPG` method and `_RST` logic (PERST/GPIO). — [Silicium-ACPI pipa DSDT.dsl](https://github.com/Project-Silicium/Silicium-ACPI/blob/main/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl)
- **[V] `qcppx8250.inf`** binds `ACPI\QCOM25A2` as "Pep_Device". `qcppx8180.inf` binds `ACPI\QCOM04A2`, and `qcppx8280.inf` binds `ACPI\QCOM0696` and also sets `HKLM\...\Services\PCI\Parameters\BusIdleTimeoutOverride`. — [windows_silicon_qcom_kona Drivers/SOC/PCIe](https://github.com/WOA-Project/windows_silicon_qcom_kona/tree/main/Drivers/SOC/PCIe), [Qualcomm-Reference-Drivers 8180_CLS / 8280_QRD](https://github.com/WOA-Project/Qualcomm-Reference-Drivers)
- **[V] `qcppx8250.sys` imports** `PoFxRegisterPlugin`, `IoGetDeviceInterfaces`, `IoRegisterPlugPlayNotification` and `MmMapIoSpaceEx`. Its strings include `PEP_ACPI_*` handlers, `QCPepInterfaceInit`, `RegisterQCPepArrivalNotification`, `QCPepPnPCallback` and `\Device\RESOURCE_HUB\`. There are no PHY/LTSSM/ECAM strings. — Kona package (as above)
- **[V] Silicium's Kona `AcpiTableUpdate.c`** sets `PRP0 = 1` unconditionally (PRP1 = 0, PRP2 from platform subtype). The alioth/pipa DXE lists contain no PCIe driver, only `XhciPciEmulationDxe`. — [Mu-Silicium KonaPkg AcpiTableUpdate.c](https://github.com/Project-Silicium/Mu-Silicium/blob/main/Silicon/Qualcomm/KonaPkg/Library/AcpiTableUpdateLib/AcpiTableUpdate.c), [aliothPkg DXE.inc](https://github.com/Project-Silicium/Mu-Silicium/blob/main/Platforms/Xiaomi/aliothPkg/Include/DXE.inc)
- **[V] renegade-apollo `uefi/apollo/make-apollo-dsdt.py`** drops `'PCI0', 'PCI1'` ("depend on QPPX (PCIe), which stays disabled"). — local file `/home/user/renegade-apollo/uefi/apollo/make-apollo-dsdt.py` line 34
- **[V] Linux `pcie-qcom.c`** (2026-11-06):
  - `qcom,pcie-sm8250` (and sm8150, sc8180x, sm8350, sm8450, sm8550, sc7280) all use `cfg_1_9_0`.
  - The driver defines `PARF_ECAM_BASE` 0x380/0x384 and `PARF_BLOCK_SLV_AXI_{WR,RD}_{BASE,LIMIT}[_HI]` 0x360–0x37C.
  - `qcom_pcie_init_outbound_blocker()` writes the DBI physical address to `PARF_ECAM_BASE` ("ECAM base must match the DBI base address") and opens the AXI block window. `OUTBOUND_BLOCKER_EN` is BIT(26) of `PARF_SYS_CTRL`.
  - A `firmware_managed` config creates a generic ECAM host. — [linux drivers/pci/controller/dwc/pcie-qcom.c](https://github.com/torvalds/linux/blob/master/drivers/pci/controller/dwc/pcie-qcom.c)
- **[V] Linux DWC core** (`pcie-designware-host.c`) has `dw_pcie_config_ecam_iatu()` and `dw_pcie_ecam_enabled()`. ECAM is used when the "config" region is large enough and aligned (bus 1 via an iATU CFG0 window, buses ≥2 via CFG1). — [linux pcie-designware-host.c](https://github.com/torvalds/linux/blob/master/drivers/pci/controller/dwc/pcie-designware-host.c)
- **[C] The ECAM patch series** (Qualcomm/Mani, v5 March 2025 → v9 September 2025) says any access to the root bus beyond device 0 function 0 "should not go out of the link and should return all F's", which is why the PARF AXI blocker exists. — [patchwork v5 7/7](https://patchwork.kernel.org/project/linux-pci/patch/20250309-ecam_v4-v5-7-8eff4b59790d@oss.qualcomm.com), [linux-arm-kernel v9 3/4](https://lists.infradead.org/pipermail/linux-arm-kernel/2025-September/1060572.html)
- **[V] Linux `sm8250.dtsi` pcie0**:
  - regs: parf `0x01c00000`, dbi `0x60000000`, elbi `0x60000f20`, atu `0x60001000`, config `0x60100000` (1 MiB);
  - ranges: I/O at 0x60200000, mem at `0x60300000` + 0x3d00000;
  - `iommu-map` RID 0x0 → SID 0x1c00 and RID **0x100 → 0x1c01**;
  - `dma-coherent`; DWC internal MSI (`msi0..msi7` SPIs starting at 141); INTx via SPI 149+; wake gpio 81. — [linux sm8250.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi)
- **[V] ios-apollo 0126 (`sweet_pcie.c`)** brings up apollo's PCIe0 directly:
  - PARF `0x01c00000`, QMP PHY `0x01c06000` (99-entry kona sequence), DBI `0x60000000`;
  - GCC pcie0 clocks/GDSC/refgen/clkref `0x8c004`;
  - PERST gpio79, CLKREQ gpio80, WAKE gpio81, WLAN_EN gpio20, SW_CTRL gpio124;
  - Gen2 x1 link; bridge bus 0→1 with a 1 MiB window at 0x60300000 (`DBI+0x20 = 0x60306030`);
  - BDF-to-SID at `PARF+0x2000`/`0x2c00`.
  - Rails, via RPMh: smpa4 1.8 V, smpa6 0.95 V (apollo digital+AON), smpa5 1.9 V, smpc8 1.35 V, ldoa9 1.2 V (PCIe 1p8), ldoa5 0.88 V (PCIe 0p9).
  - "MSI 0x17a10040 = SPI 768..", "SMMU stream 0x1c01".
  - Interrupts: "No interrupt is taken: MHI's rings are polled". — [ios-apollo xnu/patches/0126-apollo-wifi-pcie-mhi.patch](https://github.com/aTechGeekDev/ios-apollo/blob/main/xnu/patches/0126-apollo-wifi-pcie-mhi.patch)
- **[C] Windows' approach to ECAM quirks** (2016 Linaro thread): the Windows team "usually avoid[s] quirks", and where needed keys off MCFG OEM ID/revision. — [linaro-acpi list](https://lists.linaro.org/archives/list/linaro-acpi@lists.linaro.org/message/R3JXJEVNN64CCQ4F4ZC3PXM5RIVJLMBC)

### Inferences
- [I] **pci.sys does not need qcppx to enumerate.** Kona PCI0 is a standard ECAM host bridge, and qcppx is a PEP plug-in that only owns power/reset policy and gates PCI0 through `_DEP`. Replace `QPPX`/`PEP0` in PCI0's `_DEP` with nothing, or with our own bring-up device, and pci.sys should load on `PNP0A08`, provided config space at 0x60000000 behaves like ECAM when it starts.
- [I] **Qualcomm's MCFG choice (bus 0 at DBI, bus 1 at +1 MiB, MMIO starting at +2 MiB, `_CRS` bus range 0–1) implies SM8250's v1.9.0 controller supports ECAM mode in hardware** (`PARF_ECAM_BASE` + outbound blocker). Otherwise Qualcomm's own Windows tables would be unusable. This is strong but indirect evidence. Linux only enables ECAM for `cfg_1_9_0` when the DT config region spans ≥2 buses, and sm8250's DT config is 1 MiB.
- [I] Without the blocker (or with a wrong iATU), pci.sys's scan of devices 1–31 on bus 0 would read DBI aliases or send unsupported requests on the link. Expect phantom devices or a hang. The bring-up code must configure `PARF_ECAM_BASE`, the AXI blocker and the iATU CFG0 window for bus 1, and leave LTSSM up before pci.sys runs.
- [I] **MSI**: Qualcomm's Windows design routes PCIe MSI to the GIC MSI frame at 0x17A10000 (endpoint writes go to `0x17A10040` = SETSPI_NS), not to DWC's internal MSI controller, for which Windows has no driver. Windows evidently supports GICv2m-style MSI frames on this platform, since Qualcomm shipped the table. That matches ios-apollo's use of 0x17A10040. There is a discrepancy to check: ios-apollo says "SPI 768.." while the MADT frame says SPI base 832, count 128. Which SPIs actually fire must be checked on hardware.
- [I] **SMMU**: endpoint DMA and MSI writes carry SID 0x1C01, and on apollo an unmatched stream faults (ios-apollo had to install its own context). Without qcsmmu (which needs a PEP, or alioth's `qcfakepep`), a bring-up driver must give SID 0x1C01 either a bypass S2CR or an identity-mapped context that covers RAM and the MSI doorbell 0x17A10040. Otherwise Windows' HAL DMA (physical addresses) will fault. This, plus whether the hypervisor allows it, is the largest unknown. ios-apollo's read-back showed the hypervisor let it program a context.
- [I] **Where to bring up the link**:
  - (a) **UEFI DXE in Mu-Silicium**: port `sweet_pcie.c` + rails + WLAN_EN to EDK2 and run it before ExitBootServices. pci.sys then sees a ready device exactly as on Qualcomm reference designs. This is the cleanest option.
  - (b) **Early Windows driver**: binds a new ACPI device (e.g. `APLO0003`) on which PCI0 `_DEP`s; it does power + link + ECAM + SMMU, then lets PCI0 start. This is the same gating Qualcomm uses with QPPX, but in our own code. It is easier to iterate (no reflash), but a bug can hang boot unless guarded by a registry switch.
- [I] RP1's `_PR0/_PR3` power resources are empty and report `_STA=0` in the Kona DSDT. With `PciD3ColdSupported` in the WLAN INF, Windows may try D3cold. Making the power resources report "on" (`_STA=1`), or removing them, is probably needed so the radio is never powered down by Windows.

### Gaps
- I found no public report of any SM8150/SM8250/SC8180X *phone* port getting PCIe enumerated under Windows, with or without qcppx (searched GitHub/XDA via web search; nothing specific came back).
- I found no confirmation that SM8250's controller implements `PARF_ECAM_BASE`/AXI blocker at the Linux offsets. That needs hardware testing (write and read back `PARF+0x380`).
- Microsoft's Arm64 PCI/MCFG/GICv2m requirements (learn.microsoft.com) could not be fetched. Windows' support for the MADT GIC MSI frame is inferred from Qualcomm's table, not from Microsoft documentation.
- I did not find whether Qualcomm's Kona reference UEFI includes a PCIe DXE that could be lifted from a firmware dump. Silicium's alioth/pipa binaries do not.

## Q3. What did other SM8250 Windows ports achieve for Wi-Fi?

### Takeaway
None has working Wi-Fi under Windows as of November 2026.
- Silicium's status table marks WLAN ❌ under Windows for the Poco F3 (alioth), Xiaomi Pad 6 (pipa) and OnePlus 8T (kebab). Alioth's Linux column shows WLAN ✅ (mainline ath11k).
- The alioth engineering project uses a USB Ethernet adapter (ASIX AX88179) for networking.
- Its DSDT notes list PCI0/1 and QWLN among the 30+ devices that `_DEP` on PEP0.

### Cited Findings
- **[V] Mu-Silicium `Status.md`** (commit 2026-11-05):
  - Poco F3/Redmi K40/Mi 11X (alioth), Windows: WLAN ❌, Bluetooth ✅, Touch ✅, Speakers ✅, USB Host ✅, GPU ❌. Linux: WLAN ✅ (mainline kernel `nikroks/alioth`).
  - Xiaomi Pad 6 (pipa), Windows: WLAN ❌.
  - OnePlus 8T (kebab), Windows: WLAN ❌. — [Mu-Silicium Status.md](https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md)
- **[V] K40_Win11 Alioth-Engineering `driver-strategy-20260422.md`**: "Wired networking works: ASIX AX88179 USB 3.0 to Gigabit Ethernet Adapter"; "Use USB Ethernet for networking"; "Do not inject boot-start PMIC / PCIe / platform drivers". — [K40_Win11 notes/driver-strategy-20260422.md](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/driver-strategy-20260422.md)
- **[V] K40_Win11 `acpi-hid-phase17c-build-result-20261021.md`**: 31 DSDT devices `_DEP` on PEP0, including "QWLN … PCI0/1". `phase13-...-20261016.md` lists PEP0 device power configs, including `EWMD` (WLAN/BT) and `PEMD` (PCIe). — [K40_Win11 Alioth-Engineering/notes](https://github.com/zhishixuebao-0791/K40_Win11/tree/main/Alioth-Engineering/notes)
- **[C] alioth-woa-drivers `qcfakepep`**:
  - It answers the PEP0 interfaces `{55beca03-ba2c-4fbc-9efc-ee8eca56f0c7}` (device interface) and `{a0e1dd36-dfb9-4c0e-bbaa-78f0746be181}` (QueryInterface, size 0x28, v1) "with zero MMIO access".
  - It "unlocks the whole audio chain (SMMU → qcsubsys → ADSP)".
  - Its README says qcpep8250/qcpep8280 BugCheck 0x101 on SM8250. — [alioth-woa-drivers README](https://github.com/18934020489Xx/alioth-woa-drivers)
- **[V] ios-apollo (XNU, same phone)**: patches 0126–0135 give working QCA6390 Wi-Fi under XNU: scan, WPA2 join, auto-join, RSSI, on/off, Safari traffic (NOTES "State of Wi-Fi (2026-10-28)"). The approach was RPMh rails, PCIe0 PHY/link, its own apps-SMMU context for SID 0x1c01, polled MHI, BHI/BHIe firmware download, QRTR/WLFW QMI, and an ath11k port. It was ported from the Mi 10 (umi) HackiOS kernel. — [ios-apollo NOTES.md](https://github.com/aTechGeekDev/ios-apollo/blob/main/NOTES.md), [0126 patch](https://github.com/aTechGeekDev/ios-apollo/blob/main/xnu/patches/0126-apollo-wifi-pcie-mhi.patch)

### Inferences
- [I] Apollo would be the first SM8250 Windows port with native Wi-Fi. That means no reference implementation on the Windows side; the hardware side, though, is well proven (Linux ath11k on alioth/RB5; ios-apollo on apollo itself).
- [I] qcfakepep might let `qcppx8250` load, since qcppx waits for qcpep's arrival interface. Even then qcppx does no link training (Q2), so it adds nothing essential. It could, however, make `qcsmmu` usable for PCIe DMA mapping through IORT, which would remove the need for a hand-made SMMU context. That is unverified.

### Gaps
- I found no Windows Wi-Fi status for Mi 10/umi, OnePlus 8/instantnoodle or other SM8250 devices outside Silicium's table. edk2-msm's SM8250 devices never got past stub DSDTs (per renegade-apollo `mu/README.md`). The edk2-porting issues were not searched.
- The 18934020489Xx/alioth-woa-drivers repo has no Wi-Fi or PCIe work, so no data point there.

## Q4. Practical fallbacks while native Wi-Fi is unavailable

### Takeaway
1. **USB Ethernet** is the proven choice: AX88179 works on alioth under Windows, and apollo's USB host mode works.
2. **USB tethering from another phone** should also work through Windows' inbox RNDIS or UsbNcm class drivers, which are architecture-neutral inbox drivers, depending on what the phone exposes.
3. **A travel router** in client/repeater mode feeding a USB Ethernet adapter turns any Wi-Fi network into wired networking.
4. **USB Wi-Fi dongles** are the weakest option: community reports (2024–2025) say essentially no retail USB Wi-Fi adapter ships Windows ARM64 drivers.

### Cited Findings
- **[V] alioth (SM8250, Windows)**: USB keyboard/mouse "work when the hub is externally powered"; "ASIX AX88179 USB 3.0 to Gigabit Ethernet Adapter" works. — [K40_Win11 driver-strategy-20260422.md](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/driver-strategy-20260422.md); same in [next-steps-priority-20260422.md](https://github.com/zhishixuebao-0791/K40_Win11/blob/main/Alioth-Engineering/notes/next-steps-priority-20260422.md) ("continue using powered hub + USB Ethernet").
- **[C] Microsoft's "supported USB classes" page** lists `UsbNcm.sys`/`UsbNcm.inf` for CDC subclass 0Dh (NCM), with Windows 11 / Server 2022 in the support column. The driver source is public as NCM-Driver-for-Windows. — [Microsoft Learn: supported USB classes](https://learn.microsoft.com/windows-hardware/drivers/usbcon/supported-usb-classes) (search snippet only; the page could not be fetched), [NCM-Driver-for-Windows mirror](https://gitee.com/mirrors_microsoft/NCM-Driver-for-Windows)
- **[C] Pixel 6 and later phones** use CDC-NCM rather than RNDIS for USB tethering, and Windows 10/11 include a built-in UsbNcm host driver. This is a user forum post, not authoritative. — [nothing.community thread](https://nothing.community/d/57757-migrate-usb-tethering-from-rndis-to-cdc-ncm-for-native-macos-windows-support)
- **[C] Windows' `usbrndis6` (RNDIS) driver** is "readily available on all architectures of Windows — x86, x64, and ARM64". Claim made by a DIY Pico W "driverless USB Wi-Fi" project. — [Driverless USB WIFI for Windows … Pi Pico W](https://structables.lunar.icu/Driverless-USB-WIFI-for-Windows-on-Raspberry-Pi-R-)
- **[C] Conflicting report**: one Microsoft Q&A thread says that for Windows IoT on ARM, "RNDIS/Ethernet Gadget isn't compatible … no driver available". That concerns IoT Core, not Windows 11 desktop. — [Microsoft Q&A 606352](https://learn.microsoft.com/en-us/answers/questions/606352/rndisethernet-gadget-driver-on-windows-arm64.html)
- **[C] USB Wi-Fi on Windows ARM64**:
  - A TP-Link community thread concluded (October 2025) that no TP-Link or other USB Wi-Fi adapter works with Windows 11 ARM. — [TP-Link community 846508](https://community.tp-link.com/en/home/forum/topic/846508)
  - Parallels users report "no USB Wireless adapters with Windows ARM64 drivers"; ASUS USB-BE92 Nano and N-53 failed. — [Parallels forum 368139](https://forum.parallels.com/threads/anybody-get-a-usb-wifi-adapter-to-work.368139/)
  - The WoR (Windows on Raspberry Pi) driver list shows no ARM64 Wi-Fi. — [worproject/RPi-Windows-Drivers](https://github.com/worproject/RPi-Windows-Drivers)
- **[C] Realtek's USB *Ethernet* driver package includes ARM64 drivers** (RTL8152/8153 family), per forum summary. — [TP-Link community 846508](https://community.tp-link.com/en/home/forum/topic/846508)
- **[V] Apollo project state**: USB host mode works under Windows. The handoff already lists "USB Ethernet / USB tethering (USB host works)" as the interim plan. — `/home/user/renegade-apollo/docs/handoff.md` ("Wi-Fi (researched, not started)")

### Inferences
- [I] **Recommended interim setup**:
  - a USB-C hub with Power Delivery passthrough (alioth needed external hub power);
  - plus an AX88179- or RTL8153-based gigabit adapter;
  - or, wireless in practice: a pocket travel router (any OpenWrt/GL.iNet-class) in repeater/WISP mode with its LAN port cabled to that adapter.
- [I] **USB tethering**: an Android phone exposing RNDIS should bind the inbox "Remote NDIS based Internet Sharing Device". One exposing NCM (Pixel 6+) should bind UsbNcm on Windows 11 24H2. Neither needs third-party drivers. The donor phone supplies the radio.
- [I] A Pico W running RNDIS firmware is a cheap DIY Wi-Fi bridge that needs no driver, but its throughput is low (USB full-speed).
- [I] Do not rely on buying a USB Wi-Fi dongle. If one is tried anyway, MediaTek MT7921AU/MT7612U or Realtek RTL88x2BU parts are the only ones with any chance of ARM64 INF support, but I found no confirmation.

### Gaps
- I found no first-hand confirmation of a specific USB Wi-Fi chipset with a Windows 11 ARM64 driver, and no inbox ARM64 Wi-Fi class driver.
- Whether apollo's USB-C port can power a hub/adapter without external power (PD source capability under Windows) is untested. alioth needed a powered hub.

## Q5. Routes, PEP dependency, effort and risk

### Takeaway
There are four native routes and one fallback route.
- Every native route that works **avoids PEP**. The only PEP-dependent Qualcomm pieces are qcppx8250 (not needed) and qcwlan8250 (wrong hardware).
- The most promising native route is **(A)**: our own PCIe0 bring-up (UEFI DXE or early Windows driver) in ECAM mode, plus restored/edited ACPI PCI0 + MCFG/IORT/MADT, plus inbox pci.sys, plus Qualcomm's `qcwlan6800sc8180` with apollo's board file. Its main risks are ECAM/blocker behaviour, SMMU stream 0x1C01, and MSI delivery.
- A fully custom Windows Wi-Fi driver (porting ios-apollo's whole stack) is a fallback of months.
- USB Ethernet or tethering works today.

### Cited Findings
- **[V] PEP dependency matrix** (from INF/imports/DSDT, see Q1/Q2):

  | Component | Needs PEP? |
  | --- | --- |
  | `qcppx8250` | Yes: PoFx plug-in, waits for qcpep arrival |
  | `qcwlan8250` | Effectively yes: QCMS child of the subsystem/AMSS stack; QWLN `_DEP` PEP0+MMU0; targets the absent integrated WLAN |
  | `qcsmmu` | Yes, per alioth notes (or qcfakepep) |
  | pci.sys | No (inbox) |
  | `qcwlan6800sc81.sys` | No: imports only NDIS/ntoskrnl/HAL/NETIO/WDF |
  | Own drivers | No |

  — sources in Q1/Q2
- **[V] Size of the proven XNU implementation**:
  - 0126 (rails, PCIe, MHI, staged Wi-Fi bring-up) is 2,535 patch lines;
  - 0127 (WLFW handshake) 2,198 lines;
  - 0128 (ath11k en0) 3,407 lines;
  - 0129–0135 add the network stack/MAC and fixes.

  This is the scale of work for route C. — [ios-apollo xnu/patches](https://github.com/aTechGeekDev/ios-apollo/tree/main/xnu/patches)
- **[V] Prior art for the project's own style**: renegade-apollo already runs its own Windows drivers that touch hardware directly (touch, cpufreq, BT with RPMh votes and the shared QCA6390 rails smpa6/smpa5/smpc8). — `/home/user/renegade-apollo/docs/handoff.md` ("Own Windows drivers")

### Inferences
- [I] **Route A, native without PEP (recommended native path).** Ordered steps:
  1. **Probe driver** (as already offered to the user): rails (reuse apollobt's RPMh code; add smpa4, ldoa9, ldoa5) + WLAN_EN gpio20 + PCIe0 clocks/PHY/link (port `sweet_pcie.c`). Read 17cb:1101, **subsystem ID**, BAR0, and write/read back `PARF_ECAM_BASE` (0x380) and the blocker registers. Low risk; about 1–3 sessions; reuses ios-apollo code nearly 1:1.
  2. **ECAM + iATU config**: bus 1 CFG0 window at 0x60100000, MEM window 0x60300000, outbound blocker. Then dump config through the ECAM window from the driver to confirm bus 0 dev 1–31 read all-Fs. Medium risk (undocumented on v1.9.0).
  3. **ACPI**:
     - restore `PCI0` in `make-apollo-dsdt.py`, with `_DEP` changed to our bring-up device (or dropped if a UEFI DXE does the bring-up);
     - make the RP1 power resources report on;
     - `_CRS` MMIO consistent with the iATU MEM window (Qualcomm: 0x60200000/0x1DF0000; Linux: 0x60300000/0x3D00000);
     - ship Kona MCFG (fix its checksum), IORT (already in Silicium's Kona set) and MADT MSI frame (already in Silicium's APIC).
     Medium risk: a bad PCI0 can hang boot, so keep a build switch like `BT=0`.
  4. **SMMU**: bypass or identity context for SID 0x1C01 (ios-apollo's context code), or try qcsmmu8250 with qcfakepep. High uncertainty: the hypervisor may refuse, though ios-apollo succeeded with a context.
  5. **Driver**: `qcwlan6800sc8180` 200.0.117.0 with the INF edited if the subsystem differs, `bd_j3sgl.elf` placed as `bdwlan.elf`/`bdwlan.eXX`, test-signed. MSI via the GIC frame, or the driver's non-MSI path.

  Overall effort: several weeks of on-device iteration. Probability of full success: moderate (my guess 40–60%). The radio and link are proven on this exact phone; the Windows-specific glue (ECAM, MSI frame, SMMU, Windows firmware on a phone board) is not.
- [I] **Route A′ (UEFI-side bring-up)**: the same as A, but steps 1–2 run as an EDK2 DXE in the local Mu-Silicium checkout. Advantage: pci.sys sees the device exactly as on Qualcomm reference hardware, with no Windows-side ordering problem. Costs: every iteration needs a UEFI rebuild + `fastboot boot` (tethered boot is already the workflow), and the rails/WLAN_EN must stay on after ExitBootServices. Effort is similar to A. It is a good final form once A's register sequence is proven from Windows.
- [I] **Route B (Qualcomm stack with PEP: qcpep/qcfakepep + qcppx8250 + qcwlan8250)**: not viable. qcwlan8250 targets hardware SM8250 does not have (Q1), and qcppx does no link-up (Q2). PEP is not on the critical path for Wi-Fi at all. The only PEP-adjacent piece that could help is qcsmmu via qcfakepep, for DMA remapping.
- [I] **Route C (fully custom Windows Wi-Fi driver)**: port ios-apollo's PCIe + MHI + QMI/WLFW + ath11k data path into a WiFiCx/NDIS miniport, bypassing pci.sys and Qualcomm's binary.
  - Effort is very high: months, about 8,000+ lines plus WiFiCx/WDI plumbing, WPA supplicant integration with Windows' native Wi-Fi stack, and power management.
  - Licensing matters: ath11k is BSD-3-Clause/ISC (permissive), while the ios-apollo/HackiOS code is APSL.
  - Consider only if pci.sys cannot be made to work (e.g. ECAM unusable).
  - A middle variant, **C2**, is a custom bus driver that enumerates a fake-PCI PDO (resources + PCI bus interface) for `qcwlan6800sc81.sys`. That avoids pci.sys and ECAM, but must emulate config space, MSI or line interrupts, and DMA; it is hacky and fragile, medium-high effort.
- [I] **Route D (USB networking)**: available now, near-zero effort and low risk. Buy an AX88179/RTL8153 adapter and a powered USB-C hub, or tether from another phone (RNDIS/NCM inbox), or use a travel router in client mode. This should be the daily-driver answer until A works.
- [I] **Rule reminder**: Wi-Fi firmware (Qualcomm's `wlanfw20.mbn`/`m3.bin`, and apollo's `bd_j3sgl.elf`/`amss20.bin`) must never be committed. Installers should fetch or copy them on the device, as with the touch and BT firmware.

### Gaps
- No measurement exists yet of whether `PARF_ECAM_BASE`/blocker registers exist on SM8250, whether the hypervisor permits a bypass S2CR for SID 0x1C01, which SPI range the GIC MSI frame actually raises for PCIe0 (768+ vs 832+), or apollo's QCA6390 subsystem/board ID. All four are answerable by the staged probe driver.
- The success probability and week estimates above are my judgement from the evidence, not measured.
