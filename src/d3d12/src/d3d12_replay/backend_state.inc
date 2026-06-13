D3D12ReplayBackend::D3D12ReplayBackend() = default;

D3D12ReplayBackend::~D3D12ReplayBackend() = default;

bool D3D12ReplayBackend::resolve_descriptor_binding(
    std::uint64_t descriptor,
    bool gpu_descriptor,
    DescriptorBinding &binding,
    std::string &error) const
{
  return resolve_descriptor_binding_at(
      descriptor,
      gpu_descriptor,
      std::numeric_limits<std::uint64_t>::max(),
      binding,
      error);
}

bool D3D12ReplayBackend::resolve_descriptor_binding_at(
    std::uint64_t descriptor,
    bool gpu_descriptor,
    std::uint64_t sequence,
    DescriptorBinding &binding,
    std::string &error) const
{
  std::vector<trace::ObjectId> heap_object_ids;
  heap_object_ids.reserve(descriptor_heaps_.size());
  for (const auto &[heap_object_id, heap] : descriptor_heaps_) {
    (void)heap;
    heap_object_ids.push_back(heap_object_id);
  }
  return resolve_descriptor_binding_in_heaps_at(descriptor, gpu_descriptor, heap_object_ids, sequence, binding, error);
}

bool D3D12ReplayBackend::resolve_descriptor_binding_in_heaps(
    std::uint64_t descriptor,
    bool gpu_descriptor,
    const std::vector<trace::ObjectId> &heap_object_ids,
    DescriptorBinding &binding,
    std::string &error) const
{
  return resolve_descriptor_binding_in_heaps_at(
      descriptor,
      gpu_descriptor,
      heap_object_ids,
      std::numeric_limits<std::uint64_t>::max(),
      binding,
      error);
}

bool D3D12ReplayBackend::resolve_descriptor_binding_in_heaps_at(
    std::uint64_t descriptor,
    bool gpu_descriptor,
    const std::vector<trace::ObjectId> &heap_object_ids,
    std::uint64_t sequence,
    DescriptorBinding &binding,
    std::string &error) const
{
  binding = DescriptorBinding{};
  binding.descriptor = descriptor;
  if (descriptor == 0) {
    return true;
  }
  const DescriptorHeapSemanticState *best_heap = nullptr;
  trace::ObjectId best_heap_object_id = 0;
  std::uint64_t best_descriptor_index = 0;
  for (const auto heap_object_id : heap_object_ids) {
    const auto heap_it = descriptor_heaps_.find(heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      continue;
    }
    const auto &heap = heap_it->second;
    if (heap.create_sequence > sequence) {
      continue;
    }
    const auto heap_start = gpu_descriptor ? heap.gpu_start : heap.cpu_start;
    if (heap_start == 0 || heap.descriptor_size == 0 || heap.num_descriptors == 0 || descriptor < heap_start) {
      continue;
    }
    const auto offset = descriptor - heap_start;
    if (offset % heap.descriptor_size != 0) {
      continue;
    }
    const auto descriptor_index = offset / heap.descriptor_size;
    if (descriptor_index >= heap.num_descriptors) {
      continue;
    }
    if (!best_heap || heap.create_sequence > best_heap->create_sequence) {
      best_heap = &heap;
      best_heap_object_id = heap_object_id;
      best_descriptor_index = descriptor_index;
    }
  }
  if (best_heap) {
    binding.heap_object_id = best_heap_object_id;
    binding.heap_type = best_heap->type;
    binding.descriptor_index = static_cast<std::uint32_t>(best_descriptor_index);
    return true;
  }
  error = "descriptor handle does not map to a captured descriptor heap";
  return false;
}

bool D3D12ReplayBackend::resolve_gpu_virtual_address(
    std::uint64_t gpu_virtual_address,
    GpuVirtualAddressBinding &binding,
    std::string &error) const
{
  return resolve_gpu_virtual_address_at(gpu_virtual_address, std::numeric_limits<std::uint64_t>::max(), binding, error);
}

bool D3D12ReplayBackend::resolve_gpu_virtual_address_at(
    std::uint64_t gpu_virtual_address,
    std::uint64_t sequence,
    GpuVirtualAddressBinding &binding,
    std::string &error) const
{
  binding = GpuVirtualAddressBinding{};
  binding.gpu_virtual_address = gpu_virtual_address;
  if (gpu_virtual_address == 0) {
    return true;
  }
  const ResourceSemanticState *best_resource = nullptr;
  trace::ObjectId best_resource_object_id = 0;
  for (const auto &[resource_object_id, resource] : resources_) {
    if (resource.gpu_virtual_address == 0 ||
        resource.create_sequence > sequence ||
        gpu_virtual_address < resource.gpu_virtual_address) {
      continue;
    }
    const auto offset = gpu_virtual_address - resource.gpu_virtual_address;
    if (offset >= resource.width) {
      continue;
    }
    if (!best_resource || resource.create_sequence > best_resource->create_sequence) {
      best_resource = &resource;
      best_resource_object_id = resource_object_id;
    }
  }
  if (best_resource) {
    binding.resource_object_id = best_resource_object_id;
    binding.offset = gpu_virtual_address - best_resource->gpu_virtual_address;
    binding.resolved = true;
    return true;
  }
  binding.resolved = false;
  return true;
}

bool validate_gpu_virtual_address_resolve_payload(
    const trace::EventRecord &event,
    const std::string &payload_label,
    const json &payload,
    const D3D12ReplayBackend::GpuVirtualAddressBinding &binding,
    std::string &error)
{
  if (payload_has(payload, "gpuva_resolve_status")) {
    const auto status = payload_string(payload, "gpuva_resolve_status");
    if (status == "null") {
      if (binding.gpu_virtual_address != 0) {
        error = record_prefix(event) + ": " + payload_label +
                " records null GPUVA resolve for a non-null address";
        return false;
      }
      return true;
    }
    if (status == "mapped") {
      if (!binding.resolved || binding.resource_object_id == 0) {
        error = record_prefix(event) + ": " + payload_label +
                " records mapped GPUVA resolve but replay resolved no resource";
        return false;
      }
    } else if (status == "unmapped") {
      if (binding.resolved && binding.resource_object_id != 0) {
        error = record_prefix(event) + ": " + payload_label +
                " records unmapped GPUVA resolve but replay resolved a resource";
        return false;
      }
      return true;
    } else if (!status.empty()) {
      error = record_prefix(event) + ": " + payload_label +
              " has unknown gpuva_resolve_status " + status;
      return false;
    }
  }

  if (payload_has(payload, "resolved_resource_object_id")) {
    const auto recorded_resource = payload_u64(payload, "resolved_resource_object_id");
    if (recorded_resource != binding.resource_object_id) {
      error = record_prefix(event) + ": " + payload_label +
              " resolved_resource_object_id mismatch: recorded " +
              std::to_string(recorded_resource) + ", replay resolved " +
              std::to_string(binding.resource_object_id);
      return false;
    }
  }
  if (payload_has(payload, "resolved_resource_offset")) {
    const auto recorded_offset = payload_u64(payload, "resolved_resource_offset");
    if (recorded_offset != binding.offset) {
      error = record_prefix(event) + ": " + payload_label +
              " resolved_resource_offset mismatch: recorded " +
              std::to_string(recorded_offset) + ", replay resolved " +
              std::to_string(binding.offset);
      return false;
    }
  }
  return true;
}

bool D3D12ReplayBackend::parse_texture_copy_location_payload(
    const trace::EventRecord &event,
    const std::string &payload_name,
    TextureCopyLocation &location,
    std::string &error) const
{
  json payload;
  if (!payload_to_json(event, payload, error)) {
    return false;
  }
  const auto value_it = payload.find(payload_name);
  if (value_it == payload.end()) {
    error = record_prefix(event) + ": missing texture copy location " + payload_name;
    return false;
  }
  const auto &value = *value_it;
  location = TextureCopyLocation{};
  if (value.is_null()) {
    return true;
  }
  if (!value.is_object()) {
    error = record_prefix(event) + ": texture copy location must be an object";
    return false;
  }
  if (!require_payload_key(event, value, "resource_object_id", error) ||
      !require_payload_key(event, value, "type", error)) {
    return false;
  }
  location.resource_object_id = json_object_id(value["resource_object_id"]);
  location.type = payload_u32(value, "type");
  if (location.type == 0) {
    if (!payload_has(value, "subresource_index")) {
      error = record_prefix(event) + ": texture copy subresource location missing subresource_index";
      return false;
    }
    location.subresource_index = payload_u32(value, "subresource_index");
  } else if (location.type == 1) {
    const auto footprint = value.find("placed_footprint");
    if (footprint == value.end() || !footprint->is_object()) {
      error = record_prefix(event) + ": texture copy placed location missing placed_footprint";
      return false;
    }
    location.footprint_offset = payload_u64(*footprint, "offset");
    location.footprint_format = payload_u32(*footprint, "format");
    location.footprint_width = payload_u32(*footprint, "width");
    location.footprint_height = payload_u32(*footprint, "height");
    location.footprint_depth = payload_u32(*footprint, "depth");
    location.footprint_row_pitch = payload_u32(*footprint, "row_pitch");
    if (location.footprint_width == 0 || location.footprint_height == 0 || location.footprint_depth == 0 ||
        location.footprint_row_pitch == 0) {
      error = record_prefix(event) + ": texture copy placed footprint is incomplete";
      return false;
    }
  } else {
    error = record_prefix(event) + ": unsupported texture copy location type";
    return false;
  }
  return true;
}

bool D3D12ReplayBackend::initialize(const trace::TraceBundleReader &reader)
{
  last_error_.clear();
  objects_.clear();
  submissions_.clear();
  initialized_ = false;
  commands_replayed_ = 0;
  frames_seen_ = 0;
  presents_seen_ = 0;
  pipeline_assets_read_ = 0;
  semantic_calls_seen_ = 0;
  draw_calls_seen_ = 0;
  dispatch_calls_seen_ = 0;
  last_sequence_ = 0;
  bundle_root_ = reader.layout().root_path;
  present_frames_.clear();
  present_semantics_.clear();
  frame_semantics_.clear();
  command_lists_.clear();
  command_queues_.clear();
  command_allocators_.clear();
  devices_.clear();
  descriptor_heaps_.clear();
  descriptors_.clear();
  samplers_.clear();
  descriptor_copies_.clear();
  command_signatures_.clear();
  heaps_.clear();
  query_heaps_.clear();
  resources_.clear();
  replay_commands_.clear();
  blob_paths_.clear();

  if (reader.metadata().api != trace::ApiKind::D3D12) {
    last_error_ = "trace bundle is not a D3D12 bundle";
    return false;
  }

  for (const auto &object : reader.objects()) {
    D3D12TrackedObject tracked;
    tracked.object_id = object.object_id;
    tracked.kind = object.kind;
    tracked.parent_object_id = object.parent_object_id;
    tracked.debug_name = object.debug_name;
    objects_.track(tracked);
    if (object.kind == trace::ObjectKind::CommandList) {
      command_lists_.emplace(object.object_id, CommandListSemanticState{});
    }
  }

  for (const auto &asset : reader.assets()) {
    if (asset.blob_id != 0 && !asset.relative_path.empty()) {
      blob_paths_[asset.blob_id] = asset.relative_path;
    }
    if (!is_pipeline_asset_path(asset.relative_path)) {
      continue;
    }
    if (!read_pipeline_asset_json(reader.layout().root_path, asset.relative_path, last_error_)) {
      return false;
    }
    ++pipeline_assets_read_;
  }

  if (!index_present_frames(reader)) {
    return false;
  }
  if (!index_present_semantics(reader)) {
    return false;
  }
  if (!validate_present_boundaries(reader)) {
    return false;
  }
  if (!validate_frame_boundaries(reader)) {
    return false;
  }

  initialized_ = true;
  return true;
}

bool D3D12ReplayBackend::index_present_frame_event(const trace::EventRecord &event, bool replace_existing)
{
  if (event.kind != trace::EventKind::ResourceBlob) {
    return true;
  }

  json payload;
  if (!payload_to_json(event, payload, last_error_)) {
    return false;
  }

  const json &frame_payload = payload.contains("payload") && payload["payload"].is_object() ? payload["payload"] : payload;

  const std::string frame_path = payload_string(frame_payload, "frame_path");
  if (frame_path.empty()) {
    return true;
  }

  if (event.object_debug_name == "MetalPresentFrame") {
    return true;
  }

  if (event.object_debug_name != "D3D12PresentFrame") {
    last_error_ = record_prefix(event) + ": resource blob has frame_path but is not a D3D12PresentFrame";
    return false;
  }

  PresentFrame frame;
  frame.relative_path = frame_path;
  if (!require_payload_key(event, frame_payload, "frame_index", last_error_) ||
      !require_payload_key(event, frame_payload, "width", last_error_) ||
      !require_payload_key(event, frame_payload, "height", last_error_) ||
      !require_payload_key(event, frame_payload, "row_pitch", last_error_) ||
      !require_payload_key(event, frame_payload, "sync_interval", last_error_) ||
      !require_payload_key(event, frame_payload, "flags", last_error_)) {
    return false;
  }
  frame.frame_index = payload_u64(frame_payload, "frame_index");
  frame.width = payload_u32(frame_payload, "width");
  frame.height = payload_u32(frame_payload, "height");
  frame.row_pitch = payload_u32(frame_payload, "row_pitch");
  frame.sync_interval = payload_u32(frame_payload, "sync_interval");
  frame.flags = payload_u32(frame_payload, "flags");
  if (frame.width == 0 || frame.height == 0 || frame.row_pitch < frame.width * 4u) {
    last_error_ = record_prefix(event) + ": present frame payload is missing dimensions";
    return false;
  }
  const auto absolute_path = bundle_root_ / frame.relative_path;
  if (!std::filesystem::is_regular_file(absolute_path)) {
    last_error_ = record_prefix(event) + ": missing present frame asset " + frame.relative_path.generic_string();
    return false;
  }
  std::error_code file_size_error;
  const auto actual_size = std::filesystem::file_size(absolute_path, file_size_error);
  if (file_size_error) {
    last_error_ = record_prefix(event) + ": failed to stat present frame asset " + frame.relative_path.generic_string();
    return false;
  }
  const auto expected_size = static_cast<std::uintmax_t>(frame.row_pitch) * static_cast<std::uintmax_t>(frame.height);
  if (actual_size != expected_size) {
    last_error_ = record_prefix(event) + ": present frame asset size does not match row_pitch * height";
    return false;
  }

  const auto existing = present_frames_.find(frame.frame_index);
  if (existing != present_frames_.end() && !replace_existing) {
    const auto &existing_frame = existing->second;
    if (existing_frame.relative_path != frame.relative_path ||
        existing_frame.width != frame.width ||
        existing_frame.height != frame.height ||
        existing_frame.row_pitch != frame.row_pitch ||
        existing_frame.sync_interval != frame.sync_interval ||
        existing_frame.flags != frame.flags) {
      last_error_ = record_prefix(event) + ": duplicate present frame index has different payload";
      return false;
    }
    return true;
  }

  present_frames_[frame.frame_index] = std::move(frame);
  return true;
}

bool D3D12ReplayBackend::index_present_frames(const trace::TraceBundleReader &reader)
{
  for (const auto &event : reader.events()) {
    if (!index_present_frame_event(event, true)) {
      return false;
    }
  }
  return true;
}

bool D3D12ReplayBackend::index_present_semantics(const trace::TraceBundleReader &reader)
{
  constexpr std::uint32_t kDxgiPresentTest = 0x1;
  std::uint64_t present_index = 0;
  for (const auto &event : reader.events()) {
    if (event.kind != trace::EventKind::Call || event.callsite.function_name != "IDXGISwapChain::Present") {
      continue;
    }

    json payload;
    if (!payload_to_json(event, payload, last_error_)) {
      return false;
    }
    if (!require_payload_key(event, payload, "sync_interval", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }
    const auto flags = payload_u32(payload, "flags");
    const auto present_test = payload.value("present_test", false) || ((flags & kDxgiPresentTest) != 0);
    if (present_test && !payload.contains("frame_index")) {
      continue;
    }

    if (!require_payload_key(event, payload, "frame_index", last_error_)) {
      return false;
    }
    const auto frame_index = payload_u64(payload, "frame_index");
    if (frame_index != present_index) {
      last_error_ = record_prefix(event) + ": IDXGISwapChain::Present frame_index is not contiguous";
      return false;
    }
    auto &semantic = present_semantics_[frame_index];
    if (semantic.has_call) {
      last_error_ = record_prefix(event) + ": duplicate IDXGISwapChain::Present frame_index";
      return false;
    }
    semantic.frame_index = frame_index;
    semantic.call_sequence = event.callsite.sequence;
    semantic.sync_interval = payload_u32(payload, "sync_interval");
    semantic.flags = flags;
    semantic.result_code = event.callsite.result_code;
    semantic.has_call = true;
    ++present_index;
  }
  if (present_index == 0 && !present_frames_.empty()) {
    last_error_ = "D3D12 present frames exist without captured IDXGISwapChain::Present calls";
    return false;
  }
  return true;
}

bool D3D12ReplayBackend::validate_present_semantic_match(
    const trace::EventRecord &event,
    std::uint64_t frame_index,
    std::uint32_t sync_interval,
    std::uint32_t flags)
{
  auto semantic_it = present_semantics_.find(frame_index);
  if (semantic_it == present_semantics_.end() || !semantic_it->second.has_call) {
    last_error_ = record_prefix(event) + ": missing IDXGISwapChain::Present call for frame_index " + std::to_string(frame_index);
    return false;
  }

  auto &semantic = semantic_it->second;
  if (semantic.sync_interval != sync_interval || semantic.flags != flags) {
    std::ostringstream message;
    message << record_prefix(event) << ": Present boundary does not match captured IDXGISwapChain::Present parameters for frame_index "
            << frame_index;
    last_error_ = message.str();
    return false;
  }
  if (semantic.result_code < 0) {
    std::ostringstream message;
    message << record_prefix(event) << ": captured IDXGISwapChain::Present failed for frame_index " << frame_index;
    last_error_ = message.str();
    return false;
  }
  if (semantic.has_boundary) {
    last_error_ = record_prefix(event) + ": duplicate Present boundary for frame_index " + std::to_string(frame_index);
    return false;
  }

  semantic.boundary_sequence = event.callsite.sequence;
  semantic.has_boundary = true;
  return true;
}

bool D3D12ReplayBackend::validate_present_boundaries(const trace::TraceBundleReader &reader)
{
  std::uint64_t present_index = 0;
  for (const auto &event : reader.events()) {
    if (event.kind != trace::EventKind::Boundary || event.boundary != trace::BoundaryKind::Present) {
      continue;
    }

    json payload;
    if (!payload_to_json(event, payload, last_error_)) {
      return false;
    }
    if (!require_payload_key(event, payload, "frame_index", last_error_)) {
      return false;
    }
    const auto frame_index = payload_u64(payload, "frame_index");
    if (frame_index != present_index) {
      last_error_ = record_prefix(event) + ": Present boundary frame_index is not contiguous";
      return false;
    }
    if (!require_payload_key(event, payload, "sync_interval", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }
    const auto sync_interval = payload_u32(payload, "sync_interval");
    const auto flags = payload_u32(payload, "flags");
    if (!validate_present_semantic_match(event, frame_index, sync_interval, flags)) {
      return false;
    }
    const auto frame_it = present_frames_.find(frame_index);
    if (frame_it != present_frames_.end()) {
      const auto &frame = frame_it->second;
      if (frame.sync_interval != sync_interval || frame.flags != flags) {
        std::ostringstream message;
        message << record_prefix(event) << ": D3D12PresentFrame metadata does not match captured Present parameters for frame_index "
                << frame_index;
        last_error_ = message.str();
        return false;
      }
    }
    ++present_index;
  }
  if (present_index != present_semantics_.size()) {
    last_error_ = "D3D12 present boundary count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  if (!present_frames_.empty() && present_index != present_frames_.size()) {
    last_error_ = "D3D12 present frame asset count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  return true;
}

bool D3D12ReplayBackend::validate_frame_boundaries(const trace::TraceBundleReader &reader)
{
  std::uint64_t frame_begin_index = 0;
  std::uint64_t frame_end_index = 0;
  std::unordered_map<std::uint64_t, bool> open_frames;
  std::unordered_set<std::uint64_t> presented_frames;

  for (const auto &event : reader.events()) {
    if (event.kind == trace::EventKind::Boundary && event.boundary == trace::BoundaryKind::Present) {
      json payload;
      if (!payload_to_json(event, payload, last_error_)) {
        return false;
      }
      if (!require_payload_key(event, payload, "frame_index", last_error_)) {
        return false;
      }
      const auto frame_index = payload_u64(payload, "frame_index");
      if (open_frames.find(frame_index) == open_frames.end()) {
        last_error_ = record_prefix(event) + ": Present boundary is missing matching FrameBegin";
        return false;
      }
      if (presented_frames.find(frame_index) != presented_frames.end()) {
        last_error_ = record_prefix(event) + ": duplicate Present boundary for frame_index";
        return false;
      }
      presented_frames.insert(frame_index);
      auto &frame = frame_semantics_[frame_index];
      frame.frame_index = frame_index;
      frame.has_present = true;
      continue;
    }

    if (event.kind != trace::EventKind::Boundary || event.boundary != trace::BoundaryKind::Frame) {
      continue;
    }

    json payload;
    if (!payload_to_json(event, payload, last_error_)) {
      return false;
    }
    const auto label = payload_string(payload, "label");
    if (label != "FrameBegin" && label != "FrameEnd") {
      continue;
    }
    if (!require_payload_key(event, payload, "frame_index", last_error_)) {
      return false;
    }
    const auto frame_index = payload_u64(payload, "frame_index");
    auto &frame = frame_semantics_[frame_index];
    frame.frame_index = frame_index;

    if (label == "FrameBegin") {
      if (frame_index != frame_begin_index) {
        last_error_ = record_prefix(event) + ": FrameBegin frame_index is not contiguous";
        return false;
      }
      if (open_frames.find(frame_index) != open_frames.end() || frame.has_begin) {
        last_error_ = record_prefix(event) + ": duplicate FrameBegin for frame_index";
        return false;
      }
      frame.begin_sequence = event.callsite.sequence;
      frame.has_begin = true;
      open_frames.emplace(frame_index, true);
      ++frame_begin_index;
      continue;
    }

    if (frame_index != frame_end_index) {
      last_error_ = record_prefix(event) + ": FrameEnd frame_index is not contiguous";
      return false;
    }
    const auto open_frame = open_frames.find(frame_index);
    if (open_frame == open_frames.end() || !frame.has_begin) {
      last_error_ = record_prefix(event) + ": FrameEnd is missing matching FrameBegin";
      return false;
    }
    if (presented_frames.find(frame_index) == presented_frames.end() || !frame.has_present) {
      last_error_ = record_prefix(event) + ": FrameEnd is missing matching Present boundary";
      return false;
    }
    if (frame.has_end) {
      last_error_ = record_prefix(event) + ": duplicate FrameEnd for frame_index";
      return false;
    }
    frame.end_sequence = event.callsite.sequence;
    frame.has_end = true;
    open_frames.erase(open_frame);
    presented_frames.erase(frame_index);
    ++frame_end_index;
  }

  if (reader.prefix_limited()) {
    if (frame_begin_index == frame_end_index &&
        frame_begin_index == present_semantics_.size() &&
        open_frames.empty() &&
        presented_frames.empty()) {
      return true;
    }
    if (open_frames.size() == 1 && frame_begin_index == frame_end_index + 1) {
      const auto tail_frame_index = open_frames.begin()->first;
      const auto frame_it = frame_semantics_.find(tail_frame_index);
      const bool valid_tail_frame =
          frame_it != frame_semantics_.end() &&
          tail_frame_index == frame_end_index &&
          frame_it->second.has_begin &&
          !frame_it->second.has_end;
      bool valid_tail_present = false;
      if (valid_tail_frame) {
        valid_tail_present =
            (!frame_it->second.has_present && presented_frames.empty() &&
             present_semantics_.size() == frame_end_index) ||
            (frame_it->second.has_present && presented_frames.size() == 1 &&
             presented_frames.find(tail_frame_index) != presented_frames.end() &&
             present_semantics_.size() == frame_begin_index);
      }
      if (valid_tail_frame && valid_tail_present) {
        return true;
      }
    }
  }

  if (frame_begin_index != frame_end_index) {
    last_error_ = "D3D12 frame boundary count does not match";
    return false;
  }
  if (frame_begin_index != present_semantics_.size()) {
    last_error_ = "D3D12 frame boundary count does not match captured IDXGISwapChain::Present calls";
    return false;
  }
  if (!open_frames.empty() || !presented_frames.empty()) {
    last_error_ = "D3D12 frame boundaries are not fully closed";
    return false;
  }
  return true;
}

bool D3D12ReplayBackend::validate_replay_closure()
{
  if (submissions_.has_open_batch()) {
    last_error_ = "D3D12 replay ended with an open submission batch";
    return false;
  }

  if (replay_commands_.empty()) {
    last_error_ = "D3D12 replay did not collect command records";
    return false;
  }

  const auto find_closed_interval_before = [&](trace::ObjectId command_list_id,
                                               std::uint64_t sequence_limit,
                                               std::size_t &closed_begin_index,
                                               std::size_t &closed_end_index) -> bool {
    constexpr auto invalid_index = std::numeric_limits<std::size_t>::max();
    auto current_begin_index = invalid_index;
    closed_begin_index = invalid_index;
    closed_end_index = invalid_index;

    for (std::size_t command_index = 0; command_index < replay_commands_.size(); ++command_index) {
      const auto &command = replay_commands_[command_index];
      if (command.command_list_object_id != command_list_id) {
        continue;
      }
      if (command.sequence >= sequence_limit) {
        break;
      }

      if (command.kind == ReplayCommandKind::BeginCommandList) {
        current_begin_index = command_index;
        continue;
      }
      if (current_begin_index == invalid_index) {
        continue;
      }
      if (command.kind == ReplayCommandKind::EndCommandList) {
        closed_begin_index = current_begin_index;
        closed_end_index = command_index;
        current_begin_index = invalid_index;
      }
    }

    return closed_begin_index != invalid_index && closed_end_index != invalid_index;
  };

  std::unordered_set<std::size_t> submitted_command_indices;
  std::unordered_set<std::string> expanded_intervals;
  const auto interval_key = [](trace::ObjectId command_list_id,
                               std::size_t begin_index,
                               std::size_t end_index) {
    std::ostringstream key;
    key << command_list_id << ':' << begin_index << ':' << end_index;
    return key.str();
  };
  const auto mark_submitted_interval = [&](auto &self,
                                           trace::ObjectId command_list_id,
                                           std::size_t begin_index,
                                           std::size_t end_index) -> bool {
    const auto [_, inserted] = expanded_intervals.insert(interval_key(command_list_id, begin_index, end_index));
    if (!inserted) {
      return true;
    }
    for (std::size_t command_index = begin_index; command_index <= end_index; ++command_index) {
      const auto &command = replay_commands_[command_index];
      if (command.command_list_object_id != command_list_id) {
        continue;
      }
      submitted_command_indices.insert(command_index);
      if (command.kind != ReplayCommandKind::ExecuteBundle) {
        continue;
      }
      if (command.object_refs.size() < 2 || command.object_refs[1] == 0) {
        last_error_ = "D3D12 submitted ExecuteBundle command is missing bundle command list object id";
        return false;
      }
      const auto bundle_command_list_id = command.object_refs[1];
      const auto bundle_it = command_lists_.find(bundle_command_list_id);
      if (bundle_it == command_lists_.end()) {
        last_error_ = "D3D12 submitted ExecuteBundle command references an unknown bundle command list";
        return false;
      }
      if (bundle_it->second.type != 1) {
        last_error_ = "D3D12 submitted ExecuteBundle command references a non-bundle command list";
        return false;
      }
      if (bundle_it->second.recording) {
        last_error_ = "D3D12 submitted ExecuteBundle command references an open bundle command list";
        return false;
      }
      std::size_t bundle_begin_index = 0;
      std::size_t bundle_end_index = 0;
      if (!find_closed_interval_before(
              bundle_command_list_id,
              command.sequence,
              bundle_begin_index,
              bundle_end_index)) {
        last_error_ = "D3D12 submitted ExecuteBundle command references a bundle with no closed replay interval";
        return false;
      }
      if (!self(self, bundle_command_list_id, bundle_begin_index, bundle_end_index)) {
        return false;
      }
    }
    return true;
  };

  for (const auto &batch : submissions_.completed_batches()) {
    if (batch.queue_object_id == 0 || batch.execute_sequence == 0) {
      last_error_ = "D3D12 submission batch is missing queue or ExecuteCommandLists sequence";
      return false;
    }
    if (batch.command_list_ids.empty()) {
      last_error_ = "D3D12 submission batch has no command lists";
      return false;
    }

    for (const auto command_list_id : batch.command_list_ids) {
      std::size_t closed_begin_index = 0;
      std::size_t closed_end_index = 0;
      if (!find_closed_interval_before(
              command_list_id,
              batch.execute_sequence,
              closed_begin_index,
              closed_end_index)) {
        last_error_ = "D3D12 submission references a command list with no closed replay interval";
        return false;
      }

      if (!mark_submitted_interval(
              mark_submitted_interval,
              command_list_id,
              closed_begin_index,
              closed_end_index)) {
        return false;
      }
    }
  }

  if (submissions_.completed_batches().empty()) {
    last_error_ = "D3D12 replay did not observe any ExecuteCommandLists submissions";
    return false;
  }

	  return true;
	}

bool D3D12ReplayBackend::validate_native_replay_readiness()
{
  for (const auto &operation : fence_operations_) {
    std::ostringstream message;
    message << "D3D12 native replay readiness failed: fence operation sequence "
            << operation.sequence
            << " requires captured fence timeline replay";
    last_error_ = message.str();
    return false;
  }

  const auto require_gpuva_resolved = [&](const GpuVirtualAddressBinding &binding, const char *label) -> bool {
    if (binding.gpu_virtual_address != 0 && !binding.resolved) {
      last_error_ = std::string("D3D12 native replay readiness failed: unresolved GPUVA binding: ") + label;
      return false;
    }
    return true;
  };

  const auto require_root_bindings_resolved = [&](const auto &bindings, const char *label) -> bool {
    for (const auto &[root_parameter_index, binding] : bindings) {
      (void)root_parameter_index;
      if (!require_gpuva_resolved(binding, label)) {
        return false;
      }
    }
    return true;
  };

  const auto require_root_tables_bound = [&](const CommandListSemanticState &command_list, const auto &bindings, const char *label) -> bool {
    for (const auto &[root_parameter_index, binding] : bindings) {
      (void)root_parameter_index;
      if (binding.heap_object_id == 0) {
        last_error_ = std::string("D3D12 native replay readiness failed: root descriptor table missing heap binding: ") + label;
        return false;
      }
      if (std::find(
              command_list.descriptor_heap_object_ids.begin(),
              command_list.descriptor_heap_object_ids.end(),
              binding.heap_object_id) == command_list.descriptor_heap_object_ids.end()) {
        last_error_ = std::string("D3D12 native replay readiness failed: root descriptor table heap is not currently bound: ") + label;
        return false;
      }
      const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
      if (heap_it == descriptor_heaps_.end()) {
        last_error_ = std::string("D3D12 native replay readiness failed: root descriptor table references an unknown heap: ") + label;
        return false;
      }
      if (!descriptor_heap_type_is_shader_visible(heap_it->second.flags)) {
        last_error_ = std::string("D3D12 native replay readiness failed: root descriptor table references a non shader-visible heap: ") + label;
        return false;
      }
    }
    return true;
  };

  const auto require_root_parameter_type = [&](
                                             trace::ObjectId root_signature_object_id,
                                             std::uint32_t root_parameter_index,
                                             std::uint32_t expected_type,
                                             std::uint64_t sequence,
                                             const char *label) -> bool {
    const auto root_signature_it = root_signatures_.find(root_signature_object_id);
    if (root_signature_it == root_signatures_.end() ||
        !root_signature_it->second.has_root_parameter_metadata) {
      return true;
    }
    for (const auto &parameter : root_signature_it->second.root_parameters) {
      if (parameter.root_parameter_index == root_parameter_index) {
        if (parameter.parameter_type != expected_type) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: " << label
                  << " sequence " << sequence
                  << " root parameter type does not match the bound root signature";
          last_error_ = message.str();
          return false;
        }
        return true;
      }
    }
    std::ostringstream message;
    message << "D3D12 native replay readiness failed: " << label
            << " sequence " << sequence
            << " references an unknown root parameter";
    last_error_ = message.str();
    return false;
  };

  const auto require_root_binding_types = [&](
                                            trace::ObjectId root_signature_object_id,
                                            const auto &bindings,
                                            std::uint32_t expected_type,
                                            std::uint64_t sequence,
                                            const char *label) -> bool {
    for (const auto &[root_parameter_index, binding] : bindings) {
      (void)binding;
      if (!require_root_parameter_type(root_signature_object_id, root_parameter_index, expected_type, sequence, label)) {
        return false;
      }
    }
    return true;
  };

  const auto require_bound_pipeline = [&](
                                           trace::ObjectId pipeline_id,
                                           bool graphics,
                                           std::uint64_t sequence,
                                           const char *label) -> const PipelineSemanticState * {
    if (pipeline_id == 0) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " has no bound pipeline state";
      last_error_ = message.str();
      return nullptr;
    }
    const auto pipeline_it = pipelines_.find(pipeline_id);
    if (pipeline_it == pipelines_.end()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " references an unknown bound pipeline state";
      last_error_ = message.str();
      return nullptr;
    }
    if (pipeline_it->second.graphics != graphics) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " has a bound "
              << (pipeline_it->second.graphics ? "graphics" : "compute")
              << " pipeline where a " << (graphics ? "graphics" : "compute")
              << " pipeline is required";
      last_error_ = message.str();
      return nullptr;
    }
    return &pipeline_it->second;
  };

  const auto require_bound_root_signature = [&](
                                                trace::ObjectId root_signature_id,
                                                const PipelineSemanticState &pipeline,
                                                std::uint64_t sequence,
                                                const char *label) -> bool {
    if (root_signature_id == 0) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " has no bound root signature";
      last_error_ = message.str();
      return false;
    }
    if (root_signatures_.find(root_signature_id) == root_signatures_.end()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " references an unknown bound root signature";
      last_error_ = message.str();
      return false;
    }
    if (pipeline.root_signature_object_id != 0 &&
        root_signature_id != pipeline.root_signature_object_id) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence
              << " root signature does not match the bound pipeline state";
      last_error_ = message.str();
      return false;
    }
    return true;
  };

  std::vector<const DescriptorSemanticState *> descriptor_timeline;
  descriptor_timeline.reserve(descriptors_.size());
  for (const auto &descriptor : descriptors_) {
    descriptor_timeline.push_back(&descriptor);
  }
  std::sort(
      descriptor_timeline.begin(),
      descriptor_timeline.end(),
      [](const DescriptorSemanticState *lhs, const DescriptorSemanticState *rhs) {
        return lhs->create_sequence < rhs->create_sequence;
      });

  std::vector<const CopyDescriptorSemanticState *> descriptor_copy_timeline;
  descriptor_copy_timeline.reserve(descriptor_copies_.size());
  for (const auto &copy : descriptor_copies_) {
    descriptor_copy_timeline.push_back(&copy);
  }
  std::sort(
      descriptor_copy_timeline.begin(),
      descriptor_copy_timeline.end(),
      [](const CopyDescriptorSemanticState *lhs, const CopyDescriptorSemanticState *rhs) {
        return lhs->create_sequence < rhs->create_sequence;
      });

  std::unordered_map<std::string, const DescriptorSemanticState *> descriptor_state_at_sequence;
  std::size_t next_descriptor_index = 0;
  std::size_t next_descriptor_copy_index = 0;
  const auto advance_descriptor_state = [&](std::uint64_t sequence) -> bool {
    while ((next_descriptor_index < descriptor_timeline.size() &&
            descriptor_timeline[next_descriptor_index]->create_sequence <= sequence) ||
           (next_descriptor_copy_index < descriptor_copy_timeline.size() &&
            descriptor_copy_timeline[next_descriptor_copy_index]->create_sequence <= sequence)) {
      const auto next_descriptor_sequence = next_descriptor_index < descriptor_timeline.size()
                                                ? descriptor_timeline[next_descriptor_index]->create_sequence
                                                : std::numeric_limits<std::uint64_t>::max();
      const auto next_copy_sequence = next_descriptor_copy_index < descriptor_copy_timeline.size()
                                          ? descriptor_copy_timeline[next_descriptor_copy_index]->create_sequence
                                          : std::numeric_limits<std::uint64_t>::max();
      if (next_copy_sequence < next_descriptor_sequence) {
        const auto *copy = descriptor_copy_timeline[next_descriptor_copy_index++];
        for (const auto &pair : copy->descriptors) {
          DescriptorBinding dst_binding;
          DescriptorBinding src_binding;
          if (!resolve_descriptor_binding_at(pair.dst_descriptor, false, copy->create_sequence, dst_binding, last_error_) ||
              !resolve_descriptor_binding_at(pair.src_descriptor, false, copy->create_sequence, src_binding, last_error_)) {
            last_error_ = std::string("D3D12 native replay readiness failed: descriptor copy cannot be resolved: ") + last_error_;
            return false;
          }
          const auto src_it = descriptor_state_at_sequence.find(descriptor_binding_key(src_binding));
          if (src_it == descriptor_state_at_sequence.end()) {
            last_error_ = "D3D12 native replay readiness failed: descriptor copy reads from an uninitialized descriptor";
            return false;
          }
          descriptor_state_at_sequence[descriptor_binding_key(dst_binding)] = src_it->second;
        }
      } else {
        const auto *descriptor = descriptor_timeline[next_descriptor_index++];
        descriptor_state_at_sequence[descriptor_binding_key(descriptor->binding)] = descriptor;
      }
    }
    return true;
  };

  const auto require_descriptor_kind = [&](
                                           const DescriptorBinding &binding,
                                           std::uint64_t sequence,
                                           const char *expected_kind,
                                           const char *label) -> const DescriptorSemanticState * {
    if (!advance_descriptor_state(sequence)) {
      return nullptr;
    }
    const auto descriptor_it = descriptor_state_at_sequence.find(descriptor_binding_key(binding));
    if (descriptor_it == descriptor_state_at_sequence.end()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " references an uninitialized descriptor";
      last_error_ = message.str();
      return nullptr;
    }
    const auto *descriptor = descriptor_it->second;
    if (descriptor->kind != expected_kind) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " expects " << expected_kind
              << " but descriptor contains " << descriptor->kind;
      last_error_ = message.str();
      return nullptr;
    }
    return descriptor;
  };

  const auto require_descriptor_resource = [&](
                                               const DescriptorSemanticState &descriptor,
                                               trace::ObjectId resource_object_id,
                                               std::uint64_t sequence,
                                               const char *label) -> bool {
    if (resource_object_id != 0 && descriptor.resource_object_id != resource_object_id) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " descriptor resource does not match command resource";
      last_error_ = message.str();
      return false;
    }
    if (descriptor.resource_object_id != 0 &&
        resources_.find(descriptor.resource_object_id) == resources_.end()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " descriptor references an unknown resource";
      last_error_ = message.str();
      return false;
    }
    return true;
  };

  const auto require_root_table_descriptors_initialized = [&](
                                                                 const auto &bindings,
                                                                 trace::ObjectId root_signature_object_id,
                                                                 std::uint64_t sequence,
                                                                 const char *label) -> bool {
    for (const auto &[root_parameter_index, binding] : bindings) {
      if (!require_root_parameter_type(
              root_signature_object_id,
              root_parameter_index,
              0,
              sequence,
              label)) {
        return false;
      }
      if (!advance_descriptor_state(sequence)) {
        return false;
      }
      const auto require_table_descriptor = [&](
                                               const DescriptorBinding &descriptor_binding,
                                               const char *expected_kind) -> bool {
        const auto descriptor_it = descriptor_state_at_sequence.find(descriptor_binding_key(descriptor_binding));
        if (descriptor_it == descriptor_state_at_sequence.end()) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: " << label
                  << " sequence " << sequence
                  << " root descriptor table references an uninitialized descriptor";
          last_error_ = message.str();
          return false;
        }
        if (expected_kind && descriptor_it->second->kind != expected_kind) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: " << label
                  << " sequence " << sequence
                  << " root descriptor table expects " << expected_kind
                  << " but descriptor contains " << descriptor_it->second->kind;
          last_error_ = message.str();
          return false;
        }
        return true;
      };
      const auto root_signature_it = root_signatures_.find(root_signature_object_id);
      const auto *root_signature = root_signature_it != root_signatures_.end() ? &root_signature_it->second : nullptr;
      const RootDescriptorTableSemanticState *table = nullptr;
      if (root_signature && root_signature->has_descriptor_table_metadata) {
        for (const auto &candidate : root_signature->descriptor_tables) {
          if (candidate.root_parameter_index == root_parameter_index) {
            table = &candidate;
            break;
          }
        }
      }
      if (!table) {
        if (!require_table_descriptor(binding, nullptr)) {
          return false;
        }
        continue;
      }
      const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
      if (heap_it == descriptor_heaps_.end()) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: " << label
                << " sequence " << sequence
                << " root descriptor table references an unknown descriptor heap";
        last_error_ = message.str();
        return false;
      }
      const auto &heap = heap_it->second;
      for (const auto &range : table->ranges) {
        const char *expected_kind = descriptor_kind_for_root_range_type(range.type);
        if (!expected_kind) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: " << label
                  << " sequence " << sequence
                  << " root descriptor table has an unknown descriptor range type";
          last_error_ = message.str();
          return false;
        }
        const auto expected_heap_type = range.type == 3 ? 1u : 0u;
        if (heap.type != expected_heap_type) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: " << label
                  << " sequence " << sequence
                  << " root descriptor table range is bound to the wrong descriptor heap type";
          last_error_ = message.str();
          return false;
        }
        if (range.offset_from_table_start > heap.num_descriptors ||
            range.descriptor_count > heap.num_descriptors - range.offset_from_table_start ||
            binding.descriptor_index > heap.num_descriptors - range.offset_from_table_start - range.descriptor_count) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: " << label
                  << " sequence " << sequence
                  << " root descriptor table range exceeds the descriptor heap";
          last_error_ = message.str();
          return false;
        }
        for (std::uint32_t descriptor_offset = 0; descriptor_offset < range.descriptor_count; ++descriptor_offset) {
          DescriptorBinding descriptor_binding = binding;
          descriptor_binding.descriptor_index = binding.descriptor_index +
                                                range.offset_from_table_start +
                                                descriptor_offset;
          descriptor_binding.descriptor = heap.gpu_start +
                                          static_cast<std::uint64_t>(descriptor_binding.descriptor_index) *
                                              heap.descriptor_size;
          if (!require_table_descriptor(descriptor_binding, expected_kind)) {
            return false;
          }
        }
      }
    }
    return true;
  };

  const auto require_draw_targets_ready = [&](
                                                const std::vector<DescriptorBinding> &render_targets,
                                                const DescriptorBinding &depth_stencil,
                                                std::uint64_t sequence,
                                                const char *label) -> bool {
    for (const auto &render_target : render_targets) {
      const auto *descriptor = require_descriptor_kind(
          render_target,
          sequence,
          "RenderTargetView",
          label);
      if (!descriptor || !require_descriptor_resource(*descriptor, 0, sequence, label)) {
        return false;
      }
    }
    if (depth_stencil.descriptor != 0) {
      const auto *descriptor = require_descriptor_kind(
          depth_stencil,
          sequence,
          "DepthStencilView",
          label);
      if (!descriptor || !require_descriptor_resource(*descriptor, 0, sequence, label)) {
        return false;
      }
    }
    return true;
  };

  const auto require_draw_raster_state_ready = [&](
                                                    const std::vector<ViewportSemanticState> &viewports,
                                                    const std::vector<ScissorRectSemanticState> &scissor_rects,
                                                    std::uint32_t primitive_topology,
                                                    std::uint64_t sequence,
                                                    const char *label) -> bool {
    if (viewports.empty()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " has no bound viewport";
      last_error_ = message.str();
      return false;
    }
    if (scissor_rects.empty()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " has no bound scissor rect";
      last_error_ = message.str();
      return false;
    }
    if (primitive_topology == 0) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " has no bound primitive topology";
      last_error_ = message.str();
      return false;
    }
    return true;
  };

  const auto require_render_pass_targets_ready = [&](
                                                       const ReplayCommandRecord &command) -> bool {
    if (command.function_name != "ID3D12GraphicsCommandList4::BeginRenderPass") {
      return true;
    }
    const auto payload = json::parse(command.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: BeginRenderPass sequence "
              << command.sequence << " has an invalid payload";
      last_error_ = message.str();
      return false;
    }

    const auto render_targets = payload.find("render_targets");
    if (render_targets != payload.end() && render_targets->is_array()) {
      for (const auto &render_target : *render_targets) {
        const auto descriptor = payload_u64(render_target, "cpu_descriptor");
        if (descriptor == 0) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: BeginRenderPass sequence "
                  << command.sequence << " has a null render target descriptor";
          last_error_ = message.str();
          return false;
        }
        DescriptorBinding binding;
        if (!resolve_descriptor_binding_at(descriptor, false, command.sequence, binding, last_error_)) {
          last_error_ = "D3D12 native replay readiness failed: BeginRenderPass render target descriptor cannot be resolved: " + last_error_;
          return false;
        }
        const auto *descriptor_state = require_descriptor_kind(
            binding,
            command.sequence,
            "RenderTargetView",
            "BeginRenderPass");
        if (!descriptor_state || !require_descriptor_resource(*descriptor_state, 0, command.sequence, "BeginRenderPass")) {
          return false;
        }
      }
    }

    const auto depth_stencil = payload.find("depth_stencil");
    if (depth_stencil != payload.end() && depth_stencil->is_object() &&
        payload_bool(payload, "has_depth_stencil")) {
      const auto descriptor = payload_u64(*depth_stencil, "cpu_descriptor");
      if (descriptor == 0) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: BeginRenderPass sequence "
                << command.sequence << " has a null depth stencil descriptor";
        last_error_ = message.str();
        return false;
      }
      DescriptorBinding binding;
      if (!resolve_descriptor_binding_at(descriptor, false, command.sequence, binding, last_error_)) {
        last_error_ = "D3D12 native replay readiness failed: BeginRenderPass depth stencil descriptor cannot be resolved: " + last_error_;
        return false;
      }
      const auto *descriptor_state = require_descriptor_kind(
          binding,
          command.sequence,
          "DepthStencilView",
          "BeginRenderPass");
      if (!descriptor_state || !require_descriptor_resource(*descriptor_state, 0, command.sequence, "BeginRenderPass")) {
        return false;
      }
    }
    return true;
  };

  const auto require_dynamic_state_ready = [&](const ReplayCommandRecord &command) -> bool {
    if (command.kind != ReplayCommandKind::SetDynamicState) {
      return true;
    }
    const auto payload = json::parse(command.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << command.function_name
              << " sequence " << command.sequence << " has an invalid payload";
      last_error_ = message.str();
      return false;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::ClearState") {
      if (command.object_refs.size() >= 2 && command.object_refs[1] != 0 &&
          pipelines_.find(command.object_refs[1]) == pipelines_.end()) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: ClearState sequence "
                << command.sequence << " references an unknown pipeline state";
        last_error_ = message.str();
        return false;
      }
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::OMSetBlendFactor") {
      const auto blend = payload.find("blend_factor");
      if (blend == payload.end() || !blend->is_array() || blend->size() != 4) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: OMSetBlendFactor sequence "
                << command.sequence << " is missing blend_factor[4]";
        last_error_ = message.str();
        return false;
      }
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::OMSetStencilRef") {
      if (!payload_has(payload, "stencil_ref")) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: OMSetStencilRef sequence "
                << command.sequence << " is missing stencil_ref";
        last_error_ = message.str();
        return false;
      }
      return true;
    }
    std::ostringstream message;
    message << "D3D12 native replay readiness failed: unsupported dynamic state command "
            << command.function_name;
    last_error_ = message.str();
    return false;
  };

  const auto require_buffer_range = [&](
                                        trace::ObjectId resource_object_id,
                                        std::uint64_t offset,
                                        std::uint64_t size,
                                        std::uint64_t sequence,
                                        const char *label) -> bool {
    if (resource_object_id == 0) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " references a null buffer";
      last_error_ = message.str();
      return false;
    }
    const auto resource_it = resources_.find(resource_object_id);
    if (resource_it == resources_.end()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " references an unknown buffer";
      last_error_ = message.str();
      return false;
    }
    if (offset > resource_it->second.width ||
        size > resource_it->second.width - offset) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " buffer range is out of bounds";
      last_error_ = message.str();
      return false;
    }
    return true;
  };

  const auto require_texture_copy_location_ready = [&](
                                                        const TextureCopyLocation &location,
                                                        std::uint64_t sequence,
                                                        const char *label) -> const ResourceSemanticState * {
    if (location.resource_object_id == 0) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " references a null resource";
      last_error_ = message.str();
      return nullptr;
    }
    const auto resource_it = resources_.find(location.resource_object_id);
    if (resource_it == resources_.end()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " references an unknown resource";
      last_error_ = message.str();
      return nullptr;
    }
    const auto &resource = resource_it->second;
    if (location.type == 0) {
      const auto subresource_count = resource_subresource_count(resource);
      if (location.subresource_index >= subresource_count) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: " << label
                << " sequence " << sequence << " subresource index is out of bounds";
        last_error_ = message.str();
        return nullptr;
      }
    } else if (location.type == 1) {
      const auto footprint_bytes =
          static_cast<std::uint64_t>(location.footprint_row_pitch) *
          static_cast<std::uint64_t>(location.footprint_height) *
          static_cast<std::uint64_t>(location.footprint_depth);
      if (location.footprint_offset > resource.width ||
          footprint_bytes > resource.width - location.footprint_offset) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: " << label
                << " sequence " << sequence << " placed footprint range is out of bounds";
        last_error_ = message.str();
        return nullptr;
      }
    }
    return &resource;
  };

  const auto require_texture_copy_ready = [&](
                                             const TextureCopySemanticState &copy) -> bool {
    const auto *dst_resource = require_texture_copy_location_ready(
        copy.dst,
        copy.sequence,
        "CopyTextureRegion destination");
    const auto *src_resource = require_texture_copy_location_ready(
        copy.src,
        copy.sequence,
        "CopyTextureRegion source");
    if (!dst_resource || !src_resource) {
      return false;
    }
    if (copy.dst.type == 0 &&
        (copy.dst_x > dst_resource->width ||
         copy.dst_y > dst_resource->height ||
         copy.dst_z > dst_resource->depth_or_array_size)) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: CopyTextureRegion sequence "
              << copy.sequence << " destination offset is out of bounds";
      last_error_ = message.str();
      return false;
    }
    if (copy.has_src_box) {
      if (copy.src_box_right < copy.src_box_left ||
          copy.src_box_bottom < copy.src_box_top ||
          copy.src_box_back < copy.src_box_front ||
          copy.src_box_right > src_resource->width ||
          copy.src_box_bottom > src_resource->height ||
          copy.src_box_back > src_resource->depth_or_array_size) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: CopyTextureRegion sequence "
                << copy.sequence << " source box is out of bounds";
        last_error_ = message.str();
        return false;
      }
    } else if (copy.src.type == 0 && copy.dst.type == 0) {
      const auto mip = src_resource->mip_levels == 0 ? 0 : copy.src.subresource_index % src_resource->mip_levels;
      const auto copy_width = std::max<std::uint64_t>(1, src_resource->width >> mip);
      const auto copy_height = std::max<std::uint32_t>(1, src_resource->height >> mip);
      const auto copy_depth = src_resource->dimension == 4
                                  ? std::max<std::uint32_t>(1, src_resource->depth_or_array_size >> mip)
                                  : 1u;
      if (copy.dst_x > dst_resource->width ||
          copy.dst_y > dst_resource->height ||
          copy.dst_z > dst_resource->depth_or_array_size ||
          copy_width > dst_resource->width - copy.dst_x ||
          copy_height > dst_resource->height - copy.dst_y ||
          copy_depth > dst_resource->depth_or_array_size - copy.dst_z) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: CopyTextureRegion sequence "
                << copy.sequence << " destination full-copy region is out of bounds";
        last_error_ = message.str();
        return false;
      }
    }
    return true;
  };

  const auto require_rects_within_resource = [&](
                                                    const std::vector<ScissorRectSemanticState> &rects,
                                                    const ResourceSemanticState &resource,
                                                    std::uint64_t sequence,
                                                    const char *label) -> bool {
    for (const auto &rect : rects) {
      if (rect.right < rect.left ||
          rect.bottom < rect.top ||
          rect.left < 0 ||
          rect.top < 0 ||
          static_cast<std::uint64_t>(rect.right) > resource.width ||
          static_cast<std::uint64_t>(rect.bottom) > resource.height) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: " << label
                << " sequence " << sequence << " rect is out of bounds";
        last_error_ = message.str();
        return false;
      }
    }
    return true;
  };

  const auto require_descriptor_rects_within_resource = [&](
                                                              const DescriptorSemanticState &descriptor,
                                                              const std::vector<ScissorRectSemanticState> &rects,
                                                              std::uint64_t sequence,
                                                              const char *label) -> bool {
    if (descriptor.resource_object_id == 0 || rects.empty()) {
      return true;
    }
    const auto resource_it = resources_.find(descriptor.resource_object_id);
    if (resource_it == resources_.end()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: " << label
              << " sequence " << sequence << " descriptor references an unknown resource";
      last_error_ = message.str();
      return false;
    }
    return require_rects_within_resource(rects, resource_it->second, sequence, label);
  };

  const auto require_discard_ready = [&](const DiscardResourceSemanticState &discard) -> bool {
    const auto resource_it = resources_.find(discard.resource_object_id);
    if (resource_it == resources_.end()) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: DiscardResource sequence "
              << discard.sequence << " references an unknown resource";
      last_error_ = message.str();
      return false;
    }
    const auto &resource = resource_it->second;
    if (!discard.has_region) {
      return true;
    }
    const auto subresource_count = resource_subresource_count(resource);
    if (discard.subresource_count == 0 ||
        discard.first_subresource > subresource_count ||
        discard.subresource_count > subresource_count - discard.first_subresource) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: DiscardResource sequence "
              << discard.sequence << " subresource range is out of bounds";
      last_error_ = message.str();
      return false;
    }
    if (!require_rects_within_resource(discard.rects, resource, discard.sequence, "DiscardResource")) {
      return false;
    }
    return true;
  };

  const auto require_resource_copy_ready = [&](
                                                const ResourceCopySemanticState &copy) -> bool {
    const auto dst_it = resources_.find(copy.dst_resource_object_id);
    if (dst_it == resources_.end()) {
      last_error_ = "D3D12 native replay readiness failed: CopyResource references an unknown destination resource";
      return false;
    }
    const auto src_it = resources_.find(copy.src_resource_object_id);
    if (src_it == resources_.end()) {
      last_error_ = "D3D12 native replay readiness failed: CopyResource references an unknown source resource";
      return false;
    }
    const auto &dst = dst_it->second;
    const auto &src = src_it->second;
    if (dst.dimension != src.dimension ||
        dst.width != src.width ||
        dst.height != src.height ||
        dst.depth_or_array_size != src.depth_or_array_size ||
        dst.mip_levels != src.mip_levels ||
        dst.format != src.format ||
        dst.sample_count != src.sample_count ||
        dst.sample_quality != src.sample_quality) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: CopyResource sequence "
              << copy.sequence << " resource descriptions do not match";
      last_error_ = message.str();
      return false;
    }
    return true;
  };

  const auto require_resolve_ready = [&](const ResolveSubresourceSemanticState &resolve) -> bool {
    const auto dst_it = resources_.find(resolve.dst_resource_object_id);
    if (dst_it == resources_.end()) {
      last_error_ = "D3D12 native replay readiness failed: ResolveSubresource references an unknown destination resource";
      return false;
    }
    const auto src_it = resources_.find(resolve.src_resource_object_id);
    if (src_it == resources_.end()) {
      last_error_ = "D3D12 native replay readiness failed: ResolveSubresource references an unknown source resource";
      return false;
    }
    const auto &dst = dst_it->second;
    const auto &src = src_it->second;
    if (resolve.dst_subresource >= resource_subresource_count(dst) ||
        resolve.src_subresource >= resource_subresource_count(src)) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: ResolveSubresource sequence "
              << resolve.sequence << " subresource index is out of bounds";
      last_error_ = message.str();
      return false;
    }
    const auto subresource_width = [](const ResourceSemanticState &resource, std::uint32_t subresource) -> std::uint64_t {
      const auto mip = resource.mip_levels == 0 ? 0 : subresource % resource.mip_levels;
      return std::max<std::uint64_t>(1, resource.width >> mip);
    };
    const auto subresource_height = [](const ResourceSemanticState &resource, std::uint32_t subresource) -> std::uint32_t {
      const auto mip = resource.mip_levels == 0 ? 0 : subresource % resource.mip_levels;
      return std::max<std::uint32_t>(1, resource.height >> mip);
    };
    const auto dst_width = subresource_width(dst, resolve.dst_subresource);
    const auto dst_height = subresource_height(dst, resolve.dst_subresource);
    const auto src_width = subresource_width(src, resolve.src_subresource);
    const auto src_height = subresource_height(src, resolve.src_subresource);
    if (dst.dimension != src.dimension ||
        dst_width != src_width ||
        dst_height != src_height ||
        dst.format != src.format ||
        (resolve.format != 0 && resolve.format != dst.format)) {
      std::ostringstream message;
      message << "D3D12 native replay readiness failed: ResolveSubresource sequence "
              << resolve.sequence << " resource descriptions do not match";
      last_error_ = message.str();
      return false;
    }
    if (resolve.has_src_rect) {
      if (resolve.src_rect_right < resolve.src_rect_left ||
          resolve.src_rect_bottom < resolve.src_rect_top ||
          resolve.src_rect_left < 0 ||
          resolve.src_rect_top < 0 ||
          static_cast<std::uint64_t>(resolve.src_rect_right) > src_width ||
          static_cast<std::uint64_t>(resolve.src_rect_bottom) > src_height) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: ResolveSubresourceRegion sequence "
                << resolve.sequence << " source rect is out of bounds";
        last_error_ = message.str();
        return false;
      }
      if (resolve.dst_x > dst_width ||
          resolve.dst_y > dst_height ||
          static_cast<std::uint64_t>(resolve.src_rect_right - resolve.src_rect_left) > dst_width - resolve.dst_x ||
          static_cast<std::uint64_t>(resolve.src_rect_bottom - resolve.src_rect_top) > dst_height - resolve.dst_y) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: ResolveSubresourceRegion sequence "
                << resolve.sequence << " destination rect is out of bounds";
        last_error_ = message.str();
        return false;
      }
    }
    return true;
  };

  const auto require_graphics_root_binding_types = [&](
                                                     const CommandListSemanticState &command_list,
                                                     std::uint64_t sequence,
                                                     const char *label) -> bool {
    return require_root_binding_types(command_list.graphics_root_signature_object_id, command_list.graphics_root_constant_buffers, 2, sequence, label) &&
           require_root_binding_types(command_list.graphics_root_signature_object_id, command_list.graphics_root_shader_resources, 3, sequence, label) &&
           require_root_binding_types(command_list.graphics_root_signature_object_id, command_list.graphics_root_unordered_accesses, 4, sequence, label) &&
           require_root_binding_types(command_list.graphics_root_signature_object_id, command_list.graphics_root_constants, 1, sequence, label);
  };

  const auto require_compute_root_binding_types = [&](
                                                    const CommandListSemanticState &command_list,
                                                    std::uint64_t sequence,
                                                    const char *label) -> bool {
    return require_root_binding_types(command_list.compute_root_signature_object_id, command_list.compute_root_constant_buffers, 2, sequence, label) &&
           require_root_binding_types(command_list.compute_root_signature_object_id, command_list.compute_root_shader_resources, 3, sequence, label) &&
           require_root_binding_types(command_list.compute_root_signature_object_id, command_list.compute_root_unordered_accesses, 4, sequence, label) &&
           require_root_binding_types(command_list.compute_root_signature_object_id, command_list.compute_root_constants, 1, sequence, label);
  };

  for (const auto &[command_list_id, command_list] : command_lists_) {
    (void)command_list_id;
    if (!require_root_bindings_resolved(command_list.graphics_root_constant_buffers, "graphics root CBV") ||
        !require_root_bindings_resolved(command_list.compute_root_constant_buffers, "compute root CBV") ||
        !require_root_bindings_resolved(command_list.graphics_root_shader_resources, "graphics root SRV") ||
        !require_root_bindings_resolved(command_list.compute_root_shader_resources, "compute root SRV") ||
        !require_root_bindings_resolved(command_list.graphics_root_unordered_accesses, "graphics root UAV") ||
        !require_root_bindings_resolved(command_list.compute_root_unordered_accesses, "compute root UAV")) {
      return false;
    }
    if (!require_root_tables_bound(command_list, command_list.graphics_root_tables, "graphics root descriptor table") ||
        !require_root_tables_bound(command_list, command_list.compute_root_tables, "compute root descriptor table")) {
      return false;
    }
    for (const auto &clear : command_list.render_target_clears) {
      const auto *descriptor = require_descriptor_kind(
          clear.render_target,
          clear.sequence,
          "RenderTargetView",
          "ClearRenderTargetView");
      if (!descriptor ||
          !require_descriptor_resource(*descriptor, 0, clear.sequence, "ClearRenderTargetView") ||
          !require_descriptor_rects_within_resource(*descriptor, clear.rects, clear.sequence, "ClearRenderTargetView")) {
        return false;
      }
    }
    for (const auto &clear : command_list.depth_stencil_clears) {
      const auto *descriptor = require_descriptor_kind(
          clear.depth_stencil,
          clear.sequence,
          "DepthStencilView",
          "ClearDepthStencilView");
      if (!descriptor ||
          !require_descriptor_resource(*descriptor, 0, clear.sequence, "ClearDepthStencilView") ||
          !require_descriptor_rects_within_resource(*descriptor, clear.rects, clear.sequence, "ClearDepthStencilView")) {
        return false;
      }
    }
    for (const auto &clear : command_list.unordered_access_clears) {
      const auto *gpu_descriptor = require_descriptor_kind(
          clear.gpu_descriptor,
          clear.sequence,
          "UnorderedAccessView",
          clear.integer ? "ClearUnorderedAccessViewUint" : "ClearUnorderedAccessViewFloat");
      if (!gpu_descriptor ||
          !require_descriptor_resource(
              *gpu_descriptor,
              clear.resource_object_id,
              clear.sequence,
              clear.integer ? "ClearUnorderedAccessViewUint" : "ClearUnorderedAccessViewFloat") ||
          !require_descriptor_rects_within_resource(
              *gpu_descriptor,
              clear.rects,
              clear.sequence,
              clear.integer ? "ClearUnorderedAccessViewUint" : "ClearUnorderedAccessViewFloat")) {
        return false;
      }
      const auto *cpu_descriptor = require_descriptor_kind(
          clear.cpu_descriptor,
          clear.sequence,
          "UnorderedAccessView",
          clear.integer ? "ClearUnorderedAccessViewUint" : "ClearUnorderedAccessViewFloat");
      if (!cpu_descriptor ||
          !require_descriptor_resource(
              *cpu_descriptor,
              clear.resource_object_id,
              clear.sequence,
              clear.integer ? "ClearUnorderedAccessViewUint" : "ClearUnorderedAccessViewFloat")) {
        return false;
      }
    }
    for (const auto &binding : command_list.vertex_buffers) {
      if (!require_gpuva_resolved(binding.address, "vertex buffer")) {
        return false;
      }
      if (binding.address.resource_object_id != 0 &&
          !require_buffer_range(
              binding.address.resource_object_id,
              binding.address.offset,
              binding.size_in_bytes,
              binding.sequence,
              "vertex buffer")) {
        return false;
      }
    }
    if (!require_gpuva_resolved(command_list.index_buffer.address, "index buffer") ||
        (command_list.index_buffer.address.resource_object_id != 0 &&
         !require_buffer_range(
             command_list.index_buffer.address.resource_object_id,
             command_list.index_buffer.address.offset,
             command_list.index_buffer.size_in_bytes,
             command_list.index_buffer.sequence,
             "index buffer"))) {
      return false;
    }
    for (const auto &draw : command_list.draws) {
      const auto *pipeline = require_bound_pipeline(
          draw.pipeline_state_object_id,
          true,
          draw.sequence,
          "DrawInstanced");
      if (!pipeline ||
          !require_bound_root_signature(
              draw.graphics_root_signature_object_id,
              *pipeline,
              draw.sequence,
              "DrawInstanced")) {
        return false;
      }
      if (!require_root_table_descriptors_initialized(
              draw.graphics_root_tables,
              draw.graphics_root_signature_object_id,
              draw.sequence,
              "DrawInstanced")) {
        return false;
      }
      if (!require_graphics_root_binding_types(command_list, draw.sequence, "DrawInstanced")) {
        return false;
      }
      if (!require_draw_targets_ready(
              draw.render_targets,
              draw.depth_stencil,
              draw.sequence,
              "DrawInstanced")) {
        return false;
      }
      if (!require_draw_raster_state_ready(
              draw.viewports,
              draw.scissor_rects,
              draw.primitive_topology,
              draw.sequence,
              "DrawInstanced")) {
        return false;
      }
    }
    for (const auto &draw : command_list.indexed_draws) {
      const auto *pipeline = require_bound_pipeline(
          draw.pipeline_state_object_id,
          true,
          draw.sequence,
          "DrawIndexedInstanced");
      if (!pipeline ||
          !require_bound_root_signature(
              draw.graphics_root_signature_object_id,
              *pipeline,
              draw.sequence,
              "DrawIndexedInstanced")) {
        return false;
      }
      if (!require_root_table_descriptors_initialized(
              draw.graphics_root_tables,
              draw.graphics_root_signature_object_id,
              draw.sequence,
              "DrawIndexedInstanced")) {
        return false;
      }
      if (!require_graphics_root_binding_types(command_list, draw.sequence, "DrawIndexedInstanced")) {
        return false;
      }
      if (!require_draw_targets_ready(
              draw.render_targets,
              draw.depth_stencil,
              draw.sequence,
              "DrawIndexedInstanced")) {
        return false;
      }
      if (!require_draw_raster_state_ready(
              draw.viewports,
              draw.scissor_rects,
              draw.primitive_topology,
              draw.sequence,
              "DrawIndexedInstanced")) {
        return false;
      }
    }
    for (const auto &dispatch : command_list.dispatches) {
      const auto graphics_pipeline = dispatch.mesh;
      const auto *pipeline = require_bound_pipeline(
          dispatch.pipeline_state_object_id,
          graphics_pipeline,
          dispatch.sequence,
          dispatch.mesh ? "DispatchMesh" : "Dispatch");
      if (!pipeline ||
          !require_bound_root_signature(
              graphics_pipeline ? dispatch.graphics_root_signature_object_id : dispatch.compute_root_signature_object_id,
              *pipeline,
              dispatch.sequence,
              dispatch.mesh ? "DispatchMesh" : "Dispatch")) {
        return false;
      }
      if (!require_root_table_descriptors_initialized(
              dispatch.mesh ? dispatch.graphics_root_tables : dispatch.compute_root_tables,
              dispatch.mesh ? dispatch.graphics_root_signature_object_id : dispatch.compute_root_signature_object_id,
              dispatch.sequence,
              dispatch.mesh ? "DispatchMesh" : "Dispatch")) {
        return false;
      }
      if (dispatch.mesh) {
        if (!require_graphics_root_binding_types(command_list, dispatch.sequence, "DispatchMesh")) {
          return false;
        }
      } else if (!require_compute_root_binding_types(command_list, dispatch.sequence, "Dispatch")) {
        return false;
      }
      if (dispatch.mesh) {
        last_error_ = "D3D12 native replay readiness failed: DispatchMesh requires ID3D12GraphicsCommandList6";
        return false;
      }
    }
    for (const auto &execute : command_list.indirect_executes) {
      const auto signature_it = command_signatures_.find(execute.command_signature_object_id);
      if (signature_it == command_signatures_.end()) {
        last_error_ = "D3D12 native replay readiness failed: ExecuteIndirect references an unknown command signature";
        return false;
      }
      const auto command_bytes = static_cast<std::uint64_t>(execute.max_command_count) *
                                 static_cast<std::uint64_t>(signature_it->second.byte_stride);
      if (!require_buffer_range(
              execute.arg_buffer_object_id,
              execute.arg_buffer_offset,
              command_bytes,
              execute.sequence,
              "ExecuteIndirect argument buffer")) {
        return false;
      }
      if (execute.count_buffer_object_id != 0 &&
          !require_buffer_range(
              execute.count_buffer_object_id,
              execute.count_buffer_offset,
              sizeof(std::uint32_t),
              execute.sequence,
              "ExecuteIndirect count buffer")) {
        return false;
      }
      bool executes_draw = false;
      bool executes_dispatch = false;
      for (const auto &argument : signature_it->second.arguments) {
        executes_draw = executes_draw || command_signature_argument_executes_draw(argument.type);
        executes_dispatch = executes_dispatch || command_signature_argument_executes_dispatch(argument.type);
      }
      if (executes_draw) {
        const auto *pipeline = require_bound_pipeline(
            execute.pipeline_state_object_id,
            true,
            execute.sequence,
            "ExecuteIndirect");
        if (!pipeline ||
            !require_bound_root_signature(
                execute.graphics_root_signature_object_id,
                *pipeline,
                execute.sequence,
                "ExecuteIndirect")) {
          return false;
        }
        if (signature_it->second.root_signature_object_id != 0 &&
            execute.graphics_root_signature_object_id != signature_it->second.root_signature_object_id) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: ExecuteIndirect sequence "
                  << execute.sequence
                  << " root signature does not match the command signature";
          last_error_ = message.str();
          return false;
        }
        if (!require_root_table_descriptors_initialized(
                execute.graphics_root_tables,
                execute.graphics_root_signature_object_id,
                execute.sequence,
                "ExecuteIndirect")) {
          return false;
        }
        if (!require_graphics_root_binding_types(command_list, execute.sequence, "ExecuteIndirect")) {
          return false;
        }
        if (!require_draw_targets_ready(
                execute.render_targets,
                execute.depth_stencil,
                execute.sequence,
                "ExecuteIndirect")) {
          return false;
        }
        if (!require_draw_raster_state_ready(
                execute.viewports,
                execute.scissor_rects,
                execute.primitive_topology,
                execute.sequence,
                "ExecuteIndirect")) {
          return false;
        }
      }
      if (executes_dispatch) {
        const auto *pipeline = require_bound_pipeline(
            execute.pipeline_state_object_id,
            false,
            execute.sequence,
            "ExecuteIndirect");
        if (!pipeline ||
            !require_bound_root_signature(
                execute.compute_root_signature_object_id,
                *pipeline,
                execute.sequence,
                "ExecuteIndirect")) {
          return false;
        }
        if (signature_it->second.root_signature_object_id != 0 &&
            execute.compute_root_signature_object_id != signature_it->second.root_signature_object_id) {
          std::ostringstream message;
          message << "D3D12 native replay readiness failed: ExecuteIndirect sequence "
                  << execute.sequence
                  << " root signature does not match the command signature";
          last_error_ = message.str();
          return false;
        }
        if (!require_root_table_descriptors_initialized(
                execute.compute_root_tables,
                execute.compute_root_signature_object_id,
                execute.sequence,
                "ExecuteIndirect")) {
          return false;
        }
        if (!require_compute_root_binding_types(command_list, execute.sequence, "ExecuteIndirect")) {
          return false;
        }
      }
    }
    for (const auto &write : command_list.write_buffer_immediates) {
      for (const auto &entry : write.writes) {
        if (!require_gpuva_resolved(entry.dest, "WriteBufferImmediate dest")) {
          return false;
        }
        if (entry.dest.resource_object_id != 0 &&
            !require_buffer_range(
                entry.dest.resource_object_id,
                entry.dest.offset,
                sizeof(std::uint32_t),
                write.sequence,
                "WriteBufferImmediate dest")) {
          return false;
        }
      }
    }
    for (const auto &copy : command_list.buffer_copies) {
      if (!require_buffer_range(
              copy.dst_buffer_object_id,
              copy.dst_offset,
              copy.byte_count,
              copy.sequence,
              "CopyBufferRegion destination")) {
        return false;
      }
      if (!require_buffer_range(
              copy.src_buffer_object_id,
              copy.src_offset,
              copy.byte_count,
              copy.sequence,
              "CopyBufferRegion source")) {
        return false;
      }
    }
    for (const auto &copy : command_list.texture_copies) {
      if (!require_texture_copy_ready(copy)) {
        return false;
      }
    }
    for (const auto &copy : command_list.resource_copies) {
      if (!require_resource_copy_ready(copy)) {
        return false;
      }
    }
    for (const auto &discard : command_list.discards) {
      if (!require_discard_ready(discard)) {
        return false;
      }
    }
    for (const auto &query : command_list.queries) {
      const auto heap_it = query_heaps_.find(query.query_heap_object_id);
      if (heap_it == query_heaps_.end()) {
        last_error_ = "D3D12 native replay readiness failed: query command references an unknown query heap";
        return false;
      }
      const auto first_query = query.resolve ? query.start_index : query.index;
      const auto query_count = query.resolve ? query.query_count : 1u;
      if (query_count == 0 ||
          first_query > heap_it->second.count ||
          query_count > heap_it->second.count - first_query) {
        std::ostringstream message;
        message << "D3D12 native replay readiness failed: query command sequence "
                << query.sequence << " query range is out of bounds";
        last_error_ = message.str();
        return false;
      }
      if (query.resolve) {
        constexpr std::uint64_t kQueryResolveBytes = 8;
        if (!require_buffer_range(
                query.dst_buffer_object_id,
                query.aligned_dst_buffer_offset,
                static_cast<std::uint64_t>(query.query_count) * kQueryResolveBytes,
                query.sequence,
                "ResolveQueryData")) {
          return false;
        }
      }
    }
    for (const auto &predication : command_list.predications) {
      if (predication.buffer_object_id != 0) {
        constexpr std::uint64_t kPredicationBytes = 8;
        if (!require_buffer_range(
                predication.buffer_object_id,
                predication.aligned_buffer_offset,
                kPredicationBytes,
                predication.sequence,
                "SetPredication")) {
          return false;
        }
      }
    }
    for (const auto &dispatch : command_list.ray_dispatches) {
      if (!require_gpuva_resolved(dispatch.ray_generation_shader_record, "DispatchRays ray generation shader record") ||
          !require_gpuva_resolved(dispatch.miss_shader_table, "DispatchRays miss shader table") ||
          !require_gpuva_resolved(dispatch.hit_group_table, "DispatchRays hit group table") ||
          !require_gpuva_resolved(dispatch.callable_shader_table, "DispatchRays callable shader table")) {
        return false;
      }
      last_error_ = "D3D12 native replay readiness failed: DispatchRays requires a DXMT-backed replay backend";
      return false;
    }
    for (const auto &resolve : command_list.resolves) {
      if (!require_resolve_ready(resolve)) {
        return false;
      }
    }
  }

  for (const auto &command : replay_commands_) {
    if (!require_dynamic_state_ready(command)) {
      return false;
    }
    if (command.kind == ReplayCommandKind::RenderPass &&
        !require_render_pass_targets_ready(command)) {
      return false;
    }
    if (command.kind == ReplayCommandKind::TemporalUpscale) {
      last_error_ = "D3D12 native replay readiness failed: TemporalUpscale requires a DXMT-backed replay backend";
      return false;
    }
    if (command.kind == ReplayCommandKind::UnsupportedNative) {
      last_error_ = "D3D12 native replay readiness failed: unsupported command " + command.function_name;
      return false;
    }
  }
  for (const auto &[object_id, pipeline] : pipelines_) {
    (void)object_id;
    if (pipeline.requires_dxmt_backend) {
      last_error_ = "D3D12 native replay readiness failed: pipeline requires a DXMT-backed replay backend";
      return false;
    }
  }

  return true;
}

bool D3D12ReplayBackend::finalize_replay()
{
  if (!validate_replay_closure() || !validate_native_replay_readiness()) {
    return false;
  }
  D3D12NativeReplayer native_replayer(*this);
  return native_replayer.replay(last_error_);
}

bool D3D12ReplayBackend::validate_only()
{
  return validate_replay_closure();
}
