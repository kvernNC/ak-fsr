#pragma once

// M10.2 — shaders du provider FSR 4 du pilote AMD sous RADV : ils déclarent des images au type échantillonné
// float 16 bits (SPV_AMD_gpu_shader_half_float_fetch, capacité Float16ImageAMD), que seul le pilote AMD
// accepte. Réécriture : type échantillonné float 32 bits, et conversion float32 <-> float16 autour de chaque
// lecture (résultat) et écriture (texel) de ces images. Écrit pour AK-FSR (aucun code repris).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace akfsr::spirv {

enum class result { unchanged, rewritten, failed };

// `out` reçoit le module réécrit (result::rewritten) ; `error` explique un échec (module laissé tel quel).
result promote_half_images(const uint32_t *code, size_t words, std::vector<uint32_t> &out, std::string &error);

}  // namespace akfsr::spirv
