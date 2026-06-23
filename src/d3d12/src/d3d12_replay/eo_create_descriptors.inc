  void materialize_create_descriptors(const trace::EventRecord &event)
  {
    const std::string_view function_name(event.callsite.function_name);
    if (event_ordered_contains(function_name, "CreateDescriptorHeap")) {
      materialize_create_descriptor_heap(event);
      return;
    }
    if (function_name == "ID3D12Device::CreateDescriptorViewBatch") {
      materialize_create_descriptor_view_batch(event);
      return;
    }
    if (function_name == "ID3D12Device::CreateConstantBufferView" ||
        function_name == "ID3D12Device::CreateShaderResourceView" ||
        function_name == "ID3D12Device::CreateUnorderedAccessView" ||
        function_name == "ID3D12Device::CreateRenderTargetView" ||
        function_name == "ID3D12Device::CreateDepthStencilView") {
      materialize_create_descriptor_view(event);
      return;
    }
    if (function_name == "ID3D12Device::CreateSampler") {
      materialize_create_sampler(event);
      return;
    }

    event_ordered_log_stub_once(event_ordered_category(event), event);
  }

  void materialize_create_descriptor_heap(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(event, "CreateDescriptorHeap requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_object_id(event, "CreateDescriptorHeap", object_id)) {
      return;
    }

    const auto heap_it = backend_.descriptor_heaps_.find(object_id);
    if (heap_it == backend_.descriptor_heaps_.end() ||
        heap_it->second.create_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreateDescriptorHeap missing DescriptorHeapSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    const auto &heap = heap_it->second;
    NativeDescriptorHeap native;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heap.type);
    desc.NumDescriptors = heap.num_descriptors;
    desc.Flags = static_cast<D3D12_DESCRIPTOR_HEAP_FLAGS>(heap.flags);
    desc.NodeMask = heap.node_mask;
    const HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(native.heap.put()));
    if (FAILED(hr)) {
      event_ordered_record_materialize_error(event, hresult_error("CreateDescriptorHeap", hr));
      return;
    }

    native.type = desc.Type;
    native.descriptor_size = device_->GetDescriptorHandleIncrementSize(desc.Type);
    native.cpu_start = native.heap->GetCPUDescriptorHandleForHeapStart();
    if ((desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
      native.gpu_start = native.heap->GetGPUDescriptorHandleForHeapStart();
    }
    descriptor_heaps_[object_id] = std::move(native);
  }

  static std::string_view event_ordered_descriptor_kind_for_function(std::string_view function_name)
  {
    if (function_name == "ID3D12Device::CreateConstantBufferView") {
      return "ConstantBufferView";
    }
    if (function_name == "ID3D12Device::CreateShaderResourceView") {
      return "ShaderResourceView";
    }
    if (function_name == "ID3D12Device::CreateUnorderedAccessView") {
      return "UnorderedAccessView";
    }
    if (function_name == "ID3D12Device::CreateRenderTargetView") {
      return "RenderTargetView";
    }
    if (function_name == "ID3D12Device::CreateDepthStencilView") {
      return "DepthStencilView";
    }
    return {};
  }

  static std::string_view event_ordered_descriptor_kind_for_batch_kind(std::uint32_t kind)
  {
    switch (kind) {
    case 1:
      return "ConstantBufferView";
    case 2:
      return "ShaderResourceView";
    case 3:
      return "UnorderedAccessView";
    case 4:
      return "RenderTargetView";
    case 5:
      return "DepthStencilView";
    default:
      return {};
    }
  }

  void event_ordered_refresh_descriptor_state()
  {
    descriptor_timeline_.clear();
    descriptor_timeline_.reserve(backend_.descriptors_.size());
    for (const auto &descriptor : backend_.descriptors_) {
      descriptor_timeline_.push_back(&descriptor);
    }
    std::stable_sort(
        descriptor_timeline_.begin(),
        descriptor_timeline_.end(),
        [](const auto *lhs, const auto *rhs) {
          return lhs->create_sequence < rhs->create_sequence;
        });

    descriptor_slot_history_.clear();
    for (const auto *descriptor : descriptor_timeline_) {
      descriptor_slot_history_[descriptor->descriptor].push_back(
          DescriptorSlotHistoryEntry{descriptor->create_sequence, descriptor});
    }
    next_descriptor_index_ = descriptor_timeline_.size();
  }

  void event_ordered_refresh_sampler_state()
  {
    sampler_timeline_.clear();
    sampler_timeline_.reserve(backend_.samplers_.size());
    for (const auto &sampler : backend_.samplers_) {
      sampler_timeline_.push_back(&sampler);
    }
    std::stable_sort(
        sampler_timeline_.begin(),
        sampler_timeline_.end(),
        [](const auto *lhs, const auto *rhs) {
          return lhs->create_sequence < rhs->create_sequence;
        });
    next_sampler_index_ = sampler_timeline_.size();
  }

  const D3D12ReplayBackend::DescriptorSemanticState *event_ordered_find_descriptor_semantic(
      std::uint64_t sequence,
      std::uint64_t descriptor,
      std::string_view kind) const
  {
    for (auto it = backend_.descriptors_.rbegin(); it != backend_.descriptors_.rend(); ++it) {
      if (it->create_sequence == sequence &&
          it->descriptor == descriptor &&
          (kind.empty() || it->kind == kind)) {
        return &*it;
      }
    }
    return nullptr;
  }

  bool event_ordered_apply_descriptor_semantic(
      const trace::EventRecord &event,
      const D3D12ReplayBackend::DescriptorSemanticState &semantic)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE dst = relocated_cpu_descriptor(semantic.binding);
    if (dst.ptr == 0) {
      event_ordered_record_materialize_error(
          event,
          "descriptor destination cannot be relocated heap=" +
              std::to_string(semantic.binding.heap_object_id) +
              " index=" + std::to_string(semantic.binding.descriptor_index));
      return false;
    }

    std::string error;
    auto saved_resource_version_natives = std::move(resource_version_natives_);
    const bool ok = create_descriptor_view(semantic, dst, true, error);
    resource_version_natives_ = std::move(saved_resource_version_natives);
    if (!ok) {
      event_ordered_record_materialize_error(event, std::move(error));
      return false;
    }
    return true;
  }

  void materialize_create_descriptor_view(const trace::EventRecord &event)
  {
    const auto kind = event_ordered_descriptor_kind_for_function(event.callsite.function_name);
    if (kind.empty()) {
      event_ordered_log_stub_once(event_ordered_category(event), event);
      return;
    }

    json payload;
    std::string error;
    if (!payload_to_json(event, payload, error)) {
      event_ordered_record_materialize_error(event, std::move(error));
      return;
    }

    const auto descriptor = payload_u64(payload, "descriptor");
    const auto *semantic =
        event_ordered_find_descriptor_semantic(event.callsite.sequence, descriptor, kind);
    if (semantic == nullptr) {
      event_ordered_record_materialize_error(
          event,
          "descriptor view missing DescriptorSemanticState descriptor=" +
              std::to_string(descriptor) +
              " kind=" + std::string(kind));
      return;
    }

    if (event_ordered_apply_descriptor_semantic(event, *semantic)) {
      event_ordered_refresh_descriptor_state();
    }
  }

  void materialize_create_descriptor_view_batch(const trace::EventRecord &event)
  {
    json payload;
    std::string error;
    if (!payload_to_json(event, payload, error)) {
      event_ordered_record_materialize_error(event, std::move(error));
      return;
    }
    if (payload_string(payload, "schema") != "descriptor-view-v2") {
      event_ordered_record_materialize_error(
          event,
          "CreateDescriptorViewBatch only materializes descriptor-view-v2 ops");
      return;
    }

    const auto ops = payload.find("ops");
    if (ops == payload.end() || !ops->is_array()) {
      event_ordered_record_materialize_error(event, "descriptor batch ops must be an array");
      return;
    }

    bool materialized_any = false;
    for (const auto &op : *ops) {
      if (!op.is_array() || op.size() < 16) {
        event_ordered_record_materialize_error(event, "descriptor batch v2 op must be a 16-column array");
        return;
      }
      const auto sequence = json_u64(op[0], event.callsite.sequence);
      const auto kind =
          event_ordered_descriptor_kind_for_batch_kind(static_cast<std::uint32_t>(json_u64(op[1])));
      if (kind.empty()) {
        event_ordered_record_materialize_error(
            event,
            "descriptor batch op has unsupported kind=" + std::to_string(json_u64(op[1])));
        continue;
      }
      const auto descriptor = json_u64(op[2]);
      const auto *semantic = event_ordered_find_descriptor_semantic(sequence, descriptor, kind);
      if (semantic == nullptr) {
        event_ordered_record_materialize_error(
            event,
            "descriptor batch missing DescriptorSemanticState descriptor=" +
                std::to_string(descriptor) +
                " sequence=" + std::to_string(sequence) +
                " kind=" + std::string(kind));
        continue;
      }
      if (event_ordered_apply_descriptor_semantic(event, *semantic)) {
        materialized_any = true;
      }
    }

    if (materialized_any) {
      event_ordered_refresh_descriptor_state();
    }
  }

  const D3D12ReplayBackend::SamplerSemanticState *event_ordered_find_sampler_semantic(
      std::uint64_t sequence,
      std::uint64_t descriptor) const
  {
    for (auto it = backend_.samplers_.rbegin(); it != backend_.samplers_.rend(); ++it) {
      if (it->create_sequence == sequence && it->descriptor == descriptor) {
        return &*it;
      }
    }
    return nullptr;
  }

  void materialize_create_sampler(const trace::EventRecord &event)
  {
    json payload;
    std::string error;
    if (!payload_to_json(event, payload, error)) {
      event_ordered_record_materialize_error(event, std::move(error));
      return;
    }

    const auto descriptor = payload_u64(payload, "descriptor");
    const auto *semantic = event_ordered_find_sampler_semantic(event.callsite.sequence, descriptor);
    if (semantic == nullptr) {
      event_ordered_record_materialize_error(
          event,
          "CreateSampler missing SamplerSemanticState descriptor=" + std::to_string(descriptor));
      return;
    }

    if (!apply_sampler(*semantic, error)) {
      event_ordered_record_materialize_error(event, std::move(error));
      return;
    }
    event_ordered_refresh_sampler_state();
  }
