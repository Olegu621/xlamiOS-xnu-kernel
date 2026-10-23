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


### 0136 hardware acceptance

[CI39981109143](https://github.com/Olegu621/xlamiOS-xnu-kernel/actions/runs/39981109143)
built successfully. The timed device cycle verified CB2/ASID3/ACTLR0x32b,
FW_INIT0x100 and HFI control1, then sent GET_VALUE125. The real4-word reply
has header0x1040c, echoed request0x10030c and value1. This is real firmware
execution and HFI communication. FSR0x400 denotes FORMAT2 with no fault flags;
FAR/FSYNR must not be interpreted in that state. Reset, bus halt and context
shutdown completed and recovery returned automatically. GPU command execution
and accelerated iOS rendering remain required.

### 0137 power-table exchange and ACK contract

[CI39983756607](https://github.com/Olegu621/xlamiOS-xnu-kernel/actions/runs/39983756607)
built successfully. The temporary image repeated the real GMU startup and
GET_VALUE125 response, then published the actual49-word PERF packet. Firmware
returned a three-word ACK echoing request0x20310f. The host had incorrectly
required four words for every request and stopped before logging its result;
the PERF result is therefore not established by this capture. BW, features
and core start were not reached. GX stayed collapsed (GDSCR0x22f801), reset,
bus drain and context disable/synchronization succeeded, and recovery returned.

The primary KGSL gen7_hfi_send_generic_req accepts original generic replies
with result in word2, while gen7_hfi_send_generic_req_v5 checks word3 for
feature/value firmware errors on this HFI version. The0138 correction selects
the contract per command, retains exact request matching and result checks,
and logs response length/result. START/memory requests and GPU submissions
remain separate unfinished driver phases.

### 0138 HWSCHED preparation accepted by real firmware

[CI39985882744](https://github.com/Olegu621/xlamiOS-xnu-kernel/actions/runs/39985882744)
built successfully. Hardware returned result0 for PERF49 and BW160 (three-word
ACKs), IFPC-disable/HWSCHED/KPROF/DMS (four-word v5 ACKs, error0), and
CORE_FW_START (three-word ACK). Queries121/context queues and122/GMU_AB
each return1/error0. Nine exact requests were acknowledged. GX remained
collapsed, reset/drain/context disable completed and recovery returned.
SpringBoard/backboardd execute, but repeated IOAcceleratorES/Legacy matching
finds no service; the user's screen observation was Apple logo only at60s.
The same kernel was also booted with a180s diagnostic timer. The user explicitly
confirmed a visible iOS lock screen or home screen, looking as before. The
earlier60s screen observation therefore did not rule out eventual UI startup.
Keep180s in subsequent images. This observation does not prove GPU rendering.

0139 introduces START's actual allocation/reply phase, with retained NC
buffers, append-only native4K mappings, bounded ASID synchronization and an
inactive GPU root for the future GPU boot. Existing translations stay intact;
each new VA is advertised only after its backing and translation are published.
Unknown platform resources are refused. GPU OOB/SQE/ZAP/CP and rendered iOS
frames are still future phases;0139 compilation and hardware behavior pending.

### 0139 START accepted with ten actual backed objects

[CI39987869394](https://github.com/Olegu621/xlamiOS-xnu-kernel/actions/runs/39987869394)
built successfully. Firmware requested ten version1 allocations (scratch,
profiling, command buffers, ring, SMMU information, preemption records/counter
and scratch), totaling0x23c000 bytes of reported backing. Every reply followed
real allocation/mapping and bounded GMU ASID synchronization. Firmware then
returned the exact three-word START ACK with result0. The non-secure fallback
for the unused secure preemption slot was also accepted. No MMIO request
occurred. GX stayed collapsed; reset/drain/context shutdown completed without
a fault or panic. The180s cycle returned to recovery and captured the console.

The GPU root and its mappings remain inactive; this is not GPU command
completion. The shipped firmware asks for24-byte SMMU information, while newer
Qualcomm source adds a context-bank field; match that contract before GPU boot.
SQE upload, GPU context/power, CP initialization, a real memory write/fence and
the Mesa/IOSurface path remain required.
