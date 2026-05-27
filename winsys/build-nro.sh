#!/usr/bin/env bash
# Build the headless NVK smoke-test .nro for Sphaira (full Application mode).
#   docker run --rm -v "D:\switch-nvk:/work" -w /work switch-nvk-build bash winsys/build-nro.sh
# Output: /work/nvk_smoke.nro  (=> D:\switch-nvk\nvk_smoke.nro)
set -e

DKP=/opt/devkitpro
GCC=$DKP/devkitA64/bin/aarch64-none-elf-gcc
GXX=$DKP/devkitA64/bin/aarch64-none-elf-g++
STRIP=$DKP/devkitA64/bin/aarch64-none-elf-strip
MB=/work/mb
OBJ=/tmp/nvk-nro
mkdir -p "$OBJ"

ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
INC="-I$DKP/libnx/include -I/opt/switch-cross-include -Imesa-25/include -Imesa-25/src/nouveau/drm -Iwinsys -Icompat"
DEFS="-D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -include /work/compat/switch_compat.h"

echo "=== compiling app + shims ==="
$GCC -c winsys/smoke/nvk_smoke.c  -o "$OBJ/nvk_smoke.o"      $ARCH -D__SWITCH__ -D_GNU_SOURCE -Imesa-25/include -I$DKP/libnx/include -O2 -Wall
$GCC -c winsys/drm_shim.c         -o "$OBJ/drm_shim.o"       $ARCH $DEFS $INC -O2 ${DRM_SHIM_DEBUG:+-DDRM_SHIM_DEBUG}
$GCC -c winsys/switch_libc_shim.c -o "$OBJ/switch_libc_shim.o" $ARCH $DEFS $INC -O2
$GCC -c compat/compat.c           -o "$OBJ/compat.o"         $ARCH $DEFS $INC -O2

# Archive set + order from mb/build.ninja's ICD .so link.
ARCHIVES="
  src/nouveau/codegen/libnouveau_codegen.a
  src/util/libmesa_util.a src/util/libmesa_util_sse41.a src/util/blake3/libblake3.a
  src/c11/impl/libmesa_util_c11.a
  src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a
  src/compiler/rust/libcompiler_c_helpers.a
  src/nouveau/headers/libnvidia_headers_c.a
  src/nouveau/nil/liblibnil.a src/nouveau/nil/liblibnil_format_table.a
  src/compiler/nir/libnir.a src/compiler/libcompiler.a
  src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a
  src/vulkan/util/libvulkan_util.a src/compiler/spirv/libvtn.a
  src/util/libxmlconfig.a"
PORTLIBS="$DKP/portlibs/switch/lib/libz.a $DKP/portlibs/switch/lib/libzstd.a $DKP/portlibs/switch/lib/libexpat.a"

echo "=== linking ELF ==="
cd "$MB"
$GXX -specs="$DKP/libnx/switch.specs" $ARCH \
  -L$DKP/libnx/lib -L$DKP/portlibs/switch/lib \
  -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=stat -Wl,--wrap=lstat \
  -o "$OBJ/nvk_smoke.elf" \
  "$OBJ/nvk_smoke.o" "$OBJ/drm_shim.o" "$OBJ/switch_libc_shim.o" "$OBJ/compat.o" \
  -Wl,--whole-archive src/nouveau/vulkan/libnvk.a -Wl,--no-whole-archive \
  -Wl,--start-group \
    $ARCHIVES $PORTLIBS \
    -lnx -lc -lm -ldl -pthread \
  -Wl,--end-group

echo "=== packaging NRO ==="
"$STRIP" "$OBJ/nvk_smoke.elf" -o "$OBJ/nvk_smoke.stripped.elf"
"$DKP/tools/bin/nacptool" --create "NVK Smoke" "switch-nvk" "0.32.0-fencecmdlist" "$OBJ/nvk_smoke.nacp"
"$DKP/tools/bin/elf2nro" "$OBJ/nvk_smoke.elf" /work/nvk_smoke.nro \
  --icon="$DKP/libnx/default_icon.jpg" --nacp="$OBJ/nvk_smoke.nacp"

echo "=== DONE -> /work/nvk_smoke.nro ==="
ls -la /work/nvk_smoke.nro
