#!/usr/bin/env bash
# rustc wrapper for meson: targets the Switch (custom JSON) using the PREBUILT std sysroot,
# so meson's direct rustc calls find std without cargo's -Zbuild-std.
# Also STRIPS meson's injected `-C linker=<gcc>` so rustc uses the target-default rust-lld +
# the NRO link-script (no newlib crt0 → avoids __bss_start__ / pthread exe-link errors). NAK
# builds as a staticlib/rlib anyway; the sanity exe links fine under rust-lld.
export RUSTC_BOOTSTRAP=1
export RUST_TARGET_PATH=/work

final=()
all=("$@")
n=${#all[@]}
i=0
while [ $i -lt $n ]; do
  a="${all[$i]}"
  if [ "$a" = "-C" ] && [ $((i+1)) -lt $n ] && [[ "${all[$((i+1))]}" == linker=* ]]; then
    i=$((i+2)); continue            # drop "-C" "linker=<gcc>"
  fi
  if [[ "$a" == -Clinker=* ]]; then i=$((i+1)); continue; fi   # drop joined "-Clinker=<gcc>"
  final+=("$a"); i=$((i+1))
done

exec rustc -Zunstable-options \
  --sysroot /work/sysroot \
  --target aarch64-switch-horizon \
  -L /opt/devkitpro/devkitA64/aarch64-none-elf/lib \
  -L /opt/devkitpro/libnx/lib \
  "${final[@]}"
