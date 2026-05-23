#pragma once

#include "demo_common.hpp"

#include <string>

namespace demo::runtime::dx12 {

struct ValidationResult {
    bool passed = true;
    std::string reason;
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

    void clear_state() const;
    bool pump_messages() const;

private:
    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace demo::runtime::dx12
