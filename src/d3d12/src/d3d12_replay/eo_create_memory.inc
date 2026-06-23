  void materialize_create_memory(const trace::EventRecord &event)
  {
    const std::string_view function_name(event.callsite.function_name);
    if (event_ordered_contains(function_name, "CreateHeap")) {
      materialize_create_heap(event);
      return;
    }
    if (event_ordered_contains(function_name, "CreateQueryHeap")) {
      materialize_create_query_heap(event);
      return;
    }

    event_ordered_log_stub_once(event_ordered_category(event), event);
  }

  void event_ordered_record_materialize_error(const trace::EventRecord &event, std::string error)
  {
    const std::string line = "[event-ordered] materialize:error seq=" +
                             std::to_string(event.callsite.sequence) +
                             " fn=" + event_ordered_event_name(event) +
                             " " + error;
    backend_.last_error_ = std::move(error);
    event_ordered_log(line);
  }

  bool event_ordered_created_object_id(
      const trace::EventRecord &event,
      const char *label,
      trace::ObjectId &object_id)
  {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      event_ordered_record_materialize_error(
          event,
          std::string(label) + " missing output object id");
      return false;
    }
    object_id = event.object_refs[1];
    return true;
  }

  void materialize_create_heap(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(event, "CreateHeap requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_object_id(event, "CreateHeap", object_id)) {
      return;
    }

    const auto heap_it = backend_.heaps_.find(object_id);
    if (heap_it == backend_.heaps_.end()) {
      event_ordered_record_materialize_error(
          event,
          "CreateHeap missing HeapSemanticState object_id=" + std::to_string(object_id));
      return;
    }

    const auto &heap = heap_it->second;
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = heap.size_in_bytes;
    desc.Properties = heap_properties(heap);
    desc.Alignment = heap.alignment;
    auto heap_flags = static_cast<D3D12_HEAP_FLAGS>(heap.flags);
    heap_flags = static_cast<D3D12_HEAP_FLAGS>(
        heap_flags & ~(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER));
    desc.Flags = heap_flags;

    static bool logged_captured_size_v1 = false;
    if (!logged_captured_size_v1) {
      event_ordered_log(
          "[event-ordered] CreateHeap v1 uses captured SizeInBytes; replay-device enlarge prescan is TODO");
      logged_captured_size_v1 = true;
    }

    NativeHeap native;
    // TODO(event-ordered): add the replay-device placed-resource allocation prescan used by
    // create_heaps; v1 intentionally keeps the captured heap SizeInBytes.
    native.replay_size_in_bytes = desc.SizeInBytes;
    HRESULT hr = device_->CreateHeap(&desc, IID_PPV_ARGS(native.heap.put()));
    if (FAILED(hr)) {
      std::ostringstream detail;
      detail << "CreateHeap(object_id=" << object_id
             << " size=" << desc.SizeInBytes
             << " alignment=" << desc.Alignment
             << " heap_type=" << static_cast<unsigned>(desc.Properties.Type)
             << " cpu_page=" << static_cast<unsigned>(desc.Properties.CPUPageProperty)
             << " mem_pool=" << static_cast<unsigned>(desc.Properties.MemoryPoolPreference)
             << " flags=0x" << std::hex << static_cast<unsigned>(desc.Flags) << std::dec
             << ")";
      event_ordered_record_materialize_error(event, hresult_error(detail.str(), hr));
      return;
    }

    heaps_[object_id] = std::move(native);
  }

  void materialize_create_query_heap(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(event, "CreateQueryHeap requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_object_id(event, "CreateQueryHeap", object_id)) {
      return;
    }

    const auto heap_it = backend_.query_heaps_.find(object_id);
    if (heap_it == backend_.query_heaps_.end()) {
      event_ordered_record_materialize_error(
          event,
          "CreateQueryHeap missing QueryHeapSemanticState object_id=" + std::to_string(object_id));
      return;
    }

    const auto &heap = heap_it->second;
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = static_cast<D3D12_QUERY_HEAP_TYPE>(heap.type);
    desc.Count = heap.count;
    desc.NodeMask = heap.node_mask;
    ComPtr<ID3D12QueryHeap> native;
    HRESULT hr = device_->CreateQueryHeap(&desc, IID_PPV_ARGS(native.put()));
    if (FAILED(hr)) {
      event_ordered_record_materialize_error(event, hresult_error("CreateQueryHeap", hr));
      return;
    }

    query_heaps_[object_id] = std::move(native);
  }
