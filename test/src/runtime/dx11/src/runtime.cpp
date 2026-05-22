#include "runtime/dx11/runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace demo::runtime::dx11 {

namespace {

std::string format_pixel_mismatch(
    const PixelExpectation &expectation,
    const PixelRgba8 &actual
)
{
    char buffer[256];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s expected rgba(%u,%u,%u,%u) got rgba(%u,%u,%u,%u) at (%u,%u)",
        expectation.label ? expectation.label : "pixel",
        static_cast<unsigned int>(expectation.expected.r),
        static_cast<unsigned int>(expectation.expected.g),
        static_cast<unsigned int>(expectation.expected.b),
        static_cast<unsigned int>(expectation.expected.a),
        static_cast<unsigned int>(actual.r),
        static_cast<unsigned int>(actual.g),
        static_cast<unsigned int>(actual.b),
        static_cast<unsigned int>(actual.a),
        expectation.x,
        expectation.y
    );
    return buffer;
}

bool within_tolerance(std::uint8_t actual, std::uint8_t expected, std::uint8_t tolerance)
{
    const int delta = static_cast<int>(actual) - static_cast<int>(expected);
    return delta >= -static_cast<int>(tolerance) && delta <= static_cast<int>(tolerance);
}

D3D_DRIVER_TYPE resolve_primary_driver_type()
{
    const char *value = std::getenv("APITRACE_D3D11_DRIVER");
    if (!value || !*value) {
        return D3D_DRIVER_TYPE_HARDWARE;
    }

    const std::string_view driver = value;
    if (driver == "hardware") {
        return D3D_DRIVER_TYPE_HARDWARE;
    }
    if (driver == "warp") {
        return D3D_DRIVER_TYPE_WARP;
    }

    demo::fail("APITRACE_D3D11_DRIVER must be 'hardware' or 'warp'");
    return D3D_DRIVER_TYPE_HARDWARE;
}

} // namespace

Dx11Runtime Dx11Runtime::create(int width, int height, const char *class_name, const char *window_title)
{
    Dx11Runtime runtime;
    runtime.hwnd_ = demo::create_window(class_name, window_title, width, height);
    runtime.width_ = width;
    runtime.height_ = height;

    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    swap_chain_desc.BufferDesc.Width = static_cast<UINT>(width);
    swap_chain_desc.BufferDesc.Height = static_cast<UINT>(height);
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.OutputWindow = runtime.hwnd_;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_DRIVER_TYPE primary_driver_type = resolve_primary_driver_type();
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        primary_driver_type,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        runtime.swap_chain_.put(),
        runtime.device_.put(),
        &runtime.feature_level_,
        runtime.context_.put()
    );

    if (FAILED(hr) && primary_driver_type == D3D_DRIVER_TYPE_HARDWARE) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &swap_chain_desc,
            runtime.swap_chain_.put(),
            runtime.device_.put(),
            &runtime.feature_level_,
            runtime.context_.put()
        );
    }
    demo::check_hr(hr, "D3D11CreateDeviceAndSwapChain failed");

    demo::check_hr(
        runtime.swap_chain_->GetBuffer(
            0,
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void **>(runtime.back_buffer_.put())
        ),
        "GetBuffer(back buffer) failed"
    );
    demo::check_hr(
        runtime.device_->CreateRenderTargetView(runtime.back_buffer_.get(), nullptr, runtime.back_buffer_rtv_.put()),
        "CreateRenderTargetView(back buffer) failed"
    );

    runtime.viewport_.Width = static_cast<float>(width);
    runtime.viewport_.Height = static_cast<float>(height);
    runtime.viewport_.MinDepth = 0.0f;
    runtime.viewport_.MaxDepth = 1.0f;
    runtime.viewport_.TopLeftX = 0.0f;
    runtime.viewport_.TopLeftY = 0.0f;

    return runtime;
}

Dx11Runtime::~Dx11Runtime()
{
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Dx11Runtime::bind_back_buffer(ID3D11DepthStencilView *depth_stencil_view) const
{
    ID3D11RenderTargetView *render_targets[] = {back_buffer_rtv_.get()};
    context_->OMSetRenderTargets(1, render_targets, depth_stencil_view);
    context_->RSSetViewports(1, &viewport_);
}

void Dx11Runtime::clear_back_buffer(const float clear_color[4]) const
{
    context_->ClearRenderTargetView(back_buffer_rtv_.get(), clear_color);
}

void Dx11Runtime::clear_state() const
{
    context_->ClearState();
    context_->Flush();
}

void Dx11Runtime::present() const
{
    demo::check_hr(swap_chain_->Present(1, 0), "Present failed");
}

bool Dx11Runtime::pump_messages() const
{
    return demo::pump_messages();
}

std::vector<std::uint8_t> Dx11Runtime::readback_rgba(ID3D11Texture2D *texture) const
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.Usage = D3D11_USAGE_STAGING;

    demo::ComPtr<ID3D11Texture2D> staging;
    demo::check_hr(device_->CreateTexture2D(&staging_desc, nullptr, staging.put()), "CreateTexture2D(staging) failed");
    context_->CopyResource(staging.get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    demo::check_hr(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(staging) failed");

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(desc.Width) * static_cast<std::size_t>(desc.Height) * 4U);
    for (UINT row = 0; row < desc.Height; ++row) {
        const auto *src = static_cast<const std::uint8_t *>(mapped.pData) + row * mapped.RowPitch;
        auto *dst = pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(desc.Width) * 4U;
        std::memcpy(dst, src, static_cast<std::size_t>(desc.Width) * 4U);
    }

    context_->Unmap(staging.get(), 0);
    return pixels;
}

ValidationResult Dx11Runtime::validate_pixels(
    ID3D11Texture2D *texture,
    const PixelExpectation *expectations,
    std::size_t expectation_count
) const
{
    const std::vector<std::uint8_t> pixels = readback_rgba(texture);
    for (std::size_t i = 0; i < expectation_count; ++i) {
        const PixelExpectation &expectation = expectations[i];
        if (expectation.x >= static_cast<unsigned int>(width_) || expectation.y >= static_cast<unsigned int>(height_)) {
            return {false, "validation sample outside render target"};
        }

        const std::size_t index = (static_cast<std::size_t>(expectation.y) * static_cast<std::size_t>(width_) +
                                   static_cast<std::size_t>(expectation.x)) *
                                  4U;
        const PixelRgba8 actual{
            pixels[index + 0],
            pixels[index + 1],
            pixels[index + 2],
            pixels[index + 3],
        };

        if (!within_tolerance(actual.r, expectation.expected.r, expectation.tolerance) ||
            !within_tolerance(actual.g, expectation.expected.g, expectation.tolerance) ||
            !within_tolerance(actual.b, expectation.expected.b, expectation.tolerance) ||
            !within_tolerance(actual.a, expectation.expected.a, expectation.tolerance)) {
            return {false, format_pixel_mismatch(expectation, actual)};
        }
    }

    return {};
}

} // namespace demo::runtime::dx11
