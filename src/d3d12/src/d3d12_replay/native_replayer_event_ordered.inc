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

    bool bootstrapped = false;
    std::uint64_t event_count = 0;
    for (const auto &event : reader.events()) {
      if (!backend_.replay_event(event)) {
        if (!backend_.last_error().empty()) {
          error = "sequence " + std::to_string(event.callsite.sequence) + " " +
                  event_ordered_event_name(event) + ": " + backend_.last_error();
        } else {
          error = "sequence " + std::to_string(event.callsite.sequence) + " " +
                  event_ordered_event_name(event) + ": D3D12 event-ordered replay failed";
        }
        return false;
      }

      ++event_count;
      if (!bootstrapped && event_ordered_bootstrap_ready()) {
        if (!log_stage("event_ordered_bootstrap", [&] { return bootstrap(error); })) {
          return false;
        }
        bootstrapped = true;
      }

      dispatch_live(event);
    }

    if (!bootstrapped) {
      return fail(error, "event-ordered replay could not bootstrap: missing device or swapchain back-buffer semantics");
    }

    wait_for_gpu();
    const std::string summary = "[event-ordered] iterated " + std::to_string(event_count) +
                                " events, categories_hit=" + event_ordered_categories_summary();
    event_ordered_log(summary);
    replay_milestone("[event-ordered] replay:done");
    return true;
  }

private:
  bool event_ordered_bootstrap_ready() const
  {
    UINT width = 0;
    UINT height = 0;
    return !backend_.devices_.empty() && infer_window_size(width, height);
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

  void dispatch_live(const trace::EventRecord &event)
  {
    const std::string category = event_ordered_category(event);
    if (event_ordered_categories_hit_.insert(category).second) {
      event_ordered_categories_hit_order_.push_back(category);
      event_ordered_log("[event-ordered] stub: " + category + " fn=" + event_ordered_event_name(event));
    }

    // TODO(event-ordered): materialize live operations by reusing resources_, pipelines_,
    // descriptor_heaps_, command_lists_, back_buffers_, heaps_, root_signatures_,
    // command_signatures_, query_heaps_, queue_, queues_by_type_, and fence_.
  }
