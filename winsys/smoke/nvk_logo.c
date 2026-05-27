/*
 * nvk_logo.c — M3 step 2: NVK samples a TEXTURE and shows the "VULKAN" logo on
 * the Switch TV. Builds on the passed triangle (1b). The genuinely new parts:
 * a sampled texture (upload via staging + vkCmdCopyBufferToImage), a sampler, a
 * descriptor set, and a textured fragment shader — the same infra the Sascha
 * Willems demo scene needs. The logo bitmap is generated on the CPU (red field +
 * white "VULKAN" block letters), uploaded, and sampled onto a fullscreen quad —
 * so Vulkan does the rendering ("depends on Vulkan to run").
 *
 *   A..D  instance / device / queue
 *   T  texture image (logo) + staging upload + sampler + descriptor set
 *   N  quad pipeline (quad.vert/quad.frag, sampler binding 0)
 *   O  render pass: draw the textured fullscreen quad
 *   L/M  copy render target -> buffer, read back (verify red+white present)
 *   P  present on the TV (upscaled), until +
 *
 * Loaderless ICD (VK_NO_PROTOTYPES); logs sdmc:/nvk_logo.log.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "shaders/tri_shaders.h"   /* quad_vert_spv[], quad_frag_spv[] + _sz */

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
extern void (*g_drm_shim_log_sink)(const char *);

#define LOGO_W 320u
#define LOGO_H 144u
#define PIXELS (LOGO_W * LOGO_H)
#define IMG_BYTES (PIXELS * 4u)
#define SCR_W 1280u
#define SCR_H 720u
#define COL_RED   0xFF1E14C8u   /* R8G8B8A8 LE: R=0xC8 G=0x14 B=0x1E A=0xFF (Vulkan-ish red) */
#define COL_WHITE 0xFFFFFFFFu

static FILE *g_log;
static uint32_t g_shot[PIXELS];   /* CPU copy of the rendered image, for present */
static uint32_t g_logo[PIXELS];   /* CPU-generated logo bitmap, uploaded as a texture */

/* My own simple 8x8 block glyphs (MSB = leftmost column) for V U L K A N. */
static const uint8_t GLYPHS[6][8] = {
   { 0x82,0x82,0x82,0x82,0x44,0x44,0x28,0x10 }, /* V */
   { 0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x7C }, /* U */
   { 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0xFE }, /* L */
   { 0x84,0x88,0x90,0xE0,0x90,0x88,0x84,0x82 }, /* K */
   { 0x10,0x28,0x44,0x82,0xFE,0x82,0x82,0x82 }, /* A */
   { 0x82,0xC2,0xA2,0x92,0x8A,0x86,0x82,0x82 }, /* N */
};

static void put_block(uint32_t *img, int x0, int y0, int s, uint32_t c)
{
   for (int dy = 0; dy < s; dy++)
      for (int dx = 0; dx < s; dx++) {
         int x = x0 + dx, y = y0 + dy;
         if (x >= 0 && x < (int)LOGO_W && y >= 0 && y < (int)LOGO_H)
            img[y * LOGO_W + x] = c;
      }
}

static void gen_logo(void)
{
   for (uint32_t i = 0; i < PIXELS; i++) g_logo[i] = COL_RED;
   const int s = 5;                 /* per-glyph-pixel block size */
   const int gw = 8 * s, gap = 2 * s;
   const int total = 6 * gw + 5 * gap;
   int x = ((int)LOGO_W - total) / 2;
   int y = ((int)LOGO_H - gw) / 2;
   for (int g = 0; g < 6; g++) {
      for (int row = 0; row < 8; row++)
         for (int col = 0; col < 8; col++)
            if (GLYPHS[g][row] & (0x80u >> col))
               put_block(g_logo, x + col * s, y + row * s, s, COL_WHITE);
      x += gw + gap;
   }
}

static void shim_log_sink(const char *s)
{ if (g_log) { fputs(s, g_log); fflush(g_log); } printf("%s", s); fflush(stdout); }
static void llogf(const char *fmt, ...)
{
   char buf[512]; va_list ap; va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
   if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
   printf("%s\n", buf); fflush(stdout);
}
#define LOG(...) llogf(__VA_ARGS__)

static uint32_t pick_mem_type(const VkPhysicalDeviceMemoryProperties *mp,
                              uint32_t type_bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
      if ((type_bits & (1u << i)) &&
          (mp->memoryTypes[i].propertyFlags & want) == want)
         return i;
   return UINT32_MAX;
}

static void present_shot(void)
{
   PadState pad;
   padConfigureInput(1, HidNpadStyleSet_NpadStandard);
   padInitializeDefault(&pad);
   NWindow *win = nwindowGetDefault();
   Framebuffer fb;
   framebufferCreate(&fb, win, SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 2);
   framebufferMakeLinear(&fb);
   LOG("P: presenting on screen (%ux%u -> %ux%u); press + to exit",
       LOGO_W, LOGO_H, SCR_W, SCR_H);
   while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
      u32 stride;
      u8 *base = (u8 *)framebufferBegin(&fb, &stride);
      for (u32 yy = 0; yy < SCR_H; yy++) {
         uint32_t *row = (uint32_t *)(base + (size_t)yy * stride);
         u32 sy = yy * LOGO_H / SCR_H;
         for (u32 xx = 0; xx < SCR_W; xx++)
            row[xx] = g_shot[sy * LOGO_W + (xx * LOGO_W / SCR_W)];
      }
      framebufferEnd(&fb);
   }
   framebufferClose(&fb);
}

int main(void)
{
   int have_shot = 0;
   g_log = fopen("sdmc:/nvk_logo.log", "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
      nxlinkStdio();
   LOG("=== NVK logo test (M3 2: textured VULKAN logo) [BUILD logo1] ===");
   gen_logo();

   g_drm_shim_log_sink = shim_log_sink;
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
   setenv("MESA_LOG_FILE", "sdmc:/nvk_logo_mesa.log", 1);

   VkInstance inst = VK_NULL_HANDLE;
   PFN_vkCreateInstance pCreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("FAIL: no vkCreateInstance"); goto done; }
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_logo", .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app };
   VkResult r = pCreateInstance(&ici, NULL, &inst);
   LOG("A vkCreateInstance -> %d", r);
   if (r != VK_SUCCESS) goto done;

#define GI(n) ((PFN_##n)vk_icdGetInstanceProcAddr(inst, #n))
   PFN_vkEnumeratePhysicalDevices          pEnum  = GI(vkEnumeratePhysicalDevices);
   PFN_vkGetPhysicalDeviceMemoryProperties pMemP  = GI(vkGetPhysicalDeviceMemoryProperties);
   PFN_vkGetPhysicalDeviceQueueFamilyProperties pQF = GI(vkGetPhysicalDeviceQueueFamilyProperties);
   PFN_vkCreateDevice                      pCreateDev = GI(vkCreateDevice);
   PFN_vkGetDeviceProcAddr                 pGDPA  = GI(vkGetDeviceProcAddr);
   if (!pEnum || !pCreateDev || !pGDPA) { LOG("FAIL: missing instance fns"); goto done; }

   uint32_t ndev = 1; VkPhysicalDevice phys[4];
   r = pEnum(inst, &ndev, NULL);
   if (r != VK_SUCCESS || ndev == 0) { LOG("FAIL B: no devices (r=%d n=%u)", r, ndev); goto done; }
   ndev = (ndev > 4) ? 4 : ndev;
   r = pEnum(inst, &ndev, phys);
   if (r != VK_SUCCESS) { LOG("FAIL B: enum2 r=%d", r); goto done; }
   LOG("B enumerate -> %u device(s)", ndev);

   uint32_t nqf = 0; pQF(phys[0], &nqf, NULL);
   VkQueueFamilyProperties qf[8]; nqf = (nqf > 8) ? 8 : nqf;
   pQF(phys[0], &nqf, qf);
   uint32_t qfi = UINT32_MAX;
   for (uint32_t i = 0; i < nqf; i++)
      if (qfi == UINT32_MAX && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi = i;
   if (qfi == UINT32_MAX) { LOG("FAIL C: no graphics queue"); goto done; }
   LOG("C graphics queue family %u", qfi);

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
   PFN_vkGetDeviceQueue              pGetQueue = GD(vkGetDeviceQueue);
   PFN_vkAllocateMemory              pAlloc    = GD(vkAllocateMemory);
   PFN_vkMapMemory                   pMap      = GD(vkMapMemory);
   PFN_vkCreateImage                 pCreateImg= GD(vkCreateImage);
   PFN_vkGetImageMemoryRequirements  pImgReq   = GD(vkGetImageMemoryRequirements);
   PFN_vkBindImageMemory             pBindImg  = GD(vkBindImageMemory);
   PFN_vkCreateImageView             pCreateView = GD(vkCreateImageView);
   PFN_vkCreateSampler               pCreateSamp = GD(vkCreateSampler);
   PFN_vkCreateRenderPass            pCreateRP = GD(vkCreateRenderPass);
   PFN_vkCreateFramebuffer           pCreateFB = GD(vkCreateFramebuffer);
   PFN_vkCreateShaderModule          pCreateSM = GD(vkCreateShaderModule);
   PFN_vkCreateDescriptorSetLayout   pCreateDSL = GD(vkCreateDescriptorSetLayout);
   PFN_vkCreateDescriptorPool        pCreateDP = GD(vkCreateDescriptorPool);
   PFN_vkAllocateDescriptorSets      pAllocDS  = GD(vkAllocateDescriptorSets);
   PFN_vkUpdateDescriptorSets        pUpdateDS = GD(vkUpdateDescriptorSets);
   PFN_vkCreatePipelineLayout        pCreatePL = GD(vkCreatePipelineLayout);
   PFN_vkCreateGraphicsPipelines     pCreateGP = GD(vkCreateGraphicsPipelines);
   PFN_vkCreateBuffer                pCreateBuf= GD(vkCreateBuffer);
   PFN_vkGetBufferMemoryRequirements pBufReq   = GD(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory            pBindBuf  = GD(vkBindBufferMemory);
   PFN_vkCreateCommandPool           pCreatePool = GD(vkCreateCommandPool);
   PFN_vkAllocateCommandBuffers      pAllocCB  = GD(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer          pBegin    = GD(vkBeginCommandBuffer);
   PFN_vkCmdPipelineBarrier          pBarrier  = GD(vkCmdPipelineBarrier);
   PFN_vkCmdCopyBufferToImage        pCopyBI   = GD(vkCmdCopyBufferToImage);
   PFN_vkCmdBeginRenderPass          pBeginRP  = GD(vkCmdBeginRenderPass);
   PFN_vkCmdBindPipeline             pBindPipe = GD(vkCmdBindPipeline);
   PFN_vkCmdBindDescriptorSets       pBindDS   = GD(vkCmdBindDescriptorSets);
   PFN_vkCmdDraw                     pDraw     = GD(vkCmdDraw);
   PFN_vkCmdEndRenderPass            pEndRP    = GD(vkCmdEndRenderPass);
   PFN_vkCmdCopyImageToBuffer        pCopyIB   = GD(vkCmdCopyImageToBuffer);
   PFN_vkEndCommandBuffer            pEnd      = GD(vkEndCommandBuffer);
   PFN_vkQueueSubmit                 pSubmit   = GD(vkQueueSubmit);
   PFN_vkQueueWaitIdle               pWaitIdle = GD(vkQueueWaitIdle);
   if (!pGetQueue || !pAlloc || !pMap || !pCreateImg || !pBindImg || !pCreateView ||
       !pCreateSamp || !pCreateRP || !pCreateFB || !pCreateSM || !pCreateDSL ||
       !pCreateDP || !pAllocDS || !pUpdateDS || !pCreatePL || !pCreateGP ||
       !pCreateBuf || !pBindBuf || !pCreatePool || !pAllocCB || !pBarrier ||
       !pCopyBI || !pBeginRP || !pBindPipe || !pBindDS || !pDraw || !pEndRP ||
       !pCopyIB || !pSubmit || !pWaitIdle) { LOG("FAIL D: missing device fns"); goto done; }
   VkQueue queue; pGetQueue(dev, qfi, 0, &queue);
   VkPhysicalDeviceMemoryProperties mp; pMemP(phys[0], &mp);

   /* helper to alloc+bind image memory */
#define ALLOC_IMG(image, memout, want) do { \
      VkMemoryRequirements _mr; pImgReq(dev, image, &_mr); \
      uint32_t _t = pick_mem_type(&mp, _mr.memoryTypeBits, want); \
      if (_t == UINT32_MAX) _t = pick_mem_type(&mp, _mr.memoryTypeBits, 0); \
      VkMemoryAllocateInfo _ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, \
         .allocationSize = _mr.size, .memoryTypeIndex = _t }; \
      r = pAlloc(dev, &_ai, NULL, &(memout)); if (r) { LOG("FAIL: img alloc %d", r); goto done; } \
      r = pBindImg(dev, image, memout, 0); if (r) { LOG("FAIL: bindImg %d", r); goto done; } \
   } while (0)

   /* T: the texture image (logo) + a host-visible staging buffer with the bitmap. */
   VkImageCreateInfo tii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { LOGO_W, LOGO_H, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage tex = VK_NULL_HANDLE;
   r = pCreateImg(dev, &tii, NULL, &tex);
   LOG("T vkCreateImage(texture) -> %d", r);
   if (r != VK_SUCCESS) goto done;
   VkDeviceMemory tmem = VK_NULL_HANDLE; ALLOC_IMG(tex, tmem, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

   VkBufferCreateInfo sbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = IMG_BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
   VkBuffer staging = VK_NULL_HANDLE;
   r = pCreateBuf(dev, &sbci, NULL, &staging);
   if (r) { LOG("FAIL T: staging %d", r); goto done; }
   VkMemoryRequirements smr; pBufReq(dev, staging, &smr);
   uint32_t smt = pick_mem_type(&mp, smr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkMemoryAllocateInfo smai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = smr.size, .memoryTypeIndex = smt };
   VkDeviceMemory smem = VK_NULL_HANDLE;
   r = pAlloc(dev, &smai, NULL, &smem);
   if (r) { LOG("FAIL T: staging mem %d", r); goto done; }
   void *sp = NULL;
   r = pMap(dev, smem, 0, VK_WHOLE_SIZE, 0, &sp);
   if (r || !sp) { LOG("FAIL T: staging map %d", r); goto done; }
   memcpy(sp, g_logo, IMG_BYTES);
   armDCacheFlush(sp, IMG_BYTES);     /* push the CPU-written logo out for the GPU */
   r = pBindBuf(dev, staging, smem, 0);
   if (r) { LOG("FAIL T: staging bind %d", r); goto done; }

   VkImageViewCreateInfo tvci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = tex, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView tview = VK_NULL_HANDLE;
   r = pCreateView(dev, &tvci, NULL, &tview);
   if (r) { LOG("FAIL T: texview %d", r); goto done; }

   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 1.0f };
   VkSampler samp = VK_NULL_HANDLE;
   r = pCreateSamp(dev, &sci, NULL, &samp);
   LOG("T vkCreateSampler -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* descriptor set layout (binding 0 = combined image sampler, fragment). */
   VkDescriptorSetLayoutBinding dslb = { .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
   VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &dslb };
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   r = pCreateDSL(dev, &dslci, NULL, &dsl);
   if (r) { LOG("FAIL T: dsl %d", r); goto done; }
   VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
   VkDescriptorPool dpool = VK_NULL_HANDLE;
   r = pCreateDP(dev, &dpci, NULL, &dpool);
   if (r) { LOG("FAIL T: dpool %d", r); goto done; }
   VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet dset = VK_NULL_HANDLE;
   r = pAllocDS(dev, &dsai, &dset);
   LOG("T allocate descriptor set -> %d", r);
   if (r != VK_SUCCESS) goto done;
   VkDescriptorImageInfo dii = { .sampler = samp, .imageView = tview,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
   VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
   pUpdateDS(dev, 1, &wds, 0, NULL);

   /* render target image + view. */
   VkImageCreateInfo rii = tii;
   rii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage rt = VK_NULL_HANDLE;
   r = pCreateImg(dev, &rii, NULL, &rt);
   if (r) { LOG("FAIL N: rt image %d", r); goto done; }
   VkDeviceMemory rmem = VK_NULL_HANDLE; ALLOC_IMG(rt, rmem, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VkImageViewCreateInfo rvci = tvci; rvci.image = rt;
   VkImageView rview = VK_NULL_HANDLE;
   r = pCreateView(dev, &rvci, NULL, &rview);
   if (r) { LOG("FAIL N: rt view %d", r); goto done; }

   /* render pass: clear -> store, final = TRANSFER_SRC. */
   VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
   VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1, .pColorAttachments = &cref };
   VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
   VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub,
      .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp = VK_NULL_HANDLE;
   r = pCreateRP(dev, &rpci, NULL, &rp);
   if (r) { LOG("FAIL N: renderpass %d", r); goto done; }
   VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = rp, .attachmentCount = 1, .pAttachments = &rview,
      .width = LOGO_W, .height = LOGO_H, .layers = 1 };
   VkFramebuffer fbuf = VK_NULL_HANDLE;
   r = pCreateFB(dev, &fbci, NULL, &fbuf);
   if (r) { LOG("FAIL N: framebuffer %d", r); goto done; }

   /* N: quad pipeline (samples the texture). */
   VkShaderModuleCreateInfo vmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = quad_vert_spv_sz, .pCode = quad_vert_spv };
   VkShaderModuleCreateInfo fmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = quad_frag_spv_sz, .pCode = quad_frag_spv };
   VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
   r = pCreateSM(dev, &vmci, NULL, &vsm); if (r) { LOG("FAIL N: vsm %d", r); goto done; }
   r = pCreateSM(dev, &fmci, NULL, &fsm); if (r) { LOG("FAIL N: fsm %d", r); goto done; }

   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &dsl };
   VkPipelineLayout pl = VK_NULL_HANDLE;
   r = pCreatePL(dev, &plci, NULL, &pl); if (r) { LOG("FAIL N: layout %d", r); goto done; }

   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
   VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, (float)LOGO_W, (float)LOGO_H, 0.0f, 1.0f };
   VkRect2D scs = { { 0, 0 }, { LOGO_W, LOGO_H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &scs };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo msi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_FALSE,
      .colorWriteMask = 0xF };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &msi,
      .pColorBlendState = &cbs, .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe = VK_NULL_HANDLE;
   r = pCreateGP(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   LOG("N vkCreateGraphicsPipelines -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* L: readback buffer. */
   VkBufferCreateInfo rbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = IMG_BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
   VkBuffer rbuf = VK_NULL_HANDLE;
   r = pCreateBuf(dev, &rbci, NULL, &rbuf); if (r) { LOG("FAIL L: rbuf %d", r); goto done; }
   VkMemoryRequirements rbmr; pBufReq(dev, rbuf, &rbmr);
   uint32_t rbmt = pick_mem_type(&mp, rbmr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkMemoryAllocateInfo rbmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = rbmr.size, .memoryTypeIndex = rbmt };
   VkDeviceMemory rbmem = VK_NULL_HANDLE;
   r = pAlloc(dev, &rbmai, NULL, &rbmem); if (r) { LOG("FAIL L: rbmem %d", r); goto done; }
   void *cpu = NULL; r = pMap(dev, rbmem, 0, VK_WHOLE_SIZE, 0, &cpu);
   if (r || !cpu) { LOG("FAIL L: rb map %d", r); goto done; }
   memset(cpu, 0, IMG_BYTES);
   r = pBindBuf(dev, rbuf, rbmem, 0); if (r) { LOG("FAIL L: rb bind %d", r); goto done; }

   /* record: upload texture, draw textured quad, copy RT -> readback. */
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = qfi };
   VkCommandPool pool; r = pCreatePool(dev, &pci, NULL, &pool); if (r) { LOG("FAIL: pool %d", r); goto done; }
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd; r = pAllocCB(dev, &cbai, &cmd); if (r) { LOG("FAIL: allocCB %d", r); goto done; }
   VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   r = pBegin(cmd, &cbbi); if (r) { LOG("FAIL: begin %d", r); goto done; }

   VkImageSubresourceRange crange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier toDst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = tex, .subresourceRange = crange };
   pBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &toDst);
   VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { LOGO_W, LOGO_H, 1 } };
   pCopyBI(cmd, staging, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
   VkImageMemoryBarrier toRead = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = tex, .subresourceRange = crange };
   pBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &toRead);

   VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
   VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = rp, .framebuffer = fbuf, .renderArea = { { 0, 0 }, { LOGO_W, LOGO_H } },
      .clearValueCount = 1, .pClearValues = &clear };
   pBeginRP(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   pBindPipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   pBindDS(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
   pDraw(cmd, 6, 1, 0, 0);
   pEndRP(cmd);

   VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { LOGO_W, LOGO_H, 1 } };
   pCopyIB(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);

   r = pEnd(cmd);
   LOG("O recorded (upload+textured quad+copy); end -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cmd };
   r = pSubmit(queue, 1, &si, VK_NULL_HANDLE);
   LOG("vkQueueSubmit -> %d", r);
   if (r != VK_SUCCESS) goto done;
   r = pWaitIdle(queue);
   LOG("vkQueueWaitIdle -> %d", r);
   if (r != VK_SUCCESS) goto done;

   armDCacheFlush(cpu, IMG_BYTES);
   uint32_t *px = (uint32_t *)cpu;
   uint32_t red = 0, white = 0, black = 0, other = 0;
   for (uint32_t i = 0; i < PIXELS; i++) {
      if (px[i] == COL_RED) red++;
      else if (px[i] == COL_WHITE) white++;
      else if (px[i] == 0xFF000000u) black++;
      else other++;
   }
   LOG("M pixels: red=%u white=%u black=%u other=%u (center=0x%08x)",
       red, white, black, other, px[(LOGO_H/2)*LOGO_W + LOGO_W/2]);
   if (red > 1000 && white > 200)
      LOG("=== M3 STEP 2 PASSED — NVK textured the VULKAN logo on Tegra ===");
   else
      LOG("=== M3 STEP 2: unexpected pixel mix (texture/sampler may not have run) ===");

   memcpy(g_shot, px, IMG_BYTES);
   have_shot = 1;

   /* Present the logo BEFORE tearing down Vulkan, so the display (vi/nvnflinger
    * over nv) is brought up AND closed INSIDE the services-up window — the same
    * lifecycle as the headless smoke, which exits cleanly. Bringing the
    * framebuffer up/down AFTER vkDestroyInstance (NVK had already used+released
    * nv) is what crashed the libnx service teardown on exit / pressing +. */
   if (have_shot) present_shot();

done:
   if (inst != VK_NULL_HANDLE) {
      PFN_vkDestroyInstance pDestroyInstance =
         (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(inst, "vkDestroyInstance");
      if (pDestroyInstance) pDestroyInstance(inst, NULL);
      LOG("cleanup: vkDestroyInstance done");
   }
   LOG("=== done; returning to Sphaira ===");
   g_drm_shim_log_sink = NULL;
   if (g_log) { fclose(g_log); g_log = NULL; }
   return 0;   /* normal exit, exactly like the clean headless smoke */
}
