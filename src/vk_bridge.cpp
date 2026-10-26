#include "vk_bridge.hpp"

#include <cstdio>

#include <reshade.hpp>

#include "vk_context.hpp"

// M2 — test d'interop : couleur du jeu -> notre entrée (D3D11), entrée -> sortie (Vulkan), sortie ->
// couleur du jeu (D3D11).

namespace akfsr::vk {
namespace {

struct test_state {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0, height = 0;
  image in, out;
} t;

void destroy_test_images() {
  destroy_image(t.in);
  destroy_image(t.out);
  t = {};
}

bool ensure_test_images(DXGI_FORMAT format, uint32_t width, uint32_t height) {
  if (t.in.texture != nullptr && t.format == format && t.width == width && t.height == height) return true;
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    add_shutdown_hook(destroy_test_images);
  }
  if (t.in.handle != VK_NULL_HANDLE) wait_idle();
  destroy_test_images();
  if (!create_shared_image(t.in, format, width, height, 0) || !create_shared_image(t.out, format, width, height, 0)) {
    destroy_test_images();
    return false;
  }
  t.format = format;
  t.width = width;
  t.height = height;
  char buf[128];
  std::snprintf(buf, sizeof(buf), "AK-FSR: textures partagées créées : %ux%u, format DXGI %d", width, height,
                static_cast<int>(format));
  reshade::log::message(reshade::log::level::info, buf);
  return true;
}

}  // namespace

bool run_test(ID3D11Resource *color, test_mode mode) {
  if (!ready() || color == nullptr) return false;

  ID3D11Texture2D *color_tex = nullptr;
  if (FAILED(color->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&color_tex)))) {
    set_status("la couleur n'est pas une Texture2D", true);
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  color_tex->GetDesc(&desc);
  color_tex->Release();
  if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1) {
    set_status("couleur multisamplée ou en tableau", true);
    return false;
  }
  if (!ensure_test_images(desc.Format, desc.Width, desc.Height)) return false;

  const double t0 = now_ms();
  ctx().immediate->CopySubresourceRegion(t.in.texture, 0, 0, 0, 0, color, 0, nullptr);
  if (!flush_dxvk()) return false;

  VkCommandBuffer cmd = begin_commands();
  if (cmd == VK_NULL_HANDLE) return false;

  VkImageMemoryBarrier pre[2] = {
      barrier(t.in, kSharedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT),
      barrier(t.out, kSharedLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 2, pre);
  mark(cmd, 0);

  const VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  const uint32_t w = t.width, h = t.height;
  if (mode == test_mode::mirror_half) {
    const uint32_t half = w / 2;
    VkImageCopy halves[2] = {
        {sub, {0, 0, 0}, sub, {0, 0, 0}, {w - half, h, 1}},
        {sub, {0, 0, 0}, sub, {static_cast<int32_t>(w - half), 0, 0}, {half, h, 1}},
    };
    fn.vkCmdCopyImage(cmd, t.in.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.out.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, halves);
  } else {
    VkImageCopy full{sub, {0, 0, 0}, sub, {0, 0, 0}, {w, h, 1}};
    fn.vkCmdCopyImage(cmd, t.in.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.out.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &full);
  }

  VkImageMemoryBarrier post[2] = {
      barrier(t.in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kSharedLayout, 0, 0),
      barrier(t.out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kSharedLayout, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
  };
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                          nullptr, 2, post);

  if (!submit_commands(cmd, t0)) return false;
  ctx().immediate->CopySubresourceRegion(color, 0, 0, 0, 0, t.out.texture, 0, nullptr);
  return true;
}

}  // namespace akfsr::vk
