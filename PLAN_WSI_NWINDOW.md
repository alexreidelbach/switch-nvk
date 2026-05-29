# PLAN — real WSI over libnx `nwindow` (VK_NN_vi_surface) for switch-nvk

**Status (2026-05-29):**
- **M-WSI-0** ✅ surface + swapchain create (commit `0255d30`)
- **M-WSI-1** ✅ present via Approach B (CPU-wait + queue NULL), proven on
  real Tegra in `dusklight.nro` (Dusklight boota e renderiza UI)
- **M-WSI-2** 🚧 zero-copy (Approach A) — receita completa do Ghidra do Dan
  abaixo (§5)
- **M-WSI-3** 🚧 native fence payload (depende de M-WSI-2)
- **Branch ativa:** `switch-port/wsi-zero-copy` (esse trabalho).
  Mais ampla `switch-port/nvk-wsi` ainda mantém M-WSI-1.

Este doc é o plano consolidado: M-WSI design original (M-WSI-0/1 já feitos) +
o roadmap completo de performance (M-WSI-2/3 + cut Dawn + pipeline cache).

## TL;DR — por que isso importa

Hoje Dusklight roda em **~17 fps**. Profile `aurora_end`:

```
total=58.8ms
  render=0.77    GPU command build
  rml=5.9        RmlUi UI draw
  copy=0.032     (zero-copy mata esse, mas já é insignificante)
  submit=16.4    GPU exec + fence wait
  present=35.6   ⭐ vsync wait — gargalo serial
```

Com zero-copy + native fence + triple buffer, pipeline destrava e levanta
para **~30 fps** (capado pelo vsync do dock @ 30Hz).

| Otimização | Esforço | Ganho | Fonte da receita |
|---|---|---|---|
| **M-WSI-2 (zero-copy NO_BLIT)** | 1-2 sem | 17 → 25-30 fps | `dan-re/RE_NOTES.md` §131-178 |
| **M-WSI-3 (native NvMultiFence)** | 3-5 dias | abre present | `dan-re/RE_NOTES.md` §82-97 |
| Triple buffering (≥3 NvGraphicBuffers) | 1-2 dias | mantém GPU ocupada | `RE_NOTES.md` §136 (cap 4) |
| Cortar Dawn (Aurora→Vulkan direto) | 3-5 sem | +20-30% extra | engenharia nova |
| NAK pipeline cache warm-boot | 2-3 dias | elimina spikes iniciais | NVK upstream |
| Batching GX (reduzir draws/frame) | semanas | depende cena | profile-driven |

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

---

## 5. M-WSI-2: receita exata do Ghidra do Dan (zero-copy / NO_BLIT)

> Esses são os endereços decompilados em `dan-re/RE_NOTES.md` §131-178. Não
> reinventem — copiem o algoritmo.

### 5.1 `wsi_switch_surface_create_swapchain` (Ghidra: `FUN_71000f3600`)

```c
// Cap minImageCount a 4 (chain stride 0x118, base 0x278)
if (pCreateInfo->minImageCount > 4) return VK_ERROR_OUT_OF_DATE_KHR;

// Format gate: SÓ R8G8B8A8 (37) e B8G8R8A8 (44)
if (format == VK_FORMAT_R8G8B8A8_UNORM) {
    pixfmt_idx = 1;
    nvcolor_format = 0x532120;
} else if (format == VK_FORMAT_B8G8R8A8_UNORM) {
    pixfmt_idx = 5;
    nvcolor_format = 0xd12120;
} else {
    return -0xb;
}

// armazenar em chain+0x26c / +0x270 (com flag | 0x100000000)
wsi_swapchain_init(swapchain, ..., WSI_SWAPCHAIN_NO_BLIT);  // ⭐ NO_BLIT
nwindowSetDimensions(nw, extent.width, extent.height);     // FUN_71004e87f0

// per-image build (até 4)
for (i = 0; i < image_count; i++) {
    build_nvgraphic_buffer_and_configure(i);  // §5.2 (FUN_71000f2c00)
}

// slot table at chain+0x338 (stride 0x118) init 0xffffffff
// vtable:
//   acquire        = FUN_71000f3910  (§5.3)
//   get_wsi_image  = FUN_71000f2a00
//   queue_present  = FUN_71000f3c40  (§5.4)
//   destroy        = FUN_71000f3440
//   release_images = FUN_71000f2a00
```

### 5.2 NvGraphicBuffer construction (Ghidra: `FUN_71000f2c00`)

```c
// Passo 1-3: padrão Vulkan
vkCreateImage(...);                    // 2D, swapchain format, extent
vkGetImageMemoryRequirements(...);     // pick mem type (loop memoryTypeBits)
vkAllocateMemory(... VK_MEMORY_DEDICATED_ALLOCATE_INFO ...);  // sType 5
vkBindImageMemory(...);

// Passo 4: pull NIL layout (FUN_71000d31b0)
// retorna: nvmap_id, stride, block_height, size
// EQUIVALENTE NOSSO: precisamos novo helper em drm_shim — veja §5.5

// Passo 5: NvGraphicBuffer (memset 0x150 bytes, depois preenche)
NvGraphicBuffer gb = {0};
*(uint64_t*)(&gb + 0x0) = 0x2adaffcaff;          // magic 0xDAFFCAFF | (pid 42 << 32)
gb.format = pixfmt_idx;                          // 1 (RGBA8) ou 5 (BGRA8)
gb.ext_format = gb.format;
gb.num_planes = 1;
gb.planes[0].color_format = nvcolor_format;      // 0x532120 ou 0xd12120
gb.planes[0].layout = NV_LAYOUT_BLOCK_LINEAR;    // != LINEAR
gb.planes[0].kind = 0xfe;                        // ⭐ Generic_16BX2 (compositor kind)
                                                 //   ≠ NIL's pte_kind (que é 0)
gb.planes[0].block_height_log2 = nil_block_height_log2;  // GOB-8 → small log2
gb.planes[0].width = extent.width;
gb.planes[0].height = extent.height;
gb.planes[0].stride = nil_stride;
gb.planes[0].size = nil_size;
// outros consts: 0xb00 (type/usage), 0x51 (header word) — confirmar Ghidra

// Passo 6: registrar slot no nwindow (FUN_71004e8880)
nwindowConfigureBuffer(nw, slot=i, &gb);

// Passo 7: estado interno
chain->slot_to_image[i] = i;                     // identidade slot↔image
chain->per_image_fence[i] = (NvMultiFence){0};
```

### 5.3 acquire (Ghidra: `FUN_71000f3910`)

```c
VkResult wsi_switch_acquire(swapchain, timeout, semaphore, fence, &image_idx) {
    s32 slot; NvMultiFence mf;
    Result rc = nwindowDequeueBuffer(nw, &slot, &mf);  // FUN_71004e8a00
    if (R_FAILED(rc)) return VK_NOT_READY;             // (1)

    int img = chain->slot_to_image[slot];
    if (img < 0 || img != slot) {
        nwindowCancelBuffer(nw, slot, NULL);           // FUN_71004e8bd0
        return VK_ERROR_OUT_OF_DATE_KHR;               // 0xC4653214
    }

    chain->per_image_fence[img] = mf;                  // acquire-wait fence
    *image_idx = img;
    return VK_SUCCESS;
}
```

### 5.4 present — peek native NvMultiFence (Ghidra: `FUN_71000f3c40`)

```c
VkResult wsi_switch_present(swapchain, image_idx, ...) {
    VkFence render_fence = ...;  // último submit do app
    NvMultiFence mf;
    bool have_native = vk_fence_peek_native_payload(render_fence, &mf);

    if (have_native) {
        // ⭐ FAST PATH: VI compositor espera GPU-side
        return nwindowQueueBuffer(nw, slot, &mf);
    }

    // FALLBACK (M-WSI-1 atual): host wait + queue NULL
    vkWaitForFences(render_fence, ...);
    return nwindowQueueBuffer(nw, slot, NULL);
}
```

### 5.5 NvFence ↔ VkFence (drm_shim novos exports)

Para o fast path acima funcionar, `VkFence` precisa ser exportável como
`NvMultiFence`. Dan implementa via `VK_KHR_external_fence_fd`:

- `vkGetFenceFdKHR` → retorna NvFence handle
- `vkImportFenceFdKHR` → constrói VkFence a partir de NvFence
- `get_fence_sync_type` retorna `MONITORED_FENCE` (NVC7B0_SET_MONITORED_FENCE_SIGNAL_ADDRESS_*)

**Trabalho concreto no nosso drm_shim:**

- Adicionar `vkGetFenceFdKHR` / `vkImportFenceFdKHR` exports no nvk_loaderless_vk.c
- Em `drm_shim.c`, expor `drm_shim_syncobj_to_nvfence(handle, NvFence*)` helper
- WSI side: `peek_native_payload` lê a fence assinada pelo último submit
- Verificar interação com `nouveau_exec`'s `s->fence` storage (já fazemos)

### 5.6 Checklist M-WSI-2/3

- [ ] Acrescentar `WSI_SWAPCHAIN_NO_BLIT` path no nosso `wsi_common_switch.c`
- [ ] Implementar `NvGraphicBuffer` build com kind=0xfe (§5.2)
- [ ] Expor NIL layout do BO via drm_shim hook (§5.5)
- [ ] `nwindowConfigureBuffer` por slot, identidade slot=image_idx
- [ ] `vkAcquireNextImageKHR` → `nwindowDequeueBuffer` + slot→image (§5.3)
- [ ] `vkQueuePresentKHR` fast path com NvMultiFence (§5.4)
- [ ] Default `imageCount=3` (triple buffer)
- [ ] Testar com `nvk_smoke` triangle primeiro
- [ ] Depois `dusklight.nro` — profile esperado: `present<5ms`, fps≥25

---

## 6. Pós-WSI: roadmap de performance contínuo

### 6.1 Cortar Dawn (Aurora → Vulkan direto)

Dawn adiciona tradução WebGPU → Vulkan que custa ~3-5ms/frame em command
buffer build + Tint shader compile. Para port Switch único, overhead puro.

**Trabalho:**
- Aurora hoje tem `lib/webgpu/`, `lib/metal/`, `lib/d3d12/`. Adicionar `lib/vulkan/`
- Pipeline cache (descritor GX → VkPipeline)
- Render pass setup (GX EFB → Vulkan FB)
- Texture upload (GX format → Vulkan via NIL)
- Shader emit: gerar SPIR-V direto (não WGSL→Tint)

3-5 semanas. Acelera todos os ports Switch que usem Aurora.

### 6.2 NAK pipeline cache warm-boot

NVK suporta `VkPipelineCache` serialização. Hoje recompilamos pipelines a cada
boot do `.nro`.

```c
// Engine boot
FILE* f = fopen("sdmc:/dusklight/pipeline_cache.bin", "rb");
...
vkCreatePipelineCache(... &cacheInfo ...);

// Shutdown ou após N pipelines novos
vkGetPipelineCacheData(cache, &size, data);
fwrite(data, 1, size, ...);
```

2-3 dias. Elimina spikes do 1º run em Ordon (~1-2 segundos de stutter).

### 6.3 Batching GX

Profile mostra ~60-200 draws/frame em Hyrule Field. Muitos consecutivos com
mesmo material, só vertex buffer diferente. Aurora poderia fundir em
`vkCmdDrawIndexed` único com instance offset.

Profile-driven, não bloqueante. Médio-longo prazo.

---

## 7. Como começar

```bash
cd /d/switch-nvk
git checkout switch-port/wsi-zero-copy   # branch desse trabalho

# 1. Reler decompile do Dan
less dan-re/RE_NOTES.md   # §131-178 é o que importa

# 2. Editar mesa-25/src/vulkan/wsi/wsi_common_switch.c
#    - Acrescentar caminho NO_BLIT (§5.1)
#    - Build NvGraphicBuffer com kind=0xfe (§5.2)

# 3. Editar winsys/drm_shim.c
#    - Expor BO nvmap_id + NIL layout (§5.5)
#    - drm_shim_syncobj_to_nvfence helper

# 4. Build via Docker image já publicada
docker run --rm -v "$(pwd):/work" -w /work \
    ghcr.io/hayatog/switch-nvk-build:latest \
    bash compat/build-mesa.sh

# 5. Testar primeiro com nvk_smoke
nxlink -s -a 192.168.1.6 winsys/smoke/nvk_tri.nro

# 6. Depois com Dusklight (rebuild + deploy)
cd /d/Projects/dusklight
bash platforms/switch/build-docker.sh build-only
nxlink -s -a 192.168.1.6 /d/dusklight-build-nvk/dusklight.nro
```

---

## References
- Dan's WSI template: `github.com/dantiicu/vulkan-triangle-test-switch/TriangleTest.cpp` (MIT) — the VI surface + swapchain flow above.
- **Dan's Ghidra decompile:** `dan-re/RE_NOTES.md` neste repo — receitas exatas para M-WSI-2/3.
- Backend template: `mesa-25/src/vulkan/wsi/wsi_common_headless.c` (same `wsi_interface` contract).
- libnx nwindow API: `switch/display/native_window.h` (Configure/Dequeue/Queue), `switch/nvidia/graphic_buffer.h` (`NvGraphicBuffer`/`NvSurface` — the layout fields), `switch/display/framebuffer.h` (`framebufferCreate` = a known-correct `NvGraphicBuffer` build to copy).
- Layout ground-truth: NIL tiling (`pte_kind`, `gob_height_log2`) vs libnx `Framebuffer`/`nvmap`. See the depth fix in [[vulkan-nvk-switch-path]] for the kind-matching method (instrument, don't code-read).
