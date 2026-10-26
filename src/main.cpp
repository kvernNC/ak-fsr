// AK-FSR — addon ReShade : AA temporel FSR pour Batman: Arkham Knight (Proton / Steam Deck).
// FSR 3.1 en AA natif au point d'insertion (Vulkan via l'interop DXVK), jitter, vélocité des objets.

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <cstdio>
#include <filesystem>
#include <utility>

#include "frame_dump.hpp"
#include "highlights.hpp"
#include "insertion.hpp"
#include "jitter.hpp"
#include "passes.hpp"
#include "vk_bridge.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char *NAME = "AK-FSR";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "AA temporel FSR pour Batman: Arkham Knight (v" AKFSR_VERSION ", " AKFSR_MILESTONE ")";

namespace {

constexpr uint32_t kDumpFrameKey = VK_F10;

struct swapchain_info {
  uint32_t width = 0;
  uint32_t height = 0;
  reshade::api::format format = reshade::api::format::unknown;
  bool valid = false;
} g_swapchain;

template <typename... Args>
void log_fmt(reshade::log::level level, const char *fmt, Args... args) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  reshade::log::message(level, buf);
}

std::filesystem::path module_dir(HMODULE module) {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize) {
  const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_current_back_buffer());
  g_swapchain = {desc.texture.width, desc.texture.height, desc.texture.format, true};
  log_fmt(reshade::log::level::info, "AK-FSR: swapchain %ux%u format=%u resize=%d",
          desc.texture.width, desc.texture.height, static_cast<uint32_t>(desc.texture.format), resize ? 1 : 0);
}

void on_reshade_present(reshade::api::effect_runtime *runtime) {
  akfsr::jitter::on_present();
  if (runtime->is_key_pressed(kDumpFrameKey)) akfsr::dump::request_capture();
  const uint32_t toggle = akfsr::insertion::get_toggle_key();
  if (toggle != 0 && runtime->is_key_pressed(toggle)) akfsr::insertion::toggle_bypass();
}

// Touches proposées pour l'on/off (F10 est prise par la capture, Home par l'overlay de ReShade).
constexpr std::pair<uint32_t, const char *> kToggleKeys[] = {
    {0, "désactivée"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},  {VK_F8, "F8"},
    {VK_F9, "F9"},     {VK_F11, "F11"}, {VK_F12, "F12"}, {VK_END, "Fin"}, {VK_NEXT, "Page suiv."},
};

void toggle_key_row() {
  const uint32_t key = akfsr::insertion::get_toggle_key();
  const char *current = "?";
  for (const auto &[vk, name] : kToggleKeys)
    if (vk == key) current = name;
  ImGui::SetNextItemWidth(140);
  if (ImGui::BeginCombo("Touche on/off (M7)", current)) {
    for (const auto &[vk, name] : kToggleKeys)
      if (ImGui::Selectable(name, vk == key)) akfsr::insertion::set_toggle_key(vk);
    ImGui::EndCombo();
  }
  if (akfsr::insertion::bypassed()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "AK-FSR coupé par la touche");
  }
}

void pass_row(const char *name, const akfsr::passes::pass_status &s) {
  const ImVec4 color = s.found ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.9f, 0.4f, 0.4f, 1.0f);
  ImGui::TextColored(color, "%s %s", s.found ? "[OK]" : "[--]", name);
  ImGui::SameLine();
  if (s.hash != 0) {
    ImGui::Text("0x%08X | %llu / %llu", s.hash, static_cast<unsigned long long>(s.frames_found),
                static_cast<unsigned long long>(s.frames_missed));
  } else {
    ImGui::Text("| %llu / %llu", static_cast<unsigned long long>(s.frames_found),
                static_cast<unsigned long long>(s.frames_missed));
  }
}

void resource_row(const char *name, const akfsr::passes::found_resource &res) {
  if (res.handle == 0) {
    ImGui::Text("    %s : -", name);
    return;
  }
  ImGui::Text("    %s : %ux%u %s", name, res.desc.texture.width, res.desc.texture.height,
              akfsr::dump::format_string(res.desc.texture.format).c_str());
}

void on_overlay(reshade::api::effect_runtime *) {
  ImGui::TextUnformatted("AK-FSR " AKFSR_VERSION " (" AKFSR_MILESTONE " : FSR AA natif)");
  ImGui::Separator();
  if (g_swapchain.valid) {
    ImGui::Text("Swapchain : %ux%u, format %u", g_swapchain.width, g_swapchain.height,
                static_cast<uint32_t>(g_swapchain.format));
  } else {
    ImGui::TextUnformatted("Swapchain : pas encore initialisée");
  }

  const auto r = akfsr::passes::get_report();
  ImGui::Separator();
  ImGui::TextUnformatted("Passes (frame courante | frames trouvées / manquées)");
  pass_row("Insertion (DoF/bloom)", r.insertion);
  resource_row("couleur HDR (t1)", r.scene_color);
  resource_row("profondeur (t0)", r.insertion_depth);
  pass_row("SMAA du jeu (optionnelle)", r.smaa);
  {
    const char *names[] = {"option coupée", "FSR remplace le SMAA", "repli sur 0xB2A276FD (SMAA absent)",
                           "désarmée après un échec (voir log)"};
    ImGui::Text("    insertion au SMAA (M8.1) : %s, %llu frames", names[static_cast<int>(r.smaa_insertion)],
                static_cast<unsigned long long>(r.smaa_fsr_frames));
  }
  pass_row("Vélocité (motion blur)", r.velocity);
  resource_row("profondeur (t0)", r.depth);
  resource_row("vélocité objets (t1)", r.velocity_objects);
  resource_row("vélocité combinée (u0)", r.velocity_combined);
  pass_row("CB View (b1)", r.view_cb);
  pass_row("Tonemap", r.tonemap);
  if (r.view_cb_valid && ImGui::TreeNode("CB View c0..c7")) {
    for (int row = 0; row < 8; ++row) {
      ImGui::Text("c%d  %12.5g %12.5g %12.5g %12.5g", row, r.view_cb_head[row * 4], r.view_cb_head[row * 4 + 1],
                  r.view_cb_head[row * 4 + 2], r.view_cb_head[row * 4 + 3]);
    }
    ImGui::TreePop();
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Mode");
  int mode = static_cast<int>(akfsr::insertion::get_mode());
  bool changed = ImGui::RadioButton("Off", &mode, 0);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("FSR (AA natif)", &mode, 3);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("Test : aller-retour", &mode, 1);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("Test : moitié dupliquée", &mode, 2);
  if (changed) akfsr::insertion::set_mode(static_cast<akfsr::insertion::mode>(mode));
  toggle_key_row();

  auto fsr_settings = akfsr::insertion::get_fsr_settings();
  bool fsr_changed = ImGui::Checkbox("Vue de debug FSR", &fsr_settings.debug_view);
  ImGui::SameLine();
  fsr_changed |= ImGui::Checkbox("Vecteurs nuls", &fsr_settings.zero_motion);
  fsr_changed |= ImGui::SliderFloat("Netteté (RCAS)", &fsr_settings.sharpness, 0.0f, 1.0f, "%.2f");
  int prev_source = static_cast<int>(fsr_settings.prev_source);
  ImGui::TextUnformatted("ViewProj précédente :");
  ImGui::SameLine();
  bool prev_changed = ImGui::RadioButton("auto", &prev_source, 0);
  ImGui::SameLine();
  prev_changed |= ImGui::RadioButton("CB (c4..c7)", &prev_source, 1);
  ImGui::SameLine();
  prev_changed |= ImGui::RadioButton("mémorisée", &prev_source, 2);
  if (prev_changed) {
    fsr_settings.prev_source = static_cast<akfsr::fsr::prev_matrix>(prev_source);
    fsr_changed = true;
  }
  fsr_changed |= ImGui::Checkbox("Vélocité des objets (M5)", &fsr_settings.object_velocity);
  ImGui::SameLine();
  fsr_changed |= ImGui::Checkbox("Auto-exposition FFX", &fsr_settings.auto_exposure);
  ImGui::SameLine();
  fsr_changed |= ImGui::Checkbox("Jitter (M4)", &fsr_settings.jitter);
  ImGui::SameLine();
  bool patches = akfsr::passes::shader_patches_enabled();
  if (ImGui::Checkbox("Shaders corrigés (M6)", &patches)) akfsr::insertion::set_shader_patches_enabled(patches);
  const auto fsr_info = akfsr::fsr::get_info();
  if (!fsr_info.versions.empty()) {
    std::string current = "défaut (" + fsr_info.provider + ")";
    for (const auto &[id, name] : fsr_info.versions)
      if (id == fsr_settings.version_id) current = name;
    if (ImGui::BeginCombo("Version FSR", current.c_str())) {
      if (ImGui::Selectable("défaut", fsr_settings.version_id == 0)) {
        fsr_settings.version_id = 0;
        fsr_changed = true;
      }
      for (const auto &[id, name] : fsr_info.versions) {
        if (ImGui::Selectable(name.c_str(), fsr_settings.version_id == id)) {
          fsr_settings.version_id = id;
          fsr_changed = true;
        }
      }
      ImGui::EndCombo();
    }
  }
  if (ImGui::Checkbox("Insertion au SMAA du jeu (M8.1, AA du jeu sur Élevé)", &fsr_settings.smaa_insertion)) {
    fsr_changed = true;
    akfsr::passes::rearm_smaa_insertion();
  }
  fsr_changed |= ImGui::Checkbox("Hautes lumières hors FSR (M7)", &fsr_settings.highlights);
  if (fsr_settings.highlights) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    fsr_changed |= ImGui::SliderFloat("seuil", &fsr_settings.highlight_threshold, 1.0f, 64.0f, "%.1f",
                                      ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", akfsr::highlights::status().c_str());
  }
  if (fsr_changed) akfsr::insertion::set_fsr_settings(fsr_settings);
  if (!fsr_info.provider.empty()) ImGui::Text("Version utilisée : %s", fsr_info.provider.c_str());
  if (fsr_info.prev_mismatch >= 0)
    ImGui::Text("Écart ViewProj précédente CB / mémorisée : %.6f (utilisée : %s)", fsr_info.prev_mismatch,
                fsr_info.prev_from_memory ? "mémorisée" : "CB");
  ImGui::TextWrapped("FSR : %s", fsr_info.status.empty() ? "inactif" : fsr_info.status.c_str());
  ImGui::Text("Caméra (déduite du CB View) : FOV vertical %.1f°, plan proche %.2f", fsr_info.fov_deg,
              fsr_info.near_plane);

  const auto js = akfsr::jitter::get_status();
  ImGui::TextWrapped("Jitter : %s", js.message.c_str());
  ImGui::Text("  phase %u/%u, appliqué (%.3f, %.3f) px, relu dans le CB (%.3f, %.3f) px", js.phase, js.phase_count,
              js.applied_x_px, js.applied_y_px, fsr_info.jitter_px_x, fsr_info.jitter_px_y);
  for (const auto &c : js.callers) {
    ImGui::Text("  appelant 0x%07x : %s, %llu appels, M00 %.4f M11 %.4f MinZ %.2f", c.rva,
                c.main_view ? "VUE PRINCIPALE" : "autre", static_cast<unsigned long long>(c.calls), c.m00, c.m11,
                c.min_z);
  }

  ImGui::TextWrapped("Interop Vulkan : %s", akfsr::vk::status().c_str());
  const auto t = akfsr::vk::get_timings();
  ImGui::Text("CPU (copies+flush+soumission) : %.3f ms | GPU : %.3f ms", t.flush_cpu_ms, t.gpu_ms);
  ImGui::Text("  dont GPU préparation : %.3f ms | FSR : %.3f ms", t.gpu_prepare_ms, t.gpu_fsr_ms);
  ImGui::Text("Exécutions : %llu | échecs : %llu", static_cast<unsigned long long>(t.runs),
              static_cast<unsigned long long>(t.failures));

  const auto s = akfsr::dump::get_stats();
  ImGui::Separator();
  ImGui::Text("Frame : %u draws, %u dispatches", s.draws, s.dispatches);
  ImGui::Text("Shaders suivis : %zu", s.shaders_known);
  ImGui::Text("Map de buffers : %u%s", s.buffer_maps, s.buffer_maps == 0 ? " (aucun : CB non capturés)" : "");

  ImGui::Separator();
  ImGui::BeginDisabled(s.capture_busy);
  if (ImGui::Button(s.capture_busy ? "Capture en cours..." : "Capturer une frame (F10)")) akfsr::dump::request_capture();
  ImGui::EndDisabled();
  if (!s.last_capture.empty()) ImGui::TextWrapped("Dernière capture : %s", s.last_capture.c_str());
  static std::string image_dump;
  if (ImGui::Button("Écrire l'image mémoire du jeu (analyse M4)")) image_dump = akfsr::dump::dump_game_image();
  if (!image_dump.empty()) ImGui::TextWrapped("Image : %s", image_dump.c_str());
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
      reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
      akfsr::dump::register_events(module_dir(module) / "ak-fsr-dumps");
      akfsr::passes::register_events();
      akfsr::insertion::register_events();
      akfsr::jitter::install();
      reshade::register_overlay("AK-FSR", on_overlay);
      reshade::log::message(reshade::log::level::info,
                            "AK-FSR: addon loaded (v" AKFSR_VERSION ", " AKFSR_MILESTONE ")");
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay("AK-FSR", on_overlay);
      akfsr::jitter::uninstall();
      akfsr::insertion::unregister_events();
      akfsr::passes::unregister_events();
      akfsr::dump::unregister_events();
      reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
