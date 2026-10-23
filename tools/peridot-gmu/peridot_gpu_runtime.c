/* SPDX-License-Identifier: MIT */
/* Serialized raw-HWSCHED service for the first trusted iOS frontend.
 * Real mapped pages, owned CB0, acknowledged commands and GPU-written fences.
 * Separate task address spaces and preemption are not enabled on this path. */
#include <sys/errno.h>
#include "peridot_gpu.h"
#define PGDR_MAPS 1024u
#define PGDR_MAX_BYTES (256u<<20)
static struct {
    IOLock *lock;
    bool ready, poisoned;
    vm_offset_t gpu,gmu,smmu,cc,rsc;
    ArmSmmu4KConfig gmu_config;
    ArmSmmu4KResult gmu_tables;
    ArmSmmu4KContextConfig gmu_context;
    ArmSmmu4KIo io;
    struct pg_hfi hfi;
    unsigned sequence;
    uint32_t submitted, completed, mapped_bytes;
    struct { uint32_t va, bytes, count; uint64_t *pages; } maps[PGDR_MAPS];
} pgdr;

bool peridot_gpu_ready(void)
{
    return __atomic_load_n(&pgdr.ready,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&pgdr.poisoned,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&pgpu_clock_lease_held,__ATOMIC_ACQUIRE);
}
static bool pgdr_observe(void)
{
    ArmSmmu4KProgramResult state={0};
    return !pgcb_read(pgdr.smmu,0x48) &&
        !(pgcb_read(pgdr.smmu,0x20058)&0xc00001feu) &&
        !(pgcb_read(pgdr.smmu,0x22058)&0xc00001feu) &&
        !(pgcb_gread(pgdr.gmu,GEN7_GMU_GMU2HOST_INTR_INFO)&(1u<<15)) &&
        pgcb_poll(pgdr.gmu,GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS*4-0x68000,1u<<23,0,200000) &&
        ArmSmmu4KObserveOwnedContext(&pgdr.io,0,&pggx.context,&state)==ArmSmmu4KSuccess;
}
static void pgdr_poison(const char *reason)
{
    __atomic_store_n(&pgdr.poisoned,true,__ATOMIC_RELEASE);
    printf("PERIDOT GPU: service stopped accepting work: %s; all active backing remains pinned\n",reason);
}
static bool pgdr_sync(void)
{
    ArmSmmu4KProgramResult state={0};
    return ArmSmmu4KSynchronizeOwnedContext(&pgdr.io,0,&pggx.context,10000,&state)==ArmSmmu4KSuccess;
}
/* Every submission is synchronous under this lock. Thus no other host
 * submission can appear between the hardware idle observation and TLBI. */
uint64_t peridot_gpu_map(const uint64_t *pages, uint32_t count)
{
    if (!pages || !count || count>(64u<<20)/PAGE_SIZE || !peridot_gpu_ready()) return 0;
    uint64_t bytes=(uint64_t)count*PAGE_SIZE;
    IOLockLock(pgdr.lock);
    uint32_t slot=PGDR_MAPS, va=0x50000000;
    for (unsigned i=0;i<PGDR_MAPS;++i) if (!pgdr.maps[i].pages) {slot=i;break;}
    if (pgdr.poisoned || slot==PGDR_MAPS || bytes>PGDR_MAX_BYTES-pgdr.mapped_bytes) goto refuse;
    for (unsigned i=0;i<count;++i)
        if (!pages[i] || (pages[i]&PAGE_MASK) || pages[i]>=(UINT64_C(1)<<36) ||
            PAGE_SIZE>(UINT64_C(1)<<36)-pages[i]) goto refuse;
    for (unsigned pass=0;pass<=PGDR_MAPS;++pass) {
        bool conflict=false;
        if ((uint64_t)va+bytes>0xb0000000) goto refuse;
        for (unsigned i=0;i<PGDR_MAPS;++i) {
            if (!pgdr.maps[i].pages) continue;
            uint64_t start=pgdr.maps[i].va,end=start+pgdr.maps[i].bytes;
            if ((uint64_t)va<end && start<(uint64_t)va+bytes) {
                va=(uint32_t)((end+65535)&~UINT64_C(65535)); conflict=true; break;
            }
        }
        if (!conflict) break;
        if (pass==PGDR_MAPS) goto refuse;
    }
    uint64_t *copy=IOMalloc(count*sizeof(*copy));
    if (!copy) goto refuse;
    bcopy(pages,copy,count*sizeof(*copy));
    if (!pgdr_observe()) { IOFree(copy,count*sizeof(*copy)); pgdr_poison("map idle/context observation"); goto refuse; }
    /* Mark the lease before any PTE publication. An uncertain partial map
     * poisons the service; callers must not release their descriptor then. */
    pgdr.maps[slot].va=va; pgdr.maps[slot].bytes=(uint32_t)bytes;
    pgdr.maps[slot].count=count; pgdr.maps[slot].pages=copy;
    pgdr.mapped_bytes+=(uint32_t)bytes;
    for (unsigned i=0;i<count;++i) {
        ArmSmmu4KRange range={(uint64_t)va+(uint64_t)i*PAGE_SIZE,pages[i],PAGE_SIZE,
            ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KExecute,0,2};
        if (!pgrt_map_new(&pgcb.memory[8],&pgrt.gpu_tables,&range)) {
            panic("PERIDOT GPU partial map; retaining caller descriptors and clock lease");
        }
    }
    if (!pgdr_sync()) panic("PERIDOT GPU map TLB sync unconfirmed; retaining descriptors");
    IOLockUnlock(pgdr.lock);
    return va;
refuse:
    IOLockUnlock(pgdr.lock);
    return 0;
}
int peridot_gpu_unmap(uint64_t address)
{
    if (!peridot_gpu_ready()) return EIO;
    IOLockLock(pgdr.lock);
    unsigned slot=PGDR_MAPS;
    for (unsigned i=0;i<PGDR_MAPS;++i)
        if (pgdr.maps[i].pages && pgdr.maps[i].va==address) {slot=i;break;}
    if (slot==PGDR_MAPS) {IOLockUnlock(pgdr.lock);return EINVAL;}
    if (pgdr.poisoned || !pgdr_observe()) {
        pgdr_poison("unmap idle/context observation"); IOLockUnlock(pgdr.lock); return EIO;
    }
    const uint64_t attrs=(1u<<11)|(1u<<10)|(2u<<8)|(1u<<6)|3u;
    for (unsigned i=0;i<pgdr.maps[slot].bytes/4096;++i) {
        volatile uint64_t *leaf=NULL;
        uint64_t expected=pgdr.maps[slot].pages[i/(PAGE_SIZE/4096)]+(i%(PAGE_SIZE/4096))*4096+attrs;
        if (!pgrt_leaf(&pgcb.memory[8],&pgrt.gpu_tables,(uint32_t)address+i*4096,false,&leaf) ||
            !leaf || *leaf!=expected) panic("PERIDOT GPU unmap ownership mismatch; backing retained");
    }
    for (unsigned i=0;i<pgdr.maps[slot].bytes/4096;++i) {
        volatile uint64_t *leaf=NULL;
        if (!pgrt_leaf(&pgcb.memory[8],&pgrt.gpu_tables,(uint32_t)address+i*4096,false,&leaf) || !leaf)
            panic("PERIDOT GPU owned table disappeared");
        *leaf=0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (!pgdr_sync()) panic("PERIDOT GPU unmap TLB sync unconfirmed; backing retained");
    pgrt.gpu_tables.leaf_pages-=pgdr.maps[slot].bytes/4096;
    pgdr.mapped_bytes-=pgdr.maps[slot].bytes;
    IOFree(pgdr.maps[slot].pages,pgdr.maps[slot].count*sizeof(uint64_t));
    bzero(&pgdr.maps[slot],sizeof(pgdr.maps[slot]));
    IOLockUnlock(pgdr.lock);
    return 0;
}
static bool pgdr_range(uint64_t address, uint64_t bytes)
{
    if (!bytes) return false;
    for (unsigned i=0;i<PGDR_MAPS;++i) {
        uint64_t start=pgdr.maps[i].va,size=pgdr.maps[i].bytes;
        if (pgdr.maps[i].pages && address>=start && address-start<size && bytes<=size-(address-start)) return true;
    }
    return false;
}
int peridot_gpu_submit(const struct peridot_gpu_ib *ibs, uint32_t count, uint32_t *sequence)
{
    if (!ibs || !sequence || !count || count>32) return EINVAL;
    *sequence=0;
    if (!peridot_gpu_ready()) return EIO;
    IOLockLock(pgdr.lock);
    if (pgdr.poisoned || !pgdr_observe() || pgdr.submitted==0x7ffffffe) {
        pgdr_poison("submit idle/context observation or sequence exhaustion");IOLockUnlock(pgdr.lock);return EIO;
    }
    for (unsigned i=0;i<count;++i)
        if (ibs[i].reserved || !ibs[i].words || ibs[i].words>0xfffff || (ibs[i].address&3) ||
            !pgdr_range(ibs[i].address,(uint64_t)ibs[i].words*4)) {IOLockUnlock(pgdr.lock);return EINVAL;}
    uint32_t packet[150]={0},n=1,seq=++pgdr.submitted;
    /* Preserve a single BR execution path for these trusted raw batches. */
    packet[n++]=pggx_packet(0x17,1);packet[n++]=(1u<<27)|1u;
    for (unsigned i=0;i<count;++i) {
        packet[n++]=pggx_packet(0x3f,3);packet[n++]=(uint32_t)ibs[i].address;
        packet[n++]=(uint32_t)(ibs[i].address>>32);packet[n++]=ibs[i].words;
    }
    /* KGSL gen7 uses CACHE_CLEAN|BIT27 with a64-bit timestamp address.
     * This commits the completion only after cache writes reach memory. */
    packet[n++]=pggx_packet(0x46,4);packet[n++]=0x31u|(1u<<27);
    packet[n++]=pggx.fence_va+16;packet[n++]=0;packet[n++]=seq;
    packet[n++]=pggx_packet(0x26,0);
    bool ok=pggx_raw(&pgdr.hfi,pgdr.gpu,pgdr.gmu,pgdr.smmu,packet,n,&pgdr.sequence);
    volatile uint32_t *fence=pggx.fence.cpu;
    for (unsigned wait=0;ok && wait<100000;++wait) {
        __asm__ volatile("dmb oshld" ::: "memory");
        if (fence[4]==seq) break;
        delay(10);
    }
    ok=ok && fence[4]==seq && pgdr_observe();
    if (ok) {
        pgdr.completed=seq;*sequence=seq;
        if (seq<=8) printf("PERIDOT GPU: userspace batch=%u IBs=%u GPU-fence=%u completed=1\n",seq,count,fence[4]);
    }
    else pgdr_poison("command/fence completion");
    IOLockUnlock(pgdr.lock);
    return ok ? 0 : EIO;
}
int peridot_gpu_wait(uint32_t sequence, uint32_t timeout_ms)
{
    if (!sequence || !peridot_gpu_ready()) return EIO;
    if (timeout_ms>60000) timeout_ms=60000;
    for (unsigned elapsed=0;elapsed<=timeout_ms;++elapsed) {
        if (IOLockTryLock(pgdr.lock)) {
            bool bad=pgdr.poisoned,done=sequence<=pgdr.completed;
            bool future=sequence>pgdr.submitted;
            IOLockUnlock(pgdr.lock);
            if (bad) return EIO;
            if (done) return 0;
            if (future) return EINVAL;
        }
        if (elapsed==timeout_ms) break;
        IOSleep(1);
    }
    return ETIMEDOUT;
}
