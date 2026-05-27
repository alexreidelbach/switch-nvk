/*
 * nvk_smoke.c — headless NVK-on-Switch submit smoke test.
 *
 * Goal: exercise the whole M2 winsys path on real hardware, end to end, with no
 * display (WSI = M3). Staged + logged so a failure pinpoints the broken layer:
 *
 *   A vkCreateInstance                          (driver init)
 *   B vkEnumeratePhysicalDevices                (drmGetDevices2 -> open node ->
 *                                                drmGetVersion/NVIF -> our GM20B)
 *   C queue family query
 *   D vkCreateDevice + queue                    (NAK/meta init)
 *   E host-visible memory + map                 (GEM_NEW + mmap shim)
 *   F buffer + bind                             (VM_BIND -> nvAddressSpaceMapFixed)
 *   G record vkCmdFillBuffer(0xCAFEBABE)
 *   H vkQueueSubmit + vkQueueWaitIdle           (EXEC -> GPFIFO + nvFence)
 *   I read back + verify                        (GPU actually wrote memory)
 *
 * Runs as a full Application (not LibraryApplet) so all 4 cores + the big heap
 * are available. Logs to sdmc:/nvk_smoke.log (flushed every line, crash-safe)
 * and to stdout (visible live over `nxlink -s`).
 *
 * libnvk only exports vk_icdGetInstanceProcAddr, so we resolve every entrypoint
 * through it (loaderless ICD usage) — hence VK_NO_PROTOTYPES.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>   /* struct in_addr (for __nxlink_host) */
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* Full Application: grab all cores + heap (else LibraryApplet caps us hard). */
u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;   /* 0 => libnx grabs all available memory */

/* The one symbol the NVK ICD exports; everything else hangs off it. */
extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

/* drm_shim diagnostics (winsys/drm_shim.c). */
extern void (*g_drm_shim_log_sink)(const char *);
extern void drm_shim_selftest(void);
extern int  drmGetDevices2(uint32_t flags, void *devices[], int max_devices);
/* __nxlink_host (struct in_addr) is declared by <switch.h>; nonzero s_addr
 * means we were launched via nxlink. */

static FILE *g_log;

/* Sink so the drm_shim's internal trace lands in our (fetchable) log file. */
static void shim_log_sink(const char *s)
{
   if (g_log) { fputs(s, g_log); fflush(g_log); }
   printf("%s", s); fflush(stdout);
}

static void smoke_logf(const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
   printf("%s\n", buf);
   fflush(stdout);
}
#define LOG(...) smoke_logf(__VA_ARGS__)

#define FILL_VALUE 0xCAFEBABEu
#define FILL_BYTES 4096u

static uint32_t pick_mem_type(const VkPhysicalDeviceMemoryProperties *mp,
                              uint32_t type_bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) &&
          (mp->memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   return UINT32_MAX;
}

int main(void)
{
   g_log = fopen("sdmc:/nvk_smoke.log", "w");
   /* If launched via `nxlink` (host set), mirror stdout to it for a LIVE log —
    * fully remote, no FTP/Sphaira needed. Guarded on __nxlink_host so a Sphaira
    * menu launch (no host) doesn't touch sockets (that crashed). */
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
      nxlinkStdio();
   LOG("=== NVK headless smoke test (M2 winsys) [BUILD v32 fence-cmdlist] ===");

   /* Route drm_shim's trace into this same log, then prove the shim is linked
    * + reachable + logging works, independently of NVK's enumeration. */
   g_drm_shim_log_sink = shim_log_sink;
   LOG("-- drm_shim selftest --");
   drm_shim_selftest();
   LOG("app: &drmGetDevices2=%p", (void *)drmGetDevices2);
   LOG("-- drm_shim selftest done --");

   /* GM20B is SOC/non-conformant; NVK refuses device creation otherwise. */
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);   /* avoid disk-cache fs churn */
   /* Capture NVK/Mesa's own error messages (vk_errorf -> mesa_log) to a file. */
   setenv("MESA_LOG_FILE", "sdmc:/nvk_mesa.log", 1);

   PFN_vkCreateInstance pCreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("FAIL A: vk_icdGetInstanceProcAddr(vkCreateInstance) = NULL"); goto done; }

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_smoke",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance inst = VK_NULL_HANDLE;
   VkResult r = pCreateInstance(&ici, NULL, &inst);
   LOG("A vkCreateInstance -> %d", r);
   if (r != VK_SUCCESS) goto done;

#define GI(n) ((PFN_##n)vk_icdGetInstanceProcAddr(inst, #n))
   PFN_vkEnumeratePhysicalDevices          pEnum   = GI(vkEnumeratePhysicalDevices);
   PFN_vkGetPhysicalDeviceProperties       pProps  = GI(vkGetPhysicalDeviceProperties);
   PFN_vkGetPhysicalDeviceMemoryProperties pMemP   = GI(vkGetPhysicalDeviceMemoryProperties);
   PFN_vkGetPhysicalDeviceQueueFamilyProperties pQF = GI(vkGetPhysicalDeviceQueueFamilyProperties);
   PFN_vkCreateDevice                      pCreateDev = GI(vkCreateDevice);
   PFN_vkGetDeviceProcAddr                 pGDPA   = GI(vkGetDeviceProcAddr);
   if (!pEnum || !pCreateDev || !pGDPA) { LOG("FAIL: missing instance entrypoints"); goto done; }

   uint32_t ndev = 0;
   r = pEnum(inst, &ndev, NULL);
   LOG("B vkEnumeratePhysicalDevices count -> %d (r=%d)", ndev, r);
   if (r != VK_SUCCESS || ndev == 0) { LOG("FAIL B: no physical devices"); goto done; }

   VkPhysicalDevice phys[4];
   if (ndev > 4) ndev = 4;
   r = pEnum(inst, &ndev, phys);
   if (r != VK_SUCCESS) { LOG("FAIL B: enumerate(2) r=%d", r); goto done; }

   VkPhysicalDeviceProperties pdp;
   pProps(phys[0], &pdp);
   LOG("B device[0]: '%s' apiVer=%u.%u.%u type=%d vendor=0x%x dev=0x%x",
       pdp.deviceName,
       VK_VERSION_MAJOR(pdp.apiVersion), VK_VERSION_MINOR(pdp.apiVersion),
       VK_VERSION_PATCH(pdp.apiVersion), pdp.deviceType,
       pdp.vendorID, pdp.deviceID);

   uint32_t nqf = 0;
   pQF(phys[0], &nqf, NULL);
   VkQueueFamilyProperties qf[8];
   if (nqf > 8) nqf = 8;
   pQF(phys[0], &nqf, qf);
   uint32_t qfi = UINT32_MAX;
   for (uint32_t i = 0; i < nqf; i++) {
      LOG("C qfam[%u]: flags=0x%x count=%u", i, qf[i].queueFlags, qf[i].queueCount);
      if (qfi == UINT32_MAX &&
          (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)))
         qfi = i;
   }
   if (qfi == UINT32_MAX) { LOG("FAIL C: no usable queue family"); goto done; }
   LOG("C using queue family %u", qfi);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
   };
   VkDevice dev = VK_NULL_HANDLE;
   r = pCreateDev(phys[0], &dci, NULL, &dev);
   LOG("D vkCreateDevice -> %d", r);
   if (r != VK_SUCCESS) goto done;

#define GD(n) ((PFN_##n)pGDPA(dev, #n))
   PFN_vkGetDeviceQueue          pGetQueue = GD(vkGetDeviceQueue);
   PFN_vkAllocateMemory          pAlloc    = GD(vkAllocateMemory);
   PFN_vkMapMemory               pMap      = GD(vkMapMemory);
   PFN_vkCreateBuffer            pCreateBuf= GD(vkCreateBuffer);
   PFN_vkGetBufferMemoryRequirements pBufReq = GD(vkGetBufferMemoryRequirements);
   PFN_vkBindBufferMemory        pBind     = GD(vkBindBufferMemory);
   PFN_vkCreateCommandPool       pCreatePool = GD(vkCreateCommandPool);
   PFN_vkAllocateCommandBuffers  pAllocCB  = GD(vkAllocateCommandBuffers);
   PFN_vkBeginCommandBuffer      pBegin    = GD(vkBeginCommandBuffer);
   PFN_vkCmdFillBuffer           pFill     = GD(vkCmdFillBuffer);
   PFN_vkEndCommandBuffer        pEnd      = GD(vkEndCommandBuffer);
   PFN_vkQueueSubmit             pSubmit   = GD(vkQueueSubmit);
   PFN_vkQueueWaitIdle           pWaitIdle = GD(vkQueueWaitIdle);
   if (!pGetQueue || !pAlloc || !pMap || !pCreateBuf || !pBind ||
       !pCreatePool || !pAllocCB || !pFill || !pSubmit || !pWaitIdle) {
      LOG("FAIL D: missing device entrypoints"); goto done;
   }

   VkQueue queue;
   pGetQueue(dev, qfi, 0, &queue);

   /* F: buffer + memory. */
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = FILL_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buf = VK_NULL_HANDLE;
   r = pCreateBuf(dev, &bci, NULL, &buf);
   LOG("F vkCreateBuffer -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkMemoryRequirements mr;
   pBufReq(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp;
   pMemP(phys[0], &mp);
   uint32_t mt = pick_mem_type(&mp, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   LOG("E mem types=%u, chosen host-visible|coherent type=%d (reqBits=0x%x size=%llu)",
       mp.memoryTypeCount, (int)mt, mr.memoryTypeBits, (unsigned long long)mr.size);
   if (mt == UINT32_MAX) { LOG("FAIL E: no host-visible|coherent memory type"); goto done; }

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = mt,
   };
   VkDeviceMemory mem = VK_NULL_HANDLE;
   r = pAlloc(dev, &mai, NULL, &mem);
   LOG("E vkAllocateMemory -> %d", r);
   if (r != VK_SUCCESS) goto done;

   void *cpu = NULL;
   r = pMap(dev, mem, 0, VK_WHOLE_SIZE, 0, &cpu);
   LOG("E vkMapMemory -> %d (ptr=%p)", r, cpu);
   if (r != VK_SUCCESS || !cpu) goto done;
   memset(cpu, 0, FILL_BYTES);   /* clear so the fill is observable */

   r = pBind(dev, buf, mem, 0);
   LOG("F vkBindBufferMemory -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* G: command buffer with a single fill. */
   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = qfi,
   };
   VkCommandPool pool;
   r = pCreatePool(dev, &pci, NULL, &pool);
   LOG("G vkCreateCommandPool -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb;
   r = pAllocCB(dev, &cbai, &cb);
   LOG("G vkAllocateCommandBuffers -> %d", r);
   if (r != VK_SUCCESS) goto done;

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   r = pBegin(cb, &cbbi);
   LOG("G vkBeginCommandBuffer -> %d", r);
   if (r != VK_SUCCESS) goto done;

   pFill(cb, buf, 0, FILL_BYTES, FILL_VALUE);

   r = pEnd(cb);
   LOG("G vkEndCommandBuffer -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* H: submit + wait (this is the EXEC -> GPFIFO -> nvFence path). */
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cb,
   };
   r = pSubmit(queue, 1, &si, VK_NULL_HANDLE);
   LOG("H vkQueueSubmit -> %d", r);
   if (r != VK_SUCCESS) goto done;

   r = pWaitIdle(queue);
   LOG("H vkQueueWaitIdle -> %d", r);
   if (r != VK_SUCCESS) goto done;

   /* The GM20B is not IO-coherent with the CPU and our host-visible BO is mapped
    * CPU-cacheable, so invalidate the CPU cache for the range before readback —
    * otherwise we'd read stale (pre-fill) cache lines even though the GPU (after
    * the in-stream L2 flush) wrote 0xCAFEBABE to memory.  (A real HOST_COHERENT
    * heap should instead be CPU-uncached in the winsys; this proves the path.) */
   armDCacheFlush(cpu, FILL_BYTES);

   /* I: did the GPU actually write 0xCAFEBABE? */
   uint32_t *words = (uint32_t *)cpu;
   uint32_t bad = 0, first_bad = 0;
   for (uint32_t i = 0; i < FILL_BYTES / 4; i++) {
      if (words[i] != FILL_VALUE) {
         if (!bad) first_bad = i;
         bad++;
      }
   }
   if (bad == 0) {
      LOG("I VERIFY OK: all %u words == 0x%08x", FILL_BYTES / 4, FILL_VALUE);
      LOG("=== SMOKE TEST PASSED — NVK rendered to memory on Tegra ===");
   } else {
      LOG("I VERIFY FAIL: %u/%u words wrong; first bad word[%u]=0x%08x",
          bad, FILL_BYTES / 4, first_bad, words[first_bad]);
      LOG("=== SMOKE TEST FAILED at verify (submit ran, GPU write didn't land) ===");
   }

done:
   /* Tear the instance down so libnx's process exit doesn't fault on live NVK
    * state — this is what crashes ("software closed") and blocks fully-remote
    * nxlink runs. Destroying it returns to Sphaira cleanly. */
   if (inst != VK_NULL_HANDLE) {
      PFN_vkDestroyInstance pDestroyInstance =
         (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(inst, "vkDestroyInstance");
      if (pDestroyInstance)
         pDestroyInstance(inst, NULL);
      LOG("cleanup: vkDestroyInstance done");
   }

   LOG("=== done; log at sdmc:/nvk_smoke.log ===");
   if (g_log) fclose(g_log);
   return 0;
}
