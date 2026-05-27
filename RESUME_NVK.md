# NVK (Vulkan) on Nintendo Switch — RESUME / source of truth

**Standalone effort in `D:\switch-nvk\`. Does NOT involve Dusklight (hard scope rule).**
**GOAL (narrow): reproduce Dan's first artifact — a standalone "Vulkan art placeholder that
depends on Vulkan to run" (the M3 triangle/smoke test). NO in-game, NO port integration.**

Last updated: 2026-05-27.

---

## ⭐ M3 PROGRESS (2026-05-27 cont) — present, textures, 3D, real-time, Sascha shaders

On real Tegra, all shown on the TV via a libnx-framebuffer blit of NVK-rendered images:
- **Present path proven** — a blue clear shown on the TV (`nvk_tri.c` step 1a). Then the yellow triangle (1b, below).
- **Textures proven** (`nvk_logo.c`) — CPU-generated "VULKAN" logo uploaded (staging → `vkCmdCopyBufferToImage`) + sampler + descriptor set + textured quad → shown on screen.
- **3D + UBO + vertex buffer proven** (`nvk_scene.c`) — a textured CUBE (logo on faces): vertex buffer, MVP matrix in a UBO, descriptor set, backface culling.
- **Real-time @ 60 fps proven** — the cube SPINS smoothly at vsync 60 fps (per-frame MVP + re-submit + blit). KEY: `DRM_SHIM_DEBUG` per-frame SD-card logging was throttling it to ~10 fps with stutter (bursty SD writes) — building WITHOUT it = smooth 60.
- **POC: Sascha Willems' ACTUAL shaders run on our driver** (`nvk_poc.c`) — the verbatim `triangle.vert/.frag` from `SaschaWillems/Vulkan` (MIT) compiled by our NAK + rendered → the canonical RGB Vulkan triangle, on our driver. (Established PC Vulkan code working = proof.)
- **DEPTH is the open fix-once** — adding a depth attachment GR-errors on `CLEAR_SURFACE`(Z) because our winsys maps every BO `kind=0` (linear) but the Maxwell ZETA depth surface needs its proper Z block-linear KIND (NVK/NIL computes it, our `drm_shim` zeroes it). Fix = honor the depth image's kind in VM_BIND. Disabled for now (convex cube works via backface culling). **This blocks any 3D content with a depth buffer — i.e. essentially every 3D game/port — so it's the next foundational fix.**
- **Exit on framebuffer apps** is finicky (recursive User Break in the libnx/display teardown vs NVK's nv); for the POC it's irrelevant (photograph the held frame). `svcExitProcess` workaround partial.
- Shaders: `winsys/smoke/shaders/*.{vert,frag}` → `gen-shaders.sh` (glslangValidator, in the image) → embedded `tri_shaders.h`. `build-nro.sh` now takes `APP`/`TITLE`/`VERSION`.

---

## 🎉🎉 M3 TRIANGLE PASSED ON REAL TEGRA (2026-05-27, t1b) — NVK draws geometry + shows it on the TV

**`M3 STEP 1b PASSED — NVK rasterised a TRIANGLE on Tegra`** + a YELLOW TRIANGLE shown on the TV.
Our own NVK Vulkan driver, on the GM20B: `vkCreateDevice=0 → image+view+renderpass+framebuffer →
vkCreateShaderModule(vert+frag) → vkCreateGraphicsPipelines=0 → render pass (clear black) + vkCmdDraw(3)
→ copy image→buffer → readback: yellow=722 black=3374 other=0, center=0xff00ffff → present on screen`.

This proves the LAST unknowns of the graphics path on hardware: **NAK-compiled vertex+fragment shaders
EXECUTE on the GM20B**, the graphics pipeline + render pass + rasterisation work, and the result is
**displayed on the TV** (via a libnx framebuffer blit of the rendered image — present path proven earlier
with a blue clear). This is the classic "Vulkan triangle" milestone = Dan's first-artifact goal, on our
own from-scratch winsys. **M3 essentially achieved.**

- **App:** `winsys/smoke/nvk_tri.c` (build `APP=nvk_tri`). Shaders: `winsys/smoke/shaders/triangle.{vert,frag}`
  → `gen-shaders.sh` (glslangValidator, added to the Docker image) → embedded `tri_shaders.h`.
- **Present:** `present_shot()` — upscales the rendered 64×64 image to 1280×720 via the libnx framebuffer,
  shown until `+`. (Done AFTER Vulkan teardown to avoid GPU/display contention. NOT Vulkan WSI yet.)
- **Steps proven:** 1a = clear an image to a colour + readback + present (blue on TV); 1b = draw a real
  triangle (shaders) + present (yellow triangle on TV). Both headless-verifiable + on-screen.
- **NEXT (stretch / the "art placeholder"):** render a TEXTURED quad of the **Vulkan "Industry Forged"
  logo** — adds a sampled texture (upload via staging+copy, already proven) + sampler + descriptor set +
  a textured fragment shader. One standard increment above the triangle. (Optional polish: real Vulkan
  WSI swapchain over `nwindow` instead of the framebuffer blit — ref `dantiicu/vulkan-triangle-test-switch`.)

---

## 🎉 M2 SMOKE TEST PASSED ON REAL TEGRA (2026-05-27, v32) — the winsys is PROVEN

**`I VERIFY OK: all 1024 words == 0xcafebabe` / `SMOKE TEST PASSED — NVK rendered to memory on Tegra`.**
The full headless Vulkan pipeline runs end-to-end on real GM20B: `vkCreateInstance → enumerate GM20B →
vkCreateDevice=0 → buffer/mem/map/bind → vkCmdFillBuffer(0xCAFEBABE) → vkQueueSubmit=0 → vkQueueWaitIdle=0
→ CPU readback == 0xCAFEBABE`. This validates the ENTIRE M2 winsys on hardware: device creation, VM_BIND,
GPFIFO submit, fence/sync signalling, **GPU execution**, AND **CPU/GPU cache coherency**.

**THE FIX (v32): append the builtin FENCE CMDLIST after IncrFence** (mirrors `libdrm_nouveau/pushbuf.c:226`
exactly; the v22 removal of it was the bug, made on the misread `0xd5c`=ENOMEM — it's really Timeout).
`nvGpuChannelIncrFence` alone only bumps libnx's expected counter; the **GPU only increments the syncpt
when this cmdlist runs**. Without it the init was masked (NVK self-increments) but the fill stalled
(syncpt frozen). KEY insight (from the Mesa-20 research agent): the fence cmdlist's 3rd dword is
`syncpt_id | (1<<20) | (1<<16)` — **bit20 = syncpt increment, bit16 = GPU L2 FLUSH**. So this one cmdlist
fixed BOTH (a) completion (syncpt reaches target → drain `rc=0x0`, `reached=1`) AND (b) coherency (L2
flushed before the increment → the CPU readback sees 0xCAFEBABE, no non-cacheable mapping needed).
The v31 `-1` hack was REVERTED (it only "worked" for the init by coincidence; the cmdlist is the real fix).

**Other things now SETTLED (confirmed by deep research into the local Mesa 20 ref + the NVK source):**
- The GM20B is fully usable with **ZERO FECS/privileged-register writes** — Mesa 20 proves it (pure
  class-0xB197 method init, `nvc0_magic_3d_init`). Our v28 no-op of `nvk_mme_set_priv_reg` is correct;
  both init priv-writes (sm_disp_ctrl 0x419f78, warp_esr_report_mask 0x419e44) are non-essential
  robustness tweaks. The 3rd priv-write (conservative raster) is lazy/draw-time, irrelevant to a triangle.
- `0xd5c` = Timeout (channel reset), never InsufficientMemory. `nvFenceWait` timeout is MICROSECONDS.
- NVK self-uploads its full 3D golden state + MME macros; it does NOT need a kernel golden context. It
  relies on the winsys to SIGNAL the EXEC's sig-syncobj when the push retires (we now do, via the cmdlist).

## NEXT: M3 — the triangle + WSI (present to the TV)
The headless smoke (GPU write + readback) is done. M3 = an actual 3D draw + present:
1. **3D draw**: a real `vkCmdDraw` triangle into a VkImage. NVK builds all 3D state itself; the winsys is
   proven. Likely-needed from the Mesa-20 ref when cross-submit reads appear: append the **flush cmdlist +
   NO_PREFETCH NOP barrier** after kickoff (`pushbuf.c:255-264`) for inter-submit cache coherency.
2. **WSI/present**: VkSurface+VkSwapchain over libnx `nwindow`/`vi` (ref `dantiicu/vulkan-triangle-test-switch`).
   Replace the headless `main` with the triangle; present to the framebuffer.
3. Resume the web-research agent (`ac4f935a22fcb4362`, hit session limit) for external GM20B/WSI gotchas.

---

## ⭐ PRIOR FRONT (2026-05-27, superseded by the PASS above) — past the FECS wall; init EXEC hang

The **entire smoke pipeline runs on real hardware**: `vkCreateInstance → enumerate GM20B →
vkCreateDevice → mem/map → buffer/bind → cmdbuffer → vkQueueSubmit → vkQueueWaitIdle` all return `0`.
The init-submit fixes that actually stuck (on HW): force `GPFIFO_ENTRY_NOT_MAIN|NO_PREFETCH` on every
push (deko3d-style — made the init kickoff accepted); **skip-empty-EXEC** (signal sigs from GetFence,
no spurious fence cmdlist); standard `nvGpuChannelIncrFence`; a `SetObject` engine-bind prefix on the
first push. **The "WARMUP RAMP" idea is REVERTED** — heavy fence-cmdlist flushes WEDGED the console.

### ROOT CAUSE of the fill failure — FOUND (was NOT coherency): the FECS priv-reg wall
`vkCmdFillBuffer` "didn't land / readback zeros" because **the channel was being RESET by a GPU fault
during NVK's 3D init**, not a coherency miss. The fault: NVK's `nvk_mme_set_priv_reg` (in
`nvk_cmd_draw.c`) issues `SET_FALCON04` (method 0x2310) to write **privileged GR registers via the FECS
falcon** — at init it clears bit3 of `sm_disp_ctrl` (0x419f78) and bit14 of `warp_esr_report_mask`
(0x419e44), both labelled NON-essential robustness tweaks in NVK. **Horizon blocks homebrew FECS priv
writes** → FECS error → **MMU-fault error-notifier (`ERRNOTIF type=31`)** → channel reset → every later
submit returns **`0xd5c`**. (CORRECTION, stood wrong all session: `0xd5c` = MAKERESULT desc **6** =
`LibnxNvidiaError_Timeout`, **NOT** InsufficientMemory/desc 7 = `0xf5c`. The errnotif/GetErrorInfo decode
is what exposed the real FECS GR error behind the Timeout.)

### v28 FIX (applied + PROVEN active on HW): no-op the FECS priv-reg write
Patched `mesa-25/src/nouveau/vulkan/nvk_cmd_draw.c` → `nvk_mme_set_priv_reg` now consumes its 3 inline
args and **skips `mme_set_priv_reg` entirely** (no `SET_FALCON04`, no FECS wait-loop). Confirmed active:
the init pushbuf SHRANK `0x1d0c → 0x1cd0`, and across multiple HW runs **NO `ERRNOTIF type=31` and NO
`kickoff failed` ever appeared** (before, the FECS fault was reliable here). No console wedge/reboot —
system stays responsive. **We are past the FECS wall.**

### NEW BLOCKER (v28 on HW): the init EXEC RUNS but its GPU work never COMPLETES
The log freezes right after `EXEC push content` (the init submit), with NO `EXEC drain` line — i.e. the
init's 3D execution does **not** reach its end-of-submit syncpt increment → the completion fence never
signals → our drain's `nvFenceWait` blocks. This looked like an **infinite hang (3+ min black screen)**
because of a **TIMEOUT-UNITS BUG**: `nvFenceWait` takes the timeout in **MICROSECONDS**, but our drain
passed `2000000000` ("2s") = **2000 s ≈ 33 min**. So the app was *correctly* waiting on a fence that the
hung init never signals — just for 33 minutes.

### v29 (CODE STAGED — NOT YET BUILT): bounded waits + post-kickoff log
Fixed in `winsys/drm_shim.c`: drain `nvFenceWait` `2e9 → 2e6` (=2s); wait-syncs `-1 → 2e6` (=2s);
`drmSyncobjWait` already capped at 3s; added an `EXEC kickoff returned rc=0x%x` log BEFORE the drain (to
pinpoint kickoff-vs-drain). BUILD tag `v29 bounded-waits`, nacp `0.29.0-boundedwaits`. **This won't make
it PASS** — it makes the app FINISH instead of hanging 33 min, so we finally get the **first COMPLETE
log** (does the init drain time out? does NVK reach the fill? any late fault?). **Build + run next.**

### NEXT (user's call: STUDY, like the FECS dig): why does the init's GPU work hang now?
Either (a) a priv-reg I skipped is actually needed for the init to complete, or (b) the init now runs
PAST the FECS point and hits a new stall (a WFI / semaphore-acquire that never satisfies). Study NVK's
full device-init pushbuf sequence (what runs after the priv-reg writes) + how the native GM20B GL stack
(devkitPro Mesa 20 / libdrm_nouveau) completes init without FECS priv writes. The v29 complete log
informs this. Do NOT blind-iterate.

**Workflow note (HW iteration):** `nxlink -s` netloader is FLAKY (killing it wedges the netloader →
`Connection failed` on re-arm). **Use FTP:** `curl -T nvk_smoke.nro ftp://<ip>:5000/sdmc:/switch/` (upload
into `/switch/`, the favorited path), launch from Sphaira "boot as application", then poll
`ftp://<ip>:5000/sdmc:/nvk_smoke.log` — **ALWAYS read the whole log, every run (standing rule
[[feedback-nvk-read-full-log]])**. The smoke exits cleanly so FTP returns. If the app hangs, HOME → close
→ reopen Sphaira to flush the log. **Dan's NVK driver is PRIVATE — no shortcut, we solve the winsys
ourselves.** See [[vulkan-nvk-switch-path]].

---

## STATUS

- **M0 — Rust std for the Switch: DONE.** Custom target `aarch64-switch-horizon` + prebuilt sysroot;
  `std` builds AND links for the Switch (the piece Dan spent ~1.5 months shimming). See [[vulkan-nvk-switch-path]].
- **M1-build — NVK cross-COMPILES for the Switch: DONE.** All 837 build steps pass; the static
  libs are produced (`libnvk.a`, `libvulkan_runtime.a`, NAK/NIL Rust libs, vulkan_util/wsi, etc.).
  The Mesa 25.0.7 tree builds with our Rust+bindgen+cbindgen+LLVM-15+libclc toolchain.
- **M1-winsys — SYMBOL-COMPLETE; THE WHOLE NVK DRIVER NOW LINKS INTO A SWITCH EXE (2026-05-26).**
  A strict EXE link (`winsys/build-exe-linktest.sh`) of whole-archived `libnvk.a` + all 21 static libs +
  the winsys shim + the libc shim + a real `main`, via devkitA64 `switch.specs` (crt0 + `switch.ld`
  resolves the app/linker-script group), **succeeds with ZERO unresolved symbols** → a 12 MB `.elf`
  (`/tmp/nvk_linktest.elf`). Every group A/B/C symbol is accounted for. The pieces:
  - **`winsys/drm_shim.c` (+`drm_shim.h`)** — DEFINES every `drm*` symbol over libnx `nv`:
    `drmGetVersion`/`FreeVersion`, `drmGetDeviceFromDevId`/`FreeDevice`, `drmCommandWrite[Read]` (full
    nouveau-uAPI dispatch), `drmCloseBufferHandle`, `drmPrime*`, the full `drmSyncobj*` family,
    `drmGetCap`, `drmGetDevices2`/`drmGetDevices`/`drmFreeDevices`. Reports a **synthetic GM20B**
    (chipset 0x12b, SOC, Maxwell-B class set 0xb197/0x902d/0xb0b5/0xb1c0/0xa140 from a hardcoded NVIF
    SCLASS) so device probe/info/class-query succeed; `VM_INIT`→0 sets `has_vm_bind`. GEM alloc + CPU
    map are real (libnx nvmap, mirroring `libdrm_nouveau`). **VM_BIND + EXEC are no-op-success stubs (= M2).**
  - **`winsys/switch_libc_shim.c`** — the `open`/`close` wrap (`-Wl,--wrap=open,--wrap=close`) routing
    the DRM render node to `drm_shim_open` and BO `mmap(fd,map_handle)` to `drm_shim_mmap`, + every
    group-C libc/std gap (getrandom→randomGet, posix_memalign, pread/pwrite, flock, pthread_sigmask,
    sysconf, regcomp/regexec/regfree, pipe, mkfifo, dirfd, fstatat, getpwuid_r, chown family, getuid
    family). `mmap`/`munmap` moved here from `compat/compat.c` (BO maps must reach the shim).
  - **`src/vulkan/runtime/meson.build` patch** — compile `vk_drm_syncobj.c` on `host_machine.system()
    == 'horizon'` (provides `vk_drm_syncobj_get_type`/`_finish`; our shim supplies its `drmSyncobj*`).
    Folded into `patches/switch-nvk-mesa-25.0.7.patch` (now **7 files**, round-trip verified).
  - **What this does NOT yet do:** the EXE is a link harness (`main` just touches the ICD entry). A real
    **vkCreateInstance → enumerate GM20B → triangle → present** needs M2 (real submit) + M3 (WSI). See below.

- **M2-winsys — REAL GPU PATH IMPLEMENTED (structural); HW validation pending (2026-05-26).** The M1
  no-op stubs in `winsys/drm_shim.c` are now real, built from the libnx nv API + devkitPro
  `libdrm_nouveau/pushbuf.c` as the reference:
  - **VM_BIND** → `nvioctlNvhostAsGpu_AllocSpace` (reserve the fixed VA NVK chose) + `…_MapBufferEx`
    with `FixedOffset` carrying `buffer_offset=bo_offset, mapping_size=range` — so partial/suballocated
    binds map exactly `BO[bo_offset:+range]` at NVK's VA (the libnx `nvAddressSpaceMapFixed` *wrapper*
    can't do sub-ranges; the raw ioctl can). UNMAP (and NVK's quirk: OP_MAP w/ handle=0) → `UnmapBuffer`.
    Sparse reserves handled.
  - **CHANNEL_ALLOC** → a real `NvGpuChannel` (`nvGpuChannelCreate`) + Zcull ctx + a builtin cmdbuf
    holding the Maxwell syncpoint-increment & cache-flush command lists (copied verbatim from
    libdrm_nouveau). **CHANNEL_FREE** closes it.
  - **EXEC** → CPU-wait the wait-syncs' fences, `nvGpuChannelAppendEntry` per `drm_nouveau_exec_push`
    {va,va_len}, append the fence-incr cmdlist, `nvGpuChannelKickoff`, then stash the channel `NvFence`
    into the signalled syncobjs (+ flush/barrier prefix for the next submit).
  - **Syncobjs** now carry a real `NvFence`; **drmSyncobjWait** blocks via `nvFenceWait` (fence snapshotted
    under the lock, waited outside it to avoid stalling other submitters; abs-ns deadline → rel-µs).
  - Compile-clean (`-Wall -Wextra`) and **still links the full EXE** (`build-exe-linktest.sh`, exit 0).
  - **KNOWN HW-VALIDATION RISKS (can only be settled on real Tegra):** (1) `MapBufferEx` `page_size` — we
    pass the AS big-page size; NVK aligns to 4 KB, so small-page binds may need `page_size=0x1000`.
    (2) subchannel/engine binding — NVK may rely on kernel subchannel alloc (NVIF NEW) that our shim
    only acks; if so we must emit `SetObject` into the channel. (3) GPU-side wait (we CPU-serialize
    wait-syncs). (4) `gem_cpu_prep` is a no-op (no per-BO fence map yet).

---

## REPRODUCE (3 commands, ~minutes; native tools cache after first run)

```bash
# 0. Build the toolchain image (devkitA64 + meson + rust + bindgen + cbindgen + LLVM15 + libclc
#    + libdrm headers + the baked cross .pc's). Re-run only when the Dockerfile changes.
docker build -t switch-nvk-build -f D:\switch-nvk\Dockerfile D:\switch-nvk

# 1+2+3. Inside the image (mount D:\switch-nvk at /work):
docker run --rm -v "D:\switch-nvk:/work" -w /work/mesa-25 switch-nvk-build bash -lc '
  export NATIVE_PREFIX=/work/native-prefix; export PATH=/work/native-prefix/bin:$PATH; export BUILD=/work/mb
  bash /work/build-native-tools.sh     # phase 1: native (x86) mesa_clc + precomp (idempotent/cached)
  bash /work/configure-mesa.sh         # phase 2: cross-configure NVK for the Switch (meson)
  ninja -C /work/mb                    # compile -> static libs in /work/mb/src/**/lib*.a
'
```

If `mesa-25/` was re-extracted fresh, first run `bash /work/apply-patches.sh` to restore the 6
source patches (see patches/ below).

---

## FILE INVENTORY (everything needed; nothing lives outside D:\switch-nvk\)

| Path | Role |
|---|---|
| `Dockerfile` | Toolchain image `switch-nvk-build`. devkitA64 + meson/mako + Rust nightly + rust-src + bindgen + cbindgen + rustfmt + build-essential + LLVM-15/clang-15 + libclc-15(+runtime, has the `spirv-mesa3d.spv`) + llvmspirvlib + spirv-tools + libdrm-dev. Bakes the CLC/SPIR-V `.pc`s into the cross pkg-config dir, the libdrm headers into `/opt/switch-cross-include`, and an empty `libdl.a`. |
| `aarch64-switch-horizon.json` | Rust target spec = the built-in `aarch64-nintendo-switch-freestanding` + `target-family=["unix"]` + `env="newlib"` (this routed 45 build-std errors → 0). |
| `rustc-switch.sh` | meson `rust` wrapper: RUSTC_BOOTSTRAP + `-Zunstable-options` + `--sysroot /work/sysroot`, strips meson's injected `-C linker=`. |
| `build-std-sysroot.sh` | Builds the prebuilt Rust std sysroot at `/work/sysroot`. |
| `build-native-tools.sh` | **Phase 1.** Native x86 build of `mesa_clc` + the nouveau precomp compiler → installs to `/work/native-prefix` (persistent, idempotent — skips if already built). The cross build consumes them via `-Dmesa-clc=system -Dprecomp-compiler=system`. |
| `configure-mesa.sh` | **Phase 2.** Generates the cross-files into `/work/crossfiles` (persistent so ninja auto-regen works across `--rm` containers), injects `_GNU_SOURCE`/`_DEFAULT_SOURCE` + the compat `-I`/`-include` + `-I/opt/switch-cross-include` + `-lc -lm` into the cross c_args, removes CLC/SPIR-V `.pc`s from the cross dir (target must NOT pull LLVM), and runs `meson setup` with `vulkan-drivers=nouveau`, `mesa-clc=system`, `precomp-compiler=system`, `llvm=disabled`. |
| `apply-patches.sh` | Re-applies the 6 Mesa source patches to a fresh tree (idempotent). |
| `patches/switch-nvk-mesa-25.0.7.patch` | **The authoritative source patches** (git-apply-able, vs pristine 25.0.7). 6 files / 9 hunks. |
| `pristine-25.0.7/` | The unmodified upstream copies of the 7 patched files (for re-diffing). |
| `compat/switch_compat.h` | Force-included shim: `HAVE_SECURE_GETENV`, `<alloca.h>`, `<stddef.h>`. |
| `compat/sys/mman.h`, `compat/sys/sysmacros.h`, `compat/syslog.h`, `compat/dlfcn.h` | Stub headers for newlib gaps (no mmap / device macros / syslog / dlopen). `-I/work/compat` is searched first so these win. |
| `compat/compat.c` | Impls for `secure_getenv`(→getenv) + `mprotect`/`msync`/`madvise` no-ops. (`mmap`/`munmap` moved to `winsys/switch_libc_shim.c`.) Linked into the EXE. |
| `mesa-25/` | Mesa 25.0.7 source (patched). |
| `mb/` | The cross build dir (ninja). `crossfiles/` the persistent meson cross/native files. |
| `native-prefix/` | The installed native x86 tools (mesa_clc + precomp). |
| `libdrm_nouveau/` | devkitPro's Switch libdrm-nouveau (libnx nv winsys) = the M1 reference. |
| `winsys/drm_shim.{c,h}` | **The M1 winsys.** Implements all `drm*` link-TODO symbols over libnx `nv` (synthetic GM20B; real device-info/SCLASS/GEM; VM_BIND/EXEC stubbed = M2). `drm_shim.h` exports the `open`/`mmap` hooks for the libc wrap. |
| `winsys/switch_libc_shim.c` | The `open`/`close` wrap (→ drm_shim) + every group-C libc/std gap. `mmap`/`munmap` live here (route BO maps to `drm_shim_mmap`). |
| `winsys/main_linktest.c` | Minimal `main` for the EXE link test (touches the ICD entry; real init is M3). |
| `winsys/compile-test.sh` | Standalone devkitA64 compile-check for `drm_shim.c` (builds it + lists defined/undefined syms). |
| `winsys/build-exe-linktest.sh` | **The M1 proof.** Links the whole NVK driver + shims into a Switch `.elf` via `switch.specs` (`-Wl,--wrap=open,--wrap=close`, no `--gc-sections`). Clean link = symbol-complete. |
| `winsys/smoke/nvk_smoke.c` | **The M2 HW smoke test.** Loaderless headless Vulkan: instance→GM20B→device→mem→buffer/VM_BIND→`vkCmdFillBuffer`→submit→fence→readback. Logs each stage to `sdmc:/nvk_smoke.log`. Full Application. |
| `winsys/build-nro.sh` | Builds `nvk_smoke.c`+shims → strip → `elf2nro` (+nacp+default icon) → **`nvk_smoke.nro`** for Sphaira. |
| `nvk_smoke.nro` | The runnable artifact (launch via Sphaira as application). |
| `compile.log`, `configure.log`, `native.log`, `UNDEFINED_SYMBOLS.txt` | Latest build logs + the captured link TODO. |

### The 8 source patches (in patches/switch-nvk-mesa-25.0.7.patch)
1. `src/util/u_endian.h` — `__BYTE_ORDER__` builtin fallback (newlib `<endian.h>` doesn't set the macros).
2. `src/util/detect_os.h` — add `DETECT_OS_HORIZON` for `__SWITCH__` (= POSIX) + the 0-fallback.
3. `src/c11/impl/threads_posix.c` — exclude `__SWITCH__` from native `pthread_mutex_timedlock` (use emulation).
4. `src/util/os_misc.c` — route HORIZON to `<unistd.h>` + a fixed 3 GiB `os_get_total_physical_memory`.
5. `src/nouveau/vulkan/nvk_instance.c` — guard build-id behind `HAVE_DL_ITERATE_PHDR`; SHA1(PACKAGE_VERSION) fallback for the driver UUID.
6. `src/vulkan/runtime/vk_image.h` — add `DETECT_OS_HORIZON` to the `drm_format_mod` member gate.
7. `src/vulkan/runtime/meson.build` — compile `vk_drm_syncobj.c` on `host_machine.system() == 'horizon'` (our winsys shim provides the libdrm core syms it needs).
8. `src/util/libdrm.h` — on `__SWITCH__`, include the real `<xf86drm.h>` instead of the `!HAVE_LIBDRM` `-ENOENT` stub drm functions (so device enumeration reaches our winsys shim). **This was the bug that made `vkEnumeratePhysicalDevices` return 0 devices.**
(NOTE: a TEMP diagnostic patch in `src/vulkan/runtime/vk_instance.c` — logging the enumerate path via `g_drm_shim_log_sink` — is in the working tree but intentionally NOT in this patch; revert before a clean re-extract.)

---

## THE LINK TODO = the exact M1/M2 spec (from UNDEFINED_SYMBOLS.txt)

**A. Winsys / DRM (M1 — the core port; implement over libnx `nv`, ref `libdrm_nouveau/`):**
`drmGetVersion`, `drmFreeVersion`, `drmFreeDevice`, `drmGetDeviceFromDevId`, `drmCommandWrite`,
`drmCommandWriteRead`, `drmCloseBufferHandle`, `drmPrimeFDToHandle`, `drmPrimeHandleToFD`,
`drmSyncobjCreate`, `drmSyncobjDestroy`, `drmSyncobjWait`, `vk_drm_syncobj_get_type`, `vk_drm_syncobj_finish`.
→ Plan: a thin `drm_shim.c` intercepting `drmIoctl`/`drmCommandWrite[ReadWrite]` and dispatching the
nouveau ioctls (GETPARAM, NVIF, VM_INIT, VM_BIND, GEM_NEW/INFO/CPU_PREP/PUSHBUF, CHANNEL_ALLOC/FREE)
to libnx nv (nvMap/nvGpu/nvAddressSpace/nvFence). Report a synthetic GM20B (Tegra X1, chip 0x21).

**B. Switch-app linker-script / crt (NOT shims — provided by devkitA64 `switch.ld` + crt0 + the app
when we link the M3 EXE/NRO; they're undefined ONLY because the `.so` ICD has no app link env):**
`main`, `__argdata__`, `__tls_start/end/align`, `__tdata_lma`, `__tdata_lma_end`, `__got_start__/end__`,
`__relro_start`, `__eh_frame_hdr_start/end`.

**C. libc/std gaps (M2 glue — a `switch_libc_shim.c` + link `compat/compat.c`):**
`secure_getenv` (compat.c — just link it), `mmap`/`munmap` (compat.c), `posix_memalign`,
`getrandom` (→ libnx `randomGet`), `pread`/`pwrite`, `flock` (no-op), `pthread_sigmask` (no-op),
`sysconf` (sane defaults), `regcomp`/`regexec`/`regfree` (driconf — stub or disable),
`pipe`, `mkfifo`, `dirfd`, `fstatat`, `getpwuid_r`, `chown`/`fchown`/`lchown`/`chroot`,
`getuid`/`geteuid`/`getgid`/`getegid`/`getppid` (Rust-std unix layer — stub to 0/root), `strrchr`.

---

## NEXT STEPS
1. **M1 winsys: ✅ DONE — the whole NVK driver links into a Switch EXE** (zero unresolved symbols;
   `winsys/build-exe-linktest.sh`). drm_shim + switch_libc_shim + the meson patch are committed +
   reproducible. The EXE is a link harness, not yet a running app.
2. **M2 — ✅ enumeration WORKS; ⏳ vkCreateDevice (autonomous Eden debug loop established).**
   **⭐ FAST AUTONOMOUS LOOP (no Switch/FTP/user):** run the nro headless in **Eden** and read the log
   directly — `eden-cli.exe` at `D:\Eden-Windows-v0.2.0-amd64-gcc-standard`, sdmc at
   `%APPDATA%\Eden\sdmc\`. Cmd: `Start-Process eden-cli.exe "<nro>"`, ~70s, then read
   `%APPDATA%\Eden\sdmc\nvk_smoke.log`. Eden reproduces CPU/enumeration logic perfectly; M2 GPU ops are
   where it (and the intel) say real HW is needed. Build-level bugs are even faster: verify via `nm`
   (e.g. `U drmGetDevices2` vs a local stub) with zero runs.
   **TWO bugs found+fixed this way (both build/logic, not HW):**
   (a) **`src/util/libdrm.h`** returned `-ENOENT` *stub* `drm*` when `!HAVE_LIBDRM`, so `vk_instance.c`'s
   enumeration never reached our shim → 0 devices. Fixed: `#if defined(HAVE_LIBDRM) || defined(__SWITCH__)`
   uses the real `<xf86drm.h>` → our shim. (8th patch.)
   (b) **`drmGetDeviceFromDevId`** in our shim was a `-ENODEV` stub; `nvkmd_nouveau_create_dev` calls it →
   `vkCreateDevice` failed `-3`. Fixed: return the synthetic GM20B. Also added **`stat`/`lstat` `--wrap`**
   (NVK `stat()`s the render node for its dev_t).
   **RESULT: enumeration + device + VM_BIND + GPFIFO submit all WORKING (Eden full pipeline; HW up to
   VM_BIND confirmed).** GM20B enumerates ("NVIDIA Tegra X1 (NVK GM20B)", api 1.3.305, vendor 0x10de).
   The hard M2 layers, peeled via HW iteration + 3 deep research agents (see [[vulkan-nvk-switch-path]]):
   - **VM_BIND (GPU-VA): real-HW fix** — Tegra nvgpu rejects bare fixed-VA reservation (-EINVAL); a FIXED
     map is only valid inside a NON-fixed reservation. Shim reserves a small-page arena (`drm_shim_va_base`)
     + `nvkmd_nouveau_create_dev` points NVK's heap inside it; heap must fit nvgpu's small-page half
     (used 8 GiB). Confirmed working on real Tegra.
   - **GPFIFO submit: fix #1** — removed the DOUBLE syncpoint increment (in-stream cmdlist + `nvGpuChannelIncrFence`
     → kernel priv_cmd ENOMEM `0xd5c`); now in-stream increment only, fence = `GetFence()+1`.
   - **GPFIFO submit: fix #2 (real HW, 2026-05-26)** — even after fix #1, NVK's FIRST large init pushbuf
     still `0xd5c`'d on real Tegra: the per-channel submit pool grows lazily with size. **Fix = inert
     WARMUP RAMP at CHANNEL_ALLOC** (fence-cmdlists at 32→8192 dwords). On HW: warmup → submit clean →
     **`vkCreateDevice → 0` and the FULL pipeline runs (`vkQueueSubmit`/`vkQueueWaitIdle` → 0).**
   - **ONLY REMAINING (real HW): `vkCmdFillBuffer` write doesn't land — readback all zeros** despite a
     clean submit+waitidle. Suspects: CPU/GPU **cache coherency** (BOs are CPU-cacheable → stale readback /
     un-flushed GPU L2) or an early-returning fence. Fix: CPU-uncached host-visible BOs + GPU L2 flush +
     a self-contained shim GPU-write test to isolate execution-vs-coherency. The smoke `.nro`:
   `winsys/smoke/nvk_smoke.c`
   + `winsys/build-nro.sh` produce **`D:\switch-nvk\nvk_smoke.nro`** (12.6 MB, full Application:
   `__nx_applet_type=AppletType_Application`, `__nx_heap_size=0`). It's a loaderless ICD staged test
   (A vkCreateInstance → B enumerate GM20B → C qfam → D device → E host-vis mem+map → F buffer+bind →
   G vkCmdFillBuffer(0xCAFEBABE) → H QueueSubmit+WaitIdle → I readback verify), logging every step +
   VkResult to **`sdmc:/nvk_smoke.log`** (flushed, crash-safe) and stdout (live over `nxlink -s`). Sets
   `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1` + `MESA_SHADER_CACHE_DISABLE=1` in-code. **Launch via Sphaira
   "boot as application"** (not the album applet). Read `sdmc:/nvk_smoke.log` after: the last reached
   stage letter pinpoints the broken layer; then burn down the KNOWN RISKS above (MapBufferEx
   page_size; subchannel SetObject binding; GPU-side waits). UNRUN as of 2026-05-26.
3. **M3 (THE GOAL)**: WSI over libnx `nwindow`/`vi` (ref `dantiicu/vulkan-triangle-test-switch`) +
   a standalone `.nro`: vkCreateInstance → enumerate our GM20B → device → draw → present. Replace
   `main_linktest.c` with the real triangle. (`NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1` needed at runtime:
   GM20B is SOC/non-conformant, so NVK gates device creation behind that env var.)

See memories: [[dan-nvk-intel-and-goal]], [[vulkan-nvk-switch-path]], [[scope-dusk-eden-only]].
