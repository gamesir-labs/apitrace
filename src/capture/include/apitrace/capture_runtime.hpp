#pragma once

#include "apitrace/capture_options.hpp"
#include "apitrace/trace_session.hpp"

#include <memory>
#include <string_view>

namespace apitrace::runtime {

class CaptureRuntime {
public:
  explicit CaptureRuntime(CaptureOptions options);
  ~CaptureRuntime();

  void install_hooks();
  void extend_hooks_for_module(std::string_view module_name);

  bool armed() const noexcept;
  const CaptureOptions &options() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

TraceSession *ensure_process_trace_session(trace::ApiKind api);
TraceSession *current_process_trace_session() noexcept;
void seal_process_trace_session_checkpoint() noexcept;
void shutdown_process_trace_session() noexcept;

} // namespace apitrace::runtime
