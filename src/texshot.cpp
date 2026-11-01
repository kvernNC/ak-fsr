#include "texshot.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <reshade.hpp>

#include "screenshot.hpp"

namespace akfsr::texshot {
namespace {

struct pending_request {
  uint32_t cs_hash = 0;
  std::string target, name;
  bool after = false;
};

struct copy {
  ID3D11Texture2D *staging = nullptr;
  std::string name;
};

struct cb_override {
  bool pixel = false;
  uint32_t hash = 0, slot = 0, reg = 0, comp = 0;
  float value = 0;
};
std::vector<cb_override> g_overrides;
struct bound_copy {
  bool pixel = false;
  uint32_t slot = 0;
  ID3D11Buffer *original = nullptr, *ours = nullptr;
};
std::vector<bound_copy> g_bound;  // copies liées par la dernière commande, à retirer à la suivante

std::mutex g_mutex;
std::vector<pending_request> g_requests;
std::vector<copy> g_copies;
ID3D11Resource *g_after = nullptr;  // ressource à copier à la commande suivante (mode `after`)
std::string g_after_name;

void log(reshade::log::level level, const std::string &s) { reshade::log::message(level, ("AK-FSR: texshot : " + s).c_str()); }

ID3D11Resource *from_view(uint64_t view) {
  if (view == 0) return nullptr;
  ID3D11Resource *res = nullptr;
  reinterpret_cast<ID3D11View *>(view)->GetResource(&res);
  if (res != nullptr) res->Release();  // la vue garde la ressource en vie pendant la commande
  return res;
}

// Copie de la sous-ressource 0 d'une texture 2D vers une texture de staging (lecture CPU au present).
void copy_now(ID3D11DeviceContext *ctx, ID3D11Resource *res, const std::string &name) {
  ID3D11Texture2D *tex = nullptr;
  if (res == nullptr || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex)))) {
    log(reshade::log::level::warning, name + " : ressource absente ou pas une texture 2D");
    return;
  }
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.SampleDesc = {1, 0};
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Device *dev = nullptr;
  ctx->GetDevice(&dev);
  ID3D11Texture2D *staging = nullptr;
  const HRESULT hr = dev->CreateTexture2D(&d, nullptr, &staging);
  dev->Release();
  if (FAILED(hr)) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), " : texture de staging impossible (format %d, 0x%08lX)", static_cast<int>(d.Format),
                  static_cast<unsigned long>(hr));
    log(reshade::log::level::warning, name + buf);
    tex->Release();
    return;
  }
  ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
  tex->Release();
  g_copies.push_back({staging, name});
}

// Retire nos copies si elles sont encore liées (le jeu a pu lier autre chose entre-temps : on n'y touche pas).
void unbind_copies(ID3D11DeviceContext *ctx) {
  for (auto &b : g_bound) {
    ID3D11Buffer *now = nullptr;
    if (b.pixel)
      ctx->PSGetConstantBuffers(b.slot, 1, &now);
    else
      ctx->CSGetConstantBuffers(b.slot, 1, &now);
    if (now == b.ours) {
      if (b.pixel)
        ctx->PSSetConstantBuffers(b.slot, 1, &b.original);
      else
        ctx->CSSetConstantBuffers(b.slot, 1, &b.original);
    }
    if (now != nullptr) now->Release();
    if (b.original != nullptr) b.original->Release();
    b.ours->Release();
  }
  g_bound.clear();
}

// Copie du CB lié avec les constantes forcées, liée à la place de l'original.
void apply_overrides(ID3D11DeviceContext *ctx, bool pixel, uint32_t hash) {
  for (const auto &o : g_overrides) {
    if (o.pixel != pixel || o.hash != hash) continue;
    bool done = false;
    for (const auto &b : g_bound) done = done || (b.pixel == pixel && b.slot == o.slot);
    if (done) continue;  // plusieurs constantes du même CB : appliquées ensemble ci-dessous
    ID3D11Buffer *orig = nullptr;
    if (pixel)
      ctx->PSGetConstantBuffers(o.slot, 1, &orig);
    else
      ctx->CSGetConstantBuffers(o.slot, 1, &orig);
    if (orig == nullptr) continue;
    D3D11_BUFFER_DESC d;
    orig->GetDesc(&d);
    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    D3D11_BUFFER_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    D3D11_BUFFER_DESC od = d;
    od.Usage = D3D11_USAGE_DEFAULT;
    od.CPUAccessFlags = 0;
    od.MiscFlags = 0;
    ID3D11Buffer *staging = nullptr, *ours = nullptr;
    if (FAILED(dev->CreateBuffer(&sd, nullptr, &staging)) || FAILED(dev->CreateBuffer(&od, nullptr, &ours))) {
      if (staging) staging->Release();
      dev->Release();
      orig->Release();
      continue;
    }
    dev->Release();
    // Contenu actuel du CB (lecture CPU : coûteux, diagnostic seulement).
    ctx->CopyResource(staging, orig);
    D3D11_MAPPED_SUBRESOURCE m;
    std::vector<float> data(d.ByteWidth / 4);
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      std::memcpy(data.data(), m.pData, d.ByteWidth);
      ctx->Unmap(staging, 0);
    }
    staging->Release();
    for (const auto &p : g_overrides)
      if (p.pixel == pixel && p.hash == hash && p.slot == o.slot && p.reg * 4 + p.comp < data.size())
        data[p.reg * 4 + p.comp] = p.value;
    ctx->UpdateSubresource(ours, 0, nullptr, data.data(), 0, 0);
    if (pixel)
      ctx->PSSetConstantBuffers(o.slot, 1, &ours);
    else
      ctx->CSSetConstantBuffers(o.slot, 1, &ours);
    g_bound.push_back({pixel, o.slot, orig, ours});
  }
}

}  // namespace

void set_override(bool pixel, uint32_t hash, uint32_t slot, uint32_t reg, uint32_t comp, float value) {
  std::lock_guard lock(g_mutex);
  for (auto &o : g_overrides)
    if (o.pixel == pixel && o.hash == hash && o.slot == slot && o.reg == reg && o.comp == comp) {
      o.value = value;
      return;
    }
  g_overrides.push_back({pixel, hash, slot, reg, comp, value});
}

void clear_overrides() {
  std::lock_guard lock(g_mutex);
  g_overrides.clear();
}

void request(uint32_t cs_hash, const std::string &target, const std::string &name, bool after) {
  std::lock_guard lock(g_mutex);
  g_requests.push_back({cs_hash, target, name, after});
}

void on_dispatch(ID3D11DeviceContext *ctx, uint32_t cs_hash, const uint64_t *srv, size_t srv_count, const uint64_t *uav,
                 size_t uav_count) {
  std::lock_guard lock(g_mutex);
  if (!g_bound.empty()) unbind_copies(ctx);
  if (g_after != nullptr) {
    copy_now(ctx, g_after, g_after_name);
    g_after->Release();
    g_after = nullptr;
  }
  if (!g_overrides.empty()) apply_overrides(ctx, false, cs_hash);
  if (g_requests.empty()) return;
  for (size_t i = 0; i < g_requests.size();) {
    const pending_request r = g_requests[i];
    if (r.cs_hash != cs_hash || (r.after && g_after != nullptr)) {
      ++i;
      continue;
    }
    ID3D11Resource *res = nullptr;
    const char kind = r.target.empty() ? 0 : r.target[0];
    const size_t slot = r.target.size() > 1 ? std::strtoul(r.target.c_str() + 1, nullptr, 10) : 0;
    if (kind == 't' && r.target.size() <= 2) {
      res = slot < srv_count ? from_view(srv[slot]) : nullptr;
    } else if (kind == 'u' && r.target.size() <= 2) {
      res = slot < uav_count ? from_view(uav[slot]) : nullptr;
    } else {
      res = reinterpret_cast<ID3D11Resource *>(std::strtoull(r.target.c_str(), nullptr, 16));
    }
    if (r.after) {
      if (res != nullptr) {
        res->AddRef();
        g_after = res;
        g_after_name = r.name;
      }
    } else {
      copy_now(ctx, res, r.name);
    }
    g_requests.erase(g_requests.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

void on_draw(ID3D11DeviceContext *ctx, uint32_t ps_hash) {
  std::lock_guard lock(g_mutex);
  if (!g_bound.empty()) unbind_copies(ctx);
  if (g_after != nullptr) {
    copy_now(ctx, g_after, g_after_name);
    g_after->Release();
    g_after = nullptr;
  }
  if (!g_overrides.empty()) apply_overrides(ctx, true, ps_hash);
  for (size_t i = 0; i < g_requests.size();) {
    const pending_request r = g_requests[i];
    const bool handle = r.target.size() > 2;
    if (r.cs_hash != ps_hash || !handle || (r.after && g_after != nullptr)) {
      ++i;
      continue;
    }
    auto *res = reinterpret_cast<ID3D11Resource *>(std::strtoull(r.target.c_str(), nullptr, 16));
    if (r.after) {
      res->AddRef();
      g_after = res;
      g_after_name = r.name;
    } else {
      copy_now(ctx, res, r.name);
    }
    g_requests.erase(g_requests.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

void on_present(ID3D11DeviceContext *ctx, const std::filesystem::path &dir) {
  std::vector<copy> copies;
  {
    std::lock_guard lock(g_mutex);
    copies.swap(g_copies);
  }
  for (auto &c : copies) {
    log(reshade::log::level::info, c.name + " " + screenshot::save_staging(ctx, c.staging, dir / (c.name + ".png")));
    c.staging->Release();
  }
}

}  // namespace akfsr::texshot
