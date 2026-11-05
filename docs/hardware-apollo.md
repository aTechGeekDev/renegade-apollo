# apollo hardware map

Xiaomi Mi 10T / Mi 10T Pro / Redmi K30S Ultra, codename **`apollo`**.

Everything here is distilled from the verified, working `ios-apollo` (project-BadApple) port,
where each of these parts has actually been brought up on the real phone. For a Windows-on-ARM
port this is the input to the **ACPI tables** and the **driver pack** — it tells you which
Qualcomm/vendor driver each peripheral needs and which firmware blob to ship.

> Codename trap: some sources use `apollo` for the Mi 10T **Lite**, which is a *different* SoC.
> This repo is strictly the Snapdragon 865 Mi 10T / 10T Pro (`apollo` / `apollopro`).

## SoC and CPU

| | |
| --- | --- |
| SoC | Qualcomm Snapdragon 865 — **SM8250** |
| CPU | Kryo 585: 1× Cortex-A77 "prime" + 3× A77 "big" + 4× A55 "little" (8 cores) |
| Core layout (from ios-apollo `cpumask`) | bits 0–3 = little (A55), bits 4–6 = big (A77), bit 7 = prime (A77) |
| Storage | UFS 3.x (internal) |
| RAM | 6/8 GB LPDDR5 |

**WoA note:** SM8250 is well-trodden in the Windows-on-ARM community (shared with OnePlus 8/8T,
Mi 10, etc.). Most of the SoC-level UEFI platform code and the base Qualcomm driver pack can be
adapted from an existing SM8250 WoA device rather than written from scratch.

## Peripherals, firmware and the Windows driver they map to

| Part | Chip | Bus / interface | Firmware blob (from `ios-apollo`) | Windows-on-ARM driver family |
| --- | --- | --- | --- | --- |
| Display panel | 1080×2400, 60/90/144 Hz | DSI (MDSS/DPU) | — | Qualcomm display (SimpleFB for UEFI, then QC display driver) |
| Touch | Novatek **NT36675** | SPI | `novatek_nt36675_j3s_fw01.bin` | HID-over-SPI / vendor NT touch driver |
| GPU | Adreno **650** | — | `a650_gmu.bin`, `a650_sqe.fw`, `a650_zap.mdt`/`.b02` | Qualcomm Adreno 650 WoA GPU driver |
| Wi-Fi + BT combo | **QCA6390** | Wi-Fi: PCIe/SNOC · BT: UART @ 921600 | Wi-Fi: `amss20.bin`, `m3.bin`, `regdb.bin`, `bd_j3sgl.elf` · BT: `htbtfw20.tlv`, `htnv20.bin` | Qualcomm QCA6390 WoA Wi-Fi/BT drivers |
| Audio DSP | ADSP | — | `adsp.mdt`, `adsp.b00`, … | Qualcomm audio (ADSP + codec) |
| Audio codec | **WCD938x** | SoundWire | mic tuning: ACDB `mic_*.bin` | Qualcomm WCD938x codec driver |
| Speaker amps | 2× Cirrus **CS35L41** | — | — | CS35L41 amp driver |
| Haptics | Awinic **AW8697** | I2C | `aw8697_haptic.bin` | vendor haptics driver |
| PMIC (main) | **PM8150** | SPMI | — | Qualcomm SPMI/PMIC |
| PMIC (battery/charger) | **PM8150B** | SPMI | — | Qualcomm charger/fuel-gauge |
| PMIC (flash LED etc.) | **PM8150L** | SPMI | — | Qualcomm PMIC |
| RTC | on PM8150 | SPMI | — | Qualcomm RTC |
| Modem | Snapdragon **X55** | — | (not started in ios-apollo) | Qualcomm modem (hardest; often left out of WoA) |
| Sensors | Qualcomm sensor hub (SLPI) | — | SLPI firmware (not started) | Qualcomm SensorsPkg / SSC |
| Cameras | Spectra ISP | — | (not started) | — (rarely done on WoA) |
| Buttons | power, vol up, vol down | GPIO/PMIC | — | PEP / button array |

## Firmware acquisition

`ios-apollo` already has scripts that pull these exact blobs off the phone / from vendor source
(`tools/pull-gpu-fw.sh`, `pull-wifi-fw.sh`, `pull-bt-fw.sh`, `pull-audio-fw.sh`,
`pull-mic-haptic-fw.sh`; touch firmware comes from `MiCode/Xiaomi_Kernel_OpenSource` branch
`apollo-q-oss`). The blobs are the same; Windows just loads them differently (via ACPI
`_DSD`/image-load vs. the XNU driver). These scripts are the fastest way to assemble the
firmware set for the Windows driver pack.

## Deep-dive source

The engineering log `ios-apollo/NOTES.md` (~414 KB) contains the register-level details —
memory map, UART debug base, clock/regulator bring-up order, GPIO pin configs, display timing —
recorded per boot. That is the reference to mine when writing the ACPI tables, and is a
to-do for the next pass (see `docs/porting-plan.md`).
