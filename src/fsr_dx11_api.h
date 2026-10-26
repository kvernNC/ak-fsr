#pragma once

// M8.2 — interface C entre l'addon (CRT statique) et ak-fsr-dx11.dll (backend DX11 de FidelityFX,
// bibliothèques MSVC /MD). Aucune structure de FidelityFX ne traverse la frontière.

#include <d3d11.h>
#include <stdint.h>

#define AKFSR_DX11_API_VERSION 1u

enum akfsr_dx11_flags : uint32_t {
  AKFSR_DX11_HDR = 1u << 0,
  AKFSR_DX11_DEPTH_INFINITE = 1u << 1,
  AKFSR_DX11_AUTO_EXPOSURE = 1u << 2,
  AKFSR_DX11_MV_JITTERED = 1u << 3,  // vecteurs de mouvement jitterés (annulation du jitter par FSR)
};

struct akfsr_dx11_create_desc {
  uint32_t version;  // AKFSR_DX11_API_VERSION
  ID3D11Device *device;
  uint32_t width, height;  // AA natif : rendu = sortie
  uint32_t flags;          // akfsr_dx11_flags
  void (*message)(int error, const wchar_t *text);
};

struct akfsr_dx11_dispatch_desc {
  ID3D11DeviceContext *context;
  ID3D11Resource *color, *depth, *motion;  // SRV : couleur HDR, profondeur R32F, vecteurs RG16F
  ID3D11Resource *output;                  // UAV, format de la couleur
  float jitter_x, jitter_y;                // pixels
  float mv_scale_x, mv_scale_y;
  float sharpness;  // 0 : RCAS désactivé
  float frame_ms;
  uint32_t reset;
  float near_plane, far_plane, fov_y, meters_per_unit;
  uint32_t debug_view;
};

// Codes FidelityFX (0 = FFX_OK).
typedef int (*akfsr_dx11_create_fn)(const akfsr_dx11_create_desc *desc, void **context);
typedef int (*akfsr_dx11_dispatch_fn)(void *context, const akfsr_dx11_dispatch_desc *desc);
typedef void (*akfsr_dx11_destroy_fn)(void *context);
