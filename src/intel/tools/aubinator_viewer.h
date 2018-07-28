#ifndef AUBINATOR_VIEWER_H
#define AUBINATOR_VIEWER_H

#include "imgui.h"

#include "common/gen_decoder.h"
#include "common/gen_disasm.h"

struct aub_viewer_cfg {
   ImColor clear_color;
   ImColor dwords_color;
   ImColor highlight_color;
   ImColor error_color;
   ImColor missing_color;

  aub_viewer_cfg() :
    clear_color(114, 144, 154),
    dwords_color(29, 177, 194, 255),
    highlight_color(0, 230, 0, 255),
    error_color(236, 255, 0, 255),
    missing_color(230, 0, 230, 255) {}
};

struct aub_viewer_decode_cfg {
   struct ImGuiTextFilter command_filter;
   struct ImGuiTextFilter field_filter;

   bool drop_filtered;
   bool show_dwords;

  aub_viewer_decode_cfg() :
    drop_filtered(false),
    show_dwords(true) {}
};

struct aub_viewer_decode_ctx {
   struct gen_batch_decode_bo (*get_bo)(void *user_data, uint64_t address);
   unsigned (*get_state_size)(void *user_data,
                              uint32_t offset_from_dynamic_state_base_addr);

   void (*display_shader)(void *user_data, const char *shader_desc, uint64_t address);
   void (*edit_address)(void *user_data, uint64_t address, uint32_t length);

   void *user_data;

   struct gen_spec *spec;
   struct gen_disasm *disasm;

   struct aub_viewer_cfg *cfg;
   struct aub_viewer_decode_cfg *decode_cfg;

   uint64_t surface_base;
   uint64_t dynamic_base;
   uint64_t instruction_base;

};

void aub_viewer_decode_ctx_init(struct aub_viewer_decode_ctx *ctx,
                                struct aub_viewer_cfg *cfg,
                                struct aub_viewer_decode_cfg *decode_cfg,
                                struct gen_spec *spec,
                                struct gen_disasm *disasm,
                                struct gen_batch_decode_bo (*get_bo)(void *, uint64_t),
                                unsigned (*get_state_size)(void *, uint32_t),
                                void *user_data);

void aub_viewer_render_batch(struct aub_viewer_decode_ctx *ctx,
                             const void *batch, uint32_t batch_size,
                             uint64_t batch_addr);

#endif /* AUBINATOR_VIEWER_H */
