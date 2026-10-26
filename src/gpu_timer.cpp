#include "gpu_timer.hpp"

namespace akfsr::d3d {

void gpu_timer::begin(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
  current_ = nullptr;
  if (!tried_) {
    tried_ = true;
    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
    ok_ = true;
    for (auto &s : slots_)
      ok_ = ok_ && SUCCEEDED(dev->CreateQuery(&disjoint, &s.disjoint)) && SUCCEEDED(dev->CreateQuery(&stamp, &s.start)) &&
            SUCCEEDED(dev->CreateQuery(&stamp, &s.stop));
  }
  if (!ok_) return;
  slot &s = slots_[index_++ % 4];
  if (s.pending) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
    UINT64 t0 = 0, t1 = 0;
    if (ctx->GetData(s.disjoint, &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        ctx->GetData(s.start, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        ctx->GetData(s.stop, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !d.Disjoint &&
        d.Frequency != 0) {
      std::lock_guard lock(mutex_);
      last_ms_ = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(d.Frequency);
    }
    s.pending = false;
  }
  ctx->Begin(s.disjoint);
  ctx->End(s.start);
  current_ = &s;
}

void gpu_timer::end(ID3D11DeviceContext *ctx) {
  if (current_ == nullptr) return;
  ctx->End(current_->stop);
  ctx->End(current_->disjoint);
  current_->pending = true;
  current_ = nullptr;
}

double gpu_timer::last_ms() const {
  std::lock_guard lock(mutex_);
  return last_ms_;
}

void gpu_timer::release() {
  for (auto &s : slots_) {
    for (ID3D11Query **q : {&s.disjoint, &s.start, &s.stop})
      if (*q != nullptr) {
        (*q)->Release();
        *q = nullptr;
      }
    s.pending = false;
  }
  tried_ = ok_ = false;
  current_ = nullptr;
}

}  // namespace akfsr::d3d
