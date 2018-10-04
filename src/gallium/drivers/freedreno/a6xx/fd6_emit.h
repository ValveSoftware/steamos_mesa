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

#ifndef FD6_EMIT_H
#define FD6_EMIT_H

#include "pipe/p_context.h"

#include "freedreno_context.h"
#include "fd6_context.h"
#include "fd6_format.h"
#include "fd6_program.h"
#include "ir3_shader.h"

struct fd_ringbuffer;

/* grouped together emit-state for prog/vertex/state emit: */
struct fd6_emit {
	struct pipe_debug_callback *debug;
	const struct fd_vertex_state *vtx;
	const struct fd_program_stateobj *prog;
	const struct pipe_draw_info *info;
	struct ir3_shader_key key;
	enum fd_dirty_3d_state dirty;

	uint32_t sprite_coord_enable;  /* bitmask */
	bool sprite_coord_mode;
	bool rasterflat;
	bool no_decode_srgb;

	/* in binning pass, we don't have real frag shader, so we
	 * don't know if real draw disqualifies lrz write.  So just
	 * figure that out up-front and stash it in the emit.
	 */
	bool no_lrz_write;

	/* cached to avoid repeated lookups of same variants: */
	const struct ir3_shader_variant *vp, *fp;
	/* TODO: other shader stages.. */

	unsigned streamout_mask;
};

static inline const struct ir3_shader_variant *
fd6_emit_get_vp(struct fd6_emit *emit)
{
	if (!emit->vp) {
		struct ir3_shader *shader = emit->prog->vp;
		emit->vp = ir3_shader_variant(shader, emit->key, emit->debug);
	}
	return emit->vp;
}

static inline const struct ir3_shader_variant *
fd6_emit_get_fp(struct fd6_emit *emit)
{
	if (!emit->fp) {
		if (emit->key.binning_pass) {
			/* use dummy stateobj to simplify binning vs non-binning: */
			static const struct ir3_shader_variant binning_fp = {};
			emit->fp = &binning_fp;
		} else {
			struct ir3_shader *shader = emit->prog->fp;
			emit->fp = ir3_shader_variant(shader, emit->key,emit->debug);
		}
	}
	return emit->fp;
}

static inline void
fd6_event_write(struct fd_batch *batch, struct fd_ringbuffer *ring,
		enum vgt_event_type evt, bool timestamp)
{
	fd_reset_wfi(batch);

	OUT_PKT7(ring, CP_EVENT_WRITE, timestamp ? 4 : 1);
	OUT_RING(ring, CP_EVENT_WRITE_0_EVENT(evt));
	if (timestamp) {
		struct fd6_context *fd6_ctx = fd6_context(batch->ctx);
		OUT_RELOCW(ring, fd6_ctx->blit_mem, 0, 0, 0);  /* ADDR_LO/HI */
		OUT_RING(ring, ++fd6_ctx->seqno);
	}
}

static inline void
fd6_cache_flush(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
	fd6_event_write(batch, ring, 0x31, false);
}

static inline void
fd6_emit_blit(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
	emit_marker6(ring, 7);
	fd6_event_write(batch, ring, BLIT, false);
	emit_marker6(ring, 7);
}

static inline void
fd6_emit_render_cntl(struct fd_context *ctx, bool blit, bool binning)
{
#if 0
	struct fd_ringbuffer *ring = binning ? ctx->batch->binning : ctx->batch->draw;

	/* TODO eventually this partially depends on the pfb state, ie.
	 * which of the cbuf(s)/zsbuf has an UBWC flag buffer.. that part
	 * we could probably cache and just regenerate if framebuffer
	 * state is dirty (or something like that)..
	 *
	 * Other bits seem to depend on query state, like if samples-passed
	 * query is active.
	 */
	bool samples_passed = (fd6_context(ctx)->samples_passed_queries > 0);
	OUT_PKT4(ring, REG_A6XX_RB_RENDER_CNTL, 1);
	OUT_RING(ring, 0x00000000 |   /* RB_RENDER_CNTL */
			COND(binning, A6XX_RB_RENDER_CNTL_BINNING_PASS) |
			COND(binning, A6XX_RB_RENDER_CNTL_DISABLE_COLOR_PIPE) |
			COND(samples_passed, A6XX_RB_RENDER_CNTL_SAMPLES_PASSED) |
			COND(!blit, 0x8));
	OUT_PKT4(ring, REG_A6XX_GRAS_SC_CNTL, 1);
	OUT_RING(ring, 0x00000008 |   /* GRAS_SC_CNTL */
			COND(binning, A6XX_GRAS_SC_CNTL_BINNING_PASS) |
			COND(samples_passed, A6XX_GRAS_SC_CNTL_SAMPLES_PASSED));
#else
	DBG("render ctl stub");
#endif
}

static inline void
fd6_emit_lrz_flush(struct fd_ringbuffer *ring)
{
	OUT_PKT7(ring, CP_EVENT_WRITE, 1);
	OUT_RING(ring, LRZ_FLUSH);
}

static inline enum a6xx_state_block
fd6_stage2shadersb(enum shader_t type)
{
	switch (type) {
	case SHADER_VERTEX:
		return SB6_VS_SHADER;
	case SHADER_FRAGMENT:
		return SB6_FS_SHADER;
	case SHADER_COMPUTE:
		return SB6_CS_SHADER;
	default:
		unreachable("bad shader type");
		return ~0;
	}
}

void fd6_emit_vertex_bufs(struct fd_ringbuffer *ring, struct fd6_emit *emit);

void fd6_emit_state(struct fd_context *ctx, struct fd_ringbuffer *ring,
		struct fd6_emit *emit);

void fd6_emit_cs_state(struct fd_context *ctx, struct fd_ringbuffer *ring,
		struct ir3_shader_variant *cp);

void fd6_emit_restore(struct fd_batch *batch, struct fd_ringbuffer *ring);

void fd6_emit_init(struct pipe_context *pctx);

#define WRITE(reg, val) do {					\
		OUT_PKT4(ring, reg, 1);					\
		OUT_RING(ring, val);					\
	} while (0)


#endif /* FD6_EMIT_H */
