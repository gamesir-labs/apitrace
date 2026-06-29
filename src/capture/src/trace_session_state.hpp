#pragma once

#include "apitrace/capture_runtime.hpp"
#include "apitrace/raw_capture_io.hpp"
#include "apitrace/trace_bundle_io.hpp"
#include "apitrace/trace_session.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>
#include <string_view>
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
  TraceOptions options_;
  BundleCaptureSink bundle_sink_;
  RuntimeBootstrap runtime_bootstrap_;
  std::unique_ptr<trace::raw::RawCaptureWriter> raw_writer_;
  std::unordered_map<trace::ObjectId, trace::ObjectRecord> objects_;
  std::unordered_map<trace::BlobId, trace::AssetRecord> raw_staged_assets_;
  std::mutex raw_staged_assets_mutex_;
  std::uint64_t raw_commit_cadence_bytes_ = 0;
  trace::BlobId next_raw_staged_blob_id_ = 1;
  bool active_ = false;

  // TODO: separate session planning from session execution once preflight validation exists.
  // TODO: track begin/end phases explicitly so future incremental capture can reject invalid transitions.
};

} // namespace apitrace::capture::internal
