#include "vk_context.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <d3d11_3.h>
#include <reshade.hpp>

#include "dxvk_interop.hpp"
#include "vk_bridge.hpp"

namespace akfsr::vk {

functions fn;

namespace {

constexpr uint32_t kSlots = 3;
constexpr uint32_t kPoints = 4;  // timestamps par command buffer (voir mark())

struct slot {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool submitted = false;
};

struct state {
  bool tried = false;
  bool ok = false;
  context_info info;
  double timestamp_ms = 0;  // 0 : timestamps indisponibles
  VkCommandPool pool = VK_NULL_HANDLE;
  VkQueryPool queries = VK_NULL_HANDLE;
  std::array<slot, kSlots> slots{};
  uint32_t next = 0;
  uint32_t current = UINT32_MAX;  // slot en cours d'enregistrement
} g;

std::vector<void (*)()> g_shutdown_hooks;

std::mutex g_info_mutex;
timings g_timings;
std::string g_status = "non initialisé";

bool load_functions() {
  // Même ordre que le loader de DXVK : winevulkan.dll d'abord (Proton), puis vulkan-1.dll.
  HMODULE module = nullptr;
  for (const wchar_t *name : {L"winevulkan.dll", L"vulkan-1.dll"}) {
    module = GetModuleHandleW(name);
    if (module == nullptr) module = LoadLibraryW(name);
    if (module != nullptr && GetProcAddress(module, "vkGetInstanceProcAddr") != nullptr) break;
    module = nullptr;
  }
  if (module == nullptr) {
    set_status("ni winevulkan.dll ni vulkan-1.dll", true);
    return false;
  }
  fn.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
#define X(name)                                                                                   \
  fn.name = reinterpret_cast<PFN_##name>(fn.vkGetInstanceProcAddr(g.info.instance, #name));       \
  if (fn.name == nullptr) {                                                                       \
    set_status("fonction Vulkan introuvable : " #name, true);                                     \
    return false;                                                                                 \
  }
  AKFSR_VK_INSTANCE_FUNCS(X)
#undef X
#define X(name)                                                                                   \
  fn.name = reinterpret_cast<PFN_##name>(fn.vkGetDeviceProcAddr(g.info.device, #name));           \
  if (fn.name == nullptr) {                                                                       \
    set_status("fonction Vulkan introuvable : " #name, true);                                     \
    return false;                                                                                 \
  }
  AKFSR_VK_DEVICE_FUNCS(X)
#undef X
  return true;
}

// ---------------------------------------------------------------------------------------------
// Diagnostic des crashs : appels d'interop sous SEH (pas d'objets C++ à détruire dans ces fonctions)
// et gestionnaire vectoriel qui journalise les exceptions de tous les threads (DXVK compris).

std::string describe_address(const void *address) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(address), &module)) {
    char path[MAX_PATH];
    if (GetModuleFileNameA(module, path, MAX_PATH)) {
      const char *base = std::strrchr(path, '\\');
      std::snprintf(name, sizeof(name), "%s", base ? base + 1 : path);
    }
  }
  char buf[MAX_PATH + 48];
  std::snprintf(buf, sizeof(buf), "%s+0x%llx", name,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
  return buf;
}

void log_exception(const char *prefix, const EXCEPTION_RECORD *rec, DWORD thread) {
  char buf[512];
  int n = std::snprintf(buf, sizeof(buf), "AK-FSR: %s exception 0x%08lX thread %lu à %s", prefix,
                        static_cast<unsigned long>(rec->ExceptionCode), static_cast<unsigned long>(thread),
                        describe_address(rec->ExceptionAddress).c_str());
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && n > 0 &&
      n < static_cast<int>(sizeof(buf))) {
    std::snprintf(buf + n, sizeof(buf) - n, " (%s de 0x%llx)", rec->ExceptionInformation[0] == 0   ? "lecture"
                                                               : rec->ExceptionInformation[0] == 1 ? "écriture"
                                                                                                    : "exécution",
                  static_cast<unsigned long long>(rec->ExceptionInformation[1]));
  }
  reshade::log::message(reshade::log::level::error, buf);
}

std::atomic<int> g_veh_logged{0};

LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  // Exceptions bénignes : nommage de threads, OutputDebugString, exceptions C++ (MSVC) du jeu.
  if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A || code == 0xE06D7363)
    return EXCEPTION_CONTINUE_SEARCH;
  if (g_veh_logged.fetch_add(1) < 8) log_exception("[veh]", ep->ExceptionRecord, GetCurrentThreadId());
  return EXCEPTION_CONTINUE_SEARCH;
}

namespace seh {

int filter(EXCEPTION_POINTERS *ep) {
  log_exception("[seh]", ep->ExceptionRecord, GetCurrentThreadId());
  return EXCEPTION_EXECUTE_HANDLER;
}

DWORD flush(IDXGIVkInteropDevice *interop) {
  __try {
    interop->FlushRenderingCommands();
    return 0;
  } __except (filter(GetExceptionInformation())) {
    return GetExceptionCode();
  }
}

DWORD submit(IDXGIVkInteropDevice *interop, VkQueue queue, const VkSubmitInfo *info, VkFence fence, VkResult *r) {
  __try {
    interop->LockSubmissionQueue();
    *r = fn.vkQueueSubmit(queue, 1, info, fence);
    interop->ReleaseSubmissionQueue();
    return 0;
  } __except (filter(GetExceptionInformation())) {
    return GetExceptionCode();
  }
}

}  // namespace seh

bool guarded(DWORD code, const char *step) {
  if (code == 0) return true;
  char buf[160];
  std::snprintf(buf, sizeof(buf), "exception 0x%08lX pendant %s (interop désactivée)", static_cast<unsigned long>(code),
                step);
  set_status(buf, true);
  reshade::log::message(reshade::log::level::error, (std::string("AK-FSR: ") + buf).c_str());
  g.ok = false;
  return false;
}

// Attend un slot déjà soumis et relève ses timestamps.
bool reclaim(uint32_t index) {
  auto &s = g.slots[index];
  if (!s.submitted) return true;
  if (!check(fn.vkWaitForFences(g.info.device, 1, &s.fence, VK_TRUE, 1'000'000'000ull), "vkWaitForFences"))
    return false;
  s.submitted = false;
  if (g.timestamp_ms > 0) {
    // Valeur + disponibilité par point : les points non écrits par l'appelant sont ignorés.
    uint64_t ts[kPoints][2] = {};
    fn.vkGetQueryPoolResults(g.info.device, g.queries, index * kPoints, kPoints, sizeof(ts), ts, sizeof(ts[0]),
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    auto span = [&](uint32_t a, uint32_t b) {
      return ts[a][1] && ts[b][1] ? static_cast<double>(ts[b][0] - ts[a][0]) * g.timestamp_ms : 0.0;
    };
    std::lock_guard lock(g_info_mutex);
    g_timings.gpu_ms = span(0, 3);
    g_timings.gpu_prepare_ms = span(0, 1);
    g_timings.gpu_fsr_ms = span(1, 2);
  }
  return true;
}

uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < g.info.memory.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (g.info.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  return UINT32_MAX;
}

bool is_depth_format(VkFormat f) {
  return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
         f == VK_FORMAT_D16_UNORM;
}

bool has_stencil(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT; }

bool create_image_common(image &img, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage) {
  img.info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  img.info.imageType = VK_IMAGE_TYPE_2D;
  img.info.format = format;
  img.info.extent = {width, height, 1};
  img.info.mipLevels = 1;
  img.info.arrayLayers = 1;
  img.info.samples = VK_SAMPLE_COUNT_1_BIT;
  img.info.tiling = VK_IMAGE_TILING_OPTIMAL;
  img.info.usage = usage;
  img.info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img.info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  img.aspect = is_depth_format(format)
                   ? (VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                   : VK_IMAGE_ASPECT_COLOR_BIT;
  img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!check(fn.vkCreateImage(g.info.device, &img.info, nullptr, &img.handle), "vkCreateImage")) return false;

  VkMemoryRequirements req;
  fn.vkGetImageMemoryRequirements(g.info.device, img.handle, &req);
  const uint32_t type = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) {
    set_status("aucun type de mémoire device-local", true);
    return false;
  }
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type;
  if (!check(fn.vkAllocateMemory(g.info.device, &alloc, nullptr, &img.memory), "vkAllocateMemory") ||
      !check(fn.vkBindImageMemory(g.info.device, img.handle, img.memory, 0), "vkBindImageMemory"))
    return false;

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = img.handle;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  // Une vue échantillonnée d'une image depth/stencil ne porte que sur un aspect.
  view.subresourceRange = {is_depth_format(format) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : img.aspect, 0, 1,
                           0, 1};
  return check(fn.vkCreateImageView(g.info.device, &view, nullptr, &img.view), "vkCreateImageView");
}

}  // namespace

const context_info &ctx() { return g.info; }

void set_status(const std::string &s, bool failure) {
  std::lock_guard lock(g_info_mutex);
  g_status = s;
  if (failure) ++g_timings.failures;
}

bool check(VkResult r, const char *what) {
  if (r == VK_SUCCESS) return true;
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s : VkResult %d", what, static_cast<int>(r));
  set_status(buf, true);
  return false;
}

bool check_hr(HRESULT hr, const char *what) {
  if (SUCCEEDED(hr)) return true;
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s : HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
  set_status(buf, true);
  return false;
}

double now_ms() {
  static const double freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

// Correspondance DXGI -> Vulkan identique à celle de DXVK pour les formats rencontrés.
VkFormat to_vk_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return VK_FORMAT_D32_SFLOAT_S8_UINT;  // mode « any » de DXVK
    default: return VK_FORMAT_UNDEFINED;
  }
}

bool format_supports(VkFormat format, VkFormatFeatureFlags features) {
  VkFormatProperties props{};
  fn.vkGetPhysicalDeviceFormatProperties(g.info.physical, format, &props);
  return (props.optimalTilingFeatures & features) == features;
}

bool create_private_image(image &img, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage) {
  if (create_image_common(img, format, width, height, usage)) return true;
  destroy_image(img);
  return false;
}

bool create_shared_image(image &img, DXGI_FORMAT format, uint32_t width, uint32_t height,
                         VkImageUsageFlags extra_usage) {
  const VkFormat vk_format = to_vk_format(format);
  if (vk_format == VK_FORMAT_UNDEFINED) {
    set_status("format DXGI non pris en charge : " + std::to_string(static_cast<int>(format)), true);
    return false;
  }
  // Usages minimaux que DXVK déduit de BindFlags = SHADER_RESOURCE (d3d11_texture.cpp) ; les usages
  // supplémentaires ne servent qu'à nous.
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT | extra_usage;
  if (!create_image_common(img, vk_format, width, height, usage)) {
    destroy_image(img);
    return false;
  }
  img.layout = kSharedLayout;

  D3D11_TEXTURE2D_DESC1 desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
  if (!check_hr(g.info.interop->CreateTexture2DFromVkImage(&desc, img.handle, &img.texture),
                "CreateTexture2DFromVkImage")) {
    destroy_image(img);
    return false;
  }
  return true;
}

void destroy_image(image &img) {
  if (img.texture != nullptr) img.texture->Release();
  if (img.view != VK_NULL_HANDLE) fn.vkDestroyImageView(g.info.device, img.view, nullptr);
  if (img.handle != VK_NULL_HANDLE) fn.vkDestroyImage(g.info.device, img.handle, nullptr);
  if (img.memory != VK_NULL_HANDLE) fn.vkFreeMemory(g.info.device, img.memory, nullptr);
  img = {};
}

void wait_idle() {
  if (g.info.interop == nullptr || g.info.device == VK_NULL_HANDLE) return;
  seh::flush(g.info.interop);
  g.info.interop->LockSubmissionQueue();
  fn.vkDeviceWaitIdle(g.info.device);
  g.info.interop->ReleaseSubmissionQueue();
  for (auto &s : g.slots) s.submitted = false;
}

bool flush_dxvk() { return g.ok && guarded(seh::flush(g.info.interop), "FlushRenderingCommands"); }

VkCommandBuffer begin_commands() {
  if (!g.ok) return VK_NULL_HANDLE;
  const uint32_t index = g.next;
  g.next = (g.next + 1) % kSlots;
  if (!reclaim(index)) return VK_NULL_HANDLE;
  auto &s = g.slots[index];
  fn.vkResetFences(g.info.device, 1, &s.fence);
  fn.vkResetCommandBuffer(s.cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!check(fn.vkBeginCommandBuffer(s.cmd, &begin), "vkBeginCommandBuffer")) return VK_NULL_HANDLE;
  if (g.timestamp_ms > 0) fn.vkCmdResetQueryPool(s.cmd, g.queries, index * kPoints, kPoints);
  g.current = index;
  return s.cmd;
}

void mark(VkCommandBuffer cmd, uint32_t point) {
  if (g.timestamp_ms <= 0 || g.current == UINT32_MAX || point >= kPoints - 1) return;
  // BOTTOM_OF_PIPE : écrit quand tout le travail qui précède est terminé.
  fn.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, g.current * kPoints + point);
}

bool submit_commands(VkCommandBuffer cmd, double cpu_start_ms) {
  if (g.current == UINT32_MAX || g.slots[g.current].cmd != cmd) return false;
  const uint32_t index = g.current;
  g.current = UINT32_MAX;
  auto &s = g.slots[index];
  if (g.timestamp_ms > 0)
    fn.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, index * kPoints + kPoints - 1);
  if (!check(fn.vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) return false;

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  VkResult r = VK_ERROR_UNKNOWN;
  if (!guarded(seh::submit(g.info.interop, g.info.queue, &submit, s.fence, &r), "vkQueueSubmit")) return false;
  if (!check(r, "vkQueueSubmit")) return false;
  s.submitted = true;

  std::lock_guard lock(g_info_mutex);
  g_timings.flush_cpu_ms = now_ms() - cpu_start_ms;
  ++g_timings.runs;
  return true;
}

VkImageMemoryBarrier barrier(const image &img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                             VkAccessFlags dst) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img.handle;
  b.subresourceRange = {img.aspect, 0, 1, 0, 1};
  return b;
}

void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage, VkAccessFlags src,
                    VkPipelineStageFlags dst_stage, VkAccessFlags dst) {
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  fn.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void add_shutdown_hook(void (*hook)()) { g_shutdown_hooks.push_back(hook); }

// ---------------------------------------------------------------------------------------------
// Interface publique (vk_bridge.hpp).

bool init(ID3D11Device *native_device) {
  if (g.tried) return g.ok;
  g.tried = true;
  static void *veh = AddVectoredExceptionHandler(1, vectored_handler);
  (void)veh;
  auto &info = g.info;
  if (native_device == nullptr ||
      FAILED(native_device->QueryInterface(__uuidof(IDXGIVkInteropDevice1), reinterpret_cast<void **>(&info.interop)))) {
    set_status("IDXGIVkInteropDevice1 indisponible (pas DXVK ?)", true);
    return false;
  }
  info.d3d = native_device;
  info.d3d->AddRef();
  info.d3d->GetImmediateContext(&info.immediate);
  info.interop->GetVulkanHandles(&info.instance, &info.physical, &info.device);
  info.interop->GetSubmissionQueue(&info.queue, &info.family);
  if (info.device == VK_NULL_HANDLE || info.queue == VK_NULL_HANDLE || info.immediate == nullptr) {
    set_status("handles Vulkan / contexte nuls", true);
    return false;
  }
  if (!load_functions()) return false;

  VkPhysicalDeviceProperties props;
  fn.vkGetPhysicalDeviceProperties(info.physical, &props);
  fn.vkGetPhysicalDeviceMemoryProperties(info.physical, &info.memory);
  if (props.limits.timestampComputeAndGraphics) g.timestamp_ms = props.limits.timestampPeriod / 1e6;

  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = info.family;
  if (!check(fn.vkCreateCommandPool(info.device, &pool_info, nullptr, &g.pool), "vkCreateCommandPool")) return false;

  std::array<VkCommandBuffer, kSlots> cmds{};
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = g.pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = kSlots;
  if (!check(fn.vkAllocateCommandBuffers(info.device, &alloc, cmds.data()), "vkAllocateCommandBuffers")) return false;
  for (uint32_t i = 0; i < kSlots; ++i) {
    g.slots[i].cmd = cmds[i];
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!check(fn.vkCreateFence(info.device, &fence_info, nullptr, &g.slots[i].fence), "vkCreateFence")) return false;
  }
  if (g.timestamp_ms > 0) {
    VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = kSlots * kPoints;
    if (fn.vkCreateQueryPool(info.device, &query_info, nullptr, &g.queries) != VK_SUCCESS) g.timestamp_ms = 0;
  }

  char buf[256];
  std::snprintf(buf, sizeof(buf), "prêt : %s, file famille %u%s", props.deviceName, info.family,
                g.timestamp_ms > 0 ? "" : " (sans timestamps)");
  set_status(buf, false);
  g.ok = true;
  return true;
}

bool ready() { return g.ok; }

void shutdown() {
  if (g.ok) {
    wait_idle();
    for (auto hook : g_shutdown_hooks) hook();
    if (g.queries != VK_NULL_HANDLE) fn.vkDestroyQueryPool(g.info.device, g.queries, nullptr);
    for (auto &s : g.slots) {
      if (s.fence != VK_NULL_HANDLE) fn.vkDestroyFence(g.info.device, s.fence, nullptr);
    }
    if (g.pool != VK_NULL_HANDLE) fn.vkDestroyCommandPool(g.info.device, g.pool, nullptr);
  }
  if (g.info.immediate != nullptr) g.info.immediate->Release();
  if (g.info.interop != nullptr) g.info.interop->Release();
  if (g.info.d3d != nullptr) g.info.d3d->Release();
  g = {};
  set_status("arrêté", false);
}

timings get_timings() {
  std::lock_guard lock(g_info_mutex);
  return g_timings;
}

std::string status() {
  std::lock_guard lock(g_info_mutex);
  return g_status;
}

}  // namespace akfsr::vk
