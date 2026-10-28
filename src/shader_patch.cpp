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

// Projections rendues sensibles au jitter. La profondeur est rendue avec la projection jitterée (NDC décalé de
// j) mais le shader passe pixel <-> vue avec la seule échelle cb<slot>[reg_x].x / cb<slot>[reg_y].y :
//   « div d.c, ndc.c', échelle » (pixel -> vue) : le NDC lu est d'abord diminué du jitter (nouveau registre) ;
//   « mul d.c, vue.c', échelle » (vue -> pixel) : le jitter est ajouté au résultat.
// Le jitter NDC est lu en kJitterCbSlot[0].xy. Retourne le nombre d'instructions corrigées (0 : forme inattendue).
int jitter_projections(program &p, uint32_t slot, uint32_t reg_x, uint32_t reg_y) {
  constexpr uint32_t kOpAdd = 0x00, kOpDiv = 0x0E, kOpMul = 0x38, kOpDclTemps = 0x68;
  // Registre temporaire supplémentaire.
  size_t temps_pos = SIZE_MAX;
  for (size_t pos = 2; pos < p.tokens.size();) {
    const uint32_t len = instruction_length(p.tokens, pos);
    if (len == 0) return 0;
    if ((p.tokens[pos] & 0x7FF) == kOpDclTemps) temps_pos = pos;
    pos += len;
  }
  if (temps_pos == SIZE_MAX) return 0;
  const uint32_t temp = p.tokens[temps_pos + 1]++;

  auto temp_source = [](uint32_t op) {  // rN.c, sans modificateur
    return ((op >> 12) & 0xFF) == 0 && ((op >> 20) & 3) == 1 && ((op >> 2) & 3) == 2 && !(op & 0x80000000);
  };
  auto jitter_operand = [](uint32_t component, bool negate, std::vector<uint32_t> &out) {
    out.push_back(0x0020800Au | (component << 4) | (negate ? 0x80000000u : 0));  // cb[slot][reg].c
    if (negate) out.push_back(0x41);                                             // modificateur : négation
    out.push_back(kJitterCbSlot);
    out.push_back(0);
  };
  int count = 0;
  for (size_t pos = 2; pos < p.tokens.size();) {
    const uint32_t len = instruction_length(p.tokens, pos);
    const uint32_t opcode = p.tokens[pos] & 0x7FF;
    if (len == 8 && (opcode == kOpDiv || opcode == kOpMul) && !(p.tokens[pos] & 0x80000000)) {
      const uint32_t dst = p.tokens[pos + 1], src = p.tokens[pos + 3], cb = p.tokens[pos + 5];
      const bool cb_scalar = ((cb >> 12) & 0xFF) == 8 && ((cb >> 20) & 3) == 2 && ((cb >> 2) & 3) == 2 &&
                             !(cb & 0x80000000) && p.tokens[pos + 6] == slot;
      const uint32_t reg = p.tokens[pos + 7], comp = (cb >> 4) & 3;
      const bool scale = cb_scalar && ((reg == reg_x && comp == 0) || (reg == reg_y && comp == 1));
      const bool dst_single = ((dst >> 12) & 0xFF) == 0 && ((dst >> 2) & 3) == 0 && !(dst & 0x80000000);
      if (scale && dst_single && temp_source(src)) {
        std::vector<uint32_t> add;
        if (opcode == kOpDiv) {
          // add rT.c', src, -jitter ; la division lit rT.c'.
          const uint32_t c = (src >> 4) & 3;
          add = {0, 0x00100002u | (1u << (4 + c)), temp, src, p.tokens[pos + 4]};
          jitter_operand(comp, true, add);
          add[0] = kOpAdd | (static_cast<uint32_t>(add.size()) << 24);
          p.tokens[pos + 4] = temp;
          p.tokens.insert(p.tokens.begin() + static_cast<ptrdiff_t>(pos), add.begin(), add.end());
        } else {
          // add d.c, d.c, jitter, après la multiplication.
          uint32_t c = 0;
          while (c < 4 && !((dst >> (4 + c)) & 1)) ++c;
          if (c == 4 || ((dst >> 4) & 0xF) != (1u << c)) return 0;
          add = {0, dst, p.tokens[pos + 2], 0x0010000Au | (c << 4), p.tokens[pos + 2]};
          jitter_operand(comp, false, add);
          add[0] = kOpAdd | (static_cast<uint32_t>(add.size()) << 24);
          p.tokens.insert(p.tokens.begin() + static_cast<ptrdiff_t>(pos + len), add.begin(), add.end());
        }
        ++count;
        pos += len + add.size();
        continue;
      }
    }
    if (len == 0) return 0;
    pos += len;
  }
  p.tokens[1] = static_cast<uint32_t>(p.tokens.size());
  return count;
}

// Pixel shaders qui reconstruisent la position d'un pixel depuis la profondeur (décals, volumes de lumière,
// masques) : « div rX, vN, vN.wwww » donne le NDC jitteré du pixel, puis une seule instruction
// « mul rD, rX, cb<s>[k] » le multiplie par l'échelle de dé-projection. Le jitter est retiré juste avant
// (« add rT, rX, -cb12[0] », même swizzle que l'échelle : x <- jitter x, y <- jitter y) ; sans cela, les
// coordonnées de texture des décals glissent d'une frame à l'autre. La multiplication du NDC par la
// résolution (adresse du texel de profondeur, suivie de « ftou ») n'est pas touchée.
bool jitter_unprojection_ps(program &p) {
  constexpr uint32_t kOpAdd = 0x00, kOpDiv = 0x0E, kOpMul = 0x38, kOpFtou = 0x1C, kOpDclTemps = 0x68;
  auto is_v_w = [](uint32_t op) {  // vN.wwww
    return ((op >> 12) & 0xFF) == 1 && ((op >> 2) & 3) == 1 && ((op >> 4) & 0xFF) == 0xFF && !(op & 0x80000000);
  };
  uint32_t ndc_reg = UINT32_MAX, ndc_mask = 0;  // NDC suivi depuis le dernier « div rX, vN, vN.wwww »
  size_t target = SIZE_MAX, temps_pos = SIZE_MAX;
  int matches = 0;
  for (size_t pos = 2; pos < p.tokens.size();) {
    const uint32_t len = instruction_length(p.tokens, pos);
    if (len == 0) return false;
    const uint32_t opcode = p.tokens[pos] & 0x7FF;
    const uint32_t dst = len > 2 ? p.tokens[pos + 1] : 0;
    const bool writes_temp = opcode < 0x58 && len > 2 && ((dst >> 12) & 0xFF) == 0 && ((dst >> 20) & 3) == 1;
    if (opcode == kOpDclTemps) temps_pos = pos;
    if (opcode == kOpDiv && len == 7 && ((p.tokens[pos + 3] >> 12) & 0xFF) == 1 && is_v_w(p.tokens[pos + 5]) &&
        writes_temp) {  // le numérateur est l'entrée interpolée elle-même (pas déjà multipliée par la profondeur)
      ndc_reg = p.tokens[pos + 2];
      ndc_mask = (dst >> 4) & 0xF;
    } else if (ndc_reg != UINT32_MAX) {
      if (opcode == kOpMul && len == 8 && !(p.tokens[pos] & 0x80000000)) {
        const uint32_t src = p.tokens[pos + 3], cb = p.tokens[pos + 5];
        uint32_t read = 0;  // composantes du NDC lues
        for (uint32_t c = 0; c < 4; ++c)
          if ((dst >> (4 + c)) & 1) read |= 1u << ((src >> (4 + 2 * c)) & 3);
        const size_t next = pos + len;
        const bool to_texel = next < p.tokens.size() && (p.tokens[next] & 0x7FF) == kOpFtou;
        if (writes_temp && ((src >> 12) & 0xFF) == 0 && ((src >> 2) & 3) == 1 && !(src & 0x80000000) &&
            p.tokens[pos + 4] == ndc_reg && (read & ~ndc_mask) == 0 && ((cb >> 12) & 0xFF) == 8 &&
            ((cb >> 20) & 3) == 2 && ((cb >> 2) & 3) == 1 && !(cb & 0x80000000)) {
          if (!to_texel) {
            target = pos;
            ++matches;
          }
          ndc_reg = UINT32_MAX;  // seule la première multiplication du NDC compte
        }
      }
      if (ndc_reg != UINT32_MAX && writes_temp && p.tokens[pos + 2] == ndc_reg && (((dst >> 4) & 0xF) & ndc_mask))
        ndc_reg = UINT32_MAX;  // NDC écrasé
    }
    pos += len;
  }
  if (matches != 1 || temps_pos == SIZE_MAX) return false;
  const uint32_t temp = p.tokens[temps_pos + 1]++;
  const uint32_t dst = p.tokens[target + 1], src = p.tokens[target + 3], cb = p.tokens[target + 5];
  const uint32_t mask = (dst >> 4) & 0xF;
  const uint32_t add[] = {kOpAdd | (9u << 24),
                          0x00100002u | (mask << 4), temp,                   // rT.<masque>
                          src, p.tokens[target + 4],                         // NDC (swizzle d'origine)
                          cb | 0x80000000u,                                  // -cb12[0].<swizzle de l'échelle>
                          0x41, kJitterCbSlot, 0};
  // la multiplication lit rT avec le même masque (swizzle identité)
  p.tokens[target + 3] = (src & ~(0xFFu << 4)) | (0xE4u << 4);
  p.tokens[target + 4] = temp;
  p.tokens.insert(p.tokens.begin() + static_cast<ptrdiff_t>(target), std::begin(add), std::end(add));
  p.tokens[1] = static_cast<uint32_t>(p.tokens.size());
  return true;
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
  int projections = 0;                  // nombre attendu de projections cb0[6].x / cb0[7].y à corriger
};

// 0xA124DE12 : éclairage différé par tuiles. Instruction 77 « add r7.xy, r6.zw, -cb0[12].zw » : passage
// pixel -> NDC pour la reconstruction de la position vue (avant « div r7.x, r0.x, cb0[6].x » …) ; cb0[12].zw
// (origine de la zone d'affichage, nulle) devient le jitter en pixels. Les adresses de texels (instructions
// 20 et 23) gardent cb0[12].zw.
//
// 0x0E7C20A1 : occlusion ambiante (SSAO). Chaque échantillon fait l'aller-retour pixel -> vue -> pixel avec
// la seule échelle de projection : sans le jitter, les échantillons tombent à côté d'une quantité qui dépend
// de la phase et de la distance, et des zones de mur s'éclaircissent une frame sur 8 (scintillement).
//
// 0xFE8D7315, 0x816CB5D7 : composition de l'éclairage (deux variantes). La direction de vue de chaque pixel
// est reconstruite par la même dé-projection : sans le jitter, le haut des murs s'éclaircit sur certaines phases.
//
// 0xECE88095, 0x707DD08A : filtrage bilatéral de l'éclairage (deux passes) ; la position des voisins est
// reconstruite par dé-projection (6 sites chacun).
const std::vector<patch_entry> &table() {
  static const std::vector<patch_entry> kTable = {
      {0xA124DE12, "éclairage différé (reconstruction de la position)", {{77, 0, 12}}},
      {0x0E7C20A1, "occlusion ambiante (projections des échantillons)", {}, 146},
      {0xFE8D7315, "composition de l'éclairage (direction de vue)", {}, 2},
      {0x816CB5D7, "composition de l'éclairage, variante (direction de vue)", {}, 2},
      {0xECE88095, "filtrage de l'éclairage, passe 1 (positions des voisins)", {}, 6},
      {0x707DD08A, "filtrage de l'éclairage, passe 2 (positions des voisins)", {}, 6},
  };
  return kTable;
}

// Compute shaders où cb0[6].x / cb0[7].y n'est pas l'échelle de projection de la vue (volumes, grilles) :
// la règle générale les trouverait à tort.
const std::set<uint32_t> kCsExcluded = {0x354E19A3, 0x207E66FA, 0x1660CF5B, 0x5E64DE42, 0xC2A67DDB,
                                        0xF8676992, 0x33566787, 0xA8253F51, 0x8411F20C};

std::mutex g_mutex;
std::set<uint32_t> g_patched;
uint32_t g_ps_patched = 0;  // pixel shaders patchés par la règle générale

}  // namespace

bool patch(uint32_t hash, const uint8_t *code, size_t size, std::vector<uint8_t> &out) {
  const patch_entry *entry = nullptr;
  for (const auto &e : table())
    if (e.hash == hash) entry = &e;
  char buf[192];
  program p;
  if (entry == nullptr) {
    // Règles générales : pixel shaders (type 0), dé-projection unique depuis vN / vN.w ; compute shaders
    // (type 5), projections par l'échelle cb0[6].x / cb0[7].y.
    if (!load(code, size, p)) return false;
    const uint32_t type = p.tokens[0] >> 16;
    int sites = 0;
    if (type == 0)
      sites = jitter_unprojection_ps(p) ? 1 : 0;
    else if (type == 5 && !kCsExcluded.count(hash))
      sites = jitter_projections(p, 0, 6, 7);
    if (sites == 0 || !add_cb_declaration(p, kJitterCbSlot, 1)) return false;
    if (type == 5) {
      std::snprintf(buf, sizeof(buf), "AK-FSR: compute shader 0x%08X : %d projection(s) corrigée(s)", hash, sites);
      reshade::log::message(reshade::log::level::info, buf);
    }
    rebuild(p, out);
    std::lock_guard lock(g_mutex);
    g_patched.insert(hash);
    if (++g_ps_patched % 250 == 1) {
      std::snprintf(buf, sizeof(buf), "AK-FSR: %u pixel shader(s) à dé-projection corrigée (dernier 0x%08X)",
                    g_ps_patched, hash);
      reshade::log::message(reshade::log::level::info, buf);
    }
    return true;
  }
  bool ok = load(code, size, p);
  for (const auto &r : entry->retargets) {
    if (!ok) break;
    const size_t pos = find_instruction(p, r.instruction);
    ok = pos != SIZE_MAX && retarget_cb(p, pos, r.old_slot, r.old_reg, kJitterCbSlot, 0);
  }
  if (ok && entry->projections > 0) ok = jitter_projections(p, 0, 6, 7) == entry->projections;
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
