#include "d3d_compute.hpp"

#include <d3dcompiler.h>

#include <cstring>

namespace akfsr::d3d {
namespace {

using compile_fn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                     LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

compile_fn load_compiler() {
  static compile_fn fn = [] {
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    return dll != nullptr ? reinterpret_cast<compile_fn>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  }();
  return fn;
}

DXGI_FORMAT texture_format(ID3D11Resource *res, UINT &width, UINT &height) {
  ID3D11Texture2D *tex = nullptr;
  if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex)))) return DXGI_FORMAT_UNKNOWN;
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  tex->Release();
  width = desc.Width;
  height = desc.Height;
  return desc.Format;
}

}  // namespace

ID3D11ComputeShader *compile_cs(ID3D11Device *dev, const char *source, const char *entry, std::string &error) {
  const compile_fn compile = load_compiler();
  if (compile == nullptr) {
    error = "d3dcompiler_47.dll (D3DCompile) introuvable";
    return nullptr;
  }
  ID3DBlob *code = nullptr, *messages = nullptr;
  const HRESULT hr = compile(source, std::strlen(source), "ak-fsr", nullptr, nullptr, entry, "cs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
  if (FAILED(hr)) {
    error = messages != nullptr ? static_cast<const char *>(messages->GetBufferPointer()) : "D3DCompile en échec";
    if (messages != nullptr) messages->Release();
    return nullptr;
  }
  if (messages != nullptr) messages->Release();
  ID3D11ComputeShader *cs = nullptr;
  if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs))) {
    error = "CreateComputeShader en échec";
    cs = nullptr;
  }
  code->Release();
  return cs;
}

ID3D11ShaderResourceView *create_srv(ID3D11Device *dev, ID3D11Resource *res) {
  UINT w, h;
  const DXGI_FORMAT format = texture_format(res, w, h);
  if (format == DXGI_FORMAT_UNKNOWN) return nullptr;
  D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipLevels = 1;
  ID3D11ShaderResourceView *srv = nullptr;
  return SUCCEEDED(dev->CreateShaderResourceView(res, &desc, &srv)) ? srv : nullptr;
}

ID3D11UnorderedAccessView *create_uav(ID3D11Device *dev, ID3D11Resource *res) {
  UINT w, h;
  const DXGI_FORMAT format = texture_format(res, w, h);
  if (format == DXGI_FORMAT_UNKNOWN) return nullptr;
  D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  ID3D11UnorderedAccessView *uav = nullptr;
  return SUCCEEDED(dev->CreateUnorderedAccessView(res, &desc, &uav)) ? uav : nullptr;
}

cs_state_guard::cs_state_guard(ID3D11DeviceContext *ctx) : ctx_(ctx) {
  ctx_->CSGetShader(&shader_, instances_, &instance_count_);
  ctx_->CSGetShaderResources(0, 4, srv_);
  ctx_->CSGetUnorderedAccessViews(0, 2, uav_);
  ctx_->CSGetConstantBuffers(0, 1, &cb_);
}

cs_state_guard::~cs_state_guard() {
  ctx_->CSSetShader(shader_, instances_, instance_count_);
  ctx_->CSSetShaderResources(0, 4, srv_);
  ctx_->CSSetUnorderedAccessViews(0, 2, uav_, nullptr);  // nullptr : compteurs conservés
  ctx_->CSSetConstantBuffers(0, 1, &cb_);
  if (shader_ != nullptr) shader_->Release();
  for (UINT i = 0; i < instance_count_; ++i)
    if (instances_[i] != nullptr) instances_[i]->Release();
  for (auto *v : srv_)
    if (v != nullptr) v->Release();
  for (auto *v : uav_)
    if (v != nullptr) v->Release();
  if (cb_ != nullptr) cb_->Release();
}

full_cs_state_guard::full_cs_state_guard(ID3D11DeviceContext *ctx) : ctx_(ctx) {
  ctx_->CSGetShader(&shader_, instances_, &instance_count_);
  ctx_->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srv_);
  ctx_->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, uav_);
  ctx_->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cb_);
  ctx_->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_);
}

full_cs_state_guard::~full_cs_state_guard() {
  ctx_->CSSetShader(shader_, instances_, instance_count_);
  ctx_->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srv_);
  ctx_->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, uav_, nullptr);
  ctx_->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cb_);
  ctx_->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_);
  auto release = [](auto *p) {
    if (p != nullptr) p->Release();
  };
  release(shader_);
  for (UINT i = 0; i < instance_count_; ++i) release(instances_[i]);
  for (auto *p : srv_) release(p);
  for (auto *p : uav_) release(p);
  for (auto *p : cb_) release(p);
  for (auto *p : samplers_) release(p);
}

}  // namespace akfsr::d3d
