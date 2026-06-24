#include "trace_session_state.hpp"

#include "apitrace/raw_event_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace apitrace::capture::internal {

namespace {

using json = nlohmann::json;

std::optional<std::uint64_t> json_u64(const json &value, const char *key)
{
  if (!value.is_object() || !value.contains(key) || !value[key].is_number_unsigned()) {
    return std::nullopt;
  }
  return value[key].get<std::uint64_t>();
}

std::optional<std::uint32_t> json_u32(const json &value, const char *key)
{
  const auto number = json_u64(value, key);
  if (!number || *number > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*number);
}

std::uint64_t first_object_ref(const trace::EventRecord &event)
{
  return event.object_refs.empty() ? 0 : event.object_refs.front();
}

std::uint64_t third_object_ref(const trace::EventRecord &event)
{
  return event.object_refs.size() < 3 ? 0 : event.object_refs[2];
}

trace::raw::RawBlobKind raw_blob_kind_for_asset(trace::AssetKind kind)
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

trace::raw::RawEventHeader raw_header_for(
    const trace::EventRecord &event,
    trace::raw::RawEventOpcode opcode,
    std::uint64_t payload_size)
{
  trace::raw::RawEventHeader header;
  header.sequence = event.callsite.sequence;
  header.thread_id = 0;
  header.timestamp_or_monotonic_counter = event.time_ns;
  header.opcode = static_cast<std::uint32_t>(opcode);
  header.result_or_flags = static_cast<std::uint32_t>(event.callsite.result_code);
  header.payload_len = payload_size;
  return header;
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
    raw_writer_.open(options_.bundle_root);
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
  if (raw_writer_.is_open()) {
    raw_writer_.flush_commit();
    raw_writer_.close();
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
  if (!raw_writer_.is_open()) {
    bundle_sink_.writer().append_call_event(event);
    return;
  }
  auto prepared = bundle_sink_.writer().prepare_call_event(event);
  append_raw_dual_event(prepared);
  bundle_sink_.writer().append_prepared_call_event(std::move(prepared));
}

void TraceSessionState::append_call_event(trace::EventRecord &&event)
{
  if (!raw_writer_.is_open()) {
    bundle_sink_.writer().append_call_event(std::move(event));
    return;
  }
  event = bundle_sink_.writer().prepare_call_event(std::move(event));
  append_raw_dual_event(event);
  bundle_sink_.writer().append_prepared_call_event(std::move(event));
}

void TraceSessionState::append_analysis_line(std::string_view stream_name, std::string_view json_line)
{
  bundle_sink_.writer().append_analysis_line(stream_name, json_line);
}

trace::AssetRecord TraceSessionState::register_asset(const trace::AssetRecord &asset)
{
  const auto raw_blob_id = append_raw_blob_for_asset(asset, 0);
  auto registered = bundle_sink_.writer().register_asset(asset);
  if (raw_blob_id != trace::raw::kInvalidRawBlobId && registered.blob_id != asset.blob_id) {
    raw_blob_ids_by_asset_[registered.blob_id] = raw_blob_id;
  }
  return registered;
}

trace::AssetRecord TraceSessionState::register_asset(trace::AssetRecord &&asset)
{
  const auto original_blob_id = asset.blob_id;
  const auto raw_blob_id = append_raw_blob_for_asset(asset, 0);
  auto registered = bundle_sink_.writer().register_asset(std::move(asset));
  if (raw_blob_id != trace::raw::kInvalidRawBlobId && registered.blob_id != original_blob_id) {
    raw_blob_ids_by_asset_[registered.blob_id] = raw_blob_id;
  }
  return registered;
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

std::uint64_t TraceSessionState::append_raw_blob_for_asset(
    const trace::AssetRecord &asset,
    std::uint64_t producing_sequence)
{
  if (!raw_writer_.is_open() || asset.blob_id == 0 || asset.payload_bytes.empty()) {
    return trace::raw::kInvalidRawBlobId;
  }
  if (const auto existing = raw_blob_ids_by_asset_.find(asset.blob_id); existing != raw_blob_ids_by_asset_.end()) {
    return existing->second;
  }
  const auto raw_blob_id = raw_writer_.append_blob(
      asset.payload_bytes.data(),
      asset.payload_bytes.size(),
      static_cast<std::uint32_t>(raw_blob_kind_for_asset(asset.kind)),
      producing_sequence);
  if (raw_blob_id != trace::raw::kInvalidRawBlobId) {
    raw_blob_ids_by_asset_[asset.blob_id] = raw_blob_id;
  }
  return raw_blob_id;
}

void TraceSessionState::append_raw_dual_event(const trace::EventRecord &event)
{
  if (!raw_writer_.is_open()) {
    return;
  }

  std::vector<std::uint8_t> payload;
  std::optional<trace::raw::RawEventOpcode> opcode;
  const auto event_payload = json::parse(event.payload, nullptr, false);

  if (event.kind == trace::EventKind::ObjectCreate &&
      event.object_kind == trace::ObjectKind::Resource &&
      !event_payload.is_discarded()) {
    const auto dimension = json_u64(event_payload, "dimension");
    const auto width = json_u64(event_payload, "width");
    const auto height = json_u32(event_payload, "height");
    const auto depth_or_array_size = json_u32(event_payload, "depth_or_array_size");
    const auto mip_levels = json_u32(event_payload, "mip_levels");
    const auto format = json_u32(event_payload, "format");
    const auto flags = json_u32(event_payload, "flags");
    const auto initial_state = json_u32(event_payload, "initial_state");
    if (dimension && width && height && depth_or_array_size && *depth_or_array_size <= UINT16_MAX &&
        mip_levels && *mip_levels <= UINT16_MAX && format && flags && initial_state) {
      opcode = trace::raw::RawEventOpcode::ResourceCreate;
      payload = trace::raw::encode_resource_create_payload(
          event.parent_object_id,
          event.object_id,
          *dimension,
          *width,
          *height,
          static_cast<std::uint16_t>(*depth_or_array_size),
          static_cast<std::uint16_t>(*mip_levels),
          *format,
          *flags,
          *initial_state,
          event.object_debug_name);
    }
  } else if (event.kind == trace::EventKind::Call &&
             event.callsite.function_name == "ID3D12Resource::Unmap" &&
             !event.blob_refs.empty() &&
             !event_payload.is_discarded()) {
    const auto written_begin = json_u64(event_payload, "written_begin");
    const auto written_end = json_u64(event_payload, "written_end");
    const auto raw_blob = raw_blob_ids_by_asset_.find(event.blob_refs.front());
    const auto raw_blob_id = raw_blob == raw_blob_ids_by_asset_.end() ? trace::raw::kInvalidRawBlobId : raw_blob->second;
    if (written_begin && written_end && raw_blob_id != trace::raw::kInvalidRawBlobId) {
      opcode = trace::raw::RawEventOpcode::ResourceUnmap;
      payload = trace::raw::encode_resource_unmap_payload(
          first_object_ref(event),
          raw_blob_id,
          *written_begin,
          *written_end);
    }
  } else if (event.kind == trace::EventKind::Call &&
             event.callsite.function_name == "ID3D12Device::CreateGraphicsPipelineState" &&
             event.blob_refs.size() >= 2 &&
             !event_payload.is_discarded()) {
    const auto root_signature = json_u64(event_payload, "root_signature_object_id");
    const auto node_mask = json_u32(event_payload, "node_mask");
    const auto flags = json_u32(event_payload, "flags");
    const auto vs_size = event_payload.contains("vs") && event_payload["vs"].is_object()
                             ? json_u64(event_payload["vs"], "bytecode_size")
                             : std::nullopt;
    const auto ps_size = event_payload.contains("ps") && event_payload["ps"].is_object()
                             ? json_u64(event_payload["ps"], "bytecode_size")
                             : std::nullopt;
    const auto vs_raw_blob = raw_blob_ids_by_asset_.find(event.blob_refs[0]);
    const auto ps_raw_blob = raw_blob_ids_by_asset_.find(event.blob_refs[1]);
    const auto vs_raw_blob_id = vs_raw_blob == raw_blob_ids_by_asset_.end() ? trace::raw::kInvalidRawBlobId : vs_raw_blob->second;
    const auto ps_raw_blob_id = ps_raw_blob == raw_blob_ids_by_asset_.end() ? trace::raw::kInvalidRawBlobId : ps_raw_blob->second;
    if (root_signature && node_mask && flags && vs_size && ps_size &&
        vs_raw_blob_id != trace::raw::kInvalidRawBlobId &&
        ps_raw_blob_id != trace::raw::kInvalidRawBlobId) {
      opcode = trace::raw::RawEventOpcode::GraphicsPipelineCreate;
      payload = trace::raw::encode_graphics_pipeline_create_payload(
          first_object_ref(event),
          *root_signature,
          third_object_ref(event),
          vs_raw_blob_id,
          *vs_size,
          ps_raw_blob_id,
          *ps_size,
          *node_mask,
          *flags,
          event_payload.value("requires_dxmt_backend", false));
    }
  } else if (event.kind == trace::EventKind::Call &&
             event.callsite.function_name == "ID3D12GraphicsCommandList::DrawInstanced" &&
             !event_payload.is_discarded()) {
    const auto vertex_count = json_u32(event_payload, "vertex_count_per_instance");
    const auto instance_count = json_u32(event_payload, "instance_count");
    const auto start_vertex = json_u32(event_payload, "start_vertex_location");
    const auto start_instance = json_u32(event_payload, "start_instance_location");
    if (vertex_count && instance_count && start_vertex && start_instance) {
      opcode = trace::raw::RawEventOpcode::DrawInstanced;
      payload = trace::raw::encode_draw_instanced_payload(
          first_object_ref(event),
          *vertex_count,
          *instance_count,
          *start_vertex,
          *start_instance);
    }
  } else if (event.kind == trace::EventKind::Call &&
             event.callsite.function_name == "ID3D12GraphicsCommandList::Dispatch" &&
             !event_payload.is_discarded()) {
    const auto x = json_u32(event_payload, "thread_group_count_x");
    const auto y = json_u32(event_payload, "thread_group_count_y");
    const auto z = json_u32(event_payload, "thread_group_count_z");
    if (x && y && z) {
      opcode = trace::raw::RawEventOpcode::Dispatch;
      payload = trace::raw::encode_dispatch_payload(first_object_ref(event), *x, *y, *z);
    }
  } else if (event.kind == trace::EventKind::Call &&
             event.callsite.function_name == "IDXGISwapChain::Present" &&
             !event_payload.is_discarded()) {
    const auto frame_index = json_u64(event_payload, "frame_index");
    const auto sync_interval = json_u32(event_payload, "sync_interval");
    const auto flags = json_u32(event_payload, "flags");
    if (frame_index && sync_interval && flags) {
      opcode = trace::raw::RawEventOpcode::PresentCall;
      payload = trace::raw::encode_present_payload(first_object_ref(event), *frame_index, *sync_interval, *flags);
    }
  } else if (event.kind == trace::EventKind::Boundary && !event_payload.is_discarded()) {
    const auto label = event_payload.value("label", std::string());
    if (label == "FrameBegin" || label == "FrameEnd") {
      if (const auto frame_index = json_u64(event_payload, "frame_index")) {
        opcode = label == "FrameBegin" ? trace::raw::RawEventOpcode::FrameBegin : trace::raw::RawEventOpcode::FrameEnd;
        payload = trace::raw::encode_frame_boundary_payload(*frame_index);
      }
    } else if (label == "Present" && event.boundary == trace::BoundaryKind::Present) {
      const auto frame_index = json_u64(event_payload, "frame_index");
      const auto sync_interval = json_u32(event_payload, "sync_interval");
      const auto flags = json_u32(event_payload, "flags");
      if (frame_index && sync_interval && flags) {
        opcode = trace::raw::RawEventOpcode::PresentBoundary;
        payload = trace::raw::encode_present_payload(first_object_ref(event), *frame_index, *sync_interval, *flags);
      }
    }
  }

  if (!opcode) {
    opcode = trace::raw::RawEventOpcode::PassthroughFinalJson;
    payload = trace::raw::encode_passthrough_final_json_payload(trace::event_record_json(event));
  }

  const auto header = raw_header_for(event, *opcode, payload.size());
  raw_writer_.append_event(header, payload.data(), payload.size());
}

} // namespace apitrace::capture::internal
