#pragma once

// CRC32 (polynôme 0xEDB88320) sur le bytecode complet : mêmes hashes que RenoDX
// (renodx::utils::hash::ComputeCRC32), donc noms de shaders directement comparables.

#include <array>
#include <cstddef>
#include <cstdint>

namespace akfsr {

inline constexpr std::array<uint32_t, 256> kCrc32Table = [] {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    table[i] = c;
  }
  return table;
}();

inline uint32_t crc32(const void *data, size_t size) {
  auto *p = static_cast<const uint8_t *>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (; size != 0; --size, ++p) crc = (crc >> 8) ^ kCrc32Table[(crc ^ *p) & 0xFF];
  return ~crc;
}

}  // namespace akfsr
