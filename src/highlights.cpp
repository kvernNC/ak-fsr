#include "highlights.hpp"

#include <reshade.hpp>

#include <cstring>
#include <mutex>

#include "d3d_compute.hpp"

namespace akfsr::highlights {
namespace {

constexpr const char *kSource = R"(
Texture2D<float4> src : register(t0);
Texture2D<float4> excess_in : register(t1);
RWTexture2D<float4> dst : register(u0);
RWTexture2D<float4> excess_out : register(u1);
cbuffer constants : register(b0) { float threshold; uint3 size; };

[numthreads(8, 8, 1)] void extract(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size.xy)) return;
  const float3 c = src[p].rgb;
  const float m = max(c.r, max(c.g, c.b));
  const float3 kept = m > threshold ? c * (threshold / m) : c;
  dst[p] = float4(kept, 1.0);
  excess_out[p] = float4(c - kept, 0.0);
}

[numthreads(8, 8, 1)] void reinject(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size.xy)) return;
  dst[p] = float4(src[p].rgb + excess_in[p].rgb, 1.0);
}
)";

struct state {
  bool tried = false;
  ID3D11ComputeShader *extract = nullptr, *reinject = nullptr;
  ID3D11Buffer *cb = nullptr;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT width = 0, height = 0;
  ID3D11Texture2D *scratch = nullptr, *excess = nullptr;  // même format que la couleur / RGBA16F
  ID3D11UnorderedAccessView *scratch_uav = nullptr, *excess_uav = nullptr;
  ID3D11ShaderResourceView *scratch_srv = nullptr, *excess_srv = nullptr;
  bool pending = false;  // part mise de côté à rajouter
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
                        ("AK-FSR: hautes lumières : " + s).c_str());
}

template <typename T>
void release(T *&p) {
  if (p != nullptr) p->Release();
  p = nullptr;
}

void release_textures() {
  release(g.scratch_uav);
  release(g.scratch_srv);
  release(g.excess_uav);
  release(g.excess_srv);
  release(g.scratch);
  release(g.excess);
  g.format = DXGI_FORMAT_UNKNOWN;
}

bool ensure(ID3D11Device *dev, ID3D11Resource *color) {
  if (!g.tried) {
    g.tried = true;
    std::string error;
    g.extract = d3d::compile_cs(dev, kSource, "extract", error);
    if (g.extract != nullptr) g.reinject = d3d::compile_cs(dev, kSource, "reinject", error);
    D3D11_BUFFER_DESC cb{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (g.reinject == nullptr || FAILED(dev->CreateBuffer(&cb, nullptr, &g.cb))) {
      set_status("compilation des shaders : " + error, true);
      return false;
    }
  }
  if (g.cb == nullptr) return false;
  ID3D11Texture2D *tex = nullptr;
  if (FAILED(color->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex)))) return false;
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  tex->Release();
  if (g.scratch != nullptr && g.format == desc.Format && g.width == desc.Width && g.height == desc.Height) return true;
  release_textures();

  UINT support = 0;
  if (FAILED(dev->CheckFormatSupport(desc.Format, &support)) ||
      (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) == 0) {
    set_status("format de couleur " + std::to_string(desc.Format) + " sans UAV typée", true);
    return false;
  }
  D3D11_TEXTURE2D_DESC t{};
  t.Width = desc.Width;
  t.Height = desc.Height;
  t.MipLevels = t.ArraySize = 1;
  t.Format = desc.Format;
  t.SampleDesc.Count = 1;
  t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  bool ok = SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &g.scratch));
  t.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &g.excess));
  if (ok) {
    g.scratch_uav = d3d::create_uav(dev, g.scratch);
    g.scratch_srv = d3d::create_srv(dev, g.scratch);
    g.excess_uav = d3d::create_uav(dev, g.excess);
    g.excess_srv = d3d::create_srv(dev, g.excess);
  }
  if (!ok || !g.scratch_uav || !g.scratch_srv || !g.excess_uav || !g.excess_srv) {
    set_status("création des textures intermédiaires en échec", true);
    release_textures();
    return false;
  }
  g.format = desc.Format;
  g.width = desc.Width;
  g.height = desc.Height;
  return true;
}

bool update_constants(ID3D11DeviceContext *ctx, float threshold) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  auto *c = static_cast<uint32_t *>(mapped.pData);
  std::memcpy(&c[0], &threshold, 4);
  c[1] = g.width;
  c[2] = g.height;
  c[3] = 0;
  ctx->Unmap(g.cb, 0);
  return true;
}

// Exécute `cs` : t0 = src, t1 = part mise de côté, u0 = scratch, u1 = part (extraction seulement).
void run(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs, ID3D11ShaderResourceView *src, bool extracting) {
  d3d::cs_state_guard guard(ctx);
  ID3D11ShaderResourceView *srv[4] = {src, extracting ? nullptr : g.excess_srv, nullptr, nullptr};
  ID3D11UnorderedAccessView *uav[2] = {g.scratch_uav, extracting ? g.excess_uav : nullptr};
  // Délie d'abord les UAV du jeu (u0/u1) pour éviter tout conflit SRV/UAV sur la même ressource.
  ID3D11UnorderedAccessView *none[2] = {};
  ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
  ctx->CSSetShader(cs, nullptr, 0);
  ctx->CSSetShaderResources(0, 4, srv);
  ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
  ctx->CSSetConstantBuffers(0, 1, &g.cb);
  ctx->Dispatch((g.width + 7) / 8, (g.height + 7) / 8, 1);
  ID3D11ShaderResourceView *no_srv[4] = {};
  ctx->CSSetShaderResources(0, 4, no_srv);
  ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
}

}  // namespace

bool extract(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *color, float threshold) {
  g.pending = false;
  if (!ensure(dev, color) || !update_constants(ctx, threshold)) return false;
  ID3D11ShaderResourceView *src = d3d::create_srv(dev, color);
  if (src == nullptr) {
    set_status("SRV sur la couleur du jeu impossible", true);
    return false;
  }
  run(ctx, g.extract, src, true);
  src->Release();
  ctx->CopySubresourceRegion(color, 0, 0, 0, 0, g.scratch, 0, nullptr);
  g.pending = true;
  set_status("actif", false);
  return true;
}

void reinject(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *color) {
  if (!g.pending) return;
  g.pending = false;
  ID3D11ShaderResourceView *src = d3d::create_srv(dev, color);
  if (src == nullptr) return;
  run(ctx, g.reinject, src, false);
  src->Release();
  ctx->CopySubresourceRegion(color, 0, 0, 0, 0, g.scratch, 0, nullptr);
}

void shutdown() {
  release_textures();
  release(g.extract);
  release(g.reinject);
  release(g.cb);
  g = {};
}

std::string status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

}  // namespace akfsr::highlights
