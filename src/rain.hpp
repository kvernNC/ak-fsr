#pragma once

// M7.3 — éclaboussures de pluie après FSR. Le jeu les dessine juste avant le SMAA : deux draws instanciés
// (des milliers de petits quads) en mélange alpha / 1 - alpha. Elles changent de place à chaque frame : FSR les
// prend pour du bruit, les efface et fait gigoter la texture du sol en dessous. Ces draws sont redirigés vers
// une couche à part (RGB : couleur accumulée, A : transmission), composée sur la sortie de FSR :
// couleur = sortie * A + RGB. Contexte immédiat natif D3D11.

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace akfsr::rain {

// Le pixel shader `ps_hash` dessine-t-il des éclaboussures ?
bool is_rain_shader(uint32_t ps_hash);
// Dessine le draw courant (état du jeu) dans la couche de pluie au lieu de la cible du jeu. true : le draw a
// été exécuté ici, l'original doit être sauté.
bool redirect(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT indices, UINT instances, UINT first_index,
              INT vertex_offset, UINT first_instance);
// Compose la couche de la frame sur `color` (sortie de FSR) puis la vide. Sans effet si rien n'a été redirigé.
void composite(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *color);
// Fin de frame : une couche non composée (FSR non exécuté) est abandonnée.
void end_frame();
void shutdown();
std::string status();

}  // namespace akfsr::rain
