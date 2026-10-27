#include "screenshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "crc32.hpp"

using namespace reshade::api;

namespace akfsr::screenshot {
namespace {

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
}

void chunk(std::vector<uint8_t> &png, const char *type, const std::vector<uint8_t> &data) {
  put_u32(png, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  png.insert(png.end(), body.begin(), body.end());
  put_u32(png, crc32(body.data(), body.size()));
}

// zlib sans compression (blocs « stored ») : simple et suffisant pour quelques Mo.
std::vector<uint8_t> zlib_stored(const std::vector<uint8_t> &raw) {
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put_u32(z, (b << 16) | a);
  return z;
}

float half_to_float(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
  float v = exp == 0 ? std::ldexp(static_cast<float>(mant), -24)
          : exp == 31 ? 65504.0f
                      : std::ldexp(static_cast<float>(mant | 0x400), static_cast<int>(exp) - 25);
  return sign ? -v : v;
}

uint8_t to8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

}  // namespace

std::string capture(effect_runtime *runtime, const std::filesystem::path &path) {
  const resource_desc desc = runtime->get_device()->get_resource_desc(runtime->get_current_back_buffer());
  const uint32_t w = desc.texture.width, h = desc.texture.height;
  const format f = format_to_default_typed(desc.texture.format, 0);
  const uint32_t bpp = f == format::r16g16b16a16_float ? 8 : 4;
  if (f != format::r8g8b8a8_unorm && f != format::b8g8r8a8_unorm && f != format::r10g10b10a2_unorm &&
      f != format::r16g16b16a16_float)
    return "format du back buffer non géré (" + std::to_string(static_cast<uint32_t>(desc.texture.format)) + ")";
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * bpp);
  if (!runtime->capture_screenshot(pixels.data())) return "capture_screenshot en échec";

  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(w * 3 + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);  // filtre PNG « aucun »
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t *p = pixels.data() + (static_cast<size_t>(y) * w + x) * bpp;
      uint8_t r, g, b;
      if (f == format::r10g10b10a2_unorm) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        r = static_cast<uint8_t>((v & 0x3FF) >> 2);
        g = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
        b = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
      } else if (f == format::r16g16b16a16_float) {
        uint16_t c[3];
        std::memcpy(c, p, 6);
        r = to8(half_to_float(c[0]));
        g = to8(half_to_float(c[1]));
        b = to8(half_to_float(c[2]));
      } else if (f == format::b8g8r8a8_unorm) {
        r = p[2], g = p[1], b = p[0];
      } else {
        r = p[0], g = p[1], b = p[2];
      }
      raw.insert(raw.end(), {r, g, b});
    }
  }
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  put_u32(ihdr, w);
  put_u32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", zlib_stored(raw));
  chunk(png, "IEND", {});

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  FILE *file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return "écriture de " + path.string() + " impossible";
  std::fwrite(png.data(), 1, png.size(), file);
  std::fclose(file);
  char buf[64];
  std::snprintf(buf, sizeof(buf), " (%ux%u)", w, h);
  return "écrite : " + path.string() + buf;
}

}  // namespace akfsr::screenshot
