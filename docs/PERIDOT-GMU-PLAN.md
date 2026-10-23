# Peridot GMU integration evidence, 2026-10-23

Reference: https://github.com/qualcomm-linux/kgsl at
2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f (current Qualcomm source,
not proven byte-identical to the shipped Android driver).

adreno-gpulist.h gen7_11_0 selects gen7_hwsched_gpudev, GMU_VERSION4.5.0,
gen71100 firmware, 200MHz hub. Do not assume a direct A740 CP submission
backend is sufficient for this735. Match actual firmware HFI behavior.

adreno_gen7_gmu.c gen7_gmu_device_start polls bit8 under mask0x1ff;
legacy babeface is not the sole readiness criterion. Firmware startup
and HFI command ACKs must be observed, not inferred from readable registers.

Local exact firmware SHA256:
435c7bac9a328b8a100df930b84b65b4d579e42be44785d0e60e3729291bb864.
External firmware/preallocation memory from its block records, rounded4KiB:

| IOVA | Bytes | Pages |
| --- | --- | --- |
| 0x4000 |81920|20|
| 0x100000 |110592|27|
| 0x200000 |77824|19|
| 0x300000 |151552|37|
| 0x60000000 |8192|2|

ITCM and DTCM payloads use MMIO, not these SMMU allocations. HFI queues,
logs and runtime firmware memory requests need additional explicitly owned
allocations. Above105 pages are not a complete runtime memory requirement.

Acceptance sequence: context readback/disable, exact SID5 mapping, actual
translation/fault observation, matching firmware load plus TCM readback,
CM3 startup, HFI handshake and HWSCHED memory/feature ACKs, GPU command
completion, Mesa integration and actual rendered iOS frames.
