#include "trace_session_state.hpp"

#include "apitrace/raw_event_codec.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace apitrace::capture::internal {

namespace {

std::uint64_t env_u64_or_default(const char *name, std::uint64_t fallback)
{
  const char *value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  char *end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  if (end == value ||
      (end && *end != '\0') ||
      parsed == std::numeric_limits<unsigned long long>::max()) {
    return fallback;
  }
  return static_cast<std::uint64_t>(parsed);
}

std::uint64_t configured_raw_commit_cadence_bytes()
{
  constexpr std::uint64_t kDefaultRawCommitCadenceBytes = 16ull * 1024ull * 1024ull;
  return env_u64_or_default(
      "APITRACE_RAW_COMMIT_BYTES",
      env_u64_or_default("APITRACE_CHECKPOINT_ASSET_BYTES", kDefaultRawCommitCadenceBytes));
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

const char *raw_asset_directory_name(trace::AssetKind kind)
{
  switch (kind) {
  case trace::AssetKind::ShaderDxbc:
  case trace::AssetKind::ShaderDxil:
  case trace::AssetKind::RootSignature:
    return "shaders";
  case trace::AssetKind::Texture:
    return "textures";
  case trace::AssetKind::Buffer:
    return "buffers";
  case trace::AssetKind::Pipeline:
    return "pipelines";
  case trace::AssetKind::ObjectIndex:
    return "objects";
  case trace::AssetKind::Analysis:
    return "analysis";
  case trace::AssetKind::Unknown:
  default:
    return ".";
  }
}

const char *raw_asset_extension(trace::AssetKind kind)
{
  switch (kind) {
  case trace::AssetKind::ShaderDxbc:
    return ".dxbc";
  case trace::AssetKind::ShaderDxil:
    return ".dxil";
  case trace::AssetKind::RootSignature:
    return ".rootsig";
  case trace::AssetKind::Texture:
    return ".texture";
  case trace::AssetKind::Buffer:
    return ".buffer";
  case trace::AssetKind::Pipeline:
    return ".pipeline.json";
  case trace::AssetKind::ObjectIndex:
    return ".json";
  case trace::AssetKind::Analysis:
    return ".jsonl";
  case trace::AssetKind::Unknown:
  default:
    return ".bin";
  }
}

std::filesystem::path raw_asset_relative_path(trace::AssetKind kind, trace::BlobId blob_id)
{
  std::ostringstream filename;
  filename << "asset-" << std::setw(16) << std::setfill('0') << blob_id << raw_asset_extension(kind);
  const std::string directory = raw_asset_directory_name(kind);
  if (directory == ".") {
    return filename.str();
  }
  return std::filesystem::path(directory) / filename.str();
}

std::uint64_t wall_clock_nanoseconds()
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void stamp_raw_event(trace::EventRecord &event)
{
  if (event.time_ns == 0) {
    event.time_ns = wall_clock_nanoseconds();
  }
}

bool append_passthrough_raw_event(
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
  return writer.append_event(header, final_jsonl_record.data(), final_jsonl_record.size());
}

bool append_passthrough_with_blob_raw_event(
    trace::raw::RawCaptureWriter &raw_writer,
    const trace::EventRecord &event,
    std::string_view final_jsonl_record,
    const std::vector<trace::TraceBundleWriter::RegisteredAssetPayload> &assets)
{
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
  raw_writer_ = std::make_unique<trace::raw::RawCaptureWriter>();
  raw_writer_->open(options_.bundle_root);
  raw_commit_cadence_bytes_ = configured_raw_commit_cadence_bytes();
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
  if (!raw_writer_) {
    return;
  }

  stamp_raw_event(event);
  const auto final_jsonl_record = trace::event_record_json(event);
  bool raw_appended = false;
  if (event.blob_refs.empty()) {
    raw_appended = append_passthrough_raw_event(*raw_writer_, event, final_jsonl_record);
  } else {
    std::vector<trace::TraceBundleWriter::RegisteredAssetPayload> assets;
    assets.reserve(event.blob_refs.size());
    {
      std::lock_guard lock(raw_staged_assets_mutex_);
      for (const auto blob_ref : event.blob_refs) {
        const auto found = raw_staged_assets_.find(blob_ref);
        if (found == raw_staged_assets_.end()) {
          return;
        }
        auto payload = std::make_shared<const std::vector<std::uint8_t>>(found->second.payload_bytes);
        assets.push_back(trace::TraceBundleWriter::RegisteredAssetPayload{found->second, std::move(payload)});
      }
    }
    raw_appended = append_passthrough_with_blob_raw_event(
        *raw_writer_,
        event,
        final_jsonl_record,
        assets);
  }
  if (raw_appended) {
    raw_writer_->flush_commit_if_needed(raw_commit_cadence_bytes_);
  }
}

void TraceSessionState::append_analysis_line(std::string_view stream_name, std::string_view json_line)
{
  bundle_sink_.writer().append_analysis_line(stream_name, json_line);
}

trace::AssetRecord TraceSessionState::stage_raw_asset(trace::AssetRecord &&asset)
{
  if (asset.blob_id == 0) {
    asset.blob_id = next_raw_staged_blob_id_++;
  } else {
    next_raw_staged_blob_id_ = std::max(next_raw_staged_blob_id_, asset.blob_id + 1);
  }
  if (asset.relative_path.empty()) {
    asset.relative_path = raw_asset_relative_path(asset.kind, asset.blob_id);
  }
  if (asset.byte_size == 0) {
    asset.byte_size = static_cast<std::uint64_t>(asset.payload_bytes.size());
  }
  asset.binary_payload = true;
  std::lock_guard lock(raw_staged_assets_mutex_);
  raw_staged_assets_[asset.blob_id] = asset;
  return asset;
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

std::uint64_t TraceSessionState::raw_commit_cadence_bytes() const noexcept
{
  return raw_commit_cadence_bytes_;
}

} // namespace apitrace::capture::internal
