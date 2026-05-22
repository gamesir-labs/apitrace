#include "demo_common.hpp"

#include <d3d11.h>
#include <dxgi.h>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr const char *kClassName = "apitrace-triangle-d3d11";
constexpr const char *kWindowTitle = "apitrace triangle d3d11";

const char *vertex_shader_profile(D3D_FEATURE_LEVEL feature_level)
{
    switch (feature_level) {
    case D3D_FEATURE_LEVEL_11_1:
    case D3D_FEATURE_LEVEL_11_0:
        return "vs_5_0";
    case D3D_FEATURE_LEVEL_10_1:
    case D3D_FEATURE_LEVEL_10_0:
        return "vs_4_0";
    case D3D_FEATURE_LEVEL_9_3:
        return "vs_4_0_level_9_3";
    case D3D_FEATURE_LEVEL_9_2:
    case D3D_FEATURE_LEVEL_9_1:
        return "vs_4_0_level_9_1";
    default:
        return "vs_4_0";
    }
}

const char *pixel_shader_profile(D3D_FEATURE_LEVEL feature_level)
{
    switch (feature_level) {
    case D3D_FEATURE_LEVEL_11_1:
    case D3D_FEATURE_LEVEL_11_0:
        return "ps_5_0";
    case D3D_FEATURE_LEVEL_10_1:
    case D3D_FEATURE_LEVEL_10_0:
        return "ps_4_0";
    case D3D_FEATURE_LEVEL_9_3:
        return "ps_4_0_level_9_3";
    case D3D_FEATURE_LEVEL_9_2:
    case D3D_FEATURE_LEVEL_9_1:
        return "ps_4_0_level_9_1";
    default:
        return "ps_4_0";
    }
}

void run()
{
    HWND hwnd = demo::create_window(kClassName, kWindowTitle, kWidth, kHeight);

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    demo::ComPtr<ID3D11Device> device;
    demo::ComPtr<ID3D11DeviceContext> context;
    demo::ComPtr<IDXGISwapChain> swap_chain;

    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    swap_chain_desc.BufferDesc.Width = kWidth;
    swap_chain_desc.BufferDesc.Height = kHeight;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.OutputWindow = hwnd;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        swap_chain.put(),
        device.put(),
        &feature_level,
        context.put()
    );

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &swap_chain_desc,
            swap_chain.put(),
            device.put(),
            &feature_level,
            context.put()
        );
    }
    demo::check_hr(hr, "D3D11CreateDeviceAndSwapChain failed");

    demo::ComPtr<ID3D11Texture2D> back_buffer;
    demo::check_hr(
        swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(back_buffer.put())),
        "GetBuffer failed"
    );

    demo::ComPtr<ID3D11RenderTargetView> rtv;
    demo::check_hr(device->CreateRenderTargetView(back_buffer.get(), nullptr, rtv.put()), "CreateRenderTargetView failed");

    demo::ComPtr<ID3DBlob> vs_blob =
        demo::compile_shader(demo::triangle_shader_source(), "vs_main", vertex_shader_profile(feature_level));
    demo::ComPtr<ID3DBlob> ps_blob =
        demo::compile_shader(demo::triangle_shader_source(), "ps_main", pixel_shader_profile(feature_level));

    static const D3D11_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    demo::ComPtr<ID3D11InputLayout> input_layout;
    demo::check_hr(
        device->CreateInputLayout(
            input_layout_desc,
            ARRAYSIZE(input_layout_desc),
            vs_blob->GetBufferPointer(),
            vs_blob->GetBufferSize(),
            input_layout.put()
        ),
        "CreateInputLayout failed"
    );

    demo::ComPtr<ID3D11VertexShader> vertex_shader;
    demo::check_hr(
        device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, vertex_shader.put()),
        "CreateVertexShader failed"
    );

    demo::ComPtr<ID3D11PixelShader> pixel_shader;
    demo::check_hr(
        device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, pixel_shader.put()),
        "CreatePixelShader failed"
    );

    D3D11_BUFFER_DESC vertex_buffer_desc{};
    vertex_buffer_desc.ByteWidth = sizeof(demo::kTriangleVertices);
    vertex_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    vertex_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vertex_data{};
    vertex_data.pSysMem = demo::kTriangleVertices;

    demo::ComPtr<ID3D11Buffer> vertex_buffer;
    demo::check_hr(device->CreateBuffer(&vertex_buffer_desc, &vertex_data, vertex_buffer.put()), "CreateBuffer(vertex) failed");

    D3D11_BUFFER_DESC constant_buffer_desc{};
    constant_buffer_desc.ByteWidth = sizeof(demo::FrameConstants);
    constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    demo::ComPtr<ID3D11Buffer> constant_buffer;
    demo::check_hr(device->CreateBuffer(&constant_buffer_desc, nullptr, constant_buffer.put()), "CreateBuffer(constant) failed");

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(kWidth);
    viewport.Height = static_cast<float>(kHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    UINT stride = sizeof(demo::Vertex);
    UINT offset = 0;
    demo::Stopwatch clock;
    const unsigned int max_frames = demo::read_env_u32("APITRACE_TRIANGLE_MAX_FRAMES", 0);
    unsigned int frame_count = 0;

    while (demo::pump_messages()) {
        demo::FrameConstants constants{};
        constants.time = clock.seconds();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        demo::check_hr(context->Map(constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map(constant) failed");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(constant_buffer.get(), 0);

        float clear_color[4] = {0.06f, 0.07f, 0.12f, 1.0f};
        ID3D11RenderTargetView *render_target_views[] = {rtv.get()};
        ID3D11Buffer *vertex_buffers[] = {vertex_buffer.get()};
        ID3D11Buffer *constant_buffers[] = {constant_buffer.get()};
        context->OMSetRenderTargets(1, render_target_views, nullptr);
        context->RSSetViewports(1, &viewport);
        context->ClearRenderTargetView(rtv.get(), clear_color);
        context->IASetInputLayout(input_layout.get());
        context->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_shader.get(), nullptr, 0);
        context->PSSetShader(pixel_shader.get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constant_buffers);
        context->PSSetConstantBuffers(0, 1, constant_buffers);
        context->Draw(3, 0);
        demo::check_hr(swap_chain->Present(1, 0), "Present failed");
        ++frame_count;
        if (max_frames != 0 && frame_count >= max_frames) {
            break;
        }
        Sleep(16);
    }
}

} // namespace

int main()
{
    run();
    return 0;
}
