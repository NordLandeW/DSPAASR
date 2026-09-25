#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace dspaa {
// A game's draw predicate must not suppress mandatory input copies while their
// completion fence still advances. Caller already owns the immediate context.
class UnpredicatedCopy {
    ID3D11DeviceContext* context_;
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate_;
    BOOL value_ = FALSE;

  public:
    explicit UnpredicatedCopy(ID3D11DeviceContext* context) : context_(context) {
        context_->GetPredication(&predicate_, &value_);
        if (predicate_)
            context_->SetPredication(nullptr, FALSE);
    }
    ~UnpredicatedCopy() {
        if (predicate_)
            context_->SetPredication(predicate_.Get(), value_);
    }
    UnpredicatedCopy(const UnpredicatedCopy&) = delete;
    UnpredicatedCopy& operator=(const UnpredicatedCopy&) = delete;
};
} // namespace dspaa
