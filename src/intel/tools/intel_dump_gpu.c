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
#include <inttypes.h>

#include "intel_aub.h"
#include "aub_write.h"

#include "dev/gen_device_info.h"
#include "util/macros.h"

static int close_init_helper(int fd);
static int ioctl_init_helper(int fd, unsigned long request, ...);

static int (*libc_close)(int fd) = close_init_helper;
static int (*libc_ioctl)(int fd, unsigned long request, ...) = ioctl_init_helper;

static int drm_fd = -1;
static char *filename = NULL;
static FILE *files[2] = { NULL, NULL };
static int verbose = 0;
static bool device_override;

#define MAX_BO_COUNT 64 * 1024

struct bo {
   uint32_t size;
   uint64_t offset;
   void *map;
};

static struct bo *bos;

#define DRM_MAJOR 226

/* We set bit 0 in the map pointer for userptr BOs so we know not to
 * munmap them on DRM_IOCTL_GEM_CLOSE.
 */
#define USERPTR_FLAG 1
#define IS_USERPTR(p) ((uintptr_t) (p) & USERPTR_FLAG)
#define GET_PTR(p) ( (void *) ((uintptr_t) p & ~(uintptr_t) 1) )

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

static struct gen_device_info devinfo = {0};
static uint32_t device;
static struct aub_file aubs[2];

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

      aub_write_reloc(&devinfo, ((char *)relocated) + relocs[i].offset,
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

      for (int i = 0; i < ARRAY_SIZE(files); i++) {
         if (files[i] != NULL) {
            aub_file_init(&aubs[i], files[i], device);
            if (verbose == 2)
               aubs[i].verbose_log_file = stdout;
            aub_write_header(&aubs[i], program_invocation_short_name);
         }
      }

      if (verbose)
         printf("[intel_aubdump: running, "
                "output file %s, chipset id 0x%04x, gen %d]\n",
                filename, device, devinfo.gen);
   }

   /* Any aub */
   struct aub_file *any_aub = files[0] ? &aubs[0] : &aubs[1];;

   if (aub_use_execlists(any_aub))
      offset = 0x1000;
   else
      offset = aub_gtt_size(any_aub);

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

      for (int i = 0; i < ARRAY_SIZE(files); i++) {
         if (files[i] == NULL)
            continue;

         if (aub_use_execlists(&aubs[i]))
            aub_map_ppgtt(&aubs[i], bo->offset, bo->size);
      }
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

      for (int i = 0; i < ARRAY_SIZE(files); i++) {
         if (files[i] == NULL)
            continue;

         if (bo == batch_bo) {
            aub_write_trace_block(&aubs[i], AUB_TRACE_TYPE_BATCH,
                                  GET_PTR(data), bo->size, bo->offset);
         } else {
            aub_write_trace_block(&aubs[i], AUB_TRACE_TYPE_NOTYPE,
                                  GET_PTR(data), bo->size, bo->offset);
         }
      }
      if (data != bo->map)
         free(data);
   }

   for (int i = 0; i < ARRAY_SIZE(files); i++) {
      if (files[i] != NULL)
         continue;

      aub_write_exec(&aubs[i],
                     batch_bo->offset + execbuffer2->batch_start_offset,
                     offset, ring_flag);
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
         if (!strcmp(value, "1")) {
            verbose = 1;
         } else if (!strcmp(value, "2")) {
            verbose = 2;
         }
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
      case DRM_IOCTL_I915_GEM_EXECBUFFER2_WR: {
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
      if (aubs[i].file)
         aub_file_finish(&aubs[i]);
      else if (files[i])
         fclose(files[i]);
   }
   free(bos);
}
