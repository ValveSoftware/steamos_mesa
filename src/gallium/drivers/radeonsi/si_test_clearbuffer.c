/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 * All Rights Reserved.
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
 */

/* This file implements tests on the si_clearbuffer function. */

#include "si_pipe.h"

#define CLEARBUF_MIN 32
#define CLEARBUF_COUNT 16
#define CLEARBUF_MEMSZ 1024

static uint64_t
measure_clearbuf_time(struct pipe_context *ctx,
		      uint64_t memory_size)
{
	struct pipe_query *query_te;
	union pipe_query_result qresult;
	struct pipe_resource *buf;

	struct si_context *sctx = (struct si_context*)ctx;
	struct pipe_screen *screen = ctx->screen;

	buf = pipe_buffer_create(screen, 0, PIPE_USAGE_DEFAULT, memory_size);

	query_te = ctx->create_query(ctx, PIPE_QUERY_TIME_ELAPSED, 0);

	ctx->begin_query(ctx, query_te);
	/* operation  */
	si_cp_dma_clear_buffer(sctx, buf, 0, memory_size, 0x00,
			       SI_COHERENCY_SHADER, L2_LRU);
	ctx->end_query(ctx, query_te);
	ctx->get_query_result(ctx, query_te, true, &qresult);

	/* Cleanup. */
	ctx->destroy_query(ctx, query_te);
	pipe_resource_reference(&buf, NULL);

	/* Report Results */
	return qresult.u64;
}

/**
 * @brief Analyze rate of clearing a 1K Buffer averaged over 16 iterations
 * @param ctx Context of pipe to perform analysis on
 */
static void
analyze_clearbuf_perf_avg(struct pipe_context *ctx)
{
	uint index = 0;
	uint64_t result[CLEARBUF_COUNT];
	uint64_t sum = 0;
	long long int rate_kBps;

	/* Run Tests. */
	for (index = 0 ; index < CLEARBUF_COUNT ; index++) {
		result[index] = measure_clearbuf_time(ctx, CLEARBUF_MEMSZ);
		sum += result[index];
	}

	/* Calculate Results. */
	/*  kBps = (size(bytes))/(1000) / (time(ns)/(1000*1000*1000)) */
	rate_kBps = CLEARBUF_COUNT*CLEARBUF_MEMSZ;
	rate_kBps *= 1000UL*1000UL;
	rate_kBps /= sum;

	/* Display Results. */
	printf("CP DMA clear_buffer performance (buffer %lu ,repeat %u ):",
	       (uint64_t)CLEARBUF_MEMSZ,
	       CLEARBUF_COUNT );
	printf(" %llu kB/s\n", rate_kBps );
}

/**
 * @brief Analyze rate of clearing a range of Buffer sizes
 * @param ctx Context of pipe to perform analysis on
 */
static void
analyze_clearbuf_perf_rng(struct pipe_context *ctx)
{
	uint index = 0;
	uint64_t result[CLEARBUF_COUNT];
	uint64_t mem_size;
	long long int rate_kBps;

	/* Run Tests. */
	mem_size = CLEARBUF_MIN;
	for (index = 0 ; index < CLEARBUF_COUNT ; index++ ) {
		result[index] = measure_clearbuf_time(ctx, mem_size);
		mem_size <<= 1;
	}

	/* Calculate & Display Results. */
	/*  kBps = (size(bytes))/(1000) / (time(ns)/(1000*1000*1000)) */
	mem_size = CLEARBUF_MIN;
	for (index = 0 ; index < CLEARBUF_COUNT ; index++ ) {
		rate_kBps = mem_size;
		rate_kBps *= 1000UL*1000UL;
		rate_kBps /= result[index];

		printf("CP DMA clear_buffer performance (buffer %lu):",
		       mem_size );
		printf(" %llu kB/s\n", rate_kBps );

		mem_size <<= 1;
	}
}

void si_test_clearbuffer_perf(struct si_screen *sscreen)
{
	struct pipe_screen *screen = &sscreen->b;
	struct pipe_context *ctx = screen->context_create(screen, NULL, 0);

	analyze_clearbuf_perf_avg(ctx);
	analyze_clearbuf_perf_rng(ctx);

	exit(0);
}
