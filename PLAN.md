# AK-FSR — addon ReShade : AA temporel FSR pour Batman: Arkham Knight (Proton / Steam Deck)

Ce fichier est le cahier des charges du projet. Il est fait pour être placé à la racine du dépôt
(ou copié dans `CLAUDE.md`) et lu par l'agent de code avant de commencer.

Dernière mise à jour : 01/11/2026 (version de l'addon : 0.9.0, FSR 3.1 DX11 pour Windows).

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
| M7 : finitions | on/off et hautes lumières codés (branche `wip/m6-m10`) ; RCAS après tonemap et pluie bloqués | — |
| M8 : optimisations Deck inspirées de Luma | M8.1–M8.4 codés (branche `wip/m6-m10`), à tester ; M8.5 à faire | à tester |
| M9 : upscaling (rendu < natif) | `ScreenPercentage` du jeu cassé sur PC (testé) : voie simple fermée | — |
| M10 : FSR 4 sans OptiScaler (provider du pilote AMD, code MIT) | M10.2–M10.3 fonctionnent (FP8) | fonctionne : INT8, 47 fps |
| M11 : FSR 4 Lite par la DLL DX12 | à faire | à faire |
| M13 : performances sur Deck | — | **clos (01/11/2026)** : mesuré, surcoût d'AK-FSR ≈ 1 ms, le reste est le modèle FSR 4 Lite (≈ 8,8 ms) ; pas d'optimisation rentable (voir §7) |

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
2. **Insertion à la place de la passe finale du SMAA du jeu** (AA du jeu sur « On » requis) :
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
    chemin de FSR 4 (provider du pilote AMD en Vulkan, M10).
  - **À ajouter — provider FSR 4 du pilote AMD** (M10.2) : même chemin Vulkan, fonctions obtenues de
    `amdvkffx64.dll` au lieu de `amd_fidelityfx_vk.dll`.
  - **À ajouter — DX11 natif** (M8.2) : backend DX11 de FidelityFX pour FSR 3, sélectionnable dans l'onglet.
  - **À éviter** : pont DX11 → DX12 (instable sous Proton), sauf en dernier repli pour FSR 4 (§8.2).
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
- **M9 relancé (01/11/2026) — ScreenPercentage à chaud, diagnostic** (outils de dev : `ue3`, `texshot`, `cbset`).
  - **Contrôle du moteur** : `ue3 scale set ScreenPercentage 67` (USystemSettings::Exec appelé par l'addon sur le
    thread de jeu, voir `src/ue3_exec.cpp`) ; changement pris en compte sans relancer. Jeu seul à 67 % : 90 fps (plafond)
    au lieu d'environ 59 à 100 %.
  - **Cause du rendu cassé** : la scène (G-buffer, accumulation de lumière) est rendue en haut à gauche du buffer
    1280×800 (vue 857×535), mais le moteur donne au post-traitement une origine de vue **centrée** :
    ((1280-857)/2, (800-535)/2) = (211, 132), dans les constantes b0 de nombreuses passes (ligne « taille.xy,
    origine.zw », registre c6 / c7 / c8 / c11 / c12 selon le shader). Constantes de vue correctes, elles (View CB c19 ≈
    0,335, c22 = 857×535, c23 = 1280×800).
  - **Vérifié à chaud** : origine forcée à (0, 0) (`cbset cs <hash> 0 11 2 0` et `… 11 3 0`) dans la composition de
    l'éclairage `0xFE8D7315` / `0x816CB5D7` → coupure de brouillard / éclairage disparue, image correcte à 67 %
    (profondeur de champ coupée). Passes qui reçoivent (211, 132) : `0x816CB5D7`, `0xFE8D7315` (c11), `0xA124DE12`
    (c12), chaîne de flou / DoF `0x0E6435F4`, `0x2B46745A` et `0x26C26A50`, `0x65FE6DB3`, `0x6F9D46E3`, `0x983D7D7F`,
    `0xAE739EF7`, `0xB029C097`, `0xB42A7F40`, `0xCA507609` (c6), `0x0EF4130E`, `0x41F457B3`, `0x5A988DD0`,
    `0x978BFB09`, `0x98C5C8CA`, `0xF98BC308`, PS `0x311E0BDA` (c7), `0xB2A276FD` (c8).
  - **Correction durable (commit 227e87f, vérifiée sur Deck le 01/11)** : AK-FSR réécrit l'origine centrée en (0, 0)
    à l'écriture des CB du jeu (`fix_view_origin` dans `src/passes.cpp`, actif seulement si rendu < buffer). À 67 % :
    image correcte, sans coupure, **profondeur de champ comprise** (le décalage DoF vu avec `cbset` venait d'un CB que
    `cbset` ne couvrait pas). Jeu à 57 fps avec FSR 4 Lite toujours en AA sur tout le buffer (≈ 7,8 ms).
  - **Reste** : (1) **brancher FSR 4 en upscaling** (entrée 857×535 en haut à gauche, sortie 1280×800) à la place de
    l'agrandissement du moteur (pixelisé) : c'est là qu'est le gain (jeu seul à 67 % : 90 fps) ; il faut repérer la
    passe d'agrandissement du moteur et y insérer FSR, avec jitter en pixels de rendu ; (2) sortie de `0xA124DE12`
    (réflexions ?) répétée horizontalement, sans effet visible : comparer à 100 % ; (3) colonne x = 1279 anormale
    (bord droit de l'agrandissement du moteur).
  - **Upscaling FSR 4 testé sur Deck (01/11, commits 73e0713, d7b5d0b)** : FSR 4 Lite en mode « Quality » (857×535
    -> 1280×800) avant l'agrandissement du moteur, draw d'agrandissement remplacé par une copie de la sortie (le
    moteur ne lit son échelle ni dans le CB View ni dans son CB PS). Ça marche (image entière, interface nette), mais
    **le coût de FSR 4 dépend de la sortie** : 8,9 ms en upscaling contre 8,8 ms en AA natif. Frame ≈ 22 ms (≈ 45 fps)
    contre ≈ 23 ms en AA natif à 100 % : **gain ≈ 1 à 2 ms seulement**, avec une image plus floue / des traînées en
    mouvement (vélocité des objets ignorée, entrée après tonemap). Verdict : pas rentable avec FSR 4 Lite sur le Deck ;
    réglage `Upscale` laissé à 0. Plus bas, pas mieux : 58 % -> FSR 9,6 ms, frame 22,2 ms ; 50 % -> FSR 10,3 ms,
    frame 23,7 ms. **Le jeu seul ne s'allège pas avec la résolution** sur le Deck (AK-FSR coupé, même endroit :
    16,7 ms à 100 %, 18,8 ms à 50 %) : il n'est pas limité par les pixels ici (CPU, ombres, géométrie). Piste close.
  - **Option gardée dans la version publiée (01/11/2026)** : liste « Résolution de rendu » dans l'onglet (100 / 77 / 67 /
    58 / 50 %), appliquée à chaud par USystemSettings::Exec (module `ue3_exec`, désormais dans la version publiée : import
    USER32 pour le sous-classement de la fenêtre) et sauvegardée par le jeu. `Upscale` actif par défaut ; en résolution
    réduite, jamais d'AA natif ; cible de l'agrandissement incompatible (back buffer HDR de RenoDX ?) détectée avant FSR
    -> upscaling abandonné pour la session, agrandissement du jeu sans AA ni jitter. Intérêt attendu surtout sur les
    machines limitées par les pixels (ROG Ally / Ally X en 1080p, RDNA 3) : non mesuré.
  - **Piège** : `scale set ScreenPercentage` est sauvegardé par le jeu dans BmSystemSettings.ini à la sortie : toujours
    remettre 100 en fin de test (lancement suivant à 67 % sinon, rendu dégradé en AA natif).
  - `UpscaleScreenPercentage=False` ne règle rien : post-traitement centré, scène toujours en haut à gauche.

- **M10 — FSR 4 sans OptiScaler, code MIT** (spécification en §8.1). Ordre :
  1. **M10.0 — Test sans code (PC)** : FSR4-Vulkan-Bridge installé à la main en remplacement de
     `amd_fidelityfx_vk.dll`, avec sa couche Linux en `LD_PRELOAD`. Critère : « 4.1.2 » listée dans
     l'onglet, `native-fsr4-bridge.log` montre un dispatch à 0 et des pipelines `fsr4_model_…_fp8_…`,
     image correcte avec nos entrées (couleur HDR, profondeur, vecteurs, jitter).
  2. **M10.1 — Test sans code (Deck)** : même installation sans FP8. Relever si le provider du pilote
     se crée et quel modèle il choisit (attendu par AMD pour Van Gogh : INT8 Lite).
  3. **M10.2 — Module « provider pilote » dans AK-FSR**, écrit par nous à partir des seuls faits
     d'interface du §8.1 (aucun code copié d'OptiScaler, GPL-3.0, ni de FSR4-Vulkan-Bridge, sans licence) :
     - charger les DLL AMD fournies par l'utilisateur depuis `Binaries/Win64/ak-fsr/` ;
     - obtenir le provider via `UpdateFfxApiProviderEx`, vérifier l'identifiant d'implémentation ;
     - activer les fonctions Vulkan nécessaires sur le device de DXVK en interceptant `vkCreateDevice`
       **avant** que DXVK crée son device (vérifier que l'addon est chargé assez tôt : ReShade charge les
       addons au moment de `D3D11CreateDevice`, avant l'appel réel) → plus besoin de `LD_PRELOAD` ;
     - fournir au provider un `vkGetDeviceProcAddr` enveloppé qui corrige les shaders du pilote pour RADV
       (types d'image float 16 bits → 32 bits dans le SPIR-V) ;
     - repli automatique sur FSR 3.1 en cas d'échec, message clair dans l'onglet.
  4. **M10.3 — Menu « Version FSR »** : FSR 2.3.3 et 3.1.4 (`amd_fidelityfx_vk.dll`, SDK 1.1.4), FSR 4.1.2
     (provider pilote) avec le modèle effectivement utilisé (FP8, INT8, INT8-2, Lite) quand il est connu.
  Critère global : bascule FSR 2 / 3 / 4 en jeu sans crash ; coût mesuré sur PC et Deck ; FSR 4 adopté sur
  le Deck seulement s'il tient dans le budget du §6.
  Options de repli si le provider pilote ne marche pas sur le Deck : fsr4-vk (INT8 4.1.1, GPL, DLL séparée
  installée par l'utilisateur) ou backend DX12 via vkd3d-proton avec `FSR4_UPGRADE=1` (Lite officiel,
  pont DX11↔DX12 à écrire). Voir §8.2 et §8.3.

- **M11 — FSR 4 Lite sur le Deck par la DLL DX12** : la DLL Vulkan du pilote (`amdvkffx64.dll`) choisit son
  modèle d'après Vulkan seul et prend l'INT8 sur Van Gogh (constaté, empreinte `0x1BCF1FEB`). La DLL DX12
  (`amdxcffx64.dll`, même FSR 4.1.2) demande la famille du GPU au pilote par `amdxc64.dll` ; sous Wine, sans
  réponse, elle refuse tout modèle. Méthode publique de FSR4Injector (Rayekkk, BSD-3 ; shim `amdxc64` LGPL,
  dérivé de GE-Proton) : un `amdxc64.dll` de remplacement renvoie la vraie famille et la révision du GPU, et AMD
  choisit Lite sur RDNA 3.5 / Van Gogh. Pour AK-FSR (DX11) : (1) pont DX11 → DX12 (device D3D12 de vkd3d-proton,
  textures et barrières partagées avec DXVK) ; (2) notre propre `amdxc64.dll`, écrit d'après les faits
  d'interface ; (3) FSR 4 par `amdxcffx64.dll` sur ce device. Critère : Lite relevé sur le Deck, coût ≤ FSR 4 INT8
  actuel, aucun plantage. Lourd et incertain (pont DX11↔DX12 jugé instable sous Proton).

- **M13 — Performances sur le Deck** (document de travail `M13-DECK-PERF.md`, hors dépôt). Point de départ au
  benchmark intégré (Deck OLED, 1280×800, AA natif) : 59 fps sans mod, 43 fps avec FSR 4 Lite (+6,3 ms, dont ≈ 3 ms
  hors modèle), minimum 38 → 21 fps. Objectifs : surcoût hors modèle ≤ 1 ms, moyenne ≥ 47 fps, minimum nettement relevé.
  1. **§1 Mesures** (codé, `src/perf.cpp`) : timestamps GPU par passe (D3D11 + Vulkan, bulles déduites), attentes CPU
     de l'interop, événements rares, CSV par frame, moyennes 60 frames à l'écran et dans le log ; procédure et
     capture RGP dans `DECK.md`, résumé par `scripts/frames_summary.py`. **Livrable : tableau sur 2 passages du
     benchmark, à faire valider avant toute optimisation.**
  2. O1 zéro attente CPU dans l'interop ; O2 suppression des copies ; O3 passes FSR inutiles ; O4 passe MV ;
     O5 à-coups ; O6 SMAA du jeu (test utilisateur). Une optimisation par commit, mesurée après chacune.
  3. **Bilan (clos le 01/11/2026)**, deux passages du benchmark (CSV dans `/workspace/deck/m13/`) :

     | | Passage 1 | Passage 2 |
     |---|---|---|
     | Bench : moy / min / max (fps) | 43,2 (CSV) | 43 / 31 / 54 |
     | GPU total AK-FSR (moy / p95) | 8,87 / 9,82 ms | 8,81 / 9,69 ms |
     | dont FSR 4 Lite | 7,83 ms | 7,77 ms |
     | copies d'entrée / MV / copie de sortie / pluie après FSR | 0,37 / 0,26 / 0,10 / 0,27 ms | idem |
     | bulles avant / après (médiane) | 0,01 / 0,03 ms | idem |
     | CPU : flush / verrou / fence / DLL AMD | 0,32 / ≈ 0,01 / 0,02 / 0 ms | 0,29 / ≈ 0,01 / 0,02 / 0 ms |

     Surcoût hors modèle ≈ 1,0 ms (objectif ≤ 1 ms quasi atteint sans rien changer) ; le modèle FSR 4 Lite coûte
     ≈ 7,8 ms sur Van Gogh (RDNA 2, sans instructions matricielles INT8), pas ≈ 3 ms comme supposé : objectif 47 fps
     inatteignable par O1–O5 (≤ 1 ms à gagner : O1 inutile, attentes CPU nulles ; O2 bloqué, GetVulkanImageInfo fige
     DXVK). Chutes du minimum : pas dues à AK-FSR (aucune création ni attente sur les frames lentes), sauf
     l'à-coup du premier passage (compute shader de la pluie compilé à la première frame : O5, non fait).
     Pistes explorées ensuite, sans gain : upscaling FSR 4 en jeu (M9 relancé : 67 / 58 / 50 % -> 45 / 45 / 42 fps,
     le jeu seul ne s'allège pas avec la résolution sur le Deck). Restent possibles hors M13 : FSR 3.1 en mode
     « performance » (≈ 5 ms de moins), ou cap à 40 fps / 80 Hz avec lsfg-vk.
     Acquis de M13 : mesures (`[Debug] FrameLog`), commandes du moteur (`ue3`), `texshot`, `cbset`, correction de
     l'origine de vue à ScreenPercentage < 100.

- **M14 — Windows (0.9.0, 01/11/2026)** : sous Windows, DXVK et ReShade ne se combinent pas (ReShade accroche
  `system32\d3d11.dll` alors que le jeu charge le `d3d11.dll` de DXVK : ReShade retombe sur le Vulkan de DXVK, l'addon ne
  voit que du SPIR-V et ne trouve aucune passe ; remplacer le `dxgi.dll` de DXVK par celui de ReShade plante). Testé sur
  RX 9070 XT, pilote AMD 2.0.416, DXVK 3.1.1, ReShade 6.8.0 global. FSR 4 demanderait un pont D3D11 ↔ Vulkan natif
  (textures NT partagées + `VK_KHR_external_memory_win32`, fence partagée importée en sémaphore timeline) : hors
  périmètre, « help wanted » dans le README. Décision : **backend FSR 3.1 DX11 natif rétabli** (`fsr_dx11.cpp`,
  `ak-fsr-dx11.dll`, portage gameplug-labs fsr3-v3.1.2, MIT), choisi automatiquement quand le device D3D11 ne répond pas
  à l'interop DXVK ; chemin FSR 4 Linux inchangé. À valider sous Windows : `Redirecting D3D11CreateDevice` dans le log,
  `backend : FSR 3.1 (DX11 natif)`, contexte FSR 3 créé ; benchmark 1440p comparé à la référence mod DLAA + OptiScaler +
  FSR 4 FP8 (116 / 80 / 144 fps de moyenne / min / max, sans limiteur).

## 8. FSR 4 (notes)

### 8.1 Voie retenue : provider FSR 4 du pilote AMD, appelé directement (faits d'interface)

Faits relevés dans OptiScaler (`OptiScaler/fsr4/FSR4DriverProvider*.h/.cpp`, GPL-3.0) et dans
[FSR4-Vulkan-Bridge](https://github.com/SemiRealProductions/FSR4-Vulkan-Bridge) (aucune licence publiée).
Ce sont des **faits d'interopérabilité** ; l'implémentation d'AK-FSR doit être écrite de zéro.

- **DLL du pilote** : `amdvkffx64.dll`, tirée du pilote Windows AMD Adrenalin 26.10.1
  (dossier `Packages/Drivers/Display/WT6A_INF/…` de l'archive extraite avec 7-Zip, sans installer le pilote).
  Propriétaire : **jamais dans le dépôt**, fournie par l'utilisateur.
- **Condition du pilote** : il ne donne ses providers que si des DLL du SDK FFX **signées par AMD** sont
  chargées dans le processus. Méthode publique du bridge : copier `amd_fidelityfx_loader_dx12.dll` et
  `amd_fidelityfx_upscaler_dx12.dll` du FSR SDK 2.3 (`Kits/FidelityFX/signedbin/`), renommées
  `amd_fidelityfx_loader_vk.dll` et `amd_fidelityfx_upscaler_vk.dll`, et les charger avant d'interroger
  le pilote. Le pilote déduit aussi la version du SDK de la première DLL vérifiée.
- **Export** : `UpdateFfxApiProviderEx(provider*, taille, options*)` (et `UpdateFfxApiProvider` sans options).
- **Structure provider v2** (x64) : `version` u32 = 2 ; remplissage ; `descriptor_type` u64
  (`0x10000` = création d'upscaler) ; puis en sortie : identifiant u64, nom (char*), et pointeurs
  `can_provide`, `create_context`, `destroy_context`, `configure`, `query`, `dispatch`.
- **Chaîne d'options** : nœud { type de valeur u32 (0 = booléen), identifiant u32 (0 = activer l'upscaler),
  valeur u8, remplissage, `next` } ; l'upscaler doit être activé explicitement.
- **Identifiant attendu pour FSR 4.1.2** : `0xF5A5CA1E81001002`.
- **Création du contexte** : en-tête upscale (`0x10000`) chaîné avec la description du backend Vulkan
  au type SDK 2.x `0x01000003` (même disposition que `ffxCreateBackendVKDesc`) et une description de
  version upscale `0x1000B` (versions acceptées 4.0.0 à 4.1.2).
- **Shaders et RADV** : les shaders du pilote déclarent des images en float 16 bits que seul le pilote AMD
  accepte ; il faut réécrire ces types en float 32 bits dans le SPIR-V reçu par `vkCreateShaderModule`
  (via le `vkGetDeviceProcAddr` passé au provider).
- **Fonctions Vulkan requises (FP8, RDNA 4)** : `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`,
  `VK_NV_cooperative_matrix2` (conversions ; sous RADV, variable d'environnement
  `radv_cooperative_matrix2_nv=true`), `VK_KHR_shader_float16_int8`, `VK_KHR_16bit_storage`,
  `VK_KHR_variable_pointers`, `VK_KHR_vulkan_memory_model`, `VK_EXT_subgroup_size_control`.
  Pour INT8/Lite (Deck) : à établir au test M10.1.
- **Modèle choisi par le pilote selon le GPU** (logique reproduite par OptiScaler) : RDNA 4 → FP8 ;
  RDNA 3 → INT8 ; RDNA 2 de bureau → INT8-2 ; **Van Gogh (Steam Deck)** et Strix Point → **INT8 Lite**.
  Sous Proton, la détection de la puce peut échouer (le patch Proton-CachyOS force Lite).
- **Fragilité** : interface interne du pilote, liée à sa version (le bridge vérifie l'empreinte de
  `amdvkffx64.dll`). Vérifier l'identifiant et échouer proprement si la version change.

### 8.2 Repli : fsr4-vk

- [fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) : implémentation Vulkan indépendante de FSR 4.1.1 (FP8 et
  INT8, sans Lite), API FFX standard, annonce « 4.1.1 Vulkan Auto / INT8 / FP8 ». Exige
  `VK_EXT_descriptor_buffer`, buffer device address, synchronization2 sur le device.
- Licence GPL-3.0-only : DLL séparée, compilée et installée par l'utilisateur, chargée si présente ;
  jamais incluse dans le dépôt ni les binaires d'AK-FSR.
- Autre repli possible : backend DX12 via vkd3d-proton, où Proton (`FSR4_UPGRADE=1`, `upscalers.py` des
  protonfixes) installe `amdxcffx64.dll` dans le préfixe ; demande un pont DX11↔DX12 (textures et fences
  partagées entre DXVK et vkd3d-proton).

### 8.3 Essais avec OptiScaler (historique)

- FSR 4 est disponible sans code supplémentaire (constaté le 26/10/2026 sur PC) avec **OptiScaler
  nightly** chargé dans le processus : le chargeur de `amd_fidelityfx_vk.dll` (SDK v1.1.4) liste alors
  « 4.1.2 * » dans le menu « Version FSR » de l'onglet. Rendu jugé meilleur que FSR 3.1, même en Lite.
- `PROTON_FSR4_UPGRADE=1` seul : sans effet sur ce chemin (seules 3.1.4 et 2.3.3 listées ; la mise à
  niveau Proton ne vise que `amd_fidelityfx_dx12.dll`).
- Cohabitation OptiScaler + ReShade + RenoDX instable :
  - OptiScaler en `version.dll` avec les DLL du pilote AMD dans le dossier : crash en sortie de vidéo,
    puis jeu bloqué au démarrage.
  - ReShade 6.8.0 en `dxgi.dll` + OptiScaler en `wininet.dll` : blocage intermittent avant la swapchain.
  - OptiScaler en `dxgi.dll` chargeant ReShade lui-même (`ReShade64.dll`, `[Plugins] LoadReshade=true`),
    génération d'images désactivée : violation d'accès dans `amdxc64.dll` juste après l'interop.
    Avec `[FSR] Fsr4DoNotLoadAmdxc64=true` : plus de crash, mais FSR 4 n'est plus proposé
    (seules 3.1.4 et 2.3.3 listées). Aucune configuration à la fois stable et avec FSR 4 → M10.
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
- OptiScaler (GPL-3.0) et FSR4-Vulkan-Bridge (SemiRealProductions, sans licence) : sources des faits
  d'interface du §8.1 ; **aucun code repris**, crédités dans le README comme références.
- DLL AMD (`amdvkffx64.dll`, DLL signées du SDK) : propriétaires, fournies par l'utilisateur, jamais dans le dépôt.
- fsr4-vk (Astyyyyy, GPL-3.0-only) : chargé dynamiquement s'il est présent, jamais redistribué avec AK-FSR.
- Luma Framework (Filippo Tarpini et contributeurs) : idées reprises (§4.2). Tout code repris doit être
  signalé en commentaire et crédité dans le README, conformément à sa licence.
- Ne pas redistribuer de binaire du mod DLAA ni de RenoDX patché sans l'accord de leurs auteurs.
