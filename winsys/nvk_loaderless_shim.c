/*
 * nvk_loaderless_shim.c — loaderless-ICD glue for our NVK, the equivalent of Dan/Tiicu's
 * custom `nvk_loaderless_vk.c` (found via Ghidra RE of his NROs). It plugs the gap where
 * upstream Mesa's `vk_icdGetInstanceProcAddr` returns NULL for the loader-MANAGED global
 * commands (which a real Vulkan loader would normally service itself, never reaching the ICD).
 *
 * THE BUG IT FIXES (proven, not guessed): Dawn's Vulkan backend, on Switch, loads global procs
 * via `vk_icdGetInstanceProcAddr(NULL, name)`. Its `LoadGlobalProcs` does
 *   GET_GLOBAL_PROC(EnumerateInstanceLayerProperties);   // VulkanFunctions.cpp:114, REQUIRED
 * which returns DAWN_INTERNAL_ERROR if the proc is NULL. Mesa NVK does NOT implement
 * vkEnumerateInstanceLayerProperties (it's loader-only; not in nvk_instance_entrypoints), so the
 * proc is NULL -> LoadGlobalProcs fails -> VulkanInstance::Initialize fails -> "No supported
 * adapters". (vkEnumerateInstanceVersion IS provided by nvk_EnumerateInstanceVersion, and Dawn
 * loads it "allow-null" anyway, so it was never the problem.)
 *
 * FIX: --wrap=vk_icdGetInstanceProcAddr (see build-exe-linktest / dawn link). We intercept the
 * loader-managed global queries and return tiny conformant stubs; everything else falls through
 * to Mesa's real entry (__real_vk_icdGetInstanceProcAddr). No Mesa rebuild needed.
 *
 * SPDX-License-Identifier: MIT
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <string.h>

/* Provided by the linker via -Wl,--wrap=vk_icdGetInstanceProcAddr (the real Mesa NVK entry). */
extern PFN_vkVoidFunction __real_vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

/* Loaderless homebrew has no Vulkan layers — report zero, success. */
static VKAPI_ATTR VkResult VKAPI_CALL
shim_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
   (void)pProperties;
   if (pPropertyCount != NULL)
      *pPropertyCount = 0;
   return VK_SUCCESS;
}

PFN_vkVoidFunction
__wrap_vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   if (pName != NULL) {
      /* Dawn bootstraps via `GetInstanceProcAddr = vk_icdGetInstanceProcAddr(NULL,
       * "vkGetInstanceProcAddr")` and uses THAT pointer for all later lookups. Mesa returns its
       * REAL nvk_GetInstanceProcAddr there, which would bypass this shim. So return OURSELVES for
       * "vkGetInstanceProcAddr" -> Dawn (and the probe) then route every lookup through this wrap,
       * letting us service vkEnumerateInstanceLayerProperties while falling through for the rest. */
      if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
         return (PFN_vkVoidFunction)&__wrap_vk_icdGetInstanceProcAddr;
      }
      if (strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0) {
         return (PFN_vkVoidFunction)shim_EnumerateInstanceLayerProperties;
      }
   }
   return __real_vk_icdGetInstanceProcAddr(instance, pName);
}
