/* SPDX-License-Identifier: MIT */
/* Read native refresh metadata once from the existing delayed main-queue dump. */
#include <stddef.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include "sweet_drm.h"

static SEL S(const char *n) { return sel_registerName(n); }
static id call(id o, const char *s) { return o ? ((id (*)(id, SEL))objc_msgSend)(o, S(s)) : nil; }

void
peridot_display_probe(void)
{
    Class screenClass = objc_getClass("UIScreen");
    Method mainScreen = screenClass ? class_getClassMethod(screenClass, S("mainScreen")) : NULL;
    if (mainScreen && method_getNumberOfArguments(mainScreen) == 2 &&
        method_getTypeEncoding(mainScreen)[0] == '@') {
        id screen = call((id)screenClass, "mainScreen");
        Method maximum = screen ? class_getInstanceMethod(object_getClass(screen), S("maximumFramesPerSecond")) : NULL;
        const char *encoding = maximum ? method_getTypeEncoding(maximum) : "";
        if (maximum && method_getNumberOfArguments(maximum) == 2 &&
            (encoding[0] == 'q' || encoding[0] == 'l' || encoding[0] == 'Q' || encoding[0] == 'L'))
            sweet_drm_log("sweetgl: native UIScreen maximumFramesPerSecond %ld",
                ((long (*)(id, SEL))objc_msgSend)(screen, S("maximumFramesPerSecond")));
    }
    Class displayClass = objc_getClass("CADisplay");
    Method mainDisplay = displayClass ? class_getClassMethod(displayClass, S("mainDisplay")) : NULL;
    if (!mainDisplay || method_getNumberOfArguments(mainDisplay) != 2 ||
        method_getTypeEncoding(mainDisplay)[0] != '@') return;
    id display = call((id)displayClass, "mainDisplay");
    const char *names[] = {"refreshRate", "hardwareRefreshRate"};
    for (unsigned i = 0; display && i < 2; ++i) {
        Method getter = class_getInstanceMethod(object_getClass(display), S(names[i]));
        if (!getter || method_getNumberOfArguments(getter) != 2 ||
            method_getTypeEncoding(getter)[0] != 'd') continue;
        double interval = ((double (*)(id, SEL))objc_msgSend)(display, S(names[i]));
        sweet_drm_log("sweetgl: native CADisplay %s interval %.9f s, reported %.3f Hz",
            names[i], interval, interval > 0.000001 && interval < 2.0 ? 1.0 / interval : 0.0);
    }
}
