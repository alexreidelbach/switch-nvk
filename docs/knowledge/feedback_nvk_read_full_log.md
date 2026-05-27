---
name: feedback-nvk-read-full-log
description: "STANDING RULE for the switch-nvk project: always read the ENTIRE HW log every run, never just a grep slice"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 827a2abd-cef2-44f7-aecc-4979d9d434cf
---

**STANDING RULE (user, 2026-05-26), for the `D:\switch-nvk` NVK-on-Switch project — NO exceptions:** on EVERY hardware run, read the **complete** `sdmc:/nvk_smoke.log` (the full file via Read), never just a `Select-String`/grep slice. Also **archive each run's log** to `D:\switch-nvk\winsys\logs\nvk_log_vNN.txt` so all runs can be re-read later (earlier runs were lost by overwriting the same `nvk_hw.log`).

**Why:** grepping only matched lines HID the decisive clue. The v17 log, read in full, revealed that the big 1859-dword EXEC actually SUCCEEDED (`ioctl cmd=0x12 -> 0`) and a SEPARATE later EXEC failed — i.e. the NO_PREFETCH fix had worked for the main submit — which the grep had masked (it looked like "still 0xd5c, no change"). Reading the whole log changes the diagnosis.

**How to apply:** after `curl ftp://IP:5000/sdmc:/nvk_smoke.log`, copy it to `logs\nvk_log_vNN.txt` AND `Read` the entire file (not grep). Correlate the FULL ordered sequence of `EXEC enter chan=.. push_count=..`, `ioctl cmd=0x12 -> ..`, and `kickoff failed` lines — the ORDER and per-EXEC return codes carry the signal. See [[vulkan-nvk-switch-path]] and [[dusklight-debugging-heuristics]]. Also: stamp a visible nacp version (`0.NN.0-tag`) so the Sphaira UI confirms the new binary, and upload to `sdmc:/switch/nvk_smoke.nro` (the user's favorited launch path), NOT sdmc root.
