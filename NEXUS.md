# AK-FSR4-Linux-AMD — FSR 4 anti-aliasing for Batman: Arkham Knight (Linux / Proton + AMD; FSR 3.1 on Windows)

Native-resolution temporal anti-aliasing with **AMD FSR 4** for Batman: Arkham Knight, as a ReShade add-on.
The game renders at your resolution and FSR 4 replaces the game's SMAA (no upscaling). New in 0.9.0: **FSR 3.1 on
Windows** (native DX11 backend).

## Read this first

- **FSR 4: Linux (Proton) + AMD Radeon GPU only.** Not NVIDIA / Intel.
- **Windows** (native D3D11, **without DXVK**): **FSR 3.1** with FidelityFX's DX11 backend instead of FSR 4; no
  model selector or driver watermark. New in 0.9.0, tested on an RX 9070 XT: see *Windows* below.
- **Not compatible with OptiScaler** (crashes). Remove it.
- Tested on an RX 9070 XT (all four FSR 4 models) and on a Steam Deck OLED with the Lite model forced in the tab
  (about 7.8 ms per frame at 800p: built-in benchmark 43 fps average, 59 without the add-on). RDNA 2 / 3 desktop
  GPUs should work but are not tested.

## Requirements

- Batman: Arkham Knight, Steam version 1.6.2.0, Proton.
- [ReShade](https://reshade.me) 6.7.3 or newer **with add-on support**, installed as `dxgi.dll` in `Binaries/Win64`.
- Launch option: `WINEDLLOVERRIDES="dxgi=n,b" %command%`.
- In the game's graphics options: **Anti-Aliasing On** (FSR 4 takes the place of the game's SMAA) and
  **Motion Blur On**. Motion blur is required: with it Off, the game stops updating the object velocity buffer
  the add-on reads and FSR 4 shows **more aliasing than no AA at all**.
- Keep `DepthOfField=True` in `BmSystemSettings.ini` (default; not in the in-game menu): with depth of field off,
  FSR 4 may not run at all.
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

## Windows (FSR 3.1 DX11)

1. Install ReShade 6.7.3+ **with add-on support** as **`dxgi.dll` in `Binaries/Win64`**. **Do not install DXVK**.
2. Copy the archive's `Binaries/Win64/` content into the game's `Binaries/Win64/`. `amdvkffx64.dll` is not needed.
3. The Microsoft Visual C++ 2015–2022 Redistributable (x64) must be installed (usually already there).
4. Same in-game options: **Anti-Aliasing On**, **Motion Blur On**.

The tab shows the backend in use (`FSR 3.1 (DX11 natif)`).

**FSR 4 on Windows: help wanted.** On Windows, ReShade cannot sit on the game's D3D11 with DXVK under it (on Linux,
Proton makes that possible), so the add-on would need a native D3D11 ↔ Vulkan bridge (NT-handle shared textures,
`VK_KHR_external_memory_win32`, shared fence as a timeline semaphore). Details in the source repository's README.

## The tab

- **Off / FSR** (FSR 4 on Linux, FSR 3.1 on Windows; the backend in use is shown).
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

- No upscaling: keep the game's `ScreenPercentage` at 100 (the add-on resets it to 100 if it finds it lower).
- Falling rain streaks go through FSR and can look slightly softer (splashes are drawn after FSR).
- Single-player game, no anti-cheat; still a mod that hooks the renderer: use at your own risk.

## Archive content

| File | What | Licence |
|---|---|---|
| `ak-fsr4-linux-amd.addon64` | the add-on | MIT |
| `ak-fsr4-linux-amd/amd_fidelityfx_*_vk.dll` | AMD FidelityFX SDK 2.3 signed DLLs, renamed (FSR 4, Linux) | MIT (AMD) |
| `ak-fsr-dx11.dll` | bridge to FidelityFX's DX11 backend (FSR 3.1, Windows) | MIT |
| `ffx_backend_dx11_x64.dll`, `ffx_fsr3upscaler_x64.dll` | FidelityFX FSR 3.1.2 DX11 backend, gameplug-labs build | MIT (AMD, gameplug-labs) |

Not included: `amdvkffx64.dll` (AMD proprietary driver file).

## Credits

ReShade (crosire), AMD FidelityFX SDK, gameplug-labs (FSR 3.1.2 DX11 backend build), DXVK, Luma Framework (Filippo
Tarpini and contributors: ideas, FidelityFX DX11 headers), RenoDX (clshortfuse). Method inspired by the "Native DLSS Anti Aliasing for Arkham Knight" mod (Zorkanne33); no code or
files from it. Source code (MIT): https://github.com/kvernNC/ak-fsr
