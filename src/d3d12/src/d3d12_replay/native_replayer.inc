#if defined(APITRACE_HAS_D3D_NATIVE)

std::vector<D3D12_RECT> d3d12_rects_from_payload(const json &payload)
{
  std::vector<D3D12_RECT> rects;
  const auto rects_json = payload.find("rects");
  if (rects_json == payload.end() || !rects_json->is_array()) {
    return rects;
  }
  for (const auto &rect_json : *rects_json) {
    D3D12_RECT rect{};
    rect.left = payload_i32(rect_json, "left");
    rect.top = payload_i32(rect_json, "top");
    rect.right = payload_i32(rect_json, "right");
    rect.bottom = payload_i32(rect_json, "bottom");
    rects.push_back(rect);
  }
  return rects;
}

using RecordPresentFrameFn = void(WINAPI *)(UINT64, UINT, UINT, UINT, UINT, UINT, const void *, SIZE_T);
using RecordPresentSemanticsFn = void(WINAPI *)(UINT, UINT, HRESULT);
using CaptureSuppressionFn = void(WINAPI *)();

#ifdef _WIN32
template <typename Fn>
Fn resolve_d3d12_export(const char *name)
{
  HMODULE module = GetModuleHandleA("d3d12.dll");
  if (!module) {
    return nullptr;
  }
  return reinterpret_cast<Fn>(GetProcAddress(module, name));
}

class ScopedD3D12CaptureSuppression {
public:
  ScopedD3D12CaptureSuppression()
      : begin_(resolve_d3d12_export<CaptureSuppressionFn>("apitrace_d3d12_begin_capture_suppression")),
        end_(resolve_d3d12_export<CaptureSuppressionFn>("apitrace_d3d12_end_capture_suppression"))
  {
    if (begin_ && end_) {
      begin_();
      active_ = true;
    }
  }

  ScopedD3D12CaptureSuppression(const ScopedD3D12CaptureSuppression &) = delete;
  ScopedD3D12CaptureSuppression &operator=(const ScopedD3D12CaptureSuppression &) = delete;

  ~ScopedD3D12CaptureSuppression()
  {
    if (active_) {
      end_();
    }
  }

private:
  CaptureSuppressionFn begin_ = nullptr;
  CaptureSuppressionFn end_ = nullptr;
  bool active_ = false;
};
#else
template <typename Fn>
Fn resolve_d3d12_export(const char *)
{
  return nullptr;
}

class ScopedD3D12CaptureSuppression {
public:
  ScopedD3D12CaptureSuppression() = default;
};
#endif

class D3D12NativeReplayer {
public:
  explicit D3D12NativeReplayer(const D3D12ReplayBackend &backend) : backend_(backend) {}

  ~D3D12NativeReplayer()
  {
    shutdown();
  }

  bool replay(std::string &error)
  {
    if (!d3d12_native_replay_enabled()) {
      error = "D3D12 native command replay incomplete";
      return false;
    }
    if (!bootstrap(error) ||
        !create_heaps(error) ||
        !create_resources(error) ||
        !create_descriptor_heaps(error) ||
        !create_query_heaps(error) ||
        !create_root_signatures(error) ||
        !create_pipelines(error) ||
        !create_command_signatures(error) ||
        !prepare_chronological_state(error) ||
        !create_command_objects(error) ||
        !replay_submissions(error)) {
      return false;
    }
    wait_for_gpu();
    return true;
  }

private:
  struct NativeResource {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool swapchain_back_buffer = false;
  };

  struct NativeCommandList {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12GraphicsCommandList1> list1;
    ComPtr<ID3D12GraphicsCommandList2> list2;
#if defined(__ID3D12GraphicsCommandList6_INTERFACE_DEFINED__)
    ComPtr<ID3D12GraphicsCommandList6> list6;
#endif
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    std::uint64_t recorded_until_sequence = 0;
    std::vector<json> pending_render_pass_resolves;
  };

  struct ReadbackCommandObjects {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
  };

  struct NativeDescriptorHeap {
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    UINT descriptor_size = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
  };

  struct NativeHeap {
    ComPtr<ID3D12Heap> heap;
  };

  bool fail(std::string &error, std::string message) const
  {
    error = "D3D12 native command replay incomplete: " + std::move(message);
    return false;
  }

  void shutdown()
  {
    wait_for_gpu();
#ifdef _WIN32
    if (fence_event_) {
      CloseHandle(fence_event_);
      fence_event_ = nullptr;
    }
#endif
    command_lists_.clear();
    query_heaps_.clear();
    command_signatures_.clear();
    pipelines_.clear();
    root_signatures_.clear();
    descriptor_heaps_.clear();
    heaps_.clear();
    resources_.clear();
    back_buffers_[0].reset();
    back_buffers_[1].reset();
    fence_.reset();
    swap_chain_.reset();
    queue_.reset();
    device_.reset();
#if !defined(_WIN32)
    if (capture_writer_) {
      capture_writer_->close();
      capture_writer_.reset();
    }
#endif
#ifdef _WIN32
    if (hwnd_) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
#elif defined(__APPLE__)
    hwnd_ = nullptr;
    apitrace::platform::macos::destroy_window(window_handles_);
#endif
  }

  bool infer_window_size(UINT &width, UINT &height) const
  {
    for (const auto &[object_id, resource] : backend_.resources_) {
      (void)object_id;
      if (!resource.swapchain_back_buffer) {
        continue;
      }
      width = static_cast<UINT>(resource.width);
      height = resource.height;
      return width != 0 && height != 0;
    }
    return false;
  }

  bool bootstrap(std::string &error)
  {
    UINT width = 0;
    UINT height = 0;
    if (!infer_window_size(width, height)) {
      return fail(error, "trace is missing swapchain back-buffer semantics");
    }

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateDXGIFactory1", hr);
      return false;
    }

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    for (const auto &[object_id, device] : backend_.devices_) {
      (void)object_id;
      feature_level = static_cast<D3D_FEATURE_LEVEL>(device.minimum_feature_level);
      break;
    }
    hr = D3D12CreateDevice(nullptr, feature_level, IID_PPV_ARGS(device_.put()));
    if (FAILED(hr)) {
      error = hresult_error("D3D12CreateDevice", hr);
      return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue_.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateCommandQueue", hr);
      return false;
    }

#ifdef _WIN32
    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = replay_window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "apitrace_d3d12_native_retrace_window";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return fail(error, "RegisterClassA failed");
    }

    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExA(
        0,
        wc.lpszClassName,
        "apitrace D3D12 native retrace",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd_) {
      return fail(error, "CreateWindowExA failed");
    }
    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);
#elif defined(__APPLE__)
    apitrace::platform::macos::WindowSpec spec;
    spec.width = width;
    spec.height = height;
    spec.title = "apitrace D3D12 native retrace";
    spec.show = true;
    if (!apitrace::platform::macos::create_window(spec, window_handles_, error)) {
      return fail(error, error.empty() ? "failed to create native macOS replay window" : error);
    }
    hwnd_ = static_cast<HWND>(window_handles_.nswindow);
    if (!hwnd_) {
      return fail(error, "native macOS replay window did not produce an HWND token");
    }
#else
    return fail(error, "D3D12 native replay has no window bootstrap for this platform");
#endif

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.Width = width;
    swap_chain_desc.Height = height;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = kBufferCount;
    swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swap_chain1;
    hr = factory->CreateSwapChainForHwnd(queue_.get(), hwnd_, &swap_chain_desc, nullptr, nullptr, swap_chain1.put());
    if (FAILED(hr)) {
      error = hresult_error("CreateSwapChainForHwnd", hr);
      return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    hr = swap_chain1->QueryInterface(IID_PPV_ARGS(swap_chain_.put()));
    if (FAILED(hr)) {
      error = hresult_error("QueryInterface(IDXGISwapChain3)", hr);
      return false;
    }
    for (UINT index = 0; index < kBufferCount; ++index) {
      hr = swap_chain_->GetBuffer(index, IID_PPV_ARGS(back_buffers_[index].put()));
      if (FAILED(hr)) {
        error = hresult_error("IDXGISwapChain::GetBuffer", hr);
        return false;
      }
    }

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateFence", hr);
      return false;
    }
#ifdef _WIN32
    fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
      return fail(error, "CreateEventA failed");
    }
#endif
    return true;
  }

  static D3D12_HEAP_PROPERTIES heap_properties(std::uint32_t heap_type)
  {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = static_cast<D3D12_HEAP_TYPE>(heap_type);
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
  }

  bool create_heaps(std::string &error)
  {
    for (const auto &[object_id, heap] : backend_.heaps_) {
      D3D12_HEAP_DESC desc{};
      desc.SizeInBytes = heap.size_in_bytes;
      desc.Properties = heap_properties(heap.heap_type);
      desc.Alignment = heap.alignment;
      desc.Flags = static_cast<D3D12_HEAP_FLAGS>(heap.flags);
      NativeHeap native;
      HRESULT hr = device_->CreateHeap(&desc, IID_PPV_ARGS(native.heap.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateHeap", hr);
        return false;
      }
      heaps_.emplace(object_id, std::move(native));
    }
    return true;
  }

  static D3D12_RESOURCE_DESC resource_desc(const D3D12ReplayBackend::ResourceSemanticState &resource)
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(resource.dimension);
    desc.Alignment = resource.alignment;
    desc.Width = resource.width;
    desc.Height = resource.height;
    desc.DepthOrArraySize = static_cast<UINT16>(resource.depth_or_array_size);
    desc.MipLevels = static_cast<UINT16>(resource.mip_levels);
    desc.Format = static_cast<DXGI_FORMAT>(resource.format);
    desc.SampleDesc.Count = resource.sample_count;
    desc.SampleDesc.Quality = resource.sample_quality;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(resource.layout);
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(resource.flags);
    return desc;
  }

  static D3D12_CLEAR_VALUE clear_value(const D3D12ReplayBackend::ResourceSemanticState &resource)
  {
    D3D12_CLEAR_VALUE value{};
    value.Format = static_cast<DXGI_FORMAT>(resource.optimized_clear_format);
    value.Color[0] = resource.optimized_clear_color[0];
    value.Color[1] = resource.optimized_clear_color[1];
    value.Color[2] = resource.optimized_clear_color[2];
    value.Color[3] = resource.optimized_clear_color[3];
    value.DepthStencil.Depth = resource.optimized_clear_depth;
    value.DepthStencil.Stencil = static_cast<UINT8>(resource.optimized_clear_stencil);
    return value;
  }

  bool create_resources(std::string &error)
  {
    for (const auto &[object_id, resource] : backend_.resources_) {
      NativeResource native;
      native.state = static_cast<D3D12_RESOURCE_STATES>(resource.initial_state);
      native.swapchain_back_buffer = resource.swapchain_back_buffer;
      if (resource.swapchain_back_buffer) {
        if (resource.swapchain_buffer_index >= kBufferCount || !back_buffers_[resource.swapchain_buffer_index]) {
          return fail(error, "swapchain back-buffer index is out of range");
        }
        back_buffers_[resource.swapchain_buffer_index]->AddRef();
        native.resource.reset(back_buffers_[resource.swapchain_buffer_index].get());
        resources_.emplace(object_id, std::move(native));
        continue;
      }

      const auto desc = resource_desc(resource);
      const auto optimized_clear = resource.has_optimized_clear_value ? clear_value(resource) : D3D12_CLEAR_VALUE{};
      HRESULT hr = S_OK;
      if (resource.heap_object_id != 0) {
        const auto heap_it = heaps_.find(resource.heap_object_id);
        if (heap_it == heaps_.end()) {
          return fail(error, "placed resource references an unknown heap");
        }
        hr = device_->CreatePlacedResource(
            heap_it->second.heap.get(),
            resource.heap_offset,
            &desc,
            static_cast<D3D12_RESOURCE_STATES>(resource.initial_state),
            resource.has_optimized_clear_value ? &optimized_clear : nullptr,
            IID_PPV_ARGS(native.resource.put()));
        if (FAILED(hr)) {
          error = hresult_error("CreatePlacedResource", hr);
          return false;
        }
      } else {
        const auto heap = heap_properties(resource.heap_type);
        hr = device_->CreateCommittedResource(
            &heap,
            static_cast<D3D12_HEAP_FLAGS>(resource.heap_flags),
            &desc,
            static_cast<D3D12_RESOURCE_STATES>(resource.initial_state),
            resource.has_optimized_clear_value ? &optimized_clear : nullptr,
            IID_PPV_ARGS(native.resource.put()));
        if (FAILED(hr)) {
          error = hresult_error("CreateCommittedResource", hr);
          return false;
        }
      }
      resources_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool upload_resource_data(
      ID3D12Resource *resource,
      const D3D12ReplayBackend::ResourceDataUpdate &update,
      std::string &error)
  {
    void *mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    HRESULT hr = resource->Map(update.subresource, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
      error = hresult_error("Map(resource data)", hr);
      return false;
    }
    std::memcpy(
        static_cast<std::uint8_t *>(mapped) + static_cast<std::size_t>(update.written_begin),
        update.bytes.data(),
        update.bytes.size());
    D3D12_RANGE written_range{
        static_cast<SIZE_T>(update.written_begin),
        static_cast<SIZE_T>(update.written_end)};
    resource->Unmap(update.subresource, &written_range);
    return true;
  }

  bool create_descriptor_heaps(std::string &error)
  {
    for (const auto &[object_id, heap] : backend_.descriptor_heaps_) {
      NativeDescriptorHeap native;
      D3D12_DESCRIPTOR_HEAP_DESC desc{};
      desc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heap.type);
      desc.NumDescriptors = heap.num_descriptors;
      desc.Flags = static_cast<D3D12_DESCRIPTOR_HEAP_FLAGS>(heap.flags);
      desc.NodeMask = heap.node_mask;
      HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(native.heap.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateDescriptorHeap", hr);
        return false;
      }
      native.type = desc.Type;
      native.descriptor_size = device_->GetDescriptorHandleIncrementSize(desc.Type);
      native.cpu_start = native.heap->GetCPUDescriptorHandleForHeapStart();
      if ((desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        native.gpu_start = native.heap->GetGPUDescriptorHandleForHeapStart();
      }
      descriptor_heaps_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool create_query_heaps(std::string &error)
  {
    for (const auto &[object_id, heap] : backend_.query_heaps_) {
      D3D12_QUERY_HEAP_DESC desc{};
      desc.Type = static_cast<D3D12_QUERY_HEAP_TYPE>(heap.type);
      desc.Count = heap.count;
      desc.NodeMask = heap.node_mask;
      ComPtr<ID3D12QueryHeap> native;
      HRESULT hr = device_->CreateQueryHeap(&desc, IID_PPV_ARGS(native.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateQueryHeap", hr);
        return false;
      }
      query_heaps_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool create_root_signatures(std::string &error)
  {
    for (const auto &[object_id, root_signature] : backend_.root_signatures_) {
      if (root_signature.bytes.empty()) {
        return fail(error, "root signature is missing bytecode");
      }
      ComPtr<ID3D12RootSignature> native;
      HRESULT hr = device_->CreateRootSignature(
          root_signature.node_mask,
          root_signature.bytes.data(),
          root_signature.bytes.size(),
          IID_PPV_ARGS(native.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateRootSignature", hr);
        return false;
      }
      root_signatures_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool read_shader_bytes(const json &pipeline_asset, const char *field, std::vector<std::uint8_t> &bytes, std::string &error) const
  {
    const auto shader = pipeline_asset.find(field);
    if (shader == pipeline_asset.end() || shader->is_null()) {
      bytes.clear();
      return true;
    }
    if (!shader->is_object()) {
      return fail(error, std::string("pipeline shader field is not an object: ") + field);
    }
    const auto size = payload_u64(*shader, "bytecode_size");
    const auto path_key = std::string(field) + "_path";
    const auto relative_path = payload_string(*shader, path_key);
    if (size == 0 || relative_path.empty()) {
      return fail(error, std::string("pipeline shader field is incomplete: ") + field);
    }
    return read_exact_asset_bytes(backend_.bundle_root_ / relative_path, size, bytes, error);
  }

  static D3D12_INPUT_LAYOUT_DESC input_layout_desc(
      const json &pipeline_asset,
      std::vector<std::string> &semantic_names,
      std::vector<D3D12_INPUT_ELEMENT_DESC> &elements)
  {
    semantic_names.clear();
    elements.clear();
    const auto input_layout = pipeline_asset.find("input_layout");
    if (input_layout == pipeline_asset.end() || !input_layout->is_object()) {
      return {};
    }
    const auto input_elements = input_layout->find("elements");
    if (input_elements == input_layout->end() || !input_elements->is_array()) {
      return {};
    }
    semantic_names.reserve(input_elements->size());
    elements.reserve(input_elements->size());
    for (const auto &element_json : *input_elements) {
      semantic_names.push_back(payload_string(element_json, "semantic_name"));
      D3D12_INPUT_ELEMENT_DESC element{};
      element.SemanticName = semantic_names.back().c_str();
      element.SemanticIndex = payload_u32(element_json, "semantic_index");
      element.Format = static_cast<DXGI_FORMAT>(payload_u32(element_json, "format"));
      element.InputSlot = payload_u32(element_json, "input_slot");
      element.AlignedByteOffset = payload_u32(element_json, "aligned_byte_offset");
      element.InputSlotClass = static_cast<D3D12_INPUT_CLASSIFICATION>(payload_u32(element_json, "input_slot_class"));
      element.InstanceDataStepRate = payload_u32(element_json, "instance_data_step_rate");
      elements.push_back(element);
    }
    return D3D12_INPUT_LAYOUT_DESC{elements.data(), static_cast<UINT>(elements.size())};
  }

  template <typename Desc>
  static bool require_object(const json &value, const char *key, Desc &, std::string &error)
  {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_object()) {
      error = std::string("D3D12 native command replay incomplete: pipeline asset missing ") + key;
      return false;
    }
    return true;
  }

  static D3D12_BLEND_DESC blend_desc(const json &pipeline_asset)
  {
    D3D12_BLEND_DESC desc{};
    const auto &blend = pipeline_asset["blend_state"];
    desc.AlphaToCoverageEnable = blend.value("alpha_to_coverage_enable", false);
    desc.IndependentBlendEnable = blend.value("independent_blend_enable", false);
    const auto targets = blend.find("render_targets");
    if (targets != blend.end() && targets->is_array()) {
      const auto count = std::min<std::size_t>(targets->size(), D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
      for (std::size_t index = 0; index < count; ++index) {
        const auto &target = (*targets)[index];
        desc.RenderTarget[index].BlendEnable = target.value("blend_enable", false);
        desc.RenderTarget[index].LogicOpEnable = target.value("logic_op_enable", false);
        desc.RenderTarget[index].SrcBlend = static_cast<D3D12_BLEND>(payload_u32(target, "src_blend"));
        desc.RenderTarget[index].DestBlend = static_cast<D3D12_BLEND>(payload_u32(target, "dest_blend"));
        desc.RenderTarget[index].BlendOp = static_cast<D3D12_BLEND_OP>(payload_u32(target, "blend_op"));
        desc.RenderTarget[index].SrcBlendAlpha = static_cast<D3D12_BLEND>(payload_u32(target, "src_blend_alpha"));
        desc.RenderTarget[index].DestBlendAlpha = static_cast<D3D12_BLEND>(payload_u32(target, "dest_blend_alpha"));
        desc.RenderTarget[index].BlendOpAlpha = static_cast<D3D12_BLEND_OP>(payload_u32(target, "blend_op_alpha"));
        desc.RenderTarget[index].LogicOp = static_cast<D3D12_LOGIC_OP>(payload_u32(target, "logic_op"));
        desc.RenderTarget[index].RenderTargetWriteMask = static_cast<UINT8>(payload_u32(target, "render_target_write_mask"));
      }
    }
    return desc;
  }

  static D3D12_RASTERIZER_DESC rasterizer_desc(const json &pipeline_asset)
  {
    const auto &rasterizer = pipeline_asset["rasterizer_state"];
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = static_cast<D3D12_FILL_MODE>(payload_u32(rasterizer, "fill_mode"));
    desc.CullMode = static_cast<D3D12_CULL_MODE>(payload_u32(rasterizer, "cull_mode"));
    desc.FrontCounterClockwise = rasterizer.value("front_counter_clockwise", false);
    desc.DepthBias = payload_i32(rasterizer, "depth_bias");
    desc.DepthBiasClamp = payload_float(rasterizer, "depth_bias_clamp");
    desc.SlopeScaledDepthBias = payload_float(rasterizer, "slope_scaled_depth_bias");
    desc.DepthClipEnable = rasterizer.value("depth_clip_enable", false);
    desc.MultisampleEnable = rasterizer.value("multisample_enable", false);
    desc.AntialiasedLineEnable = rasterizer.value("antialiased_line_enable", false);
    desc.ForcedSampleCount = payload_u32(rasterizer, "forced_sample_count");
    desc.ConservativeRaster = static_cast<D3D12_CONSERVATIVE_RASTERIZATION_MODE>(payload_u32(rasterizer, "conservative_raster"));
    return desc;
  }

  static D3D12_DEPTH_STENCILOP_DESC depth_stencil_op_desc(const json &op)
  {
    D3D12_DEPTH_STENCILOP_DESC desc{};
    desc.StencilFailOp = static_cast<D3D12_STENCIL_OP>(payload_u32(op, "stencil_fail_op"));
    desc.StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(payload_u32(op, "stencil_depth_fail_op"));
    desc.StencilPassOp = static_cast<D3D12_STENCIL_OP>(payload_u32(op, "stencil_pass_op"));
    desc.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(payload_u32(op, "stencil_func"));
    return desc;
  }

  static D3D12_DEPTH_STENCIL_DESC depth_stencil_desc(const json &pipeline_asset)
  {
    const auto &depth_stencil = pipeline_asset["depth_stencil_state"];
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = depth_stencil.value("depth_enable", false);
    desc.DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(payload_u32(depth_stencil, "depth_write_mask"));
    desc.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(payload_u32(depth_stencil, "depth_func"));
    desc.StencilEnable = depth_stencil.value("stencil_enable", false);
    desc.StencilReadMask = static_cast<UINT8>(payload_u32(depth_stencil, "stencil_read_mask"));
    desc.StencilWriteMask = static_cast<UINT8>(payload_u32(depth_stencil, "stencil_write_mask"));
    desc.FrontFace = depth_stencil_op_desc(depth_stencil["front_face"]);
    desc.BackFace = depth_stencil_op_desc(depth_stencil["back_face"]);
    return desc;
  }

  bool create_pipelines(std::string &error)
  {
    for (const auto &[object_id, pipeline] : backend_.pipelines_) {
      const auto root_it = root_signatures_.find(pipeline.root_signature_object_id);
      if (root_it == root_signatures_.end()) {
        return fail(error, "pipeline references an unknown root signature");
      }
      json pipeline_asset;
      if (!read_pipeline_asset_json(backend_.bundle_root_, pipeline.relative_path, &pipeline_asset, error)) {
        return false;
      }
      D3D12_BLEND_DESC blend{};
      D3D12_RASTERIZER_DESC rasterizer{};
      D3D12_DEPTH_STENCIL_DESC depth_stencil{};
      if (!pipeline.graphics) {
        std::vector<std::uint8_t> cs;
        if (!read_shader_bytes(pipeline_asset, "cs", cs, error)) {
          return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_it->second.get();
        desc.CS = D3D12_SHADER_BYTECODE{cs.data(), cs.size()};
        desc.NodeMask = pipeline.node_mask;
        desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(pipeline.flags);
        ComPtr<ID3D12PipelineState> native;
        HRESULT hr = device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(native.put()));
        if (FAILED(hr)) {
          error = hresult_error("CreateComputePipelineState", hr);
          return false;
        }
        pipelines_.emplace(object_id, std::move(native));
        continue;
      }
      if (!require_object(pipeline_asset, "blend_state", blend, error) ||
          !require_object(pipeline_asset, "rasterizer_state", rasterizer, error) ||
          !require_object(pipeline_asset, "depth_stencil_state", depth_stencil, error)) {
        return false;
      }
      std::vector<std::uint8_t> vs;
      std::vector<std::uint8_t> ps;
      std::vector<std::uint8_t> ds;
      std::vector<std::uint8_t> hs;
      std::vector<std::uint8_t> gs;
      if (!read_shader_bytes(pipeline_asset, "vs", vs, error) ||
          !read_shader_bytes(pipeline_asset, "ps", ps, error) ||
          !read_shader_bytes(pipeline_asset, "ds", ds, error) ||
          !read_shader_bytes(pipeline_asset, "hs", hs, error) ||
          !read_shader_bytes(pipeline_asset, "gs", gs, error)) {
        return false;
      }
      std::vector<std::string> semantic_names;
      std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = root_it->second.get();
      desc.VS = D3D12_SHADER_BYTECODE{vs.data(), vs.size()};
      desc.PS = D3D12_SHADER_BYTECODE{ps.data(), ps.size()};
      desc.DS = D3D12_SHADER_BYTECODE{ds.data(), ds.size()};
      desc.HS = D3D12_SHADER_BYTECODE{hs.data(), hs.size()};
      desc.GS = D3D12_SHADER_BYTECODE{gs.data(), gs.size()};
      desc.BlendState = blend_desc(pipeline_asset);
      desc.SampleMask = pipeline.sample_mask;
      desc.RasterizerState = rasterizer_desc(pipeline_asset);
      desc.DepthStencilState = depth_stencil_desc(pipeline_asset);
      desc.InputLayout = input_layout_desc(pipeline_asset, semantic_names, input_elements);
      desc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(pipeline.primitive_topology_type);
      desc.NumRenderTargets = pipeline.num_render_targets;
      for (std::size_t index = 0; index < pipeline.rtv_formats.size() && index < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
        desc.RTVFormats[index] = static_cast<DXGI_FORMAT>(pipeline.rtv_formats[index]);
      }
      desc.DSVFormat = static_cast<DXGI_FORMAT>(pipeline.dsv_format);
      desc.SampleDesc.Count = pipeline.sample_count;
      desc.SampleDesc.Quality = pipeline.sample_quality;
      desc.NodeMask = pipeline.node_mask;
      desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(pipeline.flags);
      desc.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(pipeline.ib_strip_cut_value);
      ComPtr<ID3D12PipelineState> native;
      HRESULT hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(native.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateGraphicsPipelineState", hr);
        return false;
      }
      pipelines_.emplace(object_id, std::move(native));
    }
    return true;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE relocated_cpu_descriptor(const D3D12ReplayBackend::DescriptorBinding &binding) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      return handle;
    }
    handle.ptr = heap_it->second.cpu_start.ptr +
                 static_cast<SIZE_T>(binding.descriptor_index) * heap_it->second.descriptor_size;
    return handle;
  }

  D3D12_GPU_DESCRIPTOR_HANDLE relocated_gpu_descriptor(const D3D12ReplayBackend::DescriptorBinding &binding) const
  {
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      return handle;
    }
    handle.ptr = heap_it->second.gpu_start.ptr +
                 static_cast<UINT64>(binding.descriptor_index) * heap_it->second.descriptor_size;
    return handle;
  }

  bool apply_descriptor(
      const D3D12ReplayBackend::DescriptorSemanticState &semantic,
      std::string &error)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE dst = relocated_cpu_descriptor(semantic.binding);
    if (dst.ptr == 0) {
      return fail(error, "descriptor destination cannot be relocated");
    }
    ID3D12Resource *resource = semantic.resource_object_id == 0 ? nullptr : resource_for(semantic.resource_object_id, error);
    if (semantic.resource_object_id != 0 && !resource) {
      return false;
    }
    if (semantic.kind == "RenderTargetView") {
      D3D12_RENDER_TARGET_VIEW_DESC desc{};
      D3D12_RENDER_TARGET_VIEW_DESC *desc_ptr = nullptr;
      if (semantic.view_dimension != 0 || semantic.format != 0) {
        desc.Format = static_cast<DXGI_FORMAT>(semantic.format);
        desc.ViewDimension = static_cast<D3D12_RTV_DIMENSION>(semantic.view_dimension);
        switch (desc.ViewDimension) {
        case D3D12_RTV_DIMENSION_BUFFER:
          desc.Buffer.FirstElement = semantic.first_element;
          desc.Buffer.NumElements = semantic.num_elements;
          break;
        case D3D12_RTV_DIMENSION_TEXTURE1D:
          desc.Texture1D.MipSlice = semantic.mip_slice;
          break;
        case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
          desc.Texture1DArray.MipSlice = semantic.mip_slice;
          desc.Texture1DArray.FirstArraySlice = semantic.first_array_slice;
          desc.Texture1DArray.ArraySize = semantic.array_size;
          break;
        case D3D12_RTV_DIMENSION_TEXTURE2D:
          desc.Texture2D.MipSlice = semantic.mip_slice;
          desc.Texture2D.PlaneSlice = semantic.plane_slice;
          break;
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
          desc.Texture2DArray.MipSlice = semantic.mip_slice;
          desc.Texture2DArray.FirstArraySlice = semantic.first_array_slice;
          desc.Texture2DArray.ArraySize = semantic.array_size;
          desc.Texture2DArray.PlaneSlice = semantic.plane_slice;
          break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMS:
          break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
          desc.Texture2DMSArray.FirstArraySlice = semantic.first_array_slice;
          desc.Texture2DMSArray.ArraySize = semantic.array_size;
          break;
        case D3D12_RTV_DIMENSION_TEXTURE3D:
          desc.Texture3D.MipSlice = semantic.mip_slice;
          desc.Texture3D.FirstWSlice = semantic.first_w_slice;
          desc.Texture3D.WSize = semantic.w_size;
          break;
        default:
          return fail(error, "RTV dimension is not covered yet");
        }
        desc_ptr = &desc;
      }
      device_->CreateRenderTargetView(resource, desc_ptr, dst);
    } else if (semantic.kind == "DepthStencilView") {
      D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
      D3D12_DEPTH_STENCIL_VIEW_DESC *desc_ptr = nullptr;
      if (semantic.view_dimension != 0 || semantic.format != 0) {
        desc.Format = static_cast<DXGI_FORMAT>(semantic.format);
        desc.ViewDimension = static_cast<D3D12_DSV_DIMENSION>(semantic.view_dimension);
        desc.Flags = static_cast<D3D12_DSV_FLAGS>(semantic.flags);
        switch (desc.ViewDimension) {
        case D3D12_DSV_DIMENSION_TEXTURE1D:
          desc.Texture1D.MipSlice = semantic.mip_slice;
          break;
        case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
          desc.Texture1DArray.MipSlice = semantic.mip_slice;
          desc.Texture1DArray.FirstArraySlice = semantic.first_array_slice;
          desc.Texture1DArray.ArraySize = semantic.array_size;
          break;
        case D3D12_DSV_DIMENSION_TEXTURE2D:
          desc.Texture2D.MipSlice = semantic.mip_slice;
          break;
        case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
          desc.Texture2DArray.MipSlice = semantic.mip_slice;
          desc.Texture2DArray.FirstArraySlice = semantic.first_array_slice;
          desc.Texture2DArray.ArraySize = semantic.array_size;
          break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMS:
          break;
        case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
          desc.Texture2DMSArray.FirstArraySlice = semantic.first_array_slice;
          desc.Texture2DMSArray.ArraySize = semantic.array_size;
          break;
        default:
          return fail(error, "DSV dimension is not covered yet");
        }
        desc_ptr = &desc;
      }
      device_->CreateDepthStencilView(resource, desc_ptr, dst);
    } else if (semantic.kind == "ConstantBufferView") {
      if (semantic.buffer.resource_object_id == 0) {
        return fail(error, "CBV source GPUVA cannot be relocated");
      }
      const auto native_it = resources_.find(semantic.buffer.resource_object_id);
      if (native_it == resources_.end()) {
        return fail(error, "CBV source resource is missing");
      }
      D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
      desc.BufferLocation = native_it->second.resource->GetGPUVirtualAddress() + semantic.buffer.offset;
      desc.SizeInBytes = semantic.size_in_bytes;
      device_->CreateConstantBufferView(&desc, dst);
    } else if (semantic.kind == "ShaderResourceView") {
      if (!semantic.has_view_desc) {
        device_->CreateShaderResourceView(resource, nullptr, dst);
        return true;
      }
      D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
      desc.Format = static_cast<DXGI_FORMAT>(semantic.format);
      desc.ViewDimension = static_cast<D3D12_SRV_DIMENSION>(semantic.view_dimension);
      desc.Shader4ComponentMapping = semantic.shader_4_component_mapping;
      if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE1D) {
        desc.Texture1D.MostDetailedMip = semantic.most_detailed_mip;
        desc.Texture1D.MipLevels = semantic.mip_levels;
        desc.Texture1D.ResourceMinLODClamp = semantic.resource_min_lod_clamp;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE1DARRAY) {
        desc.Texture1DArray.MostDetailedMip = semantic.most_detailed_mip;
        desc.Texture1DArray.MipLevels = semantic.mip_levels;
        desc.Texture1DArray.FirstArraySlice = semantic.first_array_slice;
        desc.Texture1DArray.ArraySize = semantic.array_size;
        desc.Texture1DArray.ResourceMinLODClamp = semantic.resource_min_lod_clamp;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
        desc.Texture2D.MostDetailedMip = semantic.most_detailed_mip;
        desc.Texture2D.MipLevels = semantic.mip_levels;
        desc.Texture2D.PlaneSlice = semantic.plane_slice;
        desc.Texture2D.ResourceMinLODClamp = semantic.resource_min_lod_clamp;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
        desc.Texture2DArray.MostDetailedMip = semantic.most_detailed_mip;
        desc.Texture2DArray.MipLevels = semantic.mip_levels;
        desc.Texture2DArray.FirstArraySlice = semantic.first_array_slice;
        desc.Texture2DArray.ArraySize = semantic.array_size;
        desc.Texture2DArray.PlaneSlice = semantic.plane_slice;
        desc.Texture2DArray.ResourceMinLODClamp = semantic.resource_min_lod_clamp;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMS) {
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY) {
        desc.Texture2DMSArray.FirstArraySlice = semantic.first_array_slice;
        desc.Texture2DMSArray.ArraySize = semantic.array_size;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D) {
        desc.Texture3D.MostDetailedMip = semantic.most_detailed_mip;
        desc.Texture3D.MipLevels = semantic.mip_levels;
        desc.Texture3D.ResourceMinLODClamp = semantic.resource_min_lod_clamp;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE) {
        desc.TextureCube.MostDetailedMip = semantic.most_detailed_mip;
        desc.TextureCube.MipLevels = semantic.mip_levels;
        desc.TextureCube.ResourceMinLODClamp = semantic.resource_min_lod_clamp;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY) {
        desc.TextureCubeArray.MostDetailedMip = semantic.most_detailed_mip;
        desc.TextureCubeArray.MipLevels = semantic.mip_levels;
        desc.TextureCubeArray.First2DArrayFace = semantic.first_2d_array_face;
        desc.TextureCubeArray.NumCubes = semantic.num_cubes;
        desc.TextureCubeArray.ResourceMinLODClamp = semantic.resource_min_lod_clamp;
      } else if (desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        desc.Buffer.FirstElement = semantic.first_element;
        desc.Buffer.NumElements = semantic.num_elements;
        desc.Buffer.StructureByteStride = semantic.structure_byte_stride;
        desc.Buffer.Flags = static_cast<D3D12_BUFFER_SRV_FLAGS>(semantic.flags);
      } else {
        return fail(error, "SRV dimension is not covered yet");
      }
      device_->CreateShaderResourceView(resource, &desc, dst);
    } else if (semantic.kind == "UnorderedAccessView") {
      ID3D12Resource *counter_resource = semantic.counter_resource_object_id == 0 ? nullptr : resource_for(semantic.counter_resource_object_id, error);
      if (semantic.counter_resource_object_id != 0 && !counter_resource) {
        return false;
      }
      if (!semantic.has_view_desc) {
        device_->CreateUnorderedAccessView(resource, counter_resource, nullptr, dst);
        return true;
      }
      D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
      desc.Format = static_cast<DXGI_FORMAT>(semantic.format);
      desc.ViewDimension = static_cast<D3D12_UAV_DIMENSION>(semantic.view_dimension);
      if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE1D) {
        desc.Texture1D.MipSlice = semantic.mip_slice;
      } else if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE1DARRAY) {
        desc.Texture1DArray.MipSlice = semantic.mip_slice;
        desc.Texture1DArray.FirstArraySlice = semantic.first_array_slice;
        desc.Texture1DArray.ArraySize = semantic.array_size;
      } else if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) {
        desc.Texture2D.MipSlice = semantic.mip_slice;
        desc.Texture2D.PlaneSlice = semantic.plane_slice;
      } else if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) {
        desc.Texture2DArray.MipSlice = semantic.mip_slice;
        desc.Texture2DArray.FirstArraySlice = semantic.first_array_slice;
        desc.Texture2DArray.ArraySize = semantic.array_size;
        desc.Texture2DArray.PlaneSlice = semantic.plane_slice;
      } else if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D) {
        desc.Texture3D.MipSlice = semantic.mip_slice;
        desc.Texture3D.FirstWSlice = semantic.first_w_slice;
        desc.Texture3D.WSize = semantic.w_size;
      } else if (desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
        desc.Buffer.FirstElement = semantic.first_element;
        desc.Buffer.NumElements = semantic.num_elements;
        desc.Buffer.StructureByteStride = semantic.structure_byte_stride;
        desc.Buffer.CounterOffsetInBytes = semantic.counter_offset_in_bytes;
        desc.Buffer.Flags = static_cast<D3D12_BUFFER_UAV_FLAGS>(semantic.flags);
      } else {
        return fail(error, "UAV dimension is not covered yet");
      }
      device_->CreateUnorderedAccessView(resource, counter_resource, &desc, dst);
    } else {
      return fail(error, "descriptor kind is not covered: " + semantic.kind);
    }
    return true;
  }

  bool apply_sampler(
      const D3D12ReplayBackend::SamplerSemanticState &semantic,
      std::string &error)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE dst = relocated_cpu_descriptor(semantic.binding);
    if (dst.ptr == 0) {
      return fail(error, "sampler destination cannot be relocated");
    }
    if (!semantic.has_desc) {
      return fail(error, "sampler descriptor payload is incomplete");
    }
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = static_cast<D3D12_FILTER>(semantic.filter);
    desc.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(semantic.address_u);
    desc.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(semantic.address_v);
    desc.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(semantic.address_w);
    desc.MipLODBias = semantic.mip_lod_bias;
    desc.MaxAnisotropy = semantic.max_anisotropy;
    desc.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(semantic.comparison_func);
    desc.BorderColor[0] = semantic.border_color[0];
    desc.BorderColor[1] = semantic.border_color[1];
    desc.BorderColor[2] = semantic.border_color[2];
    desc.BorderColor[3] = semantic.border_color[3];
    desc.MinLOD = semantic.min_lod;
    desc.MaxLOD = semantic.max_lod;
    device_->CreateSampler(&desc, dst);
    return true;
  }

  bool apply_descriptor_copy(
      const D3D12ReplayBackend::CopyDescriptorSemanticState &semantic,
      std::string &error)
  {
    if (semantic.descriptors.empty()) {
      return true;
    }
    for (const auto &descriptor : semantic.descriptors) {
      D3D12ReplayBackend::DescriptorBinding dst_binding;
      if (!backend_.resolve_descriptor_binding_at(
              descriptor.dst_descriptor,
              false,
              semantic.create_sequence,
              dst_binding,
              error)) {
        return false;
      }
      D3D12ReplayBackend::DescriptorBinding src_binding;
      if (!backend_.resolve_descriptor_binding_at(
              descriptor.src_descriptor,
              false,
              semantic.create_sequence,
              src_binding,
              error)) {
        return false;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE dst = relocated_cpu_descriptor(dst_binding);
      D3D12_CPU_DESCRIPTOR_HANDLE src = relocated_cpu_descriptor(src_binding);
      if (dst.ptr == 0 || src.ptr == 0) {
        return fail(error, "CopyDescriptors descriptor cannot be relocated");
      }
      device_->CopyDescriptorsSimple(1, dst, src, static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(semantic.descriptor_heap_type));
    }
    return true;
  }

  bool sync_descriptors_until(std::uint64_t sequence, std::string &error)
  {
    while ((next_descriptor_index_ < descriptor_timeline_.size() &&
            descriptor_timeline_[next_descriptor_index_]->create_sequence <= sequence) ||
           (next_sampler_index_ < sampler_timeline_.size() &&
            sampler_timeline_[next_sampler_index_]->create_sequence <= sequence) ||
           (next_descriptor_copy_index_ < descriptor_copy_timeline_.size() &&
            descriptor_copy_timeline_[next_descriptor_copy_index_]->create_sequence <= sequence)) {
      const auto next_descriptor_sequence = next_descriptor_index_ < descriptor_timeline_.size()
                                                ? descriptor_timeline_[next_descriptor_index_]->create_sequence
                                                : std::numeric_limits<std::uint64_t>::max();
      const auto next_sampler_sequence = next_sampler_index_ < sampler_timeline_.size()
                                             ? sampler_timeline_[next_sampler_index_]->create_sequence
                                             : std::numeric_limits<std::uint64_t>::max();
      const auto next_descriptor_copy_sequence = next_descriptor_copy_index_ < descriptor_copy_timeline_.size()
                                                    ? descriptor_copy_timeline_[next_descriptor_copy_index_]->create_sequence
                                                    : std::numeric_limits<std::uint64_t>::max();
      if (next_sampler_sequence <= next_descriptor_sequence &&
          next_sampler_sequence <= next_descriptor_copy_sequence) {
        if (!apply_sampler(*sampler_timeline_[next_sampler_index_], error)) {
          return false;
        }
        ++next_sampler_index_;
      } else if (next_descriptor_copy_sequence < next_descriptor_sequence) {
        if (!apply_descriptor_copy(*descriptor_copy_timeline_[next_descriptor_copy_index_], error)) {
          return false;
        }
        ++next_descriptor_copy_index_;
      } else {
        if (!apply_descriptor(*descriptor_timeline_[next_descriptor_index_], error)) {
          return false;
        }
        ++next_descriptor_index_;
      }
    }
    return true;
  }

  bool sync_resource_data_until(std::uint64_t sequence, std::string &error)
  {
    while (next_resource_update_index_ < resource_update_timeline_.size() &&
           resource_update_timeline_[next_resource_update_index_].sequence <= sequence) {
      const auto &entry = resource_update_timeline_[next_resource_update_index_];
      auto native_it = resources_.find(entry.resource_object_id);
      if (native_it == resources_.end()) {
        return fail(error, "resource update references an unknown resource");
      }
      if (!upload_resource_data(native_it->second.resource.get(), *entry.update, error)) {
        return false;
      }
      ++next_resource_update_index_;
    }
    return true;
  }

  bool prepare_chronological_state(std::string &error)
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

    descriptor_copy_timeline_.clear();
    descriptor_copy_timeline_.reserve(backend_.descriptor_copies_.size());
    for (const auto &descriptor_copy : backend_.descriptor_copies_) {
      descriptor_copy_timeline_.push_back(&descriptor_copy);
    }
    std::stable_sort(
        descriptor_copy_timeline_.begin(),
        descriptor_copy_timeline_.end(),
        [](const auto *lhs, const auto *rhs) {
          return lhs->create_sequence < rhs->create_sequence;
        });

    resource_update_timeline_.clear();
    for (const auto &[resource_object_id, resource] : backend_.resources_) {
      for (const auto &update : resource.data_updates) {
        resource_update_timeline_.push_back(ResourceDataUpdateRef{update.sequence, resource_object_id, &update});
      }
    }
    std::stable_sort(
        resource_update_timeline_.begin(),
        resource_update_timeline_.end(),
        [](const auto &lhs, const auto &rhs) {
          return lhs.sequence < rhs.sequence;
        });

    next_descriptor_index_ = 0;
    next_sampler_index_ = 0;
    next_descriptor_copy_index_ = 0;
    next_resource_update_index_ = 0;
    (void)error;
    return true;
  }

  bool create_command_signatures(std::string &error)
  {
    for (const auto &[object_id, signature] : backend_.command_signatures_) {
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
          return fail(error, "command signature references an unknown root signature");
        }
        root_signature = root_it->second.get();
      }
      ComPtr<ID3D12CommandSignature> native;
      HRESULT hr = device_->CreateCommandSignature(&desc, root_signature, IID_PPV_ARGS(native.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandSignature", hr);
        return false;
      }
      command_signatures_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool create_command_objects(std::string &error)
  {
    for (const auto &[object_id, command_list] : backend_.command_lists_) {
      if (command_list.type != static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT) &&
          command_list.type != static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_BUNDLE)) {
        return fail(error, "only direct and bundle command lists are currently supported");
      }
      NativeCommandList native;
      native.type = static_cast<D3D12_COMMAND_LIST_TYPE>(command_list.type);
      HRESULT hr = device_->CreateCommandAllocator(native.type, IID_PPV_ARGS(native.allocator.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandAllocator", hr);
        return false;
      }
      hr = device_->CreateCommandList(0, native.type, native.allocator.get(), nullptr, IID_PPV_ARGS(native.list.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandList", hr);
        return false;
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
      native.list->Close();
      command_lists_.emplace(object_id, std::move(native));
    }
    return true;
  }

  bool find_submitted_interval(
      trace::ObjectId command_list_id,
      std::uint64_t execute_sequence,
      std::size_t &begin_index,
      std::size_t &end_index,
      std::string &error) const
  {
    constexpr auto invalid = std::numeric_limits<std::size_t>::max();
    auto current_begin = invalid;
    begin_index = invalid;
    end_index = invalid;
    for (std::size_t index = 0; index < backend_.replay_commands_.size(); ++index) {
      const auto &command = backend_.replay_commands_[index];
      if (command.command_list_object_id != command_list_id) {
        continue;
      }
      if (command.sequence >= execute_sequence) {
        break;
      }
      if (command.kind == D3D12ReplayBackend::ReplayCommandKind::BeginCommandList) {
        current_begin = index;
      } else if (command.kind == D3D12ReplayBackend::ReplayCommandKind::EndCommandList && current_begin != invalid) {
        begin_index = current_begin;
        end_index = index;
        current_begin = invalid;
      }
    }
    if (begin_index == invalid || end_index == invalid) {
      error = "D3D12 native command replay incomplete: submitted command list has no closed interval";
      return false;
    }
    return true;
  }

  bool find_closed_interval_before(
      trace::ObjectId command_list_id,
      std::uint64_t sequence_limit,
      std::size_t &begin_index,
      std::size_t &end_index,
      std::string &error) const
  {
    constexpr auto invalid = std::numeric_limits<std::size_t>::max();
    auto current_begin = invalid;
    begin_index = invalid;
    end_index = invalid;
    for (std::size_t index = 0; index < backend_.replay_commands_.size(); ++index) {
      const auto &command = backend_.replay_commands_[index];
      if (command.command_list_object_id != command_list_id) {
        continue;
      }
      if (command.sequence >= sequence_limit) {
        break;
      }
      if (command.kind == D3D12ReplayBackend::ReplayCommandKind::BeginCommandList) {
        current_begin = index;
        continue;
      }
      if (command.kind == D3D12ReplayBackend::ReplayCommandKind::EndCommandList && current_begin != invalid) {
        begin_index = current_begin;
        end_index = index;
        current_begin = invalid;
      }
    }
    if (begin_index == invalid || end_index == invalid) {
      error = "D3D12 native command replay incomplete: command list has no closed interval";
      return false;
    }
    return true;
  }

  bool replay_submissions(std::string &error)
  {
    for (const auto &batch : backend_.submissions_.completed_batches()) {
      std::vector<ID3D12CommandList *> native_lists;
      native_lists.reserve(batch.command_list_ids.size());
      for (const auto command_list_id : batch.command_list_ids) {
        auto native_it = command_lists_.find(command_list_id);
        if (native_it == command_lists_.end()) {
          return fail(error, "submission references an unknown command list");
        }
        std::size_t begin_index = 0;
        std::size_t end_index = 0;
        if (!find_submitted_interval(command_list_id, batch.execute_sequence, begin_index, end_index, error)) {
          return false;
        }
        if (!record_command_list(native_it->second, begin_index, end_index, error)) {
          return false;
        }
        native_lists.push_back(native_it->second.list.get());
      }
      if (!sync_resource_data_until(batch.execute_sequence, error) ||
          !sync_descriptors_until(batch.execute_sequence, error)) {
        return false;
      }
      if (!native_lists.empty()) {
        queue_->ExecuteCommandLists(static_cast<UINT>(native_lists.size()), native_lists.data());
        wait_for_gpu();
      }
      if (batch.presented) {
        UINT sync_interval = 1;
        UINT flags = 0;
        std::uint64_t present_frame_index = batch.present_frame_index;
        for (const auto &[frame_index, semantic] : backend_.present_semantics_) {
          if (semantic.boundary_sequence != batch.present_sequence) {
            continue;
          }
          present_frame_index = frame_index;
          sync_interval = static_cast<UINT>(semantic.sync_interval);
          flags = static_cast<UINT>(semantic.flags);
          break;
        }
        if (!capture_present_frame(present_frame_index, sync_interval, flags, error)) {
          return false;
        }
        HRESULT hr = swap_chain_->Present(sync_interval, flags);
        record_present_semantics(sync_interval, flags, hr);
        if (FAILED(hr)) {
          error = hresult_error("IDXGISwapChain::Present", hr);
          return false;
        }
        if (present_delay_ms_ != 0) {
#ifdef _WIN32
          Sleep(present_delay_ms_);
#else
          std::this_thread::sleep_for(std::chrono::milliseconds(present_delay_ms_));
#endif
        }
        if (!pump_messages()) {
          return fail(error, "replay window closed");
        }
      }
    }
    return true;
  }

  void record_present_semantics(UINT sync_interval, UINT flags, HRESULT result)
  {
    if (!d3d12_retrace_present_frame_capture_enabled()) {
      return;
    }
    static RecordPresentSemanticsFn record =
        resolve_d3d12_export<RecordPresentSemanticsFn>("apitrace_d3d12_record_present_semantics");
    if (record) {
      record(sync_interval, flags, result);
    }
  }

  bool capture_present_frame(std::uint64_t frame_index, UINT sync_interval, UINT flags, std::string &error)
  {
    if (!d3d12_retrace_present_frame_capture_enabled()) {
      return true;
    }
#ifdef _WIN32
    static RecordPresentFrameFn proxy_record_present_frame =
        resolve_d3d12_export<RecordPresentFrameFn>("apitrace_d3d12_record_present_frame");
    if (!proxy_record_present_frame) {
      return fail(error, "APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES is enabled but the capture export is missing");
    }
#endif

    const UINT back_buffer_index = swap_chain_->GetCurrentBackBufferIndex();
    if (back_buffer_index >= kBufferCount || !back_buffers_[back_buffer_index]) {
      return fail(error, "swapchain back-buffer is missing for present-frame capture");
    }
    ID3D12Resource *back_buffer = back_buffers_[back_buffer_index].get();
    const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
      return fail(error, "present-frame capture currently requires an RGBA8 swapchain");
    }
    if (desc.Width == 0 || desc.Height == 0 ||
        desc.Width > static_cast<UINT64>(std::numeric_limits<UINT>::max()) ||
        desc.Height > static_cast<UINT64>(std::numeric_limits<UINT>::max())) {
      return fail(error, "present-frame capture has invalid back-buffer dimensions");
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 readback_size = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size, &readback_size);
    if (num_rows == 0 || readback_size == 0 || footprint.Footprint.RowPitch < desc.Width * 4u) {
      return fail(error, "present-frame capture produced an invalid readback layout");
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = readback_size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const auto width = static_cast<UINT>(desc.Width);
    const auto height = static_cast<UINT>(desc.Height);
    const auto row_pitch = width * 4u;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(row_pitch) * height);

    {
      ScopedD3D12CaptureSuppression suppress_capture;
      ComPtr<ID3D12Resource> readback;
      HRESULT hr = device_->CreateCommittedResource(
          &heap,
          D3D12_HEAP_FLAG_NONE,
          &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr,
          IID_PPV_ARGS(readback.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommittedResource(readback)", hr);
        return false;
      }

      ReadbackCommandObjects commands;
      hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commands.allocator.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandAllocator(readback)", hr);
        return false;
      }
      hr = device_->CreateCommandList(
          0,
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          commands.allocator.get(),
          nullptr,
          IID_PPV_ARGS(commands.list.put()));
      if (FAILED(hr)) {
        error = hresult_error("CreateCommandList(readback)", hr);
        return false;
      }

      const auto to_copy = transition_barrier(back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      commands.list->ResourceBarrier(1, &to_copy);

      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = back_buffer;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;

      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = readback.get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = footprint;
      commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

      const auto to_present = transition_barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
      commands.list->ResourceBarrier(1, &to_present);
      hr = commands.list->Close();
      if (FAILED(hr)) {
        error = hresult_error("ID3D12GraphicsCommandList::Close(readback)", hr);
        return false;
      }

      ID3D12CommandList *lists[] = {commands.list.get()};
      queue_->ExecuteCommandLists(1, lists);
      wait_for_gpu();

      void *mapped = nullptr;
      D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
      hr = readback->Map(0, &read_range, &mapped);
      if (FAILED(hr) || !mapped) {
        error = hresult_error("Map(readback)", hr);
        return false;
      }

      for (UINT row = 0; row < height; ++row) {
        const auto *src_row = static_cast<const std::uint8_t *>(mapped) +
                              static_cast<std::size_t>(footprint.Offset) +
                              static_cast<std::size_t>(row) * footprint.Footprint.RowPitch;
        auto *dst_row = rgba.data() + static_cast<std::size_t>(row) * row_pitch;
        std::memcpy(dst_row, src_row, row_pitch);
      }
      D3D12_RANGE written_range{0, 0};
      readback->Unmap(0, &written_range);
    }

    if (!record_present_frame(
        frame_index,
        width,
        height,
        row_pitch,
        sync_interval,
        flags,
        rgba.data(),
        static_cast<SIZE_T>(rgba.size()),
        error)) {
      return false;
    }
    return true;
  }

#ifdef _WIN32
  bool record_present_frame(
      std::uint64_t frame_index,
      UINT width,
      UINT height,
      UINT row_pitch,
      UINT sync_interval,
      UINT flags,
      const void *rgba_data,
      SIZE_T rgba_size,
      std::string &error)
  {
    static RecordPresentFrameFn record =
        resolve_d3d12_export<RecordPresentFrameFn>("apitrace_d3d12_record_present_frame");
    if (!record) {
      return fail(error, "APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES is enabled but the capture export is missing");
    }
    record(frame_index, width, height, row_pitch, sync_interval, flags, rgba_data, rgba_size);
    return true;
  }
#else
  bool ensure_capture_writer(std::string &error)
  {
    if (capture_writer_) {
      return true;
    }
    const char *bundle_root = std::getenv("APITRACE_TRACE_BUNDLE");
    if (bundle_root == nullptr || *bundle_root == '\0') {
      return fail(error, "APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES requires APITRACE_TRACE_BUNDLE");
    }
    capture_writer_ = std::make_unique<trace::TraceBundleWriter>();
    if (!capture_writer_->open(bundle_root)) {
      capture_writer_.reset();
      return fail(error, "failed to open APITRACE_TRACE_BUNDLE for native D3D12 present capture");
    }
    trace::TraceMetadata metadata;
    metadata.api = trace::ApiKind::D3D12;
    metadata.producer = "apitrace_d3d12_native_retrace";
    capture_writer_->write_metadata(metadata);
    return true;
  }

  bool record_present_frame(
      std::uint64_t frame_index,
      UINT width,
      UINT height,
      UINT row_pitch,
      UINT sync_interval,
      UINT flags,
      const void *rgba_data,
      SIZE_T rgba_size,
      std::string &error)
  {
    if (!rgba_data || width == 0 || height == 0 || row_pitch == 0 || rgba_size == 0) {
      return true;
    }
    if (!ensure_capture_writer(error)) {
      return false;
    }

    trace::AssetRecord asset;
    asset.blob_id = ++capture_sequence_;
    asset.kind = trace::AssetKind::Texture;
    asset.debug_name = "d3d12-present-frame";
    const auto *begin = static_cast<const std::uint8_t *>(rgba_data);
    asset.payload_bytes.assign(begin, begin + static_cast<std::size_t>(rgba_size));
    asset = capture_writer_->register_asset(std::move(asset));

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

    trace::EventRecord event;
    event.kind = trace::EventKind::ResourceBlob;
    event.callsite.sequence = ++capture_sequence_;
    event.callsite.function_name = "resource_blob";
    event.object_debug_name = "D3D12PresentFrame";
    event.blob_refs = {asset.blob_id};
    event.payload = payload.str();
    capture_writer_->append_call_event(event);
    return true;
  }
#endif

  bool record_command_list(
      NativeCommandList &native,
      std::size_t begin_index,
      std::size_t end_index,
      std::string &error)
  {
    HRESULT hr = native.allocator->Reset();
    if (FAILED(hr)) {
      error = hresult_error("ID3D12CommandAllocator::Reset", hr);
      return false;
    }
    hr = native.list->Reset(native.allocator.get(), nullptr);
    if (FAILED(hr)) {
      error = hresult_error("ID3D12GraphicsCommandList::Reset", hr);
      return false;
    }
    for (std::size_t index = begin_index + 1; index < end_index; ++index) {
      if (!record_command(native.list.get(), backend_.replay_commands_[index], error)) {
        return false;
      }
    }
    hr = native.list->Close();
    if (FAILED(hr)) {
      error = hresult_error("ID3D12GraphicsCommandList::Close", hr);
      return false;
    }
    return true;
  }

  bool ensure_command_list_recorded(
      trace::ObjectId command_list_id,
      std::uint64_t sequence_limit,
      std::string &error)
  {
    auto native_it = command_lists_.find(command_list_id);
    if (native_it == command_lists_.end()) {
      return fail(error, "command references an unknown command list");
    }
    if (native_it->second.recorded_until_sequence != 0 &&
        native_it->second.recorded_until_sequence >= sequence_limit) {
      return true;
    }
    std::size_t begin_index = 0;
    std::size_t end_index = 0;
    if (!find_closed_interval_before(command_list_id, sequence_limit, begin_index, end_index, error)) {
      return false;
    }
    if (!record_command_list(native_it->second, begin_index, end_index, error)) {
      return false;
    }
    native_it->second.recorded_until_sequence = sequence_limit;
    return true;
  }

  bool descriptor_handle(
      const D3D12ReplayBackend::DescriptorBinding &binding,
      D3D12_CPU_DESCRIPTOR_HANDLE &handle,
      std::string &error) const
  {
    handle = {};
    if (binding.heap_object_id == 0) {
      return true;
    }
    const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      return fail(error, "descriptor binding references an unknown heap");
    }
    handle.ptr = heap_it->second.cpu_start.ptr +
                 static_cast<SIZE_T>(binding.descriptor_index) * heap_it->second.descriptor_size;
    return true;
  }

  bool descriptor_gpu_handle(
      const D3D12ReplayBackend::DescriptorBinding &binding,
      D3D12_GPU_DESCRIPTOR_HANDLE &handle,
      std::string &error) const
  {
    handle = {};
    if (binding.heap_object_id == 0) {
      return true;
    }
    const auto heap_it = descriptor_heaps_.find(binding.heap_object_id);
    if (heap_it == descriptor_heaps_.end()) {
      return fail(error, "descriptor binding references an unknown heap");
    }
    handle.ptr = heap_it->second.gpu_start.ptr +
                 static_cast<UINT64>(binding.descriptor_index) * heap_it->second.descriptor_size;
    return true;
  }

  ID3D12Resource *resource_for(trace::ObjectId object_id, std::string &error) const
  {
    const auto resource_it = resources_.find(object_id);
    if (resource_it == resources_.end() || !resource_it->second.resource) {
      error = "D3D12 native command replay incomplete: command references an unknown resource";
      return nullptr;
    }
    return resource_it->second.resource.get();
  }

  NativeCommandList *native_command_list_for(
      trace::ObjectId object_id,
      std::string &error)
  {
    auto it = command_lists_.find(object_id);
    if (it == command_lists_.end()) {
      fail(error, "command references an unknown native command list");
      return nullptr;
    }
    return &it->second;
  }

  bool relocated_gpu_virtual_address(
      std::uint64_t original_address,
      std::uint64_t sequence,
      D3D12_GPU_VIRTUAL_ADDRESS &relocated,
      std::string &error) const
  {
    relocated = 0;
    D3D12ReplayBackend::GpuVirtualAddressBinding binding;
    if (!backend_.resolve_gpu_virtual_address_at(original_address, sequence, binding, error)) {
      return false;
    }
    if (binding.resource_object_id == 0) {
      return original_address == 0;
    }
    const auto resource_it = resources_.find(binding.resource_object_id);
    if (resource_it == resources_.end()) {
      return fail(error, "GPUVA source resource cannot be relocated");
    }
    relocated = resource_it->second.resource->GetGPUVirtualAddress() + binding.offset;
    return true;
  }

  bool record_command(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    json payload = json::parse(command.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return fail(error, "command payload is not JSON");
    }
    if (!sync_resource_data_until(command.sequence, error) ||
        !sync_descriptors_until(command.sequence, error)) {
      return false;
    }

    switch (command.kind) {
    case D3D12ReplayBackend::ReplayCommandKind::SetPipelineState:
      return record_set_pipeline_state(list, command, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRootSignature:
      return record_set_root_signature(list, command, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetDescriptorHeaps:
      return record_set_descriptor_heaps(list, command, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRootDescriptorTable:
      return record_set_root_descriptor_table(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRootConstants:
      return record_set_root_constants(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRootConstantBufferView:
      return record_set_root_descriptor(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetViewports:
      return record_set_viewports(list, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetScissorRects:
      return record_set_scissors(list, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetRenderTargets:
      return record_set_render_targets(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ClearRenderTarget:
      return record_clear_render_target(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ClearDepthStencil:
      return record_clear_depth_stencil(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ClearUnorderedAccess:
      return record_clear_unordered_access(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::DiscardResource:
      return record_discard_resource(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ResourceBarrier:
      return record_resource_barriers(list, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetDynamicState:
      return record_dynamic_state(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::RenderPass:
      return record_render_pass(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Query:
      return record_query(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Predication:
      return record_predication(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::WriteBufferImmediate:
      return record_write_buffer_immediate(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::TemporalUpscale:
      return fail(error, "TemporalUpscale requires a DXMT-backed replay backend");
    case D3D12ReplayBackend::ReplayCommandKind::UnsupportedNative:
      return fail(error, "D3D12 native command replay incomplete: unsupported command " + command.function_name);
    case D3D12ReplayBackend::ReplayCommandKind::SetPrimitiveTopology:
      list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(payload_u32(payload, "primitive_topology")));
      return true;
    case D3D12ReplayBackend::ReplayCommandKind::SetVertexBuffers:
      return record_set_vertex_buffers(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::SetIndexBuffer:
      return record_set_index_buffer(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Draw:
      return record_draw(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ExecuteIndirect:
      return record_execute_indirect(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::ExecuteBundle:
      return record_execute_bundle(list, command, error);
    case D3D12ReplayBackend::ReplayCommandKind::Copy:
      return record_copy(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Resolve:
      return record_resolve(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::Dispatch:
      return record_dispatch(list, command, payload, error);
    case D3D12ReplayBackend::ReplayCommandKind::MapResource:
    case D3D12ReplayBackend::ReplayCommandKind::UnmapResource:
      return true;
    default:
      return fail(error, "unsupported command " + command.function_name);
    }
  }

  bool record_execute_bundle(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    if (command.object_refs.size() < 2 || command.object_refs[1] == 0) {
      return fail(error, "ExecuteBundle missing bundle command list object id");
    }
    const auto bundle_object_id = command.object_refs[1];
    auto bundle_it = command_lists_.find(bundle_object_id);
    if (bundle_it == command_lists_.end()) {
      return fail(error, "ExecuteBundle references an unknown bundle command list");
    }
    if (bundle_it->second.type != D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      return fail(error, "ExecuteBundle references a non-bundle command list");
    }
    if (!ensure_command_list_recorded(bundle_object_id, command.sequence, error)) {
      return false;
    }
    list->ExecuteBundle(bundle_it->second.list.get());
    return true;
  }

  bool record_query(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.object_refs.size() < 2 || command.object_refs[1] == 0) {
      return fail(error, "query command missing query heap object id");
    }
    const auto heap_it = query_heaps_.find(command.object_refs[1]);
    if (heap_it == query_heaps_.end()) {
      return fail(error, "query command references an unknown query heap");
    }
    const auto type = static_cast<D3D12_QUERY_TYPE>(payload_u32(payload, "type"));
    if (command.function_name == "ID3D12GraphicsCommandList::BeginQuery") {
      list->BeginQuery(heap_it->second.get(), type, payload_u32(payload, "index"));
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::EndQuery") {
      list->EndQuery(heap_it->second.get(), type, payload_u32(payload, "index"));
      return true;
    }
    if (command.object_refs.size() < 3 || command.object_refs[2] == 0) {
      return fail(error, "ResolveQueryData missing destination buffer object id");
    }
    ID3D12Resource *dst = resource_for(command.object_refs[2], error);
    if (!dst) {
      return false;
    }
    list->ResolveQueryData(
        heap_it->second.get(),
        type,
        payload_u32(payload, "start_index"),
        payload_u32(payload, "query_count"),
        dst,
        payload_u64(payload, "aligned_dst_buffer_offset"));
    return true;
  }

  bool record_predication(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    ID3D12Resource *buffer = nullptr;
    if (command.object_refs.size() >= 2 && command.object_refs[1] != 0) {
      buffer = resource_for(command.object_refs[1], error);
      if (!buffer) {
        return false;
      }
    }
    list->SetPredication(
        buffer,
        payload_u64(payload, "aligned_buffer_offset"),
        static_cast<D3D12_PREDICATION_OP>(payload_u32(payload, "operation")));
    return true;
  }

  bool record_write_buffer_immediate(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    auto *native = native_command_list_for(command.command_list_object_id, error);
    if (!native) {
      return false;
    }
    if (!native->list2) {
      return fail(error, "WriteBufferImmediate requires ID3D12GraphicsCommandList2");
    }
    const auto writes = payload.find("writes");
    if (writes == payload.end() || !writes->is_array()) {
      return fail(error, "WriteBufferImmediate missing writes array");
    }
    std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> parameters;
    std::vector<D3D12_WRITEBUFFERIMMEDIATE_MODE> modes;
    parameters.reserve(writes->size());
    modes.reserve(writes->size());
    bool all_default_modes = true;
    for (const auto &write : *writes) {
      if (!write.is_object()) {
        return fail(error, "WriteBufferImmediate write entry must be an object");
      }
      D3D12_GPU_VIRTUAL_ADDRESS relocated = 0;
      if (!relocated_gpu_virtual_address(payload_u64(write, "dest"), command.sequence, relocated, error)) {
        return false;
      }
      D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter{};
      parameter.Dest = relocated;
      parameter.Value = payload_u32(write, "value");
      parameters.push_back(parameter);
      D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
      const auto mode_it = write.find("mode");
      if (mode_it != write.end() && !mode_it->is_null()) {
        mode = static_cast<D3D12_WRITEBUFFERIMMEDIATE_MODE>(json_u64(*mode_it));
      }
      if (mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT) {
        all_default_modes = false;
      }
      modes.push_back(mode);
    }
    if (payload_u32(payload, "count") != parameters.size()) {
      return fail(error, "WriteBufferImmediate count does not match writes array");
    }
    native->list2->WriteBufferImmediate(
        static_cast<UINT>(parameters.size()),
        parameters.data(),
        all_default_modes ? nullptr : modes.data());
    (void)list;
    return true;
  }

  bool record_set_pipeline_state(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    if (command.object_refs.size() < 2) {
      return fail(error, "SetPipelineState missing PSO object");
    }
    const auto pipeline_it = pipelines_.find(command.object_refs[1]);
    if (pipeline_it == pipelines_.end()) {
      return fail(error, "SetPipelineState references an unknown PSO");
    }
    list->SetPipelineState(pipeline_it->second.get());
    return true;
  }

  bool record_set_root_signature(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    if (command.object_refs.size() < 2) {
      return fail(error, "SetRootSignature missing root signature object");
    }
    const auto root_it = root_signatures_.find(command.object_refs[1]);
    if (root_it == root_signatures_.end()) {
      return fail(error, "SetRootSignature references an unknown root signature");
    }
    if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootSignature") {
      list->SetComputeRootSignature(root_it->second.get());
    } else {
      list->SetGraphicsRootSignature(root_it->second.get());
    }
    return true;
  }

  bool record_set_descriptor_heaps(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    std::vector<ID3D12DescriptorHeap *> heaps;
    for (std::size_t index = 1; index < command.object_refs.size(); ++index) {
      const auto heap_it = descriptor_heaps_.find(command.object_refs[index]);
      if (heap_it == descriptor_heaps_.end()) {
        return fail(error, "SetDescriptorHeaps references an unknown heap");
      }
      heaps.push_back(heap_it->second.heap.get());
    }
    list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
    return true;
  }

  bool record_set_root_descriptor_table(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
    std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(payload_u64(payload, "base_descriptor"), true, command.sequence, binding, error)) {
      return false;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    if (!descriptor_gpu_handle(binding, handle, error)) {
      return false;
    }
    const UINT root_parameter_index = payload_u32(payload, "root_parameter_index");
    if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable") {
      list->SetComputeRootDescriptorTable(root_parameter_index, handle);
    } else {
      list->SetGraphicsRootDescriptorTable(root_parameter_index, handle);
    }
    return true;
  }

  bool record_set_root_constants(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    const auto values_json = payload.find("values");
    if (values_json == payload.end() || !values_json->is_array()) {
      return fail(error, "root constant command missing values");
    }
    std::vector<UINT> values;
    values.reserve(values_json->size());
    for (const auto &value : *values_json) {
      values.push_back(static_cast<UINT>(json_u64(value)));
    }
    const UINT root_parameter_index = payload_u32(payload, "root_parameter_index");
    const UINT dst_offset = payload_u32(payload, "dst_offset");
    if (command.function_name.find("Compute") != std::string::npos) {
      list->SetComputeRoot32BitConstants(root_parameter_index, static_cast<UINT>(values.size()), values.data(), dst_offset);
    } else {
      list->SetGraphicsRoot32BitConstants(root_parameter_index, static_cast<UINT>(values.size()), values.data(), dst_offset);
    }
    return true;
  }

  bool record_set_root_descriptor(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    D3D12ReplayBackend::GpuVirtualAddressBinding binding;
    if (!backend_.resolve_gpu_virtual_address_at(payload_u64(payload, "buffer_location"), command.sequence, binding, error)) {
      return false;
    }
    if (binding.gpu_virtual_address != 0 && !binding.resolved) {
      return fail(error, "root descriptor GPUVA cannot be relocated");
    }
    const auto resource_it = resources_.find(binding.resource_object_id);
    if (binding.resource_object_id != 0 && resource_it == resources_.end()) {
      return fail(error, "root descriptor resource cannot be relocated");
    }
    const D3D12_GPU_VIRTUAL_ADDRESS address =
        binding.resource_object_id == 0 ? 0 : resource_it->second.resource->GetGPUVirtualAddress() + binding.offset;
    const UINT root_parameter_index = payload_u32(payload, "root_parameter_index");
    if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView") {
      list->SetComputeRootConstantBufferView(root_parameter_index, address);
    } else if (command.function_name == "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView") {
      list->SetGraphicsRootShaderResourceView(root_parameter_index, address);
    } else if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView") {
      list->SetComputeRootShaderResourceView(root_parameter_index, address);
    } else if (command.function_name == "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView") {
      list->SetGraphicsRootUnorderedAccessView(root_parameter_index, address);
    } else if (command.function_name == "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView") {
      list->SetComputeRootUnorderedAccessView(root_parameter_index, address);
    } else {
      list->SetGraphicsRootConstantBufferView(root_parameter_index, address);
    }
    return true;
  }

  bool record_set_viewports(ID3D12GraphicsCommandList *list, const json &payload, std::string &error)
  {
    std::vector<D3D12_VIEWPORT> viewports;
    const auto append_viewport = [&](const json &viewport_json) {
      D3D12_VIEWPORT viewport{};
      viewport.TopLeftX = payload_float(viewport_json, "x");
      viewport.TopLeftY = payload_float(viewport_json, "y");
      viewport.Width = payload_float(viewport_json, "width");
      viewport.Height = payload_float(viewport_json, "height");
      viewport.MinDepth = payload_float(viewport_json, "min_depth");
      viewport.MaxDepth = payload_float(viewport_json, "max_depth");
      viewports.push_back(viewport);
    };
    const auto viewport_count = payload_u32(payload, "viewport_count");
    const auto viewports_json = payload.find("viewports");
    if (viewports_json != payload.end()) {
      if (!viewports_json->is_array() || viewports_json->size() != viewport_count) {
        return fail(error, "RSSetViewports viewports count does not match viewport_count");
      }
      viewports.reserve(viewports_json->size());
      for (const auto &viewport_json : *viewports_json) {
        append_viewport(viewport_json);
      }
    } else if (viewport_count == 0) {
      viewports.clear();
    } else if (viewport_count == 1) {
      const auto first = payload.find("first");
      if (first == payload.end() || !first->is_object()) {
        return fail(error, "RSSetViewports missing viewport payload");
      }
      append_viewport(*first);
    } else {
      return fail(error, "RSSetViewports first viewport cannot cover multiple viewports");
    }
    list->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
    (void)error;
    return true;
  }

  bool record_set_scissors(ID3D12GraphicsCommandList *list, const json &payload, std::string &error)
  {
    std::vector<D3D12_RECT> rects;
    const auto append_rect = [&](const json &rect_json) {
      D3D12_RECT rect{};
      rect.left = payload_i32(rect_json, "left");
      rect.top = payload_i32(rect_json, "top");
      rect.right = payload_i32(rect_json, "right");
      rect.bottom = payload_i32(rect_json, "bottom");
      rects.push_back(rect);
    };
    const auto rect_count = payload_u32(payload, "rect_count");
    const auto rects_json = payload.find("rects");
    if (rects_json != payload.end()) {
      if (!rects_json->is_array() || rects_json->size() != rect_count) {
        return fail(error, "RSSetScissorRects rects count does not match rect_count");
      }
      rects.reserve(rects_json->size());
      for (const auto &rect_json : *rects_json) {
        append_rect(rect_json);
      }
    } else if (rect_count == 0) {
      rects.clear();
    } else if (rect_count == 1) {
      const auto first = payload.find("first");
      if (first == payload.end() || !first->is_object()) {
        return fail(error, "RSSetScissorRects missing rect payload");
      }
      append_rect(*first);
    } else {
      return fail(error, "RSSetScissorRects first rect cannot cover multiple rects");
    }
    list->RSSetScissorRects(static_cast<UINT>(rects.size()), rects.data());
    return true;
  }

  bool record_set_render_targets(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (!payload_has(payload, "render_target_count") || !payload_has(payload, "dsv")) {
      return fail(error, "OMSetRenderTargets missing render target count or DSV payload");
    }
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> handles;
    const auto render_target_count = payload_u32(payload, "render_target_count");
    const auto render_targets = payload.find("render_targets");
    const bool has_render_target_array = render_targets != payload.end();
    if (has_render_target_array && !render_targets->is_array()) {
      return fail(error, "OMSetRenderTargets render target payload must be an array");
    }
    if (!has_render_target_array && render_target_count > 1) {
      return fail(error, "OMSetRenderTargets first RTV cannot cover multiple render targets");
    }
    if (!has_render_target_array && render_target_count == 1 && payload_u64(payload, "first_rtv") == 0) {
      return fail(error, "OMSetRenderTargets missing render target payload");
    }
    const auto array_size = has_render_target_array ? render_targets->size() : std::size_t{};
    const auto single_range =
        has_render_target_array &&
        (payload_bool(payload, "single_handle_to_descriptor_range") || payload_bool(payload, "single_descriptor_handle")) &&
        array_size == 1 &&
        render_target_count > 1;
    D3D12ReplayBackend::DescriptorBinding first_binding;
    UINT descriptor_size = 0;
    if (single_range) {
      if (!backend_.resolve_descriptor_binding_at(json_u64((*render_targets)[0]), false, command.sequence, first_binding, error)) {
        return false;
      }
      const auto heap_it = descriptor_heaps_.find(first_binding.heap_object_id);
      if (heap_it == descriptor_heaps_.end() || heap_it->second.descriptor_size == 0) {
        return fail(error, "OMSetRenderTargets first RTV cannot be expanded");
      }
      descriptor_size = heap_it->second.descriptor_size;
    }
    for (std::uint32_t index = 0; index < render_target_count; ++index) {
      if (has_render_target_array && !single_range && index >= render_targets->size()) {
        return fail(error, "OMSetRenderTargets render target count exceeds descriptor array");
      }
      const auto descriptor = single_range
                                  ? first_binding.descriptor + static_cast<std::uint64_t>(index) * descriptor_size
                                  : has_render_target_array ? json_u64((*render_targets)[index]) : payload_u64(payload, "first_rtv");
      D3D12ReplayBackend::DescriptorBinding binding;
      if (!backend_.resolve_descriptor_binding_at(descriptor, false, command.sequence, binding, error)) {
        return false;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE handle{};
      if (!descriptor_handle(binding, handle, error)) {
        return false;
      }
      handles.push_back(handle);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle{};
    const D3D12_CPU_DESCRIPTOR_HANDLE *dsv_ptr = nullptr;
    const auto dsv_descriptor = payload_u64(payload, "dsv");
    if (dsv_descriptor != 0) {
      D3D12ReplayBackend::DescriptorBinding dsv_binding;
      if (!backend_.resolve_descriptor_binding_at(dsv_descriptor, false, command.sequence, dsv_binding, error)) {
        return false;
      }
      if (!descriptor_handle(dsv_binding, dsv_handle, error)) {
        return false;
      }
      dsv_ptr = &dsv_handle;
    }
    list->OMSetRenderTargets(
        static_cast<UINT>(handles.size()),
        handles.empty() ? nullptr : handles.data(),
        single_range,
        dsv_ptr);
    return true;
  }

  bool record_clear_render_target(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(payload_u64(payload, "descriptor"), false, command.sequence, binding, error)) {
      return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!descriptor_handle(binding, handle, error)) {
      return false;
    }
    const auto color_json = payload.find("color");
    if (color_json == payload.end() || !color_json->is_array() || color_json->size() != 4) {
      return fail(error, "ClearRenderTargetView missing color");
    }
    FLOAT color[4] = {
        (*color_json)[0].get<float>(),
        (*color_json)[1].get<float>(),
        (*color_json)[2].get<float>(),
        (*color_json)[3].get<float>()};
    const auto rects = d3d12_rects_from_payload(payload);
    list->ClearRenderTargetView(
        handle,
        color,
        static_cast<UINT>(rects.size()),
        rects.empty() ? nullptr : rects.data());
    return true;
  }

  bool record_clear_depth_stencil(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(payload_u64(payload, "descriptor"), false, command.sequence, binding, error)) {
      return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!descriptor_handle(binding, handle, error)) {
      return false;
    }
    const auto rects = d3d12_rects_from_payload(payload);
    list->ClearDepthStencilView(
        handle,
        static_cast<D3D12_CLEAR_FLAGS>(payload_u32(payload, "clear_flags")),
        payload_float(payload, "depth"),
        static_cast<UINT8>(payload_u32(payload, "stencil")),
        static_cast<UINT>(rects.size()),
        rects.empty() ? nullptr : rects.data());
    return true;
  }

  bool clear_render_pass_render_target(
      ID3D12GraphicsCommandList *list,
      std::uint64_t descriptor,
      std::uint64_t sequence,
      const json &clear,
      std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(descriptor, false, sequence, binding, error)) {
      return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!descriptor_handle(binding, handle, error)) {
      return false;
    }
    const auto color_json = clear.find("color");
    if (color_json == clear.end() || !color_json->is_array() || color_json->size() != 4) {
      return fail(error, "render pass clear color is incomplete");
    }
    FLOAT color[4] = {
        (*color_json)[0].get<float>(),
        (*color_json)[1].get<float>(),
        (*color_json)[2].get<float>(),
        (*color_json)[3].get<float>()};
    list->ClearRenderTargetView(handle, color, 0, nullptr);
    return true;
  }

  bool clear_render_pass_depth_stencil(
      ID3D12GraphicsCommandList *list,
      std::uint64_t descriptor,
      std::uint64_t sequence,
      const json &clear,
      D3D12_CLEAR_FLAGS flags,
      std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(descriptor, false, sequence, binding, error)) {
      return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!descriptor_handle(binding, handle, error)) {
      return false;
    }
    list->ClearDepthStencilView(
        handle,
        flags,
        payload_float(clear, "depth"),
        static_cast<UINT8>(payload_u32(clear, "stencil")),
        0,
        nullptr);
    return true;
  }

  bool record_clear_unordered_access(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    D3D12ReplayBackend::DescriptorBinding gpu_binding;
    if (!backend_.resolve_descriptor_binding_at(payload_u64(payload, "gpu_descriptor"), true, command.sequence, gpu_binding, error)) {
      return false;
    }
    D3D12ReplayBackend::DescriptorBinding cpu_binding;
    if (!backend_.resolve_descriptor_binding_at(payload_u64(payload, "cpu_descriptor"), false, command.sequence, cpu_binding, error)) {
      return false;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
    if (!descriptor_gpu_handle(gpu_binding, gpu_handle, error)) {
      return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle{};
    if (!descriptor_handle(cpu_binding, cpu_handle, error)) {
      return false;
    }
    if (command.object_refs.size() < 2 || command.object_refs[1] == 0) {
      return fail(error, "ClearUnorderedAccessView missing resource ref");
    }
    ID3D12Resource *resource = resource_for(command.object_refs[1], error);
    if (!resource) {
      return false;
    }
    const auto rects_json = payload.find("rects");
    std::vector<D3D12_RECT> rects;
    if (rects_json != payload.end()) {
      if (!rects_json->is_array()) {
        return fail(error, "ClearUnorderedAccessView rects must be an array");
      }
      for (const auto &rect_json : *rects_json) {
        D3D12_RECT rect{};
        rect.left = payload_i32(rect_json, "left");
        rect.top = payload_i32(rect_json, "top");
        rect.right = payload_i32(rect_json, "right");
        rect.bottom = payload_i32(rect_json, "bottom");
        rects.push_back(rect);
      }
    }
    if (command.function_name == "ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint") {
      const auto values_json = payload.find("values");
      if (values_json == payload.end() || !values_json->is_array() || values_json->size() != 4) {
        return fail(error, "ClearUnorderedAccessViewUint missing values");
      }
      UINT values[4] = {
          static_cast<UINT>((*values_json)[0].get<std::uint32_t>()),
          static_cast<UINT>((*values_json)[1].get<std::uint32_t>()),
          static_cast<UINT>((*values_json)[2].get<std::uint32_t>()),
          static_cast<UINT>((*values_json)[3].get<std::uint32_t>())};
      list->ClearUnorderedAccessViewUint(
          gpu_handle,
          cpu_handle,
          resource,
          values,
          static_cast<UINT>(rects.size()),
          rects.empty() ? nullptr : rects.data());
    } else {
      const auto values_json = payload.find("values");
      if (values_json == payload.end() || !values_json->is_array() || values_json->size() != 4) {
        return fail(error, "ClearUnorderedAccessViewFloat missing values");
      }
      FLOAT values[4] = {
          (*values_json)[0].get<float>(),
          (*values_json)[1].get<float>(),
          (*values_json)[2].get<float>(),
          (*values_json)[3].get<float>()};
      list->ClearUnorderedAccessViewFloat(
          gpu_handle,
          cpu_handle,
          resource,
          values,
          static_cast<UINT>(rects.size()),
          rects.empty() ? nullptr : rects.data());
    }
    return true;
  }

  bool record_discard_resource(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.object_refs.size() < 2 || command.object_refs[1] == 0) {
      return fail(error, "DiscardResource missing resource ref");
    }
    ID3D12Resource *resource = resource_for(command.object_refs[1], error);
    if (!resource) {
      return false;
    }
    D3D12_DISCARD_REGION region{};
    std::vector<D3D12_RECT> rects;
    const auto rects_json = payload.find("rects");
    if (rects_json != payload.end()) {
      if (!rects_json->is_array()) {
        return fail(error, "DiscardResource rects must be an array");
      }
      for (const auto &rect_json : *rects_json) {
        D3D12_RECT rect{};
        rect.left = payload_i32(rect_json, "left");
        rect.top = payload_i32(rect_json, "top");
        rect.right = payload_i32(rect_json, "right");
        rect.bottom = payload_i32(rect_json, "bottom");
        rects.push_back(rect);
      }
    }
    region.NumRects = static_cast<UINT>(rects.size());
    region.pRects = rects.empty() ? nullptr : rects.data();
    region.FirstSubresource = payload_u32(payload, "first_subresource");
    region.NumSubresources = payload_u32(payload, "subresource_count");
    const bool has_region = region.NumSubresources != 0 || !rects.empty();
    list->DiscardResource(resource, has_region ? &region : nullptr);
    return true;
  }

  bool record_resource_barriers(ID3D12GraphicsCommandList *list, const json &payload, std::string &error)
  {
    const auto barriers_json = payload.find("barriers");
    if (barriers_json == payload.end() || !barriers_json->is_array()) {
      return fail(error, "ResourceBarrier missing barrier array");
    }
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (const auto &barrier_entry : *barriers_json) {
      json compact_barrier_storage;
      const json *barrier_json = &barrier_entry;
      if (barrier_entry.is_array()) {
        if (barrier_entry.size() < 9) {
          return fail(error, "ResourceBarrierBatch op must be a 9-column array");
        }
        compact_barrier_storage = {
            {"type", json_u64(barrier_entry[1])},
            {"flags", json_u64(barrier_entry[2])},
            {"resource_object_id", json_u64(barrier_entry[3])},
            {"before", json_u64(barrier_entry[4])},
            {"after", json_u64(barrier_entry[5])},
            {"subresource", json_u64(barrier_entry[6])},
            {"resource_before_object_id", json_u64(barrier_entry[7])},
            {"resource_after_object_id", json_u64(barrier_entry[8])},
        };
        barrier_json = &compact_barrier_storage;
      }
      if (!barrier_json->is_object()) {
        return fail(error, "ResourceBarrier entry must be an object or compact array");
      }
      if (!payload_has(*barrier_json, "type") || !payload_has(*barrier_json, "flags")) {
        return fail(error, "ResourceBarrier missing barrier type or flags");
      }
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = static_cast<D3D12_RESOURCE_BARRIER_TYPE>(payload_u32(*barrier_json, "type"));
      barrier.Flags = static_cast<D3D12_RESOURCE_BARRIER_FLAGS>(payload_u32(*barrier_json, "flags"));
      if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        if (!payload_has(*barrier_json, "resource_object_id") ||
            !payload_has(*barrier_json, "before") ||
            !payload_has(*barrier_json, "after") ||
            !payload_has(*barrier_json, "subresource")) {
          return fail(error, "ResourceBarrier missing transition payload");
        }
        ID3D12Resource *resource = resource_for(json_object_id((*barrier_json)["resource_object_id"]), error);
        if (!resource) {
          return false;
        }
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = payload_u32(*barrier_json, "subresource");
        barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(payload_u32(*barrier_json, "before"));
        barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(payload_u32(*barrier_json, "after"));
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        if (!payload_has(*barrier_json, "resource_before_object_id") ||
            !payload_has(*barrier_json, "resource_after_object_id")) {
          return fail(error, "ResourceBarrier missing aliasing payload");
        }
        const auto before_id = json_object_id((*barrier_json)["resource_before_object_id"]);
        const auto after_id = json_object_id((*barrier_json)["resource_after_object_id"]);
        barrier.Aliasing.pResourceBefore = before_id == 0 ? nullptr : resource_for(before_id, error);
        if (before_id != 0 && !barrier.Aliasing.pResourceBefore) {
          return false;
        }
        barrier.Aliasing.pResourceAfter = after_id == 0 ? nullptr : resource_for(after_id, error);
        if (after_id != 0 && !barrier.Aliasing.pResourceAfter) {
          return false;
        }
      } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
        if (!payload_has(*barrier_json, "resource_object_id")) {
          return fail(error, "ResourceBarrier missing UAV payload");
        }
        const auto resource_id = json_object_id((*barrier_json)["resource_object_id"]);
        barrier.UAV.pResource = resource_id == 0 ? nullptr : resource_for(resource_id, error);
        if (resource_id != 0 && !barrier.UAV.pResource) {
          return false;
        }
      } else {
        return fail(error, "unsupported resource barrier type");
      }
      barriers.push_back(barrier);
    }
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    return true;
  }

  bool record_dynamic_state(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.function_name == "ID3D12GraphicsCommandList::ClearState") {
      ID3D12PipelineState *pipeline = nullptr;
      if (command.object_refs.size() >= 2 && command.object_refs[1] != 0) {
        const auto pipeline_it = pipelines_.find(command.object_refs[1]);
        if (pipeline_it == pipelines_.end()) {
          return fail(error, "ClearState references an unknown PSO");
        }
        pipeline = pipeline_it->second.get();
      }
      list->ClearState(pipeline);
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::OMSetBlendFactor") {
      const auto blend = payload.find("blend_factor");
      if (blend == payload.end() || !blend->is_array() || blend->size() != 4) {
        return fail(error, "OMSetBlendFactor missing blend_factor array");
      }
      FLOAT blend_factor[4] = {
          (*blend)[0].get<float>(),
          (*blend)[1].get<float>(),
          (*blend)[2].get<float>(),
          (*blend)[3].get<float>()};
      list->OMSetBlendFactor(blend_factor);
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::OMSetStencilRef") {
      list->OMSetStencilRef(payload_u32(payload, "stencil_ref"));
      return true;
    }
    return fail(error, "dynamic state command is not covered: " + command.function_name);
  }

  bool read_src_rect(const json &payload, D3D12_RECT &rect, const D3D12_RECT **rect_ptr, std::string &error) const
  {
    *rect_ptr = nullptr;
    const auto rect_json = payload.find("src_rect");
    if (rect_json == payload.end() || rect_json->is_null()) {
      return true;
    }
    if (!rect_json->is_object()) {
      return fail(error, "ResolveSubresourceRegion src_rect must be null or an object");
    }
    rect.left = payload_i32(*rect_json, "left");
    rect.top = payload_i32(*rect_json, "top");
    rect.right = payload_i32(*rect_json, "right");
    rect.bottom = payload_i32(*rect_json, "bottom");
    *rect_ptr = &rect;
    return true;
  }

  bool can_fallback_to_resolve_subresource(
      trace::ObjectId src_resource_id,
      std::uint32_t dst_x,
      std::uint32_t dst_y,
      const D3D12_RECT *src_rect,
      std::uint32_t mode,
      std::string &error) const
  {
    constexpr std::uint32_t kResolveModeAverage = 3;
    if (mode != kResolveModeAverage) {
      return fail(error, "D3D12 native command replay incomplete: ResolveSubresourceRegion mode requires ID3D12GraphicsCommandList1");
    }
    if (dst_x != 0 || dst_y != 0) {
      return fail(error, "D3D12 native command replay incomplete: ResolveSubresourceRegion destination offset requires ID3D12GraphicsCommandList1");
    }
    if (!src_rect) {
      return true;
    }
    const auto resource_it = backend_.resources_.find(src_resource_id);
    if (resource_it == backend_.resources_.end()) {
      return fail(error, "ResolveSubresourceRegion references an unknown source resource");
    }
    const auto &resource = resource_it->second;
    if (src_rect->left == 0 &&
        src_rect->top == 0 &&
        src_rect->right == static_cast<LONG>(resource.width) &&
        src_rect->bottom == static_cast<LONG>(resource.height)) {
      return true;
    }
    return fail(error, "D3D12 native command replay incomplete: ResolveSubresourceRegion source rect requires ID3D12GraphicsCommandList1");
  }

  bool record_resolve_subresource_region(
      ID3D12GraphicsCommandList *list,
      ID3D12GraphicsCommandList1 *list1,
      ID3D12Resource *dst,
      std::uint32_t dst_subresource,
      std::uint32_t dst_x,
      std::uint32_t dst_y,
      ID3D12Resource *src,
      trace::ObjectId src_resource_id,
      std::uint32_t src_subresource,
      const D3D12_RECT *src_rect,
      std::uint32_t format,
      std::uint32_t mode,
      std::string &error) const
  {
    if (list1) {
      list1->ResolveSubresourceRegion(
          dst,
          dst_subresource,
          dst_x,
          dst_y,
          src,
          src_subresource,
          const_cast<D3D12_RECT *>(src_rect),
          static_cast<DXGI_FORMAT>(format),
          static_cast<D3D12_RESOLVE_MODE>(mode));
      return true;
    }
    if (!can_fallback_to_resolve_subresource(src_resource_id, dst_x, dst_y, src_rect, mode, error)) {
      return false;
    }
    list->ResolveSubresource(
        dst,
        dst_subresource,
        src,
        src_subresource,
        static_cast<DXGI_FORMAT>(format));
    return true;
  }

  bool record_render_pass_resolve(
      ID3D12GraphicsCommandList *list,
      ID3D12GraphicsCommandList1 *list1,
      const json &access,
      std::string &error)
  {
    const auto src_resource_id = json_object_id(access.value("src_resource_object_id", json(nullptr)));
    const auto dst_resource_id = json_object_id(access.value("dst_resource_object_id", json(nullptr)));
    if (src_resource_id == 0 || dst_resource_id == 0) {
      return fail(error, "render pass resolve missing source or destination resource");
    }
    ID3D12Resource *dst = resource_for(dst_resource_id, error);
    ID3D12Resource *src = resource_for(src_resource_id, error);
    if (!dst || !src) {
      return false;
    }
    const auto subresources = access.find("subresources");
    if (subresources == access.end() || !subresources->is_array()) {
      return fail(error, "render pass resolve missing subresources array");
    }
    if (subresources->size() != payload_u64(access, "subresource_count")) {
      return fail(error, "render pass resolve subresource_count does not match subresources payload");
    }
    const auto format = payload_u32(access, "format");
    const auto mode = payload_u32(access, "resolve_mode");
    for (const auto &subresource : *subresources) {
      D3D12_RECT src_rect{};
      const D3D12_RECT *src_rect_ptr = nullptr;
      if (!read_src_rect(subresource, src_rect, &src_rect_ptr, error)) {
        return false;
      }
      if (!record_resolve_subresource_region(
              list,
              list1,
              dst,
              payload_u32(subresource, "dst_subresource"),
              payload_u32(subresource, "dst_x"),
              payload_u32(subresource, "dst_y"),
              src,
              src_resource_id,
              payload_u32(subresource, "src_subresource"),
              src_rect_ptr,
              format,
              mode,
              error)) {
        return false;
      }
    }
    return true;
  }

  bool record_render_pass(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    auto *native = native_command_list_for(command.command_list_object_id, error);
    if (!native) {
      return false;
    }
    if (command.function_name == "ID3D12GraphicsCommandList4::EndRenderPass") {
      for (const auto &resolve_access : native->pending_render_pass_resolves) {
        if (!record_render_pass_resolve(list, native->list1.get(), resolve_access, error)) {
          return false;
        }
      }
      native->pending_render_pass_resolves.clear();
      return true;
    }
    if (command.function_name != "ID3D12GraphicsCommandList4::BeginRenderPass") {
      return fail(error, "render pass command is not covered: " + command.function_name);
    }

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> render_target_handles;
    const auto render_targets = payload.find("render_targets");
    if (render_targets != payload.end() && render_targets->is_array()) {
      render_target_handles.reserve(render_targets->size());
      for (const auto &render_target : *render_targets) {
        const auto descriptor = payload_u64(render_target, "cpu_descriptor");
        D3D12ReplayBackend::DescriptorBinding binding;
        if (descriptor != 0 &&
            !backend_.resolve_descriptor_binding_at(descriptor, false, command.sequence, binding, error)) {
          return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        if (!descriptor_handle(binding, handle, error)) {
          return false;
        }
        render_target_handles.push_back(handle);
      }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_handle{};
    const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_ptr = nullptr;
    const auto depth_stencil = payload.find("depth_stencil");
    if (depth_stencil != payload.end() && depth_stencil->is_object() &&
        payload_u64(*depth_stencil, "cpu_descriptor") != 0) {
      D3D12ReplayBackend::DescriptorBinding binding;
      if (!backend_.resolve_descriptor_binding_at(
              payload_u64(*depth_stencil, "cpu_descriptor"),
              false,
              command.sequence,
              binding,
              error)) {
        return false;
      }
      if (!descriptor_handle(binding, depth_stencil_handle, error)) {
        return false;
      }
      depth_stencil_ptr = &depth_stencil_handle;
    }

    list->OMSetRenderTargets(
        static_cast<UINT>(render_target_handles.size()),
        render_target_handles.empty() ? nullptr : render_target_handles.data(),
        FALSE,
        depth_stencil_ptr);

    if (render_targets != payload.end() && render_targets->is_array()) {
      for (const auto &render_target : *render_targets) {
        const auto beginning_access = render_target.find("beginning_access");
        if (beginning_access != render_target.end() &&
            payload_u32(*beginning_access, "type") == 2) {
          const auto clear = beginning_access->find("clear");
          if (clear == beginning_access->end() || !clear->is_object()) {
            return fail(error, "render pass RTV clear missing clear payload");
          }
          if (!clear_render_pass_render_target(
                  list,
                  payload_u64(render_target, "cpu_descriptor"),
                  command.sequence,
                  *clear,
                  error)) {
            return false;
          }
        }
        const auto ending_access = render_target.find("ending_access");
        if (ending_access != render_target.end() && ending_access->is_object() &&
            payload_u32(*ending_access, "type") == 2) {
          native->pending_render_pass_resolves.push_back(*ending_access);
        }
      }
    }

    if (depth_stencil != payload.end() && depth_stencil->is_object()) {
      D3D12_CLEAR_FLAGS flags = static_cast<D3D12_CLEAR_FLAGS>(0);
      const json *depth_clear = nullptr;
      const json *stencil_clear = nullptr;
      const auto depth_begin = depth_stencil->find("depth_beginning_access");
      if (depth_begin != depth_stencil->end() && payload_u32(*depth_begin, "type") == 2) {
        flags = static_cast<D3D12_CLEAR_FLAGS>(flags | D3D12_CLEAR_FLAG_DEPTH);
        const auto clear = depth_begin->find("clear");
        if (clear == depth_begin->end() || !clear->is_object()) {
          return fail(error, "render pass depth clear missing clear payload");
        }
        depth_clear = &*clear;
      }
      const auto stencil_begin = depth_stencil->find("stencil_beginning_access");
      if (stencil_begin != depth_stencil->end() && payload_u32(*stencil_begin, "type") == 2) {
        flags = static_cast<D3D12_CLEAR_FLAGS>(flags | D3D12_CLEAR_FLAG_STENCIL);
        const auto clear = stencil_begin->find("clear");
        if (clear == stencil_begin->end() || !clear->is_object()) {
          return fail(error, "render pass stencil clear missing clear payload");
        }
        stencil_clear = &*clear;
      }
      if (flags != 0) {
        const json &clear = depth_clear ? *depth_clear : *stencil_clear;
        if (!clear_render_pass_depth_stencil(
                list,
                payload_u64(*depth_stencil, "cpu_descriptor"),
                command.sequence,
                clear,
                flags,
                error)) {
          return false;
        }
      }
      const auto depth_end = depth_stencil->find("depth_ending_access");
      if (depth_end != depth_stencil->end() && depth_end->is_object() &&
          payload_u32(*depth_end, "type") == 2) {
        native->pending_render_pass_resolves.push_back(*depth_end);
      }
      const auto stencil_end = depth_stencil->find("stencil_ending_access");
      if (stencil_end != depth_stencil->end() && stencil_end->is_object() &&
          payload_u32(*stencil_end, "type") == 2) {
        native->pending_render_pass_resolves.push_back(*stencil_end);
      }
    }
    return true;
  }

  bool record_set_vertex_buffers(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    std::vector<D3D12_VERTEX_BUFFER_VIEW> views;
    const auto append_view = [&](const json &view_json) -> bool {
      D3D12_VERTEX_BUFFER_VIEW view{};
      view.SizeInBytes = payload_u32(view_json, "size_in_bytes");
      view.StrideInBytes = payload_u32(view_json, "stride_in_bytes");
      const auto buffer_location = payload_u64(view_json, "buffer_location");
      if (buffer_location == 0) {
        views.push_back(view);
        return true;
      }
      D3D12ReplayBackend::GpuVirtualAddressBinding binding;
      if (!backend_.resolve_gpu_virtual_address_at(buffer_location, command.sequence, binding, error)) {
        return false;
      }
      const auto resource_it = resources_.find(binding.resource_object_id);
      if (resource_it == resources_.end()) {
        return fail(error, "vertex buffer resource cannot be relocated");
      }
      view.BufferLocation = resource_it->second.resource->GetGPUVirtualAddress() + binding.offset;
      views.push_back(view);
      return true;
    };

    const auto view_count = payload_u64(payload, "view_count");
    const auto views_json = payload.find("views");
    if (views_json != payload.end()) {
      if (!views_json->is_array()) {
        return fail(error, "IASetVertexBuffers view payload must be an array");
      }
      if (views_json->size() != view_count) {
        return fail(error, "IASetVertexBuffers views count does not match view_count");
      }
      views.reserve(views_json->size());
      for (const auto &view_json : *views_json) {
        if (!append_view(view_json)) {
          return false;
        }
      }
    } else if (view_count == 0) {
      views.clear();
    } else if (view_count == 1) {
      const auto first_json = payload.find("first");
      if (first_json == payload.end() || !first_json->is_object()) {
        return fail(error, "IASetVertexBuffers missing view payload");
      }
      if (!append_view(*first_json)) {
        return false;
      }
    } else {
      return fail(error, "IASetVertexBuffers first view cannot cover multiple vertex buffers");
    }
    list->IASetVertexBuffers(payload_u32(payload, "start_slot"), static_cast<UINT>(views.size()), views.data());
    return true;
  }

  bool record_set_index_buffer(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (payload.empty() || payload_u64(payload, "buffer_location") == 0) {
      list->IASetIndexBuffer(nullptr);
      return true;
    }
    D3D12ReplayBackend::GpuVirtualAddressBinding binding;
    if (!backend_.resolve_gpu_virtual_address_at(payload_u64(payload, "buffer_location"), command.sequence, binding, error)) {
      return false;
    }
    const auto resource_it = resources_.find(binding.resource_object_id);
    if (resource_it == resources_.end()) {
      return fail(error, "index buffer resource cannot be relocated");
    }
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = resource_it->second.resource->GetGPUVirtualAddress() + binding.offset;
    view.SizeInBytes = payload_u32(payload, "size_in_bytes");
    view.Format = static_cast<DXGI_FORMAT>(payload_u32(payload, "format"));
    list->IASetIndexBuffer(&view);
    return true;
  }

  bool record_draw(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    (void)error;
    if (command.function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced") {
      list->DrawIndexedInstanced(
          payload_u32(payload, "index_count_per_instance"),
          payload_u32(payload, "instance_count"),
          payload_u32(payload, "start_index_location"),
          payload_i32(payload, "base_vertex_location"),
          payload_u32(payload, "start_instance_location"));
    } else {
      list->DrawInstanced(
          payload_u32(payload, "vertex_count_per_instance"),
          payload_u32(payload, "instance_count"),
          payload_u32(payload, "start_vertex_location"),
          payload_u32(payload, "start_instance_location"));
    }
    return true;
  }

  bool record_execute_indirect(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.object_refs.size() < 3) {
      return fail(error, "ExecuteIndirect missing refs");
    }
    const auto signature_it = command_signatures_.find(command.object_refs[1]);
    if (signature_it == command_signatures_.end()) {
      return fail(error, "ExecuteIndirect references an unknown command signature");
    }
    ID3D12Resource *arg_buffer = resource_for(command.object_refs[2], error);
    if (!arg_buffer) {
      return false;
    }
    ID3D12Resource *count_buffer = nullptr;
    if (command.object_refs.size() >= 4 && command.object_refs[3] != 0) {
      count_buffer = resource_for(command.object_refs[3], error);
      if (!count_buffer) {
        return false;
      }
    }
    list->ExecuteIndirect(
        signature_it->second.get(),
        payload_u32(payload, "max_command_count"),
        arg_buffer,
        payload_u64(payload, "arg_buffer_offset"),
        count_buffer,
        payload_u64(payload, "count_buffer_offset"));
    return true;
  }

  bool texture_copy_location(
      const json &location_json,
      D3D12_TEXTURE_COPY_LOCATION &location,
      std::string &error) const
  {
    location = {};
    ID3D12Resource *resource = resource_for(json_object_id(location_json["resource_object_id"]), error);
    if (!resource) {
      return false;
    }
    location.pResource = resource;
    location.Type = static_cast<D3D12_TEXTURE_COPY_TYPE>(payload_u32(location_json, "type"));
    if (location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      location.SubresourceIndex = payload_u32(location_json, "subresource_index");
    } else if (location.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      const auto footprint = location_json.find("placed_footprint");
      if (footprint == location_json.end() || !footprint->is_object()) {
        return fail(error, "CopyTextureRegion missing placed footprint");
      }
      location.PlacedFootprint.Offset = payload_u64(*footprint, "offset");
      location.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(payload_u32(*footprint, "format"));
      location.PlacedFootprint.Footprint.Width = payload_u32(*footprint, "width");
      location.PlacedFootprint.Footprint.Height = payload_u32(*footprint, "height");
      location.PlacedFootprint.Footprint.Depth = payload_u32(*footprint, "depth");
      location.PlacedFootprint.Footprint.RowPitch = payload_u32(*footprint, "row_pitch");
    } else {
      return fail(error, "unsupported texture copy location type");
    }
    return true;
  }

  bool record_copy(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.function_name == "ID3D12GraphicsCommandList::CopyBufferRegionBatch") {
      const auto ops = payload.find("ops");
      if (ops == payload.end() || !ops->is_array()) {
        return fail(error, "CopyBufferRegionBatch missing ops array");
      }
      for (const auto &op : *ops) {
        if (!op.is_object()) {
          return fail(error, "CopyBufferRegionBatch op is not an object");
        }
        ID3D12Resource *dst = resource_for(json_object_id(op["dst_buffer_object_id"]), error);
        ID3D12Resource *src = resource_for(json_object_id(op["src_buffer_object_id"]), error);
        if (!dst || !src) {
          return false;
        }
        list->CopyBufferRegion(
            dst,
            payload_u64(op, "dst_offset"),
            src,
            payload_u64(op, "src_offset"),
            payload_u64(op, "byte_count"));
      }
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::CopyBufferRegion") {
      if (command.object_refs.size() < 3) {
        return fail(error, "CopyBufferRegion missing buffer refs");
      }
      ID3D12Resource *dst = resource_for(command.object_refs[1], error);
      ID3D12Resource *src = resource_for(command.object_refs[2], error);
      if (!dst || !src) {
        return false;
      }
      list->CopyBufferRegion(
          dst,
          payload_u64(payload, "dst_offset"),
          src,
          payload_u64(payload, "src_offset"),
          payload_u64(payload, "byte_count"));
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::CopyResource") {
      if (command.object_refs.size() < 3) {
        return fail(error, "CopyResource missing resource refs");
      }
      ID3D12Resource *dst = resource_for(command.object_refs[1], error);
      ID3D12Resource *src = resource_for(command.object_refs[2], error);
      if (!dst || !src) {
        return false;
      }
      list->CopyResource(dst, src);
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::CopyTextureRegionBatch") {
      const auto ops = payload.find("ops");
      if (ops == payload.end() || !ops->is_array()) {
        return fail(error, "CopyTextureRegionBatch missing ops");
      }
      for (const auto &op : *ops) {
        if (!op.is_array() || op.size() < 7 ||
            !op[1].is_array() || op[1].size() < 9 ||
            !op[5].is_array() || op[5].size() < 9) {
          return fail(error, "CopyTextureRegionBatch op must use compact array payload");
        }
        const auto compact_texture_copy_location = [&](const json &location, D3D12_TEXTURE_COPY_LOCATION &target) -> bool {
          const auto resource_id = json_object_id(location[0]);
          ID3D12Resource *resource = resource_for(resource_id, error);
          if (!resource) {
            return false;
          }
          target.pResource = resource;
          target.Type = static_cast<D3D12_TEXTURE_COPY_TYPE>(json_u64(location[1]));
          if (target.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            target.PlacedFootprint.Offset = json_u64(location[3]);
            target.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(json_u64(location[4]));
            target.PlacedFootprint.Footprint.Width = static_cast<UINT>(json_u64(location[5]));
            target.PlacedFootprint.Footprint.Height = static_cast<UINT>(json_u64(location[6]));
            target.PlacedFootprint.Footprint.Depth = static_cast<UINT>(json_u64(location[7]));
            target.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(json_u64(location[8]));
          } else if (target.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
            target.SubresourceIndex = static_cast<UINT>(json_u64(location[2]));
          } else {
            return fail(error, "CopyTextureRegionBatch has unsupported texture copy location type");
          }
          return true;
        };
        D3D12_TEXTURE_COPY_LOCATION dst{};
        D3D12_TEXTURE_COPY_LOCATION src{};
        if (!compact_texture_copy_location(op[1], dst) ||
            !compact_texture_copy_location(op[5], src)) {
          return false;
        }
        D3D12_BOX src_box{};
        const D3D12_BOX *src_box_ptr = nullptr;
        if (!op[6].is_null()) {
          if (!op[6].is_array() || op[6].size() < 6) {
            return fail(error, "CopyTextureRegionBatch src_box must be null or a 6-column array");
          }
          src_box.left = static_cast<UINT>(json_u64(op[6][0]));
          src_box.top = static_cast<UINT>(json_u64(op[6][1]));
          src_box.front = static_cast<UINT>(json_u64(op[6][2]));
          src_box.right = static_cast<UINT>(json_u64(op[6][3]));
          src_box.bottom = static_cast<UINT>(json_u64(op[6][4]));
          src_box.back = static_cast<UINT>(json_u64(op[6][5]));
          src_box_ptr = &src_box;
        }
        list->CopyTextureRegion(
            &dst,
            static_cast<UINT>(json_u64(op[2])),
            static_cast<UINT>(json_u64(op[3])),
            static_cast<UINT>(json_u64(op[4])),
            &src,
            src_box_ptr);
      }
      return true;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::CopyTextureRegion") {
      D3D12_TEXTURE_COPY_LOCATION dst{};
      D3D12_TEXTURE_COPY_LOCATION src{};
      if (!texture_copy_location(payload["dst"], dst, error) ||
          !texture_copy_location(payload["src"], src, error)) {
        return false;
      }
      D3D12_BOX src_box{};
      const D3D12_BOX *src_box_ptr = nullptr;
      const auto src_box_json = payload.find("src_box");
      if (src_box_json != payload.end() && src_box_json->is_object()) {
        src_box.left = payload_u32(*src_box_json, "left");
        src_box.top = payload_u32(*src_box_json, "top");
        src_box.front = payload_u32(*src_box_json, "front");
        src_box.right = payload_u32(*src_box_json, "right");
        src_box.bottom = payload_u32(*src_box_json, "bottom");
        src_box.back = payload_u32(*src_box_json, "back");
        src_box_ptr = &src_box;
      }
      list->CopyTextureRegion(
          &dst,
          payload_u32(payload, "dst_x"),
          payload_u32(payload, "dst_y"),
          payload_u32(payload, "dst_z"),
          &src,
          src_box_ptr);
      return true;
    }
    return fail(error, "copy command is not covered: " + command.function_name);
  }

  bool record_resolve(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.object_refs.size() < 3) {
      return fail(error, "ResolveSubresource missing resource refs");
    }
    ID3D12Resource *dst = resource_for(command.object_refs[1], error);
    ID3D12Resource *src = resource_for(command.object_refs[2], error);
    if (!dst || !src) {
      return false;
    }
    if (command.function_name == "ID3D12GraphicsCommandList::ResolveSubresourceRegion") {
      auto *native = native_command_list_for(command.command_list_object_id, error);
      if (!native) {
        return false;
      }
      D3D12_RECT src_rect{};
      const D3D12_RECT *src_rect_ptr = nullptr;
      if (!read_src_rect(payload, src_rect, &src_rect_ptr, error)) {
        return false;
      }
      return record_resolve_subresource_region(
          list,
          native->list1.get(),
          dst,
          payload_u32(payload, "dst_subresource"),
          payload_u32(payload, "dst_x"),
          payload_u32(payload, "dst_y"),
          src,
          command.object_refs[2],
          payload_u32(payload, "src_subresource"),
          src_rect_ptr,
          payload_u32(payload, "format"),
          payload_u32(payload, "mode"),
          error);
    }
    list->ResolveSubresource(
        dst,
        payload_u32(payload, "dst_subresource"),
        src,
        payload_u32(payload, "src_subresource"),
        static_cast<DXGI_FORMAT>(payload_u32(payload, "format")));
    return true;
  }

  bool record_dispatch(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.function_name == "ID3D12GraphicsCommandList6::DispatchMesh") {
#if defined(__ID3D12GraphicsCommandList6_INTERFACE_DEFINED__)
      auto *native = native_command_list_for(command.command_list_object_id, error);
      if (!native) {
        return false;
      }
      if (!native->list6) {
        return fail(error, "DispatchMesh requires ID3D12GraphicsCommandList6");
      }
      native->list6->DispatchMesh(
          payload_u32(payload, "thread_group_count_x"),
          payload_u32(payload, "thread_group_count_y"),
          payload_u32(payload, "thread_group_count_z"));
      (void)list;
      return true;
#else
      return fail(error, "DispatchMesh requires D3D12 headers with ID3D12GraphicsCommandList6");
#endif
    }
    if (command.function_name != "ID3D12GraphicsCommandList::Dispatch") {
      return fail(error, "advanced dispatch command is not covered: " + command.function_name);
    }
    list->Dispatch(
        payload_u32(payload, "thread_group_count_x"),
        payload_u32(payload, "thread_group_count_y"),
        payload_u32(payload, "thread_group_count_z"));
    return true;
  }

  void wait_for_gpu()
  {
    if (!queue_ || !fence_) {
      return;
    }
    const UINT64 value = ++fence_value_;
    if (FAILED(queue_->Signal(fence_.get(), value))) {
      return;
    }
#ifdef _WIN32
    if (fence_->GetCompletedValue() < value && fence_event_ &&
        SUCCEEDED(fence_->SetEventOnCompletion(value, fence_event_))) {
      WaitForSingleObject(fence_event_, INFINITE);
    }
#else
    while (fence_->GetCompletedValue() < value) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
  }

  static constexpr UINT kBufferCount = 2;

  struct ResourceDataUpdateRef {
    std::uint64_t sequence = 0;
    trace::ObjectId resource_object_id = 0;
    const D3D12ReplayBackend::ResourceDataUpdate *update = nullptr;
  };

  const D3D12ReplayBackend &backend_;
  HWND hwnd_ = nullptr;
  UINT present_delay_ms_ = static_cast<UINT>(env_u32("APITRACE_D3D12_RETRACE_PRESENT_DELAY_MS"));
  UINT64 fence_value_ = 0;
#ifdef _WIN32
  HANDLE fence_event_ = nullptr;
#endif
#if defined(__APPLE__)
  apitrace::platform::macos::WindowHandles window_handles_;
#endif
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> swap_chain_;
  ComPtr<ID3D12Fence> fence_;
  ComPtr<ID3D12Resource> back_buffers_[kBufferCount];
  std::unordered_map<trace::ObjectId, NativeDescriptorHeap> descriptor_heaps_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12RootSignature>> root_signatures_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12PipelineState>> pipelines_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12CommandSignature>> command_signatures_;
  std::unordered_map<trace::ObjectId, NativeHeap> heaps_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12QueryHeap>> query_heaps_;
  std::unordered_map<trace::ObjectId, NativeResource> resources_;
  std::unordered_map<trace::ObjectId, NativeCommandList> command_lists_;
  std::vector<const D3D12ReplayBackend::DescriptorSemanticState *> descriptor_timeline_;
  std::vector<const D3D12ReplayBackend::SamplerSemanticState *> sampler_timeline_;
  std::vector<const D3D12ReplayBackend::CopyDescriptorSemanticState *> descriptor_copy_timeline_;
  std::vector<ResourceDataUpdateRef> resource_update_timeline_;
  std::size_t next_descriptor_index_ = 0;
  std::size_t next_sampler_index_ = 0;
  std::size_t next_descriptor_copy_index_ = 0;
  std::size_t next_resource_update_index_ = 0;
#if !defined(_WIN32)
  std::unique_ptr<trace::TraceBundleWriter> capture_writer_;
  std::uint64_t capture_sequence_ = 0;
#endif
};

#else

class D3D12NativeReplayer {
public:
  explicit D3D12NativeReplayer(const D3D12ReplayBackend &) {}

  bool replay(std::string &error)
  {
    error = "D3D12 native command replay incomplete";
    return false;
  }
};

#endif
