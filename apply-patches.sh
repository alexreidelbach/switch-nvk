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
# -N/--forward: ignore already-applied hunks (idempotent). -p1: strip the a/ b/ prefix.
if patch -p1 -N --dry-run < "$PATCH" >/dev/null 2>&1; then
  patch -p1 -N < "$PATCH"
  echo "OK: Switch source patches applied to $TREE"
else
  # Dry-run failed => either fully applied already, or a hunk truly conflicts.
  patch -p1 -N < "$PATCH" 2>&1 | grep -v "previously applied" || true
  echo "NOTE: patches already present (or a hunk needs review) — see output above"
fi

# The compat shim, stub headers, and build scripts live alongside this file and need no patching:
#   compat/{switch_compat.h,sys/mman.h,sys/sysmacros.h,syslog.h,dlfcn.h,compat.c}
#   build-native-tools.sh, configure-mesa.sh, rustc-switch.sh, build-std-sysroot.sh
#   aarch64-switch-horizon.json, Dockerfile
echo "compat/ + build scripts are version-independent (no patching needed)."
