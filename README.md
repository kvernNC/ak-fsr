# AK-FSR

Addon ReShade qui ajoute un **anti-aliasing temporel FSR** (AA natif, sans upscaling) à
**Batman: Arkham Knight**, sous **Proton** (Linux, Steam Deck), sans GPU NVIDIA.

Il reprend la *méthode* du mod « Native DLSS Anti Aliasing for Arkham Knight » (jitter de la projection,
vecteurs de mouvement, insertion avant le post-traitement), réimplémentée de zéro, avec FSR à la place de DLAA.

> Projet personnel, expérimental. Testé avec la version Steam 1.6.2.0 du jeu, sur PC (RX 9070 XT, CachyOS)
> et Steam Deck OLED. Windows n'est pas une cible.

## Ce que fait l'addon

- **Jitter** : détour de `FPerspectiveMatrix` (UE3) dans `BatmanAK.exe`, trouvée par motif d'octets ;
  séquence de Halton appliquée à la seule vue principale (apprise automatiquement).
- **Vecteurs de mouvement** : caméra par reprojection de la profondeur (matrices du CB « View »), objets
  mobiles depuis la vélocité du G-buffer du jeu (motion blur activé requis).
- **FSR 3.1** (FidelityFX SDK v1.1.4, backend **Vulkan**) appelé via l'interop de DXVK, juste avant la
  préparation DoF/bloom (CS `0xB2A276FD`), sur la couleur HDR de la scène.
- **Shaders sensibles au jitter** : patch du bytecode DXBC à la création (éclairage différé) pour que la
  reconstruction de position reste cohérente avec la profondeur jitterée.
- Onglet **AK-FSR** dans l'overlay ReShade : mode, options, diagnostics (passes détectées, temps CPU/GPU,
  capture de frame). Réglages sauvegardés dans `ReShade.ini`, section `[AK-FSR]`.

## Configuration requise

- Batman: Arkham Knight (Steam), Proton avec DXVK récent (testé : 3.0.2 sur Deck, 3.1.1 sur PC).
- [ReShade](https://reshade.me) 6.7.3+ **avec support des addons**, installé en `dxgi.dll` dans `Binaries/Win64`.
- Option : [RenoDX](https://github.com/clshortfuse/renodx) `renodx-batmanak` pour le HDR (compatible).
- Motion blur **activé** dans les options du jeu ; AA du jeu désactivé.

## Installation

1. Copier `ak-fsr.addon64` et `amd_fidelityfx_vk.dll` dans `Batman Arkham Knight/Binaries/Win64/`.
2. Dans `ReShade.ini`, utiliser un chemin relatif : `AddonPath=.\` (un chemin absolu diffère entre PC et Deck).
3. Ligne de lancement Steam (exemple, avec HDR) :
   ```
   PROTON_ENABLE_HDR=1 DXVK_HDR=1 ENABLE_HDR_WSI=1 ENABLE_GAMESCOPE_WSI=1 WINEDLLOVERRIDES="dxgi=n,b" %command%
   ```
4. En jeu, ouvrir l'overlay ReShade → onglet **AK-FSR** (mode FSR actif par défaut).
5. Désactiver les effets d'AA de ReShade (SMAA, FXAA) : ils s'appliqueraient après FSR, sur l'interface.

Options expérimentales (branche `wip/m6-m10`, toutes désactivées par défaut, voir [TESTING.md](TESTING.md)) :
touche on/off, hautes lumières hors FSR, insertion au SMAA du jeu, backend FSR 3 DX11 natif
(`ak-fsr-dx11.dll` à copier aussi), vecteurs du jeu, biais de LOD, FSR 4 par le provider du pilote AMD
(DLL AMD à placer dans `Binaries/Win64/ak-fsr/`).

### OptiScaler : à éviter

OptiScaler intercepte FidelityFX et Vulkan dans le processus : avec AK-FSR, plantages constatés (double free
dans le pilote) et blocages au démarrage. Utiliser ReShade seul en `dxgi.dll`.

### FSR 4 (optionnel, PC RDNA 4)

Avec [OptiScaler](https://github.com/optiscaler/OptiScaler) nightly chargé dans le processus, la version
« 4.1.2 * » apparaît dans le menu « Version FSR » et fonctionne. **Instable pour l'instant** : selon le
lancement, le jeu se bloque au démarrage (avant la création de la swapchain) quand OptiScaler et ReShade sont
chargés ensemble, y compris en `wininet.dll`. Piste non encore testée : OptiScaler en `dxgi.dll` qui charge
ReShade lui-même (`ReShade64.dll`, `LoadReshade=true`).

## Compilation (depuis Linux)

Compilation croisée vers Windows x64 avec `clang-cl`, `lld-link` et le SDK/CRT MSVC fournis par
[xwin](https://github.com/Jake-Shadle/xwin) (CRT statique : aucune dépendance aux DLL VC++).

```sh
git clone --recurse-submodules <ce dépôt>        # ou : git submodule update --init --recursive
xwin --accept-license --cache-dir ~/.xwin-cache splat --output ~/.xwin
scripts/fetch_ffx.sh                              # amd_fidelityfx_vk.dll (SDK v1.1.4, empreinte vérifiée)
scripts/fetch_ffx_dx11.sh                         # facultatif : backend DX11 (M8.2) -> ak-fsr-dx11.dll
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
scripts/deploy.sh                                 # copie vers le dossier du jeu (AK_GAME_DIR pour le changer)
```

Seul le sous-module `deps/imgui` de ReShade est nécessaire. Les shaders Vulkan (`shaders/*.comp`) sont
précompilés dans `src/shaders/` ; `scripts/build_shaders.sh` les régénère (glslang requis).

## État et limites

| Jalon | État |
|---|---|
| M0–M3 : addon, cartographie des passes, interop Vulkan, FSR sans jitter | fait (PC + Deck) |
| M4 : jitter | fait (PC) |
| M5 : vélocité des objets | fait (PC) |
| M6 : shaders sensibles au jitter | éclairage différé corrigé ; AO, réflexions, décalques à voir |
| M7 : finitions (touche on/off, pluie, hautes lumières…) | à faire |

- Coût sur Steam Deck : environ 3,4 ms par frame (FSR 3.1 en 800p), au-delà du budget visé de 2 ms.
- Les détails plus fins qu'un pixel peuvent encore sautiller en mouvement (limite de FSR 3.1).
- Le détail de la conception, des passes du jeu et des pièges rencontrés est dans [PLAN.md](PLAN.md).

## Crédits et licences

- Code de l'addon : licence [MIT](LICENSE).
- [ReShade](https://github.com/crosire/reshade) (crosire) — API d'addon, BSD-3-Clause.
- [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (AMD) — FSR, MIT.
- [DXVK](https://github.com/doitsujin/dxvk) — interfaces d'interop Vulkan, zlib.
- [Luma Framework](https://github.com/Filoppi/Luma-Framework) (Filippo Tarpini et contributeurs, MIT) —
  idées du §4.2 de PLAN.md (insertion au SMAA, vecteurs du jeu, biais de LOD, backend DX11) et bibliothèques
  FidelityFX DX11 précompilées (© AMD, MIT) utilisées par `ak-fsr-dx11.dll` (`scripts/fetch_ffx_dx11.sh`).
- [OptiScaler](https://github.com/optiscaler/OptiScaler) (GPL-3.0) et
  [FSR4-Vulkan-Bridge](https://github.com/SemiRealProductions/FSR4-Vulkan-Bridge) — sources des faits
  d'interface du provider FSR 4 du pilote AMD ; aucun code repris.
- [RenoDX](https://github.com/clshortfuse/renodx) — référence pour les hashes de shaders et l'organisation
  d'un addon de remplacement, MIT.
- Méthode inspirée du mod « Native DLSS Anti Aliasing for Arkham Knight » (Zorkanne33) ; aucun code ni
  binaire de ce mod n'est repris ou distribué.
