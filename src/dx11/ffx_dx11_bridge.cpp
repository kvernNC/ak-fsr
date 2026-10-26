// ak-fsr-dx11.dll — M8.2 : FSR 3 avec le backend DX11 natif de FidelityFX (pas de Vulkan, pas de copies
// D3D11 <-> Vulkan, pas de FlushRenderingCommands). Bibliothèques FidelityFX © AMD (MIT), précompilées par
// Luma Framework (MIT) : voir scripts/fetch_ffx_dx11.sh. Compilée en CRT dynamique (/MD) comme elles.

#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <new>
#include <vector>

#include "../fsr_dx11_api.h"

// Dans la bibliothèque précompilée, ffxGetResourceDX11 est exportée avec une décoration C++ (déclarée hors
// du bloc extern "C" lors de sa compilation) alors que l'en-tête la déclare en C : on relie les deux noms.
#pragma comment(linker, "/alternatename:ffxGetResourceDX11=" \
                        "?ffxGetResourceDX11@@YA?AUFfxResource@@PEAUID3D11Resource@@UFfxResourceDescription@@PEB_WW4FfxResourceStates@@@Z")

namespace {

struct bridge_context {
  FfxFsr3UpscalerContext fsr{};
  std::vector<unsigned char> scratch;
};

void (*g_message)(int, const wchar_t *) = nullptr;

void on_message(FfxMsgType type, const wchar_t *text) {
  if (g_message != nullptr) g_message(type == FFX_MESSAGE_TYPE_ERROR ? 1 : 0, text);
}

FfxResource resource(ID3D11Resource *res, const wchar_t *name, FfxResourceStates state) {
  return ffxGetResourceDX11(res, GetFfxResourceDescriptionDX11(res), name, state);
}

}  // namespace

extern "C" __declspec(dllexport) int akfsrDx11Create(const akfsr_dx11_create_desc *desc, void **context) {
  if (desc == nullptr || context == nullptr || desc->version != AKFSR_DX11_API_VERSION || desc->device == nullptr)
    return FFX_ERROR_INVALID_ARGUMENT;
  auto *c = new (std::nothrow) bridge_context;
  if (c == nullptr) return FFX_ERROR_OUT_OF_MEMORY;
  g_message = desc->message;
  const size_t size = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
  c->scratch.resize(size);
  FfxFsr3UpscalerContextDescription d{};
  FfxErrorCode rc = ffxGetInterfaceDX11(&d.backendInterface, ffxGetDeviceDX11(desc->device), c->scratch.data(), size,
                                        FFX_FSR3UPSCALER_CONTEXT_COUNT);
  if (rc != FFX_OK) {
    delete c;
    return rc;
  }
  if (desc->flags & AKFSR_DX11_HDR) d.flags |= FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
  if (desc->flags & AKFSR_DX11_DEPTH_INFINITE) d.flags |= FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;
  if (desc->flags & AKFSR_DX11_AUTO_EXPOSURE) d.flags |= FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
  if (desc->flags & AKFSR_DX11_MV_JITTERED) d.flags |= FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
  d.maxRenderSize = {desc->width, desc->height};
  d.maxUpscaleSize = {desc->width, desc->height};
  d.fpMessage = on_message;
  rc = ffxFsr3UpscalerContextCreate(&c->fsr, &d);
  if (rc != FFX_OK) {
    delete c;
    return rc;
  }
  *context = c;
  return FFX_OK;
}

extern "C" __declspec(dllexport) int akfsrDx11Dispatch(void *context, const akfsr_dx11_dispatch_desc *desc) {
  if (context == nullptr || desc == nullptr) return FFX_ERROR_INVALID_ARGUMENT;
  auto *c = static_cast<bridge_context *>(context);
  FfxFsr3UpscalerDispatchDescription d{};
  d.commandList = ffxGetCommandListDX11(desc->context);
  d.color = resource(desc->color, L"AK-FSR couleur", FFX_RESOURCE_STATE_COMPUTE_READ);
  d.depth = resource(desc->depth, L"AK-FSR profondeur", FFX_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = resource(desc->motion, L"AK-FSR vecteurs", FFX_RESOURCE_STATE_COMPUTE_READ);
  d.output = resource(desc->output, L"AK-FSR sortie", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
  d.jitterOffset = {desc->jitter_x, desc->jitter_y};
  d.motionVectorScale = {desc->mv_scale_x, desc->mv_scale_y};
  const uint32_t w = static_cast<uint32_t>(d.output.description.width);
  const uint32_t h = static_cast<uint32_t>(d.output.description.height);
  d.renderSize = {w, h};
  d.upscaleSize = {w, h};
  d.enableSharpening = desc->sharpness > 0.0f;
  d.sharpness = desc->sharpness;
  d.frameTimeDelta = desc->frame_ms;
  d.preExposure = 1.0f;
  d.reset = desc->reset != 0;
  d.cameraNear = desc->near_plane;
  d.cameraFar = desc->far_plane;
  d.cameraFovAngleVertical = desc->fov_y;
  d.viewSpaceToMetersFactor = desc->meters_per_unit;
  d.flags = desc->debug_view ? FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW : 0;
  return ffxFsr3UpscalerContextDispatch(&c->fsr, &d);
}

extern "C" __declspec(dllexport) void akfsrDx11Destroy(void *context) {
  auto *c = static_cast<bridge_context *>(context);
  if (c == nullptr) return;
  ffxFsr3UpscalerContextDestroy(&c->fsr);
  delete c;
}
