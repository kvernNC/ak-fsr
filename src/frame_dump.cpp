#include "frame_dump.hpp"

#include <reshade.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "crc32.hpp"
#include "shader_patch.hpp"

using namespace reshade::api;

namespace akfsr::dump {
namespace {

// ---------------------------------------------------------------------------------------------
// Shaders : handle de pipeline D3D11 -> hash du bytecode d'origine.

struct shader_info {
  uint32_t hash = 0;
  char stage = '?';  // 'v', 'p', 'c', 'g', 'h', 'd'
  std::vector<uint8_t> code;
};

std::shared_mutex g_shaders_mutex;
std::unordered_map<uint64_t, shader_info> g_shaders;
std::unordered_map<uint64_t, std::string> g_blends;  // état de mélange (D3D11 : objet à part) -> description de RT0

// create_pipeline voit le bytecode avant un éventuel remplacement par RenoDX (chargé après nous,
// ordre alphabétique) ; init_pipeline reçoit le handle. On fait le lien par thread.
thread_local std::vector<shader_info> t_pending_shaders;

char stage_char(pipeline_subobject_type type) {
  switch (type) {
    case pipeline_subobject_type::vertex_shader: return 'v';
    case pipeline_subobject_type::pixel_shader: return 'p';
    case pipeline_subobject_type::compute_shader: return 'c';
    case pipeline_subobject_type::geometry_shader: return 'g';
    case pipeline_subobject_type::hull_shader: return 'h';
    case pipeline_subobject_type::domain_shader: return 'd';
    default: return 0;
  }
}

std::vector<shader_info> read_shaders(uint32_t count, const pipeline_subobject *subobjects) {
  std::vector<shader_info> result;
  for (uint32_t i = 0; i < count; ++i) {
    const char stage = stage_char(subobjects[i].type);
    if (stage == 0) continue;
    const auto *desc = static_cast<const shader_desc *>(subobjects[i].data);
    if (desc == nullptr || desc->code == nullptr || desc->code_size == 0) continue;
    const auto *bytes = static_cast<const uint8_t *>(desc->code);
    result.push_back({crc32(bytes, desc->code_size), stage, {bytes, bytes + desc->code_size}});
  }
  return result;
}

// Bytecodes patchés (M6) : doivent rester valides après la création du shader.
std::mutex g_patched_mutex;
std::vector<std::unique_ptr<std::vector<uint8_t>>> g_patched_code;

bool on_create_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects) {
  t_pending_shaders = read_shaders(count, subobjects);
  bool modified = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (stage_char(subobjects[i].type) == 0) continue;
    auto *desc = static_cast<shader_desc *>(subobjects[i].data);
    if (desc == nullptr || desc->code == nullptr || desc->code_size == 0) continue;
    const auto *bytes = static_cast<const uint8_t *>(desc->code);
    auto patched = std::make_unique<std::vector<uint8_t>>();
    if (!shader_patch::patch(crc32(bytes, desc->code_size), bytes, desc->code_size, *patched)) continue;
    desc->code = patched->data();
    desc->code_size = patched->size();
    std::lock_guard lock(g_patched_mutex);
    g_patched_code.push_back(std::move(patched));
    modified = true;
  }
  return modified;
}

void on_init_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline handle) {
  for (uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::blend_state || subobjects[i].data == nullptr) continue;
    const auto &b = *static_cast<const blend_desc *>(subobjects[i].data);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "blend=%d src=%d dst=%d op=%d mask=0x%X", b.blend_enable[0] ? 1 : 0,
                  static_cast<int>(b.source_color_blend_factor[0]), static_cast<int>(b.dest_color_blend_factor[0]),
                  static_cast<int>(b.color_blend_op[0]), static_cast<unsigned>(b.render_target_write_mask[0]));
    std::unique_lock lock(g_shaders_mutex);
    g_blends[handle.handle] = buf;
  }
  auto shaders = t_pending_shaders.empty() ? read_shaders(count, subobjects) : std::move(t_pending_shaders);
  t_pending_shaders.clear();
  if (shaders.size() != 1 || handle.handle == 0) return;  // D3D11 : un shader par objet
  std::unique_lock lock(g_shaders_mutex);
  g_shaders[handle.handle] = std::move(shaders[0]);
}

void on_destroy_pipeline(device *, pipeline handle) {
  std::unique_lock lock(g_shaders_mutex);
  g_shaders.erase(handle.handle);
}

// ---------------------------------------------------------------------------------------------
// Compteurs permanents (bon marché).

std::atomic<uint32_t> g_draws{0}, g_dispatches{0}, g_maps{0};
std::atomic<uint32_t> g_last_draws{0}, g_last_dispatches{0};

// ---------------------------------------------------------------------------------------------
// Capture.

enum class phase : int { idle, armed, warmup, capturing };
std::atomic<phase> g_phase{phase::idle};
std::atomic<command_list *> g_immediate{nullptr};
std::filesystem::path g_output_root;
std::mutex g_status_mutex;
std::string g_last_capture;

bool recording() {
  const auto p = g_phase.load(std::memory_order_relaxed);
  return p == phase::warmup || p == phase::capturing;
}

constexpr size_t kSrvSlots = 32, kUavSlots = 8, kCbSlots = 14, kRtvSlots = 8;

struct stage_bindings {
  uint64_t shader = 0;
  std::array<uint64_t, kSrvSlots> srv{};
  std::array<uint64_t, kUavSlots> uav{};
  std::array<uint64_t, kCbSlots> cb{};
};

struct cmd_state {
  stage_bindings vs, ps, cs;
  std::array<uint64_t, kRtvSlots> rtv{};
  uint32_t rtv_count = 0;
  uint64_t dsv = 0;
  uint64_t blend = 0;
  std::vector<std::string> lines;

  void reset_bindings() {
    vs = {}; ps = {}; cs = {};
    rtv = {}; rtv_count = 0; dsv = 0; blend = 0;
  }
};

std::mutex g_cmd_mutex;
std::unordered_map<command_list *, std::unique_ptr<cmd_state>> g_cmd;

cmd_state &state_of(command_list *cmd) {
  std::lock_guard lock(g_cmd_mutex);
  auto &slot = g_cmd[cmd];
  if (!slot) slot = std::make_unique<cmd_state>();
  return *slot;
}

// Shaders et CB réellement référencés pendant la capture (pour les fichiers annexes).
std::mutex g_used_mutex;
std::set<uint64_t> g_used_shaders;

// Snapshots de constant buffers : contenu au dernier unmap / update, dédupliqué.
std::mutex g_cb_mutex;
std::vector<std::vector<uint8_t>> g_cb_snapshots;
std::unordered_map<uint32_t, uint32_t> g_cb_by_crc;    // crc -> id
std::unordered_map<uint64_t, uint32_t> g_cb_latest;    // buffer -> id + 1
thread_local std::unordered_map<uint64_t, void *> t_mapped;

// Cache des descriptions de ressources (vidé à chaque capture : les handles sont réutilisés).
std::mutex g_desc_mutex;
std::unordered_map<uint64_t, resource_desc> g_desc_cache;

resource_desc desc_of(device *dev, resource res) {
  {
    std::lock_guard lock(g_desc_mutex);
    if (auto it = g_desc_cache.find(res.handle); it != g_desc_cache.end()) return it->second;
  }
  const auto desc = dev->get_resource_desc(res);
  std::lock_guard lock(g_desc_mutex);
  g_desc_cache[res.handle] = desc;
  return desc;
}

const char *format_name(format f) {
  switch (f) {
    case format::unknown: return "unknown";
    case format::r32g32b32a32_typeless: return "r32g32b32a32_typeless";
    case format::r32g32b32a32_float: return "r32g32b32a32_float";
    case format::r16g16b16a16_typeless: return "r16g16b16a16_typeless";
    case format::r16g16b16a16_float: return "r16g16b16a16_float";
    case format::r16g16b16a16_unorm: return "r16g16b16a16_unorm";
    case format::r16g16b16a16_snorm: return "r16g16b16a16_snorm";
    case format::r32g32_typeless: return "r32g32_typeless";
    case format::r32g32_float: return "r32g32_float";
    case format::r32_g8_typeless: return "r32_g8_typeless";
    case format::d32_float_s8_uint: return "d32_float_s8_uint";
    case format::r32_float_x8_uint: return "r32_float_x8_uint";
    case format::r10g10b10a2_typeless: return "r10g10b10a2_typeless";
    case format::r10g10b10a2_unorm: return "r10g10b10a2_unorm";
    case format::r11g11b10_float: return "r11g11b10_float";
    case format::r8g8b8a8_typeless: return "r8g8b8a8_typeless";
    case format::r8g8b8a8_unorm: return "r8g8b8a8_unorm";
    case format::r8g8b8a8_unorm_srgb: return "r8g8b8a8_unorm_srgb";
    case format::r16g16_typeless: return "r16g16_typeless";
    case format::r16g16_float: return "r16g16_float";
    case format::r16g16_unorm: return "r16g16_unorm";
    case format::r16g16_snorm: return "r16g16_snorm";
    case format::r32_typeless: return "r32_typeless";
    case format::d32_float: return "d32_float";
    case format::r32_float: return "r32_float";
    case format::r32_uint: return "r32_uint";
    case format::r24_g8_typeless: return "r24_g8_typeless";
    case format::d24_unorm_s8_uint: return "d24_unorm_s8_uint";
    case format::r24_unorm_x8_uint: return "r24_unorm_x8_uint";
    case format::x24_unorm_g8_uint: return "x24_unorm_g8_uint";
    case format::r8g8_unorm: return "r8g8_unorm";
    case format::r16_typeless: return "r16_typeless";
    case format::r16_float: return "r16_float";
    case format::d16_unorm: return "d16_unorm";
    case format::r16_unorm: return "r16_unorm";
    case format::r8_unorm: return "r8_unorm";
    case format::a8_unorm: return "a8_unorm";
    case format::b8g8r8a8_typeless: return "b8g8r8a8_typeless";
    case format::b8g8r8a8_unorm: return "b8g8r8a8_unorm";
    case format::b8g8r8a8_unorm_srgb: return "b8g8r8a8_unorm_srgb";
    case format::bc1_unorm: return "bc1_unorm";
    case format::bc3_unorm: return "bc3_unorm";
    case format::bc5_unorm: return "bc5_unorm";
    default: return nullptr;
  }
}

std::string format_str(format f) {
  if (const char *name = format_name(f)) return name;
  return "fmt" + std::to_string(static_cast<uint32_t>(f));
}

std::string describe_resource(device *dev, resource res) {
  if (res.handle == 0) return "null";
  const auto d = desc_of(dev, res);
  char buf[160];
  if (d.type == resource_type::buffer) {
    std::snprintf(buf, sizeof(buf), "r=%llx buffer %lluB", static_cast<unsigned long long>(res.handle),
                  static_cast<unsigned long long>(d.buffer.size));
  } else {
    std::snprintf(buf, sizeof(buf), "r=%llx %ux%u", static_cast<unsigned long long>(res.handle), d.texture.width,
                  d.texture.height);
    std::string s = buf;
    if (d.texture.depth_or_layers > 1) s += "x" + std::to_string(d.texture.depth_or_layers);
    if (d.texture.levels > 1) s += " mips=" + std::to_string(d.texture.levels);
    if (d.texture.samples > 1) s += " msaa=" + std::to_string(d.texture.samples);
    return s + " " + format_str(d.texture.format);
  }
  return buf;
}

std::string describe_view(device *dev, uint64_t view_handle) {
  if (view_handle == 0) return "null";
  const resource_view view{view_handle};
  const auto res = dev->get_resource_from_view(view);
  std::string s = describe_resource(dev, res);
  const auto vd = dev->get_resource_view_desc(view);
  const auto rd = desc_of(dev, res);
  if (rd.type != resource_type::buffer && vd.format != rd.texture.format) s += " view=" + format_str(vd.format);
  return s;
}

std::string shader_str(uint64_t handle) {
  if (handle == 0) return "-";
  {
    std::lock_guard lock(g_used_mutex);
    g_used_shaders.insert(handle);
  }
  std::shared_lock lock(g_shaders_mutex);
  auto it = g_shaders.find(handle);
  if (it == g_shaders.end()) return "?";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", it->second.hash);
  return buf;
}

void append_stage(std::string &out, device *dev, const char *name, const stage_bindings &b, bool with_uav) {
  char buf[64];
  for (size_t i = 0; i < kSrvSlots; ++i) {
    if (b.srv[i] == 0) continue;
    std::snprintf(buf, sizeof(buf), "\n    %s t%zu ", name, i);
    out += buf + describe_view(dev, b.srv[i]);
  }
  if (with_uav) {
    for (size_t i = 0; i < kUavSlots; ++i) {
      if (b.uav[i] == 0) continue;
      std::snprintf(buf, sizeof(buf), "\n    %s u%zu ", name, i);
      out += buf + describe_view(dev, b.uav[i]);
    }
  }
  for (size_t i = 0; i < kCbSlots; ++i) {
    if (b.cb[i] == 0) continue;
    uint32_t snap = 0;
    {
      std::lock_guard lock(g_cb_mutex);
      if (auto it = g_cb_latest.find(b.cb[i]); it != g_cb_latest.end()) snap = it->second;
    }
    std::snprintf(buf, sizeof(buf), "\n    %s b%zu ", name, i);
    out += buf + describe_resource(dev, resource{b.cb[i]});
    out += snap ? " cb#" + std::to_string(snap - 1) : " cb#?";
  }
}

void log_draw_like(command_list *cmd, const char *what, const std::string &args, bool compute) {
  auto &st = state_of(cmd);
  device *dev = cmd->get_device();
  std::string line = std::string(what) + " " + args;
  if (compute) {
    line += " CS=" + shader_str(st.cs.shader);
    append_stage(line, dev, "CS", st.cs, true);
  } else {
    line += " VS=" + shader_str(st.vs.shader) + " PS=" + shader_str(st.ps.shader);
    for (uint32_t i = 0; i < st.rtv_count && i < kRtvSlots; ++i) {
      if (st.rtv[i] == 0) continue;
      line += "\n    RT" + std::to_string(i) + " " + describe_view(dev, st.rtv[i]);
    }
    if (st.dsv != 0) line += "\n    DS " + describe_view(dev, st.dsv);
    {
      std::shared_lock lock(g_shaders_mutex);
      const auto it = g_blends.find(st.blend);
      line += "\n    OM " + (it != g_blends.end() ? it->second : std::string("blend=défaut"));
    }
    append_stage(line, dev, "VS", st.vs, false);
    append_stage(line, dev, "PS", st.ps, true);
  }
  st.lines.push_back(std::move(line));
}

void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline p) {
  if (!recording()) return;
  auto &st = state_of(cmd);
  if ((stages & pipeline_stage::vertex_shader) != 0) st.vs.shader = p.handle;
  if ((stages & pipeline_stage::pixel_shader) != 0) st.ps.shader = p.handle;
  if ((stages & pipeline_stage::compute_shader) != 0) st.cs.shader = p.handle;
  if ((stages & pipeline_stage::output_merger) != 0) st.blend = p.handle;
}

void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t,
                         const descriptor_table_update &update) {
  if (!recording()) return;
  auto &st = state_of(cmd);
  for (auto *b : {&st.vs, &st.ps, &st.cs}) {
    const shader_stage s = b == &st.vs ? shader_stage::vertex : b == &st.ps ? shader_stage::pixel : shader_stage::compute;
    if ((stages & s) == 0) continue;
    for (uint32_t i = 0; i < update.count; ++i) {
      const uint32_t slot = update.binding + i;
      switch (update.type) {
        case descriptor_type::shader_resource_view:
          if (slot < kSrvSlots) b->srv[slot] = static_cast<const resource_view *>(update.descriptors)[i].handle;
          break;
        case descriptor_type::unordered_access_view:
          if (slot < kUavSlots) b->uav[slot] = static_cast<const resource_view *>(update.descriptors)[i].handle;
          break;
        case descriptor_type::constant_buffer:
          if (slot < kCbSlots) b->cb[slot] = static_cast<const buffer_range *>(update.descriptors)[i].buffer.handle;
          break;
        default:
          break;
      }
    }
  }
}

void on_bind_rts(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv) {
  if (!recording()) return;
  auto &st = state_of(cmd);
  st.rtv = {};
  st.rtv_count = count < kRtvSlots ? count : static_cast<uint32_t>(kRtvSlots);
  for (uint32_t i = 0; i < st.rtv_count; ++i) st.rtv[i] = rtvs[i].handle;
  st.dsv = dsv.handle;
}

bool on_draw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
  g_draws.fetch_add(1, std::memory_order_relaxed);
  if (recording())
    log_draw_like(cmd, "DRAW", "n=" + std::to_string(vertices) + " inst=" + std::to_string(instances), false);
  return false;
}

bool on_draw_indexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
  g_draws.fetch_add(1, std::memory_order_relaxed);
  if (recording())
    log_draw_like(cmd, "DRAW_INDEXED", "n=" + std::to_string(indices) + " inst=" + std::to_string(instances), false);
  return false;
}

bool on_dispatch(command_list *cmd, uint32_t x, uint32_t y, uint32_t z) {
  g_dispatches.fetch_add(1, std::memory_order_relaxed);
  if (recording())
    log_draw_like(cmd, "DISPATCH", std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z), true);
  return false;
}

bool on_indirect(command_list *cmd, indirect_command type, resource, uint64_t, uint32_t, uint32_t) {
  const bool compute = type == indirect_command::dispatch;
  (compute ? g_dispatches : g_draws).fetch_add(1, std::memory_order_relaxed);
  if (recording()) log_draw_like(cmd, compute ? "DISPATCH_INDIRECT" : "DRAW_INDIRECT", "", compute);
  return false;
}

bool on_copy_resource(command_list *cmd, resource src, resource dst) {
  if (!recording()) return false;
  device *dev = cmd->get_device();
  state_of(cmd).lines.push_back("COPY " + describe_resource(dev, src) + "  ->  " + describe_resource(dev, dst));
  return false;
}

bool on_copy_texture_region(command_list *cmd, resource src, uint32_t src_sub, const subresource_box *, resource dst,
                            uint32_t dst_sub, const subresource_box *, filter_mode) {
  if (!recording()) return false;
  device *dev = cmd->get_device();
  state_of(cmd).lines.push_back("COPY_REGION " + describe_resource(dev, src) + " [" + std::to_string(src_sub) +
                                "]  ->  " + describe_resource(dev, dst) + " [" + std::to_string(dst_sub) + "]");
  return false;
}

bool on_clear_rtv(command_list *cmd, resource_view rtv, const float color[4], uint32_t, const rect *) {
  if (!recording()) return false;
  char buf[96];
  std::snprintf(buf, sizeof(buf), " (%g %g %g %g)", color[0], color[1], color[2], color[3]);
  state_of(cmd).lines.push_back("CLEAR_RTV " + describe_view(cmd->get_device(), rtv.handle) + buf);
  return false;
}

bool on_clear_dsv(command_list *cmd, resource_view dsv, const float *depth, const uint8_t *stencil, uint32_t,
                  const rect *) {
  if (!recording()) return false;
  std::string extra;
  if (depth) extra += " depth=" + std::to_string(*depth);
  if (stencil) extra += " stencil=" + std::to_string(*stencil);
  state_of(cmd).lines.push_back("CLEAR_DSV " + describe_view(cmd->get_device(), dsv.handle) + extra);
  return false;
}

// FinishCommandList : (proxy, deferred) — on transfère et l'état du deferred context repart de zéro.
// ExecuteCommandList : (immediate, proxy) — on copie (une liste peut être exécutée plusieurs fois).
void on_execute_secondary(command_list *cmd, command_list *secondary) {
  if (!recording()) return;
  auto &dst = state_of(cmd);
  auto &src = state_of(secondary);
  char buf[64];
  if (cmd == g_immediate.load()) {
    std::snprintf(buf, sizeof(buf), "-- execute command list %p (%zu)", static_cast<void *>(secondary), src.lines.size());
    dst.lines.emplace_back(buf);
    dst.lines.insert(dst.lines.end(), src.lines.begin(), src.lines.end());
    dst.lines.emplace_back("-- end command list");
  } else {
    std::snprintf(buf, sizeof(buf), "-- recorded on deferred context %p", static_cast<void *>(secondary));
    dst.lines.emplace_back(buf);
    dst.lines.insert(dst.lines.end(), std::make_move_iterator(src.lines.begin()),
                     std::make_move_iterator(src.lines.end()));
    src.lines.clear();
    src.reset_bindings();
  }
}

void on_destroy_command_list(command_list *cmd) {
  std::lock_guard lock(g_cmd_mutex);
  g_cmd.erase(cmd);
}

void snapshot_cb(uint64_t buffer, const void *data, size_t size) {
  const uint32_t crc = crc32(data, size);
  std::lock_guard lock(g_cb_mutex);
  uint32_t id;
  if (auto it = g_cb_by_crc.find(crc); it != g_cb_by_crc.end()) {
    id = it->second;
  } else {
    id = static_cast<uint32_t>(g_cb_snapshots.size());
    auto *bytes = static_cast<const uint8_t *>(data);
    g_cb_snapshots.emplace_back(bytes, bytes + size);
    g_cb_by_crc[crc] = id;
  }
  g_cb_latest[buffer] = id + 1;
}

constexpr uint64_t kMaxCbSize = 64 * 1024;

void on_map_buffer(device *dev, resource res, uint64_t, uint64_t, map_access access, void **data) {
  g_maps.fetch_add(1, std::memory_order_relaxed);
  if (!recording() || data == nullptr || *data == nullptr || access == map_access::read_only) return;
  const auto d = desc_of(dev, res);
  if ((d.usage & resource_usage::constant_buffer) == 0 || d.buffer.size > kMaxCbSize) return;
  t_mapped[res.handle] = *data;
}

void on_unmap_buffer(device *dev, resource res) {
  auto it = t_mapped.find(res.handle);
  if (it == t_mapped.end()) return;
  void *data = it->second;
  t_mapped.erase(it);
  if (recording()) snapshot_cb(res.handle, data, static_cast<size_t>(desc_of(dev, res).buffer.size));
}

bool on_update_buffer(device *dev, const void *data, resource dst, uint64_t offset, uint64_t size) {
  if (!recording() || data == nullptr) return false;
  const auto d = desc_of(dev, dst);
  if ((d.usage & resource_usage::constant_buffer) == 0 || d.buffer.size > kMaxCbSize) return false;
  // Les CB D3D11 sont toujours mis à jour en entier.
  if (offset == 0 && (size == UINT64_MAX || size >= d.buffer.size)) snapshot_cb(dst.handle, data, d.buffer.size);
  return false;
}

// ---------------------------------------------------------------------------------------------
// Écriture de la capture.

void clear_capture_data() {
  {
    std::lock_guard lock(g_cmd_mutex);
    for (auto &[cmd, st] : g_cmd) {
      st->lines.clear();
      st->reset_bindings();
    }
  }
  { std::lock_guard lock(g_used_mutex); g_used_shaders.clear(); }
  {
    std::lock_guard lock(g_cb_mutex);
    g_cb_snapshots.clear();
    g_cb_by_crc.clear();
    g_cb_latest.clear();
  }
  { std::lock_guard lock(g_desc_mutex); g_desc_cache.clear(); }
}

std::string write_capture(const std::vector<std::string> &lines, device *dev, swapchain *sc) {
  char stamp[48];
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  const size_t len = std::strftime(stamp, sizeof(stamp), "frame_%Y%m%d_%H%M%S", &tm);
  // Plusieurs captures par seconde possibles : suffixe pour ne pas écraser la précédente.
  std::error_code ec;
  for (int n = 1; std::filesystem::exists(g_output_root / stamp, ec); ++n)
    std::snprintf(stamp + len, sizeof(stamp) - len, "_%d", n);
  const auto dir = g_output_root / stamp;
  std::filesystem::create_directories(dir / "shaders", ec);
  if (ec) return "erreur : impossible de créer " + dir.string();

  uint32_t draws = 0, dispatches = 0;
  {
    std::ofstream f(dir / "frame.log");
    f << "# AK-FSR frame dump\n";
    if (sc != nullptr) f << "# backbuffer " << describe_resource(dev, sc->get_current_back_buffer()) << "\n";
    f << "# ordre d'exécution sur le contexte immédiat ; t=SRV u=UAV b=CB, cb#N -> cbuffers.txt\n\n";
    uint32_t index = 0;
    for (const auto &line : lines) {
      if (line.rfind("--", 0) == 0) {
        f << line << "\n";
        continue;
      }
      if (line.rfind("DRAW", 0) == 0) ++draws;
      if (line.rfind("DISPATCH", 0) == 0) ++dispatches;
      f << "[" << index++ << "] " << line << "\n";
    }
  }
  {
    std::ofstream f(dir / "cbuffers.txt");
    std::lock_guard lock(g_cb_mutex);
    for (size_t id = 0; id < g_cb_snapshots.size(); ++id) {
      const auto &snap = g_cb_snapshots[id];
      f << "cb#" << id << " (" << snap.size() << " B)\n";
      const auto *floats = reinterpret_cast<const float *>(snap.data());
      char buf[128];
      for (size_t row = 0; row * 16 + 16 <= snap.size(); ++row) {
        std::snprintf(buf, sizeof(buf), "  c%-3zu %14.6g %14.6g %14.6g %14.6g\n", row, floats[row * 4],
                      floats[row * 4 + 1], floats[row * 4 + 2], floats[row * 4 + 3]);
        f << buf;
      }
    }
  }
  size_t shader_count = 0;
  {
    std::set<uint64_t> used;
    { std::lock_guard lock(g_used_mutex); used = g_used_shaders; }
    std::shared_lock lock(g_shaders_mutex);
    for (uint64_t handle : used) {
      auto it = g_shaders.find(handle);
      if (it == g_shaders.end()) continue;
      char name[48];
      std::snprintf(name, sizeof(name), "%cs_0x%08X.cso", it->second.stage, it->second.hash);
      std::ofstream f(dir / "shaders" / name, std::ios::binary);
      f.write(reinterpret_cast<const char *>(it->second.code.data()), static_cast<std::streamsize>(it->second.code.size()));
      ++shader_count;
    }
  }

  char msg[256];
  std::snprintf(msg, sizeof(msg), "AK-FSR: capture %s : %u draws, %u dispatches, %zu shaders, %zu CB", stamp, draws,
                dispatches, shader_count, g_cb_snapshots.size());
  reshade::log::message(reshade::log::level::info, msg);
  return dir.string();
}

void on_present(command_queue *queue, swapchain *sc, const rect *, const rect *, uint32_t, const rect *) {
  g_last_draws = g_draws.exchange(0, std::memory_order_relaxed);
  g_last_dispatches = g_dispatches.exchange(0, std::memory_order_relaxed);
  command_list *imm = queue->get_immediate_command_list();
  g_immediate = imm;

  switch (g_phase.load()) {
    case phase::idle:
      break;
    case phase::armed:
      clear_capture_data();
      g_phase = phase::warmup;
      break;
    case phase::warmup:
      // Ne garder du contexte immédiat que la frame suivante ; les deferred contexts gardent ce
      // qu'ils ont enregistré depuis le début de la chauffe.
      state_of(imm).lines.clear();
      g_phase = phase::capturing;
      break;
    case phase::capturing: {
      std::vector<std::string> lines = std::move(state_of(imm).lines);
      g_phase = phase::idle;
      std::string result = write_capture(lines, queue->get_device(), sc);
      clear_capture_data();
      std::lock_guard lock(g_status_mutex);
      g_last_capture = std::move(result);
      break;
    }
  }
}

}  // namespace

std::string format_string(format f) { return format_str(f); }

std::string dump_game_image() {
  // Image de l'exécutable telle qu'en mémoire (sections déchiffrées par la DRM Steam), pour l'analyse
  // statique hors jeu (recherche de FPerspectiveMatrix). Pages illisibles remplies de zéros.
  auto *base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  const size_t size = nt->OptionalHeader.SizeOfImage;

  std::error_code ec;
  std::filesystem::create_directories(g_output_root, ec);
  const auto path = g_output_root / "BatmanAK_image.bin";
  std::ofstream f(path, std::ios::binary);
  if (!f) return "erreur : impossible d'écrire " + path.string();
  std::vector<uint8_t> zeros(4096, 0);
  size_t written = 0, readable = 0;
  while (written < size) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(base + written, &mbi, sizeof(mbi));
    size_t chunk = reinterpret_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize - (base + written);
    if (chunk == 0 || written + chunk > size) chunk = size - written;
    const bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    for (size_t off = 0; off < chunk; off += zeros.size()) {
      const size_t n = std::min(zeros.size(), chunk - off);
      f.write(ok ? reinterpret_cast<const char *>(base + written + off) : reinterpret_cast<const char *>(zeros.data()),
              static_cast<std::streamsize>(n));
    }
    if (ok) readable += chunk;
    written += chunk;
  }

  std::ofstream info(g_output_root / "BatmanAK_image.txt");
  char buf[256];
  std::snprintf(buf, sizeof(buf), "base 0x%llx\nsize 0x%zx (lisible 0x%zx)\nentry 0x%lx\n",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(base)), size, readable,
                static_cast<unsigned long>(nt->OptionalHeader.AddressOfEntryPoint));
  info << buf;
  const auto *sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    char name[9] = {};
    std::memcpy(name, sec->Name, 8);
    std::snprintf(buf, sizeof(buf), "section %-8s rva 0x%08lx taille 0x%08lx flags 0x%08lx\n", name,
                  static_cast<unsigned long>(sec->VirtualAddress), static_cast<unsigned long>(sec->Misc.VirtualSize),
                  static_cast<unsigned long>(sec->Characteristics));
    info << buf;
  }
  reshade::log::message(reshade::log::level::info, ("AK-FSR: image du jeu écrite : " + path.string()).c_str());
  return path.string();
}

void request_capture() {
  phase expected = phase::idle;
  if (g_phase.compare_exchange_strong(expected, phase::armed))
    reshade::log::message(reshade::log::level::info, "AK-FSR: capture de frame demandée");
}

uint32_t shader_hash(uint64_t pipeline_handle) {
  std::shared_lock lock(g_shaders_mutex);
  auto it = g_shaders.find(pipeline_handle);
  return it == g_shaders.end() ? 0 : it->second.hash;
}

stats get_stats() {
  stats s;
  s.draws = g_last_draws;
  s.dispatches = g_last_dispatches;
  s.buffer_maps = g_maps;
  { std::shared_lock lock(g_shaders_mutex); s.shaders_known = g_shaders.size(); }
  s.capture_busy = g_phase.load() != phase::idle;
  { std::lock_guard lock(g_status_mutex); s.last_capture = g_last_capture; }
  return s;
}

void register_events(const std::filesystem::path &output_root) {
  g_output_root = output_root;
  reshade::register_event<reshade::addon_event::create_pipeline>(on_create_pipeline);
  reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
  reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
  reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
  reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
  reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
  reshade::register_event<reshade::addon_event::draw>(on_draw);
  reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
  reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
  reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
  reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
  reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
  reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_rtv);
  reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_dsv);
  reshade::register_event<reshade::addon_event::execute_secondary_command_list>(on_execute_secondary);
  reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
  reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
  reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
  reshade::register_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
  reshade::register_event<reshade::addon_event::present>(on_present);
}

void unregister_events() {
  reshade::unregister_event<reshade::addon_event::create_pipeline>(on_create_pipeline);
  reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
  reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
  reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
  reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
  reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
  reshade::unregister_event<reshade::addon_event::draw>(on_draw);
  reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
  reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
  reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
  reshade::unregister_event<reshade::addon_event::copy_resource>(on_copy_resource);
  reshade::unregister_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
  reshade::unregister_event<reshade::addon_event::clear_render_target_view>(on_clear_rtv);
  reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_dsv);
  reshade::unregister_event<reshade::addon_event::execute_secondary_command_list>(on_execute_secondary);
  reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
  reshade::unregister_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
  reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
  reshade::unregister_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
  reshade::unregister_event<reshade::addon_event::present>(on_present);
}

}  // namespace akfsr::dump
