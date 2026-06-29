#pragma once

#include "apitrace/capture_options.hpp"
#include "apitrace/raw_capture_io.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <memory>
#include <string_view>

namespace apitrace {

class TraceSession {
public:
  explicit TraceSession(TraceOptions options);
  ~TraceSession();

  void begin();
  void end();
  void flush();
  void seal_checkpoint();
  void append_call_event(const trace::EventRecord &event);
  void append_call_event(trace::EventRecord &&event);
  void append_analysis_line(std::string_view stream_name, std::string_view json_line);
  trace::AssetRecord stage_raw_asset(trace::AssetRecord &&asset);
  void record_object(const trace::ObjectRecord &object);

  bool active() const noexcept;
  std::uint64_t initial_call_sequence() const noexcept;
  const TraceOptions &options() const noexcept;
  trace::raw::RawCaptureWriter *raw_capture_writer() noexcept;
  const trace::raw::RawCaptureWriter *raw_capture_writer() const noexcept;
  std::uint64_t raw_commit_cadence_bytes() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace
