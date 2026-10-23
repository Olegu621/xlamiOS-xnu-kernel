/* SPDX-License-Identifier: MIT */
/* Included after the firmware, HFI17 and native4K SMMU integration.
 * Register/protocol provenance: qualcomm-linux/kgsl
 * 2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f, gen7_11_0 (Adreno735).
 * Linux v6.12 qcom_scm defines the SIP GPU_INIT_REGS call.
 * One bounded cold boot, GET_VALUE exchange, reset and bus drain. All
 * backing allocations remain owned until reboot, including failure paths. */
#include <IOKit/IOMapTypes.h>
#include <kern/task.h>

struct pgcb_memory {
    uint32_t iova, bytes;
    vm_size_t allocation;
    void *cpu;
    IOPhysicalAddress physical;
};
struct pgcb_saved {
    vm_offset_t base;
    uint32_t offset, value;
};
static struct {
    unsigned attempted, saved_count, bank;
    struct pgcb_memory memory[9];
    struct pgcb_saved saved[48];
} pgcb;

static uint32_t pgcb_read(vm_offset_t base, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(base + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
static void pgcb_write(vm_offset_t base, uint32_t off, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(base + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t pgcb_gread(vm_offset_t gmu, uint32_t word)
{
    if (word < 0x1a000 || word >= 0x27c00) panic("PERIDOT GMU register range");
    return pgcb_read(gmu, word * 4 - 0x68000);
}
static void pgcb_gwrite(vm_offset_t gmu, uint32_t word, uint32_t v)
{
    if (word < 0x1a000 || word >= 0x27c00) panic("PERIDOT GMU register range");
    pgcb_write(gmu, word * 4 - 0x68000, v);
}
static void pgcb_save_write(vm_offset_t base, uint32_t off, uint32_t v)
{
    if (pgcb.saved_count >= 48) panic("PERIDOT GMU snapshot capacity");
    struct pgcb_saved *s = &pgcb.saved[pgcb.saved_count++];
    *s = (struct pgcb_saved){base, off, pgcb_read(base, off)};
    pgcb_write(base, off, v);
}
static bool pgcb_poll(vm_offset_t base, uint32_t off, uint32_t mask,
    uint32_t expected, unsigned microseconds)
{
    for (unsigned i = 0; i < microseconds / 10; ++i) {
        if ((pgcb_read(base, off) & mask) == expected) return true;
        delay(10);
    }
    return false;
}
/* QCOM_A6 continuation state survives interrupted SMCCC calls. WAITQ_SLEEP
 * is reported to the caller; it requires a separate interrupt/resume path. */
static int64_t pgcb_scm(uint64_t fn, uint64_t argument, uint64_t *result)
{
    uint64_t a0 = fn, a6 = 0;
    for (unsigned n = 0; n < 64; ++n) {
        register uint64_t x0 __asm__("x0") = a0;
        register uint64_t x1 __asm__("x1") = 1;
        register uint64_t x2 __asm__("x2") = argument;
        register uint64_t x3 __asm__("x3") = 0;
        register uint64_t x4 __asm__("x4") = 0;
        register uint64_t x5 __asm__("x5") = 0;
        register uint64_t x6 __asm__("x6") = a6;
        register uint64_t x7 __asm__("x7") = 0;
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2),
            "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) : :
            "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
            "x16", "x17", "cc", "memory");
        *result = x1;
        if (x0 == 1) { a0 = 1; a6 = x6; continue; }
        if ((int64_t)x0 == -12 && n < 4) {
            a0 = fn; a6 = 0; delay(30000); continue;
        }
        return (int64_t)x0;
    }
    return -110;
}
static bool pgcb_allocate(struct pgcb_memory *m)
{
    m->allocation = (m->bytes + PAGE_MASK) & ~PAGE_MASK;
    m->cpu = pgpu_allocate(m->allocation, PAGE_SIZE, &m->physical);
    if (!m->cpu) return false;
    bzero(m->cpu, m->allocation);
    flush_dcache64(m->physical, m->allocation, TRUE);
    printf("PERIDOT GMU: remap normal-NC physical=%#llx bytes=%#lx\n",
        (unsigned long long)m->physical, (unsigned long)m->allocation);
    /* Change attributes before the first device reference. Do not recycle
     * this allocation through a cached allocator after remapping it. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    IOReturn status = IOSetProcessorCacheMode(kernel_task,
        (IOVirtualAddress)m->cpu, m->allocation, kIOWriteCombineCache);
#pragma clang diagnostic pop
    printf("PERIDOT GMU: memory iova=%#x bytes=%#x cache-status=%#x retained=1\n",
        m->iova, m->bytes, status);
    if (status != kIOReturnSuccess) return false;
    for (vm_size_t off = 0; off < m->allocation; off += PAGE_SIZE) {
        ppnum_t pp = pmap_find_phys(kernel_pmap, (addr64_t)m->cpu + off);
        if (!pp || ((uint64_t)pp << PAGE_SHIFT) != m->physical + off) return false;
    }
    return true;
}
static bool pgcb_walk(const ArmSmmu4KResult *built, const ArmSmmu4KRange *r,
    size_t count, const struct pgcb_memory *pool)
{
    for (size_t k = 0; k < count; ++k) {
        for (uint64_t off = 0; off < r[k].bytes; off += 4096) {
            uint64_t table = built->root_physical, va = r[k].iova + off;
            for (unsigned l = built->start_level; l < 4; ++l) {
                if (table < pool->physical ||
                    table - pool->physical > pool->bytes - 4096) return false;
                volatile uint64_t *p = (volatile uint64_t *)((uintptr_t)pool->cpu +
                    (table - pool->physical));
                uint64_t d = p[(va >> (12 + 9 * (3-l))) & 511];
                if ((d & 3) != 3) return false;
                table = d & UINT64_C(0x0000fffffffff000);
                if (l == 3 && table != r[k].physical + off) return false;
            }
        }
    }
    return true;
}
static bool pgcb_load(vm_offset_t gmu, const uint8_t *fw,
    const AdrenoFirmwareBlock *blocks, size_t count)
{
    unsigned tcm_words = 0, external_bytes = 0;
    /* Clear both TCMs while the processor is held in reset. */
    for (unsigned off = 0; off < 0x4000; off += 4) {
        pgcb_write(gmu, 0x5000 + off, 0);
        pgcb_write(gmu, 0x9000 + off, 0);
    }
    for (size_t k = 0; k < count; ++k) {
        const AdrenoFirmwareBlock *b = &blocks[k];
        if (!b->bytes) continue;
        if ((b->address | b->bytes | b->payload_offset) & 3) return false;
        uint32_t tcm = b->region_identifier == 1 ? 0x5000 + b->address :
            b->region_identifier == 3 ? 0x9000 + b->address - 0x10004000 : 0;
        if (tcm) {
            for (unsigned off = 0; off < b->bytes; off += 4) {
                uint32_t v;
                bcopy(fw + b->payload_offset + off, &v, 4);
                pgcb_write(gmu, tcm + off, v);
                if (pgcb_read(gmu, tcm + off) != v) return false;
                ++tcm_words;
            }
        } else {
            bool found = false;
            for (unsigned i = 0; i < 7; ++i) {
                struct pgcb_memory *m = &pgcb.memory[i];
                if (b->address < m->iova || b->address - m->iova > m->bytes ||
                    b->bytes > m->bytes - (b->address - m->iova)) continue;
                volatile uint32_t *to = (volatile uint32_t *)((uintptr_t)m->cpu +
                    b->address - m->iova);
                for (unsigned off = 0; off < b->bytes; off += 4) {
                    uint32_t v;
                    bcopy(fw + b->payload_offset + off, &v, 4);
                    to[off/4] = v;
                    if (to[off/4] != v) return false;
                }
                external_bytes += b->bytes; found = true; break;
            }
            if (!found) return false;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    printf("PERIDOT GMU: firmware upload/readback TCM-words=%u external-bytes=%u\n",
        tcm_words, external_bytes);
    return tcm_words == 135 && external_bytes == 79900;
}
static void pgcb_faults(vm_offset_t gmu, vm_offset_t smmu, const char *phase)
{
    uint64_t far = 0;
    uint32_t cb = 0x20000 + pgcb.bank * 0x1000;
    pgpu_ctx_read64((void *)smmu, cb + 0x60, &far);
    printf("PERIDOT GMU: %s reset=%#x fwinit=%#x hfi=%#x intr=%#x ao=%#x GFSR=%#x FSR=%#x FAR=%#llx SYNR=%#x\n",
        phase, pgcb_gread(gmu,0x1f800), pgcb_gread(gmu,0x1f81c),
        pgcb_gread(gmu,0x1f980), pgcb_gread(gmu,0x1f992),
        pgcb_gread(gmu,0x23b05), pgcb_read(smmu,0x48),
        pgcb_read(smmu,0x20000 + pgcb.bank * 0x1000 + 0x58), (unsigned long long)far,
        pgcb_read(smmu,cb + 0x68));
    uint32_t fsr = pgcb_read(smmu,0x20000 + pgcb.bank * 0x1000 + 0x58), syn = pgcb_read(smmu,cb + 0x68);
    if (!(fsr & 0xc00001feu)) return;
    printf("PERIDOT GMU: fault decode permission=%u translation=%u external=%u level=%u write=%u privileged=%u instruction=%u\n",
        !!(fsr & 8),!!(fsr & 2),!!(fsr & 16),syn & 3,
        !!(syn & 16),!!(syn & 32),!!(syn & 64));
}
static bool pgcb_exchange(struct pg_hfi *h, vm_offset_t gmu, vm_offset_t smmu)
{
    /* GET_VALUE/GMU_WARMBOOT is a real capability query used before
     * performance/start messages in the HWSCHED cold-start sequence. */
    const uint32_t command[] = {12u | (3u << 8) | (1u << 20), 125, 0};
    enum pg_hfi_status status = pg_hfi_send(h, command, 3);
    printf("PERIDOT GMU: GET_VALUE125 published status=%u\n", status);
    if (status != PG_HFI_OK) return false;
    for (unsigned n = 0; n < 20000; ++n) {
        uint32_t packet[255]; size_t words = 0;
        status = pg_hfi_receive(h, 1, packet, 255, &words);
        if (status == PG_HFI_OK) {
            printf("PERIDOT GMU: HFI reply words=%lu header=%#x request=%#x value=%#x\n",
                (unsigned long)words, packet[0], words > 1 ? packet[1] : 0,
                words > 2 ? packet[2] : 0);
            if (words >= 4 && ((packet[0] >> 16) & 15) == 1 &&
                (packet[0] & 255) == 12 && packet[1] == command[0]) {
                printf("PERIDOT GMU: GET_VALUE125 firmware-error=%u\n",packet[3]);
                return packet[3] == 0;
            }
        } else if (status != PG_HFI_EMPTY) {
            printf("PERIDOT GMU: response queue failed=%u\n", status); return false;
        }
        status = pg_hfi_receive(h, 2, packet, 255, &words);
        if (status == PG_HFI_OK) {
            printf("PERIDOT GMU: debug packet words=%lu header=%#x data=%#x\n",
                (unsigned long)words, packet[0], words > 1 ? packet[1] : 0);
        } else if (status != PG_HFI_EMPTY) return false;
        if (pgcb_read(smmu,0x48) || (pgcb_read(smmu,0x20000 + pgcb.bank * 0x1000 + 0x58) & 0xc00001feu)) break;
        pgcb_gwrite(gmu,0x1f991,pgcb_gread(gmu,0x1f992));
        delay(10);
    }
    printf("PERIDOT GMU: HFI ACK missing; cmd rd/wr=%u/%u rsp rd/wr=%u/%u\n",
        h->memory[16],h->memory[17],h->memory[28],h->memory[29]);
    return false;
}
#include "peridot_hwsched_prepare.c"
#include "peridot_runtime_memory.c"
static void pgpu_coldboot(vm_offset_t gmu, vm_offset_t smmu,
    const uint8_t *fw, const AdrenoFirmwareBlock *blocks, size_t count)
{
    if (pgcb.attempted++) return;
    /* peridot-u-oss reserves CB0/CB1 for GPU page-table switching and
     * starts non-GPU clients, including the GMU SID5, at context bank2. */
    pgcb.bank = 2;
    bool context = false, routed = false, released = false, exchanged = false;
    bool prepared = false, started = false;
    uint32_t mode = 0;
    PE_parse_boot_argn("peridotgpu", &mode, sizeof(mode));
    uint32_t saved_smr = 0, saved_s2cr = 0;
    vm_offset_t gpu = 0, rsc = 0, pdc = 0, cc = 0;
    ArmSmmu4KRange ranges[7];
    ArmSmmu4KConfig config = {32,36,0,mode == 19 ? 129+16384 : 129};
    ArmSmmu4KResult built = {0};
    ArmSmmu4KContextConfig cfg = {0};
    ArmSmmu4KIo io = {(void *)smmu,0x40000,pgpu_ctx_read32,pgpu_ctx_read64,
        pgpu_ctx_write32,pgpu_ctx_write64,pgpu_ctx_barrier};
    ArmSmmu4KProgramResult programmed = {0}, stopped = {0};
    struct pg_hfi hfi = {0};
    uint64_t scm_result = 0;
    int64_t scm_status;
    unsigned checkpoint = 0;
    PE_parse_boot_argn("peridotgmustop", &checkpoint, sizeof(checkpoint));
    printf("PERIDOT GMU: cold boot begin checkpoint=%u retained-until-reset=1\n",checkpoint);
    if (!pgpu_ctx_idle(smmu) || pgcb_gread(gmu,0x1f800) != 1 ||
        pgcb_gread(gmu,0x1f81c)) {
        printf("PERIDOT GMU: exclusive reset/idle precondition refused\n"); return;
    }
    const uint32_t addresses[] = {0x4000,0x100000,0x200000,0x300000,
        0x60000000,0x60002000,0x60020000,0,0};
    const uint32_t sizes[] = {81920,110592,77824,151552,8192,81920,16384,mode == 19 ? 262144u : 65536u,262144};
    for (unsigned i = 0; i < (mode == 19 ? 9u : 8u); ++i) {
        pgcb.memory[i].iova = addresses[i]; pgcb.memory[i].bytes = sizes[i];
        if (!pgcb_allocate(&pgcb.memory[i])) goto out;
        if (i < 7) ranges[i] = (ArmSmmu4KRange){addresses[i],pgcb.memory[i].physical,
            sizes[i],ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KExecute | ArmSmmu4KPrivileged,0,2};
    }
    config.table_physical = pgcb.memory[7].physical;
    ArmSmmu4KStatus status = ArmSmmu4KBuild(&config,ranges,7,
        pgcb.memory[7].cpu,pgcb.memory[7].bytes/4096,&built);
    bool walked = status == ArmSmmu4KSuccess && pgcb_walk(&built,ranges,7,&pgcb.memory[7]);
    printf("PERIDOT GMU: complete map status=%u walked=%u tables=%lu leaves=%lu\n",
        status,walked,(unsigned long)built.table_pages,(unsigned long)built.leaf_pages);
    if (!walked || !pgcb_load(gmu,fw,blocks,count)) goto out;
    hfi = (struct pg_hfi){pgcb.memory[5].cpu,pgcb.memory[5].bytes,
        0x60002000,(void *)gmu,pgpu_hfi_barrier,pgpu_hfi_notify,0};
    if (pg_hfi_initialize(&hfi) != PG_HFI_OK) goto out;
    printf("PERIDOT GMU: HFI17 ready uncached=1 firmware-version core=%#x HFI=%#x\n",
        pgcb_read(gmu,0xcfe0),pgcb_read(gmu,0xcff0));
    if (checkpoint == 1) goto out;
    cfg.lower = &config; cfg.lower_tables = &built; cfg.asid = pgcb.bank + 1;
    cfg.id0 = pgcb_read(smmu,0x20); cfg.id2 = pgcb_read(smmu,0x28);
    /* peridot-u-oss qsmmuv500_init_cb applies the matching qcom,actlr
     * entry to this bank and invalidates its TLB. Program it while disabled;
     * ProgramDisabledContext performs the subsequent bounded ASID sync. */
    pgcb_save_write(smmu,0x20000 + pgcb.bank * 0x1000 + 4,0x32b);
    uint32_t actlr = pgcb_read(smmu,0x20000 + pgcb.bank * 0x1000 + 4);
    printf("PERIDOT GMU: privileged firmware map bank=%u ASID=%u ACTLR=%#x expected=0x32b\n",
        pgcb.bank,cfg.asid,actlr);
    if (actlr != 0x32b) goto out;
    status = ArmSmmu4KProgramDisabledContext(&io,pgcb.bank,&cfg,10000,&programmed);
    context = programmed.must_retain_tables != 0;
    printf("PERIDOT GMU: complete map activate=%u enabled=%u readback=%u\n",
        status,programmed.enabled,programmed.readback_valid);
    if (status != ArmSmmu4KSuccess) goto out;
    saved_smr = pgcb_read(smmu,0x800); saved_s2cr = pgcb_read(smmu,0xc00);
    pgcb_write(smmu,0xc00,pgcb.bank); pgcb_write(smmu,0x800,0x80000005u); routed = true;
    if (pgcb_read(smmu,0xc00) != pgcb.bank || pgcb_read(smmu,0x800) != 0x80000005u) goto out;
    gpu = ml_io_map(0x3d00000,0x40000);
    rsc = ml_io_map(0x3d50000,0x10000); pdc = ml_io_map(0xb280000,0x10000);
    if (mode >= 18) cc = ml_io_map(0x3d90000,0xa000);
    if (!gpu || !rsc || !pdc || (mode >= 18 && !cc) || pgcb_read(rsc,0x101*4)) goto out;
    const uint32_t rsc_words[] = {4,11,14,13,16,15,18,17,256,8,9,10,340,341,342,343,344};
    const uint32_t rsc_values[] = {1u<<24,1,0,0,0,0,0x80000021,0,0,
        0x4520,0x4510,0x4514,0xeaaae5a0,0xe1a1ebab,0xa2e0a581,0xecac82e2,0x0020edad};
    pgcb_save_write(pdc,0x1148*4,0); pgcb_save_write(pdc,0x1140*4,0x80000001);
    for (unsigned i = 0; i < sizeof(rsc_words)/sizeof(rsc_words[0]); ++i)
        pgcb_save_write(rsc,rsc_words[i]*4,rsc_values[i]);
    printf("PERIDOT GMU: peridot PDC/RSC programmed sequence=0x154 bus-qos=0x71e20\n");
    scm_status = pgcb_scm(0x42000601,0x02002801,&scm_result);
    printf("PERIDOT GMU: SCM GPU_INIT availability status=%lld result=%#llx\n",
        (long long)scm_status,(unsigned long long)scm_result);
    if (scm_status || !scm_result) goto out;
    scm_status = pgcb_scm(0x42002801,1,&scm_result);
    printf("PERIDOT GMU: SCM GPU_INIT request=1 status=%lld result=%#llx\n",
        (long long)scm_status,(unsigned long long)scm_result);
    if (scm_status) goto out;
    const uint32_t words[] = {0x1f8f1,0x1f8f0,0x1f81c,0x1f801,0x1f985,
        0x1f984,0x23b11,0x1f82d,0x1f50b,0x1f9ce,0x1f9cf,0x1f9cd,
        0x1f40f,0x1f400,0x1f401,0x1f993,0x23b06};
    const uint32_t values[] = {1,1,0,2,0x60002000,1,
        0x80000000u | (0x32u<<18) | 0x8a0,0x4052,0x33,0,0x070b0000,
        0x60020003,1,1,1,UINT32_MAX,UINT32_MAX};
    for (unsigned i = 0; i < sizeof(words)/sizeof(words[0]); ++i)
        pgcb_save_write(gmu,words[i]*4-0x68000,values[i]);
    for (unsigned i = 0; i < 4; ++i) pgcb_save_write(gpu,(0x3c03+i)*4,0x71e20);
    pgcb_save_write(gpu,0x3c45*4,0);
    if (checkpoint == 2) goto out;
    __asm__ volatile("dsb sy" ::: "memory");
    released = true; pgcb_gwrite(gmu,0x1f800,0);
    bool ready = pgcb_poll(gmu,0x1f81c*4-0x68000,0x1ff,0x100,200000);
    pgcb_faults(gmu,smmu,ready ? "firmware ready" : "firmware timeout");
    if (!ready || checkpoint == 3) goto out;
    pgcb_gwrite(gmu,0x1f986,1);
    bool hfi_ready = pgcb_poll(gmu,0x1f980*4-0x68000,1,1,200000);
    printf("PERIDOT GMU: HFI control ready=%u\n",hfi_ready);
    if (hfi_ready) exchanged = pgcb_exchange(&hfi,gmu,smmu);
    if (exchanged && mode >= 18) prepared = pgcb_hwsched_prepare(&hfi,gmu,smmu);
    if (prepared && mode == 19)
        started = pgrt_start(&hfi,gmu,smmu,&pgcb_prepared_sequence,&built,&io,&cfg);
out:
    pgcb_faults(gmu,smmu,"before stop");
    if (released) {
        if (mode >= 18) {
            uint32_t gx = pgcb_read(cc,PGPU_GX_GDSCR);
            printf("PERIDOT HWSCHED: prepare=%u START=%u GX-GDSCR=%#x collapsed=%u\n",
                prepared,started,gx,pgpu_collapsed(gx));
            if (!pgpu_collapsed(gx))
                panic("PERIDOT HWSCHED unexpected active GX; retaining clocks and mappings");
        }
        pgcb_gwrite(gmu,0x1f800,1);
        if (!pgcb_poll(gmu,0x1f800*4-0x68000,UINT32_MAX,1,10000))
            panic("PERIDOT GMU reset unconfirmed; retaining memory and clocks");
        pgcb_write(gpu,0x3c45*4,2);
        if (!pgcb_poll(gpu,0x3c46*4,2,2,100000))
            panic("PERIDOT GMU bus drain unconfirmed; retaining memory and clocks");
        printf("PERIDOT GMU: CM3 reset and GBIF ARB halt acknowledged\n");
    }
    if (routed) {
        pgcb_write(smmu,0xc00,2u<<16); pgcb_write(smmu,0x800,saved_smr);
        if (pgcb_read(smmu,0xc00) != (2u<<16) || pgcb_read(smmu,0x800) != saved_smr)
            panic("PERIDOT GMU stream quarantine unconfirmed");
    }
    if (context) {
        ArmSmmu4KStatus off = ArmSmmu4KDisableOwnedContext(&io,pgcb.bank,&cfg,10000,&stopped);
        printf("PERIDOT GMU: context stop=%u disabled=%u sync=%u retain=%u\n",
            off,stopped.disabled_verified,stopped.tlb_synchronized,stopped.must_retain_tables);
        if (off != ArmSmmu4KSuccess || stopped.must_retain_tables)
            panic("PERIDOT GMU context stop unconfirmed; retaining memory and clocks");
    }
    if (routed) pgcb_write(smmu,0xc00,saved_s2cr);
    while (pgcb.saved_count) {
        const struct pgcb_saved *s = &pgcb.saved[--pgcb.saved_count];
        pgcb_write(s->base,s->offset,s->value);
    }
    printf("PERIDOT GMU: cold boot complete released=%u real-HFI-ACK=%u retained-until-reset=1\n",
        released,exchanged);
}
