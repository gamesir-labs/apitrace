#include "demo_common.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr UINT kFrameCount = 2;
constexpr const char *kClassName = "apitrace-triangle-d3d12";
constexpr const char *kWindowTitle = "apitrace triangle d3d12";

UINT64 align_to_256(UINT64 value)
{
    return (value + 255ULL) & ~255ULL;
}

struct FrameContext {
    demo::ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence_value = 0;
};

void create_render_targets(
    ID3D12Device *device,
    IDXGISwapChain3 *swap_chain,
    ID3D12DescriptorHeap *rtv_heap,
    UINT rtv_descriptor_size,
    demo::ComPtr<ID3D12Resource> render_targets[kFrameCount]
)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        demo::check_hr(
            swap_chain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void **>(render_targets[i].put())),
            "GetBuffer(back buffer) failed"
        );
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_handle;
        handle.ptr += static_cast<SIZE_T>(i) * static_cast<SIZE_T>(rtv_descriptor_size);
        device->CreateRenderTargetView(render_targets[i].get(), nullptr, handle);
    }
}

void run()
{
    HWND hwnd = demo::create_window(kClassName, kWindowTitle, kWidth, kHeight);

    demo::ComPtr<ID3D12Device> device;
    demo::check_hr(
        D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(device.put())),
        "D3D12CreateDevice failed"
    );

    demo::ComPtr<IDXGIFactory4> factory;
    demo::check_hr(
        CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(factory.put())),
        "CreateDXGIFactory1 failed"
    );
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    demo::ComPtr<ID3D12CommandQueue> command_queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    demo::check_hr(
        device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(command_queue.put())),
        "CreateCommandQueue failed"
    );

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.BufferCount = kFrameCount;
    swap_chain_desc.Width = kWidth;
    swap_chain_desc.Height = kHeight;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    demo::ComPtr<IDXGISwapChain1> swap_chain1;
    demo::check_hr(
        factory->CreateSwapChainForHwnd(command_queue.get(), hwnd, &swap_chain_desc, nullptr, nullptr, swap_chain1.put()),
        "CreateSwapChainForHwnd failed"
    );

    demo::ComPtr<IDXGISwapChain3> swap_chain;
    demo::check_hr(
        swap_chain1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(swap_chain.put())),
        "QueryInterface(IDXGISwapChain3) failed"
    );

    demo::ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = kFrameCount;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    demo::check_hr(
        device->CreateDescriptorHeap(&rtv_heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(rtv_heap.put())),
        "CreateDescriptorHeap(RTV) failed"
    );

    UINT rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    demo::ComPtr<ID3D12Resource> render_targets[kFrameCount];
    create_render_targets(device.get(), swap_chain.get(), rtv_heap.get(), rtv_descriptor_size, render_targets);

    demo::ComPtr<ID3D12RootSignature> root_signature;
    D3D12_ROOT_PARAMETER root_parameter{};
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameter.Descriptor.ShaderRegister = 0;
    root_parameter.Descriptor.RegisterSpace = 0;

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc{};
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_parameter;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    demo::ComPtr<ID3DBlob> root_signature_blob;
    demo::ComPtr<ID3DBlob> root_signature_error;
    demo::check_hr(
        D3D12SerializeRootSignature(
            &root_signature_desc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            root_signature_blob.put(),
            root_signature_error.put()
        ),
        "D3D12SerializeRootSignature failed"
    );
    demo::check_hr(
        device->CreateRootSignature(
            0,
            root_signature_blob->GetBufferPointer(),
            root_signature_blob->GetBufferSize(),
            __uuidof(ID3D12RootSignature),
            reinterpret_cast<void **>(root_signature.put())
        ),
        "CreateRootSignature failed"
    );

    demo::ComPtr<ID3DBlob> vs_blob = demo::compile_shader(demo::triangle_shader_source(), "vs_main", "vs_5_0");
    demo::ComPtr<ID3DBlob> ps_blob = demo::compile_shader(demo::triangle_shader_source(), "ps_main", "ps_5_0");

    static const D3D12_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_signature.get();
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    pso_desc.BlendState = D3D12_BLEND_DESC{};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState = D3D12_RASTERIZER_DESC{};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState = D3D12_DEPTH_STENCIL_DESC{};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.InputLayout = {input_layout_desc, ARRAYSIZE(input_layout_desc)};

    demo::ComPtr<ID3D12PipelineState> pipeline_state;
    demo::check_hr(
        device->CreateGraphicsPipelineState(
            &pso_desc,
            __uuidof(ID3D12PipelineState),
            reinterpret_cast<void **>(pipeline_state.put())
        ),
        "CreateGraphicsPipelineState failed"
    );

    demo::ComPtr<ID3D12CommandAllocator> command_allocators[kFrameCount];
    for (UINT i = 0; i < kFrameCount; ++i) {
        demo::check_hr(
            device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator),
                reinterpret_cast<void **>(command_allocators[i].put())
            ),
            "CreateCommandAllocator failed"
        );
    }

    demo::ComPtr<ID3D12GraphicsCommandList> command_list;
    demo::check_hr(
        device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocators[0].get(),
            pipeline_state.get(),
            __uuidof(ID3D12GraphicsCommandList),
            reinterpret_cast<void **>(command_list.put())
        ),
        "CreateCommandList failed"
    );
    demo::check_hr(command_list->Close(), "Close(initial command list) failed");

    const UINT64 vertex_buffer_size = sizeof(demo::kTriangleVertices);
    const UINT64 constant_buffer_size = align_to_256(sizeof(demo::FrameConstants));

    demo::ComPtr<ID3D12Resource> vertex_buffer;
    demo::ComPtr<ID3D12Resource> constant_buffer;

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vertex_buffer_desc{};
    vertex_buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertex_buffer_desc.Width = vertex_buffer_size;
    vertex_buffer_desc.Height = 1;
    vertex_buffer_desc.DepthOrArraySize = 1;
    vertex_buffer_desc.MipLevels = 1;
    vertex_buffer_desc.SampleDesc.Count = 1;
    vertex_buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    demo::check_hr(
        device->CreateCommittedResource(
            &upload_heap,
            D3D12_HEAP_FLAG_NONE,
            &vertex_buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            __uuidof(ID3D12Resource),
            reinterpret_cast<void **>(vertex_buffer.put())
        ),
        "CreateCommittedResource(vertex) failed"
    );

    void *vertex_map = nullptr;
    demo::check_hr(vertex_buffer->Map(0, nullptr, &vertex_map), "Map(vertex) failed");
    std::memcpy(vertex_map, demo::kTriangleVertices, sizeof(demo::kTriangleVertices));
    vertex_buffer->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC constant_buffer_desc = vertex_buffer_desc;
    constant_buffer_desc.Width = constant_buffer_size;

    demo::check_hr(
        device->CreateCommittedResource(
            &upload_heap,
            D3D12_HEAP_FLAG_NONE,
            &constant_buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            __uuidof(ID3D12Resource),
            reinterpret_cast<void **>(constant_buffer.put())
        ),
        "CreateCommittedResource(constant) failed"
    );

    FrameContext frame_context[kFrameCount];
    for (UINT i = 0; i < kFrameCount; ++i) {
        demo::check_hr(
            device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator),
                reinterpret_cast<void **>(frame_context[i].allocator.put())
            ),
            "CreateCommandAllocator failed"
        );
    }

    demo::ComPtr<ID3D12Fence> fence;
    demo::check_hr(
        device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            __uuidof(ID3D12Fence),
            reinterpret_cast<void **>(fence.put())
        ),
        "CreateFence failed"
    );

    HANDLE fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        demo::fail("CreateEventA failed");
    }

    demo::FrameConstants *constant_data = nullptr;
    demo::check_hr(constant_buffer->Map(0, nullptr, reinterpret_cast<void **>(&constant_data)), "Map(constant) failed");

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
    vertex_buffer_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
    vertex_buffer_view.SizeInBytes = static_cast<UINT>(vertex_buffer_size);
    vertex_buffer_view.StrideInBytes = sizeof(demo::Vertex);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(kWidth);
    viewport.Height = static_cast<float>(kHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor{};
    scissor.right = kWidth;
    scissor.bottom = kHeight;

    demo::Stopwatch clock;
    UINT frame_index = swap_chain->GetCurrentBackBufferIndex();
    UINT64 fence_value = 1;

    while (demo::pump_messages()) {
        frame_index = swap_chain->GetCurrentBackBufferIndex();
        FrameContext &frame = frame_context[frame_index];

        if (fence->GetCompletedValue() < frame.fence_value) {
            demo::check_hr(fence->SetEventOnCompletion(frame.fence_value, fence_event), "SetEventOnCompletion failed");
            WaitForSingleObject(fence_event, INFINITE);
        }

        demo::check_hr(frame.allocator->Reset(), "CommandAllocator::Reset failed");
        demo::check_hr(command_list->Reset(frame.allocator.get(), pipeline_state.get()), "CommandList::Reset failed");
        command_list->SetGraphicsRootSignature(root_signature.get());
        command_list->RSSetViewports(1, &viewport);
        command_list->RSSetScissorRects(1, &scissor);
        command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list->IASetVertexBuffers(0, 1, &vertex_buffer_view);

        demo::FrameConstants constants{};
        constants.time = clock.seconds();
        *constant_data = constants;
        command_list->SetGraphicsRootConstantBufferView(0, constant_buffer->GetGPUVirtualAddress());

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = render_targets[frame_index].get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv_handle.ptr += static_cast<SIZE_T>(frame_index) * static_cast<SIZE_T>(rtv_descriptor_size);
        float clear_color[4] = {0.04f, 0.05f, 0.10f, 1.0f};
        command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
        command_list->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);
        command_list->DrawInstanced(3, 1, 0, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list->ResourceBarrier(1, &barrier);

        demo::check_hr(command_list->Close(), "CommandList::Close failed");

        ID3D12CommandList *lists[] = {command_list.get()};
        command_queue->ExecuteCommandLists(1, lists);
        demo::check_hr(swap_chain->Present(1, 0), "Present failed");

        demo::check_hr(command_queue->Signal(fence.get(), fence_value), "Signal failed");
        frame.fence_value = fence_value;
        ++fence_value;

        Sleep(16);
    }

    if (constant_data) {
        constant_buffer->Unmap(0, nullptr);
    }
    CloseHandle(fence_event);
}

} // namespace

int main()
{
    run();
    return 0;
}
