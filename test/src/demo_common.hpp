#pragma once

#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace demo {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    ComPtr(ComPtr &&other) noexcept : ptr_(other.detach()) {}
    ComPtr &operator=(ComPtr &&other) noexcept
    {
        if (this != &other) {
            reset(other.detach());
        }
        return *this;
    }

    ~ComPtr()
    {
        reset();
    }

    T *get() const noexcept { return ptr_; }
    T *const *get_address_of() const noexcept { return &ptr_; }
    T **get_address_of() noexcept { return &ptr_; }
    T **put() noexcept
    {
        reset();
        return &ptr_;
    }

    T *operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    void reset(T *ptr = nullptr) noexcept
    {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = ptr;
    }

    T *detach() noexcept
    {
        T *tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

private:
    T *ptr_ = nullptr;
};

struct Stopwatch {
    Stopwatch()
    {
        QueryPerformanceFrequency(&frequency_);
        QueryPerformanceCounter(&start_);
    }

    float seconds() const
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<float>(now.QuadPart - start_.QuadPart) /
               static_cast<float>(frequency_.QuadPart);
    }

private:
    LARGE_INTEGER frequency_{};
    LARGE_INTEGER start_{};
};

inline void fail(const char *message, HRESULT hr = S_OK)
{
    if (hr == S_OK) {
        std::fprintf(stderr, "%s\n", message);
    } else {
        std::fprintf(stderr, "%s (0x%08lx)\n", message, static_cast<unsigned long>(hr));
    }
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

inline void check_hr(HRESULT hr, const char *message)
{
    if (FAILED(hr)) {
        fail(message, hr);
    }
}

inline unsigned int read_env_u32(const char *name, unsigned int fallback = 0)
{
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }

    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > std::numeric_limits<unsigned int>::max()) {
        return fallback;
    }
    return static_cast<unsigned int>(parsed);
}

inline bool pump_messages()
{
    MSG msg{};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return true;
}

inline LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
}

inline HWND create_window(const char *class_name, const char *title, int client_width, int client_height)
{
    HINSTANCE instance = GetModuleHandleA(nullptr);

    WNDCLASSA wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = class_name;

    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fail("RegisterClassA failed");
    }

    RECT rect{0, 0, client_width, client_height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExA(
        0,
        class_name,
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!hwnd) {
        fail("CreateWindowExA failed");
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    return hwnd;
}

inline std::string executable_directory()
{
    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        fail("GetModuleFileNameA failed");
    }

    std::string directory(path, length);
    const std::size_t separator = directory.find_last_of("\\/");
    if (separator == std::string::npos) {
        return ".";
    }
    directory.resize(separator);
    return directory;
}

inline std::vector<std::uint8_t> load_installed_asset_bytes(const char *relative_path, std::size_t expected_size = 0)
{
    std::string full_path = executable_directory();
    if (!full_path.empty() && full_path.back() != '\\' && full_path.back() != '/') {
        full_path.push_back('\\');
    }
    full_path.append(relative_path);
    for (char &ch : full_path) {
        if (ch == '/') {
            ch = '\\';
        }
    }

    std::ifstream stream(full_path, std::ios::binary);
    if (!stream) {
        const std::string message = "failed to open installed asset: " + full_path;
        fail(message.c_str());
    }

    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        const std::string message = "failed to stat installed asset: " + full_path;
        fail(message.c_str());
    }
    stream.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            const std::string message = "failed to read installed asset: " + full_path;
            fail(message.c_str());
        }
    }

    if (expected_size != 0 && bytes.size() != expected_size) {
        const std::string message = "installed asset size mismatch: " + full_path;
        fail(message.c_str());
    }

    return bytes;
}

inline std::string load_installed_asset_text(const char *relative_path)
{
    const std::vector<std::uint8_t> bytes = load_installed_asset_bytes(relative_path);
    return std::string(bytes.begin(), bytes.end());
}

template <typename T>
inline std::vector<T> load_installed_numeric_asset(const char *relative_path)
{
    static_assert(std::is_arithmetic_v<T>, "numeric asset loader requires arithmetic types");

    const std::string text = load_installed_asset_text(relative_path);
    std::istringstream stream(text);
    std::vector<T> values;

    T value{};
    while (stream >> value) {
        values.push_back(value);
    }

    if (stream.fail() && !stream.eof()) {
        const std::string message = "failed to parse installed numeric asset: " + std::string(relative_path);
        fail(message.c_str());
    }
    if (values.empty()) {
        const std::string message = "installed numeric asset was empty: " + std::string(relative_path);
        fail(message.c_str());
    }

    return values;
}

using D3DCompileFn = HRESULT (WINAPI *)(
    LPCVOID,
    SIZE_T,
    LPCSTR,
    const D3D_SHADER_MACRO *,
    ID3DInclude *,
    LPCSTR,
    LPCSTR,
    UINT,
    UINT,
    ID3DBlob **,
    ID3DBlob **
);

inline ComPtr<ID3DBlob> compile_shader(const char *source, const char *entry, const char *profile)
{
    static HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    if (!compiler) {
        compiler = LoadLibraryA("d3dcompiler_43.dll");
    }
    if (!compiler) {
        fail("failed to load d3dcompiler");
    }

    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) {
        fail("failed to resolve D3DCompile");
    }

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = compile(
        source,
        std::strlen(source),
        "triangle.hlsl",
        nullptr,
        nullptr,
        entry,
        profile,
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        blob.put(),
        errors.put()
    );

    if (FAILED(hr)) {
        if (errors) {
            std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
        }
        fail("shader compilation failed", hr);
    }

    return blob;
}

inline const char *triangle_shader_source()
{
    return R"(
struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

PSInput vs_main(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 1.0);
    output.color = input.color;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    return float4(input.color, 1.0);
}
)";
}

struct Vertex {
    float position[3];
    float color[3];
};

struct FrameConstants {
    float time;
    float padding[3];
};

inline constexpr Vertex kTriangleVertices[3] = {
    {{0.0f, 0.65f, 0.0f}, {1.0f, 0.2f, 0.2f}},
    {{0.62f, -0.45f, 0.0f}, {0.2f, 1.0f, 0.2f}},
    {{-0.62f, -0.45f, 0.0f}, {0.2f, 0.2f, 1.0f}},
};

} // namespace demo
