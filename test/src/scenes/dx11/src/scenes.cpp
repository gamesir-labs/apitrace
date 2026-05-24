#include "scenes/dx11/scenes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace demo::scenes::dx11 {

namespace {

using demo::runtime::dx11::Dx11Runtime;
using demo::runtime::dx11::PixelExpectation;
using demo::runtime::dx11::PixelRgba8;
using demo::runtime::dx11::ValidationResult;
using demo::scenes::shared::SceneMatrixEntry;

const SceneMatrixEntry &require_scene_matrix_entry(std::string_view name)
{
    const SceneMatrixEntry *entry = demo::scenes::shared::find_scene_matrix_entry(name);
    if (!entry) {
        demo::fail("dx11 scene missing from shared scene matrix");
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

const char *vertex_shader_profile(D3D_FEATURE_LEVEL feature_level)
{
    switch (feature_level) {
    case D3D_FEATURE_LEVEL_11_1:
    case D3D_FEATURE_LEVEL_11_0:
        return "vs_5_0";
    case D3D_FEATURE_LEVEL_10_1:
    case D3D_FEATURE_LEVEL_10_0:
        return "vs_4_0";
    case D3D_FEATURE_LEVEL_9_3:
        return "vs_4_0_level_9_3";
    case D3D_FEATURE_LEVEL_9_2:
    case D3D_FEATURE_LEVEL_9_1:
        return "vs_4_0_level_9_1";
    default:
        return "vs_4_0";
    }
}

const char *pixel_shader_profile(D3D_FEATURE_LEVEL feature_level)
{
    switch (feature_level) {
    case D3D_FEATURE_LEVEL_11_1:
    case D3D_FEATURE_LEVEL_11_0:
        return "ps_5_0";
    case D3D_FEATURE_LEVEL_10_1:
    case D3D_FEATURE_LEVEL_10_0:
        return "ps_4_0";
    case D3D_FEATURE_LEVEL_9_3:
        return "ps_4_0_level_9_3";
    case D3D_FEATURE_LEVEL_9_2:
    case D3D_FEATURE_LEVEL_9_1:
        return "ps_4_0_level_9_1";
    default:
        return "ps_4_0";
    }
}

PixelRgba8 rgba(float r, float g, float b, float a = 1.0f)
{
    const auto convert = [](float value) -> std::uint8_t {
        const float clamped = std::max(0.0f, std::min(1.0f, value));
        return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
    };

    return PixelRgba8{convert(r), convert(g), convert(b), convert(a)};
}

unsigned int sample_x(const Dx11Runtime &runtime, float fraction)
{
    return static_cast<unsigned int>(std::lround(static_cast<double>(runtime.width()) * fraction));
}

unsigned int sample_y(const Dx11Runtime &runtime, float fraction)
{
    return static_cast<unsigned int>(std::lround(static_cast<double>(runtime.height()) * fraction));
}

template <typename T>
demo::ComPtr<ID3D11Buffer> create_static_buffer(
    ID3D11Device *device,
    UINT bind_flags,
    const T *data,
    std::size_t count
)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(sizeof(T) * count);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;

    D3D11_SUBRESOURCE_DATA initial_data{};
    initial_data.pSysMem = data;

    demo::ComPtr<ID3D11Buffer> buffer;
    demo::check_hr(device->CreateBuffer(&desc, &initial_data, buffer.put()), "CreateBuffer(static) failed");
    return buffer;
}

template <typename T>
demo::ComPtr<ID3D11Buffer> create_dynamic_constant_buffer(ID3D11Device *device)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(T);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    demo::ComPtr<ID3D11Buffer> buffer;
    demo::check_hr(device->CreateBuffer(&desc, nullptr, buffer.put()), "CreateBuffer(constant) failed");
    return buffer;
}

template <typename T>
demo::ComPtr<ID3D11Buffer> create_dynamic_vertex_buffer(ID3D11Device *device, std::size_t count)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(sizeof(T) * count);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    demo::ComPtr<ID3D11Buffer> buffer;
    demo::check_hr(device->CreateBuffer(&desc, nullptr, buffer.put()), "CreateBuffer(dynamic vertex) failed");
    return buffer;
}

template <typename T>
void update_dynamic_buffer(ID3D11DeviceContext *context, ID3D11Buffer *buffer, const T &value)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    demo::check_hr(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map(constant) failed");
    std::memcpy(mapped.pData, &value, sizeof(T));
    context->Unmap(buffer, 0);
}

template <typename T>
void update_dynamic_buffer_array(ID3D11DeviceContext *context, ID3D11Buffer *buffer, const T *values, std::size_t count)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    demo::check_hr(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map(dynamic array) failed");
    std::memcpy(mapped.pData, values, sizeof(T) * count);
    context->Unmap(buffer, 0);
}

struct ShaderProgram {
    demo::ComPtr<ID3D11VertexShader> vertex_shader;
    demo::ComPtr<ID3D11PixelShader> pixel_shader;
    demo::ComPtr<ID3D11InputLayout> input_layout;
};

ShaderProgram create_program(
    Dx11Runtime &runtime,
    const char *source,
    const D3D11_INPUT_ELEMENT_DESC *input_layout_desc,
    UINT input_layout_count
)
{
    ShaderProgram program;
    demo::ComPtr<ID3DBlob> vs_blob = demo::compile_shader(
        source,
        "vs_main",
        vertex_shader_profile(runtime.feature_level())
    );
    demo::ComPtr<ID3DBlob> ps_blob = demo::compile_shader(
        source,
        "ps_main",
        pixel_shader_profile(runtime.feature_level())
    );

    demo::check_hr(
        runtime.device()->CreateVertexShader(
            vs_blob->GetBufferPointer(),
            vs_blob->GetBufferSize(),
            nullptr,
            program.vertex_shader.put()
        ),
        "CreateVertexShader failed"
    );
    demo::check_hr(
        runtime.device()->CreatePixelShader(
            ps_blob->GetBufferPointer(),
            ps_blob->GetBufferSize(),
            nullptr,
            program.pixel_shader.put()
        ),
        "CreatePixelShader failed"
    );
    demo::check_hr(
        runtime.device()->CreateInputLayout(
            input_layout_desc,
            input_layout_count,
            vs_blob->GetBufferPointer(),
            vs_blob->GetBufferSize(),
            program.input_layout.put()
        ),
        "CreateInputLayout failed"
    );
    return program;
}

template <typename RenderFn, typename ValidateFn>
ValidationResult run_scene_frames(
    Dx11Runtime &runtime,
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

        render_frame(frame, frames);
        if (frame + 1 == frames) {
            result = validate_frame();
        }
        runtime.present();
        if (frame + 1 < frames) {
            Sleep(16);
        }
    }

    return result;
}

ValidationResult validate_back_buffer(
    Dx11Runtime &runtime,
    std::initializer_list<PixelExpectation> expectations
)
{
    return runtime.validate_pixels(runtime.back_buffer(), expectations.begin(), expectations.size());
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

ValidationResult run_smoke_triangle(Dx11Runtime &runtime, unsigned int frame_budget)
{
    static const D3D11_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.04f, 0.05f, 0.08f, 1.0f};
    const std::vector<PosColorVertex> vertices = load_pos_color_vertices_asset("assets/dx11/smoke_triangle_vertices.txt");

    const ShaderProgram program = create_program(runtime, smoke_triangle_shader_source(), input_layout_desc, ARRAYSIZE(input_layout_desc));
    const auto vertex_buffer = create_static_buffer(runtime.device(), D3D11_BIND_VERTEX_BUFFER, vertices.data(), vertices.size());
    const auto constant_buffer = create_dynamic_constant_buffer<SmokeTriangleConstants>(runtime.device());

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
            update_dynamic_buffer(runtime.context(), constant_buffer.get(), constants);

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            UINT stride = sizeof(PosColorVertex);
            UINT offset = 0;
            ID3D11Buffer *vertex_buffers[] = {vertex_buffer.get()};
            ID3D11Buffer *constant_buffers[] = {constant_buffer.get()};

            runtime.context()->IASetInputLayout(program.input_layout.get());
            runtime.context()->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(program.pixel_shader.get(), nullptr, 0);
            runtime.context()->VSSetConstantBuffers(0, 1, constant_buffers);
            runtime.context()->PSSetConstantBuffers(0, 1, constant_buffers);
            runtime.context()->Draw(3, 0);
        },
        [&]() {
            return validate_back_buffer(
                runtime,
                {
                    {"triangle-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.48f), rgba(0.90f, 0.45f, 0.20f), 18},
                    {"background-corner", 16U, 16U, rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
                }
            );
        }
    );
}

ValidationResult run_indexed_instancing(Dx11Runtime &runtime, unsigned int frame_budget)
{
    static const D3D11_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    const float clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    const std::vector<Pos2Vertex> quad_vertices = load_pos2_vertices_asset("assets/dx11/indexed_instancing_vertices.txt");
    const std::vector<std::uint16_t> quad_indices = demo::load_installed_numeric_asset<std::uint16_t>(
        "assets/dx11/indexed_instancing_indices.txt"
    );
    const std::vector<InstanceVertex> instances = load_instance_vertices_asset("assets/dx11/indexed_instancing_instances.txt");

    const ShaderProgram program = create_program(runtime, instancing_shader_source(), input_layout_desc, ARRAYSIZE(input_layout_desc));
    const auto vertex_buffer = create_static_buffer(runtime.device(), D3D11_BIND_VERTEX_BUFFER, quad_vertices.data(), quad_vertices.size());
    const auto index_buffer = create_static_buffer(runtime.device(), D3D11_BIND_INDEX_BUFFER, quad_indices.data(), quad_indices.size());
    const auto instance_buffer = create_dynamic_vertex_buffer<InstanceVertex>(runtime.device(), instances.size());

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
            update_dynamic_buffer_array(
                runtime.context(),
                instance_buffer.get(),
                animated_instances.data(),
                animated_instances.size()
            );

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            UINT strides[] = {sizeof(Pos2Vertex), sizeof(InstanceVertex)};
            UINT offsets[] = {0, 0};
            ID3D11Buffer *vertex_buffers[] = {vertex_buffer.get(), instance_buffer.get()};
            runtime.context()->IASetInputLayout(program.input_layout.get());
            runtime.context()->IASetVertexBuffers(0, 2, vertex_buffers, strides, offsets);
            runtime.context()->IASetIndexBuffer(index_buffer.get(), DXGI_FORMAT_R16_UINT, 0);
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(program.pixel_shader.get(), nullptr, 0);
            runtime.context()->DrawIndexed(6, 0, 0);
            runtime.context()->DrawIndexedInstanced(6, 2, 0, 0, 1);
        },
        [&]() {
            return validate_back_buffer(
                runtime,
                {
                    {"instance-left", sample_x(runtime, 0.29f), sample_y(runtime, 0.50f), rgba(0.20f, 0.82f, 0.30f), 18},
                    {"instance-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.86f, 0.65f, 0.25f), 18},
                    {"instance-right", sample_x(runtime, 0.71f), sample_y(runtime, 0.50f), rgba(0.25f, 0.45f, 0.90f), 18},
                    {"background-top", sample_x(runtime, 0.50f), sample_y(runtime, 0.18f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
                }
            );
        }
    );
}

ValidationResult run_textured_quad(Dx11Runtime &runtime, unsigned int frame_budget)
{
    static const PosUvVertex quad_vertices[] = {
        {{-0.82f, 0.82f, 0.0f}, {0.0f, 0.0f}},
        {{0.82f, 0.82f, 0.0f}, {1.0f, 0.0f}},
        {{0.82f, -0.82f, 0.0f}, {1.0f, 1.0f}},
        {{-0.82f, 0.82f, 0.0f}, {0.0f, 0.0f}},
        {{0.82f, -0.82f, 0.0f}, {1.0f, 1.0f}},
        {{-0.82f, -0.82f, 0.0f}, {0.0f, 1.0f}},
    };
    static const D3D11_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.03f, 0.04f, 0.06f, 1.0f};
    const std::vector<std::uint8_t> texture_data =
        demo::load_installed_asset_bytes("assets/dx11/textured_quad_base.rgba", 4U * 4U * 4U);

    const ShaderProgram program = create_program(runtime, textured_shader_source(), input_layout_desc, ARRAYSIZE(input_layout_desc));
    const auto vertex_buffer = create_dynamic_vertex_buffer<PosUvVertex>(runtime.device(), ARRAYSIZE(quad_vertices));

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = 4;
    texture_desc.Height = 4;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    demo::ComPtr<ID3D11Texture2D> texture;
    demo::check_hr(runtime.device()->CreateTexture2D(&texture_desc, nullptr, texture.put()), "CreateTexture2D(texture) failed");
    runtime.context()->UpdateSubresource(texture.get(), 0, nullptr, texture_data.data(), 4U * 4U, 0);

    demo::ComPtr<ID3D11ShaderResourceView> shader_resource_view;
    demo::check_hr(
        runtime.device()->CreateShaderResourceView(texture.get(), nullptr, shader_resource_view.put()),
        "CreateShaderResourceView failed"
    );

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    demo::ComPtr<ID3D11SamplerState> sampler_state;
    demo::check_hr(runtime.device()->CreateSamplerState(&sampler_desc, sampler_state.put()), "CreateSamplerState failed");

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            const float t = animation_progress(frame, total_frames);
            const auto animated_vertices = translate_vertices(
                quad_vertices,
                lerp(-0.18f, 0.0f, t),
                lerp(0.10f, 0.0f, t)
            );
            update_dynamic_buffer(runtime.context(), vertex_buffer.get(), animated_vertices);

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            UINT stride = sizeof(PosUvVertex);
            UINT offset = 0;
            ID3D11Buffer *vertex_buffers[] = {vertex_buffer.get()};
            ID3D11ShaderResourceView *shader_resources[] = {shader_resource_view.get()};
            ID3D11SamplerState *samplers[] = {sampler_state.get()};

            runtime.context()->IASetInputLayout(program.input_layout.get());
            runtime.context()->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(program.pixel_shader.get(), nullptr, 0);
            runtime.context()->PSSetShaderResources(0, 1, shader_resources);
            runtime.context()->PSSetSamplers(0, 1, samplers);
            runtime.context()->Draw(6, 0);

            ID3D11ShaderResourceView *null_resources[] = {nullptr};
            runtime.context()->PSSetShaderResources(0, 1, null_resources);
        },
        [&]() {
            return validate_back_buffer(
                runtime,
                {
                    {"quad-top-left", sample_x(runtime, 0.35f), sample_y(runtime, 0.35f), rgba(1.0f, 0.92f, 0.24f), 20},
                    {"quad-top-right", sample_x(runtime, 0.65f), sample_y(runtime, 0.35f), rgba(0.31f, 0.78f, 1.0f), 20},
                    {"quad-bottom-left", sample_x(runtime, 0.35f), sample_y(runtime, 0.65f), rgba(0.71f, 0.27f, 1.0f), 20},
                    {"quad-bottom-right", sample_x(runtime, 0.65f), sample_y(runtime, 0.65f), rgba(1.0f, 1.0f, 1.0f), 20},
                }
            );
        }
    );
}

ValidationResult run_depth_blend_scissor(Dx11Runtime &runtime, unsigned int frame_budget)
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
    static const D3D11_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.03f, 0.03f, 0.05f, 1.0f};

    const ShaderProgram program = create_program(runtime, pos_color_shader_source(), input_layout_desc, ARRAYSIZE(input_layout_desc));
    const auto blue_buffer = create_static_buffer(runtime.device(), D3D11_BIND_VERTEX_BUFFER, blue_quad, ARRAYSIZE(blue_quad));
    const auto red_buffer = create_static_buffer(runtime.device(), D3D11_BIND_VERTEX_BUFFER, red_quad, ARRAYSIZE(red_quad));
    const auto green_buffer = create_static_buffer(runtime.device(), D3D11_BIND_VERTEX_BUFFER, green_quad, ARRAYSIZE(green_quad));

    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = static_cast<UINT>(runtime.width());
    depth_desc.Height = static_cast<UINT>(runtime.height());
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    demo::ComPtr<ID3D11Texture2D> depth_texture;
    demo::check_hr(runtime.device()->CreateTexture2D(&depth_desc, nullptr, depth_texture.put()), "CreateTexture2D(depth) failed");

    demo::ComPtr<ID3D11DepthStencilView> depth_stencil_view;
    demo::check_hr(
        runtime.device()->CreateDepthStencilView(depth_texture.get(), nullptr, depth_stencil_view.put()),
        "CreateDepthStencilView failed"
    );

    D3D11_DEPTH_STENCIL_DESC depth_state_desc{};
    depth_state_desc.DepthEnable = TRUE;
    depth_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_state_desc.DepthFunc = D3D11_COMPARISON_LESS;

    demo::ComPtr<ID3D11DepthStencilState> depth_state;
    demo::check_hr(runtime.device()->CreateDepthStencilState(&depth_state_desc, depth_state.put()), "CreateDepthStencilState failed");

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    demo::ComPtr<ID3D11BlendState> blend_state;
    demo::check_hr(runtime.device()->CreateBlendState(&blend_desc, blend_state.put()), "CreateBlendState failed");

    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.ScissorEnable = TRUE;
    rasterizer_desc.DepthClipEnable = TRUE;

    demo::ComPtr<ID3D11RasterizerState> rasterizer_state;
    demo::check_hr(runtime.device()->CreateRasterizerState(&rasterizer_desc, rasterizer_state.put()), "CreateRasterizerState failed");

    const D3D11_RECT scissor_rect{
        runtime.width() / 4,
        runtime.height() / 4,
        runtime.width() * 3 / 4,
        runtime.height() * 3 / 4,
    };
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            const float t = animation_progress(frame, total_frames);
            const D3D11_RECT animated_scissor_rect{
                static_cast<LONG>(lerp(static_cast<float>(runtime.width() / 2), static_cast<float>(scissor_rect.left), t)),
                static_cast<LONG>(lerp(static_cast<float>(runtime.height() / 2), static_cast<float>(scissor_rect.top), t)),
                static_cast<LONG>(lerp(static_cast<float>(runtime.width() / 2 + 32), static_cast<float>(scissor_rect.right), t)),
                static_cast<LONG>(lerp(static_cast<float>(runtime.height() / 2 + 32), static_cast<float>(scissor_rect.bottom), t)),
            };

            runtime.bind_back_buffer(depth_stencil_view.get());
            runtime.clear_back_buffer(clear_color);
            runtime.context()->ClearDepthStencilView(depth_stencil_view.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

            runtime.context()->RSSetState(rasterizer_state.get());
            runtime.context()->RSSetScissorRects(1, &animated_scissor_rect);
            runtime.context()->OMSetDepthStencilState(depth_state.get(), 0);
            runtime.context()->IASetInputLayout(program.input_layout.get());
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(program.pixel_shader.get(), nullptr, 0);

            UINT stride = sizeof(PosColorVertex);
            UINT offset = 0;
            ID3D11Buffer *vertex_buffers[] = {blue_buffer.get()};
            runtime.context()->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
            runtime.context()->OMSetBlendState(nullptr, blend_factor, 0xffffffffU);
            runtime.context()->Draw(6, 0);

            vertex_buffers[0] = red_buffer.get();
            runtime.context()->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
            runtime.context()->OMSetBlendState(blend_state.get(), blend_factor, 0xffffffffU);
            runtime.context()->Draw(6, 0);

            vertex_buffers[0] = green_buffer.get();
            runtime.context()->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
            runtime.context()->OMSetBlendState(nullptr, blend_factor, 0xffffffffU);
            runtime.context()->Draw(6, 0);
        },
        [&]() {
            return validate_back_buffer(
                runtime,
                {
                    {"depth-blend-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.55f, 0.25f, 0.58f, 0.50f), 28},
                    {"depth-blue-only", sample_x(runtime, 0.28f), sample_y(runtime, 0.50f), rgba(0.15f, 0.30f, 0.95f), 20},
                    {"scissor-outside", sample_x(runtime, 0.14f), sample_y(runtime, 0.50f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
                }
            );
        }
    );
}

ValidationResult run_offscreen_copy_composite(Dx11Runtime &runtime, unsigned int frame_budget)
{
    static const D3D11_INPUT_ELEMENT_DESC pos_color_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    static const D3D11_INPUT_ELEMENT_DESC composite_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.02f, 0.03f, 0.05f, 1.0f};
    const std::vector<PosColorVertex> offscreen_quad =
        load_pos_color_vertices_asset("assets/dx11/offscreen_copy_composite_offscreen_quad.txt");
    const std::vector<PosUvVertex> composite_quad =
        load_pos_uv_vertices_asset("assets/dx11/offscreen_copy_composite_composite_quad.txt");

    const ShaderProgram offscreen_program = create_program(
        runtime,
        pos_color_shader_source(),
        pos_color_layout_desc,
        ARRAYSIZE(pos_color_layout_desc)
    );
    const ShaderProgram composite_program = create_program(
        runtime,
        composite_shader_source(),
        composite_layout_desc,
        ARRAYSIZE(composite_layout_desc)
    );

    const auto offscreen_buffer = create_dynamic_vertex_buffer<PosColorVertex>(runtime.device(), offscreen_quad.size());
    const auto composite_buffer = create_static_buffer(
        runtime.device(),
        D3D11_BIND_VERTEX_BUFFER,
        composite_quad.data(),
        composite_quad.size()
    );
    const auto constant_buffer = create_dynamic_constant_buffer<TintConstants>(runtime.device());

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = static_cast<UINT>(runtime.width());
    texture_desc.Height = static_cast<UINT>(runtime.height());
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    demo::ComPtr<ID3D11Texture2D> offscreen_texture;
    demo::check_hr(runtime.device()->CreateTexture2D(&texture_desc, nullptr, offscreen_texture.put()), "CreateTexture2D(offscreen) failed");

    D3D11_TEXTURE2D_DESC copy_desc = texture_desc;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    demo::ComPtr<ID3D11Texture2D> copied_texture;
    demo::check_hr(runtime.device()->CreateTexture2D(&copy_desc, nullptr, copied_texture.put()), "CreateTexture2D(copy) failed");

    demo::ComPtr<ID3D11RenderTargetView> offscreen_rtv;
    demo::check_hr(
        runtime.device()->CreateRenderTargetView(offscreen_texture.get(), nullptr, offscreen_rtv.put()),
        "CreateRenderTargetView(offscreen) failed"
    );

    demo::ComPtr<ID3D11ShaderResourceView> offscreen_srv;
    demo::ComPtr<ID3D11ShaderResourceView> copied_srv;
    demo::check_hr(
        runtime.device()->CreateShaderResourceView(offscreen_texture.get(), nullptr, offscreen_srv.put()),
        "CreateShaderResourceView(offscreen) failed"
    );
    demo::check_hr(
        runtime.device()->CreateShaderResourceView(copied_texture.get(), nullptr, copied_srv.put()),
        "CreateShaderResourceView(copy) failed"
    );

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    demo::ComPtr<ID3D11SamplerState> sampler_state;
    demo::check_hr(runtime.device()->CreateSamplerState(&sampler_desc, sampler_state.put()), "CreateSamplerState failed");

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
            update_dynamic_buffer(runtime.context(), constant_buffer.get(), constants);
            const auto animated_offscreen_vertices = translate_vertices(
                offscreen_quad,
                lerp(0.24f, 0.0f, t),
                lerp(-0.16f, 0.0f, t)
            );
            update_dynamic_buffer_array(
                runtime.context(),
                offscreen_buffer.get(),
                animated_offscreen_vertices.data(),
                animated_offscreen_vertices.size()
            );

            ID3D11RenderTargetView *offscreen_targets[] = {offscreen_rtv.get()};
            runtime.context()->OMSetRenderTargets(1, offscreen_targets, nullptr);
            runtime.context()->ClearRenderTargetView(offscreen_rtv.get(), clear_color);

            UINT stride = sizeof(PosColorVertex);
            UINT offset = 0;
            ID3D11Buffer *offscreen_vertices[] = {offscreen_buffer.get()};
            runtime.context()->IASetInputLayout(offscreen_program.input_layout.get());
            runtime.context()->IASetVertexBuffers(0, 1, offscreen_vertices, &stride, &offset);
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(offscreen_program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(offscreen_program.pixel_shader.get(), nullptr, 0);
            runtime.context()->Draw(6, 0);

            runtime.context()->OMSetRenderTargets(0, nullptr, nullptr);
            runtime.context()->CopyResource(copied_texture.get(), offscreen_texture.get());

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            stride = sizeof(PosUvVertex);
            ID3D11Buffer *composite_vertices[] = {composite_buffer.get()};
            ID3D11ShaderResourceView *shader_resources[] = {offscreen_srv.get(), copied_srv.get()};
            ID3D11SamplerState *samplers[] = {sampler_state.get()};
            ID3D11Buffer *constant_buffers[] = {constant_buffer.get()};

            runtime.context()->IASetInputLayout(composite_program.input_layout.get());
            runtime.context()->IASetVertexBuffers(0, 1, composite_vertices, &stride, &offset);
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(composite_program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(composite_program.pixel_shader.get(), nullptr, 0);
            runtime.context()->PSSetShaderResources(0, 2, shader_resources);
            runtime.context()->PSSetSamplers(0, 1, samplers);
            runtime.context()->PSSetConstantBuffers(0, 1, constant_buffers);
            runtime.context()->Draw(6, 0);

            ID3D11ShaderResourceView *null_resources[] = {nullptr, nullptr};
            runtime.context()->PSSetShaderResources(0, 2, null_resources);
        },
        [&]() {
            return validate_back_buffer(
                runtime,
                {
                    {"composite-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.16f, 0.85f, 0.675f), 22},
                    {"composite-edge", sample_x(runtime, 0.08f), sample_y(runtime, 0.08f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
                }
            );
        }
    );
}

ValidationResult run_mip_sampling(Dx11Runtime &runtime, unsigned int frame_budget)
{
    static const PosUvVertex quad_vertices[] = {
        {{-0.88f, 0.60f, 0.0f}, {0.0f, 0.0f}},
        {{0.88f, 0.60f, 0.0f}, {1.0f, 0.0f}},
        {{0.88f, -0.60f, 0.0f}, {1.0f, 1.0f}},
        {{-0.88f, 0.60f, 0.0f}, {0.0f, 0.0f}},
        {{0.88f, -0.60f, 0.0f}, {1.0f, 1.0f}},
        {{-0.88f, -0.60f, 0.0f}, {0.0f, 1.0f}},
    };
    static const D3D11_INPUT_ELEMENT_DESC input_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.08f, 0.09f, 0.12f, 1.0f};

    const ShaderProgram program = create_program(
        runtime,
        mip_sampling_shader_source(),
        input_layout_desc,
        ARRAYSIZE(input_layout_desc)
    );
    const auto vertex_buffer = create_dynamic_vertex_buffer<PosUvVertex>(runtime.device(), ARRAYSIZE(quad_vertices));

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = 8;
    texture_desc.Height = 8;
    texture_desc.MipLevels = 4;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    demo::ComPtr<ID3D11Texture2D> texture;
    demo::check_hr(runtime.device()->CreateTexture2D(&texture_desc, nullptr, texture.put()), "CreateTexture2D(mip texture) failed");

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

    runtime.context()->UpdateSubresource(texture.get(), D3D11CalcSubresource(0, 0, 4), nullptr, mip0.data(), 8U * 4U, 0);
    runtime.context()->UpdateSubresource(texture.get(), D3D11CalcSubresource(1, 0, 4), nullptr, mip1.data(), 4U * 4U, 0);
    runtime.context()->UpdateSubresource(texture.get(), D3D11CalcSubresource(2, 0, 4), nullptr, mip2.data(), 2U * 4U, 0);
    runtime.context()->UpdateSubresource(texture.get(), D3D11CalcSubresource(3, 0, 4), nullptr, mip3.data(), 1U * 4U, 0);

    demo::ComPtr<ID3D11ShaderResourceView> full_chain_srv;
    demo::check_hr(
        runtime.device()->CreateShaderResourceView(texture.get(), nullptr, full_chain_srv.put()),
        "CreateShaderResourceView(full mip chain) failed"
    );

    D3D11_SHADER_RESOURCE_VIEW_DESC mip_slice_desc{};
    mip_slice_desc.Format = texture_desc.Format;
    mip_slice_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    mip_slice_desc.Texture2D.MostDetailedMip = 2;
    mip_slice_desc.Texture2D.MipLevels = 1;

    demo::ComPtr<ID3D11ShaderResourceView> mip_slice_srv;
    demo::check_hr(
        runtime.device()->CreateShaderResourceView(texture.get(), &mip_slice_desc, mip_slice_srv.put()),
        "CreateShaderResourceView(mip slice) failed"
    );

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    demo::ComPtr<ID3D11SamplerState> sampler_state;
    demo::check_hr(runtime.device()->CreateSamplerState(&sampler_desc, sampler_state.put()), "CreateSamplerState(mip) failed");

    return run_scene_frames(
        runtime,
        frame_budget,
        [&](unsigned int frame, unsigned int total_frames) {
            const float t = animation_progress(frame, total_frames);
            const auto animated_vertices = translate_vertices(
                quad_vertices,
                lerp(-0.10f, 0.0f, t),
                lerp(0.08f, 0.0f, t)
            );
            update_dynamic_buffer(runtime.context(), vertex_buffer.get(), animated_vertices);

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            UINT stride = sizeof(PosUvVertex);
            UINT offset = 0;
            ID3D11Buffer *vertex_buffers[] = {vertex_buffer.get()};
            ID3D11ShaderResourceView *shader_resources[] = {full_chain_srv.get(), mip_slice_srv.get()};
            ID3D11SamplerState *samplers[] = {sampler_state.get()};

            runtime.context()->IASetInputLayout(program.input_layout.get());
            runtime.context()->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(program.pixel_shader.get(), nullptr, 0);
            runtime.context()->PSSetShaderResources(0, 2, shader_resources);
            runtime.context()->PSSetSamplers(0, 1, samplers);
            runtime.context()->Draw(6, 0);

            ID3D11ShaderResourceView *null_resources[] = {nullptr, nullptr};
            runtime.context()->PSSetShaderResources(0, 2, null_resources);
        },
        [&]() {
            return validate_back_buffer(
                runtime,
                {
                    {"mip-left", sample_x(runtime, 0.24f), sample_y(runtime, 0.50f), rgba(0.96f, 0.56f, 0.16f), 18},
                    {"mip-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.22f, 0.48f, 0.94f), 18},
                    {"mip-right", sample_x(runtime, 0.76f), sample_y(runtime, 0.50f), rgba(0.26f, 0.82f, 0.46f), 18},
                    {"mip-background", sample_x(runtime, 0.06f), sample_y(runtime, 0.10f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
                }
            );
        }
    );
}

ValidationResult run_msaa_resolve(Dx11Runtime &runtime, unsigned int frame_budget)
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
    static const D3D11_INPUT_ELEMENT_DESC pos_color_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    static const D3D11_INPUT_ELEMENT_DESC textured_layout_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const float clear_color[4] = {0.03f, 0.04f, 0.07f, 1.0f};

    UINT sample_count = 0;
    for (UINT candidate : {4U, 2U}) {
        UINT quality_levels = 0;
        demo::check_hr(
            runtime.device()->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, candidate, &quality_levels),
            "CheckMultisampleQualityLevels failed"
        );
        if (quality_levels > 0) {
            sample_count = candidate;
            break;
        }
    }
    if (sample_count == 0) {
        return {false, "no supported MSAA sample count for RGBA8 render target"};
    }

    const ShaderProgram offscreen_program = create_program(
        runtime,
        pos_color_shader_source(),
        pos_color_layout_desc,
        ARRAYSIZE(pos_color_layout_desc)
    );
    const ShaderProgram composite_program = create_program(
        runtime,
        textured_shader_source(),
        textured_layout_desc,
        ARRAYSIZE(textured_layout_desc)
    );

    const auto diamond_buffer = create_dynamic_vertex_buffer<PosColorVertex>(runtime.device(), ARRAYSIZE(diamond_vertices));
    const auto composite_buffer = create_dynamic_vertex_buffer<PosUvVertex>(runtime.device(), ARRAYSIZE(composite_quad));

    D3D11_TEXTURE2D_DESC msaa_desc{};
    msaa_desc.Width = static_cast<UINT>(runtime.width());
    msaa_desc.Height = static_cast<UINT>(runtime.height());
    msaa_desc.MipLevels = 1;
    msaa_desc.ArraySize = 1;
    msaa_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    msaa_desc.SampleDesc.Count = sample_count;
    msaa_desc.SampleDesc.Quality = 0;
    msaa_desc.Usage = D3D11_USAGE_DEFAULT;
    msaa_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    demo::ComPtr<ID3D11Texture2D> msaa_texture;
    demo::check_hr(runtime.device()->CreateTexture2D(&msaa_desc, nullptr, msaa_texture.put()), "CreateTexture2D(msaa) failed");

    D3D11_TEXTURE2D_DESC resolve_desc = msaa_desc;
    resolve_desc.SampleDesc.Count = 1;
    resolve_desc.SampleDesc.Quality = 0;
    resolve_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    demo::ComPtr<ID3D11Texture2D> resolved_texture;
    demo::check_hr(
        runtime.device()->CreateTexture2D(&resolve_desc, nullptr, resolved_texture.put()),
        "CreateTexture2D(resolve target) failed"
    );

    demo::ComPtr<ID3D11RenderTargetView> msaa_rtv;
    demo::check_hr(runtime.device()->CreateRenderTargetView(msaa_texture.get(), nullptr, msaa_rtv.put()), "CreateRenderTargetView(msaa) failed");

    demo::ComPtr<ID3D11ShaderResourceView> resolved_srv;
    demo::check_hr(
        runtime.device()->CreateShaderResourceView(resolved_texture.get(), nullptr, resolved_srv.put()),
        "CreateShaderResourceView(resolve target) failed"
    );

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    demo::ComPtr<ID3D11SamplerState> sampler_state;
    demo::check_hr(runtime.device()->CreateSamplerState(&sampler_desc, sampler_state.put()), "CreateSamplerState(resolve) failed");

    D3D11_VIEWPORT offscreen_viewport{};
    offscreen_viewport.Width = static_cast<float>(runtime.width());
    offscreen_viewport.Height = static_cast<float>(runtime.height());
    offscreen_viewport.MinDepth = 0.0f;
    offscreen_viewport.MaxDepth = 1.0f;

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
            update_dynamic_buffer(runtime.context(), diamond_buffer.get(), animated_diamond);
            update_dynamic_buffer(runtime.context(), composite_buffer.get(), animated_composite);

            ID3D11RenderTargetView *offscreen_targets[] = {msaa_rtv.get()};
            runtime.context()->OMSetRenderTargets(1, offscreen_targets, nullptr);
            runtime.context()->RSSetViewports(1, &offscreen_viewport);
            runtime.context()->ClearRenderTargetView(msaa_rtv.get(), clear_color);

            UINT stride = sizeof(PosColorVertex);
            UINT offset = 0;
            ID3D11Buffer *diamond_buffers[] = {diamond_buffer.get()};
            runtime.context()->IASetInputLayout(offscreen_program.input_layout.get());
            runtime.context()->IASetVertexBuffers(0, 1, diamond_buffers, &stride, &offset);
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(offscreen_program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(offscreen_program.pixel_shader.get(), nullptr, 0);
            runtime.context()->Draw(6, 0);

            runtime.context()->OMSetRenderTargets(0, nullptr, nullptr);
            runtime.context()->ResolveSubresource(
                resolved_texture.get(),
                0,
                msaa_texture.get(),
                0,
                DXGI_FORMAT_R8G8B8A8_UNORM
            );

            runtime.bind_back_buffer();
            runtime.clear_back_buffer(clear_color);

            stride = sizeof(PosUvVertex);
            ID3D11Buffer *composite_vertices[] = {composite_buffer.get()};
            ID3D11ShaderResourceView *shader_resources[] = {resolved_srv.get()};
            ID3D11SamplerState *samplers[] = {sampler_state.get()};

            runtime.context()->IASetInputLayout(composite_program.input_layout.get());
            runtime.context()->IASetVertexBuffers(0, 1, composite_vertices, &stride, &offset);
            runtime.context()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            runtime.context()->VSSetShader(composite_program.vertex_shader.get(), nullptr, 0);
            runtime.context()->PSSetShader(composite_program.pixel_shader.get(), nullptr, 0);
            runtime.context()->PSSetShaderResources(0, 1, shader_resources);
            runtime.context()->PSSetSamplers(0, 1, samplers);
            runtime.context()->Draw(6, 0);

            ID3D11ShaderResourceView *null_resources[] = {nullptr};
            runtime.context()->PSSetShaderResources(0, 1, null_resources);
        },
        [&]() {
            return validate_back_buffer(
                runtime,
                {
                    {"msaa-center", sample_x(runtime, 0.50f), sample_y(runtime, 0.50f), rgba(0.94f, 0.97f, 1.0f), 24},
                    {"msaa-edge", 720U, 220U, rgba(0.49f, 0.51f, 0.54f), 28},
                    {"msaa-background", sample_x(runtime, 0.08f), sample_y(runtime, 0.10f), rgba(clear_color[0], clear_color[1], clear_color[2]), 12},
                }
            );
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

} // namespace demo::scenes::dx11
