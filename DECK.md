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
