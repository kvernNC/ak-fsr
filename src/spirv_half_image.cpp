#include "spirv_half_image.hpp"

#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace akfsr::spirv {
namespace {

// Opcodes utilisés (spécification SPIR-V 1.6).
enum op : uint32_t {
  OpExtension = 10,
  OpCapability = 17,
  OpTypeFloat = 22,
  OpTypeVector = 23,
  OpTypeImage = 25,
  OpTypeSampledImage = 27,
  OpFunction = 54,
  OpImageSampleImplicitLod = 87,
  OpImageSampleProjDrefExplicitLod = 94,
  OpImageFetch = 95,
  OpImageGather = 96,
  OpImageDrefGather = 97,
  OpImageRead = 98,
  OpImageWrite = 99,
  OpFConvert = 115,
  OpImageSparseSampleImplicitLod = 305,
  OpImageSparseRead = 320,
};
constexpr uint32_t kCapabilityFloat16ImageAMD = 5008;
constexpr const char *kHalfFetchExtension = "SPV_AMD_gpu_shader_half_float_fetch";

struct instruction {
  size_t offset;  // en mots
  uint32_t opcode, count;
};

// Opcodes qui ne définissent pas d'identifiant en mot 2 (stockage, décorations, contrôle de flux…) :
// exclus de la recherche du type d'une valeur. Les déclarations de type (19..39) aussi.
bool defines_value_at_word2(uint32_t opcode) {
  if (opcode >= 19 && opcode <= 39) return false;  // OpTypeVoid..OpTypePipeStorage
  switch (opcode) {
    case 0: case 3: case 4: case 5: case 6: case 7: case 8: case 10: case 11: case 14: case 15: case 16: case 17:
    case 56: case 62: case 63: case 64: case 71: case 72: case 73: case 74: case 75: case 99: case 224: case 225:
    case 246: case 247: case 248: case 249: case 250: case 251: case 252: case 253: case 254: case 255: case 256:
    case 317: case 400: case 4416: case 4417:
      return false;
    default:
      return true;
  }
}

std::string literal_string(const uint32_t *words, uint32_t count) {
  const char *s = reinterpret_cast<const char *>(words);
  return std::string(s, strnlen(s, count * 4));
}

}  // namespace

result promote_half_images(const uint32_t *code, size_t words, std::vector<uint32_t> &out, std::string &error) {
  if (words < 5 || code[0] != 0x07230203u) {
    error = "en-tête SPIR-V invalide";
    return result::failed;
  }
  std::vector<instruction> ins;
  for (size_t i = 5; i < words;) {
    const uint32_t count = code[i] >> 16, opcode = code[i] & 0xFFFF;
    if (count == 0 || i + count > words) {
      error = "instruction tronquée";
      return result::failed;
    }
    ins.push_back({i, opcode, count});
    i += count;
  }
  auto w = [&](const instruction &x, uint32_t k) { return code[x.offset + k]; };

  // Types : float16 / float32, vecteurs, images à type échantillonné float16, sampled images associées.
  std::unordered_set<uint32_t> f16;
  uint32_t f32 = 0;
  size_t f32_index = SIZE_MAX, first_half_image = SIZE_MAX, first_function = SIZE_MAX;
  std::unordered_map<uint64_t, uint32_t> vectors;   // (composante << 32 | taille) -> id
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vector_of;  // id -> (composante, taille)
  std::unordered_set<uint32_t> half_images, half_sampled;
  std::unordered_map<uint32_t, uint32_t> value_type;  // id d'une valeur -> id de son type
  for (size_t k = 0; k < ins.size(); ++k) {
    const auto &x = ins[k];
    if (x.opcode == OpTypeFloat && x.count == 3) {
      if (w(x, 2) == 16) f16.insert(w(x, 1));
      if (w(x, 2) == 32 && f32 == 0) {
        f32 = w(x, 1);
        f32_index = k;
      }
    } else if (x.opcode == OpTypeVector && x.count == 4) {
      vectors[(uint64_t(w(x, 2)) << 32) | w(x, 3)] = w(x, 1);
      vector_of[w(x, 1)] = {w(x, 2), w(x, 3)};
    } else if (x.opcode == OpTypeImage && x.count >= 9 && f16.count(w(x, 2))) {
      half_images.insert(w(x, 1));
      if (first_half_image == SIZE_MAX) first_half_image = k;
    } else if (x.opcode == OpTypeSampledImage && x.count == 3 && half_images.count(w(x, 2))) {
      half_sampled.insert(w(x, 1));
    } else if (x.opcode == OpFunction && first_function == SIZE_MAX) {
      first_function = k;
    }
    if (x.count >= 3 && defines_value_at_word2(x.opcode) && !(x.opcode >= 19 && x.opcode <= 39))
      value_type[w(x, 2)] = w(x, 1);
  }
  if (half_images.empty()) return result::unchanged;
  if (first_function == SIZE_MAX) {
    error = "aucune fonction";
    return result::failed;
  }
  if (f32 != 0 && f32_index > first_half_image) {
    error = "float32 déclaré après la première image float16";
    return result::failed;
  }

  uint32_t bound = code[3];
  std::vector<uint32_t> new_types;  // déclarations ajoutées, placées après le type float32
  if (f32 == 0) {
    f32 = bound++;
    new_types.insert(new_types.end(), {(3u << 16) | OpTypeFloat, f32, 32});
  }
  // Équivalent float32 d'un type float16 (scalaire ou vecteur) ; 0 si ce n'est pas un type float16.
  auto promoted = [&](uint32_t type) -> uint32_t {
    if (f16.count(type)) return f32;
    auto v = vector_of.find(type);
    if (v == vector_of.end() || !f16.count(v->second.first)) return 0;
    const uint64_t key = (uint64_t(f32) << 32) | v->second.second;
    auto it = vectors.find(key);
    if (it != vectors.end()) return it->second;
    const uint32_t id = bound++;
    new_types.insert(new_types.end(), {(4u << 16) | OpTypeVector, id, f32, v->second.second});
    vectors[key] = id;
    vector_of[id] = {f32, v->second.second};
    return id;
  };
  auto is_half_image_value = [&](uint32_t id) {
    auto t = value_type.find(id);
    return t != value_type.end() && (half_images.count(t->second) || half_sampled.count(t->second));
  };

  // Corps réécrit (types ajoutés insérés après coup, à la position du type float32).
  std::vector<uint32_t> body;
  body.reserve(words + 256);
  size_t insert_at = SIZE_MAX;  // position dans `body` après la déclaration float32 (ou avant la 1re image)
  uint32_t rewritten_reads = 0, rewritten_writes = 0;
  for (size_t k = 0; k < ins.size(); ++k) {
    const auto &x = ins[k];
    const uint32_t *src = code + x.offset;
    if (x.opcode == OpCapability && w(x, 1) == kCapabilityFloat16ImageAMD) continue;
    if (x.opcode == OpExtension && literal_string(src + 1, x.count - 1) == kHalfFetchExtension) continue;
    if (k == first_half_image && insert_at == SIZE_MAX && f32_index == SIZE_MAX) insert_at = body.size();
    if (x.opcode == OpTypeImage && half_images.count(w(x, 1))) {
      body.insert(body.end(), src, src + x.count);
      body[body.size() - x.count + 2] = f32;
      continue;
    }
    const bool sparse = x.opcode >= OpImageSparseSampleImplicitLod && x.opcode <= OpImageSparseRead;
    const bool read = (x.opcode >= OpImageSampleImplicitLod && x.opcode <= OpImageSampleProjDrefExplicitLod) ||
                      x.opcode == OpImageFetch || x.opcode == OpImageGather || x.opcode == OpImageDrefGather ||
                      x.opcode == OpImageRead;
    if ((read || sparse) && x.count >= 4 && is_half_image_value(w(x, 3))) {
      if (sparse) {
        error = "lecture sparse d'une image float16 (non gérée)";
        return result::failed;
      }
      const uint32_t type32 = promoted(w(x, 1));
      if (type32 == 0) {
        error = "lecture d'image float16 dont le résultat n'est pas float16";
        return result::failed;
      }
      const uint32_t tmp = bound++;
      body.insert(body.end(), src, src + x.count);
      body[body.size() - x.count + 1] = type32;
      body[body.size() - x.count + 2] = tmp;
      body.insert(body.end(), {(4u << 16) | OpFConvert, w(x, 1), w(x, 2), tmp});
      ++rewritten_reads;
      continue;
    }
    if (x.opcode == OpImageWrite && x.count >= 4 && is_half_image_value(w(x, 1))) {
      auto t = value_type.find(w(x, 3));
      const uint32_t type32 = t != value_type.end() ? promoted(t->second) : 0;
      if (type32 == 0) {
        error = "écriture d'image float16 avec un texel de type inconnu";
        return result::failed;
      }
      const uint32_t tmp = bound++;
      body.insert(body.end(), {(4u << 16) | OpFConvert, type32, tmp, w(x, 3)});
      body.insert(body.end(), src, src + x.count);
      body[body.size() - x.count + 3] = tmp;
      ++rewritten_writes;
      continue;
    }
    body.insert(body.end(), src, src + x.count);
    if (k == f32_index) insert_at = body.size();
  }
  if (insert_at == SIZE_MAX) {
    error = "position d'insertion des types introuvable";
    return result::failed;
  }
  out.assign(code, code + 5);
  out[3] = bound;
  out.insert(out.end(), body.begin(), body.begin() + insert_at);
  out.insert(out.end(), new_types.begin(), new_types.end());
  out.insert(out.end(), body.begin() + insert_at, body.end());
  (void)rewritten_reads;
  (void)rewritten_writes;
  return result::rewritten;
}

}  // namespace akfsr::spirv
