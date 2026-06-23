  void event_ordered_submit_error(const trace::EventRecord &event, std::string error)
  {
    const std::string line = "[event-ordered] submit:error seq=" +
                             std::to_string(event.callsite.sequence) +
                             " fn=" + event_ordered_event_name(event) +
                             " " + error;
    backend_.last_error_ = std::move(error);
    event_ordered_log(line);
  }

  bool event_ordered_payload_fence_value(
      const trace::EventRecord &event,
      std::uint64_t &value,
      std::string &error) const
  {
    json payload;
    if (!payload_to_json(event, payload, error)) {
      return false;
    }
    const auto value_it = payload.find("fence_value");
    if (value_it == payload.end()) {
      error = "fence operation missing fence_value";
      return false;
    }
    value = json_u64(*value_it);
    return true;
  }

  bool event_ordered_queue_and_fence(
      const trace::EventRecord &event,
      ID3D12CommandQueue *&queue,
      ID3D12Fence *&fence,
      std::uint64_t &value,
      std::string &error) const
  {
    queue = nullptr;
    fence = nullptr;
    value = 0;
    if (event.object_refs.size() < 2 || event.object_refs[0] == 0 || event.object_refs[1] == 0) {
      error = "queue fence operation requires refs {queue,fence}";
      return false;
    }
    const auto queue_it = replay_queues_by_object_.find(event.object_refs[0]);
    if (queue_it == replay_queues_by_object_.end() || !queue_it->second) {
      error = "queue fence operation references an unmaterialized queue object_id=" +
              std::to_string(event.object_refs[0]);
      return false;
    }
    const auto fence_it = app_fences_.find(event.object_refs[1]);
    if (fence_it == app_fences_.end() || !fence_it->second) {
      error = "queue fence operation references an unmaterialized app fence object_id=" +
              std::to_string(event.object_refs[1]);
      return false;
    }
    if (!event_ordered_payload_fence_value(event, value, error)) {
      return false;
    }
    queue = queue_it->second.get();
    fence = fence_it->second.get();
    return true;
  }

  bool event_ordered_fence_and_value(
      const trace::EventRecord &event,
      ID3D12Fence *&fence,
      std::uint64_t &value,
      std::string &error) const
  {
    fence = nullptr;
    value = 0;
    if (event.object_refs.empty() || event.object_refs[0] == 0) {
      error = "fence operation requires refs {fence}";
      return false;
    }
    const auto fence_it = app_fences_.find(event.object_refs[0]);
    if (fence_it == app_fences_.end() || !fence_it->second) {
      error = "fence operation references an unmaterialized app fence object_id=" +
              std::to_string(event.object_refs[0]);
      return false;
    }
    if (!event_ordered_payload_fence_value(event, value, error)) {
      return false;
    }
    fence = fence_it->second.get();
    return true;
  }

  void event_ordered_handle_execute_command_lists(const trace::EventRecord &event)
  {
    if (event.object_refs.empty() || event.object_refs[0] == 0) {
      event_ordered_submit_error(event, "ExecuteCommandLists missing queue object id");
      return;
    }
    const auto queue_it = replay_queues_by_object_.find(event.object_refs[0]);
    if (queue_it == replay_queues_by_object_.end() || !queue_it->second) {
      event_ordered_submit_error(
          event,
          "ExecuteCommandLists references an unmaterialized queue object_id=" +
              std::to_string(event.object_refs[0]));
      return;
    }

    json payload;
    std::string error;
    if (!payload_to_json(event, payload, error)) {
      event_ordered_submit_error(event, std::move(error));
      return;
    }
    const auto command_list_count =
        static_cast<std::size_t>(payload_u64(payload, "command_list_count", event.object_refs.size() - 1));
    if (command_list_count != event.object_refs.size() - 1) {
      event_ordered_submit_error(event, "ExecuteCommandLists object_refs do not match command_list_count");
      return;
    }
    if (command_list_count == 0) {
      return;
    }

    std::vector<ID3D12CommandList *> native_lists;
    native_lists.reserve(command_list_count);
    for (std::size_t index = 1; index < event.object_refs.size(); ++index) {
      const auto list_it = command_lists_.find(event.object_refs[index]);
      if (list_it == command_lists_.end() || !list_it->second.list) {
        event_ordered_submit_error(
            event,
            "ExecuteCommandLists references an unmaterialized command list object_id=" +
                std::to_string(event.object_refs[index]));
        return;
      }
      native_lists.push_back(list_it->second.list.get());
    }

    ID3D12CommandQueue *queue = queue_it->second.get();
    if (!sync_tile_mapping_updates_until(event.callsite.sequence, error, queue)) {
      event_ordered_submit_error(event, std::move(error));
      return;
    }
    queue->ExecuteCommandLists(static_cast<UINT>(native_lists.size()), native_lists.data());
    event_ordered_command_lists_submitted_ += native_lists.size();

    const std::string removed = device_removed_detail();
    if (!removed.empty()) {
      event_ordered_submit_error(event, removed);
    }
  }

  void event_ordered_handle_queue_signal(const trace::EventRecord &event)
  {
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    std::uint64_t value = 0;
    std::string error;
    if (!event_ordered_queue_and_fence(event, queue, fence, value, error)) {
      event_ordered_submit_error(event, std::move(error));
      return;
    }
    const HRESULT hr = queue->Signal(fence, value);
    if (FAILED(hr)) {
      event_ordered_submit_error(event, hresult_error("ID3D12CommandQueue::Signal", hr));
      return;
    }
    ++event_ordered_queue_signals_;
  }

  void event_ordered_handle_queue_wait(const trace::EventRecord &event)
  {
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    std::uint64_t value = 0;
    std::string error;
    if (!event_ordered_queue_and_fence(event, queue, fence, value, error)) {
      event_ordered_submit_error(event, std::move(error));
      return;
    }
    const HRESULT hr = queue->Wait(fence, value);
    if (FAILED(hr)) {
      event_ordered_submit_error(event, hresult_error("ID3D12CommandQueue::Wait", hr));
      return;
    }
    ++event_ordered_queue_waits_;
  }

  void event_ordered_handle_fence_signal(const trace::EventRecord &event)
  {
    ID3D12Fence *fence = nullptr;
    std::uint64_t value = 0;
    std::string error;
    if (!event_ordered_fence_and_value(event, fence, value, error)) {
      event_ordered_submit_error(event, std::move(error));
      return;
    }
    const HRESULT hr = fence->Signal(value);
    if (FAILED(hr)) {
      event_ordered_submit_error(event, hresult_error("ID3D12Fence::Signal", hr));
      return;
    }
    ++event_ordered_fence_signals_;
  }

  void event_ordered_handle_fence_set_event_on_completion(const trace::EventRecord &event)
  {
    ID3D12Fence *fence = nullptr;
    std::uint64_t value = 0;
    std::string error;
    if (!event_ordered_fence_and_value(event, fence, value, error)) {
      event_ordered_submit_error(event, std::move(error));
      return;
    }

    ScopedFenceEvent event_handle;
    if (!event_handle.valid()) {
      event_ordered_submit_error(event, "SetEventOnCompletion failed to create host wait event");
      return;
    }

    constexpr std::uint32_t kCpuWaitTimeoutMs = 5000;
#ifdef _WIN32
    const HRESULT hr = fence->SetEventOnCompletion(value, event_handle.get());
    if (FAILED(hr)) {
      event_ordered_submit_error(event, hresult_error("ID3D12Fence::SetEventOnCompletion", hr));
      return;
    }
#endif
    ++event_ordered_cpu_waits_;

#ifdef _WIN32
    const auto wait_result = event_handle.wait(kCpuWaitTimeoutMs);
#else
    const auto wait_result = event_handle.wait_until(fence, value, kCpuWaitTimeoutMs);
#endif
    if (wait_result == ScopedFenceEvent::WaitResult::Timeout) {
      ++event_ordered_cpu_wait_timeouts_;
      const std::string removed = device_removed_detail();
      std::ostringstream line;
      line << "[event-ordered] wait:timeout seq=" << event.callsite.sequence
           << " fence=" << event.object_refs[0]
           << " value=" << value
           << " completed=" << fence->GetCompletedValue()
           << " device_removed=" << (removed.empty() ? "none" : removed);
      event_ordered_log(line.str());
      return;
    }
    if (wait_result == ScopedFenceEvent::WaitResult::Failed) {
      event_ordered_submit_error(event, "SetEventOnCompletion host wait failed");
      return;
    }

    const std::string removed = device_removed_detail();
    if (!removed.empty()) {
      event_ordered_submit_error(event, removed);
    }
  }

  bool event_ordered_drain_queue(
      trace::ObjectId queue_object_id,
      ID3D12CommandQueue *queue,
      std::string &error)
  {
    if (!device_ || !queue) {
      return true;
    }

    ComPtr<ID3D12Fence> done;
    HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(done.put()));
    if (FAILED(hr) || !done) {
      return fail(error, hresult_error("CreateFence(event-ordered drain)", hr));
    }

    const UINT64 value = 1;
    hr = queue->Signal(done.get(), value);
    if (FAILED(hr)) {
      return fail(error, hresult_error("ID3D12CommandQueue::Signal(event-ordered drain)", hr));
    }

    ScopedFenceEvent event_handle;
    if (!event_handle.valid()) {
      return fail(error, "event-ordered final drain failed to create host wait event");
    }
#ifdef _WIN32
    hr = done->SetEventOnCompletion(value, event_handle.get());
    if (FAILED(hr)) {
      return fail(error, hresult_error("ID3D12Fence::SetEventOnCompletion(event-ordered drain)", hr));
    }
#endif
#ifdef _WIN32
    const auto wait_result = event_handle.wait(5000);
#else
    const auto wait_result = event_handle.wait_until(done.get(), value, 5000);
#endif
    if (wait_result == ScopedFenceEvent::WaitResult::Timeout) {
      std::ostringstream line;
      line << "[event-ordered] final-drain:timeout queue=" << queue_object_id
           << " value=" << value
           << " completed=" << done->GetCompletedValue()
           << " device_removed=" << device_removed_detail();
      event_ordered_log(line.str());
      return true;
    }
    if (wait_result == ScopedFenceEvent::WaitResult::Failed) {
      return fail(error, "event-ordered final drain host wait failed");
    }

    const std::string removed = device_removed_detail();
    if (!removed.empty()) {
      return fail(error, "event-ordered final drain queue=" +
                         std::to_string(queue_object_id) + ": " + removed);
    }
    return true;
  }

  bool event_ordered_final_drain_queues(std::string &error)
  {
    for (const auto &[queue_object_id, queue] : replay_queues_by_object_) {
      if (!event_ordered_drain_queue(queue_object_id, queue.get(), error)) {
        return false;
      }
    }
    return true;
  }
