# Changelog

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
