#include "trace_session_state.hpp"

#include <utility>

namespace apitrace::capture::internal {

BundleCaptureSink::BundleCaptureSink(const TraceOptions &options) : options_(options) {}

void BundleCaptureSink::open_bundle()
{
  writer_.open(options_.bundle_root);

  // TODO: create root readable files before any runtime callback can enqueue events.
  // TODO: pre-create typed asset directories so asset routing never depends on first-use side effects.
}

void BundleCaptureSink::write_initial_metadata()
{
  trace::TraceMetadata metadata;
  metadata.api = options_.api;
  metadata.producer = "apitrace";

  writer_.write_metadata(metadata);

  // TODO: decide whether metadata lives in a dedicated readable file or the first callstream record.
  // TODO: emit schema/version negotiation records before the first captured API call.
}

void BundleCaptureSink::finalize_bundle()
{
  // TODO: persist readable object index and asset index before checksum sealing.
  // TODO: finalize checksums.json only after all readable indexes and raw assets are flushed.
  writer_.close();
}

trace::TraceBundleWriter &BundleCaptureSink::writer() noexcept
{
  return writer_;
}

const trace::TraceBundleWriter &BundleCaptureSink::writer() const noexcept
{
  return writer_;
}

RuntimeBootstrap::RuntimeBootstrap(const runtime::CaptureOptions &options) : runtime_(options) {}

void RuntimeBootstrap::install_entry_hooks()
{
  runtime_.install_hooks();

  // TODO: separate initial entry hook installation from launcher/attach mode setup.
  // TODO: publish a hook plan object so tests can inspect which interception surfaces were requested.
}

void RuntimeBootstrap::shutdown_capture()
{
  // TODO: flush runtime-side pending events before bundle finalization.
  // TODO: detach or quiesce late module hooks once runtime shutdown behavior exists.
}

runtime::CaptureRuntime &RuntimeBootstrap::runtime() noexcept
{
  return runtime_;
}

const runtime::CaptureRuntime &RuntimeBootstrap::runtime() const noexcept
{
  return runtime_;
}

TraceSessionState::TraceSessionState(TraceOptions options)
    : options_(std::move(options)),
      bundle_sink_(options_),
      runtime_bootstrap_(options_.capture)
{
}

void TraceSessionState::begin()
{
  bundle_sink_.open_bundle();
  bundle_sink_.write_initial_metadata();
  runtime_bootstrap_.install_entry_hooks();

  // TODO: register root bundle files in checksums.json once hashing policy is fixed.
  // TODO: hand a bundle-facing event sink to runtime bootstrap once capture callbacks exist.
  active_ = true;
}

void TraceSessionState::end()
{
  runtime_bootstrap_.shutdown_capture();
  bundle_sink_.finalize_bundle();
  active_ = false;
}

bool TraceSessionState::active() const noexcept
{
  return active_;
}

const TraceOptions &TraceSessionState::options() const noexcept
{
  return options_;
}

} // namespace apitrace::capture::internal
