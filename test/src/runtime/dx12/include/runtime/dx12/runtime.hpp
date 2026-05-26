#pragma once

#include "demo_common.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace demo::runtime::dx12 {

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
    bool skipped = false;
    std::string reason;

    ValidationResult() = default;
    ValidationResult(bool passed, std::string reason)
        : passed(passed), reason(std::move(reason))
    {
    }

    static ValidationResult skip(std::string reason)
    {
        ValidationResult result;
        result.passed = false;
        result.skipped = true;
        result.reason = std::move(reason);
        return result;
    }
};

class Dx12Runtime {
public:
    Dx12Runtime() = default;
    Dx12Runtime(const Dx12Runtime &) = delete;
    Dx12Runtime &operator=(const Dx12Runtime &) = delete;
    Dx12Runtime(Dx12Runtime &&) noexcept = default;
    Dx12Runtime &operator=(Dx12Runtime &&) noexcept = default;
    ~Dx12Runtime();

    static Dx12Runtime create(int width, int height, const char *class_name, const char *window_title);

    HWND hwnd() const noexcept { return hwnd_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    ID3D12Device *device() const noexcept { return device_.get(); }
    ID3D12CommandQueue *queue() const noexcept { return queue_.get(); }
    ID3D12CommandAllocator *command_allocator() const noexcept { return command_allocator_.get(); }
    ID3D12GraphicsCommandList *command_list() const noexcept { return command_list_.get(); }
    ID3D12Fence *fence() const noexcept { return fence_.get(); }
    IDXGISwapChain3 *swap_chain() const noexcept { return swap_chain_.get(); }
    ID3D12Resource *back_buffer() const noexcept { return back_buffers_[frame_index_].get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE current_back_buffer_rtv() const;
    D3D12_VIEWPORT viewport() const noexcept { return viewport_; }
    D3D12_RECT scissor_rect() const noexcept { return scissor_rect_; }
    DXGI_FORMAT back_buffer_format() const noexcept { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    UINT frame_index() const noexcept { return frame_index_; }

    void begin_frame();
    void bind_back_buffer() const;
    void clear_back_buffer(const float clear_color[4]) const;
    void transition_resource(
        ID3D12Resource *resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after,
        D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE
    ) const;
    void clear_state();
    void present();
    ValidationResult present_and_validate(
        const PixelExpectation *expectations,
        std::size_t expectation_count
    );
    bool pump_messages() const;

    std::vector<std::uint8_t> readback_rgba() const;
    ValidationResult validate_pixels(
        const PixelExpectation *expectations,
        std::size_t expectation_count
    ) const;

private:
    void wait_for_gpu();
    void copy_current_back_buffer_to_readback();
    void record_current_present_frame(UINT sync_interval, UINT flags) const;

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_rect_{};
    UINT frame_index_ = 0;
    UINT rtv_descriptor_size_ = 0;
    UINT readback_row_pitch_ = 0;
    UINT64 readback_size_ = 0;
    UINT64 fence_value_ = 0;
    bool frame_open_ = false;
    demo::ComPtr<ID3D12Device> device_;
    demo::ComPtr<ID3D12CommandQueue> queue_;
    demo::ComPtr<IDXGISwapChain3> swap_chain_;
    demo::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    demo::ComPtr<ID3D12Resource> back_buffers_[2];
    demo::ComPtr<ID3D12CommandAllocator> command_allocator_;
    demo::ComPtr<ID3D12GraphicsCommandList> command_list_;
    demo::ComPtr<ID3D12Fence> fence_;
    demo::ComPtr<ID3D12Resource> readback_buffer_;
    HANDLE fence_event_ = nullptr;
};

} // namespace demo::runtime::dx12
