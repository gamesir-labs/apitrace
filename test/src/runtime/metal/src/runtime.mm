#include "runtime/metal/runtime.hpp"

#include "apitrace/metal_capi.hpp"
#include "apitrace/platform/macos_window.hpp"

#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace demo::runtime::metal {

namespace {

[[noreturn]] void fail_with_message(const std::string &message)
{
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

id<CAMetalDrawable> next_drawable_or_fail(CAMetalLayer *layer)
{
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable == nil) {
        fail_with_message("CAMetalLayer nextDrawable returned nil");
    }
    return [drawable retain];
}

std::string format_texture_descriptor_json(std::uint32_t width, std::uint32_t height)
{
    std::ostringstream json;
    json << "{\"width\":" << width
         << ",\"height\":" << height
         << ",\"pixel_format\":\"bgra8unorm\""
         << ",\"mipmap_level_count\":1}";
    return json.str();
}

std::string shell_quote(std::string_view text)
{
    std::string quoted;
    quoted.push_back('\'');
    for (const char ch : text) {
        if (ch == '\'') {
            quoted += "'\"'\"'";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::vector<std::uint8_t> read_file_bytes(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        fail_with_message("failed to open file: " + path);
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        fail_with_message("failed to stat file: " + path);
    }
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            fail_with_message("failed to read file: " + path);
        }
    }
    return bytes;
}

void write_text_file(const std::string &path, std::string_view text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        fail_with_message("failed to open file for writing: " + path);
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        fail_with_message("failed to write file: " + path);
    }
}

std::string make_temp_directory(std::string_view name)
{
    std::string path = std::string(NSTemporaryDirectory().UTF8String);
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path += "apitrace-metal-demo-";
    path += std::string(name);
    path += "-XXXXXX";

    std::vector<char> buffer(path.begin(), path.end());
    buffer.push_back('\0');
    char *created = mkdtemp(buffer.data());
    if (created == nullptr) {
        fail_with_message("mkdtemp failed");
    }
    return std::string(created);
}

std::string format_color_array(const float clear_color[4])
{
    std::ostringstream json;
    json << "[" << clear_color[0] << "," << clear_color[1] << "," << clear_color[2] << "," << clear_color[3] << "]";
    return json.str();
}

bool within_tolerance(std::uint8_t actual, std::uint8_t expected, std::uint8_t tolerance)
{
    const int delta = static_cast<int>(actual) - static_cast<int>(expected);
    return delta >= -static_cast<int>(tolerance) && delta <= static_cast<int>(tolerance);
}

std::string format_pixel_mismatch(const PixelExpectation &expectation, const PixelRgba8 &actual)
{
    char buffer[256];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s expected rgba(%u,%u,%u,%u) got rgba(%u,%u,%u,%u) at (%u,%u)",
        expectation.label ? expectation.label : "pixel",
        static_cast<unsigned int>(expectation.expected.r),
        static_cast<unsigned int>(expectation.expected.g),
        static_cast<unsigned int>(expectation.expected.b),
        static_cast<unsigned int>(expectation.expected.a),
        static_cast<unsigned int>(actual.r),
        static_cast<unsigned int>(actual.g),
        static_cast<unsigned int>(actual.b),
        static_cast<unsigned int>(actual.a),
        expectation.x,
        expectation.y);
    return buffer;
}

} // namespace

MetalRuntime::MetalRuntime(MetalRuntime &&other) noexcept
    : width_(other.width_),
      height_(other.height_),
      next_object_id_(other.next_object_id_),
      next_frame_id_(other.next_frame_id_),
      device_(other.device_),
      queue_(other.queue_),
      window_handles_(other.window_handles_),
      window_(other.window_),
      content_view_(other.content_view_),
      metal_layer_(other.metal_layer_),
      current_drawable_(other.current_drawable_),
      drawable_texture_(other.drawable_texture_),
      drawable_object_id_(other.drawable_object_id_),
      trace_session_(other.trace_session_)
{
    other.width_ = 0;
    other.height_ = 0;
    other.device_ = nil;
    other.queue_ = nil;
    other.window_handles_ = {};
    other.window_ = nil;
    other.content_view_ = nil;
    other.metal_layer_ = nil;
    other.current_drawable_ = nil;
    other.drawable_texture_ = nil;
    other.drawable_object_id_ = 0;
    other.trace_session_ = nullptr;
}

MetalRuntime &MetalRuntime::operator=(MetalRuntime &&other) noexcept
{
    if (this != &other) {
        close_trace_session();
        release_runtime_objects();
        width_ = other.width_;
        height_ = other.height_;
        next_object_id_ = other.next_object_id_;
        next_frame_id_ = other.next_frame_id_;
        device_ = other.device_;
        queue_ = other.queue_;
        window_handles_ = other.window_handles_;
        window_ = other.window_;
        content_view_ = other.content_view_;
        metal_layer_ = other.metal_layer_;
        current_drawable_ = other.current_drawable_;
        drawable_texture_ = other.drawable_texture_;
        drawable_object_id_ = other.drawable_object_id_;
        trace_session_ = other.trace_session_;

        other.width_ = 0;
        other.height_ = 0;
        other.device_ = nil;
        other.queue_ = nil;
        other.window_handles_ = {};
        other.window_ = nil;
        other.content_view_ = nil;
        other.metal_layer_ = nil;
        other.current_drawable_ = nil;
        other.drawable_texture_ = nil;
        other.drawable_object_id_ = 0;
        other.trace_session_ = nullptr;
    }
    return *this;
}

MetalRuntime::~MetalRuntime()
{
    close_trace_session();
    release_runtime_objects();
}

MetalRuntime MetalRuntime::create(std::uint32_t width, std::uint32_t height)
{
    MetalRuntime runtime;
    runtime.width_ = width;
    runtime.height_ = height;
    runtime.device_ = MTLCreateSystemDefaultDevice();
    if (runtime.device_ == nil) {
        fail_with_message("MTLCreateSystemDefaultDevice returned nil");
    }

    runtime.queue_ = [runtime.device_ newCommandQueue];
    if (runtime.queue_ == nil) {
        fail_with_message("failed to create Metal command queue");
    }

    apitrace::platform::macos::WindowSpec spec;
    spec.width = width;
    spec.height = height;
    spec.title = "apitrace_test_metal";
    spec.show = true;
    std::string window_error;
    if (!apitrace::platform::macos::create_window(spec, runtime.window_handles_, window_error)) {
        fail_with_message(window_error.empty() ? "failed to create Metal window" : window_error);
    }
    runtime.window_ = (__bridge NSWindow *)runtime.window_handles_.nswindow;
    runtime.content_view_ = (__bridge NSView *)runtime.window_handles_.content_view;
    runtime.metal_layer_ = (__bridge CAMetalLayer *)runtime.window_handles_.cametal_layer;
    runtime.metal_layer_.device = runtime.device_;

    const char *bundle_root = std::getenv("APITRACE_METAL_BUNDLE");
    if (bundle_root && *bundle_root) {
        runtime.trace_session_ = apitrace_metal_session_open(bundle_root);
        if (runtime.trace_session_ == nullptr) {
            fail_with_message("apitrace_metal_session_open failed");
        }
        runtime.drawable_object_id_ = runtime.next_object_id();
        const std::string descriptor_json = format_texture_descriptor_json(width, height);
        apitrace_metal_register_texture(runtime.trace_session_, runtime.drawable_object_id_, descriptor_json.c_str());
    }

    apitrace::platform::macos::pump_events(runtime.window_handles_);
    return runtime;
}

std::uint64_t MetalRuntime::next_object_id()
{
    return next_object_id_++;
}

std::uint64_t MetalRuntime::next_frame_id()
{
    return next_frame_id_++;
}

void MetalRuntime::set_current_d3d_sequence(std::uint64_t d3d_sequence)
{
    if (trace_session_ != nullptr) {
        apitrace_metal_set_current_d3d_sequence(trace_session_, d3d_sequence);
    }
}

CompiledLibrary MetalRuntime::compile_library(std::string_view name, std::string_view source) const
{
    const std::string temp_dir = make_temp_directory(name);
    const std::string metal_path = temp_dir + "/shader.metal";
    const std::string air_path = temp_dir + "/shader.air";
    const std::string library_path = temp_dir + "/shader.metallib";

    write_text_file(metal_path, source);

    const std::string command = "xcrun -sdk macosx metal -c -o " + shell_quote(air_path) + " " + shell_quote(metal_path) +
        " && xcrun -sdk macosx metallib -o " + shell_quote(library_path) + " " + shell_quote(air_path);
    if (std::system(command.c_str()) != 0) {
        fail_with_message("failed to compile metallib: " + std::string(name));
    }

    NSError *error = nil;
    id<MTLLibrary> library = [device_ newLibraryWithFile:[NSString stringWithUTF8String:library_path.c_str()] error:&error];
    if (library == nil) {
        const std::string message = error ? std::string([[error localizedDescription] UTF8String]) : "newLibraryWithFile failed";
        fail_with_message(message);
    }

    CompiledLibrary compiled;
    compiled.handle = library;
    compiled.metallib_bytes = read_file_bytes(library_path);
    return compiled;
}

TracedCommandBuffer MetalRuntime::begin_command_buffer(std::uint64_t d3d_sequence, std::uint64_t frame_id, const char *label)
{
    if (current_drawable_ == nil) {
        current_drawable_ = next_drawable_or_fail(metal_layer_);
        [drawable_texture_ release];
        drawable_texture_ = [current_drawable_.texture retain];
    }

    TracedCommandBuffer command_buffer;
    command_buffer.handle = [queue_ commandBuffer];
    if (command_buffer.handle == nil) {
        fail_with_message("failed to allocate MTLCommandBuffer");
    }
    if (label && *label) {
        command_buffer.handle.label = [NSString stringWithUTF8String:label];
    }
    command_buffer.object_id = next_object_id();
    command_buffer.d3d_sequence = d3d_sequence;
    command_buffer.frame_id = frame_id;

    if (trace_session_ != nullptr) {
        set_current_d3d_sequence(d3d_sequence);
        command_buffer.trace_begin =
            apitrace_metal_command_buffer_begin(trace_session_, command_buffer.object_id, frame_id, label ? label : "");
    }
    return command_buffer;
}

void MetalRuntime::commit_command_buffer(
    TracedCommandBuffer &command_buffer,
    std::uint32_t sync_interval,
    std::uint32_t flags,
    const char *link_payload)
{
    if (command_buffer.handle == nil) {
        return;
    }

    if (trace_session_ != nullptr) {
        set_current_d3d_sequence(command_buffer.d3d_sequence);
        apitrace_metal_present_drawable(
            trace_session_,
            command_buffer.object_id,
            drawable_object_id_,
            command_buffer.frame_id,
            width_,
            height_,
            sync_interval,
            flags);
    }

    [command_buffer.handle presentDrawable:current_drawable_];
    [command_buffer.handle commit];
    [command_buffer.handle waitUntilCompleted];
    if (command_buffer.handle.status != MTLCommandBufferStatusCompleted) {
        const std::string message = command_buffer.handle.error
            ? std::string([[command_buffer.handle.error localizedDescription] UTF8String])
            : "Metal command buffer did not complete successfully";
        fail_with_message(message);
    }

    if (trace_session_ != nullptr) {
        apitrace_metal_command_buffer_commit(trace_session_, command_buffer.object_id);
        apitrace_metal_emit_link(
            trace_session_,
            APITRACE_METAL_SCOPE_COMMAND_BUFFER,
            command_buffer.d3d_sequence,
            command_buffer.trace_begin,
            apitrace_metal_current_metal_sequence(trace_session_),
            link_payload ? link_payload : "{}");
        const std::vector<std::uint8_t> bgra = readback_rgba(drawable_texture_);
        apitrace::metal::record_present_frame(
            trace_session_,
            command_buffer.frame_id,
            width_,
            height_,
            width_ * 4U,
            sync_interval,
            flags,
            bgra);
    }

    [current_drawable_ release];
    current_drawable_ = nil;
    apitrace::platform::macos::pump_events(window_handles_);
    command_buffer.handle = nil;
}

TracedRenderEncoder MetalRuntime::begin_render_encoder(
    const TracedCommandBuffer &command_buffer,
    std::uint64_t d3d_sequence,
    MTLRenderPassDescriptor *descriptor,
    const std::string &payload_json)
{
    TracedRenderEncoder encoder;
    encoder.handle = [command_buffer.handle renderCommandEncoderWithDescriptor:descriptor];
    if (encoder.handle == nil) {
        fail_with_message("failed to create MTLRenderCommandEncoder");
    }
    [encoder.handle setViewport:MTLViewport{0.0, 0.0, static_cast<double>(width_), static_cast<double>(height_), 0.0, 1.0}];
    encoder.object_id = next_object_id();
    encoder.d3d_sequence = d3d_sequence;

    if (trace_session_ != nullptr) {
        set_current_d3d_sequence(d3d_sequence);
        encoder.trace_begin = apitrace_metal_render_encoder_begin(
            trace_session_,
            encoder.object_id,
            command_buffer.object_id,
            payload_json.c_str());
    }
    return encoder;
}

void MetalRuntime::end_render_encoder(TracedRenderEncoder &encoder, const char *link_payload)
{
    if (encoder.handle == nil) {
        return;
    }

    [encoder.handle endEncoding];
    if (trace_session_ != nullptr) {
        apitrace_metal_render_encoder_end(trace_session_, encoder.object_id);
        apitrace_metal_emit_link(
            trace_session_,
            APITRACE_METAL_SCOPE_ENCODER,
            encoder.d3d_sequence,
            encoder.trace_begin,
            apitrace_metal_current_metal_sequence(trace_session_),
            link_payload ? link_payload : "{}");
    }
    encoder.handle = nil;
}

TracedComputeEncoder MetalRuntime::begin_compute_encoder(
    const TracedCommandBuffer &command_buffer,
    std::uint64_t d3d_sequence,
    const char *payload_json)
{
    TracedComputeEncoder encoder;
    encoder.handle = [command_buffer.handle computeCommandEncoder];
    if (encoder.handle == nil) {
        fail_with_message("failed to create MTLComputeCommandEncoder");
    }
    encoder.object_id = next_object_id();
    encoder.d3d_sequence = d3d_sequence;

    if (trace_session_ != nullptr) {
        set_current_d3d_sequence(d3d_sequence);
        encoder.trace_begin = apitrace_metal_compute_encoder_begin(
            trace_session_,
            encoder.object_id,
            command_buffer.object_id,
            payload_json ? payload_json : "{}");
    }
    return encoder;
}

void MetalRuntime::end_compute_encoder(TracedComputeEncoder &encoder, const char *link_payload)
{
    if (encoder.handle == nil) {
        return;
    }

    [encoder.handle endEncoding];
    if (trace_session_ != nullptr) {
        apitrace_metal_compute_encoder_end(trace_session_, encoder.object_id);
        apitrace_metal_emit_link(
            trace_session_,
            APITRACE_METAL_SCOPE_ENCODER,
            encoder.d3d_sequence,
            encoder.trace_begin,
            apitrace_metal_current_metal_sequence(trace_session_),
            link_payload ? link_payload : "{}");
    }
    encoder.handle = nil;
}

TracedBlitEncoder MetalRuntime::begin_blit_encoder(
    const TracedCommandBuffer &command_buffer,
    std::uint64_t d3d_sequence,
    const char *payload_json)
{
    TracedBlitEncoder encoder;
    encoder.handle = [command_buffer.handle blitCommandEncoder];
    if (encoder.handle == nil) {
        fail_with_message("failed to create MTLBlitCommandEncoder");
    }
    encoder.object_id = next_object_id();
    encoder.d3d_sequence = d3d_sequence;

    if (trace_session_ != nullptr) {
        set_current_d3d_sequence(d3d_sequence);
        encoder.trace_begin = apitrace_metal_blit_encoder_begin(
            trace_session_,
            encoder.object_id,
            command_buffer.object_id,
            payload_json ? payload_json : "{}");
    }
    return encoder;
}

void MetalRuntime::end_blit_encoder(TracedBlitEncoder &encoder, const char *link_payload)
{
    if (encoder.handle == nil) {
        return;
    }

    [encoder.handle endEncoding];
    if (trace_session_ != nullptr) {
        apitrace_metal_blit_encoder_end(trace_session_, encoder.object_id);
        apitrace_metal_emit_link(
            trace_session_,
            APITRACE_METAL_SCOPE_ENCODER,
            encoder.d3d_sequence,
            encoder.trace_begin,
            apitrace_metal_current_metal_sequence(trace_session_),
            link_payload ? link_payload : "{}");
    }
    encoder.handle = nil;
}

std::string MetalRuntime::render_pass_payload_json(
    const float clear_color[4],
    const char *load_action,
    const char *store_action) const
{
    std::ostringstream json;
    json << "{\"command_buffer_id\":0,\"render_pass_info\":{"
         << "\"drawable_id\":" << drawable_object_id_ << ","
         << "\"color_texture_id\":" << drawable_object_id_ << ","
         << "\"width\":" << width_ << ","
         << "\"height\":" << height_ << ","
         << "\"pixel_format\":\"bgra8unorm\","
         << "\"load_action\":\"" << (load_action ? load_action : "clear") << "\","
         << "\"store_action\":\"" << (store_action ? store_action : "store") << "\","
         << "\"clear_color\":" << format_color_array(clear_color)
         << "}}";
    return json.str();
}

std::vector<std::uint8_t> MetalRuntime::readback_rgba(id<MTLTexture> texture) const
{
    const std::size_t row_pitch = static_cast<std::size_t>(width_) * 4U;
    std::vector<std::uint8_t> pixels(row_pitch * static_cast<std::size_t>(height_), 0);
    [texture getBytes:pixels.data()
          bytesPerRow:row_pitch
           fromRegion:MTLRegionMake2D(0, 0, width_, height_)
          mipmapLevel:0];
    return pixels;
}

ValidationResult MetalRuntime::validate_texture_pixels(
    id<MTLTexture> texture,
    const PixelExpectation *expectations,
    std::size_t expectation_count) const
{
    const std::vector<std::uint8_t> rgba = readback_rgba(texture);
    for (std::size_t index = 0; index < expectation_count; ++index) {
        const PixelExpectation &expectation = expectations[index];
        if (expectation.x >= width_ || expectation.y >= height_) {
            return ValidationResult(false, "pixel expectation is outside drawable bounds");
        }
        const std::size_t pixel_offset =
            (static_cast<std::size_t>(expectation.y) * static_cast<std::size_t>(width_) +
             static_cast<std::size_t>(expectation.x)) *
            4U;
        PixelRgba8 actual{
            rgba[pixel_offset + 2U],
            rgba[pixel_offset + 1U],
            rgba[pixel_offset + 0U],
            rgba[pixel_offset + 3U],
        };
        if (!within_tolerance(actual.r, expectation.expected.r, expectation.tolerance) ||
            !within_tolerance(actual.g, expectation.expected.g, expectation.tolerance) ||
            !within_tolerance(actual.b, expectation.expected.b, expectation.tolerance) ||
            !within_tolerance(actual.a, expectation.expected.a, expectation.tolerance)) {
            return ValidationResult(false, format_pixel_mismatch(expectation, actual));
        }
    }
    return ValidationResult{};
}

ValidationResult MetalRuntime::validate_buffer_words(
    id<MTLBuffer> buffer,
    const std::uint32_t *expected_words,
    std::size_t word_count) const
{
    const auto *actual_words = static_cast<const std::uint32_t *>([buffer contents]);
    if (actual_words == nullptr) {
        return ValidationResult(false, "buffer contents is null");
    }
    for (std::size_t index = 0; index < word_count; ++index) {
        if (actual_words[index] != expected_words[index]) {
            char message[256];
            std::snprintf(
                message,
                sizeof(message),
                "buffer word %zu expected 0x%08x got 0x%08x",
                index,
                expected_words[index],
                actual_words[index]);
            return ValidationResult(false, message);
        }
    }
    return ValidationResult{};
}

void MetalRuntime::close_trace_session() noexcept
{
    if (trace_session_ != nullptr) {
        apitrace_metal_session_close(trace_session_);
        trace_session_ = nullptr;
    }
}

void MetalRuntime::release_runtime_objects() noexcept
{
    [current_drawable_ release];
    current_drawable_ = nil;
    [drawable_texture_ release];
    drawable_texture_ = nil;
    metal_layer_ = nil;
    content_view_ = nil;
    window_ = nil;
    apitrace::platform::macos::destroy_window(window_handles_);
    queue_ = nil;
    device_ = nil;
}

} // namespace demo::runtime::metal
