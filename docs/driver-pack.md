# apollo Windows-on-ARM driver pack

The ACPI nodes (see [`acpi-extension.md`](acpi-extension.md)) only *describe* the hardware;
Windows still needs matching **ARM64 drivers**. SM8250 (Qualcomm codename **"Kona"**) is
heavily covered by the existing pack, so almost everything is reusable — only a couple of
device-specific leaves are apollo's own.

## Sources

| Source | What it provides |
| --- | --- |
| [`WOA-Project/windows_silicon_qcom_kona`](https://github.com/WOA-Project/windows_silicon_qcom_kona) | the SD865 driver pack — **136 INFs** across Audio, Battery, Bluetooth, Graphics, Sensors, Touch, USB, VibrationMotor, WLAN, SOC, … |
| [`WOA-Project/Qualcomm-Reference-Drivers`](https://github.com/WOA-Project/Qualcomm-Reference-Drivers) | Qualcomm reference drivers from Windows Update |
| `ios-apollo` firmware pullers (`tools/pull-*.sh`) | the exact firmware blobs (same ones Linux/XNU use) |

## Peripheral → real INF (from the Kona pack) → firmware

| Peripheral | Kona INF (`...8250`) | apollo firmware (ios-apollo puller) |
| --- | --- | --- |
| SoC core (PMIC glink, TrEE, ADC, glink, partitions) | `QcPmicGlink8250.inf`, `QcTrEE8250.inf`, `qcadc8250.inf`, `qcglink8250.inf`, `QcSOCPartition.inf` | — |
| Graphics (Adreno 650) | `qcdx8250.inf`, `qdcmlib8250.inf` | `a650_gmu.bin`, `a650_sqe.fw`, `a650_zap.*` |
| USB | `qcusbctcpm8250.inf`, `qcusbcucsi8250.inf`, `QcUSBAudio8250.inf` | — |
| **Bluetooth (QCA6390)** | `qcbtfmuart8250.inf`, `qcbtaddvscregistry8250.inf` | `htbtfw20.tlv`, `htnv20.bin` |
| **Wi-Fi (QCA6390)** | `qcwlan8250.inf` | `amss20.bin`, `m3.bin`, `regdb.bin`, `bd_j3sgl.elf` |
| **Audio (WCD938x + CS35L41)** | `qcaudminiport_Base8250.inf`, `qcadsprpc8250.inf`, `qcslimbus8250.inf`, `AudioService8250.inf` | `adsp.mdt`/`adsp.b0*`, ACDB `mic_*.bin` |
| **Haptics (AW8697)** | `qchwnhaptics8250.inf` *(generic HWN — AW8697 specifics may need tuning)* | `aw8697_haptic.bin` |
| **Battery / charger (PM8150B)** | `qcbattminiclass8250.inf`, `qcbattmngr8250.inf`, `qcfgbcl8250.inf` | — |
| Sensors (SLPI) | `qcSensors8250.inf` | SLPI firmware |
| Shutdown / power | `qcshutdownsvc.inf` | — |
| **Touch (NT36675)** | ⚠ `qcImproveTouch8250.inf` is **Qualcomm, not Novatek** | `novatek_nt36675_j3s_fw01.bin` |

## The one real driver gap: touch

The Kona touch INF (`qcImproveTouch8250.inf`) drives a Qualcomm/Synaptics-style controller, **not
the Novatek NT36675** apollo uses. So apollo needs a **Novatek HID-over-SPI function driver**.
Options, easiest first:
1. A Novatek Windows ARM64 driver from another WOA device that ships NT36675/NT36672 (reuse its
   INF + the `touch.asl` _HID it expects).
2. A generic HID-over-SPI shim keyed to `touch.asl`'s `_HID`, with the NT36675 protocol
   (the constants are in `AcpiTables/apollo/touch.asl`).
Touch is therefore the one peripheral where the ACPI is ready but the driver is not off-the-shelf.

## Notes

- Firmware is never committed (`.gitignore`); the `ios-apollo` pullers fetch the same blobs.
- Modem (X55) and cameras (Spectra ISP) are out of scope — unsupported on `ios-apollo` and rarely
  done on WOA.
- Reuse ratio is high: the real work is the ACPI (mostly done, see `acpi-extension.md`), the
  Novatek touch driver, and getting firmware onto the image.
