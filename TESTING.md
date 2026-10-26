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
