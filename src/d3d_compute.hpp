#pragma once

// Petits compute shaders D3D11 compilés à l'exécution (d3dcompiler_47 de Wine/Proton), exécutés sur le
// contexte immédiat natif au milieu du flux du jeu : l'état CS du jeu est sauvegardé puis restauré.
// Sert aux passes ajoutées sans glslang (M7 : hautes lumières).

#include <d3d11.h>

#include <string>

namespace akfsr::d3d {

// Compile `entry` (cs_5_0) dans `source` ; nullptr en cas d'échec (message dans `error`).
ID3D11ComputeShader *compile_cs(ID3D11Device *dev, const char *source, const char *entry, std::string &error);

// SRV / UAV sur toute la texture (mip 0), au format de la texture ; nullptr si impossible.
ID3D11ShaderResourceView *create_srv(ID3D11Device *dev, ID3D11Resource *res);
ID3D11UnorderedAccessView *create_uav(ID3D11Device *dev, ID3D11Resource *res);

// Sauvegarde de l'état CS (shader, t0..t3, u0..u1, b0) restauré à la destruction.
class cs_state_guard {
 public:
  explicit cs_state_guard(ID3D11DeviceContext *ctx);
  ~cs_state_guard();
  cs_state_guard(const cs_state_guard &) = delete;
  cs_state_guard &operator=(const cs_state_guard &) = delete;

 private:
  ID3D11DeviceContext *ctx_;
  ID3D11ComputeShader *shader_ = nullptr;
  ID3D11ClassInstance *instances_[256] = {};
  UINT instance_count_ = 256;
  ID3D11ShaderResourceView *srv_[4] = {};
  ID3D11UnorderedAccessView *uav_[2] = {};
  ID3D11Buffer *cb_ = nullptr;
};

}  // namespace akfsr::d3d
