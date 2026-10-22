# Adreno735 bring-up

The current iOS session uses CPU rendering. GPU work starts with the measured
peridot device tree and the actual recovery KGSL module, rather than a board
name substitution in another driver's register sequence.

## Established inputs

- Live DT: chip ID `0x43030b00`, model Adreno735,
  compatible `qcom,adreno-gpu-gen7-11-0`.
- The actual `msm_kgsl.ko` profile `adreno_gpu_core_gen7_11_0` references
  `adreno_gen7_hwsched_gpudev`, `gen71100_gmu.bin`, `gen71100_sqe.fw` and
  `gen71100_zap.mbn`. These names were resolved through ELF relocations on
  this exact profile; a list of strings from the whole module is insufficient.
  Reproducer and full findings are under ignored research:
  `audit-gpu-module.py`, `gpu-module-profile.json`.
- [Mesa's FD735 entry](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/f1ec46beaa6739fb15de953e6f4dc679b4e8c07f/src/freedreno/common/freedreno_devices.py)
  matches this ID and provides a7xx_gen2 shader/device properties and three
  CCUs. This supports using its existing compiler and renderer device data.
  It does not prove that the XNU hardware driver can submit work.

| Block | Live DT physical range / contract |
| --- | --- |
| GPU core | `0x03d00000`, `0x40000` bytes; SPI300 (GIC INTID332) |
| GMU | DT node name ends `3d69000`, but **reg base is `0x03d68000`**, size `0x37000`; HFI/GMU SPI304/305 |
| GMU PDC | `0x0b280000`, `0x10000` |
| GMU AO | `0x03d40000`, `0x10000` |
| GPUCC | `0x03d90000`, `0xa000`, `qcom,cliffs-gpucc` |
| CX GDSCR | **`0x03d9908c`**, despite node unit address `3d99108`; HW control `0x03d990a0` |
| GX GDSCR | **`0x03d99034`**, despite node unit address `3d9905c`; reset `0x03d99030` |
| GPU SMMU | `0x03da0000`, `0x40000`; TCU `0x03de6000`; separate from the APPS SMMU used for UFS/SPI |
| Streams | GPU user SID0, secure SID2, GMU SID5; these are GPU SMMU IDs |
| GCC config AHB | offset `0x71004`; memnoc `0x71010`, snoc/DVM `0x71018` |
| GPUCC clocks | AHB `0x90c8`, CX GMU `0x90e4`, CXO `0x90f0`, hub AON `0x9198`, hub CX `0x90f4`, SMMU vote `0x7000` |
| GPUCC RCGs | GMU `0x9168`, hub `0x919c`, XO `0x9010` |

Clock offsets come from Xiaomi's published
[gpucc-cliffs.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/peridot-u-oss/drivers/clk/qcom/gpucc-cliffs.c)
and [gcc-cliffs.c](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/peridot-u-oss/drivers/clk/qcom/gcc-cliffs.c).
Firmware bytes have not yet been collected or authenticated for this work.

## Implementation route

For the existing iOS16 userland, the practical frontend is the EAGL/OpenGL ES
path demonstrated in [HackiOS](https://github.com/oosexgang/hackios-xnu-userland)
and ios-topaz: CoreAnimation -> EAGL adapter -> Mesa/Freedreno -> a DRM-shaped
IOKit GPU service -> peridot hardware. The existing framebuffer can present the
result. This is an implementation choice, not a claim of current acceleration.

The useful parts of [NativeAdreno](https://github.com/UniXNU/NativeAdreno) are
its explicit memory ownership, cache visibility, GMU/HFI setup, SMMU contexts,
ring submission and fences. Its A740 hardware tables, power levels and firmware
selection need peridot equivalents. Its service ABI is different from HackiOS's
DRM-shaped API; one intentional adapter is required. Keep CPU16KiB pages and
GPU/GMU translation granules separate; do not change the working CPU page size
as a shortcut.

1. **0112 / `peridotgpu=1`:** read GCC/GPUCC clock and power handoff only.
   Refuse GPUCC reads if the configuration AHB is off or unreadable. No GPU
   core, GMU, SMMU, firmware, SCM or register-write operation in this stage.
2. Collect the phone's matching firmware and hashes through recovery. Validate
   image formats, memory windows and TrustZone/PIL requirements offline.
3. Bring up CX/clocks, inspect GPU SMMU geometry, then provide private GMU
   memory and bounded HFI startup. Preserve unknown/live/secure contexts.
4. Start CP with the matching SQE image; submit a private-memory write/fence
   and verify the result. An allocated ring is not proof of command execution.
5. Render an offscreen pattern through the FD735 Mesa path, then connect
   IOSurface buffers and CoreAnimation. Validate pixel output, completion and
   recovery before enabling it for ordinary sessions.

Current hardware acceptance: **0112 is not booted yet; GPU acceleration is not
implemented.** The default iOS boot remains on the tested CPU renderer.
