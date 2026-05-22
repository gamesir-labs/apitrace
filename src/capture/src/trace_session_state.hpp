#pragma once

#include "apitrace/capture_runtime.hpp"
#include "apitrace/trace_bundle_io.hpp"
#include "apitrace/trace_session.hpp"

#include <unordered_map>

namespace apitrace::capture::internal {

class BundleCaptureSink {
public:
  explicit BundleCaptureSink(const TraceOptions &options);

  void open_bundle();
  void write_initial_metadata();
  void finalize_bundle();

  trace::TraceBundleWriter &writer() noexcept;
  const trace::TraceBundleWriter &writer() const noexcept;

private:
  TraceOptions options_;
  trace::TraceBundleWriter writer_;

  // TODO: split callstream emission, object index emission, and checksum sealing into separate bundle phases.
  // TODO: attach bundle diagnostics so partial bundles can be explained without reading runtime state.
};

class RuntimeBootstrap {
public:
  explicit RuntimeBootstrap(const runtime::CaptureOptions &options);

  void install_entry_hooks();
  void shutdown_capture();

  runtime::CaptureRuntime &runtime() noexcept;
  const runtime::CaptureRuntime &runtime() const noexcept;

private:
  runtime::CaptureRuntime runtime_;

  // TODO: split process-entry hook installation from late-module hook extension.
  // TODO: add explicit capture lifecycle diagnostics for bootstrap failures.
};

class TraceSessionState {
public:
  explicit TraceSessionState(TraceOptions options);

  void begin();
  void end();
  void append_call_event(const trace::EventRecord &event);
  trace::AssetRecord register_asset(const trace::AssetRecord &asset);
  void record_object(const trace::ObjectRecord &object);

  bool active() const noexcept;
  const TraceOptions &options() const noexcept;

private:
  TraceOptions options_;
  BundleCaptureSink bundle_sink_;
  RuntimeBootstrap runtime_bootstrap_;
  std::unordered_map<trace::ObjectId, trace::ObjectRecord> objects_;
  bool active_ = false;

  // TODO: separate session planning from session execution once preflight validation exists.
  // TODO: track begin/end phases explicitly so future incremental capture can reject invalid transitions.
};

} // namespace apitrace::capture::internal
