extern "C" HRESULT WINAPI D3D12CreateDevice(
    IUnknown *adapter,
    D3D_FEATURE_LEVEL minimum_feature_level,
    REFIID iid,
    void **device)
{
  proxy_debug_logf(
      "D3D12CreateDevice enter adapter=%p feature=0x%08x iid=%s",
      adapter,
      static_cast<unsigned int>(minimum_feature_level),
      guid_string(iid).c_str());
  auto &downstream = downstream_module();
  if (!downstream.create_device) {
    proxy_debug_log("D3D12CreateDevice missing downstream export");
    return E_FAIL;
  }

  const HRESULT hr = downstream.create_device(adapter, minimum_feature_level, iid, device);
  proxy_debug_logf("D3D12CreateDevice hr=0x%08lx device=%p", static_cast<unsigned long>(hr), device ? *device : nullptr);
  if (proxy_hooks_disabled()) {
    proxy_debug_log("D3D12CreateDevice hook disabled");
    return hr;
  }
  if (SUCCEEDED(hr) && device && *device) {
    {
      std::lock_guard<std::mutex> lock(capture_state().mutex);
      register_object_locked(*device, apitrace::trace::ObjectKind::Device, "ID3D12Device");
      std::ostringstream payload;
      payload << "{\"minimum_feature_level\":" << static_cast<unsigned int>(minimum_feature_level) << "}";
      record_call_locked("D3D12CreateDevice", hr, {*device}, {}, payload.str());
    }
    auto *d3d_device = static_cast<ID3D12Device *>(*device);
    const auto minimum_vtable_size = is_device_iid(iid) ? device_vtable_size_for_iid(iid) : sizeof(ID3D12DeviceVtbl);
    patch_device(d3d_device, supported_device_vtable_size(d3d_device, minimum_vtable_size));
  } else {
    std::lock_guard<std::mutex> lock(capture_state().mutex);
    std::ostringstream payload;
    payload << "{\"minimum_feature_level\":" << static_cast<unsigned int>(minimum_feature_level) << "}";
    record_call_locked("D3D12CreateDevice", hr, {}, {}, payload.str());
  }
  return hr;
}

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID iid, void **debug)
{
  proxy_debug_logf("D3D12GetDebugInterface enter iid=%s", guid_string(iid).c_str());
  auto &downstream = downstream_module();
  if (!downstream.get_debug_interface) {
    proxy_debug_log("D3D12GetDebugInterface missing downstream export");
    return E_FAIL;
  }
  const HRESULT hr = downstream.get_debug_interface(iid, debug);
  proxy_debug_logf("D3D12GetDebugInterface hr=0x%08lx debug=%p", static_cast<unsigned long>(hr), debug ? *debug : nullptr);
  return hr;
}

extern "C" void WINAPI apitrace_d3d12_record_runtime_objects(
    ID3D12Device *device,
    ID3D12CommandQueue *queue,
    ID3D12CommandAllocator *allocator,
    ID3D12GraphicsCommandList *command_list,
    ID3D12Fence *fence)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto device_id = register_object_locked(device, apitrace::trace::ObjectKind::Device, "ID3D12Device");

  if (queue) {
    const bool queue_known = lookup_object_id_locked(queue) != 0;
    if (!queue_known) {
      register_object_locked(queue, apitrace::trace::ObjectKind::CommandQueue, "ID3D12CommandQueue", device_id);
      remember_bridge_command_object_locked(queue);
      record_call_locked(
          "ID3D12Device::CreateCommandQueue",
          S_OK,
          {device, queue},
          {},
          "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}");
    }
  }
  if (allocator) {
    const bool allocator_known = lookup_object_id_locked(allocator) != 0;
    if (!allocator_known) {
      register_object_locked(allocator, apitrace::trace::ObjectKind::CommandAllocator, "ID3D12CommandAllocator", device_id);
      record_call_locked("ID3D12Device::CreateCommandAllocator", S_OK, {device, allocator}, {}, "{\"type\":0}");
    }
  }
  if (command_list && allocator) {
    const bool command_list_known = lookup_object_id_locked(command_list) != 0;
    if (!command_list_known) {
      register_object_locked(command_list, apitrace::trace::ObjectKind::CommandList, "ID3D12GraphicsCommandList", device_id);
      remember_bridge_command_object_locked(command_list);
      record_call_locked("ID3D12Device::CreateCommandList", S_OK, {device, allocator, nullptr, command_list}, {}, "{\"node_mask\":0,\"type\":0}");
      record_call_locked("ID3D12GraphicsCommandList::Close", S_OK, {command_list}, {}, "{}");
    }
  }
  if (fence) {
    const bool fence_known = lookup_object_id_locked(fence) != 0;
    if (!fence_known) {
      register_object_locked(fence, apitrace::trace::ObjectKind::Fence, "ID3D12Fence", device_id);
      record_call_locked("ID3D12Device::CreateFence", S_OK, {device, fence}, {}, "{\"initial_value\":0,\"flags\":0}");
    }
  }
}

extern "C" void WINAPI apitrace_d3d12_record_execute_command_lists(
    ID3D12CommandQueue *queue,
    ID3D12GraphicsCommandList *command_list)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (!is_bridge_command_object_locked(queue) && !is_bridge_command_object_locked(command_list)) {
    return;
  }
  record_call_locked("ID3D12CommandQueue::ExecuteCommandLists", S_OK, {queue, command_list}, {}, "{\"command_list_count\":1}");
  record_boundary_locked(apitrace::trace::BoundaryKind::Submit, "{\"queue\":\"ID3D12CommandQueue\"}");
}

extern "C" void WINAPI apitrace_d3d12_record_descriptor_heap(
    ID3D12Device *device,
    ID3D12DescriptorHeap *descriptor_heap,
    const D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
  if (!descriptor_heap) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (lookup_object_id_locked(descriptor_heap) != 0) {
    return;
  }
  const auto parent = lookup_object_id_locked(device);
  register_object_locked(descriptor_heap, apitrace::trace::ObjectKind::DescriptorHeap, "ID3D12DescriptorHeap", parent);
  std::ostringstream payload;
  payload << "{"
          << "\"type\":" << (desc ? static_cast<unsigned int>(desc->Type) : 0) << ","
          << "\"num_descriptors\":" << (desc ? desc->NumDescriptors : 0) << ","
          << "\"flags\":" << (desc ? static_cast<unsigned int>(desc->Flags) : 0) << ","
          << "\"node_mask\":" << (desc ? desc->NodeMask : 0) << ","
          << "\"descriptor_size\":"
          << (desc ? descriptor_handle_increment_size(device, desc->Type) : 0) << ","
          << descriptor_heap_handle_payload(descriptor_heap)
          << "}";
  record_call_locked("ID3D12Device::CreateDescriptorHeap", S_OK, {device, descriptor_heap}, {}, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_record_swapchain_back_buffer(
    ID3D12Device *device,
    IDXGISwapChain *swap_chain,
    ID3D12Resource *back_buffer,
    UINT buffer_index,
    const D3D12_RESOURCE_DESC *desc)
{
  if (!back_buffer) {
    return;
  }

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto device_id = register_object_locked(device, apitrace::trace::ObjectKind::Device, "ID3D12Device");
  const auto swap_chain_id = register_object_locked(swap_chain, apitrace::trace::ObjectKind::SwapChain, "IDXGISwapChain", device_id);
  register_fresh_object_locked(back_buffer, apitrace::trace::ObjectKind::Resource, "IDXGISwapChainBackBuffer", swap_chain_id);
  std::ostringstream payload;
  payload << "{\"heap_type\":" << static_cast<unsigned int>(D3D12_HEAP_TYPE_DEFAULT)
          << ",\"heap_flags\":" << static_cast<unsigned int>(D3D12_HEAP_FLAG_NONE)
          << ",\"initial_state\":" << static_cast<unsigned int>(D3D12_RESOURCE_STATE_PRESENT)
          << ",\"gpu_virtual_address\":0"
          << ",\"swapchain_back_buffer\":true"
          << ",\"buffer_index\":" << buffer_index
          << ",\"resource_desc\":" << resource_desc_json(desc)
          << ",\"optimized_clear_value\":null"
          << "}";
  record_call_locked("ID3D12Device::CreateCommittedResource", S_OK, {device, back_buffer}, {}, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_record_resource_barrier(
    ID3D12GraphicsCommandList *command_list,
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after,
    UINT subresource)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (!is_bridge_command_object_locked(command_list)) {
    return;
  }
  std::ostringstream payload;
  payload << "{\"barrier_count\":1,\"barriers\":[{"
          << "\"type\":" << static_cast<unsigned int>(D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) << ","
          << "\"flags\":0,"
          << "\"resource_object_id\":" << object_id_json(lookup_object_id_locked(resource)) << ","
          << "\"before\":" << static_cast<unsigned int>(before) << ","
          << "\"after\":" << static_cast<unsigned int>(after) << ","
          << "\"subresource\":" << subresource
          << "}]}";
  record_call_locked("ID3D12GraphicsCommandList::ResourceBarrier", S_OK, {command_list, resource}, {}, payload.str());
  record_boundary_locked(apitrace::trace::BoundaryKind::Barrier, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_record_present_frame(
    UINT64 frame_index,
    UINT width,
    UINT height,
    UINT row_pitch,
    UINT sync_interval,
    UINT flags,
    const void *rgba_data,
    SIZE_T rgba_size)
{
  if (!rgba_data || width == 0 || height == 0 || row_pitch == 0 || rgba_size == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto asset = register_asset_bytes_locked(
      apitrace::trace::AssetKind::Texture,
      "d3d12-present-frame",
      rgba_data,
      static_cast<std::size_t>(rgba_size));
  if (asset.blob_id == 0 || asset.relative_path.empty()) {
    return;
  }

  std::ostringstream payload;
  payload << "{"
          << "\"frame_index\":" << frame_index << ","
          << "\"width\":" << width << ","
          << "\"height\":" << height << ","
          << "\"row_pitch\":" << row_pitch << ","
          << "\"sync_interval\":" << sync_interval << ","
          << "\"flags\":" << flags << ","
          << "\"format\":\"rgba8\","
          << "\"frame_path\":\"" << asset.relative_path.generic_string() << "\""
          << "}";
  record_resource_blob_locked("D3D12PresentFrame", {asset.blob_id}, payload.str());
}

extern "C" void WINAPI apitrace_d3d12_begin_capture_suppression()
{
  ++g_capture_suppression_depth;
}

extern "C" void WINAPI apitrace_d3d12_end_capture_suppression()
{
  if (g_capture_suppression_depth != 0) {
    --g_capture_suppression_depth;
  }
}

extern "C" void WINAPI apitrace_d3d12_record_present_semantics(
    UINT sync_interval,
    UINT flags,
    HRESULT result)
{
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  const auto present_test = (flags & DXGI_PRESENT_TEST) != 0;
  if (present_test) {
    std::ostringstream call_payload;
    call_payload << "{\"sync_interval\":" << sync_interval
                 << ",\"flags\":" << flags
                 << ",\"present_test\":true}";
    record_call_locked("IDXGISwapChain::Present", result, {}, {}, call_payload.str());
    return;
  }
  ensure_frame_begin_locked();
  const auto frame_index = capture_state().frame_index++;
  capture_state().last_present_frame_index = frame_index;
  capture_state().has_last_present_frame = true;
  std::ostringstream call_payload;
  call_payload << "{\"sync_interval\":" << sync_interval
               << ",\"flags\":" << flags
               << ",\"frame_index\":" << frame_index << "}";
  record_call_locked("IDXGISwapChain::Present", result, {}, {}, call_payload.str());

  std::ostringstream boundary_payload;
  boundary_payload << "{\"label\":\"Present\",\"frame_index\":" << frame_index
                   << ",\"sync_interval\":" << sync_interval
                   << ",\"flags\":" << flags << "}";
  record_boundary_locked(apitrace::trace::BoundaryKind::Present, boundary_payload.str());

  std::ostringstream frame_end_payload;
  frame_end_payload << "{\"label\":\"FrameEnd\",\"frame_index\":" << frame_index << "}";
  record_boundary_locked(apitrace::trace::BoundaryKind::Frame, frame_end_payload.str());
  capture_state().frame_begin_pending = true;
}

extern "C" void WINAPI apitrace_d3d12_record_graphics_pipeline(
    ID3D12Device *device,
    ID3D12PipelineState *pipeline_state,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
  if (!pipeline_state || !desc) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (lookup_object_id_locked(pipeline_state) != 0) {
    return;
  }
  const auto parent = lookup_object_id_locked(device);
  register_object_locked(pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
  std::vector<apitrace::trace::BlobId> shader_blob_refs;
  const auto payload = graphics_pipeline_asset_json_locked(desc, shader_blob_refs);
  std::vector<apitrace::trace::BlobId> blob_refs;
  blob_refs.insert(blob_refs.end(), shader_blob_refs.begin(), shader_blob_refs.end());
  record_call_locked(
      "ID3D12Device::CreateGraphicsPipelineState",
      S_OK,
      {device, desc->pRootSignature, pipeline_state},
      blob_refs,
      payload);
}

extern "C" void WINAPI apitrace_d3d12_record_compute_pipeline(
    ID3D12Device *device,
    ID3D12PipelineState *pipeline_state,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc)
{
  if (!pipeline_state || !desc) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture_state().mutex);
  if (lookup_object_id_locked(pipeline_state) != 0) {
    return;
  }
  const auto parent = lookup_object_id_locked(device);
  register_object_locked(pipeline_state, apitrace::trace::ObjectKind::PipelineState, "ID3D12PipelineState", parent);
  std::vector<apitrace::trace::BlobId> shader_blob_refs;
  const auto payload = compute_pipeline_asset_json_locked(desc, shader_blob_refs);
  std::vector<apitrace::trace::BlobId> blob_refs;
  blob_refs.insert(blob_refs.end(), shader_blob_refs.begin(), shader_blob_refs.end());
  record_call_locked(
      "ID3D12Device::CreateComputePipelineState",
      S_OK,
      {device, desc->pRootSignature, pipeline_state},
      blob_refs,
      payload);
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC *root_signature_desc,
    D3D_ROOT_SIGNATURE_VERSION version,
    ID3DBlob **blob,
    ID3DBlob **error_blob)
{
  proxy_debug_logf("D3D12SerializeRootSignature enter version=%u", static_cast<unsigned int>(version));
  auto &downstream = downstream_module();
  if (!downstream.serialize_root_signature) {
    proxy_debug_log("D3D12SerializeRootSignature missing downstream export");
    return E_FAIL;
  }
  const HRESULT hr = downstream.serialize_root_signature(root_signature_desc, version, blob, error_blob);
  proxy_debug_logf("D3D12SerializeRootSignature hr=0x%08lx blob=%p", static_cast<unsigned long>(hr), blob ? *blob : nullptr);
  return hr;
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature_desc,
    ID3DBlob **blob,
    ID3DBlob **error_blob)
{
  proxy_debug_log("D3D12SerializeVersionedRootSignature enter");
  auto &downstream = downstream_module();
  if (!downstream.serialize_versioned_root_signature) {
    proxy_debug_log("D3D12SerializeVersionedRootSignature missing downstream export");
    return E_FAIL;
  }
  const HRESULT hr = downstream.serialize_versioned_root_signature(root_signature_desc, blob, error_blob);
  proxy_debug_logf(
      "D3D12SerializeVersionedRootSignature hr=0x%08lx blob=%p",
      static_cast<unsigned long>(hr),
      blob ? *blob : nullptr);
  return hr;
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    const void *data,
    SIZE_T data_size,
    REFIID iid,
    void **deserializer)
{
  proxy_debug_logf("D3D12CreateRootSignatureDeserializer enter size=%zu iid=%s", static_cast<std::size_t>(data_size), guid_string(iid).c_str());
  auto &downstream = downstream_module();
  if (!downstream.create_root_signature_deserializer) {
    proxy_debug_log("D3D12CreateRootSignatureDeserializer missing downstream export");
    return E_FAIL;
  }
  const HRESULT hr = downstream.create_root_signature_deserializer(data, data_size, iid, deserializer);
  proxy_debug_logf(
      "D3D12CreateRootSignatureDeserializer hr=0x%08lx deserializer=%p",
      static_cast<unsigned long>(hr),
      deserializer ? *deserializer : nullptr);
  return hr;
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    const void *data,
    SIZE_T data_size,
    REFIID iid,
    void **deserializer)
{
  proxy_debug_logf("D3D12CreateVersionedRootSignatureDeserializer enter size=%zu iid=%s", static_cast<std::size_t>(data_size), guid_string(iid).c_str());
  auto &downstream = downstream_module();
  if (!downstream.create_versioned_root_signature_deserializer) {
    proxy_debug_log("D3D12CreateVersionedRootSignatureDeserializer missing downstream export");
    return E_FAIL;
  }
  const HRESULT hr = downstream.create_versioned_root_signature_deserializer(data, data_size, iid, deserializer);
  proxy_debug_logf(
      "D3D12CreateVersionedRootSignatureDeserializer hr=0x%08lx deserializer=%p",
      static_cast<unsigned long>(hr),
      deserializer ? *deserializer : nullptr);
  return hr;
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT feature_count,
    const IID *iids,
    void *configurations,
    UINT *configurations_sizes)
{
  proxy_debug_logf("D3D12EnableExperimentalFeatures enter count=%u", feature_count);
  auto &downstream = downstream_module();
  if (!downstream.enable_experimental_features) {
    proxy_debug_log("D3D12EnableExperimentalFeatures missing downstream export");
    return E_FAIL;
  }
  const HRESULT hr = downstream.enable_experimental_features(feature_count, iids, configurations, configurations_sizes);
  proxy_debug_logf("D3D12EnableExperimentalFeatures hr=0x%08lx", static_cast<unsigned long>(hr));
  return hr;
}

extern "C" DWORD WINAPI GetBehaviorValue()
{
  return 0;
}

extern "C" HRESULT WINAPI D3D12CoreCreateLayeredDevice()
{
  return E_NOTIMPL;
}

extern "C" SIZE_T WINAPI D3D12CoreGetLayeredDeviceSize()
{
  return 0;
}

extern "C" HRESULT WINAPI D3D12CoreRegisterLayers()
{
  return E_NOTIMPL;
}

extern "C" void WINAPI apitrace_bootstrap_d3d12()
{
  proxy_debug_log("apitrace_bootstrap_d3d12");
  process_attach();
}

extern "C" void WINAPI apitrace_d3d12_emit_scene_marker(
    const char *scene_name,
    const char *dx_mode,
    const char *phase)
{
  record_scene_marker(scene_name, dx_mode, phase);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
  (void)instance;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    proxy_debug_log("D3D12 proxy process attach");
    process_attach();
  } else if (reason == DLL_PROCESS_DETACH) {
    proxy_debug_log("D3D12 proxy process detach");
    process_detach();
  }
  return TRUE;
}
