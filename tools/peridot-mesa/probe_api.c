/* SPDX-License-Identifier: MIT */
#include "mapi/glapi/glapi.h"

__attribute__((visibility("default"))) void *
sweetgl_get_proc_address(const char *name)
{
    return name ? (void *)_mesa_glapi_get_proc_address(name) : 0;
}
