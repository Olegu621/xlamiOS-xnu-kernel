/* SPDX-License-Identifier: MIT */
/* START-time owned buffers. Only invalid descriptors are published; firmware
 * receives each new VA after its backing, table writes and ASID sync complete.
 * No valid translation, root or existing buffer is replaced while CM3 runs.
 * Allocation ABI: qualcomm-linux/kgsl adreno_hwsched.c, commit
 * 2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f. New-only PTE publication follows
 * upstream arm-lpae invalid-to-valid descriptor installation. */
#define PGRT_MAX_BUFFERS 48u
#define PGRT_MAX_BYTES 0x04000000u
#define PGRT_PA_MASK UINT64_C(0x0000fffffffff000)
struct pgrt_buffer {
    uint32_t requested[9], descriptor[9];
    struct pgcb_memory memory;
};
static struct {
    unsigned count;
    uint32_t bytes, cached_next, nc_next, gpu_next;
    struct pgrt_buffer buffers[PGRT_MAX_BUFFERS];
    ArmSmmu4KResult gpu_tables;
} pgrt;

static bool pgrt_store(volatile uint64_t *slot, uint64_t value)
{
    if (*slot) return false;
    __asm__ volatile("dmb oshst" ::: "memory");
    *slot = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return *slot == value;
}
/* Return a leaf slot in an owned, append-only table pool. Missing intermediate
 * pages are zeroed and published before the aligned parent descriptor store. */
static bool pgrt_leaf(struct pgcb_memory *pool, ArmSmmu4KResult *tables,
    uint32_t va, bool create, volatile uint64_t **leaf)
{
    *leaf = NULL;
    if (!pool->cpu || tables->root_physical != pool->physical ||
        tables->start_level != 1 || !tables->table_pages ||
        tables->table_pages > pool->bytes/4096) return false;
    uint64_t physical = pool->physical;
    for (unsigned level = 1; level < 3; ++level) {
        if (physical < pool->physical ||
            physical - pool->physical >= tables->table_pages * 4096) return false;
        volatile uint64_t *page = (volatile uint64_t *)((uintptr_t)pool->cpu +
            physical - pool->physical);
        volatile uint64_t *slot = page + ((va >> (39-9*level)) & 511);
        uint64_t desc = *slot;
        if (!desc) {
            if (!create) return true;
            if (tables->table_pages == pool->bytes/4096) return false;
            physical = pool->physical + tables->table_pages * 4096;
            bzero((void *)((uintptr_t)pool->cpu + physical - pool->physical),4096);
            ++tables->table_pages;
            if (!pgrt_store(slot,physical | 3)) return false;
        } else {
            physical = desc & PGRT_PA_MASK;
            if (desc != (physical | 3)) return false;
        }
    }
    if (physical < pool->physical ||
        physical - pool->physical >= tables->table_pages * 4096) return false;
    *leaf = (volatile uint64_t *)((uintptr_t)pool->cpu + physical - pool->physical) +
        ((va >> 12) & 511);
    return true;
}
static bool pgrt_map_new(struct pgcb_memory *pool, ArmSmmu4KResult *tables,
    const ArmSmmu4KRange *range)
{
    if (!range->bytes || ((range->iova | range->physical | range->bytes) & 4095) ||
        range->iova >= UINT64_C(0x100000000) ||
        range->bytes > UINT64_C(0x100000000) - range->iova ||
        range->physical >= (UINT64_C(1)<<36) ||
        range->bytes > (UINT64_C(1)<<36) - range->physical ||
        range->bytes > PGRT_MAX_BYTES ||
        !(range->permissions & ArmSmmu4KRead) || (range->permissions & ~15u) ||
        range->writeback || range->shareability != 2 ||
        (range->physical < pool->physical + pool->bytes &&
         pool->physical < range->physical + range->bytes)) return false;
    /* Refuse any pre-existing leaf before publishing the first new one. */
    for (uint64_t off = 0; off < range->bytes; off += 4096) {
        volatile uint64_t *leaf;
        if (!pgrt_leaf(pool,tables,(uint32_t)(range->iova+off),false,&leaf) ||
            (leaf && *leaf)) return false;
    }
    uint64_t attrs = (1u<<11) | (1u<<10) | (2u<<8) | 3u;
    if (!(range->permissions & ArmSmmu4KExecute)) attrs |= UINT64_C(3)<<53;
    if (!(range->permissions & ArmSmmu4KPrivileged)) attrs |= 1u<<6;
    if (!(range->permissions & ArmSmmu4KWrite)) attrs |= 1u<<7;
    for (uint64_t off = 0; off < range->bytes; off += 4096) {
        volatile uint64_t *leaf;
        if (!pgrt_leaf(pool,tables,(uint32_t)(range->iova+off),true,&leaf) ||
            !leaf || !pgrt_store(leaf,(range->physical+off) | attrs)) return false;
    }
    tables->leaf_pages += range->bytes/4096;
    return pgcb_walk(tables,range,1,pool);
}
static bool pgrt_reserve(uint32_t *cursor, uint32_t bytes, unsigned alignment,
    uint32_t limit, uint32_t *address)
{
    uint64_t start = ((uint64_t)*cursor + alignment - 1) & ~(uint64_t)(alignment-1);
    if (start >= limit || bytes > limit-start || limit-start-bytes < 4096) return false;
    *address = (uint32_t)start;
    *cursor = (uint32_t)(start+bytes+4096);
    return true;
}
static bool pgrt_allocate(const uint32_t *packet, size_t words, uint32_t *descriptor,
    ArmSmmu4KResult *gmu_tables, const ArmSmmu4KIo *io,
    const ArmSmmu4KContextConfig *context)
{
    if (words < 2 || packet[1] > 1 || words != (packet[1] ? 11u : 10u)) return false;
    uint32_t request[9] = {0};
    bcopy(packet+2,request,(words-2)*4);
    uint32_t flags = request[2], kind = request[3], size = request[7], align = request[8];
    printf("PERIDOT ALLOC: request version=%u kind=%u handle=%u flags=%#x size=%#x align=%#x\n",
        packet[1],kind,request[5],flags,size,align);
    for (unsigned i = 0; i < pgrt.count; ++i) {
        const struct pgrt_buffer *old = &pgrt.buffers[i];
        if (old->requested[3] != kind || old->requested[5] != request[5]) continue;
        const uint32_t *original = old->requested, *mapped = old->descriptor;
        if ((request[2] != original[2] && request[2] != mapped[2]) ||
            (request[7] != original[7] && request[7] != mapped[7]) ||
            request[8] != original[8] ||
            ((request[0] || request[1]) &&
             (request[0] != mapped[0] || request[1] != mapped[1])) ||
            (request[6] && request[6] != mapped[6]) ||
            (request[4] != original[4] && request[4] != mapped[4])) return false;
        bcopy(mapped,descriptor,36); return true;
    }
    if (pgrt.count >= PGRT_MAX_BUFFERS || !size || kind >= 28 ||
        (flags & ~0x13ffu) || (align & 0xffff0000u) ||
        request[0] || request[1] || request[6]) return false;
    /* Register windows and hardware-fence resources require actual platform
     * ownership. Do not answer those requests with ordinary RAM. */
    if (kind == 11 || kind == 12 || kind == 13 || kind == 19 || kind == 20 ||
        kind == 23 || kind == 26) {
        printf("PERIDOT ALLOC: platform resource kind=%u requires its real owner\n",kind);
        return false;
    }
    struct pgrt_buffer *buffer = &pgrt.buffers[pgrt.count];
    bcopy(request,buffer->requested,36);
    bcopy(request,buffer->descriptor,36);
    /* KGSL's non-secure context policy also backs the secure preemption slot
     * with one non-secure native page and reports the corrected flags/size. */
    if (flags & 0x1000u) {
        if (kind != 5) return false;
        flags &= ~0x1000u; size = 4096;
    }
    unsigned va_shift = align & 255, size_shift = (align>>8) & 255;
    if (va_shift > 24 || size_shift > 24) return false;
    unsigned va_align = 1u << (va_shift > 12 ? va_shift : 12);
    unsigned sz_align = 1u << (size_shift > 12 ? size_shift : 12);
    uint64_t rounded = ((uint64_t)size + sz_align-1) & ~(uint64_t)(sz_align-1);
    if (rounded > PGRT_MAX_BYTES-pgrt.bytes) return false;
    size = (uint32_t)rounded;
    uint32_t gmu_va = 0, gpu_va = 0;
    bool gmu_mapping = (flags & 8) || !(flags & 1);
    if (gmu_mapping) {
        uint32_t *cursor = (flags & 128) ? &pgrt.cached_next : &pgrt.nc_next;
        if (!pgrt_reserve(cursor,size,va_align,(flags & 128) ? 0x1000000 : 0x64030000,&gmu_va)) return false;
    }
    if (flags & 1) {
        if (!pgrt_reserve(&pgrt.gpu_next,size,va_align > 65536 ? va_align : 65536,
            0x44000000,&gpu_va)) return false;
    }
    buffer->memory.iova = gmu_va;
    buffer->memory.bytes = size;
    if (!pgcb_allocate(&buffer->memory)) return false;
    if (flags & 1) {
        unsigned permissions = ArmSmmu4KRead | ArmSmmu4KExecute;
        if (flags & 2) permissions |= ArmSmmu4KPrivileged;
        if (flags & 4) permissions |= ArmSmmu4KWrite;
        ArmSmmu4KRange range = {gpu_va,buffer->memory.physical,size,permissions,0,2};
        if (!pgrt_map_new(&pgcb.memory[8],&pgrt.gpu_tables,&range)) return false;
    }
    if (gmu_mapping) {
        ArmSmmu4KProgramResult observed = {0};
        if (ArmSmmu4KObserveOwnedContext(io,pgcb.bank,context,&observed) != ArmSmmu4KSuccess) return false;
        unsigned permissions = ArmSmmu4KRead | ArmSmmu4KExecute;
        /* KGSL reserves GMU-only kernel blocks as RW/privileged. Shared
         * GPU imports instead use the individual GMU access flags. */
        if (!(flags & 1) || (flags & 16)) permissions |= ArmSmmu4KPrivileged;
        if (!(flags & 1) || (flags & 32)) permissions |= ArmSmmu4KWrite;
        ArmSmmu4KRange range = {gmu_va,buffer->memory.physical,size,permissions,0,2};
        if (!pgrt_map_new(&pgcb.memory[7],gmu_tables,&range)) return false;
        /* CM3 continues using unchanged mappings. The requested new range has
         * never been advertised and cannot be consumed before our HFI ACK. */
        ArmSmmu4KProgramResult synced = {0};
        if (ArmSmmu4KSyncContextTlb(io,observed.context_offset,context->asid,0,10000,&synced) != ArmSmmu4KSuccess ||
            ArmSmmu4KObserveOwnedContext(io,pgcb.bank,context,&observed) != ArmSmmu4KSuccess) return false;
    }
    uint32_t *out = buffer->descriptor;
    out[0] = gpu_va; out[1] = 0; out[2] = flags; out[4] = request[5];
    out[6] = gmu_va; out[7] = size;
    pgrt.bytes += size; ++pgrt.count;
    bcopy(out,descriptor,36);
    printf("PERIDOT ALLOC: backed kind=%u handle=%u GMU=%#x GPU=%#x bytes=%#x total=%#x GPU-context-active=0\n",
        kind,request[5],gmu_va,gpu_va,size,pgrt.bytes);
    return true;
}
static bool pgrt_start(struct pg_hfi *h, vm_offset_t gmu, vm_offset_t smmu,
    unsigned *sequence, ArmSmmu4KResult *tables, const ArmSmmu4KIo *io,
    const ArmSmmu4KContextConfig *context)
{
    if (pgrt.count || !pgcb.memory[8].cpu || *sequence >= 4094) return false;
    pgrt.cached_next = 0x400000; pgrt.nc_next = 0x60030000;
    pgrt.gpu_next = 0x40000000;
    pgrt.gpu_tables = (ArmSmmu4KResult){pgcb.memory[8].physical,0xff44,1,0,1,0};
    printf("PERIDOT HWSCHED: runtime allocation START, inactive GPU root=%#llx\n",
        (unsigned long long)pgrt.gpu_tables.root_physical);
    uint32_t start = 10u | (1u<<8) | (++*sequence<<20);
    if (pg_hfi_send(h,&start,1) != PG_HFI_OK) return false;
    unsigned messages = 0;
    for (unsigned poll = 0; poll < 500000; ++poll) {
        uint32_t packet[255]; size_t words = 0;
        enum pg_hfi_status status = pg_hfi_receive(h,1,packet,255,&words);
        if (status == PG_HFI_OK) {
            if (++messages > 256 || *sequence >= 4094) return false;
            unsigned id = packet[0] & 255, type = (packet[0]>>16) & 15;
            if (type == 1) {
                if (words < 3 || id != 10 || packet[1] != start) return false;
                printf("PERIDOT HWSCHED: START ACK words=%lu result=%#x allocations=%u bytes=%#x\n",
                    (unsigned long)words,packet[2],pgrt.count,pgrt.bytes);
                return packet[2] == 0;
            }
            if (type != 0) return false;
            if (id == 20) {
                uint32_t reply[11] = {20u | (11u<<8) | (1u<<16),packet[0]};
                if (!pgrt_allocate(packet,words,reply+2,tables,io,context)) {
                    printf("PERIDOT HWSCHED: allocation refused without ACK; retained until reset\n");
                    return false;
                }
                reply[0] |= ++*sequence<<20;
                if (pg_hfi_send(h,reply,11) != PG_HFI_OK) return false;
            } else if (id == 110) {
                if (words != 3) return false;
                /* KGSL explicitly permits lo=hi=0 to report that a requested
                 * performance counter was not allocated. It is not a counter. */
                uint32_t reply[] = {110u | (6u<<8) | (1u<<16) | (++*sequence<<20),
                    packet[0],packet[1],packet[2],0,0};
                if (pg_hfi_send(h,reply,6) != PG_HFI_OK) return false;
                printf("PERIDOT HWSCHED: counter unavailable group=%u countable=%u\n",packet[1],packet[2]);
            } else if (id == 155) {
                printf("PERIDOT HWSCHED: process-trace notification words=%lu\n",(unsigned long)words);
            } else {
                printf("PERIDOT HWSCHED: unexpected START message id=%u words=%lu\n",id,(unsigned long)words);
                return false;
            }
        } else if (status != PG_HFI_EMPTY) return false;
        status = pg_hfi_receive(h,2,packet,255,&words);
        if (status == PG_HFI_OK) {
            printf("PERIDOT HWSCHED: START debug id=%u words=%lu\n",packet[0]&255,(unsigned long)words);
            if ((packet[0]&255) == 100) return false;
        } else if (status != PG_HFI_EMPTY) return false;
        if (pgcb_read(smmu,0x48) ||
            (pgcb_read(smmu,0x20000+pgcb.bank*0x1000+0x58) & 0xc00001feu) ||
            (pgcb_gread(gmu,0x1f992) & (1u<<15))) return false;
        pgcb_gwrite(gmu,0x1f991,pgcb_gread(gmu,0x1f992));
        delay(10);
    }
    printf("PERIDOT HWSCHED: START timeout buffers=%u bytes=%#x\n",pgrt.count,pgrt.bytes);
    return false;
}
