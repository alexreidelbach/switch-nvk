/*
 * nvk_textures.c — Roadmap Tier 1.3: mipmaps + sRGB + block-compressed (BC1)
 * textures on the Switch GM20B, by our own NVK. Five textured quads, sampling
 * three textures that exercise NIL's format/mip layout paths:
 *
 *   - texMip  (R8G8B8A8_UNORM, 3 mip levels: L0=red, L1=green, L2=blue) sampled
 *     at EXPLICIT LODs 0/1/2 via textureLod (push constant) — proves per-level
 *     mip layout (offsets/strides) + sampling. Shown as 3 stacked quads (left).
 *   - texSrgb (R8G8B8A8_SRGB, solid 188 grey) — the texture unit DECODES sRGB to
 *     linear on sample, so 188 (~0.737 encoded) becomes ~0.5 linear ~= 128 in the
 *     UNORM target (NOT 188). Proves the sRGB format path. (centre quad)
 *   - texBc1  (BC1_RGB_UNORM_BLOCK, a solid-yellow 4x4 block) — proves the
 *     block-compressed format decompress path. (right quad)
 *
 *   A..D  instance / device / queue
 *   T  three textures (mip/sRGB/BC1) uploaded via staging + barriers
 *   U  unit quad (pos+uv) vbuf/ibuf + a sampler + 3 descriptor sets
 *   N  pipeline (tex.vert/tex.frag, push-constant rect+lod, sampler)
 *   O  render pass: 3 mip-LOD quads + sRGB quad + BC1 quad, copy
 *   L  submit, read back, verify each region's colour (red/green/blue/grey/yellow)
 *   P  present on the TV, until +
 *
 * Loaderless ICD (VK_NO_PROTOTYPES); logs sdmc:/nvk_textures.log.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "shaders/tri_shaders.h"   /* tex_vert_spv[], tex_frag_spv[] + _sz */

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
extern void (*g_drm_shim_log_sink)(const char *);

#define SCN_W 512u
#define SCN_H 288u
#define SCN_PIXELS (SCN_W * SCN_H)
#define SCN_BYTES (SCN_PIXELS * 4u)
#define SCR_W 1280u
#define SCR_H 720u

#define MIP_W 64u
#define MIP_H 64u
#define MIP_LEVELS 3u   /* 64, 32, 16 */

static FILE *g_log;
static uint32_t g_shot[SCN_PIXELS];

/* unit quad in [-1,1] with UV in [0,1]: pos.xy, uv.xy (stride 16). */
static const float QUAD_V[4 * 4] = {
   -1.f,-1.f, 0.f,0.f,
    1.f,-1.f, 1.f,0.f,
    1.f, 1.f, 1.f,1.f,
   -1.f, 1.f, 0.f,1.f,
};
static const uint16_t QUAD_I[6] = { 0,1,2, 0,2,3 };

/* push constant: rect=(cx,cy,halfw,halfh), lod.x = mip level to sample. */
struct PC { float rect[4]; float lod[4]; };

static void shim_log_sink(const char *s)
{ if (g_log) { fputs(s, g_log); fflush(g_log); } printf("%s", s); fflush(stdout); }
static void slogf(const char *fmt, ...)
{
   char buf[512]; va_list ap; va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
   if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
   printf("%s\n", buf); fflush(stdout);
}
#define LOG(...) slogf(__VA_ARGS__)

static uint32_t pick_mem_type(const VkPhysicalDeviceMemoryProperties *mp,
                              uint32_t type_bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
      if ((type_bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
   return UINT32_MAX;
}
static uint32_t sample_ndc(const uint32_t *px, float nx, float ny)
{
   int ix = (int)((nx * 0.5f + 0.5f) * (float)SCN_W);
   int iy = (int)((ny * 0.5f + 0.5f) * (float)SCN_H);
   if (ix < 0) ix = 0;
   if (ix >= (int)SCN_W) ix = SCN_W - 1;
   if (iy < 0) iy = 0;
   if (iy >= (int)SCN_H) iy = SCN_H - 1;
   return px[iy * SCN_W + ix];
}
/* compare a sampled pixel's RGB to an expected RGB within a tolerance. */
static int rgb_near(uint32_t px, int er, int eg, int eb, int tol)
{
   int r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
   return (r >= er - tol && r <= er + tol) && (g >= eg - tol && g <= eg + tol) &&
          (b >= eb - tol && b <= eb + tol);
}

int main(void)
{
   g_log = fopen("sdmc:/nvk_textures.log", "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
      nxlinkStdio();
   LOG("=== NVK textures (Roadmap T1.3: mipmaps + sRGB + BC1) [BUILD tex1] ===");

   g_drm_shim_log_sink = shim_log_sink;
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
   setenv("MESA_LOG_FILE", "sdmc:/nvk_textures_mesa.log", 1);

   VkInstance inst = VK_NULL_HANDLE;
   PFN_vkCreateInstance pCreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("FAIL: no vkCreateInstance"); goto done; }
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_textures", .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkResult r = pCreateInstance(&ici, NULL, &inst);
   LOG("A vkCreateInstance -> %d", r);
   if (r != VK_SUCCESS) goto done;

#define GI(n) ((PFN_##n)vk_icdGetInstanceProcAddr(inst, #n))
   PFN_vkEnumeratePhysicalDevices          pEnum  = GI(vkEnumeratePhysicalDevices);
   PFN_vkGetPhysicalDeviceMemoryProperties pMemP  = GI(vkGetPhysicalDeviceMemoryProperties);
   PFN_vkGetPhysicalDeviceQueueFamilyProperties pQF = GI(vkGetPhysicalDeviceQueueFamilyProperties);
   PFN_vkCreateDevice                      pCreateDev = GI(vkCreateDevice);
   PFN_vkGetDeviceProcAddr                 pGDPA  = GI(vkGetDeviceProcAddr);
   if (!pEnum || !pCreateDev || !pGDPA) { LOG("FAIL: instance fns"); goto done; }

   uint32_t ndev = 1; VkPhysicalDevice phys[4];
   r = pEnum(inst, &ndev, NULL);
   if (r != VK_SUCCESS || ndev == 0) { LOG("FAIL B: no devices"); goto done; }
   ndev = (ndev > 4) ? 4 : ndev;
   r = pEnum(inst, &ndev, phys);
   if (r != VK_SUCCESS) { LOG("FAIL B: enum2 %d", r); goto done; }
   LOG("B enumerate -> %u device(s)", ndev);

   uint32_t nqf = 0; pQF(phys[0], &nqf, NULL);
   VkQueueFamilyProperties qf[8]; nqf = (nqf > 8) ? 8 : nqf;
   pQF(phys[0], &nqf, qf);
   uint32_t qfi = UINT32_MAX;
   for (uint32_t i = 0; i < nqf; i++) if (qfi==UINT32_MAX && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi = i;
   if (qfi == UINT32_MAX) { LOG("FAIL C: no gfx queue"); goto done; }
   LOG("C gfx queue family %u", qfi);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   VkDevice dev = VK_NULL_HANDLE;
   r = pCreateDev(phys[0], &dci, NULL, &dev);
   LOG("D vkCreateDevice -> %d", r);
   if (r != VK_SUCCESS) goto done;

#define GD(n) ((PFN_##n)pGDPA(dev, #n))
   PFN_vkGetDeviceQueue pGetQueue = GD(vkGetDeviceQueue);
   PFN_vkAllocateMemory pAlloc = GD(vkAllocateMemory);
   PFN_vkMapMemory pMap = GD(vkMapMemory);
   PFN_vkCreateImage pCreateImg = GD(vkCreateImage);
   PFN_vkGetImageMemoryRequirements pImgReq = GD(vkGetImageMemoryRequirements);
   PFN_vkBindImageMemory pBindImg = GD(vkBindImageMemory);
   PFN_vkCreateImageView pCreateView = GD(vkCreateImageView);
   PFN_vkCreateSampler pCreateSamp = GD(vkCreateSampler);
   PFN_vkCreateRenderPass pCreateRP = GD(vkCreateRenderPass);
   PFN_vkCreateFramebuffer pCreateFB = GD(vkCreateFramebuffer);
   PFN_vkCreateShaderModule pCreateSM = GD(vkCreateShaderModule);
   PFN_vkCreateDescriptorSetLayout pCreateDSL = GD(vkCreateDescriptorSetLayout);
   PFN_vkCreateDescriptorPool pCreateDP = GD(vkCreateDescriptorPool);
   PFN_vkAllocateDescriptorSets pAllocDS = GD(vkAllocateDescriptorSets);
   PFN_vkUpdateDescriptorSets pUpdateDS = GD(vkUpdateDescriptorSets);
   PFN_vkCreatePipelineLayout pCreatePL = GD(vkCreatePipelineLayout);
   PFN_vkCreateGraphicsPipelines pCreateGP = GD(vkCreateGraphicsPipelines);
   PFN_vkCreateBuffer pCreateBuf = GD(vkCreateBuffer);
   PFN_vkGetBufferMemoryRequirements pBufReq = GD(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory pBindBuf = GD(vkBindBufferMemory);
   PFN_vkCreateCommandPool pCreatePool = GD(vkCreateCommandPool);
   PFN_vkAllocateCommandBuffers pAllocCB = GD(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer pBegin = GD(vkBeginCommandBuffer);
   PFN_vkCmdPipelineBarrier pBarrier = GD(vkCmdPipelineBarrier);
   PFN_vkCmdCopyBufferToImage pCopyBI = GD(vkCmdCopyBufferToImage);
   PFN_vkCmdBeginRenderPass pBeginRP = GD(vkCmdBeginRenderPass);
   PFN_vkCmdBindPipeline pBindPipe = GD(vkCmdBindPipeline);
   PFN_vkCmdBindDescriptorSets pBindDS = GD(vkCmdBindDescriptorSets);
   PFN_vkCmdBindVertexBuffers pBindVB = GD(vkCmdBindVertexBuffers);
   PFN_vkCmdBindIndexBuffer pBindIB = GD(vkCmdBindIndexBuffer);
   PFN_vkCmdPushConstants pPush = GD(vkCmdPushConstants);
   PFN_vkCmdDrawIndexed pDrawIndexed = GD(vkCmdDrawIndexed);
   PFN_vkCmdEndRenderPass pEndRP = GD(vkCmdEndRenderPass);
   PFN_vkCmdCopyImageToBuffer pCopyIB = GD(vkCmdCopyImageToBuffer);
   PFN_vkEndCommandBuffer pEnd = GD(vkEndCommandBuffer);
   PFN_vkQueueSubmit pSubmit = GD(vkQueueSubmit);
   PFN_vkQueueWaitIdle pWaitIdle = GD(vkQueueWaitIdle);
   if (!pGetQueue||!pAlloc||!pMap||!pCreateImg||!pBindImg||!pCreateView||!pCreateSamp||
       !pCreateRP||!pCreateFB||!pCreateSM||!pCreateDSL||!pCreateDP||!pAllocDS||!pUpdateDS||
       !pCreatePL||!pCreateGP||!pCreateBuf||!pBindBuf||!pCreatePool||!pAllocCB||!pBarrier||
       !pCopyBI||!pBeginRP||!pBindPipe||!pBindDS||!pBindVB||!pBindIB||!pPush||!pDrawIndexed||
       !pEndRP||!pCopyIB||!pSubmit||!pWaitIdle) { LOG("FAIL D: device fns"); goto done; }
   VkQueue queue; pGetQueue(dev, qfi, 0, &queue);
   VkPhysicalDeviceMemoryProperties mp; pMemP(phys[0], &mp);

#define ALLOC_IMG(image, memout) do { \
      VkMemoryRequirements _mr; pImgReq(dev, image, &_mr); \
      uint32_t _t = pick_mem_type(&mp, _mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); \
      if (_t == UINT32_MAX) _t = pick_mem_type(&mp, _mr.memoryTypeBits, 0); \
      VkMemoryAllocateInfo _ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, \
         .allocationSize = _mr.size, .memoryTypeIndex = _t }; \
      r = pAlloc(dev, &_ai, NULL, &(memout)); if (r) { LOG("FAIL: img alloc %d", r); goto done; } \
      r = pBindImg(dev, image, memout, 0); if (r) { LOG("FAIL: bindImg %d", r); goto done; } \
   } while (0)
#define MK_HOST_BUF(buffer, memout, mapout, sz, usg) do { \
      VkBufferCreateInfo _bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (sz), \
         .usage = (usg), .sharingMode = VK_SHARING_MODE_EXCLUSIVE }; \
      r = pCreateBuf(dev, &_bi, NULL, &(buffer)); if (r) { LOG("FAIL: buf %d", r); goto done; } \
      VkMemoryRequirements _mr; pBufReq(dev, buffer, &_mr); \
      uint32_t _t = pick_mem_type(&mp, _mr.memoryTypeBits, \
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); \
      VkMemoryAllocateInfo _ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, \
         .allocationSize = _mr.size, .memoryTypeIndex = _t }; \
      r = pAlloc(dev, &_ai, NULL, &(memout)); if (r) { LOG("FAIL: bufmem %d", r); goto done; } \
      r = pMap(dev, memout, 0, VK_WHOLE_SIZE, 0, &(mapout)); if (r||!(mapout)) { LOG("FAIL: map %d", r); goto done; } \
      r = pBindBuf(dev, buffer, memout, 0); if (r) { LOG("FAIL: bindbuf %d", r); goto done; } \
   } while (0)
#define IMG_BARRIER(c, img, levels, oldL, newL, srcA, dstA, srcS, dstS) do { \
      VkImageMemoryBarrier _b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = (srcA), \
         .dstAccessMask = (dstA), .oldLayout = (oldL), .newLayout = (newL), \
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, \
         .image = (img), .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (levels), 0, 1 } }; \
      pBarrier(c, (srcS), (dstS), 0, 0, NULL, 0, NULL, 1, &_b); \
   } while (0)

   /* ---- T: build the three textures + staging uploads ---- */

   /* texMip: 64x64, 3 mips. L0=red, L1=green, L2=blue (pixel bytes R,G,B,A). */
   VkImageCreateInfo mii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { MIP_W, MIP_H, 1 }, .mipLevels = MIP_LEVELS, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage texMip = VK_NULL_HANDLE; r = pCreateImg(dev, &mii, NULL, &texMip);
   if (r) { LOG("FAIL T: mip img %d", r); goto done; }
   VkDeviceMemory mmem = VK_NULL_HANDLE; ALLOC_IMG(texMip, mmem);
   /* staging holds all 3 levels contiguously: 64x64 + 32x32 + 16x16 (RGBA8). */
   const uint32_t L0 = MIP_W*MIP_H*4, L1 = (MIP_W/2)*(MIP_H/2)*4, L2 = (MIP_W/4)*(MIP_H/4)*4;
   VkBuffer mstg; VkDeviceMemory mstgm; void *msp;
   MK_HOST_BUF(mstg, mstgm, msp, L0+L1+L2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   { uint32_t *p = (uint32_t *)msp;
     for (uint32_t i = 0; i < L0/4; i++) p[i] = 0xFF0000FFu;            /* red   (R=255) */
     uint32_t *p1 = (uint32_t *)((uint8_t *)msp + L0);
     for (uint32_t i = 0; i < L1/4; i++) p1[i] = 0xFF00FF00u;           /* green (G=255) */
     uint32_t *p2 = (uint32_t *)((uint8_t *)msp + L0 + L1);
     for (uint32_t i = 0; i < L2/4; i++) p2[i] = 0xFFFF0000u;           /* blue  (B=255) */
     armDCacheFlush(msp, L0+L1+L2); }
   VkImageViewCreateInfo mvci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = texMip,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, MIP_LEVELS, 0, 1 } };
   VkImageView mview = VK_NULL_HANDLE; r = pCreateView(dev, &mvci, NULL, &mview);
   if (r) { LOG("FAIL T: mip view %d", r); goto done; }

   /* texSrgb: 8x8 R8G8B8A8_SRGB, solid grey 188 (0xBC). */
   VkImageCreateInfo sii = mii; sii.format = VK_FORMAT_R8G8B8A8_SRGB; sii.extent = (VkExtent3D){ 8, 8, 1 }; sii.mipLevels = 1;
   VkImage texSrgb = VK_NULL_HANDLE; r = pCreateImg(dev, &sii, NULL, &texSrgb);
   if (r) { LOG("FAIL T: srgb img %d", r); goto done; }
   VkDeviceMemory smem = VK_NULL_HANDLE; ALLOC_IMG(texSrgb, smem);
   VkBuffer sstg; VkDeviceMemory sstgm; void *ssp;
   MK_HOST_BUF(sstg, sstgm, ssp, 8*8*4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   { uint32_t *p = (uint32_t *)ssp; for (int i = 0; i < 64; i++) p[i] = 0xFFBCBCBCu; armDCacheFlush(ssp, 8*8*4); }
   VkImageViewCreateInfo svci = mvci; svci.image = texSrgb; svci.format = VK_FORMAT_R8G8B8A8_SRGB;
   svci.subresourceRange.levelCount = 1;
   VkImageView sview = VK_NULL_HANDLE; r = pCreateView(dev, &svci, NULL, &sview);
   if (r) { LOG("FAIL T: srgb view %d", r); goto done; }

   /* texBc1: 8x8 BC1_RGB_UNORM_BLOCK = 2x2 blocks of 8 bytes. Solid yellow:
    * c0 = RGB565 yellow 0xFFE0 (> c1=0) => 4-colour opaque mode, index 0 = c0. */
   VkImageCreateInfo bii = mii; bii.format = VK_FORMAT_BC1_RGB_UNORM_BLOCK; bii.extent = (VkExtent3D){ 8, 8, 1 }; bii.mipLevels = 1;
   VkImage texBc1 = VK_NULL_HANDLE; r = pCreateImg(dev, &bii, NULL, &texBc1);
   if (r) { LOG("FAIL T: bc1 img %d", r); goto done; }
   VkDeviceMemory bmem = VK_NULL_HANDLE; ALLOC_IMG(texBc1, bmem);
   VkBuffer bstg; VkDeviceMemory bstgm; void *bsp;
   MK_HOST_BUF(bstg, bstgm, bsp, 4*8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   { uint8_t blk[8] = { 0xE0,0xFF, 0x00,0x00, 0x00,0x00,0x00,0x00 };  /* c0=0xFFE0 yellow, c1=0, idx=0 */
     for (int b = 0; b < 4; b++) memcpy((uint8_t *)bsp + b*8, blk, 8);
     armDCacheFlush(bsp, 4*8); }
   VkImageViewCreateInfo bvci = mvci; bvci.image = texBc1; bvci.format = VK_FORMAT_BC1_RGB_UNORM_BLOCK;
   bvci.subresourceRange.levelCount = 1;
   VkImageView bview = VK_NULL_HANDLE; r = pCreateView(dev, &bvci, NULL, &bview);
   if (r) { LOG("FAIL T: bc1 view %d", r); goto done; }
   LOG("T textures created (mip 3-level + sRGB + BC1)");

   /* sampler: mipmapMode NEAREST so explicit LODs pick exact levels; clamp. */
   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .minLod = 0.0f, .maxLod = (float)MIP_LEVELS };
   VkSampler samp = VK_NULL_HANDLE; r = pCreateSamp(dev, &sci, NULL, &samp);
   if (r) { LOG("FAIL U: sampler %d", r); goto done; }

   /* U: unit-quad vbuf + ibuf. */
   VkBuffer vbuf; VkDeviceMemory vmem; void *vp;
   MK_HOST_BUF(vbuf, vmem, vp, sizeof QUAD_V, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   memcpy(vp, QUAD_V, sizeof QUAD_V); armDCacheFlush(vp, sizeof QUAD_V);
   VkBuffer ibuf; VkDeviceMemory imem; void *ip;
   MK_HOST_BUF(ibuf, imem, ip, sizeof QUAD_I, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   memcpy(ip, QUAD_I, sizeof QUAD_I); armDCacheFlush(ip, sizeof QUAD_I);

   /* descriptor set layout: binding0 = combined image sampler (frag). 3 sets. */
   VkDescriptorSetLayoutBinding bind0 = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
   VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &bind0 };
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE; r = pCreateDSL(dev, &dslci, NULL, &dsl);
   if (r) { LOG("FAIL U: dsl %d", r); goto done; }
   VkDescriptorPoolSize psz = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 3, .poolSizeCount = 1, .pPoolSizes = &psz };
   VkDescriptorPool dpool = VK_NULL_HANDLE; r = pCreateDP(dev, &dpci, NULL, &dpool);
   if (r) { LOG("FAIL U: dpool %d", r); goto done; }
   VkImageView views[3] = { mview, sview, bview };
   VkDescriptorSet dset[3];
   for (int i = 0; i < 3; i++) {
      VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
      r = pAllocDS(dev, &dsai, &dset[i]); if (r) { LOG("FAIL U: ds[%d] %d", i, r); goto done; }
      VkDescriptorImageInfo dii = { .sampler = samp, .imageView = views[i], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
      VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset[i],
         .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
      pUpdateDS(dev, 1, &wr, 0, NULL);
   }
   LOG("U sampler + quad + 3 descriptor sets ready");

   /* render target (color only). */
   VkImageCreateInfo rii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { SCN_W, SCN_H, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage rt = VK_NULL_HANDLE; r = pCreateImg(dev, &rii, NULL, &rt);
   if (r) { LOG("FAIL N: rt %d", r); goto done; }
   VkDeviceMemory rmem = VK_NULL_HANDLE; ALLOC_IMG(rt, rmem);
   VkImageViewCreateInfo rvci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rt,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView rview = VK_NULL_HANDLE; r = pCreateView(dev, &rvci, NULL, &rview);
   if (r) { LOG("FAIL N: rtview %d", r); goto done; }

   /* render pass: single color attachment. */
   VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
   VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1, .pColorAttachments = &cref };
   VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
   VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub,
      .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp = VK_NULL_HANDLE; r = pCreateRP(dev, &rpci, NULL, &rp);
   if (r) { LOG("FAIL N: renderpass %d", r); goto done; }
   VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = rp, .attachmentCount = 1, .pAttachments = &rview, .width = SCN_W, .height = SCN_H, .layers = 1 };
   VkFramebuffer fbuf = VK_NULL_HANDLE; r = pCreateFB(dev, &fbci, NULL, &fbuf);
   if (r) { LOG("FAIL N: framebuffer %d", r); goto done; }

   /* N: pipeline (push constant rect+lod, sampler, blend off, no cull). */
   VkShaderModuleCreateInfo vmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = tex_vert_spv_sz, .pCode = tex_vert_spv };
   VkShaderModuleCreateInfo fmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = tex_frag_spv_sz, .pCode = tex_frag_spv };
   VkShaderModule vsm, fsm;
   r = pCreateSM(dev, &vmci, NULL, &vsm); if (r) { LOG("FAIL N: vsm %d", r); goto done; }
   r = pCreateSM(dev, &fmci, NULL, &fsm); if (r) { LOG("FAIL N: fsm %d", r); goto done; }
   VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(struct PC) };
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
   VkPipelineLayout pl; r = pCreatePL(dev, &plci, NULL, &pl); if (r) { LOG("FAIL N: layout %d", r); goto done; }
   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
   VkVertexInputBindingDescription vib = { .binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription via[2] = {
      { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
      { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 8 } };
   VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib,
      .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = via };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp_ = { 0, 0, (float)SCN_W, (float)SCN_H, 0.0f, 1.0f };
   VkRect2D scs = { { 0, 0 }, { SCN_W, SCN_H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp_, .scissorCount = 1, .pScissors = &scs };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo msi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_FALSE, .colorWriteMask = 0xF };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &msi,
      .pColorBlendState = &cbs, .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe = VK_NULL_HANDLE; r = pCreateGP(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   LOG("N pipeline -> %d", r); if (r != VK_SUCCESS) goto done;

   /* L: readback buffer. */
   VkBuffer rbuf; VkDeviceMemory rbmem; void *cpu;
   MK_HOST_BUF(rbuf, rbmem, cpu, SCN_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   memset(cpu, 0, SCN_BYTES);

   /* O: record — upload all textures, draw the 5 quads, copy. */
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
   VkCommandPool pool; r = pCreatePool(dev, &pci, NULL, &pool); if (r) { LOG("FAIL: pool %d", r); goto done; }
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd; r = pAllocCB(dev, &cbai, &cmd); if (r) { LOG("FAIL: allocCB %d", r); goto done; }
   VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   r = pBegin(cmd, &cbbi); if (r) { LOG("FAIL: begin %d", r); goto done; }

   /* upload texMip (3 levels). */
   IMG_BARRIER(cmd, texMip, MIP_LEVELS, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   VkBufferImageCopy mcopy[MIP_LEVELS] = {
      { .bufferOffset = 0,     .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { MIP_W,    MIP_H,    1 } },
      { .bufferOffset = L0,    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1 }, .imageExtent = { MIP_W/2,  MIP_H/2,  1 } },
      { .bufferOffset = L0+L1, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 2, 0, 1 }, .imageExtent = { MIP_W/4,  MIP_H/4,  1 } },
   };
   pCopyBI(cmd, mstg, texMip, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MIP_LEVELS, mcopy);
   IMG_BARRIER(cmd, texMip, MIP_LEVELS, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
   /* upload texSrgb. */
   IMG_BARRIER(cmd, texSrgb, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   VkBufferImageCopy scopy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 8, 8, 1 } };
   pCopyBI(cmd, sstg, texSrgb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &scopy);
   IMG_BARRIER(cmd, texSrgb, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
   /* upload texBc1. */
   IMG_BARRIER(cmd, texBc1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   VkBufferImageCopy bcopy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 8, 8, 1 } };
   pCopyBI(cmd, bstg, texBc1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bcopy);
   IMG_BARRIER(cmd, texBc1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

   VkClearValue clear; clear.color = (VkClearColorValue){ .float32 = { 0.05f, 0.05f, 0.07f, 1.0f } };
   VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fbuf,
      .renderArea = { { 0, 0 }, { SCN_W, SCN_H } }, .clearValueCount = 1, .pClearValues = &clear };
   pBeginRP(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   pBindPipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkDeviceSize voff = 0; pBindVB(cmd, 0, 1, &vbuf, &voff);
   pBindIB(cmd, ibuf, 0, VK_INDEX_TYPE_UINT16);
#define DRAW_QUAD(ds, cx, cy, hw, hh, lod) do { \
      struct PC _pc = { { (cx),(cy),(hw),(hh) }, { (float)(lod), 0,0,0 } }; \
      pBindDS(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &(ds), 0, NULL); \
      pPush(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct PC), &_pc); \
      pDrawIndexed(cmd, 6, 1, 0, 0, 0); \
   } while (0)
   /* left column: the SAME mip texture sampled at LOD 0 / 1 / 2 -> red / green / blue. */
   DRAW_QUAD(dset[0], -0.60f, -0.58f, 0.28f, 0.28f, 0);
   DRAW_QUAD(dset[0], -0.60f,  0.00f, 0.28f, 0.28f, 1);
   DRAW_QUAD(dset[0], -0.60f,  0.58f, 0.28f, 0.28f, 2);
   /* centre: sRGB grey -> linearised ~128. right: BC1 yellow. */
   DRAW_QUAD(dset[1],  0.05f,  0.00f, 0.30f, 0.30f, 0);
   DRAW_QUAD(dset[2],  0.65f,  0.00f, 0.30f, 0.30f, 0);
   pEndRP(cmd);
   VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { SCN_W, SCN_H, 1 } };
   pCopyIB(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);
   r = pEnd(cmd); LOG("O recorded (5 textured quads); end -> %d", r); if (r) goto done;

   /* L: submit, read back, verify each region. */
   {
      uint32_t *px = (uint32_t *)cpu;
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
      r = pSubmit(queue, 1, &si, VK_NULL_HANDLE);
      if (r != VK_SUCCESS) { LOG("FAIL L: submit %d", r); goto done; }
      pWaitIdle(queue);
      armDCacheFlush(cpu, SCN_BYTES);
      memcpy(g_shot, cpu, SCN_BYTES);

      uint32_t c_l0 = sample_ndc(px, -0.60f, -0.58f);  /* mip LOD0 -> red */
      uint32_t c_l1 = sample_ndc(px, -0.60f,  0.00f);  /* mip LOD1 -> green */
      uint32_t c_l2 = sample_ndc(px, -0.60f,  0.58f);  /* mip LOD2 -> blue */
      uint32_t c_sr = sample_ndc(px,  0.05f,  0.00f);  /* sRGB grey -> ~128 */
      uint32_t c_bc = sample_ndc(px,  0.65f,  0.00f);  /* BC1 -> yellow */
      LOG("L mip LOD0 = 0x%06x (want red)",    c_l0 & 0xFFFFFF);
      LOG("L mip LOD1 = 0x%06x (want green)",  c_l1 & 0xFFFFFF);
      LOG("L mip LOD2 = 0x%06x (want blue)",   c_l2 & 0xFFFFFF);
      LOG("L sRGB     = 0x%06x (want ~grey 128, NOT 188)", c_sr & 0xFFFFFF);
      LOG("L BC1      = 0x%06x (want yellow)", c_bc & 0xFFFFFF);

      int ok_mip = rgb_near(c_l0, 255,0,0, 16) && rgb_near(c_l1, 0,255,0, 16) && rgb_near(c_l2, 0,0,255, 16);
      int ok_srgb = rgb_near(c_sr, 128,128,128, 16);   /* linearised, NOT 188 */
      int ok_bc1 = rgb_near(c_bc, 255,255,0, 24);
      LOG("L: mips=%d srgb=%d bc1=%d => %s", ok_mip, ok_srgb, ok_bc1,
          (ok_mip && ok_srgb && ok_bc1) ? "OK" : "FAIL (or Eden fake GPU, verify on real HW)");
      if (ok_mip && ok_srgb && ok_bc1)
         LOG("=== TEXTURES PASSED on Tegra (mipmaps + sRGB decode + BC1 decompress) ===");
      else
         LOG("=== textures readback off (expected on Eden; verify by the TV on real HW) ===");
   }

   /* P: present the frame on the TV (static), until +. */
   {
      PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
      NWindow *win = nwindowGetDefault();
      Framebuffer fb; framebufferCreate(&fb, win, SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 2); framebufferMakeLinear(&fb);
      LOG("P: presenting textures frame; press + to exit");
      while (appletMainLoop()) {
         padUpdate(&pad);
         if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; }
            svcExitProcess();
         }
         u32 stride; u8 *base = (u8 *)framebufferBegin(&fb, &stride);
         for (u32 yy = 0; yy < SCR_H; yy++) {
            uint32_t *row = (uint32_t *)(base + (size_t)yy * stride);
            u32 sy = yy * SCN_H / SCR_H;
            for (u32 xx = 0; xx < SCR_W; xx++) row[xx] = g_shot[sy * SCN_W + (xx * SCN_W / SCR_W)];
         }
         framebufferEnd(&fb);
      }
      framebufferClose(&fb);
   }

done:
   (void)inst;
   LOG("=== done; exiting to HOME (svcExitProcess) ===");
   g_drm_shim_log_sink = NULL;
   if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; }
   svcExitProcess();
   return 0;  /* unreachable */
}
