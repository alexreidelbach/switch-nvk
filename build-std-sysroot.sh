#!/usr/bin/env bash
# Build a prebuilt Rust std SYSROOT for aarch64-switch-horizon so meson-invoked `rustc`
# (which can't use cargo's -Zbuild-std) finds std. Run in switch-nvk-build with /work=D:\switch-nvk.
set -e
export RUST_TARGET_PATH=/work RUSTC_BOOTSTRAP=1
export RUSTFLAGS='-Zunstable-options -L /opt/devkitpro/devkitA64/aarch64-none-elf/lib -L /opt/devkitpro/libnx/lib'
TGT=aarch64-switch-horizon
SYSROOT=/work/sysroot
RUSTC_SYSROOT="$(rustc --print sysroot)"

# 1. build-std (release) on a lib crate -> produces std + deps rlibs.
rm -rf /tmp/stdsr && mkdir -p /tmp/stdsr/src && cd /tmp/stdsr
printf '#![no_main]\n' > src/lib.rs
printf '[package]\nname="stdsr"\nversion="0.0.0"\nedition="2021"\n[lib]\ncrate-type=["rlib"]\n[profile.release]\npanic="abort"\n' > Cargo.toml
cargo +nightly build --release -Zbuild-std=core,alloc,std,panic_abort --target "$TGT"

# 2. Assemble a sysroot: copy host sysroot structure for the target, then overlay our built std rlibs.
DEPS=/tmp/stdsr/target/$TGT/release/deps
mkdir -p "$SYSROOT/lib/rustlib/$TGT/lib"
# seed with the host's bin/ (rustc/lld) via symlink-free copy of the target lib only
cp -f "$DEPS"/*.rlib "$SYSROOT/lib/rustlib/$TGT/lib/" 2>/dev/null || true
cp -f "$DEPS"/*.rmeta "$SYSROOT/lib/rustlib/$TGT/lib/" 2>/dev/null || true
# rustc also needs the target's self-contained objects + the host toolchain bits: symlink host sysroot for everything else
for d in "$RUSTC_SYSROOT/lib/rustlib"/*; do
  b=$(basename "$d");
  [ "$b" = "$TGT" ] && continue
  [ -e "$SYSROOT/lib/rustlib/$b" ] || ln -s "$d" "$SYSROOT/lib/rustlib/$b" 2>/dev/null || true
done
ln -sf "$RUSTC_SYSROOT/lib/rustlib/src" "$SYSROOT/lib/rustlib/src" 2>/dev/null || true
echo "=== sysroot std rlibs ==="; ls "$SYSROOT/lib/rustlib/$TGT/lib/" | grep -E 'libstd|libcore|liballoc' | head

# 3. Validate: plain rustc with --sysroot (NO build-std) compiles a std crate.
cd /tmp && printf 'fn main(){let v=vec![1,2,3];println!("{}",v.len());}\n' > t.rs
echo "=== plain rustc --sysroot test (no build-std) ==="
if rustc --target "$TGT" --sysroot "$SYSROOT" -C panic=abort t.rs -o /tmp/t 2>&1 | grep -q error; then
  rustc --target "$TGT" --sysroot "$SYSROOT" -C panic=abort t.rs -o /tmp/t 2>&1 | tail -15
  echo "SYSROOT TEST: FAIL"
else
  echo "SYSROOT TEST: OK (rustc finds std via prebuilt sysroot)"
fi
