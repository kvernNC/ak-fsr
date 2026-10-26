#pragma once

// Mesure GPU d'une plage de commandes du contexte immédiat D3D11 (requêtes timestamp, anneau de 4 frames,
// lecture sans attente). Sous DXVK, une plage couvre aussi le travail Vulkan soumis entre ses bornes.

#include <d3d11.h>

#include <mutex>

namespace akfsr::d3d {

class gpu_timer {
 public:
  void begin(ID3D11Device *dev, ID3D11DeviceContext *ctx);
  void end(ID3D11DeviceContext *ctx);
  double last_ms() const;  // 0 tant qu'aucune mesure n'est disponible
  void release();

 private:
  struct slot {
    ID3D11Query *disjoint = nullptr, *start = nullptr, *stop = nullptr;
    bool pending = false;
  } slots_[4];
  slot *current_ = nullptr;
  uint32_t index_ = 0;
  bool tried_ = false, ok_ = false;
  mutable std::mutex mutex_;
  double last_ms_ = 0;
};

}  // namespace akfsr::d3d
