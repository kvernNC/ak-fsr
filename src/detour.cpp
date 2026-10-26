#include "detour.hpp"

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

// Taille de l'octet ModRM et de ce qui le suit (SIB, déplacement) ; 0 si rip-relatif.
size_t modrm_size(const uint8_t *p) {
  const uint8_t modrm = p[0], mod = modrm >> 6, rm = modrm & 7;
  size_t n = 1;
  if (mod != 3 && rm == 4) ++n;  // SIB
  if (mod == 0 && rm == 5) return 0;  // [rip+disp32]
  if (mod == 0 && rm == 4 && (p[1] & 7) == 5) n += 4;  // SIB sans base : disp32
  if (mod == 1) n += 1;
  if (mod == 2) n += 4;
  return n;
}

// Longueur d'une instruction de prologue reconnue, 0 sinon.
size_t instruction_length(const uint8_t *p) {
  if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA) return 4;  // endbr64
  size_t n = 0;
  if (p[0] == 0x66) ++n;  // préfixe de taille d'opérande
  bool rex_w = false;
  if ((p[n] & 0xF0) == 0x40) {
    rex_w = (p[n] & 0x08) != 0;
    ++n;
  }
  const uint8_t op = p[n++];
  if (op >= 0x50 && op <= 0x5F) return n;  // push / pop reg
  if (op == 0x90) return n;                // nop
  size_t m = 0;
  switch (op) {
    case 0x89: case 0x8B: case 0x8D:  // mov r/m,r ; mov r,r/m ; lea
    case 0x31: case 0x33: case 0x29: case 0x2B: case 0x01: case 0x03:  // xor / sub / add
    case 0x85: case 0x39: case 0x3B:  // test / cmp
      m = modrm_size(p + n);
      return m != 0 ? n + m : 0;
    case 0x83:  // op r/m, imm8
      m = modrm_size(p + n);
      return m != 0 ? n + m + 1 : 0;
    case 0x81:  // op r/m, imm32
      m = modrm_size(p + n);
      return m != 0 ? n + m + 4 : 0;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
      return n + (rex_w ? 8 : 4);  // mov reg, imm
    case 0x0F:  // movaps/movups [rsp+x], xmm (0F 29 / 0F 11 / 0F 28 / 0F 10)
      if (p[n] == 0x29 || p[n] == 0x11 || p[n] == 0x28 || p[n] == 0x10) {
        m = modrm_size(p + n + 1);
        return m != 0 ? n + 1 + m : 0;
      }
      return 0;
    default:
      return 0;
  }
}

}  // namespace

bool install(void *target, const void *replacement, void **original, std::string &error) {
  auto *code = static_cast<uint8_t *>(target);
  // Saut de forwarding éventuel (jmp [rip+x]) : suivre jusqu'à la vraie fonction.
  for (int i = 0; i < 4 && code[0] == 0xFF && code[1] == 0x25; ++i) {
    int32_t rel;
    std::memcpy(&rel, code + 2, 4);
    code = *reinterpret_cast<uint8_t **>(code + 6 + rel);
  }
  size_t stolen = 0;
  while (stolen < kJumpSize) {
    const size_t len = instruction_length(code + stolen);
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
