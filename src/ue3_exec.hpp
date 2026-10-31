#pragma once

// Commandes du moteur UE3 depuis l'addon (outils de développement) : la console de BatmanAK.exe n'a plus de fenêtre,
// mais ses gestionnaires de commandes sont toujours dans l'exécutable. On appelle USystemSettings::Exec (commandes
// « scale … » : réglages du moteur, dont ScreenPercentage) sur GSystemSettings, avec un FOutputDevice à nous qui
// recopie les réponses du moteur dans ReShade.log.
//
// Localisation au premier usage, sans adresse fixe (Steam / GOG) : chaîne UTF-16 « SCALE » -> unique référence
// (lea rip) dans .text -> fonction qui la contient (table d'exceptions) -> unique appel de cette fonction, précédé de
// `lea rcx, [GSystemSettings]`. Relevé sur BatmanAK.exe Steam 1.6.2.0 : Exec RVA 0xC7AA30, GSystemSettings RVA
// 0x3123740, appelant UEngine::Exec RVA 0xBA6840. FOutputDevice : Serialize(texte, catégorie) en entrée 1 de la vtable.
//
// Fil d'exécution : ces commandes recréent des ressources et attendent le thread de rendu ; elles doivent tourner
// sur le thread de jeu (celui de la fenêtre dans UE3), jamais sur celui du present. Les commandes sont donc postées
// à la fenêtre du jeu (sous-classement de sa procédure) et exécutées dans sa boucle de messages.

#include <string>

struct HWND__;

namespace akfsr::ue3 {

// Met la commande en file et la poste à la fenêtre `hwnd` ; résultat et réponses du moteur dans le log.
void post(HWND__ *hwnd, const std::string &command);

// Diagnostic sans effet : adresses trouvées (ou raison de l'échec), écrit dans le log.
void describe();

}  // namespace akfsr::ue3
