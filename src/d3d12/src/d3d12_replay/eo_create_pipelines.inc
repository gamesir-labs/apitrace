  void materialize_create_pipelines(const trace::EventRecord &event)
  {
    if (event.callsite.result_code < 0) {
      return;
    }

    const std::string_view function_name(event.callsite.function_name);
    if (event_ordered_contains(function_name, "CreateRootSignature")) {
      materialize_create_root_signature(event);
      return;
    }
    if (function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
        function_name == "ID3D12Device::CreateComputePipelineState") {
      materialize_create_pipeline_state(event);
      return;
    }
    if (event_ordered_contains(function_name, "CreateCommandSignature")) {
      materialize_create_command_signature(event);
      return;
    }

    event_ordered_log_stub_once(event_ordered_category(event), event);
  }

  void materialize_create_root_signature(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(
          event,
          "CreateRootSignature requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_object_id(event, "CreateRootSignature", object_id)) {
      return;
    }

    const auto root_signature_it = backend_.root_signatures_.find(object_id);
    if (root_signature_it == backend_.root_signatures_.end() ||
        root_signature_it->second.create_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreateRootSignature missing RootSignatureSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    const auto &root_signature = root_signature_it->second;
    if (root_signature.bytes.empty()) {
      event_ordered_record_materialize_error(
          event,
          "CreateRootSignature RootSignatureSemanticState is missing bytecode object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(root_signature.create_sequence));
      return;
    }

    ComPtr<ID3D12RootSignature> native;
    const HRESULT hr = device_->CreateRootSignature(
        root_signature.node_mask,
        root_signature.bytes.data(),
        root_signature.bytes.size(),
        IID_PPV_ARGS(native.put()));
    if (FAILED(hr)) {
      std::ostringstream detail;
      detail << "CreateRootSignature(object_id=" << object_id
             << " create_seq=" << root_signature.create_sequence
             << " node_mask=" << root_signature.node_mask
             << " bytecode_size=" << root_signature.bytecode_size
             << " bytes=" << root_signature.bytes.size()
             << " path=" << root_signature.relative_path.generic_string()
             << ")";
      event_ordered_record_materialize_error(event, hresult_error(detail.str(), hr));
      return;
    }

    root_signatures_[object_id] = std::move(native);
  }

  bool event_ordered_created_pipeline_state_object_id(
      const trace::EventRecord &event,
      trace::ObjectId &object_id)
  {
    if (event.object_refs.size() < 2 || event.object_refs.back() == 0) {
      event_ordered_record_materialize_error(
          event,
          "CreatePipelineState missing output pipeline state object id");
      return false;
    }
    object_id = event.object_refs.back();
    return true;
  }

  void materialize_create_pipeline_state(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(
          event,
          "CreatePipelineState requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_pipeline_state_object_id(event, object_id)) {
      return;
    }

    const auto pipeline_it = backend_.pipelines_.find(object_id);
    if (pipeline_it == backend_.pipelines_.end() ||
        pipeline_it->second.create_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreatePipelineState missing PipelineSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    ComPtr<ID3D12PipelineState> native;
    std::string error;
    if (!create_native_pipeline(pipeline_it->second, native, error)) {
      event_ordered_record_materialize_error(event, std::move(error));
      return;
    }
    if (!native) {
      event_ordered_record_materialize_error(
          event,
          "CreatePipelineState returned a null native pipeline object_id=" +
              std::to_string(object_id));
      return;
    }

    pipelines_[object_id] = std::move(native);
  }

  bool event_ordered_created_command_signature_object_id(
      const trace::EventRecord &event,
      trace::ObjectId &object_id)
  {
    if (event.object_refs.size() < 2 || event.object_refs.back() == 0) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandSignature missing output command signature object id");
      return false;
    }
    object_id = event.object_refs.back();
    return true;
  }

  void materialize_create_command_signature(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandSignature requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_command_signature_object_id(event, object_id)) {
      return;
    }

    const auto signature_it = backend_.command_signatures_.find(object_id);
    if (signature_it == backend_.command_signatures_.end() ||
        signature_it->second.create_sequence != event.callsite.sequence) {
      event_ordered_record_materialize_error(
          event,
          "CreateCommandSignature missing CommandSignatureSemanticState object_id=" +
              std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    const auto &signature = signature_it->second;
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
    arguments.reserve(signature.arguments.size());
    for (const auto &argument : signature.arguments) {
      D3D12_INDIRECT_ARGUMENT_DESC desc{};
      desc.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(argument.type);
      switch (desc.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
        desc.VertexBuffer.Slot = argument.slot;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
        desc.Constant.RootParameterIndex = argument.root_parameter_index;
        desc.Constant.DestOffsetIn32BitValues = argument.dest_offset_in32bit_values;
        desc.Constant.Num32BitValuesToSet = argument.num32bit_values_to_set;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
        desc.ConstantBufferView.RootParameterIndex = argument.root_parameter_index;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
        desc.ShaderResourceView.RootParameterIndex = argument.root_parameter_index;
        break;
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        desc.UnorderedAccessView.RootParameterIndex = argument.root_parameter_index;
        break;
      default:
        break;
      }
      arguments.push_back(desc);
    }

    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = signature.byte_stride;
    desc.NumArgumentDescs = static_cast<UINT>(arguments.size());
    desc.pArgumentDescs = arguments.data();
    desc.NodeMask = signature.node_mask;
    ID3D12RootSignature *root_signature = nullptr;
    if (signature.root_signature_object_id != 0) {
      const auto root_it = root_signatures_.find(signature.root_signature_object_id);
      if (root_it == root_signatures_.end()) {
        event_ordered_record_materialize_error(
            event,
            "CreateCommandSignature references an unknown root signature object_id=" +
                std::to_string(signature.root_signature_object_id));
        return;
      }
      root_signature = root_it->second.get();
    }

    ComPtr<ID3D12CommandSignature> native;
    const HRESULT hr = device_->CreateCommandSignature(
        &desc,
        root_signature,
        IID_PPV_ARGS(native.put()));
    if (FAILED(hr)) {
      event_ordered_record_materialize_error(event, hresult_error("CreateCommandSignature", hr));
      return;
    }

    command_signatures_[object_id] = std::move(native);
  }
