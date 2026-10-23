/* SPDX-License-Identifier: MIT */
#include "peridot_hfi.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static unsigned notifications;
static int fail_notify;
static void barrier(void *cookie,unsigned publish){(void)cookie;(void)publish;}
static int notify(void *cookie){(void)cookie;++notifications;return fail_notify;}
int main(void)
{
    uint32_t *m=aligned_alloc(4096,73728);assert(m);
    struct pg_hfi h={m,73728,0x60002000,0,barrier,notify,0};
    assert(pg_hfi_initialize(&h)==PG_HFI_OK);
    assert(m[1]==210 && m[4]==17 && m[6+16*12+1]==0x60002000+848+16*4096);
    assert(m[6+16*12+2]==(16|(4<<8)));
    uint32_t packet[]={1|(3<<8)|(1<<20),0x12345678,0x87654321};
    assert(pg_hfi_send(&h,packet,3)==PG_HFI_OK && notifications==1);
    assert(m[6+11]==4 && m[848/4]==packet[0] && m[848/4+2]==packet[2]);
    assert(pg_hfi_initialize(&h)==PG_HFI_OK);
    m[6+10]=4;m[6+11]=0;
    assert(pg_hfi_send(&h,packet,3)==PG_HFI_FULL && notifications==1);
    m[6+10]=1020;m[6+11]=1016;
    assert(pg_hfi_send(&h,packet,3)==PG_HFI_FULL);
    m[6+10]=8;m[6+11]=1020;
    assert(pg_hfi_send(&h,packet,3)==PG_HFI_OK && m[6+11]==0);
    unsigned qh=6+12,qd=848/4+1024;
    m[qh+10]=1020;m[qh+11]=0;
    for(unsigned i=0;i<3;++i)m[qd+1020+i]=packet[i];
    uint32_t out[8];size_t words;
    assert(pg_hfi_receive(&h,1,out,2,&words)==PG_HFI_CAPACITY && words==3 && m[qh+10]==1020);
    assert(pg_hfi_receive(&h,1,out,8,&words)==PG_HFI_OK && words==3 && out[2]==packet[2] && m[qh+10]==0);
    assert(pg_hfi_receive(&h,1,out,8,&words)==PG_HFI_EMPTY);
    m[qh+10]=1;
    assert(pg_hfi_receive(&h,1,out,8,&words)==PG_HFI_CORRUPT && h.poisoned);
    h.poisoned=0;assert(pg_hfi_initialize(&h)==PG_HFI_OK);
    m[qh+11]=4;m[qd]=8<<8;
    assert(pg_hfi_receive(&h,1,out,8,&words)==PG_HFI_CORRUPT);
    h.poisoned=0;assert(pg_hfi_initialize(&h)==PG_HFI_OK);fail_notify=1;
    assert(pg_hfi_send(&h,packet,3)==PG_HFI_IO && h.poisoned);
    assert(pg_hfi_send(&h,packet,3)==PG_HFI_CORRUPT);
    free(m);puts("PASS HFI17 layout, queue bounds, padding, wrap, capacity, corrupt queues and indeterminate notify");return 0;
}
