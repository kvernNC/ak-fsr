#pragma once

// Point d'insertion de l'AA : juste avant le CS 0xB2A276FD (préparation DoF, puis bloom), sur sa couleur
// d'entrée (t1, HDR r11g11b10, après brouillard) et sa profondeur (t0). Présent sur PC et sur Deck.
// Modes : tests d'interop de M2 (aller-retour / moitié dupliquée) et FSR (M3).

#include <reshade.hpp>

#include <array>

#include "fsr.hpp"

namespace akfsr::insertion {

enum class mode : int { off = 0, roundtrip = 1, mirror_half = 2, fsr = 3 };

void register_events();
void unregister_events();

// Appelé par passes.cpp sur le contexte immédiat, avant que le dispatch ne soit transmis.
void before_post_pass(reshade::api::command_list *cmd, reshade::api::resource scene_color,
                      reshade::api::resource depth, reshade::api::resource velocity,
                      const fsr::view_data *view_cb);

mode get_mode();
void set_mode(mode m);
fsr::settings get_fsr_settings();
void set_fsr_settings(const fsr::settings &s);
void set_shader_patches_enabled(bool enabled);  // + sauvegarde (voir passes::set_shader_patches_enabled)
// Les réglages (mode, options FSR, patchs) sont chargés depuis ReShade.ini [AK-FSR] et sauvegardés à
// chaque modification depuis l'overlay.

}  // namespace akfsr::insertion
