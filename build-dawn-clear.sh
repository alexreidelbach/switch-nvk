#!/usr/bin/env bash
# M-DV-1 — build the standalone Dawn-Vulkan CLEAR nro (dawn-switch fork) against OUR NVK package.
# Proves Dawn -> our NVK -> our WSI, isolated from Aurora/Dusklight. See PLAN_DAWN_VULKAN.md.
#
# Runs in the devkitpro/devkita64 image (same as Dusklight's build), mounting BOTH trees:
#   /dusklight = D:\Projects\dusklight   (the vendored dawn-switch + switch-examples/clear_nro.cpp)
#   /nvk       = D:\switch-nvk           (our nvk-switch/ package from package-nvk.sh)
#   /build     = D:\dawn-clear-build     (out-of-tree build dir, persisted on D:)
# Invoke (from D:\switch-nvk, via PowerShell to avoid MSYS path mangling):
#   docker run --rm -v "D:\Projects\dusklight:/dusklight" -v "D:\switch-nvk:/nvk" \
#     -v "D:\dawn-clear-build:/build" -w /dusklight devkitpro/devkita64 \
#     bash /nvk/build-dawn-clear.sh [configure|build|both]
set -e
set -o pipefail   # so a failed cmake/ninja propagates THROUGH the `tee` (heuristic #16)
MODE="${1:-configure}"
SRC=/dusklight/platforms/switch/reference/dawn-switch
B=/build
LOG=$B/dawn-clear.log
mkdir -p "$B"

CFG_ARGS=(
  -S "$SRC" -B "$B" -G Ninja
  -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  # --- Switch + Vulkan-only Dawn ---
  -DDAWN_PLATFORM_SWITCH=ON
  -DDAWN_FETCH_DEPENDENCIES=ON
  -DDAWN_ENABLE_VULKAN=ON
  -DDAWN_ENABLE_NULL=ON
  -DDAWN_ENABLE_OPENGLES=OFF
  -DDAWN_ENABLE_DESKTOP_GL=OFF
  -DDAWN_ENABLE_METAL=OFF
  -DDAWN_ENABLE_D3D11=OFF
  -DDAWN_ENABLE_D3D12=OFF
  -DDAWN_USE_X11=OFF
  -DDAWN_USE_WAYLAND=OFF
  -DDAWN_BUILD_SAMPLES=OFF
  -DDAWN_BUILD_TESTS=OFF
  -DTINT_BUILD_TESTS=OFF
  -DTINT_BUILD_CMD_TOOLS=OFF
  -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC
  # --- the standalone clear nro + OUR NVK package ---
  -DDAWN_SWITCH_BUILD_CLEAR_NRO=ON
  -DDAWN_SWITCH_NVK_ROOT=/nvk/nvk-switch
)

echo "=== [build-dawn-clear] MODE=$MODE  $(date) ===" | tee "$LOG"
echo "NVK package: /nvk/nvk-switch (libvulkan.a $(du -h /nvk/nvk-switch/lib/libvulkan.a 2>/dev/null | cut -f1))" | tee -a "$LOG"
command -v python3 >/dev/null 2>&1 && echo "python3: $(python3 --version 2>&1)" | tee -a "$LOG" || echo "WARN: no python3 (DAWN_FETCH_DEPENDENCIES may fail)" | tee -a "$LOG"

if [ "$MODE" = "configure" ] || [ "$MODE" = "both" ]; then
  echo "=== CONFIGURE ===" | tee -a "$LOG"
  cmake "${CFG_ARGS[@]}" 2>&1 | tee -a "$LOG"
fi

if [ "$MODE" = "build" ] || [ "$MODE" = "both" ]; then
  echo "=== BUILD (target dawn_switch_clear_nro) ===" | tee -a "$LOG"
  cmake --build "$B" --target dawn_switch_clear_nro -j"$(nproc)" 2>&1 | tee -a "$LOG"
  echo "=== artifacts ===" | tee -a "$LOG"
  find "$B" -name 'dawn_switch_clear*.nro' -o -name 'dawn_switch_clear' 2>/dev/null | tee -a "$LOG"
fi
echo "=== [build-dawn-clear] done $(date) ===" | tee -a "$LOG"
