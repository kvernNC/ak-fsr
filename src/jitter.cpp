#include "jitter.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>

#include <reshade.hpp>

namespace akfsr::jitter {
namespace {

// Début de FPerspectiveMatrix(HalfFOVX, HalfFOVY, MultFOVX, MultFOVY, MinZ, MaxZ) (Steam v1.6.2.0,
// RVA 0x10420) : push rbx ; sub rsp,0x70 ; movss xmm5,[rsp+0xA8] ; movss xmm4,[rsp+0xB0] ; movaps…
// Aucun adressage relatif à rip : les 15 premiers octets se déplacent tels quels dans le trampoline.
constexpr uint8_t kPattern[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x70, 0xF3, 0x0F, 0x10, 0xAC, 0x24, 0xA8,
                                0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0xA4, 0x24, 0xB0, 0x00, 0x00, 0x00};
constexpr size_t kStolen = 15;  // push rbx (2) + sub rsp (4) + movss xmm5 (9)

// Disposition de FMatrix (ligne par ligne, 4x4 float).
constexpr size_t kM00 = 0, kM11 = 5, kM20 = 8, kM21 = 9, kM32 = 14;

using perspective_fn = void *(*)(void *self, float half_fov_x, float half_fov_y, float mult_fov_x, float mult_fov_y,
                                 float min_z, float max_z);

struct caller_slot {
  std::atomic<uint32_t> rva{0};
  std::atomic<uint64_t> calls{0};
  std::atomic<float> m00{0}, m11{0}, min_z{0};
  std::atomic<uint32_t> matches{0};  // correspondances consécutives avec le CB View
  std::atomic<bool> main_view{false};
};
constexpr size_t kMaxCallers = 8;
std::array<caller_slot, kMaxCallers> g_callers;

uint8_t *g_target = nullptr;
uint8_t *g_trampoline = nullptr;
std::array<uint8_t, 16> g_saved{};
perspective_fn g_original = nullptr;
uintptr_t g_module_base = 0;

std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_width{0}, g_height{0};
std::atomic<uint32_t> g_phase{0};
constexpr uint32_t kPhaseCount = 8;  // FSR en AA natif : 8 * (sortie/rendu)^2
std::atomic<float> g_applied_x{0}, g_applied_y{0};

std::mutex g_status_mutex;
status g_status;

float halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (; index > 0; index /= base) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(index % base);
  }
  return r;
}

caller_slot *slot_for(uint32_t rva) {
  for (auto &s : g_callers) {
    const uint32_t cur = s.rva.load(std::memory_order_relaxed);
    if (cur == rva) return &s;
    uint32_t expected = 0;
    if (cur == 0 && s.rva.compare_exchange_strong(expected, rva)) return &s;
    if (expected == rva) return &s;
  }
  return nullptr;
}

void *detour(void *self, float half_fov_x, float half_fov_y, float mult_fov_x, float mult_fov_y, float min_z,
             float max_z) {
  void *result = g_original(self, half_fov_x, half_fov_y, mult_fov_x, mult_fov_y, min_z, max_z);
  auto *m = static_cast<float *>(self);
  const uint32_t rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_module_base);
  caller_slot *slot = slot_for(rva);
  if (slot == nullptr) return result;
  slot->calls.fetch_add(1, std::memory_order_relaxed);
  slot->m00.store(m[kM00], std::memory_order_relaxed);
  slot->m11.store(m[kM11], std::memory_order_relaxed);
  slot->min_z.store(-m[kM32], std::memory_order_relaxed);  // vrai en profondeur infinie (M22 = 1)

  const uint32_t w = g_width.load(), h = g_height.load();
  if (!g_enabled.load(std::memory_order_relaxed) || !slot->main_view.load(std::memory_order_relaxed) || w == 0 ||
      h == 0)
    return result;
  // Halton(2,3) centré, en pixels ; l'indice 0 de Halton vaut 0, on commence à 1.
  const uint32_t i = g_phase.load(std::memory_order_relaxed) % kPhaseCount + 1;
  const float px = halton(i, 2) - 0.5f, py = halton(i, 3) - 0.5f;
  // clip.x += jx·w : décalage NDC ; y NDC vers le haut, y pixel vers le bas.
  m[kM20] += 2.0f * px / static_cast<float>(w);
  m[kM21] += -2.0f * py / static_cast<float>(h);
  g_applied_x.store(px, std::memory_order_relaxed);
  g_applied_y.store(py, std::memory_order_relaxed);
  return result;
}

uint8_t *find_pattern() {
  auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  const auto *sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
    if (std::memcmp(sec->Name, ".text", 5) != 0) continue;
    uint8_t *begin = base + sec->VirtualAddress;
    const size_t size = sec->Misc.VirtualSize;
    uint8_t *found = nullptr;
    size_t count = 0;
    for (size_t i = 0; i + sizeof(kPattern) <= size; ++i) {
      if (begin[i] == kPattern[0] && std::memcmp(begin + i, kPattern, sizeof(kPattern)) == 0) {
        found = begin + i;
        ++count;
      }
    }
    return count == 1 ? found : nullptr;  // une seule occurrence exigée
  }
  return nullptr;
}

void write_abs_jump(uint8_t *at, const void *destination) {
  at[0] = 0xFF;  // jmp qword ptr [rip+0]
  at[1] = 0x25;
  std::memset(at + 2, 0, 4);
  const uint64_t dest = reinterpret_cast<uint64_t>(destination);
  std::memcpy(at + 6, &dest, 8);
}

void set_message(const std::string &msg, bool installed) {
  std::lock_guard lock(g_status_mutex);
  g_status.message = msg;
  g_status.installed = installed;
}

}  // namespace

bool install() {
  if (g_target != nullptr) return true;
  g_module_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  uint8_t *target = find_pattern();
  if (target == nullptr) {
    set_message("motif de FPerspectiveMatrix introuvable ou ambigu (version de l'exe ?)", false);
    reshade::log::message(reshade::log::level::error, "AK-FSR: jitter : motif de FPerspectiveMatrix introuvable");
    return false;
  }
  g_trampoline = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (g_trampoline == nullptr) {
    set_message("VirtualAlloc du trampoline en échec", false);
    return false;
  }
  // Trampoline : octets déplacés puis saut vers la suite de la fonction d'origine.
  std::memcpy(g_trampoline, target, kStolen);
  write_abs_jump(g_trampoline + kStolen, target + kStolen);
  FlushInstructionCache(GetCurrentProcess(), g_trampoline, 64);
  g_original = reinterpret_cast<perspective_fn>(g_trampoline);

  DWORD old = 0;
  if (!VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &old)) {
    set_message("VirtualProtect en échec", false);
    return false;
  }
  std::memcpy(g_saved.data(), target, g_saved.size());
  uint8_t patch[16];
  write_abs_jump(patch, reinterpret_cast<const void *>(&detour));
  patch[14] = 0x90;  // nop : complète l'instruction déplacée (15 octets)
  patch[15] = target[15];
  std::memcpy(target, patch, 16);
  VirtualProtect(target, 16, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, 16);
  g_target = target;

  const uint32_t rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target) - g_module_base);
  {
    std::lock_guard lock(g_status_mutex);
    g_status.function_rva = rva;
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "détour de FPerspectiveMatrix installé (RVA 0x%x)", rva);
  set_message(buf, true);
  reshade::log::message(reshade::log::level::info, (std::string("AK-FSR: jitter : ") + buf).c_str());
  return true;
}

void uninstall() {
  if (g_target == nullptr) return;
  DWORD old = 0;
  if (VirtualProtect(g_target, 16, PAGE_EXECUTE_READWRITE, &old)) {
    std::memcpy(g_target, g_saved.data(), g_saved.size());
    VirtualProtect(g_target, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_target, 16);
  }
  g_target = nullptr;
  // Le trampoline n'est pas libéré : un thread du jeu pourrait encore s'y trouver.
}

void set_enabled(bool enabled) { g_enabled = enabled; }

void set_render_size(uint32_t width, uint32_t height) {
  g_width = width;
  g_height = height;
}

void on_present() { g_phase.fetch_add(1, std::memory_order_relaxed); }

void learn_main_view(float scale_x, float scale_y, float near_plane) {
  // Un appelant est « vue principale » quand sa projection correspond à celle du CB View pendant
  // plusieurs frames consécutives (le jitter ne touche pas M00, M11 ni MinZ).
  auto close = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance * std::fabs(b); };
  for (auto &s : g_callers) {
    if (s.rva.load() == 0) continue;
    const bool match = close(s.m00.load(), scale_x, 0.002f) && close(s.m11.load(), scale_y, 0.002f) &&
                       close(s.min_z.load(), near_plane, 0.01f);
    const uint32_t n = match ? s.matches.load() + 1 : 0;
    s.matches = n;
    if (n >= 30 && !s.main_view.exchange(true)) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "AK-FSR: jitter : appelant 0x%x reconnu comme vue principale", s.rva.load());
      reshade::log::message(reshade::log::level::info, buf);
    }
  }
}

void recover_from_view(const float *rows, float &jx_ndc, float &jy_ndc) {
  float sx = 0, sy = 0, sw = 0;
  for (int i = 0; i < 3; ++i) {
    const float w = rows[i * 4 + 3];
    sx += w * rows[i * 4 + 0];
    sy += w * rows[i * 4 + 1];
    sw += w * w;
  }
  jx_ndc = sw > 0 ? sx / sw : 0.0f;
  jy_ndc = sw > 0 ? sy / sw : 0.0f;
}

status get_status() {
  status s;
  {
    std::lock_guard lock(g_status_mutex);
    s = g_status;
  }
  s.phase = g_phase.load() % kPhaseCount;
  s.phase_count = kPhaseCount;
  s.applied_x_px = g_applied_x.load();
  s.applied_y_px = g_applied_y.load();
  for (auto &slot : g_callers) {
    if (slot.rva.load() == 0) continue;
    s.callers.push_back(
        {slot.rva.load(), slot.calls.load(), slot.m00.load(), slot.m11.load(), slot.min_z.load(), slot.main_view.load()});
  }
  return s;
}

}  // namespace akfsr::jitter
