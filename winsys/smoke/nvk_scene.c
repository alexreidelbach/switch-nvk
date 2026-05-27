/*
 * nvk_scene.c — M3 step 3: a 3D "Vulkan scene" on the Switch GM20B, rendered by
 * our own NVK. A textured 3D CUBE (the "VULKAN" logo on each face) drawn with a
 * real graphics pipeline: a vertex buffer, an MVP matrix in a uniform buffer, a
 * depth buffer, and a descriptor set (sampler + UBO). This is my own scene,
 * inspired by the open-source Vulkan Examples (c) Sascha Willems — not his code
 * or assets; a self-contained showcase of the driver in 3D.
 *
 *   A..D  instance / device / queue
 *   T  logo texture (upload) + sampler
 *   U  cube vertex buffer + MVP uniform buffer + descriptor set (binding0=tex,1=ubo)
 *   Z  depth image
 *   N  3D pipeline (scene.vert/scene.frag, vertex input + depth test)
 *   O  render pass (color+depth): clear, bind, vkCmdDraw(36), end
 *   L/M copy color -> buffer, read back (verify red+white present)
 *   P  present on the TV (upscaled), until +
 *
 * Loaderless ICD (VK_NO_PROTOTYPES); logs sdmc:/nvk_scene.log.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <netinet/in.h>
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "shaders/tri_shaders.h"   /* scene_vert_spv[], scene_frag_spv[] + _sz */

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
extern void (*g_drm_shim_log_sink)(const char *);

#define TEX_W 256u
#define TEX_H 256u
#define TEX_PIXELS (TEX_W * TEX_H)
#define TEX_BYTES (TEX_PIXELS * 4u)
#define SCN_W 512u
#define SCN_H 288u
#define SCN_PIXELS (SCN_W * SCN_H)
#define SCN_BYTES (SCN_PIXELS * 4u)
#define SCR_W 1280u
#define SCR_H 720u
#define COL_RED   0xFF1E14C8u
#define COL_WHITE 0xFFFFFFFFu

static FILE *g_log;
static uint32_t g_shot[SCN_PIXELS];
static uint32_t g_tex[TEX_PIXELS];
static float    g_verts[36 * 5];   /* 36 verts * (x,y,z,u,v) */

static const uint8_t GLYPHS[6][8] = {
   { 0x82,0x82,0x82,0x82,0x44,0x44,0x28,0x10 }, /* V */
   { 0x82,0x82,0x82,0x82,0x82,0x82,0x82,0x7C }, /* U */
   { 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0xFE }, /* L */
   { 0x84,0x88,0x90,0xE0,0x90,0x88,0x84,0x82 }, /* K */
   { 0x10,0x28,0x44,0x82,0xFE,0x82,0x82,0x82 }, /* A */
   { 0x82,0xC2,0xA2,0x92,0x8A,0x86,0x82,0x82 }, /* N */
};
static void put_block(int x0, int y0, int s, uint32_t c)
{
   for (int dy = 0; dy < s; dy++) for (int dx = 0; dx < s; dx++) {
      int x = x0 + dx, y = y0 + dy;
      if (x >= 0 && x < (int)TEX_W && y >= 0 && y < (int)TEX_H) g_tex[y * TEX_W + x] = c;
   }
}
static void gen_tex(void)
{
   for (uint32_t i = 0; i < TEX_PIXELS; i++) g_tex[i] = COL_RED;
   const int s = 4, gw = 8 * s, gap = 2 * s, total = 6 * gw + 5 * gap;
   int x = ((int)TEX_W - total) / 2, y = ((int)TEX_H - gw) / 2;
   for (int g = 0; g < 6; g++) {
      for (int row = 0; row < 8; row++) for (int col = 0; col < 8; col++)
         if (GLYPHS[g][row] & (0x80u >> col)) put_block(x + col * s, y + row * s, s, COL_WHITE);
      x += gw + gap;
   }
}

/* --- tiny column-major 4x4 matrix math (Vulkan clip, depth 0..1, Y-flipped) --- */
static void m_mul(float *r, const float *a, const float *b)
{
   float t[16];
   for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
      float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
      t[c * 4 + rr] = s;
   }
   memcpy(r, t, sizeof t);
}
static void m_ident(float *m){ memset(m, 0, 64); m[0]=m[5]=m[10]=m[15]=1.0f; }
static void m_persp(float *m, float fovy, float aspect, float n, float f)
{
   float t = 1.0f / tanf(fovy * 0.5f);
   memset(m, 0, 64);
   m[0]  = t / aspect;
   m[5]  = -t;                 /* flip Y for Vulkan's top-left origin */
   m[10] = f / (n - f);
   m[11] = -1.0f;
   m[14] = (n * f) / (n - f);
}
static void m_rotY(float *m, float a){ m_ident(m); float c=cosf(a),s=sinf(a); m[0]=c; m[2]=-s; m[8]=s; m[10]=c; }
static void m_rotX(float *m, float a){ m_ident(m); float c=cosf(a),s=sinf(a); m[5]=c; m[6]=s; m[9]=-s; m[10]=c; }
static void m_trans(float *m, float x, float y, float z){ m_ident(m); m[12]=x; m[13]=y; m[14]=z; }

static void gen_cube(void)
{
   static const float CV[8][3] = {
      {-0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f},{0.5f,0.5f,-0.5f},{-0.5f,0.5f,-0.5f},
      {-0.5f,-0.5f, 0.5f},{0.5f,-0.5f, 0.5f},{0.5f,0.5f, 0.5f},{-0.5f,0.5f, 0.5f},
   };
   static const int F[6][4] = {
      {1,0,3,2},{4,5,6,7},{0,4,7,3},{5,1,2,6},{4,0,1,5},{3,7,6,2},
   };
   static const float UV[4][2] = { {0,1},{1,1},{1,0},{0,0} };
   static const int TRI[6] = { 0,1,2, 0,2,3 };
   int v = 0;
   for (int f = 0; f < 6; f++)
      for (int t = 0; t < 6; t++) {
         int corner = TRI[t];
         const float *p = CV[F[f][corner]];
         g_verts[v*5+0] = p[0]; g_verts[v*5+1] = p[1]; g_verts[v*5+2] = p[2];
         g_verts[v*5+3] = UV[corner][0]; g_verts[v*5+4] = UV[corner][1];
         v++;
      }
}

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

static void present_shot(void)
{
   PadState pad;
   padConfigureInput(1, HidNpadStyleSet_NpadStandard);
   padInitializeDefault(&pad);
   NWindow *win = nwindowGetDefault();
   Framebuffer fb;
   framebufferCreate(&fb, win, SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 2);
   framebufferMakeLinear(&fb);
   LOG("P: presenting on screen (%ux%u -> %ux%u); press + to exit", SCN_W, SCN_H, SCR_W, SCR_H);
   while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
      u32 stride;
      u8 *base = (u8 *)framebufferBegin(&fb, &stride);
      for (u32 yy = 0; yy < SCR_H; yy++) {
         uint32_t *row = (uint32_t *)(base + (size_t)yy * stride);
         u32 sy = yy * SCN_H / SCR_H;
         for (u32 xx = 0; xx < SCR_W; xx++) row[xx] = g_shot[sy * SCN_W + (xx * SCN_W / SCR_W)];
      }
      framebufferEnd(&fb);
   }
   framebufferClose(&fb);
}

int main(void)
{
   int have_shot = 0;
   g_log = fopen("sdmc:/nvk_scene.log", "w");
   if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
      nxlinkStdio();
   LOG("=== NVK 3D scene (M3 3: textured cube; inspired by Vulkan Examples (c) Sascha Willems) [BUILD scene1] ===");
   gen_tex();
   gen_cube();

   g_drm_shim_log_sink = shim_log_sink;
   setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
   setenv("MESA_LOG_FILE", "sdmc:/nvk_scene_mesa.log", 1);

   VkInstance inst = VK_NULL_HANDLE;
   PFN_vkCreateInstance pCreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!pCreateInstance) { LOG("FAIL: no vkCreateInstance"); goto done; }
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nvk_scene", .apiVersion = VK_API_VERSION_1_1 };
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
   PFN_vkCmdDraw pDraw = GD(vkCmdDraw);
   PFN_vkCmdEndRenderPass pEndRP = GD(vkCmdEndRenderPass);
   PFN_vkCmdCopyImageToBuffer pCopyIB = GD(vkCmdCopyImageToBuffer);
   PFN_vkEndCommandBuffer pEnd = GD(vkEndCommandBuffer);
   PFN_vkQueueSubmit pSubmit = GD(vkQueueSubmit);
   PFN_vkQueueWaitIdle pWaitIdle = GD(vkQueueWaitIdle);
   if (!pGetQueue||!pAlloc||!pMap||!pCreateImg||!pBindImg||!pCreateView||!pCreateSamp||
       !pCreateRP||!pCreateFB||!pCreateSM||!pCreateDSL||!pCreateDP||!pAllocDS||!pUpdateDS||
       !pCreatePL||!pCreateGP||!pCreateBuf||!pBindBuf||!pCreatePool||!pAllocCB||!pBarrier||
       !pCopyBI||!pBeginRP||!pBindPipe||!pBindDS||!pBindVB||!pDraw||!pEndRP||!pCopyIB||
       !pSubmit||!pWaitIdle) { LOG("FAIL D: device fns"); goto done; }
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

   /* T: logo texture + staging upload + sampler. */
   VkImageCreateInfo tii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { TEX_W, TEX_H, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage tex = VK_NULL_HANDLE; r = pCreateImg(dev, &tii, NULL, &tex);
   if (r) { LOG("FAIL T: tex img %d", r); goto done; }
   VkDeviceMemory tmem = VK_NULL_HANDLE; ALLOC_IMG(tex, tmem, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VkBuffer staging; VkDeviceMemory smem; void *sp;
   MK_HOST_BUF(staging, smem, sp, TEX_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   memcpy(sp, g_tex, TEX_BYTES); armDCacheFlush(sp, TEX_BYTES);
   VkImageViewCreateInfo tvci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = tex,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView tview = VK_NULL_HANDLE; r = pCreateView(dev, &tvci, NULL, &tview);
   if (r) { LOG("FAIL T: texview %d", r); goto done; }
   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 1.0f };
   VkSampler samp = VK_NULL_HANDLE; r = pCreateSamp(dev, &sci, NULL, &samp);
   LOG("T texture + sampler -> %d", r); if (r) goto done;

   /* U: vertex buffer (cube) + uniform buffer (MVP). */
   VkBuffer vbuf; VkDeviceMemory vmem; void *vp;
   MK_HOST_BUF(vbuf, vmem, vp, sizeof g_verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   memcpy(vp, g_verts, sizeof g_verts); armDCacheFlush(vp, sizeof g_verts);
   VkBuffer ubuf; VkDeviceMemory umem; void *up;
   MK_HOST_BUF(ubuf, umem, up, 64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   {  float P[16], V[16], Mx[16], My[16], M[16], VM[16], MVP[16];
      m_persp(P, 1.0f /*~57deg*/, (float)SCN_W / (float)SCN_H, 0.1f, 10.0f);
      m_trans(V, 0.0f, 0.0f, -2.4f);
      m_rotY(My, 0.6f); m_rotX(Mx, 0.5f); m_mul(M, My, Mx);
      m_mul(VM, V, M); m_mul(MVP, P, VM);
      memcpy(up, MVP, 64); armDCacheFlush(up, 64);
   }
   LOG("U cube vbuf (%u verts) + MVP ubo ready", (unsigned)(sizeof g_verts / 20));

   /* descriptor set layout: binding0 = sampler (frag), binding1 = UBO (vertex). */
   VkDescriptorSetLayoutBinding binds[2] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT },
   };
   VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2, .pBindings = binds };
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE; r = pCreateDSL(dev, &dslci, NULL, &dsl);
   if (r) { LOG("FAIL U: dsl %d", r); goto done; }
   VkDescriptorPoolSize psz[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
   VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = psz };
   VkDescriptorPool dpool = VK_NULL_HANDLE; r = pCreateDP(dev, &dpci, NULL, &dpool);
   if (r) { LOG("FAIL U: dpool %d", r); goto done; }
   VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet dset = VK_NULL_HANDLE; r = pAllocDS(dev, &dsai, &dset);
   LOG("U descriptor set -> %d", r); if (r) goto done;
   VkDescriptorImageInfo dii = { .sampler = samp, .imageView = tview, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
   VkDescriptorBufferInfo dbi = { .buffer = ubuf, .offset = 0, .range = 64 };
   VkWriteDescriptorSet wr2[2] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi },
   };
   pUpdateDS(dev, 2, wr2, 0, NULL);

   /* Z: depth image + view (D32 ZETA). RE-ENABLED — with the VM_BIND kind fix the
    * winsys now maps this block-linear with kind ZF32 (0x7b), so the GM20B ZETA
    * unit + CLEAR_SURFACE(Z) work. */
   VkImageCreateInfo zii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT, .extent = { SCN_W, SCN_H, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage zimg = VK_NULL_HANDLE; r = pCreateImg(dev, &zii, NULL, &zimg);
   if (r) { LOG("FAIL Z: depth img %d", r); goto done; }
   VkDeviceMemory zmem = VK_NULL_HANDLE; ALLOC_IMG(zimg, zmem, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VkImageViewCreateInfo zvci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = zimg,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
      .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
   VkImageView zview = VK_NULL_HANDLE; r = pCreateView(dev, &zvci, NULL, &zview);
   LOG("Z depth image -> %d", r); if (r) goto done;

   /* render target (color) image + view. */
   VkImageCreateInfo rii = tii; rii.extent = (VkExtent3D){ SCN_W, SCN_H, 1 };
   rii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage rt = VK_NULL_HANDLE; r = pCreateImg(dev, &rii, NULL, &rt);
   if (r) { LOG("FAIL N: rt %d", r); goto done; }
   VkDeviceMemory rmem = VK_NULL_HANDLE; ALLOC_IMG(rt, rmem, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VkImageViewCreateInfo rvci = tvci; rvci.image = rt;
   VkImageView rview = VK_NULL_HANDLE; r = pCreateView(dev, &rvci, NULL, &rview);
   if (r) { LOG("FAIL N: rtview %d", r); goto done; }

   /* render pass: color + depth. */
   VkAttachmentDescription atts[2] = {
      { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
      { .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
   };
   VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkAttachmentReference zref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &zref };
   VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
   VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2, .pAttachments = atts, .subpassCount = 1, .pSubpasses = &sub,
      .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp = VK_NULL_HANDLE; r = pCreateRP(dev, &rpci, NULL, &rp);
   if (r) { LOG("FAIL N: renderpass %d", r); goto done; }
   VkImageView fbatt[2] = { rview, zview };
   VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = rp, .attachmentCount = 2, .pAttachments = fbatt, .width = SCN_W, .height = SCN_H, .layers = 1 };
   VkFramebuffer fbuf = VK_NULL_HANDLE; r = pCreateFB(dev, &fbci, NULL, &fbuf);
   if (r) { LOG("FAIL N: framebuffer %d", r); goto done; }

   /* N: 3D pipeline. */
   VkShaderModuleCreateInfo vmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = scene_vert_spv_sz, .pCode = scene_vert_spv };
   VkShaderModuleCreateInfo fmci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = scene_frag_spv_sz, .pCode = scene_frag_spv };
   VkShaderModule vsm, fsm;
   r = pCreateSM(dev, &vmci, NULL, &vsm); if (r) { LOG("FAIL N: vsm %d", r); goto done; }
   r = pCreateSM(dev, &fmci, NULL, &fsm); if (r) { LOG("FAIL N: fsm %d", r); goto done; }
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
   VkPipelineLayout pl; r = pCreatePL(dev, &plci, NULL, &pl); if (r) { LOG("FAIL N: layout %d", r); goto done; }
   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
   VkVertexInputBindingDescription vib = { .binding = 0, .stride = 20, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription via[2] = {
      { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
      { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = 12 } };
   VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib,
      .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = via };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp_ = { 0, 0, (float)SCN_W, (float)SCN_H, 0.0f, 1.0f };
   VkRect2D scs = { { 0, 0 }, { SCN_W, SCN_H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp_, .scissorCount = 1, .pScissors = &scs };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_BACK_BIT, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo msi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL };
   VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_FALSE, .colorWriteMask = 0xF };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &msi, .pDepthStencilState = &dss,
      .pColorBlendState = &cbs, .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe = VK_NULL_HANDLE; r = pCreateGP(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   LOG("N vkCreateGraphicsPipelines -> %d", r); if (r != VK_SUCCESS) goto done;

   /* L: readback buffer. */
   VkBuffer rbuf; VkDeviceMemory rbmem; void *cpu;
   MK_HOST_BUF(rbuf, rbmem, cpu, SCN_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   memset(cpu, 0, SCN_BYTES);

   /* record. */
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
   VkCommandPool pool; r = pCreatePool(dev, &pci, NULL, &pool); if (r) { LOG("FAIL: pool %d", r); goto done; }
   VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd; r = pAllocCB(dev, &cbai, &cmd); if (r) { LOG("FAIL: allocCB %d", r); goto done; }
   VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; /* reusable: re-submitted every frame */
   r = pBegin(cmd, &cbbi); if (r) { LOG("FAIL: begin %d", r); goto done; }

   VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier toDst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = tex, .subresourceRange = cr };
   pBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
   VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TEX_W, TEX_H, 1 } };
   pCopyBI(cmd, staging, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
   VkImageMemoryBarrier toRead = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = tex, .subresourceRange = cr };
   pBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &toRead);

   VkClearValue clears[2]; clears[0].color = (VkClearColorValue){ .float32 = { 0.06f, 0.07f, 0.10f, 1.0f } };
   clears[1].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
   VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fbuf,
      .renderArea = { { 0, 0 }, { SCN_W, SCN_H } }, .clearValueCount = 2, .pClearValues = clears };
   pBeginRP(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   pBindPipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   pBindDS(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
   VkDeviceSize voff = 0; pBindVB(cmd, 0, 1, &vbuf, &voff);
   pDraw(cmd, 36, 1, 0, 0);
   pEndRP(cmd);
   VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { SCN_W, SCN_H, 1 } };
   pCopyIB(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &region);
   r = pEnd(cmd); LOG("O recorded (3D cube); end -> %d", r); if (r) goto done;

   /* REAL-TIME LOOP: each frame re-compute the MVP (spin), re-submit the draw,
    * blit the result to the framebuffer. The libnx framebuffer present is vsync'd
    * to the display (60 Hz), so this caps at 60 fps; we measure the actual rate. */
   (void)have_shot;
   {
      PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
      NWindow *win = nwindowGetDefault();
      Framebuffer fb; framebufferCreate(&fb, win, SCR_W, SCR_H, PIXEL_FORMAT_RGBA_8888, 2); framebufferMakeLinear(&fb);
      uint32_t *px = (uint32_t *)cpu;
      float angle = 0.0f; uint32_t frames = 0; int verified = 0;
      uint64_t t0 = armGetSystemTick();
      LOG("P: real-time spin @ vsync; press + to exit");
      while (appletMainLoop()) {
         padUpdate(&pad);
         if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            /* Exit the INSTANT + is pressed, skipping framebufferClose + ALL the
             * teardown: the crash report shows the fault is a recursive stack
             * overflow inside the display/nv teardown (framebufferClose conflicts
             * with the nv that NVK used). Terminate the process directly -> the OS
             * reclaims the framebuffer/GPU/memory and returns to the HOME menu. */
            if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; }
            svcExitProcess();
         }
         /* per-frame MVP */
         float P[16], V[16], Mx[16], My[16], M[16], VM[16], MVP[16];
         m_persp(P, 1.0f, (float)SCN_W / (float)SCN_H, 0.1f, 10.0f);
         m_trans(V, 0.0f, 0.0f, -2.4f);
         m_rotY(My, angle); m_rotX(Mx, angle * 0.6f); m_mul(M, My, Mx);
         m_mul(VM, V, M); m_mul(MVP, P, VM);
         memcpy(up, MVP, 64); armDCacheFlush(up, 64);
         angle += 0.05f;
         /* render + copy (the same recorded cmd; reads the updated UBO) */
         VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
         if (pSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) { LOG("submit failed @ frame %u", frames); break; }
         pWaitIdle(queue);
         armDCacheFlush(cpu, SCN_BYTES);
         if (!verified) {
            uint32_t red = 0, white = 0;
            for (uint32_t i = 0; i < SCN_PIXELS; i++) { if (px[i] == COL_RED) red++; else if (px[i] == COL_WHITE) white++; }
            LOG("M frame0: red=%u white=%u center=0x%08x", red, white, px[(SCN_H/2)*SCN_W + SCN_W/2]);
            verified = 1;
         }
         /* upscale-blit to the vsync'd framebuffer */
         u32 stride; u8 *base = (u8 *)framebufferBegin(&fb, &stride);
         for (u32 yy = 0; yy < SCR_H; yy++) {
            uint32_t *row = (uint32_t *)(base + (size_t)yy * stride);
            u32 sy = yy * SCN_H / SCR_H;
            for (u32 xx = 0; xx < SCR_W; xx++) row[xx] = px[sy * SCN_W + (xx * SCN_W / SCR_W)];
         }
         framebufferEnd(&fb);
         frames++;
         if ((frames % 120) == 0) {
            uint64_t ns = armTicksToNs(armGetSystemTick() - t0);
            uint64_t fps = ns ? (uint64_t)frames * 1000000000ull / ns : 0;
            LOG("FPS: %u frames in %llu ms => ~%llu fps", frames, (unsigned long long)(ns/1000000ull), (unsigned long long)fps);
         }
      }
      uint64_t ns = armTicksToNs(armGetSystemTick() - t0);
      uint64_t fps = ns ? (uint64_t)frames * 1000000000ull / ns : 0;
      LOG("=== spin done: %u frames in %llu ms => ~%llu fps avg ===",
          frames, (unsigned long long)(ns/1000000ull), (unsigned long long)fps);
      framebufferClose(&fb);
   }

done:
   (void)inst;
   LOG("=== done; exiting to HOME (svcExitProcess) ===");
   g_drm_shim_log_sink = NULL;
   if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; }
   /* The normal exit path (vkDestroyInstance + C/C++ static destructors + libnx
    * __appExit service deinit) FAULTS on framebuffer apps — a recursive User
    * Break in the teardown (the headless smoke, which never opens a framebuffer,
    * exits fine). The frame work is done and the framebuffer is already closed,
    * so terminate the process directly: the OS reclaims the GPU/display/memory
    * and returns to the HOME menu cleanly, skipping the faulting teardown. */
   svcExitProcess();
   return 0;  /* unreachable */
}
