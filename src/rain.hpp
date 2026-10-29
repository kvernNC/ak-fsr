#pragma once

// M7.3 — effets transparents après FSR. Les éclaboussures de pluie (et, en option, tous les effets transparents
// dessinés sur l'image entre l'éclairage et le SMAA : particules, fumée, lueurs) changent d'une frame à l'autre et
// n'ont pas de vecteurs de mouvement : FSR les efface ou les étire en traînées. Ces draws sont redirigés vers une
// couche à part (RGB : couleur accumulée, A : transmission), composée sur la sortie de FSR :
// couleur = sortie * A + RGB. La couche, rendue avec la caméra jitterée, est lue décalée du jitter de la frame :
// les effets restent immobiles sur l'image stabilisée. Contexte immédiat natif D3D11.

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace akfsr::rain {

// Mélange du jeu pour le draw, dans la couche : sur (alpha / 1 - alpha) ou additif (dst = one).
enum class blend_mode { over, additive_one, additive_alpha };

struct draw_call {
  bool indexed = true;
  UINT count = 0, instances = 1, first = 0, first_instance = 0;
  INT vertex_offset = 0;
};

// Le pixel shader `ps_hash` dessine-t-il des éclaboussures ?
bool is_rain_shader(uint32_t ps_hash);
// Dessine le draw courant (état du jeu) dans la couche au lieu de la cible du jeu, si la cible 0 est
// `scene_color` (ou si `scene_color` vaut nullptr) et qu'aucune autre cible n'est liée. true : le draw a été
// exécuté ici, l'original doit être sauté.
bool redirect(ID3D11Device *dev, ID3D11DeviceContext *ctx, const draw_call &d, blend_mode mode,
              ID3D11Resource *scene_color);
// Compose la couche de la frame sur `color` (sortie de FSR), lue décalée de (jitter_x, jitter_y) pixels, puis la
// vide. Sans effet si rien n'a été redirigé.
void composite(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *color, float jitter_x, float jitter_y);
// Fin de frame : une couche non composée (FSR non exécuté) est abandonnée.
void end_frame();
void shutdown();
std::string status();

}  // namespace akfsr::rain
