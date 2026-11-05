# apollo Windows-on-ARM driver pack

The ACPI nodes (see [`acpi-extension.md`](acpi-extension.md)) only *describe* the hardware.
Windows still needs matching **ARM64 drivers**. For a Snapdragon 865 (SM8250 / Qualcomm
codename **"Kona"**) phone, almost the entire SoC pack is reusable from existing WOA work; only
a few device-specific leaves are apollo's own.

## Sources

| Source | What it provides |
| --- | --- |
| [`WOA-Project/windows_silicon_qcom_kona`](https://github.com/WOA-Project/windows_silicon_qcom_kona) | the SD865 platform driver pack (clocks, SPMI/PMIC, QUP buses, TLMM GPIO, USB, PCIe, display, LPASS, PEP, …) |
| [`WOA-Project/Qualcomm-Reference-Drivers`](https://github.com/WOA-Project/Qualcomm-Reference-Drivers) | Qualcomm reference drivers pulled from Windows Update |
| `ios-apollo` firmware pullers (`tools/pull-*.sh`) | the exact firmware blobs apollo needs (same blobs Linux/XNU use) |

## Peripheral → driver → firmware

| Peripheral | Driver (from the Kona pack unless noted) | Firmware blob (via ios-apollo puller) |
| --- | --- | --- |
| Clocks / GCC / RPMh | Qualcomm clock + RPMh (Kona) | — |
| SPMI + PMIC (PM8150/B/L) | Qualcomm SPMI + PMIC (Kona) | — |
| QUP buses (SPI/I2C/UART) | Qualcomm GENI/QUPv3 (Kona) | — |
| TLMM GPIO | Qualcomm pinctrl/GPIO (Kona) | — |
| USB | Qualcomm USB (Kona) | — |
| Display (framebuffer → QC display) | SimpleFB first; Qualcomm display later | — |
| **Touch (NT36675)** | HID-over-SPI + a Novatek function driver (**device-specific, likely to add**) | touch FW `novatek_nt36675_j3s_fw01.bin` (loaded by the touch driver at boot) |
| **Wi-Fi (QCA6390)** | Qualcomm/ath WLAN for QCA6390 over PCIe (Kona / reference) | `amss20.bin`, `m3.bin`, `regdb.bin`, `bd_j3sgl.elf` |
| **Bluetooth (QCA6390)** | Qualcomm BT over UART | `htbtfw20.tlv`, `htnv20.bin` |
| **Audio (WCD938x + CS35L41)** | Qualcomm LPASS/ADSP audio + codec; CS35L41 amp | `adsp.mdt`/`adsp.b0*`, ACDB `mic_*.bin` |
| **Haptics (AW8697)** | vendor/Qualcomm haptics (**device-specific**) | `aw8697_haptic.bin` |
| GPU (Adreno 650) | Qualcomm Adreno 650 WoA GPU driver | `a650_gmu.bin`, `a650_sqe.fw`, `a650_zap.*` |
| Buttons (pwr/vol) | PEP button array (Kona) | — |

## Notes

- **Reuse ratio is high:** SM8250 is heavily covered by the Kona pack, so the realistic work is
  (a) the ACPI, (b) the two device-specific leaves (Novatek touch, AW8697 haptics), and
  (c) getting firmware onto the image. This is why reusing the `ios-apollo` hardware map is the
  whole shortcut.
- **Firmware is never committed** (see `.gitignore`). The `ios-apollo` pullers fetch the same
  blobs off the phone / from Xiaomi+Qualcomm source; Windows just loads them via its drivers.
- **Modem (X55) and cameras (Spectra ISP)** are out of scope — unsupported on `ios-apollo` and
  rarely done on WOA.
- **Next concrete step:** clone `windows_silicon_qcom_kona` and enumerate the exact `.inf`
  names per block, so this table points at real files rather than families.
