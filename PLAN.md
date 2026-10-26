# AK-FSR — addon ReShade : AA temporel FSR pour Batman: Arkham Knight (Proton / Steam Deck)

Ce fichier est le cahier des charges du projet. Il est fait pour être placé à la racine du dépôt
(ou copié dans `CLAUDE.md`) et lu par l'agent de code avant de commencer.

Dernière mise à jour : 27/10/2026 (version de l'addon : 0.6.3, jalon M5).

## 1. Objectif

Écrire un **addon ReShade** (`ak-fsr.addon64`) qui ajoute un anti-aliasing temporel **FSR**
à Batman: Arkham Knight, en résolution native (mode AA), comme le fait le mod
« Native DLSS Anti Aliasing for Arkham Knight » (Nexus, Zorkanne33) avec DLAA,
mais sans dépendre d'un GPU NVIDIA.

Cible principale : **Steam Deck OLED (RDNA2, SteamOS, gamescope, Proton)**, avec le HDR de RenoDX.
Banc de test : PC CachyOS + RX 9070 XT (même chemin Proton/DXVK).
Windows n'est pas une cible.

Résultat obtenu sur PC (1440p, RX 9070 XT) : image nettement moins aliasée qu'avec le SMAA du jeu.
Priorité actuelle : **valider sur le Deck et y réduire le coût** (voir §6 et M8).

## 2. État d'avancement

| Jalon | PC | Deck |
|---|---|---|
| M0–M3 : addon, cartographie des passes, interop Vulkan, FSR sans jitter | fait | fait |
| M4 : jitter | fait | **à valider** |
| M5 : vélocité des objets | fait | **à valider** |
| M6 : shaders sensibles au jitter | éclairage différé corrigé ; AO, réflexions, décalques à voir | — |
| M7 : finitions | à faire | — |
| M8 : optimisations Deck inspirées de Luma | à faire | à faire |
| M9 : upscaling (rendu < natif) | exploratoire | — |
| M10 : choix FSR 2 / 3 / 4 (fsr4-vk, sans OptiScaler) | à faire | à faire (coût à mesurer) |

Mesure connue : environ **3,4 ms** par frame sur Deck (FSR 3.1 en 800p, chemin Vulkan), au-delà du
budget visé de 2 ms. Détail du coût (préparation / FSR / copies / flush DXVK) : à relever sur Deck.

## 3. Contraintes connues

Configuration qui fonctionne sur le Deck, et que l'addon ne doit pas casser :

- ReShade **6.7.3** avec support des addons, installé comme `dxgi.dll` dans `Binaries/Win64`,
  `AddonPath=.\` dans `ReShade.ini` (voir §9).
- RenoDX `renodx-batmanak.addon64` v0.2026.1020.1520 **patché** (voir §9) pour le HDR.
- Ligne de lancement :
  `DXVK_FRAME_RATE=45 PROTON_ENABLE_HDR=1 DXVK_HDR=1 ENABLE_HDR_WSI=1 ENABLE_GAMESCOPE_WSI=1 WINEDLLOVERRIDES="dxgi=n,b" %command%`
- lsfg-vk 2x (45 → 90 fps) sur le Deck. Alternative si le budget manque : cap 40 fps, écran à 80 Hz
  (25 ms par frame au lieu de 22,2 ms).
- Le jeu est DX11 (feature level 11_0), UE3 très modifié, exe `BatmanAK.exe` (Steam, v1.6.2.0).
- Le jeu **n'importe pas d3d11.dll** : il le charge dynamiquement. Un proxy `d3d11.dll`
  est contourné sous Wine → c'est pour ça qu'on passe par un addon ReShade.
- Le jeu crée sa swapchain en plein écran exclusif (`Windowed = FALSE`), 1280×800 sur le Deck,
  format R8G8B8A8_UNORM (RenoDX la passe en R10G10B10A2 HDR10).
- Effets ReShade SMAA/FXAA/TDAA : à désactiver avec AK-FSR (appliqués après FSR, sur l'UI).

## 4. Méthodes de référence

### 4.1 Mod DLAA (Zorkanne33) — méthode étudiée, aucun code repris

Analyse statique du binaire du mod :

1. **Jitter côté CPU** : détour de la fonction UE3 qui construit la matrice de projection
   (`FPerspectiveMatrix`) dans `BatmanAK.exe`, trouvée par motif d'octets (une seule occurrence exigée
   dans `.text`). Le mod apprend quel **appelant** construit la vue principale (sur GOG : RVA `0xA1F4A5`
   et `0xA1F4F7`) et n'ajoute le jitter qu'à celle-là.
2. **Shaders sensibles au jitter** patchés au chargement (hash 64 bits du bytecode) : éclairage, AO,
   réflexions, décalques différés.
3. **Vecteurs de mouvement** : vélocité de la passe de motion blur (objets) + reprojection de la
   profondeur avec les matrices courante/précédente (caméra), combinées par un compute shader.
4. **Point d'insertion** : première passe de post-traitement, avant bloom, motion blur, tonemap et UI.
5. Extras : RCAS après tonemap (variante du tonemapper sans grain), hautes lumières sorties de l'AA
   puis réinjectées, pluie composée après l'AA.

### 4.2 Mod Luma « Batman Arkham Knight » — idées à reprendre (M8)

Source : [Luma Framework](https://github.com/Filoppi/Luma-Framework),
`Source/Games/Batman Arkham Knight/main.cpp`, commit `dc471fa` (22/10/2026), marqué « WorkInProgress ».
Licence MIT avec addendum : toute réutilisation de code doit citer les auteurs ou le projet.

1. **FSR 3 avec le backend DX11 natif de FidelityFX** (`FidelityFX/host/backends/dx11/ffx_dx11.h`,
   en-têtes © AMD, MIT ; Luma le lie statiquement via `ffx_backend_dx11_x64.lib`) : ni Vulkan,
   ni copies DX11 ↔ Vulkan, ni `FlushRenderingCommands`.
2. **Insertion à la place de la passe finale du SMAA du jeu** (AA du jeu sur « Élevé » requis) :
   - `SMAAEdgeDetection` = PS `0x067FF80F` → sautée ; sa SRV1 est la profondeur (à capturer).
   - `SMAABlendingWeightCalculation` = PS `0x2F55F98E` → sautée.
   - `SMAANeighborhoodBlending` = CS `0x42C0137E` → remplacée par FSR ;
     SRV0 = scène en couleur linéaire (entrée), UAV0 = sortie anti-aliasée.
   → le coût du SMAA est récupéré.
3. **Vecteurs de mouvement du jeu utilisés tels quels** : seule cible au format `R16G16_FLOAT`
   (repérée au `clear_render_target_view`), valeurs en [-1, 1] → échelle = résolution × 0,5,
   jitter inclus (`mvs_jittered = true`). Pas de reprojection maison.
4. **Jitter injecté à une adresse fixe** (Steam : `BatmanAK.exe + 0x104FF`, GOG : `+ 0x104BF`) :
   moins robuste que notre recherche par motif → **ne pas reprendre**.
5. Jitter Halton(2,3) sur 8 phases, en NDC (`× 2 / résolution`, Y inversé), converti en pixels pour FSR.
6. **Biais de LOD des textures** pendant que FSR est actif (`SR::GetMipLODBias`).
7. Exposition : auto-exposition de FSR (`auto_exposure = true`), mode HDR.
8. Pas de shaders sensibles au jitter : sur ce point, AK-FSR est en avance (M6).

## 5. Architecture de l'addon

- Addon ReShade C++ (API addon de ReShade 6.x, `reshade.hpp`), DX11 uniquement, sources dans `src/`.
- Événements ReShade : `init_device`, `init_swapchain`, `create_pipeline`/`init_pipeline`
  (hash + patch de bytecode), `bind_pipeline`, `bind_render_targets_and_depth_stencil`,
  `push_descriptors`, `draw`/`draw_indexed`/`dispatch`, `map_buffer_region`/`update_buffer_region`
  (constantes caméra), `clear_render_target_view` (à ajouter pour M8.3), `present`.
- Jitter : détour de `FPerspectiveMatrix` trouvé par motif dans le `.text` de `BatmanAK.exe`,
  appelant principal appris automatiquement, Halton(2,3) sur 8 phases.
- Backends FSR :
  - **Actuel — Vulkan via l'interop DXVK** : FSR 3.1 (FidelityFX SDK v1.1.4, `amd_fidelityfx_vk.dll`)
    sur des `VkImage` à nous, enveloppées en textures D3D11 (`CreateTexture2DFromVkImage`), avec copies
    D3D11, `FlushRenderingCommands` puis soumission sous `LockSubmissionQueue`. À conserver : c'est le
    chemin par lequel FSR 4 apparaît avec OptiScaler (M10).
  - **À ajouter — DX11 natif** (M8.2) : backend DX11 de FidelityFX, sélectionnable dans l'onglet.
  - **À éviter** : pont DX11 → DX12 (instable sous Proton).
- Point d'insertion :
  - **Actuel** : CS `0xB2A276FD` (préparation DoF/bloom ; t0 profondeur, t1 couleur HDR), présent
    quels que soient les réglages.
  - **Cible M8.1** : CS `0x42C0137E` (SMAA final), avec repli automatique sur `0xB2A276FD` quand l'AA
    du jeu est désactivé.
- Onglet **AK-FSR** dans l'overlay : mode, options, diagnostics (passes détectées, temps CPU/GPU,
  capture de frame). Réglages dans `ReShade.ini`, section `[AK-FSR]`.

## 6. Budget de performance sur le Deck

- À 45 fps : 22,2 ms par frame. À 40 fps (80 Hz) : 25 ms.
- Mesurer avant d'optimiser : temps GPU du jeu **sans** AK-FSR dans une zone chargée de la ville
  (overlay de performance du Deck, niveau 3-4), puis **avec**, au même endroit.
- Coût net de FSR = coût FSR − SMAA du jeu (récupéré avec M8.1) − effets AA ReShade désactivés.
- Objectif : ≤ 2 ms net, 45 fps tenus en ville avec RenoDX et lsfg-vk.

## 7. Jalons et critères d'acceptation

Chaque jalon se valide en jeu (la nuit en ville, caméra lente face à des toits/grilles) + logs.
Ne pas passer au suivant sans validation.

- **M0 — Squelette** ✅ : addon chargé à côté de RenoDX, onglet « AK-FSR », HDR intact.
- **M1 — Cartographie** ✅ : passe de vélocité, point d'insertion, CB view/projection trouvés à chaque frame.
- **M2 — Interop** ✅ : handles Vulkan via DXVK, aller-retour d'image sans crash, sur Deck.
- **M3 — FSR sans jitter** ✅ : image stable à l'arrêt, HDR intact.
- **M4 — Jitter** ✅ PC / ⏳ Deck : le scintillement des objets statiques disparaît
  (objectif principal du projet).
- **M5 — Objets mobiles** ✅ PC / ⏳ Deck : pas de traînées sur Batman, la cape, les ennemis, la Batmobile.
- **M6 — Shaders jitter-aware** 🟡 : éclairage différé fait. AO, réflexions, décalques **seulement si**
  un scintillement résiduel est visible sur le Deck après M4/M5.
- **M7 — Finitions** : touche on/off (comparaison en jeu), RCAS optionnel, pluie, hautes lumières.
- **M8 — Optimisations Deck inspirées de Luma** (voir §4.2), dans l'ordre du gain attendu :
  1. **Insertion au SMAA du jeu** : remplacer le CS `0x42C0137E`, sauter les PS `0x067FF80F` et
     `0x2F55F98E` ; capturer la profondeur sur la SRV1 de `0x067FF80F` ; repli sur `0xB2A276FD` si
     l'AA du jeu est désactivé. Critère : même qualité, coût du SMAA récupéré.
  2. **Backend FSR 3 DX11 natif** : vérifier la provenance du backend DX11 (en-têtes AMD MIT ;
     bibliothèques précompilées présentes dans Luma `Source/External/FidelityFX/libs`, compilées
     avec MSVC : vérifier l'édition de liens avec `lld-link`, sinon recompiler depuis les sources).
     Le garder sélectionnable à côté du backend Vulkan. Critère : coût GPU mesuré sur Deck inférieur
     au chemin Vulkan, image équivalente.
  3. **Vecteurs de mouvement du jeu** (`R16G16_FLOAT`, [-1, 1], jitterés) à la place de la
     reprojection. Critère : pas de traînées sur les objets mobiles ; sinon garder la reprojection.
  4. **Biais de LOD des textures** quand FSR est actif. Critère : textures lointaines plus nettes
     sans grésillement supplémentaire.
  5. **Exposition** : fournir l'exposition du jeu (texture 1×1 R16F du tonemapper) au lieu de
     l'auto-exposition de FSR, si le gain est mesurable.
- **M9 — Upscaling (exploratoire)** : tester d'abord sans l'addon si le jeu respecte
  `ScreenPercentage=80` (et `UpscaleScreenPercentage=True`) dans `BmSystemSettings.ini` : 3D plus douce,
  UI nette, fps en hausse. Si oui, faire rendre FSR de la résolution réduite vers la native à la place
  de l'agrandissement du moteur.
- **M10 — Choix FSR 2 / FSR 3 / FSR 4 sans OptiScaler** (voir §8) :
  1. Menu « Version FSR » : lister les versions de **tous** les fournisseurs chargés, avec la DLL
     d'origine : `amd_fidelityfx_vk.dll` (FSR 2.3.3, FSR 3.1.4) et, si présente,
     `amd_fidelityfx_upscaler_vk.dll` de fsr4-vk (« 4.1.1 Vulkan Auto / INT8 / FP8 »).
     Même API FFX (`ffxCreateContext` / `ffxDispatch` / `ffxQuery`), version imposée via
     `ffxOverrideVersion` à la création du contexte.
  2. Libellés : « FSR 4 » = FP8 (RDNA 4 uniquement, PC) ; « FSR 4 Lite » = INT8 (RDNA 2/3/4, Deck).
     Masquer FP8 si le GPU ne le gère pas ; « Auto » laisse fsr4-vk choisir.
  3. Vérifier au chargement que le `VkDevice` de DXVK expose ce que fsr4-vk exige
     (`VK_EXT_descriptor_buffer`, buffer device address, synchronization2) ; sinon griser FSR 4
     et l'indiquer dans l'onglet.
  4. Repli sur FSR 3.1 si la création du contexte FSR 4 échoue (déjà le cas pour une version absente).
  Critère : bascule FSR 2 / 3 / 4 Lite / 4 en jeu sans crash ; coût mesuré pour chacune sur Deck et PC.
  FSR 4 Lite sur Deck : à n'adopter que si le coût tient dans le budget du §6.

## 8. FSR 4 et OptiScaler (notes)

### 8.1 Voie retenue : fsr4-vk, sans OptiScaler

- AMD ne fournit FSR 4 qu'en DX12 (`amd_fidelityfx_upscaler_dx12.dll`) ; le dépôt du SDK 2.3.0 indique
  « Vulkan is currently not supported ». OptiScaler l'obtient en Vulkan via un pont vers DX12 (vkd3d).
- [fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) (Astyyyyy) : implémentation Vulkan indépendante de
  FSR 4.1.1, **FP8 et INT8**, à partir des modèles et shaders extraits de la DLL DX12 officielle du
  FidelityFX SDK 2.3.0 (version 4.1.1.2740, empreinte vérifiée par ses outils). Produit
  `amd_fidelityfx_upscaler_vk.dll`, qui exporte l'**API FFX standard** et annonce les versions
  « 4.1.1 Vulkan Auto », « 4.1.1 Vulkan INT8 », « 4.1.1 Vulkan FP8 ». Compilation croisée Windows depuis
  Linux prévue (MinGW-w64 : ajouter `mingw-w64-gcc` à l'image si besoin).
- Exige sur le device : `VK_EXT_descriptor_buffer` (`vkGetDescriptorEXT`, …), buffer device address,
  synchronization2. À vérifier sur le device de DXVK (PC et Deck).
- Projet très jeune (premiers commits fin octobre 2026), licence **GPL-3.0-only** : ne pas l'inclure dans
  le dépôt ni dans les binaires d'AK-FSR ; chaque utilisateur le compile lui-même (il a besoin de la DLL
  AMD de toute façon). AK-FSR se contente de le charger s'il est présent.

### 8.2 Essais avec OptiScaler (historique)

- FSR 4 est disponible sans code supplémentaire (constaté le 26/10/2026 sur PC) avec **OptiScaler
  nightly** chargé dans le processus : le chargeur de `amd_fidelityfx_vk.dll` (SDK v1.1.4) liste alors
  « 4.1.2 * » dans le menu « Version FSR » de l'onglet. Rendu jugé meilleur que FSR 3.1, même en Lite.
- `PROTON_FSR4_UPGRADE=1` seul : sans effet sur ce chemin (seules 3.1.4 et 2.3.3 listées ; la mise à
  niveau Proton ne vise que `amd_fidelityfx_dx12.dll`).
- Cohabitation OptiScaler + ReShade + RenoDX instable :
  - OptiScaler en `version.dll` avec les DLL du pilote AMD dans le dossier : crash en sortie de vidéo,
    puis jeu bloqué au démarrage.
  - ReShade 6.8.0 en `dxgi.dll` + OptiScaler en `wininet.dll` : blocage intermittent avant la swapchain.
  - **À tester** : OptiScaler en `dxgi.dll` chargeant ReShade lui-même (`ReShade64.dll`,
    `[Plugins] LoadReshade=true`), génération d'images d'OptiScaler entièrement désactivée
    (sinon il enveloppe la swapchain avec XeFG, ce qui gêne RenoDX).
- L'addon retombe sur la version FSR par défaut si la version sauvegardée n'est plus disponible.
- Une réécriture en plugin `.asi` d'OptiScaler (appels DLSS/NGX vers OptiScaler) a été étudiée :
  écartée, car elle impose de quitter ReShade, donc de perdre RenoDX et le HDR.

## 9. Environnement de développement

- Poste : CachyOS (Linux), jeu via Proton. Pas de machine Windows.
- Claude Code dans un conteneur Podman (`arch-claude`), dossier du jeu monté en `/game`.
- Compilation croisée vers Windows x64 : `clang-cl` + `lld-link` + SDK/CRT MSVC via `xwin`
  (CRT statique), voir `cmake/clang-cl-xwin.cmake`.
- Shaders Vulkan (`shaders/*.comp`) précompilés dans `src/shaders/` (`scripts/build_shaders.sh`, glslang).
- `scripts/fetch_ffx.sh` : récupère `amd_fidelityfx_vk.dll` (SDK v1.1.4, empreinte vérifiée).
- `scripts/deploy.sh` : copie vers le dossier du jeu (`AK_GAME_DIR` pour le changer).
- Logs utiles : `ReShade.log` (dossier du jeu), `PROTON_LOG=1` si crash.

## 10. Pièges déjà rencontrés (à ne pas refaire)

- **RenoDX + gamescope** : le plein écran simulé de RenoDX n'était jamais appliqué sous gamescope
  (zone de rendu minuscule dans un coin). Deux causes dans `mods::swapchain` : `OnSetFullscreenState`
  annule le « pending » quand le jeu appelle `SetFullscreenState(FALSE)` juste après la création, et
  `OnPresent` exige `GetForegroundWindow() == hwnd` (jamais vrai sous gamescope). Le binaire utilisé
  est patché (8 octets) pour contourner les deux. À signaler en amont.
- **HDR sous gamescope** : il faut les 4 variables (`PROTON_ENABLE_HDR`, `DXVK_HDR`,
  `ENABLE_HDR_WSI`, `ENABLE_GAMESCOPE_WSI`), sinon `ChangeColorSpace` échoue (0x80070057).
- **Mode Bureau du Deck** : pas de gamescope → la couche WSI gamescope affiche une erreur
  (« Creating swapchain for non-Gamescope swapchain »). Tester en Gaming Mode.
- **Proxy `d3d11.dll`** : contourné (chargement dynamique + deux modules du même nom sous Wine).
  Ne pas toucher à `system32` du préfixe : Proton le restaure.
- **SDK NGX** (DLSS) : vérifie la signature NVIDIA de `nvngx.dll` (WinVerifyTrust + signataire « NVIDIA »).
- **`bAllowTemporalAA=True`** (TAA UE3 dans `BaseSystemSettings.ini`) : sans effet dans ce jeu.
- **`AddonPath` absolu dans `ReShade.ini`** : la lettre `S:` de Proton ne pointe pas au même endroit sur
  PC (`S:\steamapps\…`) et sur Deck (`S:\common\…`). Utiliser `AddonPath=.\`.
- **`IDXGIVkInteropSurface::GetVulkanImageInfo`** (DXVK 3.1.1-pcos) : le premier appel verrouille l'image
  via une tâche synchrone sur le thread CS qui ne se termine jamais (jeu figé). Ne jamais l'appeler sur
  les textures du jeu : passer par des `VkImage` à nous enveloppées avec
  `IDXGIVkInteropDevice1::CreateTexture2DFromVkImage` et des copies D3D11.
- **CS `0x42C0137E`** : ce n'est **pas** le brouillard (ancienne note erronée) mais la passe finale du
  **SMAA du jeu** (`SMAANeighborhoodBlending`, identifiée grâce à Luma). Elle n'existe que si l'AA du jeu
  est activé, d'où son absence sur le Deck où il était désactivé. Point d'insertion toujours présent :
  CS `0xB2A276FD` (t0 profondeur, t1 couleur HDR).

## 11. Cadre et crédits

Projet personnel. On étudie et réimplémente des **méthodes** (passes, buffers, logique) :

- Mod DLAA (Zorkanne33) : aucun code ni binaire repris ou distribué.
- fsr4-vk (Astyyyyy, GPL-3.0-only) : chargé dynamiquement s'il est présent, jamais redistribué avec AK-FSR.
- Luma Framework (Filippo Tarpini et contributeurs) : idées reprises (§4.2). Tout code repris doit être
  signalé en commentaire et crédité dans le README, conformément à sa licence.
- Ne pas redistribuer de binaire du mod DLAA ni de RenoDX patché sans l'accord de leurs auteurs.
