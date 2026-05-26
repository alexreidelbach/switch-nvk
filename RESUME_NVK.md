# NVK (Vulkan) on Nintendo Switch — RESUME / source of truth

**Standalone effort in `D:\switch-nvk\`. Does NOT involve Dusklight (hard scope rule).**
**GOAL (narrow): reproduce Dan's first artifact — a standalone "Vulkan art placeholder that
depends on Vulkan to run" (the M3 triangle/smoke test). NO in-game, NO port integration.**

Last updated: 2026-05-25.

---

## STATUS

- **M0 — Rust std for the Switch: DONE.** Custom target `aarch64-switch-horizon` + prebuilt sysroot;
  `std` builds AND links for the Switch (the piece Dan spent ~1.5 months shimming). See [[vulkan-nvk-switch-path]].
- **M1-build — NVK cross-COMPILES for the Switch: DONE.** All 837 build steps pass; the static
  libs are produced (`libnvk.a`, `libvulkan_runtime.a`, NAK/NIL Rust libs, vulkan_util/wsi, etc.).
  The Mesa 25.0.7 tree builds with our Rust+bindgen+cbindgen+LLVM-15+libclc toolchain.
- **M1-winsys — NEXT.** The final ICD `.so` link surfaces the *real* unresolved symbols, which are
  exactly the winsys + a few libc/std shims (see "THE LINK TODO" below). The `.so` itself is NOT our
  target (it also wants Switch-app linker-script symbols `__tls_*`/`main`); we link the static libs
  into a Switch EXE/NRO that uses the devkitA64 `switch.ld`. The winsys is the heart of M1.

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
| `pristine-25.0.7/` | The unmodified upstream copies of the 6 patched files (for re-diffing). |
| `compat/switch_compat.h` | Force-included shim: `HAVE_SECURE_GETENV`, `<alloca.h>`, `<stddef.h>`. |
| `compat/sys/mman.h`, `compat/sys/sysmacros.h`, `compat/syslog.h`, `compat/dlfcn.h` | Stub headers for newlib gaps (no mmap / device macros / syslog / dlopen). `-I/work/compat` is searched first so these win. |
| `compat/compat.c` | Impls for `secure_getenv`(→getenv) + the mmap/munmap/mprotect/msync/madvise stubs. **NOT yet wired into a link** — needed at EXE link time (M2). |
| `mesa-25/` | Mesa 25.0.7 source (patched). |
| `mb/` | The cross build dir (ninja). `crossfiles/` the persistent meson cross/native files. |
| `native-prefix/` | The installed native x86 tools (mesa_clc + precomp). |
| `libdrm_nouveau/` | devkitPro's Switch libdrm-nouveau (libnx nv winsys) = the M1 reference. |
| `compile.log`, `configure.log`, `native.log`, `UNDEFINED_SYMBOLS.txt` | Latest build logs + the captured link TODO. |

### The 6 source patches (in patches/switch-nvk-mesa-25.0.7.patch)
1. `src/util/u_endian.h` — `__BYTE_ORDER__` builtin fallback (newlib `<endian.h>` doesn't set the macros).
2. `src/util/detect_os.h` — add `DETECT_OS_HORIZON` for `__SWITCH__` (= POSIX) + the 0-fallback.
3. `src/c11/impl/threads_posix.c` — exclude `__SWITCH__` from native `pthread_mutex_timedlock` (use emulation).
4. `src/util/os_misc.c` — route HORIZON to `<unistd.h>` + a fixed 3 GiB `os_get_total_physical_memory`.
5. `src/nouveau/vulkan/nvk_instance.c` — guard build-id behind `HAVE_DL_ITERATE_PHDR`; SHA1(PACKAGE_VERSION) fallback for the driver UUID.
6. `src/vulkan/runtime/vk_image.h` — add `DETECT_OS_HORIZON` to the `drm_format_mod` member gate.

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
1. **M1 winsys**: write `drm_shim.c` (+ `switch_libc_shim.c` + link `compat.c`) and a meson/Makefile
   that links the NVK static libs into a Switch EXE with `switch.ld`. Reference `libdrm_nouveau/`.
2. **M2**: BO alloc (`GEM_NEW`→nvMap) + GPU VA (`VM_BIND`→nvAddressSpace) + submit (`GEM_PUSHBUF`→
   GPFIFO + nvFence).
3. **M3 (THE GOAL)**: WSI over libnx `nwindow`/`vi` (ref `dantiicu/vulkan-triangle-test-switch`) +
   a standalone `.nro` that does vkCreateInstance → enumerate our GM20B → device → draw → present.

See memories: [[dan-nvk-intel-and-goal]], [[vulkan-nvk-switch-path]], [[scope-dusk-eden-only]].
