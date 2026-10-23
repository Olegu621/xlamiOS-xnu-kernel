/* SPDX-License-Identifier: MIT */
#include "peridot_hfi.h"
#define PG_HFI_QUEUES 17u
#define PG_HFI_WORDS 1024u
#define PG_HFI_DATA_OFFSET 848u
#define PG_HFI_MIN_BYTES (PG_HFI_DATA_OFFSET+4096u*PG_HFI_QUEUES)
static enum pg_hfi_status corrupt(struct pg_hfi *h)
{ h->poisoned=1;return PG_HFI_CORRUPT; }
static unsigned priority(unsigned q)
{ return q<7?0:q<11?1:q<14?2:q<16?3:4; }
static int arguments(const struct pg_hfi *h)
{
    return h && h->memory && !((uintptr_t)h->memory&4095) && h->bytes>=PG_HFI_MIN_BYTES &&
        h->bytes<=UINT32_MAX && h->iova && !(h->iova&4095) &&
        h->bytes<=UINT64_C(0x100000000)-h->iova && h->barrier && h->notify;
}
enum pg_hfi_status pg_hfi_initialize(struct pg_hfi *h)
{
    if(!arguments(h))return PG_HFI_ARGUMENT;
    if(h->poisoned)return PG_HFI_CORRUPT;
    for(size_t i=0;i<h->bytes/4;++i)h->memory[i]=0;
    volatile uint32_t *m=h->memory;
    m[0]=0;m[1]=210;m[2]=6;m[3]=12;m[4]=17;m[5]=17;
    for(unsigned q=0;q<PG_HFI_QUEUES;++q) {
        volatile uint32_t *v=m+6+12*q;
        v[0]=1;v[1]=h->iova+PG_HFI_DATA_OFFSET+4096*q;
        v[2]=q|(priority(q)<<8);v[3]=PG_HFI_WORDS;
    }
    h->barrier(h->cookie,1);return PG_HFI_OK;
}
static enum pg_hfi_status queue(struct pg_hfi *h,unsigned q,
    volatile uint32_t **header,volatile uint32_t **data,uint32_t *rd,uint32_t *wr)
{
    if(!arguments(h) || q>=PG_HFI_QUEUES)return PG_HFI_ARGUMENT;
    if(h->poisoned)return PG_HFI_CORRUPT;
    h->barrier(h->cookie,0);
    volatile uint32_t *m=h->memory,*v=m+6+12*q;
    if(m[0] || m[1]!=210 || m[2]!=6 || m[3]!=12 || m[4]!=17 || m[5]!=17 ||
        v[0]!=1 || v[1]!=h->iova+PG_HFI_DATA_OFFSET+4096*q ||
        v[2]!=(q|(priority(q)<<8)) || v[3]!=PG_HFI_WORDS || v[4])return corrupt(h);
    *rd=v[10];*wr=v[11];
    if(*rd>=PG_HFI_WORDS || *wr>=PG_HFI_WORDS || ((*rd|*wr)&3))return corrupt(h);
    *header=v;*data=m+PG_HFI_DATA_OFFSET/4+q*PG_HFI_WORDS;
    return PG_HFI_OK;
}
enum pg_hfi_status pg_hfi_send(struct pg_hfi *h,const uint32_t *packet,size_t words)
{
    if(!packet || !words || words>255 || ((packet[0]>>8)&255)!=words)return PG_HFI_ARGUMENT;
    volatile uint32_t *v,*data;uint32_t rd,wr;
    enum pg_hfi_status status=queue(h,0,&v,&data,&rd,&wr);if(status)return status;
    unsigned padded=((unsigned)words+3)&~3u;
    if(padded>((rd-wr-1)&1023))return PG_HFI_FULL;
    for(unsigned i=0;i<padded;++i)data[(wr+i)&1023]=i<words?packet[i]:0xfafafafa;
    h->barrier(h->cookie,1);v[11]=(wr+padded)&1023;h->barrier(h->cookie,1);
    if(h->notify(h->cookie)){h->poisoned=1;return PG_HFI_IO;}
    return PG_HFI_OK;
}
enum pg_hfi_status pg_hfi_receive(struct pg_hfi *h,unsigned q,uint32_t *packet,
    size_t capacity,size_t *words)
{
    if(!packet || !words || (q!=1 && q!=2))return PG_HFI_ARGUMENT;
    *words=0;
    volatile uint32_t *v,*data;uint32_t rd,wr;
    enum pg_hfi_status status=queue(h,q,&v,&data,&rd,&wr);if(status)return status;
    unsigned available=(wr-rd)&1023;if(!available)return PG_HFI_EMPTY;
    unsigned n=(data[rd]>>8)&255,padded=(n+3)&~3u;
    if(!n || padded>available)return corrupt(h);
    *words=n;if(n>capacity)return PG_HFI_CAPACITY;
    for(unsigned i=0;i<n;++i)packet[i]=data[(rd+i)&1023];
    h->barrier(h->cookie,1);v[10]=(rd+padded)&1023;h->barrier(h->cookie,1);
    return PG_HFI_OK;
}
