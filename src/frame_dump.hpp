#pragma once

// M1 — cartographie : suivi des shaders (hash CRC32 du bytecode d'origine) et capture d'une frame
// complète (draws/dispatches/copies/clears dans l'ordre d'exécution, avec cibles, SRV/UAV/CB).

#include <reshade.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace akfsr::dump {

std::string format_string(reshade::api::format f);

void register_events(const std::filesystem::path &output_root);
void unregister_events();

// Arme une capture : chauffe d'une frame (deferred contexts), puis capture de la suivante.
void request_capture();

// Écrit l'image mémoire de BatmanAK.exe (+ description des sections) dans le dossier des dumps.
std::string dump_game_image();

// Hash CRC32 du shader d'origine derrière un handle de pipeline D3D11 (0 si inconnu).
uint32_t shader_hash(uint64_t pipeline_handle);

// Mélange de la cible 0 d'un état de mélange D3D11 (objet pipeline de ReShade). Valeurs des énumérations ReShade.
struct blend_rt0 {
  bool enable = false;
  int src = 1, dst = 0, op = 0;  // one, zero, add
  uint32_t mask = 0xF;
};
// false : état inconnu (mélange par défaut, pas d'objet lié).
bool blend_of(uint64_t pipeline_handle, blend_rt0 &out);

struct stats {
  uint32_t draws = 0;       // dernière frame complète
  uint32_t dispatches = 0;
  uint32_t buffer_maps = 0; // > 0 : les événements map/unmap arrivent (build ReShade addon complet)
  size_t shaders_known = 0;
  bool capture_busy = false;
  std::string last_capture;  // dossier de la dernière capture, ou message d'erreur
};
stats get_stats();

}  // namespace akfsr::dump
