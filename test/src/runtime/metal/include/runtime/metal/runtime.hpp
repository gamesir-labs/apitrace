#pragma once

#import <Metal/Metal.h>

#include "apitrace/metal_capi.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace demo::runtime::metal {

struct PixelRgba8 {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

struct PixelExpectation {
    const char *label;
    std::uint32_t x;
    std::uint32_t y;
    PixelRgba8 expected;
    std::uint8_t tolerance;
};

struct ValidationResult {
    bool passed = true;
    std::string reason;

    ValidationResult() = default;
    ValidationResult(bool passed_value, std::string reason_value)
        : passed(passed_value), reason(std::move(reason_value))
    {
    }
};

struct CompiledLibrary {
    id<MTLLibrary> handle = nil;
    std::vector<std::uint8_t> metallib_bytes;
};

struct TracedCommandBuffer {
    id<MTLCommandBuffer> handle = nil;
    std::uint64_t object_id = 0;
    std::uint64_t trace_begin = 0;
    std::uint64_t d3d_sequence = 0;
    std::uint64_t frame_id = 0;
};

struct TracedRenderEncoder {
    id<MTLRenderCommandEncoder> handle = nil;
    std::uint64_t object_id = 0;
    std::uint64_t trace_begin = 0;
    std::uint64_t d3d_sequence = 0;
};

struct TracedComputeEncoder {
    id<MTLComputeCommandEncoder> handle = nil;
    std::uint64_t object_id = 0;
    std::uint64_t trace_begin = 0;
    std::uint64_t d3d_sequence = 0;
};

struct TracedBlitEncoder {
    id<MTLBlitCommandEncoder> handle = nil;
    std::uint64_t object_id = 0;
    std::uint64_t trace_begin = 0;
    std::uint64_t d3d_sequence = 0;
};

class MetalRuntime {
public:
    MetalRuntime() = default;
    MetalRuntime(const MetalRuntime &) = delete;
    MetalRuntime &operator=(const MetalRuntime &) = delete;
    MetalRuntime(MetalRuntime &&other) noexcept;
    MetalRuntime &operator=(MetalRuntime &&other) noexcept;
    ~MetalRuntime();

    static MetalRuntime create(std::uint32_t width, std::uint32_t height);

    std::uint32_t width() const noexcept { return width_; }
    std::uint32_t height() const noexcept { return height_; }
    bool tracing_enabled() const noexcept { return trace_session_ != nullptr; }
    apitrace_metal_session_t *trace_session() const noexcept { return trace_session_; }
    id<MTLDevice> device() const noexcept { return device_; }
    id<MTLCommandQueue> queue() const noexcept { return queue_; }
    id<MTLTexture> drawable_texture() const noexcept { return drawable_texture_; }
    std::uint64_t drawable_object_id() const noexcept { return drawable_object_id_; }

    std::uint64_t next_object_id();
    std::uint64_t next_frame_id();
    void set_current_d3d_sequence(std::uint64_t d3d_sequence);

    CompiledLibrary compile_library(std::string_view name, std::string_view source) const;

    TracedCommandBuffer begin_command_buffer(std::uint64_t d3d_sequence, std::uint64_t frame_id, const char *label);
    void commit_command_buffer(
        TracedCommandBuffer &command_buffer,
        std::uint32_t sync_interval,
        std::uint32_t flags,
        const char *link_payload = "{}");

    TracedRenderEncoder begin_render_encoder(
        const TracedCommandBuffer &command_buffer,
        std::uint64_t d3d_sequence,
        MTLRenderPassDescriptor *descriptor,
        const std::string &payload_json);
    void end_render_encoder(TracedRenderEncoder &encoder, const char *link_payload = "{}");

    TracedComputeEncoder begin_compute_encoder(
        const TracedCommandBuffer &command_buffer,
        std::uint64_t d3d_sequence,
        const char *payload_json = "{}");
    void end_compute_encoder(TracedComputeEncoder &encoder, const char *link_payload = "{}");

    TracedBlitEncoder begin_blit_encoder(
        const TracedCommandBuffer &command_buffer,
        std::uint64_t d3d_sequence,
        const char *payload_json = "{}");
    void end_blit_encoder(TracedBlitEncoder &encoder, const char *link_payload = "{}");

    std::string render_pass_payload_json(
        const float clear_color[4],
        const char *load_action,
        const char *store_action) const;

    std::vector<std::uint8_t> readback_rgba(id<MTLTexture> texture) const;
    ValidationResult validate_texture_pixels(
        id<MTLTexture> texture,
        const PixelExpectation *expectations,
        std::size_t expectation_count) const;
    ValidationResult validate_buffer_words(
        id<MTLBuffer> buffer,
        const std::uint32_t *expected_words,
        std::size_t word_count) const;

private:
    void close_trace_session() noexcept;
    void release_runtime_objects() noexcept;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint64_t next_object_id_ = 1;
    std::uint64_t next_frame_id_ = 1;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLTexture> drawable_texture_ = nil;
    std::uint64_t drawable_object_id_ = 0;
    apitrace_metal_session_t *trace_session_ = nullptr;
};

} // namespace demo::runtime::metal
