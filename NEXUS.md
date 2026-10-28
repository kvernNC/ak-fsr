# AK-FSR — FSR anti-aliasing for Batman: Arkham Knight (Linux / Steam Deck)

*Texte de la page Nexus et de l'archive. À relire avant publication.*

Native-resolution temporal anti-aliasing with AMD FSR (3.1 or 4) for Batman: Arkham Knight, as a ReShade
add-on. No upscaling: the game renders at your resolution and FSR replaces the game's SMAA.

## Platform support — read this first

| Platform | FSR 3.1 | FSR 4 | Status |
|---|---|---|---|
| **Steam Deck** (SteamOS, Proton) | yes | yes, **INT8 model** (not Lite, see below) | tested |
| **Linux desktop, AMD RDNA 4** (RX 9000, Proton) | yes | yes, **FP8 model** | tested (RX 9070 XT) |
| Linux desktop, AMD RDNA 2 / 3 (Proton) | yes | probably INT8 | not tested |
| Other Linux handhelds (Z1, Z2…) | yes | probably INT8 (Lite not available, see below) | not tested |
| **Windows** | FSR 3.1 DX11 backend should work | **no** | **not supported, not tested** |
| NVIDIA / Intel GPUs | FSR 3.1 should work | no | not tested |

- **Windows is not supported.** The FSR 4 path relies on DXVK's Vulkan interop, which Microsoft's D3D11 does not
  have. FSR 3.1 through the DX11 backend may work but nobody has tested it.
- **FSR 4 Lite (the handheld model) is not available**, on any device. Under Proton, AMD's Vulkan library does not
  receive the GPU family it needs to pick Lite, so it uses its INT8 model. INT8 looks great but costs more.
- **Not compatible with OptiScaler.** Both hook FidelityFX and Vulkan: crashes and freezes. Remove OptiScaler.

## Performance (measured)

Steam Deck OLED, 1280×800, standing on the starting rooftop, game AA on:

| Mode | FPS |
|---|---|
| Game SMAA only (AK-FSR off) | 65 |
| FSR 3.1, DX11 backend (performance option, see below) | 57 |
| **FSR 3.1, Vulkan backend** (default) | about 48 (estimated from its measured 5.6 ms cost) |
| FSR 4 INT8, replacing SMAA | 48 |

- On the Deck, **FSR 4 costs about the same as FSR 3.1 (Vulkan) and looks better**: if you add AMD's driver file
  (see below), pick FSR 4. A 40 Hz / 40 fps limit in the Deck's performance menu keeps it smooth.
- The **DX11 backend** (tab option) is cheaper but leaves ghosting trails on moving vegetation. Use it only if you
  need the extra frames.

## Requirements

- Batman: Arkham Knight, Steam version 1.6.2.0.
- [ReShade](https://reshade.me) 6.7.3 or newer **with add-on support**, installed as `dxgi.dll` in `Binaries/Win64`.
- Proton (Steam Deck: default Proton is fine). Launch option: `WINEDLLOVERRIDES="dxgi=n,b" %command%`.
- In the game's graphics options: **Anti-Aliasing On** (FSR takes the place of the game's SMAA) and **Motion Blur On**.
- Optional: [RenoDX](https://github.com/clshortfuse/renodx) for HDR (compatible).

## Installation

1. Copy the archive's `Binaries/Win64/` content into the game's `Binaries/Win64/`.
2. In `ReShade.ini` (same folder), set `AddonPath=.\` under `[ADDON]`.
3. Start the game, open the ReShade overlay (Home) → **AK-FSR** tab: **Off / FSR 3.1 / FSR 4**.
4. Turn off ReShade's own AA effects (SMAA, FXAA): they would apply on top of FSR and the UI.

### FSR 4 (optional, extra files you must get yourself)

FSR 4 uses AMD's driver library, which **cannot be redistributed** and is not in this archive:

1. Extract `amdvkffx64.dll` from AMD's Windows driver package *Adrenalin 26.10.1* (open the installer with
   7-Zip; no installation needed) and put it in `Binaries/Win64/ak-fsr/`.
2. RX 9000 on Linux only: add `radv_cooperative_matrix2_nv=true` at the start of the launch options (FP8 model).
3. Restart the game and pick **FSR 4** in the AK-FSR tab. If anything is missing, the tab says so and FSR 3.1 is used.

The two AMD SDK files in `ak-fsr/` (`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`, MIT licence,
from AMD's official FidelityFX SDK 2.3) are included.

## How it handles the game's shaders

Temporal AA needs a sub-pixel camera offset (jitter) every frame. Batman: Arkham Knight was never made for it:
about 2,100 of its shaders (decals, light volumes, ambient occlusion, lighting) rebuild 3D positions from the
screen without accounting for that offset, which makes textures and patches of light shimmer on many surfaces.
AK-FSR rewrites those shaders in memory when the game creates them so they take the jitter into account. Game
files are not modified.

## Known limitations

- Rain splashes on the ground are drawn after FSR so they are kept intact (option "Rain after FSR", on by
  default). The falling rain streaks still go through FSR and can look slightly softer.
- The game's `ScreenPercentage` setting breaks the image (game bug): AK-FSR does not upscale.
- Single-player game, no anti-cheat; still, it is a mod that hooks the renderer: use at your own risk.

## Archive content

| File | What | Licence |
|---|---|---|
| `ak-fsr.addon64` | the add-on | MIT |
| `amd_fidelityfx_vk.dll` | AMD FidelityFX SDK 1.1.4 (FSR 3.1, Vulkan) | MIT (AMD) |
| `ak-fsr-dx11.dll` | FSR 3.1 DX11 backend (AMD FidelityFX libraries, built by Luma Framework) | MIT |
| `ak-fsr/amd_fidelityfx_*_vk.dll` | AMD FidelityFX SDK 2.3 signed DLLs, renamed | MIT (AMD) |

Not included: `amdvkffx64.dll` (AMD proprietary driver file).

## Credits

ReShade (crosire), RenoDX (clshortfuse), Luma Framework (Filippo Tarpini and contributors: ideas and DX11
FidelityFX build), AMD FidelityFX SDK, DXVK. Method inspired by the "Native DLSS Anti Aliasing for Arkham Knight"
mod (Zorkanne33); no code or files from it. Its debug log helped identify which game shaders need
the jitter correction; AK-FSR's rewriting rules and code are its own. Source: github.com/kvernNC/ak-fsr.
