void STDMETHODCALLTYPE hook_device_create_sampler(
    ID3D12Device *self,
    const D3D12_SAMPLER_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_sampler) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.create_sampler(self, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor);
  if (desc) {
    payload << ",\"filter\":" << static_cast<unsigned int>(desc->Filter)
            << ",\"address_u\":" << static_cast<unsigned int>(desc->AddressU)
            << ",\"address_v\":" << static_cast<unsigned int>(desc->AddressV)
            << ",\"address_w\":" << static_cast<unsigned int>(desc->AddressW)
            << ",\"mip_lod_bias\":" << desc->MipLODBias
            << ",\"max_anisotropy\":" << desc->MaxAnisotropy
            << ",\"comparison_func\":" << static_cast<unsigned int>(desc->ComparisonFunc)
            << ",\"border_color\":["
            << desc->BorderColor[0] << ","
            << desc->BorderColor[1] << ","
            << desc->BorderColor[2] << ","
            << desc->BorderColor[3] << "]"
            << ",\"min_lod\":" << desc->MinLOD
            << ",\"max_lod\":" << desc->MaxLOD;
  }
  payload << "}";
  record_call_locked("ID3D12Device::CreateSampler", S_OK, {self}, {}, payload.str());
}

std::string copy_descriptors_payload(
    ID3D12Device *device,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type,
    UINT dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
    const UINT *dst_descriptor_range_sizes,
    UINT src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
    const UINT *src_descriptor_range_sizes)
{
  const UINT descriptor_size = descriptor_handle_increment_size(device, descriptor_heap_type);
  std::ostringstream payload;
  payload << "{\"descriptor_heap_type\":" << static_cast<unsigned int>(descriptor_heap_type)
          << ",\"descriptor_size\":" << descriptor_size
          << ",\"dst_range_count\":" << dst_descriptor_range_count
          << ",\"src_range_count\":" << src_descriptor_range_count
          << ",\"descriptors\":[";
  bool first = true;
  UINT dst_range_index = 0;
  UINT src_range_index = 0;
  UINT dst_offset = 0;
  UINT src_offset = 0;
  while (dst_descriptor_range_starts &&
         src_descriptor_range_starts &&
         dst_range_index < dst_descriptor_range_count &&
         src_range_index < src_descriptor_range_count) {
    const UINT dst_range_size = dst_descriptor_range_sizes ? dst_descriptor_range_sizes[dst_range_index] : 1;
    const UINT src_range_size = src_descriptor_range_sizes ? src_descriptor_range_sizes[src_range_index] : 1;
    if (dst_offset >= dst_range_size) {
      ++dst_range_index;
      dst_offset = 0;
      continue;
    }
    if (src_offset >= src_range_size) {
      ++src_range_index;
      src_offset = 0;
      continue;
    }
    if (!first) {
      payload << ",";
    }
    first = false;
    payload << "{\"dst_descriptor\":"
            << (static_cast<std::uint64_t>(dst_descriptor_range_starts[dst_range_index].ptr) +
                static_cast<std::uint64_t>(dst_offset) * descriptor_size)
            << ",\"src_descriptor\":"
            << (static_cast<std::uint64_t>(src_descriptor_range_starts[src_range_index].ptr) +
                static_cast<std::uint64_t>(src_offset) * descriptor_size)
            << "}";
    ++dst_offset;
    ++src_offset;
  }
  payload << "]}";
  return payload.str();
}

void STDMETHODCALLTYPE hook_device_copy_descriptors(
    ID3D12Device *self,
    UINT dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
    const UINT *dst_descriptor_range_sizes,
    UINT src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
    const UINT *src_descriptor_range_sizes,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type)
{
  const auto hook = device_hook_for(self);
  if (hook.copy_descriptors) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.copy_descriptors(
        self,
        dst_descriptor_range_count,
        dst_descriptor_range_starts,
        dst_descriptor_range_sizes,
        src_descriptor_range_count,
        src_descriptor_range_starts,
        src_descriptor_range_sizes,
        descriptor_heap_type);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked(
      "ID3D12Device::CopyDescriptors",
      S_OK,
      {self},
      {},
      copy_descriptors_payload(
          self,
          descriptor_heap_type,
          dst_descriptor_range_count,
          dst_descriptor_range_starts,
          dst_descriptor_range_sizes,
          src_descriptor_range_count,
          src_descriptor_range_starts,
          src_descriptor_range_sizes));
}

void STDMETHODCALLTYPE hook_device_copy_descriptors_simple(
    ID3D12Device *self,
    UINT descriptor_count,
    D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_start,
    D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_start,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type)
{
  const auto hook = device_hook_for(self);
  if (hook.copy_descriptors_simple) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.copy_descriptors_simple(
        self,
        descriptor_count,
        dst_descriptor_range_start,
        src_descriptor_range_start,
        descriptor_heap_type);
  }
  const UINT range_size = descriptor_count;
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked(
      "ID3D12Device::CopyDescriptorsSimple",
      S_OK,
      {self},
      {},
      copy_descriptors_payload(
          self,
          descriptor_heap_type,
          1,
          &dst_descriptor_range_start,
          &range_size,
          1,
          &src_descriptor_range_start,
          &range_size));
}

void STDMETHODCALLTYPE hook_device_create_constant_buffer_view(
    ID3D12Device *self,
    const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_constant_buffer_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    hook.create_constant_buffer_view(self, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  const auto resolve = resolve_gpu_virtual_address_locked(desc ? desc->BufferLocation : 0);
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"buffer_location\":" << (desc ? desc->BufferLocation : 0)
          << ",\"size_in_bytes\":" << (desc ? desc->SizeInBytes : 0);
  append_gpu_virtual_address_resolve_json(payload, resolve);
  payload << "}";
  std::vector<apitrace::trace::ObjectId> extra_refs;
  append_gpuva_object_ref(extra_refs, resolve);
  record_call_locked("ID3D12Device::CreateConstantBufferView", S_OK, {self}, {}, payload.str(), extra_refs);
}

void STDMETHODCALLTYPE hook_device_create_shader_resource_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_shader_resource_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.create_shader_resource_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0)
          << ",\"shader_4_component_mapping\":" << (desc ? desc->Shader4ComponentMapping : 0)
          << ",\"view\":" << srv_desc_detail_json(desc) << "}";
  record_call_locked("ID3D12Device::CreateShaderResourceView", S_OK, {self, resource}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_device_create_unordered_access_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    ID3D12Resource *counter_resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_unordered_access_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    ScopedResourceOriginalVTable original_counter_resource_vtable(counter_resource);
    hook.create_unordered_access_view(self, resource, counter_resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0)
          << ",\"view\":" << uav_desc_detail_json(desc) << "}";
  record_call_locked("ID3D12Device::CreateUnorderedAccessView", S_OK, {self, resource, counter_resource}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_device_create_render_target_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_render_target_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.create_render_target_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0)
          << ",\"view\":" << rtv_desc_detail_json(desc) << "}";
  record_call_locked("ID3D12Device::CreateRenderTargetView", S_OK, {self, resource}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_device_create_depth_stencil_view(
    ID3D12Device *self,
    ID3D12Resource *resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
  const auto hook = device_hook_for(self);
  if (hook.create_depth_stencil_view) {
    ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.create_depth_stencil_view(self, resource, desc, descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(descriptor)
          << ",\"format\":" << (desc ? static_cast<unsigned int>(desc->Format) : 0)
          << ",\"view_dimension\":" << (desc ? static_cast<unsigned int>(desc->ViewDimension) : 0)
          << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
          << ",\"view\":" << dsv_desc_detail_json(desc) << "}";
  record_call_locked("ID3D12Device::CreateDepthStencilView", S_OK, {self, resource}, {}, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_device_create_committed_resource(
    ID3D12Device *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_committed_resource) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_committed_resource(self, heap_properties, heap_flags, desc, initial_state, optimized_clear_value, riid, resource);
  if (SUCCEEDED(hr) && resource && *resource) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      const auto resource_object_id =
          register_fresh_object_locked(*resource, apitrace::trace::ObjectKind::Resource, "ID3D12Resource", parent);
      const auto gpu_virtual_address = resource_gpu_virtual_address(static_cast<ID3D12Resource *>(*resource));
      capture_state().resource_gpu_virtual_addresses[static_cast<ID3D12Resource *>(*resource)] = {
          resource_object_id,
          gpu_virtual_address,
          desc ? desc->Width : 0,
          capture_state().next_sequence,
      };
      std::ostringstream payload;
      payload << "{\"heap_type\":" << (heap_properties ? static_cast<unsigned int>(heap_properties->Type) : 0)
              << ",\"heap_flags\":" << static_cast<unsigned int>(heap_flags)
              << ",\"initial_state\":" << static_cast<unsigned int>(initial_state)
              << ",\"gpu_virtual_address\":" << gpu_virtual_address
              << ",\"resource_desc\":" << resource_desc_json(desc)
              << ",\"optimized_clear_value\":" << clear_value_json(optimized_clear_value)
              << "}";
      record_call_locked("ID3D12Device::CreateCommittedResource", hr, {self, *resource}, {}, payload.str());
    }
    if (heap_properties && heap_properties->Type == D3D12_HEAP_TYPE_UPLOAD) {
      patch_resource(static_cast<ID3D12Resource *>(*resource));
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_heap(
    ID3D12Device *self,
    const D3D12_HEAP_DESC *desc,
    REFIID riid,
    void **heap)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_heap) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_heap(self, desc, riid, heap);
  if (SUCCEEDED(hr) && heap && *heap) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_fresh_object_locked(*heap, apitrace::trace::ObjectKind::Heap, "ID3D12Heap", parent);
    capture_state().heap_types[static_cast<ID3D12Heap *>(*heap)] =
        desc ? static_cast<UINT>(desc->Properties.Type) : 0;
    std::ostringstream payload;
    payload << "{\"size_in_bytes\":" << (desc ? desc->SizeInBytes : 0)
            << ",\"alignment\":" << (desc ? desc->Alignment : 0)
            << ",\"heap_type\":" << (desc ? static_cast<unsigned int>(desc->Properties.Type) : 0)
            << ",\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0)
            << "}";
    record_call_locked("ID3D12Device::CreateHeap", hr, {self, *heap}, {}, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_placed_resource(
    ID3D12Device *self,
    ID3D12Heap *heap,
    UINT64 heap_offset,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_placed_resource) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr =
      hook.create_placed_resource(self, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource);
  bool should_patch_resource = false;
  if (SUCCEEDED(hr) && resource && *resource) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto heap_object_id = lookup_object_id_locked(heap);
      const auto resource_object_id = register_fresh_object_locked(
          *resource,
          apitrace::trace::ObjectKind::Resource,
          "ID3D12Resource",
          heap_object_id);
      const auto gpu_virtual_address = resource_gpu_virtual_address(static_cast<ID3D12Resource *>(*resource));
      capture_state().resource_gpu_virtual_addresses[static_cast<ID3D12Resource *>(*resource)] = {
          resource_object_id,
          gpu_virtual_address,
          desc ? desc->Width : 0,
          capture_state().next_sequence,
      };
      const auto heap_type_it = capture_state().heap_types.find(heap);
      should_patch_resource =
          heap_type_it != capture_state().heap_types.end() &&
          heap_type_it->second == static_cast<UINT>(D3D12_HEAP_TYPE_UPLOAD);
      std::ostringstream payload;
      payload << "{\"heap_object_id\":" << object_id_json(heap_object_id)
              << ",\"heap_offset\":" << heap_offset
              << ",\"initial_state\":" << static_cast<unsigned int>(initial_state)
              << ",\"gpu_virtual_address\":" << gpu_virtual_address
              << ",\"resource_desc\":" << resource_desc_json(desc)
              << ",\"optimized_clear_value\":" << clear_value_json(optimized_clear_value)
              << "}";
      record_call_locked("ID3D12Device::CreatePlacedResource", hr, {self, heap, *resource}, {}, payload.str());
    }
    if (should_patch_resource) {
      patch_resource(static_cast<ID3D12Resource *>(*resource));
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_fence(
    ID3D12Device *self,
    UINT64 initial_value,
    D3D12_FENCE_FLAGS flags,
    REFIID riid,
    void **fence)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_fence) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_fence(self, initial_value, flags, riid, fence);
  if (SUCCEEDED(hr) && fence && *fence) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      const auto parent = lookup_object_id_locked(self);
      register_fresh_object_locked(*fence, apitrace::trace::ObjectKind::Fence, "ID3D12Fence", parent);
      std::ostringstream payload;
      payload << "{\"initial_value\":" << initial_value << ",\"flags\":" << static_cast<unsigned int>(flags) << "}";
      record_call_locked("ID3D12Device::CreateFence", hr, {self, *fence}, {}, payload.str());
    }
    patch_fence(static_cast<ID3D12Fence *>(*fence));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_device_create_command_signature(
    ID3D12Device *self,
    const D3D12_COMMAND_SIGNATURE_DESC *desc,
    ID3D12RootSignature *root_signature,
    REFIID riid,
    void **command_signature)
{
  const auto hook = device_hook_for(self);
  if (!hook.create_command_signature) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Device, ID3D12DeviceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.create_command_signature(self, desc, root_signature, riid, command_signature);
  if (SUCCEEDED(hr) && command_signature && *command_signature) {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto parent = lookup_object_id_locked(self);
    register_fresh_object_locked(*command_signature, apitrace::trace::ObjectKind::CommandSignature, "ID3D12CommandSignature", parent);
    std::ostringstream payload;
    payload << "{\"byte_stride\":" << (desc ? desc->ByteStride : 0)
            << ",\"argument_count\":" << (desc ? desc->NumArgumentDescs : 0)
            << ",\"node_mask\":" << (desc ? desc->NodeMask : 0);
    if (desc && desc->NumArgumentDescs > 0 && desc->pArgumentDescs) {
      payload << ",\"arguments\":[";
      for (UINT index = 0; index < desc->NumArgumentDescs; ++index) {
        if (index != 0) {
          payload << ",";
        }
        const auto &argument = desc->pArgumentDescs[index];
        payload << "{\"type\":" << static_cast<unsigned int>(argument.Type);
        switch (argument.Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
          payload << ",\"slot\":" << argument.VertexBuffer.Slot;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
          payload << ",\"root_parameter_index\":" << argument.Constant.RootParameterIndex
                  << ",\"dest_offset_in32bit_values\":" << argument.Constant.DestOffsetIn32BitValues
                  << ",\"num32bit_values_to_set\":" << argument.Constant.Num32BitValuesToSet;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
          payload << ",\"root_parameter_index\":" << argument.ConstantBufferView.RootParameterIndex;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
          payload << ",\"root_parameter_index\":" << argument.ShaderResourceView.RootParameterIndex;
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
          payload << ",\"root_parameter_index\":" << argument.UnorderedAccessView.RootParameterIndex;
          break;
        default:
          break;
        }
        payload << "}";
      }
      payload << "]";
    }
    payload << "}";
    record_call_locked("ID3D12Device::CreateCommandSignature", hr, {self, root_signature, *command_signature}, {}, payload.str());
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_allocator_reset(ID3D12CommandAllocator *self)
{
  const auto hook = allocator_hook_for(self);
  if (!hook.reset) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12CommandAllocator, ID3D12CommandAllocatorVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.reset(self);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12CommandAllocator::Reset", hr, {self}, {}, "{}");
  return hr;
}

void STDMETHODCALLTYPE hook_queue_execute_command_lists(
    ID3D12CommandQueue *self,
    UINT command_list_count,
    ID3D12CommandList *const *command_lists)
{
  const auto hook = queue_hook_for(self);
  if (hook.execute_command_lists) {
    const auto referenced_resources = command_list_array_resources_snapshot(command_list_count, command_lists);
    ScopedOriginalVTable<ID3D12CommandQueue, ID3D12CommandQueueVtbl> original_vtable(self, hook.vtable);
    ScopedCommandListArrayOriginalVTables original_command_list_vtables(command_list_count, command_lists);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.execute_command_lists(self, command_list_count, command_lists);
  }
  proxy_debug_logf("hook_queue_execute_command_lists self=%p count=%u", self, command_list_count);
  std::vector<const void *> refs = {self};
  for (UINT index = 0; index < command_list_count; ++index) {
    refs.push_back(command_lists ? command_lists[index] : nullptr);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"command_list_count\":" << command_list_count << "}";
  record_call_locked("ID3D12CommandQueue::ExecuteCommandLists", S_OK, refs, {}, payload.str());
  record_boundary_locked(apitrace::trace::BoundaryKind::Submit, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_queue_signal(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value)
{
  const auto hook = queue_hook_for(self);
  if (!hook.signal) {
    return E_FAIL;
  }
  const auto fence_hook = fence_hook_for(fence);
  ScopedOriginalVTable<ID3D12CommandQueue, ID3D12CommandQueueVtbl> original_vtable(self, hook.vtable);
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_fence_vtable(fence, fence_hook.vtable);
  const HRESULT hr = hook.signal(self, fence, value);
  proxy_debug_logf("hook_queue_signal self=%p fence=%p value=%llu", self, fence, static_cast<unsigned long long>(value));
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"fence_value\":" << value << "}";
  record_call_locked("ID3D12CommandQueue::Signal", hr, {self, fence}, {}, payload.str());
  record_boundary_locked(apitrace::trace::BoundaryKind::Fence, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_queue_wait(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value)
{
  const auto hook = queue_hook_for(self);
  if (!hook.wait) {
    return E_FAIL;
  }
  const auto fence_hook = fence_hook_for(fence);
  ScopedOriginalVTable<ID3D12CommandQueue, ID3D12CommandQueueVtbl> original_vtable(self, hook.vtable);
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_fence_vtable(fence, fence_hook.vtable);
  const HRESULT hr = hook.wait(self, fence, value);
  proxy_debug_logf("hook_queue_wait self=%p fence=%p value=%llu", self, fence, static_cast<unsigned long long>(value));
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"fence_value\":" << value << "}";
  record_call_locked("ID3D12CommandQueue::Wait", hr, {self, fence}, {}, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_command_list_query_interface(ID3D12GraphicsCommandList *self, REFIID riid, void **object)
{
  const auto hook = command_list_hook_for(self);
  if (!hook.query_interface) {
    return E_NOINTERFACE;
  }
  ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.query_interface(self, riid, object);
  if (SUCCEEDED(hr) && object && *object) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      remember_object_alias_locked(*object, self);
      record_call_locked("ID3D12GraphicsCommandList::QueryInterface", hr, {self, *object}, {}, "{}");
    }
    if (IsEqualGUID(riid, kIidD3D12GraphicsCommandList6)) {
      patch_command_list6(static_cast<ID3D12GraphicsCommandList6 *>(*object));
    } else if (IsEqualGUID(riid, kIidD3D12GraphicsCommandList4)) {
      patch_command_list4(static_cast<ID3D12GraphicsCommandList4 *>(*object));
    } else if (IsEqualGUID(riid, kIidD3D12GraphicsCommandList2)) {
      patch_command_list2(static_cast<ID3D12GraphicsCommandList2 *>(*object));
    } else if (IsEqualGUID(riid, kIidD3D12GraphicsCommandList1)) {
      patch_command_list1(static_cast<ID3D12GraphicsCommandList1 *>(*object));
    } else {
      patch_command_list(static_cast<ID3D12GraphicsCommandList *>(*object));
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_command_list_close(ID3D12GraphicsCommandList *self)
{
  const auto hook = command_list_hook_for(self);
  if (!hook.close) {
    return E_FAIL;
  }
  const auto referenced_resources = command_list_resources_snapshot(self);
  ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
  ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
  const HRESULT hr = hook.close(self);
  proxy_debug_logf("hook_command_list_close self=%p hr=0x%08lx", self, static_cast<unsigned long>(hr));
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::Close", hr, {self}, {}, "{}");
  record_boundary_locked(apitrace::trace::BoundaryKind::CommandList, "{\"label\":\"Close\"}");
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_command_list_reset(
    ID3D12GraphicsCommandList *self,
    ID3D12CommandAllocator *allocator,
    ID3D12PipelineState *initial_state)
{
  const auto hook = command_list_hook_for(self);
  if (!hook.reset) {
    return E_FAIL;
  }
  const auto allocator_hook = allocator_hook_for(allocator);
  ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
  ScopedOriginalVTable<ID3D12CommandAllocator, ID3D12CommandAllocatorVtbl> original_allocator_vtable(
      allocator,
      allocator_hook.vtable);
  const HRESULT hr = hook.reset(self, allocator, initial_state);
  proxy_debug_logf("hook_command_list_reset self=%p allocator=%p hr=0x%08lx", self, allocator, static_cast<unsigned long>(hr));
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (SUCCEEDED(hr)) {
    capture_state().command_list_resources[self].clear();
  }
  record_call_locked("ID3D12GraphicsCommandList::Reset", hr, {self, allocator, initial_state}, {}, "{}");
  return hr;
}

void STDMETHODCALLTYPE hook_command_list_clear_state(
    ID3D12GraphicsCommandList *self,
    ID3D12PipelineState *pipeline_state)
{
  const auto hook = command_list_hook_for(self);
  if (hook.clear_state) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.clear_state(self, pipeline_state);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"pipeline_state_object_id\":" << object_id_json(lookup_object_id_locked(pipeline_state)) << "}";
  record_call_locked("ID3D12GraphicsCommandList::ClearState", S_OK, {self, pipeline_state}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_pipeline_state(
    ID3D12GraphicsCommandList *self,
    ID3D12PipelineState *pipeline_state)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_pipeline_state) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_pipeline_state(self, pipeline_state);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::SetPipelineState", S_OK, {self, pipeline_state}, {}, "{}");
}

void STDMETHODCALLTYPE hook_command_list_om_set_blend_factor(
    ID3D12GraphicsCommandList *self,
    const FLOAT blend_factor[4])
{
  const auto hook = command_list_hook_for(self);
  if (hook.om_set_blend_factor) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.om_set_blend_factor(self, blend_factor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"blend_factor\":[";
  for (UINT index = 0; index < 4; ++index) {
    if (index) {
      payload << ",";
    }
    payload << (blend_factor ? blend_factor[index] : 0.0f);
  }
  payload << "]}";
  record_call_locked("ID3D12GraphicsCommandList::OMSetBlendFactor", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_om_set_stencil_ref(
    ID3D12GraphicsCommandList *self,
    UINT stencil_ref)
{
  const auto hook = command_list_hook_for(self);
  if (hook.om_set_stencil_ref) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.om_set_stencil_ref(self, stencil_ref);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"stencil_ref\":" << stencil_ref << "}";
  record_call_locked("ID3D12GraphicsCommandList::OMSetStencilRef", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_signature(
    ID3D12GraphicsCommandList *self,
    ID3D12RootSignature *root_signature)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_signature) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_signature(self, root_signature);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootSignature", S_OK, {self, root_signature}, {}, "{}");
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_signature(
    ID3D12GraphicsCommandList *self,
    ID3D12RootSignature *root_signature)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_signature) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_signature(self, root_signature);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList::SetComputeRootSignature", S_OK, {self, root_signature}, {}, "{}");
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_descriptor_table(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_descriptor_table) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_descriptor_table(self, root_parameter_index, base_descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"base_descriptor\":" << gpu_descriptor_handle_json(base_descriptor) << "}";
  record_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_descriptor_table(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_descriptor_table) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_descriptor_table(self, root_parameter_index, base_descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"base_descriptor\":" << gpu_descriptor_handle_json(base_descriptor) << "}";
  record_call_locked("ID3D12GraphicsCommandList::SetComputeRootDescriptorTable", S_OK, {self}, {}, payload.str());
}

void record_root_32bit_constants_call_locked(
    const char *function_name,
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT constant_count,
    const void *data,
    UINT dst_offset)
{
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"constant_count\":" << constant_count
          << ",\"dst_offset\":" << dst_offset
          << ",\"values\":[";
  const auto *values = static_cast<const UINT *>(data);
  for (UINT index = 0; index < constant_count; ++index) {
    if (index != 0) {
      payload << ",";
    }
    payload << (values ? values[index] : 0);
  }
  payload << "]}";
  record_call_locked(function_name, S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_32bit_constant(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT data,
    UINT dst_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_32bit_constant) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_32bit_constant(self, root_parameter_index, data, dst_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_32bit_constants_call_locked(
      "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant",
      self,
      root_parameter_index,
      1,
      &data,
      dst_offset);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_32bit_constant(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT data,
    UINT dst_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_32bit_constant) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_32bit_constant(self, root_parameter_index, data, dst_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_32bit_constants_call_locked(
      "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant",
      self,
      root_parameter_index,
      1,
      &data,
      dst_offset);
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_32bit_constants(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT constant_count,
    const void *data,
    UINT dst_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_32bit_constants) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_32bit_constants(self, root_parameter_index, constant_count, data, dst_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_32bit_constants_call_locked(
      "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants",
      self,
      root_parameter_index,
      constant_count,
      data,
      dst_offset);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_32bit_constants(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    UINT constant_count,
    const void *data,
    UINT dst_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_32bit_constants) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_32bit_constants(self, root_parameter_index, constant_count, data, dst_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_32bit_constants_call_locked(
      "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants",
      self,
      root_parameter_index,
      constant_count,
      data,
      dst_offset);
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_constant_buffer_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_constant_buffer_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_constant_buffer_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto resolve = resolve_gpu_virtual_address_locked(buffer_location);
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"buffer_location\":" << gpu_virtual_address_json(buffer_location);
  append_gpu_virtual_address_resolve_json(payload, resolve);
  payload << "}";
  std::vector<apitrace::trace::ObjectId> extra_refs;
  append_gpuva_object_ref(extra_refs, resolve);
  record_call_locked(
      "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView",
      S_OK,
      {self},
      {},
      payload.str(),
      extra_refs);
}

void record_root_descriptor_call_locked(
    const char *function_name,
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto resolve = resolve_gpu_virtual_address_locked(buffer_location);
  std::ostringstream payload;
  payload << "{\"root_parameter_index\":" << root_parameter_index
          << ",\"buffer_location\":" << gpu_virtual_address_json(buffer_location);
  append_gpu_virtual_address_resolve_json(payload, resolve);
  payload << "}";
  std::vector<apitrace::trace::ObjectId> extra_refs;
  append_gpuva_object_ref(extra_refs, resolve);
  record_call_locked(function_name, S_OK, {self}, {}, payload.str(), extra_refs);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_constant_buffer_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_constant_buffer_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_constant_buffer_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetComputeRootConstantBufferView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_shader_resource_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_shader_resource_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_shader_resource_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_shader_resource_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_shader_resource_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_shader_resource_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetComputeRootShaderResourceView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_set_graphics_root_unordered_access_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_graphics_root_unordered_access_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_graphics_root_unordered_access_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_set_compute_root_unordered_access_view(
    ID3D12GraphicsCommandList *self,
    UINT root_parameter_index,
    D3D12_GPU_VIRTUAL_ADDRESS buffer_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_compute_root_unordered_access_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_compute_root_unordered_access_view(self, root_parameter_index, buffer_location);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_root_descriptor_call_locked("ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView", self, root_parameter_index, buffer_location);
}

void STDMETHODCALLTYPE hook_command_list_rs_set_viewports(
    ID3D12GraphicsCommandList *self,
    UINT viewport_count,
    const D3D12_VIEWPORT *viewports)
{
  const auto hook = command_list_hook_for(self);
  if (hook.rs_set_viewports) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.rs_set_viewports(self, viewport_count, viewports);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"viewport_count\":" << viewport_count;
  if (viewport_count > 0 && viewports) {
    payload << ",\"first\":{\"x\":" << viewports[0].TopLeftX
            << ",\"y\":" << viewports[0].TopLeftY
            << ",\"width\":" << viewports[0].Width
            << ",\"height\":" << viewports[0].Height
            << ",\"min_depth\":" << viewports[0].MinDepth
            << ",\"max_depth\":" << viewports[0].MaxDepth << "}";
    payload << ",\"viewports\":[";
    for (UINT index = 0; index < viewport_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"x\":" << viewports[index].TopLeftX
              << ",\"y\":" << viewports[index].TopLeftY
              << ",\"width\":" << viewports[index].Width
              << ",\"height\":" << viewports[index].Height
              << ",\"min_depth\":" << viewports[index].MinDepth
              << ",\"max_depth\":" << viewports[index].MaxDepth << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::RSSetViewports", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_rs_set_scissor_rects(
    ID3D12GraphicsCommandList *self,
    UINT rect_count,
    const D3D12_RECT *rects)
{
  const auto hook = command_list_hook_for(self);
  if (hook.rs_set_scissor_rects) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.rs_set_scissor_rects(self, rect_count, rects);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"rect_count\":" << rect_count;
  if (rect_count > 0 && rects) {
    payload << ",\"first\":{\"left\":" << rects[0].left
            << ",\"top\":" << rects[0].top
            << ",\"right\":" << rects[0].right
            << ",\"bottom\":" << rects[0].bottom << "}";
    payload << ",\"rects\":[";
    for (UINT index = 0; index < rect_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"left\":" << rects[index].left
              << ",\"top\":" << rects[index].top
              << ",\"right\":" << rects[index].right
              << ",\"bottom\":" << rects[index].bottom << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::RSSetScissorRects", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_om_set_render_targets(
    ID3D12GraphicsCommandList *self,
    UINT render_target_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
    BOOL single_handle_to_descriptor_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor)
{
  const auto hook = command_list_hook_for(self);
  if (hook.om_set_render_targets) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.om_set_render_targets(self, render_target_count, render_target_descriptors, single_handle_to_descriptor_range, depth_stencil_descriptor);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"render_target_count\":" << render_target_count
          << ",\"single_handle_to_descriptor_range\":" << (single_handle_to_descriptor_range ? "true" : "false");
  if (render_target_count > 0 && render_target_descriptors) {
    payload << ",\"first_rtv\":" << descriptor_handle_json(render_target_descriptors[0]);
    payload << ",\"render_targets\":[";
    for (UINT index = 0; index < render_target_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << descriptor_handle_json(render_target_descriptors[index]);
    }
    payload << "]";
  }
  if (depth_stencil_descriptor) {
    payload << ",\"dsv\":" << descriptor_handle_json(*depth_stencil_descriptor);
  } else {
    payload << ",\"dsv\":0";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::OMSetRenderTargets", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_clear_render_target_view(
    ID3D12GraphicsCommandList *self,
    D3D12_CPU_DESCRIPTOR_HANDLE render_target_view,
    const FLOAT color_rgba[4],
    UINT rect_count,
    const D3D12_RECT *rects)
{
  const auto hook = command_list_hook_for(self);
  if (hook.clear_render_target_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.clear_render_target_view(self, render_target_view, color_rgba, rect_count, rects);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(render_target_view)
          << ",\"rect_count\":" << rect_count;
  if (color_rgba) {
    payload << ",\"color\":[" << color_rgba[0] << "," << color_rgba[1] << "," << color_rgba[2] << "," << color_rgba[3] << "]";
  }
  if (rect_count > 0 && rects) {
    payload << ",\"first_rect\":{\"left\":" << rects[0].left
            << ",\"top\":" << rects[0].top
            << ",\"right\":" << rects[0].right
            << ",\"bottom\":" << rects[0].bottom << "}";
    payload << ",\"rects\":[";
    for (UINT index = 0; index < rect_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"left\":" << rects[index].left
              << ",\"top\":" << rects[index].top
              << ",\"right\":" << rects[index].right
              << ",\"bottom\":" << rects[index].bottom << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::ClearRenderTargetView", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_clear_depth_stencil_view(
    ID3D12GraphicsCommandList *self,
    D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_view,
    D3D12_CLEAR_FLAGS clear_flags,
    FLOAT depth,
    UINT8 stencil,
    UINT rect_count,
    const D3D12_RECT *rects)
{
  const auto hook = command_list_hook_for(self);
  if (hook.clear_depth_stencil_view) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.clear_depth_stencil_view(self, depth_stencil_view, clear_flags, depth, stencil, rect_count, rects);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"descriptor\":" << descriptor_handle_json(depth_stencil_view)
          << ",\"clear_flags\":" << static_cast<unsigned int>(clear_flags)
          << ",\"depth\":" << depth
          << ",\"stencil\":" << static_cast<unsigned int>(stencil)
          << ",\"rect_count\":" << rect_count;
  if (rect_count > 0 && rects) {
    payload << ",\"first_rect\":{\"left\":" << rects[0].left
            << ",\"top\":" << rects[0].top
            << ",\"right\":" << rects[0].right
            << ",\"bottom\":" << rects[0].bottom << "}";
    payload << ",\"rects\":[";
    for (UINT index = 0; index < rect_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << "{\"left\":" << rects[index].left
              << ",\"top\":" << rects[index].top
              << ",\"right\":" << rects[index].right
              << ",\"bottom\":" << rects[index].bottom << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::ClearDepthStencilView", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_clear_unordered_access_view_uint(
    ID3D12GraphicsCommandList *self,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
    ID3D12Resource *resource,
    const UINT values[4],
    UINT rect_count,
    const D3D12_RECT *rects)
{
  const auto hook = command_list_hook_for(self);
  if (hook.clear_unordered_access_view_uint) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.clear_unordered_access_view_uint(self, gpu_handle, cpu_handle, resource, values, rect_count, rects);
  }
  {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    remember_command_list_resource_locked(self, resource);
    std::ostringstream payload;
    payload << "{\"gpu_descriptor\":" << gpu_descriptor_handle_json(gpu_handle)
            << ",\"cpu_descriptor\":" << descriptor_handle_json(cpu_handle)
            << ",\"values\":[";
    for (UINT index = 0; index < 4; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << (values ? values[index] : 0);
    }
    payload << "],\"rect_count\":" << rect_count;
    append_rect_array_json(payload, rect_count, rects);
    payload << "}";
    record_call_locked("ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint", S_OK, {self, resource}, {}, payload.str());
  }
}

void STDMETHODCALLTYPE hook_command_list_clear_unordered_access_view_float(
    ID3D12GraphicsCommandList *self,
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
    ID3D12Resource *resource,
    const FLOAT values[4],
    UINT rect_count,
    const D3D12_RECT *rects)
{
  const auto hook = command_list_hook_for(self);
  if (hook.clear_unordered_access_view_float) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.clear_unordered_access_view_float(self, gpu_handle, cpu_handle, resource, values, rect_count, rects);
  }
  {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    remember_command_list_resource_locked(self, resource);
    std::ostringstream payload;
    payload << "{\"gpu_descriptor\":" << gpu_descriptor_handle_json(gpu_handle)
            << ",\"cpu_descriptor\":" << descriptor_handle_json(cpu_handle)
            << ",\"values\":[";
    for (UINT index = 0; index < 4; ++index) {
      if (index != 0) {
        payload << ",";
      }
      payload << (values ? values[index] : 0.0f);
    }
    payload << "],\"rect_count\":" << rect_count;
    append_rect_array_json(payload, rect_count, rects);
    payload << "}";
    record_call_locked("ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat", S_OK, {self, resource}, {}, payload.str());
  }
}

void STDMETHODCALLTYPE hook_command_list_discard_resource(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *resource,
    const D3D12_DISCARD_REGION *region)
{
  const auto hook = command_list_hook_for(self);
  if (hook.discard_resource) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(resource);
    hook.discard_resource(self, resource, region);
  }
  {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    remember_command_list_resource_locked(self, resource);
    const UINT rect_count = region ? region->NumRects : 0;
    std::ostringstream payload;
    payload << "{\"first_subresource\":" << (region ? region->FirstSubresource : 0)
            << ",\"subresource_count\":" << (region ? region->NumSubresources : 0)
            << ",\"rect_count\":" << rect_count;
    append_rect_array_json(payload, rect_count, region ? region->pRects : nullptr);
    payload << "}";
    record_call_locked("ID3D12GraphicsCommandList::DiscardResource", S_OK, {self, resource}, {}, payload.str());
  }
}

void STDMETHODCALLTYPE hook_command_list_ia_set_primitive_topology(
    ID3D12GraphicsCommandList *self,
    D3D12_PRIMITIVE_TOPOLOGY primitive_topology)
{
  const auto hook = command_list_hook_for(self);
  if (hook.ia_set_primitive_topology) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.ia_set_primitive_topology(self, primitive_topology);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"primitive_topology\":" << static_cast<unsigned int>(primitive_topology) << "}";
  record_call_locked("ID3D12GraphicsCommandList::IASetPrimitiveTopology", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_ia_set_vertex_buffers(
    ID3D12GraphicsCommandList *self,
    UINT start_slot,
    UINT view_count,
    const D3D12_VERTEX_BUFFER_VIEW *views)
{
  const auto hook = command_list_hook_for(self);
  if (hook.ia_set_vertex_buffers) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.ia_set_vertex_buffers(self, start_slot, view_count, views);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  std::vector<apitrace::trace::ObjectId> extra_refs;
  payload << "{\"start_slot\":" << start_slot << ",\"view_count\":" << view_count;
  if (view_count > 0 && views) {
    payload << ",\"first\":{\"buffer_location\":" << gpu_virtual_address_json(views[0].BufferLocation)
            << ",\"size_in_bytes\":" << views[0].SizeInBytes
            << ",\"stride_in_bytes\":" << views[0].StrideInBytes << "}";
    payload << ",\"views\":[";
    for (UINT index = 0; index < view_count; ++index) {
      if (index != 0) {
        payload << ",";
      }
      const auto resolve = resolve_gpu_virtual_address_locked(views[index].BufferLocation);
      append_gpuva_object_ref(extra_refs, resolve);
      payload << "{\"buffer_location\":" << gpu_virtual_address_json(views[index].BufferLocation);
      append_gpu_virtual_address_resolve_json(payload, resolve);
      payload << ",\"size_in_bytes\":" << views[index].SizeInBytes
              << ",\"stride_in_bytes\":" << views[index].StrideInBytes << "}";
    }
    payload << "]";
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::IASetVertexBuffers", S_OK, {self}, {}, payload.str(), extra_refs);
}

void STDMETHODCALLTYPE hook_command_list_ia_set_index_buffer(
    ID3D12GraphicsCommandList *self,
    const D3D12_INDEX_BUFFER_VIEW *view)
{
  const auto hook = command_list_hook_for(self);
  if (hook.ia_set_index_buffer) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.ia_set_index_buffer(self, view);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  std::vector<apitrace::trace::ObjectId> extra_refs;
  payload << "{";
  if (view) {
    const auto resolve = resolve_gpu_virtual_address_locked(view->BufferLocation);
    append_gpuva_object_ref(extra_refs, resolve);
    payload << "\"buffer_location\":" << gpu_virtual_address_json(view->BufferLocation);
    append_gpu_virtual_address_resolve_json(payload, resolve);
    payload << ",\"size_in_bytes\":" << view->SizeInBytes
            << ",\"format\":" << static_cast<unsigned int>(view->Format);
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList::IASetIndexBuffer", S_OK, {self}, {}, payload.str(), extra_refs);
}

void STDMETHODCALLTYPE hook_command_list_resource_barrier(
    ID3D12GraphicsCommandList *self,
    UINT barrier_count,
    const D3D12_RESOURCE_BARRIER *barriers)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> barrier_resources;
  barrier_resources.reserve(barrier_count);
  for (UINT index = 0; barriers && index < barrier_count; ++index) {
    if (barriers[index].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
      barrier_resources.push_back(barriers[index].Transition.pResource);
    } else if (barriers[index].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
      barrier_resources.push_back(barriers[index].Aliasing.pResourceBefore);
      barrier_resources.push_back(barriers[index].Aliasing.pResourceAfter);
    } else if (barriers[index].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
      barrier_resources.push_back(barriers[index].UAV.pResource);
    }
  }
  if (hook.resource_barrier) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(barrier_resources);
    hook.resource_barrier(self, barrier_count, barriers);
  }
  proxy_debug_logf("hook_command_list_resource_barrier self=%p count=%u", self, barrier_count);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, barrier_resources);
  std::vector<const void *> refs = {self};
  std::ostringstream payload;
  payload << "{\"barrier_count\":" << barrier_count << ",\"barriers\":[";
  for (UINT index = 0; index < barrier_count; ++index) {
    if (index != 0) {
      payload << ",";
    }
    const auto &barrier = barriers[index];
    payload << "{\"type\":" << static_cast<unsigned int>(barrier.Type)
            << ",\"flags\":" << static_cast<unsigned int>(barrier.Flags);
    if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
      refs.push_back(barrier.Transition.pResource);
      payload << ",\"resource_object_id\":" << object_id_json(lookup_object_id_locked(barrier.Transition.pResource))
              << ",\"before\":" << static_cast<unsigned int>(barrier.Transition.StateBefore)
              << ",\"after\":" << static_cast<unsigned int>(barrier.Transition.StateAfter)
              << ",\"subresource\":" << barrier.Transition.Subresource;
    } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
      refs.push_back(barrier.Aliasing.pResourceBefore);
      refs.push_back(barrier.Aliasing.pResourceAfter);
      payload << ",\"resource_before_object_id\":"
              << object_id_json(lookup_object_id_locked(barrier.Aliasing.pResourceBefore))
              << ",\"resource_after_object_id\":"
              << object_id_json(lookup_object_id_locked(barrier.Aliasing.pResourceAfter));
    } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
      refs.push_back(barrier.UAV.pResource);
      payload << ",\"resource_object_id\":" << object_id_json(lookup_object_id_locked(barrier.UAV.pResource));
    }
    payload << "}";
  }
  payload << "]}";
  record_call_locked("ID3D12GraphicsCommandList::ResourceBarrier", S_OK, refs, {}, payload.str());
  record_boundary_locked(apitrace::trace::BoundaryKind::Barrier, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_descriptor_heaps(
    ID3D12GraphicsCommandList *self,
    UINT heap_count,
    ID3D12DescriptorHeap *const *heaps)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_descriptor_heaps) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.set_descriptor_heaps(self, heap_count, heaps);
  }
  proxy_debug_logf("hook_command_list_set_descriptor_heaps self=%p heap_count=%u", self, heap_count);
  std::vector<const void *> refs = {self};
  for (UINT index = 0; index < heap_count; ++index) {
    refs.push_back(heaps ? heaps[index] : nullptr);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"heap_count\":" << heap_count << "}";
  record_call_locked("ID3D12GraphicsCommandList::SetDescriptorHeaps", S_OK, refs, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_draw_instanced(
    ID3D12GraphicsCommandList *self,
    UINT vertex_count_per_instance,
    UINT instance_count,
    UINT start_vertex_location,
    UINT start_instance_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.draw_instanced) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.draw_instanced(self, vertex_count_per_instance, instance_count, start_vertex_location, start_instance_location);
  }
  proxy_debug_logf("hook_command_list_draw_instanced self=%p instance_count=%u", self, instance_count);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"vertex_count_per_instance\":" << vertex_count_per_instance
          << ",\"instance_count\":" << instance_count
          << ",\"start_vertex_location\":" << start_vertex_location
          << ",\"start_instance_location\":" << start_instance_location << "}";
  record_call_locked("ID3D12GraphicsCommandList::DrawInstanced", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_draw_indexed_instanced(
    ID3D12GraphicsCommandList *self,
    UINT index_count_per_instance,
    UINT instance_count,
    UINT start_index_location,
    INT base_vertex_location,
    UINT start_instance_location)
{
  const auto hook = command_list_hook_for(self);
  if (hook.draw_indexed_instanced) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.draw_indexed_instanced(self, index_count_per_instance, instance_count, start_index_location, base_vertex_location, start_instance_location);
  }
  proxy_debug_logf("hook_command_list_draw_indexed_instanced self=%p index_count=%u", self, index_count_per_instance);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"index_count_per_instance\":" << index_count_per_instance
          << ",\"instance_count\":" << instance_count
          << ",\"start_index_location\":" << start_index_location
          << ",\"base_vertex_location\":" << base_vertex_location
          << ",\"start_instance_location\":" << start_instance_location << "}";
  record_call_locked("ID3D12GraphicsCommandList::DrawIndexedInstanced", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_dispatch(ID3D12GraphicsCommandList *self, UINT x, UINT y, UINT z)
{
  const auto hook = command_list_hook_for(self);
  if (hook.dispatch) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.dispatch(self, x, y, z);
  }
  proxy_debug_logf("hook_command_list_dispatch self=%p groups=%u,%u,%u", self, x, y, z);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"thread_group_count_x\":" << x << ",\"thread_group_count_y\":" << y << ",\"thread_group_count_z\":" << z << "}";
  record_call_locked("ID3D12GraphicsCommandList::Dispatch", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_execute_indirect(
    ID3D12GraphicsCommandList *self,
    ID3D12CommandSignature *command_signature,
    UINT max_command_count,
    ID3D12Resource *arg_buffer,
    UINT64 arg_buffer_offset,
    ID3D12Resource *count_buffer,
    UINT64 count_buffer_offset)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {arg_buffer, count_buffer};
  if (hook.execute_indirect) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.execute_indirect(self, command_signature, max_command_count, arg_buffer, arg_buffer_offset, count_buffer, count_buffer_offset);
  }
  proxy_debug_logf("hook_command_list_execute_indirect self=%p max=%u", self, max_command_count);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"max_command_count\":" << max_command_count
          << ",\"arg_buffer_offset\":" << arg_buffer_offset
          << ",\"count_buffer_offset\":" << count_buffer_offset << "}";
  record_call_locked("ID3D12GraphicsCommandList::ExecuteIndirect", S_OK, {self, command_signature, arg_buffer, count_buffer}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_execute_bundle(
    ID3D12GraphicsCommandList *self,
    ID3D12GraphicsCommandList *bundle_command_list)
{
  const auto hook = command_list_hook_for(self);
  if (hook.execute_bundle) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.execute_bundle(self, bundle_command_list);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"bundle_command_list_object_id\":" << object_id_json(lookup_object_id_locked(bundle_command_list)) << "}";
  record_call_locked("ID3D12GraphicsCommandList::ExecuteBundle", S_OK, {self, bundle_command_list}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_copy_buffer_region(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *dst,
    UINT64 dst_offset,
    ID3D12Resource *src,
    UINT64 src_offset,
    UINT64 byte_count)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {dst, src};
  if (hook.copy_buffer_region) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.copy_buffer_region(self, dst, dst_offset, src, src_offset, byte_count);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"dst_offset\":" << dst_offset
          << ",\"src_offset\":" << src_offset
          << ",\"byte_count\":" << byte_count
          << ",\"dst_buffer_object_id\":" << object_id_json(lookup_object_id_locked(dst))
          << ",\"src_buffer_object_id\":" << object_id_json(lookup_object_id_locked(src)) << "}";
  record_call_locked("ID3D12GraphicsCommandList::CopyBufferRegion", S_OK, {self, dst, src}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_copy_texture_region(
    ID3D12GraphicsCommandList *self,
    const D3D12_TEXTURE_COPY_LOCATION *dst,
    UINT dst_x,
    UINT dst_y,
    UINT dst_z,
    const D3D12_TEXTURE_COPY_LOCATION *src,
    const D3D12_BOX *src_box)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {
      dst ? dst->pResource : nullptr,
      src ? src->pResource : nullptr,
  };
  if (hook.copy_texture_region) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.copy_texture_region(self, dst, dst_x, dst_y, dst_z, src, src_box);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"dst_x\":" << dst_x
          << ",\"dst_y\":" << dst_y
          << ",\"dst_z\":" << dst_z
          << ",\"dst_type\":" << (dst ? static_cast<unsigned int>(dst->Type) : 0)
          << ",\"src_type\":" << (src ? static_cast<unsigned int>(src->Type) : 0)
          << ",\"dst\":" << texture_copy_location_json_locked(dst)
          << ",\"src\":" << texture_copy_location_json_locked(src)
          << ",\"has_src_box\":" << (src_box ? "true" : "false");
  if (src_box) {
    payload << ",\"src_box\":{\"left\":" << src_box->left
            << ",\"top\":" << src_box->top
            << ",\"front\":" << src_box->front
            << ",\"right\":" << src_box->right
            << ",\"bottom\":" << src_box->bottom
            << ",\"back\":" << src_box->back << "}";
  }
  payload << "}";
  record_call_locked(
      "ID3D12GraphicsCommandList::CopyTextureRegion",
      S_OK,
      {self, dst ? dst->pResource : nullptr, src ? src->pResource : nullptr},
      {},
      payload.str());
}

void STDMETHODCALLTYPE hook_command_list_copy_resource(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *dst,
    ID3D12Resource *src)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {dst, src};
  if (hook.copy_resource) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.copy_resource(self, dst, src);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"dst_resource_object_id\":" << object_id_json(lookup_object_id_locked(dst))
          << ",\"src_resource_object_id\":" << object_id_json(lookup_object_id_locked(src)) << "}";
  record_call_locked("ID3D12GraphicsCommandList::CopyResource", S_OK, {self, dst, src}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_resolve_subresource(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *dst,
    UINT dst_subresource,
    ID3D12Resource *src,
    UINT src_subresource,
    DXGI_FORMAT format)
{
  const auto hook = command_list_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {dst, src};
  if (hook.resolve_subresource) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.resolve_subresource(self, dst, dst_subresource, src, src_subresource, format);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(self, referenced_resources);
  std::ostringstream payload;
  payload << "{\"dst_subresource\":" << dst_subresource
          << ",\"src_subresource\":" << src_subresource
          << ",\"format\":" << static_cast<unsigned int>(format)
          << ",\"dst_resource_object_id\":" << object_id_json(lookup_object_id_locked(dst))
          << ",\"src_resource_object_id\":" << object_id_json(lookup_object_id_locked(src)) << "}";
  record_call_locked("ID3D12GraphicsCommandList::ResolveSubresource", S_OK, {self, dst, src}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_begin_query(
    ID3D12GraphicsCommandList *self,
    ID3D12QueryHeap *heap,
    D3D12_QUERY_TYPE type,
    UINT index)
{
  const auto hook = command_list_hook_for(self);
  if (hook.begin_query) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.begin_query(self, heap, type, index);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"query_heap_object_id\":" << object_id_json(lookup_object_id_locked(heap))
          << ",\"type\":" << static_cast<unsigned int>(type)
          << ",\"index\":" << index << "}";
  record_call_locked("ID3D12GraphicsCommandList::BeginQuery", S_OK, {self, heap}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_end_query(
    ID3D12GraphicsCommandList *self,
    ID3D12QueryHeap *heap,
    D3D12_QUERY_TYPE type,
    UINT index)
{
  const auto hook = command_list_hook_for(self);
  if (hook.end_query) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    hook.end_query(self, heap, type, index);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"query_heap_object_id\":" << object_id_json(lookup_object_id_locked(heap))
          << ",\"type\":" << static_cast<unsigned int>(type)
          << ",\"index\":" << index << "}";
  record_call_locked("ID3D12GraphicsCommandList::EndQuery", S_OK, {self, heap}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_resolve_query_data(
    ID3D12GraphicsCommandList *self,
    ID3D12QueryHeap *heap,
    D3D12_QUERY_TYPE type,
    UINT start_index,
    UINT query_count,
    ID3D12Resource *dst_buffer,
    UINT64 aligned_dst_buffer_offset)
{
  const auto hook = command_list_hook_for(self);
  if (hook.resolve_query_data) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(dst_buffer);
    hook.resolve_query_data(self, heap, type, start_index, query_count, dst_buffer, aligned_dst_buffer_offset);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resource_locked(self, dst_buffer);
  std::ostringstream payload;
  payload << "{\"query_heap_object_id\":" << object_id_json(lookup_object_id_locked(heap))
          << ",\"type\":" << static_cast<unsigned int>(type)
          << ",\"start_index\":" << start_index
          << ",\"query_count\":" << query_count
          << ",\"dst_buffer_object_id\":" << object_id_json(lookup_object_id_locked(dst_buffer))
          << ",\"aligned_dst_buffer_offset\":" << aligned_dst_buffer_offset << "}";
  record_call_locked("ID3D12GraphicsCommandList::ResolveQueryData", S_OK, {self, heap, dst_buffer}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_set_predication(
    ID3D12GraphicsCommandList *self,
    ID3D12Resource *buffer,
    UINT64 aligned_buffer_offset,
    D3D12_PREDICATION_OP operation)
{
  const auto hook = command_list_hook_for(self);
  if (hook.set_predication) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList, ID3D12GraphicsCommandListVtbl> original_vtable(self, hook.vtable);
    ScopedResourceOriginalVTable original_resource_vtable(buffer);
    hook.set_predication(self, buffer, aligned_buffer_offset, operation);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resource_locked(self, buffer);
  std::ostringstream payload;
  payload << "{\"buffer_object_id\":" << object_id_json(lookup_object_id_locked(buffer))
          << ",\"aligned_buffer_offset\":" << aligned_buffer_offset
          << ",\"operation\":" << static_cast<unsigned int>(operation) << "}";
  record_call_locked("ID3D12GraphicsCommandList::SetPredication", S_OK, {self, buffer}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_resolve_subresource_region(
    ID3D12GraphicsCommandList1 *self,
    ID3D12Resource *dst,
    UINT dst_subresource,
    UINT dst_x,
    UINT dst_y,
    ID3D12Resource *src,
    UINT src_subresource,
    D3D12_RECT *src_rect,
    DXGI_FORMAT format,
    D3D12_RESOLVE_MODE mode)
{
  const auto hook = command_list1_hook_for(self);
  std::vector<ID3D12Resource *> referenced_resources = {dst, src};
  if (hook.resolve_subresource_region) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList1, ID3D12GraphicsCommandList1Vtbl> original_vtable(self, hook.vtable);
    ScopedResourceArrayOriginalVTables original_resource_vtables(referenced_resources);
    hook.resolve_subresource_region(
        self,
        dst,
        dst_subresource,
        dst_x,
        dst_y,
        src,
        src_subresource,
        src_rect,
        format,
        mode);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  remember_command_list_resources_locked(reinterpret_cast<ID3D12GraphicsCommandList *>(self), referenced_resources);
  std::ostringstream payload;
  payload << "{\"dst_resource_object_id\":" << object_id_json(lookup_object_id_locked(dst))
          << ",\"dst_subresource\":" << dst_subresource
          << ",\"dst_x\":" << dst_x
          << ",\"dst_y\":" << dst_y
          << ",\"src_resource_object_id\":" << object_id_json(lookup_object_id_locked(src))
          << ",\"src_subresource\":" << src_subresource
          << ",\"src_rect\":";
  if (src_rect) {
    payload << "{\"left\":" << src_rect->left
            << ",\"top\":" << src_rect->top
            << ",\"right\":" << src_rect->right
            << ",\"bottom\":" << src_rect->bottom << "}";
  } else {
    payload << "null";
  }
  payload << ",\"format\":" << static_cast<unsigned int>(format)
          << ",\"mode\":" << static_cast<unsigned int>(mode) << "}";
  record_call_locked(
      "ID3D12GraphicsCommandList::ResolveSubresourceRegion",
      S_OK,
      {self, dst, src},
      {},
      payload.str());
}

HRESULT STDMETHODCALLTYPE hook_resource_map(
    ID3D12Resource *self,
    UINT subresource,
    const D3D12_RANGE *read_range,
    void **data)
{
  const auto hook = resource_hook_for(self);
  if (!hook.map) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Resource, ID3D12ResourceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.map(self, subresource, read_range, data);

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (SUCCEEDED(hr) && data && *data) {
    capture_state().mapped_resources[self] = MappedResourceState{*data, subresource};
  }
  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource;
  if (read_range) {
    payload << ",\"read_begin\":" << static_cast<std::uint64_t>(read_range->Begin)
            << ",\"read_end\":" << static_cast<std::uint64_t>(read_range->End);
  } else {
    payload << ",\"read_range\":null";
  }
  payload << ",\"mapped\":" << ((SUCCEEDED(hr) && data && *data) ? "true" : "false") << "}";
  record_call_locked("ID3D12Resource::Map", hr, {self}, {}, payload.str());
  return hr;
}

void STDMETHODCALLTYPE hook_resource_unmap(
    ID3D12Resource *self,
    UINT subresource,
    const D3D12_RANGE *written_range)
{
  const auto hook = resource_hook_for(self);

  void *mapped_data = nullptr;
  {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto it = capture_state().mapped_resources.find(self);
    if (it != capture_state().mapped_resources.end() && it->second.subresource == subresource) {
      mapped_data = it->second.data;
    }
  }

  std::vector<apitrace::trace::BlobId> blob_refs;
  std::string buffer_path;
  const bool has_written_range = written_range && written_range->End > written_range->Begin;
  if (mapped_data && has_written_range) {
    const auto begin = static_cast<std::size_t>(written_range->Begin);
    const auto size = static_cast<std::size_t>(written_range->End - written_range->Begin);
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    const auto *bytes = static_cast<const std::uint8_t *>(mapped_data) + begin;
    const auto asset = register_asset_bytes_locked(apitrace::trace::AssetKind::Buffer, "d3d12-mapped-resource", bytes, size);
    blob_refs.push_back(asset.blob_id);
    buffer_path = asset.relative_path.generic_string();
  }

  if (hook.unmap) {
    ScopedOriginalVTable<ID3D12Resource, ID3D12ResourceVtbl> original_vtable(self, hook.vtable);
    hook.unmap(self, subresource, written_range);
  }

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  capture_state().mapped_resources.erase(self);
  std::ostringstream payload;
  payload << "{\"subresource\":" << subresource;
  if (written_range) {
    payload << ",\"written_begin\":" << static_cast<std::uint64_t>(written_range->Begin)
            << ",\"written_end\":" << static_cast<std::uint64_t>(written_range->End);
  } else {
    payload << ",\"written_range\":null";
  }
  if (!buffer_path.empty()) {
    payload << ",\"buffer_path\":\"" << buffer_path << "\"";
  }
  payload << "}";
  record_call_locked("ID3D12Resource::Unmap", S_OK, {self}, blob_refs, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_dispatch_rays(
    ID3D12GraphicsCommandList4 *self,
    const D3D12_DISPATCH_RAYS_DESC *desc)
{
  const auto hook = command_list4_hook_for(self);
  if (hook.dispatch_rays) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList4, ID3D12GraphicsCommandList4Vtbl> original_vtable(self, hook.vtable);
    hook.dispatch_rays(self, desc);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"width\":" << (desc ? desc->Width : 0)
          << ",\"height\":" << (desc ? desc->Height : 0)
          << ",\"depth\":" << (desc ? desc->Depth : 0);
  if (desc) {
    const auto ray_generation_resolve =
        resolve_gpu_virtual_address_locked(desc->RayGenerationShaderRecord.StartAddress);
    const auto miss_resolve =
        resolve_gpu_virtual_address_locked(desc->MissShaderTable.StartAddress);
    const auto hit_resolve =
        resolve_gpu_virtual_address_locked(desc->HitGroupTable.StartAddress);
    const auto callable_resolve =
        resolve_gpu_virtual_address_locked(desc->CallableShaderTable.StartAddress);
    std::vector<apitrace::trace::ObjectId> extra_refs;
    append_gpuva_object_ref(extra_refs, ray_generation_resolve);
    append_gpuva_object_ref(extra_refs, miss_resolve);
    append_gpuva_object_ref(extra_refs, hit_resolve);
    append_gpuva_object_ref(extra_refs, callable_resolve);
    payload << ",\"ray_generation_shader_record\":{"
            << "\"start_address\":" << desc->RayGenerationShaderRecord.StartAddress;
    append_gpu_virtual_address_resolve_json(payload, ray_generation_resolve);
    payload << ",\"size_in_bytes\":" << desc->RayGenerationShaderRecord.SizeInBytes
            << "},\"miss_shader_table\":{"
            << "\"start_address\":" << desc->MissShaderTable.StartAddress;
    append_gpu_virtual_address_resolve_json(payload, miss_resolve);
    payload << ",\"size_in_bytes\":" << desc->MissShaderTable.SizeInBytes
            << ",\"stride_in_bytes\":" << desc->MissShaderTable.StrideInBytes
            << "},\"hit_group_table\":{"
            << "\"start_address\":" << desc->HitGroupTable.StartAddress;
    append_gpu_virtual_address_resolve_json(payload, hit_resolve);
    payload << ",\"size_in_bytes\":" << desc->HitGroupTable.SizeInBytes
            << ",\"stride_in_bytes\":" << desc->HitGroupTable.StrideInBytes
            << "},\"callable_shader_table\":{"
            << "\"start_address\":" << desc->CallableShaderTable.StartAddress;
    append_gpu_virtual_address_resolve_json(payload, callable_resolve);
    payload << ",\"size_in_bytes\":" << desc->CallableShaderTable.SizeInBytes
            << ",\"stride_in_bytes\":" << desc->CallableShaderTable.StrideInBytes
            << "}";
    payload << "}";
    record_call_locked("ID3D12GraphicsCommandList4::DispatchRays", S_OK, {self}, {}, payload.str(), extra_refs);
    return;
  }
  payload << "}";
  record_call_locked("ID3D12GraphicsCommandList4::DispatchRays", S_OK, {self}, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_write_buffer_immediate(
    ID3D12GraphicsCommandList2 *self,
    UINT count,
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
    const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes)
{
  const auto hook = command_list2_hook_for(self);
  if (hook.write_buffer_immediate) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList2, ID3D12GraphicsCommandList2Vtbl> original_vtable(self, hook.vtable);
    hook.write_buffer_immediate(self, count, parameters, modes);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  std::vector<apitrace::trace::ObjectId> extra_refs;
  payload << "{\"count\":" << count << ",\"writes\":[";
  for (UINT index = 0; parameters && index < count; ++index) {
    if (index) {
      payload << ",";
    }
    const auto dest = static_cast<std::uint64_t>(parameters[index].Dest);
    const auto resolve = resolve_gpu_virtual_address_locked(dest);
    append_gpuva_object_ref(extra_refs, resolve);
    payload << "{\"dest\":" << dest;
    append_gpu_virtual_address_resolve_json(payload, resolve);
    payload << ",\"value\":" << parameters[index].Value
            << ",\"mode\":";
    if (modes) {
      payload << static_cast<unsigned int>(modes[index]);
    } else {
      payload << "null";
    }
    payload << "}";
  }
  payload << "]}";
  record_call_locked("ID3D12GraphicsCommandList2::WriteBufferImmediate", S_OK, {self}, {}, payload.str(), extra_refs);
}

void STDMETHODCALLTYPE hook_command_list_begin_render_pass(
    ID3D12GraphicsCommandList4 *self,
    UINT render_target_count,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
    D3D12_RENDER_PASS_FLAGS flags)
{
  const auto hook = command_list4_hook_for(self);
  if (hook.begin_render_pass) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList4, ID3D12GraphicsCommandList4Vtbl> original_vtable(self, hook.vtable);
    hook.begin_render_pass(self, render_target_count, render_targets, depth_stencil, flags);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::vector<ID3D12Resource *> refs;
  std::ostringstream payload;
  payload << "{\"render_targets_count\":" << render_target_count
          << ",\"render_targets\":[";
  for (UINT index = 0; render_targets && index < render_target_count; ++index) {
    if (index != 0) {
      payload << ",";
    }
    append_render_pass_render_target_json_locked(payload, render_targets[index], refs);
  }
  payload << "],\"has_depth_stencil\":" << (depth_stencil ? "true" : "false")
          << ",\"depth_stencil\":";
  if (depth_stencil) {
    append_render_pass_depth_stencil_json_locked(payload, *depth_stencil, refs);
  } else {
    payload << "null";
  }
  payload << ",\"flags\":" << static_cast<unsigned int>(flags) << "}";
  remember_command_list_resources_locked(reinterpret_cast<ID3D12GraphicsCommandList *>(self), refs);
  std::vector<const void *> record_refs = {self};
  record_refs.insert(record_refs.end(), refs.begin(), refs.end());
  record_call_locked("ID3D12GraphicsCommandList4::BeginRenderPass", S_OK, record_refs, {}, payload.str());
}

void STDMETHODCALLTYPE hook_command_list_end_render_pass(ID3D12GraphicsCommandList4 *self)
{
  const auto hook = command_list4_hook_for(self);
  if (hook.end_render_pass) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList4, ID3D12GraphicsCommandList4Vtbl> original_vtable(self, hook.vtable);
    hook.end_render_pass(self);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  record_call_locked("ID3D12GraphicsCommandList4::EndRenderPass", S_OK, {self}, {}, "{}");
}

void STDMETHODCALLTYPE hook_command_list_dispatch_mesh(
    ID3D12GraphicsCommandList6 *self,
    UINT thread_group_count_x,
    UINT thread_group_count_y,
    UINT thread_group_count_z)
{
  const auto hook = command_list6_hook_for(self);
  if (hook.dispatch_mesh) {
    ScopedOriginalVTable<ID3D12GraphicsCommandList6, ID3D12GraphicsCommandList6Vtbl> original_vtable(self, hook.vtable);
    hook.dispatch_mesh(self, thread_group_count_x, thread_group_count_y, thread_group_count_z);
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"thread_group_count_x\":" << thread_group_count_x
          << ",\"thread_group_count_y\":" << thread_group_count_y
          << ",\"thread_group_count_z\":" << thread_group_count_z << "}";
  record_call_locked("ID3D12GraphicsCommandList6::DispatchMesh", S_OK, {self}, {}, payload.str());
}

HRESULT STDMETHODCALLTYPE hook_fence_set_event_on_completion(ID3D12Fence *self, UINT64 value, HANDLE event)
{
  const auto hook = fence_hook_for(self);
  if (!hook.set_event_on_completion) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.set_event_on_completion(self, value, event);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"fence_value\":" << value << "}";
  record_call_locked("ID3D12Fence::SetEventOnCompletion", hr, {self}, {}, payload.str());
  return hr;
}

HRESULT STDMETHODCALLTYPE hook_fence_signal(ID3D12Fence *self, UINT64 value)
{
  const auto hook = fence_hook_for(self);
  if (!hook.signal) {
    return E_FAIL;
  }
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_vtable(self, hook.vtable);
  const HRESULT hr = hook.signal(self, value);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"fence_value\":" << value << "}";
  record_call_locked("ID3D12Fence::Signal", hr, {self}, {}, payload.str());
  return hr;
}

UINT64 STDMETHODCALLTYPE hook_fence_get_completed_value(ID3D12Fence *self)
{
  const auto hook = fence_hook_for(self);
  if (!hook.get_completed_value) {
    return 0;
  }
  ScopedOriginalVTable<ID3D12Fence, ID3D12FenceVtbl> original_vtable(self, hook.vtable);
  const UINT64 completed_value = hook.get_completed_value(self);
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  std::ostringstream payload;
  payload << "{\"completed_value\":" << completed_value << "}";
  record_call_locked("ID3D12Fence::GetCompletedValue", S_OK, {self}, {}, payload.str());
  return completed_value;
}

void process_attach()
{
}

void process_detach() noexcept
{
  apitrace::runtime::shutdown_process_trace_session();
}
