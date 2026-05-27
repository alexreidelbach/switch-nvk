/*
 * nvk_multi.c — Roadmap Tier 1.2: many draws + pipeline/descriptor switches +
 * ALPHA BLENDING, on the Switch GM20B, by our own NVK. The classic RGB "venn":
 * an opaque dark background quad, then three translucent quads (red, green,
 * blue at alpha 0.5) overlapping in the centre. The overlaps blend
 * (src_alpha, one_minus_src_alpha) into mixed colours — the proof that
 * transparency works.
 *
 * Exercises, in ONE render pass:
 *   - MULTIPLE draws per frame (4 objects),
 *   - a PIPELINE switch (opaque pipeline for the bg [blend off] -> blend
 *     pipeline for the translucent quads [blend on]),
 *   - DESCRIPTOR-SET switches (one UBO/descriptor set per object: colour+rect),
 *   - ALPHA BLENDING (the overlaps are blended, not the topmost colour).
 * All four draws are vkCmdDrawIndexed (a unit quad: 4 verts + 6 indices).
 *
 *   A..D  instance / device / queue
 *   U  unit-quad vbuf + index buffer + 4 per-object UBOs + 4 descriptor sets
 *   N  two pipelines (opaque + blend), same multi.vert/multi.frag
 *   O  render pass: clear, [opaque: bg], [blend: R, G, B], copy
 *   L  submit, read back, verify blended overlaps (sample points), report
 *   P  present the blended frame on the TV, until +
 *
 * Loaderless ICD (VK_NO_PROTOTYPES); logs sdmc:/nvk_multi.log.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "shaders/tri_shaders.h"   /* multi_vert_spv[], multi_frag_spv[] + _sz */

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

#define NOBJ 4u   /* bg + R + G + B */

static FILE *g_log;
static uint32_t g_shot[SCN_PIXELS];

/* Per-object UBO (std140): vec4 colour @0, vec2 centre @16, vec2 halfsize @24. */
struct ObjUBO { float color[4]; float center[2]; float halfsize[2]; };
static const struct ObjUBO OBJS[NOBJ] = {
   {{0.10f,0.10f,0.12f,1.00f},{ 0.00f, 0.00f},{1.00f,1.00f}},  /* 0: opaque bg (fullscreen) */
   {{1.00f,0.00f,0.00f,0.50f},{ 0.00f,-0.35f},{0.45f,0.45f}},  /* 1: red    (translucent) */
   {{0.00f,1.00f,0.00f,0.50f},{-0.30f, 0.25f},{0.45f,0.45f}},  /* 2: green  (translucent) */
   {{0.00f,0.00f,1.00f,0.50f},{ 0.30f, 0.25f},{0.45f,0.45f}},  /* 3: blue   (translucent) */
};
/* Unit quad in [-1,1] (the per-object UBO scales/positions it). */
static const float QUAD_V[4 * 2] = { -1.f,-1.f,  1.f,-1.f,  1.f,1.f,  -1.f,1.f };
static const uint16_t QUAD_I[6]  = { 0,1,2, 0,2,3 };

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

/* Sample the readback at an NDC point (x in [-1,1], y down per Vulkan). */
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

int main(void)
{
   g_log = fopen("sdmc:/nvk_multi.log", "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
      nxlinkStdio();
   LOG("=== NVK multi-draw + alpha blending (Roadmap T1.2: 4 draws, 2 pipelines, RGB venn) [BUILD multi1] ===");

   g_drm_shim_log_sink = shim_log_sink;
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
   setenv("MESA_LOG_FILE", "sdmc:/nvk_multi_mesa.log", 1);

   VkInstance inst = VK_NULL_HANDLE;
   PFN_vkCreateInstance pCreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("FAIL: no vkCreateInstance"); goto done; }
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_multi", .apiVersion = VK_API_VERSION_1_1 };
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
   PFN_vkCmdBeginRenderPass pBeginRP = GD(vkCmdBeginRenderPass);
   PFN_vkCmdBindPipeline pBindPipe = GD(vkCmdBindPipeline);
   PFN_vkCmdBindDescriptorSets pBindDS = GD(vkCmdBindDescriptorSets);
   PFN_vkCmdBindVertexBuffers pBindVB = GD(vkCmdBindVertexBuffers);
   PFN_vkCmdBindIndexBuffer pBindIB = GD(vkCmdBindIndexBuffer);
   PFN_vkCmdDrawIndexed pDrawIndexed = GD(vkCmdDrawIndexed);
   PFN_vkCmdEndRenderPass pEndRP = GD(vkCmdEndRenderPass);
   PFN_vkCmdCopyImageToBuffer pCopyIB = GD(vkCmdCopyImageToBuffer);
   PFN_vkEndCommandBuffer pEnd = GD(vkEndCommandBuffer);
   PFN_vkQueueSubmit pSubmit = GD(vkQueueSubmit);
   PFN_vkQueueWaitIdle pWaitIdle = GD(vkQueueWaitIdle);
   if (!pGetQueue||!pAlloc||!pMap||!pCreateImg||!pBindImg||!pCreateView||
       !pCreateRP||!pCreateFB||!pCreateSM||!pCreateDSL||!pCreateDP||!pAllocDS||!pUpdateDS||
       !pCreatePL||!pCreateGP||!pCreateBuf||!pBindBuf||!pCreatePool||!pAllocCB||
       !pBeginRP||!pBindPipe||!pBindDS||!pBindVB||!pBindIB||!pDrawIndexed||!pEndRP||
       !pCopyIB||!pSubmit||!pWaitIdle) { LOG("FAIL D: device fns"); goto done; }
   VkQueue queue; pGetQueue(dev, qfi, 0, &queue);
   VkPhysicalDeviceMemoryProperties mp; pMemP(phys[0], &mp);

#define ALLOC_IMG(image, memout, want) do { \
      VkMemoryRequirements _mr; pImgReq(dev, image, &_mr); \
      uint32_t _t = pick_mem_type(&mp, _mr.memoryTypeBits, want); \
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

   /* U: unit-quad vertex + index buffers, shared by all 4 objects. */
   VkBuffer vbuf; VkDeviceMemory vmem; void *vp;
   MK_HOST_BUF(vbuf, vmem, vp, sizeof QUAD_V, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   memcpy(vp, QUAD_V, sizeof QUAD_V); armDCacheFlush(vp, sizeof QUAD_V);
   VkBuffer ibuf; VkDeviceMemory imem; void *ip;
   MK_HOST_BUF(ibuf, imem, ip, sizeof QUAD_I, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   memcpy(ip, QUAD_I, sizeof QUAD_I); armDCacheFlush(ip, sizeof QUAD_I);

   /* one UBO per object (offset-0 buffers => no alignment math). */
   VkBuffer ubuf[NOBJ]; VkDeviceMemory umem[NOBJ]; void *up[NOBJ];
   for (uint32_t i = 0; i < NOBJ; i++) {
      MK_HOST_BUF(ubuf[i], umem[i], up[i], sizeof(struct ObjUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      memcpy(up[i], &OBJS[i], sizeof(struct ObjUBO)); armDCacheFlush(up[i], sizeof(struct ObjUBO));
   }
   LOG("U unit quad + %u per-object UBOs ready", NOBJ);

   /* descriptor set layout: binding0 = UBO (vertex stage). 4 sets, one per object. */
   VkDescriptorSetLayoutBinding bind0 = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT };
   VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &bind0 };
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE; r = pCreateDSL(dev, &dslci, NULL, &dsl);
   if (r) { LOG("FAIL U: dsl %d", r); goto done; }
   VkDescriptorPoolSize psz = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NOBJ };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = NOBJ, .poolSizeCount = 1, .pPoolSizes = &psz };
   VkDescriptorPool dpool = VK_NULL_HANDLE; r = pCreateDP(dev, &dpci, NULL, &dpool);
   if (r) { LOG("FAIL U: dpool %d", r); goto done; }
   VkDescriptorSet dset[NOBJ];
   for (uint32_t i = 0; i < NOBJ; i++) {
      VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
      r = pAllocDS(dev, &dsai, &dset[i]); if (r) { LOG("FAIL U: ds[%u] %d", i, r); goto done; }
      VkDescriptorBufferInfo dbi = { .buffer = ubuf[i], .offset = 0, .range = sizeof(struct ObjUBO) };
      VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset[i],
         .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi };
      pUpdateDS(dev, 1, &wr, 0, NULL);
   }
   LOG("U %u descriptor sets ready", NOBJ);

   /* render target (color only — no depth needed for 2D blending). */
   VkImageCreateInfo rii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { SCN_W, SCN_H, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage rt = VK_NULL_HANDLE; r = pCreateImg(dev, &rii, NULL, &rt);
   if (r) { LOG("FAIL N: rt %d", r); goto done; }
   VkDeviceMemory rmem = VK_NULL_HANDLE; ALLOC_IMG(rt, rmem, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

   /* N: two pipelines (same shaders/layout/vertex-input), differ only in blend. */
   VkShaderModuleCreateInfo vmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = multi_vert_spv_sz, .pCode = multi_vert_spv };
   VkShaderModuleCreateInfo fmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = multi_frag_spv_sz, .pCode = multi_frag_spv };
   VkShaderModule vsm, fsm;
   r = pCreateSM(dev, &vmci, NULL, &vsm); if (r) { LOG("FAIL N: vsm %d", r); goto done; }
   r = pCreateSM(dev, &fmci, NULL, &fsm); if (r) { LOG("FAIL N: fsm %d", r); goto done; }
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
   VkPipelineLayout pl; r = pCreatePL(dev, &plci, NULL, &pl); if (r) { LOG("FAIL N: layout %d", r); goto done; }
   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
   VkVertexInputBindingDescription vib = { .binding = 0, .stride = 8, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription via = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };
   VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib,
      .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &via };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp_ = { 0, 0, (float)SCN_W, (float)SCN_H, 0.0f, 1.0f };
   VkRect2D scs = { { 0, 0 }, { SCN_W, SCN_H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp_, .scissorCount = 1, .pScissors = &scs };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo msi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   /* opaque: blend OFF (the bg). */
   VkPipelineColorBlendAttachmentState cbaOpaque = { .blendEnable = VK_FALSE, .colorWriteMask = 0xF };
   /* translucent: standard alpha blend src_alpha / one_minus_src_alpha. */
   VkPipelineColorBlendAttachmentState cbaBlend = { .blendEnable = VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = 0xF };
   VkPipelineColorBlendStateCreateInfo cbsOpaque = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cbaOpaque };
   VkPipelineColorBlendStateCreateInfo cbsBlend  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cbaBlend };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &msi,
      .pColorBlendState = &cbsOpaque, .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipeOpaque = VK_NULL_HANDLE; r = pCreateGP(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeOpaque);
   if (r) { LOG("FAIL N: pipe opaque %d", r); goto done; }
   gpci.pColorBlendState = &cbsBlend;
   VkPipeline pipeBlend = VK_NULL_HANDLE; r = pCreateGP(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeBlend);
   LOG("N pipelines (opaque + blend) -> %d", r); if (r != VK_SUCCESS) goto done;

   /* L: readback buffer. */
   VkBuffer rbuf; VkDeviceMemory rbmem; void *cpu;
   MK_HOST_BUF(rbuf, rbmem, cpu, SCN_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   memset(cpu, 0, SCN_BYTES);

   /* O: record — clear, draw bg (opaque), draw R/G/B (blend), copy. */
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
   VkCommandPool pool; r = pCreatePool(dev, &pci, NULL, &pool); if (r) { LOG("FAIL: pool %d", r); goto done; }
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd; r = pAllocCB(dev, &cbai, &cmd); if (r) { LOG("FAIL: allocCB %d", r); goto done; }
   VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   r = pBegin(cmd, &cbbi); if (r) { LOG("FAIL: begin %d", r); goto done; }

   VkClearValue clear; clear.color = (VkClearColorValue){ .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
   VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fbuf,
      .renderArea = { { 0, 0 }, { SCN_W, SCN_H } }, .clearValueCount = 1, .pClearValues = &clear };
   pBeginRP(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   VkDeviceSize voff = 0; pBindVB(cmd, 0, 1, &vbuf, &voff);
   pBindIB(cmd, ibuf, 0, VK_INDEX_TYPE_UINT16);
   /* object 0 = opaque bg (pipeline switch #1) */
   pBindPipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeOpaque);
   pBindDS(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset[0], 0, NULL);
   pDrawIndexed(cmd, 6, 1, 0, 0, 0);
   /* objects 1..3 = translucent (pipeline switch #2; then descriptor switches) */
   pBindPipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeBlend);
   for (uint32_t i = 1; i < NOBJ; i++) {
      pBindDS(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset[i], 0, NULL);
      pDrawIndexed(cmd, 6, 1, 0, 0, 0);
   }
   pEndRP(cmd);
   VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { SCN_W, SCN_H, 1 } };
   pCopyIB(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);
   r = pEnd(cmd); LOG("O recorded (%u draws: 1 opaque bg + 3 blended); end -> %d", NOBJ, r); if (r) goto done;

   /* L: submit once, read back, verify the blended overlaps. */
   {
      uint32_t *px = (uint32_t *)cpu;
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
      r = pSubmit(queue, 1, &si, VK_NULL_HANDLE);
      if (r != VK_SUCCESS) { LOG("FAIL L: submit %d", r); goto done; }
      pWaitIdle(queue);
      armDCacheFlush(cpu, SCN_BYTES);
      memcpy(g_shot, cpu, SCN_BYTES);

      /* Sample known regions (pixel uint32 = (A<<24)|(B<<16)|(G<<8)|R), each
       * chosen to land in exactly one object set so the colours are predictable. */
      uint32_t c_bg  = sample_ndc(px, -0.92f,  0.92f) & 0x00FFFFFFu;  /* nothing -> bg */
      uint32_t c_r   = sample_ndc(px,  0.00f, -0.60f) & 0x00FFFFFFu;  /* R over bg */
      uint32_t c_g   = sample_ndc(px, -0.55f,  0.45f) & 0x00FFFFFFu;  /* G over bg */
      uint32_t c_b   = sample_ndc(px,  0.55f,  0.45f) & 0x00FFFFFFu;  /* B over bg */
      uint32_t c_cen = sample_ndc(px,  0.00f,  0.00f) & 0x00FFFFFFu;  /* R+G+B overlap */
      LOG("L bg     (-0.92, 0.92) = 0x%06x", c_bg);
      LOG("L R-only ( 0.00,-0.60) = 0x%06x", c_r);
      LOG("L G-only (-0.55, 0.45) = 0x%06x", c_g);
      LOG("L B-only ( 0.55, 0.45) = 0x%06x", c_b);
      LOG("L centre ( 0.00, 0.00) = 0x%06x  (R+G+B overlap)", c_cen);

      /* Blending proof: the five sampled regions are all DISTINCT (per-object
       * colour + blend, not one flat fill), the single-quad samples are blends of
       * their colour over bg (each not pure-saturated, since alpha=0.5 over a dark
       * bg), and the centre differs from bg and from every pure quad colour
       * (with blending OFF the centre would be solid blue = the topmost draw). */
      int all_distinct = (c_bg!=c_r && c_bg!=c_g && c_bg!=c_b && c_bg!=c_cen &&
                          c_r!=c_g && c_r!=c_b && c_r!=c_cen && c_g!=c_b && c_g!=c_cen && c_b!=c_cen);
      int cen_not_pure = (c_cen != 0x0000FFu /*R*/ && c_cen != 0x00FF00u /*G*/ && c_cen != 0xFF0000u /*B*/);
      int blended = all_distinct && cen_not_pure && (c_cen != c_bg) && (c_cen != 0u);
      LOG("L: all5_distinct=%d centre_not_pure=%d => %s", all_distinct, cen_not_pure,
          blended ? "OK" : "FAIL (no blending? - or Eden fake GPU, verify on real HW)");
      if (blended) LOG("=== MULTI-DRAW + ALPHA BLENDING PASSED on Tegra (4 draws, 2 pipelines, blended overlaps) ===");
      else         LOG("=== multi readback not a blend (expected on Eden; verify by the TV on real HW) ===");
   }

   /* P: present the blended frame on the TV (static), until +. */
   {
      PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
      NWindow *win = nwindowGetDefault();
      Framebuffer fb; framebufferCreate(&fb, win, SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 2); framebufferMakeLinear(&fb);
      LOG("P: presenting blended frame; press + to exit");
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
