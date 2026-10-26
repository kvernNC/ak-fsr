# AK-FSR — addon ReShade : AA temporel FSR pour Batman: Arkham Knight (Proton / Steam Deck)

Ce fichier est le cahier des charges du projet. Il est fait pour être placé à la racine du dépôt
(ou copié dans `CLAUDE.md`) et lu par l'agent de code avant de commencer.

## 1. Objectif

Écrire un **addon ReShade** (`ak-fsr.addon64`) qui ajoute un anti-aliasing temporel **FSR**
à Batman: Arkham Knight, en résolution native (mode AA), comme le fait le mod
« Native DLSS Anti Aliasing for Arkham Knight » (Nexus, Zorkanne33) avec DLAA,
mais sans dépendre d'un GPU NVIDIA.

Cible principale : **Steam Deck OLED (RDNA2, SteamOS, gamescope, Proton)**.
Banc de test : PC CachyOS + RX 9070 XT (même chemin Proton/DXVK).
Windows n'est pas une cible.

Plus tard (hors périmètre du v1) : upscaling (rendu < natif).

FSR 4 : disponible sans code supplémentaire (constaté le 26/10/2026 sur PC, RX 9070 XT) avec **OptiScaler
nightly** chargé dans le processus : le chargeur de `amd_fidelityfx_vk.dll` (SDK v1.1.4) liste alors « 4.1.2 * »,
sélectionnable dans le menu « Version FSR » de l'onglet. Rendu jugé meilleur que FSR 3.1, même en variante Lite.
`PROTON_FSR4_UPGRADE=1` seul (sans OptiScaler) : sans effet sur ce chemin (seules 3.1.4 et 2.3.3 listées ; la
mise à niveau Proton ne vise que `amd_fidelityfx_dx12.dll`). Coût et Deck (INT8 RDNA2) : à mesurer.
Mis de côté pour l'instant : la cohabitation OptiScaler + ReShade + RenoDX est instable (crash en sortie de vidéo,
puis jeu bloqué au démarrage avec `version.dll` et les DLL du pilote AMD dans le dossier ; tout rentre dans
l'ordre une fois ces fichiers retirés).
Essai suivant (PC, 26/10/2026) : ReShade 6.8.0 en `dxgi.dll`, OptiScaler nightly en `wininet.dll`
(`WINEDLLOVERRIDES="dxgi=n,b;wininet=n,b"`) : un lancement réussi (« 4.1.2 * » disponible), le suivant bloqué au
même endroit (avant la swapchain) -> blocage intermittent entre OptiScaler et ReShade. À tester : OptiScaler en
`dxgi.dll` chargeant ReShade lui-même (`ReShade64.dll`, `[Plugins] LoadReshade=true`). L'addon retombe sur la
version FSR par défaut si la version sauvegardée n'est plus disponible.
Effets ReShade SMAA/FXAA du preset : à désactiver (appliqués après FSR, sur l'UI).

## 2. Contraintes connues (validées le 26/10/2026)

Configuration qui fonctionne aujourd'hui sur le Deck, et que l'addon ne doit pas casser :

- ReShade **6.7.3** avec support des addons, installé comme `dxgi.dll` dans `Binaries/Win64`.
- RenoDX `renodx-batmanak.addon64` v0.2026.1020.1520 **patché** (voir §7) pour le HDR.
- Ligne de lancement :
  `DXVK_FRAME_RATE=45 PROTON_ENABLE_HDR=1 DXVK_HDR=1 ENABLE_HDR_WSI=1 ENABLE_GAMESCOPE_WSI=1 WINEDLLOVERRIDES="dxgi=n,b" %command%`
- lsfg-vk 2x (45 → 90 fps) sur le Deck.
- Le jeu est DX11 (feature level 11_0), UE3 très modifié, exe `BatmanAK.exe` (Steam, v1.6.2.0).
- Le jeu **n'importe pas d3d11.dll** : il le charge dynamiquement. Un proxy `d3d11.dll`
  est contourné sous Wine → c'est pour ça qu'on passe par un addon ReShade.
- Le jeu crée sa swapchain en plein écran exclusif (`Windowed = FALSE`), 1280×800 sur le Deck,
  format R8G8B8A8_UNORM (RenoDX la passe en R10G10B10A2 HDR10).

## 3. Ce que fait le mod DLAA (méthode, à réimplémenter, pas de code à copier)

Analyse statique du binaire du mod (pas de code repris, on ne s'inspire que de la méthode) :

1. **Jitter côté CPU** : détour (hook inline) de la fonction UE3 qui construit la matrice de
   projection (`FPerspectiveMatrix`) dans `BatmanAK.exe`, trouvée par motif d'octets (une seule
   occurrence exigée dans `.text`). Le mod apprend quel **appelant** construit la vue principale
   (sur GOG : RVA `0xA1F4A5` et `0xA1F4F7`) et n'ajoute le décalage de jitter qu'à celle-là.
   → Toutes les passes du jeu sont jitterées nativement.
2. **Shaders sensibles au jitter** patchés au chargement (identifiés par hash 64 bits du bytecode) :
   éclairage, AO, réflexions, décalques différés. Sans ça, ombres/AO/décalques lointains scintillent.
3. **Vecteurs de mouvement** :
   - objets mobiles : buffer de vélocité de la **passe de motion blur** du jeu
     (le Motion Blur doit être activé dans le jeu) ;
   - caméra : reprojection depuis la profondeur avec les matrices courante/précédente lues dans
     les constantes de la **première passe de post-traitement**.
   - Un compute shader combine les deux.
4. **Point d'insertion** : première passe de post-traitement — couleur HDR de la scène + profondeur,
   **avant** bloom, motion blur, tonemapping et UI. L'AA y est évalué et le résultat réinjecté.
5. Extras (facultatifs) : sharpening RCAS après tonemap (variante du tonemapper sans grain),
   hautes lumières sorties de l'AA puis réinjectées, pluie composée après l'AA.

Options du mod qui donnent des indices : `HideMotionBlur`, `RainAfterDLAA`, `HighlightThreshold`,
`PatchShaders`, `[Jitter] Callers=auto`, `JitterSign`.

## 4. Architecture de l'addon

- Addon ReShade C++ (API addon de ReShade 6.x, `reshade.hpp`), DX11 uniquement.
- Base conseillée : le framework **RenoDX** (open source, MIT) pour les utilitaires
  (hash de shaders, suivi des ressources/pipelines, réglages UI). L'addon peut être un addon
  séparé de `renodx-batmanak` ou un fork de celui-ci (HDR + FSR dans un seul fichier).
- Événements ReShade utilisés : `init_device`, `init_swapchain`, `create_pipeline`/`init_pipeline`
  (hash + patch de bytecode), `bind_pipeline`, `bind_render_targets_and_depth_stencil`,
  `push_descriptors`, `draw`/`draw_indexed`/`dispatch`, `map_buffer_region`/`update_buffer_region`
  (lecture des constantes caméra), `present`.
- Hook `FPerspectiveMatrix` : MinHook (ou équivalent) depuis l'addon, motif d'octets recherché
  dans le `.text` de `BatmanAK.exe` au chargement.
- Backend FSR, deux options à évaluer au jalon M3 :
  - **A — Vulkan via l'interop DXVK** : l'addon récupère le `VkDevice`/`VkImage` derrière les
    ressources DX11 (`ID3D11VkInteropDevice`, `IDXGIVkInteropSurface` de DXVK) et appelle
    **FSR 3.1 Vulkan** (FidelityFX SDK, `amd_fidelityfx_vk.dll`) sur la même file.
    Proton uniquement (OK : seule cible). Pas de pont DX12 → plus stable.
  - **B — FSR 2.2 en DX11 natif** via un portage DX11 existant (vérifier provenance et licence).
    Plus simple, fonctionne partout, qualité un peu inférieure.
  - **À éviter** : pont DX11 → DX12 (source d'instabilité sous Proton, constaté avec OptiScaler).

## 5. Jalons et critères d'acceptation

Chaque jalon se valide en jeu (le joueur se place la nuit en ville, caméra lente face à des
toits/grilles) + logs. Ne pas passer au suivant sans validation.

- **M0 — Squelette** : addon chargé par ReShade à côté de RenoDX, onglet « AK-FSR » dans
  l'overlay, log `addon loaded` dans `ReShade.log`. Aucun effet visuel. HDR toujours OK.
- **M1 — Cartographie** : avec dumps optionnels (comme `DumpFrameKey` du mod), identifier et logger
  par hash : la passe motion blur (buffer de vélocité), la première passe post (couleur + profondeur),
  le constant buffer contenant view/projection. Critère : les 3 sont trouvés à chaque frame, en jeu
  et en cinématique.
- **M2 — Interop** (si backend A) : récupérer les handles Vulkan via DXVK, faire une copie
  d'image triviale côté Vulkan et la réinjecter. Critère : image identique, pas de crash, sur Deck.
  Si échec → backend B.
- **M3 — FSR sans jitter** : FSR en AA natif au point d'insertion, vecteurs = caméra seule.
  Critère : image stable à l'arrêt, pas de casse du HDR, coût < 2 ms sur Deck.
- **M4 — Jitter** : hook `FPerspectiveMatrix`, apprentissage de l'appelant principal, séquence
  Halton. Critère : le scintillement des objets statiques disparaît (objectif principal du projet).
- **M5 — Objets mobiles** : intégrer la vélocité de la passe motion blur. Critère : pas de traînées
  sur Batman, la cape, les ennemis, la Batmobile.
- **M6 — Shaders jitter-aware** : patch des shaders éclairage/AO/réflexions/décalques.
  Critère : plus de scintillement des ombres et décalques lointains.
- **M7 — Finitions** : touche on/off, RCAS optionnel, gestion pluie/hautes lumières, réglages UI.

## 6. Environnement de développement

- Poste : CachyOS (Linux), le jeu tourne via Proton. Pas de machine Windows.
- L'addon est une **DLL Windows x64** → compilation croisée : `clang-cl` + en-têtes MSVC via
  `xwin` (préféré, compatible avec le code RenoDX), ou `mingw-w64`.
- Shaders HLSL → DXBC : `fxc.exe` via Wine, ou shaders précompilés versionnés dans le dépôt.
- Déploiement : copier `ak-fsr.addon64` dans
  `~/.steam/steam/steamapps/common/Batman Arkham Knight/Binaries/Win64/`.
- Logs utiles : `ReShade.log` (dossier du jeu), `PROTON_LOG=1` si crash.

## 7. Pièges déjà rencontrés (à ne pas refaire)

- **RenoDX + gamescope** : le plein écran simulé de RenoDX n'était jamais appliqué sous gamescope
  (fenêtre/zone de rendu minuscule dans un coin). Deux causes dans `mods::swapchain` :
  `OnSetFullscreenState` annule le « pending » quand le jeu appelle `SetFullscreenState(FALSE)` juste
  après la création, et `OnPresent` exige `GetForegroundWindow() == hwnd` (jamais vrai sous gamescope).
  Le binaire utilisé est patché (8 octets) pour contourner les deux. À signaler en amont.
- **HDR sous gamescope** : il faut les 4 variables (`PROTON_ENABLE_HDR`, `DXVK_HDR`,
  `ENABLE_HDR_WSI`, `ENABLE_GAMESCOPE_WSI`), sinon `ChangeColorSpace` échoue (0x80070057).
- **Mode Bureau du Deck** : pas de gamescope → la couche WSI gamescope affiche une erreur
  (« Creating swapchain for non-Gamescope swapchain »). Tester en Gaming Mode.
- **Proxy `d3d11.dll`** : contourné (chargement dynamique + deux modules du même nom sous Wine).
  Ne pas toucher à `system32` du préfixe : Proton le restaure.
- **SDK NGX** (DLSS) : vérifie la signature NVIDIA de `nvngx.dll` (WinVerifyTrust + signataire
  « NVIDIA »). Raison de plus pour appeler FSR directement.
- **`bAllowTemporalAA=True`** (TAA UE3 dans `BaseSystemSettings.ini`) : sans effet dans ce jeu.
- Le SMAA du jeu (option AA « Élevé ») reste utile en complément.
- **`AddonPath` absolu dans `ReShade.ini`** : la lettre `S:` de Proton ne pointe pas au même endroit
  sur PC (`S:\steamapps\…`) et sur Deck (`S:\common\…`). Utiliser `AddonPath=.\`.
- **`IDXGIVkInteropSurface::GetVulkanImageInfo`** (DXVK 3.1.1-pcos) : le premier appel verrouille l'image
  via une tâche synchrone sur le thread CS qui ne se termine jamais (jeu figé, même depuis un autre thread).
  Ne jamais l'appeler sur les textures du jeu : passer par des `VkImage` à nous enveloppées avec
  `IDXGIVkInteropDevice1::CreateTexture2DFromVkImage` et des copies D3D11.
- **CS `0x42C0137E`** (brouillard) n'existe qu'avec les réglages élevés (absent sur Deck) : le point
  d'insertion commun est le CS **`0xB2A276FD`** (t0 profondeur, t1 couleur HDR).

## 8. Cadre

Projet personnel. On étudie et réimplémente des **méthodes** (passes, buffers, logique), sans
reprendre de code décompilé du mod DLAA. Ne pas redistribuer de binaire du mod ni de RenoDX patché
sans l'accord de leurs auteurs.
