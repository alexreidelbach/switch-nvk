---
name: dan-nvk-intel-and-goal
description: "Everything Dan told the user about porting NVK/Vulkan to the Switch + the EXACT goal: a standalone Vulkan placeholder, NO game/port integration"
metadata: 
  node_type: memory
  type: project
  originSessionId: 15fe3c02-602e-4f2e-ab84-754a6a065183
---

**Dan's complete intel on the NVK/Vulkan-on-Switch port (told to the user, 2026-05-25) + our exact goal. The user stressed: SAVE ALL OF THIS — it's how we proceed.**

## THE GOAL (narrow — do NOT exceed it)
Reproduce **what Dan did first: a standalone "Vulkan art placeholder" that depends on Vulkan to run** — i.e. a self-contained `.nro`/`.elf` that initializes our NVK, requires a working Vulkan device, and draws something (the triangle/smoke test). **That is the ENTIRE current objective.** Explicit user constraint: **NO in-game integration, NO port integration of any kind** ("nada de implementar em jogo nem em Port nenhum"). M4 (wiring NVK into Aurora/Dusklight/any game) is OUT OF SCOPE. Stop at the placeholder that proves Vulkan runs on the Switch via our own NVK. This is the standalone effort in `D:\switch-nvk\` and must NOT involve Dusklight (see [[scope-dusk-eden-only]]).

## Dan = the source (identities)
`dantiicu` = `Tiicu` = `ticohq`. He built a working NVK-Vulkan for the Switch and a Dawn-on-GL fork.

## Dan's hints (verbatim-ish, decoded)
- **"Understand NVK, understand nwindow, libnx, build a WSI — you are done."** → The driver (NVK) + the WSI (VkSurface/VkSwapchain over libnx `nwindow`/`vi`) are the two pieces. WSI is the glue between the Vulkan driver and the Switch display.
- **He will NOT share his NVK build** → we port NVK to GM20B (Tegra X1 integrated Maxwell) + the Switch `nv` services OURSELVES. (His private image = `ticohq/switch-nvk-vulkan`, NVK at `/opt/nvk-switch`; denied/private.)
- **His method (the ~1.5-month grind):** "hoje tem o 20.13 do mesa driver, portei versão por versão 20.13 → 21.0 ... muita breaking change ... o switch não tem toolchain para rust ... com os std tudo shimmado ... 1.5 meses." = Started from Mesa 20.13, **forward-ported version-by-version (20.13→21.0→…) handling each release's Switch breaking-changes**; the Switch has **no Rust toolchain** so he **configured one by hand with the Rust std all shimmed** (NVK's shader compiler NAK is Rust); ~1.5 months, AI-heavy, "humanly impossible to grasp the whole structure" in that time.

## What's PUBLIC from Dan (reference, don't reinvent)
- **WSI**: `github.com/dantiicu/vulkan-triangle-test-switch` (`TriangleTest.cpp` = VI surface + swapchain) + `vulkan-smoke`/`compute-test-switch`. ← the placeholder template to mirror.
- **Dawn-on-GL**: `github.com/dantiicu/dawn-switch` (already vendored in our tree; see [[dawn-over-gl-already-patched]]).
- The Switch nv winsys exists publicly too: `devkitPro/libdrm_nouveau` (libdrm nouveau over libnx nv) + `devkitPro/mesa` (Mesa 20.1 GL/nouveau on GM20B). Cloned to `D:\switch-nvk\`.

## How this maps to OUR progress (2026-05-25)
We did NOT forward-port from 20.13; we **pinned Mesa 25.0.7** (NVK-era stable, LLVM 15) and cross-compiled it directly — and **it WORKS** (M0 Rust std shimmed + linking; M1 NVK fully cross-COMPILES, `libnvk.a` produced). Details + the dep chain in [[vulkan-nvk-switch-path]]. Remaining to hit the goal: M1 winsys shim (DRM-nouveau ioctls → libnx `nv`, ref `D:\switch-nvk\libdrm_nouveau`), M2 memory/submit, **M3 = the placeholder triangle (THE GOAL)**.
