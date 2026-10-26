#pragma once

// M4 — jitter côté CPU : détour de FPerspectiveMatrix (UE3) dans BatmanAK.exe, trouvée par motif
// d'octets. Le décalage sous-pixel (séquence de Halton) n'est ajouté qu'aux appelants qui construisent
// la vue principale, appris en comparant leur matrice à celle du CB View.

#include <cstdint>
#include <string>
#include <vector>

namespace akfsr::jitter {

bool install();  // recherche du motif + détour ; résultat dans status()
void uninstall();

void set_enabled(bool enabled);                       // jitter actif (mode FSR + option)
void set_render_size(uint32_t width, uint32_t height);  // conversion pixels -> NDC
void on_present();                                    // avance dans la séquence

// Apprentissage de la vue principale : projection déduite du CB View de la frame rendue.
void learn_main_view(float scale_x, float scale_y, float near_plane);

// Jitter (NDC) lu dans les lignes c0..c2 de la ViewProj : cᵢ.w porte la colonne « profondeur vue » de la
// rotation, donc jx = Σ cᵢ.w·cᵢ.x / Σ cᵢ.w² (de même pour y).
void recover_from_view(const float *view_rows, float &jx_ndc, float &jy_ndc);

struct caller {
  uint32_t rva = 0;  // adresse de retour, relative au module
  uint64_t calls = 0;
  float m00 = 0, m11 = 0, min_z = 0;
  bool main_view = false;
};

struct status {
  bool installed = false;
  std::string message;
  uint32_t function_rva = 0;
  uint32_t phase = 0, phase_count = 0;
  float applied_x_px = 0, applied_y_px = 0;  // dernier jitter appliqué par le détour
  std::vector<caller> callers;
};
status get_status();

}  // namespace akfsr::jitter
