#!/usr/bin/env bash
# Re-apply the Switch (Horizon) source patches to a FRESH Mesa 25.0.7 tree.
# Needed only if /work/mesa-25 is re-extracted from the upstream tarball (the patched tree is
# already on disk; this restores the patches if it's ever wiped). Idempotent: `patch -N` skips
# hunks that are already present, so re-running is safe.
#
#   docker run --rm -v "D:\switch-nvk:/work" -w /work switch-nvk-build bash /work/apply-patches.sh
set -e
PATCH=/work/patches/switch-nvk-mesa-25.0.7.patch
TREE=/work/mesa-25

if [ ! -f "$PATCH" ]; then echo "ERROR: $PATCH missing"; exit 1; fi
if [ ! -d "$TREE" ]; then echo "ERROR: $TREE missing (extract Mesa 25.0.7 there first)"; exit 1; fi

cd "$TREE"
# Idempotency: DETECT_OS_HORIZON only exists in our patched detect_os.h. If it's already there,
# the tree is patched — skip (avoids patch(1) writing .rej noise into a correct tree).
if grep -q "DETECT_OS_HORIZON" src/util/detect_os.h 2>/dev/null; then
  echo "OK: tree already carries the Switch patches (nothing to do)"
else
  patch -p1 < "$PATCH"
  echo "OK: Switch source patches applied to $TREE"
fi

# The compat shim, stub headers, and build scripts live alongside this file and need no patching:
#   compat/{switch_compat.h,sys/mman.h,sys/sysmacros.h,syslog.h,dlfcn.h,compat.c}
#   build-native-tools.sh, configure-mesa.sh, rustc-switch.sh, build-std-sysroot.sh
#   aarch64-switch-horizon.json, Dockerfile
echo "compat/ + build scripts are version-independent (no patching needed)."
