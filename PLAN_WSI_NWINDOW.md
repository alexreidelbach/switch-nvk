# PLAN — real WSI over libnx `nwindow` (VK_NN_vi_surface) for switch-nvk

**Status: DESIGN (2026-05-27). Branch `switch-port/nvk-wsi`.**
This is the proper present path Dan pointed at. It replaces the abandoned headless-surface
shortcut (see "Why headless failed" below).

## Dan's hints — keep these in mind always
> **"Understand NVK, understand nwindow, libnx, build a WSI — you are done."**
> Public reference: `github.com/dantiicu/vulkan-triangle-test-switch` — `TriangleTest.cpp` =
> **VI surface + swapchain** (MIT). It is the template; mirror it.

Dan's `TriangleTest.cpp` (fetched 2026-05-27) does exactly:
```c
/* instance */
const char *enabled_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME };
/* surface */
NWindow *nw = nwindowGetDefault();
VkViSurfaceCreateInfoNN ci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = nw };
vkCreateViSurfaceNN(instance, &ci, NULL, &surface);
/* then: standard swapchain (FIFO, 1280x720, minImageCount>=2),
 *       vkAcquireNextImageKHR(acquire_sem), render, vkQueuePresentKHR(render_sem). */
```
So the WSI we must build is the **`VK_NN_vi_surface`** backend — the surface wraps the libnx
`NWindow`, and the swapchain drives the NWindow's buffer queue.

## Why the headless shortcut failed (don't repeat it)
We tried `VK_EXT_headless_surface` (always compiled in Mesa) to avoid writing a real WSI.
Proven on real Tegra: WSI enable + `vkCreateHeadlessSurfaceEXT`→0 + **`vkCreateSwapchainKHR`→0
(5 images)** + `vkAcquireNextImageKHR`→0 + render submit→0. But `vkQueuePresentKHR` **NULL-derefs**
(Atmosphère crash report → addr2line):
```
PC: vk_common_QueueSubmit + 0xb8144  (Data Abort @ 0x0)
 ← wsi_common_queue_present + 0xe71c0
 ← main
```
The headless present submits an internal **blit command buffer** through the generated v1→v2
`vkQueueSubmit` conversion, which crashes on our setup. AND, decisively: **headless never
actually scans out** — it is a test surface; we were faking the display with a manual libnx
framebuffer blit. So headless is a dead end for real present. The VI/nwindow path below does
real, zero-copy scanout and avoids the blit submit entirely (present = `nwindowQueueBuffer`).

What carries over from the headless work (NOT wasted): the WSI lib is built+linked, NVK's WSI
machinery is enabled (`NVK_USE_WSI_PLATFORM`), and `vkCreateSwapchainKHR`/acquire/render all work
on HW. Only the backend (headless → vi) and the present mechanism change.

---

## Target architecture

Add a **`VK_NN_vi_surface` WSI backend** to our Mesa NVK, mirroring the structure of the existing
`src/vulkan/wsi/wsi_common_headless.c` (our template — same `wsi_interface` contract).

```
app (mirrors Dan's TriangleTest.cpp)
  vkCreateViSurfaceNN(window = nwindowGetDefault())   ── new entrypoint
  vkCreateSwapchainKHR / AcquireNextImageKHR / QueuePresentKHR  (unchanged Vulkan API)
        │
        ▼
NVK wsi (src/nouveau/vulkan/nvk_wsi.c)  — already enabled
        │
        ▼
NEW: src/vulkan/wsi/wsi_common_vi.c  (the backend)
  - VkIcdSurfaceVi { NWindow *nw }
  - wsi_vi_surface_get_support / _capabilities / _formats / _present_modes
  - wsi_vi_surface_create_swapchain  →  N images, each bound to an NWindow buffer
  - acquire = nwindowDequeueBuffer ; present = nwindowQueueBuffer
        │
        ▼
libnx nwindow  (nwindowConfigureBuffer / Dequeue / Queue)  →  VI compositor → TV
        │
        ▼
winsys/drm_shim.c  — expose a BO's nvmap id + NIL layout to build the NvGraphicBuffer
```

### Pieces to build

1. **Advertise `VK_NN_vi_surface`** (instance ext) + provide `vkCreateViSurfaceNN`.
   - Mesa has `VK_NN_vi_surface` only in `vk.xml` (the registry) — **no implementation exists**.
   - Add `.NN_vi_surface = true` to `nvk_instance.c` `instance_extensions` (gated `#ifdef __SWITCH__`,
     next to the existing WSI exts under `NVK_USE_WSI_PLATFORM`).
   - Implement `wsi_CreateViSurfaceNN` in `wsi_common_vi.c` (register it as a wsi entrypoint, or
     add it to the wsi entrypoint generator list). It allocates a `VkIcdSurfaceVi { base.platform =
     VK_ICD_WSI_PLATFORM_VI; NWindow *window; }` and returns it.
   - `wsi_common.c` must route `VK_ICD_WSI_PLATFORM_VI` surfaces to the vi backend (like it routes
     headless). Add the `case` + `wsi->vi` interface pointer (initialised in `wsi_device_init`,
     guarded so it only inits on horizon).

2. **`wsi_common_vi.c` backend** (copy `wsi_common_headless.c`, swap the guts):
   - `get_capabilities`: `minImageCount = 2`, `currentExtent` from `nwindowGetDimensions` (1280×720),
     `maxImageCount` = how many NWindow slots we allow (libnx default supports up to ~3-4),
     `supportedUsageFlags = COLOR_ATTACHMENT | TRANSFER_DST | TRANSFER_SRC | SAMPLED`,
     `supportedCompositeAlpha = OPAQUE`.
   - `get_formats`: `VK_FORMAT_R8G8B8A8_UNORM` (+ `B8G8R8A8_UNORM`) / `SRGB_NONLINEAR`. Must be a
     format the VI compositor can scan out (RGBA/BGRA 8888 — see `PIXEL_FORMAT_RGBA_8888`).
   - `get_present_modes`: `VK_PRESENT_MODE_FIFO_KHR` (the NWindow is vsync'd via swap interval).
     Optionally `MAILBOX`/`IMMEDIATE` via `nwindowSetSwapInterval(0)` when ≥3 buffers.
   - `get_support`: always `VK_TRUE` for the gfx queue family.
   - `create_swapchain`: see "the core" below.

3. **The core: each swapchain image ⇄ an `NWindow` buffer (zero-copy).**
   This is the integration Dan meant by "understand nwindow". `NvGraphicBuffer` (libnx
   `nvidia/graphic_buffer.h`) is what `nwindowConfigureBuffer(nw, slot, NvGraphicBuffer*)` takes;
   its `planes[0]` (`NvSurface`) carries the exact tiling the compositor reads:
   ```c
   typedef struct { u32 width, height; NvColorFormat color_format; NvLayout layout; u32 pitch;
       u32 unused; u32 offset; NvKind kind; u32 block_height_log2; NvDisplayScanFormat scan;
       u32 second_field_offset; u64 flags; u64 size; u32 unk[6]; } NvSurface;
   typedef struct { NativeHandle header; s32 unk0; s32 nvmap_id; ... u32 format; u32 stride; /*px*/
       u32 total_size; u32 num_planes; NvSurface planes[3]; ... } NvGraphicBuffer;
   ```
   **Approach A (zero-copy, the goal): NVK allocates, NWindow wraps.** Per slot:
   - Create a normal NVK `VkImage` (2D, the chosen format, `TILING_OPTIMAL` = block-linear,
     usage COLOR_ATTACHMENT|TRANSFER_DST), allocate+bind DEVICE_LOCAL memory.
   - Pull the image's **NIL layout**: `pitch`, `size_B`, **`pte_kind`** (colour kind, e.g. `0xfe`
     — same one our depth fix maps via `op->flags & 0xff`), **GOB `block_height_log2`** (NIL
     `gob_height_log2`), offset 0. NVK exposes these on `nvk_image` (`nil_image` → `level[0]`
     `row_stride_B`, `tiling.{gob_height_log2,...}`, `pte_kind`); fall back to
     `vkGetImageSubresourceLayout` for pitch/size.
   - Pull the **nvmap id** for that image's memory from `drm_shim` (new helper, below).
   - Fill an `NvGraphicBuffer`: `nvmap_id`, `format = PIXEL_FORMAT_RGBA_8888`, `stride` (px),
     `total_size`, `num_planes = 1`, `planes[0] = { w, h, NvColorFormat_A8B8G8R8, NvLayout_BlockLinear,
     pitch, kind = <NIL pte_kind>, block_height_log2 = <NIL gob>, size }`. (`magic=0xDAFFCAFF`,
     `pid=42`, usage = the standard gralloc render+scanout flags — copy libnx `framebufferCreate`'s.)
   - `nwindowConfigureBuffer(nw, slot, &gb)`.
   - The swapchain's `get_wsi_image(slot)` returns this VkImage; the compositor scans out of its
     memory directly. **No blit, no copy.**
   - **THE RISK / the crux:** NIL's block-linear layout (`kind`, `block_height_log2`, `pitch`,
     `size`) must EXACTLY match what the VI compositor expects, or the image shows garbled/faults.
     This is "understand nwindow". Ground-truth it against libnx `framebufferCreate` /
     `nvioctlNvmap_*` (what libnx fills for its own `Framebuffer`'s `NvGraphicBuffer`) and against
     NIL's tiling. (cf. the depth fix: code-reading the kind wasn't enough — we instrumented NIL.)

   **Approach B (copy-on-present, the de-risked first milestone):** swapchain images = our own
   DEVICE_LOCAL render targets (as now). Separately allocate the N `NvGraphicBuffer`s (via libnx,
   like `framebufferCreate` does — known-correct layout). On present, record a small **own**
   command buffer (`vkCmdCopyImage`/blit render-image → the dequeued buffer's image) + submit it
   (our submit path WORKS — it is NOT the crashing generic WSI blit) + `nwindowQueueBuffer`.
   Proves the nwindow present loop end-to-end while sidestepping the NIL↔compositor layout match;
   swap to Approach A once the layout is nailed.

4. **`drm_shim.c` additions** (small, additive):
   - `int drm_shim_bo_nvmap_id(uint32_t gem_handle)` (or by VA) → the libnx nvmap id our shim
     stored at GEM_NEW (we already create the nvmap; just expose its id). The wsi backend maps a
     `VkDeviceMemory`→bo handle→nvmap id. (NVK `vkGetMemoryFdKHR`/an internal accessor may also
     reach the bo; simplest is a shim getter keyed by the VA NVK assigned.)
   - Nothing else: VM_BIND/EXEC unchanged. The NWindow scanout reads via the nvmap id; no GPU
     submit involved in present.

5. **Sync model (acquire/present ⇄ `NvMultiFence`).**
   - `nwindowDequeueBuffer(nw, &slot, &fence)` returns an `NvMultiFence` the compositor signals
     when the buffer is free to write. acquire must make the app's `acquireSem`/fence wait on it.
     Simplest first: pass `NULL` (libnx waits CPU-side on the fence; needs `nvFenceInit`), then
     signal the Vulkan acquire semaphore ourselves (or have the app `vkQueueWaitIdle`-pace, as our
     test loop already does).
   - `nwindowQueueBuffer(nw, slot, &fence)`: `fence` = the render-complete `NvFence`. The cleanest
     is to hand the compositor the channel fence from the render submit (our winsys already stashes
     an `NvFence` per submit — expose it), so the compositor waits GPU-side. First milestone may
     `vkQueueWaitIdle` before queue and pass `NULL` (correct, just not pipelined).
   - This is where a real WSI earns 60fps; the first pass can be waitidle-paced like our current loop.

---

## Milestones

- **M-WSI-0 — surface + swapchain create over VI (no present yet).** Advertise `VK_NN_vi_surface`,
  implement `vkCreateViSurfaceNN` + the vi backend `get_*`, `create_swapchain` allocating N images
  (Approach B images first). Verify `vkCreateSwapchainKHR`→0 + `vkGetSwapchainImagesKHR`→N on HW.
- **M-WSI-1 — present via copy (Approach B).** acquire=dequeue, render, own-blit→NvGraphicBuffer,
  queue. **Expect the animated quad on the TV via the REAL VI compositor** (not our framebuffer
  blit hack). This is the milestone that proves the nwindow present loop.
- **M-WSI-2 — zero-copy (Approach A).** Bind swapchain VkImages' memory as the NvGraphicBuffers
  (NIL layout ↔ NvSurface). Remove the copy. Nail the kind/GOB layout match. Real zero-copy present.
- **M-WSI-3 — GPU-side sync + present modes.** Hand the render `NvFence` to `nwindowQueueBuffer`,
  drop the per-frame waitidle → smooth 60fps; FIFO/MAILBOX via swap interval.
- **(then) Dawn-over-Vulkan** can drive this swapchain — the ports enabler (ROADMAP item 8).

## Files
- NEW `src/vulkan/wsi/wsi_common_vi.c` (+ entry in `src/vulkan/wsi/meson.build`, in the non-Windows
  branch next to headless). Template = `wsi_common_headless.c`.
- EDIT `src/nouveau/vulkan/nvk_instance.c` — advertise `NN_vi_surface` (`#ifdef __SWITCH__`).
- EDIT `src/vulkan/wsi/wsi_common.c` / `wsi_common_private.h` — route `VK_ICD_WSI_PLATFORM_VI`,
  add `wsi->vi`, init it in `wsi_device_init` on horizon.
- EDIT `winsys/drm_shim.c` (+`.h`) — `drm_shim_bo_nvmap_id()` accessor.
- NEW app `winsys/smoke/nvk_vi_swapchain.c` — mirrors Dan's `TriangleTest.cpp` (vi surface + the
  spinning quad). Reuses our proven device/swapchain/render code from `nvk_swapchain.c`.
- Fold the durable Mesa edits into `patches/switch-nvk-mesa-25.0.7.patch`.

## References
- Dan's WSI template: `github.com/dantiicu/vulkan-triangle-test-switch/TriangleTest.cpp` (MIT) — the VI surface + swapchain flow above.
- Backend template: `mesa-25/src/vulkan/wsi/wsi_common_headless.c` (same `wsi_interface` contract).
- libnx nwindow API: `switch/display/native_window.h` (Configure/Dequeue/Queue), `switch/nvidia/graphic_buffer.h` (`NvGraphicBuffer`/`NvSurface` — the layout fields), `switch/display/framebuffer.h` (`framebufferCreate` = a known-correct `NvGraphicBuffer` build to copy).
- Layout ground-truth: NIL tiling (`pte_kind`, `gob_height_log2`) vs libnx `Framebuffer`/`nvmap`. See the depth fix in [[vulkan-nvk-switch-path]] for the kind-matching method (instrument, don't code-read).
