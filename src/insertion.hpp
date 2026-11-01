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

// Appelé par passes.cpp sur le contexte immédiat, avant que le dispatch ne soit transmis. `output` :
// destination du résultat (0 : en place dans scene_color). true si FSR a écrit son résultat.
bool before_post_pass(reshade::api::command_list *cmd, reshade::api::resource scene_color,
                      reshade::api::resource depth, reshade::api::resource velocity,
                      const fsr::view_data *view_cb, reshade::api::resource output = {0});

// Mode FSR effectif (ni Off, ni coupé par la touche) : les passes du jeu peuvent être remplacées.
bool fsr_active();

mode get_mode();
void set_mode(mode m);
fsr::settings get_fsr_settings();
void set_fsr_settings(const fsr::settings &s);
void set_shader_patches_enabled(bool enabled);  // + sauvegarde (voir passes::set_shader_patches_enabled)

// M7 : touche on/off (comparaison en jeu). 0 = désactivée (défaut). La bascule n'est pas sauvegardée :
// elle force temporairement le mode Off, le mode choisi dans l'onglet est conservé.
uint32_t get_toggle_key();
void set_toggle_key(uint32_t vk);
void toggle_bypass();
bool bypassed();
// Les réglages (mode, options FSR, patchs) sont chargés depuis ReShade.ini [AK-FSR] et sauvegardés à
// chaque modification depuis l'overlay.

}  // namespace akfsr::insertion
