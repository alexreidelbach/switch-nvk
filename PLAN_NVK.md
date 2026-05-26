# PLAN_NVK.md — Port Mesa NVK (Vulkan) to the Switch GM20B + libnx nv services

> **⚠️ READ `RESUME_NVK.md` FIRST — it is the live state.** This file is the original plan; below it
> is now PARTLY HISTORY. Done since: **M0 (Rust std) ✅** and **M1-build (NVK fully cross-compiles,
> `libnvk.a` produced) ✅**. The exact remaining winsys/shim symbols are in `UNDEFINED_SYMBOLS.txt`.
> **GOAL NARROWED**: the deliverable is ONLY a standalone Vulkan placeholder (M3 triangle), like
> Dan's — **NO game/Aurora/port integration (M4 is OUT of scope)**. See [[dan-nvk-intel-and-goal]].

**Status:** M0 + M1-build DONE (2026-05-25). Goal: a working open-source **Vulkan driver for Switch
homebrew** (HOS/Atmosphère `.nro`), by porting Mesa's **NVK** to the Tegra X1 **GM20B** over **libnx
nv services** — NO deko3d, NO NVIDIA L4T blob, NO dependence on Dan's private `ticohq/switch-nvk-vulkan`.
End state (narrowed): a standalone `.nro` that **requires Vulkan and draws a triangle** via our own NVK.
See [[vulkan-nvk-switch-path]].

## Why this is bounded (not "write a driver from scratch")
NVK is open Mesa (`src/nouveau/`), already Vulkan-1.4 conformant on discrete Maxwell; GM20B is
Maxwell 2nd-gen (same ISA family — the **NAK** shader compiler already targets it). The GPU is the
same Tegra part libnx's `nv` services already drive. So the port = **a winsys translation layer**
(swap nouveau's Linux-DRM backend for libnx nv) + GM20B device-info + a Mesa cross-build. The
nouveau↔libnx mapping is nearly 1:1 (both wrap the same Tegra kernel GPU iface).

## The interface we reimplement (`src/nouveau/winsys/`) ↔ libnx (`switch/nvidia/`)
| NVK winsys (3 files: device/bo/context) | libnx nv primitive |
|---|---|
| `nouveau_ws_device` { fd, `nv_device_info`, bos } — `_new/_destroy/_timestamp/_vram_used` | `nvidia/gpu.h` (NvGpu); fill `nv_device_info` for GM20B |
| `nouveau_ws_bo` — `_new/_new_mapped/_new_tiled/_map/_unmap/_wait/_destroy`, **`_bind_vma/_unbind_vma`**, `_dma_buf` | `nvidia/map.h` (nvmap alloc) + `nvidia/address_space.h` (GPU VA bind) |
| `nouveau_ws_context` — `_create/_destroy/_killed`, engines | `nvidia/channel.h` + `gpu_channel.h` (GPU channel, pushbuf submit) |
| BO/exec sync (`_wait`) | `nvidia/fence.h` (NvFence) |
| WSI surface/swapchain/present | `services/vi.h` + `nvidia/graphic_buffer.h` (ref: Dan's public `vulkan-triangle-test-switch`) |

`nouveau_ws_device.fd` (a DRM fd) → our libnx nv handle. `nouveau_private.h` is the DRM-specific
guts we replace. NAK/NIL/compiler/`vulkan/` (NVK core) stay upstream-as-is.

## What's already done / de-risked
- **WSI**: PUBLIC in `github.com/dantiicu/vulkan-triangle-test-switch` (VI surface + swapchain → nwindow).
- **Dawn-Vulkan wiring**: our `extern/aurora/cmake/aurora_core.cmake` already sets `DAWN_SWITCH_NVK_ROOT=/opt/nvk-switch` + links `libvulkan.a` (the old `FATAL_ERROR` = "driver not built yet"). `aurora-switch` builds Dawn with `DAWN_ENABLE_VULKAN=ON` on Switch.
- **Shader compiler**: NVK's NAK targets Maxwell already.
- **Fallback**: Dawn-GL ([[dawn-over-gl-already-patched]]) renders Aurora's full pipeline today without any of this — keep as plan B.

## Phased milestones
- **M0 — Mesa/NVK cross-builds for Switch.** meson cross-file → devkitA64 (aarch64-none-elf, newlib). Reconstruct Dan's `create-nvk-image.sh`. Expect to fight: Mesa's Linux/glibc/POSIX assumptions, the DRM/libdrm deps (we cut them), build only `nouveau` Vulkan + NAK. Install prefix `/opt/nvk-switch`. **This is the foundation + likely the fiddliest part.**
- **M1 — winsys over nv + GM20B enum.** Reimplement device/bo/context (3 files) over libnx nv; fill `nv_device_info` for GM20B. Milestone: `vkCreateInstance` + `vkEnumeratePhysicalDevices` returns the GM20B on real HW (no render yet — prove the layer below, anti-pattern #4).
- **M2 — memory + submission.** nvmap BOs + GPU VA bind (address_space) + channel/pushbuf submit (gpu_channel) + NvFence sync. Milestone: a trivial `vkQueueSubmit` completes without fault.
- **M3 — first triangle (THE GOAL).** WSI (ref Dan's public TriangleTest) + render pass/pipeline. Milestone: our NVK draws a triangle on the TV from a standalone `.nro`. **STOP HERE.**
- ~~**M4 — Aurora integration.**~~ **OUT OF SCOPE** (user, 2026-05-25: "nada de jogo nem Port"). Do not wire NVK into Aurora/Dusklight/any game; the deliverable ends at the M3 placeholder.

## Open risks
1. **Mesa cross-build on libnx/newlib** (M0) — Mesa assumes Linux. Biggest unknown.
2. **GM20B vs discrete Maxwell** — integrated/unified memory, GPU class IDs, firmware; patch NVK device init + memory.
3. **nouveau_ws semantics vs libnx nv** — VA binding model, pushbuf/submit format, sync — mostly 1:1 but verify per-call.
4. **Mesa version** — pick a branch where NVK+Maxwell is solid (Mesa ≥ 25.x), but old enough to build with our toolchain.

## Critical files
- Mesa: `src/nouveau/winsys/nouveau_{device,bo,context}.{c,h}`, `nouveau_private.h`, `src/nouveau/vulkan/` (NVK core — keep), `meson.build` + cross file.
- libnx: `$DEVKITPRO/libnx/include/switch/nvidia/{gpu,channel,gpu_channel,address_space,map,fence}.h`, `services/{nv,vi}.h`.
- Ours: `extern/aurora/cmake/aurora_core.cmake` (NVK link, ready), Dan's `vulkan-triangle-test-switch` (WSI ref).
