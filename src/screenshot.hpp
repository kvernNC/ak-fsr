#pragma once

// Capture du back buffer en PNG (commande « screenshot » du pilotage par fichier) : image sans compression
// vidéo, analysable à distance. Formats gérés : RGBA8/BGRA8 (sRGB compris), RGB10A2, RGBA16F (écrêté).

#include <reshade.hpp>

#include <d3d11.h>

#include <filesystem>
#include <string>

namespace akfsr::screenshot {

// Écrit `path` (PNG RGB 8 bits) ; retourne un état lisible pour le log.
std::string capture(reshade::api::effect_runtime *runtime, const std::filesystem::path &path);

// Texture de diagnostic (copie de staging D3D11 déjà remplie) -> PNG RGB 8 bits. Formats flottants HDR : x/(1+x)
// puis gamma ; profondeur et R32 : 1 - valeur (lisible) ; R16G16 : |x|·32, |y|·32. Retourne un état pour le log.
std::string save_staging(ID3D11DeviceContext *ctx, ID3D11Texture2D *staging, const std::filesystem::path &path);

}  // namespace akfsr::screenshot
