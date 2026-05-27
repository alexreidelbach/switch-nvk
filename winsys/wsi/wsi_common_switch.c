/*
 * wsi_common_switch.c — VK_NN_vi_surface WSI backend for the Nintendo Switch,
 * presenting through libnx `nwindow` (the VI compositor). Mirrors the structure
 * of wsi_common_headless.c (same wsi_interface contract) and the design reverse-
 * engineered from Dan/Tiicu's wsi_common_switch.c (see D:\switch-nvk\dan-re\RE_NOTES.md).
 *
 * Model (validated against Dan's NROs):
 *   surface  = vkCreateViSurfaceNN(window = nwindowGetDefault())  -> VkIcdSurfaceVi
 *   caps     = RGBA8/BGRA8, FIFO, <=4 images, extent from nwindowGetDimensions
 *   swapchain= per image: a renderable VkImage + a libnx nvmap buffer registered
 *              with the nwindow (nwindowConfigureBuffer); slot i = image i.
 *   acquire  = nwindowDequeueBuffer -> slot -> image; cancel+OUT_OF_DATE if unknown
 *   present  = copy rendered image -> the nwindow buffer, nwindowQueueBuffer(slot)
 *
 * First milestone (correctness-first): CPU-copy present + LINEAR nwindow buffers
 * (no block-linear/GOB matching). Optimize to zero-copy (kind=0xfe block-linear,
 * native-fence present) once pixels are proven.
 *
 * SPDX-License-Identifier: MIT
 */

#include "util/macros.h"
#include "util/u_thread.h"
#include "vk_util.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#include <vulkan/vulkan_vi.h>   /* VkViSurfaceCreateInfoNN (needs VK_USE_PLATFORM_VI_NN) */
#include <switch.h>

/* VkIcdSurfaceVi { VkIcdSurfaceBase base; void *window; } is defined by vk_icd.h.
 * .window holds the libnx NWindow* (cast on use). */

/* ---- surface ---- */

struct wsi_switch {
   struct wsi_interface base;
   struct wsi_device *wsi;
   const VkAllocationCallbacks *alloc;
   VkPhysicalDevice physical_device;
};

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateViSurfaceNN(VkInstance _instance,
                      const VkViSurfaceCreateInfoNN *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   VkIcdSurfaceVi *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN);

   surface = vk_alloc2(&instance->alloc, pAllocator, sizeof(*surface), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_VI;
   surface->window = (void *)pCreateInfo->window;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}

/* ---- query callbacks ---- */

static VkResult
wsi_switch_surface_get_support(VkIcdSurfaceBase *surface,
                               struct wsi_device *wsi_device,
                               uint32_t queueFamilyIndex,
                               VkBool32 *pSupported)
{
   *pSupported = true;
   return VK_SUCCESS;
}

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
};

static VkResult
wsi_switch_surface_get_capabilities(VkIcdSurfaceBase *icd_surface,
                                    struct wsi_device *wsi_device,
                                    VkSurfaceCapabilitiesKHR *caps)
{
   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   u32 w = 1280, h = 720;
   if (surface->window != NULL)
      nwindowGetDimensions((NWindow *)surface->window, &w, &h);

   caps->minImageCount = 2;
   caps->maxImageCount = 4;
   caps->currentExtent = (VkExtent2D){ w, h };
   caps->minImageExtent = (VkExtent2D){ 1, 1 };
   caps->maxImageExtent = (VkExtent2D){ w, h };
   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;
   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   caps->supportedUsageFlags = wsi_caps_get_image_usage();
   return VK_SUCCESS;
}

static VkResult
wsi_switch_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     const void *info_next,
                                     VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);
   VkResult result =
      wsi_switch_surface_get_capabilities(surface, wsi_device, &caps->surfaceCapabilities);

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }
      default:
         break;
      }
   }
   return result;
}

/* RGBA8 / BGRA8 UNORM, sRGB-nonlinear — the formats the VI compositor scans out. */
static const VkSurfaceFormatKHR switch_formats[] = {
   { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
   { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};

static VkResult
wsi_switch_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                               struct wsi_device *wsi_device,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(switch_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) { *f = switch_formats[i]; }
   }
   return vk_outarray_status(&out);
}

static VkResult
wsi_switch_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                                struct wsi_device *wsi_device,
                                const void *info_next,
                                uint32_t *pSurfaceFormatCount,
                                VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(switch_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         f->surfaceFormat = switch_formats[i];
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
wsi_switch_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     uint32_t *pPresentModeCount,
                                     VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }
   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);
   return (*pPresentModeCount < ARRAY_SIZE(present_modes)) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
wsi_switch_surface_get_present_rectangles(VkIcdSurfaceBase *icd_surface,
                                          struct wsi_device *wsi_device,
                                          uint32_t *pRectCount,
                                          VkRect2D *pRects)
{
   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   u32 w = 1280, h = 720;
   if (surface->window != NULL)
      nwindowGetDimensions((NWindow *)surface->window, &w, &h);
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   vk_outarray_append_typed(VkRect2D, &out, rect) {
      *rect = (VkRect2D){ .offset = { 0, 0 }, .extent = { w, h } };
   }
   return vk_outarray_status(&out);
}

/* ---- swapchain ---- */

#define WSI_SWITCH_MAX_IMAGES 4

struct wsi_switch_image {
   struct wsi_image base;
   bool busy;
};

struct wsi_switch_swapchain {
   struct wsi_swapchain base;
   NWindow *window;
   Framebuffer fb;          /* libnx framebuffer = N nwindow buffers (NvGraphicBuffer +
                             * nwindowConfigureBuffer, built correctly by libnx) */
   bool fb_created;
   VkExtent2D extent;
   VkFormat vk_format;
   uint32_t next;           /* round-robin acquire index */
   struct wsi_switch_image images[WSI_SWITCH_MAX_IMAGES];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wsi_switch_swapchain, base.base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

static struct wsi_image *
wsi_switch_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain, uint32_t image_index)
{
   struct wsi_switch_swapchain *chain = (struct wsi_switch_swapchain *)wsi_chain;
   return &chain->images[image_index].base;
}

static VkResult
wsi_switch_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain,
                                        const VkAcquireNextImageInfoKHR *info,
                                        uint32_t *image_index)
{
   struct wsi_switch_swapchain *chain = (struct wsi_switch_swapchain *)wsi_chain;
   /* Round-robin a free render image; the actual nwindow dequeue/queue throttle
    * happens in queue_present via the libnx framebuffer (CPU-copy milestone). */
   for (uint32_t n = 0; n < chain->base.image_count; n++) {
      uint32_t i = (chain->next + n) % chain->base.image_count;
      if (!chain->images[i].busy) {
         chain->images[i].busy = true;
         chain->next = (i + 1) % chain->base.image_count;
         *image_index = i;
         return VK_SUCCESS;
      }
   }
   return VK_NOT_READY;
}

static VkResult
wsi_switch_swapchain_queue_present(struct wsi_swapchain *wsi_chain,
                                   uint32_t image_index,
                                   uint64_t present_id,
                                   const VkPresentRegionKHR *damage)
{
   struct wsi_switch_swapchain *chain = (struct wsi_switch_swapchain *)wsi_chain;
   struct wsi_switch_image *img = &chain->images[image_index];

   /* CPU-copy present: the rendered (host-visible) image -> the next libnx
    * framebuffer buffer (dequeue), row by row (strides differ), then present
    * (queue). Zero-copy (image IS the nwindow buffer, kind=0xfe) comes later. */
   if (chain->fb_created && img->base.cpu_map != NULL) {
      /* The common WSI present submitted the image->cpu_map blit ASYNCHRONOUSLY
       * (signalling fences[image_index]); wait for it, then invalidate the CPU
       * cache — GM20B is NOT IO-coherent, so a HOST_COHERENT map reads stale lines
       * (this was the "constant pink" bug: we re-read frame 0). Then mirror into
       * the nwindow buffer and present. */
      const struct wsi_device *wsi = chain->base.wsi;
      u32 src_stride = img->base.row_pitches[0];
      u32 sz = src_stride * chain->extent.height;
      if (chain->base.fences[image_index] != VK_NULL_HANDLE)
         wsi->WaitForFences(chain->base.device, 1,
                            &chain->base.fences[image_index], VK_TRUE, ~0ull);
      armDCacheFlush((void *)(uintptr_t)img->base.cpu_map, sz);
      u32 dst_stride = 0;
      u8 *dst = (u8 *)framebufferBegin(&chain->fb, &dst_stride);
      const u8 *src = (const u8 *)img->base.cpu_map;
      u32 row_B = MIN2(dst_stride, src_stride);
      for (u32 y = 0; y < chain->extent.height; y++)
         memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row_B);
      framebufferEnd(&chain->fb);
   }

   img->busy = false;
   return VK_SUCCESS;
}

static VkResult
wsi_switch_swapchain_release_images(struct wsi_swapchain *wsi_chain,
                                    uint32_t count, const uint32_t *indices)
{
   struct wsi_switch_swapchain *chain = (struct wsi_switch_swapchain *)wsi_chain;
   for (uint32_t i = 0; i < count; i++)
      chain->images[indices[i]].busy = false;
   return VK_SUCCESS;
}

static VkResult
wsi_switch_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                             const VkAllocationCallbacks *pAllocator)
{
   struct wsi_switch_swapchain *chain = (struct wsi_switch_swapchain *)wsi_chain;
   if (chain->fb_created)
      framebufferClose(&chain->fb);
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].base.image != VK_NULL_HANDLE)
         wsi_destroy_image(&chain->base, &chain->images[i].base);
   }
   wsi_swapchain_finish(&chain->base);
   vk_free(pAllocator, chain);
   return VK_SUCCESS;
}

static VkResult
wsi_switch_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                                    VkDevice device,
                                    struct wsi_device *wsi_device,
                                    const VkSwapchainCreateInfoKHR *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    struct wsi_swapchain **swapchain_out)
{
   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   struct wsi_switch_swapchain *chain;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   uint32_t num_images = pCreateInfo->minImageCount;
   if (num_images > WSI_SWITCH_MAX_IMAGES)
      num_images = WSI_SWITCH_MAX_IMAGES;

   size_t size = sizeof(*chain);
   chain = vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* Renderable images via the common WSI buffer-blit path (host-visible blit
    * buffer per image => img->base.cpu_map is the rendered pixels we mirror). */
   struct wsi_cpu_image_params image_params = {
      .base.image_type = WSI_IMAGE_TYPE_CPU,
   };
   result = wsi_swapchain_init(wsi_device, &chain->base, device,
                               pCreateInfo, &image_params.base, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }

   chain->base.destroy = wsi_switch_swapchain_destroy;
   chain->base.get_wsi_image = wsi_switch_swapchain_get_wsi_image;
   chain->base.acquire_next_image = wsi_switch_swapchain_acquire_next_image;
   chain->base.queue_present = wsi_switch_swapchain_queue_present;
   chain->base.release_images = wsi_switch_swapchain_release_images;
   chain->base.present_mode = VK_PRESENT_MODE_FIFO_KHR;
   chain->base.image_count = num_images;
   chain->window = (NWindow *)surface->window;
   chain->extent = pCreateInfo->imageExtent;
   chain->vk_format = pCreateInfo->imageFormat;

   for (uint32_t i = 0; i < num_images; i++) {
      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &chain->images[i].base);
      if (result != VK_SUCCESS)
         goto fail;
      chain->images[i].busy = false;
   }

   /* Register N nwindow buffers via libnx (builds the NvGraphicBuffer + calls
    * nwindowConfigureBuffer with the correct LINEAR layout). CPU-copy present
    * mirrors the rendered host-visible image into the dequeued buffer.
    * (Zero-copy: image IS the nwindow buffer, kind=0xfe — next optimization.) */
   {
      u32 pixfmt = (chain->vk_format == VK_FORMAT_B8G8R8A8_UNORM)
                   ? PIXEL_FORMAT_BGRA_8888 : PIXEL_FORMAT_RGBA_8888;
      Result rc = framebufferCreate(&chain->fb, chain->window,
                                    chain->extent.width, chain->extent.height,
                                    pixfmt, num_images);
      if (R_FAILED(rc)) { result = VK_ERROR_INITIALIZATION_FAILED; goto fail; }
      framebufferMakeLinear(&chain->fb);
      chain->fb_created = true;
   }

   *swapchain_out = &chain->base;
   return VK_SUCCESS;

fail:
   wsi_switch_swapchain_destroy(&chain->base, pAllocator);
   return result;
}

/* ---- registration ---- */

VkResult
wsi_switch_init_wsi(struct wsi_device *wsi_device,
                    const VkAllocationCallbacks *alloc,
                    VkPhysicalDevice physical_device)
{
   struct wsi_switch *wsi;
   VkResult result;

   wsi = vk_alloc(alloc, sizeof(*wsi), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wsi->wsi = wsi_device;
   wsi->alloc = alloc;
   wsi->physical_device = physical_device;

   wsi->base.get_support = wsi_switch_surface_get_support;
   wsi->base.get_capabilities2 = wsi_switch_surface_get_capabilities2;
   wsi->base.get_formats = wsi_switch_surface_get_formats;
   wsi->base.get_formats2 = wsi_switch_surface_get_formats2;
   wsi->base.get_present_modes = wsi_switch_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_switch_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_switch_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI] = &wsi->base;
   return VK_SUCCESS;

fail:
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI] = NULL;
   return result;
}

void
wsi_switch_finish_wsi(struct wsi_device *wsi_device,
                      const VkAllocationCallbacks *alloc)
{
   struct wsi_switch *wsi =
      (struct wsi_switch *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI];
   if (wsi) {
      vk_free(alloc, wsi);
      wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI] = NULL;
   }
}
