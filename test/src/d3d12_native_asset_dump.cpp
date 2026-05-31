#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <dxc/dxcapi.h>

namespace {

template <typename T>
class ComPtr {
public:
  ComPtr() = default;
  ComPtr(const ComPtr &) = delete;
  ComPtr &operator=(const ComPtr &) = delete;
  ComPtr(ComPtr &&other) noexcept
      : ptr_(other.ptr_)
  {
    other.ptr_ = nullptr;
  }
  ComPtr &operator=(ComPtr &&other) noexcept
  {
    if (this != &other) {
      reset(other.ptr_);
      other.ptr_ = nullptr;
    }
    return *this;
  }
  ~ComPtr() { reset(); }

  T *get() const noexcept { return ptr_; }
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

private:
  T *ptr_ = nullptr;
};

[[noreturn]] void fail(const std::string &message)
{
  std::fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void check_hr(HRESULT hr, const char *message)
{
  if (FAILED(hr)) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s (0x%08x)", message, static_cast<unsigned int>(hr));
    fail(buffer);
  }
}

std::wstring widen_ascii(const char *text)
{
  std::wstring wide;
  while (text && *text) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
  }
  return wide;
}

void write_blob(const std::filesystem::path &path, IDxcBlob *blob)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    fail("failed to open output asset: " + path.string());
  }
  output.write(
      static_cast<const char *>(blob->GetBufferPointer()),
      static_cast<std::streamsize>(blob->GetBufferSize()));
  if (!output) {
    fail("failed to write output asset: " + path.string());
  }
}

void write_text(const std::filesystem::path &path, const char *text)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    fail("failed to open output text: " + path.string());
  }
  output << text;
  if (!output) {
    fail("failed to write output text: " + path.string());
  }
}

using DxcCreateInstanceProcLocal = HRESULT (*)(REFCLSID, REFIID, LPVOID *);

ComPtr<IDxcBlob> compile_output(
    DxcCreateInstanceProcLocal create_instance,
    const char *source,
    const char *entry,
    const char *profile,
    DXC_OUT_KIND output_kind)
{
  ComPtr<IDxcUtils> utils;
  ComPtr<IDxcCompiler3> compiler;
  check_hr(create_instance(CLSID_DxcUtils, IID_PPV_ARGS(utils.put())), "DxcCreateInstance(CLSID_DxcUtils) failed");
  check_hr(create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())), "DxcCreateInstance(CLSID_DxcCompiler) failed");

  ComPtr<IDxcIncludeHandler> include_handler;
  check_hr(utils->CreateDefaultIncludeHandler(include_handler.put()), "CreateDefaultIncludeHandler failed");

  const std::wstring wide_entry = widen_ascii(entry);
  const std::wstring wide_profile = widen_ascii(profile);
  const wchar_t *arguments[] = {
      L"-E",
      wide_entry.c_str(),
      L"-T",
      wide_profile.c_str(),
      L"-HV",
      L"2021",
      L"-O0",
  };

  DxcBuffer buffer{};
  buffer.Ptr = source;
  buffer.Size = std::strlen(source);
  buffer.Encoding = DXC_CP_UTF8;

  ComPtr<IDxcResult> result;
  check_hr(
      compiler->Compile(
          &buffer,
          const_cast<LPCWSTR *>(arguments),
          static_cast<UINT32>(std::size(arguments)),
          include_handler.get(),
          IID_PPV_ARGS(result.put())),
      "IDxcCompiler3::Compile failed");

  HRESULT status = E_FAIL;
  check_hr(result->GetStatus(&status), "IDxcResult::GetStatus failed");
  if (FAILED(status)) {
    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.put()), nullptr)) && errors) {
      std::fprintf(stderr, "%s\n", errors->GetStringPointer());
    }
    check_hr(status, "shader compilation failed");
  }

  ComPtr<IDxcBlob> blob;
  HRESULT output_hr = result->GetOutput(output_kind, IID_PPV_ARGS(blob.put()), nullptr);
  if (FAILED(output_hr) && output_kind == DXC_OUT_ROOT_SIGNATURE) {
    output_hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(blob.put()), nullptr);
    if (FAILED(output_hr)) {
      output_hr = result->GetResult(blob.put());
    }
  }
  check_hr(output_hr, "IDxcResult::GetOutput failed");
  if (!blob || blob->GetBufferSize() == 0) {
    fail("DXC output blob was empty");
  }
  return blob;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 3) {
    std::fprintf(stderr, "usage: apitrace_d3d12_native_asset_dump <libdxcompiler.dylib> <output-dir>\n");
    return 2;
  }

  const std::filesystem::path dxcompiler_path = argv[1];
  const std::filesystem::path output_dir = argv[2];
  std::filesystem::create_directories(output_dir);

  void *module = dlopen(dxcompiler_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!module) {
    fail(std::string("failed to load libdxcompiler: ") + dlerror());
  }
  auto create_instance = reinterpret_cast<DxcCreateInstanceProcLocal>(dlsym(module, "DxcCreateInstance"));
  if (!create_instance) {
    fail("failed to resolve DxcCreateInstance");
  }

  constexpr const char *source = R"HLSL(
#define RootSig "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)"

struct VSOut {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
};

[RootSignature(RootSig)]
VSOut vs_main(uint vertex_id : SV_VertexID)
{
  float2 positions[3] = {
    float2(-1.0, -1.0),
    float2(-1.0,  3.0),
    float2( 3.0, -1.0),
  };
  VSOut output;
  output.position = float4(positions[vertex_id], 0.0, 1.0);
  output.color = float4(1.0, 1.0, 1.0, 1.0);
  return output;
}

[RootSignature(RootSig)]
float4 ps_main(VSOut input) : SV_TARGET
{
  return input.color;
}

)HLSL";

  auto vs = compile_output(create_instance, source, "vs_main", "vs_6_0", DXC_OUT_OBJECT);
  auto ps = compile_output(create_instance, source, "ps_main", "ps_6_0", DXC_OUT_OBJECT);
  auto root_signature = compile_output(create_instance, source, "RootSig", "rootsig_1_1", DXC_OUT_ROOT_SIGNATURE);

  write_blob(output_dir / "fullscreen_triangle.vs.dxil", vs.get());
  write_blob(output_dir / "fullscreen_triangle.ps.dxil", ps.get());
  write_blob(output_dir / "fullscreen_triangle.rootsig", root_signature.get());
  write_text(output_dir / "fullscreen_triangle.hlsl", source);

  std::printf(
      "D3D12_NATIVE_ASSET_DUMP_OK vs=%zu ps=%zu rootsig=%zu\n",
      static_cast<std::size_t>(vs->GetBufferSize()),
      static_cast<std::size_t>(ps->GetBufferSize()),
      static_cast<std::size_t>(root_signature->GetBufferSize()));
  return 0;
}
