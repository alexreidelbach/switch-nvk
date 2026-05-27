#!/usr/bin/env bash
# M0: cross-configure Mesa NVK (Vulkan) for the Nintendo Switch.
# Run inside the `switch-nvk-build` image with /work = D:\switch-nvk mounted.
#   docker run --rm -v "D:\switch-nvk:/work" -w /work/mesa switch-nvk-build bash /work/configure-mesa.sh
set -e
RUST_TARGET="${RUST_TARGET:-aarch64-nintendo-switch-freestanding}"
BUILD="${BUILD:-/tmp/mb}"
# Cross/native files must live on the PERSISTENT mount (not /tmp): the build dir records their
# absolute paths, and ninja's auto-regen re-reads them in fresh --rm containers where /tmp is empty.
CROSSDIR="${CROSSDIR:-/work/crossfiles}"
mkdir -p "$CROSSDIR"

# devkitPro's base Switch cross-file (C/C++ toolchain, libnx includes, horizon machine).
/opt/devkitpro/meson-toolchain.sh switch > "$CROSSDIR/switch.cross"

# newlib hides POSIX/GNU symbols (posix_memalign, strdup/strndup, asprintf, alloca, qsort_r,
# fdopen, O_CLOEXEC, secure_getenv, the M_PI math constants...) behind feature-test macros; Mesa
# assumes glibc defaults. Expose them globally by augmenting the cross-file's c_args/cpp_args.
# Also wire the compat shim: -I/work/compat (searched before newlib, so our sys/mman.h &
# sys/sysmacros.h stubs win) + force-include switch_compat.h (HAVE_SECURE_GETENV, alloca, etc.).
sed -i "s|'-D__SWITCH__',|'-D__SWITCH__','-D_GNU_SOURCE','-D_DEFAULT_SOURCE','-DVK_USE_PLATFORM_VI_NN','-I/work/compat','-I/opt/switch-cross-include','-include','/work/compat/switch_compat.h',|g" "$CROSSDIR/switch.cross"

# The aarch64-none-elf (bare-metal) triple doesn't auto-link libc/libm into a `-shared` object, so
# the Vulkan ICD .so link fails on fprintf/fwrite (NVK's method-dump + nir_print debug code) +
# our compat impls. Add them + the compat objects' libm to the link args. (Executables link libc
# automatically; only the .so needs this.)
sed -i "s|'-lnx'|'-lnx','-lc','-lm'|g" "$CROSSDIR/switch.cross"

# Our additions: the Rust compiler (NAK) + bindgen. Meson merges multiple --cross-file.
# bindgen_clang_arguments: rustc's --target is NOT forwarded to bindgen (known meson gotcha),
# so point bindgen's clang at the aarch64 libnx sysroot explicitly.
cat > "$CROSSDIR/rust.cross" <<EOF
[binaries]
rust = ['/work/rustc-switch.sh']
bindgen = 'bindgen'

[properties]
bindgen_clang_arguments = ['--target=aarch64-none-elf', '-isystem', '/opt/devkitpro/devkitA64/aarch64-none-elf/include', '-isystem', '/opt/devkitpro/libnx/include', '-D__SWITCH__', '-D_GNU_SOURCE', '-I/work/compat', '-include', '/work/compat/switch_compat.h']
EOF

# Native (build-machine) file: build-time tools (mesa_clc) + their deps (libclc, LLVM) must use the
# NATIVE x86 pkg-config/llvm-config, NOT the cross devkitPro one (which only searches Switch portlibs).
cat > "$CROSSDIR/native.txt" <<EOF
[binaries]
pkg-config = 'pkg-config'
pkgconfig = 'pkg-config'
llvm-config = '/usr/bin/llvm-config-15'
cmake = 'cmake'
EOF

# The CLC + SPIR-V tooling is BUILD-TIME only (mesa_clc, the nouveau precomp compiler). With
# mesa-clc/precomp-compiler=system the cross build consumes the NATIVE tools (built by
# build-native-tools.sh) and does NOT compile LLVM/CLC/SPIRV-Tools for the Switch. Remove their
# .pc from the CROSS pkg-config dir so the target build can't pick them up (which would wrongly
# set HAVE_SPIRV_TOOLS and pull spirv-tools/LLVM headers into target TUs like vtn_debug.c).
XDIR=/opt/devkitpro/portlibs/switch/lib/pkgconfig
rm -f "$XDIR/SPIRV-Tools.pc" "$XDIR/SPIRV-Tools-shared.pc" "$XDIR/LLVMSPIRVLib.pc" "$XDIR/libclc.pc"

rm -rf "$BUILD"
meson setup "$BUILD" --native-file "$CROSSDIR/native.txt" --cross-file "$CROSSDIR/switch.cross" --cross-file "$CROSSDIR/rust.cross" \
  --buildtype=plain --default-library=static \
  -Dvulkan-drivers=nouveau -Dgallium-drivers='' -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dglx=disabled -Dplatforms='' -Dvideo-codecs='' -Dllvm=disabled \
  -Dshared-glapi=disabled -Dgbm=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
  "$@"
