/* SPDX-License-Identifier: MIT */
#ifndef PERIDOT_GPU_H
#define PERIDOT_GPU_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct peridot_gpu_ib { uint64_t address; uint32_t words, reserved; };
bool peridot_gpu_ready(void);
/* Kernel callers retain each actual16K physical page and descriptor until
 * unmap succeeds. These functions never accept physical addresses from users. */
uint64_t peridot_gpu_map(const uint64_t *pages, uint32_t count);
int peridot_gpu_unmap(uint64_t address);
int peridot_gpu_submit(const struct peridot_gpu_ib *, uint32_t count, uint32_t *sequence);
int peridot_gpu_wait(uint32_t sequence, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
