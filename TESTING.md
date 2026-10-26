# AK-FSR — procédure de test de la branche `wip/m6-m10`

Toutes les nouveautés sont **désactivées par défaut** : sans toucher à `ReShade.ini`, l'addon se comporte
comme la v0.6.3 (Vulkan, insertion `0xB2A276FD`, FSR 3.1). Tester **une option à la fois**, dans l'ordre
ci-dessous, en repartant de la configuration par défaut entre deux tests.

Réglages : onglet **AK-FSR** de l'overlay, ou section `[AK-FSR]` de `ReShade.ini` (jeu fermé).
Lignes de log : `ReShade.log` dans `Binaries/Win64`, préfixe `AK-FSR:`.

## 0. Référence (rien activé)

- Lancer, charger une partie, la nuit en ville.
- Log attendu, comme en 0.6.3 : `addon loaded (v0.7.0, …)`, `interop Vulkan prête…`,
  `contexte FSR créé : …, version 3.1.4`, `passe insertion (0xB2A276FD) trouvée`.
- Noter les fps et les temps GPU de l'onglet (préparation / FSR) : ce sont les valeurs de comparaison.

## 1. M7.1 — Touche on/off

- Option : combo **« Touche on/off (M7) »** (ex. F9) ; `ReShade.ini` : `ToggleKey=120` (code VK, 0 = désactivée).
- Observer : un appui coupe AK-FSR (image aliasée, jitter arrêté), « AK-FSR coupé par la touche » s'affiche
  dans l'onglet ; un second appui le réactive. Le mode choisi dans l'onglet n'est pas modifié.
- Log : `AK-FSR: touche on/off : AK-FSR désactivé` / `… réactivé`.
- Après redémarrage du jeu : AK-FSR toujours actif (la bascule n'est pas sauvegardée), la touche oui.

## 2. M7.2 — Hautes lumières hors FSR

- Option : case **« Hautes lumières hors FSR (M7) »** + curseur « seuil » (défaut 8) ;
  `ReShade.ini` : `Highlights=1`, `HighlightThreshold=8.000000`.
- Principe : la part de chaque pixel au-dessus du seuil (composante max, couleur HDR linéaire) est retirée
  avant FSR et rajoutée après (CS D3D11 compilés à l'exécution par `d3dcompiler_47` de Proton).
- Observer : néons, phares, reflets de pluie intenses en mouvement → moins de traînées et de « fantômes »
  qu'avec l'option coupée. Inconvénient attendu : ces hautes lumières gardent l'aliasing / le jitter
  (léger frémissement des points très brillants). Faire varier le seuil (2 → 32).
- Coût : deux passes plein écran et deux copies ; comparer les fps avec la référence.
- Log : `AK-FSR: hautes lumières : actif` à la première frame.
  En cas d'échec : `hautes lumières : compilation des shaders : …`, `… sans UAV typée`, ou
  `… SRV sur la couleur du jeu impossible` → l'image reste celle de FSR sans l'option (rien de cassé).

## 3. M7.3 — RCAS, pluie : rien à tester (voir PROGRESS.md, « Blocages »)

- RCAS : déjà disponible via le curseur « Netteté (RCAS) » (`Sharpness`, RCAS intégré à FSR).
- Pluie composée après l'AA : non codée (passes de pluie non identifiées). Pour débloquer : sous la pluie,
  en ville, faire une capture **F10** avec AK-FSR en mode Off et me fournir le dossier `ak-fsr-dumps/…`.
