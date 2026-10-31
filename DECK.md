# AK-FSR — test sur Steam Deck

Objectif : mesurer le coût réel sur le Deck et le modèle FSR 4 choisi par le pilote (M10.1). Tout se fait par
fichiers : pas de clavier ni d'overlay à manipuler.

## Installation (une fois)

1. Copier le contenu de `deck-package/Binaries/Win64/` dans `Batman Arkham Knight/Binaries/Win64/` du Deck
   (addon de **développement**, `amd_fidelityfx_vk.dll`, `ak-fsr-dx11.dll`, dossier `ak-fsr/` avec les DLL AMD).
   Garder ReShade 6.7.3+ en `dxgi.dll`, RenoDX et `AddonPath=.\` comme aujourd'hui.
2. **Ne pas installer OptiScaler** (plantages avec AK-FSR).
3. Ligne de lancement : celle du Deck, inchangée (`radv_cooperative_matrix2_nv` est inutile sur le Deck).

## Séquence de test

Pour chaque étape : jeu fermé, remplacer la section `[AK-FSR]` de `ReShade.ini` par le fichier
`deck-package/ini/<étape>.ini`, lancer, **charger la partie et ne rien toucher pendant 2 minutes** (Batman sur son
toit de départ : même vue à chaque fois), quitter. Puis copier `ReShade.log` et `ak-fsr-report.txt` dans
`/workspace/deck/<étape>/` et supprimer `ak-fsr-report.txt` avant l'étape suivante.

| Étape | AA du jeu | Contenu | Ce qu'on mesure |
|---|---|---|---|
| `1-sans` | Off | AK-FSR coupé (`Mode=0`) | fps du jeu seul |
| `2-ref` | Off | FSR 3.1 Vulkan, comme la v0.6.3 | coût de référence (~3,4 ms mesurés avant) |
| `3-budget` | **On** | FSR 3.1 DX11 à la place du SMAA, biais de LOD -1 | gain de M8.1 + M8.2 |
| `4-fsr4` | **On** | FSR 4 (pilote, modèle Auto), à la place du SMAA | modèle choisi, coût |

Le rapport (toutes les 30 s) donne les fps, les temps GPU de FSR (Vulkan ou DX11) et, pour l'étape 4, le modèle :
`FP8`, `INT8` (empreinte `0x1BCF1FEB`, celle du PC) ou `INT8, autre variante (Lite probable)`. Le log indique aussi
`shaders créés avec le contexte : N, empreinte globale 0x…`.

Si le jeu plante à l'étape 4 : remettre l'étape 3, et ajouter le log Proton (`PROTON_LOG=1`) au retour.

## Version publiée

`deck-package/release/` contient la version sans outils (onglet Off / FSR 3.1 / FSR 4), à installer une fois les
réglages validés à la place de l'addon de développement.

## M13 — mesures de performance (§1 de M13-DECK-PERF)

Aucune optimisation n'est active : cette version ne fait que **mesurer**. Les mesures sont coupées par défaut
(aucun coût) et s'activent dans `ReShade.ini`, section à ajouter telle quelle :

```ini
[Debug]
FrameLog=on          ; une ligne par frame dans Binaries/Win64/ak-fsr-frames.csv (fichier écrasé à chaque lancement)
PerfOverlay=off      ; on : 3 lignes de moyennes (60 frames) à l'écran, même overlay ReShade fermé
PerfOverlayCorner=1  ; 0 haut gauche, 1 haut droite, 2 bas gauche, 3 bas droite
; PerfLog=on         ; moyennes dans ReShade.log toutes les 60 frames (actif dès que FrameLog ou PerfOverlay l'est)
```

`PerfOverlay` force ReShade à préparer son interface à chaque frame (un peu de CPU) : le laisser à `off` pour les
chiffres officiels, `on` seulement pour regarder en direct. Le watermark du pilote (`FSR4L-i8`) est dessiné par la
DLL AMD dans l'image : nos lignes sont une fenêtre à part, dans le coin choisi.

### Ce qui est mesuré

| Mesure | Comment |
|---|---|
| GPU par passe | timestamps D3D11 (contexte immédiat) autour du bloc AK-FSR et des copies, timestamps Vulkan dans notre command buffer (vecteurs de mouvement, FSR, fin). Sous DXVK les deux sont dans la même base de temps : on en déduit les **bulles** (GPU entre la fin des copies d'entrée et le début de notre command buffer, puis entre sa fin et la copie de sortie). Lecture sans attente (disponibilité), jamais `WAIT_BIT`. |
| CPU, attentes | `QueryPerformanceCounter` autour de `FlushRenderingCommands`, de l'attente de `LockSubmissionQueue`, du verrou tenu (`vkQueueSubmit`), du `vkWaitForFences` de l'anneau de 3 command buffers, de `vkDeviceWaitIdle` et des attentes demandées par la DLL AMD. |
| Événements rares | création/destruction du contexte FFX, (re)création des ressources (taille/format de la cible), pipelines et modules SPIR-V créés par la DLL AMD, ses allocations mémoire et soumissions, pipeline AK-FSR, compute shaders D3D11 d'AK-FSR, repli du point d'insertion. Dans le log (horodatés, avec le n° de frame) et dans le CSV. |

Colonnes de `ak-fsr-frames.csv` (ms sauf mention) :

| Colonne | Sens |
|---|---|
| `frame`, `time_s` | n° de frame (present) et temps depuis le chargement de l'addon (s) |
| `cpu_frame_ms` | present → present (frametime vu par le CPU) |
| `insertion` | `S` FSR à la place du SMAA du jeu, `B` au point `0xB2A276FD`, `-` AK-FSR n'a pas tourné |
| `cpu_akfsr_ms` | temps CPU de `fsr::run` (attentes comprises) |
| `wait_flush_ms` | `FlushRenderingCommands` (DXVK synchronise son thread CS) |
| `wait_lock_ms`, `lock_held_ms` | attente du verrou de la file, puis verrou tenu |
| `wait_fence_ms` | fence du command buffer de la frame N-3 |
| `wait_idle_ms` | `vkDeviceWaitIdle` (recréations) |
| `wait_driver_ms` | attentes demandées par la DLL AMD |
| `gpu_total_ms` | bloc AK-FSR complet vu par D3D11 (du début à la fin, bulles comprises) |
| `gpu_pre_ms` | avant les copies (exposition du jeu, hautes lumières : 0 si coupés) |
| `gpu_copy_in_ms` | copies d'entrée D3D11 (couleur, profondeur, vélocité, exposition) |
| `gpu_gap_in_ms` | **bulle** : fin des copies → début de notre command buffer (barrière d'entrée comprise) |
| `gpu_mv_ms` | passe profondeur + vecteurs de mouvement (compute AK-FSR) |
| `gpu_fsr_ms` | `ffxDispatch` FSR 4 |
| `gpu_vk_end_ms` | barrières de fin de notre command buffer |
| `gpu_gap_out_ms` | **bulle** : fin de notre command buffer → copie de sortie |
| `gpu_copy_out_ms` | copie du résultat vers la cible du jeu |
| `gpu_post_ms` | après la copie (hautes lumières, pluie après FSR) |
| `creations` | créations de la frame, nombre et durée CPU : `amd_shader`, `amd_pipeline`, `amd_alloc`, `amd_submit`, `akfsr_pipeline`, `akfsr_d3d_shader` |
| `events` | événements rares de la frame |

Les colonnes GPU d'une frame arrivent quelques frames plus tard (lecture sans attente) ; une colonne vide = mesure
pas revenue (ou AK-FSR inactif).

### Protocole (livrable §1)

1. `[AK-FSR]` habituel (FSR 4, modèle Lite, watermark comme d'habitude), `[Debug] FrameLog=on`, `PerfOverlay=off`.
2. Lancer le jeu, lancer le **benchmark intégré deux fois de suite sans quitter le jeu** (le 2ᵉ passage montre ce
   qui relève de la compilation au premier). Noter les fps moyen / min / max affichés par le jeu à chaque passage.
3. Quitter, puis copier `Binaries/Win64/ak-fsr-frames.csv` et `ReShade.log` dans `/workspace/deck/m13/` (le CSV est
   écrasé au lancement suivant).
4. Même chose avec `Mode=0` (AK-FSR coupé) pour la référence, si on veut les frametimes « sans mod » au même format.
5. Résumé :

   ```sh
   python3 scripts/frames_summary.py ak-fsr-frames.csv          # un bloc par passage (coupés aux chargements)
   python3 scripts/frames_summary.py ak-fsr-frames.csv --all    # référence Mode=0
   ```

Tableau attendu (à remplir avec le résumé, puis à faire valider avant toute optimisation) :

| | Passage 1 | Passage 2 |
|---|---|---|
| Bench : moy / min / max (fps) | | |
| GPU total AK-FSR (moy / p95) | | |
| copies d'entrée / bulle avant | | |
| vecteurs de mouvement | | |
| FSR 4 Lite | | |
| bulle après / copie de sortie | | |
| CPU : flush / verrou / fence / pilote (moy / max) | | |
| créations pendant le passage | | |

### Capture RGP (Radeon GPU Profiler) sur le Deck

RADV écrit des captures `.rgp` (SQTT) à ouvrir dans RGP sur le PC : on y voit la frise du GPU, les trous autour de
notre bloc et la durée réelle de chaque dispatch FSR.

**À vérifier d'abord sur le Deck** (les noms ont changé selon les versions de Mesa ; la doc de mesa3d.org n'a pas pu
être consultée en écrivant ce texte) :

```sh
pacman -Q vulkan-radeon lib32-vulkan-radeon 2>/dev/null; vulkaninfo --summary | grep -i driverInfo  # version de Mesa
strings /usr/lib/libvulkan_radeon.so | grep -E '^(MESA_VK_TRACE|RADV_THREAD_TRACE|RADV_SQTT)' | sort -u
```

- Si la liste contient `MESA_VK_TRACE`, `MESA_VK_TRACE_FRAME` et `MESA_VK_TRACE_TRIGGER` (Mesa récent) :

  ```
  MESA_VK_TRACE=rgp MESA_VK_TRACE_TRIGGER=/tmp/rgp.trigger <ligne de lancement habituelle> %command%
  ```

  puis, pendant le benchmark, depuis une session SSH (ou Konsole en mode Bureau) : `touch /tmp/rgp.trigger`.
  Variante sans clavier : `MESA_VK_TRACE_FRAME=<n>` capture la frame n (à 45 fps, 4500 ≈ 100 s après le lancement).
- Si elle contient seulement `RADV_THREAD_TRACE` et `RADV_THREAD_TRACE_TRIGGER` (Mesa ancien) : mêmes usages avec
  `RADV_THREAD_TRACE=<n>` (n° de frame) ou `RADV_THREAD_TRACE_TRIGGER=/tmp/rgp.trigger`.
- Les options de taille de tampon / timing d'instructions (`…_BUFFER_SIZE`, `…_INSTRUCTION_TIMING`, selon les noms
  listés) ne sont utiles que si RGP signale un tampon plein.

La capture est écrite dans `/tmp` (nom `<exe>_<date>.rgp`, chemin exact dans la sortie de RADV : lancer avec
`PROTON_LOG=1` ou regarder `/tmp` après coup). Pour une frise lisible : **couper lsfg-vk** pendant la capture (il
ajoute ses propres frames et presents), garder le même endroit du benchmark, faire une capture avec AK-FSR et une avec
`Mode=0`. Si aucune capture n'apparaît, regarder le log Proton : RADV y écrit la raison (droits sur les compteurs de
performance, tampon trop petit…).
