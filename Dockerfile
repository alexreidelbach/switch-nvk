# Toolchain image for cross-building Mesa NVK (Vulkan) for the Nintendo Switch.
# Base = devkitPro devkitA64 (aarch64-none-elf gcc + libnx + meson-cross helper).
# Adds: meson/mako (Mesa build), Rust nightly + rust-src (NAK shader compiler is Rust;
# the Switch is a tier-3 target `aarch64-nintendo-switch-freestanding` needing -Zbuild-std),
# bindgen (NAK's C<->Rust FFI), and the usual codegen tools.
FROM devkitpro/devkita64

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3-pip bison flex curl ca-certificates pkg-config \
        clang libclang-dev llvm-dev git \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --break-system-packages --no-cache-dir 'meson>=1.4' mako pyyaml

# Rust nightly (build-std needs nightly + rust-src for the tier-3 Switch target).
ENV RUSTUP_HOME=/opt/rust/rustup CARGO_HOME=/opt/rust/cargo
ENV PATH="/opt/rust/cargo/bin:${PATH}"
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --default-toolchain nightly --profile minimal --component rust-src \
    && rustc --version && cargo --version

# bindgen (C->Rust FFI for NAK) + cbindgen (Rust->C headers for NIL, the nouveau image-layout lib).
RUN cargo install --locked bindgen-cli cbindgen && bindgen --version && cbindgen --version

# rustfmt: NAK's build formats its generated Rust bindings; meson errors without it.
RUN rustup component add rustfmt && rustfmt --version

# libclang for bindgen at runtime.
ENV LIBCLANG_PATH=/usr/lib/llvm-14/lib

# LLVM 15 + libclc-15 + SPIRV-LLVM-Translator (added as a late layer to keep the Rust layers cached).
# mesa_clc (build-time host tool for NVK's CL kernels) needs LLVM>=15 (Mesa 25.0) + libclc + the
# SPIR-V translator (LLVM-IR -> SPIR-V).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake llvm-15-dev clang-15 libclang-15-dev libclc-15-dev libclc-15 \
        llvm-spirv-15 libllvmspirvlib-15-dev spirv-tools libdrm-dev \
    && rm -rf /var/lib/apt/lists/*

# libdrm headers (xf86drm.h + drm-uapi) for the CROSS winsys. They are arch-neutral declarations;
# the native x86 build links the real libdrm.so, the Switch winsys will be reimplemented over libnx
# nv services (M1) so the aarch64 link is provided by our shim, not libdrm.so. Copy to a cross-safe
# include dir (NOT /usr/include, which would shadow newlib's stdio/etc. for the cross compiler).
RUN mkdir -p /opt/switch-cross-include/libdrm \
    && cp /usr/include/xf86drm.h /usr/include/xf86drmMode.h /opt/switch-cross-include/ 2>/dev/null || true \
    && cp -r /usr/include/libdrm/* /opt/switch-cross-include/ 2>/dev/null || true \
    && cp -r /usr/include/libdrm/* /opt/switch-cross-include/libdrm/ 2>/dev/null || true \
    && ls /opt/switch-cross-include/ | head
ENV LLVM_CONFIG=/usr/bin/llvm-config-15

# Make the build-machine CLC/SPIR-V deps visible to the Switch CROSS pkg-config + provide a stub
# libdl (Switch is static, no dlopen). mesa_clc is a host tool; these .pc's carry x86 paths (correct,
# the tool runs on the build machine). Baked here so cross-configure is self-contained.
RUN XDIR=/opt/devkitpro/portlibs/switch/lib/pkgconfig; mkdir -p "$XDIR"; \
    for name in libclc LLVMSPIRVLib SPIRV-Tools SPIRV-Tools-shared SPIRV-Headers; do \
      f=$(find /usr -name "$name.pc" 2>/dev/null | head -1); \
      if [ -n "$f" ]; then cp "$f" "$XDIR/"; echo "baked $f"; fi; \
    done; \
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs /opt/devkitpro/portlibs/switch/lib/libdl.a
