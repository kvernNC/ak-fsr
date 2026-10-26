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

## 4. M8.1 — FSR à la place du SMAA du jeu

- Prérequis : **AA du jeu sur « Élevé »** (SMAA) dans les options graphiques.
- Option : case **« Insertion au SMAA du jeu (M8.1…) »** ; `ReShade.ini` : `SmaaInsertion=1`.
- Principe : `SMAAEdgeDetection` (PS `0x067FF80F`) et `SMAABlendingWeightCalculation` (PS `0x2F55F98E`)
  sont sautées ; FSR remplace `SMAANeighborhoodBlending` (CS `0x42C0137E`, t0 → u0). Sans SMAA dans la
  frame (AA du jeu coupé), FSR tourne comme avant à `0xB2A276FD`.
- Observer dans l'onglet : ligne « SMAA du jeu » `[OK]`, puis « insertion au SMAA (M8.1) : FSR remplace le
  SMAA, N frames » (N augmente). Image : même qualité que la référence (pas de double AA, pas de flou en plus).
  Comparer les fps avec : (a) référence AA du jeu coupé, (b) référence AA du jeu « Élevé » sans l'option.
  Gain attendu : le coût du SMAA du jeu.
- Tester aussi : couper l'AA du jeu en cours de partie → l'état passe à « repli sur 0xB2A276FD »,
  l'image reste anti-aliasée.
- Log attendu (une fois) : `AK-FSR: passe SMAA du jeu (0x42C0137E, optionnelle) trouvée`, puis
  `AK-FSR: insertion au SMAA : couleur <fmt> WxH -> sortie <fmt>, profondeur de SMAAEdgeDetection`.
  **Me transmettre cette ligne** (formats réels). Si `profondeur de l'insertion 0xB2A276FD` : la SRV1 du
  PS n'a pas le format attendu, c'est sans gravité.
- En cas d'échec : `insertion au SMAA en échec, repli sur 0xB2A276FD (option désarmée)` précédé d'une
  ligne `FSR : …` (ex. `sortie incompatible avec l'entrée (format X / Y)`). L'onglet affiche « désarmée » ;
  décocher/recocher l'option pour réessayer.

## 5. M8.2 — Backend FSR 3 DX11 natif

- Installation : copier aussi **`ak-fsr-dx11.dll`** à côté de l'addon (`scripts/deploy.sh` le fait).
  Elle dépend de `msvcp140.dll` / `vcruntime140.dll` / `vcruntime140_1.dll` (fournies par Wine/Proton ;
  sinon `protontricks 1690000 vcrun2022`).
- Option : **« Backend FSR : DX11 natif (M8.2) »** ; `ReShade.ini` : `Backend=1` (0 = Vulkan, défaut).
- Principe : FSR 3 (FidelityFX, backend DX11 précompilé par Luma) appelé directement sur le contexte D3D11
  du jeu : ni interop Vulkan, ni copies vers Vulkan, ni `FlushRenderingCommands`. L'état D3D11 du jeu est
  mis de côté pendant le travail (`SwapDeviceContextState`).
- Observer : onglet « Version utilisée : 3.1 (backend DX11 natif) », « Backend DX11 : actif | CPU x ms ».
  Image équivalente au backend Vulkan (comparer à l'arrêt et en mouvement, avec et sans jitter).
  Les temps GPU « préparation / FSR » de l'onglet ne concernent que le backend Vulkan : comparer les **fps**
  (et l'overlay de performance du Deck) entre `Backend=0` et `Backend=1`, au même endroit.
  Critère : coût inférieur au chemin Vulkan sur le Deck.
- Log attendu : `backend DX11 : ak-fsr-dx11.dll chargée`, puis
  `backend DX11 : contexte FSR 3 créé : WxH (AA natif), format DXGI 26`.
- Échecs possibles (image sans AA, rien de cassé ; repasser à `Backend=0`) :
  `ak-fsr-dx11.dll introuvable … (erreur 126)` → DLL absente ou dépendance VC++ manquante ;
  `FSR : backend DX11 : …` (SRV sur la profondeur, format sans UAV typée, code FFX) → me transmettre la ligne.
- Combinable avec M8.1 (`SmaaInsertion=1`) et M7.2.

## 6. M8.3 — Vecteurs de mouvement du jeu tels quels

- Option : case **« Vecteurs du jeu tels quels (M8.3) »** + échelle « UV » / « [-1,1] » + « Y inversé » ;
  `ReShade.ini` : `GameMotionVectors=1`, `GameMvScale=0|1`, `GameMvFlipY=0|1`.
- Principe : la vélocité du G-buffer (RT4, `R16G16_FLOAT`, jitterée) est donnée directement à FSR
  (annulation du jitter par FSR), sans notre reprojection de la profondeur. Fonctionne avec les deux backends.
- Diagnostic d'abord : ligne **« R16G16F effacée (M8.3) »** de l'onglet. Si elle affiche « = RT4 », la cible
  qu'utilise Luma est bien notre RT4. Log : `cible R16G16_FLOAT effacée : WxH, …` (la première ligne peut
  dire « autre que RT4 » avant la première détection de RT4 : se fier à l'onglet).
- Réglage de l'échelle, caméra en rotation lente devant des grilles/toits, à l'arrêt du personnage :
  1. `UV`, Y normal (lecture de M5) ; 2. `[-1,1]` (lecture de Luma) ; 3. chacun avec « Y inversé ».
  Bon réglage : pas de dédoublement ni de traînée sur Batman en mouvement ; une mauvaise échelle donne des
  objets qui « fondent » ou laissent une traînée dans le mauvais sens.
- **Attendu probable** : d'après M5, RT4 est nulle sur le décor statique (seuls les objets mobiles écrivent
  leur vélocité). Si c'est le cas, l'image traîne dès que la caméra tourne : le critère du plan n'est pas
  rempli et on garde la reprojection (option coupée). Le noter dans le retour.
- Log : aucune ligne propre ; `contexte FSR créé …` réapparaît quand on coche/décoche (contexte recréé).

## 7. M8.4 — Biais de LOD des textures

- Option : curseur **« Biais de LOD (M8.4, au prochain lancement) »** ; `ReShade.ini` : `LodBias=-0.500000`
  (0 = désactivé). **Redémarrer le jeu** après changement (les samplers sont créés au chargement) ; le biais
  n'est appliqué que si le mode sauvegardé est FSR.
- Valeurs à essayer : `-0.5`, puis `-1.0` (valeur d'AMD pour l'AA natif : log2(1) − 1).
- Observer : textures au sol et façades lointaines plus nettes, sans grésillement supplémentaire en mouvement
  (comparer avec `LodBias=0` au même endroit, mêmes réglages). Trop négatif → moiré qui scintille.
- Onglet : « en vigueur : -0.50, N samplers modifiés, M laissés » (N > 0 attendu en jeu).
- Log : `AK-FSR: biais de LOD des textures : -0.50 (samplers créés à partir de maintenant)` au chargement.
