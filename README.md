# AK-FSR4-Linux-AMD

**AMD FSR 4 anti-aliasing for Batman: Arkham Knight, on Linux / Proton with an AMD GPU (FSR 3.1 on Windows).**

A [ReShade](https://reshade.me) add-on that replaces the game's SMAA with FSR 4 as native-resolution temporal
anti-aliasing (no upscaling): the game renders at your resolution, FSR 4 removes aliasing and shimmering.

> **FSR 4: Linux (Proton) + AMD Radeon only.** The add-on talks to FSR 4 through DXVK's Vulkan interop and AMD's
> FSR 4 driver library. **On Windows** (native D3D11, no DXVK), it uses **FSR 3.1** with FidelityFX's native DX11
> backend instead (new in 0.9.0, tested on an RX 9070 XT; see *Windows* below).
>
> Personal, experimental project. Tested with the Steam version 1.6.2.0 of the game on an RX 9070 XT
> (CachyOS, Mesa RADV) and on a Steam Deck OLED (SteamOS, Lite model).

## Features

- **FSR 4.1.2** temporal AA in place of the game's SMAA, before post-processing (bloom, depth of field, UI stay
  sharp and untouched).
- **FSR 4 model selector**: Auto, **FP8** (RDNA 4), **INT8** (RDNA 3), **INT8-2** (RDNA 2) or **Lite**
  (handheld model). Any model can be forced on any supported AMD GPU, to compare quality and cost.
- **Driver watermark** toggle: AMD's own overlay shows which FSR 4 provider is really running
  (`FSR4`, `FSR4-i8`, `FSR4-i8-2`, `FSR4L-i8`).
- **Jitter-aware game shaders**: about 2,100 of the game's shaders (decals, light volumes, ambient occlusion,
  deferred lighting) rebuild 3D positions from the screen without accounting for the camera jitter TAA needs,
  which makes textures and lights shimmer. They are rewritten in memory when the game creates them. Game files
  are not modified.
- Rain splashes drawn after FSR so they stay crisp; texture LOD bias adjusted for TAA.

## Platform support

| Platform | Status |
|---|---|
| Linux, AMD RDNA 4 (RX 9000), Proton | **tested** (RX 9070 XT, all four models) |
| Linux, AMD RDNA 2 / 3, Proton | should work (INT8 / INT8-2), not tested |
| Steam Deck (SteamOS) | **tested** with the Lite model forced in the tab (`FSR4L-i8`): about 7.8 ms per frame at 800p, built-in benchmark 43 fps average (59 without the add-on) |
| Windows (native D3D11, no DXVK) | **tested**: FSR 3.1 (DX11), new in 0.9.0 (RX 9070 XT, ReShade 6.8.0) |
| Windows with DXVK | not supported (see *Windows: FSR 4 help wanted*) |
| NVIDIA / Intel on Linux | not supported (FSR 4 needs AMD's driver library) |

## Requirements

- Batman: Arkham Knight (Steam, 1.6.2.0), Proton with a recent DXVK.
- [ReShade](https://reshade.me) 6.7.3+ **with add-on support**, installed as `dxgi.dll` in `Binaries/Win64`.
- Steam launch option: `WINEDLLOVERRIDES="dxgi=n,b" %command%`.
- In-game graphics options: **Anti-Aliasing On** (FSR 4 takes the SMAA's place) and **Motion Blur On**.
  Motion blur is required: with it Off, the game stops updating the object velocity buffer the add-on reads, and
  FSR 4 then shows **more aliasing than no AA at all** (the whole picture shimmers).
- Keep `DepthOfField=True` in `BmSystemSettings.ini` (default; not in the in-game menu): with depth of field off,
  the game skips the passes the add-on hooks into and FSR 4 may not run at all.
- `amdvkffx64.dll` from AMD's Windows driver (see below): AMD's FSR 4 library, proprietary, **not included**.
- Not compatible with **OptiScaler** (both hook FidelityFX and Vulkan: crashes). Remove it.
- Optional: [RenoDX](https://github.com/clshortfuse/renodx) for HDR (compatible).

## Installation

1. Download the archive from [Releases](https://github.com/kvernNC/ak-fsr/releases) and copy its
   `Binaries/Win64/` content into the game's `Binaries/Win64/`.
2. Get `amdvkffx64.dll`: download AMD's Windows driver package (*Adrenalin 26.10.1*), open the installer with
   7-Zip (no installation needed), find `amdvkffx64.dll` and put it in `Binaries/Win64/ak-fsr4-linux-amd/`.
3. RDNA 4 (RX 9000) only, for the FP8 model: add `radv_cooperative_matrix2_nv=true` at the start of the
   launch options.
4. In `ReShade.ini` (same folder), set `AddonPath=.\` under `[ADDON]`.
5. Start the game, open the ReShade overlay → **AK-FSR4-Linux-AMD** tab: **Off / FSR 4**, model, watermark.
6. Turn off ReShade's own AA effects (SMAA, FXAA): they would apply on top of FSR and the UI.

Settings are saved in `ReShade.ini`, section `[AK-FSR]`.

## Windows (FSR 3.1 DX11)

On Windows the add-on cannot reach FSR 4 (see below) and uses **FSR 3.1** instead, with FidelityFX's native DX11
backend: native-resolution AA in place of the game's SMAA, with the same jitter, shader fixes, motion vectors, rain
and LOD bias as on Linux. The FSR 4 model selector and the driver watermark are FSR 4 features
and are not available there. The tab shows the backend in use.

1. Install [ReShade](https://reshade.me) 6.7.3+ **with add-on support** for the game, as **`dxgi.dll` in
   `Binaries/Win64`**. **Do not install DXVK** (its `d3d11.dll` / `dxgi.dll` would hide the game's D3D11 from
   ReShade).
2. Copy the archive's `Binaries/Win64/` content into the game's `Binaries/Win64/`: `ak-fsr4-linux-amd.addon64`,
   `ak-fsr-dx11.dll`, `ffx_backend_dx11_x64.dll`, `ffx_fsr3upscaler_x64.dll`. `amdvkffx64.dll` is not needed.
3. The [Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)
   must be installed (usually already there).
4. Same in-game options as on Linux: **Anti-Aliasing On**, **Motion Blur On**.
5. In `ReShade.log`, the add-on writes the chosen backend: `backend : FSR 3.1 (DX11 natif)`.

## Windows: FSR 4 help wanted

FSR 4 on Windows needs a bridge the add-on does not have. On Linux, Proton puts DXVK in the prefix's `system32`, so
ReShade (as `dxgi.dll`) sits **above** DXVK: it sees the game's D3D11 and DXVK's Vulkan interop is available. On
Windows you cannot have ReShade on D3D11 and DXVK under it: ReShade hooks `system32\d3d11.dll` while the game loads
DXVK's `d3d11.dll`, so ReShade falls back to DXVK's Vulkan and the add-on only sees SPIR-V (no game pass is found);
replacing DXVK's `dxgi.dll` with ReShade's crashes.

The missing piece is a native D3D11 ↔ Vulkan bridge: textures shared with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` and
imported with `VK_KHR_external_memory_win32`, plus a shared D3D11 fence imported as a Vulkan timeline semaphore
(`VK_KHR_external_semaphore_win32`) for ordering, then FSR 4 on AMD's Windows Vulkan driver. Contributions welcome.

## How it works

- **Jitter**: `FPerspectiveMatrix` (Unreal Engine 3) is hooked in `BatmanAK.exe` (found by byte pattern) and a
  Halton sub-pixel offset is applied to the main view only.
- **Motion vectors**: camera motion by depth reprojection (matrices from the game's view constant buffer),
  moving objects from the game's G-buffer velocity (hence motion blur On).
- **FSR 4**: dispatched through DXVK's D3D11 → Vulkan interop on the scene's HDR colour, where the game's SMAA
  would run. FSR 4.1.2 comes from AMD's driver provider (`amdvkffx64.dll`), loaded next to AMD's signed
  FidelityFX SDK 2.3 DLLs (MIT, included).
- **Model selection**: the driver picks its model from the GPU family it reads. The add-on presents it a GPU of
  the chosen generation (Vulkan properties, hidden FP8 / cooperative-matrix features, and the GPU family from
  `D3DKMTQueryAdapterInfo`), for the driver library only: the game, DXVK and ReShade see the real GPU.
- **Shader rewriting**: DXBC bytecode of jitter-sensitive game shaders is patched at creation so position
  reconstruction matches the jittered depth.

Design notes, game passes and pitfalls: [PLAN.md](PLAN.md). Test log: [PROGRESS.md](PROGRESS.md). Changes:
[CHANGELOG.md](CHANGELOG.md).

## Building (from Linux)

Cross-compiled for Windows x64 with `clang-cl`, `lld-link` and the MSVC SDK/CRT from
[xwin](https://github.com/Jake-Shadle/xwin) (static CRT: no VC++ runtime needed).

```sh
git clone --recurse-submodules https://github.com/kvernNC/ak-fsr
xwin --accept-license --cache-dir ~/.xwin-cache splat --output ~/.xwin
scripts/fetch_ffx_signed.sh      # AMD's signed FidelityFX SDK 2.3 DLLs (official repo, hashes checked)
scripts/fetch_ffx_dx11.sh        # FidelityFX DX11 headers (via Luma, pinned commit, hashes checked)
scripts/fetch_ffx_dx11_gameplug.sh  # FSR 3.1.2 DX11 backend DLLs (gameplug-labs release, hash checked)
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
scripts/deploy.sh                # copies into the game folder (set AK_GAME_DIR to change it)
scripts/package_release.sh       # release archive, never contains AMD's proprietary DLL
```

Release build by default; add `-DAKFSR_DEV_TOOLS=ON` (in another build folder) for the development tools: test
profiles, remote commands, engine commands, screenshots, frame and texture capture, performance log (see
[TESTING.md](TESTING.md) and [DECK.md](DECK.md)).

## Known limitations

- No upscaling: keep the game's `ScreenPercentage` at 100 (the add-on resets it to 100 if it finds it lower, e.g.
  after version 0.8.0's experimental upscaling).
- Falling rain streaks go through FSR and can look slightly softer.
- Sub-pixel details can still flicker a little in motion.

## Credits and licences

- Add-on code: [MIT](LICENSE).
- [ReShade](https://github.com/crosire/reshade) (crosire): add-on API, BSD-3-Clause.
- [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (AMD): FSR, MIT.
- [gameplug-labs/FidelityFX-SDK](https://github.com/gameplug-labs/FidelityFX-SDK) (MIT): FSR 3.1.2 DX11 backend
  DLLs used on Windows (release `fsr3-v3.1.2`, hash-checked by `scripts/fetch_ffx_dx11_gameplug.sh`).
- [DXVK](https://github.com/doitsujin/dxvk): Vulkan interop interfaces, zlib.
- [Luma Framework](https://github.com/Filoppi/Luma-Framework) (Filippo Tarpini and contributors, MIT): ideas
  (SMAA replacement, LOD bias) and the FidelityFX DX11 headers used to build `ak-fsr-dx11.dll`
  (`scripts/fetch_ffx_dx11.sh`).
- [OptiScaler](https://github.com/optiscaler/OptiScaler) and
  [FSR4-Vulkan-Bridge](https://github.com/SemiRealProductions/FSR4-Vulkan-Bridge): interface facts about AMD's
  FSR 4 driver provider; no code taken.
- [RenoDX](https://github.com/clshortfuse/renodx): reference for shader hashes and add-on layout, MIT.
- Method inspired by the "Native DLSS Anti Aliasing for Arkham Knight" mod (Zorkanne33); no code or files from
  it are used or distributed.
- Single-player game, no anti-cheat; still a mod that hooks the renderer: use at your own risk.
