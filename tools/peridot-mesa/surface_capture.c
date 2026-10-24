/* SPDX-License-Identifier: MIT */
/* Exact20H392 deferred capture policy, before native allocation. */
#include <mach/mach.h>
#include <mach/vm_page_size.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sweet_drm.h"

extern const uint8_t *peridot_surface_cache(size_t *length);

typedef void (*capture_realize_fn)(void *, uint64_t, bool, void *);
static capture_realize_fn capture_original;
static uintptr_t capture_backing_vptr;
static unsigned capture_redirects, capture_cached, capture_layout;

static void capture_realize_sdr(void *slot, uint64_t capabilities, bool floating,
    void *color_space)
{
    capture_realize_fn original = __atomic_load_n(&capture_original, __ATOMIC_ACQUIRE);
    /* The original implementation dereferences this same retained backing.
     * Keep its reference ownership and cached-image lifecycle unchanged. */
    uint8_t *backing = *(uint8_t **)((uint8_t *)slot + 0x18);
    if (*(uintptr_t *)backing == capture_backing_vptr && backing[0x0c] == 0x0b &&
        backing[0x20] <= 1 && backing[0x21] <= 1) {
        if (!floating && backing[0x21] == 1) {
            if (*(uintptr_t *)(backing + 0x10) == 0) {
                /* Native realize_buffer now chooses1/3 and builds BGRA itself.
                 * Do not restore XR on this shared backing after allocation. */
                backing[0x21] = 0;
                if (__atomic_fetch_add(&capture_redirects, 1, __ATOMIC_RELAXED) < 8)
                    sweet_drm_log("sweetgl: deferred capture XR -> SDR before allocation, alpha %u",
                        backing[0x20]);
            } else if (__atomic_fetch_add(&capture_cached, 1, __ATOMIC_RELAXED) < 8) {
                sweet_drm_log("sweetgl: deferred capture cached XR image unchanged");
            }
        }
    } else if (__atomic_fetch_add(&capture_layout, 1, __ATOMIC_RELAXED) < 8) {
        sweet_drm_log("sweetgl: deferred capture layout guard refused");
    }
    original(slot, capabilities, floating, color_space);
}

static int capture_in_cache(size_t length, uintptr_t address, size_t bytes)
{
    const uintptr_t preferred = UINT64_C(0x180000000);
    return address >= preferred && address - preferred <= length &&
        bytes <= length - (address - preferred);
}

void peridot_surface_capture_install(void)
{
    if (strcmp(getprogname(), "backboardd")) return;
    size_t length = 0;
    const uint8_t *cache = peridot_surface_cache(&length);
    if (!cache) return;
    const uintptr_t preferred = UINT64_C(0x180000000);
    const uintptr_t realize = UINT64_C(0x18837cb30);
    const uintptr_t choice = UINT64_C(0x18837cbb0);
    const uintptr_t tail = UINT64_C(0x18837cd1c);
    const uintptr_t vtable = UINT64_C(0x1d5b29be0);
    const uintptr_t backing_vptr = UINT64_C(0x1d5b33ad8);
    const uint32_t entry[] = {0xa9bc5ff8,0xa90157f6,0xa9024ff4,0xa9037bfd};
    const uint32_t chooser[] = {
        0x39408708,0x39408309,0x7100013f,0x52800469,0x5280042a,0x1a890149,
        0x5280006a,0x1a9f054a,0x7100011f,0x1a890148,0x710002df,0x528001a9,
        0x1a881136,0x71000edf};
    const uint32_t ending[] = {0xf9400e68,0xf9400908,0xb4000048,0xf9403508,0xf9000a68};
    if (!capture_in_cache(length, realize, sizeof(entry)) ||
        !capture_in_cache(length, choice, sizeof(chooser)) ||
        !capture_in_cache(length, tail, sizeof(ending)) ||
        !capture_in_cache(length, vtable, 7 * sizeof(uintptr_t)) ||
        !capture_in_cache(length, backing_vptr, sizeof(uintptr_t)) ||
        memcmp(cache + (realize - preferred), entry, sizeof(entry)) ||
        memcmp(cache + (choice - preferred), chooser, sizeof(chooser)) ||
        memcmp(cache + (tail - preferred), ending, sizeof(ending))) {
        sweet_drm_log("sweetgl: deferred capture adapter refused: native code guard");
        return;
    }
    uintptr_t *table = (uintptr_t *)(cache + (vtable - preferred));
    uintptr_t native = (uintptr_t)(cache + (realize - preferred));
    if (table[0] || table[1] || table[2] != (uintptr_t)(cache + (0x18847309cULL - preferred)) ||
        table[4] != (uintptr_t)(cache + (0x188364938ULL - preferred)) || table[5] != native ||
        table[6] != (uintptr_t)(cache + (0x18836aa1cULL - preferred))) {
        sweet_drm_log("sweetgl: deferred capture adapter refused: native vtable guard");
        return;
    }
    vm_address_t address = (vm_address_t)&table[5];
    vm_address_t page = address & ~(vm_address_t)vm_page_mask;
    vm_address_t region = address;
    vm_size_t bytes = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t result = vm_region_64(mach_task_self(), &region, &bytes,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (result != KERN_SUCCESS || page < region || page - region > bytes ||
        vm_page_size > bytes - (page - region) || !(info.protection & VM_PROT_READ) ||
        (info.protection & VM_PROT_EXECUTE)) {
        sweet_drm_log("sweetgl: deferred capture adapter refused: VM data region");
        return;
    }
    result = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (result != KERN_SUCCESS) {
        sweet_drm_log("sweetgl: deferred capture adapter refused: private copy error %d", result);
        return;
    }
    capture_backing_vptr = (uintptr_t)(cache + (backing_vptr - preferred));
    __atomic_store_n(&capture_original, (capture_realize_fn)native, __ATOMIC_RELEASE);
    __atomic_store_n(&table[5], (uintptr_t)capture_realize_sdr, __ATOMIC_RELEASE);
    result = vm_protect(mach_task_self(), page, vm_page_size, FALSE, info.protection);
    if (result != KERN_SUCCESS) {
        __atomic_store_n(&table[5], native, __ATOMIC_RELEASE);
        kern_return_t restore = vm_protect(mach_task_self(), page, vm_page_size, FALSE, info.protection);
        sweet_drm_log("sweetgl: deferred capture adapter rolled back: protection error %d, retry %d",
            result, restore);
        return;
    }
    sweet_drm_log("sweetgl: native deferred capture SDR adapter installed, protection restored");
}
