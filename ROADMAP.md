# switch-nvk — Roadmap to "ready for ports"

Goal: prove our NVK (Vulkan on the Switch GM20B, via the libnx `nv` winsys shim) is
complete enough to be the graphics backend for **real game ports** — specifically the
**Dawn-over-Vulkan** path (an engine's renderer → Dawn → Vulkan → our NVK), which is
how a GameCube/Wii port (Aurora-style) reaches the GPU.

Each item is a small standalone test app (`winsys/smoke/nvk_*.c`), built and run on
**real Tegra** (Eden/emulators don't count — they fake GPU behaviour). Every step
ends with a photo/log proof, just like the ones we already have.

---

## ✅ Already proven (the baseline)

| Proof | App | What it validates |
|-------|-----|-------------------|
| M2 smoke (write+read 0xCAFEBABE) | `nvk_smoke` | device, VM_BIND, GPFIFO, fence, GPU exec, CPU/GPU coherency |
| Triangle | `nvk_tri` | NAK shaders execute, graphics pipeline, render pass, present |
| Textured quad | `nvk_logo` | `sampler2D`, descriptor sets, texture upload (B→I copy) |
| 3D cube + **depth** @ vsync | `nvk_scene` | VBO, MVP UBO, **ZETA depth buffer (the universal 3D blocker)** |
| Sascha Willems triangle | `nvk_poc` | an established PC Vulkan program's verbatim shaders run unmodified |
| **Cubemap skybox** | `nvk_cubemap` | 6-layer `CUBE_COMPATIBLE` image, `TYPE_CUBE` view, **`samplerCube`** |

That covers: instance/device/queue, NAK vertex+fragment shaders, graphics pipelines,
render passes, vertex/uniform buffers, 2D + cube textures, depth, and a real-time loop.

---

## 🎯 What's left to prove (simple roadmap)

### Tier 1 — Core rendering correctness (small apps, fast)
The primitives every real mesh/scene uses but we haven't exercised yet.

1. **Indexed draw** — `nvk_indexed`: `vkCmdDrawIndexed` + an index buffer (16- and
   32-bit). *Why:* essentially every imported mesh is indexed; without this we can't
   draw real geometry.
2. **Many draws + blending** — `nvk_multi`: several objects in one render pass with
   pipeline/descriptor-set switches and **alpha blending** enabled. *Why:* real
   frames are dozens-to-thousands of draws; transparency is needed for UI/particles.
3. **Mipmaps + more formats** — `nvk_textures`: a mipmapped sampler, an **sRGB**
   format, and one **block-compressed** format (BC1/DXT1). *Why:* ported textures are
   mipmapped and often compressed; this exercises NIL's format/mip layout paths.

### Tier 2 — Real presentation & frame loop (the biggest port enabler)
Today we present by copying the rendered image to CPU and blitting it through a libnx
framebuffer — a hack. A real app needs a real swapchain.

4. **Swapchain / WSI** — `nvk_swapchain`: `VK_KHR_surface` + `VK_KHR_swapchain` over
   libnx `nwindow`/`vi`, zero-copy present (acquire → render → present). *Why:* this is
   the single biggest missing piece for actual ports; engines drive the screen through
   a swapchain, not a CPU readback.
5. **Multi-frame sync** — folds into #4: per-frame semaphores + fences, double/triple
   buffering, steady pacing. *Why:* correct frame overlap without stalls/tearing.

### Tier 3 — Engine features
6. **Compute pipeline** — `nvk_compute`: a real NAK **compute shader** dispatch (not
   just `vkCmdFillBuffer`). *Why:* many engines use compute (skinning, culling, FX).
7. **Render-to-texture** — `nvk_rtt`: render into an offscreen image, then sample it in
   a second pass. *Why:* post-processing, shadow maps, mirrors — pervasive in 3D games.

### Tier 4 — The actual proof for ports
8. **Dawn-Vulkan bring-up** — get `dawn-switch`'s **Vulkan backend** to initialise on
   our NVK and render one Dawn sample. *Why:* this is the bridge. Once Dawn runs on our
   driver, the Aurora-style renderer (and therefore GC/Wii ports) has a working backend
   without us hand-writing each engine feature.

---

## Definition of done — "we can start doing ports"

- **Tier 1 + Tier 2 green** → our driver renders arbitrary indexed, blended, textured
  scenes and presents them through a real swapchain.
- **Item 8 green (Dawn init + one sample)** → the renderer abstraction can drive our
  driver, so feature gaps after this are Dawn's job, not ours.

Reach those and the standalone-NVK proof is complete: the driver is a viable Vulkan
backend for ports. (MSAA, queries/timestamps, descriptor-indexing, timeline semaphores,
geometry/tessellation = nice-to-have, add only when a specific port asks for them.)

---

## Suggested order

`nvk_indexed` → `nvk_multi` → `nvk_swapchain` (the big one) → `nvk_textures` →
`nvk_rtt` → `nvk_compute` → **Dawn-Vulkan bring-up**.

Rationale: indexed + many-draws unblock real geometry immediately; the swapchain is the
highest-value single step (do it early); the rest fills coverage; Dawn last, on top of a
driver that already does everything Dawn will ask of it.
