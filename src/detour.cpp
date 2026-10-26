#include "detour.hpp"
#include "x64_decode.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace akfsr::detour {
namespace {

constexpr size_t kJumpSize = 14;  // jmp qword ptr [rip+0] ; adresse 64 bits

void write_abs_jump(uint8_t *at, const void *destination) {
  at[0] = 0xFF;
  at[1] = 0x25;
  std::memset(at + 2, 0, 4);
  const uint64_t dest = reinterpret_cast<uint64_t>(destination);
  std::memcpy(at + 6, &dest, 8);
}

}  // namespace

bool install(void *target, const void *replacement, void **original, std::string &error) {
  auto *code = static_cast<uint8_t *>(target);
  // Sauts de forwarding éventuels (jmp [rip+x], jmp rel32) : suivre jusqu'à la vraie fonction.
  for (int i = 0; i < 4; ++i) {
    int32_t rel;
    if (code[0] == 0xFF && code[1] == 0x25) {
      std::memcpy(&rel, code + 2, 4);
      code = *reinterpret_cast<uint8_t **>(code + 6 + rel);
    } else if (code[0] == 0xE9) {
      std::memcpy(&rel, code + 1, 4);
      code = code + 5 + rel;
    } else {
      break;
    }
  }
  size_t stolen = 0;
  while (stolen < kJumpSize) {
    const size_t len = x64::instruction_length(code + stolen);
    if (len == 0) {
      char buf[128];
      int pos = std::snprintf(buf, sizeof(buf), "prologue non reconnu à +%zu :", stolen);
      for (size_t i = 0; i < 16 && pos < static_cast<int>(sizeof(buf)) - 4; ++i)
        pos += std::snprintf(buf + pos, sizeof(buf) - pos, " %02X", code[i]);
      error = buf;
      return false;
    }
    stolen += len;
  }
  auto *trampoline =
      static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (trampoline == nullptr) {
    error = "VirtualAlloc du trampoline en échec";
    return false;
  }
  std::memcpy(trampoline, code, stolen);
  write_abs_jump(trampoline + stolen, code + stolen);
  FlushInstructionCache(GetCurrentProcess(), trampoline, 64);

  DWORD old = 0;
  if (!VirtualProtect(code, stolen, PAGE_EXECUTE_READWRITE, &old)) {
    error = "VirtualProtect en échec";
    return false;
  }
  *original = trampoline;  // avant le patch : un autre thread peut entrer dans le détour aussitôt
  uint8_t patch[32];
  write_abs_jump(patch, replacement);
  std::memset(patch + kJumpSize, 0x90, stolen - kJumpSize);
  std::memcpy(code, patch, stolen);
  VirtualProtect(code, stolen, old, &old);
  FlushInstructionCache(GetCurrentProcess(), code, stolen);
  return true;
}

}  // namespace akfsr::detour
