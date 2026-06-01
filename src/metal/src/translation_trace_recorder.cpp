#include "apitrace/translation_trace_recorder.hpp"

#include "trace/src/payload_object_refs.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <utility>

namespace apitrace::metal {

namespace {

using json = nlohmann::json;

trace::MetalCallKind begin_call_kind(TranslationPassKind pass_kind)
{
  switch (pass_kind) {
  case TranslationPassKind::Render:
    return trace::MetalCallKind::RenderEncoderBegin;
  case TranslationPassKind::Compute:
    return trace::MetalCallKind::ComputeEncoderBegin;
  case TranslationPassKind::Blit:
  case TranslationPassKind::Present:
    return trace::MetalCallKind::BlitEncoderBegin;
  case TranslationPassKind::Unknown:
  default:
    return trace::MetalCallKind::Unknown;
  }
}

trace::MetalCallKind end_call_kind(TranslationPassKind pass_kind)
{
  switch (pass_kind) {
  case TranslationPassKind::Render:
    return trace::MetalCallKind::RenderEncoderEnd;
  case TranslationPassKind::Compute:
    return trace::MetalCallKind::ComputeEncoderEnd;
  case TranslationPassKind::Blit:
  case TranslationPassKind::Present:
    return trace::MetalCallKind::BlitEncoderEnd;
  case TranslationPassKind::Unknown:
  default:
    return trace::MetalCallKind::Unknown;
  }
}

std::string begin_function_name(TranslationPassKind pass_kind)
{
  switch (pass_kind) {
  case TranslationPassKind::Render:
    return "MTLCommandBuffer.renderCommandEncoder";
  case TranslationPassKind::Compute:
    return "MTLCommandBuffer.computeCommandEncoder";
  case TranslationPassKind::Blit:
  case TranslationPassKind::Present:
    return "MTLCommandBuffer.blitCommandEncoder";
  case TranslationPassKind::Unknown:
  default:
    return "MTLCommandBuffer.encoderBegin";
  }
}

std::string end_function_name(TranslationPassKind pass_kind)
{
  switch (pass_kind) {
  case TranslationPassKind::Render:
    return "MTLRenderCommandEncoder.endEncoding";
  case TranslationPassKind::Compute:
    return "MTLComputeCommandEncoder.endEncoding";
  case TranslationPassKind::Blit:
  case TranslationPassKind::Present:
    return "MTLBlitCommandEncoder.endEncoding";
  case TranslationPassKind::Unknown:
  default:
    return "MTLCommandEncoder.endEncoding";
  }
}

} // namespace

struct TranslationTraceRecorder::Impl {
  TranslationTraceRecorderOptions options;
  trace::TraceBundleWriter *bundle_writer = nullptr;
  MetalTraceBackend metal_trace_backend{MetalTraceOptions{}};
  MetalObjectRegistry object_registry;
  trace::TranslationLinkWriter link_writer;
  std::string last_error;
  bool open = false;
  bool command_buffer_active = false;
  bool encoder_active = false;
  std::uint64_t current_frame_id = 0;
  std::uint64_t current_command_buffer_id = 0;
  std::uint64_t current_submission_id = 0;
  std::uint64_t current_encoder_id = 0;
  std::uint64_t current_d3d_sequence = 0;
  std::uint64_t command_buffer_sequence_begin = 0;
  std::uint64_t encoder_sequence_begin = 0;
  TranslationPassKind current_pass_kind = TranslationPassKind::Unknown;
  bool enable_per_call_links = false;

  // TODO: keep lightweight current-scope snapshots so marker and call records can inherit DXMT-like chunk/encoder context.
  // TODO: tolerate translation layers that emit command-buffer scopes on one thread and metal-call records on another thread.
};

TranslationTraceRecorder::TranslationTraceRecorder() : impl_(std::make_unique<Impl>()) {}

TranslationTraceRecorder::~TranslationTraceRecorder() = default;

bool TranslationTraceRecorder::open(trace::TraceBundleWriter &bundle_writer,
                                    MetalBridge &bridge,
                                    TranslationTraceRecorderOptions options,
                                    trace::TranslationLinkStreamOptions link_options)
{
  if (!bundle_writer.is_open()) {
    impl_->last_error = "Trace bundle writer must be open before translation trace recording starts.";
    return false;
  }

  impl_->options = std::move(options);
  impl_->bundle_writer = &bundle_writer;
  MetalTraceOptions metal_trace_options;
  metal_trace_options.start_paused = false;
  metal_trace_options.emit_debug_markers = true;
  metal_trace_options.trace_label = impl_->options.trace_label;
  impl_->metal_trace_backend = MetalTraceBackend(std::move(metal_trace_options));

  if (impl_->options.enable_metal_trace) {
    if (!impl_->metal_trace_backend.initialize(bundle_writer, bridge)) {
      impl_->last_error = impl_->metal_trace_backend.last_error();
      return false;
    }
    impl_->metal_trace_backend.begin_trace();
  }

  if (impl_->options.enable_link_sideband) {
    if (!impl_->link_writer.open(bundle_writer, std::move(link_options))) {
      impl_->last_error = "Failed to open translation link sideband writer.";
      return false;
    }
  }

  impl_->last_error.clear();
  impl_->open = true;
  return true;
}

void TranslationTraceRecorder::begin_command_buffer(const TranslationCommandBufferInfo &info)
{
  if (!impl_->open) {
    return;
  }

  impl_->current_frame_id = info.frame_id;
  impl_->current_command_buffer_id = info.command_buffer_id;
  impl_->current_submission_id = info.submission_id;
  if (info.d3d_sequence != 0) {
    impl_->current_d3d_sequence = info.d3d_sequence;
  }
  impl_->command_buffer_active = true;
  impl_->command_buffer_sequence_begin = 0;

  if (impl_->options.enable_metal_trace) {
    MetalTraceRecord trace_record;
    trace_record.call_kind = trace::MetalCallKind::CommandBufferBegin;
    trace_record.d3d_sequence = impl_->current_d3d_sequence;
    trace_record.frame_id = info.frame_id;
    trace_record.object_id = info.command_buffer_id;
    trace_record.translated_call_name = "MTLCommandBuffer.begin";
    trace_record.translation_link_payload = info.payload.empty()
                                                ? json{{"label", info.label}, {"submission_id", info.submission_id}}.dump()
                                                : info.payload;
    trace::append_payload_text_object_refs(trace_record.translation_link_payload, trace_record.object_refs);
    impl_->metal_trace_backend.record_translated_call(trace_record);
    impl_->command_buffer_sequence_begin = impl_->metal_trace_backend.current_metal_sequence();
  }

  // TODO: expose an explicit chunk/queue-oriented helper for DXMT-style command queue integration if multiple translators converge on the same pattern.
}

void TranslationTraceRecorder::end_command_buffer()
{
  if (!impl_->open || !impl_->command_buffer_active) {
    return;
  }

  impl_->command_buffer_active = false;

  if (impl_->options.enable_metal_trace) {
    MetalTraceRecord trace_record;
    trace_record.call_kind = trace::MetalCallKind::CommandBufferCommit;
    trace_record.d3d_sequence = impl_->current_d3d_sequence;
    trace_record.frame_id = impl_->current_frame_id;
    trace_record.object_id = impl_->current_command_buffer_id;
    trace_record.translated_call_name = "MTLCommandBuffer.commit";
    trace_record.translation_link_payload =
        json{{"submission_id", impl_->current_submission_id}, {"label", std::string()}}.dump();
    impl_->metal_trace_backend.record_translated_call(trace_record);
  }

  if (impl_->options.enable_link_sideband && impl_->command_buffer_sequence_begin != 0) {
    trace::TranslationLinkRecord record;
    record.record_type = "scope";
    record.scope_kind = "command_buffer";
    record.d3d_sequence = impl_->current_d3d_sequence;
    record.metal_sequence_begin = impl_->command_buffer_sequence_begin;
    record.metal_sequence_end = impl_->metal_trace_backend.current_metal_sequence();
    record.frame_id = impl_->current_frame_id;
    record.payload = json{{"command_buffer_id", impl_->current_command_buffer_id}}.dump();
    impl_->link_writer.append_record(record);
  }

  impl_->current_command_buffer_id = 0;
  impl_->current_submission_id = 0;
  impl_->command_buffer_sequence_begin = 0;
}

void TranslationTraceRecorder::begin_encoder(const TranslationEncoderInfo &info)
{
  if (!impl_->open) {
    return;
  }

  impl_->current_encoder_id = info.encoder_id;
  impl_->current_pass_kind = info.pass_kind;
  if (info.command_buffer_id != 0) {
    impl_->current_command_buffer_id = info.command_buffer_id;
  }
  if (info.frame_id != 0) {
    impl_->current_frame_id = info.frame_id;
  }
  if (info.d3d_sequence != 0) {
    impl_->current_d3d_sequence = info.d3d_sequence;
  }
  impl_->encoder_active = true;
  impl_->encoder_sequence_begin = 0;

  if (impl_->options.enable_metal_trace) {
    MetalTraceRecord trace_record;
    trace_record.call_kind = begin_call_kind(info.pass_kind);
    trace_record.d3d_sequence = impl_->current_d3d_sequence;
    trace_record.frame_id = impl_->current_frame_id;
    trace_record.object_id = info.encoder_id;
    if (impl_->current_command_buffer_id != 0) {
      trace_record.object_refs.push_back(impl_->current_command_buffer_id);
    }
    trace_record.translated_call_name = begin_function_name(info.pass_kind);
    trace_record.translation_link_payload =
        info.payload.empty() ? json{{"label", info.label}}.dump() : info.payload;
    trace::append_payload_text_object_refs(trace_record.translation_link_payload, trace_record.object_refs);
    impl_->metal_trace_backend.record_translated_call(trace_record);
    impl_->encoder_sequence_begin = impl_->metal_trace_backend.current_metal_sequence();
  }

  // TODO: map encoder_id and pass_kind into readable analysis records so DXMT-like render/compute/blit scopes survive review.
  // TODO: allow translation layers to opt into automatic marker emission when encoder boundaries are enough for debugging.
}

void TranslationTraceRecorder::end_encoder()
{
  if (!impl_->open || !impl_->encoder_active) {
    return;
  }

  impl_->encoder_active = false;

  if (impl_->options.enable_metal_trace) {
    MetalTraceRecord trace_record;
    trace_record.call_kind = end_call_kind(impl_->current_pass_kind);
    trace_record.d3d_sequence = impl_->current_d3d_sequence;
    trace_record.frame_id = impl_->current_frame_id;
    trace_record.object_id = impl_->current_encoder_id;
    trace_record.translated_call_name = end_function_name(impl_->current_pass_kind);
    trace_record.translation_link_payload = "{}";
    impl_->metal_trace_backend.record_translated_call(trace_record);
  }

  if (impl_->options.enable_link_sideband && impl_->encoder_sequence_begin != 0) {
    trace::TranslationLinkRecord record;
    record.record_type = "scope";
    record.scope_kind = "encoder";
    record.d3d_sequence = impl_->current_d3d_sequence;
    record.metal_sequence_begin = impl_->encoder_sequence_begin;
    record.metal_sequence_end = impl_->metal_trace_backend.current_metal_sequence();
    record.frame_id = impl_->current_frame_id;
    record.payload = json{{"encoder_id", impl_->current_encoder_id}}.dump();
    impl_->link_writer.append_record(record);
  }

  impl_->current_encoder_id = 0;
  impl_->current_pass_kind = TranslationPassKind::Unknown;
  impl_->encoder_sequence_begin = 0;
}

void TranslationTraceRecorder::emit_marker(const TranslationMarkerInfo &info)
{
  if (!impl_->open) {
    return;
  }

  if (impl_->options.enable_metal_trace) {
    MetalTraceRecord trace_record;
    trace_record.call_kind = trace::MetalCallKind::InsertDebugSignpost;
    trace_record.d3d_sequence = impl_->current_d3d_sequence;
    trace_record.frame_id = impl_->current_frame_id;
    trace_record.object_id = impl_->current_encoder_id;
    trace_record.translated_call_name = "MTLCommandEncoder.insertDebugSignpost";
    trace_record.translation_link_payload =
        info.payload.empty() ? json{{"marker_name", info.marker_name}}.dump() : info.payload;
    trace::append_payload_text_object_refs(trace_record.translation_link_payload, trace_record.object_refs);
    impl_->metal_trace_backend.record_translated_call(trace_record);
  }

  // TODO: forward marker_name into MetalTraceBackend when marker-only translated-call records become worthwhile.
  // TODO: provide a narrow adapter for D3D11UserDefinedAnnotation and similar translator-facing marker APIs.
}

void TranslationTraceRecorder::record_metal_call(const MetalTraceRecord &record)
{
  if (!impl_->open || !impl_->options.enable_metal_trace) {
    return;
  }

  MetalTraceRecord normalized = record;
  if (normalized.d3d_sequence != 0) {
    impl_->current_d3d_sequence = normalized.d3d_sequence;
  } else {
    normalized.d3d_sequence = impl_->current_d3d_sequence;
  }
  if (normalized.frame_id == 0) {
    normalized.frame_id = impl_->current_frame_id;
  }
  if (normalized.object_id == 0) {
    normalized.object_id = impl_->current_encoder_id;
  }
  if (!normalized.payload_refs_scanned) {
    trace::append_payload_text_object_refs(normalized.translation_link_payload, normalized.object_refs);
    normalized.payload_refs_scanned = true;
  }
  impl_->metal_trace_backend.record_translated_call(normalized);

  if (impl_->enable_per_call_links && impl_->options.enable_link_sideband && normalized.d3d_sequence != 0) {
    trace::TranslationLinkRecord link_record;
    link_record.record_type = "scope";
    link_record.scope_kind = "draw_to_metal_calls";
    link_record.d3d_sequence = normalized.d3d_sequence;
    link_record.metal_sequence_begin = impl_->metal_trace_backend.current_metal_sequence();
    link_record.metal_sequence_end = impl_->metal_trace_backend.current_metal_sequence();
    link_record.frame_id = normalized.frame_id;
    link_record.payload = normalized.translation_link_payload;
    impl_->link_writer.append_record(link_record);
  }
}

void TranslationTraceRecorder::append_link_record(const trace::TranslationLinkRecord &record)
{
  if (!impl_->open || !impl_->options.enable_link_sideband) {
    return;
  }

  impl_->link_writer.append_record(record);
}

void TranslationTraceRecorder::close()
{
  if (impl_->options.enable_metal_trace) {
    impl_->metal_trace_backend.end_trace();
  }

  if (impl_->options.enable_link_sideband) {
    impl_->link_writer.close();
  }

  impl_->command_buffer_active = false;
  impl_->encoder_active = false;
  impl_->bundle_writer = nullptr;
  impl_->open = false;
}

bool TranslationTraceRecorder::is_open() const noexcept
{
  return impl_ && impl_->open;
}

std::uint64_t TranslationTraceRecorder::current_metal_sequence() const noexcept
{
  if (!impl_) {
    return 0;
  }

  return impl_->metal_trace_backend.current_metal_sequence();
}

const std::string &TranslationTraceRecorder::last_error() const noexcept
{
  return impl_->last_error;
}

} // namespace apitrace::metal
