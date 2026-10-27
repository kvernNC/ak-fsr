// AK-FSR — addon ReShade : AA temporel FSR pour Batman: Arkham Knight (Proton / Steam Deck).
// FSR 3.1 en AA natif au point d'insertion (Vulkan via l'interop DXVK), jitter, vélocité des objets.

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "exposure.hpp"
#include "frame_dump.hpp"
#include "fsr4_driver.hpp"
#include "fsr_dx11.hpp"
#include "highlights.hpp"
#include "insertion.hpp"
#include "jitter.hpp"
#include "lod_bias.hpp"
#include "passes.hpp"
#include "screenshot.hpp"
#include "shader_explorer.hpp"
#include "vk_bridge.hpp"
#include "vk_device_hook.hpp"
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


// Profils de test (TESTING.md) : une étape = une option activée, toutes les autres à leur défaut.
struct test_profile {
  const char *name;
  void (*apply)(akfsr::fsr::settings &);
};
const test_profile kTestProfiles[] = {
    {"0. Référence (défauts)", [](akfsr::fsr::settings &) {}},
    {"2. M7.2 hautes lumières", [](akfsr::fsr::settings &s) { s.highlights = true; }},
    {"4. M8.1 insertion au SMAA", [](akfsr::fsr::settings &s) { s.smaa_insertion = true; }},
    {"5. M8.2 backend DX11", [](akfsr::fsr::settings &s) { s.fsr_backend = akfsr::fsr::backend::dx11; }},
    {"4+5. M8.1 + M8.2 (budget Deck)", [](akfsr::fsr::settings &s) {
       s.smaa_insertion = true;
       s.fsr_backend = akfsr::fsr::backend::dx11;
     }},
    {"6. M8.3 vecteurs du jeu (UV)", [](akfsr::fsr::settings &s) { s.game_mv = true; }},
    {"6. M8.3 vecteurs du jeu ([-1,1])", [](akfsr::fsr::settings &s) {
       s.game_mv = true;
       s.game_mv_scale = 1;
     }},
    {"7. M8.4 biais de LOD -0.5 (relancer)", [](akfsr::fsr::settings &s) { s.lod_bias = -0.5f; }},
    {"8-9. M10 FSR 4 pilote (relancer)", [](akfsr::fsr::settings &s) {
       s.fsr4_driver = true;
       s.version_id = akfsr::fsr4::kDriverVersionId;
     }},
    {"10. M8.5 exposition du jeu", [](akfsr::fsr::settings &s) { s.game_exposure = true; }},
};

// Garde la netteté, l'auto-exposition et la vue de debug de l'utilisateur ; le reste revient au défaut.
void apply_test_profile(const test_profile &p) {
  const auto current = akfsr::insertion::get_fsr_settings();
  akfsr::fsr::settings s;
  s.sharpness = current.sharpness;
  s.debug_view = current.debug_view;
  s.auto_exposure = current.auto_exposure;  // réglage de l'utilisateur (M8.5 la coupe d'office à l'exécution)
  s.exposure_experimental = current.exposure_experimental;
  p.apply(s);
  akfsr::insertion::set_fsr_settings(s);
  akfsr::insertion::set_shader_patches_enabled(true);
  akfsr::insertion::set_mode(akfsr::insertion::mode::fsr);
  reshade::log::message(reshade::log::level::info, (std::string("AK-FSR: profil de test : ") + p.name).c_str());
}

std::filesystem::path g_addon_dir;

// Diagnostic au chargement (sans effet) : fichiers attendus par les options et prologue de vkCreateDevice.
void log_environment(const std::filesystem::path &dir) {
  std::error_code ec;
  auto present = [&](const std::filesystem::path &p) { return std::filesystem::exists(p, ec) ? "oui" : "non"; };
  log_fmt(reshade::log::level::info,
          "AK-FSR: diagnostic : amd_fidelityfx_vk.dll %s, ak-fsr-dx11.dll %s, d3dcompiler_47 %s",
          present(dir / "amd_fidelityfx_vk.dll"), present(dir / "ak-fsr-dx11.dll"),
          LoadLibraryW(L"d3dcompiler_47.dll") != nullptr ? "oui" : "non");
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
  g_swapchain = {desc.texture.width, desc.texture.height, desc.texture.format, true};
  static bool environment_logged = false;  // hors de DllMain : LoadLibrary interdit sous le verrou du loader
  if (!environment_logged) {
    environment_logged = true;
    log_environment(g_addon_dir);
  }
  log_fmt(reshade::log::level::info, "AK-FSR: swapchain %ux%u format=%u resize=%d",
          desc.texture.width, desc.texture.height, static_cast<uint32_t>(desc.texture.format), resize ? 1 : 0);
}

std::string write_report();
void next_test_profile();
void poll_command_file(reshade::api::effect_runtime *runtime);

// Touches utilisables sans l'overlay (prise en main à distance, petite fenêtre) : [AK-FSR] ReportKey /
// ReportModifier et ProfileKey / ProfileModifier de ReShade.ini, codes VK (touche 0 = désactivée,
// modificateur 0 = aucun). Défauts : Ctrl + Impr. écran et Alt + Impr. écran.
struct hotkey {
  uint32_t key, modifier;
};
hotkey g_report_key{VK_SNAPSHOT, VK_CONTROL}, g_profile_key{VK_SNAPSHOT, VK_MENU};

bool hotkey_pressed(reshade::api::effect_runtime *runtime, const hotkey &k, const hotkey &other) {
  if (k.key == 0 || !runtime->is_key_pressed(k.key)) return false;
  if (k.modifier != 0 && !runtime->is_key_down(k.modifier)) return false;
  // Même touche avec un autre modificateur enfoncé : c'est l'autre raccourci.
  return !(other.key == k.key && other.modifier != 0 && other.modifier != k.modifier &&
           runtime->is_key_down(other.modifier));
}

void on_reshade_present(reshade::api::effect_runtime *runtime) {
  akfsr::jitter::on_present();
  if (runtime->is_key_pressed(kDumpFrameKey)) akfsr::dump::request_capture();
  if (hotkey_pressed(runtime, g_report_key, g_profile_key)) write_report();
  if (hotkey_pressed(runtime, g_profile_key, g_report_key)) next_test_profile();
  poll_command_file(runtime);
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


// Rapport de test : réglages, passes, états et mesures en texte, écrit à côté de l'addon et copié dans le
// presse-papiers (à coller tel quel dans le retour de test).
std::string build_report() {
  std::string out;
  char line[512];
  auto add = [&](const char *fmt, auto... args) {
    std::snprintf(line, sizeof(line), fmt, args...);
    out += line;
    out += '\n';
  };
  const auto st = akfsr::insertion::get_fsr_settings();
  const auto info = akfsr::fsr::get_info();
  const auto r = akfsr::passes::get_report();
  const auto t = akfsr::vk::get_timings();
  add("AK-FSR %s (%s), swapchain %ux%u format %u", AKFSR_VERSION, AKFSR_MILESTONE, g_swapchain.width,
      g_swapchain.height, static_cast<uint32_t>(g_swapchain.format));
  add("[réglages] Mode=%d coupé_par_touche=%d ToggleKey=%u Backend=%d Version=%llu Jitter=%d ShaderPatches=%d",
      static_cast<int>(akfsr::insertion::get_mode()), akfsr::insertion::bypassed() ? 1 : 0,
      akfsr::insertion::get_toggle_key(), static_cast<int>(st.fsr_backend),
      static_cast<unsigned long long>(st.version_id), st.jitter ? 1 : 0, akfsr::passes::shader_patches_enabled() ? 1 : 0);
  add("[réglages] ObjectVelocity=%d AutoExposure=%d Sharpness=%.2f PrevMatrix=%d Highlights=%d (%.1f) SmaaInsertion=%d",
      st.object_velocity ? 1 : 0, st.auto_exposure ? 1 : 0, st.sharpness, static_cast<int>(st.prev_source),
      st.highlights ? 1 : 0, st.highlight_threshold, st.smaa_insertion ? 1 : 0);
  add("[réglages] GameMotionVectors=%d GameMvScale=%d GameMvFlipY=%d LodBias=%.2f Fsr4Driver=%d", st.game_mv ? 1 : 0,
      st.game_mv_scale, st.game_mv_flip_y ? 1 : 0, st.lod_bias, st.fsr4_driver ? 1 : 0);
  auto pass = [&](const char *name, const akfsr::passes::pass_status &p) {
    add("[passe] %-12s %s 0x%08X trouvées %llu manquées %llu", name, p.found ? "OK" : "--", p.hash,
        static_cast<unsigned long long>(p.frames_found), static_cast<unsigned long long>(p.frames_missed));
  };
  pass("insertion", r.insertion);
  pass("SMAA", r.smaa);
  pass("vélocité", r.velocity);
  pass("CB View", r.view_cb);
  pass("tonemap", r.tonemap);
  auto res = [&](const char *name, const akfsr::passes::found_resource &x) {
    add("[ressource] %-22s %ux%u %s", name, x.desc.texture.width, x.desc.texture.height,
        x.handle != 0 ? akfsr::dump::format_string(x.desc.texture.format).c_str() : "-");
  };
  res("couleur HDR", r.scene_color);
  res("profondeur insertion", r.insertion_depth);
  res("RT4 vélocité objets", r.velocity_objects);
  res("R16G16F effacée", r.cleared_rg16);
  res("exposition (tonemap)", r.exposure);
  add("[M8.5] exposition : slot t%d, GameExposure=%d mode %d échelle %.2f : %s", r.exposure_slot,
      st.game_exposure ? 1 : 0, st.exposure_mode, st.exposure_scale,
      st.game_exposure ? akfsr::exposure::status().c_str() : "option coupée");
  add("[ressource] R16G16F effacée == RT4 : %s", r.cleared_rg16.handle != 0 && r.cleared_rg16.handle == r.velocity_objects.handle ? "oui" : "non");
  const char *smaa[] = {"option coupée", "FSR remplace le SMAA", "repli 0xB2A276FD", "désarmée"};
  add("[M8.1] insertion au SMAA : %s, %llu frames", smaa[static_cast<int>(r.smaa_insertion)],
      static_cast<unsigned long long>(r.smaa_fsr_frames));
  add("[FSR] statut : %s | version utilisée : %s", info.status.c_str(), info.provider.c_str());
  add("[FSR] FOV %.1f°, plan proche %.2f, jitter relu (%.3f, %.3f) px", info.fov_deg, info.near_plane, info.jitter_px_x,
      info.jitter_px_y);
  add("[Vulkan] interop : %s", akfsr::vk::status().c_str());
  add("[Vulkan] CPU %.3f ms | GPU %.3f ms (préparation %.3f, FSR %.3f) | complet vu par D3D11 %.3f ms | %llu exécutions, %llu échecs",
      t.flush_cpu_ms, t.gpu_ms, t.gpu_prepare_ms, t.gpu_fsr_ms, info.vulkan_total_gpu_ms,
      static_cast<unsigned long long>(t.runs), static_cast<unsigned long long>(t.failures));
  const auto gt = akfsr::fsr::dx11::get_gpu_timings();
  add("[DX11] %s | CPU %.3f ms | GPU préparation %.3f, FSR %.3f, copie %.3f, total %.3f ms",
      info.dx11_status.empty() ? "non lancé" : info.dx11_status.c_str(), info.dx11_cpu_ms, gt.prepare_ms, gt.fsr_ms,
      gt.copy_ms, gt.prepare_ms + gt.fsr_ms + gt.copy_ms);
  add("[M7.2] hautes lumières : %s", st.highlights ? akfsr::highlights::status().c_str() : "option coupée");
  const auto lb = akfsr::lod_bias::get_stats();
  add("[M8.4] biais de LOD en vigueur %+.2f, %u samplers modifiés, %u laissés", lb.applied_bias, lb.modified, lb.skipped);
  const auto hook = akfsr::vk_device_hook::get_status();
  std::string added, missing;
  for (const auto &a : hook.added) added += a + " ";
  for (const auto &m : hook.missing) missing += m + " ";
  add("[M10.2] device : %s | ajoutées : %s| absentes : %s", hook.message.c_str(), added.c_str(), missing.c_str());
  const auto d = akfsr::fsr4::get_info();
  add("[M10.2] provider : %s, « %s », id 0x%016llx, modèle %s (%s), shaders %u dont %u réécrits, %u échecs ; repli : %s",
      d.tried ? d.status.c_str() : "non essayé", d.provider_name.c_str(), static_cast<unsigned long long>(d.provider_id),
      d.model.empty() ? "?" : d.model.c_str(), d.model_seen.c_str(), d.shaders, d.shaders_rewritten, d.shader_failures,
      info.fsr4_status.empty() ? "-" : info.fsr4_status.c_str());
  const auto js = akfsr::jitter::get_status();
  add("[jitter] %s", js.message.c_str());
  for (const auto &c : js.callers)
    add("[jitter] appelant 0x%07x %s, %llu appels", c.rva, c.main_view ? "VUE PRINCIPALE" : "autre",
        static_cast<unsigned long long>(c.calls));
  const auto ds = akfsr::dump::get_stats();
  add("[frame] %u draws, %u dispatches, %zu shaders suivis", ds.draws, ds.dispatches, ds.shaders_known);
  return out;
}

// Ajoute le rapport à ak-fsr-report.txt et au log ; retourne un état lisible.
std::string write_report() {
  const std::string text = build_report();
  reshade::log::message(reshade::log::level::info, ("AK-FSR: rapport de test :\n" + text).c_str());
  const auto path = g_addon_dir / "ak-fsr-report.txt";
  FILE *f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || f == nullptr) return "écriture de " + path.string() + " impossible";
  std::fwrite(text.data(), 1, text.size(), f);
  std::fputs("----\n", f);
  std::fclose(f);
  return "ajouté à " + path.string();
}

int g_profile = 0;

void next_test_profile() {
  g_profile = (g_profile + 1) % static_cast<int>(std::size(kTestProfiles));
  apply_test_profile(kTestProfiles[g_profile]);
}

// Pilotage sans clavier (prise en main à distance) : fichier ak-fsr-command.txt à côté de l'addon, lu
// environ deux fois par seconde puis supprimé. Une commande par ligne :
//   report            rapport de test
//   profile <n>|next  profil de test n (0 = référence) ou suivant
//   toggle            touche on/off
//   shaders           active l'explorateur de PS et écrit la liste de la frame dans ak-fsr-shaders.txt
//   hide <hash> / show <hash>|all  masque / rétablit les draws d'un PS (hash hexadécimal, 0x facultatif)
//   compare <hash> …  pour chaque PS : capture <hash>_on, masquage, capture <hash>_off à la frame suivante
//   screenshot <nom> [n]  capture PNG du back buffer dans ak-fsr-shots/<nom>.png, ou n frames consécutives
//                         (<nom>_0.png …, n ≤ 16) pour juger le scintillement
int g_shader_dump_in = 0;  // frames avant l'écriture de ak-fsr-shaders.txt (0 : rien)

void dump_shader_list() {
  const auto entries = akfsr::explorer::get_entries();
  const auto path = g_addon_dir / "ak-fsr-shaders.txt";
  FILE *f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return;
  std::fprintf(f, "# ordre ; hash ; position ; draws ; sommets ; dont indirects ; masqué\n");
  int i = 0;
  for (const auto &e : entries)
    std::fprintf(f, "%3d 0x%08X %-16s %5u %9u %4u %s\n", i++, e.hash, e.after_insertion ? "après_insertion" : "avant",
                 e.draws, e.vertices, e.indirect, e.hidden ? "masqué" : "");
  std::fclose(f);
  log_fmt(reshade::log::level::info, "AK-FSR: liste des pixel shaders écrite : %zu PS", entries.size());
}

// Comparaisons en cours (commande compare) : étape 0 = capture normale puis masquage, 1 = capture masquée.
std::vector<uint32_t> g_compare_queue;
int g_compare_step = 0;

void run_compare(reshade::api::effect_runtime *runtime) {
  if (g_compare_queue.empty()) return;
  const uint32_t hash = g_compare_queue.front();
  char name[32];
  std::snprintf(name, sizeof(name), "%08X_%s", hash, g_compare_step == 0 ? "on" : "off");
  const std::string result = akfsr::screenshot::capture(runtime, g_addon_dir / "ak-fsr-shots" / (std::string(name) + ".png"));
  log_fmt(reshade::log::level::info, "AK-FSR: compare : capture %s", result.c_str());
  if (g_compare_step == 0) {
    akfsr::explorer::set_hidden(hash, true);  // effet sur la frame suivante
    g_compare_step = 1;
  } else {
    akfsr::explorer::set_hidden(hash, false);
    g_compare_step = 0;
    g_compare_queue.erase(g_compare_queue.begin());
  }
}

std::string g_burst_name;
int g_burst_total = 0, g_burst_done = 0;

void run_command(reshade::api::effect_runtime *runtime, const std::string &line) {
  std::string cmd = line, arg;
  if (const size_t sp = line.find(' '); sp != std::string::npos) {
    cmd = line.substr(0, sp);
    arg = line.substr(sp + 1);
  }
  if (cmd == "report") {
    write_report();
  } else if (cmd == "profile") {
    const int count = static_cast<int>(std::size(kTestProfiles));
    if (arg == "next") {
      next_test_profile();
    } else if (!arg.empty() && std::isdigit(static_cast<unsigned char>(arg[0])) && std::atoi(arg.c_str()) < count) {
      g_profile = std::atoi(arg.c_str());
      apply_test_profile(kTestProfiles[g_profile]);
    } else {
      log_fmt(reshade::log::level::warning, "AK-FSR: commande : profil inconnu « %s » (0 à %d)", arg.c_str(), count - 1);
    }
  } else if (cmd == "toggle") {
    akfsr::insertion::toggle_bypass();
  } else if (cmd == "shaders") {
    akfsr::explorer::set_enabled(true);
    g_shader_dump_in = 3;  // laisser l'explorateur remplir une frame complète
  } else if (cmd == "hide" || cmd == "show") {
    akfsr::explorer::set_enabled(true);
    if (cmd == "show" && arg == "all") {
      akfsr::explorer::clear_hidden();
    } else {
      const uint32_t hash = static_cast<uint32_t>(std::strtoul(arg.c_str(), nullptr, 16));
      if (hash == 0)
        log_fmt(reshade::log::level::warning, "AK-FSR: commande : hash invalide « %s »", arg.c_str());
      else
        akfsr::explorer::set_hidden(hash, cmd == "hide");
    }
  } else if (cmd == "compare") {
    akfsr::explorer::set_enabled(true);
    size_t pos = 0;
    while (pos < arg.size()) {
      const size_t end = std::min(arg.find(' ', pos), arg.size());
      if (const uint32_t hash = static_cast<uint32_t>(std::strtoul(arg.substr(pos, end - pos).c_str(), nullptr, 16)))
        g_compare_queue.push_back(hash);
      pos = end + 1;
    }
  } else if (cmd == "screenshot") {
    std::string name = arg.empty() ? "capture" : arg;
    int frames = 1;
    if (const size_t sp = name.find(' '); sp != std::string::npos) {
      frames = std::clamp(std::atoi(name.c_str() + sp + 1), 1, 16);
      name.resize(sp);
    }
    for (char &c : name)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    g_burst_name = name;
    g_burst_total = frames;
    g_burst_done = 0;
    (void)runtime;
  } else if (!cmd.empty()) {
    log_fmt(reshade::log::level::warning, "AK-FSR: commande inconnue « %s »", line.c_str());
  }
}

// Captures en cours (une par frame, à partir de la frame de la commande).
void capture_burst(reshade::api::effect_runtime *runtime) {
  if (g_burst_done >= g_burst_total) return;
  const std::string file = g_burst_total == 1 ? g_burst_name : g_burst_name + "_" + std::to_string(g_burst_done);
  const std::string result = akfsr::screenshot::capture(runtime, g_addon_dir / "ak-fsr-shots" / (file + ".png"));
  log_fmt(reshade::log::level::info, "AK-FSR: capture %s", result.c_str());
  ++g_burst_done;
}

void poll_command_file(reshade::api::effect_runtime *runtime) {
  capture_burst(runtime);
  run_compare(runtime);
  if (g_shader_dump_in > 0 && --g_shader_dump_in == 0) dump_shader_list();
  static uint32_t frame = 0;
  if (++frame % 30 != 0) return;
  const auto path = g_addon_dir / "ak-fsr-command.txt";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  FILE *f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return;
  std::string text(4096, '\0');
  text.resize(std::fread(text.data(), 1, text.size(), f));
  std::fclose(f);
  DeleteFileW(path.c_str());
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) {
      log_fmt(reshade::log::level::info, "AK-FSR: commande : %s", line.c_str());
      run_command(runtime, line);
    }
    start = end + 1;
  }
}

void on_overlay(reshade::api::effect_runtime *) {
  akfsr::fsr::note_overlay_visible();
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
  resource_row("R16G16F effacée (M8.3)", r.cleared_rg16);
  resource_row("exposition 1x1 (M8.5)", r.exposure);
  if (r.cleared_rg16.handle != 0) {
    ImGui::SameLine();
    ImGui::TextUnformatted(r.cleared_rg16.handle == r.velocity_objects.handle ? "= RT4" : "!= RT4");
  }
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
  {
    int &profile = g_profile;
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Profil de test (TESTING.md)", kTestProfiles[profile].name)) {
      for (int i = 0; i < static_cast<int>(std::size(kTestProfiles)); ++i)
        if (ImGui::Selectable(kTestProfiles[i].name, i == profile)) {
          profile = i;
          apply_test_profile(kTestProfiles[i]);
        }
      ImGui::EndCombo();
    }
  }

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
  // M10.3 : versions de amd_fidelityfx_vk.dll (2.3.3, 3.1.4…) + FSR 4.1.2 du provider pilote.
  auto versions = fsr_info.versions;
  const auto driver = akfsr::fsr4::get_info();
  if (fsr_settings.fsr4_driver) {
    std::string label = akfsr::fsr4::kDriverVersionName;
    if (driver.tried && !driver.ok) label += " - indisponible";
    else if (!driver.model.empty()) label += " - " + driver.model;
    versions.emplace_back(akfsr::fsr4::kDriverVersionId, label);
  }
  if (!versions.empty() && fsr_settings.fsr_backend == akfsr::fsr::backend::vulkan) {
    std::string current = "défaut (" + fsr_info.provider + ")";
    for (const auto &[id, name] : versions)
      if (id == fsr_settings.version_id) current = name;
    if (ImGui::BeginCombo("Version FSR", current.c_str())) {
      if (ImGui::Selectable("défaut", fsr_settings.version_id == 0)) {
        fsr_settings.version_id = 0;
        fsr_changed = true;
      }
      for (const auto &[id, name] : versions) {
        if (ImGui::Selectable(name.c_str(), fsr_settings.version_id == id)) {
          fsr_settings.version_id = id;
          fsr_changed = true;
        }
      }
      ImGui::EndCombo();
    }
  }
  fsr_changed |= ImGui::Checkbox("Insertion au SMAA du jeu (M8.1, AA du jeu sur On)", &fsr_settings.smaa_insertion);
  {
    int backend = static_cast<int>(fsr_settings.fsr_backend);
    ImGui::TextUnformatted("Backend FSR :");
    ImGui::SameLine();
    bool backend_changed = ImGui::RadioButton("Vulkan (interop DXVK)", &backend, 0);
    ImGui::SameLine();
    backend_changed |= ImGui::RadioButton("DX11 natif (M8.2)", &backend, 1);
    if (backend_changed) {
      fsr_settings.fsr_backend = static_cast<akfsr::fsr::backend>(backend);
      fsr_changed = true;
    }
  }
  fsr_changed |= ImGui::Checkbox("Vecteurs du jeu tels quels (M8.3)", &fsr_settings.game_mv);
  if (fsr_settings.game_mv) {
    ImGui::SameLine();
    fsr_changed |= ImGui::RadioButton("UV", &fsr_settings.game_mv_scale, 0);
    ImGui::SameLine();
    fsr_changed |= ImGui::RadioButton("[-1,1]", &fsr_settings.game_mv_scale, 1);
    ImGui::SameLine();
    fsr_changed |= ImGui::Checkbox("Y inversé", &fsr_settings.game_mv_flip_y);
  }
  {
    ImGui::SetNextItemWidth(160);
    fsr_changed |= ImGui::SliderFloat("Biais de LOD (M8.4, au prochain lancement)", &fsr_settings.lod_bias, -2.0f, 0.0f,
                                      "%.2f");
    const auto lb = akfsr::lod_bias::get_stats();
    ImGui::Text("    en vigueur : %+.2f, %u samplers modifiés, %u laissés", lb.applied_bias, lb.modified, lb.skipped);
  }
  fsr_changed |= ImGui::Checkbox("Exposition du jeu (M8.5)", &fsr_settings.game_exposure);
  if (fsr_settings.game_exposure) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    const char *modes[] = {"telle quelle", "inverse", "exp2(x)", "exp2(-x)"};
    fsr_changed |= ImGui::Combo("##expmode", &fsr_settings.exposure_mode, modes, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    fsr_changed |= ImGui::SliderFloat("échelle", &fsr_settings.exposure_scale, 0.01f, 100.0f, "%.2f",
                                      ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", akfsr::exposure::status().c_str());
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
  fsr_changed |= ImGui::Checkbox("FSR 4 pilote (M10.2, au prochain lancement)", &fsr_settings.fsr4_driver);
  if (fsr_settings.fsr4_driver) {
    const auto hook = akfsr::vk_device_hook::get_status();
    ImGui::TextWrapped("    device Vulkan : %s", hook.message.empty() ? "détour non installé (relancer le jeu)"
                                                                       : hook.message.c_str());
  }
  if (fsr_changed) akfsr::insertion::set_fsr_settings(fsr_settings);
  if (!fsr_info.provider.empty()) ImGui::Text("Version utilisée : %s", fsr_info.provider.c_str());
  if (fsr_settings.fsr4_driver && driver.ok)
    ImGui::TextWrapped("FSR 4 pilote : %s (« %s »), modèle utilisé : %s, shaders %u dont %u réécrits, %u échecs",
                driver.status.c_str(), driver.provider_name.c_str(),
                driver.model.empty() ? (driver.model_seen.empty() ? "inconnu" : driver.model_seen.c_str())
                                     : driver.model.c_str(),
                driver.shaders,
                driver.shaders_rewritten, driver.shader_failures);
  if (!fsr_info.fsr4_status.empty())
    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "FSR 4 pilote indisponible, repli sur FSR 3.1 : %s",
                       fsr_info.fsr4_status.c_str());
  if (fsr_settings.fsr_backend == akfsr::fsr::backend::dx11) {
    ImGui::Text("Backend DX11 : %s | CPU %.3f ms", fsr_info.dx11_status.empty() ? "pas encore lancé" :
                fsr_info.dx11_status.c_str(), fsr_info.dx11_cpu_ms);
    const auto gt = akfsr::fsr::dx11::get_gpu_timings();
    if (gt.valid)
      ImGui::Text("  GPU : préparation %.3f ms | FSR %.3f ms | copie %.3f ms | total %.3f ms", gt.prepare_ms, gt.fsr_ms,
                  gt.copy_ms, gt.prepare_ms + gt.fsr_ms + gt.copy_ms);
  }
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
  if (fsr_info.vulkan_total_gpu_ms > 0)
    ImGui::Text("  chemin Vulkan complet vu par D3D11 (copies + FSR) : %.3f ms", fsr_info.vulkan_total_gpu_ms);
  ImGui::Text("Exécutions : %llu | échecs : %llu", static_cast<unsigned long long>(t.runs),
              static_cast<unsigned long long>(t.failures));

  const auto s = akfsr::dump::get_stats();
  ImGui::Separator();
  ImGui::Text("Frame : %u draws, %u dispatches", s.draws, s.dispatches);
  ImGui::Text("Shaders suivis : %zu", s.shaders_known);
  ImGui::Text("Map de buffers : %u%s", s.buffer_maps, s.buffer_maps == 0 ? " (aucun : CB non capturés)" : "");

  ImGui::Separator();
  if (ImGui::TreeNode("Explorateur de pixel shaders (M7.3, pluie)")) {
    akfsr::explorer::set_enabled(true);
    ImGui::TextWrapped("PS des draws de la dernière frame, dans l'ordre. Cocher « masquer » pour sauter leurs draws "
                       "et les reconnaître à l'écran (rien n'est sauvegardé ; fermer ce nœud démasque tout).");
    if (ImGui::Button("Tout démasquer")) akfsr::explorer::clear_hidden();
    const auto entries = akfsr::explorer::get_entries();
    if (ImGui::BeginTable("ps", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                          ImVec2(0, 300))) {
      ImGui::TableSetupColumn("masquer");
      ImGui::TableSetupColumn("PS");
      ImGui::TableSetupColumn("position");
      ImGui::TableSetupColumn("draws");
      ImGui::TableSetupColumn("sommets");
      ImGui::TableHeadersRow();
      for (const auto &e : entries) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(e.hash));
        bool hidden = e.hidden;
        if (ImGui::Checkbox("##h", &hidden)) akfsr::explorer::set_hidden(e.hash, hidden);
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::Text("0x%08X", e.hash);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.after_insertion ? "après insertion" : "avant");
        ImGui::TableNextColumn();
        ImGui::Text("%u", e.draws);
        ImGui::TableNextColumn();
        ImGui::Text("%u", e.vertices);
      }
      ImGui::EndTable();
    }
    ImGui::TreePop();
  } else {
    akfsr::explorer::set_enabled(false);
  }
  ImGui::BeginDisabled(s.capture_busy);
  if (ImGui::Button(s.capture_busy ? "Capture en cours..." : "Capturer une frame (F10)")) akfsr::dump::request_capture();
  ImGui::EndDisabled();
  if (!s.last_capture.empty()) ImGui::TextWrapped("Dernière capture : %s", s.last_capture.c_str());
  static std::string report_status;
  if (ImGui::Button("Rapport de test (fichier + presse-papiers)")) {
    ImGui::SetClipboardText(build_report().c_str());
    report_status = write_report() + ", copié";
  }
  if (!report_status.empty()) ImGui::TextWrapped("Rapport : %s", report_status.c_str());
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
      g_addon_dir = module_dir(module);
      reshade::get_config_value(nullptr, "AK-FSR", "ReportKey", g_report_key.key);
      reshade::get_config_value(nullptr, "AK-FSR", "ReportModifier", g_report_key.modifier);
      reshade::get_config_value(nullptr, "AK-FSR", "ProfileKey", g_profile_key.key);
      reshade::get_config_value(nullptr, "AK-FSR", "ProfileModifier", g_profile_key.modifier);
      akfsr::dump::register_events(module_dir(module) / "ak-fsr-dumps");
      akfsr::passes::register_events();
      akfsr::insertion::register_events();
      akfsr::jitter::install();
      // M10.2 : avant la création du device de DXVK (ReShade charge les addons dans D3D11CreateDevice).
      if (akfsr::insertion::get_fsr_settings().fsr4_driver) akfsr::vk_device_hook::install();
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
