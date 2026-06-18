# Reproducing the switch-nvk build on a fresh machine

Goal: from clean clones, rebuild `nvk-switch/lib/libvulkan.a` (our NVK + the NWindow WSI, incl. the
swapchain-recreate fix) and then the Dusklight `.nro`. Most heavy trees are gitignored (vendored
clones / build outputs); this doc lists how to restore them.

## 1. Clone the repos
- `switch-nvk`  → github.com/HayatoG/switch-nvk (this repo, branch `master`)
- `dusklight`   → github.com/HayatoG/dusklight, branch `switch-port/v1.4.1-report`
  - submodule `extern/aurora` → github.com/HayatoG/aurora-switch, branch `switch-port/v13-report`
    (`git submodule update --init`)

## 2. Restore the gitignored deps inside switch-nvk/  (all regenerable)
Run from the switch-nvk root:
- **mesa-25/** — the patched Mesa source (the build source of truth):
  1. Download Mesa **25.0.7** source and extract to `mesa-25/`.
  2. `bash apply-patches.sh`  → applies `patches/switch-nvk-mesa-25.0.7.patch` (the COMPLETE Switch
     patch: 24 files incl. the whole NWindow WSI `wsi_common_switch.c` + the swapchain-recreate
     owner-transfer fix). Idempotent.
- **libc-switch/** — `git clone https://github.com/rust-lang/libc.git libc-switch`
- **libdrm_nouveau/** — `git clone https://github.com/devkitPro/libdrm_nouveau.git`
- **native-tools/**, **native-prefix/** — NOT cloned; produced by `build-native-tools.sh` (step 4).
- **crossfiles/** — tracked in this repo (meson cross-files; do NOT delete).

## 3. Build container
`docker pull ghcr.io/hayatog/switch-nvk-build:latest`  (or `docker build -t switch-nvk-build .`
from the tracked `Dockerfile`). All steps below run inside it:
`docker run --rm -v <abs path>\switch-nvk:/work -w /work switch-nvk-build bash -lc '<cmd>'`
(On Windows/MSYS prefix with `MSYS_NO_PATHCONV=1` so `-w /work` isn't path-mangled.)

## 4. Build order (inside the container)
1. `bash build-native-tools.sh`   → native-tools/ + native-prefix/
2. `bash build-std-sysroot.sh`     → Rust std sysroot for the Horizon target
3. `bash configure-mesa.sh`        → meson-configures `mb/` (uses crossfiles/)
4. `ninja -C mb`                   → builds the archives. NOTE: the final `.so` ICD link FAILS
   (no `-lnx` for nwindow*) — that's a known dead-end, IGNORE it; the `.a` archives build fine.
5. `bash package-nvk.sh`           → MRI-merges → `nvk-switch/lib/libvulkan.a` (~73 MB)
   - Touch a changed `.c` before `ninja` if the bind-mount mtime is stale (Docker on Windows).

## 5. Build Dusklight
From the dusklight repo (its `build-docker.sh` mounts switch-nvk at `/switch-nvk` and links
`/switch-nvk/nvk-switch/lib/libvulkan.a`):
- `bash platforms/switch/build-docker.sh build`   → `dusklight.elf`
- regenerate the NRO (separate step): run `platforms/switch/make-nro.sh` in `devkitpro/devkita64`
  with the project + build dir mounted (see make-nro.sh header) → `dusklight.nro`.

## 6. Run on HW
Sphaira netloader, then `nxlink -s -r 20 -a <switch-ip> dusklight.nro`. SD layout + the prewarmed
cache `.db` are described in dusklight's `platforms/switch/RESUME_REPORT_V141.md`.

## Notes
- The WSI source lives at `mesa-25/src/vulkan/wsi/wsi_common_switch.c` (gitignored, restored by the
  patch). The tracked `winsys/wsi/wsi_common_switch.c` is an older standalone copy and is NOT the
  build source — the patch is the source of truth.
- `dan-re/` is intentionally gitignored (third-party RE notes; do not publish).
