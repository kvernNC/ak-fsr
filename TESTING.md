# AK-FSR — procédure de test de la branche `wip/m6-m10`

**Prérequis : pas d'OptiScaler** (ReShade seul en `dxgi.dll`) ; il intercepte FidelityFX et Vulkan et fait planter
le jeu avec certaines options.

Toutes les nouveautés sont **désactivées par défaut** : sans toucher à `ReShade.ini`, l'addon se comporte
comme la v0.6.3 (Vulkan, insertion `0xB2A276FD`, FSR 3.1). Tester **une option à la fois**, dans l'ordre
ci-dessous, en repartant de la configuration par défaut entre deux tests.

Réglages : onglet **AK-FSR** de l'overlay, ou section `[AK-FSR]` de `ReShade.ini` (jeu fermé).
Raccourci : combo **« Profil de test (TESTING.md) »** de l'onglet — applique l'étape choisie (mode FSR,
une seule option active, autres réglages par défaut ; netteté, auto-exposition, vue de debug et touche on/off conservées).
Les profils marqués « relancer » demandent de redémarrer le jeu. Revenir à « 0. Référence » entre deux étapes.

Sans overlay (petite fenêtre, prise en main à distance) :
- **Alt + Impr. écran** : profil de test suivant (dans l'ordre du combo, puis retour à « 0. Référence ») ;
  le log note `AK-FSR: profil de test : <nom>`.
- **Ctrl + Impr. écran** : rapport de test ajouté à `ak-fsr-report.txt` et au log.
- Touches modifiables : `[AK-FSR] ReportKey=44`, `ReportModifier=17` (Ctrl), `ProfileKey=44`,
  `ProfileModifier=18` (Alt) ; codes VK, touche 0 = désactivée, modificateur 0 = aucun.
- Pilotage par fichier (sans clavier) : `ak-fsr-command.txt` à côté de l'addon, lu deux fois par seconde puis
  supprimé ; une commande par ligne : `report`, `profile <n>` (0 = référence) ou `profile next`, `toggle`,
  `screenshot <nom> [n]` (PNG sans compression dans `ak-fsr-shots/`, n frames consécutives ≤ 16).
  `shaders` (liste des PS de la frame dans `ak-fsr-shaders.txt`),
  `hide <hash>`, `show <hash>|all` (masquer / rétablir un PS, pour la pluie M7.3).
  `compare <hash> …` : pour chaque PS, captures `<hash>_on` / `<hash>_off` sur deux frames consécutives.
  Le log note `AK-FSR: commande : …`.
- Le profil actif est sauvegardé : avant de quitter, revenir à « 0. Référence » (sinon un profil « relancer »
  s'applique au lancement suivant).
Lignes de log : `ReShade.log` dans `Binaries/Win64`, préfixe `AK-FSR:`.

## 0. Référence (rien activé)

- Lancer, charger une partie, la nuit en ville.
- Log attendu, comme en 0.6.3 : `addon loaded (v0.7.0, …)`, `interop Vulkan prête…`,
  `contexte FSR créé : …, version 3.1.4`, `passe insertion (0xB2A276FD) trouvée`.
- Noter les fps et les temps GPU de l'onglet (préparation / FSR) : ce sont les valeurs de comparaison.
- Nouvelles lignes de diagnostic (sans effet, à la création de la swapchain) — **me les transmettre** :
  `diagnostic : amd_fidelityfx_vk.dll oui, ak-fsr-dx11.dll …, d3dcompiler_47 …`,
  `diagnostic : ak-fsr\ amdvkffx64.dll …`, et surtout
  `diagnostic : vkCreateDevice de winevulkan.dll : XX XX … -> détour possible|REFUSÉ (N octets)` : dit à
  l'avance si le détour de M10.2 pourra s'installer, sans activer l'option.

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

- Prérequis : **AA du jeu sur « On »** (SMAA) dans les options graphiques.
- Option : case **« Insertion au SMAA du jeu (M8.1…) »** ; `ReShade.ini` : `SmaaInsertion=1`.
- Principe : `SMAAEdgeDetection` (PS `0x067FF80F`) et `SMAABlendingWeightCalculation` (PS `0x2F55F98E`)
  sont sautées ; FSR remplace `SMAANeighborhoodBlending` (CS `0x42C0137E`, t0 → u0). Sans SMAA dans la
  frame (AA du jeu coupé), FSR tourne comme avant à `0xB2A276FD`.
- Observer dans l'onglet : ligne « SMAA du jeu » `[OK]`, puis « insertion au SMAA (M8.1) : FSR remplace le
  SMAA, N frames » (N augmente). Image : même qualité que la référence (pas de double AA, pas de flou en plus).
  Comparer les fps avec : (a) référence AA du jeu coupé, (b) référence AA du jeu « On » sans l'option.
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
  sauvegardé puis restauré (étage compute) autour du travail.
- Observer : onglet « Version utilisée : 3.1 (backend DX11 natif) », « Backend DX11 : actif | CPU x ms ».
  Image équivalente au backend Vulkan (comparer à l'arrêt et en mouvement, avec et sans jitter).
  Comparer au même endroit, caméra immobile :
  - `Backend=0` : ligne « chemin Vulkan complet vu par D3D11 (copies + FSR) : X ms » ;
  - `Backend=1` : ligne « GPU : préparation … | FSR … | copie … | total Y ms ».
  Ces deux totaux sont comparables (mêmes requêtes timestamp D3D11 ; le total Vulkan inclut aussi l'attente
  éventuelle du GPU pendant le flush, c'est un majorant). Relever aussi les **fps** et l'overlay du Deck.
  Critère : Y < X sur le Deck.
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

## 8. M10.2 — FSR 4 par le provider du pilote AMD

- Fichiers (jamais dans le dépôt), dans **`Binaries/Win64/ak-fsr/`** :
  - `amdvkffx64.dll` du pilote Adrenalin 26.10.1 (archive extraite avec 7-Zip, §8.1 de PLAN.md) ;
  - `amd_fidelityfx_loader_dx12.dll` et `amd_fidelityfx_upscaler_dx12.dll` du FSR SDK 2.3
    (`Kits/FidelityFX/signedbin/`), **renommées** `amd_fidelityfx_loader_vk.dll` et
    `amd_fidelityfx_upscaler_vk.dll` : `scripts/fetch_ffx_signed.sh` (dépôt officiel AMD, MIT, empreintes
    vérifiées) puis `scripts/deploy.sh`, qui copie aussi `amdvkffx64.dll` depuis `Binaries/Win64/` si elle s'y trouve.
- PC (RDNA 4, modèle FP8) : ajouter `radv_cooperative_matrix2_nv=true` à la ligne de lancement Steam
  (lu par RADV à l'initialisation, avant le chargement de l'addon).
- Option : case **« FSR 4 pilote (M10.2, au prochain lancement) »** ; `ReShade.ini` : `Fsr4Driver=1`.
  **Relancer le jeu** (le détour de `vkCreateDevice` s'installe au chargement de l'addon).
  Puis choisir la version : menu « Version FSR » (voir M10.3) ou `Version=17700776142811697154`
  (= `0xF5A5CA1E81001002`).
- Logs attendus, dans l'ordre :
  1. `FSR 4 pilote : détour de vkCreateDevice installé, en attente du device de DXVK`
     (échec possible : `détour de vkCreateDevice refusé : prologue non reconnu à +N : XX XX …` → **me
     transmettre la ligne** : il faut apprendre ce prologue au décodeur) ;
  2. `FSR 4 pilote : device DXVK créé avec N extension(s) ajoutée(s) : VK_KHR_cooperative_matrix, … ;
     absentes du GPU : …` (si `vkCreateDevice en échec avec les ajouts`, le jeu démarre quand même sans
     elles) ;
  3. `FSR 4 pilote : UpdateFfxApiProviderEx : hr 0x00000000, id 0xf5a5ca1e81001002, nom « … », 2 DLL signée(s)` ;
  4. `contexte FSR créé : …, version 4.1.2 (provider pilote), modèle attendu FP8 (RDNA 4)` ;
  5. éventuellement `FSR 4 pilote : modèle relevé dans les shaders : fsr4_model_…`.
- Observer : image FSR 4 (plus stable que 3.1 sur les grilles et la pluie), pas de crash en sortie de vidéo.
  Onglet « Version utilisée : 4.1.2 (provider pilote), modèle attendu … ».
- Repli attendu en cas d'échec : `FSR 4 pilote indisponible (…), repli sur FSR 3.1` dans le log et en
  orange dans l'onglet ; l'image reste celle de FSR 3.1. Raisons possibles : DLL introuvables, `le pilote n'a
  pas donné de provider (DLL signées du SDK manquantes ?)`, `identifiant inattendu …` (autre version de
  `amdvkffx64.dll`), `création du contexte : code N`.
- Si le jeu plante au premier dispatch FSR 4 : relancer avec `Fsr4Driver=0`, me fournir `ReShade.log` et,
  si possible, `PROTON_LOG=1`. Lignes utiles : `réécriture SPIR-V impossible : …`.
- Deck (M10.1/M10.2) : même installation, sans `radv_cooperative_matrix2_nv`. Relever les extensions
  « absentes du GPU », le nom de modèle relevé, et si le contexte se crée.

## 9. M10.3 — Menu « Version FSR » (2.3.3 / 3.1.4 / 4.1.2)

- Prérequis : M10.2 installé, `Fsr4Driver=1`, backend Vulkan (`Backend=0` ; le menu est masqué en DX11).
- Menu **« Version FSR »** : `défaut`, `3.1.4`, `2.3.3` (amd_fidelityfx_vk.dll), et
  `4.1.2 (provider pilote)` suivi du modèle (`- FP8`, `- INT8`, `- INT8-2`, `- INT8 Lite`) dès qu'il est
  relevé dans les shaders du pilote, ou `- indisponible` après un échec.
- Observer : bascule 2.3.3 → 3.1.4 → 4.1.2 → 3.1.4 en jeu, **sans crash** (contexte recréé à chaque fois,
  log `création du contexte FSR (version demandée …)` puis `contexte FSR créé …`). Mesurer les fps de
  chaque version au même endroit (PC et Deck). Ligne « FSR 4 pilote : contexte créé (« nom »), modèle
  utilisé : …, shaders N dont M réécrits, 0 échecs ».
- Si 4.1.2 échoue : ligne orange « FSR 4 pilote indisponible, repli sur FSR 3.1 : … » ; le menu reste
  utilisable pour revenir à 3.1.4.

## Retour de test : bouton « Rapport de test »

En bas de l'onglet, **« Rapport de test (fichier + presse-papiers) »** : copie dans le presse-papiers et ajoute à
`Binaries/Win64/ak-fsr-report.txt` (et au log) tous les réglages, passes, ressources, états des options et
mesures (CPU/GPU des deux backends). Le faire **après chaque test**, au même endroit du jeu, et me coller le
fichier avec `ReShade.log`.

## Outil : explorateur de pixel shaders (pour débloquer M7.3, pluie)

- Onglet → nœud **« Explorateur de pixel shaders (M7.3, pluie) »** (actif seulement nœud ouvert).
- Sous la pluie, en ville : la table liste les PS de la frame (ordre, « avant » / « après insertion »,
  nombre de draws et de sommets). Cocher « masquer » sur les PS candidats (souvent beaucoup de sommets,
  ou des draws « après insertion ») jusqu'à faire disparaître la pluie ; on peut fermer l'overlay pour mieux
  voir, le masquage reste actif tant que le nœud est ouvert.
- Me noter le ou les hash qui font disparaître la pluie, et s'ils sont « avant » ou « après insertion ».
  Faire aussi un rapport de test et une capture F10 au même moment.

## 10. M8.5 — Exposition du jeu (à la place de l'auto-exposition de FSR)

> **Sans OptiScaler** : avec lui, couper l'option fait planter le jeu (voir PROGRESS.md).

- Diagnostic d'abord, option coupée : ligne **« exposition 1x1 (M8.5) »** de l'onglet et log
  `tonemap 0x…: texture 1x1 <format> en tN (exposition du jeu ?)`. Si rien n'apparaît, le tonemapper ne lit
  pas de texture 1×1 : M8.5 est sans objet.
- Option : case **« Exposition du jeu (M8.5) »**, interprétation (telle quelle / inverse / exp2(x) /
  exp2(-x)) et « échelle » ; `ReShade.ini` : `GameExposure=1`, `ExposureMode=0..3`, `ExposureScale=1.0`.
  L'auto-exposition de FFX est alors coupée d'office.
- Observer avec la **vue de debug FSR** et en passant d'une rue sombre à une zone éclairée : la bonne
  interprétation ne change ni la luminosité ni la netteté par rapport à l'auto-exposition, et réduit les
  traînées des zones très sombres ou très claires. Une mauvaise donne un rendu plus flou ou fantomatique
  (FSR croit l'image sur- ou sous-exposée). Comparer aussi les temps GPU (l'auto-exposition coûte une passe).
- Log : `exposition du jeu : actif` ; échecs possibles : `texture 1×1 du tonemapper pas encore repérée`,
  `SRV sur la texture d'exposition du jeu impossible`, `image partagée 1x1 impossible`.
