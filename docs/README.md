# docs/ — knowledge & documentation trail for switch-nvk

The accumulated research, debugging patterns, and project knowledge behind getting **Mesa NVK (Vulkan)
running on the Nintendo Switch GM20B**. These are the notes that led to the **M2 smoke-test PASS** on
real Tegra (NVK executing `vkCmdFillBuffer(0xCAFEBABE)` on the GPU + verified CPU readback).

## Start here
- **[`../BUILD_AND_RUN.md`](../BUILD_AND_RUN.md)** — everything needed to build & run (Docker, deps, the
  full pipeline, the on-device run workflow).
- **[`../RESUME_NVK.md`](../RESUME_NVK.md)** — the live source of truth (status, symbol spec, next steps).
- **[`../PLAN_NVK.md`](../PLAN_NVK.md)** — the original phased plan (M0→M3).

## knowledge/ — the research & debugging trail
- **[`vulkan_nvk_switch_path.md`](knowledge/vulkan_nvk_switch_path.md)** — the full state/history of the
  port: M0 (Rust std) → M1 (cross-build + link) → M2 (winsys + the PASS). The chronological record,
  newest at the top, including the FECS-wall discovery and the fence-cmdlist fix that made it pass.
- **[`nvk_winsys_debugging_patterns.md`](knowledge/nvk_winsys_debugging_patterns.md)** — hard-won
  anti-patterns / heuristics (libnx Result decode; `nvFenceWait` is µs; Timeout=channel-reset→read the
  error notifier; Horizon blocks FECS priv-regs; the fence cmdlist = the real GPU syncpt-increment + L2
  flush; Eden fakes the submit; etc.). **The most reusable distillation.**
- **[`dan_nvk_intel_and_goal.md`](knowledge/dan_nvk_intel_and_goal.md)** — the narrow goal (a standalone
  Vulkan placeholder) + every hint from Dan/Tiicu's prior Switch-NVK work.
- **[`feedback_nvk_read_full_log.md`](knowledge/feedback_nvk_read_full_log.md)** — the standing workflow
  rule: read the ENTIRE device log every run (grep-slicing hid real progress).
- **[`SKILL-switch-port.md`](knowledge/SKILL-switch-port.md)** — the broader Switch-port skill/dossier
  (covers both this NVK effort and the related Dusklight GLES work).

> These docs use `[[wiki-link]]` cross-references (from the authoring tool) — they point to each other by
> the `name:` slug in each file's frontmatter.
