#include "scenes/metal/scenes.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace demo::scenes::metal {

namespace {

using demo::runtime::metal::CompiledLibrary;
using demo::runtime::metal::MetalRuntime;
using demo::runtime::metal::PixelExpectation;
using demo::runtime::metal::PixelRgba8;
using demo::runtime::metal::TracedBlitEncoder;
using demo::runtime::metal::TracedCommandBuffer;
using demo::runtime::metal::TracedComputeEncoder;
using demo::runtime::metal::TracedRenderEncoder;
using demo::runtime::metal::ValidationResult;

[[noreturn]] void fail_with_message(const std::string &message)
{
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

struct BufferResource {
    std::uint64_t object_id = 0;
    id<MTLBuffer> handle = nil;
};

struct TextureResource {
    std::uint64_t object_id = 0;
    id<MTLTexture> handle = nil;
};

struct RenderPipelineResource {
    std::uint64_t object_id = 0;
    id<MTLRenderPipelineState> handle = nil;
};

struct ComputePipelineResource {
    std::uint64_t object_id = 0;
    id<MTLComputePipelineState> handle = nil;
};

struct LibraryResource {
    std::uint64_t object_id = 0;
    CompiledLibrary compiled;
};

struct ColorVertexPacked {
    float position[2];
    float padding[2];
    float color[4];
};

PixelRgba8 rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
{
    return PixelRgba8{r, g, b, a};
}

std::uint32_t sample_x(const MetalRuntime &runtime, double fraction)
{
    return static_cast<std::uint32_t>(fraction * static_cast<double>(runtime.width()));
}

std::uint32_t sample_y(const MetalRuntime &runtime, double fraction)
{
    return static_cast<std::uint32_t>(fraction * static_cast<double>(runtime.height()));
}

std::string encode_bytes_json_array(const std::vector<std::uint8_t> &bytes)
{
    std::ostringstream json;
    json << "[";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) {
            json << ",";
        }
        json << static_cast<unsigned int>(bytes[index]);
    }
    json << "]";
    return json.str();
}

std::string texture_descriptor_json(
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<std::uint8_t> &initial_bytes = {})
{
    std::ostringstream json;
    json << "{"
         << "\"width\":" << width << ","
         << "\"height\":" << height << ","
         << "\"pixel_format\":\"bgra8unorm\","
         << "\"mipmap_level_count\":1";
    if (!initial_bytes.empty()) {
        json << ",\"bytes_per_row\":" << (width * 4U)
             << ",\"initial_bytes\":" << encode_bytes_json_array(initial_bytes);
    }
    json << "}";
    return json.str();
}

std::string render_pipeline_descriptor_json(
    std::uint64_t library_id,
    const char *vertex_function,
    const char *fragment_function)
{
    std::ostringstream json;
    json << "{"
         << "\"library_id\":" << library_id << ","
         << "\"vertex_library_id\":" << library_id << ","
         << "\"fragment_library_id\":" << library_id << ","
         << "\"vertex_function\":\"" << vertex_function << "\","
         << "\"fragment_function\":\"" << fragment_function << "\","
         << "\"color_pixel_format\":\"bgra8unorm\""
         << "}";
    return json.str();
}

std::string compute_pipeline_descriptor_json(std::uint64_t library_id, const char *function_name)
{
    std::ostringstream json;
    json << "{"
         << "\"library_id\":" << library_id << ","
         << "\"function\":\"" << function_name << "\""
         << "}";
    return json.str();
}

template <typename T>
BufferResource make_buffer(MetalRuntime &runtime, const std::vector<T> &elements)
{
    BufferResource resource;
    resource.object_id = runtime.next_object_id();
    const std::size_t byte_count = elements.size() * sizeof(T);
    resource.handle = [runtime.device() newBufferWithBytes:elements.data()
                                                    length:byte_count
                                                   options:MTLResourceStorageModeShared];
    if (resource.handle == nil) {
        fail_with_message("failed to create MTLBuffer");
    }
    if (runtime.tracing_enabled()) {
        apitrace_metal_register_buffer(
            runtime.trace_session(),
            resource.object_id,
            byte_count,
            MTLStorageModeShared,
            elements.data(),
            byte_count);
    }
    return resource;
}

template <typename T>
BufferResource make_scalar_buffer(MetalRuntime &runtime, const T &value)
{
    return make_buffer(runtime, std::vector<T>{value});
}

TextureResource make_texture(
    MetalRuntime &runtime,
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<std::uint8_t> &initial_bytes)
{
    TextureResource resource;
    resource.object_id = runtime.next_object_id();
    auto *descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.depth = 1;
    descriptor.arrayLength = 1;
    descriptor.mipmapLevelCount = 1;
    descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    resource.handle = [runtime.device() newTextureWithDescriptor:descriptor];
    if (resource.handle == nil) {
        fail_with_message("failed to create MTLTexture");
    }
    [resource.handle replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:0
                         withBytes:initial_bytes.data()
                       bytesPerRow:width * 4U];
    if (runtime.tracing_enabled()) {
        const std::string descriptor_json = texture_descriptor_json(width, height, initial_bytes);
        apitrace_metal_register_texture(runtime.trace_session(), resource.object_id, descriptor_json.c_str());
    }
    return resource;
}

LibraryResource make_library(MetalRuntime &runtime, std::string_view name, std::string_view source)
{
    LibraryResource resource;
    resource.object_id = runtime.next_object_id();
    resource.compiled = runtime.compile_library(name, source);
    if (runtime.tracing_enabled()) {
        apitrace_metal_register_library(
            runtime.trace_session(),
            resource.object_id,
            resource.compiled.metallib_bytes.data(),
            resource.compiled.metallib_bytes.size());
    }
    return resource;
}

RenderPipelineResource make_render_pipeline(
    MetalRuntime &runtime,
    const LibraryResource &library,
    const char *vertex_function,
    const char *fragment_function)
{
    RenderPipelineResource resource;
    resource.object_id = runtime.next_object_id();

    auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library.compiled.handle newFunctionWithName:[NSString stringWithUTF8String:vertex_function]];
    descriptor.fragmentFunction = [library.compiled.handle newFunctionWithName:[NSString stringWithUTF8String:fragment_function]];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    NSError *error = nil;
    resource.handle = [runtime.device() newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (resource.handle == nil) {
        const std::string message = error ? std::string([[error localizedDescription] UTF8String])
                                          : "failed to create MTLRenderPipelineState";
        fail_with_message(message);
    }

    if (runtime.tracing_enabled()) {
        const std::string descriptor_json =
            render_pipeline_descriptor_json(library.object_id, vertex_function, fragment_function);
        apitrace_metal_register_render_pipeline(
            runtime.trace_session(),
            resource.object_id,
            descriptor_json.c_str(),
            library.object_id,
            library.object_id);
    }
    return resource;
}

ComputePipelineResource make_compute_pipeline(
    MetalRuntime &runtime,
    const LibraryResource &library,
    const char *function_name)
{
    ComputePipelineResource resource;
    resource.object_id = runtime.next_object_id();
    id<MTLFunction> function = [library.compiled.handle newFunctionWithName:[NSString stringWithUTF8String:function_name]];
    NSError *error = nil;
    resource.handle = [runtime.device() newComputePipelineStateWithFunction:function error:&error];
    if (resource.handle == nil) {
        const std::string message = error ? std::string([[error localizedDescription] UTF8String])
                                          : "failed to create MTLComputePipelineState";
        fail_with_message(message);
    }
    if (runtime.tracing_enabled()) {
        const std::string descriptor_json = compute_pipeline_descriptor_json(library.object_id, function_name);
        apitrace_metal_register_compute_pipeline(
            runtime.trace_session(),
            resource.object_id,
            descriptor_json.c_str(),
            library.object_id);
    }
    return resource;
}

MTLRenderPassDescriptor *make_render_pass(
    MetalRuntime &runtime,
    const float clear_color[4],
    MTLLoadAction load_action,
    MTLStoreAction store_action)
{
    auto *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    descriptor.colorAttachments[0].texture = runtime.drawable_texture();
    descriptor.colorAttachments[0].loadAction = load_action;
    descriptor.colorAttachments[0].storeAction = store_action;
    descriptor.colorAttachments[0].clearColor = MTLClearColorMake(
        clear_color[0],
        clear_color[1],
        clear_color[2],
        clear_color[3]);
    return descriptor;
}

ValidationResult present_and_validate(
    MetalRuntime &runtime,
    TracedCommandBuffer &command_buffer,
    const PixelExpectation *expectations,
    std::size_t expectation_count,
    std::uint32_t sync_interval = 1U,
    std::uint32_t flags = 0U)
{
    runtime.commit_command_buffer(command_buffer, sync_interval, flags);
    return runtime.validate_texture_pixels(runtime.drawable_texture(), expectations, expectation_count);
}

const char *solid_color_shader_source()
{
    return R"(
#include <metal_stdlib>
using namespace metal;

struct ColorVertex {
    float2 position;
    float4 color;
};

struct VSOut {
    float4 position [[position]];
    float4 color;
};

vertex VSOut vs_main(uint vid [[vertex_id]], const device ColorVertex *vertices [[buffer(0)]]) {
    VSOut out;
    ColorVertex v = vertices[vid];
    out.position = float4(v.position, 0.0, 1.0);
    out.color = v.color;
    return out;
}

fragment float4 fs_main(VSOut in [[stage_in]]) {
    return in.color;
}
)";
}

const char *instanced_shader_source()
{
    return R"(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float4 color;
};

vertex VSOut vs_main(uint vid [[vertex_id]],
                     uint iid [[instance_id]],
                     const device float2 *positions [[buffer(0)]],
                     const device float2 *offsets [[buffer(1)]]) {
    const float4 colors[2] = {
        float4(0.0, 0.4, 1.0, 1.0),
        float4(0.0, 1.0, 0.2, 1.0)
    };
    VSOut out;
    out.position = float4(positions[vid] + offsets[iid], 0.0, 1.0);
    out.color = colors[iid];
    return out;
}

fragment float4 fs_main(VSOut in [[stage_in]]) {
    return in.color;
}
)";
}

const char *texture_shader_source()
{
    return R"(
#include <metal_stdlib>
using namespace metal;

struct TexturedVertex {
    float2 position;
    float2 uv;
};

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut vs_main(uint vid [[vertex_id]], const device TexturedVertex *vertices [[buffer(0)]]) {
    VSOut out;
    TexturedVertex v = vertices[vid];
    out.position = float4(v.position, 0.0, 1.0);
    out.uv = v.uv;
    return out;
}

fragment float4 fs_main(VSOut in [[stage_in]],
                        texture2d<float, access::read> colorTexture [[texture(0)]],
                        constant uint2 &dimensions [[buffer(0)]]) {
    const float2 scaled = clamp(in.uv, float2(0.0), float2(0.9999));
    const uint2 coord = uint2(scaled * float2(dimensions));
    return colorTexture.read(coord);
}
)";
}

const char *compute_shader_source()
{
    return R"(
#include <metal_stdlib>
using namespace metal;

kernel void cs_main(device uint *outputWords [[buffer(0)]],
                    constant uint &baseIndex [[buffer(1)]],
                    uint gid [[thread_position_in_grid]]) {
    outputWords[baseIndex + gid] = 0x01020304u + gid;
}
)";
}

std::vector<std::uint8_t> checker_texture_bytes()
{
    return {
        0, 0, 255, 255,     0, 255, 0, 255,
        255, 0, 0, 255,     0, 255, 255, 255,
    };
}

ValidationResult run_smoke_triangle(MetalRuntime &runtime)
{
    const std::uint64_t d3d_sequence = 1001;
    const auto library = make_library(runtime, "smoke-triangle", solid_color_shader_source());
    const auto pipeline = make_render_pipeline(runtime, library, "vs_main", "fs_main");

    const auto vertex_buffer = make_buffer(runtime, std::vector<ColorVertexPacked>{
        {{0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{-1.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{1.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    });

    const float clear_color[4] = {0.05f, 0.05f, 0.05f, 1.0f};
    auto command_buffer = runtime.begin_command_buffer(d3d_sequence, runtime.next_frame_id(), "metal_smoke_triangle");
    const std::string payload_json = runtime.render_pass_payload_json(clear_color, "clear", "store");
    auto encoder = runtime.begin_render_encoder(
        command_buffer,
        d3d_sequence,
        make_render_pass(runtime, clear_color, MTLLoadActionClear, MTLStoreActionStore),
        payload_json);
    [encoder.handle setRenderPipelineState:pipeline.handle];
    [encoder.handle setVertexBuffer:vertex_buffer.handle offset:0 atIndex:0];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
        apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, vertex_buffer.object_id, 0, 0);
        apitrace_metal_draw_primitives(runtime.trace_session(), encoder.object_id, 3, 0, 3, 1, 0);
    }
    [encoder.handle drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    runtime.end_render_encoder(encoder);

    const PixelExpectation expectations[] = {
        {"triangle center", sample_x(runtime, 0.50), sample_y(runtime, 0.50), rgba8(255, 0, 0), 0},
    };
    return present_and_validate(runtime, command_buffer, expectations, std::size(expectations));
}

ValidationResult run_indexed_instancing(MetalRuntime &runtime)
{
    const std::uint64_t d3d_sequence = 1002;
    const auto library = make_library(runtime, "indexed-instancing", instanced_shader_source());
    const auto pipeline = make_render_pipeline(runtime, library, "vs_main", "fs_main");

    const auto positions = make_buffer(runtime, std::vector<std::array<float, 2>>{
        std::array<float, 2>{-0.18f, 0.18f},
        std::array<float, 2>{0.18f, 0.18f},
        std::array<float, 2>{0.18f, -0.18f},
        std::array<float, 2>{-0.18f, -0.18f},
    });
    const auto offsets = make_buffer(runtime, std::vector<std::array<float, 2>>{
        std::array<float, 2>{-0.42f, 0.0f},
        std::array<float, 2>{0.42f, 0.0f},
    });
    const auto index_buffer = make_buffer(runtime, std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3});

    const float clear_color[4] = {0.02f, 0.02f, 0.05f, 1.0f};
    auto command_buffer = runtime.begin_command_buffer(d3d_sequence, runtime.next_frame_id(), "metal_indexed_instancing");
    const std::string payload_json = runtime.render_pass_payload_json(clear_color, "clear", "store");
    auto encoder = runtime.begin_render_encoder(
        command_buffer,
        d3d_sequence,
        make_render_pass(runtime, clear_color, MTLLoadActionClear, MTLStoreActionStore),
        payload_json);
    [encoder.handle setRenderPipelineState:pipeline.handle];
    [encoder.handle setVertexBuffer:positions.handle offset:0 atIndex:0];
    [encoder.handle setVertexBuffer:offsets.handle offset:0 atIndex:1];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
        apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, positions.object_id, 0, 0);
        apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, offsets.object_id, 0, 1);
        apitrace_metal_draw_indexed_primitives(
            runtime.trace_session(),
            encoder.object_id,
            3,
            6,
            0,
            index_buffer.object_id,
            0,
            2,
            0,
            0);
    }
    [encoder.handle drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:6
                                 indexType:MTLIndexTypeUInt16
                               indexBuffer:index_buffer.handle
                         indexBufferOffset:0
                             instanceCount:2];
    runtime.end_render_encoder(encoder);

    const PixelExpectation expectations[] = {
        {"left quad", sample_x(runtime, 0.28), sample_y(runtime, 0.50), rgba8(0, 102, 255), 0},
        {"right quad", sample_x(runtime, 0.72), sample_y(runtime, 0.50), rgba8(0, 255, 51), 0},
    };
    return present_and_validate(runtime, command_buffer, expectations, std::size(expectations));
}

ValidationResult run_textured_quad(MetalRuntime &runtime)
{
    const std::uint64_t d3d_sequence = 1003;
    const auto library = make_library(runtime, "textured-quad", texture_shader_source());
    const auto pipeline = make_render_pipeline(runtime, library, "vs_main", "fs_main");

    struct TexturedVertex {
        float position[2];
        float uv[2];
    };
    const auto vertices = make_buffer(runtime, std::vector<TexturedVertex>{
        {{-1.0f, 1.0f}, {0.0f, 0.0f}},
        {{-1.0f, -1.0f}, {0.0f, 1.0f}},
        {{1.0f, 1.0f}, {1.0f, 0.0f}},
        {{1.0f, -1.0f}, {1.0f, 1.0f}},
    });
    const auto texture = make_texture(runtime, 2, 2, checker_texture_bytes());
    const auto dimensions = make_buffer(runtime, std::vector<std::uint32_t>{2, 2});

    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    auto command_buffer = runtime.begin_command_buffer(d3d_sequence, runtime.next_frame_id(), "metal_textured_quad");
    const std::string payload_json = runtime.render_pass_payload_json(clear_color, "clear", "store");
    auto encoder = runtime.begin_render_encoder(
        command_buffer,
        d3d_sequence,
        make_render_pass(runtime, clear_color, MTLLoadActionClear, MTLStoreActionStore),
        payload_json);
    [encoder.handle setRenderPipelineState:pipeline.handle];
    [encoder.handle setVertexBuffer:vertices.handle offset:0 atIndex:0];
    [encoder.handle setFragmentTexture:texture.handle atIndex:0];
    [encoder.handle setFragmentBuffer:dimensions.handle offset:0 atIndex:0];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
        apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, vertices.object_id, 0, 0);
        apitrace_metal_set_fragment_texture(runtime.trace_session(), encoder.object_id, texture.object_id, 0);
        apitrace_metal_set_fragment_buffer(runtime.trace_session(), encoder.object_id, dimensions.object_id, 0, 0);
        apitrace_metal_draw_primitives(runtime.trace_session(), encoder.object_id, 4, 0, 4, 1, 0);
    }
    [encoder.handle drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    runtime.end_render_encoder(encoder);

    const PixelExpectation expectations[] = {
        {"top left", sample_x(runtime, 0.25), sample_y(runtime, 0.25), rgba8(255, 0, 0), 0},
        {"top right", sample_x(runtime, 0.75), sample_y(runtime, 0.25), rgba8(0, 255, 0), 0},
        {"bottom left", sample_x(runtime, 0.25), sample_y(runtime, 0.75), rgba8(0, 0, 255), 0},
        {"bottom right", sample_x(runtime, 0.75), sample_y(runtime, 0.75), rgba8(255, 255, 0), 0},
    };
    return present_and_validate(runtime, command_buffer, expectations, std::size(expectations));
}

ValidationResult run_compute_uav(MetalRuntime &runtime)
{
    const std::uint64_t d3d_sequence = 1004;
    const auto library = make_library(runtime, "compute-uav", compute_shader_source());
    const auto compute_pipeline = make_compute_pipeline(runtime, library, "cs_main");
    const auto draw_library = make_library(runtime, "compute-present", solid_color_shader_source());
    const auto draw_pipeline = make_render_pipeline(runtime, draw_library, "vs_main", "fs_main");

    const auto output_words = make_buffer(runtime, std::vector<std::uint32_t>(8, 0));
    const auto direct_base = make_scalar_buffer(runtime, std::uint32_t{0});
    const auto indirect_base = make_scalar_buffer(runtime, std::uint32_t{4});
    const auto dispatch_args = make_buffer(runtime, std::vector<std::uint32_t>{4, 1, 1});
    const auto readback_words = make_buffer(runtime, std::vector<std::uint32_t>(8, 0));
    const auto range_source_words = make_buffer(runtime, std::vector<std::uint32_t>{9, 8, 7, 6, 5, 4, 3, 2});
    const auto range_readback_words = make_buffer(runtime, std::vector<std::uint32_t>(8, 0));
    const auto batch_fill_words = make_buffer(runtime, std::vector<std::uint32_t>(4, 0));

    const auto present_triangle = make_buffer(runtime, std::vector<ColorVertexPacked>{
        {{0.0f, 1.0f}, {0.0f, 0.0f}, {0.2f, 0.8f, 1.0f, 1.0f}},
        {{-1.0f, -1.0f}, {0.0f, 0.0f}, {0.2f, 0.8f, 1.0f, 1.0f}},
        {{1.0f, -1.0f}, {0.0f, 0.0f}, {0.2f, 0.8f, 1.0f, 1.0f}},
    });

    const float clear_color[4] = {0.01f, 0.01f, 0.01f, 1.0f};
    auto command_buffer = runtime.begin_command_buffer(d3d_sequence, runtime.next_frame_id(), "metal_compute_uav");

    auto compute_encoder = runtime.begin_compute_encoder(command_buffer, d3d_sequence);
    [compute_encoder.handle setComputePipelineState:compute_pipeline.handle];
    [compute_encoder.handle setBuffer:output_words.handle offset:0 atIndex:0];
    [compute_encoder.handle setBuffer:direct_base.handle offset:0 atIndex:1];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_compute_pipeline_state(runtime.trace_session(), compute_encoder.object_id, compute_pipeline.object_id);
        apitrace_metal_set_compute_buffer(runtime.trace_session(), compute_encoder.object_id, output_words.object_id, 0, 0);
        apitrace_metal_set_compute_buffer(runtime.trace_session(), compute_encoder.object_id, direct_base.object_id, 0, 1);
        apitrace_metal_dispatch_threadgroups(runtime.trace_session(), compute_encoder.object_id, 4, 1, 1, 1, 1, 1);
    }
    [compute_encoder.handle dispatchThreadgroups:MTLSizeMake(4, 1, 1)
                        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

    [compute_encoder.handle setBuffer:indirect_base.handle offset:0 atIndex:1];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_compute_buffer(runtime.trace_session(), compute_encoder.object_id, indirect_base.object_id, 0, 1);
        apitrace_metal_dispatch_threadgroups_indirect(
            runtime.trace_session(),
            compute_encoder.object_id,
            dispatch_args.object_id,
            0,
            1,
            1,
            1);
    }
    [compute_encoder.handle dispatchThreadgroupsWithIndirectBuffer:dispatch_args.handle
                                              indirectBufferOffset:0
                                             threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    runtime.end_compute_encoder(compute_encoder);

    auto blit_encoder = runtime.begin_blit_encoder(command_buffer, d3d_sequence);
    if (runtime.tracing_enabled()) {
        std::ostringstream batch_payload;
        batch_payload << "{\"ops\":[{\"op\":\"fill_buffer\","
                      << "\"buffer_id\":" << batch_fill_words.object_id << ","
                      << "\"range_start\":0,"
                      << "\"range_length\":" << (sizeof(std::uint32_t) * 4U) << ","
                      << "\"value\":90"
                      << "}]}";
        apitrace_metal_blit_batch(runtime.trace_session(), blit_encoder.object_id, batch_payload.str().c_str());
        apitrace_metal_copy_buffer_with_contents(
            runtime.trace_session(),
            blit_encoder.object_id,
            range_source_words.object_id,
            0,
            range_readback_words.object_id,
            0,
            sizeof(std::uint32_t) * 8U,
            [range_source_words.handle contents],
            sizeof(std::uint32_t) * 8U);
        apitrace_metal_copy_buffer(
            runtime.trace_session(),
            blit_encoder.object_id,
            output_words.object_id,
            0,
            readback_words.object_id,
            0,
            sizeof(std::uint32_t) * 8U);
    }
    [blit_encoder.handle fillBuffer:batch_fill_words.handle
                               range:NSMakeRange(0, sizeof(std::uint32_t) * 4U)
                               value:90];
    [blit_encoder.handle copyFromBuffer:range_source_words.handle
                           sourceOffset:0
                               toBuffer:range_readback_words.handle
                      destinationOffset:0
                                   size:sizeof(std::uint32_t) * 8U];
    [blit_encoder.handle copyFromBuffer:output_words.handle
                           sourceOffset:0
                               toBuffer:readback_words.handle
                      destinationOffset:0
                                   size:sizeof(std::uint32_t) * 8U];
    runtime.end_blit_encoder(blit_encoder);

    const std::string payload_json = runtime.render_pass_payload_json(clear_color, "clear", "store");
    auto render_encoder = runtime.begin_render_encoder(
        command_buffer,
        d3d_sequence,
        make_render_pass(runtime, clear_color, MTLLoadActionClear, MTLStoreActionStore),
        payload_json);
    [render_encoder.handle setRenderPipelineState:draw_pipeline.handle];
    [render_encoder.handle setVertexBuffer:present_triangle.handle offset:0 atIndex:0];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_render_pipeline_state(runtime.trace_session(), render_encoder.object_id, draw_pipeline.object_id);
        apitrace_metal_set_vertex_buffer(runtime.trace_session(), render_encoder.object_id, present_triangle.object_id, 0, 0);
        apitrace_metal_draw_primitives(runtime.trace_session(), render_encoder.object_id, 3, 0, 3, 1, 0);
    }
    [render_encoder.handle drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    runtime.end_render_encoder(render_encoder);

    runtime.commit_command_buffer(command_buffer, 1, 0);
    const std::uint32_t expected_words[] = {
        0x01020304u, 0x01020305u, 0x01020306u, 0x01020307u,
        0x01020304u, 0x01020305u, 0x01020306u, 0x01020307u,
    };
    const ValidationResult buffer_result =
        runtime.validate_buffer_words(readback_words.handle, expected_words, std::size(expected_words));
    if (!buffer_result.passed) {
        return buffer_result;
    }
    const std::uint32_t expected_range_words[] = {9, 8, 7, 6, 5, 4, 3, 2};
    const ValidationResult range_buffer_result =
        runtime.validate_buffer_words(range_readback_words.handle, expected_range_words, std::size(expected_range_words));
    if (!range_buffer_result.passed) {
        return range_buffer_result;
    }
    const std::uint32_t expected_batch_fill_words[] = {0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au};
    const ValidationResult batch_fill_result =
        runtime.validate_buffer_words(batch_fill_words.handle, expected_batch_fill_words, std::size(expected_batch_fill_words));
    if (!batch_fill_result.passed) {
        return batch_fill_result;
    }
    const PixelExpectation expectations[] = {
        {"compute present", sample_x(runtime, 0.50), sample_y(runtime, 0.50), rgba8(51, 204, 255), 0},
    };
    return runtime.validate_texture_pixels(runtime.drawable_texture(), expectations, std::size(expectations));
}

ValidationResult run_indirect_draw(MetalRuntime &runtime)
{
    const std::uint64_t d3d_sequence = 1005;
    const auto library = make_library(runtime, "indirect-draw", solid_color_shader_source());
    const auto pipeline = make_render_pipeline(runtime, library, "vs_main", "fs_main");

    const auto vertex_buffer = make_buffer(runtime, std::vector<ColorVertexPacked>{
        {{-0.55f, 0.55f}, {0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 1.0f}},
        {{-0.85f, -0.25f}, {0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 1.0f}},
        {{-0.25f, -0.25f}, {0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 1.0f}},
        {{0.55f, 0.55f}, {0.0f, 0.0f}, {1.0f, 0.0f, 1.0f, 1.0f}},
        {{0.25f, -0.25f}, {0.0f, 0.0f}, {1.0f, 0.0f, 1.0f, 1.0f}},
        {{0.85f, -0.25f}, {0.0f, 0.0f}, {1.0f, 0.0f, 1.0f, 1.0f}},
    });
    struct DrawIndirectArgs {
        std::uint32_t vertexCount;
        std::uint32_t instanceCount;
        std::uint32_t vertexStart;
        std::uint32_t baseInstance;
    };
    const auto indirect_args = make_buffer(runtime, std::vector<DrawIndirectArgs>{
        {3, 1, 0, 0},
        {3, 1, 3, 0},
    });

    const float clear_color[4] = {0.08f, 0.0f, 0.08f, 1.0f};
    auto command_buffer = runtime.begin_command_buffer(d3d_sequence, runtime.next_frame_id(), "metal_indirect_draw");
    const std::string payload_json = runtime.render_pass_payload_json(clear_color, "clear", "store");
    auto encoder = runtime.begin_render_encoder(
        command_buffer,
        d3d_sequence,
        make_render_pass(runtime, clear_color, MTLLoadActionClear, MTLStoreActionStore),
        payload_json);
    [encoder.handle setRenderPipelineState:pipeline.handle];
    [encoder.handle setVertexBuffer:vertex_buffer.handle offset:0 atIndex:0];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
        apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, vertex_buffer.object_id, 0, 0);
        apitrace_metal_draw_primitives_indirect(
            runtime.trace_session(),
            encoder.object_id,
            3,
            indirect_args.object_id,
            0);
        apitrace_metal_draw_primitives_indirect(
            runtime.trace_session(),
            encoder.object_id,
            3,
            indirect_args.object_id,
            sizeof(DrawIndirectArgs));
    }
    [encoder.handle drawPrimitives:MTLPrimitiveTypeTriangle
                     indirectBuffer:indirect_args.handle
               indirectBufferOffset:0];
    [encoder.handle drawPrimitives:MTLPrimitiveTypeTriangle
                     indirectBuffer:indirect_args.handle
               indirectBufferOffset:sizeof(DrawIndirectArgs)];
    runtime.end_render_encoder(encoder);

    const PixelExpectation expectations[] = {
        {"left indirect", sample_x(runtime, 0.25), sample_y(runtime, 0.45), rgba8(255, 255, 0), 0},
        {"right indirect", sample_x(runtime, 0.75), sample_y(runtime, 0.45), rgba8(255, 0, 255), 0},
    };
    return present_and_validate(runtime, command_buffer, expectations, std::size(expectations));
}

ValidationResult run_indexed_indirect_draw(MetalRuntime &runtime)
{
    const std::uint64_t d3d_sequence = 1011;
    const auto library = make_library(runtime, "indexed-indirect-draw", solid_color_shader_source());
    const auto pipeline = make_render_pipeline(runtime, library, "vs_main", "fs_main");

    const auto vertex_buffer = make_buffer(runtime, std::vector<ColorVertexPacked>{
        {{-0.62f, 0.62f}, {0.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}},
        {{-0.88f, -0.22f}, {0.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}},
        {{-0.36f, -0.22f}, {0.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}},
        {{0.62f, 0.62f}, {0.0f, 0.0f}, {1.0f, 0.5f, 0.0f, 1.0f}},
        {{0.36f, -0.22f}, {0.0f, 0.0f}, {1.0f, 0.5f, 0.0f, 1.0f}},
        {{0.88f, -0.22f}, {0.0f, 0.0f}, {1.0f, 0.5f, 0.0f, 1.0f}},
    });
    const auto index_buffer = make_buffer(runtime, std::vector<std::uint16_t>{0, 1, 2, 3, 4, 5});
    struct IndexedIndirectArgs {
        std::uint32_t indexCount;
        std::uint32_t instanceCount;
        std::uint32_t indexStart;
        std::int32_t baseVertex;
        std::uint32_t baseInstance;
    };
    const auto indirect_args = make_buffer(runtime, std::vector<IndexedIndirectArgs>{
        {3, 1, 0, 0, 0},
        {3, 1, 3, 0, 0},
    });

    const float clear_color[4] = {0.02f, 0.02f, 0.02f, 1.0f};
    auto command_buffer = runtime.begin_command_buffer(d3d_sequence, runtime.next_frame_id(), "metal_indexed_indirect_draw");
    const std::string payload_json = runtime.render_pass_payload_json(clear_color, "clear", "store");
    auto encoder = runtime.begin_render_encoder(
        command_buffer,
        d3d_sequence,
        make_render_pass(runtime, clear_color, MTLLoadActionClear, MTLStoreActionStore),
        payload_json);
    [encoder.handle setRenderPipelineState:pipeline.handle];
    [encoder.handle setVertexBuffer:vertex_buffer.handle offset:0 atIndex:0];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
        apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, vertex_buffer.object_id, 0, 0);
        apitrace_metal_draw_indexed_primitives_indirect(
            runtime.trace_session(),
            encoder.object_id,
            3,
            0,
            index_buffer.object_id,
            0,
            indirect_args.object_id,
            0);
        apitrace_metal_draw_indexed_primitives_indirect(
            runtime.trace_session(),
            encoder.object_id,
            3,
            0,
            index_buffer.object_id,
            0,
            indirect_args.object_id,
            sizeof(IndexedIndirectArgs));
    }
    [encoder.handle drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexType:MTLIndexTypeUInt16
                              indexBuffer:index_buffer.handle
                        indexBufferOffset:0
                           indirectBuffer:indirect_args.handle
                     indirectBufferOffset:0];
    [encoder.handle drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexType:MTLIndexTypeUInt16
                              indexBuffer:index_buffer.handle
                        indexBufferOffset:0
                           indirectBuffer:indirect_args.handle
                     indirectBufferOffset:sizeof(IndexedIndirectArgs)];
    runtime.end_render_encoder(encoder);

    const PixelExpectation expectations[] = {
        {"left indexed indirect", sample_x(runtime, 0.25), sample_y(runtime, 0.45), rgba8(0, 255, 255), 0},
        {"right indexed indirect", sample_x(runtime, 0.75), sample_y(runtime, 0.45), rgba8(255, 128, 0), 0},
    };
    return present_and_validate(runtime, command_buffer, expectations, std::size(expectations));
}

ValidationResult run_argument_buffer(MetalRuntime &runtime)
{
    const std::uint64_t d3d_sequence = 1006;
    const auto library = make_library(runtime, "argument-buffer", solid_color_shader_source());
    const auto pipeline = make_render_pipeline(runtime, library, "vs_main", "fs_main");

    const auto vertex_buffer = make_buffer(runtime, std::vector<ColorVertexPacked>{
        {{-0.8f, 0.8f}, {0.0f, 0.0f}, {1.0f, 0.5f, 0.0f, 1.0f}},
        {{-0.8f, -0.8f}, {0.0f, 0.0f}, {1.0f, 0.5f, 0.0f, 1.0f}},
        {{0.8f, 0.8f}, {0.0f, 0.0f}, {1.0f, 0.5f, 0.0f, 1.0f}},
        {{0.8f, -0.8f}, {0.0f, 0.0f}, {1.0f, 0.5f, 0.0f, 1.0f}},
    });
    const auto dummy_argument_buffer = make_buffer(runtime, std::vector<std::uint32_t>{0xabcdef01u, 0x12345678u});

    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    auto command_buffer = runtime.begin_command_buffer(d3d_sequence, runtime.next_frame_id(), "metal_argument_buffer");
    const std::string payload_json = runtime.render_pass_payload_json(clear_color, "clear", "store");
    auto encoder = runtime.begin_render_encoder(
        command_buffer,
        d3d_sequence,
        make_render_pass(runtime, clear_color, MTLLoadActionClear, MTLStoreActionStore),
        payload_json);
    [encoder.handle setRenderPipelineState:pipeline.handle];
    [encoder.handle setVertexBuffer:vertex_buffer.handle offset:0 atIndex:0];
    id<MTLResource> resources[] = {dummy_argument_buffer.handle};
    [encoder.handle useResources:resources count:1 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    if (runtime.tracing_enabled()) {
        apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
        apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, vertex_buffer.object_id, 0, 0);
        apitrace_metal_set_argument_buffer(
            runtime.trace_session(),
            encoder.object_id,
            0,
            dummy_argument_buffer.object_id,
            0);
        const std::uint64_t resource_ids[] = {dummy_argument_buffer.object_id};
        apitrace_metal_use_resources(runtime.trace_session(), encoder.object_id, resource_ids, 1, 1, 2);
        apitrace_metal_draw_primitives(runtime.trace_session(), encoder.object_id, 4, 0, 4, 1, 0);
    }
    [encoder.handle drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    runtime.end_render_encoder(encoder);

    const PixelExpectation expectations[] = {
        {"argument buffer", sample_x(runtime, 0.50), sample_y(runtime, 0.50), rgba8(255, 128, 0), 0},
    };
    return present_and_validate(runtime, command_buffer, expectations, std::size(expectations));
}

ValidationResult run_multi_pass(MetalRuntime &runtime)
{
    const std::uint64_t d3d_sequence = 1007;
    const auto library = make_library(runtime, "multi-pass", solid_color_shader_source());
    const auto pipeline = make_render_pipeline(runtime, library, "vs_main", "fs_main");

    const auto left_quad = make_buffer(runtime, std::vector<ColorVertexPacked>{
        {{-1.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{-1.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    });
    const auto right_quad = make_buffer(runtime, std::vector<ColorVertexPacked>{
        {{0.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{0.0f, -1.0f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{1.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{1.0f, -1.0f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    });

    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    auto command_buffer = runtime.begin_command_buffer(d3d_sequence, runtime.next_frame_id(), "metal_multi_pass");
    {
        const std::string payload_json = runtime.render_pass_payload_json(clear_color, "clear", "store");
        auto encoder = runtime.begin_render_encoder(
            command_buffer,
            d3d_sequence,
            make_render_pass(runtime, clear_color, MTLLoadActionClear, MTLStoreActionStore),
            payload_json);
        [encoder.handle setRenderPipelineState:pipeline.handle];
        [encoder.handle setVertexBuffer:left_quad.handle offset:0 atIndex:0];
        if (runtime.tracing_enabled()) {
            apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
            apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, left_quad.object_id, 0, 0);
            apitrace_metal_draw_primitives(runtime.trace_session(), encoder.object_id, 4, 0, 4, 1, 0);
        }
        [encoder.handle drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        runtime.end_render_encoder(encoder);
    }
    if (runtime.tracing_enabled()) {
        apitrace_metal_memory_barrier(runtime.trace_session(), command_buffer.object_id, "{\"scope\":\"render\"}");
        apitrace_metal_update_fence(runtime.trace_session(), command_buffer.object_id, 7001, 1);
        apitrace_metal_wait_for_fence(runtime.trace_session(), command_buffer.object_id, 7001, 1);
    }
    {
        const std::string payload_json = runtime.render_pass_payload_json(clear_color, "load", "store");
        auto encoder = runtime.begin_render_encoder(
            command_buffer,
            d3d_sequence,
            make_render_pass(runtime, clear_color, MTLLoadActionLoad, MTLStoreActionStore),
            payload_json);
        [encoder.handle setRenderPipelineState:pipeline.handle];
        [encoder.handle setVertexBuffer:right_quad.handle offset:0 atIndex:0];
        if (runtime.tracing_enabled()) {
            apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
            apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, right_quad.object_id, 0, 0);
            apitrace_metal_draw_primitives(runtime.trace_session(), encoder.object_id, 4, 0, 4, 1, 0);
        }
        [encoder.handle drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        runtime.end_render_encoder(encoder);
    }

    const PixelExpectation expectations[] = {
        {"multi pass left", sample_x(runtime, 0.25), sample_y(runtime, 0.50), rgba8(255, 0, 0), 0},
        {"multi pass right", sample_x(runtime, 0.75), sample_y(runtime, 0.50), rgba8(0, 255, 0), 0},
    };
    return present_and_validate(runtime, command_buffer, expectations, std::size(expectations));
}

ValidationResult run_present_smoke(MetalRuntime &runtime)
{
    const auto library = make_library(runtime, "present-smoke", solid_color_shader_source());
    const auto pipeline = make_render_pipeline(runtime, library, "vs_main", "fs_main");

    struct FrameConfig {
        std::uint64_t d3d_sequence;
        std::array<float, 4> clear;
        PixelRgba8 expected;
    };
    const FrameConfig frames[] = {
        {1008, {0.0f, 0.0f, 0.0f, 1.0f}, rgba8(0, 0, 255)},
        {1009, {0.0f, 0.0f, 0.0f, 1.0f}, rgba8(0, 255, 0)},
        {1010, {0.0f, 0.0f, 0.0f, 1.0f}, rgba8(255, 0, 0)},
    };

    for (const FrameConfig &frame : frames) {
        const auto vertices = make_buffer(runtime, std::vector<ColorVertexPacked>{
            {{0.0f, 1.0f}, {0.0f, 0.0f}, {frame.expected.r / 255.0f, frame.expected.g / 255.0f, frame.expected.b / 255.0f, 1.0f}},
            {{-1.0f, -1.0f}, {0.0f, 0.0f}, {frame.expected.r / 255.0f, frame.expected.g / 255.0f, frame.expected.b / 255.0f, 1.0f}},
            {{1.0f, -1.0f}, {0.0f, 0.0f}, {frame.expected.r / 255.0f, frame.expected.g / 255.0f, frame.expected.b / 255.0f, 1.0f}},
        });
        auto command_buffer = runtime.begin_command_buffer(frame.d3d_sequence, runtime.next_frame_id(), "metal_present_smoke");
        const std::string payload_json =
            runtime.render_pass_payload_json(frame.clear.data(), "clear", "store");
        auto encoder = runtime.begin_render_encoder(
            command_buffer,
            frame.d3d_sequence,
            make_render_pass(runtime, frame.clear.data(), MTLLoadActionClear, MTLStoreActionStore),
            payload_json);
        [encoder.handle setRenderPipelineState:pipeline.handle];
        [encoder.handle setVertexBuffer:vertices.handle offset:0 atIndex:0];
        if (runtime.tracing_enabled()) {
            apitrace_metal_set_render_pipeline_state(runtime.trace_session(), encoder.object_id, pipeline.object_id);
            apitrace_metal_set_vertex_buffer(runtime.trace_session(), encoder.object_id, vertices.object_id, 0, 0);
            apitrace_metal_draw_primitives(runtime.trace_session(), encoder.object_id, 3, 0, 3, 1, 0);
        }
        [encoder.handle drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        runtime.end_render_encoder(encoder);
        runtime.commit_command_buffer(command_buffer, 1, 0);

        const PixelExpectation expectation = {
            "present smoke",
            sample_x(runtime, 0.50),
            sample_y(runtime, 0.50),
            frame.expected,
            0,
        };
        const ValidationResult result = runtime.validate_texture_pixels(
            runtime.drawable_texture(),
            &expectation,
            1);
        if (!result.passed) {
            return result;
        }
    }

    return ValidationResult{};
}

const std::vector<SceneDefinition> &scene_definitions()
{
    static const std::vector<SceneDefinition> scenes = {
        {"metal_smoke_triangle", run_smoke_triangle},
        {"metal_indexed_instancing", run_indexed_instancing},
        {"metal_textured_quad", run_textured_quad},
        {"metal_compute_uav", run_compute_uav},
        {"metal_indirect_draw", run_indirect_draw},
        {"metal_indexed_indirect_draw", run_indexed_indirect_draw},
        {"metal_argument_buffer", run_argument_buffer},
        {"metal_multi_pass", run_multi_pass},
        {"metal_present_smoke", run_present_smoke},
    };
    return scenes;
}

} // namespace

const std::vector<SceneDefinition> &registered_scenes()
{
    return scene_definitions();
}

const SceneDefinition *find_scene(std::string_view name)
{
    const auto &scenes = scene_definitions();
    const auto it = std::find_if(
        scenes.begin(),
        scenes.end(),
        [name](const SceneDefinition &scene) { return scene.name == name; });
    return it == scenes.end() ? nullptr : &*it;
}

} // namespace demo::scenes::metal
