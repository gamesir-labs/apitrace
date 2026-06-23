void D3D12ReplayBackend::shutdown()
{
  objects_.clear();
  submissions_.clear();
  present_frames_.clear();
  present_semantics_.clear();
  command_lists_.clear();
  command_queues_.clear();
  command_allocators_.clear();
  descriptor_heaps_.clear();
  descriptors_.clear();
  samplers_.clear();
  descriptor_copies_.clear();
  command_signatures_.clear();
  fences_.clear();
  devices_.clear();
  heaps_.clear();
  query_heaps_.clear();
  resources_.clear();
  resource_versions_.clear();
  live_resource_version_indices_.clear();
  resource_versions_by_id_.clear();
  resource_versions_by_va_.clear();
  max_resource_va_width_ = 0;
  pipelines_.clear();
  pipeline_versions_.clear();
  pipeline_versions_by_id_.clear();
  root_signatures_.clear();
  fence_operations_.clear();
  replay_commands_.clear();
  tile_mapping_updates_.clear();
  bundle_root_.clear();
  event_ordered_counters_.clear();
  event_ordered_perf_breakdown_.clear();
  event_ordered_init_timing_ = {};
  event_ordered_replay_timing_ = {};
  event_ordered_timing_enabled_ = false;
  initialized_ = false;
  commands_replayed_ = 0;
  frames_seen_ = 0;
  presents_seen_ = 0;
  pipeline_assets_read_ = 0;
  semantic_calls_seen_ = 0;
  draw_calls_seen_ = 0;
  dispatch_calls_seen_ = 0;
  last_sequence_ = 0;
  event_ordered_metal_calls_replayed_ = 0;
}

void D3D12ReplayBackend::set_event_ordered_timing_enabled(bool enabled) noexcept
{
  event_ordered_timing_enabled_ = enabled;
}

const std::string &D3D12ReplayBackend::last_error() const noexcept
{
  return last_error_;
}

const std::string &D3D12ReplayBackend::event_ordered_counters() const noexcept
{
  return event_ordered_counters_;
}

const std::string &D3D12ReplayBackend::event_ordered_perf_breakdown() const noexcept
{
  return event_ordered_perf_breakdown_;
}

const D3D12ReplayBackend::EventOrderedInitTiming &
D3D12ReplayBackend::event_ordered_init_timing() const noexcept
{
  return event_ordered_init_timing_;
}

const std::vector<D3D12ReplayBackend::DescriptorSemanticState> &
D3D12ReplayBackend::descriptors() const noexcept
{
  return descriptors_;
}

const D3D12ReplayBackend::CommandListSemanticState *
D3D12ReplayBackend::command_list_state(trace::ObjectId object_id) const noexcept
{
  const auto it = command_lists_.find(object_id);
  return it == command_lists_.end() ? nullptr : &it->second;
}

const std::vector<D3D12ReplayBackend::CopyDescriptorSemanticState> &
D3D12ReplayBackend::descriptor_copies() const noexcept
{
  return descriptor_copies_;
}

const std::vector<D3D12ReplayBackend::ReplayCommandRecord> &
D3D12ReplayBackend::replay_commands() const noexcept
{
  return replay_commands_;
}
