#pragma once

// Outils de développement (voir dev_tools.cpp), compilés seulement avec AKFSR_DEV_TOOLS=1.

#include <reshade.hpp>

#include <filesystem>

#include "fsr.hpp"

namespace akfsr::dev {

void init(const std::filesystem::path &addon_dir);  // DllMain : touches du rapport et des profils
void set_swapchain(uint32_t width, uint32_t height, reshade::api::format format);
void on_present(reshade::api::effect_runtime *runtime);  // raccourcis, pilotage par fichier, captures

// Sections de l'onglet AK-FSR.
void draw_passes();
void draw_mode_and_profiles();
bool draw_options(fsr::settings &s);  // true si un réglage a changé
void draw_diagnostics();

}  // namespace akfsr::dev
