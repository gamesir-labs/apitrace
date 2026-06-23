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
    const bool perf_diag = std::getenv("DXMT_PERF_STATS") != nullptr;

    replay_milestone("[event-ordered] replay:begin");
    event_ordered_categories_hit_.clear();
    event_ordered_categories_hit_order_.clear();
    event_ordered_recorded_commands_ = 0;
    event_ordered_lists_reset_ = 0;
    event_ordered_lists_closed_ = 0;
    event_ordered_resource_updates_applied_ = 0;
    event_ordered_copy_descriptors_applied_ = 0;
    event_ordered_command_lists_submitted_ = 0;
    event_ordered_draws_recorded_ = 0;
    event_ordered_dispatches_recorded_ = 0;
    event_ordered_presents_executed_ = 0;
    event_ordered_present_errors_ = 0;
    event_ordered_queue_signals_ = 0;
    event_ordered_queue_waits_ = 0;
    event_ordered_fence_signals_ = 0;
    event_ordered_cpu_waits_ = 0;
    event_ordered_cpu_wait_timeouts_ = 0;
    event_ordered_stop_requested_ = false;
    event_ordered_presented_frames_.clear();
    resource_update_timeline_.clear();
    descriptor_copy_timeline_.clear();
    next_resource_update_index_ = 0;
    next_descriptor_copy_index_ = 0;
    event_ordered_resource_update_storage_.clear();
    event_ordered_ingested_update_keys_.clear();
    event_ordered_resource_version_update_cursors_.clear();
    event_ordered_resource_update_cursors_.clear();
    event_ordered_copied_descriptor_storage_.clear();
    event_ordered_resource_updates_applied_keys_.clear();
    event_ordered_copy_descriptors_applied_keys_.clear();
    backend_.event_ordered_counters_.clear();
    backend_.event_ordered_perf_breakdown_.clear();
    backend_.event_ordered_replay_timing_ = {};
    begin_event_ordered_present_timing();
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

    {
      ScopedMsAccumulator timer(backend_.event_ordered_replay_timing_.bootstrap_ms);
      if (!log_stage("event_ordered_bootstrap_device", [&] { return bootstrap_device(error); })) {
        return false;
      }
    }

    begin_frame_timing();
    struct EventOrderedFrameTimingScope {
      D3D12NativeReplayer *self = nullptr;
      ~EventOrderedFrameTimingScope()
      {
        if (self) {
          self->finish_frame_timing();
        }
      }
    } frame_timing_scope{this};

    bool swapchain_ready = swapchain_bootstrapped();
    std::uint64_t event_count = 0;
    for (const auto &event : reader.events()) {
      std::chrono::steady_clock::time_point _iter_t0;
      if (perf_diag) {
        _iter_t0 = std::chrono::steady_clock::now();
      }
      const std::uint64_t event_sequence = event.callsite.sequence;
      {
        std::chrono::steady_clock::time_point _t0;
        if (perf_diag) {
          _t0 = std::chrono::steady_clock::now();
        }
        ScopedMsAccumulator timer(backend_.event_ordered_replay_timing_.semantic_replay_event_ms);
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
        if (perf_diag) {
          backend_.event_ordered_replay_timing_.us_replay_event +=
              std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _t0).count();
        }
      }

      ++event_count;
      {
        std::chrono::steady_clock::time_point _t0;
        if (perf_diag) {
          _t0 = std::chrono::steady_clock::now();
        }
        ScopedMsAccumulator timer(backend_.event_ordered_replay_timing_.content_sync_ms);
        event_ordered_sync_resource_content_before_consumer(event);
        if (perf_diag) {
          backend_.event_ordered_replay_timing_.us_content_sync +=
              std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _t0).count();
        }
      }
      if (!event_ordered_maybe_bootstrap_swapchain(event, swapchain_ready, error)) {
        return false;
      }

      const auto dispatch_category = event_ordered_dispatch_category(event);
      {
        std::chrono::steady_clock::time_point _t0;
        if (perf_diag) {
          _t0 = std::chrono::steady_clock::now();
        }
        ScopedMsAccumulator timer(event_ordered_dispatch_timing_bucket(dispatch_category));
        dispatch_live(event);
        if (perf_diag) {
          backend_.event_ordered_replay_timing_.us_dispatch +=
              std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _t0).count();
        }
      }
      if (perf_diag) {
        backend_.event_ordered_replay_timing_.us_iter_total +=
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _iter_t0).count();
      }
      if (event_ordered_stop_requested_) {
        break;
      }
    }

    {
      ScopedMsAccumulator timer(backend_.event_ordered_replay_timing_.final_drain_ms);
      if (!event_ordered_final_drain_queues(error)) {
        return false;
      }
    }
    const std::string summary = "[event-ordered] iterated " + std::to_string(event_count) +
                                " events, categories_hit=" + event_ordered_categories_summary();
    event_ordered_log(summary);
    if (perf_diag) {
      backend_.event_ordered_counters_ = event_ordered_counters_summary();
      backend_.event_ordered_perf_breakdown_ = event_ordered_perf_breakdown_summary();
      event_ordered_log(backend_.event_ordered_counters_);
    }
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

    {
      ScopedMsAccumulator timer(backend_.event_ordered_replay_timing_.bootstrap_ms);
      if (!log_stage("event_ordered_bootstrap_swapchain", [&] { return bootstrap_swapchain(error); })) {
        return false;
      }
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

  enum class EventOrderedDispatchCategory {
    Create,
    Record,
    SubmitPresent,
  };

  EventOrderedDispatchCategory event_ordered_dispatch_category(const trace::EventRecord &event) const
  {
    const std::string_view function_name(event.callsite.function_name);
    if (event.kind == trace::EventKind::Boundary &&
        event.boundary == trace::BoundaryKind::Present) {
      return EventOrderedDispatchCategory::SubmitPresent;
    }
    if (function_name == "IDXGISwapChain::Present" ||
        function_name == "ID3D12CommandQueue::ExecuteCommandLists" ||
        function_name == "ID3D12CommandQueue::Signal" ||
        function_name == "ID3D12CommandQueue::Wait" ||
        function_name == "ID3D12Fence::Signal" ||
        function_name == "ID3D12Fence::SetEventOnCompletion") {
      return EventOrderedDispatchCategory::SubmitPresent;
    }
    if (event_ordered_is_record_call(event)) {
      return EventOrderedDispatchCategory::Record;
    }
    return EventOrderedDispatchCategory::Create;
  }

  std::uint64_t &event_ordered_dispatch_timing_bucket(EventOrderedDispatchCategory category)
  {
    switch (category) {
    case EventOrderedDispatchCategory::Create:
      return backend_.event_ordered_replay_timing_.dispatch_create_ms;
    case EventOrderedDispatchCategory::Record:
      return backend_.event_ordered_replay_timing_.dispatch_record_ms;
    case EventOrderedDispatchCategory::SubmitPresent:
      return backend_.event_ordered_replay_timing_.dispatch_submit_present_ms;
    }
    return backend_.event_ordered_replay_timing_.dispatch_create_ms;
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
    if (function_name == "ID3D12CommandQueue::UpdateTileMappings") {
      return "UpdateTileMappings(P4)";
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
           " copy_descriptors_applied=" + std::to_string(event_ordered_copy_descriptors_applied_) +
           " command_lists_submitted=" + std::to_string(event_ordered_command_lists_submitted_) +
           " draws_recorded=" + std::to_string(event_ordered_draws_recorded_) +
           " dispatches_recorded=" + std::to_string(event_ordered_dispatches_recorded_) +
           " presents_executed=" + std::to_string(event_ordered_presents_executed_) +
           " present_errors=" + std::to_string(event_ordered_present_errors_) +
           " queue_signals=" + std::to_string(event_ordered_queue_signals_) +
           " queue_waits=" + std::to_string(event_ordered_queue_waits_) +
           " fence_signals=" + std::to_string(event_ordered_fence_signals_) +
           " cpu_waits=" + std::to_string(event_ordered_cpu_waits_) +
           " cpu_wait_timeouts=" + std::to_string(event_ordered_cpu_wait_timeouts_);
  }

  std::string event_ordered_perf_breakdown_summary() const
  {
    const auto &timing = backend_.event_ordered_replay_timing_;
    return "bootstrap_ms=" + std::to_string(timing.bootstrap_ms) +
           " semantic_replay_event_ms=" + std::to_string(timing.semantic_replay_event_ms) +
           " content_sync_ms=" + std::to_string(timing.content_sync_ms) +
           " dispatch_create_ms=" + std::to_string(timing.dispatch_create_ms) +
           " dispatch_record_ms=" + std::to_string(timing.dispatch_record_ms) +
           " dispatch_submit_present_ms=" + std::to_string(timing.dispatch_submit_present_ms) +
           " pso_create_ms=" + std::to_string(timing.pso_create_ms) +
           " final_drain_ms=" + std::to_string(timing.final_drain_ms) +
           " first_present_ms=" + std::to_string(timing.first_present_ms) +
           " steady_per_frame_ms=" + std::to_string(timing.steady_per_frame_ms) +
           " us_replay_event_ms=" + std::to_string(timing.us_replay_event / 1000.0) +
           " us_content_sync_ms=" + std::to_string(timing.us_content_sync / 1000.0) +
           " us_dispatch_ms=" + std::to_string(timing.us_dispatch / 1000.0) +
           " us_iter_total_ms=" + std::to_string(timing.us_iter_total / 1000.0);
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
#include "eo_submit.inc"

  struct EventOrderedPresentPayload {
    std::uint64_t frame_index = 0;
    UINT sync_interval = 0;
    UINT flags = 0;
  };

  void event_ordered_present_error(const trace::EventRecord &event, std::string error)
  {
    const std::string line = "[event-ordered] present:error seq=" +
                             std::to_string(event.callsite.sequence) +
                             " fn=" + event_ordered_event_name(event) +
                             " " + error;
    backend_.last_error_ = std::move(error);
    ++event_ordered_present_errors_;
    event_ordered_log(line);
  }

  bool event_ordered_present_payload(
      const trace::EventRecord &event,
      EventOrderedPresentPayload &present,
      std::string &error) const
  {
    json payload;
    if (!payload_to_json(event, payload, error)) {
      return false;
    }
    if (!require_payload_key(event, payload, "frame_index", error) ||
        !require_payload_key(event, payload, "sync_interval", error) ||
        !require_payload_key(event, payload, "flags", error)) {
      return false;
    }
    present.frame_index = payload_u64(payload, "frame_index");
    present.sync_interval = static_cast<UINT>(payload_u32(payload, "sync_interval"));
    present.flags = static_cast<UINT>(payload_u32(payload, "flags"));
    return true;
  }

  bool event_ordered_verify_present_semantics(
      const trace::EventRecord &event,
      const EventOrderedPresentPayload &present,
      bool boundary,
      std::string &error) const
  {
    const auto semantic_it = backend_.present_semantics_.find(present.frame_index);
    if (semantic_it == backend_.present_semantics_.end() || !semantic_it->second.has_call) {
      error = "Present references missing captured IDXGISwapChain::Present frame_index=" +
              std::to_string(present.frame_index);
      return false;
    }
    const auto &semantic = semantic_it->second;
    if (semantic.sync_interval != present.sync_interval || semantic.flags != present.flags) {
      error = "Present parameters do not match captured IDXGISwapChain::Present frame_index=" +
              std::to_string(present.frame_index);
      return false;
    }
    if (boundary) {
      if (!semantic.has_boundary || semantic.boundary_sequence != event.callsite.sequence) {
        error = "Present boundary sequence does not match indexed Present semantics frame_index=" +
                std::to_string(present.frame_index);
        return false;
      }
    } else if (semantic.call_sequence != event.callsite.sequence) {
      error = "IDXGISwapChain::Present call sequence does not match indexed Present semantics frame_index=" +
              std::to_string(present.frame_index);
      return false;
    }
    return true;
  }

  UINT event_ordered_current_back_buffer_index() const
  {
    return swap_chain_ ? swap_chain_->GetCurrentBackBufferIndex() : headless_back_buffer_index_;
  }

  void event_ordered_handle_present_call(const trace::EventRecord &event)
  {
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      event_ordered_present_error(event, "IDXGISwapChain::Present missing swapchain object ref");
      return;
    }

    EventOrderedPresentPayload present;
    std::string error;
    if (!event_ordered_present_payload(event, present, error) ||
        !event_ordered_verify_present_semantics(event, present, false, error)) {
      event_ordered_present_error(event, std::move(error));
    }
  }

  void event_ordered_handle_present_boundary(const trace::EventRecord &event)
  {
    EventOrderedPresentPayload present;
    std::string error;
    if (!event_ordered_present_payload(event, present, error) ||
        !event_ordered_verify_present_semantics(event, present, true, error)) {
      event_ordered_present_error(event, std::move(error));
      return;
    }

    if (!event_ordered_presented_frames_.insert(present.frame_index).second) {
      event_ordered_log("[event-ordered] present:skip-duplicate frame=" +
                        std::to_string(present.frame_index));
      return;
    }

    const UINT back_buffer_index = event_ordered_current_back_buffer_index();
    if (back_buffer_index >= kBufferCount) {
      event_ordered_present_error(
          event,
          "Present resolved invalid back-buffer index=" + std::to_string(back_buffer_index));
      return;
    }

    if (!capture_present_frame(present.frame_index, present.sync_interval, present.flags, error)) {
      event_ordered_present_error(
          event,
          error.empty() ? "capture_present_frame failed for frame=" +
                              std::to_string(present.frame_index)
                        : std::move(error));
    }

    if (swap_chain_) {
      const HRESULT hr = swap_chain_->Present(present.sync_interval, present.flags);
      record_present_semantics(present.sync_interval, present.flags, hr);
      if (FAILED(hr)) {
        event_ordered_present_error(event, hresult_error("IDXGISwapChain::Present", hr));
      }
    } else {
      record_present_semantics(present.sync_interval, present.flags, S_OK);
      headless_back_buffer_index_ = (headless_back_buffer_index_ + 1) % kBufferCount;
    }

    ++event_ordered_presents_executed_;
    note_event_ordered_present_timing();
    log_frame_timing(present.frame_index);
    if (present_delay_ms_ != 0) {
#ifdef _WIN32
      Sleep(present_delay_ms_);
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(present_delay_ms_));
#endif
    }
    if (swap_chain_ && !pump_messages()) {
      event_ordered_present_error(event, "replay window closed");
      event_ordered_stop_requested_ = true;
    }
    if (replay_stop_after_present_frame_ != 0 &&
        present.frame_index >= replay_stop_after_present_frame_) {
      event_ordered_stop_requested_ = true;
    }
  }

  void dispatch_live(const trace::EventRecord &event)
  {
    const std::string_view function_name(event.callsite.function_name);
    if (event.kind == trace::EventKind::Boundary &&
        event.boundary == trace::BoundaryKind::Present) {
      event_ordered_handle_present_boundary(event);
      return;
    }
    if (function_name == "IDXGISwapChain::Present") {
      event_ordered_handle_present_call(event);
      return;
    }
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
    if (function_name == "ID3D12CommandQueue::ExecuteCommandLists") {
      event_ordered_handle_execute_command_lists(event);
      return;
    }
    if (function_name == "ID3D12CommandQueue::UpdateTileMappings") {
      event_ordered_log_stub_once("UpdateTileMappings(P4)", event);
      return;
    }
    if (function_name == "ID3D12CommandQueue::Signal") {
      event_ordered_handle_queue_signal(event);
      return;
    }
    if (function_name == "ID3D12CommandQueue::Wait") {
      event_ordered_handle_queue_wait(event);
      return;
    }
    if (function_name == "ID3D12Fence::Signal") {
      event_ordered_handle_fence_signal(event);
      return;
    }
    if (function_name == "ID3D12Fence::SetEventOnCompletion") {
      event_ordered_handle_fence_set_event_on_completion(event);
      return;
    }

    event_ordered_log_stub_once(event_ordered_category(event), event);

    // TODO(event-ordered): materialize remaining live operations for Map,
    // Unmap, Present, ReleaseDestroy, and Default.
  }
