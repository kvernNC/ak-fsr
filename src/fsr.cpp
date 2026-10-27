#include "fsr.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <reshade.hpp>

#include "fsr4_driver.hpp"
#include "fsr_dx11.hpp"
#include "fsr_params.hpp"
#include "gpu_timer.hpp"
#include "jitter.hpp"
#include "shaders/fsr_prepare.spv.h"
#include "vk_bridge.hpp"
#include "vk_context.hpp"

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

namespace akfsr::fsr {
namespace {

using namespace akfsr::vk;

struct ffx_functions {
  PfnFfxCreateContext create = nullptr;
  PfnFfxDestroyContext destroy = nullptr;
  PfnFfxDispatch dispatch = nullptr;
  PfnFfxQuery query = nullptr;
} ffx;

struct state {
  bool tried_dll = false;
  bool dll_ok = false;
  bool pipeline_ok = false;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;

  ffxContext context = nullptr;
  bool ctx_auto_exposure = false;
  bool ctx_game_mv = false;
  bool ctx_driver = false;  // M10.2 : contexte du provider FSR 4 du pilote
  bool ctx_exposure = false;  // M8.5 : contexte utilisé avec une texture d'exposition
  int ctx_fsr4_model = 0;     // modèle FSR 4 imposé au pilote à la création du contexte
  bool images_exposure = false;  // M8.5 : images créées pour une frame avec exposition
  std::string provider_name;     // version du contexte Vulkan courant
  uint64_t ctx_version = 0;
  bool versions_queried = false;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0, height = 0;
  image color_in, color_out, depth_src, velocity_src, depth, motion, exposure;
} g;

// Historique de la caméra, commun aux deux backends.
struct history {
  double last_run_ms = 0;
  float prev_jx = 0, prev_jy = 0;  // jitter (NDC) relu à la frame précédente
  float mem_rows[16] = {};         // ViewProj sans jitter de la frame précédente (lignes)
  float mem_t[3] = {};             // origine du repère translaté de cette frame (c21.xyz)
  bool mem_valid = false;
} h;

std::mutex g_info_mutex;
info g_info;
d3d::gpu_timer g_total_timer;  // chemin Vulkan complet, mesuré côté D3D11, seulement overlay affiché
std::atomic<double> g_overlay_seen_ms{-1e9};

void set_fsr_status(const std::string &s, bool failure) {
  {
    std::lock_guard lock(g_info_mutex);
    g_info.status = s;
  }
  if (failure) reshade::log::message(reshade::log::level::error, ("AK-FSR: FSR : " + s).c_str());
}

std::atomic<int> g_messages{0};

void ffx_message(uint32_t type, const wchar_t *message) {
  if (g_messages.fetch_add(1) >= 32 || message == nullptr) return;
  char utf8[512];
  WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof(utf8), nullptr, nullptr);
  reshade::log::message(type == FFX_API_MESSAGE_TYPE_ERROR ? reshade::log::level::error : reshade::log::level::warning,
                        (std::string("AK-FSR: [ffx] ") + utf8).c_str());
}

// FFX récupère ses fonctions device via vkGetDeviceProcAddr et appelle sans vérifier celles d'extensions
// que DXVK n'a pas activées (pointeur nul -> crash). On intercepte : alias cœur Vulkan pour les suffixes
// KHR/EXT, fonction vide pour le debug (noms d'objets, marqueurs), et chaque nom resté nul est journalisé.
VkResult VKAPI_CALL ffx_debug_stub(void *, const void *) { return VK_SUCCESS; }

PFN_vkVoidFunction VKAPI_CALL ffx_get_device_proc_addr(VkDevice device, const char *name) {
  PFN_vkVoidFunction f = fn.vkGetDeviceProcAddr(device, name);
  if (f != nullptr || name == nullptr) return f;
  std::string base = name;
  for (const char *suffix : {"KHR", "EXT"}) {
    const size_t n = std::strlen(suffix);
    if (base.size() > n && base.compare(base.size() - n, n, suffix) == 0) {
      f = fn.vkGetDeviceProcAddr(device, base.substr(0, base.size() - n).c_str());
      break;
    }
  }
  const char *outcome = "nulle";
  if (f != nullptr) {
    outcome = "remplacée par la version cœur";
  } else if (base.find("DebugUtils") != std::string::npos || base.find("DebugMarker") != std::string::npos) {
    f = reinterpret_cast<PFN_vkVoidFunction>(&ffx_debug_stub);
    outcome = "remplacée par une fonction vide";
  }
  reshade::log::message(f != nullptr ? reshade::log::level::info : reshade::log::level::warning,
                        (std::string("AK-FSR: [ffx] fonction device ") + name + " absente de DXVK : " + outcome).c_str());
  return f;
}

bool load_dll() {
  if (g.tried_dll) return g.dll_ok;
  g.tried_dll = true;
  // La DLL se trouve à côté de l'addon.
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&load_dll), &self);
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(self, path, MAX_PATH);
  if (wchar_t *slash = wcsrchr(path, L'\\')) *(slash + 1) = 0;
  wcscat_s(path, L"amd_fidelityfx_vk.dll");
  HMODULE dll = LoadLibraryW(path);
  if (dll == nullptr) {
    set_fsr_status("amd_fidelityfx_vk.dll introuvable à côté de l'addon (erreur " + std::to_string(GetLastError()) + ")",
                   true);
    return false;
  }
  ffx.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(dll, "ffxCreateContext"));
  ffx.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(dll, "ffxDestroyContext"));
  ffx.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(dll, "ffxDispatch"));
  ffx.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(dll, "ffxQuery"));
  if (ffx.create == nullptr || ffx.destroy == nullptr || ffx.dispatch == nullptr || ffx.query == nullptr) {
    set_fsr_status("exports ffx* manquants dans amd_fidelityfx_vk.dll", true);
    return false;
  }
  g.dll_ok = true;
  return true;
}

bool create_pipeline() {
  if (g.pipeline_ok) return true;
  const VkDevice dev = ctx().device;

  VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module.codeSize = sizeof(k_fsr_prepare_spv);
  module.pCode = k_fsr_prepare_spv;
  if (!check(fn.vkCreateShaderModule(dev, &module, nullptr, &g.shader), "vkCreateShaderModule")) return false;

  VkDescriptorSetLayoutBinding bindings[4] = {
      {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = bindings;
  if (!check(fn.vkCreateDescriptorSetLayout(dev, &set_info, nullptr, &g.set_layout), "vkCreateDescriptorSetLayout"))
    return false;

  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &g.set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  if (!check(fn.vkCreatePipelineLayout(dev, &layout_info, nullptr, &g.layout), "vkCreatePipelineLayout")) return false;

  VkComputePipelineCreateInfo pipe{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipe.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipe.stage.module = g.shader;
  pipe.stage.pName = "main";
  pipe.layout = g.layout;
  if (!check(fn.vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipe, nullptr, &g.pipeline),
             "vkCreateComputePipelines"))
    return false;

  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = sizes;
  if (!check(fn.vkCreateDescriptorPool(dev, &pool_info, nullptr, &g.pool), "vkCreateDescriptorPool")) return false;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = g.pool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &g.set_layout;
  if (!check(fn.vkAllocateDescriptorSets(dev, &alloc, &g.set), "vkAllocateDescriptorSets")) return false;
  g.pipeline_ok = true;
  return true;
}

void destroy_context() {
  if (g.context != nullptr) {
    if (g.ctx_driver)
      fsr4::destroy_context(&g.context);
    else
      ffx.destroy(&g.context, nullptr);
  }
  g.context = nullptr;
  g.ctx_driver = false;
}

void destroy_frame_resources() {
  destroy_context();
  destroy_image(g.color_in);
  destroy_image(g.color_out);
  destroy_image(g.depth_src);
  destroy_image(g.velocity_src);
  destroy_image(g.exposure);
  destroy_image(g.depth);
  destroy_image(g.motion);
  g.format = DXGI_FORMAT_UNKNOWN;
  g.width = g.height = 0;
}

void destroy_all() {
  destroy_frame_resources();
  g_total_timer.release();
  const VkDevice dev = ctx().device;
  if (g.pool != VK_NULL_HANDLE) fn.vkDestroyDescriptorPool(dev, g.pool, nullptr);
  if (g.pipeline != VK_NULL_HANDLE) fn.vkDestroyPipeline(dev, g.pipeline, nullptr);
  if (g.layout != VK_NULL_HANDLE) fn.vkDestroyPipelineLayout(dev, g.layout, nullptr);
  if (g.set_layout != VK_NULL_HANDLE) fn.vkDestroyDescriptorSetLayout(dev, g.set_layout, nullptr);
  if (g.shader != VK_NULL_HANDLE) fn.vkDestroyShaderModule(dev, g.shader, nullptr);
  const bool tried = g.tried_dll, dll_ok = g.dll_ok, queried = g.versions_queried;
  g = {};
  g.tried_dll = tried;  // la DLL reste chargée
  g.dll_ok = dll_ok;
  g.versions_queried = queried;
}

// Nom du GPU vu par Vulkan (RADV : « … (RADV GFX1201) »).
const char *device_name() {
  static VkPhysicalDeviceProperties props{};
  if (props.deviceName[0] == 0 && ctx().physical != VK_NULL_HANDLE)
    fn.vkGetPhysicalDeviceProperties(ctx().physical, &props);
  return props.deviceName;
}

// Versions de l'upscaler présentes dans la DLL (journalisées une fois, proposées dans l'overlay).
void query_versions() {
  if (g.versions_queried) return;
  g.versions_queried = true;
  uint64_t count = 0;
  ffxQueryDescGetVersions q{};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  q.device = ctx().device;
  q.outputCount = &count;
  if (ffx.query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) {
    reshade::log::message(reshade::log::level::warning, "AK-FSR: [ffx] liste des versions indisponible");
    return;
  }
  std::vector<uint64_t> ids(count);
  std::vector<const char *> names(count);
  q.versionIds = ids.data();
  q.versionNames = names.data();
  if (ffx.query(nullptr, &q.header) != FFX_API_RETURN_OK) return;
  std::lock_guard lock(g_info_mutex);
  for (uint64_t i = 0; i < count; ++i) {
    const std::string name = names[i] ? names[i] : "?";
    g_info.versions.emplace_back(ids[i], name);
    reshade::log::message(reshade::log::level::info,
                          ("AK-FSR: [ffx] version disponible : " + name + " (id " + std::to_string(ids[i]) + ")").c_str());
  }
}

// `exposure` : M8.5 actif. Toute bascule reconstruit tout (images, contexte), comme un changement de taille :
// garder les images en ne recréant que le contexte plante encore (double free dans le pilote, constaté en jeu).
bool ensure_images(DXGI_FORMAT format, uint32_t width, uint32_t height, bool exposure) {
  if (g.color_in.handle != VK_NULL_HANDLE && g.format == format && g.width == width && g.height == height &&
      g.images_exposure == exposure)
    return true;
  if (g.color_in.handle != VK_NULL_HANDLE && g.images_exposure != exposure)
    reshade::log::message(reshade::log::level::info,
                          exposure ? "AK-FSR: exposition du jeu activée : ressources FSR reconstruites"
                                   : "AK-FSR: exposition du jeu coupée : ressources FSR reconstruites");
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    add_shutdown_hook(destroy_all);
  }
  if (g.color_in.handle != VK_NULL_HANDLE) wait_idle();
  destroy_frame_resources();

  const VkFormat color_vk = to_vk_format(format);
  if (!format_supports(color_vk, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
    set_fsr_status("format de couleur sans support storage : " + std::to_string(static_cast<int>(format)), true);
    return false;
  }
  constexpr VkImageUsageFlags kStorage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!create_shared_image(g.color_in, format, width, height, 0) ||
      !create_shared_image(g.color_out, format, width, height, VK_IMAGE_USAGE_STORAGE_BIT) ||
      !create_shared_image(g.depth_src, DXGI_FORMAT_R32G8X24_TYPELESS, width, height, 0) ||
      !create_shared_image(g.velocity_src, DXGI_FORMAT_R16G16_FLOAT, width, height, 0) ||
      !create_private_image(g.depth, VK_FORMAT_R32_SFLOAT, width, height, kStorage) ||
      !create_private_image(g.motion, VK_FORMAT_R16G16_SFLOAT, width, height, kStorage)) {
    set_fsr_status("création des images : " + vk::status(), true);
    destroy_frame_resources();
    return false;
  }

  VkDescriptorImageInfo images[4] = {
      {VK_NULL_HANDLE, g.depth_src.view, kSharedLayout},
      {VK_NULL_HANDLE, g.depth.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, g.motion.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, g.velocity_src.view, kSharedLayout},
  };
  VkWriteDescriptorSet writes[4];
  for (uint32_t i = 0; i < 4; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = g.set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = (i == 0 || i == 3) ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  fn.vkUpdateDescriptorSets(ctx().device, 4, writes, 0, nullptr);
  // Image partagée 1×1 de l'exposition (M8.5) : seulement si demandée ; échec = FSR sans exposition.
  g.images_exposure = exposure;
  if (exposure && !create_shared_image(g.exposure, DXGI_FORMAT_R32_FLOAT, 1, 1, 0))
    reshade::log::message(reshade::log::level::error,
                          ("AK-FSR: exposition du jeu : image partagée 1x1 impossible : " + vk::status()).c_str());
  g.format = format;
  g.width = width;
  g.height = height;
  return true;
}

// Contexte FFX : recréé si l'auto-exposition ou la version demandée changent.
// `exposure` : une texture d'exposition sera fournie. Passer d'avec à sans dans un même contexte fait planter
// FidelityFX (double free dans le pilote, constaté en jeu) : contexte recréé à chaque changement.
bool ensure_context(const settings &s, bool exposure) {
  if (g.context != nullptr && g.ctx_auto_exposure == s.auto_exposure && g.ctx_version == s.version_id &&
      g.ctx_game_mv == s.game_mv && g.ctx_exposure == exposure &&
      (s.version_id != fsr4::kDriverVersionId || g.ctx_fsr4_model == s.fsr4_model))
    return true;
  if (g.context != nullptr) {
    wait_idle();
    destroy_context();
  }
  query_versions();

  ffxCreateBackendVKDesc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.vkDevice = ctx().device;
  backend.vkPhysicalDevice = ctx().physical;
  backend.vkDeviceProcAddr = ffx_get_device_proc_addr;
  ffxOverrideVersion override{};
  override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  override.versionId = s.version_id;
  if (s.version_id != 0) backend.header.pNext = &override.header;

  ffxCreateContextDescUpscale desc{};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  desc.header.pNext = &backend.header;
  desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
               (s.auto_exposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0u) |
               (s.game_mv ? FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0u);
  desc.maxRenderSize = {g.width, g.height};
  desc.maxUpscaleSize = {g.width, g.height};
  desc.fpMessage = ffx_message;
  {
    char msg[160];
    std::snprintf(msg, sizeof(msg), "AK-FSR: création du contexte FSR (version demandée %s, auto-exposition %s)",
                  s.version_id != 0 ? std::to_string(s.version_id).c_str() : "défaut", s.auto_exposure ? "oui" : "non");
    reshade::log::message(reshade::log::level::info, msg);
  }
  ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
  std::string driver_failure;
  if (s.version_id == fsr4::kDriverVersionId) {
    // M10.2 : provider FSR 4 du pilote, repli sur la version par défaut de amd_fidelityfx_vk.dll.
    if (!s.fsr4_driver) {
      driver_failure = "option « FSR 4 pilote » désactivée";
    } else {
      fsr4::set_model(static_cast<fsr4::model>(s.fsr4_model));
      rc = fsr4::create_context(&g.context, &desc.header, ctx().device, ctx().physical, ffx_get_device_proc_addr);
      if (rc == FFX_API_RETURN_OK)
        g.ctx_driver = true;
      else
        driver_failure = fsr4::get_info().status;
    }
    if (!g.ctx_driver) {
      g.context = nullptr;
      reshade::log::message(reshade::log::level::warning,
                            ("AK-FSR: FSR 4 pilote indisponible (" + driver_failure + "), repli sur FSR 3.1").c_str());
      desc.header.pNext = &backend.header;  // create_context du pilote a remplacé la chaîne
      backend.header.pNext = nullptr;
      rc = ffx.create(&g.context, &desc.header, nullptr);
    }
  } else {
    rc = ffx.create(&g.context, &desc.header, nullptr);
  }
  {
    std::lock_guard lock(g_info_mutex);
    g_info.fsr4_status = driver_failure;
  }
  if (rc != FFX_API_RETURN_OK && s.version_id != 0 && !g.ctx_driver && driver_failure.empty()) {
    // Version sauvegardée absente (ex. FSR 4 fourni par OptiScaler, lancé sans lui) : version par défaut.
    reshade::log::message(reshade::log::level::warning,
                          ("AK-FSR: version FSR demandée indisponible (code " + std::to_string(rc) +
                           "), repli sur la version par défaut")
                              .c_str());
    backend.header.pNext = nullptr;
    g.context = nullptr;
    rc = ffx.create(&g.context, &desc.header, nullptr);
  }
  if (rc != FFX_API_RETURN_OK) {
    g.context = nullptr;
    set_fsr_status("ffxCreateContext : code " + std::to_string(rc), true);
    return false;
  }
  g.ctx_auto_exposure = s.auto_exposure;
  g.ctx_game_mv = s.game_mv;
  g.ctx_exposure = exposure;
  g.ctx_fsr4_model = s.fsr4_model;
  g.ctx_version = s.version_id;
  h.last_run_ms = 0;  // historique neuf : reset à la prochaine frame

  ffxQueryGetProviderVersion provider{};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  std::string name;
  if (g.ctx_driver) {
    name = std::string(fsr4::kDriverVersionName) + ", modèle attendu " + fsr4::expected_model(device_name());
  } else {
    name = ffx.query(&g.context, &provider.header) == FFX_API_RETURN_OK && provider.versionName
               ? provider.versionName
               : "?";
  }
  g.provider_name = name;
  {
    std::lock_guard lock(g_info_mutex);
    g_info.provider = name;
  }
  char buf[192];
  std::snprintf(buf, sizeof(buf), "AK-FSR: contexte FSR créé : %ux%u (AA natif), format DXGI %d, version %s, "
                "auto-exposition %s", g.width, g.height, static_cast<int>(g.format), name.c_str(),
                s.auto_exposure ? "oui" : "non");
  reshade::log::message(reshade::log::level::info, buf);
  return true;
}

bool invert(const double m[16], double out[16]) {
  // Gauss-Jordan sur [m | I], m en colonne par colonne.
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[c * 4 + r];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c)
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    const double inv = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[c * 4 + r] = a[r][c + 4];
  return true;
}

// c0..c3 sont les lignes de la ViewProj (convention vecteur-ligne) donc les colonnes de sa transposée :
// en convention vecteur-colonne, la matrice A se lit directement colonne par colonne.
bool reprojection(const float *cur_rows, const float *prev_rows, float out[16]) {
  double cur[16], prev[16], inv_cur[16];
  for (int i = 0; i < 16; ++i) {
    cur[i] = cur_rows[i];
    prev[i] = prev_rows[i];
  }
  if (!invert(cur, inv_cur)) return false;
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      double sum = 0;
      for (int k = 0; k < 4; ++k) sum += prev[k * 4 + r] * inv_cur[c * 4 + k];
      out[c * 4 + r] = static_cast<float>(sum);
    }
  }
  return true;
}

// RT4 doit avoir le format et la taille de nos images pour la copie D3D11.
bool velocity_compatible(ID3D11Resource *velocity, uint32_t width, uint32_t height) {
  if (velocity == nullptr) return false;
  ID3D11Texture2D *tex = nullptr;
  if (FAILED(velocity->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex)))) return false;
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  tex->Release();
  return desc.Format == DXGI_FORMAT_R16G16_FLOAT && desc.Width == width && desc.Height == height &&
         desc.SampleDesc.Count == 1 && desc.ArraySize == 1;
}

FfxApiResource ffx_resource(const image &img, uint32_t state) {
  return ffxApiGetResourceVK(reinterpret_cast<void *>(img.handle),
                             ffxApiGetImageResourceDescriptionVK(img.handle, img.info, 0), state);
}

// Paramètres de la frame (jitter, matrices, vecteurs de la caméra), communs aux deux backends.
void compute_params(const frame_inputs &in, const settings &s, uint32_t width, uint32_t height, frame_params &p) {
  const double t0 = now_ms();
  const double delta = h.last_run_ms > 0 ? t0 - h.last_run_ms : 0.0;
  const bool reset = h.last_run_ms == 0 || delta > 250.0;  // reprise après une pause (vidéo, menu, mode off)
  h.last_run_ms = t0;
  p.start_ms = t0;
  p.reset = reset;
  p.frame_ms = reset ? 16.6f : static_cast<float>(delta);

  // Jitter réellement utilisé pour cette frame, relu dans la ViewProj ; retiré des matrices courante et
  // précédente (celle-ci avec le jitter relu à la frame précédente) pour des vecteurs sans jitter.
  view_data view = in.view;
  float jx = 0, jy = 0;
  jitter::recover_from_view(view.data(), jx, jy);
  const float prev_jx = reset ? jx : h.prev_jx, prev_jy = reset ? jy : h.prev_jy;
  for (int i = 0; i < 4; ++i) {
    float *cur = &view[i * 4], *prev = &view[16 + i * 4];
    cur[0] -= cur[3] * jx;
    cur[1] -= cur[3] * jy;
    prev[0] -= prev[3] * prev_jx;
    prev[1] -= prev[3] * prev_jy;
  }
  h.prev_jx = jx;
  h.prev_jy = jy;

  // Matrice précédente : celle du CB (c4..c7) ou celle mémorisée à la frame précédente, ramenée dans le
  // repère translaté courant (origine c21.xyz) : row3 += Σk (t_prev - t_cur)k · rowk.
  const float t_cur[3] = {view[84], view[85], view[86]};
  float mem_prev[16];
  const bool mem_ok = h.mem_valid && !reset;
  if (mem_ok) {
    std::memcpy(mem_prev, h.mem_rows, sizeof(mem_prev));
    for (int k = 0; k < 3; ++k)
      for (int c = 0; c < 4; ++c) mem_prev[12 + c] += (h.mem_t[k] - t_cur[k]) * h.mem_rows[k * 4 + c];
  }
  float mismatch = 0;
  if (mem_ok)
    for (int i = 0; i < 16; ++i) mismatch = std::max(mismatch, std::fabs(mem_prev[i] - view[16 + i]));
  const bool use_memory = mem_ok && s.prev_source != prev_matrix::cb;
  std::memcpy(h.mem_rows, view.data(), sizeof(h.mem_rows));  // courante sans jitter, pour la frame suivante
  std::memcpy(h.mem_t, t_cur, sizeof(h.mem_t));
  h.mem_valid = true;
  p.jitter_px_x = jx * static_cast<float>(width) * 0.5f;
  p.jitter_px_y = -jy * static_cast<float>(height) * 0.5f;

  push_constants &pc = p.pc;
  pc = {};
  p.have_reproj = reprojection(view.data(), use_memory ? mem_prev : view.data() + 16, pc.reproj);
  pc.size[0] = static_cast<float>(width);
  pc.size[1] = static_cast<float>(height);
  pc.inv_size[0] = 1.0f / pc.size[0];
  pc.inv_size[1] = 1.0f / pc.size[1];
  pc.zero_motion = (s.zero_motion || !p.have_reproj) ? 1u : 0u;
  p.mv_scale_x = static_cast<float>(width);
  p.mv_scale_y = static_cast<float>(height);
  // M8.3 : RT4 telle quelle (jitterée), seulement la profondeur à préparer.
  p.game_mv = s.game_mv && velocity_compatible(in.velocity, width, height);
  if (p.game_mv) {
    const float k = s.game_mv_scale == 1 ? 0.5f : 1.0f;
    p.mv_scale_x *= k;
    p.mv_scale_y *= s.game_mv_flip_y ? -k : k;
    pc.zero_motion = 1u;
  }
  // Vélocité des objets (RT4) : calculée par le jeu avec les ViewProj jitterées c0..c3 / c4..c7.
  p.objects = p.game_mv || (s.object_velocity && velocity_compatible(in.velocity, width, height));
  if (p.objects) {
    float pjx = 0, pjy = 0;
    jitter::recover_from_view(in.view.data() + 16, pjx, pjy);
    pc.jitter_delta_uv[0] = (jx - pjx) * 0.5f;
    pc.jitter_delta_uv[1] = -(jy - pjy) * 0.5f;
    pc.use_objects = 1u;
  }

  // Échelle verticale de la projection et plan proche (clip.z = clip.w - near), vérifiables dans l'overlay.
  const float sx = std::sqrt(view[0] * view[0] + view[4] * view[4] + view[8] * view[8]);
  const float sy = std::sqrt(view[1] * view[1] + view[5] * view[5] + view[9] * view[9]);
  p.fov = sy > 0 ? 2.0f * std::atan(1.0f / sy) : 1.0f;
  p.near_plane = view[15] - view[14];
  jitter::set_render_size(width, height);
  jitter::learn_main_view(sx, sy, p.near_plane);
  std::lock_guard lock(g_info_mutex);
  g_info.fov_deg = p.fov * 57.29578f;
  g_info.near_plane = p.near_plane;
  g_info.jitter_px_x = p.jitter_px_x;
  g_info.jitter_px_y = p.jitter_px_y;
  g_info.prev_mismatch = mem_ok ? mismatch : -1.0f;
  g_info.prev_from_memory = use_memory;
}

}  // namespace

static bool run_frame(const frame_inputs &in, const settings &s);

bool run(const frame_inputs &in, const settings &requested) {
  if (in.color == nullptr || in.depth == nullptr) return false;
  // M8.5 : exposition fournie -> pas d'auto-exposition (contexte recréé en conséquence).
  settings s = requested;
  if (in.exposure != nullptr) s.auto_exposure = false;
  return run_frame(in, s);
}

static bool run_frame(const frame_inputs &in, const settings &s) {

  ID3D11Texture2D *color_tex = nullptr;
  if (FAILED(in.color->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&color_tex)))) return false;
  D3D11_TEXTURE2D_DESC desc;
  color_tex->GetDesc(&desc);
  color_tex->Release();
  if (in.output != nullptr && in.output != in.color) {
    ID3D11Texture2D *out_tex = nullptr;
    if (FAILED(in.output->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&out_tex)))) return false;
    D3D11_TEXTURE2D_DESC out_desc;
    out_tex->GetDesc(&out_desc);
    out_tex->Release();
    if (out_desc.Format != desc.Format || out_desc.Width != desc.Width || out_desc.Height != desc.Height) {
      set_fsr_status("sortie incompatible avec l'entrée (format " + std::to_string(out_desc.Format) + " / " +
                         std::to_string(desc.Format) + ")",
                     true);
      return false;
    }
  }

  frame_params p;
  if (s.fsr_backend == backend::dx11) {
    compute_params(in, s, desc.Width, desc.Height, p);
    std::string error;
    const double t0 = now_ms();
    const bool ok = dx11::run(in, s, p, desc, error);
    {
      std::lock_guard lock(g_info_mutex);
      g_info.dx11_status = ok ? "actif" : error;
      g_info.dx11_cpu_ms = now_ms() - t0;
      if (ok) g_info.provider = "3.1 (backend DX11 natif)";
    }
    if (!ok) {
      set_fsr_status("backend DX11 : " + error, true);
      h.last_run_ms = 0;
      return false;
    }
    set_fsr_status(p.have_reproj ? "actif (DX11)" : "actif (DX11, matrice non inversible : vecteurs nuls)", false);
    return true;
  }

  if (!ready()) return false;
  if (!load_dll() || !create_pipeline()) return false;
  if (!ensure_images(desc.Format, desc.Width, desc.Height, in.exposure != nullptr)) return false;
  const bool use_exposure = in.exposure != nullptr && g.exposure.handle != VK_NULL_HANDLE;
  if (!ensure_context(s, use_exposure)) return false;
  compute_params(in, s, g.width, g.height, p);
  {
    std::lock_guard lock(g_info_mutex);
    g_info.provider = g.provider_name;  // après un passage par le backend DX11
  }
  const double t0 = p.start_ms;
  const push_constants &pc = p.pc;
  const bool objects = p.objects;
  const bool reset = p.reset;

  // 1) Entrées du jeu -> nos images partagées (copies par DXVK).
  ID3D11DeviceContext *imm = ctx().immediate;
  if (now_ms() - g_overlay_seen_ms.load(std::memory_order_relaxed) < 1000.0) g_total_timer.begin(ctx().d3d, imm);
  imm->CopySubresourceRegion(g.color_in.texture, 0, 0, 0, 0, in.color, 0, nullptr);
  imm->CopySubresourceRegion(g.depth_src.texture, 0, 0, 0, 0, in.depth, 0, nullptr);
  if (objects) imm->CopySubresourceRegion(g.velocity_src.texture, 0, 0, 0, 0, in.velocity, 0, nullptr);
  if (use_exposure) imm->CopySubresourceRegion(g.exposure.texture, 0, 0, 0, 0, in.exposure, 0, nullptr);
  if (!flush_dxvk()) {
    g_total_timer.end(imm);
    return false;
  }

  VkCommandBuffer cmd = begin_commands();
  if (cmd == VK_NULL_HANDLE) {
    g_total_timer.end(imm);
    return false;
  }
  memory_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                 VK_ACCESS_MEMORY_READ_BIT);
  mark(cmd, 0);

  // 2) Profondeur R32F + vecteurs de mouvement.
  VkImageMemoryBarrier to_general[2] = {
      barrier(g.depth, g.depth.layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT),
      barrier(g.motion, g.motion.layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT),
  };
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                          0, nullptr, 2, to_general);
  fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline);
  fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &g.set, 0, nullptr);
  fn.vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
  fn.vkCmdDispatch(cmd, (g.width + 7) / 8, (g.height + 7) / 8, 1);
  VkImageMemoryBarrier to_read[3] = {
      barrier(g.depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT),
      barrier(g.motion, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT),
      // Sortie : GENERAL pendant FSR (état UNORDERED_ACCESS déclaré), puis retour à la couche DXVK.
      barrier(g.color_out, kSharedLayout, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT),
  };
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, to_read);
  g.depth.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  g.motion.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  mark(cmd, 1);

  // 3) FSR, AA natif.
  ffxDispatchDescUpscale d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = cmd;
  d.color = ffx_resource(g.color_in, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.depth = ffx_resource(g.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = ffx_resource(p.game_mv ? g.velocity_src : g.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  if (use_exposure) d.exposure = ffx_resource(g.exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.output = ffx_resource(g.color_out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  d.jitterOffset = {p.jitter_px_x, p.jitter_px_y};
  d.motionVectorScale = {p.mv_scale_x, p.mv_scale_y};
  d.renderSize = {g.width, g.height};
  d.upscaleSize = {g.width, g.height};
  d.enableSharpening = s.sharpness > 0.0f;
  d.sharpness = s.sharpness;
  d.frameTimeDelta = p.frame_ms;  // millisecondes
  d.preExposure = 1.0f;
  d.reset = reset;
  d.cameraNear = p.near_plane > 0 ? p.near_plane : 10.0f;
  d.cameraFar = FLT_MAX;  // profondeur infinie (exigé par FFX avec DEPTH_INFINITE)
  d.cameraFovAngleVertical = p.fov;
  d.viewSpaceToMetersFactor = 0.01f;  // unités UE3 : centimètres
  d.flags = s.debug_view ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0;
  const ffxReturnCode_t rc = g.ctx_driver ? fsr4::dispatch(&g.context, &d.header) : ffx.dispatch(&g.context, &d.header);
  mark(cmd, 2);

  VkImageMemoryBarrier back = barrier(g.color_out, VK_IMAGE_LAYOUT_GENERAL, kSharedLayout, VK_ACCESS_SHADER_WRITE_BIT,
                                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &back);
  memory_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  if (!submit_commands(cmd, t0)) {
    g_total_timer.end(imm);
    return false;
  }
  if (rc != FFX_API_RETURN_OK) {
    g_total_timer.end(imm);
    // Le command buffer a été soumis (barrières seules) : la couleur du jeu n'est pas touchée.
    set_fsr_status("ffxDispatch : code " + std::to_string(rc), true);
    return false;
  }

  // 4) Résultat -> couleur du jeu.
  imm->CopySubresourceRegion(in.output != nullptr ? in.output : in.color, 0, 0, 0, 0, g.color_out.texture, 0, nullptr);
  g_total_timer.end(imm);
  {
    std::lock_guard lock(g_info_mutex);
    g_info.vulkan_total_gpu_ms = g_total_timer.last_ms();
  }
  set_fsr_status(p.have_reproj ? "actif" : "actif (matrice non inversible : vecteurs nuls)", false);
  return true;
}

void note_overlay_visible() { g_overlay_seen_ms = now_ms(); }

info get_info() {
  std::lock_guard lock(g_info_mutex);
  return g_info;
}

}  // namespace akfsr::fsr
