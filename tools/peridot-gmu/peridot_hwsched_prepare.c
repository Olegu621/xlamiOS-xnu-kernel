/* SPDX-License-Identifier: MIT */
/* HFI2.6.1 configuration phase, before START's memory requests and GPU OOB.
 * qualcomm-linux/kgsl 2b2d762c0b843c560d2ae2203f6b3cfe1474fb3f:
 * adreno_gen7_hfi.c and adreno_gen7_hwsched_hfi.c. */
#include "peridot_power_tables.h"

static bool pgcb_request(struct pg_hfi *h, vm_offset_t gmu, vm_offset_t smmu,
    const uint32_t *source, size_t count, unsigned *sequence,
    bool zero_result, bool v5_error, uint32_t *value, uint32_t *error)
{
    uint32_t wire[255];
    *value = *error = UINT32_MAX;
    if (!source || !count || count > 255 || *sequence >= 4095 ||
        ((source[0] >> 8) & 255) != count || (source[0] & 0xffff0000u)) return false;
    bcopy(source,wire,count * sizeof(uint32_t));
    wire[0] |= ++*sequence << 20;
    enum pg_hfi_status sent = pg_hfi_send(h,wire,count);
    printf("PERIDOT HWSCHED: command=%u sequence=%u words=%lu publish=%u\n",
        wire[0] & 255,*sequence,(unsigned long)count,sent);
    if (sent != PG_HFI_OK) return false;
    unsigned messages = 0;
    for (unsigned poll = 0; poll < 100000; ++poll) {
        uint32_t packet[255]; size_t words = 0;
        enum pg_hfi_status status = pg_hfi_receive(h,1,packet,255,&words);
        if (status == PG_HFI_OK) {
            if (++messages > 64) return false;
            if (((packet[0] >> 16) & 15) == 1) {
                if (words < (v5_error ? 4u : 3u) || packet[1] != wire[0] ||
                    (packet[0] & 255) != (wire[0] & 255)) {
                    printf("PERIDOT HWSCHED: ACK mismatch words=%lu expected=%#x received=%#x\n",
                        (unsigned long)words,wire[0],words > 1 ? packet[1] : 0);
                    return false;
                }
                /* KGSL uses the original three-word ACK for PERF/BW and
                 * CORE_FW_START. Only its v5 feature/value API requires
                 * the separate error field, even on HFI2.6 firmware. */
                *value = packet[2]; *error = v5_error ? packet[3] : 0;
                printf("PERIDOT HWSCHED: ACK command=%u words=%lu v5=%u result=%#x error=%u\n",
                    wire[0] & 255,(unsigned long)words,v5_error,*value,*error);
                return !*error && (!zero_result || !*value);
            }
            printf("PERIDOT HWSCHED: asynchronous message id=%u words=%lu data=%#x/%#x\n",
                packet[0] & 255,(unsigned long)words,
                words > 1 ? packet[1] : 0,words > 2 ? packet[2] : 0);
            /* START is a later phase. No memory allocation is acknowledged
             * here without a real backed GMU/GPU mapping and descriptor. */
            if ((packet[0] & 255) != 101) return false;
        } else if (status != PG_HFI_EMPTY) return false;
        status = pg_hfi_receive(h,2,packet,255,&words);
        if (status == PG_HFI_OK) {
            if (++messages > 64) return false;
            printf("PERIDOT HWSCHED: debug id=%u words=%lu data=%#x\n",
                packet[0] & 255,(unsigned long)words,words > 1 ? packet[1] : 0);
            if ((packet[0] & 255) == 100) return false;
        } else if (status != PG_HFI_EMPTY) return false;
        if (pgcb_read(smmu,0x48) ||
            (pgcb_read(smmu,0x20000 + pgcb.bank*0x1000 + 0x58) & 0xc00001feu) ||
            (pgcb_gread(gmu,0x1f992) & (1u<<15))) return false;
        pgcb_gwrite(gmu,0x1f991,pgcb_gread(gmu,0x1f992));
        delay(10);
    }
    printf("PERIDOT HWSCHED: command=%u timed out; cmd rd/wr=%u/%u response rd/wr=%u/%u\n",
        wire[0] & 255,h->memory[16],h->memory[17],h->memory[28],h->memory[29]);
    return false;
}

static unsigned pgcb_prepared_sequence;
static bool pgcb_dms_enabled;
static bool pgcb_hwsched_prepare(struct pg_hfi *h, vm_offset_t gmu, vm_offset_t smmu)
{
    unsigned sequence = 1, acknowledged = 0;
    uint32_t value = 0, error = 0;
    if (pgcb_read(gmu,0xcff0) != 0x20060001) {
        printf("PERIDOT HWSCHED: exact HFI2.6.1 configuration required\n"); return false;
    }
    printf("PERIDOT HWSCHED: prepare actual peridot PERF49/BW160, HFI2.6.1\n");
    if (!pgcb_request(h,gmu,smmu,pgcb_performance,
        sizeof(pgcb_performance)/sizeof(pgcb_performance[0]),&sequence,true,false,&value,&error)) return false;
    ++acknowledged;
    if (!pgcb_request(h,gmu,smmu,pgcb_bandwidth,
        sizeof(pgcb_bandwidth)/sizeof(pgcb_bandwidth[0]),&sequence,true,false,&value,&error)) return false;
    ++acknowledged;
    /* GX OOB, CP/SQE/ZAP and the memory-request phase follow separately.
     * Keep IFPC off until that driver owns GX power and its idle protocol. */
    const uint32_t features[][4] = {
        {11u | (4u<<8),9,0,0},
        {11u | (4u<<8),1,1,0},
        {11u | (4u<<8),16,1,0},
        {11u | (4u<<8),27,1,0},
    };
    for (unsigned i = 0; i < sizeof(features)/sizeof(features[0]); ++i) {
        bool ok = pgcb_request(h,gmu,smmu,features[i],4,&sequence,true,true,&value,&error);
        if (!ok && features[i][1] == 27 && error == 8) {
            printf("PERIDOT HWSCHED: DMS unavailable, matching KGSL optional-feature fallback\n");
            continue;
        }
        if (!ok) return false;
        if (features[i][1] == 27) pgcb_dms_enabled = true;
        ++acknowledged;
    }
    const uint32_t core_start[] = {14u | (2u<<8),0};
    if (!pgcb_request(h,gmu,smmu,core_start,2,&sequence,true,false,&value,&error)) return false;
    ++acknowledged;
    const uint32_t properties[] = {121,122};
    for (unsigned i = 0; i < sizeof(properties)/sizeof(properties[0]); ++i) {
        uint32_t query[] = {12u | (3u<<8),properties[i],0};
        if (!pgcb_request(h,gmu,smmu,query,3,&sequence,false,true,&value,&error)) return false;
        printf("PERIDOT HWSCHED: capability=%u value=%#x\n",properties[i],value);
        ++acknowledged;
    }
    printf("PERIDOT HWSCHED: core preparation acknowledged=%u; START/GPU submission pending\n",
        acknowledged);
    pgcb_prepared_sequence = sequence;
    return true;
}
