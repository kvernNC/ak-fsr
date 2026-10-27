#include "fsr4_driver.hpp"

#include <windows.h>

#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include <cctype>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <reshade.hpp>

#include "crc32.hpp"
#include "spirv_half_image.hpp"

namespace akfsr::fsr4 {
namespace {

// --- Interface du pilote (§8.1) ---------------------------------------------------------------------------
using can_provide_fn = bool (*)(uint64_t desc_type);
struct provider_data {        // version 2 de la structure, 0x50 octets
  uint32_t struct_version;    // 2
  uint64_t desc_type;         // 0x10000 : création d'upscaler
  uint64_t version_id;        // rempli par le pilote
  const char *version_name;   // idem
  can_provide_fn can_provide;
  PfnFfxCreateContext create_context;
  PfnFfxDestroyContext destroy_context;
  PfnFfxConfigure configure;
  PfnFfxQuery query;
  PfnFfxDispatch dispatch;
};
static_assert(sizeof(provider_data) == 0x50);

struct provider_option {  // nœud de la chaîne d'options de UpdateFfxApiProviderEx
  uint32_t value_type;    // 0 : booléen
  uint32_t id;            // 0 : activer l'upscaler
  uint8_t value;
  provider_option *next;
};
static_assert(sizeof(provider_option) == 0x18);

using update_provider_ex_fn = HRESULT(WINAPI *)(void *data, uint32_t size, provider_option *options);

constexpr uint64_t kBackendVkDescSdk2 = 0x01000003ull;  // ffxCreateBackendVKDesc, numérotation du SDK 2.x
constexpr uint64_t kUpscaleVersionDesc = 0x1000Bull;
constexpr uint32_t make_version(uint32_t major, uint32_t minor, uint32_t patch) {
  return (major << 22) | (minor << 12) | patch;
}
struct upscale_version_desc {
  ffxApiHeader header;
  uint32_t version;
};

// --- État -------------------------------------------------------------------------------------------------
std::mutex g_mutex;
info g_info;
provider_data g_provider{};
PFN_vkGetDeviceProcAddr g_base_gdpa = nullptr;
PFN_vkCreateShaderModule g_create_shader_module = nullptr;
PFN_vkCreateComputePipelines g_create_compute_pipelines = nullptr;

void log(reshade::log::level level, const std::string &msg) {
  reshade::log::message(level, ("AK-FSR: FSR 4 pilote : " + msg).c_str());
}

void *alloc(void *, uint64_t size) { return std::malloc(static_cast<size_t>(size)); }
void dealloc(void *, void *p) { std::free(p); }
const ffxAllocationCallbacks kAllocator{nullptr, alloc, dealloc};

std::wstring addon_dir() {
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&addon_dir), &self);
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(self, path, MAX_PATH);
  if (wchar_t *slash = wcsrchr(path, L'\\')) *(slash + 1) = 0;
  return path;
}

std::string model_label(std::string name) {
  for (char &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto has = [&](const char *s) { return name.find(s) != std::string::npos; };
  if (has("fp8")) return "FP8";
  if (has("fsr4l-i8")) return "INT8 Lite";
  if (has("fsr4-i8-2")) return "INT8-2";
  if (has("fsr4-i8")) return "INT8";
  if (has("int8") && has("lite")) return "INT8 Lite";
  if (has("int8_2") || has("int82") || has("int8-2")) return "INT8-2";
  if (has("int8")) return "INT8";
  if (has("lite")) return "Lite";
  return "";
}

// Nom de modèle dans les chaînes du SPIR-V (points d'entrée, OpString, OpName) : « fsr4_model… ».
void note_model(const uint32_t *code, size_t words) {
  for (size_t i = 5; i < words;) {
    const uint32_t count = code[i] >> 16, opcode = code[i] & 0xFFFF;
    if (count == 0 || i + count > words) return;
    size_t first = opcode == 15 ? 3 : (opcode == 7 || opcode == 5) ? 2 : 0;  // EntryPoint, String, Name
    if (first != 0 && count > first) {
      const char *s = reinterpret_cast<const char *>(code + i + first);
      const std::string name(s, strnlen(s, (count - first) * 4));
      if (name.find("fsr4_model") != std::string::npos) {
        std::lock_guard lock(g_mutex);
        if (g_info.model_seen.empty()) {
          g_info.model_seen = name;
          g_info.model = model_label(name);
          log(reshade::log::level::info, "modèle relevé dans les shaders : " + name);
        }
        return;
      }
    }
    i += count;
  }
}

// Module réécrit (images float16 -> float32) ; `storage` garde les mots en vie pendant l'appel.
std::atomic<bool> g_first_module{true};  // premier shader du contexte en cours (empreinte journalisée)
std::atomic<uint32_t> g_context_hash{0}, g_context_modules{0};  // empreinte de tous les shaders du contexte

const VkShaderModuleCreateInfo *patch_module(const VkShaderModuleCreateInfo *info, VkShaderModuleCreateInfo &copy,
                                             std::vector<uint32_t> &storage) {
  const size_t words = info->codeSize / 4;
  note_model(info->pCode, words);
  g_context_hash = g_context_hash.load() * 31u + crc32(info->pCode, info->codeSize);
  g_context_modules.fetch_add(1);
  if (g_first_module.exchange(false)) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "premier shader du contexte : %zu octets, crc32 0x%08X", info->codeSize,
                  crc32(info->pCode, info->codeSize));
    log(reshade::log::level::info, buf);
  }
  std::string error;
  const auto r = spirv::promote_half_images(info->pCode, words, storage, error);
  std::lock_guard lock(g_mutex);
  ++g_info.shaders;
  if (r == spirv::result::failed) {
    if (++g_info.shader_failures <= 4) log(reshade::log::level::warning, "réécriture SPIR-V impossible : " + error);
    return info;
  }
  if (r == spirv::result::unchanged) return info;
  ++g_info.shaders_rewritten;
  copy = *info;
  copy.codeSize = storage.size() * 4;
  copy.pCode = storage.data();
  return &copy;
}

VKAPI_ATTR VkResult VKAPI_CALL hooked_create_shader_module(VkDevice device, const VkShaderModuleCreateInfo *info,
                                                           const VkAllocationCallbacks *allocator,
                                                           VkShaderModule *module) {
  VkShaderModuleCreateInfo copy;
  std::vector<uint32_t> storage;
  return g_create_shader_module(device, patch_module(info, copy, storage), allocator, module);
}

// Modules passés directement dans la chaîne d'un étage de pipeline (VK_KHR_maintenance5).
VKAPI_ATTR VkResult VKAPI_CALL hooked_create_compute_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                               const VkComputePipelineCreateInfo *infos,
                                                               const VkAllocationCallbacks *allocator,
                                                               VkPipeline *pipelines) {
  std::vector<VkComputePipelineCreateInfo> local(infos, infos + count);
  std::vector<VkShaderModuleCreateInfo> copies(count);
  std::vector<std::vector<uint32_t>> storage(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto *chained = static_cast<const VkShaderModuleCreateInfo *>(local[i].stage.pNext);
    if (local[i].stage.module == VK_NULL_HANDLE && chained != nullptr &&
        chained->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
      local[i].stage.pNext = patch_module(chained, copies[i], storage[i]);
  }
  return g_create_compute_pipelines(device, cache, count, local.data(), allocator, pipelines);
}

PFN_vkVoidFunction VKAPI_CALL driver_get_device_proc_addr(VkDevice device, const char *name) {
  PFN_vkVoidFunction f = g_base_gdpa(device, name);
  if (f == nullptr || name == nullptr) return f;
  if (std::strcmp(name, "vkCreateShaderModule") == 0) {
    g_create_shader_module = reinterpret_cast<PFN_vkCreateShaderModule>(f);
    return reinterpret_cast<PFN_vkVoidFunction>(&hooked_create_shader_module);
  }
  if (std::strcmp(name, "vkCreateComputePipelines") == 0) {
    g_create_compute_pipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(f);
    return reinterpret_cast<PFN_vkVoidFunction>(&hooked_create_compute_pipelines);
  }
  return f;
}

// --- GPU présenté au pilote (choix du modèle) ---------------------------------------------------------------
std::atomic<int> g_model{0};
PFN_vkGetPhysicalDeviceProperties g_real_props = nullptr;
PFN_vkGetPhysicalDeviceProperties2 g_real_props2 = nullptr;

struct spoof {
  uint32_t device_id;
  const char *name;
};
// Un GPU représentatif par modèle (identifiants PCI AMD) : RDNA 4, RDNA 3, RDNA 2 de bureau, Van Gogh.
constexpr spoof kSpoofs[] = {
    {0, nullptr},
    {0x7550, "AMD Radeon RX 9070 XT (RADV GFX1201)"},
    {0x744C, "AMD Radeon RX 7900 XTX (RADV NAVI31)"},
    {0x73BF, "AMD Radeon RX 6800 XT (RADV NAVI21)"},
    {0x163F, "AMD Custom GPU 0405 (RADV VANGOGH)"},
};

std::atomic<int> g_identity_logs{0};  // remis à zéro à chaque création de contexte

void apply_spoof(VkPhysicalDeviceProperties &p, const char *call) {
  if (g_identity_logs.fetch_add(1) < 4) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s lu par le pilote : vendeur 0x%04x, appareil 0x%04x, « %s »", call, p.vendorID,
                  p.deviceID, p.deviceName);
    log(reshade::log::level::info, buf);
  }
  const int m = g_model.load();
  if (m <= 0 || m >= static_cast<int>(std::size(kSpoofs)) || p.vendorID != 0x1002) return;
  p.deviceID = kSpoofs[m].device_id;
  // Van Gogh et Strix Point (modèle Lite) sont des GPU intégrés : type présenté en conséquence.
  p.deviceType = m == static_cast<int>(model::lite) ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                                    : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  std::snprintf(p.deviceName, sizeof(p.deviceName), "%s", kSpoofs[m].name);
}

VKAPI_ATTR void VKAPI_CALL hooked_props(VkPhysicalDevice physical, VkPhysicalDeviceProperties *p) {
  g_real_props(physical, p);
  apply_spoof(*p, "vkGetPhysicalDeviceProperties");
}

VKAPI_ATTR void VKAPI_CALL hooked_props2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *p) {
  g_real_props2(physical, p);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    std::string chain;
    for (auto *s = static_cast<VkBaseOutStructure *>(p->pNext); s != nullptr; s = s->pNext)
      chain += " " + std::to_string(static_cast<int>(s->sType));
    log(reshade::log::level::info, "vkGetPhysicalDeviceProperties2 : structures demandées :" + (chain.empty() ? std::string(" aucune") : chain));
  }
  apply_spoof(p->properties, "vkGetPhysicalDeviceProperties2");
}

// Capacités masquées au pilote selon le modèle imposé (il choisit aussi d'après elles : constaté, l'identité
// Van Gogh seule laisse FP8). FP8 et matrices coopératives v2 : masquées hors FP8 ; matrices coopératives de
// base (WMMA, RDNA 3) : masquées aussi pour INT8-2 et Lite (RDNA 2 / Van Gogh).
constexpr auto kFloat8Features = static_cast<VkStructureType>(1000567000);
constexpr auto kCoopMatrix2Features = static_cast<VkStructureType>(1000593000);

bool hidden_extension(const char *name) {
  const int m = g_model.load();
  if (m == 0 || m == 1) return false;
  if (std::strcmp(name, "VK_EXT_shader_float8") == 0 || std::strcmp(name, "VK_NV_cooperative_matrix2") == 0) return true;
  return m >= 3 && std::strcmp(name, "VK_KHR_cooperative_matrix") == 0;
}

bool hidden_features(VkStructureType type) {
  const int m = g_model.load();
  if (m == 0 || m == 1) return false;
  if (type == kFloat8Features || type == kCoopMatrix2Features) return true;
  return m >= 3 && type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
}

PFN_vkGetPhysicalDeviceFeatures2 g_real_features2 = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_real_enum_ext = nullptr;
std::atomic<int> g_feature_logs{0};

VKAPI_ATTR void VKAPI_CALL hooked_features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *f) {
  g_real_features2(physical, f);
  std::string chain;
  for (auto *s = static_cast<VkBaseOutStructure *>(f->pNext); s != nullptr; s = s->pNext) {
    chain += " " + std::to_string(static_cast<int>(s->sType));
    if (hidden_features(s->sType)) {  // les VkBool32 de la structure (taille connue par type) à VK_FALSE
      const int fields_count = s->sType == kCoopMatrix2Features ? 7 : 2;  // float8 et KHR : 2 champs
      auto *fields = reinterpret_cast<VkBool32 *>(reinterpret_cast<uint8_t *>(s) + sizeof(VkBaseOutStructure));
      for (int i = 0; i < fields_count; ++i) fields[i] = VK_FALSE;
      chain += "(masquée)";
    }
  }
  if (g_feature_logs.fetch_add(1) < 4)
    log(reshade::log::level::info, "vkGetPhysicalDeviceFeatures2 lu par le pilote :" + chain);
}

VKAPI_ATTR VkResult VKAPI_CALL hooked_enum_ext(VkPhysicalDevice physical, const char *layer, uint32_t *count,
                                               VkExtensionProperties *props) {
  if (g_model.load() <= 1) return g_real_enum_ext(physical, layer, count, props);
  // Liste complète, filtrée, puis rendue selon la convention Vulkan (taille seule ou copie).
  uint32_t n = 0;
  g_real_enum_ext(physical, layer, &n, nullptr);
  std::vector<VkExtensionProperties> all(n);
  g_real_enum_ext(physical, layer, &n, all.data());
  std::vector<VkExtensionProperties> kept;
  for (const auto &e : all)
    if (!hidden_extension(e.extensionName)) kept.push_back(e);
  if (g_feature_logs.fetch_add(1) < 4)
    log(reshade::log::level::info, "vkEnumerateDeviceExtensionProperties lu par le pilote : " +
                                       std::to_string(all.size() - kept.size()) + " extension(s) masquée(s)");
  if (props == nullptr) {
    *count = static_cast<uint32_t>(kept.size());
    return VK_SUCCESS;
  }
  const uint32_t copied = std::min<uint32_t>(*count, static_cast<uint32_t>(kept.size()));
  std::copy_n(kept.begin(), copied, props);
  *count = copied;
  return copied < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// Remplace dans la table d'imports de `module` les fonctions de vulkan-1.dll nommées.
int patch_imports(HMODULE module) {
  auto *base = reinterpret_cast<uint8_t *>(module);
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress == 0) return 0;
  int patched = 0;
  for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name != 0; ++imp) {
    if (_stricmp(reinterpret_cast<const char *>(base + imp->Name), "vulkan-1.dll") != 0) continue;
    auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->OriginalFirstThunk);
    auto *slots = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData != 0; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
      const char *name = reinterpret_cast<const char *>(
          reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name);
      void *replacement = nullptr;
      if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0) {
        g_real_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(slots->u1.Function);
        replacement = reinterpret_cast<void *>(&hooked_props);
      } else if (std::strcmp(name, "vkGetPhysicalDeviceProperties2") == 0) {
        g_real_props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(slots->u1.Function);
        replacement = reinterpret_cast<void *>(&hooked_props2);
      } else if (std::strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0) {
        g_real_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(slots->u1.Function);
        replacement = reinterpret_cast<void *>(&hooked_features2);
      } else if (std::strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0) {
        g_real_enum_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(slots->u1.Function);
        replacement = reinterpret_cast<void *>(&hooked_enum_ext);
      }
      if (replacement == nullptr) continue;
      DWORD old = 0;
      if (VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) {
        slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
        VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
        ++patched;
      }
    }
  }
  return patched;
}

bool fail(const std::string &why) {
  std::lock_guard lock(g_mutex);
  g_info.ok = false;
  g_info.status = why;
  log(reshade::log::level::error, why);
  return false;
}

}  // namespace

bool load() {
  {
    std::lock_guard lock(g_mutex);
    if (g_info.tried) return g_info.ok;
    g_info.tried = true;
  }
  const std::wstring dir = addon_dir() + L"ak-fsr\\";
  // Le pilote ne donne ses providers qu'en présence de DLL du SDK signées par AMD (§8.1).
  int signed_loaded = 0;
  for (const wchar_t *name : {L"amd_fidelityfx_loader_vk.dll", L"amd_fidelityfx_upscaler_vk.dll"}) {
    if (LoadLibraryW((dir + name).c_str()) != nullptr) {
      ++signed_loaded;
    } else {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%ls introuvable dans ak-fsr\\ (erreur %lu)", name, GetLastError());
      log(reshade::log::level::warning, buf);
    }
  }
  HMODULE driver = LoadLibraryW((dir + L"amdvkffx64.dll").c_str());
  if (driver == nullptr) driver = GetModuleHandleW(L"amdvkffx64.dll");
  if (driver == nullptr)
    return fail("amdvkffx64.dll introuvable dans Binaries\\Win64\\ak-fsr\\ (erreur " + std::to_string(GetLastError()) +
                ")");
  {
    const int patched = patch_imports(driver);
    log(patched == 4 ? reshade::log::level::info : reshade::log::level::warning,
        "interception de l'identification du GPU : " + std::to_string(patched) + "/4 import(s)");
  }
  auto update = reinterpret_cast<update_provider_ex_fn>(GetProcAddress(driver, "UpdateFfxApiProviderEx"));
  if (update == nullptr) return fail("export UpdateFfxApiProviderEx absent de amdvkffx64.dll");

  provider_data data{};
  data.struct_version = 2;
  data.desc_type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  provider_option enable_upscaler{0, 0, 1, nullptr};
  const HRESULT hr = update(&data, sizeof(data), &enable_upscaler);
  char buf[200];
  std::snprintf(buf, sizeof(buf), "UpdateFfxApiProviderEx : hr 0x%08lx, id 0x%016llx, nom « %s », %d DLL signée(s)",
                static_cast<unsigned long>(hr), static_cast<unsigned long long>(data.version_id),
                data.version_name != nullptr ? data.version_name : "-", signed_loaded);
  log(reshade::log::level::info, buf);
  if (FAILED(hr) || data.create_context == nullptr || data.dispatch == nullptr || data.destroy_context == nullptr)
    return fail(signed_loaded < 2 ? "le pilote n'a pas donné de provider (DLL signées du SDK manquantes ?)"
                                  : "le pilote n'a pas donné de provider");
  if (data.can_provide != nullptr && !data.can_provide(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE))
    return fail("le provider du pilote refuse la création d'upscaler (can_provide)");
  if (data.version_id != kDriverVersionId) {
    std::snprintf(buf, sizeof(buf), "identifiant inattendu 0x%016llx (attendu 0x%016llx) : version du pilote non prise "
                  "en charge", static_cast<unsigned long long>(data.version_id),
                  static_cast<unsigned long long>(kDriverVersionId));
    return fail(buf);
  }
  std::lock_guard lock(g_mutex);
  g_provider = data;
  g_info.ok = true;
  g_info.provider_id = data.version_id;
  g_info.provider_name = data.version_name != nullptr ? data.version_name : "";
  g_info.status = "provider prêt";
  return true;
}

ffxReturnCode_t create_context(ffxContext *context, ffxCreateContextDescHeader *upscale, VkDevice device,
                               VkPhysicalDevice physical, PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  if (!load()) return FFX_API_RETURN_NO_PROVIDER;
  g_base_gdpa = get_device_proc_addr;
  g_identity_logs = 0;
  g_feature_logs = 0;
  g_first_module = true;
  g_context_hash = 0;
  g_context_modules = 0;
  {
    std::lock_guard lock(g_mutex);
    g_info.model_seen.clear();  // nom du modèle du nouveau contexte
    g_info.model.clear();
  }
  ffxCreateBackendVKDesc backend{};
  backend.header.type = kBackendVkDescSdk2;
  backend.vkDevice = device;
  backend.vkPhysicalDevice = physical;
  backend.vkDeviceProcAddr = driver_get_device_proc_addr;
  upscale_version_desc version{};
  version.header.type = kUpscaleVersionDesc;
  version.version = make_version(4, 1, 2);
  backend.header.pNext = &version.header;
  upscale->pNext = &backend.header;
  const ffxReturnCode_t rc = g_provider.create_context(context, upscale, &kAllocator);
  upscale->pNext = nullptr;
  {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "shaders créés avec le contexte : %u, empreinte globale 0x%08X",
                  g_context_modules.load(), g_context_hash.load());
    log(reshade::log::level::info, buf);
    // Les modèles INT8 compilent leurs shaders à la création (FP8 au premier dispatch, reconnu à leurs noms) :
    // empreinte connue = INT8 relevé sur PC ; autre empreinte = autre variante (Lite attendu sur le Deck).
    constexpr uint32_t kInt8Hash = 0x1BCF1FEB;  // RX 9070 XT, FP8 masqué (27/10/2026)
    std::lock_guard lock(g_mutex);
    g_info.context_shaders = g_context_modules.load();
    g_info.context_hash = g_context_hash.load();
    if (g_info.context_shaders > 0)
      g_info.model = g_info.context_hash == kInt8Hash ? "INT8" : "INT8, autre variante (Lite probable)";
  }
  if (rc == FFX_API_RETURN_OK && g_provider.query != nullptr) {
    // Nom du provider effectivement créé par le pilote (FSR4-i8, FSR4-i8-2, FSR4L-i8…).
    ffxQueryGetProviderVersion q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (g_provider.query(context, &q.header) == FFX_API_RETURN_OK) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "provider créé : « %s », id 0x%016llx", q.versionName ? q.versionName : "?",
                    static_cast<unsigned long long>(q.versionId));
      log(reshade::log::level::info, buf);
      std::lock_guard lock(g_mutex);
      if (q.versionName != nullptr) {
        const std::string label = model_label(q.versionName);
        if (!label.empty()) g_info.model = label;
      }
      g_info.created_name = q.versionName ? q.versionName : "";
    }
  }
  std::lock_guard lock(g_mutex);
  g_info.status = rc == FFX_API_RETURN_OK ? "contexte créé" : "création du contexte : code " + std::to_string(rc);
  return rc;
}

ffxReturnCode_t destroy_context(ffxContext *context) {
  return g_provider.destroy_context != nullptr ? g_provider.destroy_context(context, &kAllocator)
                                               : FFX_API_RETURN_NO_PROVIDER;
}

ffxReturnCode_t dispatch(ffxContext *context, const ffxDispatchDescHeader *desc) {
  return g_provider.dispatch != nullptr ? g_provider.dispatch(context, desc) : FFX_API_RETURN_NO_PROVIDER;
}

ffxReturnCode_t query(ffxContext *context, ffxQueryDescHeader *desc) {
  return g_provider.query != nullptr ? g_provider.query(context, desc) : FFX_API_RETURN_NO_PROVIDER;
}

void set_model(model m) {
  const int v = static_cast<int>(m);
  if (g_model.exchange(v) != v && v != 0) {
    static const char *names[] = {"auto", "FP8", "INT8", "INT8-2", "INT8 Lite"};
    log(reshade::log::level::info, std::string("modèle imposé : ") + names[v] + " (GPU présenté : " +
                                       kSpoofs[v].name + ")");
  }
}

std::string expected_model(const char *device_name) {
  const std::string n = device_name != nullptr ? device_name : "";
  auto has = [&](const char *s) { return n.find(s) != std::string::npos; };
  if (has("GFX12")) return "FP8 (RDNA 4)";
  if (has("VANGOGH") || has("GFX1033")) return "INT8 Lite (Van Gogh, Steam Deck)";
  if (has("GFX115") || has("STRIX")) return "INT8 Lite (Strix Point)";
  if (has("GFX11")) return "INT8 (RDNA 3)";
  if (has("GFX103")) return "INT8-2 (RDNA 2)";
  return "inconnu";
}

info get_info() {
  std::lock_guard lock(g_mutex);
  return g_info;
}

}  // namespace akfsr::fsr4
