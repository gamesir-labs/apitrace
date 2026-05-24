#include "runtime/dx12/runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace demo::runtime::dx12 {

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

bool use_warp_adapter()
{
    const char *value = std::getenv("APITRACE_D3D12_DRIVER");
    if (!value || !*value) {
        return false;
    }
    if (std::strcmp(value, "hardware") == 0) {
        return false;
    }
    if (std::strcmp(value, "warp") == 0) {
        return true;
    }
    demo::fail("APITRACE_D3D12_DRIVER must be 'hardware' or 'warp'");
    return false;
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC buffer_resource_desc(UINT64 width)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

} // namespace

Dx12Runtime Dx12Runtime::create(int width, int height, const char *class_name, const char *window_title)
{
    Dx12Runtime runtime;
    runtime.hwnd_ = demo::create_window(class_name, window_title, width, height);
    runtime.width_ = width;
    runtime.height_ = height;

    demo::ComPtr<IDXGIFactory4> factory;
    demo::check_hr(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "CreateDXGIFactory1 failed");

    demo::ComPtr<IDXGIAdapter1> hardware_adapter;
    demo::ComPtr<IDXGIAdapter> warp_adapter;
    IDXGIAdapter *adapter = nullptr;
    if (use_warp_adapter()) {
        demo::check_hr(factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.put())), "EnumWarpAdapter failed");
        adapter = warp_adapter.get();
    } else {
        for (UINT index = 0;; ++index) {
            demo::ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, candidate.put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_ADAPTER_DESC1 desc{};
            demo::check_hr(candidate->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1 failed");
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                hardware_adapter = std::move(candidate);
                adapter = hardware_adapter.get();
                break;
            }
        }
    }

    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(runtime.device_.put()));
    if (FAILED(hr) && adapter != nullptr && !use_warp_adapter()) {
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(runtime.device_.put()));
    }
    if (FAILED(hr) && !use_warp_adapter()) {
        demo::check_hr(factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.put())), "EnumWarpAdapter fallback failed");
        hr = D3D12CreateDevice(warp_adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(runtime.device_.put()));
    }
    demo::check_hr(hr, "D3D12CreateDevice failed");

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    demo::check_hr(runtime.device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(runtime.queue_.put())), "CreateCommandQueue failed");

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.Width = static_cast<UINT>(width);
    swap_chain_desc.Height = static_cast<UINT>(height);
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.Stereo = FALSE;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SampleDesc.Quality = 0;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swap_chain_desc.Flags = 0;

    demo::ComPtr<IDXGISwapChain1> swap_chain1;
    demo::check_hr(
        factory->CreateSwapChainForHwnd(
            runtime.queue_.get(),
            runtime.hwnd_,
            &swap_chain_desc,
            nullptr,
            nullptr,
            swap_chain1.put()
        ),
        "CreateSwapChainForHwnd failed"
    );
    demo::check_hr(factory->MakeWindowAssociation(runtime.hwnd_, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation failed");
    demo::check_hr(swap_chain1->QueryInterface(IID_PPV_ARGS(runtime.swap_chain_.put())), "QueryInterface(IDXGISwapChain3) failed");
    runtime.frame_index_ = runtime.swap_chain_->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 2;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    demo::check_hr(runtime.device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(runtime.rtv_heap_.put())), "CreateDescriptorHeap(RTV) failed");
    runtime.rtv_descriptor_size_ = runtime.device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = runtime.rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < 2; ++index) {
        demo::check_hr(runtime.swap_chain_->GetBuffer(index, IID_PPV_ARGS(runtime.back_buffers_[index].put())), "GetBuffer(back buffer) failed");
        runtime.device_->CreateRenderTargetView(runtime.back_buffers_[index].get(), nullptr, rtv_handle);
        rtv_handle.ptr += runtime.rtv_descriptor_size_;
    }

    demo::check_hr(
        runtime.device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(runtime.command_allocator_.put())),
        "CreateCommandAllocator failed"
    );
    demo::check_hr(
        runtime.device_->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            runtime.command_allocator_.get(),
            nullptr,
            IID_PPV_ARGS(runtime.command_list_.put())
        ),
        "CreateCommandList failed"
    );
    demo::check_hr(runtime.command_list_->Close(), "Close(initial command list) failed");

    demo::check_hr(runtime.device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(runtime.fence_.put())), "CreateFence failed");
    runtime.fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!runtime.fence_event_) {
        demo::fail("CreateEventA failed for D3D12 fence");
    }

    D3D12_RESOURCE_DESC back_buffer_desc = runtime.back_buffers_[0]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    runtime.device_->GetCopyableFootprints(
        &back_buffer_desc,
        0,
        1,
        0,
        &footprint,
        &num_rows,
        &row_size,
        &runtime.readback_size_
    );
    runtime.readback_row_pitch_ = footprint.Footprint.RowPitch;
    const D3D12_HEAP_PROPERTIES readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC readback_desc = buffer_resource_desc(runtime.readback_size_);
    demo::check_hr(
        runtime.device_->CreateCommittedResource(
            &readback_heap,
            D3D12_HEAP_FLAG_NONE,
            &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(runtime.readback_buffer_.put())
        ),
        "CreateCommittedResource(readback) failed"
    );

    runtime.viewport_.TopLeftX = 0.0f;
    runtime.viewport_.TopLeftY = 0.0f;
    runtime.viewport_.Width = static_cast<float>(width);
    runtime.viewport_.Height = static_cast<float>(height);
    runtime.viewport_.MinDepth = 0.0f;
    runtime.viewport_.MaxDepth = 1.0f;

    runtime.scissor_rect_.left = 0;
    runtime.scissor_rect_.top = 0;
    runtime.scissor_rect_.right = width;
    runtime.scissor_rect_.bottom = height;

    return runtime;
}

Dx12Runtime::~Dx12Runtime()
{
    clear_state();
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Dx12Runtime::wait_for_gpu()
{
    if (!queue_ || !fence_) {
        return;
    }

    const UINT64 signal_value = ++fence_value_;
    demo::check_hr(queue_->Signal(fence_.get(), signal_value), "ID3D12CommandQueue::Signal failed");
    if (fence_->GetCompletedValue() < signal_value) {
        demo::check_hr(
            fence_->SetEventOnCompletion(signal_value, fence_event_),
            "ID3D12Fence::SetEventOnCompletion failed"
        );
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Runtime::current_back_buffer_rtv() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(frame_index_) * static_cast<SIZE_T>(rtv_descriptor_size_);
    return handle;
}

void Dx12Runtime::begin_frame()
{
    if (frame_open_) {
        demo::fail("Dx12Runtime::begin_frame called while a frame is already open");
    }

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
    demo::check_hr(command_allocator_->Reset(), "ID3D12CommandAllocator::Reset failed");
    demo::check_hr(command_list_->Reset(command_allocator_.get(), nullptr), "ID3D12GraphicsCommandList::Reset failed");
    transition_resource(back_buffers_[frame_index_].get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_rect_);
    frame_open_ = true;
}

void Dx12Runtime::bind_back_buffer() const
{
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = current_back_buffer_rtv();
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
}

void Dx12Runtime::clear_back_buffer(const float clear_color[4]) const
{
    command_list_->ClearRenderTargetView(current_back_buffer_rtv(), clear_color, 0, nullptr);
}

void Dx12Runtime::transition_resource(
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after,
    D3D12_RESOURCE_BARRIER_FLAGS flags
) const
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = flags;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command_list_->ResourceBarrier(1, &barrier);
}

void Dx12Runtime::clear_state()
{
    if (frame_open_) {
        transition_resource(back_buffers_[frame_index_].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        demo::check_hr(command_list_->Close(), "ID3D12GraphicsCommandList::Close(clear_state) failed");
        ID3D12CommandList *lists[] = {command_list_.get()};
        queue_->ExecuteCommandLists(1, lists);
        frame_open_ = false;
    }
    wait_for_gpu();
}

void Dx12Runtime::present()
{
    if (!frame_open_) {
        demo::fail("Dx12Runtime::present called without begin_frame");
    }

    transition_resource(back_buffers_[frame_index_].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    demo::check_hr(command_list_->Close(), "ID3D12GraphicsCommandList::Close failed");
    ID3D12CommandList *lists[] = {command_list_.get()};
    queue_->ExecuteCommandLists(1, lists);
    demo::check_hr(swap_chain_->Present(1, 0), "IDXGISwapChain::Present failed");
    frame_open_ = false;
    wait_for_gpu();
}

ValidationResult Dx12Runtime::present_and_validate(
    const PixelExpectation *expectations,
    std::size_t expectation_count
)
{
    if (!frame_open_) {
        demo::fail("Dx12Runtime::present_and_validate called without begin_frame");
    }

    transition_resource(back_buffers_[frame_index_].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = back_buffers_[frame_index_].get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_RESOURCE_DESC back_buffer_desc = back_buffers_[frame_index_]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_size = 0;
    device_->GetCopyableFootprints(&back_buffer_desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total_size);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback_buffer_.get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    command_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    transition_resource(back_buffers_[frame_index_].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    demo::check_hr(command_list_->Close(), "ID3D12GraphicsCommandList::Close(validate) failed");

    ID3D12CommandList *lists[] = {command_list_.get()};
    queue_->ExecuteCommandLists(1, lists);
    demo::check_hr(swap_chain_->Present(1, 0), "IDXGISwapChain::Present(validate) failed");
    frame_open_ = false;
    wait_for_gpu();
    return validate_pixels(expectations, expectation_count);
}

bool Dx12Runtime::pump_messages() const
{
    return demo::pump_messages();
}

std::vector<std::uint8_t> Dx12Runtime::readback_rgba() const
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4U);
    void *mapped_data = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size_)};
    demo::check_hr(readback_buffer_->Map(0, &read_range, &mapped_data), "ID3D12Resource::Map(readback) failed");

    for (int row = 0; row < height_; ++row) {
        const auto *src = static_cast<const std::uint8_t *>(mapped_data) + static_cast<std::size_t>(row) * readback_row_pitch_;
        auto *dst = pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width_) * 4U;
        std::memcpy(dst, src, static_cast<std::size_t>(width_) * 4U);
    }

    D3D12_RANGE written_range{0, 0};
    readback_buffer_->Unmap(0, &written_range);
    return pixels;
}

ValidationResult Dx12Runtime::validate_pixels(
    const PixelExpectation *expectations,
    std::size_t expectation_count
) const
{
    const std::vector<std::uint8_t> pixels = readback_rgba();
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

} // namespace demo::runtime::dx12
