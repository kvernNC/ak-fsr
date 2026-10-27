#pragma once

// Capture du back buffer en PNG (commande « screenshot » du pilotage par fichier) : image sans compression
// vidéo, analysable à distance. Formats gérés : RGBA8/BGRA8 (sRGB compris), RGB10A2, RGBA16F (écrêté).

#include <reshade.hpp>

#include <filesystem>
#include <string>

namespace akfsr::screenshot {

// Écrit `path` (PNG RGB 8 bits) ; retourne un état lisible pour le log.
std::string capture(reshade::api::effect_runtime *runtime, const std::filesystem::path &path);

}  // namespace akfsr::screenshot
