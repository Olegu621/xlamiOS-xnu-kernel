/* SPDX-License-Identifier: MIT */
/* FD735 BO/IOSurface/IB interface for the trusted iOS GLES frontend.
 * Selector10..16 transport follows ios-sailfish sweetgl at d7a74344828c;
 * all GPU work uses the independent peridot native4K/HWSCHED engine.
 * One GPU address space is retained for this diagnostic session. */
#include <pexpert/arm64/board_config.h>
#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include "SweetSurface.h"
#include "../../../bsd/dev/peridot_gpu.h"

extern "C" void flush_dcache64(addr64_t, unsigned, int);
enum { PG_CLIENT_BOS=1024, PG_MAX_IBS=32, PG_MAX_BO_BYTES=64u<<20, PG_CLIENT_BYTES=256u<<20 };
struct pg_client_bo {
    IOMemoryDescriptor *memory;
    IOMemoryMap *mapping;
    SweetSurface *surface;
    uint64_t *pages, address, bytes;
    uint32_t count, sequence;
};

class PeridotGPU : public IOService {
    OSDeclareDefaultStructors(PeridotGPU);
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(PeridotGPU,IOService);
bool PeridotGPU::start(IOService *provider)
{
    uint32_t mode=0;
    PE_parse_boot_argn("peridotgpu",&mode,sizeof(mode));
    if (mode!=21 || !IOService::start(provider)) return false;
    setProperty(kIOUserClientClassKey,"PeridotGPUUserClient");
    setProperty("GPUModel","Adreno735");
    /* Methods return NotReady until actual CP/ZAP/fence completion and
     * the parent clock lease are both established. No accelerator alias. */
    registerService();
    IOLog("PERIDOT GPU: IOKit BO service registered; hardware readiness checked on each call\n");
    return true;
}

class PeridotGPUUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PeridotGPUUserClient);
    task_t fTask;
    IOLock *fLock;
    pg_client_bo *fBos;
    uint64_t fBytes;
    uint32_t fLastSequence;
    bool fClosing;
    pg_client_bo *bo(uint64_t handle);
    pg_client_bo *vacant(uint32_t *handle);
    bool releaseBo(pg_client_bo *entry);
    bool physicalPages(IOMemoryDescriptor *,uint64_t **,uint32_t *);
    void clean(pg_client_bo *);
    IOReturn allocate(uint64_t size,uint64_t *out);
    IOReturn importSurface(uint32_t id,uint64_t *out);
    IOReturn submit(const void *,size_t,uint64_t *);
public:
    bool initWithTask(task_t,void *,UInt32) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(PeridotGPUUserClient,IOUserClient);
bool PeridotGPUUserClient::initWithTask(task_t task,void *security,UInt32 type)
{
    fTask=TASK_NULL;fLock=NULL;fBos=NULL;fBytes=0;fLastSequence=0;fClosing=false;
    if (type || !IOUserClient::initWithTask(task,security,type)) return false;
    fTask=task;task_reference(fTask);
    fLock=IOLockAlloc();
    fBos=(pg_client_bo *)IOMalloc(PG_CLIENT_BOS*sizeof(*fBos));
    if (fBos) bzero(fBos,PG_CLIENT_BOS*sizeof(*fBos));
    return fLock && fBos;
}
pg_client_bo *PeridotGPUUserClient::bo(uint64_t handle)
{
    return handle && handle<=PG_CLIENT_BOS && fBos[handle-1].memory ? fBos+handle-1 : NULL;
}
pg_client_bo *PeridotGPUUserClient::vacant(uint32_t *handle)
{
    for (unsigned i=0;i<PG_CLIENT_BOS;++i)
        if (!fBos[i].memory) {*handle=i+1;return fBos+i;}
    return NULL;
}
bool PeridotGPUUserClient::releaseBo(pg_client_bo *entry)
{
    if (!entry->memory) return true;
    if ((entry->sequence && peridot_gpu_wait(entry->sequence,2000)) || peridot_gpu_unmap(entry->address)) {
        IOLog("PERIDOT GPU: retaining descriptor for unresolved VA=%#llx\n",(unsigned long long)entry->address);
        return false;
    }
    OSSafeReleaseNULL(entry->mapping);
    if (entry->surface) {
        entry->memory->complete(kIODirectionInOut);
        entry->surface->release();
    } else fBytes-=entry->bytes;
    entry->memory->release();
    IOFree(entry->pages,entry->count*sizeof(uint64_t));
    bzero(entry,sizeof(*entry));
    return true;
}
void PeridotGPUUserClient::free()
{
    if (fBos) {
        for (unsigned i=0;i<PG_CLIENT_BOS;++i) releaseBo(fBos+i);
        /* Unresolved descriptors intentionally retain their references and
         * wiring until reset. Their physical backing must not be recycled. */
        IOFree(fBos,PG_CLIENT_BOS*sizeof(*fBos));fBos=NULL;
    }
    if (fLock) {IOLockFree(fLock);fLock=NULL;}
    if (fTask) {task_deallocate(fTask);fTask=TASK_NULL;}
    IOUserClient::free();
}
IOReturn PeridotGPUUserClient::clientClose()
{
    if (fLock) {
        IOLockLock(fLock);fClosing=true;
        if (fBos) for (unsigned i=0;i<PG_CLIENT_BOS;++i) releaseBo(fBos+i);
        IOLockUnlock(fLock);
    }
    terminate();return kIOReturnSuccess;
}
bool PeridotGPUUserClient::physicalPages(IOMemoryDescriptor *memory,uint64_t **out,uint32_t *count)
{
    uint64_t bytes=memory->getLength();
    *out=NULL;*count=0;
    if (!bytes || bytes>PG_MAX_BO_BYTES || (bytes&PAGE_MASK)) return false;
    uint32_t n=(uint32_t)(bytes/PAGE_SIZE);
    uint64_t *pages=(uint64_t *)IOMalloc(n*sizeof(uint64_t));
    if (!pages) return false;
    for (unsigned i=0;i<n;++i) {
        IOByteCount span=0;
        pages[i]=memory->getPhysicalSegment((IOByteCount)i*PAGE_SIZE,&span,kIOMemoryMapperNone);
        if (!pages[i] || (pages[i]&PAGE_MASK) || span<PAGE_SIZE || pages[i]>=(UINT64_C(1)<<36)) {
            IOFree(pages,n*sizeof(uint64_t));return false;
        }
    }
    *out=pages;*count=n;return true;
}
void PeridotGPUUserClient::clean(pg_client_bo *entry)
{
    for (unsigned i=0;i<entry->count;++i) flush_dcache64(entry->pages[i],PAGE_SIZE,1);
}
IOReturn PeridotGPUUserClient::allocate(uint64_t size,uint64_t *out)
{
    if (!size || size>PG_MAX_BO_BYTES) return kIOReturnBadArgument;
    size=(size+PAGE_MASK)&~(uint64_t)PAGE_MASK;
    if (size>PG_CLIENT_BYTES-fBytes) return kIOReturnNoMemory;
    uint32_t handle=0,count=0;
    pg_client_bo *entry=vacant(&handle);
    if (!entry) return kIOReturnNoResources;
    IOBufferMemoryDescriptor *memory=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut|kIOMemoryKernelUserShared,size,PAGE_SIZE);
    if (!memory) return kIOReturnNoMemory;
    bzero(memory->getBytesNoCopy(),size);
    uint64_t *pages=NULL;
    if (!physicalPages(memory,&pages,&count)) {memory->release();return kIOReturnNoMemory;}
    for (unsigned i=0;i<count;++i) flush_dcache64(pages[i],PAGE_SIZE,1);
    uint64_t address=peridot_gpu_map(pages,count);
    if (!address) {IOFree(pages,count*sizeof(uint64_t));memory->release();return kIOReturnNoResources;}
    entry->memory=memory;entry->pages=pages;entry->count=count;entry->address=address;entry->bytes=size;
    fBytes+=size;
    /* Keep managed CPU aliases cached. Explicit maintenance surrounds GPU
     * ownership; changing their physmap cache type can invalidate VM aliases. */
    entry->mapping=memory->createMappingInTask(fTask,0,kIOMapAnywhere);
    if (!entry->mapping) {releaseBo(entry);return kIOReturnNoMemory;}
    out[0]=handle;out[1]=address;out[2]=entry->mapping->getAddress();
    return kIOReturnSuccess;
}
IOReturn PeridotGPUUserClient::importSurface(uint32_t id,uint64_t *out)
{
    SweetSurface *surface=sweet_surface_copy(id);
    if (!surface || !surface->memory) {OSSafeReleaseNULL(surface);return kIOReturnNotFound;}
    uint32_t handle=0,count=0;uint64_t *pages=NULL;
    pg_client_bo *entry=vacant(&handle);
    IOMemoryDescriptor *memory=surface->memory;
    if (!entry || memory->prepare(kIODirectionInOut)!=kIOReturnSuccess) {
        surface->release();return entry ? kIOReturnVMError : kIOReturnNoResources;
    }
    if (!physicalPages(memory,&pages,&count)) {
        memory->complete(kIODirectionInOut);surface->release();return kIOReturnNoMemory;
    }
    for (unsigned i=0;i<count;++i) flush_dcache64(pages[i],PAGE_SIZE,1);
    uint64_t address=peridot_gpu_map(pages,count);
    if (!address) {
        IOFree(pages,count*sizeof(uint64_t));memory->complete(kIODirectionInOut);
        surface->release();return kIOReturnNoResources;
    }
    memory->retain();entry->memory=memory;entry->surface=surface;
    entry->pages=pages;entry->count=count;entry->address=address;entry->bytes=memory->getLength();
    out[0]=handle;out[1]=address;out[2]=entry->bytes;
    return kIOReturnSuccess;
}
IOReturn PeridotGPUUserClient::submit(const void *input,size_t bytes,uint64_t *out)
{
    uint32_t header[2];
    if (!input || bytes<sizeof(header)) return kIOReturnBadArgument;
    bcopy(input,header,sizeof(header));
    uint32_t num_ibs=header[0],num_bos=header[1];
    if (!num_ibs || num_ibs>PG_MAX_IBS || !num_bos || num_bos>PG_CLIENT_BOS ||
        bytes!=8+num_ibs*sizeof(peridot_gpu_ib)+num_bos*4) return kIOReturnBadArgument;
    peridot_gpu_ib ibs[PG_MAX_IBS];
    bcopy((const uint8_t *)input+8,ibs,num_ibs*sizeof(ibs[0]));
    const uint32_t *handles=(const uint32_t *)((const uint8_t *)input+8+num_ibs*sizeof(ibs[0]));
    for (unsigned i=0;i<num_bos;++i) if (!bo(handles[i])) return kIOReturnBadArgument;
    for (unsigned i=0;i<num_ibs;++i) {
        bool owned=false;uint64_t length=(uint64_t)ibs[i].words*4;
        if (!length || ibs[i].reserved || (ibs[i].address&3)) return kIOReturnBadArgument;
        for (unsigned j=0;j<num_bos;++j) {
            pg_client_bo *entry=bo(handles[j]);
            if (ibs[i].address>=entry->address && ibs[i].address-entry->address<entry->bytes &&
                length<=entry->bytes-(ibs[i].address-entry->address)) {owned=true;break;}
        }
        if (!owned) return kIOReturnNotPermitted;
    }
    for (unsigned i=0;i<num_bos;++i) clean(bo(handles[i]));
    uint32_t sequence=0;
    int status=peridot_gpu_submit(ibs,num_ibs,&sequence);
    if (status) return status==EINVAL ? kIOReturnBadArgument : kIOReturnNotReady;
    for (unsigned i=0;i<num_bos;++i) bo(handles[i])->sequence=sequence;
    fLastSequence=sequence;*out=sequence;
    return kIOReturnSuccess;
}
IOReturn PeridotGPUUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch,OSObject *target,void *reference)
{
    (void)dispatch;(void)target;(void)reference;
    if (!args || !fLock || !fBos) return kIOReturnBadArgument;
    if (!peridot_gpu_ready()) return kIOReturnNotReady;
    IOLockLock(fLock);
    IOReturn result=kIOReturnBadArgument;
    if (fClosing) {IOLockUnlock(fLock);return kIOReturnNotOpen;}
    switch (selector) {
    case 10:
        if (args->scalarInputCount==1 && args->scalarOutputCount>=3) {
            result=allocate(args->scalarInput[0],args->scalarOutput);
            args->scalarOutputCount=result==kIOReturnSuccess ? 3 : 0;
        }
        break;
    case 11:
        if (args->scalarInputCount==1) {
            pg_client_bo *entry=bo(args->scalarInput[0]);
            result=!entry ? kIOReturnNotFound : releaseBo(entry) ? kIOReturnSuccess : kIOReturnBusy;
        }
        break;
    case 12:
        if (args->scalarInputCount==1 && args->scalarInput[0]<=UINT32_MAX && args->scalarOutputCount>=3) {
            result=importSurface((uint32_t)args->scalarInput[0],args->scalarOutput);
            args->scalarOutputCount=result==kIOReturnSuccess ? 3 : 0;
        }
        break;
    case 13:
        if (args->scalarOutputCount>=1) {
            uint64_t sequence=0;
            if (args->structureInputDescriptor) {
                IOMemoryDescriptor *d=args->structureInputDescriptor;
                size_t bytes=d->getLength();void *copy=bytes && bytes<=8192 ? IOMalloc(bytes) : NULL;
                if (!copy) {result=kIOReturnNoMemory;break;}
                result=d->prepare()==kIOReturnSuccess ? kIOReturnSuccess : kIOReturnVMError;
                if (result==kIOReturnSuccess) {
                    result=d->readBytes(0,copy,bytes)==bytes ? submit(copy,bytes,&sequence) : kIOReturnVMError;
                    d->complete();
                }
                IOFree(copy,bytes);
            } else result=submit(args->structureInput,args->structureInputSize,&sequence);
            args->scalarOutput[0]=sequence;args->scalarOutputCount=1;
        }
        break;
    case 14:
        if (args->scalarInputCount==2 && args->scalarInput[0] && args->scalarInput[0]<=fLastSequence) {
            uint32_t timeout=(uint32_t)(args->scalarInput[1]>60000 ? 60000 : args->scalarInput[1]);
            int status=peridot_gpu_wait((uint32_t)args->scalarInput[0],timeout);
            result=!status ? kIOReturnSuccess : status==ETIMEDOUT ? kIOReturnTimeout : kIOReturnNotReady;
        }
        break;
    case 15:
        if (!args->scalarInputCount && args->scalarOutputCount>=5) {
            args->scalarOutput[0]=0x43030b00;args->scalarOutput[1]=0x180000;
            args->scalarOutput[2]=0x1000000;args->scalarOutput[3]=255000000;
            args->scalarOutput[4]=0;args->scalarOutputCount=5;result=kIOReturnSuccess;
        }
        break;
    case 16:
        if (args->scalarInputCount==1) {
            pg_client_bo *entry=bo(args->scalarInput[0]);
            if (!entry) result=kIOReturnNotFound;
            else if (entry->sequence && peridot_gpu_wait(entry->sequence,0)) result=kIOReturnBusy;
            else {clean(entry);result=kIOReturnSuccess;}
        }
        break;
    default:result=kIOReturnUnsupported;break;
    }
    IOLockUnlock(fLock);
    return result;
}
#endif
