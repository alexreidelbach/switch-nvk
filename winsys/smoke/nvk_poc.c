/*
 * nvk_poc.c — PROOF OF CONCEPT: run an ESTABLISHED PC Vulkan example's actual
 * shaders on our NVK driver on the Switch, to photograph as proof the driver
 * works with real-world Vulkan code.
 *
 * The shaders are the VERBATIM vertex+fragment shaders from the SaschaWillems/
 * Vulkan "triangle" example (MIT License, (c) Sascha Willems) — the canonical
 * RGB "Vulkan triangle" recognised across the PC Vulkan world. They are compiled
 * by OUR NAK (in NVK) and executed by OUR winsys on the GM20B. A photo of the
 * resulting rainbow triangle = established PC Vulkan code rendering on our driver.
 *
 * His triangle.vert needs a UBO { mat4 projection, model, view } at binding 0 and
 * a vertex buffer of (vec3 position, vec3 color). We feed identity matrices (so
 * the triangle is drawn straight in NDC) and his classic R/G/B vertices.
 *
 * Renders one frame, then holds it on screen (vsync) so it can be photographed;
 * + exits (teardown is irrelevant for a throwaway POC). Logs sdmc:/nvk_poc.log.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "shaders/tri_shaders.h"   /* sascha_vert_spv[], sascha_frag_spv[] + _sz */

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

static FILE *g_log;
static uint32_t *g_frame = NULL;   /* readback pixels of the rendered triangle (NULL until drawn) */
static void shim_log_sink(const char *s) { if (g_log) { fputs(s, g_log); fflush(g_log); } printf("%s", s); fflush(stdout); }
static void plogf(const char *fmt, ...) {
   char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
   if (g_log) { fputs(b, g_log); fputc('\n', g_log); fflush(g_log); } printf("%s\n", b); fflush(stdout);
}
#define LOG(...) plogf(__VA_ARGS__)

/* his triangle vertices: (x,y,z) + (r,g,b) — the classic RGB triangle. */
static const float TRI[3 * 6] = {
    1.0f,  1.0f, 0.0f,   1.0f, 0.0f, 0.0f,   /* red   */
   -1.0f,  1.0f, 0.0f,   0.0f, 1.0f, 0.0f,   /* green */
    0.0f, -1.0f, 0.0f,   0.0f, 0.0f, 1.0f,   /* blue  */
};

static uint32_t pick_mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want) {
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
   return UINT32_MAX;
}

int main(void)
{
   g_log = fopen("sdmc:/nvk_poc.log", "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault())) nxlinkStdio();
   LOG("=== NVK POC: SaschaWillems/Vulkan 'triangle' shaders on our NVK [BUILD poc1] ===");
   g_drm_shim_log_sink = shim_log_sink;
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
   setenv("MESA_LOG_FILE", "sdmc:/nvk_poc_mesa.log", 1);

   VkInstance inst = VK_NULL_HANDLE;
   PFN_vkCreateInstance pCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("no vkCreateInstance"); goto hold; }
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nvk_poc", .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkResult r = pCreateInstance(&ici, NULL, &inst);
   LOG("A vkCreateInstance -> %d", r); if (r) goto hold;

#define GI(n) ((PFN_##n)vk_icdGetInstanceProcAddr(inst, #n))
   PFN_vkEnumeratePhysicalDevices pEnum = GI(vkEnumeratePhysicalDevices);
   PFN_vkGetPhysicalDeviceMemoryProperties pMemP = GI(vkGetPhysicalDeviceMemoryProperties);
   PFN_vkGetPhysicalDeviceQueueFamilyProperties pQF = GI(vkGetPhysicalDeviceQueueFamilyProperties);
   PFN_vkCreateDevice pCreateDev = GI(vkCreateDevice);
   PFN_vkGetDeviceProcAddr pGDPA = GI(vkGetDeviceProcAddr);
   if (!pEnum || !pCreateDev || !pGDPA) { LOG("instance fns missing"); goto hold; }

   uint32_t nd = 1; VkPhysicalDevice phys[4];
   if (pEnum(inst, &nd, NULL) || nd == 0) { LOG("no devices"); goto hold; }
   nd = nd > 4 ? 4 : nd; if (pEnum(inst, &nd, phys)) { LOG("enum2 fail"); goto hold; }
   uint32_t nqf = 0; pQF(phys[0], &nqf, NULL); VkQueueFamilyProperties qf[8]; nqf = nqf > 8 ? 8 : nqf; pQF(phys[0], &nqf, qf);
   uint32_t qfi = UINT32_MAX; for (uint32_t i = 0; i < nqf; i++) if (qfi==UINT32_MAX && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi = i;
   if (qfi == UINT32_MAX) { LOG("no gfx queue"); goto hold; }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
   VkDevice dev = VK_NULL_HANDLE; r = pCreateDev(phys[0], &dci, NULL, &dev);
   LOG("D vkCreateDevice -> %d", r); if (r) goto hold;

#define GD(n) ((PFN_##n)pGDPA(dev, #n))
   PFN_vkGetDeviceQueue pGetQueue = GD(vkGetDeviceQueue);
   PFN_vkAllocateMemory pAlloc = GD(vkAllocateMemory); PFN_vkMapMemory pMap = GD(vkMapMemory);
   PFN_vkCreateImage pCreateImg = GD(vkCreateImage); PFN_vkGetImageMemoryRequirements pImgReq = GD(vkGetImageMemoryRequirements);
   PFN_vkBindImageMemory pBindImg = GD(vkBindImageMemory); PFN_vkCreateImageView pCreateView = GD(vkCreateImageView);
   PFN_vkCreateRenderPass pCreateRP = GD(vkCreateRenderPass); PFN_vkCreateFramebuffer pCreateFB = GD(vkCreateFramebuffer);
   PFN_vkCreateShaderModule pCreateSM = GD(vkCreateShaderModule);
   PFN_vkCreateDescriptorSetLayout pCreateDSL = GD(vkCreateDescriptorSetLayout); PFN_vkCreateDescriptorPool pCreateDP = GD(vkCreateDescriptorPool);
   PFN_vkAllocateDescriptorSets pAllocDS = GD(vkAllocateDescriptorSets); PFN_vkUpdateDescriptorSets pUpdateDS = GD(vkUpdateDescriptorSets);
   PFN_vkCreatePipelineLayout pCreatePL = GD(vkCreatePipelineLayout); PFN_vkCreateGraphicsPipelines pCreateGP = GD(vkCreateGraphicsPipelines);
   PFN_vkCreateBuffer pCreateBuf = GD(vkCreateBuffer); PFN_vkGetBufferMemoryRequirements pBufReq = GD(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory pBindBuf = GD(vkBindBufferMemory);
   PFN_vkCreateCommandPool pCreatePool = GD(vkCreateCommandPool); PFN_vkAllocateCommandBuffers pAllocCB = GD(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer pBegin = GD(vkBeginCommandBuffer); PFN_vkCmdBeginRenderPass pBeginRP = GD(vkCmdBeginRenderPass);
   PFN_vkCmdBindPipeline pBindPipe = GD(vkCmdBindPipeline); PFN_vkCmdBindDescriptorSets pBindDS = GD(vkCmdBindDescriptorSets);
   PFN_vkCmdBindVertexBuffers pBindVB = GD(vkCmdBindVertexBuffers); PFN_vkCmdDraw pDraw = GD(vkCmdDraw);
   PFN_vkCmdEndRenderPass pEndRP = GD(vkCmdEndRenderPass); PFN_vkCmdCopyImageToBuffer pCopyIB = GD(vkCmdCopyImageToBuffer);
   PFN_vkEndCommandBuffer pEnd = GD(vkEndCommandBuffer); PFN_vkQueueSubmit pSubmit = GD(vkQueueSubmit); PFN_vkQueueWaitIdle pWaitIdle = GD(vkQueueWaitIdle);

   VkQueue queue; pGetQueue(dev, qfi, 0, &queue);
   VkPhysicalDeviceMemoryProperties mp; pMemP(phys[0], &mp);

#define MK_HOST_BUF(buffer, memo, mapo, sz, usg) do { \
      VkBufferCreateInfo _bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (sz), .usage = (usg), .sharingMode = VK_SHARING_MODE_EXCLUSIVE }; \
      if (pCreateBuf(dev, &_bi, NULL, &(buffer))) { LOG("buf fail"); goto hold; } \
      VkMemoryRequirements _m; pBufReq(dev, buffer, &_m); \
      uint32_t _t = pick_mem_type(&mp, _m.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); \
      VkMemoryAllocateInfo _a = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = _m.size, .memoryTypeIndex = _t }; \
      if (pAlloc(dev, &_a, NULL, &(memo))) { LOG("bufmem fail"); goto hold; } \
      if (pMap(dev, memo, 0, VK_WHOLE_SIZE, 0, &(mapo))) { LOG("map fail"); goto hold; } \
      if (pBindBuf(dev, buffer, memo, 0)) { LOG("bindbuf fail"); goto hold; } \
   } while (0)

   /* vertex buffer (his RGB triangle) + UBO (3 identity matrices). */
   VkBuffer vbuf; VkDeviceMemory vmem; void *vp; MK_HOST_BUF(vbuf, vmem, vp, sizeof TRI, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   memcpy(vp, TRI, sizeof TRI); armDCacheFlush(vp, sizeof TRI);
   VkBuffer ubuf; VkDeviceMemory umem; void *upm; MK_HOST_BUF(ubuf, umem, upm, 192, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   { float m[48]; memset(m, 0, sizeof m); m[0]=m[5]=m[10]=m[15]=1.0f; m[16]=m[21]=m[26]=m[31]=1.0f; m[32]=m[37]=m[42]=m[47]=1.0f; memcpy(upm, m, 192); armDCacheFlush(upm, 192); }
   LOG("vbuf + UBO(identity x3) ready");

   /* descriptor set: binding 0 = UBO (vertex stage), as his shader expects. */
   VkDescriptorSetLayoutBinding b0 = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT };
   VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b0 };
   VkDescriptorSetLayout dsl; if (pCreateDSL(dev, &dslci, NULL, &dsl)) { LOG("dsl fail"); goto hold; }
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool dpool; if (pCreateDP(dev, &dpci, NULL, &dpool)) { LOG("dpool fail"); goto hold; }
   VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet dset; if (pAllocDS(dev, &dsai, &dset)) { LOG("allocDS fail"); goto hold; }
   VkDescriptorBufferInfo dbi = { .buffer = ubuf, .offset = 0, .range = 192 };
   VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi };
   pUpdateDS(dev, 1, &wds, 0, NULL);

   /* color render target + view. */
   VkImageCreateInfo rii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { SCN_W, SCN_H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage rt; if (pCreateImg(dev, &rii, NULL, &rt)) { LOG("rt fail"); goto hold; }
   VkMemoryRequirements imr; pImgReq(dev, rt, &imr);
   uint32_t it = pick_mem_type(&mp, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (it == UINT32_MAX) it = pick_mem_type(&mp, imr.memoryTypeBits, 0);
   VkMemoryAllocateInfo iai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = imr.size, .memoryTypeIndex = it };
   VkDeviceMemory rmem; if (pAlloc(dev, &iai, NULL, &rmem) || pBindImg(dev, rt, rmem, 0)) { LOG("rt mem fail"); goto hold; }
   VkImageViewCreateInfo rvci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rt, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView rview; if (pCreateView(dev, &rvci, NULL, &rview)) { LOG("rview fail"); goto hold; }

   VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
   VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref };
   VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
   VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp; if (pCreateRP(dev, &rpci, NULL, &rp)) { LOG("rp fail"); goto hold; }
   VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &rview, .width = SCN_W, .height = SCN_H, .layers = 1 };
   VkFramebuffer fbuf; if (pCreateFB(dev, &fbci, NULL, &fbuf)) { LOG("fb fail"); goto hold; }

   VkShaderModuleCreateInfo vmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sascha_vert_spv_sz, .pCode = sascha_vert_spv };
   VkShaderModuleCreateInfo fmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sascha_frag_spv_sz, .pCode = sascha_frag_spv };
   VkShaderModule vsm, fsm; if (pCreateSM(dev, &vmci, NULL, &vsm) || pCreateSM(dev, &fmci, NULL, &fsm)) { LOG("shader module fail"); goto hold; }
   LOG("N Sascha triangle shaders loaded (vert %u B, frag %u B)", sascha_vert_spv_sz, sascha_frag_spv_sz);
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
   VkPipelineLayout pl; if (pCreatePL(dev, &plci, NULL, &pl)) { LOG("layout fail"); goto hold; }
   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
   VkVertexInputBindingDescription vib = { .binding = 0, .stride = 24, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription via[2] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 } };
   VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = via };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp_ = { 0, 0, (float)SCN_W, (float)SCN_H, 0.0f, 1.0f }; VkRect2D sc = { { 0, 0 }, { SCN_W, SCN_H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp_, .scissorCount = 1, .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_FALSE, .colorWriteMask = 0xF };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe; r = pCreateGP(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   LOG("N vkCreateGraphicsPipelines -> %d", r); if (r) goto hold;

   VkBuffer rbuf; VkDeviceMemory rbmem; void *cpu; MK_HOST_BUF(rbuf, rbmem, cpu, SCN_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   memset(cpu, 0, SCN_BYTES);

   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
   VkCommandPool pool; if (pCreatePool(dev, &pci, NULL, &pool)) { LOG("pool fail"); goto hold; }
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd; if (pAllocCB(dev, &cbai, &cmd)) { LOG("allocCB fail"); goto hold; }
   VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   pBegin(cmd, &cbbi);
   VkClearValue clear; clear.color = (VkClearColorValue){ .float32 = { 0.10f, 0.10f, 0.12f, 1.0f } };
   VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fbuf, .renderArea = { { 0, 0 }, { SCN_W, SCN_H } }, .clearValueCount = 1, .pClearValues = &clear };
   pBeginRP(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   pBindPipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   pBindDS(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
   VkDeviceSize voff = 0; pBindVB(cmd, 0, 1, &vbuf, &voff);
   pDraw(cmd, 3, 1, 0, 0);
   pEndRP(cmd);
   VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { SCN_W, SCN_H, 1 } };
   pCopyIB(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);
   pEnd(cmd);
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
   r = pSubmit(queue, 1, &si, VK_NULL_HANDLE); LOG("vkQueueSubmit -> %d", r); if (r) goto hold;
   pWaitIdle(queue); armDCacheFlush(cpu, SCN_BYTES);
   g_frame = (uint32_t *)cpu;   /* the rendered triangle, for the on-screen hold */
   {
      uint32_t *px = (uint32_t *)cpu, red = 0, green = 0, blue = 0, other = 0;
      for (uint32_t i = 0; i < SCN_PIXELS; i++) { uint32_t p = px[i] & 0xFFFFFF; if (p==0xFF) blue++; else if (p==0xFF00) green++; else if (p==0xFF0000) red++; else other++; }
      LOG("M Sascha triangle drawn: red=%u green=%u blue=%u other=%u", red, green, blue, other);
      LOG("=== POC: NVK ran SaschaWillems' triangle shaders on Tegra ===");
   }

hold:
   /* Hold the rendered frame on screen (vsync) so it can be photographed.
    * g_frame points at the readback pixels (NULL if rendering failed -> grey). */
   {
      PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
      Framebuffer fb; framebufferCreate(&fb, nwindowGetDefault(), SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 2); framebufferMakeLinear(&fb);
      LOG("P: holding on screen for the photo; press + to exit");
      while (appletMainLoop()) {
         padUpdate(&pad);
         if (padGetButtonsDown(&pad) & HidNpadButton_Plus) svcExitProcess();
         u32 stride; u8 *base = (u8 *)framebufferBegin(&fb, &stride);
         for (u32 yy = 0; yy < SCR_H; yy++) {
            uint32_t *row = (uint32_t *)(base + (size_t)yy * stride);
            u32 sy = yy * SCN_H / SCR_H;
            for (u32 xx = 0; xx < SCR_W; xx++)
               row[xx] = g_frame ? g_frame[sy * SCN_W + (xx * SCN_W / SCR_W)] : 0xFF303030u;
         }
         framebufferEnd(&fb);
      }
   }
   return 0;
}
