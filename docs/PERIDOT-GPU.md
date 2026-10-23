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
Matching GMU/SQE bytes were collected read-only from the phone's vendor_a
filesystem during0113 preparation. They have not been executed by this port:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| gen71100_gmu.bin |80680|435c7bac9a328b8a100df930b84b65b4d579e42be44785d0e60e3729291bb864|
| gen71100_sqe.fw |66036|b6df660d109504dbea5a266910b037a33d893a11c6de52f524a5fd112008c401|
| gen71100_zap.mbn (odm_a) |12088|dbfd89112520d774781ecf511ce74c31e6a5a95a8002cafd2a51f0ef1e368f45|

The GMU image parses exactly to EOF as15 KGSL block records:11 metadata
preallocations followed by4 payloads. Persistent preallocations reserve4KiB
at0x60000000 and0x60001000; cache reservations include0x4000/0x1381c,
0x100000/0x1aa78,0x200000/0x12c80,0x300000/0x24bf8. Every requested range
and payload fits the source-defined GMU VMA. This verifies format/bounds,
not authenticity or successful startup. Audit JSON and binaries stay under
ignored research/gpu-recovery-0113. The existing UniXNU parser, now under
vendor/AdrenoFirmware, independently gives the same15 blocks; host tool
tools/peridot-gmu-plan.c also checks preallocation bounds/order.
Recovery's actual SMMU boot log reports SMMUv2,24 stream groups,22 context
banks, coherent walks,48-bit VA to36-bit IPA and zero preserved boot mappings.

The manufacturer's [SM8635 KGSL profile](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8635/blob/1a24977367ee2d77998fc6957eb61a99331a347d/vendor/qcom/opensource/graphics-kernel/adreno-gpulist.h#L2459)
independently matches the actual module's gen7_11_0 firmware names and hardware
scheduler. It specifies 1536 KiB GMEM, 16 MiB UCHE alignment, highest bank bit16,
a 200 MHz GMU hub target and a 2196 KiB context record. It shares gen7_2_0
clock-gating/GBIF tables and gen7_0_0 protected registers; these are deliberate
profile references, not a reason to import all A740 initialization tables.
This is another vendor's SM8635 release: compare any register sequence with
the phone's module and DT before using it.

Its [GMU implementation](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8635/blob/1a24977367ee2d77998fc6957eb61a99331a347d/vendor/qcom/opensource/graphics-kernel/adreno_gen7_gmu.c#L32)
defines ITCM at0/16KiB, cache at0x4000/(16MiB-16KiB), DTCM at0x10004000/16KiB,
and noncached kernel ranges at0x60000000 and0xc0000000, each512MiB. Firmware
zero-payload headers can request preallocations; load records then target
these ranges. Collect and parse the matching image before choosing private
GMU mappings. A generic driver's fixed debug/HFI windows are insufficient.

## Implementation route

Broader source search found a second ready iOS16 frontend:
[ios-sailfish sweetgl](https://github.com/joranv1/ios-sailfish/tree/d7a74344828c87d37c64f87f237525cb65b7e5f5/tools/sweetgl).
It targets iOS16.7 and already contains EAGL dispatch, IOSurface imports and
CA shader workarounds. Hardware is A530, so only the frontend contract is
relevant. Its sysmem rendering avoids tiled stores beyond unpadded IOSurfaces;
nofp16 avoids CA shader precision problems. Reuse these findings rather than
recreating the whole renderer. HackiOS's pinned Mesa25.0.7 already includes
the exact FD735 ID, properties and compiler backend. No Metal implementation
is needed for the first accelerated CA interface.

Also inspected the actual [NativeMetal source](https://github.com/UniXNU/NativeMetal)
and [sheng board](https://github.com/UniXNU/UniXNU-sheng/tree/2b7a5815110bf75713eb596612b0b33231b181f8).
This is a working iPadOS17.7.11 frontend, not merely a macOS concept. It has
an existing Turnip Darwin KMD in UniXNU/patches/mesa/turnip-darwin-kmd.patch,
including native BO imports and fences; future Metal work can reuse it.
It is not the Gallium/msm ABI used by the iOS16 sweetgl frontend. Choose a
frontend deliberately rather than porting both ABIs at once. Existing EAGL
integration for16.7 remains the smaller initial compositor change.

NativeAdreno's GMU platform depends on ShengGmu.h/c from the board repository,
not only the driver and AdrenoFirmware kit. That code checks sheng's exact
firmware hash/size/chip ID and generated RSC setup, so importing only
NativeAdreno does not supply a peridot GMU startup implementation. Its state
machine is reusable; firmware preallocations, windows, power/clock offsets,
SCM setup, RSC data and HFI inputs need measured peridot equivalents.

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
   **0114 / `peridotgpu=2`: hardware PASS** for a bounded idle-CX power pulse,
   restoring its original collapsed state. GX unchanged; no firmware, clocks,
   rail votes, core/GMU/SMMU access. Configured diagnostic reset required.
   **0115 / `peridotgpu=3`: hardware result is a clean negative.** The CX
   hold completed and was restored exactly (0x22f801, ownership kept); the
   mode1 inventory matched the0112 values; the image stayed usable through
   the timed reset. The configuration AHB gate write did NOT stick: 0x90c8
   bit0 never read back during the bounded50ms poll, so the stage restored
   the original value and reported AHB_ON_TIMEOUT. The live-window report
   only ran after a verified gate, so0115 produced no CC-live lines. The
   gate's parent is the hub RCG (0x919c cmd /0x91a0 cfg), never updated
   before (0/0); the GMU RCG is explicitly parked (0x80000000) and is not
   on this path. **0116 / `peridotgpu=4`: hub RCG proven writable, gate still refuses.**
   The hub RCG latched cfg0x503 and its UPDATE bit self-cleared; the
   restore read back0/0. The live inventory finally ran and showed cbcr
   bit31 is the halt status (cleared for running branches while CX is
   powered) and bit0 is the enable latch; the configuration AHB gate still
   never latched with the hub running, so its parent clock is not the
   blocker. **0117 / `peridotgpu=5`:** restores the vendor probe invariant
   (always-on cxo-aon0x9004 and demet0x900c), then retries the AHB gate and
   toggles one HALT-class sibling (cx-ff0x90f8), printing per-branch
   before/during/accepted/after/restored values plus three live
   inventories; read-modify-write only, singles restored at once, the pair
   last, bounded10ms polls, no GMU, GPU core, SMMU, RCG, vote or firmware
   access.
2. Collect the phone's matching firmware and hashes through recovery. Validate
   image formats, memory windows and TrustZone/PIL requirements offline.
3. Bring up CX/clocks, inspect GPU SMMU geometry, then provide private GMU
   memory and bounded HFI startup. Preserve unknown/live/secure contexts.
4. Start CP with the matching SQE image; submit a private-memory write/fence
   and verify the result. An allocated ring is not proof of command execution.
5. Render an offscreen pattern through the FD735 Mesa path, then connect
   IOSurface buffers and CoreAnimation. Validate pixel output, completion and
   recovery before enabling it for ordinary sessions.

### ZAP authentication contract (offline, not executed)

The peridot Android device's proprietary-file list exposed the missing path:
odm/firmware/gen71100_zap.mbn. Root recovery mounted odm_a read-only, pulled
the file, then unmounted it. The live DT reserves0x9899a000/0x2000 for GPU
microcode. The MBN is a self-contained ELF32 with three program headers.
Only its1012-byte load segment belongs in that carveout, at its start;
the whole12088-byte file would exceed the reserved region.

[KGSL kgsl_util.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8635/blob/1a24977367ee2d77998fc6957eb61a99331a347d/vendor/qcom/opensource/graphics-kernel/kgsl_util.c#L108)
uses PAS13 and Qualcomm's MDT loader, then PAS auth-and-reset. The
[matching MDT loader](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/peridot-u-oss/drivers/soc/qcom/mdt_loader.c)
packs the148-byte header with the3896-byte signature/hash segment into
separate contiguous metadata,4044 bytes total; the file padding is omitted.
Metadata must have coherent/noncached physical storage with4KiB alignment
while SCM authenticates it. Relocatable segment flags require PAS mem-setup
for4096 bytes at the reserved base, then segment placement and auth-and-reset.
tools/peridot-zap-plan.py validates file spans, metadata, relocation and the
carveout offline. Authentication is still untested; no SCM call was issued.

Hardware acceptance: **0112's inventory completed during the0113 iOS boot.**
GCC configuration AHB was0x1; CX/GX GDSCR both0x22f801, CX HW0x2800010a,
GX domain0x3c0001. GPUCC AHB0, CX GMU0x80002001, CXO0x80000001,
hub AON1, hub CX0x80000001, memnoc/SMMU vote/CX FF0x80000000; GMU
RCG0x80000000/0, hub0/0, XO0/0. These are observed handoff values,
not an initialized submission path. iOS and actual touch input continued
after the probe. **GPU acceleration remains unimplemented.** Ordinary0113
uses the tested CPU renderer/full-panel scaler with the GPU probe disabled.
