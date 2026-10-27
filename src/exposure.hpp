#pragma once

// M8.5 — exposition du jeu pour FSR (à la place de son auto-exposition) : la texture 1×1 lue par le
// tonemapper (repérée par passes.cpp, valeur de la frame précédente au point d'insertion) est convertie
// par un CS D3D11 en texture R32F 1×1 « multiplicateur d'exposition » au format attendu par FidelityFX.
// Contenu exact de la texture du jeu inconnu : interprétation réglable.

#include <d3d11.h>

#include <string>

namespace akfsr::exposure {

enum class mode : int { as_is = 0, inverse = 1, exp2 = 2, exp2_negative = 3 };

// Texture R32F 1×1 (SRV) remplie à partir de `game` ; nullptr en cas d'échec.
ID3D11Texture2D *prepare(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *game, mode m, float scale);
void shutdown();
std::string status();

}  // namespace akfsr::exposure
