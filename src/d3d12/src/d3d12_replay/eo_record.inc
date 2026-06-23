  void event_ordered_reset_record_sidecar(
      NativeCommandList &native,
      trace::ObjectId initial_pipeline_state_object_id)
  {
    native.recorded_until_sequence = 0;
    native.pending_render_pass_resolves.clear();
    native.current_render_targets = {};
    native.current_render_target_count = 0;
    native.current_pipeline_state_object_id = initial_pipeline_state_object_id;
    native.current_graphics_root_signature_object_id = 0;
    native.current_graphics_root_tables = {};
    native.current_graphics_root_descriptors = {};
    native.current_viewport = {};
    native.has_current_viewport = false;
    native.current_scissor = {};
    native.has_current_scissor = false;
    native.current_primitive_topology = 0;
    native.current_vertex_buffer0 = {};
    native.current_index_buffer = {};
    native.target_watch_readbacks.clear();
    native.target_watch_occlusions.clear();
    native.target_watch_buffer_readbacks.clear();
  }

  void event_ordered_record_error(const trace::EventRecord &event, std::string error)
  {
    const std::string line = "[event-ordered] record:error seq=" +
                             std::to_string(event.callsite.sequence) +
                             " fn=" + event_ordered_event_name(event) +
                             " " + error;
    backend_.last_error_ = std::move(error);
    event_ordered_log(line);
  }

  bool event_ordered_initial_pipeline_from_record_event(
      const trace::EventRecord &event,
      const json &payload,
      trace::ObjectId &pipeline_object_id,
      ID3D12PipelineState *&pipeline,
      std::string &error)
  {
    pipeline_object_id = 0;
    pipeline = nullptr;
    if (event.object_refs.size() >= 3) {
      pipeline_object_id = event.object_refs[2];
    }
    if (pipeline_object_id == 0) {
      pipeline_object_id = static_cast<trace::ObjectId>(
          payload_u64(payload, "initial_pipeline_state_object_id"));
    }
    if (pipeline_object_id == 0) {
      return true;
    }
    pipeline = pipeline_for(pipeline_object_id, event.callsite.sequence, error);
    if (!pipeline && error.empty()) {
      error = "Reset references an unmaterialized initial pipeline state object_id=" +
              std::to_string(pipeline_object_id);
    }
    return pipeline != nullptr;
  }

  void event_ordered_handle_command_list_reset(const trace::EventRecord &event)
  {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      event_ordered_record_error(
          event,
          "Reset missing command allocator object id");
      return;
    }

    const auto list_it = command_lists_.find(event.object_refs.front());
    if (list_it == command_lists_.end() || !list_it->second.list) {
      event_ordered_record_error(
          event,
          "Reset references an unmaterialized command list object_id=" +
              std::to_string(event.object_refs.empty() ? 0 : event.object_refs.front()));
      return;
    }

    const auto allocator_it = command_allocators_.find(event.object_refs[1]);
    if (allocator_it == command_allocators_.end() || !allocator_it->second) {
      event_ordered_record_error(
          event,
          "Reset references an unmaterialized command allocator object_id=" +
              std::to_string(event.object_refs[1]));
      return;
    }

    json payload;
    std::string error;
    if (!payload_to_json(event, payload, error)) {
      event_ordered_record_error(event, std::move(error));
      return;
    }

    trace::ObjectId initial_pipeline_state_object_id = 0;
    ID3D12PipelineState *initial_pso = nullptr;
    if (!event_ordered_initial_pipeline_from_record_event(
            event,
            payload,
            initial_pipeline_state_object_id,
            initial_pso,
            error)) {
      event_ordered_record_error(event, std::move(error));
      return;
    }

    NativeCommandList &native = list_it->second;
    const HRESULT hr = native.list->Reset(allocator_it->second.get(), initial_pso);
    if (FAILED(hr)) {
      event_ordered_record_error(
          event,
          hresult_error("ID3D12GraphicsCommandList::Reset", hr));
      return;
    }

    event_ordered_hold_allocator_ref(native, allocator_it->second.get());
    event_ordered_reset_record_sidecar(native, initial_pipeline_state_object_id);
    ++event_ordered_lists_reset_;
  }

  void event_ordered_handle_command_list_close(const trace::EventRecord &event)
  {
    const auto list_it = command_lists_.find(event.object_refs.empty() ? 0 : event.object_refs.front());
    if (list_it == command_lists_.end() || !list_it->second.list) {
      event_ordered_record_error(
          event,
          "Close references an unmaterialized command list object_id=" +
              std::to_string(event.object_refs.empty() ? 0 : event.object_refs.front()));
      return;
    }

    NativeCommandList &native = list_it->second;
    const HRESULT hr = native.list->Close();
    if (FAILED(hr)) {
      event_ordered_record_error(
          event,
          hresult_error("ID3D12GraphicsCommandList::Close", hr));
      return;
    }
    native.recorded_until_sequence = event.callsite.sequence;
    ++event_ordered_lists_closed_;
  }

  void event_ordered_handle_command_allocator_reset(const trace::EventRecord &event)
  {
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      event_ordered_record_error(event, "CommandAllocator::Reset missing allocator object id");
      return;
    }

    const auto allocator_it = command_allocators_.find(event.object_refs.front());
    if (allocator_it == command_allocators_.end() || !allocator_it->second) {
      event_ordered_record_error(
          event,
          "CommandAllocator::Reset references an unmaterialized allocator object_id=" +
              std::to_string(event.object_refs.front()));
      return;
    }

    const HRESULT hr = allocator_it->second->Reset();
    if (FAILED(hr)) {
      event_ordered_record_error(
          event,
          hresult_error("ID3D12CommandAllocator::Reset", hr));
      return;
    }
  }

  void event_ordered_record_last_command(const trace::EventRecord &event)
  {
    if (backend_.replay_commands_.empty()) {
      event_ordered_record_error(event, "record event did not append a replay command");
      return;
    }

    const auto &record = backend_.replay_commands_.back();
    if (record.sequence != event.callsite.sequence ||
        record.function_name != event.callsite.function_name ||
        record.kind == D3D12ReplayBackend::ReplayCommandKind::Unknown ||
        record.kind == D3D12ReplayBackend::ReplayCommandKind::BeginCommandList ||
        record.kind == D3D12ReplayBackend::ReplayCommandKind::EndCommandList) {
      event_ordered_record_error(event, "record event did not match the latest replay command");
      return;
    }

    auto native_it = command_lists_.find(record.command_list_object_id);
    if (native_it == command_lists_.end() || !native_it->second.list) {
      event_ordered_record_error(
          event,
          "record command references an unmaterialized command list object_id=" +
              std::to_string(record.command_list_object_id));
      return;
    }

    std::string error;
    const json *payload = payload_to_json_cached_ref(event, error);
    if (!payload) {
      event_ordered_record_error(event, std::move(error));
      return;
    }

    struct PayloadOverrideScope {
      const json *&slot;
      const json *saved;

      PayloadOverrideScope(const json *&override_slot, const json *payload_override)
          : slot(override_slot), saved(override_slot)
      {
        slot = payload_override;
      }

      ~PayloadOverrideScope()
      {
        slot = saved;
      }
    } payload_override_scope(record_one_payload_override_, payload);

    record_one(native_it->second, record);
    if (!record_one_ok_) {
      event_ordered_record_error(event, record_one_error_);
      return;
    }
    ++event_ordered_recorded_commands_;
    if (record.kind == D3D12ReplayBackend::ReplayCommandKind::Draw) {
      ++event_ordered_draws_recorded_;
    } else if (record.kind == D3D12ReplayBackend::ReplayCommandKind::Dispatch) {
      ++event_ordered_dispatches_recorded_;
    }

    if (!update_target_texture_watch_render_targets(native_it->second, record, *payload, error)) {
      event_ordered_record_error(event, std::move(error));
      return;
    }
    update_target_texture_watch_state(native_it->second, record, *payload);
  }

  void handle_record_event(const trace::EventRecord &event)
  {
    const std::string_view function_name(event.callsite.function_name);
    if (function_name == "ID3D12GraphicsCommandList::Reset") {
      event_ordered_handle_command_list_reset(event);
      return;
    }
    if (function_name == "ID3D12GraphicsCommandList::Close") {
      event_ordered_handle_command_list_close(event);
      return;
    }
    if (function_name == "ID3D12Device::CreateCommandList" ||
        function_name == "ID3D12Device::CreateCommandList1") {
      return;
    }
    if (function_name == "ID3D12CommandAllocator::Reset") {
      event_ordered_handle_command_allocator_reset(event);
      return;
    }

    event_ordered_record_last_command(event);
  }
