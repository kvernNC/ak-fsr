#pragma once

#include <string>

// Expérience (branche experiment/ngx-optiscaler) — sonde NGX.
// Objectif : vérifier qu'OptiScaler, posé en nvngx.dll à côté du jeu, se charge sous notre addon ReShade,
// cohabite sans planter, et répond à un appel DLSS/NGX minimal (Init + capacités). Ce n'est PAS encore le
// backend : juste le test du socle (la seule vraie inconnue de l'étape 1). Aucune dépendance à un SDK
// propriétaire : les quelques signatures NGX utilisées sont déclarées à la main (ABI public de nvngx.dll).

struct ID3D11Device;

namespace akfsr::ngx_probe {

// Charge nvngx.dll, appelle NVSDK_NGX_D3D11_Init + lit les capacités, journalise tout, puis arrête NGX.
// Retourne un résumé lisible. Idempotent hors ré-entrance ; à déclencher à la demande (commande dev).
std::string run(ID3D11Device *device);

}  // namespace akfsr::ngx_probe
