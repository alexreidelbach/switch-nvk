---
name: dusklight-switch-port
description: Use when working on the Dusklight Switch homebrew port OR the standalone NVK-Vulkan-on-Switch effort (D:\switch-nvk) — anything involving libnx, devkitPro toolchain, aarch64-none-elf cross-compile, Aurora (encounter/aurora or dantiicu/aurora-switch), Dawn (WebGPU), Mesa-on-Switch (libEGL/libGLESv2/libglapi), Mesa NVK / nouveau winsys / NAK / GM20B / Tegra X1, cross-building Mesa for Switch, deko3d, JSystem GX (TEV/BP/CP), Twilight Princess decomp (zeldaret/tp), Eden/Yuzu emulator testing, or anything referencing dusklight/extern/aurora, platforms/switch/, build-switch/, D:\switch-nvk, dantiicu, ticohq, /opt/nvk-switch.
---

# Dusklight Switch Port — Operational Knowledge

This skill primes you for the highly specific work of porting Dusklight (open-source TP decomp reimplementation in C++) to Nintendo Switch via libnx homebrew. Repo is at `D:\Projects\dusklight`. The full reference dossier is at `D:\Projects\dusklight\platforms\switch\AI_SKILL_DOSSIER.md`. The full build status is at `platforms/switch/BUILD_STATUS.md`, and the latest session-end state is in `platforms/switch/RESUME_HERE.md`. Read those if you need depth.

## ⭐⭐ ACTIVE EFFORT (2026-05-27) — STANDALONE NVK Vulkan-on-Switch (separate project, NOT Dusklight)

**The current work pivoted to building our OWN Vulkan driver (Mesa NVK) for the Switch, as a STANDALONE
effort in `D:\switch-nvk\` (a git repo). It MUST NOT involve Dusklight (hard scope rule). READ
`D:\switch-nvk\RESUME_NVK.md` FIRST — it is the live source of truth (status, 3-command build recipe,
file inventory, the exact M1/M2 symbol spec, next steps).**

- **GOAL (narrow):** reproduce Dan's first artifact — a standalone "Vulkan art placeholder that depends
  on Vulkan to run" (the M3 triangle/smoke test). **NO in-game, NO port integration of any kind** (M4 is OUT).
- **DONE:** M0 (Rust std cross-compiles+links), M1-build (Mesa 25.0.7 NVK fully cross-compiles; `libnvk.a`
  + all static libs), M1-winsys (whole NVK driver LINKS into a Switch EXE/NRO), **M2-winsys ✅ PROVEN ON
  REAL TEGRA (2026-05-27, v32).**
- **🎉 M2 SMOKE TEST PASSED ON REAL TEGRA (2026-05-27, v32):** `I VERIFY OK: all 1024 words ==
  0xcafebabe / SMOKE TEST PASSED — NVK rendered to memory on Tegra`. Full headless Vulkan runs end-to-end
  on GM20B: vkCreateInstance→enumerate→**vkCreateDevice=0**→buffer/mem/map/bind→vkCmdFillBuffer(0xCAFEBABE)
  →**vkQueueSubmit=0**→**vkQueueWaitIdle=0**→**CPU readback==0xCAFEBABE**. The ENTIRE M2 winsys is validated
  on hardware (device, VM_BIND, GPFIFO submit, fence signalling, GPU execution, CPU/GPU coherency).
  - **THE FIX (v32):** after `nvGpuChannelIncrFence`, **append the builtin FENCE CMDLIST** (mirrors
    `libdrm_nouveau/pushbuf.c:226`; the v22 removal was the bug, made on the misread 0xd5c=ENOMEM — it's
    really Timeout). IncrFence alone only bumps libnx's counter; the GPU increments the syncpt only when
    that cmdlist RUNS. Its 3rd dword `syncpt_id|(1<<20)|(1<<16)` = **incr + GPU L2 FLUSH** → fixed BOTH
    completion (drain rc=0x0, reached=1) AND coherency (CPU sees 0xCAFEBABE). v31 `-1` hack reverted.
  - **Settled:** GM20B fully usable with ZERO FECS/priv-reg writes (Mesa 20 `nvc0_magic_3d_init` proves
    it) → v28 no-op of `nvk_mme_set_priv_reg` is correct; both init priv-writes non-essential, the
    conservative-raster one lazy/draw-time. NVK self-uploads its 3D golden state + MME macros.
- **🎉 M3 GRAPHICS WORKING ON REAL TEGRA (2026-05-27):** our NVK does, all shown on the Switch TV (libnx
  framebuffer blit of NVK-rendered images): a drawn **triangle** (NAK shaders) → **textured quad** ("VULKAN"
  logo: texture upload + sampler + descriptor set) → **3D textured cube** (vertex buffer + MVP UBO +
  descriptor) **spinning @ vsync 60 fps** → **3D cube WITH a depth buffer** → and a **POC running
  SaschaWillems/Vulkan's VERBATIM `triangle.vert/.frag`** (MIT) compiled by our NAK = the canonical RGB
  Vulkan triangle, established PC code on our driver. Apps: `winsys/smoke/{nvk_tri,nvk_logo,nvk_scene,nvk_poc}.c`;
  shaders → `gen-shaders.sh`(glslangValidator) → `tri_shaders.h`; `build-nro.sh` takes `APP/TITLE/VERSION`.
  - **DEPTH ✅ RESOLVED (the universal 3D blocker — fix-once → all 3D + Dawn-over-Vulkan ports).** Root
    cause: `src/vulkan/runtime/vk_image.c` sets `drm_format_mod=INVALID` only `#if LINUX||BSD`; our HORIZON
    port left it `0`=`MOD_LINEAR` → NIL forced ALL images linear → GM20B ZETA GR-faults on `CLEAR_SURFACE`(Z).
    Fix: **(1)** add `DETECT_OS_HORIZON` to that guard (folded into the patch); **(2)** `drm_shim.c vm_bind_op`
    takes the PTE kind from `op->flags & 0xff` (NVK puts it there for the new uAPI, not GEM_NEW tile_flags).
    Confirmed: depth `pte_kind=0x7b`, `gob=1`, no ERRNOTIF, cube renders. Diagnosed by instrumenting NIL
    (ground-truth `mod=0x0`) — code-reading "should be 0x7b" vs HW "0" only broke once we INSTRUMENTED. See [[nvk-winsys-debugging-patterns]] #13.
  - **Lessons:** `DRM_SHIM_DEBUG` per-frame SD `fflush` throttled the spin to ~10fps (build without it for 60);
    framebuffer apps fault on process teardown (irrelevant for a POC photo); present = libnx framebuffer, not Vulkan WSI yet.
- **NEXT:** the `texturecubemap` POC (6-layer CUBE image + `samplerCube` + Sascha's skybox shaders, saved as
  `sascha_skybox.{vert,frag}`); then real WSI/swapchain over `nwindow`/`vi` (ref `dantiicu/vulkan-triangle-test-switch`).
- **Reproducible/durable:** the 8 Mesa source patches are in `patches/switch-nvk-mesa-25.0.7.patch`
  (`apply-patches.sh` restores idempotently). Build = `build-native-tools.sh` → `configure-mesa.sh`
  → `ninja`; the smoke `.nro` via `winsys/build-nro.sh`. ⚠️ The `nvk_cmd_draw.c` FECS no-op patch is a
  WORKING-TREE edit — fold it into the patch file before any clean re-extract.
- **KEY pitfalls:** cross `-Dmesa-clc=enabled` wrongly builds LLVM FOR the target (use 2-phase native
  tools); Docker bind-mount mtimes make ninja SKIP edits (delete the `.o`); `nvFenceWait` timeout is µs;
  decode every libnx Result before theorizing. Full list: `[[nvk-winsys-debugging-patterns]]`.
- Memories: `[[dan-nvk-intel-and-goal]]`, `[[vulkan-nvk-switch-path]]`, `[[nvk-winsys-debugging-patterns]]`,
  `[[feedback-nvk-read-full-log]]`, `[[scope-dusk-eden-only]]`.

**The Dusklight GLES render work below is PAUSED in favor of the NVK effort (still valid; resume via
`platforms/switch/RESUME_HERE.md` if/when we return to it).**

## ⭐ LATEST (2026-05-25) — native Mesa-GLES backend running on HW; engine boots to TITLE + GX capture works (gx=1). No pixels yet (replay log-only).

**Read `platforms/switch/RESUME_HERE.md` first — it's current. This section supersedes the deko3d-era render plan below.**

- **Backend = native Aurora Mesa-GLES (deko3d OFF).** `DUSK_GLES=ON DUSK_DEKO=OFF DUSK_RMLUI=OFF`. `lib/gles/gpu.cpp` (EGL→ES3.1 ctx→present) PROVEN on real Tegra (`OpenGL ES 3.2 Mesa 20.1 nouveau`). Pivot rationale: GLES compiles GLSL at runtime (no deko offline-DKSH/coverage tail); reusable for other GC/Wii Switch ports. NOT Dawn-on-GLES (fell to Null), NOT Dan's NVK-Vulkan (Maxwell-weak + private). Plan: `PLAN_GLES.md`. Decision memory: `[[dusklight-gles-backend-plan]]`.
- **Boot cascade DONE** (backend-agnostic null-guards, Eden-masked, crash on real HW): TLS `tls_model("initial-exec")` not global-dynamic (#18); audio/fade/vibration/kankyo-player/camera-Midna null-guards (#20). Engine boots THROUGH to the **TITLE scene (F_SP102)**, runs steadily (frame 23+).
- **G2 step 1 DONE: GX capture works** — `[gfx-deko] gx=1`, `[title] Draw`. Guards (heuristic #21): generalize deko `#ifdef`s (incl. common.hpp decls) to `|| AURORA_BACKEND_GLES`; texture.cpp 5 stubs (broken textures OK); **`find_pipeline_impl` hash-only** (unblocks gx>0). `build-docker.sh build-only` skips the ~4min configure (Docker is NOT CPU-limited).
- **⚠️ NO PIXELS YET — the TV shows only the GLES blue clear. `deko_replay` is LOG-ONLY (no glDrawElements).** "gx=1 / title Draw" = capture working, NOT visible. **Verify pixels by the TV, not logs (heuristic #21).** REMAINING (the pixel work): fix a ByteBuffer push UINT32_MAX-length crash, then write `gfx::gles_replay` (GL SSBO/EBO + runtime flat GLSL + `glDrawElements`) from the real S2DUMP layout (verts = BIG-ENDIAN XYZ stride=12, uint16 tris, no per-draw uniform). See RESUME_HERE.md + heuristic #21.
- The deko3d state below is HISTORY for the RENDER path; the engine/boot/heap infra it describes is backend-agnostic and carries over.

## Project state (deko3d era — 2026-05-23; render path superseded by the GLES pivot above)

- **Stack (was ACTIVE, now pivoting to GLES):** Dusklight → Aurora (gx) → **deko3d** (native) → libnx → Tegra X1. The FIRST Dawn→switch-mesa→GLES attempt was abandoned (Dawn fell to Null) — but the NEW plan is a NATIVE aurora GLES path (raw EGL+GLESv2, no Dawn), which the raw `egl_test` POC proved works in Eden.
- **Build:** cross-compiles aarch64-none-elf via devkitA64 in `devkitpro/devkita64` Docker. `bash platforms/switch/build-docker.sh build` produces only the `.elf`; the strip + `elf2nro --icon --nacp` step is separate and required for a runnable `.nro`. RmlUi is now `AURORA_ENABLE_RMLUI=ON`.
- **Renders ✅:** native deko3d pipeline proven (uam→DKSH→runtime load→draw, RGB triangle); RmlUi-on-deko proven; the **authentic prelaunch HOME renders in Eden ~60 fps** — real logo/bg PNGs (libpng), Fira Sans/Alegreya fonts, gradient, **offscreen-layer opacity fades**, controller-navigable menu (M3 input done).
- **Boots ✅:** engine boots to the main loop; the `.gcm` disc loads the FULL game via libnx stdio (`aurora_dvd_open`) — whole engine inits, no crash. But `fapGm_Execute` (per-frame gameplay + GX render) is STUBBED → **no gameplay pixels yet**.
- **Next (the big chunk):** **Phase 4 = GX→deko3d gameplay render** via a precompiled DKSH shader cache keyed by `ShaderConfig` hash (`PLAN_PHASE4.md`; deko3d has no on-device shader compile — uam is CLI-only). Blocker: needs a working PC build to run the stage-walker that dumps every `ShaderConfig` (Switch can't dump — the cache only fills when a draw renders, and gameplay doesn't render yet). Menu-authenticity follow-up (re-include real `src/dusk/ui/`) is `PLAN_MENU.md`, optional polish.
- **Branch:** `switch-port/deko3d-backend` (active, off `main`), committed but NOT pushed. `switch-port/opengl-core` was an abandoned aside.

## Anti-patterns — DO NOT REPEAT THESE

These were observed in real AI sessions on this codebase. Each one cost real time. The 6 below are the EGL/Dawn-era findings; the **deko3d-era ones (#7–12)** live in the `dusklight-debugging-heuristics` memory and matter more now — notably: `DuskLog.fatal`/engine `fatal()` calls `std::abort()` (#7); a deko3d `DkCmdList` is invalidated by `dkCmdBufClear` so record inline per frame (#8); **`svcOutputDebugString` drops under heavy log volume — use `dusk_switch_log` (file) for engine traces (#9)**; RmlUi-on-deko needs `dkCmdBufBarrier(Fragments, InvalidateFlags_Image)` before sampling an offscreen layer, and per-frame reset/reuse of layer images to avoid descriptor-slot overrun.

### 1. Header presence ≠ runtime support
`#ifdef EGL_KHR_fence_sync` in `eglext.h` does NOT mean Mesa runtime advertises the extension. Always check `eglQueryString(EGL_EXTENSIONS)` at runtime, not headers at compile-time. Mesa 20.1 (devkitPro) advertises only: `EGL_KHR_config_attribs`, `EGL_KHR_create_context`, `EGL_KHR_get_all_proc_addresses`, `EGL_KHR_surfaceless_context`. Anything else is missing.

### 2. `TARGET_PC` is defined ON Switch too
`CMakeLists.txt:317` sets `TARGET_PC` on every non-original-GC build (PC, Android, iOS, Switch). For excluding PC-specific code on Switch use **`#ifndef __SWITCH__`** (the devkitA64 toolchain macro), never `#if !TARGET_PC`.

### 3. `bool initialize() returned true` does NOT mean working
Aurora's `webgpu::initialize` returns true even when Dawn picked the Null backend. Always check the actual chosen backend (`g_backendType`). Mental model: "init succeeded → check which adapter you got." See `extern/aurora/lib/webgpu/gpu.cpp` around line 467 for `g_adapter.GetInfo`.

### 4. Don't reason about layer N's bug while layer N-1 is unproven
**Lesson from this session:** instead of patching Dawn-on-GLES blindly, building a 100-line standalone `egl_test.nro` proved that switch-mesa+EGL works in Eden. Should have been step 1, not step 10. **Always build a minimal POC of the layer below before instrumenting the layer above.**

### 5. Don't infer enum ordinals — grep them
When traces show `type=6` or `wgpu_backend=1`, the FIRST move is `grep -n 'enum.*Backend' extern/aurora/include/` (or the relevant SDK). Never guess from position. AuroraBackend is `{ AUTO=0, WEBGPU=1, D3D11=2, D3D12=3, METAL=4, OPENGL=5, OPENGLES=6, VULKAN=7, NULL=8 }`. `wgpu::BackendType` is `{ Undefined=0, Null=1, ..., Vulkan=6, OpenGL=7, OpenGLES=8 }`.

### 6. Newlib ctype macro pollution is a known landmine
`<ctype.h>` defines macros `_U _L _N _S _P _X _B _C` — any of these as a class member, struct field, or local variable name will fail to compile on aarch64-none-elf. Pre-emptively grep before assuming code is portable.

## Critical conventions

- **Filtered PC-only sources:** `CMakeLists.txt:472-490` excludes `src/dusk/{audio,imgui,ui,file_select,iso_validate,autosave,gyro}` on Switch. EXCEPTIONS re-included: `JASCriticalSection.cpp` and `DspStub.cpp` (no SDL/PC deps). NOTE: the prelaunch HOME currently renders from a hand-built RML in `extern/aurora/lib/rmlui.cpp` (`rmlui::initialize`) + custom input in `update_menu_input` — re-including the real `src/dusk/ui/` is a deferred follow-up (`PLAN_MENU.md`).
- **RmlUi is ON:** `AURORA_ENABLE_RMLUI=ON` in `build-docker.sh`; the deko render interface is `lib/rmlui/DekoRenderInterface.cpp` (not the wgpu `WebGPURenderInterface`, which is excluded on deko). Validated deko UI APIs are in the `deko3d-2d-rmlui-api` memory.
- **deko3d has no runtime shader compile:** uam is a CLI tool; deko loads only prebuilt `.dksh`. Embed DKSH as a C header (see the triangle/`tri_shaders_dksh.h` + RmlUi `rml_shaders_dksh.h` pattern). This is why Phase 4 uses a precompiled DKSH cache.
- **Applet config:** `platforms/switch/src/switch_stubs.cpp` sets `__nx_applet_type = AppletType_Application` and `__nx_heap_size = 0`. Without these → LibraryApplet → ~512MB heap → OOM.
- **File-based logging:** `dusk_switch_log(msg)` writes to `sdmc:/dusklight.log` AND `svcOutputDebugString` (Eden captures latter, file logs are crash-safe). Define `extern "C" void dusk_switch_log(const char*);` in any TU you want to trace from.
- **Eden log path:** `C:\Users\Guilherme\AppData\Roaming\Eden\log\eden_log.txt` (live debug stream) and `C:\Users\Guilherme\AppData\Roaming\Eden\sdmc\dusklight.log` (file from inside .nro). Use `grep -aE` (binary-safe).
- **NRO production:** docker one-liner with `aarch64-none-elf-strip` + `elf2nro --icon --nacp`. See history in BUILD_STATUS.md.

## Domain references (paste these into context when needed)

| Domain | Single highest-leverage URL |
|---|---|
| libnx | https://github.com/switchbrew/libnx/blob/master/nx/include/switch/runtime/env.h |
| Switch GPU / deko3d | https://github.com/devkitPro/deko3d/blob/master/Primer.md |
| Dawn/Tint | https://dawn.googlesource.com/dawn/+/refs/heads/main/src/dawn/native/opengl/BackendGL.cpp |
| Aurora/GX TEV | http://www.amnoid.de/gc/tev.html |
| GX registers (YAGCD) | https://hitmen.c02.at/files/yagcd/yagcd/chap5.html |
| JSystem (TP decomp) | https://github.com/zeldaret/tp/tree/main/include/JSystem |
| AAPCS64 | https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst |

## When you start a Switch port task

1. **Read `platforms/switch/RESUME_HERE.md` first** — the latest session-end state (most current of all). Then BUILD_STATUS.md for deeper history.
2. **Read GAP_AUDIT.md** — assumptions catalog, which ones are validated/invalidated
3. **Check git branch** — main vs `switch-port/*` experimental branches
4. **Before any patch:** is there a smaller layer I should probe first? (anti-pattern 4)
5. **After patch:** what's the runtime evidence it worked? (anti-pattern 3) — check actual log output, not just compile/link success
