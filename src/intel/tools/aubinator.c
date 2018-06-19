/*
 * Copyright © 2016 Intel Corporation
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "util/list.h"
#include "util/macros.h"

#include "common/gen_decoder.h"
#include "common/gen_disasm.h"
#include "common/gen_gem.h"
#include "intel_aub.h"

/* Below is the only command missing from intel_aub.h in libdrm
 * So, reuse intel_aub.h from libdrm and #define the
 * AUB_MI_BATCH_BUFFER_END as below
 */
#define AUB_MI_BATCH_BUFFER_END (0x0500 << 16)

#define CSI "\e["
#define BLUE_HEADER  CSI "0;44m"
#define GREEN_HEADER CSI "1;42m"
#define NORMAL       CSI "0m"

/* options */

static int option_full_decode = true;
static int option_print_offsets = true;
static int max_vbo_lines = -1;
static enum { COLOR_AUTO, COLOR_ALWAYS, COLOR_NEVER } option_color;

/* state */

uint16_t pci_id = 0;
char *input_file = NULL, *xml_path = NULL;
struct gen_device_info devinfo;
struct gen_batch_decode_ctx batch_ctx;

struct bo_map {
   struct list_head link;
   struct gen_batch_decode_bo bo;
};

static struct list_head maps;

FILE *outfile;

struct brw_instruction;

static void
add_gtt_bo_map(struct gen_batch_decode_bo bo)
{
   struct bo_map *m = calloc(1, sizeof(*m));

   m->bo = bo;
   list_add(&m->link, &maps);
}

static void
clear_bo_maps(void)
{
   list_for_each_entry_safe(struct bo_map, i, &maps, link) {
      list_del(&i->link);
      free(i);
   }
}

static struct gen_batch_decode_bo
get_gen_batch_bo(void *user_data, uint64_t address)
{
   list_for_each_entry(struct bo_map, i, &maps, link)
      if (i->bo.addr <= address && i->bo.addr + i->bo.size > address)
         return i->bo;

   return (struct gen_batch_decode_bo) { .map = NULL };
}

#define GEN_ENGINE_RENDER 1
#define GEN_ENGINE_BLITTER 2

static void
handle_trace_block(uint32_t *p)
{
   int operation = p[1] & AUB_TRACE_OPERATION_MASK;
   int type = p[1] & AUB_TRACE_TYPE_MASK;
   int address_space = p[1] & AUB_TRACE_ADDRESS_SPACE_MASK;
   int header_length = p[0] & 0xffff;
   int engine = GEN_ENGINE_RENDER;
   struct gen_batch_decode_bo bo = {
      .map = p + header_length + 2,
      /* Addresses written by aubdump here are in canonical form but the batch
       * decoder always gives us addresses with the top 16bits zeroed, so do
       * the same here.
       */
      .addr = gen_48b_address((devinfo.gen >= 8 ? ((uint64_t) p[5] << 32) : 0) |
                              ((uint64_t) p[3])),
      .size = p[4],
   };

   switch (operation) {
   case AUB_TRACE_OP_DATA_WRITE:
      if (address_space == AUB_TRACE_MEMTYPE_GTT)
         add_gtt_bo_map(bo);
      break;
   case AUB_TRACE_OP_COMMAND_WRITE:
      switch (type) {
      case AUB_TRACE_TYPE_RING_PRB0:
         engine = GEN_ENGINE_RENDER;
         break;
      case AUB_TRACE_TYPE_RING_PRB2:
         engine = GEN_ENGINE_BLITTER;
         break;
      default:
         fprintf(outfile, "command write to unknown ring %d\n", type);
         break;
      }

      (void)engine; /* TODO */
      gen_print_batch(&batch_ctx, bo.map, bo.size, 0);

      clear_bo_maps();
      break;
   }
}

static void
aubinator_init(uint16_t aub_pci_id, const char *app_name)
{
   if (!gen_get_device_info(pci_id, &devinfo)) {
      fprintf(stderr, "can't find device information: pci_id=0x%x\n", pci_id);
      exit(EXIT_FAILURE);
   }

   enum gen_batch_decode_flags batch_flags = 0;
   if (option_color == COLOR_ALWAYS)
      batch_flags |= GEN_BATCH_DECODE_IN_COLOR;
   if (option_full_decode)
      batch_flags |= GEN_BATCH_DECODE_FULL;
   if (option_print_offsets)
      batch_flags |= GEN_BATCH_DECODE_OFFSETS;
   batch_flags |= GEN_BATCH_DECODE_FLOATS;

   gen_batch_decode_ctx_init(&batch_ctx, &devinfo, outfile, batch_flags,
                             xml_path, get_gen_batch_bo, NULL, NULL);
   batch_ctx.max_vbo_decoded_lines = max_vbo_lines;

   char *color = GREEN_HEADER, *reset_color = NORMAL;
   if (option_color == COLOR_NEVER)
      color = reset_color = "";

   fprintf(outfile, "%sAubinator: Intel AUB file decoder.%-80s%s\n",
           color, "", reset_color);

   if (input_file)
      fprintf(outfile, "File name:        %s\n", input_file);

   if (aub_pci_id)
      fprintf(outfile, "PCI ID:           0x%x\n", aub_pci_id);

   fprintf(outfile, "Application name: %s\n", app_name);

   fprintf(outfile, "Decoding as:      %s\n", gen_get_device_name(pci_id));

   /* Throw in a new line before the first batch */
   fprintf(outfile, "\n");
}

static void
handle_trace_header(uint32_t *p)
{
   /* The intel_aubdump tool from IGT is kind enough to put a PCI-ID= tag in
    * the AUB header comment.  If the user hasn't specified a hardware
    * generation, try to use the one from the AUB file.
    */
   uint32_t *end = p + (p[0] & 0xffff) + 2;
   int aub_pci_id = 0;
   if (end > &p[12] && p[12] > 0)
      sscanf((char *)&p[13], "PCI-ID=%i", &aub_pci_id);

   if (pci_id == 0)
      pci_id = aub_pci_id;

   char app_name[33];
   strncpy(app_name, (char *)&p[2], 32);
   app_name[32] = 0;

   aubinator_init(aub_pci_id, app_name);
}

static void
handle_memtrace_version(uint32_t *p)
{
   int header_length = p[0] & 0xffff;
   char app_name[64];
   int app_name_len = MIN2(4 * (header_length + 1 - 5), ARRAY_SIZE(app_name) - 1);
   int pci_id_len = 0;
   int aub_pci_id = 0;

   strncpy(app_name, (char *)&p[5], app_name_len);
   app_name[app_name_len] = 0;
   sscanf(app_name, "PCI-ID=%i %n", &aub_pci_id, &pci_id_len);
   if (pci_id == 0)
      pci_id = aub_pci_id;
   aubinator_init(aub_pci_id, app_name + pci_id_len);
}

static void
handle_memtrace_reg_write(uint32_t *p)
{
   static struct execlist_regs {
      uint32_t render_elsp[4];
      int render_elsp_index;
      uint32_t blitter_elsp[4];
      int blitter_elsp_index;
   } state = {};

   uint32_t offset = p[1];
   uint32_t value = p[5];

   int engine;
   uint64_t context_descriptor;

   switch (offset) {
   case 0x2230: /* render elsp */
      state.render_elsp[state.render_elsp_index++] = value;
      if (state.render_elsp_index < 4)
         return;

      state.render_elsp_index = 0;
      engine = GEN_ENGINE_RENDER;
      context_descriptor = (uint64_t)state.render_elsp[2] << 32 |
         state.render_elsp[3];
      break;
   case 0x22230: /* blitter elsp */
      state.blitter_elsp[state.blitter_elsp_index++] = value;
      if (state.blitter_elsp_index < 4)
         return;

      state.blitter_elsp_index = 0;
      engine = GEN_ENGINE_BLITTER;
      context_descriptor = (uint64_t)state.blitter_elsp[2] << 32 |
         state.blitter_elsp[3];
      break;
   case 0x2510: /* render elsq0 lo */
      state.render_elsp[3] = value;
      return;
      break;
   case 0x2514: /* render elsq0 hi */
      state.render_elsp[2] = value;
      return;
      break;
   case 0x22510: /* blitter elsq0 lo */
      state.blitter_elsp[3] = value;
      return;
      break;
   case 0x22514: /* blitter elsq0 hi */
      state.blitter_elsp[2] = value;
      return;
      break;
   case 0x2550: /* render elsc */
      engine = GEN_ENGINE_RENDER;
      context_descriptor = (uint64_t)state.render_elsp[2] << 32 |
         state.render_elsp[3];
      break;
   case 0x22550: /* blitter elsc */
      engine = GEN_ENGINE_BLITTER;
      context_descriptor = (uint64_t)state.blitter_elsp[2] << 32 |
         state.blitter_elsp[3];
      break;
   default:
      return;
   }

   const uint32_t pphwsp_size = 4096;
   uint32_t pphwsp_addr = context_descriptor & 0xfffff000;
   struct gen_batch_decode_bo pphwsp_bo = get_gen_batch_bo(NULL, pphwsp_addr);
   uint32_t *context = (uint32_t *)((uint8_t *)pphwsp_bo.map +
                                    (pphwsp_bo.addr - pphwsp_addr) +
                                    pphwsp_size);

   uint32_t ring_buffer_head = context[5];
   uint32_t ring_buffer_tail = context[7];
   uint32_t ring_buffer_start = context[9];

   struct gen_batch_decode_bo ring_bo = get_gen_batch_bo(NULL,
                                                         ring_buffer_start);
   assert(ring_bo.size > 0);
   void *commands = (uint8_t *)ring_bo.map + (ring_bo.addr - ring_buffer_start);
   (void)engine; /* TODO */
   gen_print_batch(&batch_ctx, commands, ring_buffer_tail - ring_buffer_head,
                   0);
   clear_bo_maps();
}

static void
handle_memtrace_mem_write(uint32_t *p)
{
   struct gen_batch_decode_bo bo = {
      .map = p + 5,
      /* Addresses written by aubdump here are in canonical form but the batch
       * decoder always gives us addresses with the top 16bits zeroed, so do
       * the same here.
       */
      .addr = gen_48b_address(*(uint64_t*)&p[1]),
      .size = p[4],
   };
   uint32_t address_space = p[3] >> 28;

   if (address_space != 1)
      return;

   add_gtt_bo_map(bo);
}

struct aub_file {
   FILE *stream;

   uint32_t *map, *end, *cursor;
   uint32_t *mem_end;
};

static struct aub_file *
aub_file_open(const char *filename)
{
   struct aub_file *file;
   struct stat sb;
   int fd;

   file = calloc(1, sizeof *file);
   fd = open(filename, O_RDONLY);
   if (fd == -1) {
      fprintf(stderr, "open %s failed: %s\n", filename, strerror(errno));
      exit(EXIT_FAILURE);
   }

   if (fstat(fd, &sb) == -1) {
      fprintf(stderr, "stat failed: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
   }

   file->map = mmap(NULL, sb.st_size,
                    PROT_READ, MAP_SHARED, fd, 0);
   if (file->map == MAP_FAILED) {
      fprintf(stderr, "mmap failed: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
   }

   close(fd);

   file->cursor = file->map;
   file->end = file->map + sb.st_size / 4;

   return file;
}

#define TYPE(dw)       (((dw) >> 29) & 7)
#define OPCODE(dw)     (((dw) >> 23) & 0x3f)
#define SUBOPCODE(dw)  (((dw) >> 16) & 0x7f)

#define MAKE_HEADER(type, opcode, subopcode) \
                   (((type) << 29) | ((opcode) << 23) | ((subopcode) << 16))

#define TYPE_AUB            0x7

/* Classic AUB opcodes */
#define OPCODE_AUB          0x01
#define SUBOPCODE_HEADER    0x05
#define SUBOPCODE_BLOCK     0x41
#define SUBOPCODE_BMP       0x1e

/* Newer version AUB opcode */
#define OPCODE_NEW_AUB      0x2e
#define SUBOPCODE_REG_POLL  0x02
#define SUBOPCODE_REG_WRITE 0x03
#define SUBOPCODE_MEM_POLL  0x05
#define SUBOPCODE_MEM_WRITE 0x06
#define SUBOPCODE_VERSION   0x0e

#define MAKE_GEN(major, minor) ( ((major) << 8) | (minor) )

enum {
   AUB_ITEM_DECODE_OK,
   AUB_ITEM_DECODE_FAILED,
   AUB_ITEM_DECODE_NEED_MORE_DATA,
};

static int
aub_file_decode_batch(struct aub_file *file)
{
   uint32_t *p, h, *new_cursor;
   int header_length, bias;

   assert(file->cursor < file->end);

   p = file->cursor;
   h = *p;
   header_length = h & 0xffff;

   switch (OPCODE(h)) {
   case OPCODE_AUB:
      bias = 2;
      break;
   case OPCODE_NEW_AUB:
      bias = 1;
      break;
   default:
      fprintf(outfile, "unknown opcode %d at %td/%td\n",
              OPCODE(h), file->cursor - file->map,
              file->end - file->map);
      return AUB_ITEM_DECODE_FAILED;
   }

   new_cursor = p + header_length + bias;
   if ((h & 0xffff0000) == MAKE_HEADER(TYPE_AUB, OPCODE_AUB, SUBOPCODE_BLOCK)) {
      assert(file->end - file->cursor >= 4);
      new_cursor += p[4] / 4;
   }

   assert(new_cursor <= file->end);

   switch (h & 0xffff0000) {
   case MAKE_HEADER(TYPE_AUB, OPCODE_AUB, SUBOPCODE_HEADER):
      handle_trace_header(p);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_AUB, SUBOPCODE_BLOCK):
      handle_trace_block(p);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_AUB, SUBOPCODE_BMP):
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_VERSION):
      handle_memtrace_version(p);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_REG_WRITE):
      handle_memtrace_reg_write(p);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_MEM_WRITE):
      handle_memtrace_mem_write(p);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_MEM_POLL):
      fprintf(outfile, "memory poll block (dwords %d):\n", h & 0xffff);
      break;
   case MAKE_HEADER(TYPE_AUB, OPCODE_NEW_AUB, SUBOPCODE_REG_POLL):
      break;
   default:
      fprintf(outfile, "unknown block type=0x%x, opcode=0x%x, "
             "subopcode=0x%x (%08x)\n", TYPE(h), OPCODE(h), SUBOPCODE(h), h);
      break;
   }
   file->cursor = new_cursor;

   return AUB_ITEM_DECODE_OK;
}

static int
aub_file_more_stuff(struct aub_file *file)
{
   return file->cursor < file->end || (file->stream && !feof(file->stream));
}

static void
setup_pager(void)
{
   int fds[2];
   pid_t pid;

   if (!isatty(1))
      return;

   if (pipe(fds) == -1)
      return;

   pid = fork();
   if (pid == -1)
      return;

   if (pid == 0) {
      close(fds[1]);
      dup2(fds[0], 0);
      execlp("less", "less", "-FRSi", NULL);
   }

   close(fds[0]);
   dup2(fds[1], 1);
   close(fds[1]);
}

static void
print_help(const char *progname, FILE *file)
{
   fprintf(file,
           "Usage: %s [OPTION]... FILE\n"
           "Decode aub file contents from FILE.\n\n"
           "      --help             display this help and exit\n"
           "      --gen=platform     decode for given platform (3 letter platform name)\n"
           "      --headers          decode only command headers\n"
           "      --color[=WHEN]     colorize the output; WHEN can be 'auto' (default\n"
           "                         if omitted), 'always', or 'never'\n"
           "      --max-vbo-lines=N  limit the number of decoded VBO lines\n"
           "      --no-pager         don't launch pager\n"
           "      --no-offsets       don't print instruction offsets\n"
           "      --xml=DIR          load hardware xml description from directory DIR\n",
           progname);
}

int main(int argc, char *argv[])
{
   struct aub_file *file;
   int c, i;
   bool help = false, pager = true;
   const struct option aubinator_opts[] = {
      { "help",          no_argument,       (int *) &help,                 true },
      { "no-pager",      no_argument,       (int *) &pager,                false },
      { "no-offsets",    no_argument,       (int *) &option_print_offsets, false },
      { "gen",           required_argument, NULL,                          'g' },
      { "headers",       no_argument,       (int *) &option_full_decode,   false },
      { "color",         required_argument, NULL,                          'c' },
      { "xml",           required_argument, NULL,                          'x' },
      { "max-vbo-lines", required_argument, NULL,                          'v' },
      { NULL,            0,                 NULL,                          0 }
   };

   outfile = stdout;

   i = 0;
   while ((c = getopt_long(argc, argv, "", aubinator_opts, &i)) != -1) {
      switch (c) {
      case 'g': {
         const int id = gen_device_name_to_pci_device_id(optarg);
         if (id < 0) {
            fprintf(stderr, "can't parse gen: '%s', expected ivb, byt, hsw, "
                                   "bdw, chv, skl, kbl or bxt\n", optarg);
            exit(EXIT_FAILURE);
         } else {
            pci_id = id;
         }
         break;
      }
      case 'c':
         if (optarg == NULL || strcmp(optarg, "always") == 0)
            option_color = COLOR_ALWAYS;
         else if (strcmp(optarg, "never") == 0)
            option_color = COLOR_NEVER;
         else if (strcmp(optarg, "auto") == 0)
            option_color = COLOR_AUTO;
         else {
            fprintf(stderr, "invalid value for --color: %s", optarg);
            exit(EXIT_FAILURE);
         }
         break;
      case 'x':
         xml_path = strdup(optarg);
         break;
      case 'v':
         max_vbo_lines = atoi(optarg);
         break;
      default:
         break;
      }
   }

   if (optind < argc)
      input_file = argv[optind];

   if (help || !input_file) {
      print_help(argv[0], stderr);
      exit(0);
   }

   /* Do this before we redirect stdout to pager. */
   if (option_color == COLOR_AUTO)
      option_color = isatty(1) ? COLOR_ALWAYS : COLOR_NEVER;

   if (isatty(1) && pager)
      setup_pager();

   list_inithead(&maps);

   file = aub_file_open(input_file);

   while (aub_file_more_stuff(file) &&
          aub_file_decode_batch(file) == AUB_ITEM_DECODE_OK);

   fflush(stdout);
   /* close the stdout which is opened to write the output */
   close(1);
   free(xml_path);

   wait(NULL);

   return EXIT_SUCCESS;
}
