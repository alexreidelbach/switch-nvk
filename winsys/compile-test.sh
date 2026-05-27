#!/usr/bin/env bash
# Standalone compile-check for the NVK winsys shim with the devkitA64 cross
# toolchain. Run inside the switch-nvk-build image:
#   docker run --rm -v "D:\switch-nvk:/work" -w /work switch-nvk-build bash winsys/compile-test.sh
set -e
GCC=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc
NM=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm

"$GCC" -c winsys/drm_shim.c -o /tmp/drm_shim.o \
  -march=armv8-a -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE \
  -I/opt/devkitpro/libnx/include \
  -I/opt/switch-cross-include \
  -Imesa-25/include \
  -Imesa-25/src/nouveau/drm \
  -Iwinsys -Icompat -include /work/compat/switch_compat.h \
  -Wall -Wextra -Wno-unused-parameter

echo "=== compile OK ==="
echo "=== DEFINED (text) symbols ==="
"$NM" --defined-only /tmp/drm_shim.o | grep " T " | sort
echo "=== UNDEFINED references ==="
"$NM" -u /tmp/drm_shim.o | sort
