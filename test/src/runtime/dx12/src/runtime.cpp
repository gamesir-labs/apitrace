#include "runtime/dx12/runtime.hpp"

namespace demo::runtime::dx12 {

Dx12Runtime Dx12Runtime::create(int width, int height, const char *class_name, const char *window_title)
{
    Dx12Runtime runtime;
    runtime.hwnd_ = demo::create_window(class_name, window_title, width, height);
    runtime.width_ = width;
    runtime.height_ = height;
    return runtime;
}

Dx12Runtime::~Dx12Runtime()
{
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Dx12Runtime::clear_state() const
{
    // TODO: reset D3D12 command state once the runtime owns a device and queue.
}

bool Dx12Runtime::pump_messages() const
{
    return demo::pump_messages();
}

} // namespace demo::runtime::dx12
