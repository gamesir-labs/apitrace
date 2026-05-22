#pragma once

#include "apitrace/metal_bridge.hpp"

#include <string>
#include <string_view>

namespace apitrace::metal {

struct MetalTraceOptions {
  bool start_paused = false;
  bool emit_debug_markers = true;
  std::string trace_label;
};

struct MetalTraceRecord {
  std::string translated_call_name;
  std::string encoder_label;
  std::string translation_link_payload;

  // TODO: attach translated resource, pipeline, and encoder arguments once the recording payload shape is defined.
  // TODO: keep translation_link_payload opaque to apitrace and let the translation layer own its schema and interpretation.
};

class MetalTraceBackend {
public:
  explicit MetalTraceBackend(MetalTraceOptions options);

  bool initialize(MetalBridge &bridge);
  void begin_trace();
  void record_translated_call(const MetalTraceRecord &record);
  void record_frame_boundary(std::string_view frame_label);
  void record_command_buffer(std::string_view command_buffer_label);
  void end_trace();

  bool active() const noexcept;
  const MetalTraceOptions &options() const noexcept;
  const std::string &last_error() const noexcept;

private:
  MetalTraceOptions options_;
  std::string last_error_;
  bool active_ = false;

  // TODO: keep D3D-side records and Metal-side records independently writable without embedding cross-link logic here.
  // TODO: split marker emission, translated-call snapshots, and command-buffer observations once Metal trace payloads are defined.
  // TODO: connect this backend to generic trace-session lifecycle hooks without pretending translated Metal records replace the primary D3D callstream.
};

} // namespace apitrace::metal
