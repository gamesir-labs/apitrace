#include "trace_session_state.hpp"

#include "apitrace/raw_event_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace apitrace::capture::internal {

namespace {

bool event_references_captured_blob(const trace::EventRecord &event)
{
  return !event.blob_refs.empty() ||
         event.payload.find("asset-") != std::string::npos;
}

trace::raw::RawBlobKind raw_blob_kind_for_asset_kind(trace::AssetKind kind)
{
  switch (kind) {
  case trace::AssetKind::Buffer:
    return trace::raw::RawBlobKind::Buffer;
  case trace::AssetKind::ShaderDxbc:
    return trace::raw::RawBlobKind::ShaderDxbc;
  case trace::AssetKind::ShaderDxil:
    return trace::raw::RawBlobKind::ShaderDxil;
  case trace::AssetKind::RootSignature:
    return trace::raw::RawBlobKind::RootSignature;
  default:
    return trace::raw::RawBlobKind::Unknown;
  }
}

void append_passthrough_raw_event(
    trace::raw::RawCaptureWriter &writer,
    const trace::EventRecord &event,
    std::string_view final_jsonl_record)
{
  trace::raw::RawEventHeader header;
  header.sequence = event.callsite.sequence;
  header.timestamp_or_monotonic_counter = event.time_ns;
  header.opcode = static_cast<std::uint32_t>(trace::raw::RawEventOpcode::PassthroughFinalJson);
  header.result_or_flags = static_cast<std::uint32_t>(event.callsite.result_code);
  header.payload_len = final_jsonl_record.size();
  writer.append_event(header, final_jsonl_record.data(), final_jsonl_record.size());
}

bool append_passthrough_with_blob_raw_event(
    trace::raw::RawCaptureWriter &raw_writer,
    trace::TraceBundleWriter &bundle_writer,
    const trace::EventRecord &event,
    std::string_view final_jsonl_record)
{
  const auto assets = bundle_writer.registered_asset_payloads_for_blob_refs(event.blob_refs);
  if (assets.size() != event.blob_refs.size()) {
    return false;
  }

  std::vector<trace::raw::PassthroughBlobDescriptor> descriptors;
  descriptors.reserve(assets.size());
  for (const auto &entry : assets) {
    if (entry.asset.blob_id == 0 ||
        entry.asset.relative_path.empty() ||
        !entry.payload) {
      return false;
    }
    const auto raw_kind = raw_blob_kind_for_asset_kind(entry.asset.kind);
    const auto raw_blob_id = raw_writer.append_blob(
        entry.payload->empty() ? nullptr : entry.payload->data(),
        static_cast<std::uint64_t>(entry.payload->size()),
        static_cast<std::uint32_t>(raw_kind),
        event.callsite.sequence);
    if (raw_blob_id == trace::raw::kInvalidRawBlobId) {
      return false;
    }

    trace::raw::PassthroughBlobDescriptor descriptor;
    descriptor.provisional_asset_path = entry.asset.relative_path.generic_string();
    descriptor.final_blob_id = entry.asset.blob_id;
    descriptor.raw_blob_id = raw_blob_id;
    descriptor.raw_blob_kind = static_cast<std::uint32_t>(raw_kind);
    descriptor.debug_name = entry.asset.debug_name;
    descriptors.push_back(std::move(descriptor));
  }

  const auto payload = trace::raw::encode_passthrough_with_blob_payload(final_jsonl_record, descriptors);
  trace::raw::RawEventHeader header;
  header.sequence = event.callsite.sequence;
  header.timestamp_or_monotonic_counter = event.time_ns;
  header.opcode = static_cast<std::uint32_t>(trace::raw::RawEventOpcode::PassthroughWithBlob);
  header.result_or_flags = static_cast<std::uint32_t>(event.callsite.result_code);
  header.payload_len = payload.size();
  return raw_writer.append_event(header, payload.data(), payload.size());
}

} // namespace

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
  if (options_.capture.raw_format_reserved) {
    raw_writer_ = std::make_unique<trace::raw::RawCaptureWriter>();
    raw_writer_->open(options_.bundle_root);
  }
  bundle_sink_.write_initial_metadata();
  runtime_bootstrap_.install_entry_hooks();

  // TODO: register root bundle files in checksums.json once hashing policy is fixed.
  // TODO: hand a bundle-facing event sink to runtime bootstrap once capture callbacks exist.
  active_ = true;
}

void TraceSessionState::end()
{
  runtime_bootstrap_.shutdown_capture();
  if (raw_writer_) {
    raw_writer_->flush_commit();
    raw_writer_->close();
    raw_writer_.reset();
  }
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
  trace::EventRecord copy = event;
  append_call_event(std::move(copy));
}

void TraceSessionState::append_call_event(trace::EventRecord &&event)
{
  auto &writer = bundle_sink_.writer();
  if (!raw_writer_) {
    writer.append_call_event(std::move(event));
    return;
  }

  event = writer.prepare_call_event(std::move(event));
  if (!event_references_captured_blob(event)) {
    const auto final_jsonl_record = trace::event_record_json(event);
    append_passthrough_raw_event(*raw_writer_, event, final_jsonl_record);
    writer.append_prepared_call_event(std::move(event), final_jsonl_record);
  } else {
    const auto final_jsonl_record = trace::event_record_json(event);
    append_passthrough_with_blob_raw_event(*raw_writer_, writer, event, final_jsonl_record);
    writer.append_prepared_call_event(std::move(event), final_jsonl_record);
  }
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

trace::raw::RawCaptureWriter *TraceSessionState::raw_capture_writer() noexcept
{
  return raw_writer_.get();
}

const trace::raw::RawCaptureWriter *TraceSessionState::raw_capture_writer() const noexcept
{
  return raw_writer_.get();
}

} // namespace apitrace::capture::internal
