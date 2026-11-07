# HDMI / external display from Xiaomi Mi 10T / 10T Pro (apollo, SM8250) under Windows 11 ARM64

Researched 2026-11-07. Network note: in this session WebFetch could not reach gsmarena.com, notebookcheck.net,
synaptics.com, drivers.synaptics.com or kb.plugable.com (hosts not allowed), so those are cited from search-engine
snippets only. GitHub was reachable; the device-tree findings below come from primary sources: Xiaomi's own kernel
source and a live apollo DTB, parsed locally.

Labels used: **[Verified]** = checked directly in primary source/data in this session; **[Claim]** = stated by a
source but not independently checked; **[Inference]** = my reasoning.

## 1. Hardware: does apollo's USB-C port support DisplayPort Alternate Mode?

### Takeaway
No, not in any shipped configuration. Xiaomi runs apollo's primary USB controller as **USB 2.0 only**. The
SM8250 USB3/DP QMP combo PHY is replaced by a no-op PHY and set to `disabled`, the DP controller (`sde_dp`) is
`disabled`, and the MDSS display pipeline has no DP connector. Spec databases list the port as "USB Type-C 2.0, OTG".
Wired DP-alt-mode HDMI (USB-C to HDMI cables, DP-alt docks) does not work on this phone under Android, and it
cannot work under Windows either.

### Cited Findings
- **[Verified] Xiaomi kernel source, apollo-q-oss** (`github.com/MiCode/Xiaomi_Kernel_OpenSource`, branch
  `apollo-q-oss`, sparse-cloned): `arch/arm64/boot/dts/vendor/qcom/xiaomi-sm8250-common.dtsi` (Xiaomi's tree spells
  the path `venor/`) lines ~400-412 —
  ```
  &usb0 { dwc3@a600000 { usb-phy = <&usb2_phy0>, <&usb_nop_phy>; maximum-speed = "high-speed"; }; };
  &usb_qmp_dp_phy { status = "disabled"; };
  &usb1 { extcon = <&extcon_usb1>; status = "disabled"; };
  ```
  Qualcomm's base `kona-usb.dtsi` in the same tree has `usb-phy = <&usb2_phy0>, <&usb_qmp_dp_phy>;
  maximum-speed = "super-speed-plus";` (line 86/94). So Xiaomi deliberately drops SuperSpeed and the DP combo PHY.
  — [MiCode/Xiaomi_Kernel_OpenSource apollo-q-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/apollo-q-oss)
- **[Verified]** Same tree, `apollo-sm8250.dtsi` line 225: `&sde_dp { status = "disabled"; };`, and
  `&mdss_mdp { connectors = <&sde_wb &sde_dsi &sde_dsi1 &sde_rscc>; };`. Only DSI and writeback connectors, no DP.
  — [MiCode apollo-q-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/apollo-q-oss)
- **[Verified]** The same `xiaomi-sm8250-common.dtsi` is included by umi (Mi 10), cmi (Mi 10 Pro), cas (Mi 10
  Ultra) and lmi (K30 Pro / Poco F2 Pro). Each of those device dtsi files also has its own `&sde_dp` override, and
  none overrides `&usb0` back to SuperSpeed. So all of Xiaomi's 2020 SM8250 phones are USB 2.0 with DP disabled.
  — [MiCode apollo-q-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/apollo-q-oss)
- **[Verified] Live apollo DTB** (`edk2-porting/edk2-msm` `Platform/Xiaomi/sm8250/FdtBlob_compat/apollo.dtb`,
  copied into this session's scratchpad). It was dumped from a running phone: `/chosen/bootargs` carries a real serial,
  `androidboot.hwc=INDIA`, `androidboot.product.hardware.sku=pro`, panel `j3s_37_02_0a_dsc_video`, and
  `androidboot.dp=0x0`. Parsed with a small Python FDT parser:
  - `/soc/ssusb@a600000/dwc3@a600000`: `maximum-speed = "high-speed"`, `usb-phy = <hsphy@88e3000, usb_nop_phy>`
  - `/soc/ssphy@88e8000` (`qcom,usb-ssphy-qmp-dp-combo`): `status = "disabled"`
  - `/soc/qcom,dp_display@ae90000` (`qcom,dp-display`): `status = "disabled"`
  - `/soc/ssusb@a800000` (second controller) and `ssphy@88eb000`: `disabled`
  - `fsa4480@42` on i2c@884000 is present. It is the analog-audio/SBU switch for USB-C headsets (apollo has no
    3.5 mm jack). The disabled DP node references it as `qcom,dp-aux-switch`, which is inherited from Qualcomm's
    kona reference dtsi. `qcom,msm-ext-disp` is also present (base dtsi, unused).
  - The alioth (Poco F3) and lmi (Poco F2 Pro) DTBs in the same directory show the identical USB2-only / DP-disabled
    configuration.
  — [edk2-porting/edk2-msm](https://github.com/edk2-porting/edk2-msm)
- **[Claim]** GSMArena lists the Mi 10T Pro 5G as "USB Type-C 2.0, OTG". PhoneArena ("Type-C, USB 2.0"), IMEI.org
  and a Vodafone device guide agree. Teltarif lists "USB-C 3.0" and "Wireless HDMI", the only dissenting spec entry.
  — [GSMArena Mi 10T Pro](https://www.gsmarena.com/xiaomi_mi_10t_pro_5g-10437.php);
  [PhoneArena compare](https://www.phonearena.com/phones/compare/Xiaomi-Mi-10T-Pro,Xiaomi-Mi-10-Pro/phones/11513,11390);
  [Vodafone guide](https://deviceguides.vodafone.co.uk/xiaomi/mi-10t-pro-android-10-0/specifications);
  contradicted by [Teltarif](https://www.teltarif.de/smartphone/xiaomi/mi-10t-pro/)
- **[Claim]** On XDA, a user reports the sister phone Mi 10 Ultra (cas, same common dtsi) gives "no output on usb to
  hdmi connector". Another user suggests a DisplayLink USB-to-HDMI adapter (with the Android DisplayLink app), says it
  should work up to Full HD over USB 2.0, and says the adapter had to be powered.
  — [XDA thread](https://forum.xda-developers.com/t/xiaomi-mi-11-ultra-usb-type-version.4255487/post-84828279)
- **[Claim]** The SM8250 SoC itself does DP alt mode: upstream Linux enabled the PMIC Type-C port manager "found in
  e.g. SM8250, and hence RB5" plus Type-C DP altmode to get display working on the RB5 board.
  — [qcom arm64 defconfig for 6.7](https://salsa.debian.org/kimocoder/qcom-linux/-/tags/qcom-arm64-defconfig-for-6.7-2)

### Inferences
- The DT configuration is a software statement. Physical routing of the USB-C SuperSpeed pairs (TX1/RX1/TX2/RX2) on
  apollo's board is not proven by it. However, Xiaomi replacing the QMP PHY with `usb-nop-xceiv` across the whole
  2020 SM8250 line, the "USB 2.0" spec listings, and the absence of any user report of wired video out together make
  it very likely the SS lanes are not routed to the connector, or at least were never validated. Even if they were,
  nothing in Xiaomi's firmware/DT supports DP alt mode, so every route to it would be new bring-up work.
- The presence of `fsa4480`, `qcom,msm-ext-disp` and the `sde_dp_usbplug_cc` pinctrl nodes is **not** evidence of DP
  support. They are inherited from Qualcomm's kona reference DTSI. FSA4480 is needed anyway for USB-C analog audio.
- Wired-video consequence: USB-C-to-HDMI cables, "USB-C hubs with HDMI" (DP alt mode), USB-C monitors with
  DP-alt input, and MHL adapters will not work on apollo under any OS.

### Gaps
- No teardown, schematic or board photo was found that shows whether the USB-C SS pins are connected. The
  GSMArena/Notebookcheck review pages could not be fetched (host blocked), so their exact wording on USB/video is
  from search snippets only.
- The R (Android 11) `apollo-r-oss` branch no longer exists on MiCode (only `apollo-q-oss`). The live DTB (dumped
  from a later firmware) shows the same configuration, so this is very unlikely to have changed.

## 2. If native DP alt mode existed: what would Windows need? Status on other SM8150/SM8250 Windows ports

### Takeaway
This route is moot for apollo, because there is no DP alt mode in hardware/firmware. Even on DP-capable Snapdragon
devices, Windows DP output needs Qualcomm's full display driver (qcdx) plus the Type-C/altmode stack (PMIC GLINK /
UCSI through the ADSP) and Qualcomm's PEP. The project has already found that PEP cannot run on SM8250, so all of
these are out.

### Cited Findings
- **[Verified, project doc]** The project runs Windows with no Qualcomm PEP because qcpep cannot run on SM8250. Every
  Qualcomm driver that needs PEP (including GPU) is therefore unusable, and the desktop runs on the UEFI framebuffer
  only. — `/home/user/renegade-apollo/docs/handoff.md` ("Current state")
- **[Claim]** On Linux, DP alt mode on SM8250 (RB5) depends on the PMIC Type-C port manager driver plus the DP
  altmode driver, and a tester saw VDM Tx errors on first plug with two dongles, i.e. negotiation was fragile even
  upstream. — [qcom arm64 defconfig 6.7 tag](https://salsa.debian.org/kimocoder/qcom-linux/-/tags/qcom-arm64-defconfig-for-6.7-2)
- **[Claim]** SM8150 got upstream "DisplayPort and USB-C altmode" only on the SM8150-HDK dev board (Linaro patch
  series). — [lore.kernel.org sm8150-hdk display series](https://lore.kernel.org/all/3712b09c-53e4-418f-a2cd-79231dceea10@linaro.org/T/)

### Inferences
- On Qualcomm Windows laptops, DP alt mode works through qcdx8180/8280 (display) plus Qualcomm's UCSI/PMIC GLINK
  drivers running on the ADSP, all under qcpep. A custom driver would have to replace all of these: DP
  controller, DP PHY/PLL, Type-C mux/orientation, PD VDM negotiation, and a WDDM or IddCx display path. That is a very
  large project, and on apollo it has no target, because the PHY isn't wired up for DP.
- I found no Windows port of a Xiaomi SM8250 phone, or any other SM8250/SM8150 phone, that outputs DP over USB-C.

### Gaps
- No public status page was found for DP output in other WOA phone ports (e.g. OnePlus 8 / Xiaomi Pad 5). The search
  returned only Linux upstream work.

## 3. Practical routes: DisplayLink USB graphics, Miracast, remote desktop

### Takeaway
The only realistic wired route is a **DisplayLink USB graphics adapter or dock** (DL-3xxx / DL-5xxx / DL-6xxx
chipsets) running the official Synaptics DisplayLink Windows driver. That driver has supported **Windows 11 ARM64**
since release 11.3 (early 2024), and the current 12.2 M2 (June 2026) still lists ARM64. On apollo it will run over
**USB 2.0 (480 Mbit/s)** and render on the CPU (no GPU driver), so expect a usable 1080p office/desktop screen with
visible lag on motion, not smooth video. Miracast is blocked until a Wi-Fi driver with Wi-Fi Direct exists. Remote
Desktop over a USB Ethernet adapter can show the phone's desktop on another PC's monitor.

### Cited Findings
DisplayLink on Windows ARM64:
- **[Claim, primary via snippet]** DisplayLink 11.3 M0 release notes (2024): "EXE installer now supports Arm64
  platforms". DisplayLink for Windows on Arm64 is available in all packages except MSI.
  — [Synaptics 11.3 M0 notes](https://www.synaptics.com/sites/default/files/release_notes/2024-03/DisplayLink%20USB%20Graphics%20Software%20for%20Windows11.3%20M0-Release%20Notes.txt);
  [devicebase summary](https://devicebase.net/en/synaptics-displaylink-driver/updates/exe-installer-now-supports-arm64-platforms/47t)
- **[Claim]** Older (2021) notes supported Windows 10 on Snapdragon only through the INF package, not the EXE.
  — [Synaptics 8.4 M2 notes](https://www.synaptics.com/sites/default/files/release_notes/2021-02/DisplayLink%20USB%20Graphics%20Software%20for%20Windows8.4%20M2-Release%20Notes.txt)
- **[Claim]** 11.5 M2 (Oct 2024) added a separate ARM64 Ethernet driver and fixed MAC address pass-through on ARM64.
  11.7 M0 (Mar/Apr 2025) lists an ARM64 MSI installer and Windows 10/11 ARM64 as supported.
  — [Synaptics 11.5 M2](https://www.synaptics.com/sites/default/files/release_notes/2024-11/DisplayLink%20USB%20Graphics%20Software%20for%20Windows11.5%20M2-Release%20Notes.txt);
  [Synaptics 11.7 M0](https://www.synaptics.com/sites/default/files/release_notes/2025-04/DisplayLink%20USB%20Graphics%20Software%20for%20Windows11.7%20M0-Release%20Notes.txt).
  Plugable's KB says MSI still excludes ARM64 for 11.7+, which conflicts with the Synaptics notes. —
  [Plugable KB](https://kb.plugable.com/are-displaylink-drivers-available-for-windows-11-on-arm)
- **[Claim] Latest as of Nov 2026:** DisplayLink USB Graphics Software for Windows **12.2 M2**, package 12.2.2440.0
  (29 June 2026), graphics driver 12.2.2412.0 (19 June 2026). It installs on Windows 10/11 x64 and **ARM64**, and it
  fixes "image distortion on select ARM64-based systems" (VIFR-7564).
  — [Synaptics 12.2 M2 notes](https://drivers.synaptics.com/sites/default/files/release_notes/2026-07/DisplayLink%20USB%20Graphics%20Software%20for%20Windows12.2%20M2-Release%20Notes.txt);
  [Synaptics downloads](https://www.synaptics.com/displaylink/downloads/windows)
- **[Claim]** The legacy USB 2.0 chips DL-1x0/DL-1x5 are, per DisplayLink, supported by the open-source `udl`
  driver (Linux). Their product brief lists only Windows 7/10, and I found no evidence that the current Windows 10/11
  packages support them. — [DisplayLink OS support article](https://support.displaylink.com/knowledgebase/articles/543910);
  [DL-1x5 product brief](https://synaptics.com/sites/default/files/displaylink/assets/displaylink-1x5-series-product-brief.pdf)

USB 2.0 bandwidth / performance:
- **[Claim]** DisplayLink USB 3.0 chips are backward compatible with USB 2.0 hosts. E.g. a Plugable DL-3900 product is
  "Fully backward compatible with USB 2.0 PCs. USB 3.0 connection recommended for best performance." Belkin: on a USB
  2.0 port "the device will operate at USB 2.0 speeds". DisplayLink support staff use forcing USB 2.0 enumeration as
  a diagnostic, so 2.0 mode is a supported mode.
  — [DisplayLink support article 525038](https://support.displaylink.com/knowledgebase/articles/525038);
  [Belkin FAQ](https://www.manualslib.fr/manual/666225/Belkin-B2B043Eac00.html?page=15);
  [DisplayLink forum](https://www.displaylink.org/forum/showthread.php?p=90134)
- **[Claim, old, ~2009]** Tom's Hardware, quoting DisplayLink about first-generation hardware, wrote that USB 2.0
  "doesn't provide enough bandwidth to support resolutions above 1024x768" for smooth use. This is outdated for
  modern compressing chips, but it shows the 2.0 bottleneck.
  — [Tom's Hardware](https://www.tomshardware.com/reviews/usb-graphics-adapter%2C3006-2.html)
- **[Claim]** XDA user: a DisplayLink USB-to-HDMI adapter should do up to Full HD over USB 2.0, and the adapter needed
  external power on the phone. — [XDA](https://forum.xda-developers.com/t/xiaomi-mi-11-ultra-usb-type-version.4255487/post-84828279)
- **[Claim]** Microsoft IDD (IddCx) model: DisplayLink's Windows driver is an indirect display driver. The OS hands it
  DirectX surfaces. Before IddCx 1.4 it rendered via the POST adapter, falling back to **WARP** (software rasterizer)
  if that adapter is stopped, and IddCx 1.4 adds `IddCxAdapterSetRenderAdapter`.
  — [Microsoft IDD overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview);
  [IddCx 1.4 updates](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iddcx1.4-updates-for-console-and-remote-idds)

Other USB display chipsets:
- **[Claim]** Silicon Motion SM768 (InstantView) Windows driver v2.24 R01 (Sept 2025) is listed for Windows 10/11,
  and the SM768 datasheet lists only 32/64-bit x86 Windows. No ARM64 Windows driver was found.
  — [Plugable SMI drivers](https://plugable.com/pages/siliconmotion); [SM768 datasheet](https://wmsc.lcsc.com/wmsc/upload/file/pdf/v2/lcsc/2312032209_Silicon-Motion-SM768GX000000-AB_C5899711.pdf)

Wireless / remote:
- **[Verified, project doc]** Wi-Fi is "researched, not started". Qualcomm's Kona `qcwlan8250` needs the PEP/QCMS
  stack, and the lead is a QCA6390 PCIe driver from the 8180 package. USB Ethernet / USB tethering works now because
  USB host works. — `/home/user/renegade-apollo/docs/handoff.md`

### Inferences
- **DisplayLink should work on apollo's Windows install in principle.** apollo has working USB host mode, the
  DisplayLink driver ships ARM64 binaries, and IddCx can render through WARP when there is no GPU driver, as on apollo
  today where Windows already composes in software on the Basic Display/framebuffer. I found no report of DisplayLink
  on a no-GPU WOA phone, so this needs a test.
- **Expected performance** (inference): 480 Mbit/s raw, roughly 35-40 MB/s usable bulk. DisplayLink sends compressed
  deltas, so a mostly static 1920x1080 desktop (text, browser) should be usable. Full-screen video or scrolling will
  stutter. Encoding/compression runs on the CPU (apollo is currently capped at ~1.8 GHz with no thermal governor), and
  WARP composition also runs on the CPU, so the CPU is likely the bottleneck as much as USB 2.0. Keep a single
  monitor at ≤1080p60 and don't add other heavy USB traffic (e.g. USB storage) on the same port.
- **Power / USB-C role risk** (inference, important). Today host mode works with passive OTG setups, where the phone
  is Source + DFP. Many USB-C DisplayLink docks with their own power supply also try to charge the host (they act as
  power Source, i.e. default data role DFP). That requires a PD Data Role Swap so the phone becomes host. On apollo
  under Windows there is no Qualcomm UCSI/PMIC GLINK/PD stack running (Microsoft's URS driver owns the port, URS0), so
  a charging dock may make the phone enumerate as a USB device instead of host, or not negotiate at all. Safer
  options: (a) a small bus-powered DisplayLink USB-to-HDMI adapter (USB-A or USB-C plug) through a USB-C OTG
  adapter/hub, if the phone's VBUS supply holds up; or (b) a DisplayLink adapter behind a **self-powered USB hub
  that does not back-feed power to the phone** (USB-A upstream via an OTG adapter). The phone then runs on battery.
  The XDA report that the adapter "had to be powered" supports option (b).
- **Miracast**: Windows' Miracast source needs a Wi-Fi driver with Wi-Fi Direct support (or Miracast over
  Infrastructure with a working network). Neither exists on apollo yet, so Miracast is a long-term option only if the
  QCA6390 Wi-Fi work succeeds and that driver supports Wi-Fi Direct.
- **Remote Desktop fallback**: with a USB Ethernet adapter (works now), another PC on the LAN can show the phone's
  desktop full-screen on its monitor. Windows 11 Pro/Enterprise is required for the RDP host role (Home has no RDP
  server). It is not HDMI out, but it needs no new drivers and may feel smoother than DisplayLink over USB 2.0, because
  RDP is designed for low bandwidth. The keyboard/mouse would be the client PC's.

### Gaps
- Full text of the Synaptics release notes (supported chipset table, ARM64 known issues) could not be fetched (host
  blocked). The chipset coverage of 12.2 M2 on ARM64 is therefore not verified line by line.
- No report was found of DisplayLink working on any WOA phone port (Lumia 950 XL, SM8150/SM8250 phones), with or
  without a GPU driver.
- Unknown: apollo's VBUS/OTG current limit under Windows (PM8150B OTG boost configured by UEFI), and whether Windows'
  URS stack on apollo handles PD Source docks.
- No ARM64 Windows driver found for MacroSilicon MS9132/MS9133 "USB 3.0 to HDMI" adapters (not researched in depth;
  treat as unsupported).

## 4. Recommendation: what to buy / avoid

### Takeaway
Buy a genuine **DisplayLink** (Synaptics DL-3xxx/DL-5xxx/DL-6xxx) USB-to-HDMI adapter. Prefer a small single-output
one, used with a passive USB-C OTG adapter or a self-powered hub that does not charge the phone. Install DisplayLink
12.2 M2 (ARM64) and test at 1080p. **Avoid** anything that relies on DP Alt Mode (USB-C-to-HDMI cables, "USB-C
multiport adapters" with HDMI, USB-C monitors) or MHL. Also avoid non-DisplayLink "USB display" adapters (Silicon
Motion InstantView, MacroSilicon), which have no ARM64 Windows driver, and legacy DL-1x5 USB 2.0 adapters, which
have no current Windows driver.

### Cited Findings
- apollo's port is USB 2.0 only with DP disabled — [MiCode apollo-q-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/apollo-q-oss); live DTB in [edk2-msm](https://github.com/edk2-porting/edk2-msm)
- DisplayLink Windows driver supports ARM64 (11.3+, current 12.2 M2) — [Synaptics 12.2 M2 notes](https://drivers.synaptics.com/sites/default/files/release_notes/2026-07/DisplayLink%20USB%20Graphics%20Software%20for%20Windows12.2%20M2-Release%20Notes.txt)
- DisplayLink USB 3.0 parts work on USB 2.0 at reduced performance — [DisplayLink support 525038](https://support.displaylink.com/knowledgebase/articles/525038); [Belkin FAQ](https://www.manualslib.fr/manual/666225/Belkin-B2B043Eac00.html?page=15)
- SM768 has no ARM64 Windows driver listed — [Plugable SMI drivers](https://plugable.com/pages/siliconmotion)
- DL-1x5 is supported by open-source udl, not current Windows packages — [DisplayLink OS support](https://support.displaylink.com/knowledgebase/articles/543910)

### Inferences
- Suggested test order (inference): (1) install DisplayLink 12.2 M2 ARM64 (EXE or INF) on the phone. (2) Plug a
  DisplayLink adapter into a **powered USB hub** connected through a USB-C OTG adapter. Check Device Manager for the
  "DisplayLink USB Device"/IddCx monitor, then extend the desktop at 1920x1080. (3) Only after that try a USB-C
  DisplayLink dock that charges, and watch whether the phone stays USB host.
- If DisplayLink fails because of IddCx/WARP or driver issues, fall back to RDP over USB Ethernet. Revisit Miracast
  only once Wi-Fi exists.
- Do not spend effort on DP alt mode bring-up for apollo: the controller is configured USB 2.0 only, the QMP DP PHY
  is disabled, and Windows' DP path depends on the PEP-bound Qualcomm stack.

### Gaps
- No hands-on report of a specific adapter model on apollo/Windows. A "known good" product list for WOA phones does
  not exist in the sources I could reach.
