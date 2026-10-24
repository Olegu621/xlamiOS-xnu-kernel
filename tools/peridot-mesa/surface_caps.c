/* SPDX-License-Identifier: MIT */
/*20H392 distinguishes CIF10 support from the deep-framebuffer choice.
 * The FD735 frontend imports single-plane surfaces. Withhold both native
 * capabilities before either the display server or its UI client starts. */
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sweet_drm.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

static int writable_address(uintptr_t address)
{
    vm_address_t region = address;
    vm_size_t bytes = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t result = vm_region_64(mach_task_self(), &region, &bytes,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return result == KERN_SUCCESS && address >= region && address - region < bytes &&
        (info.protection & VM_PROT_WRITE) != 0;
}

struct native_surface_cap {
    const char *name;
    uintptr_t invoke, value, once;
    size_t bytes, return_offset;
    uint32_t entry[4], exit[4];
};

static void disable_cap(const uint8_t *cache, size_t length,
    const struct native_surface_cap *cap)
{
    const uintptr_t preferred = UINT64_C(0x180000000);
    const uintptr_t invoke_offset = cap->invoke - preferred;
    const uintptr_t value_offset = cap->value - preferred;
    const uintptr_t once_offset = cap->once - preferred;
    if (invoke_offset > length || length - invoke_offset < cap->bytes ||
        value_offset >= length || once_offset > length ||
        length - once_offset < sizeof(dispatch_once_t) ||
        cap->return_offset > cap->bytes || cap->bytes - cap->return_offset < sizeof(cap->exit) ||
        memcmp(cache + invoke_offset, cap->entry, sizeof(cap->entry)) != 0 ||
        memcmp(cache + invoke_offset + cap->return_offset, cap->exit, sizeof(cap->exit)) != 0) {
        sweet_drm_log("sweetgl: %s capability unchanged: native code guard refused", cap->name);
        return;
    }
    uint8_t *value = (uint8_t *)(uintptr_t)(cache + value_offset);
    dispatch_once_t *once = (dispatch_once_t *)(uintptr_t)(cache + once_offset);
    if (!writable_address((uintptr_t)value) || !writable_address((uintptr_t)once)) {
        sweet_drm_log("sweetgl: %s capability unchanged: native data is not writable", cap->name);
        return;
    }
    /* Both exact initializers have no block captures: x0 is overwritten by
     * the native query. Use the original once token before changing its result. */
    dispatch_once_f(once, NULL, (dispatch_function_t)(uintptr_t)(cache + invoke_offset));
    uint8_t before = __atomic_load_n(value, __ATOMIC_ACQUIRE);
    /* dispatch_once_f has completed initialization before returning. Its
     * predicate can retain a quiescent-generation encoding instead of -1;
     * only libdispatch may interpret that private state. */
    if (before > 1) {
        sweet_drm_log("sweetgl: %s capability unchanged: native value %u, once %#llx",
            cap->name, before, (unsigned long long)__atomic_load_n(once, __ATOMIC_ACQUIRE));
        return;
    }
    __atomic_store_n(value, 0, __ATOMIC_RELEASE);
    sweet_drm_log("sweetgl: native %s surface capability %u -> 0 for FD735 interop, once %#llx",
        cap->name, before, (unsigned long long)__atomic_load_n(once, __ATOMIC_ACQUIRE));
}

const uint8_t *peridot_surface_cache(size_t *length)
{
    const uint8_t *cache = _dyld_get_shared_cache_range(length);
    static const uint8_t uuid[16] = {
        0x8f,0x97,0x04,0x52,0xc9,0xe3,0x3e,0x88,
        0xb1,0x14,0xfe,0xb1,0x86,0x4b,0xc9,0xe0};
    if (!cache || *length < 104 || memcmp(cache + 88, uuid, sizeof(uuid)) != 0) {
        sweet_drm_log("sweetgl: surface capabilities unchanged: unrecognized shared cache");
        return NULL;
    }
    return cache;
}

void peridot_surface_caps_install(void)
{
    const char *process = getprogname();
    if (strcmp(process, "backboardd") && strcmp(process, "SpringBoard")) return;
    size_t length = 0;
    const uint8_t *cache = peridot_surface_cache(&length);
    if (!cache) return;
    sweet_drm_log("sweetgl: shared cache base %p size %#zx process %s", cache, length, process);
    static const struct native_surface_cap caps[] = {
        {"CIF10", UINT64_C(0x188378950), UINT64_C(0x1e1df6b88), UINT64_C(0x1e1df6ba0),
         112, 96, {0xa9bf7bfd,0x910003fd,0xf026bf20,0x91068000},
         {0xd02cd3e9,0x392e2128,0xa8c17bfd,0xd65f03c0}},
        /* CADeviceUseCIF10 reads CAEnableDeepFramebuffer. AppleDisplay then
         * copies it to flags+0x220 bit3, selecting b3a8 instead of BGRA. */
        {"deep-framebuffer", UINT64_C(0x188381f38), UINT64_C(0x1e1df7318), UINT64_C(0x1e1df7330),
         152, 132, {0xd100c3ff,0xa9014ff4,0xa9027bfd,0x910083fd},
         {0xa9427bfd,0xa9414ff4,0x9100c3ff,0xd65f03c0}},
        /* Exact CoreMaterial MTIOSurfaceCreate calls this before choosing
         * single-plane BGRA versus deep b3a8/two-plane storage. */
        {"CoreMaterial-deep-color", UINT64_C(0x1a7ca6a2c), UINT64_C(0x1dd930348), UINT64_C(0x1dd930350),
         36, 20, {0xa9bf7bfd,0x910003fd,0xf0182760,0x91376000},
         {0xd01ae448,0x390d2100,0xa8c17bfd,0xd65f03c0}},
    };
    setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i)
        disable_cap(cache, length, &caps[i]);
    sweet_drm_log("sweetgl: single-plane surface policy installed in %s", process);
}
