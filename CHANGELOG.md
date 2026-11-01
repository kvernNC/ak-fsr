# Changelog

## 0.9.0 — 2026-11-01

### Added
- **Windows support with FSR 3.1** (native D3D11, without DXVK): FidelityFX's native DX11 backend (FSR 3.1.2,
  gameplug-labs build) as native-resolution AA in place of the game's SMAA. Jitter, shader fixes, motion vectors,
  rain after FSR and LOD bias work the same as on Linux. Ships as `ak-fsr-dx11.dll` plus
  `ffx_backend_dx11_x64.dll` and `ffx_fsr3upscaler_x64.dll` next to the add-on; needs the Visual C++ 2015–2022
  Redistributable (x64).
- Automatic backend choice on the first frame, logged with its reason and shown in the tab: FSR 4 Vulkan when the
  D3D11 device exposes DXVK's interop (Linux / Proton), otherwise FSR 3.1 DX11.

### Changed
- The FSR 4 model selector, driver watermark and render resolution (upscaling) are shown for the FSR 4 backend only.
- README / Nexus page: Windows installation (ReShade as `dxgi.dll`, no DXVK) and a *FSR 4 on Windows: help wanted*
  section (why DXVK + ReShade cannot be combined on Windows, and the native D3D11 ↔ Vulkan bridge it would need).

### Notes
- Linux / Proton is unchanged (FSR 4, models, watermark, Steam Deck).
- Windows tested on an RX 9070 XT (ReShade 6.8.0, no DXVK): FSR 3.1 DX11 backend chosen and running.

## 0.8.0 — 2026-11-01

### Added
- **Render resolution** in the tab: 100 % (native AA, default), 77 % (Ultra Quality), 67 % (Quality),
  58 % (Balanced), 50 % (Performance). Below 100 %, the game renders fewer pixels and **FSR 4 upscales** the image
  to the screen resolution, before the UI. Applied immediately through the engine's own `scale set ScreenPercentage`
  command; the game saves it in `BmSystemSettings.ini`.
- Fix for the game's broken `ScreenPercentage` (seams in fog and lighting, offset or garbled image, depth of field):
  the post-processing view origin is corrected whenever the render resolution is reduced.
- Safeguards: no native AA on a reduced render resolution; if the game's final upscale target does not match
  (size / format, e.g. an HDR back buffer), upscaling turns itself off for the session and the game upscales alone.

### Notes
- FSR 4's cost depends on the output resolution: the gain is what the game saves by rendering fewer pixels. On a
  Steam Deck the measured gain is nil (the game is not pixel-limited there); GPU-limited setups (1080p handhelds,
  desktop GPUs at high resolution) should benefit more, not measured yet.
- In upscaling mode, moving objects use camera motion only (slight trails on fast characters).
- Steam Deck tested with the Lite model forced in the tab: about 7.8 ms per frame at 800p (built-in benchmark:
  43 fps average, 59 without the add-on). The add-on's own overhead around the model is about 1 ms.
- The add-on now imports `USER32.dll` (engine commands run on the game window's thread).

## 0.7.0 (AK-FSR4-Linux-AMD) — 2026-10-30

First release under the new name, **FSR 4 only**.

### Added
- FSR 4.1.2 through AMD's driver provider (`amdvkffx64.dll`, not included), without OptiScaler.
- **FSR 4 model selector**: Auto, FP8 (RDNA 4), INT8 (RDNA 3), INT8-2 (RDNA 2), INT8 Lite (handheld).
- **Driver watermark** toggle (AMD's overlay shows the provider really running).
- About 2,100 game shaders made jitter-aware (decals, light volumes, ambient occlusion, lighting): no more
  shimmering textures and light patches.
- Rain splashes drawn after FSR.

### Changed
- Renamed to `ak-fsr4-linux-amd` (add-on, DLL folder `ak-fsr4-linux-amd/`, archive).
- Linux / Proton with an AMD Radeon GPU only.

### Removed
- FSR 3.1 (Vulkan and DX11 backends).

## 0.7.0 (AK-FSR) — 2026-10-27

- FSR 3.1 temporal AA in place of the game's SMAA (Vulkan through DXVK, or native DX11 backend).
- On/off key, texture LOD bias for TAA, highlights kept out of FSR, game exposure option.

## 0.6.3 — 2026-10-26

- First version: FSR temporal AA at native resolution, camera jitter, camera and object motion vectors.
