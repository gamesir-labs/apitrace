int main(int argc, char **argv)
{
  if (argc != 3 && argc != 4) {
    std::cerr << "usage: test_d3d12_replay_semantics <bundle> <bundle-finalize> [case]\n";
    return 2;
  }

  const std::filesystem::path bundle = argv[1];
  g_bundle_finalize = std::filesystem::path(argv[2]);
  const std::string selected_case = argc == 4 ? argv[3] : std::string();

  auto run_copy_texture_batch_invalid_refs_case = [](const std::filesystem::path &case_bundle) {
    if (!write_copy_texture_batch_invalid_refs_bundle(case_bundle)) {
      std::cerr << "failed to write copy-texture batch invalid-refs bundle\n";
      return false;
    }
    apitrace::trace::TraceBundleReader copy_texture_batch_invalid_refs_reader;
    if (!open_finalized_bundle(copy_texture_batch_invalid_refs_reader, case_bundle)) {
      std::cerr << "reader failed to reopen copy-texture batch invalid-refs bundle: "
                << copy_texture_batch_invalid_refs_reader.last_error() << "\n";
      return false;
    }
    apitrace::d3d12::D3D12ReplayBackend copy_texture_batch_invalid_refs_backend;
    if (replay_all_events(copy_texture_batch_invalid_refs_reader, copy_texture_batch_invalid_refs_backend)) {
      std::cerr << "copy-texture batch invalid refs were accepted\n";
      return false;
    }
    return copy_texture_batch_invalid_refs_backend.last_error().find("object_refs[0] is not a command list") !=
           std::string::npos;
  };

  if (!selected_case.empty()) {
    if (selected_case == "copy-texture-batch-invalid-refs") {
      return run_copy_texture_batch_invalid_refs_case(bundle) ? 0 : 1;
    }
    std::cerr << "unknown test case: " << selected_case << "\n";
    return 2;
  }

  std::filesystem::remove_all(bundle);

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    std::cerr << "failed to open bundle\n";
    return 1;
  }

  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "test_d3d12_replay_semantics";
  writer.write_metadata(metadata);

  std::uint64_t next_event_sequence = 1;
  auto next_sequence = [&next_event_sequence]() { return next_event_sequence++; };

  append(writer, object_create(next_sequence(), 1, ObjectKind::Device, 0, "ID3D12Device"));
  append(writer, call(next_sequence(), "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));

  append(writer, object_create(next_sequence(), 2, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateDescriptorHeap",
      {1, 2},
      "{\"type\":1,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":4096,\"gpu_start\":0}"));

  append(writer, object_create(next_sequence(), 6, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateDescriptorHeap",
      {1, 6},
      "{\"type\":0,\"num_descriptors\":10,\"flags\":1,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":40960}"));

  append(writer, object_create(next_sequence(), 7, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateDescriptorHeap",
      {1, 7},
      "{\"type\":2,\"num_descriptors\":4,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":12288,\"gpu_start\":0}"));

  append(writer, object_create(next_sequence(), 8, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateDescriptorHeap",
      {1, 8},
      "{\"type\":3,\"num_descriptors\":4,\"flags\":0,\"node_mask\":0,"
      "\"descriptor_size\":32,\"cpu_start\":24576,\"gpu_start\":0}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateSampler",
      {1},
      "{\"descriptor\":4096,\"filter\":0,\"address_u\":1,\"address_v\":2,\"address_w\":3,"
      "\"mip_lod_bias\":0.5,\"max_anisotropy\":4,\"comparison_func\":1,"
      "\"border_color\":[0.1,0.2,0.3,0.4],\"min_lod\":0,\"max_lod\":16}"));

  append(writer, object_create(next_sequence(), 3, ObjectKind::Heap, 1, "ID3D12Heap"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateHeap",
      {1, 3},
      "{\"size_in_bytes\":65536,\"alignment\":65536,\"heap_type\":1,\"flags\":0}"));

  append(writer, object_create(next_sequence(), 4, ObjectKind::Resource, 3, "ID3D12Resource"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreatePlacedResource",
      {1, 3, 4},
      "{\"heap_object_id\":3,\"heap_offset\":0,\"initial_state\":0,\"gpu_virtual_address\":8192,"
      "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));

  append(writer, object_create(next_sequence(), 5, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommittedResource",
      {1, 5},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
      "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));

  append(writer, object_create(next_sequence(), 6, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommittedResource",
      {1, 6},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":20480,"
      "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));

  append(writer, object_create(next_sequence(), 9, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommittedResource",
      {1, 9},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":32768,"
      "\"resource_desc\":" + texture2d_desc_json(128, 64, 6, 28) + ",\"optimized_clear_value\":null}"));

  append(writer, object_create(next_sequence(), 20, ObjectKind::Resource, 1, "ID3D12Resource"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommittedResource",
      {1, 20},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":0,"
      "\"resource_desc\":" + texture3d_desc_json(64, 32, 8, 28) + ",\"optimized_clear_value\":null}"));

  append(writer, object_create(next_sequence(), 19, ObjectKind::Resource, 1, "IDXGISwapChainBackBuffer"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommittedResource",
      {1, 19},
      "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":4,\"gpu_virtual_address\":0,"
      "\"swapchain_back_buffer\":true,\"buffer_index\":0,"
      "\"resource_desc\":" + texture2d_desc_json(2, 2, 1, 28) + ",\"optimized_clear_value\":null}"));

  append(writer, object_create(next_sequence(), 18, ObjectKind::QueryHeap, 1, "ID3D12QueryHeap"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateQueryHeap",
      {1, 18},
      "{\"type\":0,\"count\":4,\"node_mask\":0}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateConstantBufferView",
      {1, 4},
      "{\"descriptor\":8192,\"buffer_location\":8192,\"size_in_bytes\":256,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":4,"
      "\"resolved_resource_offset\":0,\"resolved_resource_width\":4096}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateShaderResourceView",
      {1, 9},
      "{\"descriptor\":8224,\"format\":28,\"view_dimension\":5,\"shader_4_component_mapping\":5768,"
      "\"view\":{\"most_detailed_mip\":1,\"mip_levels\":2,\"first_array_slice\":2,"
      "\"array_size\":3,\"plane_slice\":0,\"resource_min_lod_clamp\":0.25}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateShaderResourceView",
      {1, 9},
      "{\"descriptor\":8256,\"format\":28,\"view_dimension\":10,\"shader_4_component_mapping\":5768,"
      "\"view\":{\"most_detailed_mip\":0,\"mip_levels\":4,\"first_2d_array_face\":0,"
      "\"num_cubes\":1,\"resource_min_lod_clamp\":0}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateUnorderedAccessView",
      {1, 9},
      "{\"descriptor\":8288,\"format\":28,\"view_dimension\":5,"
      "\"view\":{\"mip_slice\":1,\"first_array_slice\":1,\"array_size\":2,\"plane_slice\":0}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateUnorderedAccessView",
      {1, 20},
      "{\"descriptor\":8320,\"format\":28,\"view_dimension\":8,"
      "\"view\":{\"mip_slice\":0,\"first_w_slice\":1,\"w_size\":3}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateShaderResourceView",
      {1, 9},
      "{\"descriptor\":8448,\"format\":0,\"view_dimension\":0,"
      "\"shader_4_component_mapping\":0,\"view\":null}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateUnorderedAccessView",
      {1, 9},
      "{\"descriptor\":8480,\"format\":0,\"view_dimension\":0,\"view\":null}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CopyDescriptorsSimple",
      {1},
      "{\"descriptor_heap_type\":0,\"descriptor_size\":32,"
      "\"descriptors\":[{\"dst_descriptor\":8352,\"src_descriptor\":8192}]}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CopyDescriptors",
      {1},
      "{\"descriptor_heap_type\":0,\"descriptor_size\":32,\"dst_range_count\":1,\"src_range_count\":2,"
      "\"descriptors\":["
      "{\"dst_descriptor\":8384,\"src_descriptor\":8192},"
      "{\"dst_descriptor\":8416,\"src_descriptor\":8224}"
      "]}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateRenderTargetView",
      {1, 9},
      "{\"descriptor\":12288,\"format\":28,\"view_dimension\":5,"
      "\"view\":{\"mip_slice\":1,\"first_array_slice\":1,\"array_size\":2,\"plane_slice\":0}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateRenderTargetView",
      {1, 20},
      "{\"descriptor\":12320,\"format\":28,\"view_dimension\":8,"
      "\"view\":{\"mip_slice\":0,\"first_w_slice\":1,\"w_size\":3}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateDepthStencilView",
      {1, 9},
      "{\"descriptor\":24576,\"format\":45,\"view_dimension\":4,\"flags\":1,"
      "\"view\":{\"mip_slice\":1,\"first_array_slice\":1,\"array_size\":2}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateDepthStencilView",
      {1, 9},
      "{\"descriptor\":24608,\"format\":45,\"view_dimension\":6,\"flags\":0,"
      "\"view\":{\"first_array_slice\":0,\"array_size\":2}}"));

  append(writer, object_create(next_sequence(), 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
  apitrace::trace::AssetRecord root_signature_asset;
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::RootSignature,
          "rootsig",
          "rootsig",
          root_signature_asset)) {
    std::cerr << "failed to write root signature asset\n";
    return 1;
  }
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateRootSignature",
      {1, 10},
      "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
          root_signature_asset.relative_path.generic_string() + "\"}",
      {root_signature_asset.blob_id}));

  append(writer, object_create(next_sequence(), 11, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  apitrace::trace::AssetRecord compute_shader_asset;
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::ShaderDxil,
          "cs",
          "cs",
          compute_shader_asset)) {
    std::cerr << "failed to write compute shader asset\n";
    return 1;
  }
  apitrace::trace::AssetRecord compute_pipeline_asset;
  const std::string compute_pipeline_json =
      "{\"type\":\"compute\",\"root_signature_object_id\":10,\"node_mask\":0,\"flags\":0,"
      "\"cs\":{\"bytecode_size\":2,\"cs_path\":\"" +
      compute_shader_asset.relative_path.generic_string() + "\"}}";
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "compute-pipeline",
          compute_pipeline_json,
          compute_pipeline_asset)) {
    std::cerr << "failed to write compute pipeline asset\n";
    return 1;
  }
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateComputePipelineState",
      {1, 11},
      "{\"pipeline_path\":\"" + compute_pipeline_asset.relative_path.generic_string() + "\"}",
      {compute_pipeline_asset.blob_id, compute_shader_asset.blob_id}));

  append(writer, object_create(next_sequence(), 32, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  apitrace::trace::AssetRecord vertex_shader_asset;
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::ShaderDxil,
          "vs",
          "vs",
          vertex_shader_asset)) {
    std::cerr << "failed to write vertex shader asset\n";
    return 1;
  }
  apitrace::trace::AssetRecord graphics_pipeline_asset;
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::Pipeline,
          "graphics-pipeline",
          native_ready_graphics_pipeline_json(10, vertex_shader_asset),
          graphics_pipeline_asset)) {
    std::cerr << "failed to write graphics pipeline asset\n";
    return 1;
  }
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateGraphicsPipelineState",
      {1, 32},
      "{\"pipeline_path\":\"" + graphics_pipeline_asset.relative_path.generic_string() + "\"}",
      {graphics_pipeline_asset.blob_id, vertex_shader_asset.blob_id}));

  append(writer, object_create(next_sequence(), 31, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device2::CreatePipelineState",
      {1, 31},
      "{\"pipeline_path\":\"" + compute_pipeline_asset.relative_path.generic_string() +
          "\",\"source\":\"stream\",\"stream_size\":32,\"requires_dxmt_backend\":false}",
      {compute_pipeline_asset.blob_id, compute_shader_asset.blob_id}));

  const auto missing_pipeline_shader_ref_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-pipeline-shader-ref");
  const auto incomplete_pipeline_metadata_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-incomplete-pipeline-metadata");
  {
    apitrace::trace::TraceBundleWriter missing_ref_writer;
    if (!missing_ref_writer.open(missing_pipeline_shader_ref_bundle)) {
      std::cerr << "failed to open missing-pipeline-shader-ref bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata missing_ref_metadata;
    missing_ref_metadata.api = apitrace::trace::ApiKind::D3D12;
    missing_ref_metadata.producer = "test_d3d12_missing_pipeline_shader_ref";
    missing_ref_writer.write_metadata(missing_ref_metadata);
    append(missing_ref_writer, object_create(next_sequence(), 101, ObjectKind::Device, 0, "ID3D12Device"));
    append(missing_ref_writer, object_create(next_sequence(), 110, ObjectKind::RootSignature, 101, "ID3D12RootSignature"));
    apitrace::trace::AssetRecord missing_ref_root_signature_asset;
    if (!write_asset(
            missing_ref_writer,
            apitrace::trace::AssetKind::RootSignature,
            "rootsig",
            "rootsig",
            missing_ref_root_signature_asset)) {
      std::cerr << "failed to write missing-ref root signature asset\n";
      return 1;
    }
    append(missing_ref_writer, call(
        next_sequence(),
        "ID3D12Device::CreateRootSignature",
        {101, 110},
        "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
            missing_ref_root_signature_asset.relative_path.generic_string() + "\"}",
        {missing_ref_root_signature_asset.blob_id}));
    append(missing_ref_writer, object_create(next_sequence(), 111, ObjectKind::PipelineState, 101, "ID3D12PipelineState"));
    apitrace::trace::AssetRecord missing_ref_shader_asset;
    if (!write_asset(
            missing_ref_writer,
            apitrace::trace::AssetKind::ShaderDxil,
            "cs",
            "cs",
            missing_ref_shader_asset)) {
      std::cerr << "failed to write missing-ref shader asset\n";
      return 1;
    }
    apitrace::trace::AssetRecord missing_ref_pipeline_asset;
    const std::string missing_ref_pipeline_json =
        "{\"type\":\"compute\",\"root_signature_object_id\":110,\"node_mask\":0,\"flags\":0,"
        "\"cs\":{\"bytecode_size\":2,\"cs_path\":\"" +
        missing_ref_shader_asset.relative_path.generic_string() + "\"}}";
    if (!write_asset(
            missing_ref_writer,
            apitrace::trace::AssetKind::Pipeline,
            "compute-pipeline",
            missing_ref_pipeline_json,
            missing_ref_pipeline_asset)) {
      std::cerr << "failed to write missing-ref pipeline asset\n";
      return 1;
    }
    append(missing_ref_writer, call(
        next_sequence(),
        "ID3D12Device::CreateComputePipelineState",
        {101, 111},
        "{\"pipeline_path\":\"" + missing_ref_pipeline_asset.relative_path.generic_string() + "\"}",
        {missing_ref_pipeline_asset.blob_id}));
    missing_ref_writer.close();
  }
  {
    apitrace::trace::TraceBundleWriter incomplete_writer;
    if (!incomplete_writer.open(incomplete_pipeline_metadata_bundle)) {
      std::cerr << "failed to open incomplete-pipeline-metadata bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata incomplete_metadata;
    incomplete_metadata.api = apitrace::trace::ApiKind::D3D12;
    incomplete_metadata.producer = "test_d3d12_incomplete_pipeline_metadata";
    incomplete_writer.write_metadata(incomplete_metadata);
    append(incomplete_writer, object_create(next_sequence(), 201, ObjectKind::Device, 0, "ID3D12Device"));
    append(incomplete_writer, object_create(next_sequence(), 210, ObjectKind::RootSignature, 201, "ID3D12RootSignature"));
    apitrace::trace::AssetRecord incomplete_root_signature_asset;
    if (!write_asset(
            incomplete_writer,
            apitrace::trace::AssetKind::RootSignature,
            "rootsig",
            "rootsig",
            incomplete_root_signature_asset)) {
      std::cerr << "failed to write incomplete root signature asset\n";
      return 1;
    }
    append(incomplete_writer, call(
        next_sequence(),
        "ID3D12Device::CreateRootSignature",
        {201, 210},
        "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
            incomplete_root_signature_asset.relative_path.generic_string() + "\"}",
        {incomplete_root_signature_asset.blob_id}));
    append(incomplete_writer, object_create(next_sequence(), 211, ObjectKind::PipelineState, 201, "ID3D12PipelineState"));
    apitrace::trace::AssetRecord incomplete_shader_asset;
    if (!write_asset(
            incomplete_writer,
            apitrace::trace::AssetKind::ShaderDxil,
            "cs",
            "cs",
            incomplete_shader_asset)) {
      std::cerr << "failed to write incomplete shader asset\n";
      return 1;
    }
    apitrace::trace::AssetRecord incomplete_pipeline_asset;
    const std::string incomplete_pipeline_json =
        "{\"type\":\"compute\",\"root_signature_object_id\":210,\"node_mask\":0,\"flags\":0,"
        "\"cs\":{\"cs_path\":\"" +
        incomplete_shader_asset.relative_path.generic_string() + "\"}}";
    if (!write_asset(
            incomplete_writer,
            apitrace::trace::AssetKind::Pipeline,
            "compute-pipeline",
            incomplete_pipeline_json,
            incomplete_pipeline_asset)) {
      std::cerr << "failed to write incomplete pipeline asset\n";
      return 1;
    }
    append(incomplete_writer, call(
        next_sequence(),
        "ID3D12Device::CreateComputePipelineState",
        {201, 211},
        "{\"pipeline_path\":\"" + incomplete_pipeline_asset.relative_path.generic_string() + "\"}",
        {incomplete_pipeline_asset.blob_id, incomplete_shader_asset.blob_id}));
    incomplete_writer.close();
  }

  append(writer, object_create(next_sequence(), 12, ObjectKind::CommandSignature, 1, "ID3D12CommandSignature"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommandSignature",
      {1, 10, 12},
      "{\"byte_stride\":12,\"argument_count\":1,\"node_mask\":0,"
      "\"arguments\":[{\"type\":1}]}"));

  append(writer, object_create(next_sequence(), 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommandQueue",
      {1, 13},
      "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));

  append(writer, object_create(next_sequence(), 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommandAllocator",
      {1, 14},
      "{\"type\":0}"));

  append(writer, object_create(next_sequence(), 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommandList",
      {1, 14, 11, 15},
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
      "\"initial_pipeline_state_object_id\":11}"));

  append(writer, object_create(next_sequence(), 16, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommandAllocator",
      {1, 16},
      "{\"type\":1}"));

  append(writer, object_create(next_sequence(), 17, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
  append(writer, call(
      next_sequence(),
      "ID3D12Device::CreateCommandList",
      {1, 16, 0, 17},
      "{\"node_mask\":0,\"type\":1,\"command_allocator_object_id\":16,"
      "\"initial_pipeline_state_object_id\":0}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::Close",
      {17},
      "{}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::Reset",
      {17, 16, 0},
      "{\"command_allocator_object_id\":16,\"initial_pipeline_state_object_id\":0}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::Close",
      {17},
      "{}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::Reset",
      {15, 14, 11},
      "{\"command_allocator_object_id\":14,\"initial_pipeline_state_object_id\":11}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::RSSetViewports",
      {15},
      "{\"viewport_count\":1,\"first\":{\"x\":0,\"y\":0,\"width\":128,"
      "\"height\":64,\"min_depth\":0,\"max_depth\":1}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::RSSetScissorRects",
      {15},
      "{\"rect_count\":1,\"first\":{\"left\":0,\"top\":0,\"right\":128,\"bottom\":64}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::OMSetRenderTargets",
      {15},
      "{\"render_target_count\":2,\"single_handle_to_descriptor_range\":true,"
      "\"render_targets\":[12288],\"dsv\":24576}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::IASetPrimitiveTopology",
      {15},
      "{\"primitive_topology\":4}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ClearRenderTargetView",
      {15},
      "{\"descriptor\":12288,\"color\":[0.25,0.5,0.75,1],\"rect_count\":1,"
      "\"rects\":[{\"left\":0,\"top\":0,\"right\":64,\"bottom\":32}]}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ClearDepthStencilView",
      {15},
      "{\"descriptor\":24576,\"clear_flags\":3,\"depth\":0.5,\"stencil\":7,"
      "\"rect_count\":1,\"rects\":[{\"left\":4,\"top\":8,\"right\":96,\"bottom\":48}]}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint",
      {15, 9},
      "{\"gpu_descriptor\":41056,\"cpu_descriptor\":8288,\"values\":[1,2,3,4],"
      "\"rect_count\":1,\"rects\":[{\"left\":0,\"top\":0,\"right\":16,\"bottom\":16}]}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat",
      {15, 9},
      "{\"gpu_descriptor\":41056,\"cpu_descriptor\":8288,\"values\":[0.25,0.5,0.75,1],"
      "\"rect_count\":1,\"rects\":[{\"left\":2,\"top\":4,\"right\":32,\"bottom\":24}]}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::DiscardResource",
      {15, 9},
      "{\"first_subresource\":0,\"subresource_count\":1,\"rect_count\":0,\"rects\":[]}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ResourceBarrier",
      {15, 9, 4, 9},
      "{\"barrier_count\":3,\"barriers\":["
      "{\"type\":2,\"flags\":0,\"resource_object_id\":9},"
      "{\"type\":1,\"flags\":0,\"resource_before_object_id\":4,\"resource_after_object_id\":9},"
      "{\"type\":0,\"flags\":0,\"resource_object_id\":9,\"before\":0,\"after\":64,\"subresource\":4294967295}"
      "]}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList4::BeginRenderPass",
      {15, 9, 4},
      "{\"render_targets_count\":1,"
      "\"render_targets\":[{\"cpu_descriptor\":12288,"
      "\"beginning_access\":{\"type\":2,\"clear\":{\"format\":28,\"color\":[1,0.5,0.25,1],\"depth\":0,\"stencil\":0}},"
      "\"ending_access\":{\"type\":2,\"src_resource_object_id\":9,\"dst_resource_object_id\":4,"
      "\"subresource_count\":1,\"subresources\":[{\"src_subresource\":0,\"dst_subresource\":1,"
      "\"dst_x\":0,\"dst_y\":0,\"src_rect\":{\"left\":0,\"top\":0,\"right\":64,\"bottom\":64}}],"
      "\"format\":28,\"resolve_mode\":3,\"preserve_resolve_source\":true}}],"
      "\"has_depth_stencil\":true,"
      "\"depth_stencil\":{\"cpu_descriptor\":24576,"
      "\"depth_beginning_access\":{\"type\":2,\"clear\":{\"format\":45,\"color\":[0,0,0,0],\"depth\":1,\"stencil\":0}},"
      "\"stencil_beginning_access\":{\"type\":3,\"clear\":{\"format\":0,\"color\":[0,0,0,0],\"depth\":0,\"stencil\":0}},"
      "\"depth_ending_access\":{\"type\":1,\"src_resource_object_id\":0,\"dst_resource_object_id\":0,"
      "\"subresource_count\":0,\"subresources\":[],\"format\":0,\"resolve_mode\":0,\"preserve_resolve_source\":false},"
      "\"stencil_ending_access\":{\"type\":3,\"src_resource_object_id\":0,\"dst_resource_object_id\":0,"
      "\"subresource_count\":0,\"subresources\":[],\"format\":0,\"resolve_mode\":0,\"preserve_resolve_source\":false}},"
      "\"flags\":1}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList4::EndRenderPass",
      {15},
      "{}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::CopyTextureRegion",
      {15, 9, 9},
      "{\"dst\":{\"resource_object_id\":9,\"type\":0,\"subresource_index\":1},"
      "\"dst_x\":8,\"dst_y\":4,\"dst_z\":0,"
      "\"src\":{\"resource_object_id\":9,\"type\":0,\"subresource_index\":0},"
      "\"src_box\":{\"left\":2,\"top\":3,\"front\":0,\"right\":32,\"bottom\":24,\"back\":1}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::CopyBufferRegion",
      {15, 5, 6},
      "{\"dst_buffer_object_id\":5,\"dst_offset\":128,"
      "\"src_buffer_object_id\":6,\"src_offset\":64,\"byte_count\":512}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ResolveSubresourceRegion",
      {15, 9, 9},
      "{\"dst_resource_object_id\":9,\"dst_subresource\":0,\"dst_x\":0,\"dst_y\":0,"
      "\"src_resource_object_id\":9,\"src_subresource\":0,"
      "\"src_rect\":{\"left\":0,\"top\":0,\"right\":128,\"bottom\":64},"
      "\"format\":28,\"mode\":3}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::BeginQuery",
      {15, 18},
      "{\"query_heap_object_id\":18,\"type\":0,\"index\":1}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::EndQuery",
      {15, 18},
      "{\"query_heap_object_id\":18,\"type\":0,\"index\":1}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ResolveQueryData",
      {15, 18, 5},
      "{\"query_heap_object_id\":18,\"type\":0,\"start_index\":1,"
      "\"query_count\":1,\"dst_buffer_object_id\":5,\"aligned_dst_buffer_offset\":256}"));

  apitrace::trace::AssetRecord query_result_asset;
  if (!write_asset(
          writer,
          apitrace::trace::AssetKind::Buffer,
          "query-result",
          std::string("\x2a\x00\x00\x00\x00\x00\x00\x00", 8),
          query_result_asset)) {
    std::cerr << "failed to write query resolve result asset\n";
    return 1;
  }
  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ResolveQueryDataResult",
      {15, 18, 5},
      "{\"query_heap_object_id\":18,\"type\":0,\"start_index\":1,"
      "\"query_count\":1,\"dst_buffer_object_id\":5,"
      "\"aligned_dst_buffer_offset\":256,\"resolved_size\":8,"
      "\"buffer_path\":\"" + query_result_asset.relative_path.generic_string() + "\"}",
      {query_result_asset.blob_id}));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetPredication",
      {15, 5},
      "{\"buffer_object_id\":5,\"aligned_buffer_offset\":256,\"operation\":1}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::IASetVertexBuffers",
      {15, 5},
      "{\"start_slot\":0,\"view_count\":1,\"first\":{\"buffer_location\":16448,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":5,"
      "\"resolved_resource_offset\":64,\"resolved_resource_width\":4096,"
      "\"size_in_bytes\":512,\"stride_in_bytes\":16}}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::IASetIndexBuffer",
      {15, 6},
      "{\"buffer_location\":20480,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":6,"
      "\"resolved_resource_offset\":0,\"resolved_resource_width\":4096,"
      "\"size_in_bytes\":256,\"format\":57}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView",
      {15, 4},
      "{\"root_parameter_index\":0,\"buffer_location\":8192,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":4,"
      "\"resolved_resource_offset\":0,\"resolved_resource_width\":4096}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView",
      {15, 5},
      "{\"root_parameter_index\":1,\"buffer_location\":16448,"
      "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":5,"
      "\"resolved_resource_offset\":64,\"resolved_resource_width\":4096}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView",
      {15},
      "{\"root_parameter_index\":2,\"buffer_location\":0,"
      "\"gpuva_resolve_status\":\"null\",\"resolved_resource_object_id\":0,"
      "\"resolved_resource_offset\":0,\"resolved_resource_width\":0}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetDescriptorHeaps",
      {15, 6},
      "{\"heap_count\":1}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable",
      {15},
      "{\"root_parameter_index\":3,\"base_descriptor\":40960}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable",
      {15},
      "{\"root_parameter_index\":4,\"base_descriptor\":40992}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetPipelineState",
      {15, 32},
      "{}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetGraphicsRootSignature",
      {15, 10},
      "{}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ExecuteIndirect",
      {15, 12, 4, 5},
      "{\"max_command_count\":3,\"arg_buffer_offset\":64,\"count_buffer_offset\":128}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetPipelineState",
      {15, 11},
      "{}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::SetComputeRootSignature",
      {15, 10},
      "{}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::Dispatch",
      {15},
      "{\"thread_group_count_x\":2,\"thread_group_count_y\":3,\"thread_group_count_z\":4}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::ExecuteBundle",
      {15, 17},
      "{\"bundle_command_list_object_id\":17}"));

  append(writer, call(
      next_sequence(),
      "ID3D12GraphicsCommandList::Close",
      {15},
      "{}"));

  apitrace::trace::AssetRecord present_frame_asset;
  present_frame_asset.blob_id = 2000;
  present_frame_asset.kind = apitrace::trace::AssetKind::Texture;
  present_frame_asset.debug_name = "poisoned-diagnostic-present-frame";
  present_frame_asset.payload_bytes.assign(4 * 4, 0x00);
  present_frame_asset = writer.register_asset(std::move(present_frame_asset));

  append(writer, boundary(
      next_sequence(),
      apitrace::trace::BoundaryKind::Frame,
      "{\"label\":\"FrameBegin\",\"frame_index\":0}"));

  append(writer, call(
      next_sequence(),
      "ID3D12CommandQueue::ExecuteCommandLists",
      {13, 15},
      "{\"command_list_count\":1}"));

  append(writer, call(
      next_sequence(),
      "IDXGISwapChain::Present",
      {},
      "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}"));

  append(writer, boundary(
      next_sequence(),
      apitrace::trace::BoundaryKind::Present,
      "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}"));

  append(writer, resource_blob(
      next_sequence(),
      "D3D12PresentFrame",
      {present_frame_asset.blob_id},
      "{\"frame_index\":0,\"width\":2,\"height\":2,\"row_pitch\":8,"
      "\"sync_interval\":1,\"flags\":0,\"format\":\"rgba8\",\"frame_path\":\"" +
          present_frame_asset.relative_path.generic_string() + "\"}"));

  append(writer, boundary(
      next_sequence(),
      apitrace::trace::BoundaryKind::Frame,
      "{\"label\":\"FrameEnd\",\"frame_index\":0}"));

  writer.close();

  apitrace::trace::TraceBundleReader reader;
  if (!open_finalized_bundle(reader, bundle)) {
    std::cerr << "reader failed to reopen bundle: " << g_open_bundle_error << "\n";
    return 1;
  }

  apitrace::d3d12::D3D12ReplayBackend backend;
  if (!backend.initialize(reader)) {
    std::cerr << "backend initialize failed: " << backend.last_error() << "\n";
    return 1;
  }

  for (const auto &event : reader.events()) {
    if (!backend.replay_event(event)) {
      std::cerr << "backend replay_event failed: " << backend.last_error() << "\n";
      return 1;
    }
  }

  const auto *list = backend.command_list_state(15);
  if (!list) {
    std::cerr << "missing command list semantic state\n";
    return 1;
  }
  if (list->viewports.size() != 1 || list->viewports[0].width != 128.0f || list->viewports[0].height != 64.0f) {
    std::cerr << "viewport semantic state was not preserved\n";
    return 1;
  }
  if (list->render_targets.size() != 2 ||
      list->render_targets[0].descriptor_index != 0 ||
      list->render_targets[1].descriptor_index != 1 ||
      list->depth_stencil.descriptor != 24576) {
    std::cerr << "render target semantic state was not preserved\n";
    return 1;
  }
  if (list->render_target_clears.size() != 1 ||
      list->render_target_clears[0].rects.size() != 1 ||
      list->render_target_clears[0].rects[0].right != 64 ||
      list->depth_stencil_clears.size() != 1 ||
      list->depth_stencil_clears[0].rects.size() != 1 ||
      list->depth_stencil_clears[0].rects[0].top != 8) {
    std::cerr << "clear rect semantic state was not preserved\n";
    return 1;
  }
  const auto &descriptor_copies = backend.descriptor_copies();
  if (descriptor_copies.size() != 2 ||
      descriptor_copies[0].descriptor_heap_type != 0 ||
      descriptor_copies[0].descriptors.size() != 1 ||
      descriptor_copies[0].descriptors[0].dst_descriptor != 8352 ||
      descriptor_copies[0].descriptors[0].src_descriptor != 8192 ||
      descriptor_copies[1].descriptor_heap_type != 0 ||
      descriptor_copies[1].descriptors.size() != 2 ||
      descriptor_copies[1].descriptors[0].dst_descriptor != 8384 ||
      descriptor_copies[1].descriptors[0].src_descriptor != 8192 ||
      descriptor_copies[1].descriptors[1].dst_descriptor != 8416 ||
      descriptor_copies[1].descriptors[1].src_descriptor != 8224) {
    std::cerr << "descriptor copy semantic state was not preserved\n";
    return 1;
  }
  const auto &descriptors = backend.descriptors();
  const auto descriptor_has_view_desc = [&](std::uint64_t descriptor, const char *kind, bool expected) {
    for (const auto &state : descriptors) {
      if (state.descriptor == descriptor && state.kind == kind) {
        return state.has_view_desc == expected;
      }
    }
    return false;
  };
  if (!descriptor_has_view_desc(8224, "ShaderResourceView", true) ||
      !descriptor_has_view_desc(8288, "UnorderedAccessView", true) ||
      !descriptor_has_view_desc(8448, "ShaderResourceView", false) ||
      !descriptor_has_view_desc(8480, "UnorderedAccessView", false)) {
    std::cerr << "descriptor null-view semantic state was not preserved\n";
    return 1;
  }
  if (list->unordered_access_clears.size() != 2 ||
      !list->unordered_access_clears[0].integer ||
      list->unordered_access_clears[0].uint_values[2] != 3 ||
      list->unordered_access_clears[0].resource_object_id != 9 ||
      list->unordered_access_clears[1].integer ||
      list->unordered_access_clears[1].float_values[2] != 0.75f ||
      list->unordered_access_clears[1].rects.size() != 1 ||
      list->unordered_access_clears[1].rects[0].top != 4) {
    std::cerr << "unordered access clear semantic state was not preserved\n";
    return 1;
  }
  if (list->discards.size() != 1 ||
      list->discards[0].resource_object_id != 9 ||
      list->discards[0].first_subresource != 0 ||
      list->discards[0].subresource_count != 1 ||
      list->discards[0].rect_count != 0) {
    std::cerr << "discard resource semantic state was not preserved\n";
    return 1;
  }
  if (list->texture_copies.size() != 1 ||
      !list->texture_copies[0].has_src_box ||
      list->texture_copies[0].src_box_left != 2 ||
      list->texture_copies[0].src_box_bottom != 24) {
    std::cerr << "texture copy source box semantic state was not preserved\n";
    return 1;
  }
  if (list->buffer_copies.size() != 1 ||
      list->buffer_copies[0].dst_buffer_object_id != 5 ||
      list->buffer_copies[0].dst_offset != 128 ||
      list->buffer_copies[0].src_buffer_object_id != 6 ||
      list->buffer_copies[0].src_offset != 64 ||
      list->buffer_copies[0].byte_count != 512) {
    std::cerr << "CopyBufferRegion semantic state was not preserved\n";
    return 1;
  }
  if (list->resolves.size() != 1 ||
      list->resolves[0].dst_x != 0 ||
      list->resolves[0].dst_y != 0 ||
      list->resolves[0].mode != 3 ||
      !list->resolves[0].has_src_rect ||
      list->resolves[0].src_rect_left != 0 ||
      list->resolves[0].src_rect_bottom != 64) {
    std::cerr << "ResolveSubresourceRegion semantic state was not preserved\n";
    return 1;
  }
  if (list->queries.size() != 3 ||
      list->queries[0].query_heap_object_id != 18 ||
      list->queries[0].index != 1 ||
      !list->queries[1].end ||
      !list->queries[2].resolve ||
      list->queries[2].dst_buffer_object_id != 5 ||
      list->queries[2].aligned_dst_buffer_offset != 256) {
    std::cerr << "query command semantic state was not preserved\n";
    return 1;
  }
  if (list->predications.size() != 1 ||
      list->predications[0].buffer_object_id != 5 ||
      list->predications[0].operation != 1) {
    std::cerr << "predication semantic state was not preserved\n";
    return 1;
  }
  const auto *resource5 = apitrace::d3d12::D3D12ReplayBackendTestHook::resource_state(backend, 5);
  if (!resource5 ||
      resource5->data_updates.size() != 1 ||
      resource5->data_updates[0].written_begin != 256 ||
      resource5->data_updates[0].written_end != 264 ||
      resource5->data_updates[0].blob_refs.size() != 1 ||
      resource5->data_updates[0].blob_refs[0] == 0 ||
      resource5->data_updates[0].relative_path.empty() ||
      !std::filesystem::is_regular_file(bundle / resource5->data_updates[0].relative_path) ||
      resource5->data_updates[0].bytes.size() != 8 ||
      resource5->data_updates[0].bytes[0] != 0x2a) {
    std::cerr << "ResolveQueryDataResult resource data update was not preserved\n";
    return 1;
  }
  if (list->vertex_buffers.size() != 1 ||
      list->vertex_buffers[0].slot != 0 ||
      list->vertex_buffers[0].address.resource_object_id != 5 ||
      list->vertex_buffers[0].address.offset != 64 ||
      list->vertex_buffers[0].size_in_bytes != 512 ||
      list->vertex_buffers[0].stride_in_bytes != 16 ||
      list->index_buffer.address.resource_object_id != 6 ||
      list->index_buffer.address.offset != 0 ||
      list->index_buffer.size_in_bytes != 256 ||
      list->index_buffer.format != 57) {
    std::cerr << "input assembler GPUVA semantic state was not preserved\n";
    return 1;
  }
  const auto graphics_root_cbv = list->graphics_root_constant_buffers.find(0);
  const auto compute_root_srv = list->compute_root_shader_resources.find(1);
  const auto graphics_root_uav = list->graphics_root_unordered_accesses.find(2);
  const auto graphics_root_table = list->graphics_root_tables.find(3);
  const auto compute_root_table = list->compute_root_tables.find(4);
  if (graphics_root_cbv == list->graphics_root_constant_buffers.end() ||
      graphics_root_cbv->second.resource_object_id != 4 ||
      graphics_root_cbv->second.offset != 0 ||
      compute_root_srv == list->compute_root_shader_resources.end() ||
      compute_root_srv->second.resource_object_id != 5 ||
      compute_root_srv->second.offset != 64 ||
      graphics_root_uav == list->graphics_root_unordered_accesses.end() ||
      graphics_root_uav->second.resource_object_id != 0 ||
      graphics_root_uav->second.gpu_virtual_address != 0 ||
      graphics_root_table == list->graphics_root_tables.end() ||
      graphics_root_table->second.heap_object_id != 6 ||
      graphics_root_table->second.descriptor_index != 0 ||
      compute_root_table == list->compute_root_tables.end() ||
      compute_root_table->second.heap_object_id != 6 ||
      compute_root_table->second.descriptor_index != 1) {
    std::cerr << "root descriptor GPUVA semantic state was not preserved\n";
    return 1;
  }
  if (list->dispatches.size() != 1 ||
      list->dispatches[0].thread_group_count_x != 2 ||
      list->dispatches[0].thread_group_count_y != 3 ||
      list->dispatches[0].thread_group_count_z != 4 ||
      list->dispatches[0].pipeline_state_object_id != 11 ||
      list->dispatches[0].compute_root_signature_object_id != 10 ||
      list->dispatches[0].mesh) {
    std::cerr << "Dispatch semantic state was not preserved\n";
    return 1;
  }
  if (list->indirect_executes.size() != 1 ||
      list->indirect_executes[0].command_signature_object_id != 12 ||
      list->indirect_executes[0].arg_buffer_object_id != 4 ||
      list->indirect_executes[0].count_buffer_object_id != 5 ||
      list->indirect_executes[0].pipeline_state_object_id != 32 ||
      list->indirect_executes[0].graphics_root_signature_object_id != 10 ||
      list->indirect_executes[0].compute_root_signature_object_id != 0 ||
      list->indirect_executes[0].graphics_root_tables.size() != 1 ||
      list->indirect_executes[0].render_targets.size() != 2 ||
      list->indirect_executes[0].depth_stencil.descriptor != 24576 ||
      list->indirect_executes[0].viewports.size() != 1 ||
      list->indirect_executes[0].scissor_rects.size() != 1 ||
      list->indirect_executes[0].primitive_topology != 4) {
    std::cerr << "ExecuteIndirect semantic state was not preserved\n";
    return 1;
  }
  if (!list->temporal_upscales.empty()) {
    std::cerr << "native-ready replay fixture unexpectedly contains TemporalUpscale\n";
    return 1;
  }
  if (list->barrier_count != 3 ||
      list->barriers.size() != 3 ||
      list->barriers[0].type != 2 ||
      list->barriers[0].resource_object_id != 9 ||
      list->barriers[1].type != 1 ||
      list->barriers[1].resource_before_object_id != 4 ||
      list->barriers[1].resource_after_object_id != 9 ||
      list->barriers[2].type != 0 ||
      list->barriers[2].resource_object_id != 9 ||
      list->barriers[2].before != 0 ||
      list->barriers[2].after != 64 ||
      list->barriers[2].subresource != 4294967295u) {
    std::cerr << "resource barrier semantic state was not preserved\n";
    return 1;
  }
  if (list->bundle_executes.size() != 1 ||
      list->bundle_executes[0].bundle_command_list_object_id != 17) {
    std::cerr << "ExecuteBundle semantic state was not preserved\n";
    return 1;
  }
  std::size_t bundle_close_count = 0;
  std::uint64_t latest_bundle_close_sequence = 0;
  std::uint64_t execute_bundle_sequence = 0;
  for (const auto &command : backend.replay_commands()) {
    if (command.command_list_object_id == 17 &&
        command.kind == apitrace::d3d12::D3D12ReplayBackend::ReplayCommandKind::EndCommandList) {
      ++bundle_close_count;
      latest_bundle_close_sequence = command.sequence;
    }
    if (command.command_list_object_id == 15 &&
        command.kind == apitrace::d3d12::D3D12ReplayBackend::ReplayCommandKind::ExecuteBundle) {
      execute_bundle_sequence = command.sequence;
    }
  }
  if (bundle_close_count != 2 ||
      latest_bundle_close_sequence == 0 ||
      execute_bundle_sequence <= latest_bundle_close_sequence) {
    std::cerr << "ExecuteBundle should use the latest closed bundle interval before execution\n";
    return 1;
  }
  if (backend.present_frames_.size() != 1 ||
      backend.present_semantics_.size() != 1 ||
      backend.frame_semantics_.size() != 1 ||
      backend.present_frames_.begin()->second.relative_path.empty() ||
      !std::filesystem::is_regular_file(bundle / backend.present_frames_.begin()->second.relative_path)) {
    std::cerr << "diagnostic PresentFrame metadata was not indexed without becoming replay input\n";
    return 1;
  }
  if (backend.replay_commands().empty() ||
      present_frame_asset.debug_name != "poisoned-diagnostic-present-frame") {
    std::cerr << "poisoned diagnostic PresentFrame should remain metadata-only replay evidence\n";
    return 1;
  }
  if (backend.submissions_.completed_batches().empty() ||
      !backend.submissions_.completed_batches().back().presented ||
      backend.submissions_.completed_batches().back().present_frame_index != 0) {
    std::cerr << "submission present frame_index was not preserved for retrace capture\n";
    return 1;
  }
  if (!backend.validate_replay_closure()) {
    std::cerr << "backend replay closure validation failed: " << backend.last_error() << "\n";
    return 1;
  }
  if (!backend.validate_native_replay_readiness()) {
    std::cerr << "native-ready replay validation failed: " << backend.last_error() << "\n";
    return 1;
  }
  const auto draw_without_pipeline_assets_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-without-pipeline-assets");
  if (!write_draw_without_pipeline_assets_bundle(draw_without_pipeline_assets_bundle)) {
    std::cerr << "failed to write draw-without-pipeline-assets bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(missing_pipeline_shader_ref_bundle, "asset path is not covered by blob_refs")) {
    std::cerr << "D3D12 replay accepted a pipeline shader path missing from blob_refs\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(incomplete_pipeline_metadata_bundle, "pipeline shader stage is missing bytecode_size")) {
    std::cerr << "D3D12 replay accepted incomplete pipeline shader metadata\n";
    return 1;
  }

  const auto missing_pipeline_sample_desc_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-pipeline-sample-desc");
  if (!write_graphics_pipeline_missing_metadata_bundle(missing_pipeline_sample_desc_bundle, "sample_desc")) {
    std::cerr << "failed to write missing-pipeline-sample-desc bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          missing_pipeline_sample_desc_bundle,
          "graphics pipeline asset missing sample_desc")) {
    std::cerr << "D3D12 replay accepted a graphics pipeline missing sample_desc\n";
    return 1;
  }

  const auto missing_pipeline_rtv_formats_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-pipeline-rtv-formats");
  if (!write_graphics_pipeline_missing_metadata_bundle(missing_pipeline_rtv_formats_bundle, "rtv_formats")) {
    std::cerr << "failed to write missing-pipeline-rtv-formats bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          missing_pipeline_rtv_formats_bundle,
          "graphics pipeline asset missing rtv_formats")) {
    std::cerr << "D3D12 replay accepted a graphics pipeline missing rtv_formats\n";
    return 1;
  }

  const auto missing_pipeline_depth_enable_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-pipeline-depth-enable");
  if (!write_graphics_pipeline_missing_metadata_bundle(missing_pipeline_depth_enable_bundle, "depth_enable")) {
    std::cerr << "failed to write missing-pipeline-depth-enable bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          missing_pipeline_depth_enable_bundle,
          "pipeline depth_stencil_state is missing boolean field depth_enable")) {
    std::cerr << "D3D12 replay accepted a graphics pipeline missing depth_enable\n";
    return 1;
  }

  const auto root_signature_wrong_blob_ref_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-root-signature-wrong-blob-ref");
  if (!write_root_signature_wrong_blob_ref_bundle(root_signature_wrong_blob_ref_bundle)) {
    std::cerr << "failed to write root-signature wrong-blob-ref bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(root_signature_wrong_blob_ref_bundle, "asset path is not covered by blob_refs")) {
    std::cerr << "D3D12 replay accepted a root signature path missing from blob_refs\n";
    return 1;
  }

  const auto pipeline_unknown_root_signature_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-pipeline-unknown-root-signature");
  if (!write_pipeline_unknown_root_signature_bundle(pipeline_unknown_root_signature_bundle)) {
    std::cerr << "failed to write pipeline unknown-root-signature bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          pipeline_unknown_root_signature_bundle,
          "pipeline asset references an unknown root signature")) {
    std::cerr << "D3D12 replay accepted a pipeline with an unknown root signature object id\n";
    return 1;
  }

  const auto draw_without_bound_pipeline_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-without-bound-pipeline");
  if (!write_draw_without_bound_pipeline_bundle(draw_without_bound_pipeline_bundle)) {
    std::cerr << "failed to write draw-without-bound-pipeline bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader draw_without_bound_pipeline_reader;
  if (!open_finalized_bundle(draw_without_bound_pipeline_reader, draw_without_bound_pipeline_bundle)) {
    std::cerr << "reader failed to reopen draw-without-bound-pipeline bundle: "
              << draw_without_bound_pipeline_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend draw_without_bound_pipeline_backend;
  if (!replay_all_events(draw_without_bound_pipeline_reader, draw_without_bound_pipeline_backend)) {
    std::cerr << "draw-without-bound-pipeline replay_event failed: "
              << draw_without_bound_pipeline_backend.last_error() << "\n";
    return 1;
  }
  if (draw_without_bound_pipeline_backend.validate_native_replay_readiness() ||
      draw_without_bound_pipeline_backend.last_error().find("DrawInstanced sequence") == std::string::npos ||
      draw_without_bound_pipeline_backend.last_error().find("has no bound pipeline state") == std::string::npos) {
    std::cerr << "native readiness accepted a draw without a bound pipeline: "
              << draw_without_bound_pipeline_backend.last_error() << "\n";
    return 1;
  }

  const auto draw_without_bound_root_signature_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-without-bound-root-signature");
  if (!write_draw_without_bound_root_signature_bundle(draw_without_bound_root_signature_bundle)) {
    std::cerr << "failed to write draw-without-bound-root-signature bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader draw_without_bound_root_signature_reader;
  if (!open_finalized_bundle(draw_without_bound_root_signature_reader, draw_without_bound_root_signature_bundle)) {
    std::cerr << "reader failed to reopen draw-without-bound-root-signature bundle: "
              << draw_without_bound_root_signature_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend draw_without_bound_root_signature_backend;
  if (!replay_all_events(draw_without_bound_root_signature_reader, draw_without_bound_root_signature_backend)) {
    std::cerr << "draw-without-bound-root-signature replay_event failed: "
              << draw_without_bound_root_signature_backend.last_error() << "\n";
    return 1;
  }
  if (draw_without_bound_root_signature_backend.validate_native_replay_readiness() ||
      draw_without_bound_root_signature_backend.last_error().find("DrawInstanced sequence") == std::string::npos ||
      draw_without_bound_root_signature_backend.last_error().find("has no bound root signature") == std::string::npos) {
    std::cerr << "native readiness accepted a draw without a bound root signature: "
              << draw_without_bound_root_signature_backend.last_error() << "\n";
    return 1;
  }

  const auto draw_with_mismatched_root_parameter_type_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-with-mismatched-root-parameter-type");
  if (!write_draw_with_mismatched_root_parameter_type_bundle(draw_with_mismatched_root_parameter_type_bundle)) {
    std::cerr << "failed to write draw-with-mismatched-root-parameter-type bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader draw_with_mismatched_root_parameter_type_reader;
  if (!open_finalized_bundle(draw_with_mismatched_root_parameter_type_reader, draw_with_mismatched_root_parameter_type_bundle)) {
    std::cerr << "reader failed to reopen draw-with-mismatched-root-parameter-type bundle: "
              << draw_with_mismatched_root_parameter_type_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend draw_with_mismatched_root_parameter_type_backend;
  if (!replay_all_events(
          draw_with_mismatched_root_parameter_type_reader,
          draw_with_mismatched_root_parameter_type_backend)) {
    std::cerr << "draw-with-mismatched-root-parameter-type replay_event failed: "
              << draw_with_mismatched_root_parameter_type_backend.last_error() << "\n";
    return 1;
  }
  if (draw_with_mismatched_root_parameter_type_backend.validate_native_replay_readiness() ||
      draw_with_mismatched_root_parameter_type_backend.last_error().find("DrawInstanced sequence") == std::string::npos ||
      draw_with_mismatched_root_parameter_type_backend.last_error().find("root parameter type does not match") == std::string::npos) {
    std::cerr << "native readiness accepted a mismatched root parameter binding: "
              << draw_with_mismatched_root_parameter_type_backend.last_error() << "\n";
    return 1;
  }

  const auto dispatch_without_bound_pipeline_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-dispatch-without-bound-pipeline");
  if (!write_dispatch_without_bound_pipeline_bundle(dispatch_without_bound_pipeline_bundle)) {
    std::cerr << "failed to write dispatch-without-bound-pipeline bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader dispatch_without_bound_pipeline_reader;
  if (!open_finalized_bundle(dispatch_without_bound_pipeline_reader, dispatch_without_bound_pipeline_bundle)) {
    std::cerr << "reader failed to reopen dispatch-without-bound-pipeline bundle: "
              << dispatch_without_bound_pipeline_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend dispatch_without_bound_pipeline_backend;
  if (!replay_all_events(dispatch_without_bound_pipeline_reader, dispatch_without_bound_pipeline_backend)) {
    std::cerr << "dispatch-without-bound-pipeline replay_event failed: "
              << dispatch_without_bound_pipeline_backend.last_error() << "\n";
    return 1;
  }
  if (dispatch_without_bound_pipeline_backend.validate_native_replay_readiness() ||
      dispatch_without_bound_pipeline_backend.last_error().find("Dispatch sequence") == std::string::npos ||
      dispatch_without_bound_pipeline_backend.last_error().find("has no bound pipeline state") == std::string::npos) {
    std::cerr << "native readiness accepted a dispatch without a bound pipeline: "
              << dispatch_without_bound_pipeline_backend.last_error() << "\n";
    return 1;
  }

  const auto execute_indirect_without_bound_pipeline_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-execute-indirect-without-bound-pipeline");
  if (!write_execute_indirect_without_bound_pipeline_bundle(execute_indirect_without_bound_pipeline_bundle)) {
    std::cerr << "failed to write ExecuteIndirect-without-bound-pipeline bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader execute_indirect_without_bound_pipeline_reader;
  if (!open_finalized_bundle(execute_indirect_without_bound_pipeline_reader, execute_indirect_without_bound_pipeline_bundle)) {
    std::cerr << "reader failed to reopen ExecuteIndirect-without-bound-pipeline bundle: "
              << execute_indirect_without_bound_pipeline_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend execute_indirect_without_bound_pipeline_backend;
  if (!replay_all_events(execute_indirect_without_bound_pipeline_reader, execute_indirect_without_bound_pipeline_backend)) {
    std::cerr << "ExecuteIndirect-without-bound-pipeline replay_event failed: "
              << execute_indirect_without_bound_pipeline_backend.last_error() << "\n";
    return 1;
  }
  if (execute_indirect_without_bound_pipeline_backend.validate_native_replay_readiness() ||
      execute_indirect_without_bound_pipeline_backend.last_error().find("ExecuteIndirect sequence") == std::string::npos ||
      execute_indirect_without_bound_pipeline_backend.last_error().find("has no bound pipeline state") == std::string::npos) {
    std::cerr << "native readiness accepted ExecuteIndirect without a bound pipeline: "
              << execute_indirect_without_bound_pipeline_backend.last_error() << "\n";
    return 1;
  }

  const auto execute_indirect_argument_buffer_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-execute-indirect-argument-buffer-out-of-bounds");
  if (!write_execute_indirect_argument_buffer_out_of_bounds_bundle(execute_indirect_argument_buffer_out_of_bounds_bundle)) {
    std::cerr << "failed to write ExecuteIndirect argument-buffer-out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader execute_indirect_argument_buffer_out_of_bounds_reader;
  if (!open_finalized_bundle(execute_indirect_argument_buffer_out_of_bounds_reader, execute_indirect_argument_buffer_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen ExecuteIndirect argument-buffer-out-of-bounds bundle: "
              << execute_indirect_argument_buffer_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend execute_indirect_argument_buffer_out_of_bounds_backend;
  if (!replay_all_events(execute_indirect_argument_buffer_out_of_bounds_reader, execute_indirect_argument_buffer_out_of_bounds_backend)) {
    std::cerr << "ExecuteIndirect argument-buffer-out-of-bounds replay_event failed: "
              << execute_indirect_argument_buffer_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (execute_indirect_argument_buffer_out_of_bounds_backend.validate_native_replay_readiness() ||
      execute_indirect_argument_buffer_out_of_bounds_backend.last_error().find("ExecuteIndirect argument buffer sequence") == std::string::npos ||
      execute_indirect_argument_buffer_out_of_bounds_backend.last_error().find("buffer range is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted ExecuteIndirect with an out-of-bounds argument buffer: "
              << execute_indirect_argument_buffer_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const struct {
    const char *suffix;
    bool include_viewport;
    bool include_scissor;
    bool include_topology;
    const char *error_substring;
  } execute_indirect_missing_raster_state_cases[] = {
      {"execute-indirect-without-viewport", false, true, true, "has no bound viewport"},
      {"execute-indirect-without-scissor", true, false, true, "has no bound scissor rect"},
      {"execute-indirect-without-primitive-topology", true, true, false, "has no bound primitive topology"},
  };
  for (const auto &test_case : execute_indirect_missing_raster_state_cases) {
    const auto execute_indirect_missing_raster_state_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-" + test_case.suffix);
    if (!write_execute_indirect_missing_raster_state_bundle(
            execute_indirect_missing_raster_state_bundle,
            test_case.include_viewport,
            test_case.include_scissor,
            test_case.include_topology)) {
      std::cerr << "failed to write " << test_case.suffix << " bundle\n";
      return 1;
    }
    std::string readiness_error;
    if (!expect_d3d12_native_readiness_failure(
            execute_indirect_missing_raster_state_bundle,
            test_case.error_substring,
            readiness_error)) {
      std::cerr << "native readiness did not reject " << test_case.suffix
                << ": " << readiness_error << "\n";
      return 1;
    }
  }

  const auto clear_rtv_with_srv_descriptor_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-clear-rtv-with-srv-descriptor");
  if (!write_clear_rtv_with_srv_descriptor_bundle(clear_rtv_with_srv_descriptor_bundle)) {
    std::cerr << "failed to write clear-RTV-with-SRV-descriptor bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader clear_rtv_with_srv_descriptor_reader;
  if (!open_finalized_bundle(clear_rtv_with_srv_descriptor_reader, clear_rtv_with_srv_descriptor_bundle)) {
    std::cerr << "reader failed to reopen clear-RTV-with-SRV-descriptor bundle: "
              << clear_rtv_with_srv_descriptor_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend clear_rtv_with_srv_descriptor_backend;
  if (!replay_all_events(clear_rtv_with_srv_descriptor_reader, clear_rtv_with_srv_descriptor_backend)) {
    std::cerr << "clear-RTV-with-SRV-descriptor replay_event failed: "
              << clear_rtv_with_srv_descriptor_backend.last_error() << "\n";
    return 1;
  }
  if (clear_rtv_with_srv_descriptor_backend.validate_native_replay_readiness() ||
      clear_rtv_with_srv_descriptor_backend.last_error().find("ClearRenderTargetView sequence") == std::string::npos ||
      clear_rtv_with_srv_descriptor_backend.last_error().find("expects RenderTargetView") == std::string::npos) {
    std::cerr << "native readiness accepted ClearRenderTargetView with an SRV descriptor: "
              << clear_rtv_with_srv_descriptor_backend.last_error() << "\n";
    return 1;
  }

  const auto buffer_srv_range_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-buffer-srv-range-out-of-bounds");
  if (!write_buffer_srv_range_out_of_bounds_bundle(buffer_srv_range_out_of_bounds_bundle)) {
    std::cerr << "failed to write buffer-SRV-range-out-of-bounds bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          buffer_srv_range_out_of_bounds_bundle,
          "descriptor buffer view range is out of bounds")) {
    std::cerr << "D3D12 replay accepted an out-of-bounds buffer SRV descriptor\n";
    return 1;
  }

  const auto typed_buffer_srv_range_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-typed-buffer-srv-range-out-of-bounds");
  if (!write_typed_buffer_srv_range_out_of_bounds_bundle(typed_buffer_srv_range_out_of_bounds_bundle)) {
    std::cerr << "failed to write typed-buffer-SRV-range-out-of-bounds bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          typed_buffer_srv_range_out_of_bounds_bundle,
          "descriptor buffer view range is out of bounds")) {
    std::cerr << "D3D12 replay accepted an out-of-bounds typed buffer SRV descriptor\n";
    return 1;
  }

  const auto texture_srv_mip_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-texture-srv-mip-out-of-bounds");
  if (!write_texture_srv_mip_out_of_bounds_bundle(texture_srv_mip_out_of_bounds_bundle)) {
    std::cerr << "failed to write texture-SRV-mip-out-of-bounds bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          texture_srv_mip_out_of_bounds_bundle,
          "descriptor texture view mip range is out of bounds")) {
    std::cerr << "D3D12 replay accepted an out-of-bounds texture SRV descriptor\n";
    return 1;
  }

  const auto clear_rtv_rect_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-clear-rtv-rect-out-of-bounds");
  if (!write_clear_rtv_rect_out_of_bounds_bundle(clear_rtv_rect_out_of_bounds_bundle)) {
    std::cerr << "failed to write clear-RTV-rect-out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader clear_rtv_rect_out_of_bounds_reader;
  if (!open_finalized_bundle(clear_rtv_rect_out_of_bounds_reader, clear_rtv_rect_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen clear-RTV-rect-out-of-bounds bundle: "
              << clear_rtv_rect_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend clear_rtv_rect_out_of_bounds_backend;
  if (!replay_all_events(clear_rtv_rect_out_of_bounds_reader, clear_rtv_rect_out_of_bounds_backend)) {
    std::cerr << "clear-RTV-rect-out-of-bounds replay_event failed: "
              << clear_rtv_rect_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (clear_rtv_rect_out_of_bounds_backend.validate_native_replay_readiness() ||
      clear_rtv_rect_out_of_bounds_backend.last_error().find("ClearRenderTargetView sequence") == std::string::npos ||
      clear_rtv_rect_out_of_bounds_backend.last_error().find("rect is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted ClearRenderTargetView with an out-of-bounds rect: "
              << clear_rtv_rect_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto query_range_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-query-range-out-of-bounds");
  if (!write_query_range_out_of_bounds_bundle(query_range_out_of_bounds_bundle)) {
    std::cerr << "failed to write query range-out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader query_range_out_of_bounds_reader;
  if (!open_finalized_bundle(query_range_out_of_bounds_reader, query_range_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen query range-out-of-bounds bundle: "
              << query_range_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend query_range_out_of_bounds_backend;
  if (!replay_all_events(query_range_out_of_bounds_reader, query_range_out_of_bounds_backend)) {
    std::cerr << "query range-out-of-bounds replay_event failed: "
              << query_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (query_range_out_of_bounds_backend.validate_native_replay_readiness() ||
      query_range_out_of_bounds_backend.last_error().find("query command sequence") == std::string::npos ||
      query_range_out_of_bounds_backend.last_error().find("query range is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds query range: "
              << query_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto clear_state_unknown_pipeline_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-clear-state-unknown-pipeline");
  if (!write_clear_state_unknown_pipeline_bundle(clear_state_unknown_pipeline_bundle)) {
    std::cerr << "failed to write ClearState unknown-pipeline bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader clear_state_unknown_pipeline_reader;
  if (!open_finalized_bundle(clear_state_unknown_pipeline_reader, clear_state_unknown_pipeline_bundle)) {
    std::cerr << "reader failed to reopen ClearState unknown-pipeline bundle: "
              << clear_state_unknown_pipeline_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend clear_state_unknown_pipeline_backend;
  if (!replay_all_events(clear_state_unknown_pipeline_reader, clear_state_unknown_pipeline_backend)) {
    std::cerr << "ClearState unknown-pipeline replay_event failed: "
              << clear_state_unknown_pipeline_backend.last_error() << "\n";
    return 1;
  }
  if (clear_state_unknown_pipeline_backend.validate_native_replay_readiness() ||
      clear_state_unknown_pipeline_backend.last_error().find("ClearState sequence") == std::string::npos ||
      clear_state_unknown_pipeline_backend.last_error().find("references an unknown pipeline state") == std::string::npos) {
    std::cerr << "native readiness accepted ClearState with an unknown pipeline state: "
              << clear_state_unknown_pipeline_backend.last_error() << "\n";
    return 1;
  }

  const auto predication_range_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-predication-range-out-of-bounds");
  if (!write_predication_range_out_of_bounds_bundle(predication_range_out_of_bounds_bundle)) {
    std::cerr << "failed to write predication range-out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader predication_range_out_of_bounds_reader;
  if (!open_finalized_bundle(predication_range_out_of_bounds_reader, predication_range_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen predication range-out-of-bounds bundle: "
              << predication_range_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend predication_range_out_of_bounds_backend;
  if (!replay_all_events(predication_range_out_of_bounds_reader, predication_range_out_of_bounds_backend)) {
    std::cerr << "predication range-out-of-bounds replay_event failed: "
              << predication_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (predication_range_out_of_bounds_backend.validate_native_replay_readiness() ||
      predication_range_out_of_bounds_backend.last_error().find("SetPredication sequence") == std::string::npos ||
      predication_range_out_of_bounds_backend.last_error().find("buffer range is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds predication buffer range: "
              << predication_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto fence_signal_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-fence-signal");
  if (!write_fence_signal_bundle(fence_signal_bundle)) {
    std::cerr << "failed to write fence signal bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader fence_signal_reader;
  if (!open_finalized_bundle(fence_signal_reader, fence_signal_bundle)) {
    std::cerr << "reader failed to reopen fence signal bundle: "
              << fence_signal_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend fence_signal_backend;
  if (!replay_all_events(fence_signal_reader, fence_signal_backend)) {
    std::cerr << "fence signal replay_event failed: "
              << fence_signal_backend.last_error() << "\n";
    return 1;
  }
  if (fence_signal_backend.validate_native_replay_readiness() ||
      fence_signal_backend.last_error().find("fence operation sequence") == std::string::npos ||
      fence_signal_backend.last_error().find("requires captured fence timeline replay") == std::string::npos) {
    std::cerr << "native readiness accepted a captured fence timeline operation: "
              << fence_signal_backend.last_error() << "\n";
    return 1;
  }

  const auto unmap_wrong_blob_ref_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-unmap-wrong-blob-ref");
  if (!write_unmap_wrong_blob_ref_bundle(unmap_wrong_blob_ref_bundle)) {
    std::cerr << "failed to write Unmap wrong-blob-ref bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader unmap_wrong_blob_ref_reader;
  if (!open_finalized_bundle(unmap_wrong_blob_ref_reader, unmap_wrong_blob_ref_bundle)) {
    std::cerr << "reader failed to reopen Unmap wrong-blob-ref bundle: "
              << unmap_wrong_blob_ref_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend unmap_wrong_blob_ref_backend;
  if (!replay_all_events(unmap_wrong_blob_ref_reader, unmap_wrong_blob_ref_backend)) {
    std::cerr << "D3D12 replay rejected a skippable Unmap buffer path missing from blob_refs: "
              << unmap_wrong_blob_ref_backend.last_error() << "\n";
    return 1;
  }
  const auto *unmap_wrong_blob_ref_resource =
      apitrace::d3d12::D3D12ReplayBackendTestHook::resource_state(unmap_wrong_blob_ref_backend, 2);
  if (!unmap_wrong_blob_ref_resource ||
      !unmap_wrong_blob_ref_resource->data_updates.empty()) {
    std::cerr << "D3D12 replay did not skip the uncovered Unmap buffer payload\n";
    return 1;
  }

  const auto resolve_query_wrong_blob_ref_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-resolve-query-result-wrong-blob-ref");
  if (!write_resolve_query_data_result_wrong_blob_ref_bundle(resolve_query_wrong_blob_ref_bundle)) {
    std::cerr << "failed to write ResolveQueryDataResult wrong-blob-ref bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(resolve_query_wrong_blob_ref_bundle, "asset path is not covered by blob_refs")) {
    std::cerr << "D3D12 replay accepted a ResolveQueryDataResult buffer path missing from blob_refs\n";
    return 1;
  }

  const auto create_command_list1_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-create-command-list1");
  if (!write_create_command_list1_bundle(create_command_list1_bundle)) {
    std::cerr << "failed to write CreateCommandList1 semantic bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader create_list1_reader;
  if (!open_finalized_bundle(create_list1_reader, create_command_list1_bundle)) {
    std::cerr << "reader failed to reopen CreateCommandList1 bundle: "
              << create_list1_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend create_list1_backend;
  if (!replay_all_events(create_list1_reader, create_list1_backend)) {
    std::cerr << "CreateCommandList1 replay_event failed: "
              << create_list1_backend.last_error() << "\n";
    return 1;
  }
  const auto *created_list1 = create_list1_backend.command_list_state(15);
  if (!created_list1 ||
      created_list1->recording ||
      created_list1->type != 0 ||
      created_list1->node_mask != 2 ||
      created_list1->allocator_object_id != 0 ||
      created_list1->pipeline_state_object_id != 0 ||
      created_list1->first_sequence == 0 ||
      created_list1->close_sequence != 0) {
    std::cerr << "CreateCommandList1 semantic state was not preserved\n";
    return 1;
  }
  if (create_list1_backend.validate_replay_closure() ||
      create_list1_backend.last_error().find("D3D12 replay did not collect command records") == std::string::npos) {
    std::cerr << "CreateCommandList1-only bundle unexpectedly satisfied replay closure: "
              << create_list1_backend.last_error() << "\n";
    return 1;
  }

  const auto table_without_heap_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-descriptor-table-without-bound-heap");
  if (!write_descriptor_table_without_bound_heap_bundle(table_without_heap_bundle)) {
    std::cerr << "failed to write descriptor-table without-bound-heap bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(table_without_heap_bundle, "descriptor handle does not map to a captured descriptor heap")) {
    std::cerr << "D3D12 replay accepted a descriptor table without a bound heap\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend unbound_table_backend;
  apitrace::d3d12::D3D12ReplayBackendTestHook::seed_unbound_root_descriptor_table(unbound_table_backend);
  if (unbound_table_backend.validate_native_replay_readiness() ||
      unbound_table_backend.last_error().find("root descriptor table heap is not currently bound") == std::string::npos) {
    std::cerr << "native readiness accepted an unbound root descriptor table: "
              << unbound_table_backend.last_error() << "\n";
    return 1;
  }

  const auto table_rebind_clears_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-descriptor-table-rebind-clears");
  if (!write_descriptor_table_rebind_clears_bundle(table_rebind_clears_bundle)) {
    std::cerr << "failed to write descriptor-table rebind-clears bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader table_rebind_clears_reader;
  if (!open_finalized_bundle(table_rebind_clears_reader, table_rebind_clears_bundle)) {
    std::cerr << "reader failed to reopen descriptor-table rebind-clears bundle: "
              << table_rebind_clears_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend table_rebind_clears_backend;
  if (!replay_all_events(table_rebind_clears_reader, table_rebind_clears_backend)) {
    std::cerr << "descriptor-table rebind-clears replay_event failed: "
              << table_rebind_clears_backend.last_error() << "\n";
    return 1;
  }
  const auto *table_rebind_clears_list = table_rebind_clears_backend.command_list_state(15);
  if (!table_rebind_clears_list ||
      table_rebind_clears_list->descriptor_heap_object_ids.size() != 1 ||
      table_rebind_clears_list->descriptor_heap_object_ids[0] != 3 ||
      !table_rebind_clears_list->graphics_root_tables.empty() ||
      !table_rebind_clears_list->compute_root_tables.empty()) {
    std::cerr << "SetDescriptorHeaps did not clear stale root descriptor table bindings\n";
    return 1;
  }
  if (!table_rebind_clears_backend.validate_native_replay_readiness()) {
    std::cerr << "descriptor-table rebind-clears bundle was rejected by native readiness: "
              << table_rebind_clears_backend.last_error() << "\n";
    return 1;
  }

  const auto draw_with_late_descriptor_table_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-with-late-descriptor-table");
  if (!write_draw_with_late_descriptor_table_bundle(draw_with_late_descriptor_table_bundle)) {
    std::cerr << "failed to write draw with late descriptor-table bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader draw_with_late_descriptor_table_reader;
  if (!open_finalized_bundle(draw_with_late_descriptor_table_reader, draw_with_late_descriptor_table_bundle)) {
    std::cerr << "reader failed to reopen draw with late descriptor-table bundle: "
              << draw_with_late_descriptor_table_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend draw_with_late_descriptor_table_backend;
  if (!replay_all_events(draw_with_late_descriptor_table_reader, draw_with_late_descriptor_table_backend)) {
    std::cerr << "draw with late descriptor-table replay_event failed: "
              << draw_with_late_descriptor_table_backend.last_error() << "\n";
    return 1;
  }
  if (draw_with_late_descriptor_table_backend.validate_native_replay_readiness() ||
      draw_with_late_descriptor_table_backend.last_error().find("DrawInstanced sequence") == std::string::npos ||
      draw_with_late_descriptor_table_backend.last_error().find("root descriptor table references an uninitialized descriptor") == std::string::npos) {
    std::cerr << "native readiness accepted a draw whose descriptor table was initialized after execution: "
              << draw_with_late_descriptor_table_backend.last_error() << "\n";
    return 1;
  }

  const auto incomplete_descriptor_table_range_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-with-incomplete-descriptor-table-range");
  if (!write_draw_with_incomplete_descriptor_table_range_bundle(incomplete_descriptor_table_range_bundle)) {
    std::cerr << "failed to write draw with incomplete descriptor-table range bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader incomplete_descriptor_table_range_reader;
  if (!open_finalized_bundle(incomplete_descriptor_table_range_reader, incomplete_descriptor_table_range_bundle)) {
    std::cerr << "reader failed to reopen incomplete descriptor-table range bundle: "
              << incomplete_descriptor_table_range_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend incomplete_descriptor_table_range_backend;
  if (!replay_all_events(incomplete_descriptor_table_range_reader, incomplete_descriptor_table_range_backend)) {
    std::cerr << "incomplete descriptor-table range replay_event failed: "
              << incomplete_descriptor_table_range_backend.last_error() << "\n";
    return 1;
  }
  if (incomplete_descriptor_table_range_backend.validate_native_replay_readiness() ||
      incomplete_descriptor_table_range_backend.last_error().find("DrawInstanced sequence") == std::string::npos ||
      incomplete_descriptor_table_range_backend.last_error().find("root descriptor table references an uninitialized descriptor") == std::string::npos) {
    std::cerr << "native readiness accepted a draw with an incomplete descriptor-table range: "
              << incomplete_descriptor_table_range_backend.last_error() << "\n";
    return 1;
  }

  const auto draw_with_late_render_target_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-draw-with-late-render-target");
  if (!write_draw_with_late_render_target_bundle(draw_with_late_render_target_bundle)) {
    std::cerr << "failed to write draw with late render-target bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader draw_with_late_render_target_reader;
  if (!open_finalized_bundle(draw_with_late_render_target_reader, draw_with_late_render_target_bundle)) {
    std::cerr << "reader failed to reopen draw with late render-target bundle: "
              << draw_with_late_render_target_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend draw_with_late_render_target_backend;
  if (!replay_all_events(draw_with_late_render_target_reader, draw_with_late_render_target_backend)) {
    std::cerr << "draw with late render-target replay_event failed: "
              << draw_with_late_render_target_backend.last_error() << "\n";
    return 1;
  }
  if (draw_with_late_render_target_backend.validate_native_replay_readiness() ||
      draw_with_late_render_target_backend.last_error().find("DrawInstanced sequence") == std::string::npos ||
      draw_with_late_render_target_backend.last_error().find("references an uninitialized descriptor") == std::string::npos) {
    std::cerr << "native readiness accepted a draw whose render target was initialized after execution: "
              << draw_with_late_render_target_backend.last_error() << "\n";
    return 1;
  }

  const struct {
    const char *suffix;
    bool include_viewport;
    bool include_scissor;
    bool include_topology;
    const char *error_substring;
  } missing_raster_state_cases[] = {
      {"draw-without-viewport", false, true, true, "has no bound viewport"},
      {"draw-without-scissor", true, false, true, "has no bound scissor rect"},
      {"draw-without-primitive-topology", true, true, false, "has no bound primitive topology"},
  };
  for (const auto &test_case : missing_raster_state_cases) {
    const auto draw_missing_raster_state_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-" + test_case.suffix);
    if (!write_draw_missing_raster_state_bundle(
            draw_missing_raster_state_bundle,
            test_case.include_viewport,
            test_case.include_scissor,
            test_case.include_topology)) {
      std::cerr << "failed to write " << test_case.suffix << " bundle\n";
      return 1;
    }
    std::string readiness_error;
    if (!expect_d3d12_native_readiness_failure(
            draw_missing_raster_state_bundle,
            test_case.error_substring,
            readiness_error)) {
      std::cerr << "native readiness did not reject " << test_case.suffix
                << ": " << readiness_error << "\n";
      return 1;
    }
  }

  const auto render_pass_with_late_render_target_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-render-pass-with-late-render-target");
  if (!write_render_pass_with_late_render_target_bundle(render_pass_with_late_render_target_bundle)) {
    std::cerr << "failed to write render pass with late render-target bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader render_pass_with_late_render_target_reader;
  if (!open_finalized_bundle(render_pass_with_late_render_target_reader, render_pass_with_late_render_target_bundle)) {
    std::cerr << "reader failed to reopen render pass with late render-target bundle: "
              << render_pass_with_late_render_target_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend render_pass_with_late_render_target_backend;
  if (!replay_all_events(render_pass_with_late_render_target_reader, render_pass_with_late_render_target_backend)) {
    std::cerr << "render pass with late render-target replay_event failed: "
              << render_pass_with_late_render_target_backend.last_error() << "\n";
    return 1;
  }
  if (render_pass_with_late_render_target_backend.validate_native_replay_readiness() ||
      render_pass_with_late_render_target_backend.last_error().find("BeginRenderPass sequence") == std::string::npos ||
      render_pass_with_late_render_target_backend.last_error().find("references an uninitialized descriptor") == std::string::npos) {
    std::cerr << "native readiness accepted a render pass whose render target was initialized after execution: "
              << render_pass_with_late_render_target_backend.last_error() << "\n";
    return 1;
  }

  const auto write_buffer_immediate_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-write-buffer-immediate");
  if (!write_write_buffer_immediate_bundle(write_buffer_immediate_bundle)) {
    std::cerr << "failed to write WriteBufferImmediate bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader write_buffer_immediate_reader;
  if (!open_finalized_bundle(write_buffer_immediate_reader, write_buffer_immediate_bundle)) {
    std::cerr << "reader failed to reopen WriteBufferImmediate bundle: "
              << write_buffer_immediate_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend write_buffer_immediate_backend;
  if (!replay_all_events(write_buffer_immediate_reader, write_buffer_immediate_backend)) {
    std::cerr << "WriteBufferImmediate replay_event failed: "
              << write_buffer_immediate_backend.last_error() << "\n";
    return 1;
  }
  const auto *write_buffer_immediate_list = write_buffer_immediate_backend.command_list_state(15);
  if (!write_buffer_immediate_list ||
      write_buffer_immediate_list->write_buffer_immediates.size() != 1 ||
      write_buffer_immediate_list->write_buffer_immediates[0].writes.size() != 1 ||
      write_buffer_immediate_list->write_buffer_immediates[0].writes[0].dest.resource_object_id != 5 ||
      write_buffer_immediate_list->write_buffer_immediates[0].writes[0].dest.offset != 16 ||
      write_buffer_immediate_list->write_buffer_immediates[0].writes[0].value != 3735928559u) {
    std::cerr << "WriteBufferImmediate semantic state was not preserved\n";
    return 1;
  }
  if (!write_buffer_immediate_backend.validate_replay_closure()) {
    std::cerr << "WriteBufferImmediate replay closure validation failed: "
              << write_buffer_immediate_backend.last_error() << "\n";
    return 1;
  }
  if (!write_buffer_immediate_backend.validate_native_replay_readiness()) {
    std::cerr << "WriteBufferImmediate bundle was not accepted by native readiness: "
              << write_buffer_immediate_backend.last_error() << "\n";
    return 1;
  }

  const auto write_buffer_immediate_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-write-buffer-immediate-out-of-bounds");
  if (!write_write_buffer_immediate_bundle(write_buffer_immediate_out_of_bounds_bundle, 4094)) {
    std::cerr << "failed to write WriteBufferImmediate out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader write_buffer_immediate_out_of_bounds_reader;
  if (!open_finalized_bundle(write_buffer_immediate_out_of_bounds_reader, write_buffer_immediate_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen WriteBufferImmediate out-of-bounds bundle: "
              << write_buffer_immediate_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend write_buffer_immediate_out_of_bounds_backend;
  if (!replay_all_events(write_buffer_immediate_out_of_bounds_reader, write_buffer_immediate_out_of_bounds_backend)) {
    std::cerr << "WriteBufferImmediate out-of-bounds replay_event failed: "
              << write_buffer_immediate_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (write_buffer_immediate_out_of_bounds_backend.validate_native_replay_readiness() ||
      write_buffer_immediate_out_of_bounds_backend.last_error().find("WriteBufferImmediate dest sequence") == std::string::npos ||
      write_buffer_immediate_out_of_bounds_backend.last_error().find("buffer range is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds WriteBufferImmediate destination: "
              << write_buffer_immediate_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto copy_buffer_range_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-buffer-range-out-of-bounds");
  if (!write_copy_buffer_range_out_of_bounds_bundle(copy_buffer_range_out_of_bounds_bundle)) {
    std::cerr << "failed to write copy-buffer range out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader copy_buffer_range_out_of_bounds_reader;
  if (!open_finalized_bundle(copy_buffer_range_out_of_bounds_reader, copy_buffer_range_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen copy-buffer range out-of-bounds bundle: "
              << copy_buffer_range_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend copy_buffer_range_out_of_bounds_backend;
  if (!replay_all_events(copy_buffer_range_out_of_bounds_reader, copy_buffer_range_out_of_bounds_backend)) {
    std::cerr << "copy-buffer range out-of-bounds replay_event failed: "
              << copy_buffer_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (copy_buffer_range_out_of_bounds_backend.validate_native_replay_readiness() ||
      copy_buffer_range_out_of_bounds_backend.last_error().find("CopyBufferRegion destination sequence") == std::string::npos ||
      copy_buffer_range_out_of_bounds_backend.last_error().find("buffer range is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds CopyBufferRegion range: "
              << copy_buffer_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto execute_bundle_copy_buffer_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-execute-bundle-copy-buffer");
  if (!write_execute_bundle_copy_buffer_bundle(execute_bundle_copy_buffer_bundle)) {
    std::cerr << "failed to write ExecuteBundle copy-buffer bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader execute_bundle_copy_buffer_reader;
  if (!open_finalized_bundle(execute_bundle_copy_buffer_reader, execute_bundle_copy_buffer_bundle)) {
    std::cerr << "reader failed to reopen ExecuteBundle copy-buffer bundle: "
              << execute_bundle_copy_buffer_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend execute_bundle_copy_buffer_backend;
  if (!replay_all_events(execute_bundle_copy_buffer_reader, execute_bundle_copy_buffer_backend)) {
    std::cerr << "ExecuteBundle copy-buffer replay_event failed: "
              << execute_bundle_copy_buffer_backend.last_error() << "\n";
    return 1;
  }
  if (!execute_bundle_copy_buffer_backend.validate_replay_closure()) {
    std::cerr << "ExecuteBundle copy-buffer bundle did not satisfy replay closure: "
              << execute_bundle_copy_buffer_backend.last_error() << "\n";
    return 1;
  }
  if (!execute_bundle_copy_buffer_backend.validate_native_replay_readiness()) {
    std::cerr << "ExecuteBundle copy-buffer bundle was rejected by native readiness: "
              << execute_bundle_copy_buffer_backend.last_error() << "\n";
    return 1;
  }

  const auto copy_texture_source_box_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-texture-source-box-out-of-bounds");
  if (!write_copy_texture_source_box_out_of_bounds_bundle(copy_texture_source_box_out_of_bounds_bundle)) {
    std::cerr << "failed to write copy-texture source-box out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader copy_texture_source_box_out_of_bounds_reader;
  if (!open_finalized_bundle(copy_texture_source_box_out_of_bounds_reader, copy_texture_source_box_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen copy-texture source-box out-of-bounds bundle: "
              << copy_texture_source_box_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend copy_texture_source_box_out_of_bounds_backend;
  if (!replay_all_events(copy_texture_source_box_out_of_bounds_reader, copy_texture_source_box_out_of_bounds_backend)) {
    std::cerr << "copy-texture source-box out-of-bounds replay_event failed: "
              << copy_texture_source_box_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (copy_texture_source_box_out_of_bounds_backend.validate_native_replay_readiness() ||
      copy_texture_source_box_out_of_bounds_backend.last_error().find("CopyTextureRegion sequence") == std::string::npos ||
      copy_texture_source_box_out_of_bounds_backend.last_error().find("source box is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds CopyTextureRegion source box: "
              << copy_texture_source_box_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto copy_texture_batch_invalid_refs_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-texture-batch-invalid-refs");
  if (!run_copy_texture_batch_invalid_refs_case(copy_texture_batch_invalid_refs_bundle)) {
    return 1;
  }

  const auto copy_texture_full_copy_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-texture-full-copy-out-of-bounds");
  if (!write_copy_texture_full_copy_out_of_bounds_bundle(copy_texture_full_copy_out_of_bounds_bundle)) {
    std::cerr << "failed to write copy-texture full-copy out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader copy_texture_full_copy_out_of_bounds_reader;
  if (!open_finalized_bundle(copy_texture_full_copy_out_of_bounds_reader, copy_texture_full_copy_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen copy-texture full-copy out-of-bounds bundle: "
              << copy_texture_full_copy_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend copy_texture_full_copy_out_of_bounds_backend;
  if (!replay_all_events(copy_texture_full_copy_out_of_bounds_reader, copy_texture_full_copy_out_of_bounds_backend)) {
    std::cerr << "copy-texture full-copy out-of-bounds replay_event failed: "
              << copy_texture_full_copy_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (copy_texture_full_copy_out_of_bounds_backend.validate_native_replay_readiness() ||
      copy_texture_full_copy_out_of_bounds_backend.last_error().find("destination full-copy region is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds full CopyTextureRegion: "
              << copy_texture_full_copy_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto copy_texture_missing_location_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-texture-missing-location");
  if (!write_copy_texture_missing_location_bundle(copy_texture_missing_location_bundle)) {
    std::cerr << "failed to write CopyTextureRegion missing-location bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          copy_texture_missing_location_bundle,
          "CopyTextureRegion missing texture copy location payload")) {
    std::cerr << "D3D12 replay accepted CopyTextureRegion without source location\n";
    return 1;
  }

  const auto set_pipeline_state_missing_ref_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-set-pipeline-state-missing-ref");
  if (!write_set_pipeline_state_missing_ref_bundle(set_pipeline_state_missing_ref_bundle)) {
    std::cerr << "failed to write SetPipelineState missing-ref bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          set_pipeline_state_missing_ref_bundle,
          "SetPipelineState missing pipeline state object id")) {
    std::cerr << "D3D12 replay accepted SetPipelineState without a PSO ref\n";
    return 1;
  }

  const auto viewport_first_cannot_cover_multiple_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-viewport-first-cannot-cover-multiple");
  if (!write_viewport_first_cannot_cover_multiple_bundle(viewport_first_cannot_cover_multiple_bundle)) {
    std::cerr << "failed to write viewport first-only mismatch bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          viewport_first_cannot_cover_multiple_bundle,
          "RSSetViewports first viewport cannot cover multiple viewports")) {
    std::cerr << "D3D12 replay accepted first-only viewport payload for multiple viewports\n";
    return 1;
  }

  const auto omset_depth_only_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-omset-depth-only");
  if (!write_omset_render_targets_depth_only_bundle(omset_depth_only_bundle, true)) {
    std::cerr << "failed to write OMSetRenderTargets depth-only bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader omset_depth_only_reader;
  if (!open_finalized_bundle(omset_depth_only_reader, omset_depth_only_bundle)) {
    std::cerr << "reader failed to reopen OMSetRenderTargets depth-only bundle: "
              << omset_depth_only_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend omset_depth_only_backend;
  if (!replay_all_events(omset_depth_only_reader, omset_depth_only_backend)) {
    std::cerr << "OMSetRenderTargets depth-only replay_event failed: "
              << omset_depth_only_backend.last_error() << "\n";
    return 1;
  }
  const auto *omset_depth_only_list = omset_depth_only_backend.command_list_state(15);
  if (!omset_depth_only_list ||
      omset_depth_only_list->render_target_count != 0 ||
      !omset_depth_only_list->render_targets.empty()) {
    std::cerr << "OMSetRenderTargets depth-only semantic state was not preserved\n";
    return 1;
  }

  const auto omset_missing_dsv_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-omset-missing-dsv");
  if (!write_omset_render_targets_depth_only_bundle(omset_missing_dsv_bundle, false)) {
    std::cerr << "failed to write OMSetRenderTargets missing-DSV bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          omset_missing_dsv_bundle,
          "missing payload key dsv")) {
    std::cerr << "D3D12 replay accepted OMSetRenderTargets without explicit DSV payload\n";
    return 1;
  }

  const auto resource_barrier_subresource_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-resource-barrier-subresource-out-of-bounds");
  if (!write_resource_barrier_subresource_out_of_bounds_bundle(resource_barrier_subresource_out_of_bounds_bundle)) {
    std::cerr << "failed to write ResourceBarrier subresource out-of-bounds bundle\n";
    return 1;
  }
  if (!expect_d3d12_replay_failure(
          resource_barrier_subresource_out_of_bounds_bundle,
          "ResourceBarrier subresource index is out of bounds")) {
    std::cerr << "D3D12 replay accepted an out-of-bounds ResourceBarrier subresource\n";
    return 1;
  }

  const auto resource_barrier_missing_uav_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-resource-barrier-missing-uav-payload");
  std::filesystem::remove_all(resource_barrier_missing_uav_payload_bundle);
  {
    apitrace::trace::TraceBundleWriter missing_uav_writer;
    if (!missing_uav_writer.open(resource_barrier_missing_uav_payload_bundle)) {
      std::cerr << "failed to open ResourceBarrier missing UAV payload bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata missing_uav_metadata;
    missing_uav_metadata.api = apitrace::trace::ApiKind::D3D12;
    missing_uav_metadata.producer = "test_d3d12_resource_barrier_missing_uav_payload";
    missing_uav_writer.write_metadata(missing_uav_metadata);
    std::uint64_t sequence = 1;
    append(missing_uav_writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
    append(missing_uav_writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
    append(missing_uav_writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
    append(missing_uav_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandAllocator",
        {1, 14},
        "{\"type\":0}"));
    append(missing_uav_writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
    append(missing_uav_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandList",
        {1, 14, 0, 15},
        "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
        "\"initial_pipeline_state_object_id\":0}"));
    append(missing_uav_writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::ResourceBarrier",
        {15},
        "{\"barrier_count\":1,\"barriers\":[{\"type\":2,\"flags\":0}]}"));
    missing_uav_writer.close();
  }
  if (!expect_d3d12_replay_failure(
          resource_barrier_missing_uav_payload_bundle,
          "missing payload key resource_object_id")) {
    std::cerr << "D3D12 replay accepted ResourceBarrier UAV without resource payload\n";
    return 1;
  }

  const auto discard_resource_rect_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-discard-resource-rect-out-of-bounds");
  if (!write_discard_resource_rect_out_of_bounds_bundle(discard_resource_rect_out_of_bounds_bundle)) {
    std::cerr << "failed to write DiscardResource rect out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader discard_resource_rect_out_of_bounds_reader;
  if (!open_finalized_bundle(discard_resource_rect_out_of_bounds_reader, discard_resource_rect_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen DiscardResource rect out-of-bounds bundle: "
              << discard_resource_rect_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend discard_resource_rect_out_of_bounds_backend;
  if (!replay_all_events(discard_resource_rect_out_of_bounds_reader, discard_resource_rect_out_of_bounds_backend)) {
    std::cerr << "DiscardResource rect out-of-bounds replay_event failed: "
              << discard_resource_rect_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (discard_resource_rect_out_of_bounds_backend.validate_native_replay_readiness() ||
      discard_resource_rect_out_of_bounds_backend.last_error().find("DiscardResource sequence") == std::string::npos ||
      discard_resource_rect_out_of_bounds_backend.last_error().find("rect is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds DiscardResource rect: "
              << discard_resource_rect_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto resolve_subresource_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-resolve-subresource-out-of-bounds");
  if (!write_resolve_subresource_out_of_bounds_bundle(resolve_subresource_out_of_bounds_bundle, false)) {
    std::cerr << "failed to write ResolveSubresource out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader resolve_subresource_out_of_bounds_reader;
  if (!open_finalized_bundle(resolve_subresource_out_of_bounds_reader, resolve_subresource_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen ResolveSubresource out-of-bounds bundle: "
              << resolve_subresource_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend resolve_subresource_out_of_bounds_backend;
  if (!replay_all_events(resolve_subresource_out_of_bounds_reader, resolve_subresource_out_of_bounds_backend)) {
    std::cerr << "ResolveSubresource out-of-bounds replay_event failed: "
              << resolve_subresource_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (resolve_subresource_out_of_bounds_backend.validate_native_replay_readiness() ||
      resolve_subresource_out_of_bounds_backend.last_error().find("ResolveSubresource sequence") == std::string::npos ||
      resolve_subresource_out_of_bounds_backend.last_error().find("subresource index is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds ResolveSubresource subresource: "
              << resolve_subresource_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto resolve_region_rect_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-resolve-region-rect-out-of-bounds");
  if (!write_resolve_subresource_out_of_bounds_bundle(resolve_region_rect_out_of_bounds_bundle, true)) {
    std::cerr << "failed to write ResolveSubresourceRegion rect out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader resolve_region_rect_out_of_bounds_reader;
  if (!open_finalized_bundle(resolve_region_rect_out_of_bounds_reader, resolve_region_rect_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen ResolveSubresourceRegion rect out-of-bounds bundle: "
              << resolve_region_rect_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend resolve_region_rect_out_of_bounds_backend;
  if (!replay_all_events(resolve_region_rect_out_of_bounds_reader, resolve_region_rect_out_of_bounds_backend)) {
    std::cerr << "ResolveSubresourceRegion rect out-of-bounds replay_event failed: "
              << resolve_region_rect_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (resolve_region_rect_out_of_bounds_backend.validate_native_replay_readiness() ||
      resolve_region_rect_out_of_bounds_backend.last_error().find("ResolveSubresourceRegion sequence") == std::string::npos ||
      resolve_region_rect_out_of_bounds_backend.last_error().find("source rect is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds ResolveSubresourceRegion source rect: "
              << resolve_region_rect_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto vertex_buffer_range_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-vertex-buffer-range-out-of-bounds");
  if (!write_ia_buffer_range_out_of_bounds_bundle(vertex_buffer_range_out_of_bounds_bundle, false)) {
    std::cerr << "failed to write vertex buffer range out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader vertex_buffer_range_out_of_bounds_reader;
  if (!open_finalized_bundle(vertex_buffer_range_out_of_bounds_reader, vertex_buffer_range_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen vertex buffer range out-of-bounds bundle: "
              << vertex_buffer_range_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend vertex_buffer_range_out_of_bounds_backend;
  if (!replay_all_events(vertex_buffer_range_out_of_bounds_reader, vertex_buffer_range_out_of_bounds_backend)) {
    std::cerr << "vertex buffer range out-of-bounds replay_event failed: "
              << vertex_buffer_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (vertex_buffer_range_out_of_bounds_backend.validate_native_replay_readiness() ||
      vertex_buffer_range_out_of_bounds_backend.last_error().find("vertex buffer sequence") == std::string::npos ||
      vertex_buffer_range_out_of_bounds_backend.last_error().find("buffer range is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds vertex buffer range: "
              << vertex_buffer_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto index_buffer_range_out_of_bounds_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-index-buffer-range-out-of-bounds");
  if (!write_ia_buffer_range_out_of_bounds_bundle(index_buffer_range_out_of_bounds_bundle, true)) {
    std::cerr << "failed to write index buffer range out-of-bounds bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader index_buffer_range_out_of_bounds_reader;
  if (!open_finalized_bundle(index_buffer_range_out_of_bounds_reader, index_buffer_range_out_of_bounds_bundle)) {
    std::cerr << "reader failed to reopen index buffer range out-of-bounds bundle: "
              << index_buffer_range_out_of_bounds_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend index_buffer_range_out_of_bounds_backend;
  if (!replay_all_events(index_buffer_range_out_of_bounds_reader, index_buffer_range_out_of_bounds_backend)) {
    std::cerr << "index buffer range out-of-bounds replay_event failed: "
              << index_buffer_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }
  if (index_buffer_range_out_of_bounds_backend.validate_native_replay_readiness() ||
      index_buffer_range_out_of_bounds_backend.last_error().find("index buffer sequence") == std::string::npos ||
      index_buffer_range_out_of_bounds_backend.last_error().find("buffer range is out of bounds") == std::string::npos) {
    std::cerr << "native readiness accepted an out-of-bounds index buffer range: "
              << index_buffer_range_out_of_bounds_backend.last_error() << "\n";
    return 1;
  }

  const auto copy_resource_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-resource");
  if (!write_copy_resource_bundle(copy_resource_bundle, false)) {
    std::cerr << "failed to write CopyResource bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader copy_resource_reader;
  if (!open_finalized_bundle(copy_resource_reader, copy_resource_bundle)) {
    std::cerr << "reader failed to reopen CopyResource bundle: "
              << copy_resource_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend copy_resource_backend;
  if (!replay_all_events(copy_resource_reader, copy_resource_backend)) {
    std::cerr << "CopyResource replay_event failed: "
              << copy_resource_backend.last_error() << "\n";
    return 1;
  }
  const auto *copy_resource_list = copy_resource_backend.command_list_state(15);
  if (!copy_resource_list ||
      copy_resource_list->resource_copies.size() != 1 ||
      copy_resource_list->resource_copies[0].dst_resource_object_id != 5 ||
      copy_resource_list->resource_copies[0].src_resource_object_id != 6) {
    std::cerr << "CopyResource semantic state was not preserved\n";
    return 1;
  }
  if (!copy_resource_backend.validate_native_replay_readiness()) {
    std::cerr << "CopyResource bundle was rejected by native readiness: "
              << copy_resource_backend.last_error() << "\n";
    return 1;
  }

  const auto copy_resource_mismatched_desc_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-copy-resource-mismatched-desc");
  if (!write_copy_resource_bundle(copy_resource_mismatched_desc_bundle, true)) {
    std::cerr << "failed to write CopyResource mismatched-desc bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader copy_resource_mismatched_desc_reader;
  if (!open_finalized_bundle(copy_resource_mismatched_desc_reader, copy_resource_mismatched_desc_bundle)) {
    std::cerr << "reader failed to reopen CopyResource mismatched-desc bundle: "
              << copy_resource_mismatched_desc_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend copy_resource_mismatched_desc_backend;
  if (!replay_all_events(copy_resource_mismatched_desc_reader, copy_resource_mismatched_desc_backend)) {
    std::cerr << "CopyResource mismatched-desc replay_event failed: "
              << copy_resource_mismatched_desc_backend.last_error() << "\n";
    return 1;
  }
  if (copy_resource_mismatched_desc_backend.validate_native_replay_readiness() ||
      copy_resource_mismatched_desc_backend.last_error().find("CopyResource sequence") == std::string::npos ||
      copy_resource_mismatched_desc_backend.last_error().find("resource descriptions do not match") == std::string::npos) {
    std::cerr << "native readiness accepted a CopyResource with mismatched resource descriptions: "
              << copy_resource_mismatched_desc_backend.last_error() << "\n";
    return 1;
  }

  const auto resolve_region_requires_list1_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-resolve-region-requires-list1");
  if (!write_resolve_region_requires_list1_bundle(resolve_region_requires_list1_bundle)) {
    std::cerr << "failed to write ResolveSubresourceRegion requires-list1 bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader resolve_region_reader;
  if (!open_finalized_bundle(resolve_region_reader, resolve_region_requires_list1_bundle)) {
    std::cerr << "reader failed to reopen ResolveSubresourceRegion requires-list1 bundle: "
              << resolve_region_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend resolve_region_backend;
  if (!replay_all_events(resolve_region_reader, resolve_region_backend)) {
    std::cerr << "ResolveSubresourceRegion requires-list1 replay_event failed: "
              << resolve_region_backend.last_error() << "\n";
    return 1;
  }
  const auto *resolve_region_list = resolve_region_backend.command_list_state(15);
  if (!resolve_region_list ||
      resolve_region_list->resolves.size() != 1 ||
      resolve_region_list->resolves[0].dst_x != 4 ||
      resolve_region_list->resolves[0].mode != 3 ||
      !resolve_region_list->resolves[0].has_src_rect) {
    std::cerr << "ResolveSubresourceRegion requires-list1 semantic state was not preserved\n";
    return 1;
  }
  if (!resolve_region_backend.validate_replay_closure()) {
    std::cerr << "ResolveSubresourceRegion requires-list1 replay closure validation failed: "
              << resolve_region_backend.last_error() << "\n";
    return 1;
  }
  if (!resolve_region_backend.validate_native_replay_readiness()) {
    std::cerr << "ResolveSubresourceRegion requires-list1 bundle was not accepted by native readiness: "
              << resolve_region_backend.last_error() << "\n";
    return 1;
  }

  const auto dispatch_rays_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-dispatch-rays");
  if (!write_dispatch_rays_bundle(dispatch_rays_bundle)) {
    std::cerr << "failed to write DispatchRays semantic bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader dispatch_rays_reader;
  if (!open_finalized_bundle(dispatch_rays_reader, dispatch_rays_bundle)) {
    std::cerr << "reader failed to reopen DispatchRays bundle: " << dispatch_rays_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend dispatch_rays_backend;
  if (!replay_all_events(dispatch_rays_reader, dispatch_rays_backend)) {
    std::cerr << "DispatchRays replay_event failed: " << dispatch_rays_backend.last_error() << "\n";
    return 1;
  }
  const auto *dispatch_rays_list = dispatch_rays_backend.command_list_state(15);
  if (!dispatch_rays_list ||
      dispatch_rays_list->ray_dispatches.size() != 1 ||
      dispatch_rays_list->ray_dispatches[0].width != 16 ||
      dispatch_rays_list->ray_dispatches[0].ray_generation_shader_record.resource_object_id != 4 ||
      dispatch_rays_list->ray_dispatches[0].miss_shader_table.offset != 64 ||
      dispatch_rays_list->ray_dispatches[0].hit_group_table.offset != 192 ||
      dispatch_rays_list->ray_dispatches[0].callable_shader_table.resource_object_id != 0) {
    std::cerr << "DispatchRays semantic state was not preserved\n";
    return 1;
  }
  if (!dispatch_rays_backend.validate_replay_closure()) {
    std::cerr << "DispatchRays replay closure validation failed: " << dispatch_rays_backend.last_error() << "\n";
    return 1;
  }
  if (dispatch_rays_backend.validate_native_replay_readiness() ||
      dispatch_rays_backend.last_error().find("DispatchRays requires a DXMT-backed replay backend") == std::string::npos) {
    std::cerr << "DispatchRays bundle was not rejected by native readiness: "
              << dispatch_rays_backend.last_error() << "\n";
    return 1;
  }

  const auto temporal_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-temporal-upscale");
  if (!write_temporal_upscale_bundle(temporal_bundle)) {
    std::cerr << "failed to write TemporalUpscale semantic bundle\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader temporal_reader;
  if (!open_finalized_bundle(temporal_reader, temporal_bundle)) {
    std::cerr << "reader failed to reopen TemporalUpscale bundle: " << temporal_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend temporal_backend;
  if (!replay_all_events(temporal_reader, temporal_backend)) {
    std::cerr << "TemporalUpscale replay_event failed: " << temporal_backend.last_error() << "\n";
    return 1;
  }
  const auto *temporal_list = temporal_backend.command_list_state(15);
  if (!temporal_list ||
      temporal_list->temporal_upscales.size() != 1 ||
      temporal_list->temporal_upscales[0].input_content_width != 128 ||
      temporal_list->temporal_upscales[0].input_content_height != 64 ||
      !temporal_list->temporal_upscales[0].auto_exposure ||
      !temporal_list->temporal_upscales[0].depth_reversed ||
      temporal_list->temporal_upscales[0].color_object_id != 9 ||
      temporal_list->temporal_upscales[0].output_object_id != 9 ||
      temporal_list->temporal_upscales[0].motion_vector_scale_x != 0.5f ||
      temporal_list->temporal_upscales[0].jitter_offset_y != -0.25f) {
    std::cerr << "TemporalUpscale semantic state was not preserved\n";
    return 1;
  }
  if (!temporal_backend.validate_replay_closure()) {
    std::cerr << "TemporalUpscale replay closure validation failed: " << temporal_backend.last_error() << "\n";
    return 1;
  }
  if (temporal_backend.validate_native_replay_readiness() ||
      temporal_backend.last_error().find("TemporalUpscale requires a DXMT-backed replay backend") == std::string::npos) {
    std::cerr << "TemporalUpscale bundle was not rejected by native readiness: "
              << temporal_backend.last_error() << "\n";
    return 1;
  }

  const auto mesh_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-mesh-stream-pipeline");
  std::filesystem::remove_all(mesh_bundle);
  {
    apitrace::trace::TraceBundleWriter mesh_writer;
    if (!mesh_writer.open(mesh_bundle)) {
      std::cerr << "failed to open mesh stream pipeline bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata mesh_metadata;
    mesh_metadata.api = apitrace::trace::ApiKind::D3D12;
    mesh_metadata.producer = "test_d3d12_mesh_stream_pipeline";
    mesh_writer.write_metadata(mesh_metadata);
    std::uint64_t sequence = 1;
    append(mesh_writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
    append(mesh_writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
    append(mesh_writer, object_create(sequence++, 10, ObjectKind::RootSignature, 1, "ID3D12RootSignature"));
    apitrace::trace::AssetRecord mesh_root_signature_asset;
    if (!write_asset(
            mesh_writer,
            apitrace::trace::AssetKind::RootSignature,
            "rootsig",
            "rootsig",
            mesh_root_signature_asset)) {
      std::cerr << "failed to write mesh root signature asset\n";
      return 1;
    }
    apitrace::trace::AssetRecord mesh_shader_asset;
    if (!write_asset(
            mesh_writer,
            apitrace::trace::AssetKind::ShaderDxil,
            "ms",
            "ms",
            mesh_shader_asset)) {
      std::cerr << "failed to write mesh shader asset\n";
      return 1;
    }
    append(mesh_writer, call(
        sequence++,
        "ID3D12Device::CreateRootSignature",
        {1, 10},
        "{\"node_mask\":0,\"bytecode_size\":7,\"root_signature_path\":\"" +
            mesh_root_signature_asset.relative_path.generic_string() + "\"}",
        {mesh_root_signature_asset.blob_id}));
    append(mesh_writer, object_create(sequence++, 32, ObjectKind::PipelineState, 1, "ID3D12PipelineState"));
    apitrace::trace::AssetRecord mesh_pipeline_asset;
    const std::string mesh_pipeline_json =
        "{\"type\":\"mesh\",\"source\":\"stream\",\"root_signature_object_id\":10,"
        "\"node_mask\":0,\"flags\":0,\"input_layout\":{\"element_count\":0,\"elements\":[]},"
        "\"blend_state\":{\"alpha_to_coverage_enable\":false,\"independent_blend_enable\":false,"
        "\"render_targets\":[]},\"sample_mask\":4294967295,"
        "\"rasterizer_state\":{},\"depth_stencil_state\":{},\"stream_output\":{},"
        "\"primitive_topology_type\":3,\"num_render_targets\":1,\"rtv_formats\":[28],"
        "\"dsv_format\":0,\"sample_desc\":{\"count\":1,\"quality\":0},"
        "\"ib_strip_cut_value\":0,"
        "\"ps\":{\"bytecode_size\":2,\"ps_path\":\"" +
        mesh_shader_asset.relative_path.generic_string() +
        "\"},\"ms\":{\"bytecode_size\":2,\"ms_path\":\"" +
        mesh_shader_asset.relative_path.generic_string() + "\"}}";
    if (!write_asset(
            mesh_writer,
            apitrace::trace::AssetKind::Pipeline,
            "mesh-stream-pipeline",
            mesh_pipeline_json,
            mesh_pipeline_asset)) {
      std::cerr << "failed to write mesh stream pipeline asset\n";
      return 1;
    }
    append(mesh_writer, call(
        sequence++,
        "ID3D12Device2::CreatePipelineState",
        {1, 32},
        "{\"pipeline_path\":\"" + mesh_pipeline_asset.relative_path.generic_string() +
            "\",\"source\":\"stream\",\"stream_size\":64,\"requires_dxmt_backend\":true}",
        {mesh_pipeline_asset.blob_id, mesh_shader_asset.blob_id, mesh_shader_asset.blob_id}));
    append(mesh_writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
    append(mesh_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandQueue",
        {1, 13},
        "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
    append(mesh_writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
    append(mesh_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandAllocator",
        {1, 14},
        "{\"type\":0}"));
    append(mesh_writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
    append(mesh_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandList",
        {1, 14, 32, 15},
        "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
        "\"initial_pipeline_state_object_id\":32}"));
    append(mesh_writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
    append(mesh_writer, call(
        sequence++,
        "ID3D12CommandQueue::ExecuteCommandLists",
        {13, 15},
        "{\"command_list_count\":1}"));
    mesh_writer.close();
  }
  apitrace::trace::TraceBundleReader mesh_reader;
  if (!open_finalized_bundle(mesh_reader, mesh_bundle)) {
    std::cerr << "reader failed to reopen mesh stream pipeline bundle: " << mesh_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend mesh_backend;
  if (!replay_all_events(mesh_reader, mesh_backend)) {
    std::cerr << "mesh stream replay_event failed: " << mesh_backend.last_error() << "\n";
    return 1;
  }
  if (mesh_backend.validate_native_replay_readiness() ||
      mesh_backend.last_error().find("pipeline requires a DXMT-backed replay backend") == std::string::npos) {
    std::cerr << "mesh stream pipeline was not rejected by native readiness: "
              << mesh_backend.last_error() << "\n";
    return 1;
  }

  const auto mismatch_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-gpuva-mismatch");
  std::filesystem::remove_all(mismatch_bundle);
  {
    apitrace::trace::TraceBundleWriter mismatch_writer;
    if (!mismatch_writer.open(mismatch_bundle)) {
      std::cerr << "failed to open gpuva mismatch bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata mismatch_metadata;
    mismatch_metadata.api = apitrace::trace::ApiKind::D3D12;
    mismatch_metadata.producer = "test_d3d12_gpuva_mismatch";
    mismatch_writer.write_metadata(mismatch_metadata);
    std::uint64_t sequence = 1;
    append(mismatch_writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
    append(mismatch_writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
    append(mismatch_writer, object_create(sequence++, 2, ObjectKind::DescriptorHeap, 1, "ID3D12DescriptorHeap"));
    append(mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateDescriptorHeap",
        {1, 2},
        "{\"type\":1,\"num_descriptors\":1,\"flags\":0,\"node_mask\":0,"
        "\"descriptor_size\":32,\"cpu_start\":8192,\"gpu_start\":0}"));
    append(mismatch_writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "ID3D12Resource"));
    append(mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateCommittedResource",
        {1, 4},
        "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":8192,"
        "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));
    append(mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateConstantBufferView",
        {1, 4},
        "{\"descriptor\":8192,\"buffer_location\":8192,\"size_in_bytes\":256,"
        "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":999,"
        "\"resolved_resource_offset\":0,\"resolved_resource_width\":4096}"));
    mismatch_writer.close();
  }
  apitrace::trace::TraceBundleReader mismatch_reader;
  if (!open_finalized_bundle(mismatch_reader, mismatch_bundle)) {
    std::cerr << "reader failed to reopen gpuva mismatch bundle: " << mismatch_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend mismatch_backend;
  if (replay_all_events(mismatch_reader, mismatch_backend) ||
      !error_contains_any(
          mismatch_backend.last_error(),
          {"resolved_resource_object_id mismatch", "payload references unknown object id 999"})) {
    std::cerr << "GPUVA mismatch bundle was not rejected: " << mismatch_backend.last_error() << "\n";
    return 1;
  }

  const auto root_mismatch_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-root-gpuva-mismatch");
  std::filesystem::remove_all(root_mismatch_bundle);
  {
    apitrace::trace::TraceBundleWriter root_mismatch_writer;
    if (!root_mismatch_writer.open(root_mismatch_bundle)) {
      std::cerr << "failed to open root gpuva mismatch bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata root_mismatch_metadata;
    root_mismatch_metadata.api = apitrace::trace::ApiKind::D3D12;
    root_mismatch_metadata.producer = "test_d3d12_root_gpuva_mismatch";
    root_mismatch_writer.write_metadata(root_mismatch_metadata);
    std::uint64_t sequence = 1;
    append(root_mismatch_writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
    append(root_mismatch_writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
    append(root_mismatch_writer, object_create(sequence++, 4, ObjectKind::Resource, 1, "ID3D12Resource"));
    append(root_mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateCommittedResource",
        {1, 4},
        "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":8192,"
        "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));
    append(root_mismatch_writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
    append(root_mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandAllocator",
        {1, 14},
        "{\"type\":0}"));
    append(root_mismatch_writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
    append(root_mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandList",
        {1, 14, 0, 15},
        "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
        "\"initial_pipeline_state_object_id\":0}"));
    append(root_mismatch_writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView",
        {15, 4},
        "{\"root_parameter_index\":0,\"buffer_location\":8192,"
        "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":999,"
        "\"resolved_resource_offset\":0,\"resolved_resource_width\":4096}"));
    root_mismatch_writer.close();
  }
  apitrace::trace::TraceBundleReader root_mismatch_reader;
  if (!open_finalized_bundle(root_mismatch_reader, root_mismatch_bundle)) {
    std::cerr << "reader failed to reopen root gpuva mismatch bundle: " << root_mismatch_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend root_mismatch_backend;
  if (replay_all_events(root_mismatch_reader, root_mismatch_backend) ||
      !error_contains_any(
          root_mismatch_backend.last_error(),
          {"root buffer_location resolved_resource_object_id mismatch", "payload references unknown object id 999"})) {
    std::cerr << "root GPUVA mismatch bundle was not rejected: " << root_mismatch_backend.last_error() << "\n";
    return 1;
  }

  const auto vertex_mismatch_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-vertex-gpuva-mismatch");
  std::filesystem::remove_all(vertex_mismatch_bundle);
  {
    apitrace::trace::TraceBundleWriter vertex_mismatch_writer;
    if (!vertex_mismatch_writer.open(vertex_mismatch_bundle)) {
      std::cerr << "failed to open vertex gpuva mismatch bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata vertex_mismatch_metadata;
    vertex_mismatch_metadata.api = apitrace::trace::ApiKind::D3D12;
    vertex_mismatch_metadata.producer = "test_d3d12_vertex_gpuva_mismatch";
    vertex_mismatch_writer.write_metadata(vertex_mismatch_metadata);
    std::uint64_t sequence = 1;
    append(vertex_mismatch_writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
    append(vertex_mismatch_writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
    append(vertex_mismatch_writer, object_create(sequence++, 5, ObjectKind::Resource, 1, "ID3D12Resource"));
    append(vertex_mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateCommittedResource",
        {1, 5},
        "{\"heap_type\":1,\"heap_flags\":0,\"initial_state\":0,\"gpu_virtual_address\":16384,"
        "\"resource_desc\":" + buffer_desc_json(4096) + ",\"optimized_clear_value\":null}"));
    append(vertex_mismatch_writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
    append(vertex_mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandAllocator",
        {1, 14},
        "{\"type\":0}"));
    append(vertex_mismatch_writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
    append(vertex_mismatch_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandList",
        {1, 14, 0, 15},
        "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
        "\"initial_pipeline_state_object_id\":0}"));
    append(vertex_mismatch_writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::IASetVertexBuffers",
        {15},
        "{\"start_slot\":0,\"view_count\":1,\"views\":[{\"buffer_location\":16384,"
        "\"gpuva_resolve_status\":\"mapped\",\"resolved_resource_object_id\":999,"
        "\"resolved_resource_offset\":0,\"resolved_resource_width\":4096,"
        "\"size_in_bytes\":512,\"stride_in_bytes\":16}]}"));
    vertex_mismatch_writer.close();
  }
  apitrace::trace::TraceBundleReader vertex_mismatch_reader;
  if (!open_finalized_bundle(vertex_mismatch_reader, vertex_mismatch_bundle)) {
    std::cerr << "reader failed to reopen vertex gpuva mismatch bundle: " << vertex_mismatch_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend vertex_mismatch_backend;
  if (replay_all_events(vertex_mismatch_reader, vertex_mismatch_backend) ||
      !error_contains_any(
          vertex_mismatch_backend.last_error(),
          {"vertex buffer_location resolved_resource_object_id mismatch", "payload references unknown object id 999"})) {
    std::cerr << "vertex GPUVA mismatch bundle was not rejected: " << vertex_mismatch_backend.last_error() << "\n";
    return 1;
  }

  const auto missing_vertex_view_payload_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-missing-vertex-view-payload");
  std::filesystem::remove_all(missing_vertex_view_payload_bundle);
  {
    apitrace::trace::TraceBundleWriter missing_vertex_view_writer;
    if (!missing_vertex_view_writer.open(missing_vertex_view_payload_bundle)) {
      std::cerr << "failed to open missing vertex view payload bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata missing_vertex_view_metadata;
    missing_vertex_view_metadata.api = apitrace::trace::ApiKind::D3D12;
    missing_vertex_view_metadata.producer = "test_d3d12_missing_vertex_view_payload";
    missing_vertex_view_writer.write_metadata(missing_vertex_view_metadata);
    std::uint64_t sequence = 1;
    append(missing_vertex_view_writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
    append(missing_vertex_view_writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
    append(missing_vertex_view_writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
    append(missing_vertex_view_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandAllocator",
        {1, 14},
        "{\"type\":0}"));
    append(missing_vertex_view_writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
    append(missing_vertex_view_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandList",
        {1, 14, 0, 15},
        "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
        "\"initial_pipeline_state_object_id\":0}"));
    append(missing_vertex_view_writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::IASetVertexBuffers",
        {15},
        "{\"start_slot\":0,\"view_count\":1}"));
    missing_vertex_view_writer.close();
  }
  if (!expect_d3d12_replay_failure(
          missing_vertex_view_payload_bundle,
          "IASetVertexBuffers missing view payload")) {
    std::cerr << "D3D12 replay accepted IASetVertexBuffers without view payload\n";
    return 1;
  }

  const auto root_unmapped_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-root-gpuva-unmapped");
  std::filesystem::remove_all(root_unmapped_bundle);
  {
    apitrace::trace::TraceBundleWriter root_unmapped_writer;
    if (!root_unmapped_writer.open(root_unmapped_bundle)) {
      std::cerr << "failed to open root gpuva unmapped bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata root_unmapped_metadata;
    root_unmapped_metadata.api = apitrace::trace::ApiKind::D3D12;
    root_unmapped_metadata.producer = "test_d3d12_root_gpuva_unmapped";
    root_unmapped_writer.write_metadata(root_unmapped_metadata);
    std::uint64_t sequence = 1;
    append(root_unmapped_writer, object_create(sequence++, 1, ObjectKind::Device, 0, "ID3D12Device"));
    append(root_unmapped_writer, call(sequence++, "D3D12CreateDevice", {1}, "{\"minimum_feature_level\":45056}"));
    append(root_unmapped_writer, object_create(sequence++, 13, ObjectKind::CommandQueue, 1, "ID3D12CommandQueue"));
    append(root_unmapped_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandQueue",
        {1, 13},
        "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}"));
    append(root_unmapped_writer, object_create(sequence++, 14, ObjectKind::CommandAllocator, 1, "ID3D12CommandAllocator"));
    append(root_unmapped_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandAllocator",
        {1, 14},
        "{\"type\":0}"));
    append(root_unmapped_writer, object_create(sequence++, 15, ObjectKind::CommandList, 1, "ID3D12GraphicsCommandList"));
    append(root_unmapped_writer, call(
        sequence++,
        "ID3D12Device::CreateCommandList",
        {1, 14, 0, 15},
        "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":14,"
        "\"initial_pipeline_state_object_id\":0}"));
    append(root_unmapped_writer, call(
        sequence++,
        "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView",
        {15},
        "{\"root_parameter_index\":0,\"buffer_location\":65536,"
        "\"gpuva_resolve_status\":\"unmapped\",\"resolved_resource_object_id\":0,"
        "\"resolved_resource_offset\":0,\"resolved_resource_width\":0}"));
    append(root_unmapped_writer, call(sequence++, "ID3D12GraphicsCommandList::Close", {15}, "{}"));
    append(root_unmapped_writer, call(
        sequence++,
        "ID3D12CommandQueue::ExecuteCommandLists",
        {13, 15},
        "{\"command_list_count\":1}"));
    root_unmapped_writer.close();
  }
  apitrace::trace::TraceBundleReader root_unmapped_reader;
  if (!open_finalized_bundle(root_unmapped_reader, root_unmapped_bundle)) {
    std::cerr << "reader failed to reopen root gpuva unmapped bundle: " << root_unmapped_reader.last_error() << "\n";
    return 1;
  }
  apitrace::d3d12::D3D12ReplayBackend root_unmapped_backend;
  if (!replay_all_events(root_unmapped_reader, root_unmapped_backend)) {
    std::cerr << "root GPUVA unmapped bundle did not preserve diagnostics: "
              << root_unmapped_backend.last_error() << "\n";
    return 1;
  }
  const auto *root_unmapped_list = root_unmapped_backend.command_list_state(15);
  const auto root_unmapped_cbv = root_unmapped_list
                                     ? root_unmapped_list->graphics_root_constant_buffers.find(0)
                                     : decltype(root_unmapped_list->graphics_root_constant_buffers)::const_iterator{};
  if (!root_unmapped_list ||
      root_unmapped_cbv == root_unmapped_list->graphics_root_constant_buffers.end() ||
      root_unmapped_cbv->second.gpu_virtual_address != 65536 ||
      root_unmapped_cbv->second.resolved ||
      root_unmapped_cbv->second.resource_object_id != 0) {
    std::cerr << "root GPUVA unmapped diagnostic state was not preserved\n";
    return 1;
  }
  if (root_unmapped_backend.validate_native_replay_readiness() ||
      root_unmapped_backend.last_error().find("unresolved GPUVA binding") == std::string::npos) {
    std::cerr << "root GPUVA unmapped bundle was not rejected by native readiness: "
              << root_unmapped_backend.last_error() << "\n";
    return 1;
  }

  backend.shutdown();
  return 0;
}
