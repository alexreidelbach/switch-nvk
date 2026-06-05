# switch-nvk

> **🇧🇷 Português** — Driver **NVK** (o Vulkan open-source da Mesa para GPUs NVIDIA) portado
> para o **Tegra X1 / GM20B (Maxwell)** do Nintendo Switch, rodando como homebrew sobre
> **libnx / Horizon** — sem blob da NVIDIA, sem driver proprietário. Inclui nosso próprio
> **WSI** (integração com o window-system), apresentando direto pelo `nwindow`/`vi` do libnx.
>
> **🇬🇧 English** — The Mesa **NVK** driver (Mesa's open-source Vulkan driver for NVIDIA GPUs)
> ported to the Nintendo Switch's **Tegra X1 / GM20B (Maxwell)**, running as homebrew on
> **libnx / Horizon** — no NVIDIA blob, no proprietary driver. Ships our own **WSI**
> (window-system integration) that presents directly through libnx `nwindow`/`vi`.

## 🌿 Branches

🇧🇷 **As branches deste repo são úteis — principalmente as do WSI.** Cada uma é um ponto de
referência limpo para quem quer levar Vulkan ao Switch (ou estudar partes isoladas do port):

🇬🇧 **The branches in this repo are useful — especially the WSI ones.** Each is a clean
reference point for anyone bringing Vulkan to the Switch (or studying isolated parts of the port):

| Branch | 🇧🇷 O que é | 🇬🇧 What it is |
|--------|-----------|----------------|
| **`master`** | Driver completo + **zero-copy WSI**. O estado mais avançado. | Full driver + **zero-copy WSI**. The most complete state. |
| **`switch-port/nvk-wsi`** | **Bring-up do WSI**: nosso swapchain `VK_NN_vi_surface` sobre o `nwindow` do libnx (acquire → render → `vkQueuePresentKHR` → compositor VI). Referência focada de como implementar *present*/WSI Vulkan no Switch. | **WSI bring-up**: our `VK_NN_vi_surface` swapchain over libnx `nwindow` (acquire → render → `vkQueuePresentKHR` → VI compositor). A focused reference for implementing Vulkan *present*/WSI on Switch. |
| **`switch-port/wsi-zero-copy`** | **WSI zero-copy**: scanout block-linear (`kind=0xfe`) direto no buffer do `nwindow`, eliminando o `memcpy` por frame (present ~9,4 ms → µs). | **Zero-copy WSI**: block-linear scanout (`kind=0xfe`) straight into the `nwindow` buffer, removing the per-frame `memcpy` (present ~9.4 ms → µs). |

🇧🇷 Se você só quer entender/reaproveitar o **WSI**, comece pela `switch-port/nvk-wsi`
(implementação mínima e isolada) e depois veja a `switch-port/wsi-zero-copy` para a versão
otimizada que está na `master`.

🇬🇧 If you only want to understand/reuse the **WSI**, start from `switch-port/nvk-wsi`
(minimal, isolated implementation), then look at `switch-port/wsi-zero-copy` for the optimized
version that lives in `master`.

## 📜 Licença / License

🇧🇷 Código **AGPL-3.0** (veja [`LICENSE`](LICENSE)). Além disso, vale o
[`FORK_POLICY.md`](FORK_POLICY.md): **qualquer fork deste projeto precisa permanecer 100%
open source.**

🇬🇧 Code is **AGPL-3.0** (see [`LICENSE`](LICENSE)). On top of that, the
[`FORK_POLICY.md`](FORK_POLICY.md) applies: **any fork of this project must stay 100% open
source.**
