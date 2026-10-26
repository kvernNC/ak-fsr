# AK-FSR — journal de la branche `wip/m6-m10`

Branche de travail autonome (pas de push). Un commit par sous-étape ; procédure de test dans
[TESTING.md](TESTING.md).

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

## Blocages

- **M7.3 — RCAS après tonemap** : le RCAS intégré à FSR (`Sharpness`) couvre le besoin. Un RCAS
  après le tonemap (variante sans grain du mod DLAA) demande d'agir *après* le dispatch du tonemapper ;
  ReShade n'a pas d'événement « après dispatch », il faudrait remplacer le tonemapper ou s'accrocher à la
  passe suivante. Non fait ; le gain par rapport au RCAS intégré est faible.
- **M7.3 — Pluie après l'AA** : les draws de pluie ne sont pas identifiés (aucune capture sous la pluie).
  Il faut une capture F10 sous la pluie, mode Off, pour trouver leurs PS/VS et savoir s'ils sont dessinés
  avant `0xB2A276FD` (il faudrait alors les sauter puis les rejouer après FSR) ou après (rien à faire).
