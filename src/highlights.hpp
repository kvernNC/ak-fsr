#pragma once

// M7 — hautes lumières : avant FSR, la part de chaque pixel au-dessus d'un seuil (composante max) est
// retirée de la couleur et mise de côté ; après FSR, elle est rajoutée. Les reflets très intenses
// (néons, phares, éclairs) ne laissent plus de traînées ni de scintillement dans l'historique de FSR.
// CS D3D11 sur le contexte immédiat natif, avant l'interop Vulkan.

#include <d3d11.h>

#include <string>

namespace akfsr::highlights {

// Retire les hautes lumières de `color` (en place). false : rien n'a été modifié.
bool extract(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *color, float threshold);
// Rajoute la part mise de côté par le dernier extract() réussi.
void reinject(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *color);
void shutdown();
std::string status();

}  // namespace akfsr::highlights
