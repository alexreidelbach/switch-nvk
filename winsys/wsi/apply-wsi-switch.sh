#!/bin/bash
# Apply the VK_NN_vi_surface / nwindow WSI backend to a fresh mesa-25 tree (idempotent).
# Run AFTER apply-patches.sh. The mesa-25/ tree is gitignored, so this script + the tracked
# backend copy (winsys/wsi/wsi_common_switch.c) are the durable form of the WSI work.
# Manual backstop / full description: D:\switch-nvk\RESUME_NVK.md  "WSI WORKING" section.
# Also requires: configure-mesa.sh already injects -DVK_USE_PLATFORM_VI_NN (tracked).
#   usage: bash winsys/wsi/apply-wsi-switch.sh [/work/mesa-25]
set -e
ME="$(cd "$(dirname "$0")" && pwd)"
M="${1:-/work/mesa-25}"
W="$M/src/vulkan/wsi"
N="$M/src/nouveau/vulkan"

# 1. the backend (new file)
cp "$ME/wsi_common_switch.c" "$W/wsi_common_switch.c"

# 2. wsi_common.h — size the wsi[] array for VK_ICD_WSI_PLATFORM_VI (=12, past METAL)
grep -q 'VK_USE_PLATFORM_VI_NN' "$W/wsi_common.h" || perl -0777 -pi -e \
  's/#define VK_ICD_WSI_PLATFORM_MAX \(VK_ICD_WSI_PLATFORM_METAL \+ 1\)/#ifdef VK_USE_PLATFORM_VI_NN\n#define VK_ICD_WSI_PLATFORM_MAX (VK_ICD_WSI_PLATFORM_VI + 1)\n#else\n#define VK_ICD_WSI_PLATFORM_MAX (VK_ICD_WSI_PLATFORM_METAL + 1)\n#endif/' \
  "$W/wsi_common.h"

# 3. wsi_common_private.h — init/finish declarations
grep -q 'wsi_switch_init_wsi' "$W/wsi_common_private.h" || perl -0777 -pi -e \
  's/(VK_DEFINE_NONDISP_HANDLE_CASTS\(wsi_swapchain, base, VkSwapchainKHR,)/#ifdef VK_USE_PLATFORM_VI_NN\nVkResult wsi_switch_init_wsi(struct wsi_device *wsi_device, const VkAllocationCallbacks *alloc, VkPhysicalDevice physical_device);\nvoid wsi_switch_finish_wsi(struct wsi_device *wsi_device, const VkAllocationCallbacks *alloc);\n#endif\n\n$1/' \
  "$W/wsi_common_private.h"

# 4. wsi_common.c — init + finish calls (after the headless ones)
grep -q 'wsi_switch_init_wsi' "$W/wsi_common.c" || perl -0777 -pi -e \
  's/(result = wsi_headless_init_wsi\(wsi, alloc, pdevice\);\n   if \(result != VK_SUCCESS\)\n      goto fail;\n#endif\n)/$1\n#ifdef VK_USE_PLATFORM_VI_NN\n   result = wsi_switch_init_wsi(wsi, alloc, pdevice);\n   if (result != VK_SUCCESS)\n      goto fail;\n#endif\n/' \
  "$W/wsi_common.c"
grep -q 'wsi_switch_finish_wsi' "$W/wsi_common.c" || perl -0777 -pi -e \
  's/(   wsi_headless_finish_wsi\(wsi, alloc\);\n)/$1#ifdef VK_USE_PLATFORM_VI_NN\n   wsi_switch_finish_wsi(wsi, alloc);\n#endif\n/' \
  "$W/wsi_common.c"

# 5. meson.build — compile the backend on horizon
grep -q 'wsi_common_switch' "$W/meson.build" || perl -0777 -pi -e \
  "s/(  files_vulkan_wsi \\+= files\\('wsi_common_headless.c'\\)\nendif\n)/\$1\nif host_machine.system() == 'horizon'\n  files_vulkan_wsi += files('wsi_common_switch.c')\nendif\n/" \
  "$W/meson.build"

# 6. nvk_instance.c — advertise VK_NN_vi_surface
grep -q 'NN_vi_surface' "$N/nvk_instance.c" || perl -0777 -pi -e \
  's/(   .EXT_headless_surface = true,\n#endif\n)/$1#ifdef VK_USE_PLATFORM_VI_NN\n   .NN_vi_surface = true,\n#endif\n/' \
  "$N/nvk_instance.c"

echo "WSI switch backend applied to $M (verify: grep -l wsi_common_switch $W/meson.build)"
