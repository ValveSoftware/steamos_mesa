/*
 * Copyright (C) 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2018 Google, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_prim.h"

#include "freedreno_state.h"
#include "freedreno_resource.h"

#include "fd6_draw.h"
#include "fd6_context.h"
#include "fd6_emit.h"
#include "fd6_program.h"
#include "fd6_format.h"
#include "fd6_zsa.h"


static void
draw_impl(struct fd_context *ctx, struct fd_ringbuffer *ring,
		struct fd6_emit *emit, unsigned index_offset)
{
	const struct pipe_draw_info *info = emit->info;
	enum pc_di_primtype primtype = ctx->primtypes[info->mode];

	fd6_emit_state(ctx, ring, emit);

	if (emit->dirty & (FD_DIRTY_VTXBUF | FD_DIRTY_VTXSTATE))
		fd6_emit_vertex_bufs(ring, emit);

	OUT_PKT4(ring, REG_A6XX_VFD_INDEX_OFFSET, 2);
	OUT_RING(ring, info->index_size ? info->index_bias : info->start); /* VFD_INDEX_OFFSET */
	OUT_RING(ring, info->start_instance);   /* VFD_INSTANCE_START_OFFSET */

	OUT_PKT4(ring, REG_A6XX_PC_RESTART_INDEX, 1);
	OUT_RING(ring, info->primitive_restart ? /* PC_RESTART_INDEX */
			info->restart_index : 0xffffffff);

	fd6_emit_render_cntl(ctx, false, emit->key.binning_pass);
	fd6_draw_emit(ctx->batch, ring, primtype,
			emit->key.binning_pass ? IGNORE_VISIBILITY : USE_VISIBILITY,
			info, index_offset);
}

/* fixup dirty shader state in case some "unrelated" (from the state-
 * tracker's perspective) state change causes us to switch to a
 * different variant.
 */
static void
fixup_shader_state(struct fd_context *ctx, struct ir3_shader_key *key)
{
	struct fd6_context *fd6_ctx = fd6_context(ctx);
	struct ir3_shader_key *last_key = &fd6_ctx->last_key;

	if (!ir3_shader_key_equal(last_key, key)) {
		if (ir3_shader_key_changes_fs(last_key, key)) {
			ctx->dirty_shader[PIPE_SHADER_FRAGMENT] |= FD_DIRTY_SHADER_PROG;
			ctx->dirty |= FD_DIRTY_PROG;
		}

		if (ir3_shader_key_changes_vs(last_key, key)) {
			ctx->dirty_shader[PIPE_SHADER_VERTEX] |= FD_DIRTY_SHADER_PROG;
			ctx->dirty |= FD_DIRTY_PROG;
		}

		fd6_ctx->last_key = *key;
	}
}

static bool
fd6_draw_vbo(struct fd_context *ctx, const struct pipe_draw_info *info,
             unsigned index_offset)
{
	struct fd6_context *fd6_ctx = fd6_context(ctx);
	struct fd6_emit emit = {
		.debug = &ctx->debug,
		.vtx  = &ctx->vtx,
		.prog = &ctx->prog,
		.info = info,
		.key = {
			.color_two_side = ctx->rasterizer->light_twoside,
			.vclamp_color = ctx->rasterizer->clamp_vertex_color,
			.fclamp_color = ctx->rasterizer->clamp_fragment_color,
			.rasterflat = ctx->rasterizer->flatshade,
			.half_precision = ctx->in_blit &&
					fd_half_precision(&ctx->batch->framebuffer),
			.ucp_enables = ctx->rasterizer->clip_plane_enable,
			.has_per_samp = (fd6_ctx->fsaturate || fd6_ctx->vsaturate ||
					fd6_ctx->fastc_srgb || fd6_ctx->vastc_srgb),
			.vsaturate_s = fd6_ctx->vsaturate_s,
			.vsaturate_t = fd6_ctx->vsaturate_t,
			.vsaturate_r = fd6_ctx->vsaturate_r,
			.fsaturate_s = fd6_ctx->fsaturate_s,
			.fsaturate_t = fd6_ctx->fsaturate_t,
			.fsaturate_r = fd6_ctx->fsaturate_r,
			.vastc_srgb = fd6_ctx->vastc_srgb,
			.fastc_srgb = fd6_ctx->fastc_srgb,
			.vsamples = ctx->tex[PIPE_SHADER_VERTEX].samples,
			.fsamples = ctx->tex[PIPE_SHADER_FRAGMENT].samples,
		},
		.rasterflat = ctx->rasterizer->flatshade,
		.sprite_coord_enable = ctx->rasterizer->sprite_coord_enable,
		.sprite_coord_mode = ctx->rasterizer->sprite_coord_mode,
	};

	fixup_shader_state(ctx, &emit.key);

	unsigned dirty = ctx->dirty;
	const struct ir3_shader_variant *vp = fd6_emit_get_vp(&emit);
	const struct ir3_shader_variant *fp = fd6_emit_get_fp(&emit);

	/* do regular pass first, since that is more likely to fail compiling: */

	if (!vp || !fp)
		return false;

	ctx->stats.vs_regs += ir3_shader_halfregs(vp);
	ctx->stats.fs_regs += ir3_shader_halfregs(fp);

	/* figure out whether we need to disable LRZ write for binning
	 * pass using draw pass's fp:
	 */
	emit.no_lrz_write = fp->writes_pos || fp->has_kill;

	emit.key.binning_pass = false;
	emit.dirty = dirty;

	draw_impl(ctx, ctx->batch->draw, &emit, index_offset);

	/* and now binning pass: */
	emit.key.binning_pass = true;
	emit.dirty = dirty & ~(FD_DIRTY_BLEND);
	emit.vp = NULL;   /* we changed key so need to refetch vp */
	emit.fp = NULL;
	draw_impl(ctx, ctx->batch->binning, &emit, index_offset);

	if (emit.streamout_mask) {
		struct fd_ringbuffer *ring = ctx->batch->draw;

		for (unsigned i = 0; i < PIPE_MAX_SO_BUFFERS; i++) {
			if (emit.streamout_mask & (1 << i)) {
				OUT_PKT7(ring, CP_EVENT_WRITE, 1);
				OUT_RING(ring, FLUSH_SO_0 + i);
			}
		}
	}

	fd_context_all_clean(ctx);

	return true;
}

static bool is_z32(enum pipe_format format)
{
	switch (format) {
	case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
	case PIPE_FORMAT_Z32_UNORM:
	case PIPE_FORMAT_Z32_FLOAT:
		return true;
	default:
		return false;
	}
}

#if 0
static void
fd6_clear_lrz(struct fd_batch *batch, struct fd_resource *zsbuf, double depth)
{
	struct fd_ringbuffer *ring;
	uint32_t clear = util_pack_z(PIPE_FORMAT_Z16_UNORM, depth);

	// TODO mid-frame clears (ie. app doing crazy stuff)??  Maybe worth
	// splitting both clear and lrz clear out into their own rb's.  And
	// just throw away any draws prior to clear.  (Anything not fullscreen
	// clear, just fallback to generic path that treats it as a normal
	// draw

	if (!batch->lrz_clear) {
		batch->lrz_clear = fd_ringbuffer_new(batch->ctx->pipe, 0x1000);
		fd_ringbuffer_set_parent(batch->lrz_clear, batch->gmem);
	}

	ring = batch->lrz_clear;

	OUT_WFI5(ring);

	OUT_PKT4(ring, REG_A6XX_RB_CCU_CNTL, 1);
	OUT_RING(ring, 0x10000000);

	OUT_PKT4(ring, REG_A6XX_HLSQ_UPDATE_CNTL, 1);
	OUT_RING(ring, 0x20fffff);

	OUT_PKT4(ring, REG_A6XX_GRAS_SU_CNTL, 1);
	OUT_RING(ring, A6XX_GRAS_SU_CNTL_LINEHALFWIDTH(0.0));

	OUT_PKT4(ring, REG_A6XX_GRAS_CNTL, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A6XX_GRAS_CL_CNTL, 1);
	OUT_RING(ring, 0x00000181);

	OUT_PKT4(ring, REG_A6XX_GRAS_LRZ_CNTL, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A6XX_RB_MRT_BUF_INFO(0), 5);
	OUT_RING(ring, A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT(RB5_R16_UNORM) |
			A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(TILE6_LINEAR) |
			A6XX_RB_MRT_BUF_INFO_COLOR_SWAP(WZYX));
	OUT_RING(ring, A6XX_RB_MRT_PITCH(zsbuf->lrz_pitch * 2));
	OUT_RING(ring, A6XX_RB_MRT_ARRAY_PITCH(fd_bo_size(zsbuf->lrz)));
	OUT_RELOCW(ring, zsbuf->lrz, 0x1000, 0, 0);

	OUT_PKT4(ring, REG_A6XX_RB_RENDER_CNTL, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A6XX_RB_DEST_MSAA_CNTL, 1);
	OUT_RING(ring, A6XX_RB_DEST_MSAA_CNTL_SAMPLES(MSAA_ONE));

	OUT_PKT4(ring, REG_A6XX_RB_BLIT_CNTL, 1);
	OUT_RING(ring, A6XX_RB_BLIT_CNTL_BUF(BLIT_MRT0));

	OUT_PKT4(ring, REG_A6XX_RB_CLEAR_CNTL, 1);
	OUT_RING(ring, A6XX_RB_CLEAR_CNTL_FAST_CLEAR |
			A6XX_RB_CLEAR_CNTL_MASK(0xf));

	OUT_PKT4(ring, REG_A6XX_RB_CLEAR_COLOR_DW0, 1);
	OUT_RING(ring, clear);  /* RB_CLEAR_COLOR_DW0 */

	OUT_PKT4(ring, REG_A6XX_VSC_RESOLVE_CNTL, 2);
	OUT_RING(ring, A6XX_VSC_RESOLVE_CNTL_X(zsbuf->lrz_width) |
			 A6XX_VSC_RESOLVE_CNTL_Y(zsbuf->lrz_height));
	OUT_RING(ring, 0x00000000);   // XXX UNKNOWN_0CDE

	OUT_PKT4(ring, REG_A6XX_RB_CNTL, 1);
	OUT_RING(ring, A6XX_RB_CNTL_BYPASS);

	OUT_PKT4(ring, REG_A6XX_RB_RESOLVE_CNTL_1, 2);
	OUT_RING(ring, A6XX_RB_RESOLVE_CNTL_1_X(0) |
			A6XX_RB_RESOLVE_CNTL_1_Y(0));
	OUT_RING(ring, A6XX_RB_RESOLVE_CNTL_2_X(zsbuf->lrz_width - 1) |
			A6XX_RB_RESOLVE_CNTL_2_Y(zsbuf->lrz_height - 1));

	fd6_emit_blit(batch->ctx, ring);
}
#endif

#if 0
clear_with_cp_blit()
{
	/* Clear with CP_BLIT */
	WRITE(REG_A6XX_GRAS_2D_BLIT_CNTL, 0x10f43180);

	OUT_PKT4(ring, REG_A6XX_SP_PS_2D_SRC_INFO, 7);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);

	WRITE(0xacc0, 0xf181);
	WRITE(0xacc0, 0xf181);

	WRITE(REG_A6XX_GRAS_2D_BLIT_CNTL, 0x10f43180);
	WRITE(REG_A6XX_RB_2D_BLIT_CNTL, 0x10f43180);

	OUT_PKT4(ring, REG_A6XX_RB_2D_SRC_SOLID_C0, 4);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0xff);
	OUT_RING(ring, 0);

	DBG("%x %x %x %x\n", color->ui[0], color->ui[1], color->ui[2], color->ui[3]);

	struct pipe_surface *psurf = pfb->cbufs[0];
	struct fd_resource *rsc = fd_resource(psurf->texture);
	struct fd_resource_slice *slice = fd_resource_slice(rsc, psurf->u.tex.level);

	uint32_t offset = fd_resource_offset(rsc, psurf->u.tex.level,
										 psurf->u.tex.first_layer);
	uint32_t stride = slice->pitch * rsc->cpp;

	enum a6xx_color_fmt format = fd6_pipe2color(pfmt);
	OUT_PKT4(ring, REG_A6XX_RB_2D_DST_INFO, 9);
	OUT_RING(ring,
			 A6XX_RB_2D_DST_INFO_COLOR_FORMAT(format) |
			 A6XX_RB_2D_DST_INFO_TILE_MODE(TILE6_LINEAR) |
			 A6XX_RB_2D_DST_INFO_COLOR_SWAP(WXYZ));
	OUT_RELOCW(ring, rsc->bo, offset, 0, 0);  /* RB_2D_DST_LO/HI */
	OUT_RING(ring, A6XX_RB_2D_DST_SIZE_PITCH(stride));
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);

	OUT_PKT4(ring, REG_A6XX_GRAS_2D_SRC_TL_X, 4);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);

	OUT_PKT4(ring, REG_A6XX_GRAS_2D_DST_TL, 2);
	OUT_RING(ring,
			 A6XX_GRAS_2D_DST_TL_X(ctx->batch->max_scissor.minx) |
			 A6XX_GRAS_2D_DST_TL_Y(ctx->batch->max_scissor.miny));
	OUT_RING(ring,
			 A6XX_GRAS_2D_DST_BR_X(ctx->batch->max_scissor.maxx) |
			 A6XX_GRAS_2D_DST_BR_Y(ctx->batch->max_scissor.maxy));

	OUT_PKT7(ring, CP_BLIT, 1);
	OUT_RING(ring, CP_BLIT_0_OP(BLIT_OP_SCALE));
}
#endif

static bool
fd6_clear(struct fd_context *ctx, unsigned buffers,
		const union pipe_color_union *color, double depth, unsigned stencil)
{
	struct pipe_framebuffer_state *pfb = &ctx->batch->framebuffer;
	struct pipe_scissor_state *scissor = fd_context_get_scissor(ctx);
	struct fd_ringbuffer *ring = ctx->batch->draw;

	if ((buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) &&
			is_z32(pfb->zsbuf->format))
		return false;

	fd6_emit_render_cntl(ctx, true, false);

	OUT_PKT4(ring, REG_A6XX_RB_BLIT_SCISSOR_TL, 2);
	OUT_RING(ring, A6XX_RB_BLIT_SCISSOR_TL_X(scissor->minx) |
			 A6XX_RB_BLIT_SCISSOR_TL_Y(scissor->miny));
	OUT_RING(ring, A6XX_RB_BLIT_SCISSOR_BR_X(scissor->maxx - 1) |
			 A6XX_RB_BLIT_SCISSOR_BR_Y(scissor->maxy - 1));

	if (buffers & PIPE_CLEAR_COLOR) {
		for (int i = 0; i < pfb->nr_cbufs; i++) {
			union util_color uc = {0};

			if (!pfb->cbufs[i])
				continue;

			if (!(buffers & (PIPE_CLEAR_COLOR0 << i)))
				continue;

			enum pipe_format pfmt = pfb->cbufs[i]->format;

			// XXX I think RB_CLEAR_COLOR_DWn wants to take into account SWAP??
			union pipe_color_union swapped;
			switch (fd6_pipe2swap(pfmt)) {
			case WZYX:
				swapped.ui[0] = color->ui[0];
				swapped.ui[1] = color->ui[1];
				swapped.ui[2] = color->ui[2];
				swapped.ui[3] = color->ui[3];
				break;
			case WXYZ:
				swapped.ui[2] = color->ui[0];
				swapped.ui[1] = color->ui[1];
				swapped.ui[0] = color->ui[2];
				swapped.ui[3] = color->ui[3];
				break;
			case ZYXW:
				swapped.ui[3] = color->ui[0];
				swapped.ui[0] = color->ui[1];
				swapped.ui[1] = color->ui[2];
				swapped.ui[2] = color->ui[3];
				break;
			case XYZW:
				swapped.ui[3] = color->ui[0];
				swapped.ui[2] = color->ui[1];
				swapped.ui[1] = color->ui[2];
				swapped.ui[0] = color->ui[3];
				break;
			}

			if (util_format_is_pure_uint(pfmt)) {
				util_format_write_4ui(pfmt, swapped.ui, 0, &uc, 0, 0, 0, 1, 1);
			} else if (util_format_is_pure_sint(pfmt)) {
				util_format_write_4i(pfmt, swapped.i, 0, &uc, 0, 0, 0, 1, 1);
			} else {
				util_pack_color(swapped.f, pfmt, &uc);
			}

			OUT_PKT4(ring, REG_A6XX_RB_BLIT_DST_INFO, 1);
			OUT_RING(ring, A6XX_RB_BLIT_DST_INFO_TILE_MODE(TILE6_LINEAR) |
				A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT(fd6_pipe2color(pfmt)));

			OUT_PKT4(ring, REG_A6XX_RB_BLIT_INFO, 1);
			OUT_RING(ring, A6XX_RB_BLIT_INFO_GMEM |
				A6XX_RB_BLIT_INFO_CLEAR_MASK(0xf));

			OUT_PKT4(ring, REG_A6XX_RB_BLIT_BASE_GMEM, 1);
			OUT_RINGP(ring, i, &ctx->batch->gmem_patches);

			OUT_PKT4(ring, REG_A6XX_RB_UNKNOWN_88D0, 1);
			OUT_RING(ring, 0);

			OUT_PKT4(ring, REG_A6XX_RB_BLIT_CLEAR_COLOR_DW0, 4);
			OUT_RING(ring, uc.ui[0]);
			OUT_RING(ring, uc.ui[1]);
			OUT_RING(ring, uc.ui[2]);
			OUT_RING(ring, uc.ui[3]);

			fd6_emit_blit(ctx, ring);
		}
	}

	if (pfb->zsbuf && (buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL))) {
		enum pipe_format pfmt = pfb->zsbuf->format;
		uint32_t clear = util_pack_z_stencil(pfmt, depth, stencil);
		uint32_t mask = 0;

		if (buffers & PIPE_CLEAR_DEPTH)
			mask |= 0x1;

		if (buffers & PIPE_CLEAR_STENCIL)
			mask |= 0x2;

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_DST_INFO, 1);
		OUT_RING(ring, A6XX_RB_BLIT_DST_INFO_TILE_MODE(TILE6_LINEAR) |
			A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT(fd6_pipe2color(pfmt)));

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_INFO, 1);
		OUT_RING(ring, A6XX_RB_BLIT_INFO_GMEM |
			// XXX UNK0 for separate stencil ??
			A6XX_RB_BLIT_INFO_DEPTH |
			A6XX_RB_BLIT_INFO_CLEAR_MASK(mask));

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_BASE_GMEM, 1);
		OUT_RINGP(ring, MAX_RENDER_TARGETS, &ctx->batch->gmem_patches);

		OUT_PKT4(ring, REG_A6XX_RB_UNKNOWN_88D0, 1);
		OUT_RING(ring, 0);

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_CLEAR_COLOR_DW0, 1);
		OUT_RING(ring, clear);

		fd6_emit_blit(ctx, ring);

#if 0
		if (pfb->zsbuf && (buffers & PIPE_CLEAR_DEPTH)) {
			struct fd_resource *zsbuf = fd_resource(pfb->zsbuf->texture);
			if (zsbuf->lrz) {
				zsbuf->lrz_valid = true;
				fd6_clear_lrz(ctx->batch, zsbuf, depth);
			}
		}
#endif
	}

	return true;
}

void
fd6_draw_init(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);
	ctx->draw_vbo = fd6_draw_vbo;
	ctx->clear = fd6_clear;
}
