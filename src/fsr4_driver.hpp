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

// Modèle attendu d'après le nom du GPU (RADV : « … (RADV GFX1201) »), logique d'AMD décrite au §8.1.
std::string expected_model(const char *device_name);

struct info {
  bool tried = false, ok = false;
  std::string status;          // état ou raison de l'échec
  std::string provider_name;   // nom donné par le pilote
  uint64_t provider_id = 0;
  std::string model_seen;      // nom de modèle relevé dans les shaders du pilote (fp8, int8, lite…)
  uint32_t shaders = 0, shaders_rewritten = 0, shader_failures = 0;
};
info get_info();

}  // namespace akfsr::fsr4
