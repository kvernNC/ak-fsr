// M13 §1 — mesures de performance : voir perf.hpp.
//
// Les mesures GPU arrivent avec quelques frames de retard (timestamps lus sans attente) : chaque frame a sa
// ligne dans un anneau, écrite dans le CSV et comptée dans les moyennes kDelay frames plus tard, une fois ses
// timestamps D3D11 et Vulkan relevés (colonnes vides s'ils ne sont jamais arrivés).

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "perf.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

namespace akfsr::perf {
namespace {

constexpr size_t kRows = 32;       // anneau de lignes (frames)
constexpr uint64_t kDelay = 10;    // une ligne est close kDelay frames après sa frame
constexpr size_t kWindow = 60;     // moyenne glissante
constexpr size_t kD3dSlots = 8;    // jeux de requêtes timestamp D3D11 en vol
constexpr int kMarks = static_cast<int>(mark::count);
constexpr int kWaits = static_cast<int>(wait::count);
constexpr int kCreations = static_cast<int>(creation::count);
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

const char *const kCreationNames[kCreations] = {"amd_shader", "amd_pipeline", "amd_alloc", "amd_submit",
                                                "akfsr_pipeline", "akfsr_d3d_shader"};

struct row {
  uint64_t frame = UINT64_MAX;
  double time_ms = kNaN;       // horodatage du present de la frame (depuis init)
  double cpu_frame_ms = kNaN;  // present -> present
  char insertion = '-';        // S : SMAA du jeu ; B : 0xB2A276FD ; - : AK-FSR n'a pas tourné
  uint32_t runs = 0;           // passages dans le bloc AK-FSR (2 = repli dans la même frame)
  double cpu_run_ms = 0;
  double waits[kWaits] = {};
  double d3d[kMarks] = {};
  uint32_t d3d_mask = 0;
  double vk[4] = {};
  uint32_t vk_mask = 0;
  uint32_t creations[kCreations] = {};
  double creation_ms[kCreations] = {};
  std::string events;
  char rt4[48] = {};
};

// Grandeurs dérivées d'une ligne close (ms, NaN si absente).
enum metric : int {
  m_cpu_frame, m_cpu_run, m_gpu_total, m_pre, m_copy_in, m_gap_in, m_mv, m_fsr, m_vk_end, m_gap_out, m_copy_out,
  m_post, m_count
};
struct derived {
  double m[m_count];
  double waits[kWaits];
  bool ran;
};

struct d3d_slot {
  ID3D11Query *disjoint = nullptr;
  ID3D11Query *stamps[kMarks] = {};
  uint64_t frame = 0;
  uint32_t mask = 0;
  bool pending = false;
};

std::mutex g_mutex;
std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_frame{0};
bool g_overlay = false, g_log = false, g_registered = false;
int g_overlay_corner = 0;
FILE *g_csv = nullptr;
double g_t0 = 0, g_last_present = 0;
std::array<row, kRows> g_rows;

// D3D11
ID3D11DeviceContext *g_ctx = nullptr;  // contexte immédiat (référence gardée)
bool g_d3d_tried = false, g_d3d_ok = false;
std::array<d3d_slot, kD3dSlots> g_slots;
d3d_slot *g_current = nullptr;
uint32_t g_next_slot = 0;
uint64_t g_d3d_lost = 0;

// Fenêtre glissante et dernier résumé (affichage).
std::array<derived, kWindow> g_window{};
size_t g_window_count = 0, g_window_next = 0, g_since_log = 0;
std::string g_summary[3];
uint64_t g_events_logged = 0;

row *find_row(uint64_t frame) {
  row &r = g_rows[frame % kRows];
  return r.frame == frame ? &r : nullptr;
}

void start_row(uint64_t frame) {
  row &r = g_rows[frame % kRows];
  r = row{};
  r.frame = frame;
}

void log_line(reshade::log::level level, const std::string &s) { reshade::log::message(level, ("AK-FSR: " + s).c_str()); }

bool parse_on(const char *section, const char *key, bool fallback) {
  char buf[32] = {};
  size_t size = sizeof(buf);
  if (!reshade::get_config_value(nullptr, section, key, buf, &size)) return fallback;
  std::string v(buf);
  for (char &c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return v == "on" || v == "1" || v == "true" || v == "yes" || v == "oui";
}

// --- Timestamps D3D11 ----------------------------------------------------------------------------------------

bool create_d3d_queries() {
  if (g_d3d_tried) return g_d3d_ok;
  g_d3d_tried = true;
  ID3D11Device *dev = nullptr;
  g_ctx->GetDevice(&dev);
  if (dev == nullptr) return false;
  D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
  g_d3d_ok = true;
  for (auto &s : g_slots) {
    g_d3d_ok = g_d3d_ok && SUCCEEDED(dev->CreateQuery(&disjoint, &s.disjoint));
    for (auto &q : s.stamps) g_d3d_ok = g_d3d_ok && SUCCEEDED(dev->CreateQuery(&stamp, &q));
  }
  dev->Release();
  if (!g_d3d_ok) log_line(reshade::log::level::warning, "mesures : requêtes timestamp D3D11 indisponibles");
  return g_d3d_ok;
}

// Lecture sans attente ; true si le slot est libéré (résultat relevé ou perdu).
bool read_slot(d3d_slot &s) {
  if (!s.pending) return true;
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
  if (g_ctx->GetData(s.disjoint, &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
  uint64_t ticks[kMarks] = {};
  for (int i = 0; i < kMarks; ++i)
    if ((s.mask & (1u << i)) &&
        g_ctx->GetData(s.stamps[i], &ticks[i], sizeof(ticks[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
      return false;
  s.pending = false;
  row *r = find_row(s.frame);
  if (r == nullptr || d.Disjoint || d.Frequency == 0) return true;
  // Ticks bruts -> ns : sous DXVK, même base que les timestamps Vulkan (fréquence = 1e9 / timestampPeriod).
  const double ns_per_tick = 1e9 / static_cast<double>(d.Frequency);
  for (int i = 0; i < kMarks; ++i)
    if (s.mask & (1u << i)) r->d3d[i] = static_cast<double>(ticks[i]) * ns_per_tick;
  r->d3d_mask = s.mask;
  return true;
}

void release_d3d() {
  for (auto &s : g_slots) {
    if (s.disjoint != nullptr) s.disjoint->Release();
    for (auto &q : s.stamps)
      if (q != nullptr) q->Release();
    s = {};
  }
  if (g_ctx != nullptr) g_ctx->Release();
  g_ctx = nullptr;
  g_current = nullptr;
  g_d3d_tried = g_d3d_ok = false;
}

// --- Lignes closes : dérivées, CSV, moyennes -----------------------------------------------------------------

derived derive(const row &r) {
  derived d;
  for (double &v : d.m) v = kNaN;
  for (int i = 0; i < kWaits; ++i) d.waits[i] = r.waits[i];
  d.ran = r.runs != 0;
  d.m[m_cpu_frame] = r.cpu_frame_ms;
  d.m[m_cpu_run] = d.ran ? r.cpu_run_ms : kNaN;
  auto has_d = [&](mark m) { return (r.d3d_mask >> static_cast<int>(m)) & 1u; };
  auto D = [&](mark m) { return r.d3d[static_cast<int>(m)]; };
  auto has_v = [&](int i) { return (r.vk_mask >> i) & 1u; };
  auto span = [](double a, double b) { return (b - a) * 1e-6; };
  // Écarts D3D11 <-> Vulkan : rejetés s'ils sont aberrants (bases de temps différentes).
  auto gap = [](double ms) { return ms > -0.05 && ms < 1000.0 ? std::max(ms, 0.0) : kNaN; };
  if (has_d(mark::block_start) && has_d(mark::block_end)) d.m[m_gpu_total] = span(D(mark::block_start), D(mark::block_end));
  if (has_d(mark::block_start) && has_d(mark::copy_in_start))
    d.m[m_pre] = span(D(mark::block_start), D(mark::copy_in_start));
  if (has_d(mark::copy_in_start) && has_d(mark::copy_in_end))
    d.m[m_copy_in] = span(D(mark::copy_in_start), D(mark::copy_in_end));
  if (has_d(mark::copy_out_start) && has_d(mark::copy_out_end))
    d.m[m_copy_out] = span(D(mark::copy_out_start), D(mark::copy_out_end));
  if (has_d(mark::copy_out_end) && has_d(mark::block_end)) d.m[m_post] = span(D(mark::copy_out_end), D(mark::block_end));
  if (has_v(0) && has_v(1)) d.m[m_mv] = span(r.vk[0], r.vk[1]);
  if (has_v(1) && has_v(2)) d.m[m_fsr] = span(r.vk[1], r.vk[2]);
  if (has_v(2) && has_v(3)) d.m[m_vk_end] = span(r.vk[2], r.vk[3]);
  if (has_d(mark::copy_in_end) && has_v(0)) d.m[m_gap_in] = gap(span(D(mark::copy_in_end), r.vk[0]));
  if (has_v(3) && has_d(mark::copy_out_start)) d.m[m_gap_out] = gap(span(r.vk[3], D(mark::copy_out_start)));
  return d;
}

void csv_value(std::string &line, double v) {
  char buf[32];
  if (std::isnan(v)) {
    line += ',';
    return;
  }
  std::snprintf(buf, sizeof(buf), ",%.3f", v);
  line += buf;
}

std::string describe_creations(const row &r) {
  std::string created;
  for (int i = 0; i < kCreations; ++i)
    if (r.creations[i] != 0) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%s%s=%u(%.1fms)", created.empty() ? "" : " ", kCreationNames[i],
                    r.creations[i], r.creation_ms[i]);
      created += buf;
    }
  return created;
}

void write_csv(const row &r, const derived &d, const std::string &created) {
  if (g_csv == nullptr) return;
  std::string line = std::to_string(r.frame);
  csv_value(line, r.time_ms * 1e-3);
  csv_value(line, d.m[m_cpu_frame]);
  line += ',';
  line += r.insertion;
  csv_value(line, d.m[m_cpu_run]);
  for (double w : d.waits) csv_value(line, w);
  for (int m : {m_gpu_total, m_pre, m_copy_in, m_gap_in, m_mv, m_fsr, m_vk_end, m_gap_out, m_copy_out, m_post})
    csv_value(line, d.m[m]);
  line += ',';
  line += r.rt4;
  line += ',' + created + ",\"";
  for (char c : r.events) line += c == '"' ? '\'' : c;
  line += "\"\n";
  std::fputs(line.c_str(), g_csv);
  if (r.frame % 60 == 0) std::fflush(g_csv);
}

struct stat {
  double avg = kNaN, max = kNaN;
};
stat window_stat(int metric, int wait_index = -1) {
  double sum = 0, mx = 0;
  size_t n = 0;
  for (size_t i = 0; i < g_window_count; ++i) {
    const derived &d = g_window[i];
    if (wait_index < 0 && !d.ran && metric != m_cpu_frame) continue;
    const double v = wait_index >= 0 ? d.waits[wait_index] : d.m[metric];
    if (std::isnan(v)) continue;
    sum += v;
    mx = std::max(mx, v);
    ++n;
  }
  return n == 0 ? stat{} : stat{sum / static_cast<double>(n), mx};
}

void update_summary() {
  auto a = [](int m) { return window_stat(m).avg; };
  auto w = [](wait x) { return window_stat(0, static_cast<int>(x)); };
  const double cpu_frame = a(m_cpu_frame);
  const double vk_in_d3d = a(m_gap_in) + a(m_mv) + a(m_fsr) + a(m_vk_end) + a(m_gap_out);
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "AK-FSR GPU (moy. %zu) : total %.2f ms | entrées %.2f + copies %.2f | MV %.2f | FSR %.2f | "
                "fin %.2f | copie sortie %.2f + après %.2f | bulles %.2f + %.2f",
                g_window_count, a(m_gpu_total), a(m_pre), a(m_copy_in), a(m_mv), a(m_fsr), a(m_vk_end), a(m_copy_out),
                a(m_post), a(m_gap_in), a(m_gap_out));
  g_summary[0] = buf;
  const stat f = w(wait::flush), l = w(wait::lock), h = w(wait::held), fe = w(wait::fence), i = w(wait::idle),
             dr = w(wait::driver);
  std::snprintf(buf, sizeof(buf),
                "CPU attente moy/max (ms) : flush %.2f/%.2f | verrou %.2f/%.2f | tenu %.2f/%.2f | fence %.2f/%.2f | "
                "idle %.2f/%.2f | pilote %.2f/%.2f",
                f.avg, f.max, l.avg, l.max, h.avg, h.max, fe.avg, fe.max, i.avg, i.max, dr.avg, dr.max);
  g_summary[1] = buf;
  const stat frame_stat = window_stat(m_cpu_frame);
  std::snprintf(buf, sizeof(buf), "frame %.2f ms (%.1f fps, pire %.2f ms) | CPU AK-FSR %.2f ms | bloc Vulkan vu par D3D11 %.2f ms",
                cpu_frame, cpu_frame > 0 ? 1000.0 / cpu_frame : 0.0, frame_stat.max, a(m_cpu_run), vk_in_d3d);
  g_summary[2] = buf;
}

void close_row(row &r) {
  const derived d = derive(r);
  const std::string created = describe_creations(r);
  write_csv(r, d, created);
  // Créations hors démarrage (pipelines, allocations, soumissions de la DLL AMD) : candidates aux à-coups.
  if (g_log && !created.empty() && ++g_events_logged <= 500) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "créations [frame %llu, %.3f s] ", static_cast<unsigned long long>(r.frame),
                  r.time_ms * 1e-3);
    log_line(reshade::log::level::info, buf + created);
  }
  g_window[g_window_next] = d;
  g_window_next = (g_window_next + 1) % kWindow;
  g_window_count = std::min(g_window_count + 1, kWindow);
  update_summary();
  if (g_log && ++g_since_log >= kWindow) {
    g_since_log = 0;
    for (const auto &s : g_summary) log_line(reshade::log::level::info, "mesures : " + s);
  }
}

// --- Affichage ---------------------------------------------------------------------------------------------

void on_overlay(reshade::api::effect_runtime *) {
  std::string lines[3];
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < 3; ++i) lines[i] = g_summary[i];
  }
  if (lines[0].empty()) return;
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const bool right = g_overlay_corner == 1 || g_overlay_corner == 3, bottom = g_overlay_corner >= 2;
  ImGui::SetNextWindowPos(ImVec2(right ? size.x - 8.0f : 8.0f, bottom ? size.y - 8.0f : 8.0f), ImGuiCond_Always,
                          ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##ak-fsr-perf", nullptr, kFlags)) {
    for (const auto &l : lines) ImGui::TextUnformatted(l.c_str());
  }
  ImGui::End();
}

}  // namespace

double now_ms() {
  static const double freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

void init(const std::filesystem::path &addon_dir) {
  const bool frame_log = parse_on("Debug", "FrameLog", false);
  g_overlay = parse_on("Debug", "PerfOverlay", false);
  g_log = parse_on("Debug", "PerfLog", frame_log || g_overlay);
  reshade::get_config_value(nullptr, "Debug", "PerfOverlayCorner", g_overlay_corner);
  g_t0 = g_last_present = now_ms();
  start_row(0);
  if (frame_log) {
    const auto path = addon_dir / "ak-fsr-frames.csv";
    g_csv = _wfopen(path.c_str(), L"w");
    if (g_csv != nullptr) {
      std::setvbuf(g_csv, nullptr, _IOFBF, 1 << 16);
      std::fputs("frame,time_s,cpu_frame_ms,insertion,cpu_akfsr_ms,wait_flush_ms,wait_lock_ms,lock_held_ms,"
                 "wait_fence_ms,wait_idle_ms,wait_driver_ms,gpu_total_ms,gpu_pre_ms,gpu_copy_in_ms,gpu_gap_in_ms,"
                 "gpu_mv_ms,gpu_fsr_ms,gpu_vk_end_ms,gpu_gap_out_ms,gpu_copy_out_ms,gpu_post_ms,rt4,creations,events\n",
                 g_csv);
    } else {
      log_line(reshade::log::level::error, "mesures : impossible d'ouvrir ak-fsr-frames.csv");
    }
  }
  g_enabled = frame_log || g_overlay || g_log;
  if (g_overlay && !g_registered) {
    // reshade_overlay force ReShade à préparer ImGui à chaque frame : seulement si la ligne est demandée.
    reshade::register_event<reshade::addon_event::reshade_overlay>(on_overlay);
    g_registered = true;
  }
  if (g_enabled)
    log_line(reshade::log::level::info, std::string("mesures M13 actives : FrameLog ") + (frame_log ? "on" : "off") +
                                            ", PerfOverlay " + (g_overlay ? "on" : "off") + ", PerfLog " +
                                            (g_log ? "on" : "off"));
}

void shutdown() {
  std::lock_guard lock(g_mutex);
  if (g_registered) {
    reshade::unregister_event<reshade::addon_event::reshade_overlay>(on_overlay);
    g_registered = false;
  }
  release_d3d();
  if (g_csv != nullptr) {
    std::fclose(g_csv);
    g_csv = nullptr;
  }
}

void release_device_objects() {
  std::lock_guard lock(g_mutex);
  release_d3d();
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }
uint64_t frame() { return g_frame.load(std::memory_order_relaxed); }

void on_present() {
  if (!enabled()) return;
  std::lock_guard lock(g_mutex);
  const double t = now_ms();
  const uint64_t f = g_frame.load();
  if (row *r = find_row(f)) {
    r->time_ms = t - g_t0;
    r->cpu_frame_ms = t - g_last_present;
  }
  g_last_present = t;
  if (g_ctx != nullptr)
    for (auto &s : g_slots) read_slot(s);
  if (f >= kDelay)
    if (row *r = find_row(f - kDelay)) close_row(*r);
  g_frame.store(f + 1);
  start_row(f + 1);
}

void add_wait(wait w, double ms) {
  if (!enabled()) return;
  std::lock_guard lock(g_mutex);
  if (row *r = find_row(frame())) r->waits[static_cast<int>(w)] += ms;
}

void add_cpu_run(double ms) {
  if (!enabled()) return;
  std::lock_guard lock(g_mutex);
  if (row *r = find_row(frame())) r->cpu_run_ms += ms;
}

void note_insertion(bool smaa) {
  if (!enabled()) return;
  std::lock_guard lock(g_mutex);
  if (row *r = find_row(frame())) {
    r->insertion = smaa ? 'S' : 'B';
    ++r->runs;
  }
}

void d3d_mark(ID3D11DeviceContext *ctx, mark m) {
  if (!enabled() || ctx == nullptr) return;
  std::lock_guard lock(g_mutex);
  if (g_ctx == nullptr) {
    g_ctx = ctx;
    g_ctx->AddRef();
  }
  // Les marques passent par le contexte immédiat natif, obtenu par ReShade ou par GetImmediateContext : un seul
  // objet sous DXVK, donc pas de comparaison de pointeurs. g_ctx sert aux lectures (on_present).
  if (!create_d3d_queries()) return;
  const int i = static_cast<int>(m);
  if (m == mark::block_start) {
    if (g_current != nullptr) {  // bloc précédent non fermé : abandonné
      ctx->End(g_current->disjoint);
      g_current->pending = false;
    }
    d3d_slot &s = g_slots[g_next_slot++ % kD3dSlots];
    if (!read_slot(s)) ++g_d3d_lost;  // résultats encore en vol après kD3dSlots blocs : perdus
    s.pending = false;
    s.frame = frame();
    s.mask = 0;
    ctx->Begin(s.disjoint);
    g_current = &s;
  }
  if (g_current == nullptr) return;
  ctx->End(g_current->stamps[i]);
  g_current->mask |= 1u << i;
  if (m == mark::block_end) {
    ctx->End(g_current->disjoint);
    g_current->pending = true;
    g_current = nullptr;
  }
}

void set_vk_stamps(uint64_t f, const double ns[4], const bool valid[4]) {
  if (!enabled()) return;
  std::lock_guard lock(g_mutex);
  row *r = find_row(f);
  if (r == nullptr) return;
  r->vk_mask = 0;
  for (int i = 0; i < 4; ++i)
    if (valid[i]) {
      r->vk[i] = ns[i];
      r->vk_mask |= 1u << i;
    }
}

void note_rt4(uint32_t clears_imm, uint32_t clears_def, uint32_t draws_imm, uint32_t draws_def, bool used) {
  if (!enabled()) return;
  std::lock_guard lock(g_mutex);
  // Format : c<effacements imm>/<diff> d<draws imm>/<diff> u|i (utilisée / ignorée).
  if (row *r = find_row(frame()))
    std::snprintf(r->rt4, sizeof(r->rt4), "c%u/%u d%u/%u %c", clears_imm, clears_def, draws_imm, draws_def,
                  used ? 'u' : 'i');
}

void add_creation(creation c, double ms) {
  if (!enabled()) return;
  std::lock_guard lock(g_mutex);
  if (row *r = find_row(frame())) {
    ++r->creations[static_cast<int>(c)];
    r->creation_ms[static_cast<int>(c)] += ms;
  }
}

void event(const char *fmt, ...) {
  char text[384];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  const uint64_t f = frame();
  const double t = (now_ms() - g_t0) * 1e-3;
  {
    std::lock_guard lock(g_mutex);
    if (row *r = find_row(f)) {
      if (!r->events.empty()) r->events += "; ";
      r->events += text;
    }
    if (++g_events_logged > 500) return;  // le CSV garde tout ; le log est borné
  }
  char line[480];
  std::snprintf(line, sizeof(line), "événement [frame %llu, %.3f s] %s", static_cast<unsigned long long>(f), t, text);
  log_line(reshade::log::level::info, line);
}

}  // namespace akfsr::perf
