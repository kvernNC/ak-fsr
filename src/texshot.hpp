#pragma once

// Outils de développement : capture d'une texture du jeu au milieu de la frame (commande « texshot » du pilotage par
// fichier). La texture est copiée sur le contexte immédiat juste avant le dispatch d'un compute shader donné (ou juste
// après, à la commande suivante), puis enregistrée en PNG au present. Sert à trouver la passe qui introduit un défaut
// (M9 : ScreenPercentage) en suivant l'image le long de la chaîne de rendu.

#include <d3d11.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace akfsr::texshot {

// `cs_hash` : CS (ou PS : déclenché au draw, cible = handle seulement).
// `target` : t0..t7 / u0..u1 (vues liées au CS) ou handle de ressource en hexadécimal (relevé dans une capture de frame).
// `after` : copie à la commande suivante (sortie de la passe) au lieu d'avant le dispatch.
void request(uint32_t cs_hash, const std::string &target, const std::string &name, bool after);

// Contexte immédiat, avant chaque dispatch (vues liées au CS) et avant chaque draw (mode `after`).
void on_dispatch(ID3D11DeviceContext *ctx, uint32_t cs_hash, const uint64_t *srv, size_t srv_count, const uint64_t *uav,
                 size_t uav_count);
void on_draw(ID3D11DeviceContext *ctx, uint32_t ps_hash);

// Constante forcée à chaque dispatch / draw du shader `hash` (« cs » ou « ps ») : registre c<reg>.<comp> du CB lié en
// b<slot>, sur une copie du CB (celui du jeu n'est pas modifié). Le CB du jeu est remis à la commande suivante s'il est
// encore celui lié. `clear_overrides` retire tout.
void set_override(bool pixel, uint32_t hash, uint32_t slot, uint32_t reg, uint32_t comp, float value);
void clear_overrides();

// Present : écrit les copies en attente dans `dir`.
void on_present(ID3D11DeviceContext *ctx, const std::filesystem::path &dir);

}  // namespace akfsr::texshot
