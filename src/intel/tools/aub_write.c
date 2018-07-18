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

#include "aub_write.h"

#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "i915_drm.h"
#include "intel_aub.h"

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

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   return (v + a - 1) & ~(a - 1);
}

static void
aub_ppgtt_table_finish(struct aub_ppgtt_table *table)
{
   for (unsigned i = 0; i < ARRAY_SIZE(table->subtables); i++) {
      aub_ppgtt_table_finish(table->subtables[i]);
      free(table->subtables[i]);
   }
}

void
aub_file_init(struct aub_file *aub, FILE *file, uint16_t pci_id)
{
   memset(aub, 0, sizeof(*aub));

   aub->file = file;
   aub->pci_id = pci_id;
   fail_if(!gen_get_device_info(pci_id, &aub->devinfo),
           "failed to identify chipset=0x%x\n", pci_id);
   aub->addr_bits = aub->devinfo.gen >= 8 ? 48 : 32;

   aub->pml4.phys_addr = PML4_PHYS_ADDR;
}

void
aub_file_finish(struct aub_file *aub)
{
   aub_ppgtt_table_finish(&aub->pml4);
   fclose(aub->file);
}

uint32_t
aub_gtt_size(struct aub_file *aub)
{
   return NUM_PT_ENTRIES * (aub->addr_bits > 32 ? GEN8_PTE_SIZE : PTE_SIZE);
}

static void
data_out(struct aub_file *aub, const void *data, size_t size)
{
   if (size == 0)
      return;

   fail_if(fwrite(data, 1, size, aub->file) == 0,
           "Writing to output failed\n");
}

static void
dword_out(struct aub_file *aub, uint32_t data)
{
   data_out(aub, &data, sizeof(data));
}

static void
mem_trace_memory_write_header_out(struct aub_file *aub, uint64_t addr,
                                  uint32_t len, uint32_t addr_space)
{
   uint32_t dwords = ALIGN(len, sizeof(uint32_t)) / sizeof(uint32_t);

   dword_out(aub, CMD_MEM_TRACE_MEMORY_WRITE | (5 + dwords - 1));
   dword_out(aub, addr & 0xFFFFFFFF);   /* addr lo */
   dword_out(aub, addr >> 32);   /* addr hi */
   dword_out(aub, addr_space);   /* gtt */
   dword_out(aub, len);
}

static void
register_write_out(struct aub_file *aub, uint32_t addr, uint32_t value)
{
   uint32_t dwords = 1;

   dword_out(aub, CMD_MEM_TRACE_REGISTER_WRITE | (5 + dwords - 1));
   dword_out(aub, addr);
   dword_out(aub, AUB_MEM_TRACE_REGISTER_SIZE_DWORD |
                  AUB_MEM_TRACE_REGISTER_SPACE_MMIO);
   dword_out(aub, 0xFFFFFFFF);   /* mask lo */
   dword_out(aub, 0x00000000);   /* mask hi */
   dword_out(aub, value);
}

static void
populate_ppgtt_table(struct aub_file *aub, struct aub_ppgtt_table *table,
                     int start, int end, int level)
{
   static uint64_t phys_addrs_allocator = (PML4_PHYS_ADDR >> 12) + 1;
   uint64_t entries[512] = {0};
   int dirty_start = 512, dirty_end = 0;

   if (aub->verbose_log_file) {
      fprintf(aub->verbose_log_file,
              "  PPGTT (0x%016" PRIx64 "), lvl %d, start: %x, end: %x\n",
              table->phys_addr, level, start, end);
   }

   for (int i = start; i <= end; i++) {
      if (!table->subtables[i]) {
         dirty_start = min(dirty_start, i);
         dirty_end = max(dirty_end, i);
         if (level == 1) {
            table->subtables[i] =
               (void *)(phys_addrs_allocator++ << 12);
            if (aub->verbose_log_file) {
               fprintf(aub->verbose_log_file,
                       "   Adding entry: %x, phys_addr: 0x%016" PRIx64 "\n",
                       i, (uint64_t)table->subtables[i]);
            }
         } else {
            table->subtables[i] =
               calloc(1, sizeof(struct aub_ppgtt_table));
            table->subtables[i]->phys_addr =
               phys_addrs_allocator++ << 12;
            if (aub->verbose_log_file) {
               fprintf(aub->verbose_log_file,
                       "   Adding entry: %x, phys_addr: 0x%016" PRIx64 "\n",
                       i, table->subtables[i]->phys_addr);
            }
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
      mem_trace_memory_write_header_out(aub, write_addr, write_size,
                                        AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_PHYSICAL);
      data_out(aub, entries + dirty_start, write_size);
   }
}

void
aub_map_ppgtt(struct aub_file *aub, uint64_t start, uint64_t size)
{
   uint64_t l4_start = start & 0xff8000000000;
   uint64_t l4_end = ((start + size - 1) | 0x007fffffffff) & 0xffffffffffff;

#define L4_index(addr) (((addr) >> 39) & 0x1ff)
#define L3_index(addr) (((addr) >> 30) & 0x1ff)
#define L2_index(addr) (((addr) >> 21) & 0x1ff)
#define L1_index(addr) (((addr) >> 12) & 0x1ff)

#define L3_table(addr) (aub->pml4.subtables[L4_index(addr)])
#define L2_table(addr) (L3_table(addr)->subtables[L3_index(addr)])
#define L1_table(addr) (L2_table(addr)->subtables[L2_index(addr)])

   if (aub->verbose_log_file) {
      fprintf(aub->verbose_log_file,
              " Mapping PPGTT address: 0x%" PRIx64 ", size: %" PRIu64"\n",
              start, size);
   }

   populate_ppgtt_table(aub, &aub->pml4, L4_index(l4_start), L4_index(l4_end), 4);

   for (uint64_t l4 = l4_start; l4 < l4_end; l4 += (1ULL << 39)) {
      uint64_t l3_start = max(l4, start & 0xffffc0000000);
      uint64_t l3_end = min(l4 + (1ULL << 39) - 1,
                            ((start + size - 1) | 0x00003fffffff) & 0xffffffffffff);
      uint64_t l3_start_idx = L3_index(l3_start);
      uint64_t l3_end_idx = L3_index(l3_end);

      populate_ppgtt_table(aub, L3_table(l4), l3_start_idx, l3_end_idx, 3);

      for (uint64_t l3 = l3_start; l3 < l3_end; l3 += (1ULL << 30)) {
         uint64_t l2_start = max(l3, start & 0xffffffe00000);
         uint64_t l2_end = min(l3 + (1ULL << 30) - 1,
                               ((start + size - 1) | 0x0000001fffff) & 0xffffffffffff);
         uint64_t l2_start_idx = L2_index(l2_start);
         uint64_t l2_end_idx = L2_index(l2_end);

         populate_ppgtt_table(aub, L2_table(l3), l2_start_idx, l2_end_idx, 2);

         for (uint64_t l2 = l2_start; l2 < l2_end; l2 += (1ULL << 21)) {
            uint64_t l1_start = max(l2, start & 0xfffffffff000);
            uint64_t l1_end = min(l2 + (1ULL << 21) - 1,
                                  ((start + size - 1) | 0x000000000fff) & 0xffffffffffff);
            uint64_t l1_start_idx = L1_index(l1_start);
            uint64_t l1_end_idx = L1_index(l1_end);

            populate_ppgtt_table(aub, L1_table(l2), l1_start_idx, l1_end_idx, 1);
         }
      }
   }
}

static uint64_t
ppgtt_lookup(struct aub_file *aub, uint64_t ppgtt_addr)
{
   return (uint64_t)L1_table(ppgtt_addr)->subtables[L1_index(ppgtt_addr)];
}

static void
write_execlists_header(struct aub_file *aub, const char *name)
{
   char app_name[8 * 4];
   int app_name_len, dwords;

   app_name_len =
      snprintf(app_name, sizeof(app_name), "PCI-ID=0x%X %s",
               aub->pci_id, name);
   app_name_len = ALIGN(app_name_len, sizeof(uint32_t));

   dwords = 5 + app_name_len / sizeof(uint32_t);
   dword_out(aub, CMD_MEM_TRACE_VERSION | (dwords - 1));
   dword_out(aub, AUB_MEM_TRACE_VERSION_FILE_VERSION);
   dword_out(aub, aub->devinfo.simulator_id << AUB_MEM_TRACE_VERSION_DEVICE_SHIFT);
   dword_out(aub, 0);      /* version */
   dword_out(aub, 0);      /* version */
   data_out(aub, app_name, app_name_len);

   /* GGTT PT */
   uint32_t ggtt_ptes = STATIC_GGTT_MAP_SIZE >> 12;

   mem_trace_memory_write_header_out(aub, STATIC_GGTT_MAP_START >> 12,
                                     ggtt_ptes * GEN8_PTE_SIZE,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT_ENTRY);
   for (uint32_t i = 0; i < ggtt_ptes; i++) {
      dword_out(aub, 1 + 0x1000 * i + STATIC_GGTT_MAP_START);
      dword_out(aub, 0);
   }

   /* RENDER_RING */
   mem_trace_memory_write_header_out(aub, RENDER_RING_ADDR, RING_SIZE,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < RING_SIZE; i += sizeof(uint32_t))
      dword_out(aub, 0);

   /* RENDER_PPHWSP */
   mem_trace_memory_write_header_out(aub, RENDER_CONTEXT_ADDR,
                                     PPHWSP_SIZE +
                                     sizeof(render_context_init),
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < PPHWSP_SIZE; i += sizeof(uint32_t))
      dword_out(aub, 0);

   /* RENDER_CONTEXT */
   data_out(aub, render_context_init, sizeof(render_context_init));

   /* BLITTER_RING */
   mem_trace_memory_write_header_out(aub, BLITTER_RING_ADDR, RING_SIZE,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < RING_SIZE; i += sizeof(uint32_t))
      dword_out(aub, 0);

   /* BLITTER_PPHWSP */
   mem_trace_memory_write_header_out(aub, BLITTER_CONTEXT_ADDR,
                                     PPHWSP_SIZE +
                                     sizeof(blitter_context_init),
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < PPHWSP_SIZE; i += sizeof(uint32_t))
      dword_out(aub, 0);

   /* BLITTER_CONTEXT */
   data_out(aub, blitter_context_init, sizeof(blitter_context_init));

   /* VIDEO_RING */
   mem_trace_memory_write_header_out(aub, VIDEO_RING_ADDR, RING_SIZE,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < RING_SIZE; i += sizeof(uint32_t))
      dword_out(aub, 0);

   /* VIDEO_PPHWSP */
   mem_trace_memory_write_header_out(aub, VIDEO_CONTEXT_ADDR,
                                     PPHWSP_SIZE +
                                     sizeof(video_context_init),
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   for (uint32_t i = 0; i < PPHWSP_SIZE; i += sizeof(uint32_t))
      dword_out(aub, 0);

   /* VIDEO_CONTEXT */
   data_out(aub, video_context_init, sizeof(video_context_init));

   register_write_out(aub, HWS_PGA_RCSUNIT, RENDER_CONTEXT_ADDR);
   register_write_out(aub, HWS_PGA_VCSUNIT0, VIDEO_CONTEXT_ADDR);
   register_write_out(aub, HWS_PGA_BCSUNIT, BLITTER_CONTEXT_ADDR);

   register_write_out(aub, GFX_MODE_RCSUNIT, 0x80008000 /* execlist enable */);
   register_write_out(aub, GFX_MODE_VCSUNIT0, 0x80008000 /* execlist enable */);
   register_write_out(aub, GFX_MODE_BCSUNIT, 0x80008000 /* execlist enable */);
}

static void write_legacy_header(struct aub_file *aub, const char *name)
{
   char app_name[8 * 4];
   char comment[16];
   int comment_len, comment_dwords, dwords;
   uint32_t entry = 0x200003;

   comment_len = snprintf(comment, sizeof(comment), "PCI-ID=0x%x", aub->pci_id);
   comment_dwords = ((comment_len + 3) / 4);

   /* Start with a (required) version packet. */
   dwords = 13 + comment_dwords;
   dword_out(aub, CMD_AUB_HEADER | (dwords - 2));
   dword_out(aub, (4 << AUB_HEADER_MAJOR_SHIFT) |
                  (0 << AUB_HEADER_MINOR_SHIFT));

   /* Next comes a 32-byte application name. */
   strncpy(app_name, name, sizeof(app_name));
   app_name[sizeof(app_name) - 1] = 0;
   data_out(aub, app_name, sizeof(app_name));

   dword_out(aub, 0); /* timestamp */
   dword_out(aub, 0); /* timestamp */
   dword_out(aub, comment_len);
   data_out(aub, comment, comment_dwords * 4);

   /* Set up the GTT. The max we can handle is 64M */
   dword_out(aub, CMD_AUB_TRACE_HEADER_BLOCK |
                  ((aub->addr_bits > 32 ? 6 : 5) - 2));
   dword_out(aub, AUB_TRACE_MEMTYPE_GTT_ENTRY |
                  AUB_TRACE_TYPE_NOTYPE | AUB_TRACE_OP_DATA_WRITE);
   dword_out(aub, 0); /* subtype */
   dword_out(aub, 0); /* offset */
   dword_out(aub, aub_gtt_size(aub)); /* size */
   if (aub->addr_bits > 32)
      dword_out(aub, 0);
   for (uint32_t i = 0; i < NUM_PT_ENTRIES; i++) {
      dword_out(aub, entry + 0x1000 * i);
      if (aub->addr_bits > 32)
         dword_out(aub, 0);
   }
}

void
aub_write_header(struct aub_file *aub, const char *app_name)
{
   if (aub_use_execlists(aub))
      write_execlists_header(aub, app_name);
   else
      write_legacy_header(aub, app_name);
}

/**
 * Break up large objects into multiple writes.  Otherwise a 128kb VBO
 * would overflow the 16 bits of size field in the packet header and
 * everything goes badly after that.
 */
void
aub_write_trace_block(struct aub_file *aub,
                      uint32_t type, void *virtual,
                      uint32_t size, uint64_t gtt_offset)
{
   uint32_t block_size;
   uint32_t subtype = 0;
   static const char null_block[8 * 4096];

   for (uint32_t offset = 0; offset < size; offset += block_size) {
      block_size = min(8 * 4096, size - offset);

      if (aub_use_execlists(aub)) {
         block_size = min(4096, block_size);
         mem_trace_memory_write_header_out(aub,
                                           ppgtt_lookup(aub, gtt_offset + offset),
                                           block_size,
                                           AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_PHYSICAL);
      } else {
         dword_out(aub, CMD_AUB_TRACE_HEADER_BLOCK |
                        ((aub->addr_bits > 32 ? 6 : 5) - 2));
         dword_out(aub, AUB_TRACE_MEMTYPE_GTT |
                        type | AUB_TRACE_OP_DATA_WRITE);
         dword_out(aub, subtype);
         dword_out(aub, gtt_offset + offset);
         dword_out(aub, align_u32(block_size, 4));
         if (aub->addr_bits > 32)
            dword_out(aub, (gtt_offset + offset) >> 32);
      }

      if (virtual)
         data_out(aub, ((char *) virtual) + offset, block_size);
      else
         data_out(aub, null_block, block_size);

      /* Pad to a multiple of 4 bytes. */
      data_out(aub, null_block, -block_size & 3);
   }
}

static void
aub_dump_execlist(struct aub_file *aub, uint64_t batch_offset, int ring_flag)
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
   default:
      unreachable("unknown ring");
   }

   mem_trace_memory_write_header_out(aub, ring_addr, 16,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   dword_out(aub, AUB_MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965 | (3 - 2));
   dword_out(aub, batch_offset & 0xFFFFFFFF);
   dword_out(aub, batch_offset >> 32);
   dword_out(aub, 0 /* MI_NOOP */);

   mem_trace_memory_write_header_out(aub, ring_addr + 8192 + 20, 4,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   dword_out(aub, 0); /* RING_BUFFER_HEAD */
   mem_trace_memory_write_header_out(aub, ring_addr + 8192 + 28, 4,
                                     AUB_MEM_TRACE_MEMORY_ADDRESS_SPACE_GGTT);
   dword_out(aub, 16); /* RING_BUFFER_TAIL */

   if (aub->devinfo.gen >= 11) {
      register_write_out(aub, elsq_reg, descriptor & 0xFFFFFFFF);
      register_write_out(aub, elsq_reg + sizeof(uint32_t), descriptor >> 32);
      register_write_out(aub, control_reg, 1);
   } else {
      register_write_out(aub, elsp_reg, 0);
      register_write_out(aub, elsp_reg, 0);
      register_write_out(aub, elsp_reg, descriptor >> 32);
      register_write_out(aub, elsp_reg, descriptor & 0xFFFFFFFF);
   }

   dword_out(aub, CMD_MEM_TRACE_REGISTER_POLL | (5 + 1 - 1));
   dword_out(aub, status_reg);
   dword_out(aub, AUB_MEM_TRACE_REGISTER_SIZE_DWORD |
                  AUB_MEM_TRACE_REGISTER_SPACE_MMIO);
   if (aub->devinfo.gen >= 11) {
      dword_out(aub, 0x00000001);   /* mask lo */
      dword_out(aub, 0x00000000);   /* mask hi */
      dword_out(aub, 0x00000001);
   } else {
      dword_out(aub, 0x00000010);   /* mask lo */
      dword_out(aub, 0x00000000);   /* mask hi */
      dword_out(aub, 0x00000000);
   }
}

static void
aub_dump_ringbuffer(struct aub_file *aub, uint64_t batch_offset,
                    uint64_t offset, int ring_flag)
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

   aub_mi_bbs_len = aub->addr_bits > 32 ? 3 : 2;
   ringbuffer[ring_count] = AUB_MI_BATCH_BUFFER_START | (aub_mi_bbs_len - 2);
   aub_write_reloc(&aub->devinfo, &ringbuffer[ring_count + 1], batch_offset);
   ring_count += aub_mi_bbs_len;

   /* Write out the ring.  This appears to trigger execution of
    * the ring in the simulator.
    */
   dword_out(aub, CMD_AUB_TRACE_HEADER_BLOCK |
                  ((aub->addr_bits > 32 ? 6 : 5) - 2));
   dword_out(aub, AUB_TRACE_MEMTYPE_GTT | ring | AUB_TRACE_OP_COMMAND_WRITE);
   dword_out(aub, 0); /* general/surface subtype */
   dword_out(aub, offset);
   dword_out(aub, ring_count * 4);
   if (aub->addr_bits > 32)
      dword_out(aub, offset >> 32);

   data_out(aub, ringbuffer, ring_count * 4);
}

void
aub_write_exec(struct aub_file *aub, uint64_t batch_addr,
               uint64_t offset, int ring_flag)
{
   if (aub_use_execlists(aub)) {
      aub_dump_execlist(aub, batch_addr, ring_flag);
   } else {
      /* Dump ring buffer */
      aub_dump_ringbuffer(aub, batch_addr, offset, ring_flag);
   }
   fflush(aub->file);
}
