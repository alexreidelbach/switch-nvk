/*
 * nvk_tri.c — M3 step 1b: NVK draws a real TRIANGLE on the Switch GM20B and
 * shows it on the TV. Builds on the PASSED clear+present (step 1a).
 *
 *   A..D  instance / device / queue
 *   J  optimal RGBA8 color image (render target) + view + framebuffer
 *   N  graphics pipeline: hardcoded-triangle vertex shader + solid-yellow
 *      fragment shader (NAK-compiled SPIR-V, embedded from tri_shaders.h)
 *   O  render pass: clear BLACK, vkCmdDraw(3) the triangle, end
 *   L  copy image -> host-visible buffer (de-tile), read back
 *   M  verify: a chunk of YELLOW pixels exist (the triangle was rasterised)
 *   P  present the rendered image on the TV (upscaled), until +
 *
 * The genuinely new part vs M2/1a: the GRAPHICS pipeline + NAK shaders executing
 * on the GM20B. Loaderless ICD (VK_NO_PROTOTYPES); logs sdmc:/nvk_tri.log.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "shaders/tri_shaders.h"   /* tri_vert_spv[], tri_frag_spv[] + _sz */

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
extern void (*g_drm_shim_log_sink)(const char *);

#define IMG_W 64u
#define IMG_H 64u
#define PIXELS (IMG_W * IMG_H)
#define IMG_BYTES (PIXELS * 4u)
#define SCR_W 1280u
#define SCR_H 720u
#define YELLOW_U32 0xFF00FFFFu   /* R8G8B8A8 bytes FF FF 00 FF (yellow, opaque) */
#define BLACK_U32  0xFF000000u

static FILE *g_log;
static uint32_t g_shot[PIXELS];

static void shim_log_sink(const char *s)
{ if (g_log) { fputs(s, g_log); fflush(g_log); } printf("%s", s); fflush(stdout); }
static void trilogf(const char *fmt, ...)
{
   char buf[512]; va_list ap; va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
   if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
   printf("%s\n", buf); fflush(stdout);
}
#define LOG(...) trilogf(__VA_ARGS__)

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
   LOG("P: presenting on screen (upscaled %ux%u -> %ux%u); press + to exit",
       IMG_W, IMG_H, SCR_W, SCR_H);
   while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
      u32 stride;
      u8 *base = (u8 *)framebufferBegin(&fb, &stride);
      for (u32 y = 0; y < SCR_H; y++) {
         uint32_t *row = (uint32_t *)(base + (size_t)y * stride);
         u32 sy = y * IMG_H / SCR_H;
         for (u32 x = 0; x < SCR_W; x++)
            row[x] = g_shot[sy * IMG_W + (x * IMG_W / SCR_W)];
      }
      framebufferEnd(&fb);
   }
   framebufferClose(&fb);
}

int main(void)
{
   int have_shot = 0;
   g_log = fopen("sdmc:/nvk_tri.log", "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
      nxlinkStdio();
   LOG("=== NVK triangle test (M3 1b: draw + present) [BUILD t1b] ===");

   g_drm_shim_log_sink = shim_log_sink;
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
   setenv("MESA_LOG_FILE", "sdmc:/nvk_tri_mesa.log", 1);

   VkInstance inst = VK_NULL_HANDLE;
   PFN_vkCreateInstance pCreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("FAIL: no vkCreateInstance"); goto done; }
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_tri", .apiVersion = VK_API_VERSION_1_1 };
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
   PFN_vkCreateRenderPass            pCreateRP = GD(vkCreateRenderPass);
   PFN_vkCreateFramebuffer           pCreateFB = GD(vkCreateFramebuffer);
   PFN_vkCreateShaderModule          pCreateSM = GD(vkCreateShaderModule);
   PFN_vkCreatePipelineLayout        pCreatePL = GD(vkCreatePipelineLayout);
   PFN_vkCreateGraphicsPipelines     pCreateGP = GD(vkCreateGraphicsPipelines);
   PFN_vkCreateBuffer                pCreateBuf= GD(vkCreateBuffer);
   PFN_vkGetBufferMemoryRequirements pBufReq   = GD(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory            pBindBuf  = GD(vkBindBufferMemory);
   PFN_vkCreateCommandPool           pCreatePool = GD(vkCreateCommandPool);
   PFN_vkAllocateCommandBuffers      pAllocCB  = GD(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer          pBegin    = GD(vkBeginCommandBuffer);
   PFN_vkCmdBeginRenderPass          pBeginRP  = GD(vkCmdBeginRenderPass);
   PFN_vkCmdBindPipeline             pBindPipe = GD(vkCmdBindPipeline);
   PFN_vkCmdDraw                     pDraw     = GD(vkCmdDraw);
   PFN_vkCmdEndRenderPass            pEndRP    = GD(vkCmdEndRenderPass);
   PFN_vkCmdCopyImageToBuffer        pCopy     = GD(vkCmdCopyImageToBuffer);
   PFN_vkEndCommandBuffer            pEnd      = GD(vkEndCommandBuffer);
   PFN_vkQueueSubmit                 pSubmit   = GD(vkQueueSubmit);
   PFN_vkQueueWaitIdle               pWaitIdle = GD(vkQueueWaitIdle);
   if (!pGetQueue || !pAlloc || !pMap || !pCreateImg || !pImgReq || !pBindImg ||
       !pCreateView || !pCreateRP || !pCreateFB || !pCreateSM || !pCreatePL ||
       !pCreateGP || !pCreateBuf || !pBindBuf || !pCreatePool || !pAllocCB ||
       !pBeginRP || !pBindPipe || !pDraw || !pEndRP || !pCopy || !pSubmit || !pWaitIdle) {
      LOG("FAIL D: missing device fns"); goto done;
   }
   VkQueue queue; pGetQueue(dev, qfi, 0, &queue);
   VkPhysicalDeviceMemoryProperties mp; pMemP(phys[0], &mp);

   /* J: color image (render target) + memory + view. */
   VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { IMG_W, IMG_H, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage img = VK_NULL_HANDLE;
   r = pCreateImg(dev, &ii, NULL, &img);
   LOG("J vkCreateImage -> %d", r);
   if (r != VK_SUCCESS) goto done;
   VkMemoryRequirements imr; pImgReq(dev, img, &imr);
   uint32_t imt = pick_mem_type(&mp, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (imt == UINT32_MAX) imt = pick_mem_type(&mp, imr.memoryTypeBits, 0);
   VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = imr.size, .memoryTypeIndex = imt };
   VkDeviceMemory imem = VK_NULL_HANDLE;
   r = pAlloc(dev, &imai, NULL, &imem);
   if (r != VK_SUCCESS) { LOG("FAIL J: img alloc %d", r); goto done; }
   r = pBindImg(dev, img, imem, 0);
   if (r != VK_SUCCESS) { LOG("FAIL J: bindImg %d", r); goto done; }
   VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView view = VK_NULL_HANDLE;
   r = pCreateView(dev, &vci, NULL, &view);
   LOG("J vkCreateImageView -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* render pass: 1 color attachment, clear -> store, final = TRANSFER_SRC. */
   VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
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
      .attachmentCount = 1, .pAttachments = &att,
      .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp = VK_NULL_HANDLE;
   r = pCreateRP(dev, &rpci, NULL, &rp);
   LOG("N vkCreateRenderPass -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = rp, .attachmentCount = 1, .pAttachments = &view,
      .width = IMG_W, .height = IMG_H, .layers = 1 };
   VkFramebuffer fbuf = VK_NULL_HANDLE;
   r = pCreateFB(dev, &fbci, NULL, &fbuf);
   LOG("N vkCreateFramebuffer -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* N: shader modules + graphics pipeline. */
   VkShaderModuleCreateInfo vmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = tri_vert_spv_sz, .pCode = tri_vert_spv };
   VkShaderModuleCreateInfo fmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = tri_frag_spv_sz, .pCode = tri_frag_spv };
   VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
   r = pCreateSM(dev, &vmci, NULL, &vsm);
   if (r != VK_SUCCESS) { LOG("FAIL N: vert module %d", r); goto done; }
   r = pCreateSM(dev, &fmci, NULL, &fsm);
   if (r != VK_SUCCESS) { LOG("FAIL N: frag module %d", r); goto done; }
   LOG("N shader modules created (vert %u B, frag %u B)", tri_vert_spv_sz, tri_frag_spv_sz);

   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout pl = VK_NULL_HANDLE;
   r = pCreatePL(dev, &plci, NULL, &pl);
   if (r != VK_SUCCESS) { LOG("FAIL N: pipeline layout %d", r); goto done; }

   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" },
   };
   VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, (float)IMG_W, (float)IMG_H, 0.0f, 1.0f };
   VkRect2D sc = { { 0, 0 }, { IMG_W, IMG_H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_FALSE,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vis,
      .pInputAssemblyState = &ias, .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl,
      .renderPass = rp, .subpass = 0 };
   VkPipeline pipe = VK_NULL_HANDLE;
   r = pCreateGP(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   LOG("N vkCreateGraphicsPipelines -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* L: host-visible readback buffer. */
   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = IMG_BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
   VkBuffer buf = VK_NULL_HANDLE;
   r = pCreateBuf(dev, &bci, NULL, &buf);
   if (r != VK_SUCCESS) { LOG("FAIL L: createBuffer %d", r); goto done; }
   VkMemoryRequirements bmr; pBufReq(dev, buf, &bmr);
   uint32_t bmt = pick_mem_type(&mp, bmr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (bmt == UINT32_MAX) { LOG("FAIL L: no host-visible mem"); goto done; }
   VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = bmr.size, .memoryTypeIndex = bmt };
   VkDeviceMemory bmem = VK_NULL_HANDLE;
   r = pAlloc(dev, &bmai, NULL, &bmem);
   if (r != VK_SUCCESS) { LOG("FAIL L: allocBufMem %d", r); goto done; }
   void *cpu = NULL;
   r = pMap(dev, bmem, 0, VK_WHOLE_SIZE, 0, &cpu);
   if (r != VK_SUCCESS || !cpu) { LOG("FAIL L: map %d", r); goto done; }
   memset(cpu, 0, IMG_BYTES);
   r = pBindBuf(dev, buf, bmem, 0);
   if (r != VK_SUCCESS) { LOG("FAIL L: bindBuf %d", r); goto done; }

   /* O: record render pass (clear black, draw triangle) + copy to buffer. */
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = qfi };
   VkCommandPool pool; r = pCreatePool(dev, &pci, NULL, &pool);
   if (r != VK_SUCCESS) { LOG("FAIL: pool %d", r); goto done; }
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cb; r = pAllocCB(dev, &cbai, &cb);
   if (r != VK_SUCCESS) { LOG("FAIL: allocCB %d", r); goto done; }
   VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   r = pBegin(cb, &cbbi);
   if (r != VK_SUCCESS) { LOG("FAIL: begin %d", r); goto done; }

   VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } }; /* black */
   VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = rp, .framebuffer = fbuf,
      .renderArea = { { 0, 0 }, { IMG_W, IMG_H } },
      .clearValueCount = 1, .pClearValues = &clear };
   pBeginRP(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   pBindPipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   pDraw(cb, 3, 1, 0, 0);
   pEndRP(cb);

   VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { IMG_W, IMG_H, 1 } };
   pCopy(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

   r = pEnd(cb);
   LOG("O recorded (renderpass+draw+copy); vkEndCommandBuffer -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb };
   r = pSubmit(queue, 1, &si, VK_NULL_HANDLE);
   LOG("vkQueueSubmit -> %d", r);
   if (r != VK_SUCCESS) goto done;
   r = pWaitIdle(queue);
   LOG("vkQueueWaitIdle -> %d", r);
   if (r != VK_SUCCESS) goto done;

   armDCacheFlush(cpu, IMG_BYTES);

   /* M: did the triangle rasterise? count yellow vs black. */
   uint32_t *px = (uint32_t *)cpu;
   uint32_t yellow = 0, black = 0, other = 0;
   for (uint32_t i = 0; i < PIXELS; i++) {
      if (px[i] == YELLOW_U32) yellow++;
      else if (px[i] == BLACK_U32) black++;
      else other++;
   }
   LOG("M pixels: yellow=%u black=%u other=%u (px[center]=0x%08x)",
       yellow, black, other, px[(IMG_H/2) * IMG_W + IMG_W/2]);
   if (yellow > 100 && black > 100) {
      LOG("=== M3 STEP 1b PASSED — NVK rasterised a TRIANGLE on Tegra ===");
   } else {
      LOG("=== M3 STEP 1b: unexpected pixel mix (shaders/draw may not have run) ===");
   }

   memcpy(g_shot, px, IMG_BYTES);
   have_shot = 1;

done:
   if (inst != VK_NULL_HANDLE) {
      PFN_vkDestroyInstance pDestroyInstance =
         (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(inst, "vkDestroyInstance");
      if (pDestroyInstance) pDestroyInstance(inst, NULL);
      LOG("cleanup: vkDestroyInstance done");
   }
   if (have_shot) present_shot();
   LOG("=== done; log at sdmc:/nvk_tri.log ===");
   if (g_log) fclose(g_log);
   return 0;
}
