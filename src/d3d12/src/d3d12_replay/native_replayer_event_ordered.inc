  bool replay_event_ordered(const trace::TraceBundleReader &reader, std::string &error)
  {
    if (!d3d12_native_replay_enabled()) {
      error = "D3D12 native command replay incomplete";
      return false;
    }
    if (!backend_.initialized_) {
      error = "D3D12 event-ordered replay backend was not initialized";
      return false;
    }

    replay_milestone("[event-ordered] replay:begin");
    event_ordered_categories_hit_.clear();
    event_ordered_categories_hit_order_.clear();
    event_ordered_recorded_commands_ = 0;
    event_ordered_lists_reset_ = 0;
    event_ordered_lists_closed_ = 0;
    event_ordered_resource_updates_applied_ = 0;
    event_ordered_copy_descriptors_applied_ = 0;
    resource_update_timeline_.clear();
    descriptor_copy_timeline_.clear();
    next_resource_update_index_ = 0;
    next_descriptor_copy_index_ = 0;
    event_ordered_resource_update_storage_.clear();
    event_ordered_copied_descriptor_storage_.clear();
    event_ordered_resource_updates_applied_keys_.clear();
    event_ordered_copy_descriptors_applied_keys_.clear();
    struct EventOrderedRecordingScope {
      D3D12NativeReplayer *self = nullptr;
      bool saved = false;
      explicit EventOrderedRecordingScope(D3D12NativeReplayer *owner)
          : self(owner), saved(owner->event_ordered_recording_)
      {
        self->event_ordered_recording_ = true;
      }
      ~EventOrderedRecordingScope()
      {
        self->event_ordered_recording_ = saved;
      }
    } event_ordered_recording_scope(this);

    if (!log_stage("event_ordered_bootstrap_device", [&] { return bootstrap_device(error); })) {
      return false;
    }

    bool swapchain_ready = swapchain_bootstrapped();
    std::uint64_t event_count = 0;
    for (const auto &event : reader.events()) {
      const std::uint64_t event_sequence = event.callsite.sequence;
      if (!backend_.replay_event(event)) {
        if (!backend_.last_error().empty()) {
          error = "sequence " + std::to_string(event_sequence) + " " +
                  event_ordered_event_name(event) + ": " + backend_.last_error();
        } else {
          error = "sequence " + std::to_string(event_sequence) + " " +
                  event_ordered_event_name(event) + ": D3D12 event-ordered replay failed";
        }
        return false;
      }

      ++event_count;
      event_ordered_sync_resource_content_before_consumer(event);
      if (!event_ordered_maybe_bootstrap_swapchain(event, swapchain_ready, error)) {
        return false;
      }

      dispatch_live(event);
    }

    wait_for_gpu();
    const std::string summary = "[event-ordered] iterated " + std::to_string(event_count) +
                                " events, categories_hit=" + event_ordered_categories_summary();
    event_ordered_log(summary);
    event_ordered_log(event_ordered_counters_summary());
    replay_milestone("[event-ordered] replay:done");
    return true;
  }

private:
  bool event_ordered_swapchain_bootstrap_ready() const
  {
    UINT width = 0;
    UINT height = 0;
    return infer_window_size(width, height);
  }

  bool event_ordered_is_swapchain_create_event(const trace::EventRecord &event) const
  {
    return event_ordered_contains(event.callsite.function_name, "CreateSwapChain");
  }

  bool event_ordered_is_present_event(const trace::EventRecord &event) const
  {
    if (event.kind == trace::EventKind::Boundary && event.boundary == trace::BoundaryKind::Present) {
      return true;
    }
    return event.callsite.function_name == "IDXGISwapChain::Present";
  }

  bool event_ordered_is_swapchain_backbuffer_alias_event(const trace::EventRecord &event) const
  {
    if (!event_ordered_contains(event.callsite.function_name, "CreateCommittedResource")) {
      return false;
    }

    json payload;
    std::string ignored_error;
    if (!payload_to_json(event, payload, ignored_error)) {
      return false;
    }
    return payload.value("swapchain_back_buffer", false);
  }

  bool event_ordered_maybe_bootstrap_swapchain(
      const trace::EventRecord &event,
      bool &swapchain_ready,
      std::string &error)
  {
    if (swapchain_ready) {
      return true;
    }

    const bool swapchain_create = event_ordered_is_swapchain_create_event(event);
    const bool backbuffer_alias = event_ordered_is_swapchain_backbuffer_alias_event(event);
    const bool present = event_ordered_is_present_event(event);
    if (!swapchain_create && !backbuffer_alias && !present) {
      return true;
    }

    if (!event_ordered_swapchain_bootstrap_ready()) {
      if (backbuffer_alias) {
        return fail(
            error,
            "event-ordered replay reached a swapchain back-buffer alias before swapchain semantics were ready");
      }
      if (present) {
        return fail(
            error,
            "event-ordered replay reached Present before swapchain back-buffer semantics were ready");
      }
      return true;
    }

    if (!log_stage("event_ordered_bootstrap_swapchain", [&] { return bootstrap_swapchain(error); })) {
      return false;
    }
    swapchain_ready = true;
    return true;
  }

  void event_ordered_log(const std::string &line) const
  {
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
    replay_milestone(line.c_str());
  }

  static bool event_ordered_starts_with(std::string_view text, std::string_view prefix)
  {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
  }

  static bool event_ordered_contains(std::string_view text, std::string_view needle)
  {
    return text.find(needle) != std::string_view::npos;
  }

  static std::string event_ordered_event_name(const trace::EventRecord &event)
  {
    if (!event.callsite.function_name.empty()) {
      return event.callsite.function_name;
    }
    if (event.kind == trace::EventKind::Boundary) {
      switch (event.boundary) {
      case trace::BoundaryKind::Frame:
        return "Boundary::Frame";
      case trace::BoundaryKind::CommandList:
        return "Boundary::CommandList";
      case trace::BoundaryKind::Submit:
        return "Boundary::Submit";
      case trace::BoundaryKind::Present:
        return "Boundary::Present";
      case trace::BoundaryKind::Fence:
        return "Boundary::Fence";
      case trace::BoundaryKind::Barrier:
        return "Boundary::Barrier";
      case trace::BoundaryKind::DebugMarker:
        return "Boundary::DebugMarker";
      }
    }
    return "Event";
  }

  static bool event_ordered_is_record_call(std::string_view function_name)
  {
    if (function_name == "ID3D12CommandAllocator::Reset") {
      return true;
    }
    if (!event_ordered_starts_with(function_name, "ID3D12GraphicsCommandList")) {
      return false;
    }
    const auto separator = function_name.find("::");
    if (separator == std::string_view::npos) {
      return false;
    }
    const auto method = function_name.substr(separator + 2);
    return method == "Reset" ||
           method == "Close" ||
           event_ordered_starts_with(method, "Set") ||
           event_ordered_starts_with(method, "Draw") ||
           event_ordered_starts_with(method, "Dispatch");
  }

  bool event_ordered_is_record_call(const trace::EventRecord &event) const
  {
    const std::string_view function_name(event.callsite.function_name);
    if (function_name == "ID3D12CommandAllocator::Reset" ||
        function_name == "ID3D12GraphicsCommandList::Reset" ||
        function_name == "ID3D12GraphicsCommandList::Close") {
      return true;
    }
    if (backend_.replay_commands_.empty()) {
      return false;
    }
    const auto &record = backend_.replay_commands_.back();
    return record.sequence == event.callsite.sequence &&
           record.function_name == event.callsite.function_name &&
           record.command_list_object_id != 0 &&
           record.kind != D3D12ReplayBackend::ReplayCommandKind::Unknown;
  }

  static std::string event_ordered_category(const trace::EventRecord &event)
  {
    const std::string_view function_name(event.callsite.function_name);
    if (event.kind == trace::EventKind::ObjectDestroy ||
        event_ordered_contains(function_name, "::Release") ||
        event_ordered_contains(function_name, "::Destroy")) {
      return "ReleaseDestroy";
    }
    if (event.kind == trace::EventKind::Boundary && event.boundary == trace::BoundaryKind::Present) {
      return "Present";
    }
    if (function_name == "IDXGISwapChain::Present") {
      return "Present";
    }
    if (event_ordered_contains(function_name, "CreateHeap")) {
      return "CreateHeap";
    }
    if (event_ordered_contains(function_name, "CreateCommittedResource")) {
      return "CreateCommittedResource";
    }
    if (event_ordered_contains(function_name, "CreatePlacedResource")) {
      return "CreatePlacedResource";
    }
    if (event_ordered_contains(function_name, "CreateReservedResource")) {
      return "CreateReservedResource";
    }
    if (event_ordered_contains(function_name, "CreateDescriptorHeap")) {
      return "CreateDescriptorHeap";
    }
    if (function_name == "ID3D12Device::CreateConstantBufferView") {
      return "CreateView(CBV)";
    }
    if (function_name == "ID3D12Device::CreateShaderResourceView") {
      return "CreateView(SRV)";
    }
    if (function_name == "ID3D12Device::CreateUnorderedAccessView") {
      return "CreateView(UAV)";
    }
    if (function_name == "ID3D12Device::CreateRenderTargetView") {
      return "CreateView(RTV)";
    }
    if (function_name == "ID3D12Device::CreateDepthStencilView") {
      return "CreateView(DSV)";
    }
    if (function_name == "ID3D12Device::CreateSampler") {
      return "CreateView(Sampler)";
    }
    if (function_name == "ID3D12Device::CreateDescriptorViewBatch") {
      return "CreateView(Batch)";
    }
    if (event_ordered_contains(function_name, "CreateRootSignature")) {
      return "CreateRootSignature";
    }
    if (event_ordered_contains(function_name, "CreateCommandSignature")) {
      return "CreateCommandSignature";
    }
    if (function_name == "ID3D12Device::CreateGraphicsPipelineState") {
      return "CreatePipelineState(graphics)";
    }
    if (function_name == "ID3D12Device::CreateComputePipelineState") {
      return "CreatePipelineState(compute)";
    }
    if (function_name == "ID3D12Device2::CreatePipelineState") {
      return "CreatePipelineState(graphics/compute)";
    }
    if (event_ordered_contains(function_name, "CreateQueryHeap")) {
      return "CreateQueryHeap";
    }
    if (event_ordered_contains(function_name, "CreateCommandAllocator")) {
      return "CreateCommandAllocator";
    }
    if (event_ordered_contains(function_name, "CreateCommandList")) {
      return "CreateCommandList";
    }
    if (event_ordered_contains(function_name, "CreateCommandQueue")) {
      return "CreateCommandQueue";
    }
    if (event_ordered_contains(function_name, "CreateFence")) {
      return "CreateFence";
    }
    if (event_ordered_is_record_call(function_name)) {
      return "record";
    }
    if (function_name == "ID3D12CommandQueue::ExecuteCommandLists") {
      return "ExecuteCommandLists";
    }
    if (function_name == "ID3D12CommandQueue::Signal") {
      return "QueueSignal";
    }
    if (function_name == "ID3D12CommandQueue::Wait") {
      return "QueueWait";
    }
    if (function_name == "ID3D12Fence::Signal") {
      return "FenceSignal";
    }
    if (function_name == "ID3D12Fence::SetEventOnCompletion") {
      return "FenceSetEventOnCompletion";
    }
    if (function_name == "ID3D12Resource::Map") {
      return "Map";
    }
    if (function_name == "ID3D12Resource::Unmap") {
      return "Unmap";
    }
    return "Default";
  }

  std::string event_ordered_categories_summary() const
  {
    if (event_ordered_categories_hit_order_.empty()) {
      return "<none>";
    }
    std::string summary;
    for (std::size_t index = 0; index < event_ordered_categories_hit_order_.size(); ++index) {
      if (index != 0) {
        summary += ",";
      }
      summary += event_ordered_categories_hit_order_[index];
    }
    return summary;
  }

  std::string event_ordered_counters_summary() const
  {
    return "[event-ordered] counters recorded_commands=" +
           std::to_string(event_ordered_recorded_commands_) +
           " lists_reset=" + std::to_string(event_ordered_lists_reset_) +
           " lists_closed=" + std::to_string(event_ordered_lists_closed_) +
           " resource_updates_applied=" + std::to_string(event_ordered_resource_updates_applied_) +
           " copy_descriptors_applied=" + std::to_string(event_ordered_copy_descriptors_applied_);
  }

  void event_ordered_log_stub_once(const std::string &category, const trace::EventRecord &event)
  {
    if (event_ordered_categories_hit_.insert(category).second) {
      event_ordered_categories_hit_order_.push_back(category);
      event_ordered_log("[event-ordered] stub: " + category + " fn=" + event_ordered_event_name(event));
    }
  }

#include "eo_create_memory.inc"
#include "eo_create_resources.inc"
#include "eo_create_descriptors.inc"
#include "eo_create_pipelines.inc"
#include "eo_create_command_objects.inc"
#include "eo_content.inc"
#include "eo_record.inc"

  void dispatch_live(const trace::EventRecord &event)
  {
    const std::string_view function_name(event.callsite.function_name);
    if (event_ordered_contains(function_name, "CreateHeap") ||
        event_ordered_contains(function_name, "CreateQueryHeap")) {
      materialize_create_memory(event);
      return;
    }
    if (event_ordered_contains(function_name, "CreateCommittedResource") ||
        event_ordered_contains(function_name, "CreatePlacedResource") ||
        event_ordered_contains(function_name, "CreateReservedResource")) {
      materialize_create_resources(event);
      return;
    }
    if (event_ordered_contains(function_name, "CreateDescriptorHeap") ||
        function_name == "ID3D12Device::CreateDescriptorViewBatch" ||
        function_name == "ID3D12Device::CreateConstantBufferView" ||
        function_name == "ID3D12Device::CreateShaderResourceView" ||
        function_name == "ID3D12Device::CreateUnorderedAccessView" ||
        function_name == "ID3D12Device::CreateRenderTargetView" ||
        function_name == "ID3D12Device::CreateDepthStencilView" ||
        function_name == "ID3D12Device::CreateSampler") {
      materialize_create_descriptors(event);
      return;
    }
    if (function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
        function_name == "ID3D12Device::CreateComputePipelineState" ||
        function_name == "ID3D12Device2::CreatePipelineState" ||
        event_ordered_contains(function_name, "CreateRootSignature") ||
        event_ordered_contains(function_name, "CreateCommandSignature")) {
      materialize_create_pipelines(event);
      return;
    }
    if (event_ordered_contains(function_name, "CreateCommandQueue") ||
        event_ordered_contains(function_name, "CreateCommandAllocator") ||
        event_ordered_contains(function_name, "CreateCommandList") ||
        event_ordered_contains(function_name, "CreateFence")) {
      materialize_create_command_objects(event);
      return;
    }
    if (event_ordered_is_record_call(event)) {
      handle_record_event(event);
      return;
    }
    if (event_ordered_is_copy_descriptors_event(event)) {
      event_ordered_apply_copy_descriptors(event);
      return;
    }

    event_ordered_log_stub_once(event_ordered_category(event), event);

    // TODO(event-ordered): materialize remaining live operations for record,
    // ExecuteCommandLists, QueueSignal, QueueWait, FenceSignal,
    // FenceSetEventOnCompletion, Map, Unmap, Present, ReleaseDestroy, and Default.
  }
