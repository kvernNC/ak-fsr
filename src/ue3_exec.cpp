#include "ue3_exec.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <reshade.hpp>

namespace akfsr::ue3 {
namespace {

void log(reshade::log::level level, const std::string &s) { reshade::log::message(level, ("AK-FSR: [ue3] " + s).c_str()); }

// --- Localisation --------------------------------------------------------------------------------------------

using exec_fn = int(__fastcall *)(void *self, const wchar_t *cmd, void *ar);

struct targets {
  bool tried = false, ok = false;
  std::string error;
  uintptr_t module = 0;
  exec_fn exec = nullptr;
  void *settings = nullptr;  // GSystemSettings
} g_t;

struct section {
  uintptr_t begin = 0, end = 0;
};

bool find_section(uintptr_t module, const char *name, section &out) {
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(module);
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(module + dos->e_lfanew);
  auto *sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (std::strncmp(reinterpret_cast<const char *>(sec->Name), name, 8) == 0) {
      out.begin = module + sec->VirtualAddress;
      out.end = out.begin + sec->Misc.VirtualSize;
      return true;
    }
  }
  return false;
}

// Chaîne UTF-16 exacte (précédée et suivie d'un caractère nul), alignée sur 2 octets ; unique exigée.
uintptr_t find_wstring(const section &s, const wchar_t *text) {
  const size_t len = std::wcslen(text) * 2;
  uintptr_t found = 0;
  for (uintptr_t p = (s.begin + 1) & ~uintptr_t(1); p + len + 2 <= s.end; p += 2) {
    if (std::memcmp(reinterpret_cast<const void *>(p), text, len) != 0) continue;
    if (*reinterpret_cast<const wchar_t *>(p - 2) != 0 || *reinterpret_cast<const wchar_t *>(p + len) != 0) continue;
    if (found != 0) return 0;  // plusieurs : refus
    found = p;
  }
  return found;
}

// Unique `lea r64, [rip + disp32]` (48/4C 8D modrm 00 reg 101) vers `target`.
uintptr_t find_lea(const section &text, uintptr_t target) {
  uintptr_t found = 0;
  for (uintptr_t p = text.begin; p + 7 <= text.end; ++p) {
    const auto *b = reinterpret_cast<const uint8_t *>(p);
    if ((b[0] != 0x48 && b[0] != 0x4C) || b[1] != 0x8D || (b[2] & 0xC7) != 0x05) continue;
    int32_t disp;
    std::memcpy(&disp, b + 3, 4);
    if (p + 7 + disp != target) continue;
    if (found != 0) return 0;
    found = p;
  }
  return found;
}

// Unique `call rel32` vers `target`.
uintptr_t find_call(const section &text, uintptr_t target) {
  uintptr_t found = 0;
  for (uintptr_t p = text.begin; p + 5 <= text.end; ++p) {
    if (*reinterpret_cast<const uint8_t *>(p) != 0xE8) continue;
    int32_t rel;
    std::memcpy(&rel, reinterpret_cast<const void *>(p + 1), 4);
    if (p + 5 + rel != target) continue;
    if (found != 0) return 0;
    found = p;
  }
  return found;
}

bool locate() {
  if (g_t.tried) return g_t.ok;
  g_t.tried = true;
  g_t.module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  section text, rdata;
  if (!find_section(g_t.module, ".text", text) || !find_section(g_t.module, ".rdata", rdata)) {
    g_t.error = "sections .text / .rdata introuvables";
    return false;
  }
  const uintptr_t str = find_wstring(rdata, L"SCALE");
  if (str == 0) {
    g_t.error = "chaîne L\"SCALE\" absente ou multiple";
    return false;
  }
  const uintptr_t lea = find_lea(text, str);
  if (lea == 0) {
    g_t.error = "référence à L\"SCALE\" absente ou multiple";
    return false;
  }
  DWORD64 image = 0;
  const RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(lea, &image, nullptr);
  if (rf == nullptr) {
    g_t.error = "pas d'entrée .pdata pour la référence";
    return false;
  }
  // Entrée chaînée (fragment) : remonter à la fonction primaire.
  for (int guard = 0; guard < 8; ++guard) {
    const auto *unwind = reinterpret_cast<const uint8_t *>(image + rf->UnwindData);
    if (((unwind[0] >> 3) & UNW_FLAG_CHAININFO) == 0) break;
    const uint8_t count = unwind[2];
    rf = reinterpret_cast<const RUNTIME_FUNCTION *>(unwind + 4 + ((count + 1) & ~1) * 2);
  }
  const uintptr_t func = image + rf->BeginAddress;
  const uintptr_t call = find_call(text, func);
  if (call == 0) {
    g_t.error = "appel de USystemSettings::Exec absent ou multiple";
    return false;
  }
  // `lea rcx, [rip + disp32]` (48 8D 0D) dans les 16 octets avant l'appel : this = GSystemSettings.
  uintptr_t settings = 0;
  for (uintptr_t p = call - 7; p + 16 >= call && p > text.begin; --p) {
    const auto *b = reinterpret_cast<const uint8_t *>(p);
    if (b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x0D) {
      int32_t disp;
      std::memcpy(&disp, b + 3, 4);
      settings = p + 7 + disp;
      break;
    }
  }
  if (settings == 0) {
    g_t.error = "lea rcx, [GSystemSettings] introuvable avant l'appel";
    return false;
  }
  g_t.exec = reinterpret_cast<exec_fn>(func);
  g_t.settings = reinterpret_cast<void *>(settings);
  g_t.ok = true;
  return true;
}

// --- FOutputDevice à nous ------------------------------------------------------------------------------------

// Disposition attendue par le moteur : pointeur de vtable, puis indicateurs (UBOOL). Entrées : 0 destructeur,
// 1 Serialize(const TCHAR*, EName), 2 Flush, 3 TearDown. Marge de 64 octets pour d'éventuels champs lus.
struct output_device;
void __fastcall od_dtor(output_device *, unsigned) {}
void __fastcall od_serialize(output_device *, const wchar_t *text, int) {
  if (text == nullptr) return;
  char utf8[1024];
  WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
  log(reshade::log::level::info, std::string("moteur : ") + utf8);
}
void __fastcall od_noop(output_device *) {}
void *const kVtable[] = {reinterpret_cast<void *>(&od_dtor), reinterpret_cast<void *>(&od_serialize),
                         reinterpret_cast<void *>(&od_noop), reinterpret_cast<void *>(&od_noop)};
struct output_device {
  void *const *vtable = kVtable;
  uint32_t allow_suppression = 0, suppress_event_tag = 0, auto_emit_line_terminator = 1;
  uint8_t spare[64] = {};
};

// Appel protégé (aucun objet C++ à détruire ici).
DWORD call_exec(const wchar_t *cmd, output_device *ar, int *result) {
  __try {
    *result = g_t.exec(g_t.settings, cmd, ar);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return GetExceptionCode();
  }
}

void run(const std::string &command) {
  if (!locate()) {
    log(reshade::log::level::error, "commande refusée (" + command + ") : " + g_t.error);
    return;
  }
  wchar_t wide[512];
  if (MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wide, 512) == 0) return;
  output_device ar;
  int result = 0;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "exécution sur le thread %lu : ", GetCurrentThreadId());
  log(reshade::log::level::info, buf + command);
  if (const DWORD code = call_exec(wide, &ar, &result)) {
    std::snprintf(buf, sizeof(buf), "exception 0x%08lX dans USystemSettings::Exec", static_cast<unsigned long>(code));
    log(reshade::log::level::error, buf);
    return;
  }
  log(reshade::log::level::info, result ? "commande reconnue par le moteur" : "commande non reconnue par le moteur");
}

// --- File de commandes et thread de la fenêtre ---------------------------------------------------------------

constexpr UINT kMessage = WM_APP + 0x4B33;  // « K3 »
std::mutex g_mutex;
std::deque<std::string> g_queue;
HWND g_hwnd = nullptr;
WNDPROC g_original = nullptr;

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == kMessage) {
    std::deque<std::string> pending;
    {
      std::lock_guard lock(g_mutex);
      pending.swap(g_queue);
    }
    for (const auto &c : pending) run(c);
    return 0;
  }
  return CallWindowProcW(g_original, hwnd, msg, wp, lp);
}

}  // namespace

void post(HWND__ *hwnd, const std::string &command) {
  if (hwnd == nullptr) {
    log(reshade::log::level::error, "pas de fenêtre de jeu connue : commande ignorée");
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    if (g_hwnd != hwnd) {
      g_original = reinterpret_cast<WNDPROC>(
          SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
      g_hwnd = g_original != nullptr ? hwnd : nullptr;
      char buf[128];
      std::snprintf(buf, sizeof(buf), "fenêtre %p sous-classée (thread de la fenêtre %lu, present %lu)",
                    static_cast<void *>(hwnd), GetWindowThreadProcessId(hwnd, nullptr), GetCurrentThreadId());
      log(g_hwnd != nullptr ? reshade::log::level::info : reshade::log::level::error,
          g_hwnd != nullptr ? buf : "sous-classement de la fenêtre impossible");
      if (g_hwnd == nullptr) return;
    }
    g_queue.push_back(command);
  }
  PostMessageW(hwnd, kMessage, 0, 0);
}

void describe() {
  if (!locate()) {
    log(reshade::log::level::warning, "commandes du moteur indisponibles : " + g_t.error);
    return;
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf), "USystemSettings::Exec RVA 0x%llX, GSystemSettings RVA 0x%llX",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_t.exec) - g_t.module),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_t.settings) - g_t.module));
  log(reshade::log::level::info, buf);
}

}  // namespace akfsr::ue3
