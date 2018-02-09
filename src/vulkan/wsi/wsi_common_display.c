/*
 * Copyright © 2017 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "util/macros.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <math.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include "util/hash_table.h"
#include "util/list.h"

#include "vk_util.h"
#include "wsi_common_private.h"
#include "wsi_common_display.h"
#include "wsi_common_queue.h"

#if 0
#define wsi_display_debug(...) fprintf(stderr, __VA_ARGS__)
#define wsi_display_debug_code(...)     __VA_ARGS__
#else
#define wsi_display_debug(...)
#define wsi_display_debug_code(...)
#endif

/* These have lifetime equal to the instance, so they effectively
 * never go away. This means we must keep track of them separately
 * from all other resources.
 */
typedef struct wsi_display_mode {
   struct list_head             list;
   struct wsi_display_connector *connector;
   bool                         valid; /* was found in most recent poll */
   bool                         preferred;
   uint32_t                     clock; /* in kHz */
   uint16_t                     hdisplay, hsync_start, hsync_end, htotal, hskew;
   uint16_t                     vdisplay, vsync_start, vsync_end, vtotal, vscan;
   uint32_t                     flags;
} wsi_display_mode;

typedef struct wsi_display_connector {
   struct list_head             list;
   struct wsi_display           *wsi;
   uint32_t                     id;
   uint32_t                     crtc_id;
   char                         *name;
   bool                         connected;
   bool                         active;
   struct list_head             display_modes;
   wsi_display_mode             *current_mode;
   drmModeModeInfo              current_drm_mode;
} wsi_display_connector;

struct wsi_display {
   struct wsi_interface         base;

   const VkAllocationCallbacks  *alloc;

   int                          fd;

   pthread_mutex_t              wait_mutex;
   pthread_cond_t               wait_cond;
   pthread_t                    wait_thread;

   struct list_head             connectors;
};

#define wsi_for_each_display_mode(_mode, _conn)                 \
   list_for_each_entry_safe(struct wsi_display_mode, _mode,     \
                            &(_conn)->display_modes, list)

#define wsi_for_each_connector(_conn, _dev)                             \
   list_for_each_entry_safe(struct wsi_display_connector, _conn,        \
                            &(_dev)->connectors, list)

enum wsi_image_state {
   WSI_IMAGE_IDLE,
   WSI_IMAGE_DRAWING,
   WSI_IMAGE_QUEUED,
   WSI_IMAGE_FLIPPING,
   WSI_IMAGE_DISPLAYING
};

struct wsi_display_image {
   struct wsi_image             base;
   struct wsi_display_swapchain *chain;
   enum wsi_image_state         state;
   uint32_t                     fb_id;
   uint32_t                     buffer[4];
   uint64_t                     flip_sequence;
};

struct wsi_display_swapchain {
   struct wsi_swapchain         base;
   struct wsi_display           *wsi;
   VkIcdSurfaceDisplay          *surface;
   uint64_t                     flip_sequence;
   VkResult                     status;
   struct wsi_display_image     images[0];
};

ICD_DEFINE_NONDISP_HANDLE_CASTS(wsi_display_mode, VkDisplayModeKHR)
ICD_DEFINE_NONDISP_HANDLE_CASTS(wsi_display_connector, VkDisplayKHR)

static bool
wsi_display_mode_matches_drm(wsi_display_mode *wsi,
                             drmModeModeInfoPtr drm)
{
   return wsi->clock == drm->clock &&
      wsi->hdisplay == drm->hdisplay &&
      wsi->hsync_start == drm->hsync_start &&
      wsi->hsync_end == drm->hsync_end &&
      wsi->htotal == drm->htotal &&
      wsi->hskew == drm->hskew &&
      wsi->vdisplay == drm->vdisplay &&
      wsi->vsync_start == drm->vsync_start &&
      wsi->vsync_end == drm->vsync_end &&
      wsi->vtotal == drm->vtotal &&
      MAX2(wsi->vscan, 1) == MAX2(drm->vscan, 1) &&
      wsi->flags == drm->flags;
}

static double
wsi_display_mode_refresh(struct wsi_display_mode *wsi)
{
   return (double) wsi->clock * 1000.0 / ((double) wsi->htotal *
                                          (double) wsi->vtotal *
                                          (double) MAX2(wsi->vscan, 1));
}

static uint64_t wsi_get_current_monotonic(void)
{
   struct timespec tv;

   clock_gettime(CLOCK_MONOTONIC, &tv);
   return tv.tv_nsec + tv.tv_sec*1000000000ull;
}

static uint64_t wsi_rel_to_abs_time(uint64_t rel_time)
{
   uint64_t current_time = wsi_get_current_monotonic();

   /* check for overflow */
   if (rel_time > UINT64_MAX - current_time)
      return UINT64_MAX;

   return current_time + rel_time;
}

static struct wsi_display_mode *
wsi_display_find_drm_mode(struct wsi_device *wsi_device,
                          struct wsi_display_connector *connector,
                          drmModeModeInfoPtr mode)
{
   wsi_for_each_display_mode(display_mode, connector) {
      if (wsi_display_mode_matches_drm(display_mode, mode))
         return display_mode;
   }
   return NULL;
}

static void
wsi_display_invalidate_connector_modes(struct wsi_device *wsi_device,
                                       struct wsi_display_connector *connector)
{
   wsi_for_each_display_mode(display_mode, connector) {
      display_mode->valid = false;
   }
}

static VkResult
wsi_display_register_drm_mode(struct wsi_device *wsi_device,
                              struct wsi_display_connector *connector,
                              drmModeModeInfoPtr drm_mode)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];
   struct wsi_display_mode *display_mode =
      wsi_display_find_drm_mode(wsi_device, connector, drm_mode);

   if (display_mode) {
      display_mode->valid = true;
      return VK_SUCCESS;
   }

   display_mode = vk_zalloc(wsi->alloc, sizeof (struct wsi_display_mode),
                            8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!display_mode)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   display_mode->connector = connector;
   display_mode->valid = true;
   display_mode->preferred = (drm_mode->type & DRM_MODE_TYPE_PREFERRED) != 0;
   display_mode->clock = drm_mode->clock; /* kHz */
   display_mode->hdisplay = drm_mode->hdisplay;
   display_mode->hsync_start = drm_mode->hsync_start;
   display_mode->hsync_end = drm_mode->hsync_end;
   display_mode->htotal = drm_mode->htotal;
   display_mode->hskew = drm_mode->hskew;
   display_mode->vdisplay = drm_mode->vdisplay;
   display_mode->vsync_start = drm_mode->vsync_start;
   display_mode->vsync_end = drm_mode->vsync_end;
   display_mode->vtotal = drm_mode->vtotal;
   display_mode->vscan = drm_mode->vscan;
   display_mode->flags = drm_mode->flags;

   list_addtail(&display_mode->list, &connector->display_modes);
   return VK_SUCCESS;
}

/*
 * Update our information about a specific connector
 */

static struct wsi_display_connector *
wsi_display_find_connector(struct wsi_device *wsi_device,
                          uint32_t connector_id)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   wsi_for_each_connector(connector, wsi) {
      if (connector->id == connector_id)
         return connector;
   }

   return NULL;
}

static struct wsi_display_connector *
wsi_display_alloc_connector(struct wsi_display *wsi,
                            uint32_t connector_id)
{
   struct wsi_display_connector *connector =
      vk_zalloc(wsi->alloc, sizeof (struct wsi_display_connector),
                8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

   connector->id = connector_id;
   connector->wsi = wsi;
   connector->active = false;
   /* XXX use EDID name */
   connector->name = "monitor";
   list_inithead(&connector->display_modes);
   return connector;
}

static struct wsi_display_connector *
wsi_display_get_connector(struct wsi_device *wsi_device,
                          uint32_t connector_id)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   if (wsi->fd < 0)
      return NULL;

   drmModeConnectorPtr drm_connector =
      drmModeGetConnector(wsi->fd, connector_id);

   if (!drm_connector)
      return NULL;

   struct wsi_display_connector *connector =
      wsi_display_find_connector(wsi_device, connector_id);

   if (!connector) {
      connector = wsi_display_alloc_connector(wsi, connector_id);
      if (!connector) {
         drmModeFreeConnector(drm_connector);
         return NULL;
      }
      list_addtail(&connector->list, &wsi->connectors);
   }

   connector->connected = drm_connector->connection != DRM_MODE_DISCONNECTED;

   /* Mark all connector modes as invalid */
   wsi_display_invalidate_connector_modes(wsi_device, connector);

   /*
    * List current modes, adding new ones and marking existing ones as
    * valid
    */
   for (int m = 0; m < drm_connector->count_modes; m++) {
      VkResult result = wsi_display_register_drm_mode(wsi_device,
                                                      connector,
                                                      &drm_connector->modes[m]);
      if (result != VK_SUCCESS) {
         drmModeFreeConnector(drm_connector);
         return NULL;
      }
   }

   drmModeFreeConnector(drm_connector);

   return connector;
}

#define MM_PER_PIXEL     (1.0/96.0 * 25.4)

static uint32_t
mode_size(struct wsi_display_mode *mode)
{
   /* fortunately, these are both uint16_t, so this is easy */
   return (uint32_t) mode->hdisplay * (uint32_t) mode->vdisplay;
}

static void
wsi_display_fill_in_display_properties(struct wsi_device *wsi_device,
                                       struct wsi_display_connector *connector,
                                       VkDisplayPropertiesKHR *properties)
{
   properties->display = wsi_display_connector_to_handle(connector);
   properties->displayName = connector->name;

   /* Find the first preferred mode and assume that's the physical
    * resolution. If there isn't a preferred mode, find the largest mode and
    * use that.
    */

   struct wsi_display_mode *preferred_mode = NULL, *largest_mode = NULL;
   wsi_for_each_display_mode(display_mode, connector) {
      if (!display_mode->valid)
         continue;
      if (display_mode->preferred) {
         preferred_mode = display_mode;
         break;
      }
      if (largest_mode == NULL ||
          mode_size(display_mode) > mode_size(largest_mode))
      {
         largest_mode = display_mode;
      }
   }

   if (preferred_mode) {
      properties->physicalResolution.width = preferred_mode->hdisplay;
      properties->physicalResolution.height = preferred_mode->vdisplay;
   } else if (largest_mode) {
      properties->physicalResolution.width = largest_mode->hdisplay;
      properties->physicalResolution.height = largest_mode->vdisplay;
   } else {
      properties->physicalResolution.width = 1024;
      properties->physicalResolution.height = 768;
   }

   /* Make up physical size based on 96dpi */
   properties->physicalDimensions.width =
      floor(properties->physicalResolution.width * MM_PER_PIXEL + 0.5);
   properties->physicalDimensions.height =
      floor(properties->physicalResolution.height * MM_PER_PIXEL + 0.5);

   properties->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   properties->planeReorderPossible = VK_FALSE;
   properties->persistentContent = VK_FALSE;
}

/*
 * Implement vkGetPhysicalDeviceDisplayPropertiesKHR (VK_KHR_display)
 */
VkResult
wsi_display_get_physical_device_display_properties(
   VkPhysicalDevice physical_device,
   struct wsi_device *wsi_device,
   uint32_t *property_count,
   VkDisplayPropertiesKHR *properties)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   if (wsi->fd < 0)
      goto bail;

   drmModeResPtr mode_res = drmModeGetResources(wsi->fd);

   if (!mode_res)
      goto bail;

   VK_OUTARRAY_MAKE(conn, properties, property_count);

   /* Get current information */

   for (int c = 0; c < mode_res->count_connectors; c++) {
      struct wsi_display_connector *connector =
         wsi_display_get_connector(wsi_device, mode_res->connectors[c]);

      if (!connector) {
         drmModeFreeResources(mode_res);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      if (connector->connected) {
         vk_outarray_append(&conn, prop) {
            wsi_display_fill_in_display_properties(wsi_device,
                                                   connector,
                                                   prop);
         }
      }
   }

   drmModeFreeResources(mode_res);

   return vk_outarray_status(&conn);

bail:
   *property_count = 0;
   return VK_SUCCESS;
}

/*
 * Implement vkGetPhysicalDeviceDisplayPlanePropertiesKHR (VK_KHR_display
 */
VkResult
wsi_display_get_physical_device_display_plane_properties(
   VkPhysicalDevice physical_device,
   struct wsi_device *wsi_device,
   uint32_t *property_count,
   VkDisplayPlanePropertiesKHR *properties)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   VK_OUTARRAY_MAKE(conn, properties, property_count);

   wsi_for_each_connector(connector, wsi) {
      vk_outarray_append(&conn, prop) {
         if (connector && connector->active) {
            prop->currentDisplay = wsi_display_connector_to_handle(connector);
            prop->currentStackIndex = 0;
         } else {
            prop->currentDisplay = VK_NULL_HANDLE;
            prop->currentStackIndex = 0;
         }
      }
   }
   return vk_outarray_status(&conn);
}

/*
 * Implement vkGetDisplayPlaneSupportedDisplaysKHR (VK_KHR_display)
 */

VkResult
wsi_display_get_display_plane_supported_displays(
   VkPhysicalDevice physical_device,
   struct wsi_device *wsi_device,
   uint32_t plane_index,
   uint32_t *display_count,
   VkDisplayKHR *displays)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   VK_OUTARRAY_MAKE(conn, displays, display_count);

   int c = 0;

   wsi_for_each_connector(connector, wsi) {
      if (c == plane_index && connector->connected) {
         vk_outarray_append(&conn, display) {
            *display = wsi_display_connector_to_handle(connector);
         }
      }
      c++;
   }
   return vk_outarray_status(&conn);
}

/*
 * Implement vkGetDisplayModePropertiesKHR (VK_KHR_display)
 */

VkResult
wsi_display_get_display_mode_properties(VkPhysicalDevice physical_device,
                                        struct wsi_device *wsi_device,
                                        VkDisplayKHR display,
                                        uint32_t *property_count,
                                        VkDisplayModePropertiesKHR *properties)
{
   struct wsi_display_connector *connector =
      wsi_display_connector_from_handle(display);

   VK_OUTARRAY_MAKE(conn, properties, property_count);

   wsi_for_each_display_mode(display_mode, connector) {
      if (display_mode->valid) {
         vk_outarray_append(&conn, prop) {
            prop->displayMode = wsi_display_mode_to_handle(display_mode);
            prop->parameters.visibleRegion.width = display_mode->hdisplay;
            prop->parameters.visibleRegion.height = display_mode->vdisplay;
            prop->parameters.refreshRate =
               (uint32_t) (wsi_display_mode_refresh(display_mode) * 1000 + 0.5);
         }
      }
   }
   return vk_outarray_status(&conn);
}

static bool
wsi_display_mode_matches_vk(wsi_display_mode *wsi,
                            const VkDisplayModeParametersKHR *vk)
{
   return (vk->visibleRegion.width == wsi->hdisplay &&
           vk->visibleRegion.height == wsi->vdisplay &&
           fabs(wsi_display_mode_refresh(wsi) * 1000.0 - vk->refreshRate) < 10);
}

/*
 * Implement vkCreateDisplayModeKHR (VK_KHR_display)
 */
VkResult
wsi_display_create_display_mode(VkPhysicalDevice physical_device,
                                struct wsi_device *wsi_device,
                                VkDisplayKHR display,
                                const VkDisplayModeCreateInfoKHR *create_info,
                                const VkAllocationCallbacks *allocator,
                                VkDisplayModeKHR *mode)
{
   struct wsi_display_connector *connector =
      wsi_display_connector_from_handle(display);

   if (create_info->flags != 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* Check and see if the requested mode happens to match an existing one and
    * return that. This makes the conformance suite happy. Doing more than
    * this would involve embedding the CVT function into the driver, which seems
    * excessive.
    */
   wsi_for_each_display_mode(display_mode, connector) {
      if (display_mode->valid) {
         if (wsi_display_mode_matches_vk(display_mode, &create_info->parameters)) {
            *mode = wsi_display_mode_to_handle(display_mode);
            return VK_SUCCESS;
         }
      }
   }
   return VK_ERROR_INITIALIZATION_FAILED;
}

/*
 * Implement vkGetDisplayPlaneCapabilities
 */
VkResult
wsi_get_display_plane_capabilities(VkPhysicalDevice physical_device,
                                   struct wsi_device *wsi_device,
                                   VkDisplayModeKHR mode_khr,
                                   uint32_t plane_index,
                                   VkDisplayPlaneCapabilitiesKHR *capabilities)
{
   struct wsi_display_mode *mode = wsi_display_mode_from_handle(mode_khr);

   /* XXX use actual values */
   capabilities->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
   capabilities->minSrcPosition.x = 0;
   capabilities->minSrcPosition.y = 0;
   capabilities->maxSrcPosition.x = 0;
   capabilities->maxSrcPosition.y = 0;
   capabilities->minSrcExtent.width = mode->hdisplay;
   capabilities->minSrcExtent.height = mode->vdisplay;
   capabilities->maxSrcExtent.width = mode->hdisplay;
   capabilities->maxSrcExtent.height = mode->vdisplay;
   capabilities->minDstPosition.x = 0;
   capabilities->minDstPosition.y = 0;
   capabilities->maxDstPosition.x = 0;
   capabilities->maxDstPosition.y = 0;
   capabilities->minDstExtent.width = mode->hdisplay;
   capabilities->minDstExtent.height = mode->vdisplay;
   capabilities->maxDstExtent.width = mode->hdisplay;
   capabilities->maxDstExtent.height = mode->vdisplay;
   return VK_SUCCESS;
}

VkResult
wsi_create_display_surface(VkInstance instance,
                           const VkAllocationCallbacks *allocator,
                           const VkDisplaySurfaceCreateInfoKHR *create_info,
                           VkSurfaceKHR *surface_khr)
{
   VkIcdSurfaceDisplay *surface = vk_zalloc(allocator, sizeof *surface, 8,
                                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_DISPLAY;

   surface->displayMode = create_info->displayMode;
   surface->planeIndex = create_info->planeIndex;
   surface->planeStackIndex = create_info->planeStackIndex;
   surface->transform = create_info->transform;
   surface->globalAlpha = create_info->globalAlpha;
   surface->alphaMode = create_info->alphaMode;
   surface->imageExtent = create_info->imageExtent;

   *surface_khr = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}


static VkResult
wsi_display_surface_get_support(VkIcdSurfaceBase *surface,
                                struct wsi_device *wsi_device,
                                const VkAllocationCallbacks *allocator,
                                uint32_t queueFamilyIndex,
                                int local_fd,
                                VkBool32* pSupported)
{
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

static VkResult
wsi_display_surface_get_capabilities(VkIcdSurfaceBase *surface_base,
                                     VkSurfaceCapabilitiesKHR* caps)
{
   VkIcdSurfaceDisplay *surface = (VkIcdSurfaceDisplay *) surface_base;
   wsi_display_mode *mode = wsi_display_mode_from_handle(surface->displayMode);

   caps->currentExtent.width = mode->hdisplay;
   caps->currentExtent.height = mode->vdisplay;

   /* XXX Figure out extents based on driver capabilities */
   caps->maxImageExtent = caps->minImageExtent = caps->currentExtent;

   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

   caps->minImageCount = 2;
   caps->maxImageCount = 0;

   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;
   caps->supportedUsageFlags =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   return VK_SUCCESS;
}

static VkResult
wsi_display_surface_get_capabilities2(VkIcdSurfaceBase *icd_surface,
                                      const void *info_next,
                                      VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   return wsi_display_surface_get_capabilities(icd_surface,
                                               &caps->surfaceCapabilities);
}

static const struct {
   VkFormat     format;
   uint32_t     drm_format;
} available_surface_formats[] = {
   { .format = VK_FORMAT_B8G8R8A8_SRGB, .drm_format = DRM_FORMAT_XRGB8888 },
   { .format = VK_FORMAT_B8G8R8A8_UNORM, .drm_format = DRM_FORMAT_XRGB8888 },
};

static VkResult
wsi_display_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                                struct wsi_device *wsi_device,
                                uint32_t *surface_format_count,
                                VkSurfaceFormatKHR *surface_formats)
{
   VK_OUTARRAY_MAKE(out, surface_formats, surface_format_count);

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      vk_outarray_append(&out, f) {
         f->format = available_surface_formats[i].format;
         f->colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_display_surface_get_formats2(VkIcdSurfaceBase *surface,
                                 struct wsi_device *wsi_device,
                                 const void *info_next,
                                 uint32_t *surface_format_count,
                                 VkSurfaceFormat2KHR *surface_formats)
{
   VK_OUTARRAY_MAKE(out, surface_formats, surface_format_count);

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      vk_outarray_append(&out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = available_surface_formats[i].format;
         f->surfaceFormat.colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_display_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                      uint32_t *present_mode_count,
                                      VkPresentModeKHR *present_modes)
{
   VK_OUTARRAY_MAKE(conn, present_modes, present_mode_count);

   vk_outarray_append(&conn, present) {
      *present = VK_PRESENT_MODE_FIFO_KHR;
   }

   return vk_outarray_status(&conn);
}

static void
wsi_display_destroy_buffer(struct wsi_display *wsi,
                           uint32_t buffer)
{
   (void) drmIoctl(wsi->fd, DRM_IOCTL_MODE_DESTROY_DUMB,
                   &((struct drm_mode_destroy_dumb) { .handle = buffer }));
}

static VkResult
wsi_display_image_init(VkDevice device_h,
                       struct wsi_swapchain *drv_chain,
                       const VkSwapchainCreateInfoKHR *create_info,
                       const VkAllocationCallbacks *allocator,
                       struct wsi_display_image *image)
{
   struct wsi_display_swapchain *chain =
      (struct wsi_display_swapchain *) drv_chain;
   struct wsi_display *wsi = chain->wsi;
   uint32_t drm_format = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
      if (create_info->imageFormat == available_surface_formats[i].format) {
         drm_format = available_surface_formats[i].drm_format;
         break;
      }
   }

   /* the application provided an invalid format, bail */
   if (drm_format == 0)
      return VK_ERROR_DEVICE_LOST;

   VkResult result = wsi_create_native_image(&chain->base, create_info,
                                             0, NULL, NULL,
                                             &image->base);
   if (result != VK_SUCCESS)
      return result;

   memset(image->buffer, 0, sizeof (image->buffer));

   for (unsigned int i = 0; i < image->base.num_planes; i++) {
      int ret = drmPrimeFDToHandle(wsi->fd, image->base.fds[i],
                                   &image->buffer[i]);

      close(image->base.fds[i]);
      image->base.fds[i] = -1;
      if (ret < 0)
         goto fail_handle;
   }

   image->chain = chain;
   image->state = WSI_IMAGE_IDLE;
   image->fb_id = 0;

   int ret = drmModeAddFB2(wsi->fd,
                           create_info->imageExtent.width,
                           create_info->imageExtent.height,
                           drm_format,
                           image->buffer,
                           image->base.row_pitches,
                           image->base.offsets,
                           &image->fb_id, 0);

   if (ret)
      goto fail_fb;

   return VK_SUCCESS;

fail_fb:
fail_handle:
   for (unsigned int i = 0; i < image->base.num_planes; i++) {
      if (image->buffer[i])
         wsi_display_destroy_buffer(wsi, image->buffer[i]);
      if (image->base.fds[i] != -1) {
         close(image->base.fds[i]);
         image->base.fds[i] = -1;
      }
   }

   wsi_destroy_image(&chain->base, &image->base);

   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void
wsi_display_image_finish(struct wsi_swapchain *drv_chain,
                         const VkAllocationCallbacks *allocator,
                         struct wsi_display_image *image)
{
   struct wsi_display_swapchain *chain =
      (struct wsi_display_swapchain *) drv_chain;
   struct wsi_display *wsi = chain->wsi;

   drmModeRmFB(wsi->fd, image->fb_id);
   for (unsigned int i = 0; i < image->base.num_planes; i++)
      wsi_display_destroy_buffer(wsi, image->buffer[i]);
   wsi_destroy_image(&chain->base, &image->base);
}

static VkResult
wsi_display_swapchain_destroy(struct wsi_swapchain *drv_chain,
                              const VkAllocationCallbacks *allocator)
{
   struct wsi_display_swapchain *chain =
      (struct wsi_display_swapchain *) drv_chain;

   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_display_image_finish(drv_chain, allocator, &chain->images[i]);
   vk_free(allocator, chain);
   return VK_SUCCESS;
}

static struct wsi_image *
wsi_display_get_wsi_image(struct wsi_swapchain *drv_chain,
                          uint32_t image_index)
{
   struct wsi_display_swapchain *chain =
      (struct wsi_display_swapchain *) drv_chain;

   return &chain->images[image_index].base;
}

static void
wsi_display_idle_old_displaying(struct wsi_display_image *active_image)
{
   struct wsi_display_swapchain *chain = active_image->chain;

   wsi_display_debug("idle everyone but %ld\n",
                     active_image - &(chain->images[0]));
   for (uint32_t i = 0; i < chain->base.image_count; i++)
      if (chain->images[i].state == WSI_IMAGE_DISPLAYING &&
          &chain->images[i] != active_image)
      {
         wsi_display_debug("idle %d\n", i);
         chain->images[i].state = WSI_IMAGE_IDLE;
      }
}

static VkResult
_wsi_display_queue_next(struct wsi_swapchain *drv_chain);

static void
wsi_display_page_flip_handler2(int fd,
                               unsigned int frame,
                               unsigned int sec,
                               unsigned int usec,
                               uint32_t crtc_id,
                               void *data)
{
   struct wsi_display_image *image = data;
   struct wsi_display_swapchain *chain = image->chain;

   wsi_display_debug("image %ld displayed at %d\n",
                     image - &(image->chain->images[0]), frame);
   image->state = WSI_IMAGE_DISPLAYING;
   wsi_display_idle_old_displaying(image);
   VkResult result = _wsi_display_queue_next(&(chain->base));
   if (result != VK_SUCCESS)
      chain->status = result;
}

static void wsi_display_page_flip_handler(int fd,
                                          unsigned int frame,
                                          unsigned int sec,
                                          unsigned int usec,
                                          void *data)
{
   wsi_display_page_flip_handler2(fd, frame, sec, usec, 0, data);
}

static drmEventContext event_context = {
   .version = DRM_EVENT_CONTEXT_VERSION,
   .page_flip_handler = wsi_display_page_flip_handler,
#if DRM_EVENT_CONTEXT_VERSION >= 3
   .page_flip_handler2 = wsi_display_page_flip_handler2,
#endif
};

static void *
wsi_display_wait_thread(void *data)
{
   struct wsi_display *wsi = data;
   struct pollfd pollfd = {
      .fd = wsi->fd,
      .events = POLLIN
   };

   pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
   for (;;) {
      int ret = poll(&pollfd, 1, -1);
      if (ret > 0) {
         pthread_mutex_lock(&wsi->wait_mutex);
         (void) drmHandleEvent(wsi->fd, &event_context);
         pthread_mutex_unlock(&wsi->wait_mutex);
         pthread_cond_broadcast(&wsi->wait_cond);
      }
   }
   return NULL;
}

static int
wsi_display_start_wait_thread(struct wsi_display *wsi)
{
   if (!wsi->wait_thread) {
      int ret = pthread_create(&wsi->wait_thread, NULL,
                               wsi_display_wait_thread, wsi);
      if (ret)
         return ret;
   }
   return 0;
}

/*
 * Wait for at least one event from the kernel to be processed.
 * Call with wait_mutex held
 */
static int
wsi_display_wait_for_event(struct wsi_display *wsi,
                           uint64_t timeout_ns)
{
   int ret;

   ret = wsi_display_start_wait_thread(wsi);

   if (ret)
      return ret;

   struct timespec abs_timeout = {
      .tv_sec = timeout_ns / 1000000000ULL,
      .tv_nsec = timeout_ns % 1000000000ULL,
   };

   ret = pthread_cond_timedwait(&wsi->wait_cond, &wsi->wait_mutex,
                                &abs_timeout);

   wsi_display_debug("%9ld done waiting for event %d\n", pthread_self(), ret);
   return ret;
}

static VkResult
wsi_display_acquire_next_image(struct wsi_swapchain *drv_chain,
                               uint64_t timeout,
                               VkSemaphore semaphore,
                               uint32_t *image_index)
{
   struct wsi_display_swapchain *chain =
      (struct wsi_display_swapchain *)drv_chain;
   struct wsi_display *wsi = chain->wsi;
   int ret = 0;
   VkResult result = VK_SUCCESS;

   /* Bail early if the swapchain is broken */
   if (chain->status != VK_SUCCESS)
      return chain->status;

   if (timeout != 0 && timeout != UINT64_MAX)
      timeout = wsi_rel_to_abs_time(timeout);

   pthread_mutex_lock(&wsi->wait_mutex);
   for (;;) {
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (chain->images[i].state == WSI_IMAGE_IDLE) {
            *image_index = i;
            wsi_display_debug("image %d available\n", i);
            chain->images[i].state = WSI_IMAGE_DRAWING;
            result = VK_SUCCESS;
            goto done;
         }
         wsi_display_debug("image %d state %d\n", i, chain->images[i].state);
      }

      if (ret == ETIMEDOUT) {
         result = VK_TIMEOUT;
         goto done;
      }

      ret = wsi_display_wait_for_event(wsi, timeout);

      if (ret && ret != ETIMEDOUT) {
         result = VK_ERROR_OUT_OF_DATE_KHR;
         goto done;
      }
   }
done:
   pthread_mutex_unlock(&wsi->wait_mutex);

   if (result != VK_SUCCESS)
      return result;

   return chain->status;
}

/*
 * Check whether there are any other connectors driven by this crtc
 */
static bool
wsi_display_crtc_solo(struct wsi_display *wsi,
                      drmModeResPtr mode_res,
                      drmModeConnectorPtr connector,
                      uint32_t crtc_id)
{
   /* See if any other connectors share the same encoder */
   for (int c = 0; c < mode_res->count_connectors; c++) {
      if (mode_res->connectors[c] == connector->connector_id)
         continue;

      drmModeConnectorPtr other_connector =
         drmModeGetConnector(wsi->fd, mode_res->connectors[c]);

      if (other_connector) {
         bool match = (other_connector->encoder_id == connector->encoder_id);
         drmModeFreeConnector(other_connector);
         if (match)
            return false;
      }
   }

   /* See if any other encoders share the same crtc */
   for (int e = 0; e < mode_res->count_encoders; e++) {
      if (mode_res->encoders[e] == connector->encoder_id)
         continue;

      drmModeEncoderPtr other_encoder =
         drmModeGetEncoder(wsi->fd, mode_res->encoders[e]);

      if (other_encoder) {
         bool match = (other_encoder->crtc_id == crtc_id);
         drmModeFreeEncoder(other_encoder);
         if (match)
            return false;
      }
   }
   return true;
}

/*
 * Pick a suitable CRTC to drive this connector. Prefer a CRTC which is
 * currently driving this connector and not any others. Settle for a CRTC
 * which is currently idle.
 */
static uint32_t
wsi_display_select_crtc(struct wsi_display_connector *connector,
                        drmModeResPtr mode_res,
                        drmModeConnectorPtr drm_connector)
{
   struct wsi_display *wsi = connector->wsi;

   /* See what CRTC is currently driving this connector */
   if (drm_connector->encoder_id) {
      drmModeEncoderPtr encoder =
         drmModeGetEncoder(wsi->fd, drm_connector->encoder_id);

      if (encoder) {
         uint32_t crtc_id = encoder->crtc_id;
         drmModeFreeEncoder(encoder);
         if (crtc_id) {
            if (wsi_display_crtc_solo(wsi, mode_res, drm_connector, crtc_id))
               return crtc_id;
         }
      }
   }
   uint32_t crtc_id = 0;
   for (int c = 0; crtc_id == 0 && c < mode_res->count_crtcs; c++) {
      drmModeCrtcPtr crtc = drmModeGetCrtc(wsi->fd, mode_res->crtcs[c]);
      if (crtc && crtc->buffer_id == 0)
         crtc_id = crtc->crtc_id;
      drmModeFreeCrtc(crtc);
   }
   return crtc_id;
}

static VkResult
wsi_display_setup_connector(wsi_display_connector *connector,
                            wsi_display_mode *display_mode)
{
   struct wsi_display *wsi = connector->wsi;

   if (connector->current_mode == display_mode && connector->crtc_id)
      return VK_SUCCESS;

   VkResult result = VK_SUCCESS;

   drmModeResPtr mode_res = drmModeGetResources(wsi->fd);
   if (!mode_res) {
      if (errno == ENOMEM)
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      else
         result = VK_ERROR_OUT_OF_DATE_KHR;
      goto bail;
   }

   drmModeConnectorPtr drm_connector =
      drmModeGetConnectorCurrent(wsi->fd, connector->id);

   if (!drm_connector) {
      if (errno == ENOMEM)
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      else
         result = VK_ERROR_OUT_OF_DATE_KHR;
      goto bail_mode_res;
   }

   /* Pick a CRTC if we don't have one */
   if (!connector->crtc_id) {
      connector->crtc_id = wsi_display_select_crtc(connector,
                                                   mode_res, drm_connector);
      if (!connector->crtc_id) {
         result = VK_ERROR_OUT_OF_DATE_KHR;
         goto bail_connector;
      }
   }

   if (connector->current_mode != display_mode) {

      /* Find the drm mode corresponding to the requested VkDisplayMode */
      drmModeModeInfoPtr drm_mode = NULL;

      for (int m = 0; m < drm_connector->count_modes; m++) {
         drm_mode = &drm_connector->modes[m];
         if (wsi_display_mode_matches_drm(display_mode, drm_mode))
            break;
         drm_mode = NULL;
      }

      if (!drm_mode) {
         result = VK_ERROR_OUT_OF_DATE_KHR;
         goto bail_connector;
      }

      connector->current_mode = display_mode;
      connector->current_drm_mode = *drm_mode;
   }

bail_connector:
   drmModeFreeConnector(drm_connector);
bail_mode_res:
   drmModeFreeResources(mode_res);
bail:
   return result;

}

/*
 * Check to see if the kernel has no flip queued and if there's an image
 * waiting to be displayed.
 */
static VkResult
_wsi_display_queue_next(struct wsi_swapchain *drv_chain)
{
   struct wsi_display_swapchain *chain =
      (struct wsi_display_swapchain *) drv_chain;
   struct wsi_display *wsi = chain->wsi;
   VkIcdSurfaceDisplay *surface = chain->surface;
   wsi_display_mode *display_mode =
      wsi_display_mode_from_handle(surface->displayMode);
   wsi_display_connector *connector = display_mode->connector;

   if (wsi->fd < 0)
      return VK_ERROR_OUT_OF_DATE_KHR;

   if (display_mode != connector->current_mode)
      connector->active = false;

   for (;;) {

      /* Check to see if there is an image to display, or if some image is
       * already queued */

      struct wsi_display_image *image = NULL;

      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         struct wsi_display_image *tmp_image = &chain->images[i];

         switch (tmp_image->state) {
         case WSI_IMAGE_FLIPPING:
            /* already flipping, don't send another to the kernel yet */
            return VK_SUCCESS;
         case WSI_IMAGE_QUEUED:
            /* find the oldest queued */
            if (!image || tmp_image->flip_sequence < image->flip_sequence)
               image = tmp_image;
            break;
         default:
            break;
         }
      }

      if (!image)
         return VK_SUCCESS;

      int ret;
      if (connector->active) {
         ret = drmModePageFlip(wsi->fd, connector->crtc_id, image->fb_id,
                                   DRM_MODE_PAGE_FLIP_EVENT, image);
         if (ret == 0) {
            image->state = WSI_IMAGE_FLIPPING;
            return VK_SUCCESS;
         }
         wsi_display_debug("page flip err %d %s\n", ret, strerror(-ret));
      } else {
         ret = -EINVAL;
      }

      if (ret == -EINVAL) {
         VkResult result = wsi_display_setup_connector(connector, display_mode);

         if (result != VK_SUCCESS) {
            image->state = WSI_IMAGE_IDLE;
            return result;
         }

         /* XXX allow setting of position */
         ret = drmModeSetCrtc(wsi->fd, connector->crtc_id,
                              image->fb_id, 0, 0,
                              &connector->id, 1,
                              &connector->current_drm_mode);
         if (ret == 0) {
            /* Assume that the mode set is synchronous and that any
             * previous image is now idle.
             */
            image->state = WSI_IMAGE_DISPLAYING;
            wsi_display_idle_old_displaying(image);
            connector->active = true;
            return VK_SUCCESS;
         }
      }

      if (ret != -EACCES) {
         connector->active = false;
         image->state = WSI_IMAGE_IDLE;
         return VK_ERROR_OUT_OF_DATE_KHR;
      }

      /* Some other VT is currently active. Sit here waiting for
       * our VT to become active again by polling once a second
       */
      usleep(1000 * 1000);
      connector->active = false;
   }
}

static VkResult
wsi_display_queue_present(struct wsi_swapchain *drv_chain,
                          uint32_t image_index,
                          const VkPresentRegionKHR *damage)
{
   struct wsi_display_swapchain *chain =
      (struct wsi_display_swapchain *) drv_chain;
   struct wsi_display *wsi = chain->wsi;
   struct wsi_display_image *image = &chain->images[image_index];
   VkResult result;

   /* Bail early if the swapchain is broken */
   if (chain->status != VK_SUCCESS)
      return chain->status;

   assert(image->state == WSI_IMAGE_DRAWING);
   wsi_display_debug("present %d\n", image_index);

   pthread_mutex_lock(&wsi->wait_mutex);

   image->flip_sequence = ++chain->flip_sequence;
   image->state = WSI_IMAGE_QUEUED;

   result = _wsi_display_queue_next(drv_chain);
   if (result != VK_SUCCESS)
      chain->status = result;

   pthread_mutex_unlock(&wsi->wait_mutex);

   if (result != VK_SUCCESS)
      return result;

   return chain->status;
}

static VkResult
wsi_display_surface_create_swapchain(
   VkIcdSurfaceBase *icd_surface,
   VkDevice device,
   struct wsi_device *wsi_device,
   int local_fd,
   const VkSwapchainCreateInfoKHR *create_info,
   const VkAllocationCallbacks *allocator,
   struct wsi_swapchain **swapchain_out)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   assert(create_info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   const unsigned num_images = create_info->minImageCount;
   struct wsi_display_swapchain *chain =
      vk_zalloc(allocator,
                sizeof(*chain) + num_images * sizeof(chain->images[0]),
                8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = wsi_swapchain_init(wsi_device, &chain->base, device,
                                        create_info, allocator);

   chain->base.destroy = wsi_display_swapchain_destroy;
   chain->base.get_wsi_image = wsi_display_get_wsi_image;
   chain->base.acquire_next_image = wsi_display_acquire_next_image;
   chain->base.queue_present = wsi_display_queue_present;
   chain->base.present_mode = create_info->presentMode;
   chain->base.image_count = num_images;

   chain->wsi = wsi;
   chain->status = VK_SUCCESS;

   chain->surface = (VkIcdSurfaceDisplay *) icd_surface;

   for (uint32_t image = 0; image < chain->base.image_count; image++) {
      result = wsi_display_image_init(device, &chain->base,
                                      create_info, allocator,
                                      &chain->images[image]);
      if (result != VK_SUCCESS) {
         while (image > 0) {
            --image;
            wsi_display_image_finish(&chain->base, allocator,
                                     &chain->images[image]);
         }
         vk_free(allocator, chain);
         goto fail_init_images;
      }
   }

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail_init_images:
   return result;
}

static bool
wsi_init_pthread_cond_monotonic(pthread_cond_t *cond)
{
   pthread_condattr_t condattr;
   bool ret = false;

   if (pthread_condattr_init(&condattr) != 0)
      goto fail_attr_init;

   if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0)
      goto fail_attr_set;

   if (pthread_cond_init(cond, &condattr) != 0)
      goto fail_cond_init;

   ret = true;

fail_cond_init:
fail_attr_set:
   pthread_condattr_destroy(&condattr);
fail_attr_init:
   return ret;
}

VkResult
wsi_display_init_wsi(struct wsi_device *wsi_device,
                     const VkAllocationCallbacks *alloc,
                     int display_fd)
{
   struct wsi_display *wsi = vk_zalloc(alloc, sizeof(*wsi), 8,
                                       VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   VkResult result;

   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wsi->fd = display_fd;
   wsi->alloc = alloc;

   list_inithead(&wsi->connectors);

   int ret = pthread_mutex_init(&wsi->wait_mutex, NULL);
   if (ret) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_mutex;
   }

   if (!wsi_init_pthread_cond_monotonic(&wsi->wait_cond)) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_cond;
   }

   wsi->base.get_support = wsi_display_surface_get_support;
   wsi->base.get_capabilities = wsi_display_surface_get_capabilities;
   wsi->base.get_capabilities2 = wsi_display_surface_get_capabilities2;
   wsi->base.get_formats = wsi_display_surface_get_formats;
   wsi->base.get_formats2 = wsi_display_surface_get_formats2;
   wsi->base.get_present_modes = wsi_display_surface_get_present_modes;
   wsi->base.create_swapchain = wsi_display_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = &wsi->base;

   return VK_SUCCESS;

fail_cond:
   pthread_mutex_destroy(&wsi->wait_mutex);
fail_mutex:
   vk_free(alloc, wsi);
fail:
   return result;
}

void
wsi_display_finish_wsi(struct wsi_device *wsi_device,
                       const VkAllocationCallbacks *alloc)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   if (wsi) {
      wsi_for_each_connector(connector, wsi) {
         wsi_for_each_display_mode(mode, connector) {
            vk_free(wsi->alloc, mode);
         }
         vk_free(wsi->alloc, connector);
      }

      pthread_mutex_lock(&wsi->wait_mutex);
      if (wsi->wait_thread) {
         pthread_cancel(wsi->wait_thread);
         pthread_join(wsi->wait_thread, NULL);
      }
      pthread_mutex_unlock(&wsi->wait_mutex);
      pthread_mutex_destroy(&wsi->wait_mutex);
      pthread_cond_destroy(&wsi->wait_cond);

      vk_free(alloc, wsi);
   }
}

/*
 * Implement vkReleaseDisplay
 */
VkResult
wsi_release_display(VkPhysicalDevice            physical_device,
                    struct wsi_device           *wsi_device,
                    VkDisplayKHR                display)
{
   struct wsi_display *wsi =
      (struct wsi_display *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY];

   if (wsi->fd >= 0) {
      close(wsi->fd);
      wsi->fd = -1;
   }
   return VK_SUCCESS;
}
