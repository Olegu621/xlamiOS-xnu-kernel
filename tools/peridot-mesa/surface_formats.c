/* SPDX-License-Identifier: MIT */
/* Negotiate supported contents storage before native QuartzCore allocation.
 * RGB10A8_2P_XR needs color/alpha plane interop which this frontend lacks. */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sweet_drm.h"

extern const uint8_t *peridot_surface_cache(size_t *length);

typedef void (*format_setter)(id, SEL, id);
struct format_hook {
    const char *name;
    uintptr_t preferred;
    uint32_t entry[4];
    format_setter original;
    unsigned logged;
    unsigned redirected;
};

static struct format_hook context_hook = {
    "CAContext", UINT64_C(0x18853f07c),
    {0xa9bc5ff8,0xa90157f6,0xa9024ff4,0xa9037bfd}, NULL, 0, 0};
static struct format_hook layer_hook = {
    "CALayer", UINT64_C(0x1882908bc),
    {0xa9be4ff4,0xa9017bfd,0x910043fd,0xaa0003f3}, NULL, 0, 0};

static int needs_sdr(id requested)
{
    return requested && CFGetTypeID((CFTypeRef)requested) == CFStringGetTypeID() &&
        (CFEqual((CFTypeRef)requested, CFSTR("RGBA10XR")) ||
         CFEqual((CFTypeRef)requested, CFSTR("Automatic")));
}

static id negotiated_format(struct format_hook *hook, id requested)
{
    int is_string = requested &&
        CFGetTypeID((CFTypeRef)requested) == CFStringGetTypeID();
    int use_sdr = needs_sdr(requested);
    unsigned *counter = use_sdr ? &hook->redirected : &hook->logged;
    if (__atomic_fetch_add(counter, 1, __ATOMIC_RELAXED) < 24) {
        char name[64] = "(nil or non-string)";
        if (is_string && !CFStringGetCString((CFStringRef)requested, name,
                sizeof(name), kCFStringEncodingUTF8))
            strcpy(name, "(long string)");
        sweet_drm_log("sweetgl: native %s contentsFormat %s%s", hook->name,
            name, use_sdr ? " -> RGBA8 before allocation" : " preserved");
    }
    return use_sdr ? (id)CFSTR("RGBA8") : requested;
}

static void context_set_contents_format(id object, SEL selector, id requested)
{
    format_setter original = __atomic_load_n(&context_hook.original, __ATOMIC_ACQUIRE);
    original(object, selector, negotiated_format(&context_hook, requested));
}

static void layer_set_contents_format(id object, SEL selector, id requested)
{
    format_setter original = __atomic_load_n(&layer_hook.original, __ATOMIC_ACQUIRE);
    original(object, selector, negotiated_format(&layer_hook, requested));
}

static void install_hook(const uint8_t *cache, size_t length,
    struct format_hook *hook, IMP replacement)
{
    uintptr_t offset = hook->preferred - UINT64_C(0x180000000);
    if (offset > length || sizeof(hook->entry) > length - offset ||
        memcmp(cache + offset, hook->entry, sizeof(hook->entry))) {
        sweet_drm_log("sweetgl: %s contents format unchanged: native code guard", hook->name);
        return;
    }
    Class cls = objc_getClass(hook->name);
    Method method = cls ? class_getInstanceMethod(cls, sel_registerName("setContentsFormat:")) : NULL;
    char result[8] = {0}, argument[8] = {0};
    if (method) {
        method_getReturnType(method, result, sizeof(result));
        method_getArgumentType(method, 2, argument, sizeof(argument));
    }
    if (!method || method_getNumberOfArguments(method) != 3 ||
        result[0] != 'v' || argument[0] != '@' ||
        method_getImplementation(method) != (IMP)(uintptr_t)(cache + offset)) {
        sweet_drm_log("sweetgl: %s contents format unchanged: native method guard", hook->name);
        return;
    }
    /* Publish a valid original before the runtime makes the wrapper visible. */
    __atomic_store_n(&hook->original, (format_setter)method_getImplementation(method), __ATOMIC_RELEASE);
    IMP previous = method_setImplementation(method, replacement);
    if (previous) __atomic_store_n(&hook->original, (format_setter)previous, __ATOMIC_RELEASE);
    sweet_drm_log("sweetgl: native %s contents-format negotiation installed", hook->name);
}

static void renegotiate_existing_contexts(const uint8_t *cache, size_t length)
{
    if (!__atomic_load_n(&context_hook.original, __ATOMIC_ACQUIRE)) return;
    const uintptr_t base = UINT64_C(0x180000000);
    const uintptr_t all_offset = UINT64_C(0x1882c1bf8) - base;
    const uintptr_t get_offset = UINT64_C(0x18853f044) - base;
    const uint32_t all_entry[] = {0xa9bf7bfd,0x910003fd,0x94007dae,0xb4000060};
    const uint32_t get_entry[] = {0xf9400408,0x3943a108,0x51000508,0x7100111f};
    Class cls = objc_getClass("CAContext");
    SEL all_selector = sel_registerName("allContexts");
    SEL get_selector = sel_registerName("contentsFormat");
    SEL set_selector = sel_registerName("setContentsFormat:");
    Method all = cls ? class_getClassMethod(cls, all_selector) : NULL;
    Method get = cls ? class_getInstanceMethod(cls, get_selector) : NULL;
    if (all_offset > length || sizeof(all_entry) > length - all_offset ||
        get_offset > length || sizeof(get_entry) > length - get_offset ||
        memcmp(cache + all_offset, all_entry, sizeof(all_entry)) ||
        memcmp(cache + get_offset, get_entry, sizeof(get_entry)) || !all || !get ||
        method_getImplementation(all) != (IMP)(uintptr_t)(cache + all_offset) ||
        method_getImplementation(get) != (IMP)(uintptr_t)(cache + get_offset)) {
        sweet_drm_log("sweetgl: existing contents formats unchanged: native method guard");
        return;
    }
    Class pool_class = objc_getClass("NSAutoreleasePool");
    if (!pool_class) return;
    id pool = ((id (*)(id, SEL))objc_msgSend)((id)pool_class, sel_registerName("new"));
    if (!pool) return;
    id snapshot = ((id (*)(id, SEL))objc_msgSend)((id)cls, all_selector);
    CFIndex count = 0, inspected = 0, changed = 0;
    if (snapshot && CFGetTypeID((CFTypeRef)snapshot) == CFArrayGetTypeID()) {
        count = CFArrayGetCount((CFArrayRef)snapshot);
        inspected = count > 64 ? 64 : count;
        for (CFIndex i = 0; i < inspected; ++i) {
            id object = (id)CFArrayGetValueAtIndex((CFArrayRef)snapshot, i);
            id requested = ((id (*)(id, SEL))objc_msgSend)(object, get_selector);
            if (needs_sdr(requested)) {
                context_set_contents_format(object, set_selector, requested);
                ++changed;
            }
        }
    }
    sweet_drm_log("sweetgl: existing contexts negotiated %ld, inspected %ld of %ld",
        (long)changed, (long)inspected, (long)count);
    ((void (*)(id, SEL))objc_msgSend)(pool, sel_registerName("drain"));
}

void peridot_surface_formats_install(void)
{
    const char *process = getprogname();
    if (strcmp(process, "SpringBoard") && strcmp(process, "backboardd")) return;
    size_t length = 0;
    const uint8_t *cache = peridot_surface_cache(&length);
    if (!cache) return;
    install_hook(cache, length, &context_hook, (IMP)context_set_contents_format);
    install_hook(cache, length, &layer_hook, (IMP)layer_set_contents_format);
    /* Native allContexts releases its list lock before returning the snapshot. */
    renegotiate_existing_contexts(cache, length);
}
