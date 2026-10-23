/* SPDX-License-Identifier: MIT */
/*20H392 QuartzCore derives CIF10 interop from the donor ChipID. The FD735
 * frontend currently imports single-plane surfaces, so this process must
 * withhold that Apple-only surface capability. No executable page changes. */
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

void peridot_surface_caps_install(void)
{
    if (strcmp(getprogname(), "backboardd") != 0) return;
    size_t length = 0;
    const uint8_t *cache = _dyld_get_shared_cache_range(&length);
    static const uint8_t uuid[16] = {
        0x8f,0x97,0x04,0x52,0xc9,0xe3,0x3e,0x88,
        0xb1,0x14,0xfe,0xb1,0x86,0x4b,0xc9,0xe0};
    const uintptr_t preferred = UINT64_C(0x180000000);
    const uintptr_t invoke_offset = UINT64_C(0x188378950) - preferred;
    const uintptr_t value_offset = UINT64_C(0x1e1df6b88) - preferred;
    const uintptr_t once_offset = UINT64_C(0x1e1df6ba0) - preferred;
    static const uint32_t prologue[4] = {0xa9bf7bfd,0x910003fd,0xf026bf20,0x91068000};
    static const uint32_t epilogue[4] = {0xd02cd3e9,0x392e2128,0xa8c17bfd,0xd65f03c0};
    if (!cache || length < 104 || memcmp(cache + 88, uuid, sizeof(uuid)) != 0 ||
        invoke_offset > length || length - invoke_offset < 112 ||
        value_offset >= length || once_offset > length ||
        length - once_offset < sizeof(dispatch_once_t) ||
        memcmp(cache + invoke_offset, prologue, sizeof(prologue)) != 0 ||
        memcmp(cache + invoke_offset + 96, epilogue, sizeof(epilogue)) != 0) {
        sweet_drm_log("sweetgl: CIF10 capability unchanged: unrecognized shared cache");
        return;
    }
    uint8_t *value = (uint8_t *)(uintptr_t)(cache + value_offset);
    dispatch_once_t *once = (dispatch_once_t *)(uintptr_t)(cache + once_offset);
    if (!writable_address((uintptr_t)value) || !writable_address((uintptr_t)once)) {
        sweet_drm_log("sweetgl: CIF10 capability unchanged: native data is not writable");
        return;
    }
    /* The exact native initializer has no block captures: x0 is overwritten
     * by its ChipID query. Use its own once token before changing the result. */
    dispatch_once_f(once, NULL, (dispatch_function_t)(uintptr_t)(cache + invoke_offset));
    uint8_t before = __atomic_load_n(value, __ATOMIC_ACQUIRE);
    if (before > 1 || *once != (dispatch_once_t)-1) {
        sweet_drm_log("sweetgl: CIF10 capability unchanged: unexpected native state");
        return;
    }
    __atomic_store_n(value, 0, __ATOMIC_RELEASE);
    sweet_drm_log("sweetgl: native CIF10 surface capability %u -> 0 for FD735 interop", before);
}
