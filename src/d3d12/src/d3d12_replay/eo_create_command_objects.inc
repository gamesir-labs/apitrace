  void materialize_create_command_objects(const trace::EventRecord &event)
  {
    if (event.callsite.result_code < 0) {
      return;
    }

    const std::string_view function_name(event.callsite.function_name);
    if (event_ordered_contains(function_name, "CreateCommandQueue")) {
      materialize_create_command_queue(event);
      return;
    }
    if (event_ordered_contains(function_name, "CreateCommandAllocator")) {
      materialize_create_command_allocator(event);
      return;
    }
    if (function_name == "ID3D12Device::CreateCommandList") {
      materialize_create_command_list(event);
      return;
    }
    if (function_name == "ID3D12Device::CreateCommandList1") {
      materialize_create_command_list1(event);
      return;
    }
    if (event_ordered_contains(function_name, "CreateFence")) {
      materialize_create_fence(event);
      return;
    }

    event_ordered_log_stub_once(event_ordered_category(event), event);
  }

  static bool event_ordered_supported_command_list_type(D3D12_COMMAND_LIST_TYPE type)
  {
    switch (type) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
    case D3D12_COMMAND_LIST_TYPE_BUNDLE:
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
    case D3D12_COMMAND_LIST_TYPE_COPY:
      return true;
    default:
      return false;
    }
  }

  bool event_ordered_created_command_list_object_id(
      const trace::EventRecord &event,
      const char *label,
      trace::ObjectId &object_id)
  {
    if (event.object_refs.size() < 2 || event.object_refs.back() == 0) {
      event_ordered_record_materialize_error(
          event,
          std::string(label) + " missing output command list object id");
      return false;
    }
    object_id = event.object_refs.back();
    return true;
  }

  void event_ordered_query_command_list_interfaces(NativeCommandList &native)
  {
    if (!native.list) {
      return;
    }
    if (FAILED(native.list->QueryInterface(IID_PPV_ARGS(native.list1.put())))) {
      native.list1.reset();
    }
    if (FAILED(native.list->QueryInterface(IID_PPV_ARGS(native.list2.put())))) {
      native.list2.reset();
    }
#if defined(__ID3D12GraphicsCommandList6_INTERFACE_DEFINED__)
    if (FAILED(native.list->QueryInterface(IID_PPV_ARGS(native.list6.put())))) {
      native.list6.reset();
    }
#endif
  }

  void event_ordered_hold_allocator_ref(
      NativeCommandList &native,
      ID3D12CommandAllocator *allocator)
  {
    if (!allocator) {
      return;
    }
    allocator->AddRef();
    native.allocator.reset(allocator);
  }

  HRESULT event_ordered_create_command_list1(
      UINT node_mask,
      D3D12_COMMAND_LIST_TYPE type,
      UINT flags,
      REFIID riid,
      void **command_list)
  {
    if (!device_) {
      return E_FAIL;
    }

    const GUID iid_device4 = {
        0xe865df17,
        0xa9ee,
        0x46f9,
        {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}};
    void *device4 = nullptr;
    HRESULT hr = device_->QueryInterface(iid_device4, &device4);
    if (FAILED(hr) || device4 == nullptr) {
      return hr;
    }

    // The vendored native-retrace d3d12.h currently stops before ID3D12Device4.
    // Use the stable D3D12 vtable slot for ID3D12Device4::CreateCommandList1.
    auto *unknown = reinterpret_cast<IUnknown *>(device4);
    void **vtable = *reinterpret_cast<void ***>(device4);
    using CreateCommandList1Fn = HRESULT(STDMETHODCALLTYPE *)(
        void *,
        UINT,
        D3D12_COMMAND_LIST_TYPE,
        UINT,
        REFIID,
        void **);

    constexpr std::size_t kCreateCommandList1VtableIndex = 51;
    if (vtable == nullptr || vtable[kCreateCommandList1VtableIndex] == nullptr) {
      unknown->Release();
      return E_FAIL;
    }

    auto create_command_list1 =
        reinterpret_cast<CreateCommandList1Fn>(vtable[kCreateCommandList1VtableIndex]);
    hr = create_command_list1(device4, node_mask, type, flags, riid, command_list);
    unknown->Release();
    return hr;
  }

  void materialize_create_command_queue(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandQueue requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_object_id(event, "CreateCommandQueue", object_id)) {
      return;
    }

    const auto queue_it = backend_.command_queues_.find(object_id);
    if (queue_it == backend_.command_queues_.end() ||
        queue_it->second.create_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandQueue missing CommandQueueSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    const auto &queue = queue_it->second;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = static_cast<D3D12_COMMAND_LIST_TYPE>(queue.type);
    desc.Priority = queue.priority;
    desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(queue.flags);
    desc.NodeMask = queue.node_mask;

    ComPtr<ID3D12CommandQueue> native;
    const HRESULT hr = device_->CreateCommandQueue(&desc, IID_PPV_ARGS(native.put()));
    if (FAILED(hr)) {
      event_ordered_record_materialize_error(event, hresult_error("CreateCommandQueue", hr));
      return;
    }
    if (!native) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandQueue returned a null native queue object_id=" +
              std::to_string(object_id));
      return;
    }

    replay_queues_by_object_[object_id] = std::move(native);
    // TODO(event-ordered P4): replay QueueSignal/QueueWait against captured
    // queue topology in replay_queues_by_object_.
  }

  void materialize_create_command_allocator(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandAllocator requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_object_id(event, "CreateCommandAllocator", object_id)) {
      return;
    }

    const auto allocator_it = backend_.command_allocators_.find(object_id);
    if (allocator_it == backend_.command_allocators_.end() ||
        allocator_it->second.create_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandAllocator missing CommandAllocatorSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    const auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(allocator_it->second.type);
    if (!event_ordered_supported_command_list_type(type)) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandAllocator unsupported command list type=" +
              std::to_string(allocator_it->second.type));
      return;
    }

    ComPtr<ID3D12CommandAllocator> native;
    const HRESULT hr = device_->CreateCommandAllocator(type, IID_PPV_ARGS(native.put()));
    if (FAILED(hr)) {
      event_ordered_record_materialize_error(event, hresult_error("CreateCommandAllocator", hr));
      return;
    }
    if (!native) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandAllocator returned a null native allocator object_id=" +
              std::to_string(object_id));
      return;
    }

    command_allocators_[object_id] = std::move(native);
    // TODO(event-ordered P2): replay ID3D12CommandAllocator::Reset at its
    // recorded event rather than during allocator materialization.
  }

  ID3D12PipelineState *event_ordered_initial_pipeline_for_command_list(
      const trace::EventRecord &event,
      const D3D12ReplayBackend::CommandListSemanticState &command_list)
  {
    if (command_list.pipeline_state_object_id == 0) {
      return nullptr;
    }
    const auto pipeline_it = pipelines_.find(command_list.pipeline_state_object_id);
    if (pipeline_it == pipelines_.end()) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList references an unmaterialized initial pipeline state object_id=" +
              std::to_string(command_list.pipeline_state_object_id));
      return nullptr;
    }
    return pipeline_it->second.get();
  }

  void materialize_create_command_list(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_command_list_object_id(event, "CreateCommandList", object_id)) {
      return;
    }

    const auto command_list_it = backend_.command_lists_.find(object_id);
    if (command_list_it == backend_.command_lists_.end() ||
        command_list_it->second.first_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList missing CommandListSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    const auto &command_list = command_list_it->second;
    const auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(command_list.type);
    if (!event_ordered_supported_command_list_type(type)) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList unsupported command list type=" +
              std::to_string(command_list.type));
      return;
    }

    const auto allocator_it = command_allocators_.find(command_list.allocator_object_id);
    if (allocator_it == command_allocators_.end() || !allocator_it->second) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList references an unmaterialized command allocator object_id=" +
              std::to_string(command_list.allocator_object_id));
      return;
    }

    ID3D12PipelineState *initial_pso =
        event_ordered_initial_pipeline_for_command_list(event, command_list);
    if (command_list.pipeline_state_object_id != 0 && initial_pso == nullptr) {
      return;
    }

    NativeCommandList native;
    native.type = type;
    event_ordered_hold_allocator_ref(native, allocator_it->second.get());

    const HRESULT hr = device_->CreateCommandList(
        command_list.node_mask,
        native.type,
        allocator_it->second.get(),
        initial_pso,
        IID_PPV_ARGS(native.list.put()));
    if (FAILED(hr)) {
      event_ordered_record_materialize_error(event, hresult_error("CreateCommandList", hr));
      return;
    }
    if (!native.list) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList returned a null native command list object_id=" +
              std::to_string(object_id));
      return;
    }

    event_ordered_query_command_list_interfaces(native);
    native.current_pipeline_state_object_id = command_list.pipeline_state_object_id;

    // TODO(event-ordered P2): replay Reset/record/Close from later events. A
    // CreateCommandList result is born open, so materialization intentionally
    // does not immediately Close it like the old batch path did.
    command_lists_[object_id] = std::move(native);
  }

  void materialize_create_command_list1(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList1 requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_command_list_object_id(event, "CreateCommandList1", object_id)) {
      return;
    }

    const auto command_list_it = backend_.command_lists_.find(object_id);
    if (command_list_it == backend_.command_lists_.end() ||
        command_list_it->second.first_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList1 missing CommandListSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    const auto &command_list = command_list_it->second;
    const auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(command_list.type);
    if (!event_ordered_supported_command_list_type(type)) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList1 unsupported command list type=" +
              std::to_string(command_list.type));
      return;
    }

    json payload;
    std::string error;
    if (!payload_to_json(event, payload, error)) {
      event_ordered_record_materialize_error(event, std::move(error));
      return;
    }

    NativeCommandList native;
    native.type = type;
    const HRESULT hr = event_ordered_create_command_list1(
        command_list.node_mask,
        native.type,
        payload_u32(payload, "flags"),
        IID_PPV_ARGS(native.list.put()));
    if (FAILED(hr)) {
      event_ordered_record_materialize_error(event, hresult_error("CreateCommandList1", hr));
      return;
    }
    if (!native.list) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandList1 returned a null native command list object_id=" +
              std::to_string(object_id));
      return;
    }

    event_ordered_query_command_list_interfaces(native);

    // TODO(event-ordered P2): replay later Reset/record/Close events. A
    // CreateCommandList1 result is born closed and has no recorded allocator at
    // creation time, so materialization only stores the native list.
    command_lists_[object_id] = std::move(native);
  }

  void materialize_create_fence(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(
          event,
          "CreateFence requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_object_id(event, "CreateFence", object_id)) {
      return;
    }

    const auto fence_it = backend_.fences_.find(object_id);
    if (fence_it == backend_.fences_.end() ||
        fence_it->second.create_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreateFence missing FenceSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    const auto &fence = fence_it->second;
    ComPtr<ID3D12Fence> native;
    const HRESULT hr = device_->CreateFence(
        fence.initial_value,
        static_cast<D3D12_FENCE_FLAGS>(fence.flags),
        IID_PPV_ARGS(native.put()));
    if (FAILED(hr)) {
      event_ordered_record_materialize_error(event, hresult_error("CreateFence", hr));
      return;
    }
    if (!native) {
      event_ordered_record_materialize_error(
          event,
          "CreateFence returned a null native fence object_id=" +
              std::to_string(object_id));
      return;
    }

    // TODO(event-ordered P4): replay QueueSignal/QueueWait/FenceSignal and
    // SetEventOnCompletion from fence operation events. Keep app fences
    // separate from the replayer's internal fence_.
    app_fences_[object_id] = std::move(native);
  }
