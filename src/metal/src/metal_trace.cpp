#include "apitrace/metal_trace.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace apitrace::metal {

namespace {

using json = nlohmann::json;

trace::MetalEventRecord to_event_record(const MetalTraceRecord &record, std::uint64_t sequence)
{
  trace::MetalEventRecord event;
  event.call_kind = record.call_kind;
  event.metal_sequence = sequence;
  event.d3d_sequence = record.d3d_sequence;
  event.frame_id = record.frame_id;
  event.object_id = record.object_id;
  event.object_refs.assign(record.object_refs.begin(), record.object_refs.end());
  event.blob_refs.assign(record.blob_refs.begin(), record.blob_refs.end());
  event.function_name = record.translated_call_name;
  if (event.function_name.empty()) {
    event.function_name = "Metal.translatedCall";
  }
  event.payload = record.translation_link_payload.empty() ? std::string("{}") : record.translation_link_payload;
  event.payload_refs_scanned = record.payload_refs_scanned;
  return event;
}

trace::MetalEventRecord to_event_record(MetalTraceRecord &&record, std::uint64_t sequence)
{
  trace::MetalEventRecord event;
  event.call_kind = record.call_kind;
  event.metal_sequence = sequence;
  event.d3d_sequence = record.d3d_sequence;
  event.frame_id = record.frame_id;
  event.object_id = record.object_id;
  event.object_refs = std::move(record.object_refs);
  event.blob_refs = std::move(record.blob_refs);
  event.function_name = std::move(record.translated_call_name);
  if (event.function_name.empty()) {
    event.function_name = "Metal.translatedCall";
  }
  event.payload = record.translation_link_payload.empty() ? std::string("{}") : std::move(record.translation_link_payload);
  event.payload_refs_scanned = record.payload_refs_scanned;
  return event;
}

} // namespace

MetalTraceBackend::MetalTraceBackend(MetalTraceOptions options)
    : options_(std::move(options))
{
}

bool MetalTraceBackend::initialize(trace::TraceBundleWriter &bundle_writer, MetalBridge &bridge)
{
  if (!bridge.ready()) {
    last_error_ = "Metal bridge must be initialized before Metal trace starts.";
    return false;
  }

  if (!bundle_writer.is_open()) {
    last_error_ = "Trace bundle writer must be open before Metal trace starts.";
    return false;
  }

  // TODO: resolve trace-sink ownership between translation-layer-driven and standalone Metal debug sessions.
  bundle_writer_ = &bundle_writer;
  last_sequence_ = 0;
  last_error_.clear();
  return true;
}

void MetalTraceBackend::begin_trace()
{
  // TODO: start a concrete Metal trace scope once translation-layer lifecycle and output-target policies are defined.
  active_ = true;
}

void MetalTraceBackend::record_translated_call(const MetalTraceRecord &record)
{
  if (!active_ || bundle_writer_ == nullptr) {
    return;
  }

  bundle_writer_->append_metal_event(to_event_record(record, ++last_sequence_));
}

void MetalTraceBackend::record_translated_call(MetalTraceRecord &&record)
{
  if (!active_ || bundle_writer_ == nullptr) {
    return;
  }

  bundle_writer_->append_metal_event(to_event_record(std::move(record), ++last_sequence_));
}

void MetalTraceBackend::record_frame_boundary(std::string_view frame_label)
{
  MetalTraceRecord record;
  record.call_kind = trace::MetalCallKind::PushDebugGroup;
  record.translated_call_name = "Metal.frameBoundary";
  record.translation_link_payload = json{{"label", std::string(frame_label)}}.dump();
  record_translated_call(record);
}

void MetalTraceBackend::record_command_buffer(std::string_view command_buffer_label)
{
  MetalTraceRecord record;
  record.call_kind = trace::MetalCallKind::CommandBufferCommit;
  record.translated_call_name = "Metal.commandBuffer";
  record.translation_link_payload = json{{"label", std::string(command_buffer_label)}}.dump();
  record_translated_call(record);
}

void MetalTraceBackend::end_trace()
{
  active_ = false;
  bundle_writer_ = nullptr;
}

bool MetalTraceBackend::active() const noexcept
{
  return active_;
}

std::uint64_t MetalTraceBackend::current_metal_sequence() const noexcept
{
  return last_sequence_;
}

const MetalTraceOptions &MetalTraceBackend::options() const noexcept
{
  return options_;
}

const std::string &MetalTraceBackend::last_error() const noexcept
{
  return last_error_;
}

} // namespace apitrace::metal
