// Test natif (Linux) du décodeur de prologues du détour générique (M10.2) :
//   g++ -std=c++20 -Isrc tests/x64_decode_test.cpp -o /tmp/t && /tmp/t
#include "x64_decode.hpp"

#include <cstdio>
#include <vector>

using akfsr::x64::instruction_length;

struct sample {
  const char *what;
  std::vector<uint8_t> bytes;
  std::vector<size_t> lengths;  // longueurs attendues, 0 = refus attendu
};

int main() {
  const sample samples[] = {
      {"gcc/mingw : push r15..rbx ; sub rsp, 0x88",
       {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x55, 0x57, 0x56, 0x53, 0x48, 0x81, 0xEC, 0x88, 0, 0, 0},
       {2, 2, 2, 2, 1, 1, 1, 1, 7}},
      {"MSVC : mov [rsp+8], rbx ; mov [rsp+0x10], rsi ; push rdi ; sub rsp, 0x20",
       {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20},
       {5, 5, 1, 4}},
      {"push rbp ; mov rbp, rsp ; sub rsp, 0x30", {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x30}, {1, 3, 4}},
      {"movaps [rsp+0x40], xmm6 ; mov eax, gs:[0x30]",
       {0x0F, 0x29, 0x74, 0x24, 0x40, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0, 0, 0},
       {5, 9}},
      {"mov dword [rsp+0x28], 0 ; movzx eax, byte [rcx]",
       {0xC7, 0x44, 0x24, 0x28, 0, 0, 0, 0, 0x0F, 0xB6, 0x01}, {8, 3}},
      {"endbr64 ; xor eax, eax ; lea rbp, [rsp-0x10]",
       {0xF3, 0x0F, 0x1E, 0xFA, 0x31, 0xC0, 0x48, 0x8D, 0x6C, 0x24, 0xF0}, {4, 2, 5}},
      {"refus : mov rax, [rip+x]", {0x48, 0x8B, 0x05, 1, 2, 3, 4}, {0}},
      {"refus : jmp rel32", {0xE9, 1, 2, 3, 4}, {0}},
      {"refus : call rel32", {0xE8, 1, 2, 3, 4}, {0}},
  };
  int failures = 0;
  for (const auto &s : samples) {
    std::vector<uint8_t> buf = s.bytes;
    buf.resize(buf.size() + 16, 0xCC);
    size_t pos = 0;
    bool ok = true;
    for (size_t expected : s.lengths) {
      const size_t len = instruction_length(buf.data() + pos);
      if (len != expected) {
        std::printf("ÉCHEC : %s (à +%zu : %zu au lieu de %zu)\n", s.what, pos, len, expected);
        ok = false;
        break;
      }
      pos += len;
    }
    if (!ok) ++failures;
  }
  std::printf(failures == 0 ? "OK\n" : "%d échec(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
