/* SPDX-License-Identifier: MIT */
/* One owned gen7_11_0 cold GPU session. SM8635 KGSL register/PM4 contracts:
 * LineageOS ed4ad0d1ad00b755af2fe61387dcc5397ad20b8f; Linux qcom_mdt_load
 * and qcom_scm PAS13. All backing survives errors and stays pinned to reboot.
 * HFI completion alone is insufficient: the final CPU observation must find
 * two GPU-written values in an initially zero, separately owned NC page. */
#include "peridot_gen7_board.h"
static struct {
    struct pgcb_memory sqe, reglist, fence, metadata;
    ArmSmmu4KConfig config;
    ArmSmmu4KContextConfig context;
    uint32_t sqe_va, reglist_va, fence_va, saved_smr, saved_s2cr;
    uint32_t zap_file, zap_bytes, zap_memory;
    const uint8_t *zap;
    bool context_owned, routed, oob_issued, oob_ack, cp_ready, zap_ready, completed;
} pggx;

static uint32_t pggx_read(vm_offset_t gpu, vm_offset_t gmu, uint32_t word)
{
    if (word >= 0x1a000) return pgcb_gread(gmu,word);
    if (word >= 0x10000) panic("PERIDOT GPU register outside owned mapping");
    return pgcb_read(gpu,word*4);
}
static void pggx_write(vm_offset_t gpu, vm_offset_t gmu, uint32_t word, uint32_t value)
{
    if (word >= 0x1a000) { pgcb_gwrite(gmu,word,value); return; }
    if (word >= 0x10000) panic("PERIDOT GPU register outside owned mapping");
    pgcb_write(gpu,word*4,value);
}
static const uint8_t *pggx_firmware(const char *name, uint64_t expected)
{
    DTEntry map, chosen; const uint64_t *region, *base, *size;
    unsigned n = 0;
    if (SecureDTLookupEntry(NULL,"/chosen/memory-map",&map) != kSuccess ||
        SecureDTGetProperty(map,name,(const void **)&region,&n) != kSuccess || n != 16 ||
        SecureDTLookupEntry(NULL,"/chosen",&chosen) != kSuccess ||
        SecureDTGetProperty(chosen,"dram-base",(const void **)&base,&n) != kSuccess || n != 8 ||
        SecureDTGetProperty(chosen,"dram-size",(const void **)&size,&n) != kSuccess || n != 8)
        return NULL;
    uint64_t pa = region[0], bytes = region[1], rounded = (bytes+16383)&~UINT64_C(16383);
    if ((pa & 16383) || bytes != expected || pa < *base || pa-*base > *size ||
        rounded > *size-(pa-*base)) return NULL;
    return (const uint8_t *)ml_static_ptovirt(pa);
}
static uint32_t pggx_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint16_t pggx_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1]<<8);
}
/* Accept only the captured self-contained ELF32 image, including its header,
 * signature and one relocatable PT_LOAD. No split-file or address guessing. */
static bool pggx_prepare_zap(void)
{
    const uint8_t *fw = pggx_firmware("PeridotZAPFirmware",12088);
    if (!fw || pggx_u32(fw) != 0x464c457f || fw[4] != 1 || fw[5] != 1 ||
        fw[6] != 1 || pggx_u16(fw+16) != 2 || pggx_u16(fw+18) != 164 || pggx_u32(fw+20) != 1 ||
        pggx_u16(fw+40) != 52 || pggx_u16(fw+42) != 32 || pggx_u16(fw+44) != 3)
        return false;
    uint32_t ph = pggx_u32(fw+28);
    if (ph < 52 || ph > 12088-96) return false;
    uint32_t head = 0, hash = 0, hash_size = 0, loads = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const uint8_t *p = fw+ph+i*32;
        uint32_t type=pggx_u32(p), offset=pggx_u32(p+4), address=pggx_u32(p+12);
        uint32_t file=pggx_u32(p+16), memory=pggx_u32(p+20), flags=pggx_u32(p+24);
        if (offset > 12088 || file > 12088-offset) return false;
        if (i == 0) {
            if (type == 1 || offset || file < ph+96 || file > 4096) return false;
            head = file;
        } else if ((flags & 0x07000000) == 0x02000000) {
            if (hash_size || type == 1 || !file) return false;
            hash = offset; hash_size = file;
        } else if (type == 1) {
            if (++loads != 1 || !(flags & 0x08000000) || !file || file > memory ||
                memory > 4096 || (address & 4095)) return false;
            pggx.zap_file=offset; pggx.zap_bytes=file; pggx.zap_memory=memory;
        } else return false;
    }
    if (loads != 1 || head != 148 || hash_size != 3896 || head+hash_size != 4044)
        return false;
    pggx.metadata.bytes = 4096;
    if (!pgcb_allocate(&pggx.metadata)) return false;
    bcopy(fw,pggx.metadata.cpu,head);
    bcopy(fw+hash,(uint8_t *)pggx.metadata.cpu+head,hash_size);
    __asm__ volatile("dsb sy" ::: "memory");
    pggx.zap = fw;
    printf("PERIDOT GPU: ZAP parsed metadata=4044 load=%u memory=%u reserved=0x9899a000/0x2000\n",
        pggx.zap_bytes,pggx.zap_memory);
    return true;
}
static bool pggx_buffer(struct pgcb_memory *m, uint32_t bytes,
    unsigned permissions, uint32_t *va)
{
    if (!bytes || bytes > 0x100000 || pggx.context_owned) return false;
    m->bytes = (bytes+4095)&~4095u;
    if (!pgrt_reserve(&pgrt.gpu_next,m->bytes,65536,0x44000000,va) || !pgcb_allocate(m)) return false;
    ArmSmmu4KRange range = {*va,m->physical,m->bytes,permissions,0,2};
    return pgrt_map_new(&pgcb.memory[8],&pgrt.gpu_tables,&range);
}
static bool pggx_prepare(const ArmSmmu4KIo *io, vm_offset_t smmu)
{
    const uint8_t *sqe = pggx_firmware("PeridotSQEFirmware",66036);
    if (!sqe || !pggx_prepare_zap() ||
        !pggx_buffer(&pggx.sqe,66032,ArmSmmu4KRead|ArmSmmu4KPrivileged|ArmSmmu4KExecute,&pggx.sqe_va) ||
        !pggx_buffer(&pggx.reglist,4096,ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,&pggx.reglist_va) ||
        !pggx_buffer(&pggx.fence,4096,ArmSmmu4KRead|ArmSmmu4KWrite,&pggx.fence_va)) return false;
    bcopy(sqe+4,pggx.sqe.cpu,66032);
    __asm__ volatile("dsb sy" ::: "memory");
    if (bcmp(sqe+4,pggx.sqe.cpu,66032)) return false;
    pggx.config = (ArmSmmu4KConfig){32,36,pgcb.memory[8].physical,16384};
    pggx.context.lower=&pggx.config; pggx.context.lower_tables=&pgrt.gpu_tables;
    pggx.context.asid=1; pggx.context.id0=pgcb_read(smmu,0x20); pggx.context.id2=pgcb_read(smmu,0x28);
    /* This boot's initial full-SMMU inventory proved all streams invalid.
     * Recheck the exact new bank/slot before obtaining this separate lease. */
    pggx.saved_smr=pgcb_read(smmu,0x804); pggx.saved_s2cr=pgcb_read(smmu,0xc04);
    if ((pggx.saved_smr & 0x80000000u) || (pgcb_read(smmu,0x20000) & 1)) return false;
    ArmSmmu4KContextImage image = {0};
    if (ArmSmmu4KEncodeContext(&pggx.context,&image) != ArmSmmu4KSuccess) return false;
    unsigned info = 0;
    for (unsigned i=0; i<pgrt.count; ++i) {
        struct pgrt_buffer *b=&pgrt.buffers[i];
        if (b->requested[3] != 3) continue;
        /* The installed firmware requests exactly24 bytes, before the later
         * context_bank extension. Never write past that logical contract. */
        if (++info != 1 || b->requested[7] != 24 || b->memory.bytes < 24) return false;
        volatile uint32_t *v=b->memory.cpu;
        v[0]=0x241350d5; v[1]=0; v[2]=(uint32_t)image.ttbr0;
        v[3]=(uint32_t)(image.ttbr0>>32); v[4]=0xdecafbad; v[5]=0;
    }
    if (info != 1) return false;
    __asm__ volatile("dsb sy" ::: "memory");
    pgcb_save_write(smmu,0x20004,0x32b);
    if (pgcb_read(smmu,0x20004) != 0x32b) return false;
    ArmSmmu4KProgramResult programmed={0};
    ArmSmmu4KStatus status=ArmSmmu4KProgramDisabledContext(io,0,&pggx.context,10000,&programmed);
    pggx.context_owned=programmed.must_retain_tables != 0;
    printf("PERIDOT GPU: CB0 activate=%u enabled=%u readback=%u SQE=%#x bytes=66032 fence=%#x\n",
        status,programmed.enabled,programmed.readback_valid,pggx.sqe_va,pggx.fence_va);
    if (status != ArmSmmu4KSuccess) return false;
    pgcb_write(smmu,0xc04,0); pgcb_write(smmu,0x804,0x80000000u); pggx.routed=true;
    return pgcb_read(smmu,0xc04) == 0 && pgcb_read(smmu,0x804) == 0x80000000u;
}
static void pggx_registers(vm_offset_t gpu, vm_offset_t gmu)
{
#define GXW(r,v) pggx_write(gpu,gmu,(r),(v))
    for (unsigned i=0;i<sizeof(pggx_gbif)/sizeof(pggx_gbif[0]);++i) GXW(pggx_gbif[i][0],pggx_gbif[i][1]);
    /* Preserve the board QSB values already exercised by the GMU session. */
    for (unsigned i=0;i<4;++i) GXW(GEN7_GBIF_QSB_SIDE0+i,0x71e20);
    GXW(GEN7_UCHE_GBIF_GX_CONFIG,0x10240e0);
    GXW(GEN7_RBBM_PERFCTR_GPU_BUSY_MASKED,UINT32_MAX);
    GXW(GEN7_UCHE_TRAP_BASE_LO,0xfffff000); GXW(GEN7_UCHE_TRAP_BASE_HI,0x1ffff);
    GXW(GEN7_UCHE_WRITE_THRU_BASE_LO,0xfffff000); GXW(GEN7_UCHE_WRITE_THRU_BASE_HI,0x1ffff);
    GXW(GEN7_UCHE_GMEM_RANGE_MIN_LO,0x1000000); GXW(GEN7_UCHE_GMEM_RANGE_MIN_HI,0);
    GXW(GEN7_UCHE_GMEM_RANGE_MAX_LO,0x117ffff); GXW(GEN7_UCHE_GMEM_RANGE_MAX_HI,0);
    GXW(GEN7_UCHE_CACHE_WAYS,0x800000); GXW(GEN7_UCHE_CMDQ_CONFIG,0x6690e);
    /* Captured board HBB16, MAL32, UBWC4; client PF defaults to1 in KGSL. */
    GXW(GEN7_RB_NC_MODE_CNTL,0x816); GXW(GEN7_TPL1_NC_MODE_CNTL,6);
    GXW(GEN7_SP_NC_MODE_CNTL,0x26); GXW(GEN7_GRAS_NC_MODE_CNTL,0x60);
    GXW(GEN7_UCHE_MODE_CNTL,0x600000); GXW(GEN7_UCHE_CLIENT_PF,0x81);
    GXW(GEN7_RBBM_INTERFACE_HANG_INT_CNTL,0x40cfffff);
    GXW(GEN7_RB_CMP_DBG_ECO_CNTL,pggx_read(gpu,gmu,GEN7_RB_CMP_DBG_ECO_CNTL)|(1u<<11));
    GXW(GEN7_GPU_GMU_AO_GPU_CX_BUSY_MASK,0xff000000);
    GXW(GEN7_GMU_CX_GMU_POWER_COUNTER_SELECT_0,
        (pgcb_gread(gmu,GEN7_GMU_CX_GMU_POWER_COUNTER_SELECT_0)&~255u)|0x20);
    GXW(GEN7_GMU_CX_GMU_POWER_COUNTER_SELECT_1,4); GXW(GEN7_GMU_CX_GMU_POWER_COUNTER_ENABLE,1);
    GXW(GEN7_CP_PROTECT_CNTL,11);
    for (unsigned i=0;i<sizeof(pggx_protect)/sizeof(pggx_protect[0]);++i) GXW(pggx_protect[i][0],pggx_protect[i][1]);
    /* No LLCC slice lease exists. Leave its activation/control untouched. */
    GXW(GEN7_CP_APRIV_CNTL,0x6f); GXW(GEN7_CP_BV_APRIV_CNTL,0xf);
    GXW(GEN7_CP_CHICKEN_DBG,1); GXW(GEN7_CP_BV_CHICKEN_DBG,1);
    GXW(GEN7_RBBM_SECVID_TSB_CNTL,0); GXW(GEN7_RBBM_SECVID_TSB_TRUSTED_BASE_LO,0xf0000000);
    GXW(GEN7_RBBM_SECVID_TSB_TRUSTED_BASE_HI,0); GXW(GEN7_RBBM_SECVID_TSB_TRUSTED_SIZE,0x10000000);
    for (unsigned i=0;i<sizeof(pggx_hwcg)/sizeof(pggx_hwcg[0]);++i) GXW(pggx_hwcg[i][0],pggx_hwcg[i][1]);
    GXW(GEN7_RBBM_CLOCK_CNTL,0x8aa8aa82);
    /* Real16-byte cpu_gpu_lock plus static register pairs, dynamic count0.
     * Keep IFPC/preemption disabled while the first raw command is proven. */
    uint32_t *list=pggx.reglist.cpu;
    const unsigned ifpc=sizeof(pggx_ifpc_list)/sizeof(pggx_ifpc_list[0]);
    const unsigned preempt=sizeof(pggx_preempt_list)/sizeof(pggx_preempt_list[0]);
    list[3]=ifpc|(preempt<<8); unsigned next=4;
    for (unsigned i=0;i<ifpc;++i) { list[next++]=pggx_ifpc_list[i]; list[next++]=pggx_read(gpu,gmu,pggx_ifpc_list[i]); }
    for (unsigned i=0;i<preempt;++i) { list[next++]=pggx_preempt_list[i]; list[next++]=pggx_read(gpu,gmu,pggx_preempt_list[i]); }
    __asm__ volatile("dsb sy" ::: "memory");
    GXW(GEN7_CP_SQE_INSTR_BASE_LO,pggx.sqe_va); GXW(GEN7_CP_SQE_INSTR_BASE_HI,0);
    printf("PERIDOT GPU: gen7_11_0 registers/SQE configured reglist=%#x IFPC=%u preempt=%u words=%u\n",
        pggx.reglist_va,ifpc,preempt,next);
#undef GXW
}
static uint32_t pggx_packet(unsigned opcode, unsigned words)
{
    unsigned a=opcode,b=words;
    a^=a>>16; a^=a>>8; a^=a>>4;
    b^=b>>16; b^=b>>8; b^=b>>4;
    return 0x70000000u|words|(((0x9669u>>(b&15))&1)<<15)|(opcode<<16)|(((0x9669u>>(a&15))&1)<<23);
}
static bool pggx_raw(struct pg_hfi *h, vm_offset_t gpu, vm_offset_t gmu, vm_offset_t smmu,
    uint32_t *packet, size_t words, unsigned *sequence)
{
    packet[0]=131u|((uint32_t)words<<8);
    uint32_t value=0,error=0;
    bool ack=pgcb_request(h,gmu,smmu,packet,words,sequence,true,false,&value,&error);
    bool idle=ack && pgcb_poll(gmu,GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS*4-0x68000,1u<<23,0,200000);
    printf("PERIDOT GPU: raw ACK=%u idle=%u busy=%#x CB0-FSR=%#x CP-status=%#x/%#x RBBM-int=%#x\n",
        ack,idle,pgcb_gread(gmu,GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS),pgcb_read(smmu,0x20058),
        pggx_read(gpu,gmu,GEN7_CP_STATUS_1),pggx_read(gpu,gmu,GEN7_CP_CP2GMU_STATUS),
        pggx_read(gpu,gmu,GEN7_RBBM_INT_0_STATUS));
    if (pgcb_read(smmu,0x20058)&0xc00001feu) {
        uint64_t far=0;
        pgpu_ctx_read64((void *)smmu,0x20060,&far);
        printf("PERIDOT GPU: CB0 fault FAR=%#llx SYNR=%#x\n",(unsigned long long)far,pgcb_read(smmu,0x20068));
    }
    return idle && !(pgcb_read(smmu,0x20058)&0xc00001feu) && !pgcb_read(smmu,0x48);
}
static bool pggx_zap_call(uint32_t fn, uint32_t info, uint64_t b, uint64_t c)
{
    uint64_t result=UINT64_MAX;
    int64_t transport=pgcb_scm_args(fn,info,13,b,c,&result);
    printf("PERIDOT GPU: PAS13 function=%#x transport=%lld secure-result=%#llx\n",
        fn,(long long)transport,(unsigned long long)result);
    return !transport && !result;
}
static bool pggx_authenticate(void)
{
    if (!pggx.zap || !pggx.cp_ready ||
        !pggx_zap_call(0x42000201,0x82,pggx.metadata.physical,0) ||
        !pggx_zap_call(0x42000202,3,0x9899a000,4096)) return false;
    vm_offset_t memory=ml_io_map(0x9899a000,0x2000);
    if (!memory || ((pggx.zap_bytes|pggx.zap_memory)&3)) return false;
    for (unsigned off=0;off<pggx.zap_memory;off+=4) {
        uint32_t v=off<pggx.zap_bytes ? pggx_u32(pggx.zap+pggx.zap_file+off) : 0;
        pgcb_write(memory,off,v);
        if (pgcb_read(memory,off)!=v) return false;
    }
    pggx.zap_ready=pggx_zap_call(0x42000205,1,0,0);
    return pggx.zap_ready;
}
static void pggx_stop(struct pg_hfi *h, vm_offset_t gmu, vm_offset_t smmu,
    vm_offset_t cc, vm_offset_t rsc, const ArmSmmu4KIo *io, unsigned *sequence)
{
    if (pggx.oob_issued) {
        pgcb_gwrite(gmu,GEN7_GMU_HOST2GMU_INTR_SET,1u<<31);
        pgcb_gwrite(gmu,GEN7_GMU_CX_GMU_POWER_COUNTER_ENABLE,0);
        uint32_t value=0,error=0, slumber[]={33u|(3u<<8),1,1};
        bool ack=pgcb_request(h,gmu,smmu,slumber,3,sequence,true,false,&value,&error);
        pgcb_gwrite(gmu,GEN7_GPU_GMU_CX_GMU_CX_FALNEXT_INTF,1);
        bool idle=ack && pgcb_poll(gmu,GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS*4-0x68000,1u<<30,0,100000);
        printf("PERIDOT GPU: slumber ACK=%u idle=%u GX=%#x\n",ack,idle,pgcb_read(cc,PGPU_GX_GDSCR));
        if (!idle) panic("PERIDOT GPU slumber unconfirmed; retaining clocks and translations");
        pgcb_gwrite(gmu,GEN7_GMU_CM3_SYSRESET,1);
        pgcb_gwrite(gmu,GEN7_GMU_RSCC_CONTROL_REQ,1);
        /* RSC_STATUS0_DRV0 is word4. Word0x101 is SEQ_BUSY, a different
         * register which cannot acknowledge the sleep request's bit16. */
        bool sleep=pgcb_poll(rsc,4*4,1u<<16,1u<<16,100000);
        if (sleep) pgcb_gwrite(gmu,GEN7_GMU_RSCC_CONTROL_REQ,0);
        bool collapsed=sleep && pgcb_poll(cc,PGPU_GX_GDSCR,
            PGPU_ON|PGPU_COLLAPSE|PGPU_HW_CONTROL|PGPU_OVERRIDE,PGPU_COLLAPSE,100000);
        printf("PERIDOT GPU: RSC sleep=%u GX=%#x\n",sleep,pgcb_read(cc,PGPU_GX_GDSCR));
        if (!collapsed || !pgpu_collapsed(pgcb_read(cc,PGPU_GX_GDSCR)))
            panic("PERIDOT GPU collapse unconfirmed; retaining clocks and translations");
    }
    if (pggx.routed) {
        pgcb_write(smmu,0xc04,2u<<16); pgcb_write(smmu,0x804,pggx.saved_smr);
        if (pgcb_read(smmu,0xc04)!=(2u<<16) || pgcb_read(smmu,0x804)!=pggx.saved_smr)
            panic("PERIDOT GPU stream quarantine unconfirmed");
    }
    if (pggx.context_owned) {
        ArmSmmu4KProgramResult stopped={0};
        ArmSmmu4KStatus off=ArmSmmu4KDisableOwnedContext(io,0,&pggx.context,10000,&stopped);
        printf("PERIDOT GPU: CB0 stop=%u disabled=%u sync=%u\n",off,stopped.disabled_verified,stopped.tlb_synchronized);
        if (off!=ArmSmmu4KSuccess || stopped.must_retain_tables)
            panic("PERIDOT GPU context stop unconfirmed; retaining clocks and translations");
    }
    if (pggx.routed) pgcb_write(smmu,0xc04,pggx.saved_s2cr);
}
static bool pggx_boot(struct pg_hfi *h, vm_offset_t gpu, vm_offset_t gmu, vm_offset_t smmu,
    vm_offset_t cc, vm_offset_t rsc, const ArmSmmu4KIo *io, unsigned *sequence)
{
    if (!pgpu_collapsed(pgcb_read(cc,PGPU_GX_GDSCR))) return false;
    bool prepared=pggx_prepare(io,smmu);
    if (prepared) {
        uint32_t value=0,error=0,vote[]={30u|(4u<<8),1,1,1};
        prepared=pgcb_request(h,gmu,smmu,vote,4,sequence,true,false,&value,&error);
    }
    if (prepared) {
        pggx.oob_issued=true;
        pgcb_gwrite(gmu,GEN7_GMU_HOST2GMU_INTR_SET,1u<<30);
        pggx.oob_ack=pgcb_poll(gmu,GEN7_GMU_GMU2HOST_INTR_INFO*4-0x68000,1u<<31,1u<<31,100000);
        printf("PERIDOT GPU: OOB wake ACK=%u intr=%#x GX=%#x gfx=%#x\n",pggx.oob_ack,
            pgcb_gread(gmu,GEN7_GMU_GMU2HOST_INTR_INFO),pgcb_read(cc,PGPU_GX_GDSCR),
            pgcb_gread(gmu,GEN7_GMU_GFX_PWR_CLK_STATUS));
        pgcb_gwrite(gmu,GEN7_GMU_GMU2HOST_INTR_CLR,1u<<31);
    }
    if (pggx.oob_ack && !pgpu_collapsed(pgcb_read(cc,PGPU_GX_GDSCR))) {
        pggx_registers(gpu,gmu);
        uint32_t init[]={0,pggx_packet(0x17,1),1u<<27,pggx_packet(0x48,7),
            0x14b|(pgcb_dms_enabled ? 0 : 1u<<11),3,0x20000000,2,pggx.reglist_va,0,1u<<31};
        pggx.cp_ready=pggx_raw(h,gpu,gmu,smmu,init,11,sequence);
        if (pggx.cp_ready && pggx_authenticate()) {
            uint32_t unsecure[]={0,pggx_packet(0x66,1),0};
            if (pggx_raw(h,gpu,gmu,smmu,unsecure,3,sequence)) {
                /* Only the GPU receives these values; the host never stores
                 * either expected value into this observation page. */
                volatile uint32_t *fence=pggx.fence.cpu;
                if (fence[0] || fence[1]) panic("PERIDOT GPU fence was not initially zero");
                uint32_t write[]={0,pggx_packet(0x3d,4),pggx.fence_va,0,0x7350cafe,0xace01340,
                    pggx_packet(0x26,0)};
                bool idle=pggx_raw(h,gpu,gmu,smmu,write,7,sequence);
                for (unsigned wait=0;idle && wait<10000;++wait) {
                    __asm__ volatile("dsb sy" ::: "memory");
                    if (fence[0]==0x7350cafe && fence[1]==0xace01340) break;
                    delay(10);
                }
                pggx.completed=idle && fence[0]==0x7350cafe && fence[1]==0xace01340;
                printf("PERIDOT GPU: actual MEM_WRITE completion=%u observed=%#x/%#x\n",
                    pggx.completed,fence[0],fence[1]);
            }
        }
    }
    pggx_stop(h,gmu,smmu,cc,rsc,io,sequence);
    printf("PERIDOT GPU: session finished SQE=%u ZAP=%u memory-write=%u retained-until-reset=1\n",
        pggx.cp_ready,pggx.zap_ready,pggx.completed);
    return pggx.completed;
}
