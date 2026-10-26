#include "fsr_dx11.hpp"

#include <d3d11_1.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <reshade.hpp>

#include "d3d_compute.hpp"
#include "fsr_dx11_api.h"

namespace akfsr::fsr::dx11 {
namespace {

// Portage HLSL de shaders/fsr_prepare.comp (mêmes constantes, voir push_constants).
constexpr const char *kPrepareSource = R"(
Texture2D<float> depth_src : register(t0);
Texture2D<float2> object_velocity : register(t1);
RWTexture2D<float> out_depth : register(u0);
RWTexture2D<float2> out_motion : register(u1);
cbuffer constants : register(b0) {
  float4x4 reproj;
  float2 size;
  float2 inv_size;
  float2 jitter_delta_uv;
  uint zero_motion;
  uint use_objects;
};

[numthreads(8, 8, 1)] void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= (uint2)size)) return;
  const float depth = depth_src.Load(int3(p, 0));
  out_depth[p] = depth;
  float2 motion = 0.0;
  if (zero_motion == 0) {
    const float2 object = use_objects != 0 ? object_velocity.Load(int3(p, 0)) : float2(0.0, 0.0);
    if (any(object != 0.0)) {
      motion = object + jitter_delta_uv;
    } else {
      const float2 uv = (float2(p) + 0.5) * inv_size;
      const float4 prev = mul(reproj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
      if (abs(prev.w) > 1e-8) {
        const float2 ndc = prev.xy / prev.w;
        motion = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) - uv;
      }
    }
  }
  out_motion[p] = motion;
}
)";

struct bridge {
  bool tried = false;
  akfsr_dx11_create_fn create = nullptr;
  akfsr_dx11_dispatch_fn dispatch = nullptr;
  akfsr_dx11_destroy_fn destroy = nullptr;
} b;

struct state {
  bool tried_shader = false;
  ID3D11ComputeShader *prepare = nullptr;
  ID3D11Buffer *cb = nullptr;
  ID3DDeviceContextState *our_state = nullptr;
  void *context = nullptr;
  bool ctx_auto_exposure = false;
  bool ctx_game_mv = false;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT width = 0, height = 0;
  ID3D11Texture2D *depth = nullptr, *motion = nullptr, *output = nullptr;
  ID3D11UnorderedAccessView *depth_uav = nullptr, *motion_uav = nullptr;
  // Requêtes de mesure : disjoint + 4 timestamps (début, fin préparation, fin FSR, fin copie), en anneau.
  struct query_set {
    ID3D11Query *disjoint = nullptr;
    ID3D11Query *stamp[4] = {};
    bool pending = false;
  } queries[4];
  uint32_t query_index = 0;
  bool queries_tried = false, queries_ok = false;
} g;

std::mutex g_timing_mutex;
gpu_timings g_timings;

void create_queries(ID3D11Device *dev) {
  D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
  g.queries_ok = true;
  for (auto &q : g.queries) {
    g.queries_ok &= SUCCEEDED(dev->CreateQuery(&disjoint, &q.disjoint));
    for (auto *&s : q.stamp) g.queries_ok &= SUCCEEDED(dev->CreateQuery(&stamp, &s));
  }
}

// Lit sans attendre le jeu de requêtes qui va être réutilisé (émis 4 frames plus tôt).
template <typename QuerySet>
void collect(ID3D11DeviceContext *ctx, QuerySet &q) {
  if (!q.pending) return;
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
  UINT64 t[4] = {};
  if (ctx->GetData(q.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
  for (int i = 0; i < 4; ++i)
    if (ctx->GetData(q.stamp[i], &t[i], sizeof(t[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
  q.pending = false;
  if (disjoint.Disjoint || disjoint.Frequency == 0) return;
  const double to_ms = 1000.0 / static_cast<double>(disjoint.Frequency);
  std::lock_guard lock(g_timing_mutex);
  g_timings = {(t[1] - t[0]) * to_ms, (t[2] - t[1]) * to_ms, (t[3] - t[2]) * to_ms, true};
}

template <typename T>
void release(T *&p) {
  if (p != nullptr) p->Release();
  p = nullptr;
}

void ffx_message(int error, const wchar_t *text) {
  static int count = 0;
  if (++count > 32 || text == nullptr) return;
  char utf8[512];
  WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
  reshade::log::message(error ? reshade::log::level::error : reshade::log::level::warning,
                        (std::string("AK-FSR: [ffx dx11] ") + utf8).c_str());
}

bool load_bridge(std::string &error) {
  if (!b.tried) {
    b.tried = true;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&load_bridge), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t *slash = wcsrchr(path, L'\\')) *(slash + 1) = 0;
    wcscat_s(path, L"ak-fsr-dx11.dll");
    if (HMODULE dll = LoadLibraryW(path)) {
      b.create = reinterpret_cast<akfsr_dx11_create_fn>(GetProcAddress(dll, "akfsrDx11Create"));
      b.dispatch = reinterpret_cast<akfsr_dx11_dispatch_fn>(GetProcAddress(dll, "akfsrDx11Dispatch"));
      b.destroy = reinterpret_cast<akfsr_dx11_destroy_fn>(GetProcAddress(dll, "akfsrDx11Destroy"));
      reshade::log::message(b.destroy ? reshade::log::level::info : reshade::log::level::error,
                            b.destroy ? "AK-FSR: backend DX11 : ak-fsr-dx11.dll chargée"
                                      : "AK-FSR: backend DX11 : exports manquants dans ak-fsr-dx11.dll");
    } else {
      reshade::log::message(reshade::log::level::error,
                            ("AK-FSR: backend DX11 : ak-fsr-dx11.dll introuvable à côté de l'addon (erreur " +
                             std::to_string(GetLastError()) + ")")
                                .c_str());
    }
  }
  if (b.create == nullptr || b.dispatch == nullptr || b.destroy == nullptr) {
    error = "ak-fsr-dx11.dll absente ou invalide";
    return false;
  }
  return true;
}

void destroy_context() {
  if (g.context != nullptr) b.destroy(g.context);
  g.context = nullptr;
}

void release_textures() {
  destroy_context();
  release(g.depth_uav);
  release(g.motion_uav);
  release(g.depth);
  release(g.motion);
  release(g.output);
  g.format = DXGI_FORMAT_UNKNOWN;
}

bool ensure_shared(ID3D11Device *dev, std::string &error) {
  if (g.tried_shader) {
    if (g.prepare == nullptr) error = "préparation indisponible (voir log)";
    return g.prepare != nullptr;
  }
  g.tried_shader = true;
  ID3D11Device1 *dev1 = nullptr;
  if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1)))) {
    error = "ID3D11Device1 indisponible";
  } else {
    const D3D_FEATURE_LEVEL level = dev->GetFeatureLevel();
    if (FAILED(dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr,
                                              &g.our_state)))
      error = "CreateDeviceContextState en échec";
    dev1->Release();
  }
  D3D11_BUFFER_DESC cb{sizeof(push_constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE,
                       0, 0};
  if (g.our_state != nullptr && SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &g.cb)))
    g.prepare = d3d::compile_cs(dev, kPrepareSource, "main", error);
  if (g.prepare == nullptr)
    reshade::log::message(reshade::log::level::error, ("AK-FSR: backend DX11 : " + error).c_str());
  return g.prepare != nullptr;
}

bool ensure_textures(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC &color, std::string &error) {
  if (g.output != nullptr && g.format == color.Format && g.width == color.Width && g.height == color.Height)
    return true;
  release_textures();
  UINT support = 0;
  if (FAILED(dev->CheckFormatSupport(color.Format, &support)) ||
      (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) == 0) {
    error = "format de couleur " + std::to_string(color.Format) + " sans UAV typée";
    return false;
  }
  D3D11_TEXTURE2D_DESC t{};
  t.Width = color.Width;
  t.Height = color.Height;
  t.MipLevels = t.ArraySize = 1;
  t.SampleDesc.Count = 1;
  t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  t.Format = color.Format;
  bool ok = SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &g.output));
  t.Format = DXGI_FORMAT_R32_FLOAT;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &g.depth));
  t.Format = DXGI_FORMAT_R16G16_FLOAT;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&t, nullptr, &g.motion));
  if (ok) {
    g.depth_uav = d3d::create_uav(dev, g.depth);
    g.motion_uav = d3d::create_uav(dev, g.motion);
  }
  if (!ok || g.depth_uav == nullptr || g.motion_uav == nullptr) {
    error = "création des textures intermédiaires en échec";
    release_textures();
    return false;
  }
  g.format = color.Format;
  g.width = color.Width;
  g.height = color.Height;
  return true;
}

bool ensure_context(ID3D11Device *dev, const settings &s, std::string &error) {
  if (g.context != nullptr && g.ctx_auto_exposure == s.auto_exposure && g.ctx_game_mv == s.game_mv) return true;
  destroy_context();
  akfsr_dx11_create_desc desc{};
  desc.version = AKFSR_DX11_API_VERSION;
  desc.device = dev;
  desc.width = g.width;
  desc.height = g.height;
  desc.flags = AKFSR_DX11_HDR | AKFSR_DX11_DEPTH_INFINITE | (s.auto_exposure ? AKFSR_DX11_AUTO_EXPOSURE : 0u) |
               (s.game_mv ? AKFSR_DX11_MV_JITTERED : 0u);
  desc.message = ffx_message;
  const int rc = b.create(&desc, &g.context);
  if (rc != 0) {
    g.context = nullptr;
    error = "création du contexte FSR : code " + std::to_string(rc);
    return false;
  }
  g.ctx_auto_exposure = s.auto_exposure;
  g.ctx_game_mv = s.game_mv;
  char buf[160];
  std::snprintf(buf, sizeof(buf), "AK-FSR: backend DX11 : contexte FSR 3 créé : %ux%u (AA natif), format DXGI %d",
                g.width, g.height, static_cast<int>(g.format));
  reshade::log::message(reshade::log::level::info, buf);
  return true;
}

ID3D11ShaderResourceView *srv(ID3D11Device *dev, ID3D11Resource *res, DXGI_FORMAT format) {
  D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipLevels = 1;
  ID3D11ShaderResourceView *view = nullptr;
  return SUCCEEDED(dev->CreateShaderResourceView(res, &desc, &view)) ? view : nullptr;
}

}  // namespace

bool run(const frame_inputs &in, const settings &s, const frame_params &p, const D3D11_TEXTURE2D_DESC &color,
         std::string &error) {
  ID3D11Device *dev = nullptr;
  in.color->GetDevice(&dev);
  if (dev == nullptr) {
    error = "device introuvable";
    return false;
  }
  ID3D11DeviceContext *imm = nullptr;
  dev->GetImmediateContext(&imm);
  ID3D11DeviceContext1 *imm1 = nullptr;
  if (imm != nullptr) imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&imm1));
  bool ok = imm1 != nullptr && load_bridge(error) && ensure_shared(dev, error) && ensure_textures(dev, color, error) &&
            ensure_context(dev, s, error);
  if (imm1 == nullptr && error.empty()) error = "ID3D11DeviceContext1 indisponible";

  ID3D11ShaderResourceView *depth_src = nullptr, *velocity_src = nullptr;
  if (ok) {
    // Profondeur du jeu D32_FLOAT_S8X24 (typeless) : aspect profondeur.
    depth_src = srv(dev, in.depth, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    if (p.objects) velocity_src = srv(dev, in.velocity, DXGI_FORMAT_R16G16_FLOAT);
    if (depth_src == nullptr) {
      error = "SRV sur la profondeur du jeu impossible";
      ok = false;
    }
  }
  int rc = 0;
  if (ok) {
    if (!g.queries_tried) {
      g.queries_tried = true;
      create_queries(dev);
    }
    auto &q = g.queries[g.query_index++ % 4];
    if (g.queries_ok) {
      collect(imm1, q);
      imm1->Begin(q.disjoint);
      imm1->End(q.stamp[0]);
    }

    // État D3D11 du jeu mis de côté : FidelityFX lie librement ses ressources.
    ID3DDeviceContextState *game_state = nullptr;
    imm1->SwapDeviceContextState(g.our_state, &game_state);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(imm1->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      std::memcpy(mapped.pData, &p.pc, sizeof(p.pc));
      imm1->Unmap(g.cb, 0);
    }
    ID3D11ShaderResourceView *srvs[2] = {depth_src, velocity_src};
    ID3D11UnorderedAccessView *uavs[2] = {g.depth_uav, g.motion_uav};
    imm1->CSSetShader(g.prepare, nullptr, 0);
    imm1->CSSetShaderResources(0, 2, srvs);
    imm1->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    imm1->CSSetConstantBuffers(0, 1, &g.cb);
    imm1->Dispatch((g.width + 7) / 8, (g.height + 7) / 8, 1);
    ID3D11ShaderResourceView *no_srv[2] = {};
    ID3D11UnorderedAccessView *no_uav[2] = {};
    imm1->CSSetShaderResources(0, 2, no_srv);
    imm1->CSSetUnorderedAccessViews(0, 2, no_uav, nullptr);
    if (g.queries_ok) imm1->End(q.stamp[1]);

    akfsr_dx11_dispatch_desc d{};
    d.context = imm1;
    d.color = in.color;
    d.depth = g.depth;
    d.motion = p.game_mv ? in.velocity : g.motion;  // M8.3 : RT4 du jeu telle quelle
    d.output = g.output;
    d.jitter_x = p.jitter_px_x;
    d.jitter_y = p.jitter_px_y;
    d.mv_scale_x = p.mv_scale_x;
    d.mv_scale_y = p.mv_scale_y;
    d.sharpness = s.sharpness;
    d.frame_ms = p.frame_ms;
    d.reset = p.reset ? 1u : 0u;
    d.near_plane = p.near_plane > 0 ? p.near_plane : 10.0f;
    d.far_plane = FLT_MAX;
    d.fov_y = p.fov;
    d.meters_per_unit = 0.01f;
    d.debug_view = s.debug_view ? 1u : 0u;
    rc = b.dispatch(g.context, &d);
    if (g.queries_ok) imm1->End(q.stamp[2]);

    imm1->SwapDeviceContextState(game_state, nullptr);
    if (game_state != nullptr) game_state->Release();
    if (rc == 0) {
      imm1->CopySubresourceRegion(in.output != nullptr ? in.output : in.color, 0, 0, 0, 0, g.output, 0, nullptr);
    } else {
      error = "dispatch FSR : code " + std::to_string(rc);
      ok = false;
    }
    if (g.queries_ok) {
      imm1->End(q.stamp[3]);
      imm1->End(q.disjoint);
      q.pending = true;
    }
  }
  release(depth_src);
  release(velocity_src);
  release(imm1);
  release(imm);
  release(dev);
  return ok;
}

gpu_timings get_gpu_timings() {
  std::lock_guard lock(g_timing_mutex);
  return g_timings;
}

void shutdown() {
  for (auto &q : g.queries) {
    release(q.disjoint);
    for (auto *&s : q.stamp) release(s);
  }
  release_textures();
  release(g.prepare);
  release(g.cb);
  release(g.our_state);
  g = {};
}

}  // namespace akfsr::fsr::dx11
