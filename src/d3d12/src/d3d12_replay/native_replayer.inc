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
    replay_milestone("replay:begin");
    if (!log_stage("bootstrap", [&] { return bootstrap(error); }) ||
        !log_stage("create_heaps", [&] { return create_heaps(error); }) ||
        !log_stage("create_resources", [&] { return create_resources(error); }) ||
        !log_stage("create_descriptor_heaps", [&] { return create_descriptor_heaps(error); }) ||
        !log_stage("create_query_heaps", [&] { return create_query_heaps(error); }) ||
        !log_stage("create_root_signatures", [&] { return create_root_signatures(error); }) ||
        !log_stage("create_pipelines", [&] { return create_pipelines(error); }) ||
        !log_stage("create_command_signatures", [&] { return create_command_signatures(error); }) ||
        !log_stage("prepare_chronological_state", [&] { return prepare_chronological_state(error); }) ||
        !log_stage("create_command_objects", [&] { return create_command_objects(error); }) ||
        !log_stage("replay_submissions", [&] { return replay_submissions(error); })) {
      // Guarantee a non-empty error even if a deep return-false path forgot to set one (e.g. a GPU
      // device-removed fault surfaced asynchronously). Without this, the retrace CLI falls back to a
      // generic "finalization failed" and we lose the failing batch, which is invisible under Wine.
      if (error.empty()) {
        error = "D3D12 native command replay incomplete: replay stage failed at batch=" +
                std::to_string(replay_batch_index_);
        const std::string removed = device_removed_detail();
        if (!removed.empty()) {
          error += " (" + removed + ")";
        }
      }
      return false;
    }
    wait_for_gpu();
    replay_milestone((std::string("dbg:setpso_calls=") + std::to_string(dbg_setpso_calls_) +
                      " copy_calls=" + std::to_string(dbg_copy_calls_)).c_str());
    replay_milestone("replay:done");
    return true;
  }

  // Append a flushed milestone line to APITRACE_D3D12_REPLAY_MILESTONE_LOG (if set). Used to
  // observe exactly which replay stage runs / stalls under Wine, where console output is swallowed.
  void replay_milestone(const char *what) const
  {
    const char *path = std::getenv("APITRACE_D3D12_REPLAY_MILESTONE_LOG");
    if (path == nullptr || *path == '\0') {
      return;
    }
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (out) {
      out << what << "\n";
      out.flush();
    }
  }

  // Log the top command kinds by cumulative record time, prefixed with `prefix`. Shared by the
  // per-batch batch:slow log and the mid-grind watchdog so a slow/stuck list is attributable.
  void dump_command_kind_breakdown(const char *prefix) const
  {
    std::array<std::size_t, kCommandKindCount> order{};
    for (std::size_t i = 0; i < kCommandKindCount; ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      return command_kind_micros_[a] > command_kind_micros_[b];
    });
    std::string top;
    for (std::size_t r = 0; r < 4; ++r) {
      const std::size_t k = order[r];
      if (command_kind_micros_[k] == 0) {
        break;
      }
      top += " kind" + std::to_string(k) + "=" + std::to_string(command_kind_micros_[k] / 1000) +
             "ms/" + std::to_string(command_kind_counts_[k]);
    }
    replay_milestone((std::string(prefix) + " cumtop:" + top).c_str());
  }

  template <typename Fn>
  bool log_stage(const char *name, Fn &&fn)
  {
    std::string begin = std::string("stage:begin ") + name;
    replay_milestone(begin.c_str());
    const bool ok = fn();
    std::string end = std::string(ok ? "stage:end   " : "stage:FAIL  ") + name;
    replay_milestone(end.c_str());
    return ok;
  }

private:
  struct NativeResource {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool swapchain_back_buffer = false;
  };

  struct TargetTextureWatchReadback {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    D3D12ReplayBackend::ReplayCommandKind kind = D3D12ReplayBackend::ReplayCommandKind::Unknown;
    std::string function_name;
    std::string phase;
    std::string reason;
    std::vector<trace::ObjectId> object_refs;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT x = 0;
    UINT y = 0;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };

  struct TargetTextureWatchRootDescriptor {
    trace::ObjectId resource_object_id = 0;
    std::uint64_t offset = 0;
    std::string kind;
  };

  struct TargetTextureWatchBufferView {
    trace::ObjectId resource_object_id = 0;
    std::uint64_t offset = 0;
    std::uint32_t size_in_bytes = 0;
    std::uint32_t stride_in_bytes = 0;
    std::uint32_t format = 0;
    bool valid = false;
  };

  struct TargetTextureWatchOcclusion {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    std::string function_name;
    std::string reason;
    trace::ObjectId pipeline_state_object_id = 0;
    ComPtr<ID3D12QueryHeap> query_heap;
    ComPtr<ID3D12Resource> readback;
  };

  struct TargetTextureWatchBufferReadback {
    std::uint64_t sequence = 0;
    trace::ObjectId command_list_object_id = 0;
    std::string label;
    trace::ObjectId resource_object_id = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t stride_in_bytes = 0;
    std::uint32_t format = 0;
    ComPtr<ID3D12Resource> readback;
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
    std::array<trace::ObjectId, 8> current_render_targets{};
    UINT current_render_target_count = 0;
    trace::ObjectId current_pipeline_state_object_id = 0;
    trace::ObjectId current_graphics_root_signature_object_id = 0;
    std::array<std::uint64_t, 16> current_graphics_root_tables{};
    std::array<TargetTextureWatchRootDescriptor, 16> current_graphics_root_descriptors{};
    D3D12_VIEWPORT current_viewport{};
    bool has_current_viewport = false;
    D3D12_RECT current_scissor{};
    bool has_current_scissor = false;
    std::uint32_t current_primitive_topology = 0;
    TargetTextureWatchBufferView current_vertex_buffer0;
    TargetTextureWatchBufferView current_index_buffer;
    std::vector<TargetTextureWatchReadback> target_watch_readbacks;
    std::vector<TargetTextureWatchOcclusion> target_watch_occlusions;
    std::vector<TargetTextureWatchBufferReadback> target_watch_buffer_readbacks;
  };

  struct ReadbackCommandObjects {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
  };

  struct ReplaySequenceScope {
    D3D12NativeReplayer *self = nullptr;
    std::uint64_t saved = 0;

    ReplaySequenceScope(D3D12NativeReplayer *owner, std::uint64_t sequence)
        : self(owner), saved(owner ? owner->current_replay_sequence_ : 0)
    {
      if (self) {
        self->current_replay_sequence_ = sequence;
      }
    }

    ~ReplaySequenceScope()
    {
      if (self) {
        self->current_replay_sequence_ = saved;
      }
    }
  };

  struct ShaderStageInfo {
    const char *field = nullptr;
    const char *path_key = nullptr;
    D3D12_SHADER_BYTECODE D3D12_GRAPHICS_PIPELINE_STATE_DESC::*desc_member = nullptr;
    std::uint64_t bytecode_size = 0;
    std::filesystem::path relative_path;
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
    std::uint64_t replay_size_in_bytes = 0;
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
    pipeline_version_natives_.clear();
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

  // Headless replay (env APITRACE_D3D12_RETRACE_HEADLESS): render to offscreen back-buffer
  // resources instead of an on-screen window + DXGI swapchain. Avoids the win32u display-device
  // lock deadlock under wine-on-macOS (CreateWindowExA / CreateSwapChainForHwnd enumerate the
  // display, which deadlocks from the replay worker thread). Present-frame capture still works
  // (it reads the back-buffer via a READBACK copy, never the screen). See bugs.md BUG-20260614-002.
  bool headless_enabled() const
  {
    return env_enabled("APITRACE_D3D12_RETRACE_HEADLESS", false);
  }

  // Find the recorded swapchain back-buffer resource (for its true format/dims). Returns nullptr
  // if the trace has no swapchain back-buffer semantics.
  const D3D12ReplayBackend::ResourceSemanticState *recorded_back_buffer() const
  {
    for (const auto &[object_id, resource] : backend_.resources_) {
      (void)object_id;
      if (resource.swapchain_back_buffer) {
        return &resource;
      }
    }
    return nullptr;
  }

  const D3D12ReplayBackend::ResourceSemanticState *recorded_resource_at(
      trace::ObjectId object_id,
      std::uint64_t sequence) const
  {
    const D3D12ReplayBackend::ResourceSemanticState *best = nullptr;
    for (const auto &resource : backend_.resource_versions_) {
      if (resource.resource_object_id != object_id || resource.create_sequence > sequence) {
        continue;
      }
      if (best == nullptr || resource.create_sequence > best->create_sequence) {
        best = &resource;
      }
    }
    if (best) {
      return best;
    }
    const auto resource_it = backend_.resources_.find(object_id);
    if (resource_it == backend_.resources_.end()) {
      return nullptr;
    }
    return &resource_it->second;
  }

  static std::uint32_t dxgi_format_cast_family(DXGI_FORMAT format)
  {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
      return 1;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
      return 2;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
      return 3;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
      return 4;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
      return 5;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
      return 6;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
      return 7;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
      return 8;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
      return 9;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
      return 10;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
      return 11;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
      return 12;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
      return 13;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
      return 14;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
      return 15;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
      return 16;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
      return 17;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
      return 18;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return 19;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return 20;
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
      return 21;
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      return 22;
    default:
      return 0;
    }
  }

  static bool dxgi_formats_cast_compatible(DXGI_FORMAT lhs, DXGI_FORMAT rhs)
  {
    if (lhs == rhs) {
      return true;
    }
    const auto lhs_family = dxgi_format_cast_family(lhs);
    const auto rhs_family = dxgi_format_cast_family(rhs);
    return lhs_family != 0 && lhs_family == rhs_family;
  }

  DXGI_FORMAT effective_swapchain_back_buffer_format() const
  {
    const auto *fallback = recorded_back_buffer();
    if (!fallback) {
      return DXGI_FORMAT_UNKNOWN;
    }

    DXGI_FORMAT effective = static_cast<DXGI_FORMAT>(fallback->format);
    std::uint32_t selected_recorded_format = fallback->format;
    trace::ObjectId selected_resource_object_id = fallback->resource_object_id;
    std::uint64_t selected_sequence = std::numeric_limits<std::uint64_t>::max();

    for (const auto &descriptor : backend_.descriptors_) {
      if (descriptor.kind != "RenderTargetView" ||
          !descriptor.has_view_desc ||
          descriptor.resource_object_id == 0 ||
          descriptor.format == 0) {
        continue;
      }
      const auto *resource = recorded_resource_at(descriptor.resource_object_id, descriptor.create_sequence);
      if (resource == nullptr ||
          !resource->swapchain_back_buffer ||
          resource->format == 0) {
        continue;
      }
      const auto recorded_format = static_cast<DXGI_FORMAT>(resource->format);
      const auto view_format = static_cast<DXGI_FORMAT>(descriptor.format);
      if (dxgi_formats_cast_compatible(recorded_format, view_format)) {
        continue;
      }
      if (descriptor.create_sequence < selected_sequence) {
        effective = view_format;
        selected_recorded_format = resource->format;
        selected_resource_object_id = descriptor.resource_object_id;
        selected_sequence = descriptor.create_sequence;
      }
    }

    if (selected_sequence != std::numeric_limits<std::uint64_t>::max()) {
      replay_milestone((std::string("bb:effective-format resource=") +
                        std::to_string(selected_resource_object_id) +
                        " recorded_format=" + std::to_string(selected_recorded_format) +
                        " effective_format=" + std::to_string(static_cast<std::uint32_t>(effective)) +
                        " rtv_seq=" + std::to_string(selected_sequence)).c_str());
    }
    return effective;
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

    const bool headless = headless_enabled();
    const DXGI_FORMAT swapchain_format = effective_swapchain_back_buffer_format();
    if (swapchain_format == DXGI_FORMAT_UNKNOWN) {
      return fail(error, "trace is missing swapchain back-buffer format semantics");
    }
    if (headless) {
      // Offscreen path: create kBufferCount committed render-target textures matching the effective
      // back-buffer format. Older bundles may miss ResizeBuffers, so the recorded resource format can
      // be stale while later RTV creation records the typed format that actually rendered.
      // No window, no swapchain -> no win32u display query -> no deadlock. create_resources() later
      // aliases the trace's swapchain resources onto these exactly as it does for real back-buffers.
      const auto *bb = recorded_back_buffer();
      if (!bb) {
        return fail(error, "headless replay: trace is missing swapchain back-buffer semantics");
      }
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Alignment = 0;
      desc.Width = bb->width != 0 ? bb->width : width;
      desc.Height = bb->height != 0 ? bb->height : height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = swapchain_format;
      desc.SampleDesc.Count = 1;
      desc.SampleDesc.Quality = 0;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

      D3D12_HEAP_PROPERTIES heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
      for (UINT index = 0; index < kBufferCount; ++index) {
        // COMMON (== PRESENT, both 0) matches the resting state the trace's barriers assume.
        hr = device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(back_buffers_[index].put()));
        if (FAILED(hr)) {
          error = hresult_error("CreateCommittedResource(headless back-buffer)", hr);
          return false;
        }
      }
    } else {
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
    // Use an explicit (0,0) origin instead of CW_USEDEFAULT: under headless wine-on-macOS the
    // CW_USEDEFAULT placement makes win32u query the virtual screen rect (NtGdiOpenDCW ->
    // get_virtual_screen_rect -> lock_display_devices), which deadlocks on the display-device lock
    // when called from the replay worker thread. An explicit origin skips that query.
    hwnd_ = CreateWindowExA(
        0,
        wc.lpszClassName,
        "apitrace D3D12 native retrace",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
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

    // Match the effective back-buffer format so windowed replay uses the same
    // resource semantics as the headless path. Present-frame export decodes
    // from the readback byte layout instead of assuming this resource format.
    const auto *recorded_bb = recorded_back_buffer();
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.Width = width;
    swap_chain_desc.Height = height;
    swap_chain_desc.Format = recorded_bb ? swapchain_format : DXGI_FORMAT_R8G8B8A8_UNORM;
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

  static D3D12_HEAP_PROPERTIES heap_properties(const D3D12ReplayBackend::HeapSemanticState &semantic_heap)
  {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = static_cast<D3D12_HEAP_TYPE>(semantic_heap.heap_type);
    heap.CPUPageProperty = static_cast<D3D12_CPU_PAGE_PROPERTY>(semantic_heap.cpu_page_property);
    heap.MemoryPoolPreference = static_cast<D3D12_MEMORY_POOL>(semantic_heap.memory_pool_preference);
    heap.CreationNodeMask = semantic_heap.creation_node_mask ? semantic_heap.creation_node_mask : 1;
    heap.VisibleNodeMask = semantic_heap.visible_node_mask ? semantic_heap.visible_node_mask : 1;
    return heap;
  }

  static D3D12_HEAP_PROPERTIES heap_properties(std::uint32_t heap_type)
  {
    D3D12ReplayBackend::HeapSemanticState heap{};
    heap.heap_type = heap_type;
    heap.cpu_page_property = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.memory_pool_preference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.creation_node_mask = 1;
    heap.visible_node_mask = 1;
    return heap_properties(heap);
  }

  static D3D12_RESOURCE_DESC resource_desc(const D3D12ReplayBackend::ResourceSemanticState &resource)
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(resource.dimension);
    desc.Alignment = resource.alignment;
    desc.Width = resource.width;
    desc.Height = resource.height;
    desc.DepthOrArraySize = static_cast<UINT16>(resource.depth_or_array_size);
    desc.MipLevels = static_cast<UINT16>(resource.recorded_mip_levels);
    desc.Format = static_cast<DXGI_FORMAT>(resource.format);
    desc.SampleDesc.Count = resource.sample_count;
    desc.SampleDesc.Quality = resource.sample_quality;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(resource.layout);
    // Strip ALLOW_CROSS_ADAPTER to stay consistent with the SHARED_CROSS_ADAPTER strip in
    // create_heaps: offline single-adapter replay does not share, and DXMT's placed-resource
    // validation requires the heap and resource cross-adapter flags to agree.
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        resource.flags & ~D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    return desc;
  }

  static std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
  {
    if (alignment <= 1) {
      return value;
    }
    const std::uint64_t remainder = value % alignment;
    if (!remainder) {
      return value;
    }
    const std::uint64_t add = alignment - remainder;
    if (value > std::numeric_limits<std::uint64_t>::max() - add) {
      return 0;
    }
    return value + add;
  }

  bool current_device_heap_requirement(
      trace::ObjectId heap_object_id,
      std::uint64_t &required_size,
      std::string &error) const
  {
    required_size = 0;
    for (const auto &resource : backend_.resource_versions_) {
      if (resource.heap_object_id != heap_object_id) {
        continue;
      }
      const auto desc = resource_desc(resource);
      const auto allocation_info = device_->GetResourceAllocationInfo(1, 1, &desc);
      if (allocation_info.SizeInBytes == 0) {
        std::ostringstream detail;
        detail << "GetResourceAllocationInfo returned zero for placed resource object="
               << resource.resource_object_id << " create_seq=" << resource.create_sequence;
        error = detail.str();
        return false;
      }
      if (resource.heap_offset >
          std::numeric_limits<std::uint64_t>::max() - allocation_info.SizeInBytes) {
        std::ostringstream detail;
        detail << "placed resource range overflow object=" << resource.resource_object_id
               << " create_seq=" << resource.create_sequence;
        error = detail.str();
        return false;
      }
      required_size = std::max<std::uint64_t>(
          required_size, resource.heap_offset + allocation_info.SizeInBytes);
    }
    return true;
  }

  bool create_heaps(std::string &error)
  {
    for (const auto &[object_id, heap] : backend_.heaps_) {
      D3D12_HEAP_DESC desc{};
      desc.SizeInBytes = heap.size_in_bytes;
      desc.Properties = heap_properties(heap);
      desc.Alignment = heap.alignment;
      // Offline single-device replay never shares heaps across processes or adapters, so the
      // SHARED / SHARED_CROSS_ADAPTER flags are no-ops here. Strip them: a CUSTOM heap with
      // SHARED_CROSS_ADAPTER + a CPU-visible page property is invalid per the D3D12 spec (DXMT
      // rejects it as "custom-cross-adapter-cpu-visible"), and we render into the heap identically
      // without sharing. The matching resource ALLOW_CROSS_ADAPTER flag is stripped in resource_desc.
      auto heap_flags = static_cast<D3D12_HEAP_FLAGS>(heap.flags);
      heap_flags = static_cast<D3D12_HEAP_FLAGS>(
          heap_flags & ~(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER));
      desc.Flags = heap_flags;
      std::uint64_t required_size = 0;
      if (!current_device_heap_requirement(object_id, required_size, error)) {
        return false;
      }
      if (required_size > desc.SizeInBytes) {
        const std::uint64_t heap_alignment = desc.Alignment ? desc.Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        const std::uint64_t replay_size = align_up(required_size, heap_alignment);
        if (replay_size == 0) {
          std::ostringstream detail;
          detail << "CreateHeap replay-size overflow object_id=" << object_id
                 << " recorded_size=" << desc.SizeInBytes
                 << " required_size=" << required_size
                 << " alignment=" << heap_alignment;
          error = detail.str();
          return false;
        }
        std::ostringstream detail;
        detail << "heap:resize-for-replay object=" << object_id
               << " recorded_size=" << desc.SizeInBytes
               << " replay_size=" << replay_size
               << " required_size=" << required_size
               << " alignment=" << heap_alignment;
        replay_milestone(detail.str().c_str());
        desc.SizeInBytes = replay_size;
      }
      NativeHeap native;
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
        error = hresult_error(detail.str(), hr);
        return false;
      }
      heaps_.emplace(object_id, std::move(native));
    }
    return true;
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

  bool make_native_resource(
      const D3D12ReplayBackend::ResourceSemanticState &resource,
      NativeResource &native,
      std::string &error)
  {
    native.state = static_cast<D3D12_RESOURCE_STATES>(resource.initial_state);
    native.swapchain_back_buffer = resource.swapchain_back_buffer;
    if (resource.swapchain_back_buffer) {
      if (resource.swapchain_buffer_index >= kBufferCount || !back_buffers_[resource.swapchain_buffer_index]) {
        return fail(error, "swapchain back-buffer index is out of range");
      }
      back_buffers_[resource.swapchain_buffer_index]->AddRef();
      native.resource.reset(back_buffers_[resource.swapchain_buffer_index].get());
      return true;
    }

    const auto desc = resource_desc(resource);
    const auto optimized_clear = resource.has_optimized_clear_value ? clear_value(resource) : D3D12_CLEAR_VALUE{};
    HRESULT hr = S_OK;
    const auto log_create_failure = [&](const char *call_name, HRESULT failed_hr) {
      D3D12_RESOURCE_ALLOCATION_INFO allocation_info =
          device_->GetResourceAllocationInfo(1, 1, &desc);
      std::ostringstream detail;
      detail << "resource:create-fail call=" << call_name
             << " hr=0x" << std::hex << static_cast<std::uint32_t>(failed_hr) << std::dec
             << " object=" << resource.resource_object_id
             << " create_seq=" << resource.create_sequence
             << " dim=" << resource.dimension
             << " fmt=" << resource.format
             << " width=" << resource.width
             << " height=" << resource.height
             << " depth_or_array=" << resource.depth_or_array_size
             << " mips=" << resource.mip_levels
             << " sample_count=" << resource.sample_count
             << " layout=" << resource.layout
             << " flags=0x" << std::hex << resource.flags << std::dec
             << " heap_object=" << resource.heap_object_id
             << " heap_type=" << resource.heap_type
             << " heap_flags=0x" << std::hex << resource.heap_flags << std::dec
             << " heap_offset=" << resource.heap_offset
             << " alloc_size=" << allocation_info.SizeInBytes
             << " alloc_alignment=" << allocation_info.Alignment
             << " initial_state=0x" << std::hex << resource.initial_state << std::dec
             << " reserved=" << (resource.reserved_resource ? "1" : "0")
             << " optimized_clear=" << (resource.has_optimized_clear_value ? "1" : "0");
      replay_milestone(detail.str().c_str());
    };
    if (resource.reserved_resource) {
      hr = device_->CreateReservedResource(
          &desc, static_cast<D3D12_RESOURCE_STATES>(resource.initial_state),
          resource.has_optimized_clear_value ? &optimized_clear : nullptr,
          IID_PPV_ARGS(native.resource.put()));
      if (FAILED(hr)) {
        log_create_failure("CreateReservedResource", hr);
        error = hresult_error("CreateReservedResource", hr);
        return false;
      }
    } else if (resource.heap_object_id != 0) {
      const auto heap_it = heaps_.find(resource.heap_object_id);
      if (heap_it == heaps_.end()) {
        return fail(error, "placed resource references an unknown heap");
      }
      hr = device_->CreatePlacedResource(
          heap_it->second.heap.get(), resource.heap_offset, &desc,
          static_cast<D3D12_RESOURCE_STATES>(resource.initial_state),
          resource.has_optimized_clear_value ? &optimized_clear : nullptr,
          IID_PPV_ARGS(native.resource.put()));
      if (FAILED(hr)) {
        log_create_failure("CreatePlacedResource", hr);
        error = hresult_error("CreatePlacedResource", hr);
        return false;
      }
    } else {
      const auto heap = heap_properties(resource.heap_type);
      hr = device_->CreateCommittedResource(
          &heap, static_cast<D3D12_HEAP_FLAGS>(resource.heap_flags), &desc,
          static_cast<D3D12_RESOURCE_STATES>(resource.initial_state),
          resource.has_optimized_clear_value ? &optimized_clear : nullptr,
          IID_PPV_ARGS(native.resource.put()));
      if (FAILED(hr)) {
        log_create_failure("CreateCommittedResource", hr);
        error = hresult_error("CreateCommittedResource", hr);
        return false;
      }
    }
    return true;
  }

  bool create_resources(std::string &error)
  {
    // Latest version per object_id (covers the common, non-reused case and all readers that index
    // resources_ directly).
    for (const auto &[object_id, resource] : backend_.resources_) {
      NativeResource native;
      if (!make_native_resource(resource, native, error)) {
        return false;
      }
      resources_.emplace(object_id, std::move(native));
    }

    // For object_ids created more than once (D3D12 pointer reuse across lifetimes), build a native
    // resource per version so descriptors/commands resolve to the version live at their sequence.
    std::unordered_map<trace::ObjectId, std::size_t> version_counts;
    for (const auto &resource : backend_.resource_versions_) {
      ++version_counts[resource.resource_object_id];
    }
    for (const auto &resource : backend_.resource_versions_) {
      if (version_counts[resource.resource_object_id] < 2) {
        continue;  // single version: resources_ already holds it
      }
      NativeResource native;
      if (!make_native_resource(resource, native, error)) {
        return false;
      }
      resource_version_natives_[resource.resource_object_id].emplace_back(
          resource.create_sequence, std::move(native));
    }
    for (auto &[object_id, versions] : resource_version_natives_) {
      (void)object_id;
      std::stable_sort(versions.begin(), versions.end(),
                       [](const auto &a, const auto &b) { return a.first < b.first; });
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
        std::ostringstream detail;
        detail << "rootsig:create-fail hr=0x"
               << std::hex << static_cast<std::uint32_t>(hr) << std::dec
               << " object=" << object_id
               << " create_seq=" << root_signature.create_sequence
               << " node_mask=" << root_signature.node_mask
               << " bytecode_size=" << root_signature.bytecode_size
               << " bytes=" << root_signature.bytes.size()
               << " path=" << root_signature.relative_path.generic_string();
        replay_milestone(detail.str().c_str());
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
    return read_prefix_asset_bytes(backend_.bundle_root_ / relative_path, size, bytes, error);
  }

  bool shader_stage_info(const json &pipeline_asset, const char *field, const char *path_key, ShaderStageInfo &info) const
  {
    const auto shader = pipeline_asset.find(field);
    if (shader == pipeline_asset.end() || shader->is_null() || !shader->is_object()) {
      return false;
    }
    const auto size = payload_u64(*shader, "bytecode_size");
    const auto relative_path = payload_string(*shader, path_key);
    if (size == 0 || relative_path.empty()) {
      return false;
    }
    info.field = field;
    info.path_key = path_key;
    info.bytecode_size = size;
    info.relative_path = relative_path;
    return true;
  }

  static std::uint32_t read_le_u32(const std::uint8_t *bytes)
  {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
  }

  std::uint64_t dxbc_declared_size(const std::filesystem::path &absolute_path) const
  {
    std::ifstream input(absolute_path, std::ios::binary);
    if (!input) {
      return 0;
    }
    std::array<std::uint8_t, 28> header{};
    input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
      return 0;
    }
    if (std::memcmp(header.data(), "DXBC", 4) != 0) {
      return 0;
    }
    return read_le_u32(header.data() + 24);
  }

  void build_shader_candidate_index() const
  {
    if (shader_candidate_index_built_) {
      return;
    }
    shader_candidate_index_built_ = true;
    const auto shader_dir = backend_.bundle_root_ / "shaders";
    std::error_code ec;
    if (!std::filesystem::is_directory(shader_dir, ec)) {
      return;
    }
    for (std::filesystem::directory_iterator it(shader_dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) {
        continue;
      }
      const auto relative_path = std::filesystem::path("shaders") / it->path().filename();
      if (!is_shader_asset_path(relative_path)) {
        continue;
      }
      const auto file_size = it->file_size(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      const auto declared_size = dxbc_declared_size(it->path());
      if (declared_size == 0 || declared_size != file_size) {
        continue;
      }
      shader_candidate_paths_by_size_[declared_size].push_back(relative_path);
    }
    for (auto &[size, paths] : shader_candidate_paths_by_size_) {
      (void)size;
      std::sort(paths.begin(), paths.end());
    }
  }

  const std::vector<std::filesystem::path> &shader_candidates_for_size(std::uint64_t size) const
  {
    static const std::vector<std::filesystem::path> empty;
    build_shader_candidate_index();
    const auto it = shader_candidate_paths_by_size_.find(size);
    return it == shader_candidate_paths_by_size_.end() ? empty : it->second;
  }

  struct ShaderStageCandidateSet {
    ShaderStageInfo stage;
    std::vector<std::filesystem::path> candidates;
  };

  template <typename CreateFn>
  bool try_graphics_pso_candidate_combination(
      const D3D12ReplayBackend::PipelineSemanticState &pipeline,
      const std::vector<ShaderStageCandidateSet> &sets,
      std::size_t index,
      D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
      std::vector<std::vector<std::uint8_t>> &candidate_bytes,
      std::vector<std::filesystem::path> &chosen_paths,
      CreateFn &&create,
      ComPtr<ID3D12PipelineState> &native) const
  {
    if (index == sets.size()) {
      ComPtr<ID3D12PipelineState> retry_native;
      if (FAILED(create(desc, retry_native))) {
        return false;
      }
      std::string message = "pso:shader-candidates object=" +
                            std::to_string(pipeline.pipeline_state_object_id) +
                            " create_sequence=" + std::to_string(pipeline.create_sequence);
      for (std::size_t i = 0; i < sets.size(); ++i) {
        message += " stage=";
        message += sets[i].stage.field;
        message += " from=";
        message += sets[i].stage.relative_path.generic_string();
        message += " to=";
        message += chosen_paths[i].generic_string();
      }
      replay_milestone(message.c_str());
      native = std::move(retry_native);
      return true;
    }

    const auto &set = sets[index];
    const auto previous = desc.*(set.stage.desc_member);
    for (const auto &candidate : set.candidates) {
      std::string candidate_error;
      if (!read_exact_asset_bytes(
              backend_.bundle_root_ / candidate,
              set.stage.bytecode_size,
              candidate_bytes[index],
              candidate_error)) {
        continue;
      }
      desc.*(set.stage.desc_member) =
          D3D12_SHADER_BYTECODE{candidate_bytes[index].data(), candidate_bytes[index].size()};
      chosen_paths[index] = candidate;
      if (try_graphics_pso_candidate_combination(
              pipeline,
              sets,
              index + 1,
              desc,
              candidate_bytes,
              chosen_paths,
              create,
              native)) {
        return true;
      }
    }
    desc.*(set.stage.desc_member) = previous;
    candidate_bytes[index].clear();
    return false;
  }

  template <typename CreateFn>
  bool retry_graphics_pso_shader_candidates(
      const D3D12ReplayBackend::PipelineSemanticState &pipeline,
      const json &pipeline_asset,
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC &base_desc,
      CreateFn &&create,
      ComPtr<ID3D12PipelineState> &native) const
  {
    std::array<ShaderStageInfo, 5> stages{};
    stages[0].desc_member = &D3D12_GRAPHICS_PIPELINE_STATE_DESC::VS;
    stages[1].desc_member = &D3D12_GRAPHICS_PIPELINE_STATE_DESC::PS;
    stages[2].desc_member = &D3D12_GRAPHICS_PIPELINE_STATE_DESC::DS;
    stages[3].desc_member = &D3D12_GRAPHICS_PIPELINE_STATE_DESC::HS;
    stages[4].desc_member = &D3D12_GRAPHICS_PIPELINE_STATE_DESC::GS;
    const std::array<std::pair<const char *, const char *>, 5> names{{
        {"vs", "vs_path"},
        {"ps", "ps_path"},
        {"ds", "ds_path"},
        {"hs", "hs_path"},
        {"gs", "gs_path"},
    }};
    std::vector<ShaderStageCandidateSet> candidate_sets;
    for (std::size_t index = 0; index < stages.size(); ++index) {
      auto &stage = stages[index];
      if (!shader_stage_info(pipeline_asset, names[index].first, names[index].second, stage)) {
        continue;
      }
      const auto declared_size = dxbc_declared_size(backend_.bundle_root_ / stage.relative_path);
      if (declared_size == 0 || declared_size == stage.bytecode_size) {
        continue;
      }
      ShaderStageCandidateSet set;
      set.stage = stage;
      const auto &candidates = shader_candidates_for_size(stage.bytecode_size);
      constexpr std::size_t kMaxCandidatesPerStage = 32;
      for (const auto &candidate : candidates) {
        if (candidate == stage.relative_path) {
          continue;
        }
        set.candidates.push_back(candidate);
        if (set.candidates.size() >= kMaxCandidatesPerStage) {
          break;
        }
      }
      if (!set.candidates.empty()) {
        candidate_sets.push_back(std::move(set));
      }
    }
    if (candidate_sets.empty()) {
      return false;
    }

    auto desc = base_desc;
    std::vector<std::vector<std::uint8_t>> candidate_bytes(candidate_sets.size());
    std::vector<std::filesystem::path> chosen_paths(candidate_sets.size());
    return try_graphics_pso_candidate_combination(
        pipeline,
        candidate_sets,
        0,
        desc,
        candidate_bytes,
        chosen_paths,
        create,
        native);
  }

  template <typename CreateFn>
  bool retry_compute_pso_shader_candidates(
      const D3D12ReplayBackend::PipelineSemanticState &pipeline,
      const json &pipeline_asset,
      const D3D12_COMPUTE_PIPELINE_STATE_DESC &base_desc,
      CreateFn &&create,
      ComPtr<ID3D12PipelineState> &native) const
  {
    ShaderStageInfo stage;
    if (!shader_stage_info(pipeline_asset, "cs", "cs_path", stage)) {
      return false;
    }
    const auto declared_size = dxbc_declared_size(backend_.bundle_root_ / stage.relative_path);
    if (declared_size == 0 || declared_size == stage.bytecode_size) {
      return false;
    }
    const auto &candidates = shader_candidates_for_size(stage.bytecode_size);
    constexpr std::size_t kMaxCandidates = 32;
    std::size_t tried = 0;
    for (const auto &candidate : candidates) {
      if (candidate == stage.relative_path) {
        continue;
      }
      if (tried++ >= kMaxCandidates) {
        break;
      }
      std::vector<std::uint8_t> candidate_bytes;
      std::string candidate_error;
      if (!read_exact_asset_bytes(
              backend_.bundle_root_ / candidate,
              stage.bytecode_size,
              candidate_bytes,
              candidate_error)) {
        continue;
      }
      D3D12_COMPUTE_PIPELINE_STATE_DESC retry_desc = base_desc;
      retry_desc.CS = D3D12_SHADER_BYTECODE{candidate_bytes.data(), candidate_bytes.size()};
      ComPtr<ID3D12PipelineState> retry_native;
      if (FAILED(create(retry_desc, retry_native))) {
        continue;
      }
      replay_milestone((std::string("pso:shader-candidates object=") +
                        std::to_string(pipeline.pipeline_state_object_id) +
                        " create_sequence=" + std::to_string(pipeline.create_sequence) +
                        " stage=cs from=" + stage.relative_path.generic_string() +
                        " to=" + candidate.generic_string()).c_str());
      native = std::move(retry_native);
      return true;
    }
    return false;
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
    const auto make_native_pipeline = [&](const D3D12ReplayBackend::PipelineSemanticState &pipeline,
                                          ComPtr<ID3D12PipelineState> &native,
                                          std::string &error) -> bool {
      ID3D12RootSignature *root_signature = nullptr;
      if (!pipeline.uses_embedded_root_signature) {
        const auto root_it = root_signatures_.find(pipeline.root_signature_object_id);
        if (root_it == root_signatures_.end()) {
          return fail(error, "pipeline references an unknown root signature");
        }
        root_signature = root_it->second.get();
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
        desc.pRootSignature = root_signature;
        desc.CS = D3D12_SHADER_BYTECODE{cs.data(), cs.size()};
        desc.NodeMask = pipeline.node_mask;
        desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(pipeline.flags);
        const auto create_compute_pipeline =
            [&](const D3D12_COMPUTE_PIPELINE_STATE_DESC &pipeline_desc,
                ComPtr<ID3D12PipelineState> &pipeline_state) -> HRESULT {
          return device_->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(pipeline_state.put()));
        };
        HRESULT hr = create_compute_pipeline(desc, native);
        if (FAILED(hr)) {
          if (retry_compute_pso_shader_candidates(pipeline, pipeline_asset, desc, create_compute_pipeline, native)) {
            return true;
          }
          error = hresult_error("CreateComputePipelineState", hr) +
                  " path=" + pipeline.relative_path.generic_string() +
                  " object=" + std::to_string(pipeline.pipeline_state_object_id) +
                  " create_sequence=" + std::to_string(pipeline.create_sequence) +
                  " root=" + std::to_string(pipeline.root_signature_object_id) +
                  " cs_bytes=" + std::to_string(cs.size());
          return false;
        }
        return true;
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
      desc.pRootSignature = root_signature;
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
      const auto create_graphics_pipeline =
          [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC &pipeline_desc,
              ComPtr<ID3D12PipelineState> &pipeline_state) -> HRESULT {
        return device_->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(pipeline_state.put()));
      };
      HRESULT hr = create_graphics_pipeline(desc, native);
      if (FAILED(hr)) {
        if (retry_graphics_pso_shader_candidates(pipeline, pipeline_asset, desc, create_graphics_pipeline, native)) {
          return true;
        }
        error = hresult_error("CreateGraphicsPipelineState", hr) +
                " path=" + pipeline.relative_path.generic_string() +
                " object=" + std::to_string(pipeline.pipeline_state_object_id) +
                " create_sequence=" + std::to_string(pipeline.create_sequence) +
                " root=" + std::to_string(pipeline.root_signature_object_id) +
                " rts=" + std::to_string(pipeline.num_render_targets) +
                " dsv=" + std::to_string(pipeline.dsv_format) +
                " sample=" + std::to_string(pipeline.sample_count) + "x" +
                std::to_string(pipeline.sample_quality) +
                " vs_bytes=" + std::to_string(vs.size()) +
                " ps_bytes=" + std::to_string(ps.size()) +
                " ds_bytes=" + std::to_string(ds.size()) +
                " hs_bytes=" + std::to_string(hs.size()) +
                " gs_bytes=" + std::to_string(gs.size());
        return false;
      }
      return true;
    };

    for (const auto &[object_id, pipeline] : backend_.pipelines_) {
      ComPtr<ID3D12PipelineState> native;
      if (!make_native_pipeline(pipeline, native, error)) {
        return false;
      }
      pipelines_.emplace(object_id, std::move(native));
    }

    std::unordered_map<trace::ObjectId, std::size_t> version_counts;
    for (const auto &pipeline : backend_.pipeline_versions_) {
      ++version_counts[pipeline.pipeline_state_object_id];
    }
    for (const auto &pipeline : backend_.pipeline_versions_) {
      if (version_counts[pipeline.pipeline_state_object_id] < 2) {
        continue;  // single version: pipelines_ already holds it
      }
      ComPtr<ID3D12PipelineState> native;
      if (!make_native_pipeline(pipeline, native, error)) {
        return false;
      }
      pipeline_version_natives_[pipeline.pipeline_state_object_id].emplace_back(
          pipeline.create_sequence, std::move(native));
    }
    for (auto &[object_id, versions] : pipeline_version_natives_) {
      (void)object_id;
      std::stable_sort(versions.begin(), versions.end(),
                       [](const auto &a, const auto &b) { return a.first < b.first; });
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

  bool allocate_cpu_descriptor_scratch(
      D3D12_DESCRIPTOR_HEAP_TYPE type,
      D3D12_CPU_DESCRIPTOR_HANDLE &handle,
      std::string &error)
  {
    handle = {};
    const auto index = static_cast<std::size_t>(type);
    if (index >= cpu_descriptor_scratch_.size()) {
      return fail(error, "CPU descriptor scratch heap type is out of range");
    }
    auto &scratch = cpu_descriptor_scratch_[index];
    if (scratch.descriptor_size == 0) {
      scratch.descriptor_size = device_->GetDescriptorHandleIncrementSize(type);
    }
    if (scratch.heaps.empty() || scratch.next >= scratch.capacity) {
      D3D12_DESCRIPTOR_HEAP_DESC desc{};
      desc.Type = type;
      desc.NumDescriptors = scratch.capacity;
      desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      ComPtr<ID3D12DescriptorHeap> heap;
      const HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put()));
      if (FAILED(hr)) {
        return fail(error, hresult_error("CreateDescriptorHeap(cpu scratch)", hr));
      }
      scratch.current_start = heap->GetCPUDescriptorHandleForHeapStart();
      scratch.heaps.push_back(std::move(heap));
      scratch.next = 0;
    }
    handle.ptr = scratch.current_start.ptr +
                 static_cast<SIZE_T>(scratch.next++) * scratch.descriptor_size;
    return true;
  }

  // BUG-007 diagnostic: log when a clear/set-render-target binding targets a known back-buffer RTV
  // slot, so we can tell whether the back-buffer is bound as a DRAW target vs only cleared.
  void note_back_buffer_rtv_use(const D3D12ReplayBackend::DescriptorBinding &binding,
                                const char *how, std::uint64_t sequence)
  {
    if (back_buffer_rtv_slots_.empty()) {
      return;
    }
    const std::uint64_t key =
        (static_cast<std::uint64_t>(binding.heap_object_id) << 20) ^ binding.descriptor_index;
    if (back_buffer_rtv_slots_.count(key)) {
      replay_milestone((std::string("bb:rtv-use ") + how + " heap=" +
                        std::to_string(binding.heap_object_id) + " idx=" +
                        std::to_string(binding.descriptor_index) + " seq=" +
                        std::to_string(sequence)).c_str());
    }
  }

  bool apply_descriptor(
      const D3D12ReplayBackend::DescriptorSemanticState &semantic,
      std::string &error)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE dst = relocated_cpu_descriptor(semantic.binding);
    if (dst.ptr == 0) {
      return fail(error, "descriptor destination cannot be relocated");
    }
    return create_descriptor_view(semantic, dst, true, error);
  }

  bool create_descriptor_view(
      const D3D12ReplayBackend::DescriptorSemanticState &semantic,
      D3D12_CPU_DESCRIPTOR_HANDLE dst,
      bool track_back_buffer_rtv,
      std::string &error)
  {
    if (dst.ptr == 0) {
      return fail(error, "descriptor destination cannot be zero");
    }
    struct SequenceRestore {
      D3D12NativeReplayer *self = nullptr;
      std::uint64_t saved = 0;
      ~SequenceRestore()
      {
        if (self) {
          self->current_replay_sequence_ = saved;
        }
      }
    } restore{this, current_replay_sequence_};
    const auto saved_sequence = current_replay_sequence_;
    // Resolve resource references to the version live when this descriptor was created.
    current_replay_sequence_ = semantic.create_sequence;
    ID3D12Resource *resource = semantic.resource_object_id == 0 ? nullptr : resource_for(semantic.resource_object_id, error);
    if (semantic.resource_object_id != 0 && !resource) {
      current_replay_sequence_ = saved_sequence;
      return false;
    }
    // Diagnostic: a view format incompatible with the recorded resource format (e.g. RG11B10F view
    // on a BC7 resource) makes DXMT's Metal texture-view creation assert. Log the offending
    // descriptor so we can tell a wrong-resource resolution from a recorded-format issue.
    if (resource && semantic.format != 0 && (semantic.kind == "ShaderResourceView" ||
        semantic.kind == "UnorderedAccessView" || semantic.kind == "RenderTargetView")) {
      const auto res_it = backend_.resources_.find(semantic.resource_object_id);
      if (res_it != backend_.resources_.end()) {
        const auto res_fmt = res_it->second.format;
        const auto is_bc = [](std::uint32_t f) { return (f >= 70 && f <= 84) || (f >= 94 && f <= 99); };
        if (is_bc(res_fmt) != is_bc(semantic.format) && res_fmt != semantic.format) {
          replay_milestone((std::string("descriptor:format-mismatch kind=") + semantic.kind +
                            " resource=" + std::to_string(semantic.resource_object_id) +
                            " resource_format=" + std::to_string(res_fmt) +
                            " view_format=" + std::to_string(semantic.format) +
                            " view_dim=" + std::to_string(semantic.view_dimension) +
                            " create_seq=" + std::to_string(semantic.create_sequence)).c_str());
        }
      }
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
      // Diagnostic (BUG-007): record which descriptor slots hold an RTV that points at a swapchain
      // back-buffer, so the clear/set-render-target paths can report whether the back-buffer is ever
      // bound as a DRAW target (OMSetRenderTargets) vs only cleared.
      if (track_back_buffer_rtv && std::getenv("APITRACE_D3D12_REPLAY_MILESTONE_LOG") && resource != nullptr) {
        for (UINT bi = 0; bi < kBufferCount; ++bi) {
          if (back_buffers_[bi] && back_buffers_[bi].get() == resource) {
            back_buffer_rtv_slots_.insert(
                (static_cast<std::uint64_t>(semantic.binding.heap_object_id) << 20) ^
                semantic.binding.descriptor_index);
            replay_milestone((std::string("bb:rtv-created slot heap=") +
                              std::to_string(semantic.binding.heap_object_id) + " idx=" +
                              std::to_string(semantic.binding.descriptor_index) + " bb=" +
                              std::to_string(bi) + " res=" +
                              std::to_string(semantic.resource_object_id) +
                              " seq=" + std::to_string(semantic.create_sequence)).c_str());
            break;
          }
        }
      }
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
      ID3D12Resource *buffer = resource_for(semantic.buffer.resource_object_id, error);
      if (!buffer) {
        if (error.empty()) {
          error = "CBV source resource is missing";
        }
        return false;
      }
      D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
      desc.BufferLocation = buffer->GetGPUVirtualAddress() + semantic.buffer.offset;
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
      current_replay_sequence_ = saved_sequence;
      return fail(error, "descriptor kind is not covered: " + semantic.kind);
    }
    current_replay_sequence_ = saved_sequence;
    return true;
  }

  const D3D12ReplayBackend::DescriptorSemanticState *descriptor_semantic_at(
      std::uint64_t descriptor,
      std::uint64_t sequence) const
  {
    const auto history_it = descriptor_slot_history_.find(descriptor);
    if (history_it == descriptor_slot_history_.end() || history_it->second.empty()) {
      return nullptr;
    }
    const auto &history = history_it->second;
    const auto upper = std::upper_bound(
        history.begin(),
        history.end(),
        sequence,
        [](std::uint64_t value, const DescriptorSlotHistoryEntry &entry) {
          return value < entry.sequence;
        });
    if (upper == history.begin()) {
      return nullptr;
    }
    return (upper - 1)->descriptor;
  }

  bool materialize_cpu_descriptor_at(
      std::uint64_t descriptor,
      std::uint64_t sequence,
      D3D12_CPU_DESCRIPTOR_HANDLE &handle,
      D3D12ReplayBackend::DescriptorBinding *binding_out,
      std::string &error)
  {
    handle = {};
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(descriptor, false, sequence, binding, error)) {
      return false;
    }
    if (binding_out) {
      *binding_out = binding;
    }
    if (binding.heap_object_id == 0) {
      return true;
    }
    const auto *semantic = descriptor_semantic_at(descriptor, sequence);
    if (semantic == nullptr) {
      return fail(error, "CPU descriptor has no recorded contents at command sequence");
    }
    const auto heap_type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(binding.heap_type);
    if (!allocate_cpu_descriptor_scratch(heap_type, handle, error)) {
      return false;
    }
    return create_descriptor_view(*semantic, handle, false, error);
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
    const ReplaySequenceScope sync_scope(this, sequence);
    while (next_resource_update_index_ < resource_update_timeline_.size() &&
           resource_update_timeline_[next_resource_update_index_].sequence <= sequence) {
      const auto &entry = resource_update_timeline_[next_resource_update_index_];
      // Resolve the SAME version that reads (record_copy/bindings via resource_for) will see at this
      // update's sequence, NOT the flat resources_ last-version. For a reused object_id (a per-frame
      // ring/pool upload buffer, e.g. one seen reused 192x), the flat map points at the final native
      // while reads resolve the sequence-correct native -> the read version never gets the data ->
      // textures/constants read empty -> draws render black (BUG-007 layer 2).
      const ReplaySequenceScope update_scope(this, entry.sequence);
      ID3D12Resource *target = resource_for(entry.resource_object_id, error);
      if (!target) {
        return false;
      }
      if (!upload_resource_data(target, *entry.update, error)) {
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
    descriptor_slot_history_.clear();
    struct ResourceUpdateKey {
      trace::ObjectId resource_object_id = 0;
      std::uint64_t sequence = 0;
      std::uint32_t subresource = 0;
      std::uint64_t written_begin = 0;
      std::uint64_t written_end = 0;
      std::string relative_path;

      bool operator==(const ResourceUpdateKey &other) const noexcept
      {
        return resource_object_id == other.resource_object_id &&
               sequence == other.sequence &&
               subresource == other.subresource &&
               written_begin == other.written_begin &&
               written_end == other.written_end &&
               relative_path == other.relative_path;
      }
    };
    struct ResourceUpdateKeyHash {
      std::size_t operator()(const ResourceUpdateKey &key) const noexcept
      {
        std::size_t hash = std::hash<trace::ObjectId>{}(key.resource_object_id);
        hash ^= std::hash<std::uint64_t>{}(key.sequence) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::uint32_t>{}(key.subresource) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::uint64_t>{}(key.written_begin) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::uint64_t>{}(key.written_end) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::string>{}(key.relative_path) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return hash;
      }
    };
    std::unordered_set<ResourceUpdateKey, ResourceUpdateKeyHash> resource_update_keys;
    const auto add_resource_update = [&](trace::ObjectId resource_object_id,
                                         const D3D12ReplayBackend::ResourceDataUpdate &update) {
      const auto effective_sequence = update.apply_sequence != 0 ? update.apply_sequence : update.sequence;
      ResourceUpdateKey key;
      key.resource_object_id = resource_object_id;
      key.sequence = effective_sequence;
      key.subresource = update.subresource;
      key.written_begin = update.written_begin;
      key.written_end = update.written_end;
      key.relative_path = update.relative_path.generic_string();
      if (!resource_update_keys.insert(std::move(key)).second) {
        return;
      }
      resource_update_timeline_.push_back(ResourceDataUpdateRef{effective_sequence, resource_object_id, &update});
    };
    for (const auto &resource : backend_.resource_versions_) {
      for (const auto &update : resource.data_updates) {
        add_resource_update(resource.resource_object_id, update);
      }
    }
    for (const auto &[resource_object_id, resource] : backend_.resources_) {
      for (const auto &update : resource.data_updates) {
        add_resource_update(resource_object_id, update);
      }
    }
    std::stable_sort(
        resource_update_timeline_.begin(),
        resource_update_timeline_.end(),
        [](const auto &lhs, const auto &rhs) {
          return lhs.sequence < rhs.sequence;
        });

    struct DescriptorHistoryEvent {
      std::uint64_t sequence = 0;
      const D3D12ReplayBackend::DescriptorSemanticState *descriptor = nullptr;
      const D3D12ReplayBackend::CopyDescriptorSemanticState *copy = nullptr;
    };
    std::vector<DescriptorHistoryEvent> descriptor_history_events;
    descriptor_history_events.reserve(descriptor_timeline_.size() + descriptor_copy_timeline_.size());
    for (const auto *descriptor : descriptor_timeline_) {
      descriptor_history_events.push_back(DescriptorHistoryEvent{descriptor->create_sequence, descriptor, nullptr});
    }
    for (const auto *copy : descriptor_copy_timeline_) {
      descriptor_history_events.push_back(DescriptorHistoryEvent{copy->create_sequence, nullptr, copy});
    }
    std::stable_sort(
        descriptor_history_events.begin(),
        descriptor_history_events.end(),
        [](const auto &lhs, const auto &rhs) {
          return lhs.sequence < rhs.sequence;
        });
    for (const auto &event : descriptor_history_events) {
      if (event.descriptor != nullptr) {
        descriptor_slot_history_[event.descriptor->descriptor].push_back(
            DescriptorSlotHistoryEntry{event.sequence, event.descriptor});
        continue;
      }
      for (const auto &pair : event.copy->descriptors) {
        const auto *source = descriptor_semantic_at(pair.src_descriptor, event.sequence);
        if (source != nullptr) {
          descriptor_slot_history_[pair.dst_descriptor].push_back(
              DescriptorSlotHistoryEntry{event.sequence, source});
        }
      }
    }

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
      switch (static_cast<D3D12_COMMAND_LIST_TYPE>(command_list.type)) {
      case D3D12_COMMAND_LIST_TYPE_DIRECT:
      case D3D12_COMMAND_LIST_TYPE_BUNDLE:
      case D3D12_COMMAND_LIST_TYPE_COMPUTE:
      case D3D12_COMMAND_LIST_TYPE_COPY:
        break;
      default:
        return fail(error, "unsupported command list type for native replay");
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

  // One closed [begin_index, end_index] command interval for a command list, with the sequence of
  // its EndCommandList. Per list_id these are appended in scan order (ascending index/sequence).
  struct CommandListInterval {
    std::size_t begin_index = 0;
    std::size_t end_index = 0;
    std::uint64_t end_sequence = 0;
  };

  // Build, in a single O(replay_commands_) pass, the closed command intervals per command_list_id.
  // find_submitted_interval / find_closed_interval_before previously each scanned all replay_commands_
  // from index 0 on every call (once per list per batch) = O(total commands) per batch, which stalls
  // the heavy anchor batches for minutes once replay_commands_ holds millions of entries. With this
  // index those lookups become a binary search over one list's intervals.
  void build_command_list_intervals()
  {
    command_list_intervals_.clear();
    std::unordered_map<trace::ObjectId, std::size_t> open_begin;
    for (std::size_t index = 0; index < backend_.replay_commands_.size(); ++index) {
      const auto &command = backend_.replay_commands_[index];
      if (command.kind == D3D12ReplayBackend::ReplayCommandKind::BeginCommandList) {
        open_begin[command.command_list_object_id] = index;
      } else if (command.kind == D3D12ReplayBackend::ReplayCommandKind::EndCommandList) {
        const auto it = open_begin.find(command.command_list_object_id);
        if (it != open_begin.end()) {
          command_list_intervals_[command.command_list_object_id].push_back(
              CommandListInterval{it->second, index, command.sequence});
          open_begin.erase(it);
        }
      }
    }
    // Lazily-filled per-command parsed-payload cache. Command intervals are re-recorded across many
    // batches (a recorded list submitted repeatedly), so the same command index is parsed multiple
    // times (measured: ~1.5x+ for the hottest kinds). Parsing each payload at most once and reusing
    // the DOM removes that redundant json::parse, the dominant retrace-side CPU cost. is_discarded()
    // marks "not yet parsed" (a successfully parsed payload is an object, never discarded).
    payload_dom_cache_.assign(backend_.replay_commands_.size(), json(json::value_t::discarded));
  }

  // Parsed payload for a command index, parsed at most once and cached. Returns a null/discarded json
  // if the payload is not valid JSON (callers already handle non-object payloads).
  const json &cached_payload(std::size_t index, const std::string &payload_text)
  {
    json &slot = payload_dom_cache_[index];
    if (slot.is_discarded()) {
      slot = json::parse(payload_text, nullptr, false);
    }
    return slot;
  }

  // Largest closed interval for list_id whose EndCommandList sequence is < limit_sequence. Mirrors
  // the old linear scans (which returned the last closed Begin/End pair before encountering a command
  // of that list at sequence >= the limit), now via binary search over the per-list interval index.
  const CommandListInterval *interval_before(trace::ObjectId command_list_id,
                                             std::uint64_t limit_sequence) const
  {
    const auto it = command_list_intervals_.find(command_list_id);
    if (it == command_list_intervals_.end() || it->second.empty()) {
      return nullptr;
    }
    const auto &intervals = it->second;  // ascending by end_sequence
    auto lower = std::lower_bound(
        intervals.begin(), intervals.end(), limit_sequence,
        [](const CommandListInterval &iv, std::uint64_t seq) { return iv.end_sequence < seq; });
    if (lower == intervals.begin()) {
      return nullptr;
    }
    return &*(lower - 1);
  }

  bool find_submitted_interval(
      trace::ObjectId command_list_id,
      std::uint64_t execute_sequence,
      std::size_t &begin_index,
      std::size_t &end_index,
      std::string &error) const
  {
    if (const CommandListInterval *iv = interval_before(command_list_id, execute_sequence)) {
      begin_index = iv->begin_index;
      end_index = iv->end_index;
      return true;
    }
    error = "D3D12 native command replay incomplete: submitted command list has no closed interval";
    return false;
  }

  bool find_closed_interval_before(
      trace::ObjectId command_list_id,
      std::uint64_t sequence_limit,
      std::size_t &begin_index,
      std::size_t &end_index,
      std::string &error) const
  {
    if (const CommandListInterval *iv = interval_before(command_list_id, sequence_limit)) {
      begin_index = iv->begin_index;
      end_index = iv->end_index;
      return true;
    }
    error = "D3D12 native command replay incomplete: command list has no closed interval";
    return false;
  }

  bool replay_submissions(std::string &error)
  {
    // Build the per-command-list interval index once, so per-batch interval lookups are a binary
    // search instead of a full scan of replay_commands_ (which is O(total commands) per batch).
    build_command_list_intervals();
    for (const auto &batch : backend_.submissions_.completed_batches()) {
      // Per-batch timing + command-count instrumentation: a batch that takes a long time gets a
      // milestone with how many commands it recorded, so a slow anchor batch is attributable to
      // command volume (json parse / record cost) vs per-binding resolution cost. Cheap (one clock
      // read per batch); only logs above a threshold so it does not flood.
      const auto batch_begin = std::chrono::steady_clock::now();
      std::size_t batch_command_count = 0;
      std::vector<ID3D12CommandList *> native_lists;
      std::vector<NativeCommandList *> recorded_native_lists;
      native_lists.reserve(batch.command_list_ids.size());
      recorded_native_lists.reserve(batch.command_list_ids.size());
      // D3D12 command lists capture GPU virtual addresses, GPU descriptor handles, and commands;
      // upload buffer and shader-visible descriptor heap contents are still read by the GPU when
      // ExecuteCommandLists runs. Translation layers such as DXMT may encode backend commands while
      // the command list is being replay-recorded, so preload execution-time resource data and
      // shader-visible descriptors before recording the lists. CPU descriptors used by commands
      // such as OMSetRenderTargets are still materialized at their command sequence below.
      if (!sync_resource_data_until(batch.execute_sequence, error) ||
          !sync_descriptors_until(batch.execute_sequence, error)) {
        if (error.empty()) {
          error = "batch pre-record sync failed (batch=" +
                  std::to_string(replay_batch_index_) +
                  " seq=" + std::to_string(batch.execute_sequence) + ")";
        }
        return false;
      }
      for (const auto command_list_id : batch.command_list_ids) {
        auto native_it = command_lists_.find(command_list_id);
        if (native_it == command_lists_.end()) {
          return fail(error, "submission references an unknown command list");
        }
        std::size_t begin_index = 0;
        std::size_t end_index = 0;
        if (!find_submitted_interval(command_list_id, batch.execute_sequence, begin_index, end_index, error)) {
          if (error.empty()) {
            error = "find_submitted_interval failed (batch=" + std::to_string(replay_batch_index_) +
                    " list=" + std::to_string(command_list_id) + ")";
          }
          return false;
        }
        batch_command_count += (end_index > begin_index) ? (end_index - begin_index) : 0;
        if (!record_command_list(native_it->second, begin_index, end_index, error)) {
          if (error.empty()) {
            error = "record_command_list failed (batch=" + std::to_string(replay_batch_index_) +
                    " list=" + std::to_string(command_list_id) + ")";
          }
          return false;
        }
        native_lists.push_back(native_it->second.list.get());
        recorded_native_lists.push_back(&native_it->second);
      }
      if (!sync_resource_data_until(batch.execute_sequence, error) ||
          !sync_descriptors_until(batch.execute_sequence, error) ||
          !sync_tile_mapping_updates_until(batch.execute_sequence, error)) {
        if (error.empty()) {
          error = "batch pre-submit sync failed (batch=" + std::to_string(replay_batch_index_) +
                  " seq=" + std::to_string(batch.execute_sequence) + ")";
        }
        return false;
      }
      if (!native_lists.empty()) {
        // Route the batch to a queue matching its command-list type (DIRECT/COMPUTE/COPY). All
        // lists in a batch share a type (they executed on one queue). With a full wait_for_gpu after
        // every batch, cross-queue ordering (e.g. compute auto-exposure -> graphics tonemap) is
        // preserved by serialization without needing captured fence replay.
        D3D12_COMMAND_LIST_TYPE batch_type = command_lists_.at(batch.command_list_ids.front()).type;
        ID3D12CommandQueue *batch_queue = get_or_create_queue(batch_type);
        if (!batch_queue) {
          return fail(error, "failed to create command queue for batch list type");
        }
        // Bounded per-batch markers: every batch for the first few, then sparse. Lets us see
        // whether a stall is between ExecuteCommandLists and wait_for_gpu (DXMT submission) or in
        // the GPU wait itself, without flooding for a full replay.
        const bool trace_batch = replay_batch_index_ < 8 || (replay_batch_index_ % 50) == 0;
        if (trace_batch) {
          replay_milestone((std::string("submit:execute batch=") +
                            std::to_string(replay_batch_index_) +
                            " seq=" + std::to_string(batch.execute_sequence) +
                            " qtype=" + std::to_string(static_cast<int>(batch_type))).c_str());
        }
        batch_queue->ExecuteCommandLists(static_cast<UINT>(native_lists.size()), native_lists.data());
        if (trace_batch) {
          replay_milestone((std::string("submit:executed batch=") +
                            std::to_string(replay_batch_index_)).c_str());
        }
        wait_for_gpu(batch_queue);
        if (trace_batch) {
          replay_milestone((std::string("submit:waited batch=") +
                            std::to_string(replay_batch_index_)).c_str());
        }
        if (!flush_target_texture_watch(recorded_native_lists, error)) {
          if (error.empty()) {
            error = "target texture watch failed (batch=" +
                    std::to_string(replay_batch_index_) +
                    " seq=" + std::to_string(batch.execute_sequence) + ")";
          }
          return false;
        }
        // Detect an asynchronous GPU fault from this batch's submission. wait_for_gpu() cannot tell
        // a completed fence from a removed device, so check explicitly and stop at the offending
        // batch with the actual reason instead of failing opaquely much later.
        const std::string removed = device_removed_detail();
        if (!removed.empty()) {
          return fail(error, "batch=" + std::to_string(replay_batch_index_) +
                                 " seq=" + std::to_string(batch.execute_sequence) + ": " + removed);
        }
      }
      // Attribute a slow batch: how long it took and how many commands it recorded. Threshold keeps
      // the common fast batches quiet. This pinpoints whether a stall is command-volume bound.
      {
        const auto batch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - batch_begin)
                                  .count();
        if (batch_ms >= 1000) {
          dump_command_kind_breakdown(
              (std::string("batch:slow batch=") + std::to_string(replay_batch_index_) +
               " seq=" + std::to_string(batch.execute_sequence) +
               " commands=" + std::to_string(batch_command_count) +
               " elapsed_ms=" + std::to_string(batch_ms))
                  .c_str());
        }
      }
      ++replay_batch_index_;
      const bool stop_targets_recorded_list = replay_stop_command_list_id() != 0;
      if (replay_stop_after_sequence_ != 0 &&
          (replay_stop_sequence_reached_ ||
           (!stop_targets_recorded_list && batch.execute_sequence >= replay_stop_after_sequence_))) {
        if (std::getenv("APITRACE_D3D12_REPLAY_MILESTONE_LOG")) {
          replay_milestone((std::string("probe:context label=stop-sequence index=") +
                            std::to_string(replay_stop_after_sequence_) +
                            " batch_seq=" + std::to_string(batch.execute_sequence)).c_str());
          probe_configured_resources(replay_stop_after_sequence_);
        }
        break;
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
          if (error.empty()) {
            error = "capture_present_frame failed (batch=" + std::to_string(replay_batch_index_) +
                    " frame=" + std::to_string(present_frame_index) + ")";
          }
          return false;
        }
        if (swap_chain_) {
          HRESULT hr = swap_chain_->Present(sync_interval, flags);
          record_present_semantics(sync_interval, flags, hr);
          if (FAILED(hr)) {
            error = hresult_error("IDXGISwapChain::Present", hr);
            return false;
          }
        } else {
          // Headless: no swapchain to flip. Capture already read the rendered back-buffer above.
          // Rotate the back-buffer index to mirror FLIP_DISCARD's 0,1,0,1 alternation so the
          // trace's per-resource swapchain_buffer_index aliasing stays consistent.
          record_present_semantics(sync_interval, flags, S_OK);
          headless_back_buffer_index_ = (headless_back_buffer_index_ + 1) % kBufferCount;
        }
        if (present_delay_ms_ != 0) {
#ifdef _WIN32
          Sleep(present_delay_ms_);
#else
          std::this_thread::sleep_for(std::chrono::milliseconds(present_delay_ms_));
#endif
        }
        if (swap_chain_ && !pump_messages()) {
          return fail(error, "replay window closed");
        }
        if (replay_stop_after_present_frame_ != 0 && present_frame_index >= replay_stop_after_present_frame_) {
          break;
        }
      }
    }
    return true;
  }

  bool sync_tile_mapping_updates_until(std::uint64_t sequence, std::string &error)
  {
    while (next_tile_mapping_update_index_ < backend_.tile_mapping_updates_.size() &&
           backend_.tile_mapping_updates_[next_tile_mapping_update_index_].sequence <= sequence) {
      if (!apply_tile_mapping_update(backend_.tile_mapping_updates_[next_tile_mapping_update_index_], error)) {
        return false;
      }
      ++next_tile_mapping_update_index_;
    }
    return true;
  }

  bool apply_tile_mapping_update(
      const D3D12ReplayBackend::TileMappingUpdateRecord &update,
      std::string &error)
  {
    const auto resource_it = resources_.find(update.resource_object_id);
    if (resource_it == resources_.end()) {
      return fail(error, "UpdateTileMappings references an unknown resource");
    }
    ID3D12Heap *native_heap = nullptr;
    if (update.heap_object_id != 0) {
      const auto heap_it = heaps_.find(update.heap_object_id);
      if (heap_it == heaps_.end()) {
        return fail(error, "UpdateTileMappings references an unknown heap");
      }
      native_heap = heap_it->second.heap.get();
    }

    json payload = json::parse(update.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return fail(error, "UpdateTileMappings payload must be a JSON object");
    }
    const auto region_count = payload_u32(payload, "region_count");
    const auto range_count = payload_u32(payload, "range_count");
    const auto regions_json = payload.find("regions");
    const auto region_sizes_json = payload.find("region_sizes");
    const auto range_flags_json = payload.find("range_flags");
    const auto heap_offsets_json = payload.find("heap_range_offsets");
    const auto tile_counts_json = payload.find("range_tile_counts");
    if (regions_json == payload.end() || !regions_json->is_array() ||
        region_sizes_json == payload.end() || !region_sizes_json->is_array() ||
        range_flags_json == payload.end() || !range_flags_json->is_array() ||
        heap_offsets_json == payload.end() || !heap_offsets_json->is_array() ||
        tile_counts_json == payload.end() || !tile_counts_json->is_array()) {
      return fail(error, "UpdateTileMappings payload arrays are missing");
    }
    if (regions_json->size() != region_count || region_sizes_json->size() != region_count ||
        range_flags_json->size() != range_count || heap_offsets_json->size() != range_count ||
        tile_counts_json->size() != range_count) {
      return fail(error, "UpdateTileMappings payload counts do not match arrays");
    }

    std::vector<D3D12_TILED_RESOURCE_COORDINATE> regions;
    std::vector<D3D12_TILE_REGION_SIZE> region_sizes;
    std::vector<D3D12_TILE_RANGE_FLAGS> range_flags;
    std::vector<UINT> heap_offsets;
    std::vector<UINT> tile_counts;
    regions.reserve(regions_json->size());
    region_sizes.reserve(region_sizes_json->size());
    range_flags.reserve(range_flags_json->size());
    heap_offsets.reserve(heap_offsets_json->size());
    tile_counts.reserve(tile_counts_json->size());

    for (const auto &region_json : *regions_json) {
      if (!region_json.is_object()) {
        return fail(error, "UpdateTileMappings region must be an object");
      }
      D3D12_TILED_RESOURCE_COORDINATE region{};
      region.Subresource = payload_u32(region_json, "subresource");
      region.X = payload_u32(region_json, "x");
      region.Y = payload_u32(region_json, "y");
      region.Z = payload_u32(region_json, "z");
      regions.push_back(region);
    }
    for (const auto &size_json : *region_sizes_json) {
      if (!size_json.is_object()) {
        return fail(error, "UpdateTileMappings region size must be an object");
      }
      D3D12_TILE_REGION_SIZE size{};
      size.NumTiles = payload_u32(size_json, "num_tiles");
      size.UseBox = payload_bool(size_json, "use_box");
      size.Width = payload_u32(size_json, "width");
      size.Height = payload_u32(size_json, "height");
      size.Depth = payload_u32(size_json, "depth");
      region_sizes.push_back(size);
    }
    for (const auto &flag_json : *range_flags_json) {
      range_flags.push_back(static_cast<D3D12_TILE_RANGE_FLAGS>(json_u64(flag_json)));
    }
    for (const auto &offset_json : *heap_offsets_json) {
      heap_offsets.push_back(static_cast<UINT>(json_u64(offset_json)));
    }
    for (const auto &count_json : *tile_counts_json) {
      tile_counts.push_back(static_cast<UINT>(json_u64(count_json)));
    }

#ifdef _WIN32
    // System (mingw) d3d12.h declares the standard 10-argument signature with
    // the pHeap parameter. This build is capture-only and never replays, but
    // it must compile.
    queue_->UpdateTileMappings(
        resource_it->second.resource.get(),
        region_count,
        regions.data(),
        region_sizes.data(),
        native_heap,
        range_count,
        range_flags.data(),
        heap_offsets.data(),
        tile_counts.data(),
        static_cast<D3D12_TILE_MAPPING_FLAGS>(payload_u32(payload, "flags")));
#else
    // The vendored third_party/d3d-headers d3d12.h used by the native retrace
    // build (matching DXMT's d3d12.dll vtable ABI) omits the pHeap parameter.
    (void)native_heap;
    queue_->UpdateTileMappings(
        resource_it->second.resource.get(),
        region_count,
        regions.data(),
        region_sizes.data(),
        range_count,
        range_flags.data(),
        heap_offsets.data(),
        tile_counts.data(),
        static_cast<D3D12_TILE_MAPPING_FLAGS>(payload_u32(payload, "flags")));
#endif
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

  static std::string hex32(std::uint32_t v)
  {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return std::string(buf);
  }

  // Read the center texel (raw 32-bit little-endian) of back_buffers_[index] via a tiny readback.
  // Diagnostic only: tells whether a back-buffer holds a flat clear color vs a real image.
  bool probe_back_buffer_center(UINT index, std::uint32_t &out, std::string &error)
  {
    if (index >= kBufferCount || !back_buffers_[index]) {
      return fail(error, "probe: back-buffer missing");
    }
    ID3D12Resource *bb = back_buffers_[index].get();
    const D3D12_RESOURCE_DESC desc = bb->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 row_size = 0, total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    if (total == 0) {
      return fail(error, "probe: zero footprint");
    }
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ScopedD3D12CaptureSuppression suppress;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.put())))) {
      return fail(error, "probe: readback alloc failed");
    }
    ReadbackCommandObjects cmds;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmds.allocator.put()))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmds.allocator.get(), nullptr, IID_PPV_ARGS(cmds.list.put())))) {
      return fail(error, "probe: cmd alloc failed");
    }
    const auto to_src = transition_barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmds.list->ResourceBarrier(1, &to_src);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb.get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    cmds.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto to_present = transition_barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    cmds.list->ResourceBarrier(1, &to_present);
    if (FAILED(cmds.list->Close())) {
      return fail(error, "probe: close failed");
    }
    ID3D12CommandList *lists[] = {cmds.list.get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();
    void *mapped = nullptr;
    if (FAILED(rb->Map(0, nullptr, &mapped)) || !mapped) {
      return fail(error, "probe: map failed");
    }
    const UINT cx = static_cast<UINT>(desc.Width) / 2, cy = static_cast<UINT>(desc.Height) / 2;
    const auto *px = static_cast<const std::uint8_t *>(mapped) + static_cast<std::size_t>(fp.Offset) +
                     static_cast<std::size_t>(cy) * fp.Footprint.RowPitch + static_cast<std::size_t>(cx) * 4u;
    out = static_cast<std::uint32_t>(px[0]) | (static_cast<std::uint32_t>(px[1]) << 8) |
          (static_cast<std::uint32_t>(px[2]) << 16) | (static_cast<std::uint32_t>(px[3]) << 24);
    // Full-buffer nonzero scan (sampled): how many sampled texels have any non-zero RGB bits. Tells
    // whether THIS back-buffer holds a real image even if the center texel is black (sky/road).
    probe_last_nonzero_permille_ = 0;
    {
      const UINT w = static_cast<UINT>(desc.Width), h = static_cast<UINT>(desc.Height);
      std::uint64_t nz = 0, cnt = 0;
      const auto *base = static_cast<const std::uint8_t *>(mapped) + static_cast<std::size_t>(fp.Offset);
      for (UINT y = 0; y < h; y += 8) {
        const auto *row = base + static_cast<std::size_t>(y) * fp.Footprint.RowPitch;
        for (UINT x = 0; x < w; x += 8) {
          const auto *p = row + static_cast<std::size_t>(x) * 4u;
          // R10G10B10A2 little-endian: nonzero if any of the 30 RGB bits set (mask off the 2 alpha bits)
          const std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                  (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
          if (v & 0x3FFFFFFFu) ++nz;
          ++cnt;
        }
      }
      if (cnt) probe_last_nonzero_permille_ = static_cast<std::uint32_t>(nz * 1000 / cnt);
    }
    D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    return true;
  }

  // Probe an arbitrary TEXTURE2D resource (by object_id) for non-zero content (sampled). Transitions
  // from COMMON (decay-safe for most resources). Diagnostic only (BUG-007): tells whether an offscreen
  // render target (e.g. scene-color) holds a real image, to separate "scene render empty" from
  // "back-buffer composite/present issue". Logs a milestone; returns false on setup failure.
  bool probe_texture_nonzero(trace::ObjectId object_id, std::uint64_t frame_index)
  {
    std::string ignore;
    ID3D12Resource *res = resource_for(object_id, ignore);
    if (!res) {
      replay_milestone((std::string("probe-tex object=") + std::to_string(object_id) +
                        " UNRESOLVED").c_str());
      return false;
    }
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
      replay_milestone((std::string("probe-tex object=") + std::to_string(object_id) +
                        " not-tex2d dim=" + std::to_string(static_cast<int>(desc.Dimension))).c_str());
      return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 row_size = 0, total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    if (total == 0) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ScopedD3D12CaptureSuppression suppress;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.put())))) return false;
    ReadbackCommandObjects cmds;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmds.allocator.put()))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmds.allocator.get(), nullptr, IID_PPV_ARGS(cmds.list.put())))) return false;
    // No explicit barrier: an explicit transition from a guessed before-state (e.g. COMMON) fought
    // DXMT's own state tracking and caused a device fault. Rely on implicit COMMON promotion / DXMT's
    // tracking; a state mismatch only warns. Diagnostic copy only.
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb.get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    cmds.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (FAILED(cmds.list->Close())) return false;
    ID3D12CommandList *lists[] = {cmds.list.get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();
    void *mapped = nullptr;
    if (FAILED(rb->Map(0, nullptr, &mapped)) || !mapped) return false;
    const UINT w = static_cast<UINT>(desc.Width), h = static_cast<UINT>(desc.Height);
    const auto *base = static_cast<const std::uint8_t *>(mapped) + static_cast<std::size_t>(fp.Offset);
    std::uint64_t nz = 0, cnt = 0;
    std::uint64_t sum[4] = {};
    std::uint32_t minv[4] = {255u, 255u, 255u, 255u};
    std::uint32_t maxv[4] = {};
    bool rgba_stats_supported = true;
    for (UINT y = 0; y < h; y += 8) {
      const auto *row = base + static_cast<std::size_t>(y) * fp.Footprint.RowPitch;
      for (UINT x = 0; x < w; x += 8) {
        const auto fmt = static_cast<DXGI_FORMAT>(desc.Format);
        const UINT bytes_per_pixel = fmt == DXGI_FORMAT_R8_UNORM ? 1u : 4u;
        const auto *p = row + static_cast<std::size_t>(x) * bytes_per_pixel;
        std::uint32_t rgba[4] = {};
        switch (fmt) {
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
          const std::uint32_t px =
              static_cast<std::uint32_t>(p[0]) |
              (static_cast<std::uint32_t>(p[1]) << 8) |
              (static_cast<std::uint32_t>(p[2]) << 16) |
              (static_cast<std::uint32_t>(p[3]) << 24);
          rgba[0] = ((px & 0x3FFu) * 255u + 511u) / 1023u;
          rgba[1] = (((px >> 10) & 0x3FFu) * 255u + 511u) / 1023u;
          rgba[2] = (((px >> 20) & 0x3FFu) * 255u + 511u) / 1023u;
          rgba[3] = (((px >> 30) & 0x3u) * 255u + 1u) / 3u;
          break;
        }
        case DXGI_FORMAT_R8G8B8A8_UNORM:
          rgba[0] = p[0];
          rgba[1] = p[1];
          rgba[2] = p[2];
          rgba[3] = p[3];
          break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
          rgba[0] = p[2];
          rgba[1] = p[1];
          rgba[2] = p[0];
          rgba[3] = fmt == DXGI_FORMAT_B8G8R8X8_UNORM ? 255u : p[3];
          break;
        case DXGI_FORMAT_R8_UNORM:
          rgba[0] = rgba[1] = rgba[2] = p[0];
          rgba[3] = 255u;
          break;
        default:
          rgba_stats_supported = false;
          break;
        }
        if (rgba[0] || rgba[1] || rgba[2] || rgba[3]) ++nz;
        if (rgba_stats_supported) {
          for (UINT c = 0; c < 4; ++c) {
            sum[c] += rgba[c];
            minv[c] = std::min(minv[c], rgba[c]);
            maxv[c] = std::max(maxv[c], rgba[c]);
          }
        }
        ++cnt;
      }
    }
    write_probe_texture_ppm(object_id, frame_index, desc, fp, base);
    D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    std::string message = std::string("probe-tex object=") + std::to_string(object_id) +
                          " frame=" + std::to_string(frame_index) + " fmt=" +
                          std::to_string(static_cast<int>(desc.Format)) + " " +
                          std::to_string(w) + "x" + std::to_string(h) +
                          " nonzero_permille=" + std::to_string(cnt ? nz * 1000 / cnt : 0);
    if (rgba_stats_supported && cnt) {
      auto append_channel = [&](const char *name, UINT c) {
        char stats[128];
        std::snprintf(stats, sizeof(stats), " %s[min=%u max=%u mean=%.2f]",
                      name, minv[c], maxv[c], static_cast<double>(sum[c]) / static_cast<double>(cnt));
        message += stats;
      };
      append_channel("r", 0);
      append_channel("g", 1);
      append_channel("b", 2);
      append_channel("a", 3);
    }
    replay_milestone(message.c_str());
    return true;
  }

  void write_probe_texture_ppm(
      trace::ObjectId object_id,
      std::uint64_t frame_index,
      const D3D12_RESOURCE_DESC &desc,
      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp,
      const std::uint8_t *base)
  {
#ifdef _WIN32
    const char *sink_dir = std::getenv("APITRACE_D3D12_RETRACE_PRESENT_FRAME_DIR");
    if (sink_dir == nullptr || *sink_dir == '\0') {
      return;
    }
    const UINT w = static_cast<UINT>(desc.Width);
    const UINT h = static_cast<UINT>(desc.Height);
    if (w == 0 || h == 0) {
      return;
    }
    const auto fmt = static_cast<DXGI_FORMAT>(desc.Format);
    switch (fmt) {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R8_UNORM:
      break;
    default:
      replay_milestone((std::string("probe-tex-dump object=") + std::to_string(object_id) +
                        " frame=" + std::to_string(frame_index) +
                        " skipped fmt=" + std::to_string(static_cast<int>(desc.Format))).c_str());
      return;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(sink_dir), ec);
    if (ec) {
      replay_milestone((std::string("probe-tex-dump object=") + std::to_string(object_id) +
                        " frame=" + std::to_string(frame_index) +
                        " mkdir-failed " + ec.message()).c_str());
      return;
    }

    char stem[128];
    std::snprintf(stem, sizeof(stem), "probe-texture-object-%llu-frame-%llu",
                  static_cast<unsigned long long>(object_id),
                  static_cast<unsigned long long>(frame_index));
    const fs::path ppm_path = fs::path(sink_dir) / (std::string(stem) + ".ppm");
    std::ofstream ppm(ppm_path, std::ios::binary | std::ios::trunc);
    if (!ppm) {
      replay_milestone((std::string("probe-tex-dump object=") + std::to_string(object_id) +
                        " frame=" + std::to_string(frame_index) +
                        " open-failed " + ppm_path.string()).c_str());
      return;
    }

    ppm << "P6\n" << w << ' ' << h << "\n255\n";
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * 3u);
    for (UINT y = 0; y < h; ++y) {
      const auto *src_row = base + static_cast<std::size_t>(y) * fp.Footprint.RowPitch;
      for (UINT x = 0; x < w; ++x) {
        auto *dst = rgb.data() + static_cast<std::size_t>(x) * 3u;
        switch (fmt) {
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
          const auto *src = src_row + static_cast<std::size_t>(x) * 4u;
          const std::uint32_t px =
              static_cast<std::uint32_t>(src[0]) |
              (static_cast<std::uint32_t>(src[1]) << 8) |
              (static_cast<std::uint32_t>(src[2]) << 16) |
              (static_cast<std::uint32_t>(src[3]) << 24);
          const std::uint32_t r10 = px & 0x3FFu;
          const std::uint32_t g10 = (px >> 10) & 0x3FFu;
          const std::uint32_t b10 = (px >> 20) & 0x3FFu;
          dst[0] = static_cast<std::uint8_t>((r10 * 255u + 511u) / 1023u);
          dst[1] = static_cast<std::uint8_t>((g10 * 255u + 511u) / 1023u);
          dst[2] = static_cast<std::uint8_t>((b10 * 255u + 511u) / 1023u);
          break;
        }
        case DXGI_FORMAT_R8G8B8A8_UNORM: {
          const auto *src = src_row + static_cast<std::size_t>(x) * 4u;
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
          break;
        }
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM: {
          const auto *src = src_row + static_cast<std::size_t>(x) * 4u;
          dst[0] = src[2];
          dst[1] = src[1];
          dst[2] = src[0];
          break;
        }
        case DXGI_FORMAT_R8_UNORM: {
          const std::uint8_t v = src_row[x];
          dst[0] = v;
          dst[1] = v;
          dst[2] = v;
          break;
        }
        default:
          dst[0] = dst[1] = dst[2] = 0;
          break;
        }
      }
      ppm.write(reinterpret_cast<const char *>(rgb.data()),
                static_cast<std::streamsize>(rgb.size()));
    }
    if (!ppm) {
      replay_milestone((std::string("probe-tex-dump object=") + std::to_string(object_id) +
                        " frame=" + std::to_string(frame_index) +
                        " write-failed " + ppm_path.string()).c_str());
      return;
    }
    replay_milestone((std::string("probe-tex-dump object=") + std::to_string(object_id) +
                      " frame=" + std::to_string(frame_index) +
                      " path=" + ppm_path.string()).c_str());
#else
    (void)object_id;
    (void)frame_index;
    (void)desc;
    (void)fp;
    (void)base;
#endif
  }

  // Decode a half-precision (IEEE 754 binary16) bit pattern to float. Diagnostic helper for
  // probe_texture_values (R16_FLOAT / R16G16B16A16_FLOAT / R11G11B10_FLOAT channels).
  static float half_bits_to_float(std::uint32_t h, unsigned mant_bits, unsigned exp_bits)
  {
    const unsigned exp_mask = (1u << exp_bits) - 1u;
    const int bias = (1 << (exp_bits - 1)) - 1;
    const std::uint32_t mant = h & ((1u << mant_bits) - 1u);
    const std::uint32_t exp = (h >> mant_bits) & exp_mask;
    if (exp == 0) {
      return mant == 0 ? 0.0f
                       : static_cast<float>(mant) / static_cast<float>(1u << mant_bits) *
                             std::pow(2.0f, static_cast<float>(1 - bias));
    }
    if (exp == exp_mask) {
      return mant == 0 ? std::numeric_limits<float>::infinity()
                       : std::numeric_limits<float>::quiet_NaN();
    }
    return (1.0f + static_cast<float>(mant) / static_cast<float>(1u << mant_bits)) *
           std::pow(2.0f, static_cast<float>(static_cast<int>(exp) - bias));
  }

  // Probe an arbitrary small TEXTURE2D (by object_id) and log decoded float texel values. Supports
  // R32_FLOAT / R16_FLOAT / R11G11B10_FLOAT / R16G16B16A16_FLOAT / R32G32B32A32_FLOAT. Logs the
  // first up-to-16 texels plus min/max/mean of channel 0.
  bool probe_texture_values(trace::ObjectId object_id, std::uint64_t frame_index)
  {
    std::string ignore;
    ID3D12Resource *res = resource_for(object_id, ignore);
    if (!res) {
      replay_milestone((std::string("probe-val object=") + std::to_string(object_id) +
                        " UNRESOLVED").c_str());
      return false;
    }
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
      replay_milestone((std::string("probe-val object=") + std::to_string(object_id) +
                        " not-tex2d dim=" + std::to_string(static_cast<int>(desc.Dimension))).c_str());
      return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 row_size = 0, total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    if (total == 0) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ScopedD3D12CaptureSuppression suppress;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.put())))) return false;
    ReadbackCommandObjects cmds;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmds.allocator.put()))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmds.allocator.get(), nullptr, IID_PPV_ARGS(cmds.list.put())))) return false;
    // No explicit barrier (same rationale as probe_texture_nonzero): rely on implicit COMMON
    // promotion / DXMT state tracking; a guessed before-state fought DXMT and faulted.
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb.get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    cmds.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (FAILED(cmds.list->Close())) return false;
    ID3D12CommandList *lists[] = {cmds.list.get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();
    void *mapped = nullptr;
    if (FAILED(rb->Map(0, nullptr, &mapped)) || !mapped) return false;
    const UINT w = static_cast<UINT>(desc.Width), h = static_cast<UINT>(desc.Height);
    const auto *base = static_cast<const std::uint8_t *>(mapped) + static_cast<std::size_t>(fp.Offset);
    const std::uint32_t fmt = static_cast<std::uint32_t>(desc.Format);

    auto decode_ch0 = [&](const std::uint8_t *p) -> float {
      const std::uint32_t u = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                              (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
      switch (fmt) {
      case 41:   // R32_FLOAT
      case 2: {  // R32G32B32A32_FLOAT (channel 0)
        float f; std::memcpy(&f, &u, 4); return f;
      }
      case 54:   // R16_FLOAT
      case 10:   // R16G16B16A16_FLOAT (channel 0)
        return half_bits_to_float(u & 0xFFFFu, 10, 5);
      case 26:   // R11G11B10_FLOAT (channel 0 = R: 6 mantissa, 5 exp, no sign)
        return half_bits_to_float(u & 0x7FFu, 6, 5);
      default: {
        float f; std::memcpy(&f, &u, 4); return f;  // best-effort: treat as float32
      }
      }
    };
    // bytes per texel for stepping columns
    UINT bpt = 4;
    switch (fmt) { case 2: bpt = 16; break; case 10: bpt = 8; break; default: bpt = 4; break; }

    std::string head;  // first up-to-16 ch0 floats
    float vmin = std::numeric_limits<float>::infinity(), vmax = -std::numeric_limits<float>::infinity();
    double sum = 0.0; std::uint64_t cnt = 0; UINT printed = 0;
    for (UINT y = 0; y < h; ++y) {
      const auto *row = base + static_cast<std::size_t>(y) * fp.Footprint.RowPitch;
      for (UINT x = 0; x < w; ++x) {
        const float v = decode_ch0(row + static_cast<std::size_t>(x) * bpt);
        if (v == v) { vmin = std::min(vmin, v); vmax = std::max(vmax, v); sum += v; ++cnt; }
        if (printed < 16) {
          char b[40]; std::snprintf(b, sizeof(b), "%s%.6g", printed ? "," : "", v); head += b; ++printed;
        }
      }
    }
    D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    char stat[160];
    std::snprintf(stat, sizeof(stat), " min=%.6g max=%.6g mean=%.6g",
                  cnt ? vmin : 0.0f, cnt ? vmax : 0.0f, cnt ? sum / static_cast<double>(cnt) : 0.0);
    replay_milestone((std::string("probe-val object=") + std::to_string(object_id) +
                      " frame=" + std::to_string(frame_index) + " fmt=" + std::to_string(fmt) +
                      " " + std::to_string(w) + "x" + std::to_string(h) +
                      " ch0=[" + head + "]" + stat).c_str());
    return true;
  }

  void probe_configured_resources(std::uint64_t frame_index)
  {
    // Reuse the existing retrace probe controls for both present-time and stop-sequence diagnostics.
    if (const std::uint64_t probe_obj = env_u64("APITRACE_D3D12_RETRACE_PROBE_RESOURCE")) {
      std::string probe_err;
      if (!probe_resource_head(static_cast<trace::ObjectId>(probe_obj), 64, probe_err)) {
        replay_milestone((std::string("probe-res frame=") + std::to_string(frame_index) +
                          " FAILED: " + probe_err).c_str());
      }
    }
    if (const std::uint64_t probe_tex = env_u64("APITRACE_D3D12_RETRACE_PROBE_TEXTURE")) {
      probe_texture_nonzero(static_cast<trace::ObjectId>(probe_tex), frame_index);
    }
    if (const char *tv = std::getenv("APITRACE_D3D12_RETRACE_PROBE_TEXTURE_VALUES")) {
      std::string_view sv(tv);
      std::size_t pos = 0;
      while (pos < sv.size()) {
        std::size_t comma = sv.find(',', pos);
        const std::size_t end = (comma == std::string_view::npos) ? sv.size() : comma;
        const std::string tok(sv.substr(pos, end - pos));
        const std::uint64_t id = tok.empty() ? 0 : std::strtoull(tok.c_str(), nullptr, 10);
        if (id != 0) {
          probe_texture_values(static_cast<trace::ObjectId>(id), frame_index);
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
      }
    }
  }

  // Read the first `count` bytes of a DEFAULT/any-heap resource by GPU-copying into a readback
  // buffer. Diagnostic only (BUG-007): tells whether a vertex/index/texture resource that a draw
  // reads actually holds non-zero data, i.e. whether the upload->copy chain populated it.
  bool probe_resource_head(trace::ObjectId object_id, std::size_t count, std::string &error)
  {
    std::string ignore;
    ID3D12Resource *res = resource_for(object_id, ignore);
    if (!res) {
      return fail(error, "probe-res: object not resolvable");
    }
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
      return fail(error, "probe-res: not a buffer");
    }
    const std::size_t n = std::min<std::size_t>(count, static_cast<std::size_t>(desc.Width));
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = n;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ScopedD3D12CaptureSuppression suppress;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.put())))) {
      return fail(error, "probe-res: readback alloc failed");
    }
    ReadbackCommandObjects cmds;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmds.allocator.put()))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmds.allocator.get(), nullptr, IID_PPV_ARGS(cmds.list.put())))) {
      return fail(error, "probe-res: cmd alloc failed");
    }
    cmds.list->CopyBufferRegion(rb.get(), 0, res, 0, n);
    if (FAILED(cmds.list->Close())) {
      return fail(error, "probe-res: close failed");
    }
    ID3D12CommandList *lists[] = {cmds.list.get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();
    void *mapped = nullptr;
    if (FAILED(rb->Map(0, nullptr, &mapped)) || !mapped) {
      return fail(error, "probe-res: map failed");
    }
    const auto *b = static_cast<const std::uint8_t *>(mapped);
    std::size_t nz = 0;
    std::string head;
    for (std::size_t i = 0; i < n; ++i) {
      if (b[i] != 0) ++nz;
      if (i < 16) head += hex32(b[i]).substr(6);  // last 2 hex chars = the byte
    }
    D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    replay_milestone((std::string("probe-res object=") + std::to_string(object_id) +
                      " bytes=" + std::to_string(n) + " nonzero=" + std::to_string(nz) +
                      " head=" + head).c_str());
    return true;
  }

  bool capture_present_frame(
      std::uint64_t frame_index,
      UINT sync_interval,
      UINT flags,
      std::string &error)
  {
    if (!d3d12_retrace_present_frame_capture_enabled()) {
      return true;
    }
    // Optional lower bound: only capture frames >= APITRACE_D3D12_RETRACE_CAPTURE_FROM_FRAME. Lets a
    // replay seek to a deep frame (e.g. the anomaly anchor) without writing thousands of GB of
    // earlier frames, and lets perf runs capture only the frames of interest. The GPU readback is
    // also skipped for earlier frames, so it adds no per-frame cost before the window.
    static const std::uint64_t capture_from_frame =
        env_u64("APITRACE_D3D12_RETRACE_CAPTURE_FROM_FRAME");
    if (capture_from_frame != 0 && frame_index < capture_from_frame) {
      return true;
    }
    // Optional stride: with APITRACE_D3D12_RETRACE_CAPTURE_STRIDE=N capture only every Nth present
    // frame (relative to capture_from_frame), so a long replay yields a sparse set of frames to eyeball
    // for correctness instead of one per present. N=0/1 means capture every frame.
    static const std::uint64_t capture_stride =
        env_u64("APITRACE_D3D12_RETRACE_CAPTURE_STRIDE");
    if (capture_stride > 1 && ((frame_index - capture_from_frame) % capture_stride) != 0) {
      return true;
    }
#ifdef _WIN32
    // Capture needs either the proxy export (standalone capture d3d12.dll) or a
    // direct file sink dir (real translation-layer d3d12.dll, e.g. DXMT). Bail
    // early only if neither is available, so we don't do a pointless readback.
    static RecordPresentFrameFn proxy_record_present_frame =
        resolve_d3d12_export<RecordPresentFrameFn>("apitrace_d3d12_record_present_frame");
    const char *sink_dir = std::getenv("APITRACE_D3D12_RETRACE_PRESENT_FRAME_DIR");
    if (!proxy_record_present_frame && !(sink_dir && *sink_dir)) {
      return fail(error,
                  "APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES is enabled but neither the "
                  "capture export nor APITRACE_D3D12_RETRACE_PRESENT_FRAME_DIR is available");
    }
#endif

    const UINT back_buffer_index =
        swap_chain_ ? swap_chain_->GetCurrentBackBufferIndex() : headless_back_buffer_index_;
    if (back_buffer_index >= kBufferCount || !back_buffers_[back_buffer_index]) {
      return fail(error, "swapchain back-buffer is missing for present-frame capture");
    }
    // Diagnostic: dump the center texel of EVERY back-buffer at this present. If all are a flat
    // color, the scene never landed on any swapchain back-buffer (RTV/binding bug); if one holds a
    // real image, the captured index is just rotated wrong. Gated on the milestone log being set.
    if (std::getenv("APITRACE_D3D12_REPLAY_MILESTONE_LOG")) {
      for (UINT bi = 0; bi < kBufferCount; ++bi) {
        std::uint32_t center = 0;
        std::string probe_err;
        if (probe_back_buffer_center(bi, center, probe_err)) {
          replay_milestone((std::string("capture:probe frame=") + std::to_string(frame_index) +
                            " using_index=" + std::to_string(back_buffer_index) +
                            " bb[" + std::to_string(bi) + "]_center=0x" +
                            hex32(center) + " nonzero_permille=" +
                            std::to_string(probe_last_nonzero_permille_)).c_str());
        } else {
          replay_milestone((std::string("capture:probe frame=") + std::to_string(frame_index) +
                            " bb[" + std::to_string(bi) + "] FAILED: " + probe_err).c_str());
        }
      }
      // Optional: probe a specific resource's head bytes (APITRACE_D3D12_RETRACE_PROBE_RESOURCE) to
      // see whether a vertex/index/texture buffer a draw reads is actually populated.
      probe_configured_resources(frame_index);
    }
    ID3D12Resource *back_buffer = back_buffers_[back_buffer_index].get();
    const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      break;
    default:
      return fail(error, "present-frame capture requires an RGBA8 or R10G10B10A2 swapchain");
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
    DXGI_FORMAT readback_decode_format = footprint.Footprint.Format;
    switch (readback_decode_format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      break;
    default:
      return fail(error, "present-frame capture produced an unsupported readback format");
    }
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

      auto decode_rows = [&](DXGI_FORMAT decode_format) {
        for (UINT row = 0; row < height; ++row) {
          const auto *src_row = static_cast<const std::uint8_t *>(mapped) +
                                static_cast<std::size_t>(footprint.Offset) +
                                static_cast<std::size_t>(row) * footprint.Footprint.RowPitch;
          auto *dst_row = rgba.data() + static_cast<std::size_t>(row) * row_pitch;
          if (decode_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            for (UINT col = 0; col < width; ++col) {
              const auto *src_px = src_row + static_cast<std::size_t>(col) * 4u;
              const std::uint32_t px =
                  static_cast<std::uint32_t>(src_px[0]) |
                  (static_cast<std::uint32_t>(src_px[1]) << 8) |
                  (static_cast<std::uint32_t>(src_px[2]) << 16) |
                  (static_cast<std::uint32_t>(src_px[3]) << 24);
              const std::uint32_t r10 = px & 0x3FFu;
              const std::uint32_t g10 = (px >> 10) & 0x3FFu;
              const std::uint32_t b10 = (px >> 20) & 0x3FFu;
              const std::uint32_t a2 = (px >> 30) & 0x3u;
              auto *dst_px = dst_row + static_cast<std::size_t>(col) * 4u;
              dst_px[0] = static_cast<std::uint8_t>((r10 * 255u + 511u) / 1023u);
              dst_px[1] = static_cast<std::uint8_t>((g10 * 255u + 511u) / 1023u);
              dst_px[2] = static_cast<std::uint8_t>((b10 * 255u + 511u) / 1023u);
              dst_px[3] = static_cast<std::uint8_t>(a2 * 85u);
            }
          } else {
            std::memcpy(dst_row, src_row, row_pitch);
          }
        }
      };
      decode_rows(readback_decode_format);
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
  // Write captured RGBA8 present frames to a sink directory as raw .rgba blobs
  // plus a viewable .ppm (P6, alpha dropped). Used when the loaded d3d12.dll is
  // a real translation layer (e.g. DXMT) that does not export the proxy capture
  // entry point. Returns false only on a genuine I/O error.
  bool write_present_frame_to_dir(
      const char *dir,
      std::uint64_t frame_index,
      UINT width,
      UINT height,
      UINT row_pitch,
      const void *rgba_data,
      SIZE_T rgba_size,
      std::string &error)
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec) {
      return fail(error, "failed to create present-frame sink dir: " + ec.message());
    }

    char stem[64];
    std::snprintf(stem, sizeof(stem), "present-frame-%06llu",
                  static_cast<unsigned long long>(frame_index));
    const auto *bytes = static_cast<const std::uint8_t *>(rgba_data);

    // Raw RGBA8 (tightly packed width*4 per row, no padding).
    const fs::path raw_path = fs::path(dir) / (std::string(stem) + ".rgba");
    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    if (!raw) {
      return fail(error, "failed to open present-frame raw file: " + raw_path.string());
    }
    raw.write(reinterpret_cast<const char *>(bytes), static_cast<std::streamsize>(rgba_size));
    if (!raw) {
      return fail(error, "failed to write present-frame raw file: " + raw_path.string());
    }
    raw.close();

    auto write_ppm = [&](const fs::path &ppm_path,
                         const std::uint8_t *ppm_bytes,
                         SIZE_T ppm_size) -> bool {
      if (!ppm_bytes || ppm_size == 0) {
        return true;
      }
      std::ofstream ppm(ppm_path, std::ios::binary | std::ios::trunc);
      if (!ppm) {
        return fail(error, "failed to open present-frame ppm file: " + ppm_path.string());
      }
      ppm << "P6\n" << width << ' ' << height << "\n255\n";
      std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * 3u);
      for (UINT row = 0; row < height; ++row) {
        const auto *src_row = ppm_bytes + static_cast<std::size_t>(row) * row_pitch;
        for (UINT col = 0; col < width; ++col) {
          const auto *px = src_row + static_cast<std::size_t>(col) * 4u;
          rgb[col * 3u + 0] = px[0];
          rgb[col * 3u + 1] = px[1];
          rgb[col * 3u + 2] = px[2];
        }
        ppm.write(reinterpret_cast<const char *>(rgb.data()),
                  static_cast<std::streamsize>(rgb.size()));
      }
      if (!ppm) {
        return fail(error, "failed to write present-frame ppm file: " + ppm_path.string());
      }
      return true;
    };

    const fs::path ppm_path = fs::path(dir) / (std::string(stem) + ".ppm");
    if (!write_ppm(ppm_path, bytes, rgba_size)) {
      return false;
    }
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
    static RecordPresentFrameFn record =
        resolve_d3d12_export<RecordPresentFrameFn>("apitrace_d3d12_record_present_frame");
    if (record) {
      record(frame_index, width, height, row_pitch, sync_interval, flags, rgba_data, rgba_size);
      return true;
    }
    // No proxy capture export (real translation-layer d3d12.dll): fall back to a
    // direct file sink if the caller provided a target directory.
    const char *sink_dir = std::getenv("APITRACE_D3D12_RETRACE_PRESENT_FRAME_DIR");
    if (sink_dir && *sink_dir) {
      return write_present_frame_to_dir(sink_dir, frame_index, width, height, row_pitch,
                                        rgba_data, rgba_size, error);
    }
    return fail(error,
                "APITRACE_D3D12_RETRACE_CAPTURE_PRESENT_FRAMES is enabled but neither the "
                "capture export nor APITRACE_D3D12_RETRACE_PRESENT_FRAME_DIR is available");
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

  trace::ObjectId resource_object_id_for_cpu_descriptor(
      std::uint64_t descriptor,
      std::uint64_t sequence,
      std::string &error) const
  {
    D3D12ReplayBackend::DescriptorBinding binding;
    if (!backend_.resolve_descriptor_binding_at(descriptor, false, sequence, binding, error)) {
      return 0;
    }
    if (binding.heap_object_id == 0) {
      return 0;
    }
    const auto *semantic = descriptor_semantic_at(descriptor, sequence);
    if (!semantic || semantic->kind != "RenderTargetView") {
      return 0;
    }
    return semantic->resource_object_id;
  }

  trace::ObjectId target_texture_watch_object_id() const
  {
    return static_cast<trace::ObjectId>(env_u64("APITRACE_D3D12_RETRACE_PROBE_TEXTURE"));
  }

  bool target_texture_watch_enabled() const
  {
    return target_texture_watch_object_id() != 0;
  }

  bool target_texture_watch_near_stop_sequence(std::uint64_t sequence) const
  {
    return replay_stop_after_sequence_ != 0 &&
           sequence + 4096 >= replay_stop_after_sequence_ &&
           sequence <= replay_stop_after_sequence_;
  }

  bool target_texture_watch_focused_stop_sequence(std::uint64_t sequence) const
  {
    return replay_stop_after_sequence_ != 0 &&
           sequence + 160 >= replay_stop_after_sequence_ &&
           sequence <= replay_stop_after_sequence_;
  }

  static bool target_texture_watch_is_draw_command(
      const D3D12ReplayBackend::ReplayCommandRecord &command)
  {
    return command.kind == D3D12ReplayBackend::ReplayCommandKind::Draw;
  }

  std::string target_texture_watch_descriptor_summary(
      std::uint64_t descriptor,
      std::uint64_t sequence) const
  {
    if (descriptor == 0) {
      return "0";
    }
    std::ostringstream out;
    out << descriptor;
    const auto *semantic = descriptor_semantic_at(descriptor, sequence);
    if (!semantic) {
      out << ":none";
      return out.str();
    }
    out << ":" << semantic->kind
        << ":res=" << semantic->resource_object_id
        << ":fmt=" << semantic->format
        << ":dim=" << semantic->view_dimension;
    if (semantic->has_view_desc) {
      out << ":mip=" << semantic->most_detailed_mip
          << ":plane=" << semantic->plane_slice
          << ":slice=" << semantic->first_array_slice;
    }
    return out.str();
  }

  static std::string target_texture_watch_root_descriptor_summary(
      const TargetTextureWatchRootDescriptor &descriptor)
  {
    if (descriptor.resource_object_id == 0 && descriptor.kind.empty()) {
      return "0";
    }
    std::ostringstream out;
    out << (descriptor.kind.empty() ? "RootDescriptor" : descriptor.kind)
        << ":res=" << descriptor.resource_object_id
        << ":off=" << descriptor.offset;
    return out.str();
  }

  static std::string target_texture_watch_buffer_view_summary(
      const TargetTextureWatchBufferView &view)
  {
    if (!view.valid) {
      return "none";
    }
    std::ostringstream out;
    out << "res=" << view.resource_object_id
        << ":off=" << view.offset
        << ":size=" << view.size_in_bytes;
    if (view.stride_in_bytes != 0) {
      out << ":stride=" << view.stride_in_bytes;
    }
    if (view.format != 0) {
      out << ":fmt=" << view.format;
    }
    return out.str();
  }

  static std::string target_texture_watch_hex_bytes(
      const std::uint8_t *bytes,
      std::size_t count)
  {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
      out.push_back(kHex[(bytes[i] >> 4) & 0xf]);
      out.push_back(kHex[bytes[i] & 0xf]);
    }
    return out;
  }

  static float target_texture_watch_float32(const std::uint8_t *bytes)
  {
    float value = 0.0f;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
  }

  static std::uint16_t target_texture_watch_u16(const std::uint8_t *bytes)
  {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1] << 8);
  }

  void log_target_texture_watch_draw_state(
      const NativeCommandList &native,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      const std::string &reason) const
  {
    if (!std::getenv("APITRACE_D3D12_REPLAY_MILESTONE_LOG")) {
      return;
    }

    std::string rtvs;
    for (UINT i = 0; i < native.current_render_target_count &&
                     i < native.current_render_targets.size(); ++i) {
      rtvs += i ? "," : "";
      rtvs += std::to_string(native.current_render_targets[i]);
    }

    std::ostringstream out;
    out << "target-draw-state"
        << " object=" << target_texture_watch_object_id()
        << " seq=" << command.sequence
        << " list=" << command.command_list_object_id
        << " fn=" << command.function_name
        << " reason=" << reason
        << " pso=" << native.current_pipeline_state_object_id
        << " root_sig=" << native.current_graphics_root_signature_object_id
        << " topo=" << native.current_primitive_topology
        << " rtvs=" << rtvs
        << " vb0=" << target_texture_watch_buffer_view_summary(native.current_vertex_buffer0)
        << " ib=" << target_texture_watch_buffer_view_summary(native.current_index_buffer)
        << " draw_index_count=" << payload_u32(payload, "index_count_per_instance")
        << " draw_instance_count=" << payload_u32(payload, "instance_count")
        << " draw_start_index=" << payload_u32(payload, "start_index_location")
        << " draw_base_vertex=" << payload_i32(payload, "base_vertex_location");
    if (native.has_current_viewport) {
      out << " vp=" << native.current_viewport.TopLeftX << "," << native.current_viewport.TopLeftY
          << "," << native.current_viewport.Width << "," << native.current_viewport.Height
          << "," << native.current_viewport.MinDepth << "," << native.current_viewport.MaxDepth;
    } else {
      out << " vp=none";
    }
    if (native.has_current_scissor) {
      out << " sc=" << native.current_scissor.left << "," << native.current_scissor.top
          << "," << native.current_scissor.right << "," << native.current_scissor.bottom;
    } else {
      out << " sc=none";
    }
    for (UINT root : {5u, 6u, 7u, 8u, 12u}) {
      out << " rootTable" << root << "="
          << target_texture_watch_descriptor_summary(
                 native.current_graphics_root_tables[root], command.sequence);
    }
    for (UINT root : {0u, 1u, 2u, 3u, 4u, 9u, 10u, 11u, 13u, 14u, 15u}) {
      const auto &descriptor = native.current_graphics_root_descriptors[root];
      if (descriptor.resource_object_id != 0 || !descriptor.kind.empty()) {
        out << " rootDesc" << root << "="
            << target_texture_watch_root_descriptor_summary(descriptor);
      }
    }
    replay_milestone(out.str().c_str());
  }

  static bool target_texture_watch_is_blue(DXGI_FORMAT format, const std::uint8_t *px,
                                           UINT &r, UINT &g, UINT &b, UINT &a,
                                           std::uint32_t &u32)
  {
    r = g = b = 0;
    a = 255;
    u32 = static_cast<std::uint32_t>(px[0]) |
          (static_cast<std::uint32_t>(px[1]) << 8) |
          (static_cast<std::uint32_t>(px[2]) << 16) |
          (static_cast<std::uint32_t>(px[3]) << 24);
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      r = px[0];
      g = px[1];
      b = px[2];
      a = px[3];
      break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
      r = px[2];
      g = px[1];
      b = px[0];
      a = format == DXGI_FORMAT_B8G8R8X8_UNORM ? 255u : px[3];
      break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      r = ((u32 & 0x3FFu) * 255u + 511u) / 1023u;
      g = (((u32 >> 10) & 0x3FFu) * 255u + 511u) / 1023u;
      b = (((u32 >> 20) & 0x3FFu) * 255u + 511u) / 1023u;
      a = (((u32 >> 30) & 0x3u) * 255u + 1u) / 3u;
      break;
    default:
      return false;
    }
    return r <= 4 && g <= 4 && b >= 240;
  }

  bool append_target_texture_watch_readback(
      NativeCommandList &native,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const char *phase,
      const char *reason,
      std::string &error)
  {
    const trace::ObjectId target_object_id = target_texture_watch_object_id();
    if (target_object_id == 0) {
      return true;
    }
    ID3D12Resource *res = resource_for(target_object_id, error);
    if (!res) {
      return false;
    }
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count != 1) {
      return true;
    }
    switch (static_cast<DXGI_FORMAT>(desc.Format)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      break;
    default:
      return true;
    }
    const UINT x = static_cast<UINT>(desc.Width / 2);
    const UINT y = desc.Height / 2;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    fp.Offset = 0;
    fp.Footprint.Format = static_cast<DXGI_FORMAT>(desc.Format);
    fp.Footprint.Width = 1;
    fp.Footprint.Height = 1;
    fp.Footprint.Depth = 1;
    fp.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    const UINT64 total = fp.Footprint.RowPitch;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ScopedD3D12CaptureSuppression suppress;
    TargetTextureWatchReadback readback{};
    if (FAILED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(readback.readback.put())))) {
      return fail(error, "target texture watch readback allocation failed");
    }
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.readback.get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_BOX box{};
    box.left = x;
    box.top = y;
    box.front = 0;
    box.right = x + 1;
    box.bottom = y + 1;
    box.back = 1;
    native.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    readback.sequence = command.sequence;
    readback.command_list_object_id = command.command_list_object_id;
    readback.kind = command.kind;
    readback.function_name = command.function_name;
    readback.phase = phase ? phase : "";
    readback.reason = reason ? reason : "";
    readback.object_refs = command.object_refs;
    readback.footprint = fp;
    readback.x = x;
    readback.y = y;
    readback.width = static_cast<UINT>(desc.Width);
    readback.height = desc.Height;
    readback.format = static_cast<DXGI_FORMAT>(desc.Format);
    native.target_watch_readbacks.push_back(std::move(readback));
    return true;
  }

  bool begin_target_texture_watch_occlusion(
      NativeCommandList &native,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const std::string &reason,
      std::string &error)
  {
    ScopedD3D12CaptureSuppression suppress;
    TargetTextureWatchOcclusion occlusion{};
    D3D12_QUERY_HEAP_DESC query_desc{};
    query_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    query_desc.Count = 1;
    HRESULT hr = device_->CreateQueryHeap(
        &query_desc, IID_PPV_ARGS(occlusion.query_heap.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateQueryHeap(target watch occlusion)", hr);
      return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(std::uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(occlusion.readback.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateCommittedResource(target watch occlusion)", hr);
      return false;
    }

    native.list->BeginQuery(
        occlusion.query_heap.get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    occlusion.sequence = command.sequence;
    occlusion.command_list_object_id = command.command_list_object_id;
    occlusion.function_name = command.function_name;
    occlusion.reason = reason;
    occlusion.pipeline_state_object_id = native.current_pipeline_state_object_id;
    native.target_watch_occlusions.push_back(std::move(occlusion));
    return true;
  }

  bool end_target_texture_watch_occlusion(NativeCommandList &native, std::string &error)
  {
    if (native.target_watch_occlusions.empty()) {
      return true;
    }
    auto &occlusion = native.target_watch_occlusions.back();
    native.list->EndQuery(
        occlusion.query_heap.get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    native.list->ResolveQueryData(
        occlusion.query_heap.get(), D3D12_QUERY_TYPE_OCCLUSION,
        0, 1, occlusion.readback.get(), 0);
    (void)error;
    return true;
  }

  bool append_target_texture_watch_buffer_readback(
      NativeCommandList &native,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string_view label,
      trace::ObjectId resource_object_id,
      std::uint64_t offset,
      std::uint64_t requested_size,
      std::uint32_t stride_in_bytes,
      std::uint32_t format,
      std::string &error)
  {
    if (resource_object_id == 0 || requested_size == 0) {
      return true;
    }
    ID3D12Resource *res = resource_for(resource_object_id, error);
    if (!res) {
      return false;
    }
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset >= desc.Width) {
      return true;
    }
    const std::uint64_t size = std::min<std::uint64_t>(
        requested_size, static_cast<std::uint64_t>(desc.Width) - offset);
    if (size == 0) {
      return true;
    }

    TargetTextureWatchBufferReadback readback{};
    readback.sequence = command.sequence;
    readback.command_list_object_id = command.command_list_object_id;
    readback.label = std::string(label);
    readback.resource_object_id = resource_object_id;
    readback.offset = offset;
    readback.size = size;
    readback.stride_in_bytes = stride_in_bytes;
    readback.format = format;
    native.target_watch_buffer_readbacks.push_back(std::move(readback));
    return true;
  }

  bool populate_target_texture_watch_buffer_readback(
      TargetTextureWatchBufferReadback &readback,
      std::string &error)
  {
    if (readback.readback) {
      return true;
    }
    ID3D12Resource *res = resource_for(readback.resource_object_id, error);
    if (!res) {
      return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = readback.size;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ScopedD3D12CaptureSuppression suppress;
    HRESULT hr = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(readback.readback.put()));
    if (FAILED(hr)) {
      error = hresult_error("CreateCommittedResource(target watch buffer)", hr);
      return false;
    }
    ReadbackCommandObjects cmds;
    if (FAILED(device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmds.allocator.put()))) ||
        FAILED(device_->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmds.allocator.get(), nullptr,
            IID_PPV_ARGS(cmds.list.put())))) {
      return fail(error, "target watch buffer command allocation failed");
    }
    cmds.list->CopyBufferRegion(
        readback.readback.get(), 0, res, readback.offset, readback.size);
    if (FAILED(cmds.list->Close())) {
      return fail(error, "target watch buffer command close failed");
    }
    ID3D12CommandList *lists[] = {cmds.list.get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_for_gpu();
    return true;
  }

  bool append_target_texture_watch_draw_buffer_readbacks(
      NativeCommandList &native,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      std::string &error)
  {
    if (native.current_vertex_buffer0.valid) {
      const std::uint64_t vb_size =
          std::min<std::uint64_t>(native.current_vertex_buffer0.size_in_bytes, 512);
      if (!append_target_texture_watch_buffer_readback(
              native, command, "vb0",
              native.current_vertex_buffer0.resource_object_id,
              native.current_vertex_buffer0.offset,
              vb_size,
              native.current_vertex_buffer0.stride_in_bytes,
              0,
              error)) {
        return false;
      }
    }
    if (native.current_index_buffer.valid) {
      const std::uint64_t ib_size =
          std::min<std::uint64_t>(native.current_index_buffer.size_in_bytes, 256);
      if (!append_target_texture_watch_buffer_readback(
              native, command, "ib",
              native.current_index_buffer.resource_object_id,
              native.current_index_buffer.offset,
              ib_size,
              0,
              native.current_index_buffer.format,
              error)) {
        return false;
      }
    }
    for (UINT root : {0u, 1u, 2u, 3u, 4u}) {
      const auto &descriptor = native.current_graphics_root_descriptors[root];
      if (descriptor.resource_object_id == 0 || descriptor.kind != "RootCBV") {
        continue;
      }
      const std::string label = std::string("rootCBV") + std::to_string(root);
      if (!append_target_texture_watch_buffer_readback(
              native, command, label,
              descriptor.resource_object_id,
              descriptor.offset,
              256,
              0,
              0,
              error)) {
        return false;
      }
    }
    return true;
  }

  bool target_command_may_write_watched_texture(
      const NativeCommandList &native,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &reason,
      std::string &error) const
  {
    const trace::ObjectId target_object_id = target_texture_watch_object_id();
    if (target_object_id == 0) {
      return false;
    }

    switch (command.kind) {
    case D3D12ReplayBackend::ReplayCommandKind::SetRenderTargets:
    case D3D12ReplayBackend::ReplayCommandKind::SetDynamicState:
    case D3D12ReplayBackend::ReplayCommandKind::SetPipelineState:
    case D3D12ReplayBackend::ReplayCommandKind::SetRootSignature:
    case D3D12ReplayBackend::ReplayCommandKind::SetDescriptorHeaps:
    case D3D12ReplayBackend::ReplayCommandKind::SetRootDescriptorTable:
    case D3D12ReplayBackend::ReplayCommandKind::SetRootConstants:
    case D3D12ReplayBackend::ReplayCommandKind::SetRootConstantBufferView:
    case D3D12ReplayBackend::ReplayCommandKind::SetViewports:
    case D3D12ReplayBackend::ReplayCommandKind::SetScissorRects:
    case D3D12ReplayBackend::ReplayCommandKind::SetPrimitiveTopology:
    case D3D12ReplayBackend::ReplayCommandKind::SetVertexBuffers:
    case D3D12ReplayBackend::ReplayCommandKind::SetIndexBuffer:
    case D3D12ReplayBackend::ReplayCommandKind::ResourceBarrier:
    case D3D12ReplayBackend::ReplayCommandKind::BeginCommandList:
    case D3D12ReplayBackend::ReplayCommandKind::EndCommandList:
    case D3D12ReplayBackend::ReplayCommandKind::MapResource:
    case D3D12ReplayBackend::ReplayCommandKind::UnmapResource:
      return false;
    default:
      break;
    }

    if (command.kind == D3D12ReplayBackend::ReplayCommandKind::Draw ||
        command.kind == D3D12ReplayBackend::ReplayCommandKind::ExecuteIndirect ||
        command.kind == D3D12ReplayBackend::ReplayCommandKind::ExecuteBundle ||
        command.kind == D3D12ReplayBackend::ReplayCommandKind::RenderPass) {
      for (UINT i = 0; i < native.current_render_target_count &&
                       i < native.current_render_targets.size(); ++i) {
        if (native.current_render_targets[i] == target_object_id) {
          reason = "bound-rtv";
          return true;
        }
      }
      return false;
    }

    if (command.kind == D3D12ReplayBackend::ReplayCommandKind::ClearRenderTarget) {
      const auto descriptor = payload_u64(payload, "descriptor");
      if (descriptor != 0 &&
          resource_object_id_for_cpu_descriptor(descriptor, command.sequence, error) == target_object_id) {
        reason = "clear-rtv";
        return true;
      }
      return false;
    }

    if (command.kind == D3D12ReplayBackend::ReplayCommandKind::ClearUnorderedAccess ||
        command.kind == D3D12ReplayBackend::ReplayCommandKind::DiscardResource) {
      if (command.object_refs.size() >= 2 && command.object_refs[1] == target_object_id) {
        reason = command.kind == D3D12ReplayBackend::ReplayCommandKind::ClearUnorderedAccess
                     ? "clear-uav"
                     : "discard";
        return true;
      }
      return false;
    }

    if (command.kind == D3D12ReplayBackend::ReplayCommandKind::Copy) {
      if (command.function_name == "ID3D12GraphicsCommandList::CopyResource") {
        if (command.object_refs.size() >= 2 && command.object_refs[1] == target_object_id) {
          reason = "copy-resource-dst";
          return true;
        }
        return false;
      }
      if (command.function_name == "ID3D12GraphicsCommandList::CopyTextureRegion") {
        const auto dst = payload.find("dst");
        if (dst != payload.end() && dst->is_object() &&
            json_object_id((*dst)["resource_object_id"]) == target_object_id) {
          reason = "copy-texture-dst";
          return true;
        }
        return false;
      }
      if (command.function_name == "ID3D12GraphicsCommandList::CopyTextureRegionBatch") {
        const auto ops = payload.find("ops");
        if (ops != payload.end() && ops->is_array()) {
          for (const auto &op : *ops) {
            if (op.is_array() && op.size() > 1 && op[1].is_array() && !op[1].empty() &&
                json_object_id(op[1][0]) == target_object_id) {
              reason = "copy-texture-batch-dst";
              return true;
            }
          }
        }
        return false;
      }
      return false;
    }

    if (command.kind == D3D12ReplayBackend::ReplayCommandKind::Resolve) {
      if (command.object_refs.size() >= 2 && command.object_refs[1] == target_object_id) {
        reason = "resolve-dst";
        return true;
      }
      return false;
    }

    if (command.kind == D3D12ReplayBackend::ReplayCommandKind::Dispatch ||
        command.kind == D3D12ReplayBackend::ReplayCommandKind::TemporalUpscale ||
        command.kind == D3D12ReplayBackend::ReplayCommandKind::WriteBufferImmediate ||
        command.kind == D3D12ReplayBackend::ReplayCommandKind::Predication ||
        command.kind == D3D12ReplayBackend::ReplayCommandKind::Query) {
      return false;
    }

    return false;
  }

  bool update_target_texture_watch_render_targets(
      NativeCommandList &native,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (command.kind != D3D12ReplayBackend::ReplayCommandKind::SetRenderTargets) {
      return true;
    }
    native.current_render_targets = {};
    native.current_render_target_count = 0;
    const auto render_target_count = payload_u32(payload, "render_target_count");
    const auto render_targets = payload.find("render_targets");
    const bool has_render_target_array = render_targets != payload.end() && render_targets->is_array();
    const bool single_range =
        has_render_target_array &&
        (payload_bool(payload, "single_handle_to_descriptor_range") ||
         payload_bool(payload, "single_descriptor_handle")) &&
        render_targets->size() == 1 &&
        render_target_count > 1;

    D3D12ReplayBackend::DescriptorBinding first_binding;
    UINT descriptor_size = 0;
    if (single_range) {
      if (!backend_.resolve_descriptor_binding_at(json_u64((*render_targets)[0]), false,
                                                  command.sequence, first_binding, error)) {
        return false;
      }
      const auto heap_it = descriptor_heaps_.find(first_binding.heap_object_id);
      if (heap_it == descriptor_heaps_.end() || heap_it->second.descriptor_size == 0) {
        return true;
      }
      descriptor_size = heap_it->second.descriptor_size;
    }

    const UINT count = std::min<UINT>(
        render_target_count, static_cast<UINT>(native.current_render_targets.size()));
    for (UINT index = 0; index < count; ++index) {
      std::uint64_t descriptor = 0;
      if (single_range) {
        descriptor = first_binding.descriptor + static_cast<std::uint64_t>(index) * descriptor_size;
      } else if (has_render_target_array && index < render_targets->size()) {
        descriptor = json_u64((*render_targets)[index]);
      } else if (!has_render_target_array && render_target_count == 1) {
        descriptor = payload_u64(payload, "first_rtv");
      }
      if (descriptor == 0) {
        continue;
      }
      native.current_render_targets[index] =
          resource_object_id_for_cpu_descriptor(descriptor, command.sequence, error);
    }
    native.current_render_target_count = count;
    return true;
  }

  void update_target_texture_watch_state(
      NativeCommandList &native,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload)
  {
    switch (command.kind) {
    case D3D12ReplayBackend::ReplayCommandKind::SetPipelineState:
      native.current_pipeline_state_object_id =
          command.object_refs.size() >= 2 ? command.object_refs[1] : 0;
      break;
    case D3D12ReplayBackend::ReplayCommandKind::SetRootSignature:
      if (command.function_name.find("Compute") == std::string::npos) {
        native.current_graphics_root_signature_object_id =
            command.object_refs.size() >= 2 ? command.object_refs[1] : 0;
      }
      break;
    case D3D12ReplayBackend::ReplayCommandKind::SetRootDescriptorTable:
      if (command.function_name.find("Compute") == std::string::npos) {
        const UINT root = payload_u32(payload, "root_parameter_index");
        if (root < native.current_graphics_root_tables.size()) {
          native.current_graphics_root_tables[root] =
              payload_u64(payload, "base_descriptor");
        }
      }
      break;
    case D3D12ReplayBackend::ReplayCommandKind::SetRootConstantBufferView:
      if (command.function_name.find("Compute") == std::string::npos) {
        const UINT root = payload_u32(payload, "root_parameter_index");
        if (root < native.current_graphics_root_descriptors.size()) {
          auto &descriptor = native.current_graphics_root_descriptors[root];
          descriptor.resource_object_id =
              static_cast<trace::ObjectId>(payload_u64(payload, "resolved_resource_object_id"));
          descriptor.offset = payload_u64(payload, "resolved_resource_offset");
          descriptor.kind = command.function_name.find("ShaderResourceView") != std::string::npos
                                ? "RootSRV"
                                : command.function_name.find("UnorderedAccessView") != std::string::npos
                                      ? "RootUAV"
                                      : "RootCBV";
        }
      }
      break;
    case D3D12ReplayBackend::ReplayCommandKind::SetViewports: {
      const auto viewports_json = payload.find("viewports");
      const json *first = nullptr;
      if (viewports_json != payload.end() && viewports_json->is_array() &&
          !viewports_json->empty() && (*viewports_json)[0].is_object()) {
        first = &(*viewports_json)[0];
      } else {
        const auto first_it = payload.find("first");
        if (first_it != payload.end() && first_it->is_object()) {
          first = &(*first_it);
        }
      }
      if (first) {
        native.current_viewport.TopLeftX = payload_float(*first, "x");
        native.current_viewport.TopLeftY = payload_float(*first, "y");
        native.current_viewport.Width = payload_float(*first, "width");
        native.current_viewport.Height = payload_float(*first, "height");
        native.current_viewport.MinDepth = payload_float(*first, "min_depth");
        native.current_viewport.MaxDepth = payload_float(*first, "max_depth");
        native.has_current_viewport = true;
      } else if (payload_u32(payload, "viewport_count") == 0) {
        native.has_current_viewport = false;
      }
      break;
    }
    case D3D12ReplayBackend::ReplayCommandKind::SetScissorRects: {
      const auto rects_json = payload.find("rects");
      const json *first = nullptr;
      if (rects_json != payload.end() && rects_json->is_array() &&
          !rects_json->empty() && (*rects_json)[0].is_object()) {
        first = &(*rects_json)[0];
      } else {
        const auto first_it = payload.find("first");
        if (first_it != payload.end() && first_it->is_object()) {
          first = &(*first_it);
        }
      }
      if (first) {
        native.current_scissor.left = payload_i32(*first, "left");
        native.current_scissor.top = payload_i32(*first, "top");
        native.current_scissor.right = payload_i32(*first, "right");
        native.current_scissor.bottom = payload_i32(*first, "bottom");
        native.has_current_scissor = true;
      } else if (payload_u32(payload, "rect_count") == 0) {
        native.has_current_scissor = false;
      }
      break;
    }
    case D3D12ReplayBackend::ReplayCommandKind::SetPrimitiveTopology:
      native.current_primitive_topology = payload_u32(payload, "primitive_topology");
      break;
    case D3D12ReplayBackend::ReplayCommandKind::SetVertexBuffers: {
      const auto start_slot = payload_u32(payload, "start_slot");
      if (start_slot != 0) {
        break;
      }
      const auto view_count = payload_u64(payload, "view_count");
      const json *first = nullptr;
      const auto views_json = payload.find("views");
      if (views_json != payload.end() && views_json->is_array() &&
          !views_json->empty() && (*views_json)[0].is_object()) {
        first = &(*views_json)[0];
      } else {
        const auto first_it = payload.find("first");
        if (first_it != payload.end() && first_it->is_object()) {
          first = &(*first_it);
        }
      }
      if (view_count == 0 || !first || payload_u64(*first, "buffer_location") == 0) {
        native.current_vertex_buffer0 = {};
        break;
      }
      native.current_vertex_buffer0.resource_object_id =
          static_cast<trace::ObjectId>(payload_u64(*first, "resolved_resource_object_id"));
      native.current_vertex_buffer0.offset = payload_u64(*first, "resolved_resource_offset");
      native.current_vertex_buffer0.size_in_bytes = payload_u32(*first, "size_in_bytes");
      native.current_vertex_buffer0.stride_in_bytes = payload_u32(*first, "stride_in_bytes");
      native.current_vertex_buffer0.format = 0;
      native.current_vertex_buffer0.valid = true;
      break;
    }
    case D3D12ReplayBackend::ReplayCommandKind::SetIndexBuffer:
      if (payload.empty() || payload_u64(payload, "buffer_location") == 0) {
        native.current_index_buffer = {};
        break;
      }
      native.current_index_buffer.resource_object_id =
          static_cast<trace::ObjectId>(payload_u64(payload, "resolved_resource_object_id"));
      native.current_index_buffer.offset = payload_u64(payload, "resolved_resource_offset");
      native.current_index_buffer.size_in_bytes = payload_u32(payload, "size_in_bytes");
      native.current_index_buffer.stride_in_bytes = 0;
      native.current_index_buffer.format = payload_u32(payload, "format");
      native.current_index_buffer.valid = true;
      break;
    default:
      break;
    }
  }

  bool flush_target_texture_watch(
      const std::vector<NativeCommandList *> &recorded_lists,
      std::string &error)
  {
    const trace::ObjectId target_object_id = target_texture_watch_object_id();
    if (target_object_id == 0) {
      return true;
    }
    const auto flush_buffer_readbacks = [&](NativeCommandList &native) -> bool {
      for (auto &readback : native.target_watch_buffer_readbacks) {
        if (!populate_target_texture_watch_buffer_readback(readback, error)) {
          return false;
        }
        void *mapped = nullptr;
        if (FAILED(readback.readback->Map(0, nullptr, &mapped)) || !mapped) {
          return fail(error, "target texture watch buffer map failed");
        }
        const auto *bytes = static_cast<const std::uint8_t *>(mapped);
        std::size_t nonzero = 0;
        for (std::uint64_t i = 0; i < readback.size; ++i) {
          if (bytes[i] != 0) {
            ++nonzero;
          }
        }

        std::ostringstream out;
        out << "target-buffer"
            << " object=" << target_object_id
            << " seq=" << readback.sequence
            << " list=" << readback.command_list_object_id
            << " label=" << readback.label
            << " res=" << readback.resource_object_id
            << " off=" << readback.offset
            << " size=" << readback.size
            << " nonzero=" << nonzero
            << " hex=" << target_texture_watch_hex_bytes(
                              bytes, static_cast<std::size_t>(
                                         std::min<std::uint64_t>(readback.size, 64)));
        if (readback.label == "vb0" && readback.stride_in_bytes >= 40) {
          const std::uint64_t vertex_count =
              std::min<std::uint64_t>(readback.size / readback.stride_in_bytes, 8);
          out << " vertices=";
          for (std::uint64_t vertex = 0; vertex < vertex_count; ++vertex) {
            const auto *v = bytes + vertex * readback.stride_in_bytes;
            if (vertex != 0) {
              out << ";";
            }
            out << target_texture_watch_float32(v + 0) << ","
                << target_texture_watch_float32(v + 4) << ","
                << target_texture_watch_float32(v + 8) << ","
                << target_texture_watch_float32(v + 12)
                << "|uv=" << target_texture_watch_float32(v + 32) << ","
                << target_texture_watch_float32(v + 36);
          }
        } else if (readback.label == "ib" && readback.format == DXGI_FORMAT_R16_UINT) {
          const std::uint64_t index_count =
              std::min<std::uint64_t>(readback.size / sizeof(std::uint16_t), 32);
          out << " indices=";
          for (std::uint64_t index = 0; index < index_count; ++index) {
            if (index != 0) {
              out << ",";
            }
            out << target_texture_watch_u16(bytes + index * sizeof(std::uint16_t));
          }
        } else if (readback.label.rfind("rootCBV", 0) == 0) {
          const std::uint64_t float_count =
              std::min<std::uint64_t>(readback.size / sizeof(float), 16);
          out << " floats=";
          for (std::uint64_t index = 0; index < float_count; ++index) {
            if (index != 0) {
              out << ",";
            }
            out << target_texture_watch_float32(bytes + index * sizeof(float));
          }
        }
        D3D12_RANGE none{0, 0};
        readback.readback->Unmap(0, &none);
        replay_milestone(out.str().c_str());
      }
      native.target_watch_buffer_readbacks.clear();
      return true;
    };

    for (auto *native : recorded_lists) {
      if (!native || native->target_watch_readbacks.empty()) {
        if (native && !native->target_watch_occlusions.empty()) {
          for (auto &occlusion : native->target_watch_occlusions) {
            void *mapped = nullptr;
            if (FAILED(occlusion.readback->Map(0, nullptr, &mapped)) || !mapped) {
              return fail(error, "target texture watch occlusion map failed");
            }
            const auto samples = *static_cast<const std::uint64_t *>(mapped);
            D3D12_RANGE none{0, 0};
            occlusion.readback->Unmap(0, &none);
            replay_milestone(
                (std::string("target-draw-occlusion object=") +
                 std::to_string(target_object_id) +
                 " seq=" + std::to_string(occlusion.sequence) +
                 " list=" + std::to_string(occlusion.command_list_object_id) +
                 " fn=" + occlusion.function_name +
                 " reason=" + occlusion.reason +
                 " pso=" + std::to_string(occlusion.pipeline_state_object_id) +
                 " samples=" + std::to_string(samples))
                    .c_str());
          }
          native->target_watch_occlusions.clear();
        }
        if (native && !flush_buffer_readbacks(*native)) {
          return false;
        }
        continue;
      }
      bool was_blue = false;
      bool initialized = false;
      for (auto &watch : native->target_watch_readbacks) {
        void *mapped = nullptr;
        if (FAILED(watch.readback->Map(0, nullptr, &mapped)) || !mapped) {
          return fail(error, "target texture watch map failed");
        }
        const auto *base = static_cast<const std::uint8_t *>(mapped) +
                           static_cast<std::size_t>(watch.footprint.Offset);
        const auto *px = base;
        UINT r = 0;
        UINT g = 0;
        UINT b = 0;
        UINT a = 0;
        std::uint32_t u32 = 0;
        const bool is_blue =
            target_texture_watch_is_blue(watch.format, px, r, g, b, a, u32);
        D3D12_RANGE none{0, 0};
        watch.readback->Unmap(0, &none);

        const bool transition_to_blue = initialized && !was_blue && is_blue;
        const bool first_sample_blue = !initialized && is_blue;
        const bool near_stop_sequence =
            target_texture_watch_near_stop_sequence(watch.sequence);
        if (transition_to_blue || first_sample_blue || near_stop_sequence) {
          std::string refs;
          for (std::size_t i = 0; i < watch.object_refs.size(); ++i) {
            refs += i ? "," : "";
            refs += std::to_string(watch.object_refs[i]);
          }
          const char *kind =
              transition_to_blue ? "transition" : first_sample_blue ? "already-blue" : "sample";
          replay_milestone(
              (std::string("target-blue ") +
               kind +
               " object=" + std::to_string(target_object_id) +
               " seq=" + std::to_string(watch.sequence) +
               " list=" + std::to_string(watch.command_list_object_id) +
               " kind=" + std::to_string(static_cast<int>(watch.kind)) +
               " fn=" + watch.function_name +
               " phase=" + watch.phase +
               " reason=" + watch.reason +
               " xy=" + std::to_string(watch.x) + "," + std::to_string(watch.y) +
               " rgba=" + std::to_string(r) + "," + std::to_string(g) + "," +
               std::to_string(b) + "," + std::to_string(a) +
               " u32=" + std::to_string(u32) +
               " refs=" + refs)
                  .c_str());
        }
        initialized = true;
        was_blue = is_blue;
      }
      native->target_watch_readbacks.clear();
      for (auto &occlusion : native->target_watch_occlusions) {
        void *mapped = nullptr;
        if (FAILED(occlusion.readback->Map(0, nullptr, &mapped)) || !mapped) {
          return fail(error, "target texture watch occlusion map failed");
        }
        const auto samples = *static_cast<const std::uint64_t *>(mapped);
        D3D12_RANGE none{0, 0};
        occlusion.readback->Unmap(0, &none);
        replay_milestone(
            (std::string("target-draw-occlusion object=") +
             std::to_string(target_object_id) +
             " seq=" + std::to_string(occlusion.sequence) +
             " list=" + std::to_string(occlusion.command_list_object_id) +
             " fn=" + occlusion.function_name +
             " reason=" + occlusion.reason +
             " pso=" + std::to_string(occlusion.pipeline_state_object_id) +
             " samples=" + std::to_string(samples))
                .c_str());
      }
      native->target_watch_occlusions.clear();
      if (!flush_buffer_readbacks(*native)) {
        return false;
      }
    }
    return true;
  }

  // Resolve the initial pipeline state recorded on the BeginCommandList marker at `begin_index`
  // (CreateCommandList/Reset initial_pipeline_state_object_id). Returns nullptr if none / unresolved.
  ID3D12PipelineState *initial_pipeline_state_for(std::size_t begin_index)
  {
    if (begin_index >= backend_.replay_commands_.size()) {
      return nullptr;
    }
    const auto &begin_cmd = backend_.replay_commands_[begin_index];
    if (begin_cmd.kind != D3D12ReplayBackend::ReplayCommandKind::BeginCommandList) {
      return nullptr;
    }
    const json payload = json::parse(begin_cmd.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return nullptr;
    }
    const auto pso_id = payload_u64(payload, "initial_pipeline_state_object_id");
    if (pso_id == 0) {
      return nullptr;
    }
    return pipeline_for(static_cast<trace::ObjectId>(pso_id), begin_cmd.sequence);
  }

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
    // Reset with the recorded initial pipeline state, not nullptr. Some applications bind graphics
    // PSO through Reset/CreateCommandList and then draw without a separate SetPipelineState.
    // Passing nullptr leaves the list with no PSO and drops those draws.
    ID3D12PipelineState *initial_pso = initial_pipeline_state_for(begin_index);
    // Diagnostic (BUG-007): a native list whose TYPE is not DIRECT but whose recorded interval has
    // Draw commands is the mismatch (graphics PSO/draw on a COPY/COMPUTE list -> DXMT drops them).
    if (std::getenv("APITRACE_D3D12_REPLAY_MILESTONE_LOG")) {
      static int dbg = 0;
      std::size_t n_setpso = 0, n_draw = 0;
      const trace::ObjectId dbg_list_id =
          backend_.replay_commands_[begin_index].command_list_object_id;
      for (std::size_t i = begin_index + 1; i < end_index; ++i) {
        if (backend_.replay_commands_[i].command_list_object_id != dbg_list_id) {
          continue;
        }
        const auto k = backend_.replay_commands_[i].kind;
        if (k == D3D12ReplayBackend::ReplayCommandKind::SetPipelineState) ++n_setpso;
        else if (k == D3D12ReplayBackend::ReplayCommandKind::Draw) ++n_draw;
      }
      if (n_draw > 0 && native.type != D3D12_COMMAND_LIST_TYPE_DIRECT && dbg < 16) {
        ++dbg;
        replay_milestone((std::string("listtypebug list=") +
                          std::to_string(dbg_list_id) +
                          " native_type=" + std::to_string(static_cast<int>(native.type)) +
                          " draws=" + std::to_string(n_draw) + " setpso=" + std::to_string(n_setpso) +
                          " begin=" + std::to_string(begin_index)).c_str());
      }
    }
    hr = native.list->Reset(native.allocator.get(), initial_pso);
    if (FAILED(hr)) {
      error = hresult_error("ID3D12GraphicsCommandList::Reset", hr);
      return false;
    }
    native.current_render_targets = {};
    native.current_render_target_count = 0;
    native.current_pipeline_state_object_id = 0;
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
    const auto record_begin = std::chrono::steady_clock::now();
    bool grind_warned = false;
    // replay_commands_ is a GLOBAL sequence-ordered stream; when the app records multiple command
    // lists concurrently their commands INTERLEAVE, so a [Begin,End] index window can contain other
    // lists' commands. Record ONLY commands belonging to THIS list, or we would dump e.g. graphics
    // draws into a COPY/COMPUTE native list and DXMT would drop them (BUG-007).
    const trace::ObjectId this_list_id =
        backend_.replay_commands_[begin_index].command_list_object_id;
    const trace::ObjectId stop_command_list_id = replay_stop_command_list_id();
    const bool stop_this_list =
        replay_stop_after_sequence_ != 0 &&
        (stop_command_list_id == 0 || stop_command_list_id == this_list_id);
    const bool watch_this_list =
        target_texture_watch_enabled() &&
        (native.type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         native.type == D3D12_COMMAND_LIST_TYPE_COPY);
    for (std::size_t index = begin_index + 1; index < end_index; ++index) {
      const auto &cmd_ref = backend_.replay_commands_[index];
      if (cmd_ref.command_list_object_id != this_list_id) {
        continue;  // interleaved command from another concurrently-recorded list
      }
      if (stop_this_list && cmd_ref.sequence > replay_stop_after_sequence_) {
        replay_stop_sequence_reached_ = true;
        break;
      }
      // Mid-grind watchdog: if a single command list is taking pathologically long to record, dump
      // the cumulative per-kind breakdown WHILE still inside the loop (the per-batch batch:slow log
      // only fires at the end, which never happens for a stuck list). Checked sparsely so the check
      // itself is free.
      if (!grind_warned && (index - begin_index) % 2000 == 0) {
        const auto grind_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - record_begin)
                                  .count();
        if (grind_ms >= 12000) {
          grind_warned = true;
          dump_command_kind_breakdown(
              (std::string("batch:grinding batch=") + std::to_string(replay_batch_index_) +
               " at_index=" + std::to_string(index - begin_index) + "/" +
               std::to_string(end_index - begin_index) + " grind_ms=" + std::to_string(grind_ms) +
               " cur_kind=" + std::to_string(static_cast<int>(cmd_ref.kind)) +
               " cur_fn=" + cmd_ref.function_name + " cur_seq=" + std::to_string(cmd_ref.sequence))
                  .c_str());
        }
      }
      const json &payload = cached_payload(index, cmd_ref.payload);
      std::string target_watch_reason;
      const bool target_watch_command =
          watch_this_list &&
          target_command_may_write_watched_texture(
              native, cmd_ref, payload, target_watch_reason, error);
      const bool target_watch_focused_sample =
          watch_this_list &&
          !target_watch_command &&
          target_texture_watch_focused_stop_sequence(cmd_ref.sequence);
      const char *target_watch_before_phase =
          target_watch_command ? "before" : "scan-before";
      const char *target_watch_after_phase =
          target_watch_command ? "after" : "scan-after";
      const char *target_watch_reason_cstr =
          target_watch_command ? target_watch_reason.c_str() : "near-stop-scan";
      const bool target_watch_draw =
          target_watch_command &&
          native.type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
          target_texture_watch_is_draw_command(cmd_ref) &&
          target_texture_watch_near_stop_sequence(cmd_ref.sequence);
      if (!error.empty()) {
        return false;
      }
      if ((target_watch_command || target_watch_focused_sample) &&
          !append_target_texture_watch_readback(
              native, cmd_ref, target_watch_before_phase, target_watch_reason_cstr, error)) {
        return false;
      }
      if (target_watch_draw) {
        log_target_texture_watch_draw_state(native, cmd_ref, payload, target_watch_reason);
        if (!append_target_texture_watch_draw_buffer_readbacks(native, cmd_ref, error)) {
          return false;
        }
        if (!begin_target_texture_watch_occlusion(native, cmd_ref, target_watch_reason, error)) {
          return false;
        }
      }
      const auto cmd_begin = std::chrono::steady_clock::now();
      const bool ok = record_command(native.list.get(), cmd_ref, payload, error);
      if (target_watch_draw &&
          !end_target_texture_watch_occlusion(native, error)) {
        return false;
      }
      const auto kind_idx = static_cast<std::size_t>(cmd_ref.kind);
      if (kind_idx < kCommandKindCount) {
        command_kind_micros_[kind_idx] += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - cmd_begin)
                .count());
        ++command_kind_counts_[kind_idx];
      }
      if (!ok) {
        if (error.empty()) {
          error = "record_command failed with no detail: " + cmd_ref.function_name +
                  " (kind=" + std::to_string(static_cast<int>(cmd_ref.kind)) +
                  " seq=" + std::to_string(cmd_ref.sequence) + ")";
        }
        return false;
      }
      if (!update_target_texture_watch_render_targets(native, cmd_ref, payload, error)) {
        return false;
      }
      update_target_texture_watch_state(native, cmd_ref, payload);
      if ((target_watch_command || target_watch_focused_sample) &&
          !append_target_texture_watch_readback(
              native, cmd_ref, target_watch_after_phase, target_watch_reason_cstr, error)) {
        return false;
      }
      if (stop_this_list && cmd_ref.sequence >= replay_stop_after_sequence_) {
        replay_stop_sequence_reached_ = true;
        break;
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

  trace::ObjectId replay_stop_command_list_id()
  {
    if (replay_stop_command_list_id_initialized_) {
      return replay_stop_command_list_id_;
    }
    replay_stop_command_list_id_initialized_ = true;
    if (replay_stop_after_sequence_ == 0) {
      return 0;
    }
    for (const auto &command : backend_.replay_commands_) {
      if (command.sequence != replay_stop_after_sequence_) {
        continue;
      }
      replay_stop_command_list_id_ = command.command_list_object_id;
      break;
    }
    return replay_stop_command_list_id_;
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
    // If this object_id was reused across lifetimes, resolve to the version live at the current
    // replay sequence (largest create_sequence <= cursor); otherwise fall back to the flat map.
    const auto ver_it = resource_version_natives_.find(object_id);
    if (ver_it != resource_version_natives_.end() && !ver_it->second.empty()) {
      const auto &versions = ver_it->second;
      const NativeResource *chosen = nullptr;
      for (const auto &[create_seq, native] : versions) {
        if (create_seq <= current_replay_sequence_) {
          chosen = &native;
        } else {
          break;  // sorted by create_sequence
        }
      }
      if (chosen == nullptr) {
        chosen = &versions.front().second;  // before first create: use earliest
      }
      if (chosen->resource) {
        return chosen->resource.get();
      }
    }
    const auto resource_it = resources_.find(object_id);
    if (resource_it == resources_.end() || !resource_it->second.resource) {
      error = "D3D12 native command replay incomplete: command references an unknown resource";
      return nullptr;
    }
    return resource_it->second.resource.get();
  }

  ID3D12PipelineState *pipeline_for(trace::ObjectId object_id, std::uint64_t sequence) const
  {
    const auto version_it = pipeline_version_natives_.find(object_id);
    if (version_it != pipeline_version_natives_.end() && !version_it->second.empty()) {
      const auto &versions = version_it->second;
      const ComPtr<ID3D12PipelineState> *chosen = nullptr;
      for (const auto &[create_seq, native] : versions) {
        if (create_seq <= sequence) {
          chosen = &native;
        } else {
          break;
        }
      }
      if (chosen == nullptr) {
        chosen = &versions.front().second;
      }
      return chosen->get();
    }
    const auto pipeline_it = pipelines_.find(object_id);
    return pipeline_it == pipelines_.end() ? nullptr : pipeline_it->second.get();
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
    return relocated_gpu_virtual_address(binding, original_address, relocated, error);
  }

  bool relocated_recorded_gpu_virtual_address(
      const json &payload,
      std::string_view address_key,
      std::uint64_t sequence,
      D3D12_GPU_VIRTUAL_ADDRESS &relocated,
      std::string &error) const
  {
    relocated = 0;
    D3D12ReplayBackend::GpuVirtualAddressBinding binding;
    const auto status = payload_string(payload, "gpuva_resolve_status");
    if (!backend_.resolve_recorded_gpu_virtual_address_at(
            payload_u64(payload, address_key),
            payload_u64(payload, "resolved_resource_object_id"),
            status == "unmapped",
            sequence,
            binding,
            error)) {
      return false;
    }
    return relocated_gpu_virtual_address(binding, payload_u64(payload, address_key), relocated, error);
  }

  bool relocated_gpu_virtual_address(
      const D3D12ReplayBackend::GpuVirtualAddressBinding &binding,
      std::uint64_t original_address,
      D3D12_GPU_VIRTUAL_ADDRESS &relocated,
      std::string &error) const
  {
    if (binding.resource_object_id == 0) {
      if (original_address == 0) {
        return true;
      }
      // A non-zero recorded GPU virtual address that resolves to no live resource at this replay
      // sequence. Surface it with the address so it is diagnosable instead of failing opaquely.
      // Causes seen: the backing buffer's gpu_virtual_address was not captured, the resource is not
      // yet created at this sequence, or an object_id was reused (see resource lifetime versioning).
      std::ostringstream out;
      out << "recorded GPU virtual address 0x" << std::hex << original_address
          << " resolves to no live resource at replay sequence " << std::dec
          << current_replay_sequence_;
      return fail(error, out.str());
    }
    ID3D12Resource *resource = resource_for(binding.resource_object_id, error);
    if (!resource) {
      if (error.empty()) {
        error = "GPUVA source resource cannot be relocated";
      }
      return false;
    }
    relocated = resource->GetGPUVirtualAddress() + binding.offset;
    return true;
  }

  bool record_command(
      ID3D12GraphicsCommandList *list,
      const D3D12ReplayBackend::ReplayCommandRecord &command,
      const json &payload,
      std::string &error)
  {
    if (payload.is_discarded() || !payload.is_object()) {
      return fail(error, "command payload is not JSON");
    }
    // Resolve resource references to the version live when this command was recorded.
    const ReplaySequenceScope command_scope(this, command.sequence);
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
    case D3D12ReplayBackend::ReplayCommandKind::BeginCommandList:
    case D3D12ReplayBackend::ReplayCommandKind::EndCommandList:
      // Recording-lifetime markers (CreateCommandList / Reset / Close). The replayer drives list
      // open/close itself in record_command_list via its own Reset()/Close(); an interior marker
      // (e.g. a list created then Reset to re-record) is a no-op here, not a real GPU command.
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
      if (!relocated_recorded_gpu_virtual_address(write, "dest", command.sequence, relocated, error)) {
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
    ID3D12PipelineState *pipeline = pipeline_for(command.object_refs[1], command.sequence);
    if (!pipeline) {
      return fail(error, "SetPipelineState references an unknown PSO");
    }
    list->SetPipelineState(pipeline);
    ++dbg_setpso_calls_;
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
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    if (!relocated_recorded_gpu_virtual_address(payload, "buffer_location", command.sequence, address, error)) {
      return false;
    }
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
    static bool logged_vp = false;
    if (!logged_vp && std::getenv("APITRACE_D3D12_REPLAY_MILESTONE_LOG") && !viewports.empty()) {
      logged_vp = true;
      replay_milestone((std::string("viewport:first count=") + std::to_string(viewports.size()) +
                        " w=" + std::to_string(static_cast<long long>(viewports[0].Width)) +
                        " h=" + std::to_string(static_cast<long long>(viewports[0].Height)) +
                        " x=" + std::to_string(static_cast<long long>(viewports[0].TopLeftX)) +
                        " y=" + std::to_string(static_cast<long long>(viewports[0].TopLeftY))).c_str());
    }
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
    static bool logged_sc = false;
    if (!logged_sc && std::getenv("APITRACE_D3D12_REPLAY_MILESTONE_LOG") && !rects.empty()) {
      logged_sc = true;
      replay_milestone((std::string("scissor:first count=") + std::to_string(rects.size()) +
                        " l=" + std::to_string(rects[0].left) + " t=" + std::to_string(rects[0].top) +
                        " r=" + std::to_string(rects[0].right) + " b=" + std::to_string(rects[0].bottom)).c_str());
    }
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
      note_back_buffer_rtv_use(binding, "OMSetRenderTargets", command.sequence);
      D3D12_CPU_DESCRIPTOR_HANDLE handle{};
      if (!materialize_cpu_descriptor_at(descriptor, command.sequence, handle, nullptr, error)) {
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
      if (!materialize_cpu_descriptor_at(dsv_descriptor, command.sequence, dsv_handle, nullptr, error)) {
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
    note_back_buffer_rtv_use(binding, "ClearRTV", command.sequence);
    // A/B diagnostic (BUG-007): optionally skip clears that target a back-buffer RTV slot, to test
    // whether clears executing after the scene draws are wiping rendered content (ordering bug).
    if (std::getenv("APITRACE_D3D12_RETRACE_SKIP_BB_CLEAR") && !back_buffer_rtv_slots_.empty()) {
      const std::uint64_t key =
          (static_cast<std::uint64_t>(binding.heap_object_id) << 20) ^ binding.descriptor_index;
      if (back_buffer_rtv_slots_.count(key)) {
        return true;  // skip this back-buffer clear
      }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    if (!materialize_cpu_descriptor_at(payload_u64(payload, "descriptor"), command.sequence, handle, nullptr, error)) {
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
    if (!materialize_cpu_descriptor_at(payload_u64(payload, "descriptor"), command.sequence, handle, nullptr, error)) {
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
    if (!materialize_cpu_descriptor_at(descriptor, sequence, handle, nullptr, error)) {
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
    if (!materialize_cpu_descriptor_at(descriptor, sequence, handle, nullptr, error)) {
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
    if (!materialize_cpu_descriptor_at(payload_u64(payload, "cpu_descriptor"), command.sequence, cpu_handle, nullptr, error)) {
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
        pipeline = pipeline_for(command.object_refs[1], command.sequence);
        if (!pipeline) {
          return fail(error, "ClearState references an unknown PSO");
        }
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
        if (descriptor != 0 &&
            !materialize_cpu_descriptor_at(descriptor, command.sequence, handle, nullptr, error)) {
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
      if (!materialize_cpu_descriptor_at(
              payload_u64(*depth_stencil, "cpu_descriptor"),
              command.sequence,
              depth_stencil_handle,
              nullptr,
              error)) {
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
      D3D12_GPU_VIRTUAL_ADDRESS relocated = 0;
      if (!relocated_recorded_gpu_virtual_address(view_json, "buffer_location", command.sequence, relocated, error)) {
        return false;
      }
      view.BufferLocation = relocated;
      views.push_back(view);
      return true;
    };

    const auto view_count = payload_u64(payload, "view_count");
    const auto views_json = payload.find("views");
    if (views_json != payload.end()) {
      if (!views_json->is_array()) {
        return fail(error, "IASetVertexBuffers view payload must be an array");
      }
      if (!views_json->empty() && views_json->size() != view_count) {
        return fail(error, "IASetVertexBuffers views count does not match view_count");
      }
      views.reserve(static_cast<std::size_t>(view_count));
      for (const auto &view_json : *views_json) {
        if (!append_view(view_json)) {
          return false;
        }
      }
      while (views.size() < view_count) {
        views.push_back(D3D12_VERTEX_BUFFER_VIEW{});
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
    D3D12_GPU_VIRTUAL_ADDRESS relocated = 0;
    if (!relocated_recorded_gpu_virtual_address(payload, "buffer_location", command.sequence, relocated, error)) {
      return false;
    }
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = relocated;
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
    ++dbg_copy_calls_;  // BUG-007 layer2 diagnostic: count copy commands actually recorded/executed
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

  // Return a native command queue of the given list type, creating+caching it on first use.
  // The DIRECT queue is the primary queue_ (also used for swapchain/present); COMPUTE/COPY queues
  // are created on demand so COMPUTE/COPY command lists execute on a type-matching queue (D3D12
  // requires the command-list type to match its execution queue). Returns nullptr on failure.
  ID3D12CommandQueue *get_or_create_queue(D3D12_COMMAND_LIST_TYPE type)
  {
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
      return queue_.get();
    }
    auto it = queues_by_type_.find(static_cast<UINT>(type));
    if (it != queues_by_type_.end()) {
      return it->second.get();
    }
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(q.put()))) || !q) {
      return nullptr;
    }
    auto *raw = q.get();
    queues_by_type_.emplace(static_cast<UINT>(type), std::move(q));
    return raw;
  }

  void wait_for_gpu(ID3D12CommandQueue *signal_queue = nullptr)
  {
    ID3D12CommandQueue *q = signal_queue ? signal_queue : queue_.get();
    if (!q || !fence_) {
      return;
    }
    const UINT64 value = ++fence_value_;
    if (FAILED(q->Signal(fence_.get(), value))) {
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

  // Returns a human-readable device-removed reason if the device has faulted, else an empty string.
  // A removed device makes wait_for_gpu() return immediately (GetCompletedValue() == UINT64_MAX),
  // so an illegal submission (e.g. binding a BC7 resource as an RTV) otherwise fails silently and we
  // only learn about it as a generic finalize failure. This surfaces the actual GPU fault reason.
  std::string device_removed_detail() const
  {
    if (!device_) {
      return {};
    }
    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (reason == S_OK) {
      return {};
    }
    std::ostringstream out;
    out << "device removed, GetDeviceRemovedReason=0x" << std::hex
        << static_cast<unsigned long>(reason);
    return out.str();
  }

  static constexpr UINT kBufferCount = 2;

  struct ResourceDataUpdateRef {
    std::uint64_t sequence = 0;
    trace::ObjectId resource_object_id = 0;
    const D3D12ReplayBackend::ResourceDataUpdate *update = nullptr;
  };

  struct DescriptorSlotHistoryEntry {
    std::uint64_t sequence = 0;
    const D3D12ReplayBackend::DescriptorSemanticState *descriptor = nullptr;
  };

  struct CpuDescriptorScratch {
    std::vector<ComPtr<ID3D12DescriptorHeap>> heaps;
    D3D12_CPU_DESCRIPTOR_HANDLE current_start{};
    UINT descriptor_size = 0;
    UINT next = 0;
    UINT capacity = 4096;
  };

  const D3D12ReplayBackend &backend_;
  HWND hwnd_ = nullptr;
  UINT present_delay_ms_ = static_cast<UINT>(env_u32("APITRACE_D3D12_RETRACE_PRESENT_DELAY_MS"));
  std::uint64_t replay_stop_after_present_frame_ = env_u64("APITRACE_D3D12_RETRACE_STOP_AFTER_PRESENT_FRAME");
  std::uint64_t replay_stop_after_sequence_ = env_u64("APITRACE_D3D12_RETRACE_STOP_AFTER_SEQUENCE");
  bool replay_stop_sequence_reached_ = false;
  bool replay_stop_command_list_id_initialized_ = false;
  trace::ObjectId replay_stop_command_list_id_ = 0;
  UINT64 fence_value_ = 0;
  std::uint64_t replay_batch_index_ = 0;
  // Per-command-kind cumulative record time (microseconds) + invocation counts, to attribute a slow
  // batch to a specific command kind. kCommandKindCount covers the ReplayCommandKind enum with slack.
  static constexpr std::size_t kCommandKindCount = 64;
  std::array<std::uint64_t, kCommandKindCount> command_kind_micros_{};
  std::array<std::uint64_t, kCommandKindCount> command_kind_counts_{};
  std::unordered_map<trace::ObjectId, std::vector<CommandListInterval>> command_list_intervals_;
  std::vector<json> payload_dom_cache_;
  std::unordered_set<std::uint64_t> back_buffer_rtv_slots_;
  std::uint64_t dbg_setpso_calls_ = 0;
  std::uint64_t dbg_copy_calls_ = 0;
  std::uint32_t probe_last_nonzero_permille_ = 0;
  UINT headless_back_buffer_index_ = 0;
  std::unordered_map<UINT, ComPtr<ID3D12CommandQueue>> queues_by_type_;
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
  std::unordered_map<trace::ObjectId, std::vector<std::pair<std::uint64_t, ComPtr<ID3D12PipelineState>>>>
      pipeline_version_natives_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12CommandSignature>> command_signatures_;
  std::unordered_map<trace::ObjectId, NativeHeap> heaps_;
  std::unordered_map<trace::ObjectId, ComPtr<ID3D12QueryHeap>> query_heaps_;
  std::array<CpuDescriptorScratch, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> cpu_descriptor_scratch_;
  std::unordered_map<std::uint64_t, std::vector<DescriptorSlotHistoryEntry>> descriptor_slot_history_;
  mutable bool shader_candidate_index_built_ = false;
  mutable std::unordered_map<std::uint64_t, std::vector<std::filesystem::path>> shader_candidate_paths_by_size_;
  std::unordered_map<trace::ObjectId, NativeResource> resources_;
  // Per-version native resources for object_ids reused across lifetimes (BUG-20260614-004). Built
  // from backend_.resource_versions_ when more than one version exists for an id. Each entry is a
  // (create_sequence, native resource) sorted by create_sequence; resource_for resolves to the
  // version live at current_replay_sequence_. Empty when no id is reused (resources_ suffices).
  std::unordered_map<trace::ObjectId, std::vector<std::pair<std::uint64_t, NativeResource>>>
      resource_version_natives_;
  std::uint64_t current_replay_sequence_ = 0;
  std::unordered_map<trace::ObjectId, NativeCommandList> command_lists_;
  std::vector<const D3D12ReplayBackend::DescriptorSemanticState *> descriptor_timeline_;
  std::vector<const D3D12ReplayBackend::SamplerSemanticState *> sampler_timeline_;
  std::vector<const D3D12ReplayBackend::CopyDescriptorSemanticState *> descriptor_copy_timeline_;
  std::vector<ResourceDataUpdateRef> resource_update_timeline_;
  std::size_t next_descriptor_index_ = 0;
  std::size_t next_sampler_index_ = 0;
  std::size_t next_descriptor_copy_index_ = 0;
  std::size_t next_resource_update_index_ = 0;
  std::size_t next_tile_mapping_update_index_ = 0;
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
