/*
 * nvk_swapchain.c — Roadmap Tier 2.4: a REAL VkSwapchain on the Switch GM20B, by
 * our own NVK. Uses VK_EXT_headless_surface (wsi_common_headless.c, always built
 * on non-Windows) so we get genuine VK_KHR_surface + VK_KHR_swapchain semantics
 * WITHOUT a custom window-system backend: vkCreateHeadlessSurfaceEXT ->
 * vkCreateSwapchainKHR -> a per-frame vkAcquireNextImageKHR / render /
 * vkQueuePresentKHR loop with acquire+render semaphores. The headless present is
 * a no-op scanout, so we copy the just-rendered swapchain image to CPU and blit
 * it to the libnx framebuffer to actually show it on the TV. The point is the
 * SWAPCHAIN path (surface caps/formats/present-modes, acquire/present/recycle,
 * WSI sync) — exactly what Dawn-over-Vulkan drives the screen through.
 *
 *   A..D  instance (+KHR_surface,+EXT_headless_surface) / device (+KHR_swapchain) / queue
 *   S  headless surface + caps/formats/present-modes + vkCreateSwapchainKHR (FIFO)
 *   U  unit-quad vbuf/ibuf + per-frame UBO (moving coloured quad, reuses multi.vert/frag)
 *   N  pipeline + render pass + per-swapchain-image view/framebuffer/cmd
 *   P  loop: acquire -> update UBO -> submit(render) -> present -> blit to TV, until +
 *
 * Loaderless ICD (VK_NO_PROTOTYPES); logs sdmc:/nvk_swapchain.log.
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
#define MAX_IMG 8u

static FILE *g_log;
static uint32_t g_shot[SCN_PIXELS];

struct ObjUBO { float color[4]; float center[2]; float halfsize[2]; };
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

int main(void)
{
   g_log = fopen("sdmc:/nvk_swapchain.log", "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
      nxlinkStdio();
   LOG("=== NVK swapchain (Roadmap T2.4: real VkSwapchain via headless surface) [BUILD sc1] ===");

   g_drm_shim_log_sink = shim_log_sink;
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
   setenv("MESA_LOG_FILE", "sdmc:/nvk_swapchain_mesa.log", 1);

   VkInstance inst = VK_NULL_HANDLE;
   PFN_vkCreateInstance pCreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("FAIL: no vkCreateInstance"); goto done; }
   const char *iexts[] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_swapchain", .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
      .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts };
   VkResult r = pCreateInstance(&ici, NULL, &inst);
   LOG("A vkCreateInstance (+surface,+headless) -> %d", r);
   if (r != VK_SUCCESS) goto done;

#define GI(n) ((PFN_##n)vk_icdGetInstanceProcAddr(inst, #n))
   PFN_vkEnumeratePhysicalDevices          pEnum  = GI(vkEnumeratePhysicalDevices);
   PFN_vkGetPhysicalDeviceMemoryProperties pMemP  = GI(vkGetPhysicalDeviceMemoryProperties);
   PFN_vkGetPhysicalDeviceQueueFamilyProperties pQF = GI(vkGetPhysicalDeviceQueueFamilyProperties);
   PFN_vkCreateDevice                      pCreateDev = GI(vkCreateDevice);
   PFN_vkGetDeviceProcAddr                 pGDPA  = GI(vkGetDeviceProcAddr);
   PFN_vkCreateHeadlessSurfaceEXT          pCreateHeadless = GI(vkCreateHeadlessSurfaceEXT);
   PFN_vkGetPhysicalDeviceSurfaceSupportKHR      pSurfSupport = GI(vkGetPhysicalDeviceSurfaceSupportKHR);
   PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pSurfCaps = GI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
   PFN_vkGetPhysicalDeviceSurfaceFormatsKHR      pSurfFmts = GI(vkGetPhysicalDeviceSurfaceFormatsKHR);
   PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pSurfModes = GI(vkGetPhysicalDeviceSurfacePresentModesKHR);
   if (!pEnum || !pCreateDev || !pGDPA) { LOG("FAIL: instance fns"); goto done; }
   if (!pCreateHeadless || !pSurfSupport || !pSurfCaps || !pSurfFmts || !pSurfModes) {
      LOG("FAIL: WSI instance fns missing (surface/headless not wired?)"); goto done; }

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
   const char *dexts[] = { "VK_KHR_swapchain" };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };
   VkDevice dev = VK_NULL_HANDLE;
   r = pCreateDev(phys[0], &dci, NULL, &dev);
   LOG("D vkCreateDevice (+swapchain) -> %d", r);
   if (r != VK_SUCCESS) goto done;

#define GD(n) ((PFN_##n)pGDPA(dev, #n))
   PFN_vkGetDeviceQueue pGetQueue = GD(vkGetDeviceQueue);
   PFN_vkAllocateMemory pAlloc = GD(vkAllocateMemory);
   PFN_vkMapMemory pMap = GD(vkMapMemory);
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
   PFN_vkCmdPipelineBarrier pBarrier = GD(vkCmdPipelineBarrier);
   PFN_vkEndCommandBuffer pEnd = GD(vkEndCommandBuffer);
   PFN_vkQueueSubmit pSubmit = GD(vkQueueSubmit);
   PFN_vkQueueWaitIdle pWaitIdle = GD(vkQueueWaitIdle);
   PFN_vkCreateSemaphore pCreateSem = GD(vkCreateSemaphore);
   PFN_vkCreateSwapchainKHR pCreateSC = GD(vkCreateSwapchainKHR);
   PFN_vkGetSwapchainImagesKHR pGetSCImgs = GD(vkGetSwapchainImagesKHR);
   PFN_vkAcquireNextImageKHR pAcquire = GD(vkAcquireNextImageKHR);
   PFN_vkQueuePresentKHR pPresent = GD(vkQueuePresentKHR);
   if (!pGetQueue||!pAlloc||!pMap||!pCreateView||!pCreateRP||!pCreateFB||!pCreateSM||!pCreateDSL||
       !pCreateDP||!pAllocDS||!pUpdateDS||!pCreatePL||!pCreateGP||!pCreateBuf||!pBindBuf||!pCreatePool||
       !pAllocCB||!pBeginRP||!pBindPipe||!pBindDS||!pBindVB||!pBindIB||!pDrawIndexed||!pEndRP||!pCopyIB||
       !pBarrier||!pSubmit||!pWaitIdle||!pCreateSem) { LOG("FAIL D: device fns"); goto done; }
   if (!pCreateSC || !pGetSCImgs || !pAcquire || !pPresent) {
      LOG("FAIL D: swapchain fns missing (KHR_swapchain not wired?)"); goto done; }
   VkQueue queue; pGetQueue(dev, qfi, 0, &queue);
   VkPhysicalDeviceMemoryProperties mp; pMemP(phys[0], &mp);

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

   /* ---- S: headless surface + swapchain ---- */
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   VkHeadlessSurfaceCreateInfoEXT hsci = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
   r = pCreateHeadless(inst, &hsci, NULL, &surface);
   LOG("S vkCreateHeadlessSurfaceEXT -> %d", r); if (r != VK_SUCCESS) goto done;

   VkBool32 supp = VK_FALSE; pSurfSupport(phys[0], qfi, surface, &supp);
   LOG("S surface support on qfam %u = %d", qfi, supp);
   VkSurfaceCapabilitiesKHR caps; r = pSurfCaps(phys[0], surface, &caps);
   if (r) { LOG("FAIL S: surface caps %d", r); goto done; }
   uint32_t nfmt = 0; pSurfFmts(phys[0], surface, &nfmt, NULL);
   VkSurfaceFormatKHR fmts[16]; nfmt = (nfmt > 16) ? 16 : nfmt;
   pSurfFmts(phys[0], surface, &nfmt, fmts);
   uint32_t nmodes = 0; pSurfModes(phys[0], surface, &nmodes, NULL);
   LOG("S caps: minImg=%u maxImg=%u curExtent=%ux%u usage=0x%x; %u formats, %u present modes",
       caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height,
       caps.supportedUsageFlags, nfmt, nmodes);

   /* choose a format: prefer R8G8B8A8_UNORM (matches nwindow RGBA), else B8G8R8A8 (swap r/b). */
   VkFormat scfmt = fmts[0].format; VkColorSpaceKHR sccs = fmts[0].colorSpace; int swap_rb = 0;
   for (uint32_t i = 0; i < nfmt; i++) {
      if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { scfmt = fmts[i].format; sccs = fmts[i].colorSpace; swap_rb = 0; break; }
      if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { scfmt = fmts[i].format; sccs = fmts[i].colorSpace; swap_rb = 1; }
   }
   VkExtent2D ext = caps.currentExtent;
   if (ext.width == 0xFFFFFFFFu) { ext.width = SCN_W; ext.height = SCN_H; }
   if (ext.width > SCN_W || ext.height > SCN_H) { LOG("FAIL S: surface extent %ux%u exceeds readback %ux%u", ext.width, ext.height, SCN_W, SCN_H); goto done; }
   uint32_t want_img = caps.minImageCount + 1;
   if (caps.maxImageCount && want_img > caps.maxImageCount) want_img = caps.maxImageCount;
   if (want_img > MAX_IMG) want_img = MAX_IMG;
   VkImageUsageFlags scusage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   if ((caps.supportedUsageFlags & scusage) != scusage) { LOG("FAIL S: surface lacks COLOR_ATTACHMENT|TRANSFER_SRC (0x%x)", caps.supportedUsageFlags); goto done; }

   VkSwapchainCreateInfoKHR scci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface, .minImageCount = want_img, .imageFormat = scfmt, .imageColorSpace = sccs,
      .imageExtent = ext, .imageArrayLayers = 1, .imageUsage = scusage,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   r = pCreateSC(dev, &scci, NULL, &swapchain);
   LOG("S vkCreateSwapchainKHR (fmt=%d ext=%ux%u img=%u FIFO) -> %d", scfmt, ext.width, ext.height, want_img, r);
   if (r != VK_SUCCESS) goto done;
   uint32_t nimg = 0; pGetSCImgs(dev, swapchain, &nimg, NULL);
   VkImage scimg[MAX_IMG]; nimg = (nimg > MAX_IMG) ? MAX_IMG : nimg;
   r = pGetSCImgs(dev, swapchain, &nimg, scimg);
   LOG("S swapchain has %u images -> %d", nimg, r); if (r) goto done;

   /* ---- U: quad + per-frame UBO ---- */
   VkBuffer vbuf; VkDeviceMemory vmem; void *vp;
   MK_HOST_BUF(vbuf, vmem, vp, sizeof QUAD_V, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   memcpy(vp, QUAD_V, sizeof QUAD_V); armDCacheFlush(vp, sizeof QUAD_V);
   VkBuffer ibuf; VkDeviceMemory imem; void *ip;
   MK_HOST_BUF(ibuf, imem, ip, sizeof QUAD_I, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   memcpy(ip, QUAD_I, sizeof QUAD_I); armDCacheFlush(ip, sizeof QUAD_I);
   VkBuffer ubuf; VkDeviceMemory umem; void *up;
   MK_HOST_BUF(ubuf, umem, up, sizeof(struct ObjUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   struct ObjUBO obj = { {1.f,1.f,1.f,1.f}, {0.f,0.f}, {0.25f,0.35f} };
   memcpy(up, &obj, sizeof obj); armDCacheFlush(up, sizeof obj);

   VkDescriptorSetLayoutBinding bind0 = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT };
   VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bind0 };
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE; r = pCreateDSL(dev, &dslci, NULL, &dsl); if (r) { LOG("FAIL U: dsl %d", r); goto done; }
   VkDescriptorPoolSize psz = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &psz };
   VkDescriptorPool dpool = VK_NULL_HANDLE; r = pCreateDP(dev, &dpci, NULL, &dpool); if (r) { LOG("FAIL U: dpool %d", r); goto done; }
   VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet dset = VK_NULL_HANDLE; r = pAllocDS(dev, &dsai, &dset); if (r) { LOG("FAIL U: ds %d", r); goto done; }
   VkDescriptorBufferInfo dbi = { .buffer = ubuf, .offset = 0, .range = sizeof(struct ObjUBO) };
   VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi };
   pUpdateDS(dev, 1, &wr, 0, NULL);

   /* ---- N: render pass (clear -> TRANSFER_SRC so we can copy) + pipeline ---- */
   VkAttachmentDescription att = { .format = scfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
   VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref };
   VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
   VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
      .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp = VK_NULL_HANDLE; r = pCreateRP(dev, &rpci, NULL, &rp); if (r) { LOG("FAIL N: rp %d", r); goto done; }

   VkImageView scview[MAX_IMG]; VkFramebuffer scfb[MAX_IMG];
   for (uint32_t i = 0; i < nimg; i++) {
      VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = scimg[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = scfmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
      r = pCreateView(dev, &vci, NULL, &scview[i]); if (r) { LOG("FAIL N: scview[%u] %d", i, r); goto done; }
      VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
         .attachmentCount = 1, .pAttachments = &scview[i], .width = ext.width, .height = ext.height, .layers = 1 };
      r = pCreateFB(dev, &fci, NULL, &scfb[i]); if (r) { LOG("FAIL N: scfb[%u] %d", i, r); goto done; }
   }

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
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib, .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &via };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp_ = { 0, 0, (float)ext.width, (float)ext.height, 0.0f, 1.0f };
   VkRect2D scs = { { 0, 0 }, { ext.width, ext.height } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp_, .scissorCount = 1, .pScissors = &scs };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo msi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_FALSE, .colorWriteMask = 0xF };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &msi, .pColorBlendState = &cbs,
      .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe = VK_NULL_HANDLE; r = pCreateGP(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   LOG("N pipeline -> %d", r); if (r != VK_SUCCESS) goto done;

   /* readback buffer (copy the rendered swapchain image here -> blit to TV). */
   VkBuffer rbuf; VkDeviceMemory rbmem; void *cpu;
   MK_HOST_BUF(rbuf, rbmem, cpu, ext.width * ext.height * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

   /* per-image command buffers: clear+draw into the swapchain image, copy to
    * readback, then transition the image to PRESENT_SRC for vkQueuePresentKHR. */
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
   VkCommandPool pool; r = pCreatePool(dev, &pci, NULL, &pool); if (r) { LOG("FAIL: pool %d", r); goto done; }
   VkCommandBuffer cmds[MAX_IMG];
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = nimg };
   r = pAllocCB(dev, &cbai, cmds); if (r) { LOG("FAIL: allocCB %d", r); goto done; }
   VkClearValue clear; clear.color = (VkClearColorValue){ .float32 = { 0.05f, 0.06f, 0.10f, 1.0f } };
   VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { ext.width, ext.height, 1 } };
   for (uint32_t i = 0; i < nimg; i++) {
      VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      r = pBegin(cmds[i], &cbbi); if (r) { LOG("FAIL: begin[%u] %d", i, r); goto done; }
      VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = scfb[i],
         .renderArea = { { 0, 0 }, { ext.width, ext.height } }, .clearValueCount = 1, .pClearValues = &clear };
      pBeginRP(cmds[i], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
      pBindPipe(cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      pBindDS(cmds[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
      VkDeviceSize voff = 0; pBindVB(cmds[i], 0, 1, &vbuf, &voff);
      pBindIB(cmds[i], ibuf, 0, VK_INDEX_TYPE_UINT16);
      pDrawIndexed(cmds[i], 6, 1, 0, 0, 0);
      pEndRP(cmds[i]);
      pCopyIB(cmds[i], scimg[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);
      VkImageMemoryBarrier toPresent = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = 0,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = scimg[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
      pBarrier(cmds[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &toPresent);
      r = pEnd(cmds[i]); if (r) { LOG("FAIL: end[%u] %d", i, r); goto done; }
   }

   VkSemaphore acquireSem;
   VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
   r = pCreateSem(dev, &semci, NULL, &acquireSem); if (r) { LOG("FAIL: acquireSem %d", r); goto done; }
   LOG("=== swapchain ready; entering acquire/present loop ===");

   /* ---- P: the real swapchain frame loop ---- */
   {
      PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
      NWindow *win = nwindowGetDefault();
      Framebuffer fb; framebufferCreate(&fb, win, SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 2); framebufferMakeLinear(&fb);
      uint32_t *px = (uint32_t *)cpu;
      uint32_t frames = 0; int logged = 0; uint64_t t0 = armGetSystemTick();
      while (appletMainLoop()) {
         padUpdate(&pad);
         if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; }
            svcExitProcess();
         }
         uint32_t idx = 0;
         if (frames == 0) LOG("P f0: calling vkAcquireNextImageKHR...");
         VkResult ar = pAcquire(dev, swapchain, UINT64_MAX, acquireSem, VK_NULL_HANDLE, &idx);
         if (frames == 0) LOG("P f0: acquire -> %d idx=%u", ar, idx);
         if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { LOG("acquire @frame %u -> %d", frames, ar); break; }

         /* animate: a ping-pong horizontal move + a slow colour cycle. */
         float ph = (float)((frames % 240u)) / 240.0f;        /* 0..1 */
         float tri = ph < 0.5f ? (ph * 2.0f) : (2.0f - ph * 2.0f);  /* 0..1..0 */
         obj.center[0] = -0.6f + 1.2f * tri;
         obj.color[0] = tri; obj.color[1] = 1.0f - tri; obj.color[2] = 0.5f + 0.5f * tri;
         memcpy(up, &obj, sizeof obj); armDCacheFlush(up, sizeof obj);

         /* Wait on acquireSem (signalled by acquire); no render semaphore — we
          * vkQueueWaitIdle before present, so present needs no wait-semaphore.
          * (Our winsys CPU-waits a submit's wait-semaphores before kickoff; the
          * WSI present does an internal QueueSubmit waiting on the present
          * wait-semaphore, which deadlocks/stalls on our binary-semaphore wait —
          * so we present with waitSemaphoreCount=0.) */
         VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
         VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &acquireSem, .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1, .pCommandBuffers = &cmds[idx] };
         r = pSubmit(queue, 1, &si, VK_NULL_HANDLE);
         if (frames == 0) LOG("P f0: submit -> %d", r);
         if (r != VK_SUCCESS) { LOG("submit @frame %u -> %d", frames, r); break; }

         pWaitIdle(queue);                 /* render+copy fully done before present */
         if (frames == 0) LOG("P f0: waitidle done; calling vkQueuePresentKHR (no wait-sem)...");

         VkPresentInfoKHR pinfo = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &idx };
         VkResult pr = pPresent(queue, &pinfo);
         if (frames == 0) LOG("P f0: present -> %d", pr);
         if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { LOG("present @frame %u -> %d", frames, pr); break; }

         armDCacheFlush(cpu, ext.width * ext.height * 4u);
         if (!logged) { LOG("P frame0: acquired idx=%u, center=0x%08x present=%d", idx, px[(ext.height/2)*ext.width + ext.width/2], pr); logged = 1; }

         /* blit the readback (swapchain image) to the libnx framebuffer. */
         u32 stride; u8 *base = (u8 *)framebufferBegin(&fb, &stride);
         for (u32 yy = 0; yy < SCR_H; yy++) {
            uint32_t *row = (uint32_t *)(base + (size_t)yy * stride);
            u32 sy = yy * ext.height / SCR_H;
            for (u32 xx = 0; xx < SCR_W; xx++) {
               uint32_t c = px[sy * ext.width + (xx * ext.width / SCR_W)];
               if (swap_rb) c = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
               row[xx] = c;
            }
         }
         framebufferEnd(&fb);
         frames++;
         if ((frames % 120) == 0) {
            uint64_t ns = armTicksToNs(armGetSystemTick() - t0);
            uint64_t fps = ns ? (uint64_t)frames * 1000000000ull / ns : 0;
            LOG("FPS: %u frames in %llu ms => ~%llu fps (swapchain acquire/present)", frames, (unsigned long long)(ns/1000000ull), (unsigned long long)fps);
         }
      }
      framebufferClose(&fb);
   }

done:
   (void)inst; (void)g_shot;
   LOG("=== done; exiting to HOME (svcExitProcess) ===");
   g_drm_shim_log_sink = NULL;
   if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; }
   svcExitProcess();
   return 0;  /* unreachable */
}
