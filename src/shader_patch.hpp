#pragma once

// M6 — patch du bytecode DXBC des shaders sensibles au jitter, à leur création (hash CRC32 d'origine).
// Principe : les passes qui reconstruisent la position à partir du pixel ignorent le jitter (projection
// sans M[2][0]/M[2][1]) alors que la profondeur a été rendue avec ; on fait lire au shader un décalage en
// pixels depuis un constant buffer injecté (slot kJitterCbSlot), rempli à chaque dispatch avec le jitter.
// Sans CB lié, DXVK lit des zéros : le shader patché se comporte alors comme l'original.

#include <cstdint>
#include <vector>

namespace akfsr::shader_patch {

constexpr uint32_t kJitterCbSlot = 12;  // libre dans tout le jeu (13 : injection RenoDX)

// Remplit `out` avec le bytecode patché si `hash` est dans la table et que le shader a la forme attendue.
bool patch(uint32_t hash, const uint8_t *code, size_t size, std::vector<uint8_t> &out);

// Le shader de ce hash lit le jitter en kJitterCbSlot (après patch réussi).
bool needs_jitter_cb(uint32_t hash);

}  // namespace akfsr::shader_patch
