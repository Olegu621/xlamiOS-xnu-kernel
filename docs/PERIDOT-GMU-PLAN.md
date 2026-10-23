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

## Combined cold-start results (2026-10-23)

0134 ([CI39978394149](https://github.com/Olegu621/xlamiOS-xnu-kernel/actions/runs/39978394149))
successfully allocated and remapped all shared buffers as normal NC, built
six native4K tables with129 mapped leaves, uploaded/read back135 TCM words
and79900 external firmware bytes, enabled the SMMU context/SID5 and received
success from the actual SIP GPU_INIT_REGS call. DTCM reports core4.5.1 and
HFI2.6.1. These observations do not establish a running firmware/HFI service.

The first actual CM3 instruction fetch faults: FSR0x408, FAR0x4000,
FSYNR0=0x120063. Architectural bit3 identifies a permission fault at level3;
PNU/IND identify a privileged instruction request. FW_INIT remains0.
The processor was then reset, GBIF ARB halt acknowledged, the context disabled
and TLB synchronization confirmed. No panic was found, and the temporary boot
returned automatically to recovery after continuing through iOS userland.

The preceding0133 failure was in XNU cache retyping: a32-bit ppnum_t was shifted
before conversion to a64-bit physical address in pmap_sync_page_attributes_phys.
0134 fixes that conversion and requests kIOWriteCombineCache (ARM MAIR0x44
normal inner/outer NC) for the pinned shared buffers. Every CPU physical page
mapping is checked again after retyping; buffers stay owned until reboot.

Beyond the105 firmware pages above, HFI occupies IOVA0x60002000/0x14000
(20 native4K pages) and logs occupy0x60020000/0x4000 (four pages). The page-table
pool is a separate retained64KiB physical allocation; six of its16 native pages
are used. It is never mapped into the GMU address space.

The next combined0136 build adds explicit privileged firmware/HFI/log page
permissions, the actual matching peridot qcom,actlr setting0x32b, and GMU CB2
with its own ASID3. The stock peridot-u-oss driver reserves CB0/CB1 for GPU
page-table switching and starts other clients at CB2. This matches its policy;
it is not proof that bank0 caused the fault.0136 hardware acceptance is pending.

Actual CMD-DB and DT power inputs have also been captured privately on the PC.
The board has11 GPU levels255..1100MHz (initial index10), GMU220/550MHz with
corners64/128, and ten DDR bandwidth levels. A49-dword HFI2.6.1 performance
packet and160-dword bandwidth packet have been derived, including actual
SH0/MC0/ACV/CN0 addresses, units, widths and VCD grouping. They have not been
sent to the device. HWSCHED startup/memory replies, GPU command completion and
Mesa/IOSurface rendering remain required for hardware acceleration.
