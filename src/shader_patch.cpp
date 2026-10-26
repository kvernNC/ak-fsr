#include "shader_patch.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

#include <reshade.hpp>

namespace akfsr::shader_patch {
namespace {

// ---------------------------------------------------------------------------------------------
// Checksum DXBC : MD5 sur le conteneur à partir de l'octet 20, avec le bourrage spécifique de DXBC
// (vérifié sur les 193 shaders d'une capture).

struct md5 {
  uint32_t s[4] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};

  static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

  void block(const uint8_t *b) {
    static const int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                               14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const std::array<uint32_t, 64> kK = [] {
      std::array<uint32_t, 64> k{};
      for (int i = 0; i < 64; ++i) k[i] = static_cast<uint32_t>(std::fabs(std::sin(static_cast<double>(i + 1))) * 4294967296.0);
      return k;
    }();
    uint32_t m[16];
    std::memcpy(m, b, 64);
    uint32_t a = s[0], bb = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; ++i) {
      uint32_t f;
      int g;
      if (i < 16) {
        f = (bb & c) | (~bb & d);
        g = i;
      } else if (i < 32) {
        f = (d & bb) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = bb ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (bb | ~d);
        g = (7 * i) % 16;
      }
      f += a + kK[i] + m[g];
      a = d;
      d = c;
      c = bb;
      bb += rotl(f, kS[i]);
    }
    s[0] += a;
    s[1] += bb;
    s[2] += c;
    s[3] += d;
  }
};

void dxbc_checksum(std::vector<uint8_t> &dxbc) {
  const uint8_t *data = dxbc.data() + 20;
  const size_t size = dxbc.size() - 20;
  md5 h;
  const size_t full = size & ~size_t(63);
  for (size_t o = 0; o < full; o += 64) h.block(data + o);
  const size_t left = size - full;
  const uint32_t bits = static_cast<uint32_t>(size * 8);
  const uint32_t count = (bits >> 2) | 1;
  uint8_t blk[64] = {};
  if (left >= 56) {
    std::memcpy(blk, data + full, left);
    blk[left] = 0x80;
    h.block(blk);
    std::memset(blk, 0, 64);
    std::memcpy(blk, &bits, 4);
    std::memcpy(blk + 60, &count, 4);
  } else {
    std::memcpy(blk, &bits, 4);
    std::memcpy(blk + 4, data + full, left);
    blk[4 + left] = 0x80;
    std::memcpy(blk + 60, &count, 4);
  }
  h.block(blk);
  std::memcpy(dxbc.data() + 4, h.s, 16);
}

// ---------------------------------------------------------------------------------------------
// Programme SM5 (chunk SHEX) : parcours des instructions, retarget d'un opérande cb, ajout d'un dcl.

struct program {
  std::vector<uint8_t> container;
  size_t shex_offset = 0;  // offset du chunk SHEX (en-tête « SHEX » + taille)
  std::vector<uint32_t> tokens;
};

bool is_declaration(uint32_t opcode) {
  // Déclarations : 0x58..0x6A et les déclarations SM5 (0x8F..0x9F, hors instructions de calcul).
  static const std::set<uint32_t> kNonDecl = {107, 108, 110, 111, 113, 114, 115, 117, 118, 119, 120,
                                               121, 122, 123, 124, 126, 127, 134, 135, 136, 137, 140};
  if (opcode < 0x58) return false;
  if (kNonDecl.count(opcode) || (opcode >= 160 && opcode < 188)) return false;
  return true;
}

uint32_t instruction_length(const std::vector<uint32_t> &t, size_t pos) {
  const uint32_t opcode = t[pos] & 0x7FF;
  return opcode == 0x35 ? t[pos + 1] : (t[pos] >> 24) & 0x7F;  // customdata : longueur explicite
}

bool load(const uint8_t *code, size_t size, program &p) {
  if (size < 32 || std::memcmp(code, "DXBC", 4) != 0) return false;
  p.container.assign(code, code + size);
  uint32_t count;
  std::memcpy(&count, code + 28, 4);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t off;
    std::memcpy(&off, code + 32 + 4 * i, 4);
    if (off + 8 > size) return false;
    if (std::memcmp(code + off, "SHEX", 4) == 0 || std::memcmp(code + off, "SHDR", 4) == 0) {
      uint32_t len;
      std::memcpy(&len, code + off + 4, 4);
      if (off + 8 + len > size) return false;
      p.shex_offset = off;
      p.tokens.resize(len / 4);
      std::memcpy(p.tokens.data(), code + off + 8, len);
      return p.tokens.size() > 2;
    }
  }
  return false;
}

// Position (indice de jeton) de l'instruction n, déclarations exclues ; SIZE_MAX si absente.
size_t find_instruction(const program &p, uint32_t n) {
  size_t pos = 2;
  uint32_t index = 0;
  while (pos < p.tokens.size()) {
    const uint32_t len = instruction_length(p.tokens, pos);
    if (len == 0) return SIZE_MAX;
    if (!is_declaration(p.tokens[pos] & 0x7FF)) {
      if (index == n) return pos;
      ++index;
    }
    pos += len;
  }
  return SIZE_MAX;
}

// Dans l'instruction à `pos`, remplace l'opérande cb[old_slot][old_reg] (index immédiats) par
// cb[new_slot][new_reg].
bool retarget_cb(program &p, size_t pos, uint32_t old_slot, uint32_t old_reg, uint32_t new_slot, uint32_t new_reg) {
  const size_t end = pos + instruction_length(p.tokens, pos);
  for (size_t k = pos + 1; k + 2 < end; ++k) {
    const uint32_t op = p.tokens[k];
    const bool cb_2d_immediate = ((op >> 12) & 0xFF) == 8 && ((op >> 20) & 3) == 2 && ((op >> 22) & 0x3F) == 0;
    if (!cb_2d_immediate) continue;
    const size_t idx = k + 1 + ((op & 0x80000000) ? 1 : 0);
    if (idx + 1 < end && p.tokens[idx] == old_slot && p.tokens[idx + 1] == old_reg) {
      p.tokens[idx] = new_slot;
      p.tokens[idx + 1] = new_reg;
      return true;
    }
  }
  return false;
}

// Ajoute « dcl_constantbuffer cb<slot>[registers], immediateIndexed » après le dernier dcl_constantbuffer.
bool add_cb_declaration(program &p, uint32_t slot, uint32_t registers) {
  size_t pos = 2, last = SIZE_MAX;
  while (pos < p.tokens.size()) {
    const uint32_t len = instruction_length(p.tokens, pos);
    if (len == 0) return false;
    if ((p.tokens[pos] & 0x7FF) == 0x59 && len == 4) {
      if (p.tokens[pos + 2] == slot) return false;  // déjà déclaré
      last = pos;
    }
    pos += len;
  }
  if (last == SIZE_MAX) return false;
  uint32_t decl[4] = {p.tokens[last] & ~(1u << 11), p.tokens[last + 1], slot, registers};  // accès immédiat
  p.tokens.insert(p.tokens.begin() + static_cast<ptrdiff_t>(last + 4), decl, decl + 4);
  p.tokens[1] = static_cast<uint32_t>(p.tokens.size());
  return true;
}

void rebuild(program &p, std::vector<uint8_t> &out) {
  uint32_t old_len;
  std::memcpy(&old_len, p.container.data() + p.shex_offset + 4, 4);
  const uint32_t new_len = static_cast<uint32_t>(p.tokens.size() * 4);
  const size_t tail = p.shex_offset + 8 + old_len;
  out.assign(p.container.begin(), p.container.begin() + static_cast<ptrdiff_t>(p.shex_offset + 4));
  out.insert(out.end(), reinterpret_cast<const uint8_t *>(&new_len), reinterpret_cast<const uint8_t *>(&new_len) + 4);
  out.insert(out.end(), reinterpret_cast<const uint8_t *>(p.tokens.data()),
             reinterpret_cast<const uint8_t *>(p.tokens.data()) + new_len);
  out.insert(out.end(), p.container.begin() + static_cast<ptrdiff_t>(tail), p.container.end());
  const int32_t delta = static_cast<int32_t>(new_len) - static_cast<int32_t>(old_len);
  uint32_t count;
  std::memcpy(&count, out.data() + 28, 4);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t off;
    std::memcpy(&off, out.data() + 32 + 4 * i, 4);
    if (off > p.shex_offset) {
      off += delta;
      std::memcpy(out.data() + 32 + 4 * i, &off, 4);
    }
  }
  const uint32_t total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 24, &total, 4);
  dxbc_checksum(out);
}

// ---------------------------------------------------------------------------------------------
// Table des patchs.

struct cb_retarget {
  uint32_t instruction;          // indice d'instruction (déclarations exclues)
  uint32_t old_slot, old_reg;    // opérande d'origine
};

struct patch_entry {
  uint32_t hash;
  const char *name;
  std::vector<cb_retarget> retargets;  // chaque opérande lit kJitterCbSlot[0] à la place
};

// 0xA124DE12 : éclairage différé par tuiles. Instruction 77 « add r7.xy, r6.zw, -cb0[12].zw » : passage
// pixel -> NDC pour la reconstruction de la position vue (avant « div r7.x, r0.x, cb0[6].x » …) ; cb0[12].zw
// (origine de la zone d'affichage, nulle) devient le jitter en pixels. Les adresses de texels (instructions
// 20 et 23) gardent cb0[12].zw.
const std::vector<patch_entry> &table() {
  static const std::vector<patch_entry> kTable = {
      {0xA124DE12, "éclairage différé (reconstruction de la position)", {{77, 0, 12}}},
  };
  return kTable;
}

std::mutex g_mutex;
std::set<uint32_t> g_patched;

}  // namespace

bool patch(uint32_t hash, const uint8_t *code, size_t size, std::vector<uint8_t> &out) {
  const patch_entry *entry = nullptr;
  for (const auto &e : table())
    if (e.hash == hash) entry = &e;
  if (entry == nullptr) return false;

  char buf[192];
  program p;
  bool ok = load(code, size, p);
  for (const auto &r : entry->retargets) {
    if (!ok) break;
    const size_t pos = find_instruction(p, r.instruction);
    ok = pos != SIZE_MAX && retarget_cb(p, pos, r.old_slot, r.old_reg, kJitterCbSlot, 0);
  }
  ok = ok && add_cb_declaration(p, kJitterCbSlot, 1);
  if (!ok) {
    std::snprintf(buf, sizeof(buf), "AK-FSR: patch 0x%08X (%s) : forme inattendue, shader laissé intact", hash,
                  entry->name);
    reshade::log::message(reshade::log::level::warning, buf);
    return false;
  }
  rebuild(p, out);
  {
    std::lock_guard lock(g_mutex);
    g_patched.insert(hash);
  }
  std::snprintf(buf, sizeof(buf), "AK-FSR: shader 0x%08X patché (%s), %zu -> %zu octets", hash, entry->name, size,
                out.size());
  reshade::log::message(reshade::log::level::info, buf);
  return true;
}

bool needs_jitter_cb(uint32_t hash) {
  std::lock_guard lock(g_mutex);
  return g_patched.count(hash) != 0;
}

}  // namespace akfsr::shader_patch
