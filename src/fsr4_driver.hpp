#pragma once

// M10.2 — FSR 4 par le provider du pilote AMD (amdvkffx64.dll, propriétaire, fournie par l'utilisateur dans
// Binaries/Win64/ak-fsr/ avec les DLL signées du SDK FFX renommées en _vk). Écrit d'après les seuls faits
// d'interface de PLAN.md §8.1 : aucun code repris d'OptiScaler ni de FSR4-Vulkan-Bridge.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <ffx_api/ffx_api.h>

#include <cstdint>
#include <string>

namespace akfsr::fsr4 {

constexpr uint64_t kDriverVersionId = 0xF5A5CA1E81001002ull;  // FSR 4.1.2, provider du pilote
constexpr const char *kDriverVersionName = "4.1.2 (provider pilote)";

// Charge les DLL et interroge le pilote (une seule fois). false + raison dans get_info().status.
bool load();

// Contexte d'upscale : `upscale` est l'en-tête 0x10000 rempli par l'appelant (sans chaîne) ; la description
// du backend Vulkan (type SDK 2.x) et la version demandée sont ajoutées ici.
ffxReturnCode_t create_context(ffxContext *context, ffxCreateContextDescHeader *upscale, VkDevice device,
                               VkPhysicalDevice physical, PFN_vkGetDeviceProcAddr get_device_proc_addr);
ffxReturnCode_t destroy_context(ffxContext *context);
ffxReturnCode_t dispatch(ffxContext *context, const ffxDispatchDescHeader *desc);
ffxReturnCode_t query(ffxContext *context, ffxQueryDescHeader *desc);

// Modèle imposé : le pilote choisit son modèle d'après le GPU qu'il lit par vkGetPhysicalDeviceProperties(2)
// (importées de vulkan-1.dll) ; ces appels sont interceptés dans sa table d'imports pour lui présenter un GPU
// représentatif du modèle voulu. À régler avant create_context.
enum class model : int { automatic = 0, fp8 = 1, int8 = 2, int8_navi2 = 3, lite = 4 };
void set_model(model m);

// Watermark du pilote (nom du provider et modèle dessinés sur l'image) : variable d'environnement MLSR-WATERMARK,
// lue par le pilote à la création du contexte. À régler avant create_context.
void set_watermark(bool on);

// M12b §3.2 — profil d'identité : GPU et pilote présentés en entier au pilote (par défaut Ryzen Z2 A sous pilote
// AMD Windows), appliqué après le GPU du modèle imposé. Seul amdvkffx64.dll le voit : les DLL signées
// amd_fidelityfx_*_vk.dll n'importent pas Vulkan (constaté), le jeu, DXVK et ReShade gardent la réponse réelle.
// Réglages lus dans la section [FSR4.LiteProfile] de ReShade.ini au chargement (load()).
struct identity_profile {
  bool enabled = false;
  uint32_t vendor_id = 0x1002;
  uint32_t device_id = 0x163F;  // Van Gogh / Z2 A (identique au Deck)
  VkPhysicalDeviceType device_type = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  std::string device_name = "AMD Radeon(TM) Graphics";
  uint32_t driver_version = VK_MAKE_API_VERSION(0, 2, 0, 999);  // valeur haute
  VkDriverId driver_id = VK_DRIVER_ID_AMD_PROPRIETARY;
  std::string driver_name = "AMD proprietary driver";
  std::string driver_info = "26.10.1";  // version Adrenalin plausible
  // Cœurs AMD (profil gpuinfo Z2 A sous RADV), seulement si le profil de base ne suffit pas.
  bool spoof_shader_core = false;
  uint32_t shader_engines = 1, arrays_per_engine = 1, cu_per_shader_array = 8, simd_per_cu = 2, waves_per_simd = 16,
           wave_size = 64, active_cu_count = 8;
};
identity_profile &identity();  // à régler avant create_context
std::string describe_identity();  // ligne pour le watermark / l'UI

// Modèle attendu d'après le nom du GPU (RADV : « … (RADV GFX1201) »), logique d'AMD décrite au §8.1.
std::string expected_model(const char *device_name);

struct info {
  bool tried = false, ok = false;
  std::string status;          // état ou raison de l'échec
  std::string provider_name;   // nom donné par le pilote
  uint64_t provider_id = 0;
  std::string model_seen;      // nom de modèle relevé dans les shaders du pilote (fsr4_model_…)
  std::string model;           // M10.3 : FP8, INT8, INT8-2, INT8 Lite (déduit de model_seen), vide si inconnu
  uint32_t shaders = 0, shaders_rewritten = 0, shader_failures = 0;
  std::string created_name;    // nom du provider créé, demandé au contexte (FSR4-i8, FSR4L-i8…)
  uint32_t context_shaders = 0, context_hash = 0;  // shaders créés avec le dernier contexte et leur empreinte
};
info get_info();

}  // namespace akfsr::fsr4
