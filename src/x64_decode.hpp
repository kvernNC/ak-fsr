#pragma once

// Décodeur de longueur d'instructions x64, limité aux prologues courants (MSVC, gcc/mingw de Wine) :
// sert au détour générique (detour.cpp). Retourne 0 pour toute instruction inconnue, rip-relative ou de
// branchement, qu'on ne peut pas déplacer telle quelle dans un trampoline. Sans dépendance (testable).

#include <cstddef>
#include <cstdint>

namespace akfsr::x64 {

// Taille de l'octet ModRM et de ce qui le suit (SIB, déplacement) ; 0 si rip-relatif.
inline size_t modrm_size(const uint8_t *p) {
  const uint8_t modrm = p[0], mod = modrm >> 6, rm = modrm & 7;
  size_t n = 1;
  if (mod != 3 && rm == 4) ++n;  // SIB
  if (mod == 0 && rm == 5) return 0;  // [rip+disp32]
  if (mod == 0 && rm == 4 && (p[1] & 7) == 5) n += 4;  // SIB sans base : disp32
  if (mod == 1) n += 1;
  if (mod == 2) n += 4;
  return n;
}

inline size_t instruction_length(const uint8_t *p) {
  if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA) return 4;  // endbr64
  size_t n = 0;
  bool opsize = false;
  for (; n < 4; ++n) {  // préfixes : taille d'opérande, segments fs/gs, rep (movss…)
    if (p[n] == 0x66) opsize = true;
    else if (p[n] != 0x64 && p[n] != 0x65 && p[n] != 0xF2 && p[n] != 0xF3) break;
  }
  bool rex_w = false;
  if ((p[n] & 0xF0) == 0x40) {
    rex_w = (p[n] & 0x08) != 0;
    ++n;
  }
  const uint8_t op = p[n++];
  if (op >= 0x50 && op <= 0x5F) return n;  // push / pop reg
  if (op == 0x90) return n;                // nop
  const size_t imm32 = opsize ? 2 : 4;
  size_t m = 0;
  switch (op) {
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:  // mov, lea
    case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23:  // add / or / and
    case 0x29: case 0x2B: case 0x31: case 0x33: case 0x39: case 0x3B:  // sub / xor / cmp
    case 0x84: case 0x85:                                              // test
      m = modrm_size(p + n);
      return m != 0 ? n + m : 0;
    case 0x80: case 0x83: case 0xC0: case 0xC1: case 0xC6:  // op r/m, imm8 ; shifts ; mov r/m8, imm8
      m = modrm_size(p + n);
      return m != 0 ? n + m + 1 : 0;
    case 0x81: case 0xC7:  // op r/m, imm32 ; mov r/m, imm32
      m = modrm_size(p + n);
      return m != 0 ? n + m + imm32 : 0;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
      return n + (rex_w ? 8 : imm32);  // mov reg, imm
    case 0x0F:
      switch (p[n]) {
        case 0x10: case 0x11: case 0x28: case 0x29:  // movups/movaps/movss/movsd
        case 0xB6: case 0xB7: case 0xBE: case 0xBF:  // movzx / movsx
        case 0x1F:                                   // nop r/m
          m = modrm_size(p + n + 1);
          return m != 0 ? n + 1 + m : 0;
        default:
          return 0;
      }
    default:
      return 0;  // dont call/jmp/jcc/ret : jamais déplacés
  }
}

}  // namespace akfsr::x64
