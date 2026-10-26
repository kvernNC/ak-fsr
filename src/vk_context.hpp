#pragma once

// Socle Vulkan interne : device DXVK (interop), table de fonctions, images partagées avec D3D11,
// images privées, command buffers de frame et soumission dans l'ordre du flux D3D11.

#include <d3d11.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

struct IDXGIVkInteropDevice1;

namespace akfsr::vk {

#define AKFSR_VK_INSTANCE_FUNCS(X)       \
  X(vkGetDeviceProcAddr)                 \
  X(vkGetPhysicalDeviceProperties)       \
  X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceFormatProperties)

#define AKFSR_VK_DEVICE_FUNCS(X)   \
  X(vkCreateCommandPool)           \
  X(vkDestroyCommandPool)          \
  X(vkAllocateCommandBuffers)      \
  X(vkResetCommandBuffer)          \
  X(vkBeginCommandBuffer)          \
  X(vkEndCommandBuffer)            \
  X(vkCreateFence)                 \
  X(vkDestroyFence)                \
  X(vkWaitForFences)               \
  X(vkResetFences)                 \
  X(vkQueueSubmit)                 \
  X(vkDeviceWaitIdle)              \
  X(vkCreateImage)                 \
  X(vkDestroyImage)                \
  X(vkCreateImageView)             \
  X(vkDestroyImageView)            \
  X(vkGetImageMemoryRequirements)  \
  X(vkAllocateMemory)              \
  X(vkFreeMemory)                  \
  X(vkBindImageMemory)             \
  X(vkCreateQueryPool)             \
  X(vkDestroyQueryPool)            \
  X(vkGetQueryPoolResults)         \
  X(vkCreateShaderModule)          \
  X(vkDestroyShaderModule)         \
  X(vkCreateDescriptorSetLayout)   \
  X(vkDestroyDescriptorSetLayout)  \
  X(vkCreatePipelineLayout)        \
  X(vkDestroyPipelineLayout)       \
  X(vkCreateComputePipelines)      \
  X(vkDestroyPipeline)             \
  X(vkCreateDescriptorPool)        \
  X(vkDestroyDescriptorPool)       \
  X(vkAllocateDescriptorSets)      \
  X(vkUpdateDescriptorSets)        \
  X(vkCmdResetQueryPool)           \
  X(vkCmdWriteTimestamp)           \
  X(vkCmdPipelineBarrier)          \
  X(vkCmdCopyImage)                \
  X(vkCmdBindPipeline)             \
  X(vkCmdBindDescriptorSets)       \
  X(vkCmdPushConstants)            \
  X(vkCmdDispatch)

struct functions {
#define X(name) PFN_##name name = nullptr;
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
  AKFSR_VK_INSTANCE_FUNCS(X)
  AKFSR_VK_DEVICE_FUNCS(X)
#undef X
};
extern functions fn;

struct context_info {
  ID3D11Device *d3d = nullptr;
  ID3D11DeviceContext *immediate = nullptr;  // contexte immédiat natif (DXVK)
  IDXGIVkInteropDevice1 *interop = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceMemoryProperties memory{};
};
const context_info &ctx();

// Couche dans laquelle DXVK garde une texture importée avec BindFlags = SHADER_RESOURCE seul
// (D3D11CommonTexture::OptimizeLayout) : nos command buffers doivent toujours l'y laisser.
constexpr VkImageLayout kSharedLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

struct image {
  VkImage handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;  // aspects de l'image (barrières)
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;       // suivi pour les images privées
  ID3D11Texture2D *texture = nullptr;                     // images partagées uniquement
};

VkFormat to_vk_format(DXGI_FORMAT format);
bool format_supports(VkFormat format, VkFormatFeatureFlags features);

// Image privée (jamais vue par D3D11), avec une vue sur toute l'image.
bool create_private_image(image &img, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage);
// Image partagée : VkImage à nous enveloppée en texture D3D11 (BindFlags = SHADER_RESOURCE). La vue
// porte sur l'aspect profondeur pour les formats depth/stencil.
bool create_shared_image(image &img, DXGI_FORMAT format, uint32_t width, uint32_t height, VkImageUsageFlags extra_usage);
void destroy_image(image &img);  // l'appelant garantit que le GPU n'utilise plus l'image (wait_idle)

void wait_idle();   // flush DXVK puis vkDeviceWaitIdle sous verrou de file
bool flush_dxvk();  // tout le travail D3D11 enregistré est soumis à la file au retour

// Command buffer de la frame (timestamps compris) ; nullptr en cas d'échec.
VkCommandBuffer begin_commands();
// Point de mesure GPU : 0 = début (après la barrière d'entrée), 1 = fin de la préparation, 2 = fin de FSR.
void mark(VkCommandBuffer cmd, uint32_t point);
// Termine et soumet le command buffer sous verrou de la file DXVK (après flush_dxvk).
bool submit_commands(VkCommandBuffer cmd, double cpu_start_ms);

VkImageMemoryBarrier barrier(const image &img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                             VkAccessFlags dst);
// Barrière mémoire globale : écritures de [src] visibles par [dst].
void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage, VkAccessFlags src,
                    VkPipelineStageFlags dst_stage, VkAccessFlags dst);

// Libération des ressources d'un module (appelée par shutdown, GPU déjà inactif).
void add_shutdown_hook(void (*hook)());

void set_status(const std::string &s, bool failure);
bool check(VkResult r, const char *what);
bool check_hr(HRESULT hr, const char *what);
double now_ms();

}  // namespace akfsr::vk
