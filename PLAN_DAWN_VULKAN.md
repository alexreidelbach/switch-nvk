# PLAN — Dawn-over-Vulkan bring-up (our NVK → Dusklight's Aurora)

**Goal:** make our NVK the graphics backend for the Dusklight Switch port, via Dawn's Vulkan
backend. This is the bridge: `Dusklight → Aurora GX → lib/webgpu/gpu.cpp → Dawn (Vulkan) → our NVK`.
Created 2026-05-27, after WSI proven on HW. Companion to `RESUME_NVK.md` (NVK state).

---

## ✅ THE BRIDGE IS SHAPE-COMPATIBLE (the key de-risk — verified by reading Dan's dawn-switch)

Dan built `dawn-switch` to consume HIS loaderless static NVK ICD. We built our NVK from the RE of his.
**Every integration point matches what our NVK already provides:**

| Layer | Dawn-switch (Dan) expects | Our NVK provides | File |
|---|---|---|---|
| ICD bootstrap | `vk_icdGetInstanceProcAddr(VK_NULL, "vkGetInstanceProcAddr")`, fallback `&vk_icdGetInstanceProcAddr` | `libnvk.a` exports exactly `vk_icdGetInstanceProcAddr` | `vulkan/VulkanFunctions.cpp:99-105` |
| Lib | static `libvulkan.a`, `LoadVulkan` = no-op on Switch | our merged `libvulkan.a` | `vulkan/BackendVk.cpp:71-72,362-366` |
| Surface | `wgpu::SurfaceSourceSwitchNWindow{window}` → `vkCreateViSurfaceNN` (gated on `InstanceExt::ViSurface`) | our `wsi_common_switch.c` impls `vkCreateViSurfaceNN`; `nvk_instance.c` advertises `NN_vi_surface=true` | `vulkan/SwapChainVk.cpp:668-687` |
| Aurora surface src | on Switch builds `SurfaceSourceSwitchNWindow{nwindow}` | matches | aurora `lib/dawn/BackendBinding.cpp:17-25` |

⇒ **Not a research project — a well-defined bring-up.** The only real engineering deltas vs Dan's setup:
(1) our NVK uses `drm_shim.c`+`switch_libc_shim.c` (libnx `nv` via libdrm-core + `--wrap=open,close,stat,lstat`),
NOT Dan's `nvkmd/switch` direct-libnx; so the consumer link drops `drm_nouveau` and adds our shim + the
wrap flags. (2) we package 21 Mesa static libs into one `libvulkan.a`.

---

## Consumer link recipe (what Dawn/Aurora must do to link our NVK)

From Dan's `dawn_switch_add_nro` (dawn-switch `native/CMakeLists.txt:1111-1130`) ADAPTED for our winsys
(authoritative archive set + flags = our `winsys/build-exe-linktest.sh`):
```
compile-defs : __SWITCH__ NX VK_USE_PLATFORM_VI_NN
include      : <nvk-switch>/include            (Vulkan-Headers: vulkan.h, vk_icd.h, vulkan_vi.h)
link         : webgpu_dawn  <nvk-switch>/lib/libvulkan.a  z zstd expat nx m dl pthread
               (NO drm_nouveau — our drm_shim replaces it, baked into libvulkan.a)
link-options : -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=stat -Wl,--wrap=lstat
               -Wl,--gc-sections -Wl,--allow-multiple-definition
runtime env  : NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1  (GM20B is SOC/non-conformant; NVK gates on it)
```
Our `libvulkan.a` = fat merge of: `libnvk.a` (whole) + the 18 support archives + the 3 shim objects
(`drm_shim.o`, `switch_libc_shim.o`, `compat.o`). The shims being IN the archive means the final link
only needs the `--wrap` flags. (force-included `compat/switch_compat.h` at shim compile time.)

---

## MILESTONES (incremental, prove the layer below first — heuristic #4)

### ⬜ M-DV-0 — Package our NVK as `nvk-switch/{lib/libvulkan.a, include/}`   (NOT HW-gated)
`package-nvk.sh`: compile the 3 shims, MRI-merge libnvk.a + the 18 archives + the shim .o into one
`libvulkan.a`, copy Vulkan-Headers into `include/`. **Verify (autonomous, via `nm`):**
`vk_icdGetInstanceProcAddr` DEFINED (T), `vkCreateViSurfaceNN` DEFINED, `__wrap_open`/`__wrap_stat`
DEFINED, no unexpected UNDEFs beyond libnx/libc/portlibs. This is the foundational deliverable.

### ⬜ M-DV-1 — Standalone Dawn-Vulkan **clear** NRO on real Tegra   (HW-gated — proves the bridge ISOLATED)
Write our own `clear_nro.cpp` (Dan's sample sources are NOT in the public fork — only the CMake recipe):
minimal Dawn WebGPU — `wgpu::CreateInstance` → request a Vulkan adapter → device → `SurfaceSourceSwitchNWindow`
(nwindowGetDefault) → configure → per-frame: clear to a cycling colour → present. Build dawn-switch
STANDALONE with `-DDAWN_SWITCH_BUILD_CLEAR_NRO=ON -DDAWN_SWITCH_NVK_ROOT=<our package>` (adapt the link).
**Success = a clear colour on the TV via Dawn → our NVK → our WSI.** Zero Aurora/Dusklight variables.
(Then the triangle NRO = shaders through Tint→SPIR-V→NAK on our driver.)

### ⬜ M-DV-2 — Wire into Dusklight's Aurora; prelaunch HOME menu on NVK   (HW-gated)
Flip Dusklight build: `AURORA_BACKEND_GLES=OFF AURORA_BACKEND_DEKO3D=OFF` → the `else()` wgpu path
(`lib/webgpu/gpu.cpp`). Point `aurora_core.cmake:119-141` `DAWN_SWITCH_NVK_ROOT` at our package + apply
the consumer link recipe (drop drm_nouveau, add shim/wrap/expat). Build Dusklight. **Target: the
prelaunch RmlUi HOME menu** (already renders via Dawn/wgpu on PC) showing on the TV through NVK.

### ⬜ M-DV-3 — GX gameplay render via the full Aurora wgpu path → our NVK   (the endgame)
Boot the engine to TITLE (the GLES bring-up already reaches it) with the wgpu/Vulkan backend; GX→WGSL→
Tint→SPIR-V→NAK pipeline draws gameplay through our NVK. Reuse the GLES-era boot null-guards (backend-
agnostic). This is where the WSI zero-copy TODO (kind=0xfe, RE_NOTES) eventually matters for 60fps.

---

## Risks / open questions
- **Dawn-Vulkan build for Switch:** Dusklight currently dead-links Dawn (GLES path, types only). Need
  `DAWN_ENABLE_BACKEND_VULKAN` to actually COMPILE the Vulkan backend for aarch64 devkitA64. Biggest
  unknown — M-DV-1 (standalone) isolates it.
- **MRI merge collisions:** thin archives → extract .o; duplicate basenames across libs are OK in a fat
  archive (consumer uses start-group/gc-sections). Mirror build-exe-linktest's exact set/order.
- **`--allow-multiple-definition`:** Dan uses it (our libc shim overrides newlib syms). Keep it.
- **WSI present is the CPU-copy fallback** (zero-copy is a post-port TODO, see RESUME_NVK / RE_NOTES) —
  fine for bring-up, revisit for 60fps gameplay.
- **Scope:** the NVK repo (`D:\switch-nvk`) owns M-DV-0/1; Dusklight (`D:\Projects\dusklight`) owns
  M-DV-2/3. Both in play now (the integrate-into-Dusklight goal supersedes the standalone-only rule).
