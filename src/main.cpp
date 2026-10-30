// AK-FSR — addon ReShade : AA temporel FSR pour Batman: Arkham Knight (Proton / Steam Deck).
// FSR 3.1 ou 4 en AA natif (Vulkan via l'interop DXVK, ou DX11 natif), jitter, vélocité des objets.
// Les outils de développement (profils, rapport, pilotage par fichier, captures, compteurs) sont dans
// dev_tools.cpp, compilés seulement avec AKFSR_DEV_TOOLS=1 (branche dev).

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <iterator>
#include <utility>

#include "frame_dump.hpp"
#include "fsr4_driver.hpp"
#include "highlights.hpp"
#include "insertion.hpp"
#include "jitter.hpp"
#include "passes.hpp"
#include "vk_device_hook.hpp"
#include "version.h"
#if AKFSR_DEV_TOOLS
#include "dev_tools.hpp"
#endif

extern "C" __declspec(dllexport) const char *NAME = "AK-FSR";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "AA temporel FSR pour Batman: Arkham Knight (v" AKFSR_VERSION ")";

namespace {

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

std::filesystem::path g_addon_dir;

// Diagnostic au chargement (sans effet) : fichiers attendus par les options et prologue de vkCreateDevice.
void log_environment(const std::filesystem::path &dir) {
  std::error_code ec;
  auto present = [&](const std::filesystem::path &p) { return std::filesystem::exists(p, ec) ? "oui" : "non"; };
  log_fmt(reshade::log::level::info,
          "AK-FSR: diagnostic : d3dcompiler_47 %s", LoadLibraryW(L"d3dcompiler_47.dll") != nullptr ? "oui" : "non");
  const auto amd = dir / "ak-fsr";
  log_fmt(reshade::log::level::info,
          "AK-FSR: diagnostic : ak-fsr\\ amdvkffx64.dll %s, amd_fidelityfx_loader_vk.dll %s, "
          "amd_fidelityfx_upscaler_vk.dll %s",
          present(amd / "amdvkffx64.dll"), present(amd / "amd_fidelityfx_loader_vk.dll"),
          present(amd / "amd_fidelityfx_upscaler_vk.dll"));
  akfsr::vk_device_hook::log_prologue();
}

void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize) {
  const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_current_back_buffer());
#if AKFSR_DEV_TOOLS
  akfsr::dev::set_swapchain(desc.texture.width, desc.texture.height, desc.texture.format);
#endif
  static bool environment_logged = false;  // hors de DllMain : LoadLibrary interdit sous le verrou du loader
  if (!environment_logged) {
    environment_logged = true;
    log_environment(g_addon_dir);
  }
  log_fmt(reshade::log::level::info, "AK-FSR: swapchain %ux%u format=%u resize=%d",
          desc.texture.width, desc.texture.height, static_cast<uint32_t>(desc.texture.format), resize ? 1 : 0);
}

void on_reshade_present(reshade::api::effect_runtime *runtime) {
  akfsr::jitter::on_present();
#if AKFSR_DEV_TOOLS
  akfsr::dev::on_present(runtime);
#endif
  const uint32_t toggle = akfsr::insertion::get_toggle_key();
  if (toggle != 0 && runtime->is_key_pressed(toggle)) akfsr::insertion::toggle_bypass();
}

// Touches proposées pour l'on/off (F10 : capture en développement, Home : overlay de ReShade).
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
  if (ImGui::BeginCombo("Touche on/off", current)) {
    for (const auto &[vk, name] : kToggleKeys)
      if (ImGui::Selectable(name, vk == key)) akfsr::insertion::set_toggle_key(vk);
    ImGui::EndCombo();
  }
  if (akfsr::insertion::bypassed()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "AK-FSR coupé par la touche");
  }
}


// Modèle FSR 4 imposé au pilote (ordre de fsr4::model) : Auto (son choix d'après le GPU), FP8, INT8, INT8-2 ou
// Lite, par le GPU présenté au pilote et les capacités masquées. Sur PC, les trois INT8 donnaient jusqu'ici les mêmes
// shaders : le nom du provider créé (« choisi par le pilote ») et le watermark (MLSR-WATERMARK) font foi.
bool fsr4_model_combo(akfsr::fsr::settings &s) {
  static const char *models[] = {"Auto (choix du pilote)", "FP8 (RDNA 4)", "INT8 (RDNA 3)", "INT8-2 (RDNA 2)",
                                 "INT8 Lite (Deck)"};
  int choice = std::clamp(s.fsr4_model, 0, 4);
  ImGui::SetNextItemWidth(200);
  if (!ImGui::Combo("Modèle FSR 4", &choice, models, static_cast<int>(std::size(models)))) return false;
  s.fsr4_model = choice;
  return true;
}

#if !AKFSR_DEV_TOOLS
// Version publiée : un seul choix, l'AA utilisé ; le reste suit les réglages validés (voir fsr.hpp).
void on_overlay(reshade::api::effect_runtime *) {
  ImGui::TextUnformatted("AK-FSR " AKFSR_VERSION " : anti-aliasing FSR 4");
  ImGui::Separator();
  auto s = akfsr::insertion::get_fsr_settings();
  const auto info = akfsr::fsr::get_info();
  const auto driver = akfsr::fsr4::get_info();
  const bool fsr4_possible = akfsr::vk_device_hook::get_status().device_seen && !(driver.tried && !driver.ok);
  const bool off = akfsr::insertion::get_mode() == akfsr::insertion::mode::off;
  int choice = off ? 0 : 1;
  bool changed = ImGui::RadioButton("Off", &choice, 0);
  ImGui::BeginDisabled(!fsr4_possible && off);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("FSR 4", &choice, 1);
  ImGui::EndDisabled();
  if (changed)
    akfsr::insertion::set_mode(choice == 0 ? akfsr::insertion::mode::off : akfsr::insertion::mode::fsr);
  if (!fsr4_possible) {
    ImGui::TextDisabled("FSR 4 : DLL AMD absentes de ak-fsr\\ ou pilote incompatible (voir README)");
  } else {
    bool model_changed = fsr4_model_combo(s);
    ImGui::SameLine();
    model_changed |= ImGui::Checkbox("Watermark du pilote", &s.fsr4_watermark);
    if (model_changed) akfsr::insertion::set_fsr_settings(s);
    ImGui::Text("Modèle choisi par le pilote : %s", !driver.model.empty() ? driver.model.c_str() : "pas encore connu");
    if (driver.context_shaders > 0)
      ImGui::TextDisabled("(%u shaders, empreinte 0x%08X)", driver.context_shaders, driver.context_hash);
  }
  if (!off && !info.status.empty() && info.status.rfind("actif", 0) != 0)
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "FSR : %s", info.status.c_str());
}
#else
void on_overlay(reshade::api::effect_runtime *) {
  akfsr::fsr::note_overlay_visible();
#if AKFSR_DEV_TOOLS
  ImGui::TextUnformatted("AK-FSR " AKFSR_VERSION " (développement, " AKFSR_MILESTONE ")");
  ImGui::Separator();
  akfsr::dev::draw_passes();
#else
  ImGui::TextUnformatted("AK-FSR " AKFSR_VERSION);
#endif
  ImGui::Separator();
  int mode = static_cast<int>(akfsr::insertion::get_mode());
  bool changed = ImGui::RadioButton("Off", &mode, 0);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("FSR (AA natif)", &mode, 3);
  if (changed) akfsr::insertion::set_mode(static_cast<akfsr::insertion::mode>(mode));
#if AKFSR_DEV_TOOLS
  ImGui::SameLine();
  akfsr::dev::draw_mode_and_profiles();
#endif
  toggle_key_row();

  auto fsr_settings = akfsr::insertion::get_fsr_settings();
  bool fsr_changed = false;
  const auto fsr_info = akfsr::fsr::get_info();
  fsr_changed |= fsr4_model_combo(fsr_settings);
  ImGui::SameLine();
  fsr_changed |= ImGui::Checkbox("Watermark", &fsr_settings.fsr4_watermark);
  {
    const auto driver_info = akfsr::fsr4::get_info();
    ImGui::SameLine();
    ImGui::Text("choisi par le pilote : %s (%u shaders, empreinte 0x%08X)",
                driver_info.model.empty() ? "pas encore connu" : driver_info.model.c_str(), driver_info.context_shaders,
                driver_info.context_hash);
  }
  fsr_changed |= ImGui::Checkbox("Remplacer le SMAA du jeu (AA du jeu sur On)", &fsr_settings.smaa_insertion);
  ImGui::SetNextItemWidth(160);
  fsr_changed |= ImGui::SliderFloat("Biais de LOD (au prochain lancement ; -1 conseillé)", &fsr_settings.lod_bias,
                                    -2.0f, 0.0f, "%.2f");
  fsr_changed |= ImGui::SliderFloat("Netteté (RCAS)", &fsr_settings.sharpness, 0.0f, 1.0f, "%.2f");
  fsr_changed |= ImGui::Checkbox("Auto-exposition FSR", &fsr_settings.auto_exposure);
  ImGui::SameLine();
  fsr_changed |= ImGui::Checkbox("Hautes lumières hors FSR", &fsr_settings.highlights);
  ImGui::SameLine();
  fsr_changed |= ImGui::Checkbox("Pluie après FSR", &fsr_settings.rain_after_fsr);
  ImGui::SameLine();
  fsr_changed |= ImGui::Checkbox("Effets transparents après FSR", &fsr_settings.translucent_after_fsr);
  if (fsr_settings.highlights) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    fsr_changed |= ImGui::SliderFloat("seuil", &fsr_settings.highlight_threshold, 1.0f, 64.0f, "%.1f",
                                      ImGuiSliderFlags_Logarithmic);
  }
#if AKFSR_DEV_TOOLS
  fsr_changed |= akfsr::dev::draw_options(fsr_settings);
#endif
  if (fsr_changed) akfsr::insertion::set_fsr_settings(fsr_settings);

  ImGui::Separator();
  if (!fsr_info.provider.empty()) ImGui::Text("Version utilisée : %s", fsr_info.provider.c_str());
  ImGui::TextWrapped("FSR : %s", fsr_info.status.empty() ? "inactif" : fsr_info.status.c_str());
  if (akfsr::insertion::get_mode() == akfsr::insertion::mode::fsr && !akfsr::jitter::get_status().installed)
    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Jitter indisponible (version du jeu ?)");
#if AKFSR_DEV_TOOLS
  ImGui::Separator();
  akfsr::dev::draw_diagnostics();
#endif
}

#endif

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
      reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
      g_addon_dir = module_dir(module);
#if AKFSR_DEV_TOOLS
      akfsr::dev::init(g_addon_dir);
#endif
      akfsr::dump::register_events(module_dir(module) / "ak-fsr-dumps");
      akfsr::passes::register_events();
      akfsr::insertion::register_events();
      akfsr::jitter::install();
      // M10.2 : avant la création du device de DXVK (ReShade charge les addons dans D3D11CreateDevice).
      akfsr::vk_device_hook::install();
      reshade::register_overlay("AK-FSR", on_overlay);
      reshade::log::message(reshade::log::level::info,
                            "AK-FSR: addon loaded (v" AKFSR_VERSION AKFSR_BUILD_KIND ")");
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
