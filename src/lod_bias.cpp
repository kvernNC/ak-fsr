#include "lod_bias.hpp"

#include <reshade.hpp>

#include <atomic>
#include <cstdio>

using namespace reshade::api;

namespace akfsr::lod_bias {
namespace {

std::atomic<float> g_bias{0.0f};
std::atomic<uint32_t> g_modified{0}, g_skipped{0};

bool on_create_sampler(device *, sampler_desc &desc) {
  const float bias = g_bias.load(std::memory_order_relaxed);
  if (bias == 0.0f) return false;
  const auto f = static_cast<uint32_t>(desc.filter);
  // Textures du décor : filtrage min linéaire ou anisotrope, pas de comparaison (ombres), mips présents.
  if ((f & 0x80) != 0 || (f & 0x50) == 0 || desc.max_lod <= desc.min_lod) {
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  desc.mip_lod_bias += bias;
  g_modified.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace

void configure(float bias, bool fsr_mode) {
  const float applied = fsr_mode ? bias : 0.0f;
  g_bias = applied;
  if (applied != 0.0f) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "AK-FSR: biais de LOD des textures : %+.2f (samplers créés à partir de maintenant)",
                  applied);
    reshade::log::message(reshade::log::level::info, buf);
  }
}

void register_events() { reshade::register_event<reshade::addon_event::create_sampler>(on_create_sampler); }
void unregister_events() { reshade::unregister_event<reshade::addon_event::create_sampler>(on_create_sampler); }

stats get_stats() { return {g_bias.load(), g_modified.load(), g_skipped.load()}; }

}  // namespace akfsr::lod_bias
