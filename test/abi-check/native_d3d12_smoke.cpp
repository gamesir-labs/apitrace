// Minimal D3D12 ABI smoke test that runs against the DXMT native dylibs.
//
// Goal: prove that the vendored mingw-w64 standalone headers under
// third_party/d3d-headers/ are ABI-compatible with the DXMT-built
// d3d12.dylib / dxgi.dylib / winemetal.dylib. Loads the libraries via dlopen,
// resolves D3D12CreateDevice and CreateDXGIFactory1, instantiates a device,
// and queries one of the larger D3D12_FEATURE structs whose size is part of
// the ABI contract.
//
// Exit code 0 + stdout containing "ABI_SMOKE_OK" → success.

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace {

template <typename T>
T resolve(void *handle, const char *name)
{
    void *sym = dlsym(handle, name);
    if (!sym) {
        std::fprintf(stderr, "[abi-smoke] dlsym(%s) failed: %s\n", name, dlerror());
    }
    return reinterpret_cast<T>(sym);
}

void *load(const char *path)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        std::fprintf(stderr, "[abi-smoke] dlopen(%s) failed: %s\n", path, dlerror());
    }
    return h;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <dxmt-native-build-dir>\n", argv[0]);
        return 2;
    }

    const std::string root = argv[1];
    const std::string winemetal_path = root + "/src/nativemetal/winemetal.dylib";
    const std::string dxgi_path      = root + "/src/dxgi/dxgi.dylib";
    const std::string d3d12_path     = root + "/src/d3d12/d3d12.dylib";

    void *h_winemetal = load(winemetal_path.c_str());
    if (!h_winemetal) return 3;
    void *h_dxgi = load(dxgi_path.c_str());
    if (!h_dxgi) return 4;
    void *h_d3d12 = load(d3d12_path.c_str());
    if (!h_d3d12) return 5;

    using PFN_D3D12CreateDevice_t = HRESULT (WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    using PFN_CreateDXGIFactory1_t = HRESULT (WINAPI *)(REFIID, void **);

    auto p_D3D12CreateDevice  = resolve<PFN_D3D12CreateDevice_t>(h_d3d12, "D3D12CreateDevice");
    auto p_CreateDXGIFactory1 = resolve<PFN_CreateDXGIFactory1_t>(h_dxgi, "CreateDXGIFactory1");

    if (!p_D3D12CreateDevice || !p_CreateDXGIFactory1) {
        std::fprintf(stderr, "[abi-smoke] missing required exports\n");
        return 6;
    }

#ifdef __APPLE__
    void *h_metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
    if (h_metal) {
        using PFN_void_ptr = void *(*)();
        auto p_MTLCopyAllDevices = resolve<PFN_void_ptr>(h_metal, "MTLCopyAllDevices");
        auto p_MTLCreateSystemDefaultDevice = resolve<PFN_void_ptr>(h_metal, "MTLCreateSystemDefaultDevice");
        void *metal_devices = p_MTLCopyAllDevices ? p_MTLCopyAllDevices() : nullptr;
        void *default_device = p_MTLCreateSystemDefaultDevice ? p_MTLCreateSystemDefaultDevice() : nullptr;
        std::fprintf(stderr, "[abi-smoke] MTLCopyAllDevices=%p MTLCreateSystemDefaultDevice=%p\n",
                     metal_devices, default_device);
    } else {
        std::fprintf(stderr, "[abi-smoke] dlopen(Metal.framework) failed: %s\n", dlerror());
    }
    using PFN_WMTCopyAllDevices_t = void *(*)();
    using PFN_NSArrayCount_t = std::uint64_t (*)(void *);
    auto p_WMTCopyAllDevices = resolve<PFN_WMTCopyAllDevices_t>(h_winemetal, "WMTCopyAllDevices");
    auto p_NSArray_count = resolve<PFN_NSArrayCount_t>(h_winemetal, "NSArray_count");
    void *wmt_devices = p_WMTCopyAllDevices ? p_WMTCopyAllDevices() : nullptr;
    std::uint64_t wmt_count = (p_NSArray_count && wmt_devices) ? p_NSArray_count(wmt_devices) : 0;
    std::fprintf(stderr, "[abi-smoke] WMTCopyAllDevices=%p NSArray_count=%llu\n",
                 wmt_devices, static_cast<unsigned long long>(wmt_count));
#endif

    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = p_CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void **>(&factory));
    if (hr != 0 || !factory) {
        std::fprintf(stderr, "[abi-smoke] CreateDXGIFactory1 failed: 0x%08x\n", static_cast<unsigned>(hr));
        return 7;
    }

    IDXGIAdapter1 *probe_adapter = nullptr;
    HRESULT hr_probe = factory->EnumAdapters1(0, &probe_adapter);
    std::fprintf(stderr, "[abi-smoke] factory->EnumAdapters1(0) hr=0x%08x adapter=%p\n",
                 static_cast<unsigned>(hr_probe), static_cast<void *>(probe_adapter));
    if (probe_adapter) {
        DXGI_ADAPTER_DESC1 desc = {};
        probe_adapter->GetDesc1(&desc);
        std::fprintf(stderr, "[abi-smoke] adapter desc VendorId=0x%x DeviceId=0x%x\n",
                     desc.VendorId, desc.DeviceId);
        probe_adapter->Release();
    }
    if (hr_probe == DXGI_ERROR_NOT_FOUND) {
        std::printf("ABI_SMOKE_SKIP no Metal-backed DXGI adapter is available in this process\n");
        factory->Release();
        return 0;
    }

    ID3D12Device *device = nullptr;
    hr = p_D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, reinterpret_cast<void **>(&device));
    if (hr != 0 || !device) {
        std::fprintf(stderr, "[abi-smoke] D3D12CreateDevice failed: 0x%08x\n", static_cast<unsigned>(hr));
        factory->Release();
        return 8;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, static_cast<UINT>(sizeof(options)));
    if (hr != 0) {
        std::fprintf(stderr, "[abi-smoke] CheckFeatureSupport(D3D12_OPTIONS) failed: 0x%08x sizeof=%zu\n",
                     static_cast<unsigned>(hr), sizeof(options));
        device->Release();
        factory->Release();
        return 9;
    }

    std::printf("ABI_SMOKE_OK device=%p factory=%p sizeof(D3D12_OPTIONS)=%zu tiledTier=%u\n",
                static_cast<void *>(device), static_cast<void *>(factory),
                sizeof(options), static_cast<unsigned>(options.TiledResourcesTier));

    device->Release();
    factory->Release();
    return 0;
}
