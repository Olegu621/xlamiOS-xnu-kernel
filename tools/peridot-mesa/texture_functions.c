/* SPDX-License-Identifier: MIT */
/*20H392 QuartzCore shader expressions and uniform dispatch, process private. */
#include <mach/mach.h>
#include <mach/vm_page_size.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sweet_drm.h"

extern const uint8_t *peridot_surface_cache(size_t *length);

struct const_page {
    vm_address_t address;
    vm_prot_t protection;
};

static int describe_page(uintptr_t address, struct const_page *page)
{
    vm_address_t start = address;
    vm_size_t bytes = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t result = vm_region_64(mach_task_self(), &start, &bytes,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    page->address = (vm_address_t)address & ~(vm_address_t)vm_page_mask;
    if (result != KERN_SUCCESS || page->address < start ||
        page->address - start > bytes || vm_page_size > bytes - (page->address - start) ||
        !(info.protection & VM_PROT_READ)) return 0;
    page->protection = info.protection;
    return 1;
}

static int in_cache(size_t length, uintptr_t preferred, size_t bytes)
{
    const uintptr_t base = UINT64_C(0x180000000);
    return preferred >= base && preferred - base <= length &&
        bytes <= length - (preferred - base);
}

void peridot_texture_functions_install(void)
{
    if (strcmp(getprogname(), "backboardd") != 0) return;
    size_t length = 0;
    const uint8_t *cache = peridot_surface_cache(&length);
    if (!cache) return;
    const uintptr_t base = UINT64_C(0x180000000);
    const uintptr_t table_va = UINT64_C(0x1d5b48a10);
    const uintptr_t cases_va = UINT64_C(0x18855e1e4) - 0x11;
    const uintptr_t shader_va = UINT64_C(0x1884b30d8);
    const uintptr_t dispatch_va = UINT64_C(0x1884b3e68);
    const uintptr_t anchor_va = UINT64_C(0x188596886);
    const uint32_t entry[] = {0xa9ba6ffc,0xa90167fa,0xa9025ff8,0xa90357f6};
    const uint32_t dispatch[] = {0xf9408fe8,0x51004508,0x7100851f,0x54001ca8,
        0xf0000549,0x91079129,0x1000008a,0x3868692b,0x8b0b094a,0xd61f0140};
    const char anchor[] = "colorMatrix4x4Bias(s0)";
    static const struct {
        uint8_t function, uniforms;
        const char *expression;
    } fixes[] = {
        {0x2d,0,"sgl_vcm(s0)"}, {0x14,0,"sgl_cm4(s0)"},
        {0x26,0xde,"sgl_nop()"}, {0x27,0x95,"sgl_vblur(s1)"},
    };
    if (!in_cache(length, table_va, 0x2e * sizeof(char *)) ||
        !in_cache(length, cases_va, 0x2e) ||
        !in_cache(length, shader_va, sizeof(entry)) ||
        !in_cache(length, dispatch_va, sizeof(dispatch)) ||
        !in_cache(length, anchor_va, sizeof(anchor)) ||
        memcmp(cache + shader_va - base, entry, sizeof(entry)) ||
        memcmp(cache + dispatch_va - base, dispatch, sizeof(dispatch)) ||
        memcmp(cache + anchor_va - base, anchor, sizeof(anchor))) {
        sweet_drm_log("sweetgl:20H392 texture functions refused: native code guard");
        return;
    }
    const char **table = (const char **)(uintptr_t)(cache + table_va - base);
    uint8_t *cases = (uint8_t *)(uintptr_t)(cache + cases_va - base);
    if (table[0x13] != (const char *)(cache + anchor_va - base) || cases[0x13] != 0) {
        sweet_drm_log("sweetgl:20H392 texture functions refused: anchor table");
        return;
    }
    for (unsigned i = 0; i < sizeof(fixes) / sizeof(fixes[0]); ++i) {
        unsigned fn = fixes[i].function;
        if (table[fn] || cases[fn] != 0xde) {
            sweet_drm_log("sweetgl:20H392 texture functions refused: entry %#x", fn);
            return;
        }
    }
    struct const_page pages[2];
    if (!describe_page((uintptr_t)table, &pages[0]) ||
        !describe_page((uintptr_t)cases, &pages[1]) || pages[0].address == pages[1].address) {
        sweet_drm_log("sweetgl:20H392 texture functions refused: VM regions");
        return;
    }
    /* Acquire both private writable copies before publishing any entry.
     * No instruction bytes change. The second page contains the native
     * byte lookup table in __TEXT, and recovers its original RX protection. */
    unsigned acquired = 0;
    for (; acquired < 2; ++acquired) {
        kern_return_t result = vm_protect(mach_task_self(), pages[acquired].address,
            vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (result != KERN_SUCCESS) {
            sweet_drm_log("sweetgl:20H392 texture functions refused: page %u copy error %d",
                acquired, result);
            break;
        }
    }
    if (acquired == 2) {
        for (unsigned i = 0; i < sizeof(fixes) / sizeof(fixes[0]); ++i) {
            unsigned fn = fixes[i].function;
            __atomic_store_n(&cases[fn], fixes[i].uniforms, __ATOMIC_RELEASE);
            __atomic_store_n(&table[fn], fixes[i].expression, __ATOMIC_RELEASE);
        }
    }
    int restored = 1;
    for (unsigned i = 0; i < acquired; ++i) {
        kern_return_t result = vm_protect(mach_task_self(), pages[i].address,
            vm_page_size, FALSE, pages[i].protection);
        if (result != KERN_SUCCESS) {
            restored = 0;
            sweet_drm_log("sweetgl:20H392 texture functions: protection restore page %u error %d", i, result);
        }
    }
    if (acquired == 2) {
        for (unsigned i = 0; i < sizeof(fixes) / sizeof(fixes[0]); ++i)
            sweet_drm_log("sweetgl:20H392 texture function %#x = %s, uniforms %#x, protections %s",
                fixes[i].function, table[fixes[i].function], cases[fixes[i].function],
                restored ? "restored" : "restore failed");
    }
}
