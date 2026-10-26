#pragma once

// M1 — détection à chaque frame des passes identifiées par les captures (hash CRC32 des CS).
// Tout se passe sur le contexte immédiat (aucun ExecuteCommandList observé en jeu).

#include <d3d11.h>
#include <reshade.hpp>

#include <array>
#include <cstdint>

namespace akfsr::passes {

// Point d'insertion, présent sur PC et Deck : t0 profondeur, t1 couleur HDR de scène (après brouillard).
constexpr uint32_t kInsertionCs = 0xB2A276FD;
constexpr uint32_t kFogCs = 0x42C0137E;        // brouillard / diffusion : absent avec les réglages du Deck
constexpr uint32_t kVelocityCs = 0xF98BC308;   // t0 profondeur, t1 vélocité objets -> u0 vélocité combinée
constexpr std::array<uint32_t, 4> kTonemapCs = {0x978BFB09, 0xB6B56605, 0xF01CCC7E, 0x3A4E0B90};
constexpr uint64_t kViewCbSize = 608;          // CB « View » UE3 en b1 des passes post

struct found_resource {
  uint64_t handle = 0;
  reshade::api::resource_desc desc{};
};

struct pass_status {
  bool found = false;           // vue dans la dernière frame
  uint64_t frames_found = 0;
  uint64_t frames_missed = 0;   // frames sans la passe depuis la première détection
  uint32_t hash = 0;            // hash effectivement vu (tonemapper : variante)
};

struct frame_report {
  pass_status insertion, fog, velocity, view_cb, tonemap;
  found_resource scene_color, insertion_depth, depth, velocity_objects, velocity_combined, view_cb_buffer;
  std::array<float, 32> view_cb_head{};  // c0..c7 du CB View (dernier contenu écrit)
  bool view_cb_valid = false;
  uint64_t frames = 0;
};

void register_events();
void unregister_events();
frame_report get_report();

// M6 : correction du jitter dans les shaders patchés (sinon décalage nul = shader d'origine).
void set_shader_patches_enabled(bool enabled);
bool shader_patches_enabled();

}  // namespace akfsr::passes
