#include "apitrace/translation_trace_recorder.hpp"

#include <memory>
#include <utility>

namespace apitrace::metal {

struct TranslationTraceRecorder::Impl {
  TranslationTraceRecorderOptions options;
  MetalTraceBackend metal_trace_backend{MetalTraceOptions{}};
  trace::TranslationLinkWriter link_writer;
  std::string last_error;
  bool open = false;
  bool command_buffer_active = false;
  bool encoder_active = false;

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
  MetalTraceOptions metal_trace_options;
  metal_trace_options.start_paused = false;
  metal_trace_options.emit_debug_markers = true;
  metal_trace_options.trace_label = impl_->options.trace_label;
  impl_->metal_trace_backend = MetalTraceBackend(std::move(metal_trace_options));

  if (impl_->options.enable_metal_trace) {
    if (!impl_->metal_trace_backend.initialize(bridge)) {
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

  impl_->command_buffer_active = true;

  if (impl_->options.enable_link_sideband) {
    trace::TranslationLinkRecord record;
    record.record_type = "command_buffer_begin";
    record.payload = info.payload;
    impl_->link_writer.append_record(record);
  }

  // TODO: map frame_id, command_buffer_id, and submission_id into readable sideband fields instead of dropping them.
  // TODO: expose an explicit chunk/queue-oriented helper for DXMT-style command queue integration if multiple translators converge on the same pattern.
}

void TranslationTraceRecorder::end_command_buffer()
{
  if (!impl_->open || !impl_->command_buffer_active) {
    return;
  }

  impl_->command_buffer_active = false;

  if (impl_->options.enable_link_sideband) {
    trace::TranslationLinkRecord record;
    record.record_type = "command_buffer_end";
    impl_->link_writer.append_record(record);
  }
}

void TranslationTraceRecorder::begin_encoder(const TranslationEncoderInfo &info)
{
  if (!impl_->open) {
    return;
  }

  impl_->encoder_active = true;

  if (impl_->options.enable_link_sideband) {
    trace::TranslationLinkRecord record;
    record.record_type = "encoder_begin";
    record.payload = info.payload;
    impl_->link_writer.append_record(record);
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

  if (impl_->options.enable_link_sideband) {
    trace::TranslationLinkRecord record;
    record.record_type = "encoder_end";
    impl_->link_writer.append_record(record);
  }
}

void TranslationTraceRecorder::emit_marker(const TranslationMarkerInfo &info)
{
  if (!impl_->open) {
    return;
  }

  if (impl_->options.enable_link_sideband) {
    trace::TranslationLinkRecord record;
    record.record_type = "marker";
    record.payload = info.payload.empty() ? info.marker_name : info.payload;
    impl_->link_writer.append_record(record);
  }

  // TODO: forward marker_name into MetalTraceBackend when marker-only translated-call records become worthwhile.
  // TODO: provide a narrow adapter for D3D11UserDefinedAnnotation and similar translator-facing marker APIs.
}

void TranslationTraceRecorder::record_metal_call(const MetalTraceRecord &record)
{
  if (!impl_->open || !impl_->options.enable_metal_trace) {
    return;
  }

  impl_->metal_trace_backend.record_translated_call(record);
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
  impl_->open = false;
}

bool TranslationTraceRecorder::is_open() const noexcept
{
  return impl_ && impl_->open;
}

const std::string &TranslationTraceRecorder::last_error() const noexcept
{
  return impl_->last_error;
}

} // namespace apitrace::metal
