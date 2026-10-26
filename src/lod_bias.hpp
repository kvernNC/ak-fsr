#pragma once

// M8.4 — biais de LOD des textures quand FSR est actif : ajouté au MipLODBias des samplers de textures
// (filtrage linéaire ou anisotrope, sans comparaison) à leur création. Les samplers étant créés au
// chargement, un changement ne s'applique qu'au prochain lancement du jeu.

#include <cstdint>

namespace akfsr::lod_bias {

// Biais lu au chargement des réglages (0 = désactivé) ; appliqué si le mode sauvegardé est FSR.
void configure(float bias, bool fsr_mode);
void register_events();
void unregister_events();

struct stats {
  float applied_bias = 0;  // biais en vigueur pour cette session
  uint32_t modified = 0, skipped = 0;
};
stats get_stats();

}  // namespace akfsr::lod_bias
