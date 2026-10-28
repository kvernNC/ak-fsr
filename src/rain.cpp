#include "rain.hpp"

#include <reshade.hpp>

#include <mutex>

#include "d3d_compute.hpp"

namespace akfsr::rain {
namespace {

// Éclaboussures au sol (capture du 28/10, état de mélange src_alpha / inv_src_alpha, masque RGB).
constexpr uint32_t kRainShaders[] = {0x1C362EF9, 0x5DF220CF};

constexpr const char *kSource = R"(
Texture2D<float4> color : register(t0);
Texture2D<float4> layer : register(t1);
RWTexture2D<float4> dst : register(u0);

[numthreads(8, 8, 1)] void main(uint2 p : SV_DispatchThreadID) {
  uint w, h;
  color.GetDimensions(w, h);
  if (any(p >= uint2(w, h))) return;
  const float4 l = layer[p];
  dst[p] = float4(color[p].rgb * l.a + l.rgb, 1.0);
}
)";

struct state {
  bool tried = false;
  ID3D11ComputeShader *cs = nullptr;
  ID3D11BlendState *blend = nullptr;
  UINT width = 0, height = 0;
  DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D *layer = nullptr, *scratch = nullptr;
  ID3D11RenderTargetView *layer_rtv = nullptr;
  ID3D11ShaderResourceView *layer_srv = nullptr;
  ID3D11UnorderedAccessView *scratch_uav = nullptr;
  bool drawn = false;  // la couche a reçu des draws depuis le dernier vidage
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
                        ("AK-FSR: pluie après FSR : " + s).c_str());
}

template <typename T>
void release(T *&p) {
  if (p != nullptr) p->Release();
  p = nullptr;
}

void release_layer() {
  release(g.layer_rtv);
  release(g.layer_srv);
  release(g.layer);
  g.width = g.height = 0;
}

void release_scratch() {
  release(g.scratch_uav);
  release(g.scratch);
  g.color_format = DXGI_FORMAT_UNKNOWN;
}

bool ensure_pipeline(ID3D11Device *dev) {
  if (!g.tried) {
    g.tried = true;
    std::string error;
    g.cs = d3d::compile_cs(dev, kSource, "main", error);
    D3D11_BLEND_DESC b{};
    auto &rt = b.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;  // couleur : comme le jeu
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;  // alpha : transmission, T *= 1 - a
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (g.cs == nullptr || FAILED(dev->CreateBlendState(&b, &g.blend))) {
      set_status("initialisation en échec : " + error, true);
      return false;
    }
  }
  return g.cs != nullptr && g.blend != nullptr;
}

bool ensure_layer(ID3D11Device *dev, UINT width, UINT height) {
  if (g.layer != nullptr && g.width == width && g.height == height) return true;
  release_layer();
  D3D11_TEXTURE2D_DESC t{};
  t.Width = width;
  t.Height = height;
  t.MipLevels = t.ArraySize = 1;
  t.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  t.SampleDesc.Count = 1;
  t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev->CreateTexture2D(&t, nullptr, &g.layer)) ||
      FAILED(dev->CreateRenderTargetView(g.layer, nullptr, &g.layer_rtv)) ||
      (g.layer_srv = d3d::create_srv(dev, g.layer)) == nullptr) {
    set_status("création de la couche en échec", true);
    release_layer();
    return false;
  }
  g.width = width;
  g.height = height;
  return true;
}

bool ensure_scratch(ID3D11Device *dev, ID3D11Resource *color) {
  ID3D11Texture2D *tex = nullptr;
  if (FAILED(color->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex)))) return false;
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  tex->Release();
  if (desc.Width != g.width || desc.Height != g.height) return false;  // couche et sortie de tailles différentes
  if (g.scratch != nullptr && g.color_format == desc.Format) return true;
  release_scratch();
  D3D11_TEXTURE2D_DESC t{};
  t.Width = desc.Width;
  t.Height = desc.Height;
  t.MipLevels = t.ArraySize = 1;
  t.Format = desc.Format;
  t.SampleDesc.Count = 1;
  t.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(dev->CreateTexture2D(&t, nullptr, &g.scratch)) || (g.scratch_uav = d3d::create_uav(dev, g.scratch)) == nullptr) {
    set_status("format de sortie " + std::to_string(desc.Format) + " sans UAV typée", true);
    release_scratch();
    return false;
  }
  g.color_format = desc.Format;
  return true;
}

}  // namespace

bool is_rain_shader(uint32_t ps_hash) {
  for (uint32_t h : kRainShaders)
    if (h == ps_hash) return true;
  return false;
}

bool redirect(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT indices, UINT instances, UINT first_index,
              INT vertex_offset, UINT first_instance) {
  if (!ensure_pipeline(dev)) return false;
  ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView *dsv = nullptr;
  ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
  bool ok = rtvs[0] != nullptr;
  if (ok) {
    ID3D11Resource *res = nullptr;
    rtvs[0]->GetResource(&res);
    ID3D11Texture2D *tex = nullptr;
    ok = res != nullptr && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex)));
    if (ok) {
      D3D11_TEXTURE2D_DESC desc;
      tex->GetDesc(&desc);
      tex->Release();
      ok = ensure_layer(dev, desc.Width, desc.Height);
    }
    if (res != nullptr) res->Release();
  }
  if (ok) {
    if (!g.drawn) {
      const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // rien de dessiné, transmission totale
      ctx->ClearRenderTargetView(g.layer_rtv, clear);
    }
    ID3D11BlendState *blend = nullptr;
    float factor[4];
    UINT sample_mask;
    ctx->OMGetBlendState(&blend, factor, &sample_mask);
    ctx->OMSetRenderTargets(1, &g.layer_rtv, dsv);
    ctx->OMSetBlendState(g.blend, factor, sample_mask);
    ctx->DrawIndexedInstanced(indices, instances, first_index, vertex_offset, first_instance);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    ctx->OMSetBlendState(blend, factor, sample_mask);
    if (blend != nullptr) blend->Release();
    g.drawn = true;
    set_status("actif", false);
  }
  for (auto *r : rtvs)
    if (r != nullptr) r->Release();
  if (dsv != nullptr) dsv->Release();
  return ok;
}

void composite(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *color) {
  if (!g.drawn) return;
  g.drawn = false;
  if (!ensure_scratch(dev, color)) return;
  ID3D11ShaderResourceView *src = d3d::create_srv(dev, color);
  if (src == nullptr) return;
  {
    d3d::cs_state_guard guard(ctx);
    ID3D11UnorderedAccessView *none[1] = {};
    ctx->CSSetUnorderedAccessViews(0, 1, none, nullptr);
    ID3D11ShaderResourceView *srv[2] = {src, g.layer_srv};
    ctx->CSSetShader(g.cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g.scratch_uav, nullptr);
    ctx->Dispatch((g.width + 7) / 8, (g.height + 7) / 8, 1);
    ID3D11ShaderResourceView *no_srv[2] = {};
    ctx->CSSetShaderResources(0, 2, no_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, none, nullptr);
  }
  src->Release();
  ctx->CopySubresourceRegion(color, 0, 0, 0, 0, g.scratch, 0, nullptr);
}

void end_frame() { g.drawn = false; }

void shutdown() {
  release_layer();
  release_scratch();
  release(g.cs);
  release(g.blend);
  g = {};
}

std::string status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

}  // namespace akfsr::rain
