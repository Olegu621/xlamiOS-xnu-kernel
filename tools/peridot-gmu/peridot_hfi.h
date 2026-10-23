/* SPDX-License-Identifier: MIT */
#ifndef PERIDOT_HFI_H
#define PERIDOT_HFI_H
#include <stdint.h>
#include <stddef.h>
enum pg_hfi_status {PG_HFI_OK,PG_HFI_ARGUMENT,PG_HFI_CORRUPT,PG_HFI_FULL,PG_HFI_EMPTY,PG_HFI_CAPACITY,PG_HFI_IO};
struct pg_hfi {
    volatile uint32_t *memory;
    size_t bytes;
    uint32_t iova;
    void *cookie;
    void (*barrier)(void *,unsigned publish);
    int (*notify)(void *);
    unsigned poisoned;
};
/* Caller supplies stopped-GMU, exclusively owned, CPU/GMU coherent or
 * noncached memory and real device barriers/notification. Initialization
 * cannot establish physical mappings or cache attributes. Storage remains
 * pinned until the GMU is stopped and SMMU transactions have drained. */
enum pg_hfi_status pg_hfi_initialize(struct pg_hfi *);
/* Complete wire packet including header; command queue0, max255 dwords.
 * A notification failure after publication is indeterminate and poisons it. */
enum pg_hfi_status pg_hfi_send(struct pg_hfi *,const uint32_t *,size_t);
/* Receive from response queue1 or debug queue2. Capacity failure preserves
 * the packet. Successful receive advances the real shared read index. */
enum pg_hfi_status pg_hfi_receive(struct pg_hfi *,unsigned,uint32_t *,size_t,size_t *);
#endif
