# AK-FSR — journal de la branche `wip/m6-m10`

Branche de travail autonome (pas de push), partie de `main` (v0.6.3). Un commit par sous-étape ;
procédure de test détaillée dans [TESTING.md](TESTING.md). Version de l'addon : **0.7.0**.

| Étape | État | Option (défaut : désactivée) |
|---|---|---|
| M7.1 touche on/off | codé, à tester | `ToggleKey` |
| M7.2 hautes lumières hors FSR | codé, à tester | `Highlights`, `HighlightThreshold` |
| M7.3 RCAS après tonemap, pluie | bloqué (documenté) | — |
| M8.1 insertion au SMAA du jeu | codé, à tester | `SmaaInsertion` |
| M8.2 backend FSR 3 DX11 natif | codé, à tester (DLL séparée) | `Backend=1` |
| M8.3 vecteurs du jeu tels quels | codé, à tester | `GameMotionVectors`, `GameMvScale`, `GameMvFlipY` |
| M8.4 biais de LOD des textures | codé, à tester | `LodBias` |
| M10.2 provider FSR 4 du pilote | codé, à tester (DLL AMD à fournir) | `Fsr4Driver`, `Version` |
| M10.3 menu Version FSR (2 / 3 / 4) | codé, à tester | `Version` |

Exclus comme demandé : M6, M9. Non demandé : M8.5 (exposition du jeu).

## Résumé de la session du 27/10/2026

- **Défaut inchangé** : sans option, chemin Vulkan, insertion `0xB2A276FD`, FSR 3.1, comme en 0.6.3.
  Les nouveaux événements (draws, clears, samplers) ne font rien tant que leur option est coupée.
- **Compilation** : vérifiée commit par commit dans un dossier propre (sans le backend DX11, cas d'un
  clone neuf) ; l'addon n'importe toujours que `KERNEL32.dll`. Aucun test en jeu possible ici : tout est
  **à valider en jeu** selon TESTING.md.
- **M7.1** : touche choisie dans l'onglet (F5…F12, Fin, Page suiv.) ; force Off sans toucher au mode
  sauvegardé.
- **M7.2** : CS D3D11 compilés à l'exécution (`d3dcompiler_47` de Proton) — glslang n'est pas
  disponible dans le conteneur (téléchargement refusé), d'où ce choix pour toutes les nouvelles passes.
- **M8.1** : `SMAAEdgeDetection` / `SMAABlendingWeightCalculation` sautées, FSR écrit dans l'UAV de
  `SMAANeighborhoodBlending` ; profondeur prise sur la SRV1 du PS d'arêtes si son format convient. Repli
  automatique sur `0xB2A276FD` sans SMAA ; désarmement après un échec (copie entrée → sortie pour cette
  frame).
- **M8.2** : bibliothèques FidelityFX DX11 de Luma vérifiées : COFF standard (pas de LTCG), mais
  compilées en **/MD** (`FAILIFMISMATCH RuntimeLibrary=MD_DynamicRelease`) → incompatibles avec l'addon en
  CRT statique. Solution : DLL séparée `ak-fsr-dx11.dll` en CRT dynamique (msvcp140/vcruntime140 de Wine),
  interface C. `ffxGetResourceDX11` y est exportée avec une décoration C++ : reliée par
  `/alternatename`. Préparation (profondeur, reprojection) portée en HLSL ; état du jeu préservé par
  `SwapDeviceContextState`. Récupération : `scripts/fetch_ffx_dx11.sh` (commit Luma épinglé, SHA-256).
- **M8.3** : RT4 passée directement à FSR (annulation du jitter par FSR), échelle et sens de Y réglables
  faute de certitude (M5 lit des UV, Luma des valeurs [-1, 1]). D'après M5, RT4 est nulle sur le décor
  statique : des traînées en rotation de caméra sont probables → garder la reprojection si c'est le cas.
  Diagnostic ajouté : cible `R16G16_FLOAT` effacée (méthode de Luma) comparée à RT4.
- **M8.4** : `MipLODBias` ajouté aux samplers linéaires/anisotropes sans comparaison, à leur création ;
  effet au prochain lancement.
- **M10.2** : écrit à partir des seuls faits d'interface (§8.1 et dispositions de structures relevées lors
  de la session précédente) ; aucun code repris. Détour **générique** de `vkCreateDevice` dans winevulkan
  (petit décodeur de prologue ; refuse rip-relatif et branchements), ajout des seules extensions/fonctions
  prises en charge, nouvel essai sans elles si la création échoue. Réécriture SPIR-V des images float16
  testée en natif (`tests/spirv_half_image_test.cpp`). Repli sur FSR 3.1 avec message dans l'onglet.
- **M10.3** : entrée « 4.1.2 (provider pilote) » dans le menu, avec le modèle relevé dans les shaders du
  pilote (FP8 / INT8 / INT8-2 / INT8 Lite) ou le modèle attendu d'après le nom RADV du GPU.

## Blocages

- **M7.3 — RCAS après tonemap** : le RCAS intégré à FSR (`Sharpness`) couvre le besoin. Un RCAS
  après le tonemap (variante sans grain du mod DLAA) demande d'agir *après* le dispatch du tonemapper ;
  ReShade n'a pas d'événement « après dispatch », il faudrait remplacer le tonemapper ou s'accrocher à la
  passe suivante. Non fait ; le gain par rapport au RCAS intégré est faible.
- **M7.3 — Pluie après l'AA** : les draws de pluie ne sont pas identifiés (aucune capture sous la pluie).
  Il faut une capture F10 sous la pluie, mode Off, pour trouver leurs PS/VS et savoir s'ils sont dessinés
  avant `0xB2A276FD` (il faudrait alors les sauter puis les rejouer après FSR) ou après (rien à faire).
- **glslang indisponible** : nouvelles passes en HLSL compilé à l'exécution ; `shaders/fsr_prepare.comp`
  et son SPIR-V précompilé n'ont pas été modifiés.

## Points incertains à vérifier en priorité (M10.2)

- Signature exacte des fonctions du provider (déduite de l'API FFX publique) et disposition de la
  description de version `0x1000B` (`{en-tête, version u32}`, version 4.1.2 codée
  `(4 << 22) | (1 << 12) | 2`).
- sTypes de `VK_EXT_shader_float8` (1000567000) et `VK_NV_cooperative_matrix2` (1000593000), absents des
  en-têtes Vulkan 1.3.296 : déduits des numéros d'extension.
- Prologue de `vkCreateDevice` de winevulkan reconnu par le décodeur (sinon la ligne de log donne les octets).
- `radv_cooperative_matrix2_nv=true` doit être dans la ligne de lancement (lu par RADV avant l'addon).

## Prochaines étapes proposées

1. Tester dans l'ordre de TESTING.md sur PC, puis sur Deck (M8.1 + M8.2 en priorité pour le budget).
2. Me renvoyer `ReShade.log` et les lignes signalées « me transmettre ».
3. Selon les résultats : garder ou retirer M8.3 ; ajuster `LodBias` ; corriger M10.2 au besoin.
4. Capture F10 sous la pluie pour débloquer M7.3.
