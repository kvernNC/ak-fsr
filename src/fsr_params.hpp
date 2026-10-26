#pragma once

// Paramètres de frame partagés par les backends FSR (Vulkan, DX11).

#include <cstdint>

namespace akfsr::fsr {

// Constantes du shader de préparation (GLSL fsr_prepare.comp / HLSL de fsr_dx11.cpp), même disposition.
struct push_constants {
  float reproj[16];  // colonne par colonne (mat4 GLSL, float4x4 column_major HLSL)
  float size[2];
  float inv_size[2];
  float jitter_delta_uv[2];  // jitter courant - précédent (UV), à ajouter à la vélocité des objets
  uint32_t zero_motion;
  uint32_t use_objects;
};
static_assert(sizeof(push_constants) == 96);

struct frame_params {
  push_constants pc{};
  float jitter_px_x = 0, jitter_px_y = 0;
  bool reset = true;
  float frame_ms = 16.6f;
  float near_plane = 10.0f, fov = 1.0f;
  bool objects = false;      // vélocité des objets (RT4) utilisée
  bool have_reproj = false;
  bool game_mv = false;  // M8.3 : RT4 utilisée directement comme vecteurs de mouvement
  float mv_scale_x = 0, mv_scale_y = 0;
  double start_ms = 0;
};

}  // namespace akfsr::fsr
