#pragma once

// M8.2 — FSR 3 avec le backend DX11 natif de FidelityFX (ak-fsr-dx11.dll, à côté de l'addon) : préparation
// (profondeur R32F, vecteurs de mouvement) en CS D3D11, puis FSR, sur le contexte immédiat, sans Vulkan.
// L'état compute du jeu est sauvegardé puis restauré autour de ce travail.

#include <d3d11.h>

#include <string>

#include "fsr.hpp"
#include "fsr_params.hpp"

namespace akfsr::fsr::dx11 {

bool run(const frame_inputs &in, const settings &s, const frame_params &p, const D3D11_TEXTURE2D_DESC &color,
         std::string &error);
void shutdown();

// ak-fsr-dx11.dll présente et valide (chargée au premier appel).
bool available();

// Durées GPU mesurées par requêtes timestamp D3D11 (résultats lus avec quelques frames de retard).
struct gpu_timings {
  double prepare_ms = 0, fsr_ms = 0, copy_ms = 0;  // préparation, FSR, copie du résultat
  bool valid = false;
};
gpu_timings get_gpu_timings();

}  // namespace akfsr::fsr::dx11
