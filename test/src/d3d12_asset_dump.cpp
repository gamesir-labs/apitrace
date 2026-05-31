#include "demo_common.hpp"

#include <d3d12.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_blob(const std::filesystem::path &path, ID3DBlob *blob)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        const std::string message = "failed to open output asset: " + path.string();
        demo::fail(message.c_str());
    }
    output.write(
        static_cast<const char *>(blob->GetBufferPointer()),
        static_cast<std::streamsize>(blob->GetBufferSize())
    );
    if (!output) {
        const std::string message = "failed to write output asset: " + path.string();
        demo::fail(message.c_str());
    }
}

void write_text(const std::filesystem::path &path, const char *text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        const std::string message = "failed to open output text: " + path.string();
        demo::fail(message.c_str());
    }
    output << text;
    if (!output) {
        const std::string message = "failed to write output text: " + path.string();
        demo::fail(message.c_str());
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: apitrace_d3d12_asset_dump <output-dir>\n");
        return 2;
    }

    const std::filesystem::path output_dir = argv[1];
    std::filesystem::create_directories(output_dir);

    constexpr const char *source = R"(
struct VSOut {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
};

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

float4 ps_main(VSOut input) : SV_TARGET
{
  return input.color;
}
)";

    auto vs = demo::compile_shader(source, "vs_main", "vs_5_0");
    auto ps = demo::compile_shader(source, "ps_main", "ps_5_0");

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc{};
    root_signature_desc.NumParameters = 0;
    root_signature_desc.pParameters = nullptr;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = nullptr;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    demo::ComPtr<ID3DBlob> root_signature;
    demo::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(
        &root_signature_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        root_signature.put(),
        errors.put()
    );
    if (FAILED(hr)) {
        if (errors) {
            std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
        }
        demo::fail("D3D12SerializeRootSignature failed", hr);
    }

    write_blob(output_dir / "fullscreen_triangle.vs.dxbc", vs.get());
    write_blob(output_dir / "fullscreen_triangle.ps.dxbc", ps.get());
    write_blob(output_dir / "fullscreen_triangle.rootsig", root_signature.get());
    write_text(output_dir / "fullscreen_triangle.hlsl", source);

    std::printf(
        "D3D12_ASSET_DUMP_OK vs=%zu ps=%zu rootsig=%zu\n",
        static_cast<std::size_t>(vs->GetBufferSize()),
        static_cast<std::size_t>(ps->GetBufferSize()),
        static_cast<std::size_t>(root_signature->GetBufferSize())
    );
    return 0;
}
