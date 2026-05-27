#!/usr/bin/env bash
# Full ninja (rebuilds libnvk.a WITH the link_whole'd runtime incl. patched
# vk_instance.o), ignore the known ICD .so failure, then rebuild + verify the nro.
XDIR=/opt/devkitpro/portlibs/switch/lib/pkgconfig
rm -f "$XDIR/SPIRV-Tools.pc" "$XDIR/SPIRV-Tools-shared.pc" "$XDIR/LLVMSPIRVLib.pc" "$XDIR/libclc.pc"
export NATIVE_PREFIX=/work/native-prefix
export PATH=/work/native-prefix/bin:$PATH

ninja -C /work/mb || echo "(ninja .so link failed as expected; .a libs are built)"

echo "=== libnvk.a size + diag string ==="
ls -la /work/mb/src/nouveau/vulkan/libnvk.a
strings /work/mb/src/nouveau/vulkan/libnvk.a | grep -F "try_create_for_drm" >/dev/null \
  && echo "[diag IN libnvk.a]" || echo "[diag NOT in libnvk.a]"

bash /work/winsys/build-nro.sh

echo "=== diag string in final nro? ==="
strings /work/nvk_smoke.nro | grep -F "try_create_for_drm" >/dev/null \
  && echo "[diag IN nro]" || echo "[diag NOT in nro]"
