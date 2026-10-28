#pragma once

// FSR 3.1 (FidelityFX SDK v1.1.4, backend Vulkan) en AA natif au point d'insertion.
// Vecteurs de mouvement : caméra seule, par reprojection de la profondeur (CB View c0..c7, sans jitter).
// Jitter (M4) : appliqué par le détour de FPerspectiveMatrix, relu dans le CB View et transmis à FSR.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ID3D11Resource;

namespace akfsr::fsr {

// Défauts : version de développement = nouveautés coupées (tests un par un) ; version publiée = réglages validés
// en jeu (PROGRESS.md) : SMAA du jeu remplacé quand il est actif, biais de LOD -1.
enum class backend : int;
#if AKFSR_DEV_TOOLS
inline constexpr bool kDefaultSmaaInsertion = false;
inline constexpr float kDefaultLodBias = 0.0f;
inline constexpr int kDefaultBackend = 0;  // Vulkan
#else
// Backend DX11 : sur le Deck, FSR 3.1 coûte 2,1 ms de plus par frame au lieu de 5,6 ms en Vulkan (27/10/2026).
inline constexpr bool kDefaultSmaaInsertion = true;
inline constexpr float kDefaultLodBias = -1.0f;
inline constexpr int kDefaultBackend = 1;  // DX11, repli sur Vulkan si ak-fsr-dx11.dll manque
#endif

// Début du CB View copié à chaque frame : c0..c21 (ViewProj courante et précédente, …, c21 = origine du
// repère translaté).
inline constexpr size_t kViewFloats = 88;
using view_data = std::array<float, kViewFloats>;

enum class prev_matrix : int { automatic = 0, cb = 1, memory = 2 };
enum class backend : int { vulkan = 0, dx11 = 1 };  // M8.2 : backend DX11 natif de FidelityFX

struct settings {
  bool debug_view = false;   // vue de debug intégrée à FSR
  bool zero_motion = false;  // vecteurs nuls (diagnostic de la reprojection)
  float sharpness = 0.0f;    // RCAS intégré (0 = désactivé)
  bool auto_exposure = false;  // passe d'auto-exposition de FFX (recrée le contexte)
  uint64_t version_id = 0;     // version de l'upscaler (0 = défaut de la DLL ; recrée le contexte)
  bool jitter = true;           // jitter de la projection (M4)
  prev_matrix prev_source = prev_matrix::automatic;  // ViewProj précédente pour les vecteurs de mouvement
  bool object_velocity = true;  // vélocité des objets du G-buffer (RT4, M5)
  bool highlights = false;        // M7 : hautes lumières sorties de FSR puis rajoutées
  float highlight_threshold = 8.0f;  // seuil sur la composante max (couleur HDR linéaire)
  bool rain_after_fsr = true;        // M7.3 : éclaboussures de pluie dessinées à part, composées après FSR
  bool smaa_insertion = kDefaultSmaaInsertion;  // M8.1 : FSR à la place du SMAA du jeu (repli sur 0xB2A276FD)
  backend fsr_backend = static_cast<backend>(kDefaultBackend);  // M8.2
  // M8.3 : vélocité du G-buffer (RT4) passée telle quelle à FSR, sans reprojection (jitter annulé par FSR).
  bool game_mv = false;
  int game_mv_scale = 0;     // 0 : UV (× taille, lecture de M5) ; 1 : [-1, 1] (× taille / 2, lecture de Luma)
  bool game_mv_flip_y = false;
  float lod_bias = kDefaultLodBias;  // M8.4 : biais de LOD des textures (0 = désactivé ; pris en compte au lancement)
  // M10.2 : FSR 4 par le provider du pilote AMD (DLL dans ak-fsr/, détour de vkCreateDevice au lancement).
  bool fsr4_driver = false;
  // Modèle FSR 4 imposé au pilote (fsr4::model) : 0 = choix du pilote. Recrée le contexte.
  int fsr4_model = 0;
  // M8.5 : exposition du jeu (texture 1×1 du tonemapper) à la place de l'auto-exposition de FSR.
  bool game_exposure = false;
  int exposure_mode = 0;       // exposure::mode : tel quel, inverse, exp2, exp2(-x)
  float exposure_scale = 1.0f;
  // Ancienne neutralisation de M8.5 (plantage dû à OptiScaler chargé dans le processus), conservée pour la
  // compatibilité des fichiers ReShade.ini : sans effet.
  bool exposure_experimental = false;
};

struct frame_inputs {
  ID3D11Resource *color = nullptr;  // HDR r11g11b10, réécrite avec le résultat
  ID3D11Resource *depth = nullptr;  // D32_FLOAT_S8X24 (typeless)
  ID3D11Resource *velocity = nullptr;  // RT4 du G-buffer (r16g16_float, UV préc. - UV cour., jitterée)
  ID3D11Resource *exposure = nullptr;  // M8.5 : R32F 1×1 préparée (nullptr : auto-exposition / aucune)
  ID3D11Resource *output = nullptr;    // destination du résultat (nullptr : `color`) ; même format et taille
  view_data view{};                 // CB View c0..c21 (voir kViewFloats)
};

bool run(const frame_inputs &in, const settings &s);

struct info {
  std::string status;
  std::string provider;                                 // version effectivement utilisée
  std::vector<std::pair<uint64_t, std::string>> versions;  // versions présentes dans la DLL
  float fov_deg = 0, near_plane = 0;  // déduits de la matrice, pour vérification
  float jitter_px_x = 0, jitter_px_y = 0;  // jitter relu dans le CB View (pixels), transmis à FSR
  float prev_mismatch = -1;                // écart max entre ViewProj précédente du CB et mémorisée (-1 : n/a)
  bool prev_from_memory = false;
  std::string dx11_status;  // M8.2 : état du backend DX11
  double vulkan_total_gpu_ms = 0;  // chemin Vulkan complet vu par D3D11 (copies, flush et FSR compris)
  std::string fsr4_status;  // M10.2 : FSR 4 pilote demandé mais indisponible (repli), vide sinon
  double dx11_cpu_ms = 0;
};
info get_info();

// L'overlay est affiché : active la mesure GPU complète du chemin Vulkan (inactive sinon).
void note_overlay_visible();

}  // namespace akfsr::fsr
