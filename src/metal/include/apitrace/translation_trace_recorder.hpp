#pragma once

#include "apitrace/metal_bridge.hpp"
#include "apitrace/metal_state.hpp"
#include "apitrace/metal_trace.hpp"
#include "apitrace/trace_bundle_io.hpp"
#include "apitrace/translation_link_writer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace apitrace::metal {

enum class TranslationPassKind {
  Unknown,
  Render,
  Compute,
  Blit,
  Present,
};

struct TranslationTraceRecorderOptions {
  bool enable_metal_trace = true;
  bool enable_link_sideband = true;
  std::string trace_label;

  // TODO: add per-producer policy knobs once DXMT-like and non-DXMT translation layers need different defaults.
};

struct TranslationCommandBufferInfo {
  std::uint64_t frame_id = 0;
  std::uint64_t command_buffer_id = 0;
  std::uint64_t submission_id = 0;
  std::uint64_t d3d_sequence = 0;
  std::string label;
  std::string payload;

  // TODO: keep payload opaque so translation layers can attach queue/chunk metadata without schema negotiation here.
};

struct TranslationEncoderInfo {
  std::uint64_t encoder_id = 0;
  std::uint64_t command_buffer_id = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t d3d_sequence = 0;
  TranslationPassKind pass_kind = TranslationPassKind::Unknown;
  std::string label;
  std::string payload;

  // TODO: support deferred-encoding metadata such as DXMT's argument-buffer sizing and pass merge hints when needed.
};

struct TranslationMarkerInfo {
  std::string marker_name;
  std::string payload;

  // TODO: support BeginEvent/EndEvent-style nesting once translation layers want richer annotation replay.
};

class TranslationTraceRecorder {
public:
  TranslationTraceRecorder();
  ~TranslationTraceRecorder();

  bool open(trace::TraceBundleWriter &bundle_writer,
            MetalBridge &bridge,
            TranslationTraceRecorderOptions options = {},
            trace::TranslationLinkStreamOptions link_options = {});
  void begin_command_buffer(const TranslationCommandBufferInfo &info);
  void end_command_buffer();
  void begin_encoder(const TranslationEncoderInfo &info);
  void end_encoder();
  void emit_marker(const TranslationMarkerInfo &info);
  void record_metal_call(const MetalTraceRecord &record);
  void record_metal_call(MetalTraceRecord &&record);
  void append_link_record(const trace::TranslationLinkRecord &record);
  void close();

  bool is_open() const noexcept;
  std::uint64_t current_metal_sequence() const noexcept;
  const std::string &last_error() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace::metal
