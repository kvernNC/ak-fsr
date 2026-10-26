#pragma once

// M2 — pont Vulkan via l'interop DXVK : on récupère le VkDevice/VkQueue de DXVK et on insère notre
// propre travail Vulkan dans l'ordre exact du flux D3D11 (flush DXVK, verrou de la file, vkQueueSubmit,
// déverrouillage). Interface publique ; le socle est dans vk_context.

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11Resource;

namespace akfsr::vk {

enum class test_mode : int {
  roundtrip = 0,   // couleur -> image intermédiaire -> couleur (image identique attendue)
  mirror_half = 1, // moitié gauche recopiée sur la droite (preuve visible de la réinjection)
};

// Initialisation paresseuse à partir du device D3D11 natif (DXVK). false si pas de DXVK.
bool init(ID3D11Device *native_device);
void shutdown();
bool ready();

// Exécute le test d'interop sur la texture.
bool run_test(ID3D11Resource *color, test_mode mode);

struct timings {
  double flush_cpu_ms = 0;  // coût CPU du flush + verrou + soumission (stall compris)
  double gpu_ms = 0;        // durée GPU de notre command buffer (timestamps, après la barrière d'entrée)
  double gpu_prepare_ms = 0;  // dont préparation (profondeur + vecteurs)
  double gpu_fsr_ms = 0;      // dont FSR
  uint64_t runs = 0;
  uint64_t failures = 0;
};
timings get_timings();
std::string status();  // dernier état / dernière erreur, pour l'overlay et le log

}  // namespace akfsr::vk
