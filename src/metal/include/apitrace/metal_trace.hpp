#pragma once

#include "apitrace/metal_bridge.hpp"
#include "apitrace/metal_event_types.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace apitrace::metal {

struct MetalTraceOptions {
  bool start_paused = false;
  bool emit_debug_markers = true;
  std::string trace_label;
};

struct MetalTraceRecord {
  trace::MetalCallKind call_kind = trace::MetalCallKind::Unknown;
  std::uint64_t d3d_sequence = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t object_id = 0;
  std::vector<trace::ObjectId> object_refs;
  std::vector<trace::BlobId> blob_refs;
  std::string translated_call_name;
  std::string encoder_label;
  std::string translation_link_payload;
  bool payload_refs_scanned = false;

  // TODO: attach translated resource, pipeline, and encoder arguments once the recording payload shape is defined.
  // TODO: keep translation_link_payload opaque to apitrace and let the translation layer own its schema and interpretation.
};

class MetalTraceBackend {
public:
  explicit MetalTraceBackend(MetalTraceOptions options);

  bool initialize(trace::TraceBundleWriter &bundle_writer, MetalBridge &bridge);
  void begin_trace();
  void record_translated_call(const MetalTraceRecord &record);
  void record_translated_call(MetalTraceRecord &&record);
  void record_frame_boundary(std::string_view frame_label);
  void record_command_buffer(std::string_view command_buffer_label);
  void end_trace();

  bool active() const noexcept;
  std::uint64_t current_metal_sequence() const noexcept;
  const MetalTraceOptions &options() const noexcept;
  const std::string &last_error() const noexcept;

private:
  trace::TraceBundleWriter *bundle_writer_ = nullptr;
  MetalTraceOptions options_;
  std::string last_error_;
  bool active_ = false;
  std::uint64_t last_sequence_ = 0;

  // TODO: keep D3D-side records and Metal-side records independently writable without embedding cross-link logic here.
  // TODO: split marker emission, translated-call snapshots, and command-buffer observations once Metal trace payloads are defined.
  // TODO: connect this backend to generic trace-session lifecycle hooks without pretending translated Metal records replace the primary D3D callstream.
};

} // namespace apitrace::metal
