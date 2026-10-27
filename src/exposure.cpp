#include "exposure.hpp"

#include <reshade.hpp>

#include <cstring>
#include <mutex>

#include "d3d_compute.hpp"

namespace akfsr::exposure {
namespace {

constexpr const char *kSource = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer constants : register(b0) { uint mode; float scale; uint2 pad; };

[numthreads(1, 1, 1)] void main() {
  float v = src.Load(int3(0, 0, 0)).r;
  if (mode == 1) v = v > 0.0 ? 1.0 / v : 1.0;
  else if (mode == 2) v = exp2(v);
  else if (mode == 3) v = exp2(-v);
  dst[uint2(0, 0)] = clamp(v * scale, 1e-6, 1e6);
}
)";

struct state {
  bool tried = false;
  ID3D11ComputeShader *cs = nullptr;
  ID3D11Buffer *cb = nullptr;
  ID3D11Texture2D *out = nullptr;
  ID3D11UnorderedAccessView *uav = nullptr;
} g;

std::mutex g_status_mutex;
std::string g_status = "inactif";

void set_status(const std::string &s, bool failure) {
  {
    std::lock_guard lock(g_status_mutex);
    if (g_status == s) return;
    g_status = s;
  }
  reshade::log::message(failure ? reshade::log::level::error : reshade::log::level::info,
                        ("AK-FSR: exposition du jeu : " + s).c_str());
}

template <typename T>
void release(T *&p) {
  if (p != nullptr) p->Release();
  p = nullptr;
}

bool ensure(ID3D11Device *dev) {
  if (g.tried) return g.uav != nullptr;
  g.tried = true;
  std::string error;
  g.cs = d3d::compile_cs(dev, kSource, "main", error);
  D3D11_BUFFER_DESC cb{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  D3D11_TEXTURE2D_DESC t{};
  t.Width = t.Height = t.MipLevels = t.ArraySize = 1;
  t.Format = DXGI_FORMAT_R32_FLOAT;
  t.SampleDesc.Count = 1;
  t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (g.cs == nullptr || FAILED(dev->CreateBuffer(&cb, nullptr, &g.cb)) ||
      FAILED(dev->CreateTexture2D(&t, nullptr, &g.out)) || (g.uav = d3d::create_uav(dev, g.out)) == nullptr) {
    set_status("initialisation en échec " + error, true);
    return false;
  }
  return true;
}

}  // namespace

ID3D11Texture2D *prepare(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *game, mode m, float scale) {
  if (game == nullptr) {
    set_status("texture 1×1 du tonemapper pas encore repérée", false);
    return nullptr;
  }
  if (!ensure(dev)) return nullptr;
  ID3D11ShaderResourceView *src = d3d::create_srv(dev, game);
  if (src == nullptr) {
    set_status("SRV sur la texture d'exposition du jeu impossible", true);
    return nullptr;
  }
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    const uint32_t values[4] = {static_cast<uint32_t>(m), 0, 0, 0};
    std::memcpy(mapped.pData, values, sizeof(values));
    std::memcpy(static_cast<uint8_t *>(mapped.pData) + 4, &scale, 4);
    ctx->Unmap(g.cb, 0);
  }
  {
    d3d::cs_state_guard guard(ctx);
    ID3D11UnorderedAccessView *none[2] = {};
    ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
    ctx->CSSetShader(g.cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, &g.uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g.cb);
    ctx->Dispatch(1, 1, 1);
    ID3D11ShaderResourceView *no_srv = nullptr;
    ctx->CSSetShaderResources(0, 1, &no_srv);
    ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
  }
  src->Release();
  set_status("actif", false);
  return g.out;
}

void shutdown() {
  release(g.uav);
  release(g.out);
  release(g.cb);
  release(g.cs);
  g = {};
}

std::string status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

}  // namespace akfsr::exposure
