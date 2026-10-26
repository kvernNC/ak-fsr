#pragma once

// FSR 3.1 (FidelityFX SDK v1.1.4, backend Vulkan) en AA natif au point d'insertion.
// Vecteurs de mouvement : caméra seule, par reprojection de la profondeur (CB View c0..c7, sans jitter).
// Jitter (M4) : appliqué par le détour de FPerspectiveMatrix, relu dans le CB View et transmis à FSR.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ID3D11Resource;

namespace akfsr::fsr {

// Début du CB View copié à chaque frame : c0..c21 (ViewProj courante et précédente, …, c21 = origine du
// repère translaté).
inline constexpr size_t kViewFloats = 88;
using view_data = std::array<float, kViewFloats>;

enum class prev_matrix : int { automatic = 0, cb = 1, memory = 2 };

struct settings {
  bool debug_view = false;   // vue de debug intégrée à FSR
  bool zero_motion = false;  // vecteurs nuls (diagnostic de la reprojection)
  float sharpness = 0.0f;    // RCAS intégré (0 = désactivé)
  bool auto_exposure = false;  // passe d'auto-exposition de FFX (recrée le contexte)
  uint64_t version_id = 0;     // version de l'upscaler (0 = défaut de la DLL ; recrée le contexte)
  bool jitter = true;           // jitter de la projection (M4)
  prev_matrix prev_source = prev_matrix::automatic;  // ViewProj précédente pour les vecteurs de mouvement
  bool object_velocity = true;  // vélocité des objets du G-buffer (RT4, M5)
};

struct frame_inputs {
  ID3D11Resource *color = nullptr;  // HDR r11g11b10, réécrite avec le résultat
  ID3D11Resource *depth = nullptr;  // D32_FLOAT_S8X24 (typeless)
  ID3D11Resource *velocity = nullptr;  // RT4 du G-buffer (r16g16_float, UV préc. - UV cour., jitterée)
  view_data view{};                 // CB View c0..c21 (voir kViewFloats)
};

bool run(const frame_inputs &in, const settings &s);

struct info {
  std::string status;
  std::string provider;                                 // version effectivement utilisée
  std::vector<std::pair<uint64_t, std::string>> versions;  // versions présentes dans la DLL
  float fov_deg = 0, near_plane = 0;  // déduits de la matrice, pour vérification
  float jitter_px_x = 0, jitter_px_y = 0;  // jitter relu dans le CB View (pixels), transmis à FSR
  float prev_mismatch = -1;                // écart max entre ViewProj précédente du CB et mémorisée (-1 : n/a)
  bool prev_from_memory = false;
};
info get_info();

}  // namespace akfsr::fsr
