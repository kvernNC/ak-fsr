#pragma once

// Outil de diagnostic (M7.3, pluie) : liste les pixel shaders des draws de la frame, dans l'ordre de
// première apparition, situés avant ou après le point d'insertion (0xB2A276FD), et permet d'en masquer
// (draws sautés) pour les reconnaître à l'écran. Inactif tant que l'outil n'est pas ouvert dans l'onglet.

#include <cstdint>
#include <vector>

namespace akfsr::explorer {

void set_enabled(bool enabled);
bool enabled();

// Appelés par passes.cpp sur le contexte immédiat.
bool on_draw(uint32_t ps_hash, uint32_t vertices, bool indirect = false);  // true : draw à sauter (hash 0 = inconnu)
bool on_dispatch(uint32_t cs_hash);  // compute shaders : true = dispatch à sauter
void on_deferred_draw();             // draw d'un contexte différé (compté seulement)
void on_insertion();                                 // passage du point d'insertion dans la frame
void on_present();

struct entry {
  char kind = 'P';  // 'P' pixel shader, 'C' compute shader, 'D' draws des contextes différés
  uint32_t hash = 0;
  uint32_t draws = 0, vertices = 0;
  uint32_t indirect = 0;  // dont draws indirects (nombre de sommets inconnu : particules GPU…)
  bool after_insertion = false;  // première apparition après 0xB2A276FD
  bool hidden = false;
};
std::vector<entry> get_entries();  // dernière frame complète
void set_hidden(uint32_t hash, bool hidden);
void clear_hidden();

}  // namespace akfsr::explorer
