/*
 * Copyright © 2017 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/** @file vc4_tiling_lt.c
 *
 * Helper functions from vc4_tiling.c that will be compiled for using NEON
 * assembly or not.
 *
 * If VC4_BUILD_NEON is set, then the functions will be suffixed with _neon.
 * They will only use NEON assembly if __ARM_ARCH is also set, to keep the x86
 * sim build working.
 */

#include <string.h>
#include "pipe/p_state.h"
#include "vc4_tiling.h"

#ifdef VC4_BUILD_NEON
#define NEON_TAG(x) x ## _neon
#else
#define NEON_TAG(x) x ## _base
#endif

/** Returns the stride in bytes of a 64-byte microtile. */
static uint32_t
vc4_utile_stride(int cpp)
{
        switch (cpp) {
        case 1:
                return 8;
        case 2:
        case 4:
        case 8:
                return 16;
        default:
                unreachable("bad cpp");
        }
}

static void
vc4_load_utile(void *cpu, void *gpu, uint32_t cpu_stride, uint32_t cpp)
{
        uint32_t gpu_stride = vc4_utile_stride(cpp);
#if defined(VC4_BUILD_NEON) && defined(PIPE_ARCH_ARM)
        if (gpu_stride == 8) {
                __asm__ volatile (
                        /* Load from the GPU in one shot, no interleave, to
                         * d0-d7.
                         */
                        "vldm %0, {q0, q1, q2, q3}\n"
                        /* Store each 8-byte line to cpu-side destination,
                         * incrementing it by the stride each time.
                         */
                        "vst1.8 d0, [%1], %2\n"
                        "vst1.8 d1, [%1], %2\n"
                        "vst1.8 d2, [%1], %2\n"
                        "vst1.8 d3, [%1], %2\n"
                        "vst1.8 d4, [%1], %2\n"
                        "vst1.8 d5, [%1], %2\n"
                        "vst1.8 d6, [%1], %2\n"
                        "vst1.8 d7, [%1]\n"
                        :
                        : "r"(gpu), "r"(cpu), "r"(cpu_stride)
                        : "q0", "q1", "q2", "q3");
        } else {
                assert(gpu_stride == 16);
                __asm__ volatile (
                        /* Load from the GPU in one shot, no interleave, to
                         * d0-d7.
                         */
                        "vldm %0, {q0, q1, q2, q3};\n"
                        /* Store each 16-byte line in 2 parts to the cpu-side
                         * destination.  (vld1 can only store one d-register
                         * at a time).
                         */
                        "vst1.8 d0, [%1], %3\n"
                        "vst1.8 d1, [%2], %3\n"
                        "vst1.8 d2, [%1], %3\n"
                        "vst1.8 d3, [%2], %3\n"
                        "vst1.8 d4, [%1], %3\n"
                        "vst1.8 d5, [%2], %3\n"
                        "vst1.8 d6, [%1]\n"
                        "vst1.8 d7, [%2]\n"
                        :
                        : "r"(gpu), "r"(cpu), "r"(cpu + 8), "r"(cpu_stride)
                        : "q0", "q1", "q2", "q3");
        }
#elif defined (PIPE_ARCH_AARCH64)
	if (gpu_stride == 8) {
                __asm__ volatile (
                        /* Load from the GPU in one shot, no interleave, to
                         * d0-d7.
                         */
                        "ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%0]\n"
                        /* Store each 8-byte line to cpu-side destination,
                         * incrementing it by the stride each time.
                         */
                        "st1 {v0.D}[0], [%1], %2\n"
                        "st1 {v0.D}[1], [%1], %2\n"
                        "st1 {v1.D}[0], [%1], %2\n"
                        "st1 {v1.D}[1], [%1], %2\n"
                        "st1 {v2.D}[0], [%1], %2\n"
                        "st1 {v2.D}[1], [%1], %2\n"
                        "st1 {v3.D}[0], [%1], %2\n"
                        "st1 {v3.D}[1], [%1]\n"
			:
                        : "r"(gpu), "r"(cpu), "r"(cpu_stride)
                        : "v0", "v1", "v2", "v3");
        } else {
                assert(gpu_stride == 16);
                __asm__ volatile (
                        /* Load from the GPU in one shot, no interleave, to
                         * d0-d7.
                         */
                        "ld1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%0]\n"
                        /* Store each 16-byte line in 2 parts to the cpu-side
                         * destination.  (vld1 can only store one d-register
                         * at a time).
                         */
                        "st1 {v0.D}[0], [%1], %3\n"
                        "st1 {v0.D}[1], [%2], %3\n"
                        "st1 {v1.D}[0], [%1], %3\n"
                        "st1 {v1.D}[1], [%2], %3\n"
                        "st1 {v2.D}[0], [%1], %3\n"
                        "st1 {v2.D}[1], [%2], %3\n"
                        "st1 {v3.D}[0], [%1]\n"
                        "st1 {v3.D}[1], [%2]\n"
                        :
                        : "r"(gpu), "r"(cpu), "r"(cpu + 8), "r"(cpu_stride)
                        : "v0", "v1", "v2", "v3");
        }
#else
        for (uint32_t gpu_offset = 0; gpu_offset < 64; gpu_offset += gpu_stride) {
                memcpy(cpu, gpu + gpu_offset, gpu_stride);
                cpu += cpu_stride;
        }
#endif
}

static void
vc4_store_utile(void *gpu, void *cpu, uint32_t cpu_stride, uint32_t cpp)
{
        uint32_t gpu_stride = vc4_utile_stride(cpp);

#if defined(VC4_BUILD_NEON) && defined(PIPE_ARCH_ARM)
        if (gpu_stride == 8) {
                __asm__ volatile (
                        /* Load each 8-byte line from cpu-side source,
                         * incrementing it by the stride each time.
                         */
                        "vld1.8 d0, [%1], %2\n"
                        "vld1.8 d1, [%1], %2\n"
                        "vld1.8 d2, [%1], %2\n"
                        "vld1.8 d3, [%1], %2\n"
                        "vld1.8 d4, [%1], %2\n"
                        "vld1.8 d5, [%1], %2\n"
                        "vld1.8 d6, [%1], %2\n"
                        "vld1.8 d7, [%1]\n"
                        /* Load from the GPU in one shot, no interleave, to
                         * d0-d7.
                         */
                        "vstm %0, {q0, q1, q2, q3}\n"
                        :
                        : "r"(gpu), "r"(cpu), "r"(cpu_stride)
                        : "q0", "q1", "q2", "q3");
        } else {
                assert(gpu_stride == 16);
                __asm__ volatile (
                        /* Load each 16-byte line in 2 parts from the cpu-side
                         * destination.  (vld1 can only store one d-register
                         * at a time).
                         */
                        "vld1.8 d0, [%1], %3\n"
                        "vld1.8 d1, [%2], %3\n"
                        "vld1.8 d2, [%1], %3\n"
                        "vld1.8 d3, [%2], %3\n"
                        "vld1.8 d4, [%1], %3\n"
                        "vld1.8 d5, [%2], %3\n"
                        "vld1.8 d6, [%1]\n"
                        "vld1.8 d7, [%2]\n"
                        /* Store to the GPU in one shot, no interleave. */
                        "vstm %0, {q0, q1, q2, q3}\n"
                        :
                        : "r"(gpu), "r"(cpu), "r"(cpu + 8), "r"(cpu_stride)
                        : "q0", "q1", "q2", "q3");
        }
#elif defined (PIPE_ARCH_AARCH64)
	if (gpu_stride == 8) {
                __asm__ volatile (
                        /* Load each 8-byte line from cpu-side source,
                         * incrementing it by the stride each time.
                         */
                        "ld1 {v0.D}[0], [%1], %2\n"
                        "ld1 {v0.D}[1], [%1], %2\n"
                        "ld1 {v1.D}[0], [%1], %2\n"
                        "ld1 {v1.D}[1], [%1], %2\n"
                        "ld1 {v2.D}[0], [%1], %2\n"
                        "ld1 {v2.D}[1], [%1], %2\n"
                        "ld1 {v3.D}[0], [%1], %2\n"
                        "ld1 {v3.D}[1], [%1]\n"
                        /* Store to the GPU in one shot, no interleave. */
                        "st1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%0]\n"
                        :
                        : "r"(gpu), "r"(cpu), "r"(cpu_stride)
                        : "v0", "v1", "v2", "v3");
        } else {
                assert(gpu_stride == 16);
                __asm__ volatile (
                        /* Load each 16-byte line in 2 parts from the cpu-side
                         * destination.  (vld1 can only store one d-register
                         * at a time).
                         */
                        "ld1 {v0.D}[0], [%1], %3\n"
                        "ld1 {v0.D}[1], [%2], %3\n"
                        "ld1 {v1.D}[0], [%1], %3\n"
                        "ld1 {v1.D}[1], [%2], %3\n"
                        "ld1 {v2.D}[0], [%1], %3\n"
                        "ld1 {v2.D}[1], [%2], %3\n"
                        "ld1 {v3.D}[0], [%1]\n"
                        "ld1 {v3.D}[1], [%2]\n"
                        /* Store to the GPU in one shot, no interleave. */
                        "st1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%0]\n"
                        :
                        : "r"(gpu), "r"(cpu), "r"(cpu + 8), "r"(cpu_stride)
                        : "v0", "v1", "v2", "v3");
        }
#else
        for (uint32_t gpu_offset = 0; gpu_offset < 64; gpu_offset += gpu_stride) {
                memcpy(gpu + gpu_offset, cpu, gpu_stride);
                cpu += cpu_stride;
        }
#endif

}

/**
 * Helper for loading or storing to an LT image, where the box is aligned
 * to utiles.
 *
 * This just breaks the box down into calls to the fast
 * vc4_load_utile/vc4_store_utile helpers.
 */
static inline void
vc4_lt_image_helper(void *gpu, uint32_t gpu_stride,
                    void *cpu, uint32_t cpu_stride,
                    int cpp, const struct pipe_box *box, bool to_cpu)
{
        uint32_t utile_w = vc4_utile_width(cpp);
        uint32_t utile_h = vc4_utile_height(cpp);
        uint32_t xstart = box->x;
        uint32_t ystart = box->y;

        for (uint32_t y = 0; y < box->height; y += utile_h) {
                for (uint32_t x = 0; x < box->width; x += utile_w) {
                        void *gpu_tile = gpu + ((ystart + y) * gpu_stride +
                                                (xstart + x) * 64 / utile_w);
                        if (to_cpu) {
                                vc4_load_utile(cpu + (cpu_stride * y +
                                                      x * cpp),
                                               gpu_tile,
                                               cpu_stride, cpp);
                        } else {
                                vc4_store_utile(gpu_tile,
                                                cpu + (cpu_stride * y +
                                                       x * cpp),
                                                cpu_stride, cpp);
                        }
                }
        }
}

static inline void
vc4_lt_image_cpp_helper(void *gpu, uint32_t gpu_stride,
                        void *cpu, uint32_t cpu_stride,
                        int cpp, const struct pipe_box *box, bool to_cpu)
{
        switch (cpp) {
        case 1:
                vc4_lt_image_helper(gpu, gpu_stride, cpu, cpu_stride, 1, box,
                                    to_cpu);
                break;
        case 2:
                vc4_lt_image_helper(gpu, gpu_stride, cpu, cpu_stride, 2, box,
                                    to_cpu);
                break;
        case 4:
                vc4_lt_image_helper(gpu, gpu_stride, cpu, cpu_stride, 4, box,
                                    to_cpu);
                break;
        case 8:
                vc4_lt_image_helper(gpu, gpu_stride, cpu, cpu_stride, 8, box,
                                    to_cpu);
                break;
        default:
                unreachable("bad cpp");
        }
}

void
NEON_TAG(vc4_load_lt_image)(void *dst, uint32_t dst_stride,
                            void *src, uint32_t src_stride,
                            int cpp, const struct pipe_box *box)
{
        vc4_lt_image_cpp_helper(src, src_stride, dst, dst_stride, cpp, box,
                                true);
}

void
NEON_TAG(vc4_store_lt_image)(void *dst, uint32_t dst_stride,
                             void *src, uint32_t src_stride,
                             int cpp, const struct pipe_box *box)
{
        vc4_lt_image_cpp_helper(dst, dst_stride, src, src_stride, cpp, box,
                                false);
}
