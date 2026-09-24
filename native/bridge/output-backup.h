#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <stdexcept>

namespace dspaa {
// One reusable image per camera. The caller validates output and serializes every
// operation on the render thread. This preserves the caller's spatial fallback if
// NGX writes some output pixels and then returns an error.
class OutputBackup {
  public:
    void capture(ID3D11DeviceContext* context, ID3D11Resource* output) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(output->QueryInterface(IID_PPV_ARGS(&texture))))
            throw std::runtime_error("Cannot capture a non-texture fallback output.");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        texture->GetDevice(&device);
        if (image_) {
            D3D11_TEXTURE2D_DESC previous{};
            image_->GetDesc(&previous);
            Microsoft::WRL::ComPtr<ID3D11Device> previousDevice;
            image_->GetDevice(&previousDevice);
            if (previousDevice.Get() != device.Get() || previous.Width != desc.Width ||
                previous.Height != desc.Height || previous.Format != desc.Format)
                image_.Reset();
        }
        if (!image_) {
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = desc.CPUAccessFlags = desc.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &image_)))
                throw std::runtime_error("Cannot allocate the full-resolution fallback backup.");
        }
        context->CopyResource(image_.Get(), output);
    }

    void restore(ID3D11DeviceContext* context, ID3D11Resource* output) const noexcept {
        if (image_)
            context->CopyResource(output, image_.Get());
    }

  private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> image_;
};
} // namespace dspaa
