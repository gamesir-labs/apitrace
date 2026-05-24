#include "scenes/dx12/scenes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace demo::scenes::dx12 {

namespace {

using demo::runtime::dx12::Dx12Runtime;
using demo::runtime::dx12::PixelExpectation;
using demo::runtime::dx12::PixelRgba8;
using demo::runtime::dx12::ValidationResult;
using demo::scenes::shared::SceneMatrixEntry;

const SceneMatrixEntry &require_scene_matrix_entry(std::string_view name)
{
    const SceneMatrixEntry *entry = demo::scenes::shared::find_scene_matrix_entry(name);
    if (!entry) {
        demo::fail("dx12 scene missing from shared scene matrix");
    }
    return *entry;
}

struct PosColorVertex {
    float position[3];
    float color[4];
};

struct Pos2Vertex {
    float position[2];
};

struct InstanceVertex {
    float offset[2];
    float color[4];
};

struct PosUvVertex {
    float position[3];
    float uv[2];
};

struct TintConstants {
    float tint[4];
};

struct SmokeTriangleConstants {
    float offset[2];
    float padding[2];
    float tint[4];
};

struct TextureSubresourceData {
    const std::uint8_t *data;
    UINT row_pitch;
};

struct ProgramOptions {
    const D3D12_BLEND_DESC *blend_desc = nullptr;
    const D3D12_RASTERIZER_DESC *rasterizer_desc = nullptr;
    const D3D12_DEPTH_STENCIL_DESC *depth_stencil_desc = nullptr;
    DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
    UINT sample_count = 1;
    UINT sample_quality = 0;
};

PixelRgba8 rgba(float r, float g, float b, float a = 1.0f)
{
    const auto convert = [](float value) -> std::uint8_t {
        const float clamped = std::max(0.0f, std::min(1.0f, value));
        return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
    };

    return PixelRgba8{convert(r), convert(g), convert(b), convert(a)};
}

unsigned int sample_x(const Dx12Runtime &runtime, float fraction)
{
    return static_cast<unsigned int>(std::lround(static_cast<double>(runtime.width()) * fraction));
}

unsigned int sample_y(const Dx12Runtime &runtime, float fraction)
{
    return static_cast<unsigned int>(std::lround(static_cast<double>(runtime.height()) * fraction));
}

float animation_progress(unsigned int frame, unsigned int total_frames, unsigned int settle_limit = 48U)
{
    if (total_frames <= 1U) {
        return 1.0f;
    }

    const unsigned int settle_frames = std::min(total_frames, settle_limit);
    const unsigned int denominator = settle_frames > 1U ? settle_frames - 1U : 1U;
    return static_cast<float>(std::min(frame, denominator)) / static_cast<float>(denominator);
}

float lerp(float from, float to, float t)
{
    return from + (to - from) * t;
}

template <typename Vertex>
std::vector<Vertex> require_struct_count(const char *asset_path, const std::vector<float> &scalars, std::size_t component_count)
{
    if (scalars.size() % component_count != 0) {
        const std::string message = std::string("asset component count mismatch: ") + asset_path;
        demo::fail(message.c_str());
    }
    return std::vector<Vertex>(scalars.size() / component_count);
}

std::vector<PosColorVertex> load_pos_color_vertices_asset(const char *asset_path)
{
    const std::vector<float> scalars = demo::load_installed_numeric_asset<float>(asset_path);
    std::vector<PosColorVertex> vertices = require_struct_count<PosColorVertex>(asset_path, scalars, 7U);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const std::size_t base = index * 7U;
        vertices[index] = {
            {scalars[base + 0U], scalars[base + 1U], scalars[base + 2U]},
            {scalars[base + 3U], scalars[base + 4U], scalars[base + 5U], scalars[base + 6U]},
        };
    }
    return vertices;
}

std::vector<Pos2Vertex> load_pos2_vertices_asset(const char *asset_path)
{
    const std::vector<float> scalars = demo::load_installed_numeric_asset<float>(asset_path);
    std::vector<Pos2Vertex> vertices = require_struct_count<Pos2Vertex>(asset_path, scalars, 2U);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const std::size_t base = index * 2U;
        vertices[index] = {{scalars[base + 0U], scalars[base + 1U]}};
    }
    return vertices;
}

std::vector<InstanceVertex> load_instance_vertices_asset(const char *asset_path)
{
    const std::vector<float> scalars = demo::load_installed_numeric_asset<float>(asset_path);
    std::vector<InstanceVertex> instances = require_struct_count<InstanceVertex>(asset_path, scalars, 6U);
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const std::size_t base = index * 6U;
        instances[index] = {
            {scalars[base + 0U], scalars[base + 1U]},
            {scalars[base + 2U], scalars[base + 3U], scalars[base + 4U], scalars[base + 5U]},
        };
    }
    return instances;
}

std::vector<PosUvVertex> load_pos_uv_vertices_asset(const char *asset_path)
{
    const std::vector<float> scalars = demo::load_installed_numeric_asset<float>(asset_path);
    std::vector<PosUvVertex> vertices = require_struct_count<PosUvVertex>(asset_path, scalars, 5U);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const std::size_t base = index * 5U;
        vertices[index] = {
            {scalars[base + 0U], scalars[base + 1U], scalars[base + 2U]},
            {scalars[base + 3U], scalars[base + 4U]},
        };
    }
    return vertices;
}

template <std::size_t N>
std::array<PosColorVertex, N> translate_vertices(const PosColorVertex (&source)[N], float dx, float dy)
{
    std::array<PosColorVertex, N> translated{};
    for (std::size_t index = 0; index < N; ++index) {
        translated[index] = source[index];
        translated[index].position[0] += dx;
        translated[index].position[1] += dy;
    }
    return translated;
}

std::vector<PosColorVertex> translate_vertices(const std::vector<PosColorVertex> &source, float dx, float dy)
{
    std::vector<PosColorVertex> translated(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        translated[index] = source[index];
        translated[index].position[0] += dx;
        translated[index].position[1] += dy;
    }
    return translated;
}

template <std::size_t N>
std::array<PosUvVertex, N> translate_vertices(const PosUvVertex (&source)[N], float dx, float dy)
{
    std::array<PosUvVertex, N> translated{};
    for (std::size_t index = 0; index < N; ++index) {
        translated[index] = source[index];
        translated[index].position[0] += dx;
        translated[index].position[1] += dy;
    }
    return translated;
}

std::vector<PosUvVertex> translate_vertices(const std::vector<PosUvVertex> &source, float dx, float dy)
{
    std::vector<PosUvVertex> translated(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        translated[index] = source[index];
        translated[index].position[0] += dx;
        translated[index].position[1] += dy;
    }
    return translated;
}

const char *smoke_triangle_shader_source()
{
    return R"(
cbuffer TintData : register(b0)
{
    float2 offset;
    float2 padding;
    float4 tint;
};

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
};

PSInput vs_main(VSInput input)
{
    PSInput output;
    output.position = float4(input.position.xy + offset, input.position.z, 1.0);
    output.color = input.color;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    return input.color * tint;
}
)";
}

const char *instancing_shader_source()
{
    return R"(
struct VSInput
{
    float2 position : POSITION;
    float2 instanceOffset : TEXCOORD0;
    float4 instanceColor : COLOR0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
};

PSInput vs_main(VSInput input)
{
    PSInput output;
    output.position = float4(input.position + input.instanceOffset, 0.0, 1.0);
    output.color = input.instanceColor;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    return input.color;
}
)";
}

const char *textured_shader_source()
{
    return R"(
Texture2D colorTexture : register(t0);
SamplerState colorSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSInput vs_main(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    return colorTexture.Sample(colorSampler, input.uv);
}
)";
}

const char *pos_color_shader_source()
{
    return R"(
struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
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
    return input.color;
}
)";
}

const char *composite_shader_source()
{
    return R"(
Texture2D offscreenPrimary : register(t0);
Texture2D offscreenCopy : register(t1);
SamplerState colorSampler : register(s0);

cbuffer TintData : register(b0)
{
    float4 tint;
};

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSInput vs_main(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    float4 a = offscreenPrimary.Sample(colorSampler, input.uv);
    float4 b = offscreenCopy.Sample(colorSampler, input.uv);
    return lerp(a, b, 0.5) * tint;
}
)";
}

const char *mip_sampling_shader_source()
{
    return R"(
Texture2D fullChainTexture : register(t0);
Texture2D mipSliceTexture : register(t1);
SamplerState colorSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSInput vs_main(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    return output;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    float2 localUv;
    if (input.uv.x < (1.0 / 3.0)) {
        localUv = float2(input.uv.x * 3.0, input.uv.y);
        return fullChainTexture.SampleLevel(colorSampler, localUv, 0.0);
    }
    if (input.uv.x < (2.0 / 3.0)) {
        localUv = float2((input.uv.x - (1.0 / 3.0)) * 3.0, input.uv.y);
        return fullChainTexture.SampleLevel(colorSampler, localUv, 3.0);
    }

    localUv = float2((input.uv.x - (2.0 / 3.0)) * 3.0, input.uv.y);
    return mipSliceTexture.SampleLevel(colorSampler, localUv, 0.0);
}
)";
}

UINT64 align_to(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
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

D3D12_RESOURCE_DESC buffer_desc(UINT64 width)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC texture2d_desc(
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    UINT16 mip_levels = 1,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
    UINT sample_count = 1,
    UINT sample_quality = 0
)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = mip_levels;
    desc.Format = format;
    desc.SampleDesc.Count = sample_count;
    desc.SampleDesc.Quality = sample_quality;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

D3D12_BLEND_DESC default_blend_desc()
{
    D3D12_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    for (auto &target : desc.RenderTarget) {
        target.BlendEnable = FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return desc;
}

D3D12_RASTERIZER_DESC default_rasterizer_desc()
{
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_NONE;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

D3D12_DEPTH_STENCIL_DESC default_depth_stencil_desc()
{
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = FALSE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    return desc;
}

demo::ComPtr<ID3D12DescriptorHeap> create_descriptor_heap(
    ID3D12Device *device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT descriptor_count,
    D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE
)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.NumDescriptors = descriptor_count;
    desc.Type = type;
    desc.Flags = flags;

    demo::ComPtr<ID3D12DescriptorHeap> heap;
    demo::check_hr(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put())), "CreateDescriptorHeap failed");
    return heap;
}

D3D12_CPU_DESCRIPTOR_HANDLE descriptor_cpu_handle(
    ID3D12DescriptorHeap *heap,
    UINT descriptor_size,
    UINT index
)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptor_size) * static_cast<SIZE_T>(index);
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE descriptor_gpu_handle(
    ID3D12DescriptorHeap *heap,
    UINT descriptor_size,
    UINT index
)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(descriptor_size) * static_cast<UINT64>(index);
    return handle;
}

template <typename T>
demo::ComPtr<ID3D12Resource> create_upload_buffer(
    ID3D12Device *device,
    const T *data,
    std::size_t count,
    UINT64 alignment = 0
);

demo::ComPtr<ID3D12Resource> create_texture_resource(
    Dx12Runtime &runtime,
    const D3D12_RESOURCE_DESC &desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *clear_value = nullptr
)
{
    demo::ComPtr<ID3D12Resource> resource;
    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    demo::check_hr(
        runtime.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initial_state,
            clear_value,
            IID_PPV_ARGS(resource.put())
        ),
        "CreateCommittedResource(texture) failed"
    );
    return resource;
}

demo::ComPtr<ID3D12Resource> create_texture_upload_buffer(
    ID3D12Device *device,
    const D3D12_RESOURCE_DESC &texture_desc,
    const std::vector<TextureSubresourceData> &subresources
)
{
    const UINT subresource_count = static_cast<UINT>(subresources.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresource_count);
    std::vector<UINT> row_counts(subresource_count);
    std::vector<UINT64> row_sizes(subresource_count);
    UINT64 upload_size = 0;
    device->GetCopyableFootprints(
        &texture_desc,
        0,
        subresource_count,
        0,
        footprints.data(),
        row_counts.data(),
        row_sizes.data(),
        &upload_size
    );

    auto upload_buffer = create_upload_buffer<std::uint8_t>(
        device,
        nullptr,
        static_cast<std::size_t>(upload_size)
    );

    void *mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    demo::check_hr(upload_buffer->Map(0, &read_range, &mapped), "Map(texture upload buffer) failed");
    auto *upload_bytes = static_cast<std::uint8_t *>(mapped);

    for (UINT subresource = 0; subresource < subresource_count; ++subresource) {
        const auto &footprint = footprints[subresource];
        const TextureSubresourceData &src = subresources[subresource];
        for (UINT row = 0; row < row_counts[subresource]; ++row) {
            std::memcpy(
                upload_bytes + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                src.data + static_cast<std::size_t>(row) * src.row_pitch,
                src.row_pitch
            );
        }
    }

    D3D12_RANGE written_range{0, static_cast<SIZE_T>(upload_size)};
    upload_buffer->Unmap(0, &written_range);
    return std::move(upload_buffer);
}

void copy_texture_upload(
    ID3D12Device *device,
    ID3D12GraphicsCommandList *command_list,
    ID3D12Resource *texture,
    ID3D12Resource *upload_buffer,
    const D3D12_RESOURCE_DESC &texture_desc,
    UINT subresource_count
)
{
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresource_count);
    std::vector<UINT> row_counts(subresource_count);
    std::vector<UINT64> row_sizes(subresource_count);
    UINT64 upload_size = 0;
    device->GetCopyableFootprints(
        &texture_desc,
        0,
        subresource_count,
        0,
        footprints.data(),
        row_counts.data(),
        row_sizes.data(),
        &upload_size
    );

    for (UINT subresource = 0; subresource < subresource_count; ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload_buffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[subresource];

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresource;

        command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
}

template <typename T>
demo::ComPtr<ID3D12Resource> create_upload_buffer(ID3D12Device *device, const T *data, std::size_t count, UINT64 alignment)
{
    UINT64 byte_width = static_cast<UINT64>(sizeof(T) * count);
    if (alignment != 0) {
        byte_width = align_to(byte_width, alignment);
    }

    demo::ComPtr<ID3D12Resource> resource;
    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = buffer_desc(byte_width);
    demo::check_hr(
        device->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(resource.put())
        ),
        "CreateCommittedResource(upload buffer) failed"
    );

    if (data && count != 0) {
        void *mapped = nullptr;
        D3D12_RANGE read_range{0, 0};
        demo::check_hr(resource->Map(0, &read_range, &mapped), "Map(upload buffer) failed");
        std::memcpy(mapped, data, sizeof(T) * count);
        D3D12_RANGE written_range{0, static_cast<SIZE_T>(sizeof(T) * count)};
        resource->Unmap(0, &written_range);
    }

    return resource;
}

template <typename T>
void update_upload_buffer(ID3D12Resource *resource, const T &value)
{
    void *mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    demo::check_hr(resource->Map(0, &read_range, &mapped), "Map(upload constant buffer) failed");
    std::memcpy(mapped, &value, sizeof(T));
    D3D12_RANGE written_range{0, sizeof(T)};
    resource->Unmap(0, &written_range);
}

template <typename T>
void update_upload_buffer_array(ID3D12Resource *resource, const T *values, std::size_t count)
{
    void *mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    demo::check_hr(resource->Map(0, &read_range, &mapped), "Map(upload buffer array) failed");
    std::memcpy(mapped, values, sizeof(T) * count);
    D3D12_RANGE written_range{0, static_cast<SIZE_T>(sizeof(T) * count)};
    resource->Unmap(0, &written_range);
}

demo::ComPtr<ID3D12RootSignature> create_root_signature(
    Dx12Runtime &runtime,
    const D3D12_ROOT_PARAMETER *parameters,
    UINT parameter_count,
    const D3D12_STATIC_SAMPLER_DESC *static_samplers = nullptr,
    UINT static_sampler_count = 0
)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc{};
    root_signature_desc.NumParameters = parameter_count;
    root_signature_desc.pParameters = parameters;
    root_signature_desc.NumStaticSamplers = static_sampler_count;
    root_signature_desc.pStaticSamplers = static_samplers;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    demo::ComPtr<ID3DBlob> serialized;
    demo::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(
        &root_signature_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.put(),
        errors.put()
    );
    if (FAILED(hr)) {
        if (errors) {
            std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
        }
        demo::fail("D3D12SerializeRootSignature failed", hr);
    }

    demo::ComPtr<ID3D12RootSignature> root_signature;
    demo::check_hr(
        runtime.device()->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(root_signature.put())
        ),
        "CreateRootSignature failed"
    );
    return root_signature;
}

struct ShaderProgram {
    demo::ComPtr<ID3D12RootSignature> root_signature;
    demo::ComPtr<ID3D12PipelineState> pipeline_state;
};

ShaderProgram create_program(
    Dx12Runtime &runtime,
    const char *source,
    const D3D12_INPUT_ELEMENT_DESC *input_layout_desc,
    UINT input_layout_count,
    const D3D12_ROOT_PARAMETER *root_parameters,
    UINT root_parameter_count,
    const D3D12_STATIC_SAMPLER_DESC *static_samplers = nullptr,
    UINT static_sampler_count = 0,
    ProgramOptions options = {}
)
{
    ShaderProgram program;
    program.root_signature = create_root_signature(runtime, root_parameters, root_parameter_count, static_samplers, static_sampler_count);

    demo::ComPtr<ID3DBlob> vs_blob = demo::compile_shader(source, "vs_main", "vs_5_0");
    demo::ComPtr<ID3DBlob> ps_blob = demo::compile_shader(source, "ps_main", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = program.root_signature.get();
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    pso_desc.BlendState = options.blend_desc ? *options.blend_desc : default_blend_desc();
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState = options.rasterizer_desc ? *options.rasterizer_desc : default_rasterizer_desc();
    if (options.sample_count > 1) {
        pso_desc.RasterizerState.MultisampleEnable = TRUE;
    }
    pso_desc.DepthStencilState = options.depth_stencil_desc ? *options.depth_stencil_desc : default_depth_stencil_desc();
    pso_desc.InputLayout = {input_layout_desc, input_layout_count};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = runtime.back_buffer_format();
    pso_desc.DSVFormat = options.dsv_format;
    pso_desc.SampleDesc.Count = options.sample_count;
    pso_desc.SampleDesc.Quality = options.sample_quality;

    demo::check_hr(runtime.device()->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(program.pipeline_state.put())), "CreateGraphicsPipelineState failed");
    return program;
}

template <typename RenderFn, typename ValidateFn>
ValidationResult run_scene_frames(
    Dx12Runtime &runtime,
    unsigned int frame_budget,
    RenderFn &&render_frame,
    ValidateFn &&validate_frame
)
{
    const unsigned int frames = frame_budget == 0 ? 1U : frame_budget;
    ValidationResult result{};

    for (unsigned int frame = 0; frame < frames; ++frame) {
        if (!runtime.pump_messages()) {
            return {false, "window closed before validation completed"};
        }

        runtime.begin_frame();
        render_frame(frame, frames);
        if (frame + 1 == frames) {
            const std::vector<PixelExpectation> expectations = validate_frame();
            result = runtime.present_and_validate(expectations.data(), expectations.size());
        } else {
            runtime.present();
            Sleep(16);
        }
    }

    return result;
}

ValidationResult unimplemented_scene(const char *scene_name)
{
    return {false, std::string(scene_name) + ": dx12 scene is reserved but not implemented yet"};
}

ValidationResult run_smoke_triangle(Dx12Runtime &runtime, unsigned int frame_budget)
{
    static const D3D12_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.04f, 0.05f, 0.08f, 1.0f};
    const std::vector<PosColorVertex> vertices = load_pos_color_vertices_asset("assets/dx11/smoke_triangle_vertices.txt");

    D3D12_ROOT_PARAMETER root_parameter{};
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameter.Descriptor.ShaderRegister = 0;
    root_parameter.Descriptor.RegisterSpace = 0;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    const ShaderProgram program = create_program(runtime, smoke_triangle_shader_source(), input_layout_desc, ARRAYSIZE(input_layout_desc), &root_parameter, 1);
    const auto vertex_buffer = create_upload_buffer(runtime.device(), vertices.data(), vertices.size());
    const auto constant_buffer = create_upload_buffer<SmokeTriangleConstants>(runtime.device(), nullptr, 1, 256U);

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
    vertex_buffer_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
    vertex_buffer_view.SizeInBytes = static_cast<UINT>(sizeof(PosColorVertex) * vertices.size());
    vertex_buffer_view.StrideInBytes = sizeof(PosColorVertex);

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            const float t = animation_progress(frame, total_frames);
            const SmokeTriangleConstants constants{
                {lerp(-0.28f, 0.0f, t), lerp(-0.16f, 0.0f, t)},
                {0.0f, 0.0f},
                {
                    lerp(0.45f, 0.90f, t),
                    lerp(0.55f, 0.90f, t),
                    lerp(0.75f, 1.00f, t),
                    1.00f,
                },
            };
            update_upload_buffer(constant_buffer.get(), constants);

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            runtime.command_list()->SetGraphicsRootSignature(program.root_signature.get());
            runtime.command_list()->SetPipelineState(program.pipeline_state.get());
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.command_list()->IASetVertexBuffers(0, 1, &vertex_buffer_view);
            runtime.command_list()->SetGraphicsRootConstantBufferView(0, constant_buffer->GetGPUVirtualAddress());
            runtime.command_list()->DrawInstanced(3, 1, 0, 0);
        },
        [&]() {
            return std::vector<PixelExpectation>{
                {"triangle-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.48f), rgba(0.90f, 0.45f, 0.20f), 18},
                {"background-corner", 16U, 16U, rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
            };
        }
    );
}

ValidationResult run_indexed_instancing(Dx12Runtime &runtime, unsigned int frame_budget)
{
    static const D3D12_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    const float clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    const std::vector<Pos2Vertex> quad_vertices = load_pos2_vertices_asset("assets/dx11/indexed_instancing_vertices.txt");
    const std::vector<std::uint16_t> quad_indices = demo::load_installed_numeric_asset<std::uint16_t>(
        "assets/dx11/indexed_instancing_indices.txt"
    );
    const std::vector<InstanceVertex> instances = load_instance_vertices_asset("assets/dx11/indexed_instancing_instances.txt");

    const ShaderProgram program = create_program(runtime, instancing_shader_source(), input_layout_desc, ARRAYSIZE(input_layout_desc), nullptr, 0);
    const auto vertex_buffer = create_upload_buffer(runtime.device(), quad_vertices.data(), quad_vertices.size());
    const auto index_buffer = create_upload_buffer(runtime.device(), quad_indices.data(), quad_indices.size());
    const auto instance_buffer = create_upload_buffer<InstanceVertex>(runtime.device(), nullptr, instances.size());

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
    vertex_buffer_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
    vertex_buffer_view.SizeInBytes = static_cast<UINT>(sizeof(Pos2Vertex) * quad_vertices.size());
    vertex_buffer_view.StrideInBytes = sizeof(Pos2Vertex);

    D3D12_VERTEX_BUFFER_VIEW instance_buffer_view{};
    instance_buffer_view.BufferLocation = instance_buffer->GetGPUVirtualAddress();
    instance_buffer_view.SizeInBytes = static_cast<UINT>(sizeof(InstanceVertex) * instances.size());
    instance_buffer_view.StrideInBytes = sizeof(InstanceVertex);

    D3D12_INDEX_BUFFER_VIEW index_buffer_view{};
    index_buffer_view.BufferLocation = index_buffer->GetGPUVirtualAddress();
    index_buffer_view.SizeInBytes = static_cast<UINT>(sizeof(std::uint16_t) * quad_indices.size());
    index_buffer_view.Format = DXGI_FORMAT_R16_UINT;

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            const float t = animation_progress(frame, total_frames);
            std::vector<InstanceVertex> animated_instances(instances.size());
            for (std::size_t index = 0; index < animated_instances.size(); ++index) {
                animated_instances[index] = instances[index];
                animated_instances[index].offset[0] = lerp(instances[index].offset[0] * 0.18f, instances[index].offset[0], t);
                animated_instances[index].offset[1] = lerp((static_cast<float>(index) - 1.0f) * 0.14f, 0.0f, t);
                animated_instances[index].color[0] = lerp(0.20f, instances[index].color[0], t);
                animated_instances[index].color[1] = lerp(0.25f, instances[index].color[1], t);
                animated_instances[index].color[2] = lerp(0.30f, instances[index].color[2], t);
                animated_instances[index].color[3] = 1.0f;
            }
            update_upload_buffer_array(instance_buffer.get(), animated_instances.data(), animated_instances.size());

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            D3D12_VERTEX_BUFFER_VIEW views[] = {vertex_buffer_view, instance_buffer_view};
            runtime.command_list()->SetGraphicsRootSignature(program.root_signature.get());
            runtime.command_list()->SetPipelineState(program.pipeline_state.get());
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.command_list()->IASetVertexBuffers(0, 2, views);
            runtime.command_list()->IASetIndexBuffer(&index_buffer_view);
            runtime.command_list()->DrawIndexedInstanced(6, 1, 0, 0, 0);
            runtime.command_list()->DrawIndexedInstanced(6, 2, 0, 0, 1);
        },
        [&]() {
            return std::vector<PixelExpectation>{
                {"instance-left", sample_x(runtime, 0.29f), sample_y(runtime, 0.50f), rgba(0.20f, 0.82f, 0.30f), 18},
                {"instance-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.86f, 0.65f, 0.25f), 18},
                {"instance-right", sample_x(runtime, 0.71f), sample_y(runtime, 0.50f), rgba(0.25f, 0.45f, 0.90f), 18},
                {"background-top", sample_x(runtime, 0.50f), sample_y(runtime, 0.18f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
            };
        }
    );
}

ValidationResult run_textured_quad(Dx12Runtime &runtime, unsigned int frame_budget)
{
    static const PosUvVertex quad_vertices[] = {
        {{-0.82f, 0.82f, 0.0f}, {0.0f, 0.0f}},
        {{0.82f, 0.82f, 0.0f}, {1.0f, 0.0f}},
        {{0.82f, -0.82f, 0.0f}, {1.0f, 1.0f}},
        {{-0.82f, 0.82f, 0.0f}, {0.0f, 0.0f}},
        {{0.82f, -0.82f, 0.0f}, {1.0f, 1.0f}},
        {{-0.82f, -0.82f, 0.0f}, {0.0f, 1.0f}},
    };
    static const D3D12_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.03f, 0.04f, 0.06f, 1.0f};
    const std::vector<std::uint8_t> texture_data =
        demo::load_installed_asset_bytes("assets/dx11/textured_quad_base.rgba", 4U * 4U * 4U);

    D3D12_DESCRIPTOR_RANGE descriptor_range{};
    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range.NumDescriptors = 1;
    descriptor_range.BaseShaderRegister = 0;
    descriptor_range.RegisterSpace = 0;
    descriptor_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER root_parameter{};
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameter.DescriptorTable.NumDescriptorRanges = 1;
    root_parameter.DescriptorTable.pDescriptorRanges = &descriptor_range;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.ShaderRegister = 0;
    sampler_desc.RegisterSpace = 0;
    sampler_desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;

    const ShaderProgram program = create_program(
        runtime,
        textured_shader_source(),
        input_layout_desc,
        ARRAYSIZE(input_layout_desc),
        &root_parameter,
        1,
        &sampler_desc,
        1
    );

    const auto vertex_buffer = create_upload_buffer<PosUvVertex>(runtime.device(), nullptr, ARRAYSIZE(quad_vertices));

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
    vertex_buffer_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
    vertex_buffer_view.SizeInBytes = static_cast<UINT>(sizeof(quad_vertices));
    vertex_buffer_view.StrideInBytes = sizeof(PosUvVertex);

    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = 4;
    texture_desc.Height = 4;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const D3D12_HEAP_PROPERTIES default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    demo::ComPtr<ID3D12Resource> texture;
    demo::check_hr(
        runtime.device()->CreateCommittedResource(
            &default_heap,
            D3D12_HEAP_FLAG_NONE,
            &texture_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(texture.put())
        ),
        "CreateCommittedResource(texture) failed"
    );

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 upload_size = 0;
    runtime.device()->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, &num_rows, &row_size, &upload_size);
    const auto upload_buffer = create_upload_buffer<std::uint8_t>(runtime.device(), nullptr, static_cast<std::size_t>(upload_size));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = 1;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    demo::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    demo::check_hr(runtime.device()->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(descriptor_heap.put())), "CreateDescriptorHeap(SRV) failed");

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    runtime.device()->CreateShaderResourceView(texture.get(), &srv_desc, descriptor_heap->GetCPUDescriptorHandleForHeapStart());

    bool uploaded = false;

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            if (!uploaded) {
                void *mapped = nullptr;
                D3D12_RANGE read_range{0, 0};
                demo::check_hr(upload_buffer->Map(0, &read_range, &mapped), "Map(texture upload) failed");
                auto *upload_bytes = static_cast<std::uint8_t *>(mapped);
                for (UINT row = 0; row < 4; ++row) {
                    std::memcpy(
                        upload_bytes + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                        texture_data.data() + static_cast<std::size_t>(row) * 16U,
                        16U
                    );
                }
                D3D12_RANGE written_range{0, static_cast<SIZE_T>(upload_size)};
                upload_buffer->Unmap(0, &written_range);

                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = upload_buffer.get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = footprint;

                D3D12_TEXTURE_COPY_LOCATION texture_dst{};
                texture_dst.pResource = texture.get();
                texture_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                texture_dst.SubresourceIndex = 0;

                runtime.command_list()->CopyTextureRegion(&texture_dst, 0, 0, 0, &src, nullptr);
                runtime.transition_resource(texture.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                uploaded = true;
            }

            const float t = animation_progress(frame, total_frames);
            const auto animated_vertices = translate_vertices(
                quad_vertices,
                lerp(-0.18f, 0.0f, t),
                lerp(0.10f, 0.0f, t)
            );
            update_upload_buffer_array(vertex_buffer.get(), animated_vertices.data(), animated_vertices.size());

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            ID3D12DescriptorHeap *heaps[] = {descriptor_heap.get()};
            runtime.command_list()->SetDescriptorHeaps(1, heaps);
            runtime.command_list()->SetGraphicsRootSignature(program.root_signature.get());
            runtime.command_list()->SetPipelineState(program.pipeline_state.get());
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.command_list()->IASetVertexBuffers(0, 1, &vertex_buffer_view);
            runtime.command_list()->SetGraphicsRootDescriptorTable(0, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);
        },
        [&]() {
            return std::vector<PixelExpectation>{
                {"quad-top-left", sample_x(runtime, 0.35f), sample_y(runtime, 0.35f), rgba(1.0f, 0.92f, 0.24f), 20},
                {"quad-top-right", sample_x(runtime, 0.65f), sample_y(runtime, 0.35f), rgba(0.31f, 0.78f, 1.0f), 20},
                {"quad-bottom-left", sample_x(runtime, 0.35f), sample_y(runtime, 0.65f), rgba(0.71f, 0.27f, 1.0f), 20},
                {"quad-bottom-right", sample_x(runtime, 0.65f), sample_y(runtime, 0.65f), rgba(1.0f, 1.0f, 1.0f), 20},
            };
        }
    );
}

ValidationResult run_depth_blend_scissor(Dx12Runtime &runtime, unsigned int frame_budget)
{
    static const PosColorVertex blue_quad[] = {
        {{-0.60f, 0.42f, 0.80f}, {0.15f, 0.30f, 0.95f, 1.0f}},
        {{0.60f, 0.42f, 0.80f}, {0.15f, 0.30f, 0.95f, 1.0f}},
        {{0.60f, -0.42f, 0.80f}, {0.15f, 0.30f, 0.95f, 1.0f}},
        {{-0.60f, 0.42f, 0.80f}, {0.15f, 0.30f, 0.95f, 1.0f}},
        {{0.60f, -0.42f, 0.80f}, {0.15f, 0.30f, 0.95f, 1.0f}},
        {{-0.60f, -0.42f, 0.80f}, {0.15f, 0.30f, 0.95f, 1.0f}},
    };
    static const PosColorVertex red_quad[] = {
        {{-0.34f, 0.24f, 0.60f}, {0.95f, 0.20f, 0.20f, 0.50f}},
        {{0.34f, 0.24f, 0.60f}, {0.95f, 0.20f, 0.20f, 0.50f}},
        {{0.34f, -0.24f, 0.60f}, {0.95f, 0.20f, 0.20f, 0.50f}},
        {{-0.34f, 0.24f, 0.60f}, {0.95f, 0.20f, 0.20f, 0.50f}},
        {{0.34f, -0.24f, 0.60f}, {0.95f, 0.20f, 0.20f, 0.50f}},
        {{-0.34f, -0.24f, 0.60f}, {0.95f, 0.20f, 0.20f, 0.50f}},
    };
    static const PosColorVertex green_quad[] = {
        {{-0.30f, 0.18f, 0.90f}, {0.10f, 0.90f, 0.25f, 1.0f}},
        {{0.30f, 0.18f, 0.90f}, {0.10f, 0.90f, 0.25f, 1.0f}},
        {{0.30f, -0.18f, 0.90f}, {0.10f, 0.90f, 0.25f, 1.0f}},
        {{-0.30f, 0.18f, 0.90f}, {0.10f, 0.90f, 0.25f, 1.0f}},
        {{0.30f, -0.18f, 0.90f}, {0.10f, 0.90f, 0.25f, 1.0f}},
        {{-0.30f, -0.18f, 0.90f}, {0.10f, 0.90f, 0.25f, 1.0f}},
    };
    static const D3D12_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.03f, 0.03f, 0.05f, 1.0f};

    D3D12_DEPTH_STENCIL_DESC depth_desc = default_depth_stencil_desc();
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blend_desc = default_blend_desc();
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

    ProgramOptions opaque_options{};
    opaque_options.depth_stencil_desc = &depth_desc;
    opaque_options.dsv_format = DXGI_FORMAT_D32_FLOAT;

    ProgramOptions blend_options = opaque_options;
    blend_options.blend_desc = &blend_desc;

    const ShaderProgram opaque_program = create_program(
        runtime,
        pos_color_shader_source(),
        input_layout_desc,
        ARRAYSIZE(input_layout_desc),
        nullptr,
        0,
        nullptr,
        0,
        opaque_options
    );
    const ShaderProgram blend_program = create_program(
        runtime,
        pos_color_shader_source(),
        input_layout_desc,
        ARRAYSIZE(input_layout_desc),
        nullptr,
        0,
        nullptr,
        0,
        blend_options
    );

    const auto blue_buffer = create_upload_buffer(runtime.device(), blue_quad, ARRAYSIZE(blue_quad));
    const auto red_buffer = create_upload_buffer(runtime.device(), red_quad, ARRAYSIZE(red_quad));
    const auto green_buffer = create_upload_buffer(runtime.device(), green_quad, ARRAYSIZE(green_quad));

    D3D12_VERTEX_BUFFER_VIEW blue_view{};
    blue_view.BufferLocation = blue_buffer->GetGPUVirtualAddress();
    blue_view.SizeInBytes = sizeof(blue_quad);
    blue_view.StrideInBytes = sizeof(PosColorVertex);

    D3D12_VERTEX_BUFFER_VIEW red_view{};
    red_view.BufferLocation = red_buffer->GetGPUVirtualAddress();
    red_view.SizeInBytes = sizeof(red_quad);
    red_view.StrideInBytes = sizeof(PosColorVertex);

    D3D12_VERTEX_BUFFER_VIEW green_view{};
    green_view.BufferLocation = green_buffer->GetGPUVirtualAddress();
    green_view.SizeInBytes = sizeof(green_quad);
    green_view.StrideInBytes = sizeof(PosColorVertex);

    const D3D12_RESOURCE_DESC depth_texture_desc = texture2d_desc(
        static_cast<UINT>(runtime.width()),
        static_cast<UINT>(runtime.height()),
        DXGI_FORMAT_D32_FLOAT,
        1,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
    );
    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;
    depth_clear.DepthStencil.Stencil = 0;
    const auto depth_texture = create_texture_resource(
        runtime,
        depth_texture_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depth_clear
    );

    const auto dsv_heap = create_descriptor_heap(runtime.device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    runtime.device()->CreateDepthStencilView(depth_texture.get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            const float t = animation_progress(frame, total_frames);
            const D3D12_RECT animated_scissor_rect{
                static_cast<LONG>(lerp(static_cast<float>(runtime.width() / 2), static_cast<float>(runtime.width() / 4), t)),
                static_cast<LONG>(lerp(static_cast<float>(runtime.height() / 2), static_cast<float>(runtime.height() / 4), t)),
                static_cast<LONG>(lerp(static_cast<float>(runtime.width() / 2 + 32), static_cast<float>(runtime.width() * 3 / 4), t)),
                static_cast<LONG>(lerp(static_cast<float>(runtime.height() / 2 + 32), static_cast<float>(runtime.height() * 3 / 4), t)),
            };
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = runtime.current_back_buffer_rtv();
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();

            runtime.command_list()->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            runtime.clear_back_buffer(clear_color);
            runtime.command_list()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            runtime.command_list()->RSSetScissorRects(1, &animated_scissor_rect);
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            runtime.command_list()->SetGraphicsRootSignature(opaque_program.root_signature.get());
            runtime.command_list()->SetPipelineState(opaque_program.pipeline_state.get());
            runtime.command_list()->IASetVertexBuffers(0, 1, &blue_view);
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);

            runtime.command_list()->SetGraphicsRootSignature(blend_program.root_signature.get());
            runtime.command_list()->SetPipelineState(blend_program.pipeline_state.get());
            runtime.command_list()->IASetVertexBuffers(0, 1, &red_view);
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);

            runtime.command_list()->SetGraphicsRootSignature(opaque_program.root_signature.get());
            runtime.command_list()->SetPipelineState(opaque_program.pipeline_state.get());
            runtime.command_list()->IASetVertexBuffers(0, 1, &green_view);
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);
        },
        [&]() {
            return std::vector<PixelExpectation>{
                {"depth-blend-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.55f, 0.25f, 0.58f, 0.50f), 28},
                {"depth-blue-only", sample_x(runtime, 0.28f), sample_y(runtime, 0.50f), rgba(0.15f, 0.30f, 0.95f), 20},
                {"scissor-outside", sample_x(runtime, 0.14f), sample_y(runtime, 0.50f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
            };
        }
    );
}

ValidationResult run_offscreen_copy_composite(Dx12Runtime &runtime, unsigned int frame_budget)
{
    static const D3D12_INPUT_ELEMENT_DESC pos_color_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static const D3D12_INPUT_ELEMENT_DESC composite_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.02f, 0.03f, 0.05f, 1.0f};
    const std::vector<PosColorVertex> offscreen_quad =
        load_pos_color_vertices_asset("assets/dx11/offscreen_copy_composite_offscreen_quad.txt");
    const std::vector<PosUvVertex> composite_quad =
        load_pos_uv_vertices_asset("assets/dx11/offscreen_copy_composite_composite_quad.txt");

    D3D12_DESCRIPTOR_RANGE descriptor_range{};
    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range.NumDescriptors = 2;
    descriptor_range.BaseShaderRegister = 0;
    descriptor_range.RegisterSpace = 0;
    descriptor_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER root_parameters[2]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &descriptor_range;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[1].Descriptor.ShaderRegister = 0;
    root_parameters[1].Descriptor.RegisterSpace = 0;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.ShaderRegister = 0;
    sampler_desc.RegisterSpace = 0;
    sampler_desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;

    const ShaderProgram offscreen_program = create_program(
        runtime,
        pos_color_shader_source(),
        pos_color_layout_desc,
        ARRAYSIZE(pos_color_layout_desc),
        nullptr,
        0
    );
    const ShaderProgram composite_program = create_program(
        runtime,
        composite_shader_source(),
        composite_layout_desc,
        ARRAYSIZE(composite_layout_desc),
        root_parameters,
        ARRAYSIZE(root_parameters),
        &sampler_desc,
        1
    );

    const auto offscreen_buffer = create_upload_buffer<PosColorVertex>(runtime.device(), nullptr, offscreen_quad.size());
    const auto composite_buffer = create_upload_buffer(runtime.device(), composite_quad.data(), composite_quad.size());
    const auto constant_buffer = create_upload_buffer<TintConstants>(runtime.device(), nullptr, 1, 256U);

    D3D12_VERTEX_BUFFER_VIEW offscreen_buffer_view{};
    offscreen_buffer_view.BufferLocation = offscreen_buffer->GetGPUVirtualAddress();
    offscreen_buffer_view.SizeInBytes = static_cast<UINT>(sizeof(PosColorVertex) * offscreen_quad.size());
    offscreen_buffer_view.StrideInBytes = sizeof(PosColorVertex);

    D3D12_VERTEX_BUFFER_VIEW composite_buffer_view{};
    composite_buffer_view.BufferLocation = composite_buffer->GetGPUVirtualAddress();
    composite_buffer_view.SizeInBytes = static_cast<UINT>(sizeof(PosUvVertex) * composite_quad.size());
    composite_buffer_view.StrideInBytes = sizeof(PosUvVertex);

    D3D12_CLEAR_VALUE render_target_clear{};
    render_target_clear.Format = runtime.back_buffer_format();
    render_target_clear.Color[0] = clear_color[0];
    render_target_clear.Color[1] = clear_color[1];
    render_target_clear.Color[2] = clear_color[2];
    render_target_clear.Color[3] = clear_color[3];

    const D3D12_RESOURCE_DESC offscreen_desc = texture2d_desc(
        static_cast<UINT>(runtime.width()),
        static_cast<UINT>(runtime.height()),
        runtime.back_buffer_format(),
        1,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    );
    const auto offscreen_texture = create_texture_resource(
        runtime,
        offscreen_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &render_target_clear
    );
    const auto copied_texture = create_texture_resource(
        runtime,
        texture2d_desc(
            static_cast<UINT>(runtime.width()),
            static_cast<UINT>(runtime.height()),
            runtime.back_buffer_format()
        ),
        D3D12_RESOURCE_STATE_COPY_DEST
    );

    const UINT rtv_descriptor_size = runtime.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT srv_descriptor_size = runtime.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto rtv_heap = create_descriptor_heap(runtime.device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    const auto srv_heap = create_descriptor_heap(
        runtime.device(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        2,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    );

    runtime.device()->CreateRenderTargetView(
        offscreen_texture.get(),
        nullptr,
        descriptor_cpu_handle(rtv_heap.get(), rtv_descriptor_size, 0)
    );

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = runtime.back_buffer_format();
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    runtime.device()->CreateShaderResourceView(
        offscreen_texture.get(),
        &srv_desc,
        descriptor_cpu_handle(srv_heap.get(), srv_descriptor_size, 0)
    );
    runtime.device()->CreateShaderResourceView(
        copied_texture.get(),
        &srv_desc,
        descriptor_cpu_handle(srv_heap.get(), srv_descriptor_size, 1)
    );

    bool sampled_textures = false;

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            const float t = animation_progress(frame, total_frames);
            const TintConstants constants{{
                lerp(0.55f, 0.80f, t),
                lerp(0.70f, 1.00f, t),
                lerp(0.60f, 0.90f, t),
                1.00f,
            }};
            update_upload_buffer(constant_buffer.get(), constants);
            const auto animated_offscreen_vertices = translate_vertices(
                offscreen_quad,
                lerp(0.24f, 0.0f, t),
                lerp(-0.16f, 0.0f, t)
            );
            update_upload_buffer_array(offscreen_buffer.get(), animated_offscreen_vertices.data(), animated_offscreen_vertices.size());

            if (sampled_textures) {
                runtime.transition_resource(
                    offscreen_texture.get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET
                );
                runtime.transition_resource(
                    copied_texture.get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST
                );
            }

            const D3D12_CPU_DESCRIPTOR_HANDLE offscreen_rtv = descriptor_cpu_handle(rtv_heap.get(), rtv_descriptor_size, 0);
            runtime.command_list()->OMSetRenderTargets(1, &offscreen_rtv, FALSE, nullptr);
            runtime.command_list()->ClearRenderTargetView(offscreen_rtv, clear_color, 0, nullptr);
            runtime.command_list()->SetGraphicsRootSignature(offscreen_program.root_signature.get());
            runtime.command_list()->SetPipelineState(offscreen_program.pipeline_state.get());
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.command_list()->IASetVertexBuffers(0, 1, &offscreen_buffer_view);
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);

            runtime.transition_resource(
                offscreen_texture.get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_SOURCE
            );
            runtime.command_list()->CopyResource(copied_texture.get(), offscreen_texture.get());
            runtime.transition_resource(
                offscreen_texture.get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            );
            runtime.transition_resource(
                copied_texture.get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            );

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            ID3D12DescriptorHeap *heaps[] = {srv_heap.get()};
            runtime.command_list()->SetDescriptorHeaps(1, heaps);
            runtime.command_list()->SetGraphicsRootSignature(composite_program.root_signature.get());
            runtime.command_list()->SetPipelineState(composite_program.pipeline_state.get());
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.command_list()->IASetVertexBuffers(0, 1, &composite_buffer_view);
            runtime.command_list()->SetGraphicsRootDescriptorTable(0, descriptor_gpu_handle(srv_heap.get(), srv_descriptor_size, 0));
            runtime.command_list()->SetGraphicsRootConstantBufferView(1, constant_buffer->GetGPUVirtualAddress());
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);

            sampled_textures = true;
        },
        [&]() {
            return std::vector<PixelExpectation>{
                {"composite-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.16f, 0.85f, 0.675f), 22},
                {"composite-edge", sample_x(runtime, 0.08f), sample_y(runtime, 0.08f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
            };
        }
    );
}

ValidationResult run_mip_sampling(Dx12Runtime &runtime, unsigned int frame_budget)
{
    static const PosUvVertex quad_vertices[] = {
        {{-0.88f, 0.60f, 0.0f}, {0.0f, 0.0f}},
        {{0.88f, 0.60f, 0.0f}, {1.0f, 0.0f}},
        {{0.88f, -0.60f, 0.0f}, {1.0f, 1.0f}},
        {{-0.88f, 0.60f, 0.0f}, {0.0f, 0.0f}},
        {{0.88f, -0.60f, 0.0f}, {1.0f, 1.0f}},
        {{-0.88f, -0.60f, 0.0f}, {0.0f, 1.0f}},
    };
    static const D3D12_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.08f, 0.09f, 0.12f, 1.0f};

    D3D12_DESCRIPTOR_RANGE descriptor_range{};
    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range.NumDescriptors = 2;
    descriptor_range.BaseShaderRegister = 0;
    descriptor_range.RegisterSpace = 0;
    descriptor_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER root_parameter{};
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameter.DescriptorTable.NumDescriptorRanges = 1;
    root_parameter.DescriptorTable.pDescriptorRanges = &descriptor_range;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.ShaderRegister = 0;
    sampler_desc.RegisterSpace = 0;
    sampler_desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;

    const ShaderProgram program = create_program(
        runtime,
        mip_sampling_shader_source(),
        input_layout_desc,
        ARRAYSIZE(input_layout_desc),
        &root_parameter,
        1,
        &sampler_desc,
        1
    );
    const auto vertex_buffer = create_upload_buffer<PosUvVertex>(runtime.device(), nullptr, ARRAYSIZE(quad_vertices));

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
    vertex_buffer_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
    vertex_buffer_view.SizeInBytes = sizeof(quad_vertices);
    vertex_buffer_view.StrideInBytes = sizeof(PosUvVertex);

    const auto fill_mip = [](UINT width, UINT height, const PixelRgba8 &color) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
        for (std::size_t index = 0; index < bytes.size(); index += 4U) {
            bytes[index + 0U] = color.r;
            bytes[index + 1U] = color.g;
            bytes[index + 2U] = color.b;
            bytes[index + 3U] = color.a;
        }
        return bytes;
    };

    const std::vector<std::uint8_t> mip0 = fill_mip(8, 8, rgba(0.96f, 0.56f, 0.16f));
    const std::vector<std::uint8_t> mip1 = fill_mip(4, 4, rgba(0.84f, 0.30f, 0.72f));
    const std::vector<std::uint8_t> mip2 = fill_mip(2, 2, rgba(0.26f, 0.82f, 0.46f));
    const std::vector<std::uint8_t> mip3 = fill_mip(1, 1, rgba(0.22f, 0.48f, 0.94f));

    const D3D12_RESOURCE_DESC texture_desc = texture2d_desc(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    const auto texture = create_texture_resource(runtime, texture_desc, D3D12_RESOURCE_STATE_COPY_DEST);
    const std::vector<TextureSubresourceData> subresources = {
        {mip0.data(), 8U * 4U},
        {mip1.data(), 4U * 4U},
        {mip2.data(), 2U * 4U},
        {mip3.data(), 1U * 4U},
    };
    const auto upload_buffer = create_texture_upload_buffer(runtime.device(), texture_desc, subresources);

    const UINT srv_descriptor_size = runtime.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto descriptor_heap = create_descriptor_heap(
        runtime.device(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        2,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    );

    D3D12_SHADER_RESOURCE_VIEW_DESC full_chain_desc{};
    full_chain_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    full_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    full_chain_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    full_chain_desc.Texture2D.MipLevels = 4;
    runtime.device()->CreateShaderResourceView(
        texture.get(),
        &full_chain_desc,
        descriptor_cpu_handle(descriptor_heap.get(), srv_descriptor_size, 0)
    );

    D3D12_SHADER_RESOURCE_VIEW_DESC mip_slice_desc = full_chain_desc;
    mip_slice_desc.Texture2D.MostDetailedMip = 2;
    mip_slice_desc.Texture2D.MipLevels = 1;
    runtime.device()->CreateShaderResourceView(
        texture.get(),
        &mip_slice_desc,
        descriptor_cpu_handle(descriptor_heap.get(), srv_descriptor_size, 1)
    );

    bool uploaded = false;

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            if (!uploaded) {
                copy_texture_upload(
                    runtime.device(),
                    runtime.command_list(),
                    texture.get(),
                    upload_buffer.get(),
                    texture_desc,
                    static_cast<UINT>(subresources.size())
                );
                runtime.transition_resource(
                    texture.get(),
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                );
                uploaded = true;
            }

            const float t = animation_progress(frame, total_frames);
            const auto animated_vertices = translate_vertices(
                quad_vertices,
                lerp(-0.10f, 0.0f, t),
                lerp(0.08f, 0.0f, t)
            );
            update_upload_buffer_array(vertex_buffer.get(), animated_vertices.data(), animated_vertices.size());

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            ID3D12DescriptorHeap *heaps[] = {descriptor_heap.get()};
            runtime.command_list()->SetDescriptorHeaps(1, heaps);
            runtime.command_list()->SetGraphicsRootSignature(program.root_signature.get());
            runtime.command_list()->SetPipelineState(program.pipeline_state.get());
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.command_list()->IASetVertexBuffers(0, 1, &vertex_buffer_view);
            runtime.command_list()->SetGraphicsRootDescriptorTable(0, descriptor_gpu_handle(descriptor_heap.get(), srv_descriptor_size, 0));
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);
        },
        [&]() {
            return std::vector<PixelExpectation>{
                {"mip-left", sample_x(runtime, 0.24f), sample_y(runtime, 0.50f), rgba(0.96f, 0.56f, 0.16f), 18},
                {"mip-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.22f, 0.48f, 0.94f), 18},
                {"mip-right", sample_x(runtime, 0.76f), sample_y(runtime, 0.50f), rgba(0.26f, 0.82f, 0.46f), 18},
                {"mip-background", sample_x(runtime, 0.06f), sample_y(runtime, 0.10f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
            };
        }
    );
}

ValidationResult run_msaa_resolve(Dx12Runtime &runtime, unsigned int frame_budget)
{
    static const PosColorVertex diamond_vertices[] = {
        {{0.0f, 0.78f, 0.0f}, {0.94f, 0.97f, 1.0f, 1.0f}},
        {{0.78f, 0.0f, 0.0f}, {0.94f, 0.97f, 1.0f, 1.0f}},
        {{0.0f, -0.78f, 0.0f}, {0.94f, 0.97f, 1.0f, 1.0f}},
        {{0.0f, 0.78f, 0.0f}, {0.94f, 0.97f, 1.0f, 1.0f}},
        {{0.0f, -0.78f, 0.0f}, {0.94f, 0.97f, 1.0f, 1.0f}},
        {{-0.78f, 0.0f, 0.0f}, {0.94f, 0.97f, 1.0f, 1.0f}},
    };
    static const PosUvVertex composite_quad[] = {
        {{-0.88f, 0.88f, 0.0f}, {0.0f, 0.0f}},
        {{0.88f, 0.88f, 0.0f}, {1.0f, 0.0f}},
        {{0.88f, -0.88f, 0.0f}, {1.0f, 1.0f}},
        {{-0.88f, 0.88f, 0.0f}, {0.0f, 0.0f}},
        {{0.88f, -0.88f, 0.0f}, {1.0f, 1.0f}},
        {{-0.88f, -0.88f, 0.0f}, {0.0f, 1.0f}},
    };
    static const D3D12_INPUT_ELEMENT_DESC pos_color_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static const D3D12_INPUT_ELEMENT_DESC textured_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.03f, 0.04f, 0.07f, 1.0f};

    UINT sample_count = 0;
    for (UINT candidate : {4U, 2U}) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality_levels{};
        quality_levels.Format = runtime.back_buffer_format();
        quality_levels.SampleCount = candidate;
        demo::check_hr(
            runtime.device()->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &quality_levels,
                sizeof(quality_levels)
            ),
            "CheckFeatureSupport(MSAA levels) failed"
        );
        if (quality_levels.NumQualityLevels > 0) {
            sample_count = candidate;
            break;
        }
    }
    if (sample_count == 0) {
        return {false, "no supported MSAA sample count for RGBA8 render target"};
    }

    D3D12_DESCRIPTOR_RANGE descriptor_range{};
    descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_range.NumDescriptors = 1;
    descriptor_range.BaseShaderRegister = 0;
    descriptor_range.RegisterSpace = 0;
    descriptor_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER root_parameter{};
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameter.DescriptorTable.NumDescriptorRanges = 1;
    root_parameter.DescriptorTable.pDescriptorRanges = &descriptor_range;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.ShaderRegister = 0;
    sampler_desc.RegisterSpace = 0;
    sampler_desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;

    ProgramOptions msaa_options{};
    msaa_options.sample_count = sample_count;

    const ShaderProgram offscreen_program = create_program(
        runtime,
        pos_color_shader_source(),
        pos_color_layout_desc,
        ARRAYSIZE(pos_color_layout_desc),
        nullptr,
        0,
        nullptr,
        0,
        msaa_options
    );
    const ShaderProgram composite_program = create_program(
        runtime,
        textured_shader_source(),
        textured_layout_desc,
        ARRAYSIZE(textured_layout_desc),
        &root_parameter,
        1,
        &sampler_desc,
        1
    );

    const auto diamond_buffer = create_upload_buffer<PosColorVertex>(runtime.device(), nullptr, ARRAYSIZE(diamond_vertices));
    const auto composite_buffer = create_upload_buffer<PosUvVertex>(runtime.device(), nullptr, ARRAYSIZE(composite_quad));

    D3D12_VERTEX_BUFFER_VIEW diamond_buffer_view{};
    diamond_buffer_view.BufferLocation = diamond_buffer->GetGPUVirtualAddress();
    diamond_buffer_view.SizeInBytes = sizeof(diamond_vertices);
    diamond_buffer_view.StrideInBytes = sizeof(PosColorVertex);

    D3D12_VERTEX_BUFFER_VIEW composite_buffer_view{};
    composite_buffer_view.BufferLocation = composite_buffer->GetGPUVirtualAddress();
    composite_buffer_view.SizeInBytes = sizeof(composite_quad);
    composite_buffer_view.StrideInBytes = sizeof(PosUvVertex);

    D3D12_CLEAR_VALUE msaa_clear{};
    msaa_clear.Format = runtime.back_buffer_format();
    msaa_clear.Color[0] = clear_color[0];
    msaa_clear.Color[1] = clear_color[1];
    msaa_clear.Color[2] = clear_color[2];
    msaa_clear.Color[3] = clear_color[3];

    const D3D12_RESOURCE_DESC msaa_desc = texture2d_desc(
        static_cast<UINT>(runtime.width()),
        static_cast<UINT>(runtime.height()),
        runtime.back_buffer_format(),
        1,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        sample_count
    );
    const auto msaa_texture = create_texture_resource(
        runtime,
        msaa_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &msaa_clear
    );
    const auto resolved_texture = create_texture_resource(
        runtime,
        texture2d_desc(
            static_cast<UINT>(runtime.width()),
            static_cast<UINT>(runtime.height()),
            runtime.back_buffer_format()
        ),
        D3D12_RESOURCE_STATE_RESOLVE_DEST
    );

    const UINT rtv_descriptor_size = runtime.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT srv_descriptor_size = runtime.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto rtv_heap = create_descriptor_heap(runtime.device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    const auto srv_heap = create_descriptor_heap(
        runtime.device(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        1,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    );

    runtime.device()->CreateRenderTargetView(
        msaa_texture.get(),
        nullptr,
        descriptor_cpu_handle(rtv_heap.get(), rtv_descriptor_size, 0)
    );

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = runtime.back_buffer_format();
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    runtime.device()->CreateShaderResourceView(
        resolved_texture.get(),
        &srv_desc,
        descriptor_cpu_handle(srv_heap.get(), srv_descriptor_size, 0)
    );

    bool resolved_ready = false;

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            const float t = animation_progress(frame, total_frames);
            auto animated_diamond = translate_vertices(diamond_vertices, lerp(-0.08f, 0.0f, t), lerp(0.06f, 0.0f, t));
            for (PosColorVertex &vertex : animated_diamond) {
                vertex.position[0] *= lerp(0.72f, 1.0f, t);
                vertex.position[1] *= lerp(0.72f, 1.0f, t);
            }
            const auto animated_composite = translate_vertices(
                composite_quad,
                lerp(0.10f, 0.0f, t),
                lerp(-0.08f, 0.0f, t)
            );
            update_upload_buffer_array(diamond_buffer.get(), animated_diamond.data(), animated_diamond.size());
            update_upload_buffer_array(composite_buffer.get(), animated_composite.data(), animated_composite.size());

            if (resolved_ready) {
                runtime.transition_resource(
                    resolved_texture.get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RESOLVE_DEST
                );
            }

            const D3D12_CPU_DESCRIPTOR_HANDLE msaa_rtv = descriptor_cpu_handle(rtv_heap.get(), rtv_descriptor_size, 0);
            runtime.command_list()->OMSetRenderTargets(1, &msaa_rtv, FALSE, nullptr);
            runtime.command_list()->ClearRenderTargetView(msaa_rtv, clear_color, 0, nullptr);
            runtime.command_list()->SetGraphicsRootSignature(offscreen_program.root_signature.get());
            runtime.command_list()->SetPipelineState(offscreen_program.pipeline_state.get());
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.command_list()->IASetVertexBuffers(0, 1, &diamond_buffer_view);
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);

            runtime.transition_resource(
                msaa_texture.get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE
            );
            runtime.command_list()->ResolveSubresource(
                resolved_texture.get(),
                0,
                msaa_texture.get(),
                0,
                runtime.back_buffer_format()
            );
            runtime.transition_resource(
                msaa_texture.get(),
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            runtime.transition_resource(
                resolved_texture.get(),
                D3D12_RESOURCE_STATE_RESOLVE_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            );

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            ID3D12DescriptorHeap *heaps[] = {srv_heap.get()};
            runtime.command_list()->SetDescriptorHeaps(1, heaps);
            runtime.command_list()->SetGraphicsRootSignature(composite_program.root_signature.get());
            runtime.command_list()->SetPipelineState(composite_program.pipeline_state.get());
            runtime.command_list()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.command_list()->IASetVertexBuffers(0, 1, &composite_buffer_view);
            runtime.command_list()->SetGraphicsRootDescriptorTable(0, descriptor_gpu_handle(srv_heap.get(), srv_descriptor_size, 0));
            runtime.command_list()->DrawInstanced(6, 1, 0, 0);

            resolved_ready = true;
        },
        [&]() {
            return std::vector<PixelExpectation>{
                {"msaa-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.94f, 0.97f, 1.0f), 24},
                {"msaa-edge", 720U, 220U, rgba(0.49f, 0.51f, 0.54f), 28},
                {"msaa-background", sample_x(runtime, 0.08f), sample_y(runtime, 0.10f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
            };
        }
    );
}

const std::vector<SceneDefinition> kScenes = {
    {
        require_scene_matrix_entry("smoke_triangle").name,
        require_scene_matrix_entry("smoke_triangle").tier,
        true,
        &run_smoke_triangle,
    },
    {
        require_scene_matrix_entry("indexed_instancing").name,
        require_scene_matrix_entry("indexed_instancing").tier,
        true,
        &run_indexed_instancing,
    },
    {
        require_scene_matrix_entry("textured_quad").name,
        require_scene_matrix_entry("textured_quad").tier,
        true,
        &run_textured_quad,
    },
    {
        require_scene_matrix_entry("depth_blend_scissor").name,
        require_scene_matrix_entry("depth_blend_scissor").tier,
        true,
        &run_depth_blend_scissor,
    },
    {
        require_scene_matrix_entry("offscreen_copy_composite").name,
        require_scene_matrix_entry("offscreen_copy_composite").tier,
        true,
        &run_offscreen_copy_composite,
    },
    {
        require_scene_matrix_entry("mip_sampling").name,
        require_scene_matrix_entry("mip_sampling").tier,
        true,
        &run_mip_sampling,
    },
    {
        require_scene_matrix_entry("msaa_resolve").name,
        require_scene_matrix_entry("msaa_resolve").tier,
        true,
        &run_msaa_resolve,
    },
};

} // namespace

const std::vector<SceneDefinition> &registered_scenes()
{
    return kScenes;
}

const SceneDefinition *find_scene(std::string_view name)
{
    const auto it = std::find_if(
        kScenes.begin(),
        kScenes.end(),
        [&](const SceneDefinition &scene) {
            return name == scene.name;
        }
    );
    return it == kScenes.end() ? nullptr : &*it;
}

} // namespace demo::scenes::dx12
