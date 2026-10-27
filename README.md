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

### Onglet AK-FSR

Version publiée (branche `main`) : un seul choix, **Off / FSR 3.1 / FSR 4**, et le modèle FSR 4 utilisé (choisi
par le pilote selon le GPU). Le reste suit les réglages validés en jeu : SMAA du jeu remplacé par FSR quand l'AA
du jeu est actif (sinon insertion habituelle), biais de LOD -1, FSR 4 proposé dès que les DLL AMD sont en place.
Les clés de `ReShade.ini` ci-dessous restent lues pour qui veut s'en écarter.

### Options (clés de `[AK-FSR]`, toutes visibles dans l'onglet de la version de développement)

Détails et procédure de test : [TESTING.md](TESTING.md).

| Option | Clé | État (PC, 27/10/2026) |
|---|---|---|
| Touche on/off | `ToggleKey` | validée |
| Hautes lumières hors FSR | `Highlights` | validée (fonctionnement) |
| FSR à la place du SMAA du jeu (AA du jeu sur « On ») | `SmaaInsertion=1` | validée, coût du SMAA récupéré |
| Backend FSR 3 DX11 natif (`ak-fsr-dx11.dll` à copier) | `Backend=1` | validée, moins coûteux que Vulkan |
| Vecteurs de mouvement du jeu | `GameMotionVectors=1` | fonctionne ; reprojection conservée par défaut |
| Biais de LOD des textures | `LodBias` | **-1.0 recommandé** avec FSR (netteté +3,5 %, sans scintillement ajouté) ; pris en compte au lancement |
| Exposition du jeu | `GameExposure=1` | fonctionne ; intérêt qualité à juger |
| FSR 4 par le provider du pilote AMD | `Fsr4Driver=1` + menu « Version FSR » | validée (FP8, RX 9070 XT) |

Réglages conseillés sur PC (défauts de la version publiée) : `SmaaInsertion=1` (AA du jeu sur « On »),
`LodBias=-1.0`, et FSR 4 si le GPU le permet (voir ci-dessous). Le Deck n'a pas encore été testé avec ces options.

Version de développement seulement : pilotage sans clavier par `ak-fsr-command.txt` à côté de l'addon (`report`,
`profile <n>`, `toggle`, `screenshot <nom> [n]`, `look`, `capture`…), rapport, profils de test, explorateur de
shaders, compteurs ; voir TESTING.md.

### OptiScaler : à éviter

OptiScaler intercepte FidelityFX et Vulkan dans le processus : avec AK-FSR, plantages constatés (double free
dans le pilote) et blocages au démarrage. Utiliser ReShade seul en `dxgi.dll`.

### FSR 4 (optionnel, GPU AMD récents)

FSR 4.1.2 est obtenu directement du provider du pilote AMD, sans OptiScaler :

1. Placer dans `Binaries/Win64/ak-fsr/` :
   - `amdvkffx64.dll`, tirée du pilote Windows AMD Adrenalin (archive extraite avec 7-Zip, sans installer le
     pilote) — propriétaire, jamais dans ce dépôt ;
   - les DLL signées du FidelityFX SDK 2.3 renommées en `_vk` : `scripts/fetch_ffx_signed.sh` puis
     `scripts/deploy.sh` s'en chargent (dépôt officiel AMD, MIT, empreintes vérifiées).
2. RDNA 4 (modèle FP8) : ajouter `radv_cooperative_matrix2_nv=true` au début de la ligne de lancement.
3. `Fsr4Driver=1`, relancer le jeu, puis choisir « 4.1.2 (provider pilote) » dans le menu « Version FSR ».
   En cas d'échec, l'addon revient à FSR 3.1 avec un message dans l'onglet.

Mesures sur RX 9070 XT (800×600) : FSR 4 est moins crénelé et scintille moins que FSR 3.1, pour un coût GPU de
0,26 ms contre 0,17 ms.

## Branches

- `main` : version publiée, compilée sans les outils de développement (`AKFSR_DEV_TOOLS=OFF` par défaut).
- `dev` : développement et tests, mêmes sources compilées avec les outils (`AKFSR_DEV_TOOLS=ON` par défaut).

Les deux branches ne diffèrent que par cette valeur par défaut : le travail se fait sur `dev`, puis `dev` est
fusionnée dans `main`. Chacune peut produire l'autre variante avec `-DAKFSR_DEV_TOOLS=ON|OFF`.

## Compilation (depuis Linux)

Compilation croisée vers Windows x64 avec `clang-cl`, `lld-link` et le SDK/CRT MSVC fournis par
[xwin](https://github.com/Jake-Shadle/xwin) (CRT statique : aucune dépendance aux DLL VC++).

```sh
git clone --recurse-submodules <ce dépôt>        # ou : git submodule update --init --recursive
xwin --accept-license --cache-dir ~/.xwin-cache splat --output ~/.xwin
scripts/fetch_ffx.sh                              # amd_fidelityfx_vk.dll (SDK v1.1.4, empreinte vérifiée)
scripts/fetch_ffx_dx11.sh                         # facultatif : backend DX11 (M8.2) -> ak-fsr-dx11.dll
scripts/fetch_ffx_signed.sh                       # facultatif : DLL signées du SDK 2.3 pour FSR 4 (M10)
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
| M7 : finitions | touche on/off et hautes lumières faites ; pluie après l'AA : passes non identifiées |
| M8 : optimisations (insertion au SMAA, backend DX11, vecteurs du jeu, biais de LOD, exposition) | faites (PC), Deck à tester |
| M10 : FSR 4 par le provider du pilote AMD, sans OptiScaler | fait (PC, FP8), Deck à tester |

- Coût sur Steam Deck mesuré avant M8 : environ 3,4 ms par frame (FSR 3.1 en 800p, chemin Vulkan), au-delà du
  budget visé de 2 ms ; M8.1 (SMAA récupéré) et M8.2 (DX11) doivent le réduire.
- Les détails plus fins qu'un pixel peuvent encore sautiller en mouvement (moins avec FSR 4).
- Journal des tests et blocages : [PROGRESS.md](PROGRESS.md) ; procédure de test sur Steam Deck : [DECK.md](DECK.md) ; page de publication : [NEXUS.md](NEXUS.md)
  (archive : `scripts/package_release.sh`, sans aucune DLL propriétaire d'AMD).
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
