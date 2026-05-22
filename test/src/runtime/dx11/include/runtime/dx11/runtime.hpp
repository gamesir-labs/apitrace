#pragma once

#include "demo_common.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace demo::runtime::dx11 {

struct PixelRgba8 {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

struct PixelExpectation {
    const char *label;
    unsigned int x;
    unsigned int y;
    PixelRgba8 expected;
    std::uint8_t tolerance;
};

struct ValidationResult {
    bool passed = true;
    std::string reason;
};

class Dx11Runtime {
public:
    Dx11Runtime() = default;
    Dx11Runtime(const Dx11Runtime &) = delete;
    Dx11Runtime &operator=(const Dx11Runtime &) = delete;
    Dx11Runtime(Dx11Runtime &&) noexcept = default;
    Dx11Runtime &operator=(Dx11Runtime &&) noexcept = default;
    ~Dx11Runtime();

    static Dx11Runtime create(int width, int height, const char *class_name, const char *window_title);

    HWND hwnd() const noexcept { return hwnd_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    D3D_FEATURE_LEVEL feature_level() const noexcept { return feature_level_; }

    ID3D11Device *device() const noexcept { return device_.get(); }
    ID3D11DeviceContext *context() const noexcept { return context_.get(); }
    IDXGISwapChain *swap_chain() const noexcept { return swap_chain_.get(); }
    ID3D11Texture2D *back_buffer() const noexcept { return back_buffer_.get(); }
    ID3D11RenderTargetView *back_buffer_rtv() const noexcept { return back_buffer_rtv_.get(); }

    void bind_back_buffer(ID3D11DepthStencilView *depth_stencil_view = nullptr) const;
    void clear_back_buffer(const float clear_color[4]) const;
    void clear_state() const;
    void present() const;
    bool pump_messages() const;

    std::vector<std::uint8_t> readback_rgba(ID3D11Texture2D *texture) const;
    ValidationResult validate_pixels(
        ID3D11Texture2D *texture,
        const PixelExpectation *expectations,
        std::size_t expectation_count
    ) const;

private:
    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_11_0;
    D3D11_VIEWPORT viewport_{};
    demo::ComPtr<ID3D11Device> device_;
    demo::ComPtr<ID3D11DeviceContext> context_;
    demo::ComPtr<IDXGISwapChain> swap_chain_;
    demo::ComPtr<ID3D11Texture2D> back_buffer_;
    demo::ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;
};

} // namespace demo::runtime::dx11
