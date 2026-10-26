#include "passes.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "frame_dump.hpp"
#include "insertion.hpp"
#include "jitter.hpp"
#include "shader_patch.hpp"

using namespace reshade::api;

namespace akfsr::passes {
namespace {

// État du compute shader courant sur le contexte immédiat (les deferred contexts sont ignorés).
struct cs_state {
  uint32_t hash = 0;
  std::array<uint64_t, 8> srv{};
  std::array<uint64_t, 2> uav{};
  uint64_t cb1 = 0;
};

std::atomic<command_list *> g_immediate{nullptr};
cs_state g_cs;  // accédé uniquement depuis le thread du contexte immédiat

struct frame_flags {
  bool insertion = false, fog = false, velocity = false, view_cb = false, tonemap = false;
  uint32_t tonemap_hash = 0;
} g_current;

std::mutex g_report_mutex;
frame_report g_report;

// Contenu du CB View : capturé à l'unmap de ce buffer précis.
std::atomic<uint64_t> g_view_cb_handle{0};
thread_local void *t_view_cb_mapped = nullptr;
std::mutex g_view_mutex;
fsr::view_data g_view_head{};
bool g_view_valid = false;

bool is_immediate(command_list *cmd) { return cmd == g_immediate.load(std::memory_order_relaxed); }

found_resource resolve_view(device *dev, uint64_t view) {
  if (view == 0) return {};
  const auto res = dev->get_resource_from_view(resource_view{view});
  return {res.handle, dev->get_resource_desc(res)};
}

void log_transition(const char *name, bool found) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "AK-FSR: passe %s %s", name, found ? "trouvée" : "perdue");
  reshade::log::message(found ? reshade::log::level::info : reshade::log::level::warning, buf);
}

void update_status(pass_status &s, bool found_now, const char *name) {
  if (found_now) {
    ++s.frames_found;
  } else if (s.frames_found > 0) {
    ++s.frames_missed;
  }
  if (found_now != s.found) log_transition(name, found_now);
  s.found = found_now;
}

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline p) {
  if ((stages & pipeline_stage::compute_shader) == 0 || !is_immediate(cmd)) return;
  g_cs.hash = p.handle != 0 ? dump::shader_hash(p.handle) : 0;
}

void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t,
                         const descriptor_table_update &update) {
  if ((stages & shader_stage::compute) == 0 || !is_immediate(cmd)) return;
  for (uint32_t i = 0; i < update.count; ++i) {
    const uint32_t slot = update.binding + i;
    switch (update.type) {
      case descriptor_type::shader_resource_view:
        if (slot < g_cs.srv.size()) g_cs.srv[slot] = static_cast<const resource_view *>(update.descriptors)[i].handle;
        break;
      case descriptor_type::unordered_access_view:
        if (slot < g_cs.uav.size()) g_cs.uav[slot] = static_cast<const resource_view *>(update.descriptors)[i].handle;
        break;
      case descriptor_type::constant_buffer:
        if (slot == 1) g_cs.cb1 = static_cast<const buffer_range *>(update.descriptors)[i].buffer.handle;
        break;
      default:
        break;
    }
  }
}

void note_view_cb(device *dev) {
  if (g_cs.cb1 == 0) return;
  const auto desc = dev->get_resource_desc(resource{g_cs.cb1});
  if (desc.type != resource_type::buffer || desc.buffer.size != kViewCbSize) return;
  g_current.view_cb = true;
  g_view_cb_handle = g_cs.cb1;
  std::lock_guard lock(g_report_mutex);
  g_report.view_cb_buffer = {g_cs.cb1, desc};
}

// M6 : jitter (pixels) des shaders patchés, lié en b12 juste avant leur dispatch.
std::atomic<bool> g_patches_enabled{true};
ID3D11Buffer *g_jitter_cb = nullptr;

void bind_jitter_cb(device *dev, command_list *cmd) {
  auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
  if (g_jitter_cb == nullptr) {
    D3D11_BUFFER_DESC desc{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(reinterpret_cast<ID3D11Device *>(dev->get_native())->CreateBuffer(&desc, nullptr, &g_jitter_cb))) return;
  }
  float jitter[4] = {};
  uint32_t w, h;
  {
    std::lock_guard lock(g_report_mutex);
    w = g_report.scene_color.desc.texture.width;
    h = g_report.scene_color.desc.texture.height;
  }
  if (g_patches_enabled.load() && w != 0 && h != 0) {
    std::lock_guard lock(g_view_mutex);
    if (g_view_valid) {
      float jx, jy;
      jitter::recover_from_view(g_view_head.data(), jx, jy);
      jitter[2] = jx * static_cast<float>(w) * 0.5f;   // même convention que le jitter transmis à FSR
      jitter[3] = -jy * static_cast<float>(h) * 0.5f;
    }
  }
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(ctx->Map(g_jitter_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
  std::memcpy(mapped.pData, jitter, sizeof(jitter));
  ctx->Unmap(g_jitter_cb, 0);
  ctx->CSSetConstantBuffers(shader_patch::kJitterCbSlot, 1, &g_jitter_cb);
}

bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t) {
  if (!is_immediate(cmd) || g_cs.hash == 0) return false;
  device *dev = cmd->get_device();
  if (shader_patch::needs_jitter_cb(g_cs.hash)) bind_jitter_cb(dev, cmd);
  if (g_cs.hash == kInsertionCs) {
    g_current.insertion = true;
    note_view_cb(dev);
    const auto depth = resolve_view(dev, g_cs.srv[0]);
    const auto color = resolve_view(dev, g_cs.srv[1]);
    {
      std::lock_guard lock(g_report_mutex);
      g_report.scene_color = color;
      g_report.insertion_depth = depth;
    }
    // Contenu du CB View au moment de ce dispatch (dernier Unmap avant lui).
    fsr::view_data view{};
    bool view_valid;
    {
      std::lock_guard lock(g_view_mutex);
      view = g_view_head;
      view_valid = g_view_valid && g_view_cb_handle.load() == g_cs.cb1;
    }
    // RT4 du G-buffer (vélocité des objets), repérée comme t1 de la passe de vélocité à la frame précédente.
    uint64_t velocity;
    {
      std::lock_guard lock(g_report_mutex);
      // Seulement si la passe de vélocité a tourné à la frame précédente : après une vidéo ou un chargement,
      // la texture a pu être détruite (pointeur pendant -> crash au retour en jeu).
      velocity = g_report.velocity.found ? g_report.velocity_objects.handle : 0;
    }
    insertion::before_post_pass(cmd, resource{color.handle}, resource{depth.handle}, resource{velocity},
                                view_valid ? &view : nullptr);
  } else if (g_cs.hash == kFogCs) {
    g_current.fog = true;
  } else if (g_cs.hash == kVelocityCs) {
    g_current.velocity = true;
    note_view_cb(dev);
    const auto depth = resolve_view(dev, g_cs.srv[0]);
    const auto objects = resolve_view(dev, g_cs.srv[1]);
    const auto combined = resolve_view(dev, g_cs.uav[0]);
    std::lock_guard lock(g_report_mutex);
    g_report.depth = depth;
    g_report.velocity_objects = objects;
    g_report.velocity_combined = combined;
  } else if (std::find(kTonemapCs.begin(), kTonemapCs.end(), g_cs.hash) != kTonemapCs.end()) {
    g_current.tonemap = true;
    g_current.tonemap_hash = g_cs.hash;
  }
  return false;
}

void on_map_buffer(device *, resource res, uint64_t, uint64_t, map_access access, void **data) {
  if (res.handle != g_view_cb_handle.load(std::memory_order_relaxed) || data == nullptr ||
      access == map_access::read_only)
    return;
  t_view_cb_mapped = *data;
}

void on_unmap_buffer(device *, resource res) {
  if (t_view_cb_mapped == nullptr || res.handle != g_view_cb_handle.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_view_mutex);
  std::memcpy(g_view_head.data(), t_view_cb_mapped, sizeof(g_view_head));
  g_view_valid = true;
  t_view_cb_mapped = nullptr;
}

bool on_update_buffer(device *, const void *data, resource dst, uint64_t offset, uint64_t size) {
  if (dst.handle != g_view_cb_handle.load(std::memory_order_relaxed) || data == nullptr || offset != 0 ||
      (size != UINT64_MAX && size < sizeof(g_view_head)))
    return false;
  std::lock_guard lock(g_view_mutex);
  std::memcpy(g_view_head.data(), data, sizeof(g_view_head));
  g_view_valid = true;
  return false;
}

// Oublie immédiatement les ressources repérées qui sont libérées par le jeu.
void on_destroy_resource(device *, resource res) {
  std::lock_guard lock(g_report_mutex);
  for (auto *r : {&g_report.velocity_objects, &g_report.velocity_combined, &g_report.depth, &g_report.scene_color,
                  &g_report.insertion_depth}) {
    if (r->handle == res.handle) *r = {};
  }
}

void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *) {
  g_immediate = queue->get_immediate_command_list();
  {
    std::lock_guard lock(g_report_mutex);
    ++g_report.frames;
    update_status(g_report.insertion, g_current.insertion, "insertion (0xB2A276FD)");
    update_status(g_report.fog, g_current.fog, "brouillard (0x42C0137E, optionnelle)");
    update_status(g_report.velocity, g_current.velocity, "vélocité (0xF98BC308)");
    update_status(g_report.view_cb, g_current.view_cb, "CB View (608 B)");
    update_status(g_report.tonemap, g_current.tonemap, "tonemap");
    g_report.insertion.hash = kInsertionCs;
    g_report.fog.hash = kFogCs;
    g_report.velocity.hash = kVelocityCs;
    if (g_current.tonemap) g_report.tonemap.hash = g_current.tonemap_hash;
    std::lock_guard view_lock(g_view_mutex);
    std::copy_n(g_view_head.begin(), g_report.view_cb_head.size(), g_report.view_cb_head.begin());
    g_report.view_cb_valid = g_view_valid;
  }
  g_current = {};
}

}  // namespace

void set_shader_patches_enabled(bool enabled) { g_patches_enabled = enabled; }
bool shader_patches_enabled() { return g_patches_enabled.load(); }

frame_report get_report() {
  std::lock_guard lock(g_report_mutex);
  return g_report;
}

void register_events() {
  reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
  reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
  reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
  reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
  reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
  reshade::register_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
  reshade::register_event<reshade::addon_event::present>(on_present);
  reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
}

void unregister_events() {
  if (g_jitter_cb != nullptr) {
    g_jitter_cb->Release();
    g_jitter_cb = nullptr;
  }
  reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
  reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
  reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
  reshade::unregister_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
  reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
  reshade::unregister_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
  reshade::unregister_event<reshade::addon_event::present>(on_present);
  reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
}

}  // namespace akfsr::passes
