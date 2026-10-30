# AK-FSR4-Linux-AMD — FSR 4 anti-aliasing for Batman: Arkham Knight (Linux / Proton, AMD only)

Native-resolution temporal anti-aliasing with **AMD FSR 4** for Batman: Arkham Knight, as a ReShade add-on.
No upscaling: the game renders at your resolution and FSR 4 replaces the game's SMAA.

## Read this first

- **Linux (Proton) + AMD Radeon GPU only.** Not Windows, not NVIDIA / Intel.
- **Not compatible with OptiScaler** (crashes). Remove it.
- Tested on an RX 9070 XT (all four FSR 4 models). RDNA 2 / 3 desktop GPUs should work but are not tested;
  the Steam Deck with the Lite model is not tested yet.

## Requirements

- Batman: Arkham Knight, Steam version 1.6.2.0, Proton.
- [ReShade](https://reshade.me) 6.7.3 or newer **with add-on support**, installed as `dxgi.dll` in `Binaries/Win64`.
- Launch option: `WINEDLLOVERRIDES="dxgi=n,b" %command%`.
- In the game's graphics options: **Anti-Aliasing On** (FSR 4 takes the place of the game's SMAA) and
  **Motion Blur On**.
- `amdvkffx64.dll`, AMD's FSR 4 driver library: proprietary, **not in this archive** (see below).
- Optional: [RenoDX](https://github.com/clshortfuse/renodx) for HDR (compatible).

## Installation

1. Copy the archive's `Binaries/Win64/` content into the game's `Binaries/Win64/`.
2. Download AMD's Windows driver package (*Adrenalin 26.10.1*), open the installer with 7-Zip (no installation
   needed), extract `amdvkffx64.dll` and put it in `Binaries/Win64/ak-fsr4-linux-amd/`.
3. RX 9000 (RDNA 4) only, for the FP8 model: add `radv_cooperative_matrix2_nv=true` at the start of the launch
   options.
4. In `ReShade.ini` (same folder), set `AddonPath=.\` under `[ADDON]`.
5. Start the game, open the ReShade overlay → **AK-FSR4-Linux-AMD** tab.
6. Turn off ReShade's own AA effects (SMAA, FXAA): they would apply on top of FSR and the UI.

## The tab

- **Off / FSR 4**.
- **FSR 4 model**: Auto (driver's choice for your GPU), FP8 (RDNA 4), INT8 (RDNA 3), INT8-2 (RDNA 2),
  INT8 Lite (handheld). Any model can be forced to compare quality and cost.
- **Driver watermark**: AMD's overlay shows the FSR 4 provider really running (`FSR4`, `FSR4-i8`, `FSR4-i8-2`,
  `FSR4L-i8`). Leave it off to play.

## How it handles the game's shaders

Temporal AA needs a sub-pixel camera offset (jitter) every frame. Batman: Arkham Knight was never made for it:
about 2,100 of its shaders (decals, light volumes, ambient occlusion, lighting) rebuild 3D positions from the
screen without accounting for that offset, which makes textures and patches of light shimmer. The add-on
rewrites those shaders in memory when the game creates them. Game files are not modified.

## Known limitations

- The game's `ScreenPercentage` setting breaks the image (game bug): this add-on does not upscale.
- Falling rain streaks go through FSR and can look slightly softer (splashes are drawn after FSR).
- Single-player game, no anti-cheat; still a mod that hooks the renderer: use at your own risk.

## Archive content

| File | What | Licence |
|---|---|---|
| `ak-fsr4-linux-amd.addon64` | the add-on | MIT |
| `ak-fsr4-linux-amd/amd_fidelityfx_*_vk.dll` | AMD FidelityFX SDK 2.3 signed DLLs, renamed | MIT (AMD) |

Not included: `amdvkffx64.dll` (AMD proprietary driver file).

## Credits

ReShade (crosire), AMD FidelityFX SDK, DXVK, Luma Framework (Filippo Tarpini and contributors: ideas), RenoDX
(clshortfuse). Method inspired by the "Native DLSS Anti Aliasing for Arkham Knight" mod (Zorkanne33); no code or
files from it. Source code (MIT): https://github.com/kvernNC/ak-fsr
