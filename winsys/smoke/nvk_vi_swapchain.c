/*
 * nvk_vi_swapchain.c — REAL VkSwapchain on the Switch via our own wsi_common_switch.c
 * (VK_NN_vi_surface over libnx nwindow). Mirrors Dan/Tiicu's TriangleTest.cpp flow:
 *   instance(+VK_KHR_surface,+VK_NN_vi_surface) -> vkCreateViSurfaceNN(nwindowGetDefault())
 *   -> device(+VK_KHR_swapchain) -> vkCreateSwapchainKHR -> acquire/clear/present loop.
 *
 * First test: clear each acquired swapchain image to a cycling colour and present it
 * via the REAL swapchain (NO manual framebuffer blit — the present path goes to the
 * VI compositor through our wsi_common_switch backend). If the TV cycles colour, the
 * whole VI surface + swapchain + present path works on HW.
 *
 * Loaderless ICD (VK_NO_PROTOTYPES); logs sdmc:/nvk_vi_swapchain.log.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <netinet/in.h>
#include <switch.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
extern void (*g_drm_shim_log_sink)(const char *);

#define MAX_IMG 8u

static FILE *g_log;
static void shim_log_sink(const char *s){ if (g_log){fputs(s,g_log);fflush(g_log);} printf("%s",s); fflush(stdout); }
static void slogf(const char *fmt, ...)
{
   char b[512]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
   if (g_log){fputs(b,g_log);fputc('\n',g_log);fflush(g_log);} printf("%s\n",b); fflush(stdout);
}
#define LOG(...) slogf(__VA_ARGS__)

int main(void)
{
   g_log = fopen("sdmc:/nvk_vi_swapchain.log", "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault())) nxlinkStdio();
   LOG("=== NVK VI swapchain (real VK_NN_vi_surface over nwindow) [BUILD vi1] ===");

   g_drm_shim_log_sink = shim_log_sink;
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
   setenv("MESA_LOG_FILE", "sdmc:/nvk_vi_swapchain_mesa.log", 1);

   VkInstance inst = VK_NULL_HANDLE;
   PFN_vkCreateInstance pCreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("FAIL: no vkCreateInstance"); goto done; }
   const char *iexts[] = { "VK_KHR_surface", "VK_NN_vi_surface" };
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nvk_vi_swapchain", .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
      .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts };
   VkResult r = pCreateInstance(&ici, NULL, &inst);
   LOG("A vkCreateInstance (+surface,+vi) -> %d", r); if (r) goto done;

#define GI(n) ((PFN_##n)vk_icdGetInstanceProcAddr(inst, #n))
   PFN_vkEnumeratePhysicalDevices pEnum = GI(vkEnumeratePhysicalDevices);
   PFN_vkGetPhysicalDeviceQueueFamilyProperties pQF = GI(vkGetPhysicalDeviceQueueFamilyProperties);
   PFN_vkCreateDevice pCreateDev = GI(vkCreateDevice);
   PFN_vkGetDeviceProcAddr pGDPA = GI(vkGetDeviceProcAddr);
   PFN_vkCreateViSurfaceNN pCreateVi = GI(vkCreateViSurfaceNN);
   PFN_vkGetPhysicalDeviceSurfaceSupportKHR pSupp = GI(vkGetPhysicalDeviceSurfaceSupportKHR);
   PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pCaps = GI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
   PFN_vkGetPhysicalDeviceSurfaceFormatsKHR pFmts = GI(vkGetPhysicalDeviceSurfaceFormatsKHR);
   PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pModes = GI(vkGetPhysicalDeviceSurfacePresentModesKHR);
   if (!pEnum||!pCreateDev||!pGDPA) { LOG("FAIL: instance fns"); goto done; }
   if (!pCreateVi) { LOG("FAIL: no vkCreateViSurfaceNN (VI WSI not wired?)"); goto done; }

   uint32_t ndev = 1; VkPhysicalDevice phys[4];
   r = pEnum(inst, &ndev, NULL); if (r || !ndev) { LOG("FAIL B"); goto done; }
   ndev = ndev > 4 ? 4 : ndev; pEnum(inst, &ndev, phys);
   LOG("B enumerate -> %u dev", ndev);
   uint32_t nqf = 0; pQF(phys[0], &nqf, NULL); VkQueueFamilyProperties qf[8]; nqf = nqf>8?8:nqf; pQF(phys[0],&nqf,qf);
   uint32_t qfi = UINT32_MAX; for (uint32_t i=0;i<nqf;i++) if (qfi==UINT32_MAX && (qf[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) qfi=i;
   if (qfi==UINT32_MAX) { LOG("FAIL C"); goto done; }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex=qfi, .queueCount=1, .pQueuePriorities=&prio };
   const char *dexts[] = { "VK_KHR_swapchain" };
   VkDeviceCreateInfo dci = { .sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount=1, .pQueueCreateInfos=&qci, .enabledExtensionCount=1, .ppEnabledExtensionNames=dexts };
   VkDevice dev = VK_NULL_HANDLE; r = pCreateDev(phys[0], &dci, NULL, &dev);
   LOG("D vkCreateDevice (+swapchain) -> %d", r); if (r) goto done;

#define GD(n) ((PFN_##n)pGDPA(dev, #n))
   PFN_vkGetDeviceQueue pGetQ = GD(vkGetDeviceQueue);
   PFN_vkCreateCommandPool pCreatePool = GD(vkCreateCommandPool);
   PFN_vkAllocateCommandBuffers pAllocCB = GD(vkAllocateCommandBuffers);
   PFN_vkResetCommandBuffer pResetCB = GD(vkResetCommandBuffer);
   PFN_vkBeginCommandBuffer pBegin = GD(vkBeginCommandBuffer);
   PFN_vkCmdPipelineBarrier pBarrier = GD(vkCmdPipelineBarrier);
   PFN_vkCmdClearColorImage pClear = GD(vkCmdClearColorImage);
   PFN_vkEndCommandBuffer pEnd = GD(vkEndCommandBuffer);
   PFN_vkQueueSubmit pSubmit = GD(vkQueueSubmit);
   PFN_vkQueueWaitIdle pWaitIdle = GD(vkQueueWaitIdle);
   PFN_vkCreateSemaphore pCreateSem = GD(vkCreateSemaphore);
   PFN_vkCreateSwapchainKHR pCreateSC = GD(vkCreateSwapchainKHR);
   PFN_vkGetSwapchainImagesKHR pGetImgs = GD(vkGetSwapchainImagesKHR);
   PFN_vkAcquireNextImageKHR pAcquire = GD(vkAcquireNextImageKHR);
   PFN_vkQueuePresentKHR pPresent = GD(vkQueuePresentKHR);
   if (!pGetQ||!pCreatePool||!pAllocCB||!pBegin||!pBarrier||!pClear||!pEnd||!pSubmit||!pWaitIdle||!pCreateSem) { LOG("FAIL: device fns"); goto done; }
   if (!pCreateSC||!pGetImgs||!pAcquire||!pPresent) { LOG("FAIL: swapchain fns"); goto done; }
   VkQueue queue; pGetQ(dev, qfi, 0, &queue);

   /* S: VI surface + swapchain */
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   VkViSurfaceCreateInfoNN vci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = nwindowGetDefault() };
   r = pCreateVi(inst, &vci, NULL, &surface);
   LOG("S vkCreateViSurfaceNN -> %d", r); if (r) goto done;
   if (pSupp) { VkBool32 sup=0; pSupp(phys[0], qfi, surface, &sup); LOG("S surface support qfam %u = %d", qfi, sup); }
   VkSurfaceCapabilitiesKHR caps; r = pCaps ? pCaps(phys[0], surface, &caps) : VK_ERROR_UNKNOWN;
   if (r) { LOG("FAIL S: caps %d", r); goto done; }
   VkExtent2D ext = caps.currentExtent;
   if (ext.width==0xFFFFFFFFu){ ext.width=1280; ext.height=720; }
   uint32_t nfmt=0; if(pFmts) pFmts(phys[0],surface,&nfmt,NULL);
   VkSurfaceFormatKHR fmts[8]; nfmt=nfmt>8?8:nfmt; if(pFmts) pFmts(phys[0],surface,&nfmt,fmts);
   VkFormat scfmt = nfmt ? fmts[0].format : VK_FORMAT_R8G8B8A8_UNORM;
   VkColorSpaceKHR sccs = nfmt ? fmts[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   LOG("S caps minImg=%u maxImg=%u extent=%ux%u fmts=%u fmt=%d", caps.minImageCount, caps.maxImageCount, ext.width, ext.height, nfmt, scfmt);

   uint32_t want = caps.minImageCount; if (want < 2) want = 2;
   VkSwapchainCreateInfoKHR scci = { .sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface=surface,
      .minImageCount=want, .imageFormat=scfmt, .imageColorSpace=sccs, .imageExtent=ext, .imageArrayLayers=1,
      .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE, .preTransform=caps.currentTransform,
      .compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode=VK_PRESENT_MODE_FIFO_KHR, .clipped=VK_TRUE };
   VkSwapchainKHR sc = VK_NULL_HANDLE; r = pCreateSC(dev, &scci, NULL, &sc);
   LOG("S vkCreateSwapchainKHR (fmt=%d ext=%ux%u img=%u) -> %d", scfmt, ext.width, ext.height, want, r);
   if (r) goto done;
   uint32_t nimg=0; pGetImgs(dev, sc, &nimg, NULL); VkImage img[MAX_IMG]; nimg=nimg>MAX_IMG?MAX_IMG:nimg; pGetImgs(dev, sc, &nimg, img);
   LOG("S swapchain images = %u", nimg);

   VkCommandPoolCreateInfo pci = { .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex=qfi };
   VkCommandPool pool; r = pCreatePool(dev,&pci,NULL,&pool); if(r){LOG("FAIL pool %d",r);goto done;}
   VkCommandBuffer cmd[MAX_IMG];
   VkCommandBufferAllocateInfo cbai = { .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=nimg };
   r = pAllocCB(dev,&cbai,cmd); if(r){LOG("FAIL allocCB %d",r);goto done;}
   VkSemaphore acquireSem, renderSem; VkSemaphoreCreateInfo semci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   pCreateSem(dev,&semci,NULL,&acquireSem); pCreateSem(dev,&semci,NULL,&renderSem);
   LOG("=== entering acquire/clear/present loop ===");

   PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
   uint32_t frame=0; int logged=0;
   while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) { if(g_log){fflush(g_log);fclose(g_log);g_log=NULL;} svcExitProcess(); }

      uint32_t idx=0;
      VkResult ar = pAcquire(dev, sc, UINT64_MAX, acquireSem, VK_NULL_HANDLE, &idx);
      if (frame==0) LOG("P f0: acquire -> %d idx=%u", ar, idx);
      if (ar!=VK_SUCCESS && ar!=VK_SUBOPTIMAL_KHR) { LOG("acquire @%u -> %d", frame, ar); break; }

      /* cycling colour */
      float t = (frame % 180) / 180.0f;
      VkClearColorValue col = { .float32 = { 0.5f+0.5f*sinf(t*6.283f), t, 1.0f-t, 1.0f } };

      pResetCB(cmd[idx], 0);
      VkCommandBufferBeginInfo bi = { .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
      pBegin(cmd[idx], &bi);
      VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 };
      VkImageMemoryBarrier toClear = { .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED, .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .image=img[idx], .subresourceRange=rng };
      pBarrier(cmd[idx], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,NULL,0,NULL,1,&toClear);
      pClear(cmd[idx], img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
      VkImageMemoryBarrier toPresent = { .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .image=img[idx], .subresourceRange=rng };
      pBarrier(cmd[idx], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,0,NULL,0,NULL,1,&toPresent);
      pEnd(cmd[idx]);

      VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo si = { .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount=1, .pWaitSemaphores=&acquireSem, .pWaitDstStageMask=&ws,
         .commandBufferCount=1, .pCommandBuffers=&cmd[idx], .signalSemaphoreCount=1, .pSignalSemaphores=&renderSem };
      r = pSubmit(queue, 1, &si, VK_NULL_HANDLE);
      if (frame==0) LOG("P f0: submit -> %d", r);
      if (r) { LOG("submit @%u -> %d", frame, r); break; }
      pWaitIdle(queue);

      VkPresentInfoKHR pinfo = { .sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount=1, .pWaitSemaphores=&renderSem,
         .swapchainCount=1, .pSwapchains=&sc, .pImageIndices=&idx };
      VkResult pr = pPresent(queue, &pinfo);
      if (frame==0) LOG("P f0: present -> %d", pr);
      if (pr!=VK_SUCCESS && pr!=VK_SUBOPTIMAL_KHR) { LOG("present @%u -> %d", frame, pr); break; }
      pWaitIdle(queue);
      if (!logged) { LOG("=== first frame presented via VI swapchain — cycling colour on TV ==="); logged=1; }
      frame++;
      if ((frame % 120)==0) LOG("frames=%u", frame);
   }

done:
   (void)inst;
   LOG("=== done; svcExitProcess ===");
   g_drm_shim_log_sink = NULL;
   if (g_log){fflush(g_log);fclose(g_log);g_log=NULL;}
   svcExitProcess();
   return 0;
}
