#include "trace_session_state.hpp"

#include <algorithm>
#include <utility>
#include <vector>

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

void TraceSessionState::flush()
{
  if (!active_) {
    return;
  }
  bundle_sink_.writer().flush();
}

void TraceSessionState::seal_checkpoint()
{
  if (!active_) {
    return;
  }
  bundle_sink_.writer().seal_checkpoint();
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
  if (options_.enable_object_graph && !objects_.empty()) {
    std::vector<trace::ObjectRecord> object_records;
    object_records.reserve(objects_.size());
    for (const auto &entry : objects_) {
      object_records.push_back(entry.second);
    }
    std::sort(
        object_records.begin(),
        object_records.end(),
        [](const trace::ObjectRecord &lhs, const trace::ObjectRecord &rhs) {
          return lhs.object_id < rhs.object_id;
        });
    bundle_sink_.writer().write_object_index(object_records);
  }
  bundle_sink_.finalize_bundle();
  active_ = false;
}

void TraceSessionState::append_call_event(const trace::EventRecord &event)
{
  bundle_sink_.writer().append_call_event(event);
}

void TraceSessionState::append_call_event(trace::EventRecord &&event)
{
  bundle_sink_.writer().append_call_event(std::move(event));
}

void TraceSessionState::append_analysis_line(std::string_view stream_name, std::string_view json_line)
{
  bundle_sink_.writer().append_analysis_line(stream_name, json_line);
}

trace::AssetRecord TraceSessionState::register_asset(const trace::AssetRecord &asset)
{
  return bundle_sink_.writer().register_asset(asset);
}

trace::AssetRecord TraceSessionState::register_asset(trace::AssetRecord &&asset)
{
  return bundle_sink_.writer().register_asset(std::move(asset));
}

void TraceSessionState::record_object(const trace::ObjectRecord &object)
{
  if (object.object_id == 0) {
    return;
  }
  objects_[object.object_id] = object;
}

bool TraceSessionState::active() const noexcept
{
  return active_;
}

std::uint64_t TraceSessionState::initial_call_sequence() const noexcept
{
  return bundle_sink_.writer().initial_call_sequence();
}

const TraceOptions &TraceSessionState::options() const noexcept
{
  return options_;
}

} // namespace apitrace::capture::internal
