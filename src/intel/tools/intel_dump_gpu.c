/*
 * Copyright © 2015 Intel Corporation
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <i915_drm.h>

#include "intel_aub.h"

#include "dev/gen_device_info.h"
#include "util/macros.h"

#ifndef ALIGN
#define ALIGN(x, y) (((x) + (y)-1) & ~((y)-1))
#endif

#define MI_LOAD_REGISTER_IMM_n(n) ((0x22 << 23) | (2 * (n) - 1))
#define MI_LRI_FORCE_POSTED       (1<<12)

#define MI_BATCH_NON_SECURE_I965 (1 << 8)

#define MI_BATCH_BUFFER_END (0xA << 23)

#define min(a, b) ({                            \
         __typeof(a) _a = (a);                  \
         __typeof(b) _b = (b);                  \
         _a < _b ? _a : _b;                     \
      })

#define max(a, b) ({                            \
         __typeof(a) _a = (a);                  \
         __typeof(b) _b = (b);                  \
         _a > _b ? _a : _b;                     \
      })

#define HWS_PGA_RCSUNIT      0x02080
#define HWS_PGA_VCSUNIT0   0x12080
#define HWS_PGA_BCSUNIT      0x22080

#define GFX_MODE_RCSUNIT   0x0229c
#define GFX_MODE_VCSUNIT0   0x1229c
#define GFX_MODE_BCSUNIT   0x2229c

#define EXECLIST_SUBMITPORT_RCSUNIT   0x02230
#define EXECLIST_SUBMITPORT_VCSUNIT0   0x12230
#define EXECLIST_SUBMITPORT_BCSUNIT   0x22230

#define EXECLIST_STATUS_RCSUNIT      0x02234
#define EXECLIST_STATUS_VCSUNIT0   0x12234
#define EXECLIST_STATUS_BCSUNIT      0x22234

#define EXECLIST_SQ_CONTENTS0_RCSUNIT   0x02510
#define EXECLIST_SQ_CONTENTS0_VCSUNIT0   0x12510
#define EXECLIST_SQ_CONTENTS0_BCSUNIT   0x22510

#define EXECLIST_CONTROL_RCSUNIT   0x02550
#define EXECLIST_CONTROL_VCSUNIT0   0x12550
#define EXECLIST_CONTROL_BCSUNIT   0x22550

#define MEMORY_MAP_SIZE (64 /* MiB */ * 1024 * 1024)

#define PTE_SIZE 4
#define GEN8_PTE_SIZE 8

#define NUM_PT_ENTRIES (ALIGN(MEMORY_MAP_SIZE, 4096) / 4096)
#define PT_SIZE ALIGN(NUM_PT_ENTRIES * GEN8_PTE_SIZE, 4096)

#define RING_SIZE         (1 * 4096)
#define PPHWSP_SIZE         (1 * 4096)
#define GEN11_LR_CONTEXT_RENDER_SIZE    (14 * 4096)
#define GEN10_LR_CONTEXT_RENDER_SIZE    (19 * 4096)
#define GEN9_LR_CONTEXT_RENDER_SIZE     (22 * 4096)
#define GEN8_LR_CONTEXT_RENDER_SIZE     (20 * 4096)
#define GEN8_LR_CONTEXT_OTHER_SIZE      (2 * 4096)


#define STATIC_GGTT_MAP_START 0

#define RENDER_RING_ADDR STATIC_GGTT_MAP_START
#define RENDER_CONTEXT_ADDR (RENDER_RING_ADDR + RING_SIZE)

#define BLITTER_RING_ADDR (RENDER_CONTEXT_ADDR + PPHWSP_SIZE + GEN10_LR_CONTEXT_RENDER_SIZE)
#define BLITTER_CONTEXT_ADDR (BLITTER_RING_ADDR + RING_SIZE)

#define VIDEO_RING_ADDR (BLITTER_CONTEXT_ADDR + PPHWSP_SIZE + GEN8_LR_CONTEXT_OTHER_SIZE)
#define VIDEO_CONTEXT_ADDR (VIDEO_RING_ADDR + RING_SIZE)

#define STATIC_GGTT_MAP_END (VIDEO_CONTEXT_ADDR + PPHWSP_SIZE + GEN8_LR_CONTEXT_OTHER_SIZE)
#define STATIC_GGTT_MAP_SIZE (STATIC_GGTT_MAP_END - STATIC_GGTT_MAP_START)

#define PML4_PHYS_ADDR ((uint64_t)(STATIC_GGTT_MAP_END))

#define CONTEXT_FLAGS (0x339)   /* Normal Priority | L3-LLC Coherency |
                                 * PPGTT Enabled |
                                 * Legacy Context with 64 bit VA support |
                                 * Valid
                                 */

#define RENDER_CONTEXT_DESCRIPTOR  ((uint64_t)1 << 62 | RENDER_CONTEXT_ADDR  | CONTEXT_FLAGS)
#define BLITTER_CONTEXT_DESCRIPTOR ((uint64_t)2 << 62 | BLITTER_CONTEXT_ADDR | CONTEXT_FLAGS)
#define VIDEO_CONTEXT_DESCRIPTOR   ((uint64_t)3 << 62 | VIDEO_CONTEXT_ADDR   | CONTEXT_FLAGS)

static const uint32_t render_context_init[GEN9_LR_CONTEXT_RENDER_SIZE / /* Choose the largest */
                                          sizeof(uint32_t)] = {
   0 /* MI_NOOP */,
   MI_LOAD_REGISTER_IMM_n(14) | MI_LRI_FORCE_POSTED,
   0x2244 /* CONTEXT_CONTROL */,      0x90009 /* Inhibit Synchronous Context Switch | Engine Context Restore Inhibit */,
   0x2034 /* RING_HEAD */,         0,
   0x2030 /* RING_TAIL */,         0,
   0x2038 /* RING_BUFFER_START */,      RENDER_RING_ADDR,
   0x203C /* RING_BUFFER_CONTROL */,   (RING_SIZE - 4096) | 1 /* Buffer Length | Ring Buffer Enable */,
   0x2168 /* BB_HEAD_U */,         0,
   0x2140 /* BB_HEAD_L */,         0,
   0x2110 /* BB_STATE */,         0,
   0x211C /* SECOND_BB_HEAD_U */,      0,
   0x2114 /* SECOND_BB_HEAD_L */,      0,
   0x2118 /* SECOND_BB_STATE */,      0,
   0x21C0 /* BB_PER_CTX_PTR */,      0,
   0x21C4 /* RCS_INDIRECT_CTX */,      0,
   0x21C8 /* RCS_INDIRECT_CTX_OFFSET */,   0,
   /* MI_NOOP */
   0, 0,

   0 /* MI_NOOP */,
   MI_LOAD_REGISTER_IMM_n(9) | MI_LRI_FORCE_POSTED,
   0x23A8 /* CTX_TIMESTAMP */,   0,
   0x228C /* PDP3_UDW */,      0,
   0x2288 /* PDP3_LDW */,      0,
   0x2284 /* PDP2_UDW */,      0,
   0x2280 /* PDP2_LDW */,      0,
   0x227C /* PDP1_UDW */,      0,
   0x2278 /* PDP1_LDW */,      0,
   0x2274 /* PDP0_UDW */,      PML4_PHYS_ADDR >> 32,
   0x2270 /* PDP0_LDW */,      PML4_PHYS_ADDR,
   /* MI_NOOP */
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

   0 /* MI_NOOP */,
   MI_LOAD_REGISTER_IMM_n(1),
   0x20C8 /* R_PWR_CLK_STATE */, 0x7FFFFFFF,
   MI_BATCH_BUFFER_END
};

static const uint32_t blitter_context_init[GEN8_LR_CONTEXT_OTHER_SIZE /
                                           sizeof(uint32_t)] = {
   0 /* MI_NOOP */,
   MI_LOAD_REGISTER_IMM_n(11) | MI_LRI_FORCE_POSTED,
   0x22244 /* CONTEXT_CONTROL */,      0x90009 /* Inhibit Synchronous Context Switch | Engine Context Restore Inhibit */,
   0x22034 /* RING_HEAD */,      0,
   0x22030 /* RING_TAIL */,      0,
   0x22038 /* RING_BUFFER_START */,   BLITTER_RING_ADDR,
   0x2203C /* RING_BUFFER_CONTROL */,   (RING_SIZE - 4096) | 1 /* Buffer Length | Ring Buffer Enable */,
   0x22168 /* BB_HEAD_U */,      0,
   0x22140 /* BB_HEAD_L */,      0,
   0x22110 /* BB_STATE */,         0,
   0x2211C /* SECOND_BB_HEAD_U */,      0,
   0x22114 /* SECOND_BB_HEAD_L */,      0,
   0x22118 /* SECOND_BB_STATE */,      0,
   /* MI_NOOP */
   0, 0, 0, 0, 0, 0, 0, 0,

   0 /* MI_NOOP */,
   MI_LOAD_REGISTER_IMM_n(9) | MI_LRI_FORCE_POSTED,
   0x223A8 /* CTX_TIMESTAMP */,   0,
   0x2228C /* PDP3_UDW */,      0,
   0x22288 /* PDP3_LDW */,      0,
   0x22284 /* PDP2_UDW */,      0,
   0x22280 /* PDP2_LDW */,      0,
   0x2227C /* PDP1_UDW */,      0,
   0x22278 /* PDP1_LDW */,      0,
   0x22274 /* PDP0_UDW */,      PML4_PHYS_ADDR >> 32,
   0x22270 /* PDP0_LDW */,      PML4_PHYS_ADDR,
   /* MI_NOOP */
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

   MI_BATCH_BUFFER_END
};

static const uint32_t video_context_init[GEN8_LR_CONTEXT_OTHER_SIZE /
                                         sizeof(uint32_t)] = {
   0 /* MI_NOOP */,
   MI_LOAD_REGISTER_IMM_n(11) | MI_LRI_FORCE_POSTED,
   0x1C244 /* CONTEXT_CONTROL */,      0x90009 /* Inhibit Synchronous Context Switch | Engine Context Restore Inhibit */,
   0x1C034 /* RING_HEAD */,      0,
   0x1C030 /* RING_TAIL */,      0,
   0x1C038 /* RING_BUFFER_START */,   VIDEO_RING_ADDR,
   0x1C03C /* RING_BUFFER_CONTROL */,   (RING_SIZE - 4096) | 1 /* Buffer Length | Ring Buffer Enable */,
   0x1C168 /* BB_HEAD_U */,      0,
   0x1C140 /* BB_HEAD_L */,      0,
   0x1C110 /* BB_STATE */,         0,
   0x1C11C /* SECOND_BB_HEAD_U */,      0,
   0x1C114 /* SECOND_BB_HEAD_L */,      0,
   0x1C118 /* SECOND_BB_STATE */,      0,
   /* MI_NOOP */
   0, 0, 0, 0, 0, 0, 0, 0,

   0 /* MI_NOOP */,
   MI_LOAD_REGISTER_IMM_n(9) | MI_LRI_FORCE_POSTED,
   0x1C3A8 /* CTX_TIMESTAMP */,   0,
   0x1C28C /* PDP3_UDW */,      0,
   0x1C288 /* PDP3_LDW */,      0,
   0x1C284 /* PDP2_UDW */,      0,
   0x1C280 /* PDP2_LDW */,      0,
   0x1C27C /* PDP1_UDW */,      0,
   0x1C278 /* PDP1_LDW */,      0,
   0x1C274 /* PDP0_UDW */,      PML4_PHYS_ADDR >> 32,
   0x1C270 /* PDP0_LDW */,      PML4_PHYS_ADDR,
   /* MI_NOOP */
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

   MI_BATCH_BUFFER_END
};

static int close_init_helper(int fd);
static int ioctl_init_helper(int fd, unsigned long request, ...);

static int (*libc_close)(int fd) = close_init_helper;
static int (*libc_ioctl)(int fd, unsigned long request, ...) = ioctl_init_helper;

static int drm_fd = -1;
static char *filename = NULL;
static FILE *files[2] = { NULL, NULL };
static struct gen_device_info devinfo = {0};
static int verbose = 0;
static bool device_override;
static uint32_t device;
static int addr_bits = 0;

#define MAX_BO_COUNT 64 * 1024

struct bo {
   uint32_t size;
   uint64_t offset;
   void *map;
};

static struct bo *bos;

#define DRM_MAJOR 226

#ifndef DRM_I915_GEM_USERPTR

#define DRM_I915_GEM_USERPTR      0x33
#define DRM_IOCTL_I915_GEM_USERPTR   DRM_IOWR (DRM_COMMAND_BASE + DRM_I915_GEM_USERPTR, struct drm_i915_gem_userptr)

struct drm_i915_gem_userptr {
   __u64 user_ptr;
   __u64 user_size;
   __u32 flags;
#define I915_USERPTR_READ_ONLY 0x1
#define I915_USERPTR_UNSYNCHRONIZED 0x80000000
   /**
    * Returned handle for the object.
    *
    * Object handles are nonzero.
    */
   __u32 handle;
};

#endif

/* We set bit 0 in the map pointer for userptr BOs so we know not to
 * munmap them on DRM_IOCTL_GEM_CLOSE.
 */
#define USERPTR_FLAG 1
#define IS_USERPTR(p) ((uintptr_t) (p) & USERPTR_FLAG)
#define GET_PTR(p) ( (void *) ((uintptr_t) p & ~(uintptr_t) 1) )

#ifndef I915_EXEC_BATCH_FIRST
#define I915_EXEC_BATCH_FIRST (1 << 18)
#endif

static inline bool use_execlists(void)
{
   return devinfo.gen >= 8;
}

static void __attribute__ ((format(__printf__, 2, 3)))
fail_if(int cond, const char *format, ...)
{
   va_list args;

   if (!cond)
      return;

   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);

   raise(SIGTRAP);
}

static struct bo *
get_bo(uint32_t handle)
{
   struct bo *bo;

   fail_if(handle >= MAX_BO_COUNT, "bo handle too large\n");
   bo = &bos[handle];

   return bo;
}

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   return (v + a - 1) & ~(a - 1);
}

static void
dword_out(uint32_t data)
{
   for (int i = 0; i < ARRAY_SIZE (files); i++) {
      if (files[i] == NULL)
         continue;

      fail_if(fwrite(&data, 1, 4, files[i]) == 0,
              "Writing to output failed\n");
   }
}

static void
data_out(const void *data, size_t size)
{
   if (size == 0)
      return;

   for (int i = 0; i < ARRAY_SIZE (files); i++) {
      if (files[i] == NULL)
         continue;

      fail_if(fwrite(data, 1, size, files[i]) == 0,
              "Writing to output failed\n");
   }
}

static uint32_t
gtt_size(void)
{
   return NUM_PT_ENTRIES * (addr_bits > 32 ? GEN8_PTE_SIZE : PTE_SIZE);
}

static void
mem_trace_memory_write_header_out(uint64_t addr, uint32_t len,
                                  uint32_t addr_space)
{
   uint32_t dwords = ALIGN(len, sizeof(uint32_t)) / sizeof(uint32_t);

   dword_out(CMD_MEM_TRACE_MEMORY_WRITE | (5 + dwords - 1));
   dword_out(addr & 0xFFFFFFFF);   /* addr lo */
   dword_out(addr >> 32);   /* addr hi */
   dword_out(addr_space);   /* gtt */
   dword_out(len);
}

static void
register_write_out(uint32_t addr, uint32_t value)
{
   uint32_t dwords = 1;

   dword_out(CMD_MEM_TRACE_REGISTER_WRITE | (5 + dwords - 1));
   dword_out(addr);
   dword_out(AUB_MEM_TRACE_REGISTER_SIZE_DWORD |
             AUB_MEM_TRACE_REGISTER_SPACE_MMIO);
   dword_out(0xFFFFFFFF);   /* mask lo */
   dword_out(0x00000000);   /* mask hi */
   dword_out(value);
}

static struct ppgtt_table {
   uint64_t phys_addr;
   struct ppgtt_table *subtables[512];
} pml4 = {PML4_PHYS_ADDR};

static void
populate_ppgtt_table(struct ppgtt_table *table, int start, int end,
                     int level)
{
   static uint64_t phys_addrs_allocator = (PML4_PHYS_ADDR >> 12) + 1;
   uint64_t entries[512] = {0};
   int dirty_start = 512, dirty_end = 0;

   for (int i = start; i <= end; i++) {
      if (!table->subtables[i]) {
         dirty_start = min(dirty_start, i);
         dirty_end = max(dirty_end, i);
         if (level == 1) {
            table->subtables[i] =
               (void *)(phys_addrs_allocator++ << 12);
         } else {
            table->subtables[i] =
               calloc(1, sizeof(struct ppgtt_table));
            table->subtables[i]->phys_addr =
               phys_addrs_allocator++ << 12;
         }
      }
      entries[i] = 3 /* read/write | present */ |
         (level == 1 ? (uint64_t)table->subtables[i] :
          table->subtables[i]->phys_addr);
   }

   if (dirty_start <= dirty_end) {
      uint64_t write_addr = table->phys_addr + dirty_start *
         sizeof(uint64_t);
      uint64_t write_size = (dirty_end - dirty_start + 1) *
         sizeof(uint64_t);
      mem_trace_memory_write_header_out(write_addr, write_size,
                                        AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_PHYSICAL);
      data_out(entries + dirty_start, write_size);
   }
}

static void
map_ppgtt(uint64_t start, uint64_t size)
{
   uint64_t l4_start = start & 0xff8000000000;
   uint64_t l3_start = start & 0xffffc0000000;
   uint64_t l2_start = start & 0xffffffe00000;
   uint64_t l1_start = start & 0xfffffffff000;
   uint64_t l4_end = ((start + size - 1) | 0x007fffffffff) & 0xffffffffffff;
   uint64_t l3_end = ((start + size - 1) | 0x00003fffffff) & 0xffffffffffff;
   uint64_t l2_end = ((start + size - 1) | 0x0000001fffff) & 0xffffffffffff;
   uint64_t l1_end = ((start + size - 1) | 0x000000000fff) & 0xffffffffffff;

#define L4_index(addr) (((addr) >> 39) & 0x1ff)
#define L3_index(addr) (((addr) >> 30) & 0x1ff)
#define L2_index(addr) (((addr) >> 21) & 0x1ff)
#define L1_index(addr) (((addr) >> 12) & 0x1ff)

#define L3_table(addr) (pml4.subtables[L4_index(addr)])
#define L2_table(addr) (L3_table(addr)->subtables[L3_index(addr)])
#define L1_table(addr) (L2_table(addr)->subtables[L2_index(addr)])

   populate_ppgtt_table(&pml4, L4_index(l4_start), L4_index(l4_end), 4);

   for (uint64_t a = l4_start; a < l4_end; a += (1ULL << 39)) {
      uint64_t _start = max(a, l3_start);
      uint64_t _end = min(a + (1ULL << 39), l3_end);

      populate_ppgtt_table(L3_table(a), L3_index(_start),
                           L3_index(_end), 3);
   }

   for (uint64_t a = l3_start; a < l3_end; a += (1ULL << 30)) {
      uint64_t _start = max(a, l2_start);
      uint64_t _end = min(a + (1ULL << 30), l2_end);

      populate_ppgtt_table(L2_table(a), L2_index(_start),
                           L2_index(_end), 2);
   }

   for (uint64_t a = l2_start; a < l2_end; a += (1ULL << 21)) {
      uint64_t _start = max(a, l1_start);
      uint64_t _end = min(a + (1ULL << 21), l1_end);

      populate_ppgtt_table(L1_table(a), L1_index(_start),
                           L1_index(_end), 1);
   }
}

static uint64_t
ppgtt_lookup(uint64_t ppgtt_addr)
{
   return (uint64_t)L1_table(ppgtt_addr)->subtables[L1_index(ppgtt_addr)];
}

static void
write_execlists_header(void)
{
   char app_name[8 * 4];
   int app_name_len, dwords;

   app_name_len =
      snprintf(app_name, sizeof(app_name), "PCI-ID=0x%X %s", device,
               program_invocation_short_name);
   app_name_len = ALIGN(app_name_len, sizeof(uint32_t));

   dwords = 5 + app_name_len / sizeof(uint32_t);
   dword_out(CMD_MEM_TRACE_VERSION | (dwords - 1));
   dword_out(AUB_MEM_TRACE_VERSION_FILE_VERSION);
   dword_out(devinfo.simulator_id << AUB_MEM_TRACE_VERSION_DEVICE_SHIFT);
   dword_out(0);      /* version */
   dword_out(0);      /* version */
   data_out(app_name, app_name_len);

   /* GGTT PT */
   uint32_t ggtt_ptes = STATIC_GGTT_MAP_SIZE >> 12;

   mem_trace_memory_write_header_out(STATIC_GGTT_MAP_START >> 12,
                                     ggtt_ptes * GEN8_PTE_SIZE,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT_ENTRY);
   for (uint32_t i = 0; i < ggtt_ptes; i++) {
      dword_out(1 + 0x1000 * i + STATIC_GGTT_MAP_START);
      dword_out(0);
   }

   /* RENDER_RING */
   mem_trace_memory_write_header_out(RENDER_RING_ADDR, RING_SIZE,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < RING_SIZE; i += sizeof(uint32_t))
      dword_out(0);

   /* RENDER_PPHWSP */
   mem_trace_memory_write_header_out(RENDER_CONTEXT_ADDR,
                                     PPHWSP_SIZE +
                                     sizeof(render_context_init),
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < PPHWSP_SIZE; i += sizeof(uint32_t))
      dword_out(0);

   /* RENDER_CONTEXT */
   data_out(render_context_init, sizeof(render_context_init));

   /* BLITTER_RING */
   mem_trace_memory_write_header_out(BLITTER_RING_ADDR, RING_SIZE,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < RING_SIZE; i += sizeof(uint32_t))
      dword_out(0);

   /* BLITTER_PPHWSP */
   mem_trace_memory_write_header_out(BLITTER_CONTEXT_ADDR,
                                     PPHWSP_SIZE +
                                     sizeof(blitter_context_init),
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < PPHWSP_SIZE; i += sizeof(uint32_t))
      dword_out(0);

   /* BLITTER_CONTEXT */
   data_out(blitter_context_init, sizeof(blitter_context_init));

   /* VIDEO_RING */
   mem_trace_memory_write_header_out(VIDEO_RING_ADDR, RING_SIZE,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < RING_SIZE; i += sizeof(uint32_t))
      dword_out(0);

   /* VIDEO_PPHWSP */
   mem_trace_memory_write_header_out(VIDEO_CONTEXT_ADDR,
                                     PPHWSP_SIZE +
                                     sizeof(video_context_init),
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < PPHWSP_SIZE; i += sizeof(uint32_t))
      dword_out(0);

   /* VIDEO_CONTEXT */
   data_out(video_context_init, sizeof(video_context_init));

   register_write_out(HWS_PGA_RCSUNIT, RENDER_CONTEXT_ADDR);
   register_write_out(HWS_PGA_VCSUNIT0, VIDEO_CONTEXT_ADDR);
   register_write_out(HWS_PGA_BCSUNIT, BLITTER_CONTEXT_ADDR);

   register_write_out(GFX_MODE_RCSUNIT, 0x80008000 /* execlist enable */);
   register_write_out(GFX_MODE_VCSUNIT0, 0x80008000 /* execlist enable */);
   register_write_out(GFX_MODE_BCSUNIT, 0x80008000 /* execlist enable */);
}

static void write_legacy_header(void)
{
   char app_name[8 * 4];
   char comment[16];
   int comment_len, comment_dwords, dwords;
   uint32_t entry = 0x200003;

   comment_len = snprintf(comment, sizeof(comment), "PCI-ID=0x%x", device);
   comment_dwords = ((comment_len + 3) / 4);

   /* Start with a (required) version packet. */
   dwords = 13 + comment_dwords;
   dword_out(CMD_AUB_HEADER | (dwords - 2));
   dword_out((4 << AUB_HEADER_MAJOR_SHIFT) |
             (0 << AUB_HEADER_MINOR_SHIFT));

   /* Next comes a 32-byte application name. */
   strncpy(app_name, program_invocation_short_name, sizeof(app_name));
   app_name[sizeof(app_name) - 1] = 0;
   data_out(app_name, sizeof(app_name));

   dword_out(0); /* timestamp */
   dword_out(0); /* timestamp */
   dword_out(comment_len);
   data_out(comment, comment_dwords * 4);

   /* Set up the GTT. The max we can handle is 64M */
   dword_out(CMD_AUB_TRACE_HEADER_BLOCK | ((addr_bits > 32 ? 6 : 5) - 2));
   dword_out(AUB_TRACE_MEMTYPE_GTT_ENTRY |
             AUB_TRACE_TYPE_NOTYPE | AUB_TRACE_OP_DATA_WRITE);
   dword_out(0); /* subtype */
   dword_out(0); /* offset */
   dword_out(gtt_size()); /* size */
   if (addr_bits > 32)
      dword_out(0);
   for (uint32_t i = 0; i < NUM_PT_ENTRIES; i++) {
      dword_out(entry + 0x1000 * i);
      if (addr_bits > 32)
         dword_out(0);
   }
}

/**
 * Break up large objects into multiple writes.  Otherwise a 128kb VBO
 * would overflow the 16 bits of size field in the packet header and
 * everything goes badly after that.
 */
static void
aub_write_trace_block(uint32_t type, void *virtual, uint32_t size, uint64_t gtt_offset)
{
   uint32_t block_size;
   uint32_t subtype = 0;
   static const char null_block[8 * 4096];

   for (uint32_t offset = 0; offset < size; offset += block_size) {
      block_size = min(8 * 4096, size - offset);

      if (use_execlists()) {
         block_size = min(4096, block_size);
         mem_trace_memory_write_header_out(ppgtt_lookup(gtt_offset + offset),
                                           block_size,
                                           AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_PHYSICAL);
      } else {
         dword_out(CMD_AUB_TRACE_HEADER_BLOCK |
                   ((addr_bits > 32 ? 6 : 5) - 2));
         dword_out(AUB_TRACE_MEMTYPE_GTT |
                   type | AUB_TRACE_OP_DATA_WRITE);
         dword_out(subtype);
         dword_out(gtt_offset + offset);
         dword_out(align_u32(block_size, 4));
         if (addr_bits > 32)
            dword_out((gtt_offset + offset) >> 32);
      }

      if (virtual)
         data_out(((char *) GET_PTR(virtual)) + offset, block_size);
      else
         data_out(null_block, block_size);

      /* Pad to a multiple of 4 bytes. */
      data_out(null_block, -block_size & 3);
   }
}

static void
write_reloc(void *p, uint64_t v)
{
   if (addr_bits > 32) {
      /* From the Broadwell PRM Vol. 2a,
       * MI_LOAD_REGISTER_MEM::MemoryAddress:
       *
       *   "This field specifies the address of the memory
       *   location where the register value specified in the
       *   DWord above will read from.  The address specifies
       *   the DWord location of the data. Range =
       *   GraphicsVirtualAddress[63:2] for a DWord register
       *   GraphicsAddress [63:48] are ignored by the HW and
       *   assumed to be in correct canonical form [63:48] ==
       *   [47]."
       *
       * In practice, this will always mean the top bits are zero
       * because of the GTT size limitation of the aubdump tool.
       */
      const int shift = 63 - 47;
      *(uint64_t *)p = (((int64_t)v) << shift) >> shift;
   } else {
      *(uint32_t *)p = v;
   }
}

static void
aub_dump_execlist(uint64_t batch_offset, int ring_flag)
{
   uint32_t ring_addr;
   uint64_t descriptor;
   uint32_t elsp_reg;
   uint32_t elsq_reg;
   uint32_t status_reg;
   uint32_t control_reg;

   switch (ring_flag) {
   case I915_EXEC_DEFAULT:
   case I915_EXEC_RENDER:
      ring_addr = RENDER_RING_ADDR;
      descriptor = RENDER_CONTEXT_DESCRIPTOR;
      elsp_reg = EXECLIST_SUBMITPORT_RCSUNIT;
      elsq_reg = EXECLIST_SQ_CONTENTS0_RCSUNIT;
      status_reg = EXECLIST_STATUS_RCSUNIT;
      control_reg = EXECLIST_CONTROL_RCSUNIT;
      break;
   case I915_EXEC_BSD:
      ring_addr = VIDEO_RING_ADDR;
      descriptor = VIDEO_CONTEXT_DESCRIPTOR;
      elsp_reg = EXECLIST_SUBMITPORT_VCSUNIT0;
      elsq_reg = EXECLIST_SQ_CONTENTS0_VCSUNIT0;
      status_reg = EXECLIST_STATUS_VCSUNIT0;
      control_reg = EXECLIST_CONTROL_VCSUNIT0;
      break;
   case I915_EXEC_BLT:
      ring_addr = BLITTER_RING_ADDR;
      descriptor = BLITTER_CONTEXT_DESCRIPTOR;
      elsp_reg = EXECLIST_SUBMITPORT_BCSUNIT;
      elsq_reg = EXECLIST_SQ_CONTENTS0_BCSUNIT;
      status_reg = EXECLIST_STATUS_BCSUNIT;
      control_reg = EXECLIST_CONTROL_BCSUNIT;
      break;
   }

   mem_trace_memory_write_header_out(ring_addr, 16,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   dword_out(AUB_MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965 | (3 - 2));
   dword_out(batch_offset & 0xFFFFFFFF);
   dword_out(batch_offset >> 32);
   dword_out(0 /* MI_NOOP */);

   mem_trace_memory_write_header_out(ring_addr + 8192 + 20, 4,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   dword_out(0); /* RING_BUFFER_HEAD */
   mem_trace_memory_write_header_out(ring_addr + 8192 + 28, 4,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   dword_out(16); /* RING_BUFFER_TAIL */

   if (devinfo.gen >= 11) {
      register_write_out(elsq_reg, descriptor & 0xFFFFFFFF);
      register_write_out(elsq_reg + sizeof(uint32_t), descriptor >> 32);
      register_write_out(control_reg, 1);
   } else {
      register_write_out(elsp_reg, 0);
      register_write_out(elsp_reg, 0);
      register_write_out(elsp_reg, descriptor >> 32);
      register_write_out(elsp_reg, descriptor & 0xFFFFFFFF);
   }

   dword_out(CMD_MEM_TRACE_REGISTER_POLL | (5 + 1 - 1));
   dword_out(status_reg);
   dword_out(AUB_MEM_TRACE_REGISTER_SIZE_DWORD |
             AUB_MEM_TRACE_REGISTER_SPACE_MMIO);
   if (devinfo.gen >= 11) {
      dword_out(0x00000001);   /* mask lo */
      dword_out(0x00000000);   /* mask hi */
      dword_out(0x00000001);
   } else {
      dword_out(0x00000010);   /* mask lo */
      dword_out(0x00000000);   /* mask hi */
      dword_out(0x00000000);
   }
}

static void
aub_dump_ringbuffer(uint64_t batch_offset, uint64_t offset, int ring_flag)
{
   uint32_t ringbuffer[4096];
   unsigned aub_mi_bbs_len;
   int ring = AUB_TRACE_TYPE_RING_PRB0; /* The default ring */
   int ring_count = 0;

   if (ring_flag == I915_EXEC_BSD)
      ring = AUB_TRACE_TYPE_RING_PRB1;
   else if (ring_flag == I915_EXEC_BLT)
      ring = AUB_TRACE_TYPE_RING_PRB2;

   /* Make a ring buffer to execute our batchbuffer. */
   memset(ringbuffer, 0, sizeof(ringbuffer));

   aub_mi_bbs_len = addr_bits > 32 ? 3 : 2;
   ringbuffer[ring_count] = AUB_MI_BATCH_BUFFER_START | (aub_mi_bbs_len - 2);
   write_reloc(&ringbuffer[ring_count + 1], batch_offset);
   ring_count += aub_mi_bbs_len;

   /* Write out the ring.  This appears to trigger execution of
    * the ring in the simulator.
    */
   dword_out(CMD_AUB_TRACE_HEADER_BLOCK |
             ((addr_bits > 32 ? 6 : 5) - 2));
   dword_out(AUB_TRACE_MEMTYPE_GTT | ring | AUB_TRACE_OP_COMMAND_WRITE);
   dword_out(0); /* general/surface subtype */
   dword_out(offset);
   dword_out(ring_count * 4);
   if (addr_bits > 32)
      dword_out(offset >> 32);

   data_out(ringbuffer, ring_count * 4);
}

static void *
relocate_bo(struct bo *bo, const struct drm_i915_gem_execbuffer2 *execbuffer2,
            const struct drm_i915_gem_exec_object2 *obj)
{
   const struct drm_i915_gem_exec_object2 *exec_objects =
      (struct drm_i915_gem_exec_object2 *) (uintptr_t) execbuffer2->buffers_ptr;
   const struct drm_i915_gem_relocation_entry *relocs =
      (const struct drm_i915_gem_relocation_entry *) (uintptr_t) obj->relocs_ptr;
   void *relocated;
   int handle;

   relocated = malloc(bo->size);
   fail_if(relocated == NULL, "intel_aubdump: out of memory\n");
   memcpy(relocated, GET_PTR(bo->map), bo->size);
   for (size_t i = 0; i < obj->relocation_count; i++) {
      fail_if(relocs[i].offset >= bo->size, "intel_aubdump: reloc outside bo\n");

      if (execbuffer2->flags & I915_EXEC_HANDLE_LUT)
         handle = exec_objects[relocs[i].target_handle].handle;
      else
         handle = relocs[i].target_handle;

      write_reloc(((char *)relocated) + relocs[i].offset,
                  get_bo(handle)->offset + relocs[i].delta);
   }

   return relocated;
}

static int
gem_ioctl(int fd, unsigned long request, void *argp)
{
   int ret;

   do {
      ret = libc_ioctl(fd, request, argp);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

static void *
gem_mmap(int fd, uint32_t handle, uint64_t offset, uint64_t size)
{
   struct drm_i915_gem_mmap mmap = {
      .handle = handle,
      .offset = offset,
      .size = size
   };

   if (gem_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP, &mmap) == -1)
      return MAP_FAILED;

   return (void *)(uintptr_t) mmap.addr_ptr;
}

static int
gem_get_param(int fd, uint32_t param)
{
   int value;
   drm_i915_getparam_t gp = {
      .param = param,
      .value = &value
   };

   if (gem_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) == -1)
      return 0;

   return value;
}

static void
dump_execbuffer2(int fd, struct drm_i915_gem_execbuffer2 *execbuffer2)
{
   struct drm_i915_gem_exec_object2 *exec_objects =
      (struct drm_i915_gem_exec_object2 *) (uintptr_t) execbuffer2->buffers_ptr;
   uint32_t ring_flag = execbuffer2->flags & I915_EXEC_RING_MASK;
   uint32_t offset;
   struct drm_i915_gem_exec_object2 *obj;
   struct bo *bo, *batch_bo;
   int batch_index;
   void *data;

   /* We can't do this at open time as we're not yet authenticated. */
   if (device == 0) {
      device = gem_get_param(fd, I915_PARAM_CHIPSET_ID);
      fail_if(device == 0 || devinfo.gen == 0, "failed to identify chipset\n");
   }
   if (devinfo.gen == 0) {
      fail_if(!gen_get_device_info(device, &devinfo),
              "failed to identify chipset=0x%x\n", device);

      addr_bits = devinfo.gen >= 8 ? 48 : 32;

      if (use_execlists())
         write_execlists_header();
      else
         write_legacy_header();

      if (verbose)
         printf("[intel_aubdump: running, "
                "output file %s, chipset id 0x%04x, gen %d]\n",
                filename, device, devinfo.gen);
   }

   if (use_execlists())
      offset = 0x1000;
   else
      offset = gtt_size();

   if (verbose)
      printf("Dumping execbuffer2:\n");

   for (uint32_t i = 0; i < execbuffer2->buffer_count; i++) {
      obj = &exec_objects[i];
      bo = get_bo(obj->handle);

      /* If bo->size == 0, this means they passed us an invalid
       * buffer.  The kernel will reject it and so should we.
       */
      if (bo->size == 0) {
         if (verbose)
            printf("BO #%d is invalid!\n", obj->handle);
         return;
      }

      if (obj->flags & EXEC_OBJECT_PINNED) {
         bo->offset = obj->offset;
         if (verbose)
            printf("BO #%d (%dB) pinned @ 0x%lx\n",
                   obj->handle, bo->size, bo->offset);
      } else {
         if (obj->alignment != 0)
            offset = align_u32(offset, obj->alignment);
         bo->offset = offset;
         if (verbose)
            printf("BO #%d (%dB) @ 0x%lx\n", obj->handle,
                   bo->size, bo->offset);
         offset = align_u32(offset + bo->size + 4095, 4096);
      }

      if (bo->map == NULL && bo->size > 0)
         bo->map = gem_mmap(fd, obj->handle, 0, bo->size);
      fail_if(bo->map == MAP_FAILED, "intel_aubdump: bo mmap failed\n");

      if (use_execlists())
         map_ppgtt(bo->offset, bo->size);
   }

   batch_index = (execbuffer2->flags & I915_EXEC_BATCH_FIRST) ? 0 :
      execbuffer2->buffer_count - 1;
   batch_bo = get_bo(exec_objects[batch_index].handle);
   for (uint32_t i = 0; i < execbuffer2->buffer_count; i++) {
      obj = &exec_objects[i];
      bo = get_bo(obj->handle);

      if (obj->relocation_count > 0)
         data = relocate_bo(bo, execbuffer2, obj);
      else
         data = bo->map;

      if (bo == batch_bo) {
         aub_write_trace_block(AUB_TRACE_TYPE_BATCH,
                               data, bo->size, bo->offset);
      } else {
         aub_write_trace_block(AUB_TRACE_TYPE_NOTYPE,
                               data, bo->size, bo->offset);
      }
      if (data != bo->map)
         free(data);
   }

   if (use_execlists()) {
      aub_dump_execlist(batch_bo->offset +
                        execbuffer2->batch_start_offset, ring_flag);
   } else {
      /* Dump ring buffer */
      aub_dump_ringbuffer(batch_bo->offset +
                          execbuffer2->batch_start_offset, offset,
                          ring_flag);
   }

   for (int i = 0; i < ARRAY_SIZE(files); i++) {
      if (files[i] != NULL)
         fflush(files[i]);
   }

   if (device_override &&
       (execbuffer2->flags & I915_EXEC_FENCE_ARRAY) != 0) {
      struct drm_i915_gem_exec_fence *fences =
         (void*)(uintptr_t)execbuffer2->cliprects_ptr;
      for (uint32_t i = 0; i < execbuffer2->num_cliprects; i++) {
         if ((fences[i].flags & I915_EXEC_FENCE_SIGNAL) != 0) {
            struct drm_syncobj_array arg = {
               .handles = (uintptr_t)&fences[i].handle,
               .count_handles = 1,
               .pad = 0,
            };
            libc_ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &arg);
         }
      }
   }
}

static void
add_new_bo(int handle, uint64_t size, void *map)
{
   struct bo *bo = &bos[handle];

   fail_if(handle >= MAX_BO_COUNT, "intel_aubdump: bo handle out of range\n");
   fail_if(size == 0, "intel_aubdump: bo size is invalid\n");

   bo->size = size;
   bo->map = map;
}

static void
remove_bo(int handle)
{
   struct bo *bo = get_bo(handle);

   if (bo->map && !IS_USERPTR(bo->map))
      munmap(bo->map, bo->size);
   bo->size = 0;
   bo->map = NULL;
}

__attribute__ ((visibility ("default"))) int
close(int fd)
{
   if (fd == drm_fd)
      drm_fd = -1;

   return libc_close(fd);
}

static FILE *
launch_command(char *command)
{
   int i = 0, fds[2];
   char **args = calloc(strlen(command), sizeof(char *));
   char *iter = command;

   args[i++] = iter = command;

   while ((iter = strstr(iter, ",")) != NULL) {
      *iter = '\0';
      iter += 1;
      args[i++] = iter;
   }

   if (pipe(fds) == -1)
      return NULL;

   switch (fork()) {
   case 0:
      dup2(fds[0], 0);
      fail_if(execvp(args[0], args) == -1,
              "intel_aubdump: failed to launch child command\n");
      return NULL;

   default:
      free(args);
      return fdopen(fds[1], "w");

   case -1:
      return NULL;
   }
}

static void
maybe_init(void)
{
   static bool initialized = false;
   FILE *config;
   char *key, *value;

   if (initialized)
      return;

   initialized = true;

   config = fdopen(3, "r");
   while (fscanf(config, "%m[^=]=%m[^\n]\n", &key, &value) != EOF) {
      if (!strcmp(key, "verbose")) {
         verbose = 1;
      } else if (!strcmp(key, "device")) {
         fail_if(sscanf(value, "%i", &device) != 1,
                 "intel_aubdump: failed to parse device id '%s'",
                 value);
         device_override = true;
      } else if (!strcmp(key, "file")) {
         filename = strdup(value);
         files[0] = fopen(filename, "w+");
         fail_if(files[0] == NULL,
                 "intel_aubdump: failed to open file '%s'\n",
                 filename);
      } else if (!strcmp(key,  "command")) {
         files[1] = launch_command(value);
         fail_if(files[1] == NULL,
                 "intel_aubdump: failed to launch command '%s'\n",
                 value);
      } else {
         fprintf(stderr, "intel_aubdump: unknown option '%s'\n", key);
      }

      free(key);
      free(value);
   }
   fclose(config);

   bos = calloc(MAX_BO_COUNT, sizeof(bos[0]));
   fail_if(bos == NULL, "intel_aubdump: out of memory\n");
}

#define LOCAL_IOCTL_I915_GEM_EXECBUFFER2_WR                             \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_EXECBUFFER2, struct drm_i915_gem_execbuffer2)

__attribute__ ((visibility ("default"))) int
ioctl(int fd, unsigned long request, ...)
{
   va_list args;
   void *argp;
   int ret;
   struct stat buf;

   va_start(args, request);
   argp = va_arg(args, void *);
   va_end(args);

   if (_IOC_TYPE(request) == DRM_IOCTL_BASE &&
       drm_fd != fd && fstat(fd, &buf) == 0 &&
       (buf.st_mode & S_IFMT) == S_IFCHR && major(buf.st_rdev) == DRM_MAJOR) {
      drm_fd = fd;
      if (verbose)
         printf("[intel_aubdump: intercept drm ioctl on fd %d]\n", fd);
   }

   if (fd == drm_fd) {
      maybe_init();

      switch (request) {
      case DRM_IOCTL_I915_GETPARAM: {
         struct drm_i915_getparam *getparam = argp;

         if (device_override && getparam->param == I915_PARAM_CHIPSET_ID) {
            *getparam->value = device;
            return 0;
         }

         ret = libc_ioctl(fd, request, argp);

         /* If the application looks up chipset_id
          * (they typically do), we'll piggy-back on
          * their ioctl and store the id for later
          * use. */
         if (getparam->param == I915_PARAM_CHIPSET_ID)
            device = *getparam->value;

         return ret;
      }

      case DRM_IOCTL_I915_GEM_EXECBUFFER: {
         static bool once;
         if (!once) {
            fprintf(stderr, "intel_aubdump: "
                    "application uses DRM_IOCTL_I915_GEM_EXECBUFFER, not handled\n");
            once = true;
         }
         return libc_ioctl(fd, request, argp);
      }

      case DRM_IOCTL_I915_GEM_EXECBUFFER2:
      case LOCAL_IOCTL_I915_GEM_EXECBUFFER2_WR: {
         dump_execbuffer2(fd, argp);
         if (device_override)
            return 0;

         return libc_ioctl(fd, request, argp);
      }

      case DRM_IOCTL_I915_GEM_CREATE: {
         struct drm_i915_gem_create *create = argp;

         ret = libc_ioctl(fd, request, argp);
         if (ret == 0)
            add_new_bo(create->handle, create->size, NULL);

         return ret;
      }

      case DRM_IOCTL_I915_GEM_USERPTR: {
         struct drm_i915_gem_userptr *userptr = argp;

         ret = libc_ioctl(fd, request, argp);
         if (ret == 0)
            add_new_bo(userptr->handle, userptr->user_size,
                       (void *) (uintptr_t) (userptr->user_ptr | USERPTR_FLAG));
         return ret;
      }

      case DRM_IOCTL_GEM_CLOSE: {
         struct drm_gem_close *close = argp;

         remove_bo(close->handle);

         return libc_ioctl(fd, request, argp);
      }

      case DRM_IOCTL_GEM_OPEN: {
         struct drm_gem_open *open = argp;

         ret = libc_ioctl(fd, request, argp);
         if (ret == 0)
            add_new_bo(open->handle, open->size, NULL);

         return ret;
      }

      case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
         struct drm_prime_handle *prime = argp;

         ret = libc_ioctl(fd, request, argp);
         if (ret == 0) {
            off_t size;

            size = lseek(prime->fd, 0, SEEK_END);
            fail_if(size == -1, "intel_aubdump: failed to get prime bo size\n");
            add_new_bo(prime->handle, size, NULL);
         }

         return ret;
      }

      default:
         return libc_ioctl(fd, request, argp);
      }
   } else {
      return libc_ioctl(fd, request, argp);
   }
}

static void
init(void)
{
   libc_close = dlsym(RTLD_NEXT, "close");
   libc_ioctl = dlsym(RTLD_NEXT, "ioctl");
   fail_if(libc_close == NULL || libc_ioctl == NULL,
           "intel_aubdump: failed to get libc ioctl or close\n");
}

static int
close_init_helper(int fd)
{
   init();
   return libc_close(fd);
}

static int
ioctl_init_helper(int fd, unsigned long request, ...)
{
   va_list args;
   void *argp;

   va_start(args, request);
   argp = va_arg(args, void *);
   va_end(args);

   init();
   return libc_ioctl(fd, request, argp);
}

static void __attribute__ ((destructor))
fini(void)
{
   free(filename);
   for (int i = 0; i < ARRAY_SIZE(files); i++) {
      if (files[i] != NULL)
         fclose(files[i]);
   }
   free(bos);
}
