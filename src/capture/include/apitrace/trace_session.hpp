#pragma once

#include "apitrace/capture_options.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <memory>

namespace apitrace {

class TraceSession {
public:
  explicit TraceSession(TraceOptions options);
  ~TraceSession();

  void begin();
  void end();
  void append_call_event(const trace::EventRecord &event);
  trace::AssetRecord register_asset(const trace::AssetRecord &asset);
  void record_object(const trace::ObjectRecord &object);

  bool active() const noexcept;
  const TraceOptions &options() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace
