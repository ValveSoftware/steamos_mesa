/*
 * Copyright © 2014 Broadcom
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

/** @file v3d_fence.c
 *
 * Seqno-based fence management.
 *
 * We have two mechanisms for waiting in our kernel API: You can wait on a BO
 * to have all rendering to from any process to be completed, or wait on a
 * seqno for that particular seqno to be passed.  The fence API we're
 * implementing is based on waiting for all rendering in the context to have
 * completed (with no reference to what other processes might be doing with
 * the same BOs), so we can just use the seqno of the last rendering we'd
 * fired off as our fence marker.
 */

#include "util/u_inlines.h"

#include "v3d_context.h"
#include "v3d_bufmgr.h"

struct v3d_fence {
        struct pipe_reference reference;
        uint32_t sync;
};

static void
v3d_fence_reference(struct pipe_screen *pscreen,
                    struct pipe_fence_handle **pp,
                    struct pipe_fence_handle *pf)
{
        struct v3d_screen *screen = v3d_screen(pscreen);
        struct v3d_fence **p = (struct v3d_fence **)pp;
        struct v3d_fence *f = (struct v3d_fence *)pf;
        struct v3d_fence *old = *p;

        if (pipe_reference(&(*p)->reference, &f->reference)) {
                drmSyncobjDestroy(screen->fd, old->sync);
                free(old);
        }
        *p = f;
}

static boolean
v3d_fence_finish(struct pipe_screen *pscreen,
		 struct pipe_context *ctx,
                 struct pipe_fence_handle *pf,
                 uint64_t timeout_ns)
{
        struct v3d_screen *screen = v3d_screen(pscreen);
        struct v3d_fence *f = (struct v3d_fence *)pf;

        return drmSyncobjWait(screen->fd, &f->sync, 1, timeout_ns, 0, NULL);
}

struct v3d_fence *
v3d_fence_create(struct v3d_context *v3d)
{
        struct v3d_fence *f = calloc(1, sizeof(*f));
        if (!f)
                return NULL;

        uint32_t new_sync;
        /* Make a new sync object for the context. */
        int ret = drmSyncobjCreate(v3d->fd, DRM_SYNCOBJ_CREATE_SIGNALED,
                                   &new_sync);
        if (ret) {
                free(f);
                return NULL;
        }

        pipe_reference_init(&f->reference, 1);
        f->sync = v3d->out_sync;
        v3d->out_sync = new_sync;

        return f;
}

void
v3d_fence_init(struct v3d_screen *screen)
{
        screen->base.fence_reference = v3d_fence_reference;
        screen->base.fence_finish = v3d_fence_finish;
}
