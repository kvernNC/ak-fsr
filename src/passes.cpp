#include "passes.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "frame_dump.hpp"
#include "insertion.hpp"
#include "jitter.hpp"
#include "shader_explorer.hpp"
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

// Pixel shader courant (M8.1 : passes SMAA du jeu), même règle d'accès.
struct ps_state {
  uint32_t hash = 0;
  uint64_t srv1 = 0;
} g_ps;

// M8.1 : option (copie atomique des réglages) ; désarmé après un échec, jusqu'au prochain changement.
std::atomic<bool> g_smaa_option{false};
std::atomic<bool> g_smaa_disarmed{false};
// CB View déjà capturé : il n'est appris qu'à la passe d'insertion ou de vélocité, après le SMAA dans la frame.
std::atomic<bool> g_view_known{false};

bool smaa_insertion_armed() {
  return g_smaa_option.load(std::memory_order_relaxed) && !g_smaa_disarmed.load(std::memory_order_relaxed) &&
         g_view_known.load(std::memory_order_relaxed) && insertion::fsr_active();
}

struct frame_flags {
  bool insertion = false, smaa = false, velocity = false, view_cb = false, tonemap = false;
  bool fsr_done = false;      // FSR a déjà tourné cette frame (au SMAA, M8.1)
  bool smaa_skipped = false;  // passes SMAA du jeu sautées cette frame
  uint64_t edge_depth = 0;    // profondeur lue par SMAAEdgeDetection (t1)
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
  if (!is_immediate(cmd)) return;
  if ((stages & pipeline_stage::compute_shader) != 0) g_cs.hash = p.handle != 0 ? dump::shader_hash(p.handle) : 0;
  if ((stages & pipeline_stage::pixel_shader) != 0 &&
      (g_smaa_option.load(std::memory_order_relaxed) || explorer::enabled()))
    g_ps.hash = p.handle != 0 ? dump::shader_hash(p.handle) : 0;
}

// M8.1 : SMAAEdgeDetection et SMAABlendingWeightCalculation sont sautées quand FSR remplace le SMAA.
bool skip_smaa_draw() {
  if (!g_smaa_option.load(std::memory_order_relaxed)) return false;
  if (g_ps.hash != kSmaaEdgePs && g_ps.hash != kSmaaWeightsPs) return false;
  if (g_ps.hash == kSmaaEdgePs) g_current.edge_depth = g_ps.srv1;
  if (!smaa_insertion_armed()) return false;
  g_current.smaa_skipped = true;
  return true;
}

bool on_draw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
  if (!is_immediate(cmd)) return false;
  const bool hidden = explorer::on_draw(g_ps.hash, vertices * instances);
  return skip_smaa_draw() || hidden;
}
bool on_draw_indexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
  if (!is_immediate(cmd)) return false;
  const bool hidden = explorer::on_draw(g_ps.hash, indices * instances);
  return skip_smaa_draw() || hidden;
}

void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t,
                         const descriptor_table_update &update) {
  if (!is_immediate(cmd)) return;
  if ((stages & shader_stage::pixel) != 0 && g_smaa_option.load(std::memory_order_relaxed) && update.type == descriptor_type::shader_resource_view &&
      update.binding <= 1 && update.binding + update.count > 1)
    g_ps.srv1 = static_cast<const resource_view *>(update.descriptors)[1 - update.binding].handle;
  if ((stages & shader_stage::compute) == 0) return;
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

uint64_t previous_velocity() {
  std::lock_guard lock(g_report_mutex);
  // Seulement si la passe de vélocité a tourné à la frame précédente : après une vidéo ou un chargement,
  // la texture a pu être détruite (pointeur pendant -> crash au retour en jeu).
  return g_report.velocity.found ? g_report.velocity_objects.handle : 0;
}

// M8.1 : FSR à la place de SMAANeighborhoodBlending (t0 couleur linéaire -> u0). true : dispatch sauté.
bool replace_smaa(command_list *cmd, device *dev) {
  const auto color = resolve_view(dev, g_cs.srv[0]);
  const auto output = resolve_view(dev, g_cs.uav[0]);
  // Profondeur : celle de SMAAEdgeDetection si elle a le format de celle de l'insertion habituelle.
  found_resource depth;
  {
    std::lock_guard lock(g_report_mutex);
    depth = g_report.insertion_depth;
  }
  bool edge_depth = false;
  if (g_current.edge_depth != 0) {
    const auto edge = resolve_view(dev, g_current.edge_depth);
    if (edge.handle != 0 && (depth.handle == 0 || edge.desc.texture.format == depth.desc.texture.format)) {
      depth = edge;
      edge_depth = true;
    }
  }
  fsr::view_data view{};
  bool view_valid;
  {
    std::lock_guard lock(g_view_mutex);
    view = g_view_head;
    view_valid = g_view_valid;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    char buf[200];
    std::snprintf(buf, sizeof(buf), "AK-FSR: insertion au SMAA : couleur %s %ux%u -> sortie %s, profondeur %s",
                  dump::format_string(color.desc.texture.format).c_str(), color.desc.texture.width,
                  color.desc.texture.height, dump::format_string(output.desc.texture.format).c_str(),
                  edge_depth ? "de SMAAEdgeDetection" : "de l'insertion 0xB2A276FD");
    reshade::log::message(reshade::log::level::info, buf);
  }
  // Entrées pas encore connues (début de partie, retour de vidéo) : repli pour cette frame seulement.
  const bool ready = color.handle != 0 && output.handle != 0 && depth.handle != 0 && view_valid;
  if (ready && insertion::before_post_pass(cmd, resource{color.handle}, resource{depth.handle},
                                           resource{previous_velocity()}, &view, resource{output.handle})) {
    g_current.fsr_done = true;
    return true;
  }
  // Échec de FSR : désarmé. Dans les deux cas, sortie = entrée (sans AA) si possible, puis FSR au point
  // d'insertion habituel.
  if (ready) {
    g_smaa_disarmed = true;
    reshade::log::message(reshade::log::level::warning,
                          "AK-FSR: insertion au SMAA en échec, repli sur 0xB2A276FD (option désarmée)");
  }
  if (!g_current.smaa_skipped) return false;  // SMAA complet : on le laisse tourner
  if (color.handle != 0 && output.handle != 0 && color.desc.texture.format == output.desc.texture.format &&
      color.desc.texture.width == output.desc.texture.width && color.desc.texture.height == output.desc.texture.height) {
    reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native())
        ->CopySubresourceRegion(reinterpret_cast<ID3D11Resource *>(output.handle), 0, 0, 0, 0,
                                reinterpret_cast<ID3D11Resource *>(color.handle), 0, nullptr);
    return true;
  }
  return false;  // SMAA avec des poids périmés pendant une frame
}

// M8.5 : première SRV 1×1 du tonemapper = exposition du jeu (adaptation de l'œil).
void note_exposure(device *dev) {
  for (int slot = 0; slot < static_cast<int>(g_cs.srv.size()); ++slot) {
    const auto res = resolve_view(dev, g_cs.srv[slot]);
    if (res.desc.type != resource_type::texture_2d || res.desc.texture.width != 1 || res.desc.texture.height != 1)
      continue;
    std::lock_guard lock(g_report_mutex);
    // Le jeu alterne deux textures d'exposition (double tampon) : journaliser chaque texture une seule fois.
    static std::array<uint64_t, 8> seen{};
    static size_t seen_count = 0;
    if (std::find(seen.begin(), seen.end(), res.handle) == seen.end()) {
      seen[seen_count++ % seen.size()] = res.handle;
      char buf[160];
      std::snprintf(buf, sizeof(buf), "AK-FSR: tonemap 0x%08X : texture 1x1 %s en t%d (exposition du jeu ?)", g_cs.hash,
                    dump::format_string(res.desc.texture.format).c_str(), slot);
      reshade::log::message(reshade::log::level::info, buf);
    }
    g_report.exposure = res;
    g_report.exposure_slot = slot;
    return;
  }
}

bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t) {
  if (!is_immediate(cmd) || g_cs.hash == 0) return false;
  device *dev = cmd->get_device();
  if (shader_patch::needs_jitter_cb(g_cs.hash)) bind_jitter_cb(dev, cmd);
  if (g_cs.hash == kInsertionCs) {
    g_current.insertion = true;
    explorer::on_insertion();
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
    const uint64_t velocity = previous_velocity();
    if (!g_current.fsr_done)
      insertion::before_post_pass(cmd, resource{color.handle}, resource{depth.handle}, resource{velocity},
                                  view_valid ? &view : nullptr);
  } else if (g_cs.hash == kSmaaCs) {
    g_current.smaa = true;
    if (smaa_insertion_armed()) return replace_smaa(cmd, dev);
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
    note_exposure(dev);
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
  g_view_known = true;
  t_view_cb_mapped = nullptr;
}

bool on_update_buffer(device *, const void *data, resource dst, uint64_t offset, uint64_t size) {
  if (dst.handle != g_view_cb_handle.load(std::memory_order_relaxed) || data == nullptr || offset != 0 ||
      (size != UINT64_MAX && size < sizeof(g_view_head)))
    return false;
  std::lock_guard lock(g_view_mutex);
  std::memcpy(g_view_head.data(), data, sizeof(g_view_head));
  g_view_valid = true;
  g_view_known = true;
  return false;
}

// M8.3 : Luma repère la vélocité du jeu comme la cible R16G16_FLOAT effacée ; on la compare à RT4.
bool on_clear_rtv(command_list *cmd, resource_view rtv, const float[4], uint32_t, const rect *) {
  if (!is_immediate(cmd)) return false;
  const auto res = resolve_view(cmd->get_device(), rtv.handle);
  if (res.desc.type != resource_type::texture_2d || res.desc.texture.format != format::r16g16_float) return false;
  std::lock_guard lock(g_report_mutex);
  if (g_report.cleared_rg16.handle == res.handle) return false;
  g_report.cleared_rg16 = res;
  char buf[160];
  std::snprintf(buf, sizeof(buf), "AK-FSR: cible R16G16_FLOAT effacée : %ux%u, %s", res.desc.texture.width,
                res.desc.texture.height,
                res.handle == g_report.velocity_objects.handle ? "c'est RT4 (vélocité des objets)" : "autre que RT4");
  reshade::log::message(reshade::log::level::info, buf);
  return false;
}

// Oublie immédiatement les ressources repérées qui sont libérées par le jeu.
void on_destroy_resource(device *, resource res) {
  std::lock_guard lock(g_report_mutex);
  for (auto *r : {&g_report.velocity_objects, &g_report.velocity_combined, &g_report.depth, &g_report.scene_color,
                  &g_report.insertion_depth, &g_report.cleared_rg16, &g_report.exposure}) {
    if (r->handle == res.handle) *r = {};
  }
}

void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *) {
  g_immediate = queue->get_immediate_command_list();
  explorer::on_present();
  {
    std::lock_guard lock(g_report_mutex);
    ++g_report.frames;
    update_status(g_report.insertion, g_current.insertion, "insertion (0xB2A276FD)");
    update_status(g_report.smaa, g_current.smaa, "SMAA du jeu (0x42C0137E, optionnelle)");
    using st = frame_report::smaa_state;
    if (g_current.fsr_done) ++g_report.smaa_fsr_frames;
    g_report.smaa_insertion = g_smaa_disarmed.load()                        ? st::disarmed
                              : !g_smaa_option.load()                         ? st::off
                              : g_current.fsr_done                            ? st::active
                                                                              : st::fallback;
    update_status(g_report.velocity, g_current.velocity, "vélocité (0xF98BC308)");
    update_status(g_report.view_cb, g_current.view_cb, "CB View (608 B)");
    update_status(g_report.tonemap, g_current.tonemap, "tonemap");
    g_report.insertion.hash = kInsertionCs;
    g_report.smaa.hash = kSmaaCs;
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
void set_smaa_insertion(bool enabled) {
  if (enabled != g_smaa_option.exchange(enabled)) g_smaa_disarmed = false;
}
bool shader_patches_enabled() { return g_patches_enabled.load(); }

frame_report get_report() {
  std::lock_guard lock(g_report_mutex);
  return g_report;
}

void register_events() {
  reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
  reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
  reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
  reshade::register_event<reshade::addon_event::draw>(on_draw);
  reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
  reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
  reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
  reshade::register_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
  reshade::register_event<reshade::addon_event::present>(on_present);
  reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
  reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_rtv);
}

void unregister_events() {
  if (g_jitter_cb != nullptr) {
    g_jitter_cb->Release();
    g_jitter_cb = nullptr;
  }
  reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
  reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
  reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
  reshade::unregister_event<reshade::addon_event::draw>(on_draw);
  reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
  reshade::unregister_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
  reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
  reshade::unregister_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
  reshade::unregister_event<reshade::addon_event::present>(on_present);
  reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
  reshade::unregister_event<reshade::addon_event::clear_render_target_view>(on_clear_rtv);
}

}  // namespace akfsr::passes
