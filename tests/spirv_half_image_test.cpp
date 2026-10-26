// Test natif (Linux) de la réécriture SPIR-V de M10.2, sur un module construit à la main :
//   g++ -std=c++20 -Isrc tests/spirv_half_image_test.cpp src/spirv_half_image.cpp -o /tmp/t && /tmp/t
#include "spirv_half_image.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace akfsr::spirv;
std::vector<uint32_t> m;
void I(uint32_t op, std::vector<uint32_t> a){ m.push_back(((a.size()+1)<<16)|op); m.insert(m.end(),a.begin(),a.end()); }
std::vector<uint32_t> str(const char*s){ std::vector<uint32_t> v((strlen(s)+4)/4,0); memcpy(v.data(),s,strlen(s)); return v;}
int main(){
  m={0x07230203,0x10000,0,100,0};
  I(17,{1}); I(17,{9}); I(17,{5008});
  auto e=str("SPV_AMD_gpu_shader_half_float_fetch"); I(10,e);
  I(14,{0,1});
  // ids: 1 void 2 fnty 3 f16 4 v4h 5 img 6 ptr 7 var 8 int 9 v2i 10 c0 11 coord 12 fn 13 label 14 ld 15 rd 16 f32
  I(19,{1}); I(33,{2,1}); I(22,{16,32}); I(22,{3,16}); I(23,{4,3,4});
  I(25,{5,3,1,0,0,0,2,2}); // image f16 2D storage Rgba16f? format 2
  I(32,{6,0,5}); I(59,{6,7,0}); I(21,{8,32,1}); I(23,{9,8,2}); I(43,{8,10,0}); I(44,{9,11,10,10});
  I(54,{1,12,0,2}); I(248,{13}); I(61,{5,14,7}); I(98,{4,15,14,11}); I(99,{14,11,15}); I(253,{}); I(56,{});
  m[3]=17;
  std::vector<uint32_t> out; std::string err;
  auto r=promote_half_images(m.data(),m.size(),out,err);
  int failures = 0;
  auto expect = [&](bool ok, const char *what) { if (!ok) { printf("ÉCHEC : %s\n", what); ++failures; } };
  expect(r == result::rewritten, "module réécrit");
  bool cap = false, ext = false, img32 = false, conv = false; int converts = 0;
  for (size_t i = 5; i < out.size();) {
    const uint32_t c = out[i] >> 16, o = out[i] & 0xffff;
    if (o == 17 && out[i + 1] == 5008) cap = true;
    if (o == 10) ext = true;
    if (o == 25 && out[i + 2] == 16) img32 = true;
    if (o == 115) ++converts;
    i += c;
  }
  (void)conv;
  expect(!cap && !ext, "capacité et extension AMD retirées");
  expect(img32, "image au type échantillonné float32");
  expect(converts == 2, "une conversion pour la lecture, une pour l'écriture");
  std::vector<uint32_t> again;
  expect(promote_half_images(out.data(), out.size(), again, err) == result::unchanged, "second passage sans effet");
  printf(failures == 0 ? "OK\n" : "%d échec(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
