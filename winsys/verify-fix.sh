#!/usr/bin/env bash
# Autonomous build-level verification of the libdrm.h fix: vk_instance.c should
# now REFERENCE drmGetDevices2 (U = binds to our shim) instead of carrying a
# local no-op stub (t). No device needed.
NM=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm
O=/work/mb/src/vulkan/runtime/libvulkan_instance.a.p/vk_instance.c.o
rm -f "$O"
XDIR=/opt/devkitpro/portlibs/switch/lib/pkgconfig
rm -f "$XDIR/SPIRV-Tools.pc" "$XDIR/SPIRV-Tools-shared.pc" "$XDIR/LLVMSPIRVLib.pc" "$XDIR/libclc.pc"
export NATIVE_PREFIX=/work/native-prefix; export PATH=/work/native-prefix/bin:$PATH
ninja -C /work/mb 2>&1 | grep -E "Compiling.*vk_instance|FAILED" | head -3
echo "(ignore the trailing .so link failure)"
echo "=== drm symbols in vk_instance.c.o (U=ref-to-our-shim, t=local stub) ==="
"$NM" "$O" 2>/dev/null | grep -iE "drmGetDevices2|drmGetVersion|drmFreeDevices|drmFreeVersion"
