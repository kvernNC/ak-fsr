#include "vk_device_hook.hpp"

#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstring>
#include <memory>
#include <mutex>

#include <reshade.hpp>

#include "detour.hpp"

namespace akfsr::vk_device_hook {
namespace {

// Extensions demandées par le provider FSR 4 du pilote (PLAN.md §8.1), avec la structure de fonctions à
// chaîner pour celles qui n'existent pas dans le cœur Vulkan (0 : fonctions dans Vulkan11/12/13Features).
struct wanted_extension {
  const char *name;
  VkStructureType features;
};
// sTypes absents des en-têtes 1.3.296 : numéros d'extension 568 (EXT_shader_float8) et 594
// (NV_cooperative_matrix2), soit 1000000000 + (n - 1) * 1000.
constexpr auto kFloat8Features = static_cast<VkStructureType>(1000567000);
constexpr auto kCoopMatrix2Features = static_cast<VkStructureType>(1000593000);
constexpr wanted_extension kWanted[] = {
    {"VK_KHR_cooperative_matrix", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR},
    {"VK_EXT_shader_float8", kFloat8Features},
    {"VK_NV_cooperative_matrix2", kCoopMatrix2Features},
    {"VK_KHR_shader_float16_int8", static_cast<VkStructureType>(0)},
    {"VK_KHR_16bit_storage", static_cast<VkStructureType>(0)},
    {"VK_KHR_variable_pointers", static_cast<VkStructureType>(0)},
    {"VK_KHR_vulkan_memory_model", static_cast<VkStructureType>(0)},
    {"VK_EXT_subgroup_size_control", static_cast<VkStructureType>(0)},
};

// Structure de fonctions générique : le pilote ne remplit que les champs qu'il connaît, le reste reste à 0.
struct feature_blob {
  VkStructureType sType;
  void *pNext;
  VkBool32 fields[64];
};

PFN_vkCreateDevice g_original = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_enum_extensions = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_get_features2 = nullptr;

std::mutex g_mutex;
status g_status;

void log(reshade::log::level level, const std::string &msg) {
  reshade::log::message(level, ("AK-FSR: FSR 4 pilote : " + msg).c_str());
}

template <typename T>
T *find_in_chain(const void *chain, VkStructureType type) {
  for (auto *s = static_cast<const VkBaseOutStructure *>(chain); s != nullptr; s = s->pNext)
    if (s->sType == type) return reinterpret_cast<T *>(const_cast<VkBaseOutStructure *>(s));
  return nullptr;
}

// Active dans `dst` les fonctions prises en charge parmi celles listées (champs VkBool32 de même nom).
#define AKFSR_ENABLE(dst, supported, field) \
  if ((supported).field) (dst)->field = VK_TRUE

void enable_core_features(VkPhysicalDevice physical, const VkDeviceCreateInfo *info) {
  auto *v11 = find_in_chain<VkPhysicalDeviceVulkan11Features>(info->pNext,
                                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
  auto *v12 = find_in_chain<VkPhysicalDeviceVulkan12Features>(info->pNext,
                                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
  auto *v13 = find_in_chain<VkPhysicalDeviceVulkan13Features>(info->pNext,
                                                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
  VkPhysicalDeviceVulkan13Features s13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s13};
  VkPhysicalDeviceVulkan11Features s11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &s12};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s11};
  g_get_features2(physical, &f2);
  // Les structures de DXVK sont modifiées sur place (elles ne servent qu'à cet appel) : seulement des
  // fonctions prises en charge passées de VK_FALSE à VK_TRUE.
  if (v11 != nullptr) {
    AKFSR_ENABLE(v11, s11, storageBuffer16BitAccess);
    AKFSR_ENABLE(v11, s11, uniformAndStorageBuffer16BitAccess);
    AKFSR_ENABLE(v11, s11, variablePointersStorageBuffer);
    AKFSR_ENABLE(v11, s11, variablePointers);
  }
  if (v12 != nullptr) {
    AKFSR_ENABLE(v12, s12, shaderFloat16);
    AKFSR_ENABLE(v12, s12, shaderInt8);
    AKFSR_ENABLE(v12, s12, storageBuffer8BitAccess);
    AKFSR_ENABLE(v12, s12, vulkanMemoryModel);
    AKFSR_ENABLE(v12, s12, vulkanMemoryModelDeviceScope);
  }
  if (v13 != nullptr) {
    AKFSR_ENABLE(v13, s13, subgroupSizeControl);
    AKFSR_ENABLE(v13, s13, computeFullSubgroups);
  }
  if (v11 == nullptr || v12 == nullptr || v13 == nullptr)
    log(reshade::log::level::warning, "structures Vulkan11/12/13Features absentes de la demande de DXVK : "
                                      "fonctions 16 bits / modèle mémoire non activées");
}

VKAPI_ATTR VkResult VKAPI_CALL hooked_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkDevice *device) {
  uint32_t count = 0;
  g_enum_extensions(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> available(count);
  g_enum_extensions(physical, nullptr, &count, available.data());
  auto supported = [&](const char *name) {
    for (const auto &e : available)
      if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
  };

  VkDeviceCreateInfo local = *info;
  std::vector<const char *> extensions(info->ppEnabledExtensionNames,
                                       info->ppEnabledExtensionNames + info->enabledExtensionCount);
  std::vector<std::unique_ptr<feature_blob>> blobs;
  status st;
  for (const auto &w : kWanted) {
    bool present = false;
    for (const char *e : extensions)
      if (std::strcmp(e, w.name) == 0) present = true;
    if (!supported(w.name)) {
      st.missing.emplace_back(w.name);
      continue;
    }
    if (!present) {
      extensions.push_back(w.name);
      st.added.emplace_back(w.name);
    }
    if (w.features != 0 && find_in_chain<VkBaseOutStructure>(info->pNext, w.features) == nullptr) {
      auto blob = std::make_unique<feature_blob>();
      std::memset(blob.get(), 0, sizeof(feature_blob));
      blob->sType = w.features;
      VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, blob.get()};
      g_get_features2(physical, &f2);  // fonctions prises en charge = fonctions activées
      blob->pNext = nullptr;
      blobs.push_back(std::move(blob));
    }
  }
  enable_core_features(physical, info);
  // Nos structures en tête de chaîne, suivies de celles de DXVK.
  const void *next = info->pNext;
  for (auto it = blobs.rbegin(); it != blobs.rend(); ++it) {
    (*it)->pNext = const_cast<void *>(next);
    next = it->get();
  }
  local.pNext = next;
  local.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  local.ppEnabledExtensionNames = extensions.data();

  VkResult r = g_original(physical, &local, allocator, device);
  std::string added;
  for (const auto &a : st.added) added += (added.empty() ? "" : ", ") + a;
  if (r != VK_SUCCESS) {
    // Échec avec nos ajouts : demande d'origine de DXVK, intacte (le jeu doit démarrer).
    log(reshade::log::level::error, "vkCreateDevice en échec avec les ajouts (" + std::to_string(r) +
                                        "), nouvel essai sans eux");
    r = g_original(physical, info, allocator, device);
    st.added.clear();
    st.message = "device créé sans les fonctions de FSR 4 (échec avec elles)";
  } else {
    st.message = "device DXVK créé avec " + std::to_string(st.added.size()) + " extension(s) ajoutée(s)";
  }
  std::string missing;
  for (const auto &m : st.missing) missing += (missing.empty() ? "" : ", ") + m;
  log(reshade::log::level::info, st.message + (added.empty() ? "" : " : " + added) +
                                     (missing.empty() ? "" : " ; absentes du GPU : " + missing));
  st.installed = true;
  st.device_seen = true;
  std::lock_guard lock(g_mutex);
  g_status = st;
  return r;
}

}  // namespace

bool install() {
  {
    std::lock_guard lock(g_mutex);
    if (g_status.installed) return true;
  }
  HMODULE vk = GetModuleHandleW(L"winevulkan.dll");
  if (vk == nullptr) vk = GetModuleHandleW(L"vulkan-1.dll");
  if (vk == nullptr) vk = LoadLibraryW(L"vulkan-1.dll");
  void *create = vk != nullptr ? reinterpret_cast<void *>(GetProcAddress(vk, "vkCreateDevice")) : nullptr;
  g_enum_extensions = vk != nullptr ? reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                                          GetProcAddress(vk, "vkEnumerateDeviceExtensionProperties"))
                                    : nullptr;
  g_get_features2 = vk != nullptr ? reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                                        GetProcAddress(vk, "vkGetPhysicalDeviceFeatures2"))
                                  : nullptr;
  std::string error;
  if (create == nullptr || g_enum_extensions == nullptr || g_get_features2 == nullptr) {
    error = "exports Vulkan introuvables (winevulkan.dll / vulkan-1.dll)";
  } else if (!detour::install(create, reinterpret_cast<const void *>(&hooked_create_device),
                              reinterpret_cast<void **>(&g_original), error)) {
    error = "détour de vkCreateDevice refusé : " + error;
  }
  std::lock_guard lock(g_mutex);
  if (!error.empty()) {
    g_status.message = error;
    log(reshade::log::level::error, error);
    return false;
  }
  g_status.installed = true;
  g_status.message = "détour de vkCreateDevice installé, en attente du device de DXVK";
  log(reshade::log::level::info, g_status.message);
  return true;
}

status get_status() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

}  // namespace akfsr::vk_device_hook
