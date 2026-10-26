#include "insertion.hpp"

#include <atomic>
#include <mutex>

#include "dxvk_interop.hpp"
#include "jitter.hpp"
#include "passes.hpp"
#include "vk_bridge.hpp"

using namespace reshade::api;

namespace akfsr::insertion {
namespace {

std::atomic<mode> g_mode{mode::fsr};
std::atomic<bool> g_logged_failure{false};
std::atomic<uint32_t> g_toggle_key{0};
std::atomic<bool> g_bypass{false};
std::mutex g_settings_mutex;
fsr::settings g_fsr_settings;

void on_destroy_device(device *) { vk::shutdown(); }

// Réglages persistants : section [AK-FSR] de ReShade.ini (configuration globale).
constexpr const char *kSection = "AK-FSR";

void save_settings() {
  const fsr::settings s = get_fsr_settings();
  reshade::set_config_value(nullptr, kSection, "Mode", static_cast<int>(g_mode.load()));
  reshade::set_config_value(nullptr, kSection, "Jitter", s.jitter);
  reshade::set_config_value(nullptr, kSection, "ShaderPatches", passes::shader_patches_enabled());
  reshade::set_config_value(nullptr, kSection, "AutoExposure", s.auto_exposure);
  reshade::set_config_value(nullptr, kSection, "Sharpness", s.sharpness);
  reshade::set_config_value(nullptr, kSection, "PrevMatrix", static_cast<int>(s.prev_source));
  reshade::set_config_value(nullptr, kSection, "ObjectVelocity", s.object_velocity);
  reshade::set_config_value(nullptr, kSection, "Version", s.version_id);
  reshade::set_config_value(nullptr, kSection, "ToggleKey", g_toggle_key.load());
}

void load_settings() {
  int m = static_cast<int>(g_mode.load());
  reshade::get_config_value(nullptr, kSection, "Mode", m);
  if (m >= 0 && m <= static_cast<int>(mode::fsr)) g_mode = static_cast<mode>(m);
  fsr::settings s;
  reshade::get_config_value(nullptr, kSection, "Jitter", s.jitter);
  reshade::get_config_value(nullptr, kSection, "AutoExposure", s.auto_exposure);
  reshade::get_config_value(nullptr, kSection, "Sharpness", s.sharpness);
  int prev = static_cast<int>(s.prev_source);
  reshade::get_config_value(nullptr, kSection, "PrevMatrix", prev);
  if (prev >= 0 && prev <= static_cast<int>(fsr::prev_matrix::memory)) s.prev_source = static_cast<fsr::prev_matrix>(prev);
  reshade::get_config_value(nullptr, kSection, "Version", s.version_id);
  reshade::get_config_value(nullptr, kSection, "ObjectVelocity", s.object_velocity);
  {
    std::lock_guard lock(g_settings_mutex);
    g_fsr_settings = s;
  }
  uint32_t key = 0;
  reshade::get_config_value(nullptr, kSection, "ToggleKey", key);
  g_toggle_key = key;
  bool patches = true;
  reshade::get_config_value(nullptr, kSection, "ShaderPatches", patches);
  passes::set_shader_patches_enabled(patches);
}

}  // namespace

void before_post_pass(command_list *cmd, resource scene_color, resource depth, resource velocity,
                      const fsr::view_data *view_cb) {
  const mode m = g_bypass.load(std::memory_order_relaxed) ? mode::off : g_mode.load(std::memory_order_relaxed);
  if (m == mode::off || scene_color.handle == 0) {
    jitter::set_enabled(false);
    return;
  }

  device *dev = cmd->get_device();
  if (!vk::init(reinterpret_cast<ID3D11Device *>(dev->get_native()))) {
    if (!g_logged_failure.exchange(true)) {
      reshade::log::message(reshade::log::level::error, ("AK-FSR: interop Vulkan indisponible : " + vk::status()).c_str());
    }
    return;
  }
  static bool logged_ready = false;
  if (!logged_ready) {
    logged_ready = true;
    reshade::log::message(reshade::log::level::info, ("AK-FSR: interop Vulkan " + vk::status()).c_str());
  }

  auto *color = reinterpret_cast<ID3D11Resource *>(scene_color.handle);
  jitter::set_enabled(m == mode::fsr && get_fsr_settings().jitter);
  if (m == mode::fsr) {
    if (depth.handle == 0 || view_cb == nullptr) return;
    fsr::frame_inputs in;
    in.color = color;
    in.depth = reinterpret_cast<ID3D11Resource *>(depth.handle);
    in.velocity = reinterpret_cast<ID3D11Resource *>(velocity.handle);
    in.view = *view_cb;
    fsr::run(in, get_fsr_settings());  // les échecs sont journalisés par fsr
    return;
  }

  const auto test = m == mode::mirror_half ? vk::test_mode::mirror_half : vk::test_mode::roundtrip;
  if (!vk::run_test(color, test) && !g_logged_failure.exchange(true)) {
    reshade::log::message(reshade::log::level::error, ("AK-FSR: test interop en échec : " + vk::status()).c_str());
  }
}

mode get_mode() { return g_mode.load(); }
void set_mode(mode m) {
  g_mode = m;
  save_settings();
}

fsr::settings get_fsr_settings() {
  std::lock_guard lock(g_settings_mutex);
  return g_fsr_settings;
}

void set_fsr_settings(const fsr::settings &s) {
  {
    std::lock_guard lock(g_settings_mutex);
    g_fsr_settings = s;
  }
  save_settings();
}

void set_shader_patches_enabled(bool enabled) {
  passes::set_shader_patches_enabled(enabled);
  save_settings();
}

uint32_t get_toggle_key() { return g_toggle_key.load(); }
void set_toggle_key(uint32_t vk) {
  g_toggle_key = vk;
  if (vk == 0) g_bypass = false;
  save_settings();
}

void toggle_bypass() {
  const bool off = !g_bypass.load();
  g_bypass = off;
  reshade::log::message(reshade::log::level::info,
                        off ? "AK-FSR: touche on/off : AK-FSR désactivé" : "AK-FSR: touche on/off : AK-FSR réactivé");
}
bool bypassed() { return g_bypass.load(); }

void register_events() {
  load_settings();
  reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
}
void unregister_events() { reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device); }

}  // namespace akfsr::insertion
