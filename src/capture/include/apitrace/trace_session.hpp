#pragma once

#include "apitrace/capture_options.hpp"
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
  trace::AssetRecord register_asset(const trace::AssetRecord &asset);
  trace::AssetRecord register_asset(trace::AssetRecord &&asset);
  void record_object(const trace::ObjectRecord &object);

  bool active() const noexcept;
  std::uint64_t initial_call_sequence() const noexcept;
  const TraceOptions &options() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace
