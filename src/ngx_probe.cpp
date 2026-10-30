#include "ngx_probe.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <windows.h>

#include <cstdio>

#include <reshade.hpp>

namespace akfsr::ngx_probe {
namespace {

// --- Sous-ensemble minimal de l'API NGX (nvngx.dll), déclaré à la main pour l'interopérabilité ---
// Ordre des méthodes virtuelles identique à nvsdk_ngx_params.h (l'ABI du vtable en dépend).

using NVSDK_NGX_Result = unsigned int;
constexpr NVSDK_NGX_Result kResultSuccess = 0x1;
constexpr int kVersionApi = 0x15;  // NGX 1.5.0

struct NVSDK_NGX_Parameter {
  virtual void Set(const char *, unsigned long long) = 0;
  virtual void Set(const char *, float) = 0;
  virtual void Set(const char *, double) = 0;
  virtual void Set(const char *, unsigned int) = 0;
  virtual void Set(const char *, int) = 0;
  virtual void Set(const char *, ID3D11Resource *) = 0;
  virtual void Set(const char *, ID3D12Resource *) = 0;
  virtual void Set(const char *, void *) = 0;
  virtual NVSDK_NGX_Result Get(const char *, unsigned long long *) const = 0;
  virtual NVSDK_NGX_Result Get(const char *, float *) const = 0;
  virtual NVSDK_NGX_Result Get(const char *, double *) const = 0;
  virtual NVSDK_NGX_Result Get(const char *, unsigned int *) const = 0;
  virtual NVSDK_NGX_Result Get(const char *, int *) const = 0;
  virtual NVSDK_NGX_Result Get(const char *, ID3D11Resource **) const = 0;
  virtual NVSDK_NGX_Result Get(const char *, ID3D12Resource **) const = 0;
  virtual NVSDK_NGX_Result Get(const char *, void **) const = 0;
  virtual void Reset() = 0;
};

using PFN_Init = NVSDK_NGX_Result(*)(unsigned long long app_id, const wchar_t *data_path,
                                                ID3D11Device *device, int sdk_version);
using PFN_GetCapabilityParameters = NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter **out);
using PFN_DestroyParameters = NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter *);
using PFN_Shutdown1 = NVSDK_NGX_Result(*)(ID3D11Device *device);

int get_int(NVSDK_NGX_Parameter *p, const char *name) {
  int v = -1;
  return p != nullptr && p->Get(name, &v) == kResultSuccess ? v : -1;
}

}  // namespace

std::string run(ID3D11Device *device) {
  char buf[256];
  if (device == nullptr) return "device D3D11 absent";

  // nvngx.dll à côté de l'addon (= OptiScaler renommé). Chargement explicite, pas d'import.
  HMODULE dll = LoadLibraryW(L"nvngx.dll");
  if (dll == nullptr) {
    std::snprintf(buf, sizeof(buf), "nvngx.dll introuvable (erreur %lu) — OptiScaler posé en nvngx.dll ?",
                  GetLastError());
    reshade::log::message(reshade::log::level::warning, (std::string("AK-FSR: [ngx] ") + buf).c_str());
    return buf;
  }

  auto init = reinterpret_cast<PFN_Init>(GetProcAddress(dll, "NVSDK_NGX_D3D11_Init"));
  auto caps = reinterpret_cast<PFN_GetCapabilityParameters>(
      GetProcAddress(dll, "NVSDK_NGX_D3D11_GetCapabilityParameters"));
  auto destroy = reinterpret_cast<PFN_DestroyParameters>(GetProcAddress(dll, "NVSDK_NGX_D3D11_DestroyParameters"));
  auto shutdown = reinterpret_cast<PFN_Shutdown1>(GetProcAddress(dll, "NVSDK_NGX_D3D11_Shutdown1"));
  reshade::log::message(reshade::log::level::info,
                        (std::string("AK-FSR: [ngx] nvngx.dll chargée ; exports Init=") +
                         (init ? "oui" : "non") + " caps=" + (caps ? "oui" : "non") +
                         " shutdown=" + (shutdown ? "oui" : "non"))
                            .c_str());
  if (init == nullptr || caps == nullptr) return "nvngx.dll sans les exports NGX attendus";

  // AppId arbitraire ; OptiScaler l'ignore. Chemin de données = dossier courant.
  const NVSDK_NGX_Result ri = init(0x414B4653ull, L".", device, kVersionApi);
  std::snprintf(buf, sizeof(buf), "AK-FSR: [ngx] NVSDK_NGX_D3D11_Init : 0x%08X", ri);
  reshade::log::message(ri == kResultSuccess ? reshade::log::level::info : reshade::log::level::warning, buf);
  if (ri != kResultSuccess) {
    if (shutdown != nullptr) shutdown(device);
    std::snprintf(buf, sizeof(buf), "Init NGX refusé (0x%08X) — OptiScaler n'a pas initialisé DLSS", ri);
    return buf;
  }

  NVSDK_NGX_Parameter *params = nullptr;
  const NVSDK_NGX_Result rc = caps(&params);
  int available = -1, needs_driver = -1, init_result = -1;
  if (rc == kResultSuccess && params != nullptr) {
    available = get_int(params, "SuperSampling.Available");
    needs_driver = get_int(params, "SuperSampling.NeedsUpdatedDriver");
    init_result = get_int(params, "SuperSampling.FeatureInitResult");
  }
  std::snprintf(buf, sizeof(buf),
                "AK-FSR: [ngx] capacités (0x%08X) : DLSS disponible=%d, pilote à jour requis=%d, init=0x%X",
                rc, available, needs_driver, init_result);
  reshade::log::message(reshade::log::level::info, buf);

  if (destroy != nullptr && params != nullptr) destroy(params);
  if (shutdown != nullptr) shutdown(device);

  std::snprintf(buf, sizeof(buf), "Init OK, DLSS disponible=%d (voir ReShade.log [ngx])", available);
  return buf;
}

}  // namespace akfsr::ngx_probe
