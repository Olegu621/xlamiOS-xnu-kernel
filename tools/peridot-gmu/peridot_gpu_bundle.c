/* SPDX-License-Identifier: MIT */
/* Install only the explicit boot-carried GLES image, under a new versioned
 * filename. Existing files are compared and never overwritten. The standard
 * VFS handles HFS/journaling; no raw filesystem offsets or partition writes. */
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/mount.h>
#include <sys/vnode_internal.h>
static char pg_bundle_path[112];
static bool pg_bundle_active;

const char *peridot_gles_library(void)
{
    return __atomic_load_n(&pg_bundle_active,__ATOMIC_ACQUIRE) ? pg_bundle_path : NULL;
}
void peridot_gles_stage(void)
{
    uint32_t enabled=0;
    PE_parse_boot_argn("peridotgles",&enabled,sizeof(enabled));
    if (enabled!=1 || !peridot_gpu_ready() || peridot_gles_library()) return;
    DTEntry map,chosen;const uint64_t *region,*base,*size;const uint8_t *sha;
    unsigned n=0;
    if (SecureDTLookupEntry(NULL,"/chosen/memory-map",&map)!=kSuccess ||
        SecureDTGetProperty(map,"PeridotGLESDriver",(const void **)&region,&n)!=kSuccess || n!=16 ||
        SecureDTGetProperty(map,"PeridotGLESDriverSHA256",(const void **)&sha,&n)!=kSuccess || n!=32 ||
        SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess ||
        SecureDTGetProperty(chosen,"dram-base",(const void **)&base,&n)!=kSuccess || n!=8 ||
        SecureDTGetProperty(chosen,"dram-size",(const void **)&size,&n)!=kSuccess || n!=8) {
        printf("PERIDOT GLES: reserved driver/DRAM metadata absent\n");return;
    }
    uint64_t pa=region[0],bytes=region[1],rounded=(bytes+16383)&~UINT64_C(16383);
    if ((pa&16383) || bytes<4096 || bytes>(32u<<20) || pa<*base || pa-*base>*size ||
        rounded>*size-(pa-*base)) return;
    const uint8_t *image=(const uint8_t *)ml_static_ptovirt(pa);
    if (!image || pggx_u32(image)!=0xfeedfacf || pggx_u32(image+4)!=0x0100000c ||
        pggx_u32(image+8)!=0 || pggx_u32(image+12)!=6) {
        printf("PERIDOT GLES: expected arm64 Mach-O dynamic library\n");return;
    }
    char tag[33];const char hex[]="0123456789abcdef";
    for (unsigned i=0;i<16;++i) {tag[2*i]=hex[sha[i]>>4];tag[2*i+1]=hex[sha[i]&15];}
    tag[32]=0;
    snprintf(pg_bundle_path,sizeof(pg_bundle_path),"/usr/lib/peridot-gles-%s.dylib",tag);
    vfs_context_t context=vfs_context_kernel();vnode_t node=NULLVP;
    int flags=FREAD|FWRITE|O_CREAT|O_EXCL|O_NOFOLLOW;
    int error=vnode_open(pg_bundle_path,flags,0644,0,&node,context);
    bool created=error==0;
    if (error==EEXIST) {
        flags=FREAD|O_NOFOLLOW;
        node=NULLVP;
        error=vnode_open(pg_bundle_path,flags,0,0,&node,context);
    }
    if (error) {printf("PERIDOT GLES: versioned file open error=%d\n",error);return;}
    if (vnode_vtype(node)!=VREG) error=EINVAL;
    if (!error && created) {
        int residual=0;
        error=vn_rdwr(UIO_WRITE,node,(caddr_t)(uintptr_t)image,(int)bytes,0,UIO_SYSSPACE,
            IO_UNIT,vfs_context_ucred(context),&residual,vfs_context_proc(context));
        if (!error && residual) error=EIO;
        if (!error) error=VNOP_FSYNC(node,MNT_WAIT,context);
    }
    struct vnode_attr attr;
    VATTR_INIT(&attr);VATTR_WANTED(&attr,va_data_size);VATTR_WANTED(&attr,va_mode);
    if (!error) error=vnode_getattr(node,&attr,context);
    if (!error && (!VATTR_IS_SUPPORTED(&attr,va_data_size) || attr.va_data_size!=bytes)) error=EINVAL;
    if (!error && (!VATTR_IS_SUPPORTED(&attr,va_mode) || (attr.va_mode&0044)!=0044 ||
        (attr.va_mode&0022))) error=EACCES;
    uint8_t *scratch=NULL;
    if (!error) {scratch=IOMalloc(16384);if (!scratch) error=ENOMEM;}
    for (uint64_t offset=0;!error && offset<bytes;offset+=16384) {
        unsigned count=(unsigned)(bytes-offset<16384 ? bytes-offset : 16384);
        int residual=0;
        error=vn_rdwr(UIO_READ,node,(caddr_t)scratch,(int)count,(off_t)offset,UIO_SYSSPACE,
            0,vfs_context_ucred(context),&residual,vfs_context_proc(context));
        if (!error && (residual || bcmp(scratch,image+offset,count))) error=EIO;
    }
    if (scratch) IOFree(scratch,16384);
    int close_error=vnode_close(node,created ? FREAD|FWRITE|FWASWRITTEN : FREAD,context);
    if (!error) error=close_error;
    if (error) {
        printf("PERIDOT GLES: file verification failed=%d; environment injection disabled\n",error);
        return;
    }
    __atomic_store_n(&pg_bundle_active,true,__ATOMIC_RELEASE);
    printf("PERIDOT GLES: exact boot-carried library ready bytes=%llu created=%u path=%s\n",
        (unsigned long long)bytes,created,pg_bundle_path);
}
