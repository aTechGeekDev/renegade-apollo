# apollogpu: Adreno 650 on Windows (milestone 1)

ios-apollo brought the Mi 10T's Adreno 650 up under XNU with its own code: no Qualcomm PEP, no kgsl. This driver
is that bring-up (`xnu/bsd/dev/sweet_gpu.c`, `sweet_gpu_probe`, steps 1-22, as the patch series 0096-0186 leaves
it) on Windows, with the hardware sequence unchanged. Every register write, ring word, HFI word and TrustZone call
was compared with the XNU source mechanically.

| Step | What | Proven on apollo (ios-apollo) |
| --- | --- | --- |
| 1-5 | GPU config-bus clock, GPU clock controller, CX power, GMU/bus clocks | boot 12 |
| 6-10 | GMU alive (19.2 MHz counter), RSCC, SMMU IDs and contexts, GMU memory writable | boots 12-14 |
| 11-14 | GMU memory and SMMU context 1, `a650_gmu.bin` loaded, GMU firmware runs, HFI queues up | boot 16 |
| 15-17 | RPMh levels from cmd-db, GMU tables, GPU speed, 3D core powered | boot 17 |
| 18-19 | ring + `a650_sqe.fw` behind SMMU context 0, hw_init, command processor takes CP_ME_INIT | boots 18-19 |
| 20 | TrustZone authenticates `a650_zap.mdt/.b02`, the core leaves secure mode | boots 22-24 |
| 21-22 | the core writes our memory; its 2D engine draws three bands, copied to the panel | boot 26 |

Then the teardown: core halted, GX given back, GMU slumber and reset, zap shader handed back to TrustZone, RPMh
channel asleep, SMMU contexts off, CX collapsed. The GPU is never kept on (milestone 2 adds that for Mesa).

## Windows specifics

- **Exact instructions** (`apollogpu_asm.asm`, armasm64): every register access is one `ldr`/`str` (the hypervisor
  emulates the SMMU's global registers and handles nothing else; the stored value is always a register, never
  `wzr`); `dc civac` for memory the GPU reads; `smc #0` with x0-x7 in and out.
- **Memory** the GMU and the core reach is allocated write-combined (normal non-cacheable, matching the SMMU
  entries' attribute 0), so no RAM is mapped twice with different cache attributes; the zap header (one physical
  address for TrustZone) is contiguous. All of it is freed after the teardown.
- **Firmware** from `System32\drivers\apollogpu\`: the phone's own files, as ios-apollo `tools/pull-gpu-fw.sh`
  takes them (never committed).
- **Parameters** (`HKLM\SYSTEM\CurrentControlSet\Services\apollogpu\Parameters`): `Steps` (default 22), `Level`
  (1 = 305 MHz .. 6 = 587 MHz, default 1), `Zap` (TrustZone ladder 0..3, default 3).
- **Log**: every line to `Parameters\Log` and `\Status`; the hive is flushed to disk before each step and each
  TrustZone call, so after a freeze `install-apollogpu.ps1 -Status` shows where it stopped.
- **Demand start**: it runs only when started (`-Run`), never at boot.

## Use

```sh
# Kali, phone in recovery (OrangeFox): the GPU firmware into ios-apollo/research/apollo-gpu/
cd ~/ios-apollo && tools/pull-gpu-fw.sh
```
Copy `research/apollo-gpu/` and the driver-builds files `apollogpu.sys` + `install-apollogpu.ps1` to the phone, then
in Windows (Administrator PowerShell, in that folder):
```powershell
powershell -ExecutionPolicy Bypass -File install-apollogpu.ps1 -Firmware .\apollo-gpu -Run
```
Success: the three bands appear on the screen (Windows paints over them as it redraws) and the script prints
`RESULT: the GPU works on Windows`.
