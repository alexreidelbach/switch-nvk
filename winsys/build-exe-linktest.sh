#!/usr/bin/env bash
# Strict link test: link the NVK static libs + the winsys shim + the libc shim
# into a real Switch EXE (devkitA64 switch.specs -> crt0 + switch.ld resolves the
# app/linker-script symbols). An EXE link errors on ANY unresolved symbol, so a
# clean link proves M1 is symbol-complete. Run inside switch-nvk-build:
#   docker run --rm -v "D:\switch-nvk:/work" -w /work switch-nvk-build bash winsys/build-exe-linktest.sh
set -e

DKP=/opt/devkitpro
GCC=$DKP/devkitA64/bin/aarch64-none-elf-gcc
GXX=$DKP/devkitA64/bin/aarch64-none-elf-g++   # link driver: pulls libstdc++ (nv50_ir codegen is C++)
MB=/work/mb
OBJ=/tmp/nvk-link
mkdir -p "$OBJ"

ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
INC="-I$DKP/libnx/include -I/opt/switch-cross-include -Imesa-25/include -Imesa-25/src/nouveau/drm -Iwinsys -Icompat"
DEFS="-D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -include /work/compat/switch_compat.h"

echo "=== compiling shims + main ==="
$GCC -c winsys/drm_shim.c        -o "$OBJ/drm_shim.o"        $ARCH $DEFS $INC -O2
$GCC -c winsys/switch_libc_shim.c -o "$OBJ/switch_libc_shim.o" $ARCH $DEFS $INC -O2
$GCC -c compat/compat.c          -o "$OBJ/compat.o"         $ARCH $DEFS $INC -O2
$GCC -c winsys/main_linktest.c   -o "$OBJ/main.o"           $ARCH -O2

# Archive set + order taken verbatim from the .so link in mb/build.ninja.
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

echo "=== linking EXE (no --gc-sections; --wrap open/close) ==="
cd "$MB"
$GXX -specs="$DKP/libnx/switch.specs" $ARCH \
  -L$DKP/libnx/lib -L$DKP/portlibs/switch/lib \
  -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=stat -Wl,--wrap=lstat \
  -o /tmp/nvk_linktest.elf \
  "$OBJ/main.o" "$OBJ/drm_shim.o" "$OBJ/switch_libc_shim.o" "$OBJ/compat.o" \
  -Wl,--whole-archive src/nouveau/vulkan/libnvk.a -Wl,--no-whole-archive \
  -Wl,--start-group \
    $ARCHIVES \
    $PORTLIBS \
    -lnx -lc -lm -ldl -pthread \
  -Wl,--end-group

echo "=== LINK OK -> /tmp/nvk_linktest.elf ==="
$DKP/devkitA64/bin/aarch64-none-elf-size /tmp/nvk_linktest.elf
