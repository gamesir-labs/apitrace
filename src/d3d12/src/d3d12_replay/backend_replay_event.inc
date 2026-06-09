bool D3D12ReplayBackend::replay_event(const trace::EventRecord &event)
{
  if (!initialized_) {
    last_error_ = "D3D12 replay backend was not initialized";
    return false;
  }

  if (event.callsite.sequence != 0 && event.callsite.sequence < last_sequence_) {
    last_error_ = record_prefix(event) + ": sequence number regressed";
    return false;
  }
  last_sequence_ = event.callsite.sequence;

  if (event.kind == trace::EventKind::Boundary) {
    switch (event.boundary) {
    case trace::BoundaryKind::Frame:
      ++frames_seen_;
      if (submissions_.has_open_batch()) {
        submissions_.end_batch();
      }
      return true;
    case trace::BoundaryKind::Present:
    {
      json boundary_payload;
      if (!payload_to_json(event, boundary_payload, last_error_)) {
        return false;
      }
      const auto frame_index = payload_u64(boundary_payload, "frame_index", presents_seen_);
      const auto semantic = present_semantics_.find(frame_index);
      (void)semantic;
      ++presents_seen_;
      submissions_.mark_present(event.callsite.sequence, frame_index);
      if (submissions_.has_open_batch()) {
        submissions_.end_batch();
      }
      return true;
    }
    case trace::BoundaryKind::Submit:
      if (submissions_.has_open_batch()) {
        submissions_.end_batch();
      }
      return true;
    case trace::BoundaryKind::CommandList:
    case trace::BoundaryKind::Fence:
    case trace::BoundaryKind::Barrier:
    case trace::BoundaryKind::DebugMarker:
      return true;
    }
  }

	  json payload;
	  if (!payload_to_json(event, payload, last_error_)) {
	    return false;
	  }
	  if (event.kind == trace::EventKind::Call &&
	      event.object_refs.empty() &&
	      event.blob_refs.empty() &&
	      payload.empty() &&
	      event.callsite.function_name.rfind("ID3D12", 0) == 0) {
	    ++commands_replayed_;
	    return true;
	  }

	  if (event.kind == trace::EventKind::ObjectCreate) {
    if (event.object_id == 0) {
      last_error_ = record_prefix(event) + ": object_create missing object_id";
      return false;
    }

    D3D12TrackedObject tracked;
    tracked.object_id = event.object_id;
    tracked.kind = event.object_kind;
    tracked.parent_object_id = event.parent_object_id;
    tracked.debug_name = event.object_debug_name;
    objects_.track(tracked);
	    if (event.object_kind == trace::ObjectKind::CommandList) {
	      command_lists_.emplace(event.object_id, CommandListSemanticState{});
	    } else if (event.object_kind == trace::ObjectKind::RootSignature && payload.value("external_root_signature", false)) {
	      RootSignatureSemanticState root_signature;
	      root_signature.root_signature_object_id = event.object_id;
	      root_signature.create_sequence = event.callsite.sequence;
	      root_signatures_[root_signature.root_signature_object_id] = std::move(root_signature);
	    } else if (event.object_kind == trace::ObjectKind::Fence && payload.value("external_fence", false)) {
	      FenceSemanticState fence;
	      fence.fence_object_id = event.object_id;
	      fence.create_sequence = event.callsite.sequence;
	      fence.initial_value = payload_u64(payload, "initial_value");
	      fence.current_value = fence.initial_value;
	      fence.completed_value = fence.initial_value;
	      fence.flags = payload_u32(payload, "flags");
	      fences_[fence.fence_object_id] = fence;
	    } else if (event.object_kind == trace::ObjectKind::Resource && payload.value("external_resource", false)) {
	      if (!require_payload_key(event, payload, "resource_desc", last_error_)) {
	        return false;
	      }
	      const auto &resource_desc = payload["resource_desc"];
	      if (!resource_desc.is_object()) {
	        last_error_ = record_prefix(event) + ": resource_desc must be an object";
	        return false;
	      }
	      static constexpr const char *required_resource_desc_keys[] = {
	          "dimension",
	          "alignment",
	          "width",
	          "height",
	          "depth_or_array_size",
	          "mip_levels",
	          "format",
	          "sample_count",
	          "sample_quality",
	          "layout",
	          "flags",
	      };
	      for (const char *key : required_resource_desc_keys) {
	        if (!require_payload_key(event, resource_desc, key, last_error_)) {
	          return false;
	        }
	      }
	      ResourceSemanticState resource;
	      resource.resource_object_id = event.object_id;
	      resource.create_sequence = event.callsite.sequence;
	      resource.initial_state = 0;
	      resource.current_state = 0;
	      resource.width = payload_u64(resource_desc, "width");
	      resource.alignment = payload_u64(resource_desc, "alignment");
	      resource.dimension = payload_u32(resource_desc, "dimension");
	      resource.height = payload_u32(resource_desc, "height");
	      resource.depth_or_array_size = payload_u32(resource_desc, "depth_or_array_size");
	      resource.mip_levels = payload_u32(resource_desc, "mip_levels");
	      resource.format = payload_u32(resource_desc, "format");
	      resource.sample_count = payload_u32(resource_desc, "sample_count");
	      resource.sample_quality = payload_u32(resource_desc, "sample_quality");
	      resource.layout = payload_u32(resource_desc, "layout");
	      resource.flags = payload_u32(resource_desc, "flags");
	      if (resource.width == 0 || resource.height == 0 || resource.depth_or_array_size == 0 ||
	          resource.mip_levels == 0 || resource.sample_count == 0) {
	        last_error_ = record_prefix(event) + ": resource_desc has invalid dimensions or sample count";
	        return false;
	      }
	      resources_[resource.resource_object_id] = std::move(resource);
	    }
    ++commands_replayed_;
    return true;
  }

  if (event.kind == trace::EventKind::ObjectDestroy) {
    if (event.object_id != 0) {
      objects_.forget(event.object_id);
      command_lists_.erase(event.object_id);
      resources_.erase(event.object_id);
      command_signatures_.erase(event.object_id);
      command_queues_.erase(event.object_id);
      command_allocators_.erase(event.object_id);
      devices_.erase(event.object_id);
      heaps_.erase(event.object_id);
      query_heaps_.erase(event.object_id);
    }
    ++commands_replayed_;
    return true;
  }

  if (event.kind == trace::EventKind::ResourceBlob) {
    if (!index_present_frame_event(event, false)) {
      return false;
    }
    ++commands_replayed_;
    return true;
  }

  if (!validate_object_refs(event, objects_, last_error_)) {
    return false;
  }

  if (!is_supported_d3d12_call(event.callsite.function_name)) {
    last_error_ = record_prefix(event) + ": unsupported D3D12 call";
    return false;
  }

  const auto &function_name = event.callsite.function_name;
  const auto command_list_id = event.object_refs.empty() ? trace::ObjectId{} : event.object_refs.front();
  auto command_list_it = command_lists_.find(command_list_id);
  auto command_list_state = command_list_it == command_lists_.end() ? nullptr : &command_list_it->second;
  const auto replay_command_kind = replay_command_kind_for(function_name);
  const bool is_command_list_command =
      command_list_state != nullptr && replay_command_kind != ReplayCommandKind::Unknown;
  if (is_command_list_command && replay_command_kind != ReplayCommandKind::BeginCommandList &&
      !command_list_state->recording) {
    last_error_ = record_prefix(event) + ": command-list call outside a recording interval";
    return false;
  }
  if (replay_command_kind != ReplayCommandKind::Unknown) {
    ReplayCommandRecord command;
    command.kind = replay_command_kind;
    command.sequence = event.callsite.sequence;
    command.command_list_object_id = command_list_state ? command_list_id : trace::ObjectId{};
    if (function_name == "ID3D12Device::CreateCommandList" && !event.object_refs.empty()) {
      command.command_list_object_id = event.object_refs.back();
    }
    command.object_refs = event.object_refs;
    command.function_name = function_name;
    command.payload = event.payload;
    replay_commands_.push_back(std::move(command));
    if (command_list_state) {
      command_list_state->command_indices.push_back(replay_commands_.size() - 1);
    }
  }

  const auto ingest_descriptor_payload = [&](const trace::EventRecord &source_event,
                                           const json &descriptor_payload,
                                           std::string function_name,
                                           trace::ObjectId resource_object_id,
                                           trace::ObjectId counter_resource_object_id,
                                           std::uint64_t create_sequence) -> bool {
    if (!require_payload_key(source_event, descriptor_payload, "descriptor", last_error_)) {
      return false;
    }
    DescriptorSemanticState descriptor;
    std::string descriptor_kind;
    if (function_name == "ID3D12Device::CreateConstantBufferView") {
      descriptor_kind = "ConstantBufferView";
    } else if (function_name == "ID3D12Device::CreateShaderResourceView") {
      descriptor_kind = "ShaderResourceView";
    } else if (function_name == "ID3D12Device::CreateUnorderedAccessView") {
      descriptor_kind = "UnorderedAccessView";
    } else if (function_name == "ID3D12Device::CreateRenderTargetView") {
      descriptor_kind = "RenderTargetView";
    } else if (function_name == "ID3D12Device::CreateDepthStencilView") {
      descriptor_kind = "DepthStencilView";
    } else {
      last_error_ = record_prefix(source_event) + ": descriptor batch op has unsupported function";
      return false;
    }
    descriptor.kind = std::move(descriptor_kind);
    descriptor.create_sequence = create_sequence;
    descriptor.descriptor = payload_u64(descriptor_payload, "descriptor");
    descriptor.resource_object_id = resource_object_id;
    descriptor.counter_resource_object_id = counter_resource_object_id;
    if (!resolve_descriptor_binding_at(descriptor.descriptor, false, create_sequence, descriptor.binding, last_error_)) {
      last_error_ = record_prefix(source_event) + ": " + last_error_;
      return false;
    }
    descriptor.format = payload_u32(descriptor_payload, "format");
    descriptor.view_dimension = payload_u32(descriptor_payload, "view_dimension");
    descriptor.flags = payload_u32(descriptor_payload, "flags");
    descriptor.shader_4_component_mapping = payload_u32(descriptor_payload, "shader_4_component_mapping");
    descriptor.buffer_location = payload_u64(descriptor_payload, "buffer_location");
    descriptor.size_in_bytes = payload_u32(descriptor_payload, "size_in_bytes");
    if (function_name == "ID3D12Device::CreateConstantBufferView" && descriptor.buffer_location != 0) {
      if (!resolve_gpu_virtual_address_at(descriptor.buffer_location, create_sequence, descriptor.buffer, last_error_)) {
        last_error_ = record_prefix(source_event) + ": " + last_error_;
        return false;
      }
      if (!validate_gpu_virtual_address_resolve_payload(source_event, "descriptor buffer_location", descriptor_payload, descriptor.buffer, last_error_)) {
        return false;
      }
    }
    const auto view = descriptor_payload.find("view");
    if (view != descriptor_payload.end()) {
      if (!view->is_null() && !view->is_object()) {
        last_error_ = record_prefix(source_event) + ": descriptor view payload must be an object";
        return false;
      }
      if (view->is_object()) {
        descriptor.has_view_desc = true;
        descriptor.first_element = payload_u64(*view, "first_element");
        descriptor.num_elements = payload_u32(*view, "num_elements");
        descriptor.structure_byte_stride = payload_u32(*view, "structure_byte_stride");
        descriptor.counter_offset_in_bytes = payload_u64(*view, "counter_offset_in_bytes");
        descriptor.flags = payload_u32(*view, "flags", descriptor.flags);
        descriptor.most_detailed_mip = payload_u32(*view, "most_detailed_mip");
        descriptor.mip_levels = payload_u32(*view, "mip_levels");
        descriptor.mip_slice = payload_u32(*view, "mip_slice");
        descriptor.plane_slice = payload_u32(*view, "plane_slice");
        descriptor.first_array_slice = payload_u32(*view, "first_array_slice");
        descriptor.array_size = payload_u32(*view, "array_size");
        descriptor.first_2d_array_face = payload_u32(*view, "first_2d_array_face");
        descriptor.num_cubes = payload_u32(*view, "num_cubes");
        descriptor.first_w_slice = payload_u32(*view, "first_w_slice");
        descriptor.w_size = payload_u32(*view, "w_size");
        descriptor.resource_min_lod_clamp = payload_float(*view, "resource_min_lod_clamp");
        if (payload_has(*view, "location")) {
          if (!resolve_gpu_virtual_address_at(payload_u64(*view, "location"), create_sequence, descriptor.raytracing_acceleration_structure, last_error_)) {
            last_error_ = record_prefix(source_event) + ": " + last_error_;
            return false;
          }
          if (!validate_gpu_virtual_address_resolve_payload(
                  event,
                  "descriptor view.location",
                  *view,
                  descriptor.raytracing_acceleration_structure,
                  last_error_)) {
            return false;
          }
        }
      }
    }
	    if (descriptor.resource_object_id != 0) {
	      auto resource_it = resources_.find(descriptor.resource_object_id);
	      if (resource_it == resources_.end()) {
	        last_error_ = record_prefix(source_event) + ": descriptor references an unknown resource";
	        return false;
	      }
	      auto &resource = resource_it->second;
	      const auto resolved_width = payload_u64(descriptor_payload, "resolved_resource_width");
	      if (resolved_width > resource.width) {
	        resource.width = resolved_width;
	      }
	      if (function_name == "ID3D12Device::CreateConstantBufferView" && descriptor.buffer.resolved) {
	        if (descriptor.buffer.offset > resource.width ||
            descriptor.size_in_bytes > resource.width - descriptor.buffer.offset) {
          last_error_ = record_prefix(source_event) + ": ConstantBufferView buffer range is out of bounds";
          return false;
        }
      }
    if ((function_name == "ID3D12Device::CreateShaderResourceView" ||
           function_name == "ID3D12Device::CreateUnorderedAccessView") &&
          descriptor.has_view_desc &&
          descriptor.view_dimension == 1) {
        const auto stride = descriptor.structure_byte_stride != 0
                                ? descriptor.structure_byte_stride
                                : dxgi_format_element_size(descriptor.format);
        if (stride == 0) {
          last_error_ = record_prefix(source_event) + ": descriptor buffer view has unknown element size";
          return false;
        }
        const auto view_bytes = descriptor.num_elements == 0
                                    ? 0
                                    : (descriptor.num_elements - 1ull) * stride + stride;
        const auto view_offset = descriptor.first_element * static_cast<std::uint64_t>(stride);
        if (view_offset > resource.width ||
            view_bytes > resource.width - view_offset) {
          last_error_ = record_prefix(source_event) + ": descriptor buffer view range is out of bounds";
          return false;
        }
      }
      const auto require_texture_dimension = [&](std::uint32_t expected_dimension) -> bool {
        if (resource.dimension != expected_dimension) {
          last_error_ = record_prefix(source_event) + ": descriptor texture view dimension does not match resource";
          return false;
        }
        return true;
      };
      const auto require_mip_slice = [&](std::uint32_t mip) -> bool {
        if (mip >= resource.mip_levels) {
          last_error_ = record_prefix(source_event) + ": descriptor texture view mip range is out of bounds";
          return false;
        }
        return true;
      };
      const auto require_mip_range = [&](std::uint32_t first_mip, std::uint32_t mip_count) -> bool {
        constexpr std::uint32_t all_remaining = std::numeric_limits<std::uint32_t>::max();
        if (first_mip >= resource.mip_levels ||
            (mip_count != all_remaining &&
             (mip_count == 0 || mip_count > resource.mip_levels - first_mip))) {
          last_error_ = record_prefix(source_event) + ": descriptor texture view mip range is out of bounds";
          return false;
        }
        return true;
      };
      const auto require_array_range = [&](std::uint32_t first_slice, std::uint32_t slice_count) -> bool {
        constexpr std::uint32_t all_remaining = std::numeric_limits<std::uint32_t>::max();
        if (first_slice >= resource.depth_or_array_size ||
            (slice_count != all_remaining &&
             (slice_count == 0 || slice_count > resource.depth_or_array_size - first_slice))) {
          last_error_ = record_prefix(source_event) + ": descriptor texture view array range is out of bounds";
          return false;
        }
        return true;
      };
      const auto require_w_range = [&](std::uint32_t first_slice, std::uint32_t slice_count) -> bool {
        return require_array_range(first_slice, slice_count);
      };
      if (function_name == "ID3D12Device::CreateShaderResourceView" && descriptor.has_view_desc) {
        switch (descriptor.view_dimension) {
        case 2:
          if (!require_texture_dimension(2) ||
              !require_mip_range(descriptor.most_detailed_mip, descriptor.mip_levels)) {
            return false;
          }
          break;
        case 3:
          if (!require_texture_dimension(2) ||
              !require_mip_range(descriptor.most_detailed_mip, descriptor.mip_levels) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 4:
          if (!require_texture_dimension(3) ||
              !require_mip_range(descriptor.most_detailed_mip, descriptor.mip_levels)) {
            return false;
          }
          break;
        case 5:
          if (!require_texture_dimension(3) ||
              !require_mip_range(descriptor.most_detailed_mip, descriptor.mip_levels) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 6:
          if (!require_texture_dimension(3)) {
            return false;
          }
          break;
        case 7:
          if (!require_texture_dimension(3) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 8:
          if (!require_texture_dimension(4) ||
              !require_mip_range(descriptor.most_detailed_mip, descriptor.mip_levels)) {
            return false;
          }
          break;
        case 9:
          if (!require_texture_dimension(3) ||
              !require_mip_range(descriptor.most_detailed_mip, descriptor.mip_levels) ||
              resource.depth_or_array_size < 6) {
            last_error_ = record_prefix(source_event) + ": descriptor texture view array range is out of bounds";
            return false;
          }
          break;
        case 10:
          if (!require_texture_dimension(3) ||
              !require_mip_range(descriptor.most_detailed_mip, descriptor.mip_levels) ||
              descriptor.num_cubes == 0 ||
              descriptor.num_cubes > (std::numeric_limits<std::uint32_t>::max() / 6) ||
              !require_array_range(descriptor.first_2d_array_face, descriptor.num_cubes * 6)) {
            return false;
          }
          break;
        default:
          break;
        }
      }
      if (function_name == "ID3D12Device::CreateUnorderedAccessView" && descriptor.has_view_desc) {
        switch (descriptor.view_dimension) {
        case 2:
          if (!require_texture_dimension(2) || !require_mip_slice(descriptor.mip_slice)) {
            return false;
          }
          break;
        case 3:
          if (!require_texture_dimension(2) ||
              !require_mip_slice(descriptor.mip_slice) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 4:
          if (!require_texture_dimension(3) || !require_mip_slice(descriptor.mip_slice)) {
            return false;
          }
          break;
        case 5:
          if (!require_texture_dimension(3) ||
              !require_mip_slice(descriptor.mip_slice) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 8:
          if (!require_texture_dimension(4) ||
              !require_mip_slice(descriptor.mip_slice) ||
              !require_w_range(descriptor.first_w_slice, descriptor.w_size)) {
            return false;
          }
          break;
        default:
          break;
        }
      }
      if (function_name == "ID3D12Device::CreateRenderTargetView" && descriptor.has_view_desc) {
        switch (descriptor.view_dimension) {
        case 2:
          if (!require_texture_dimension(2) || !require_mip_slice(descriptor.mip_slice)) {
            return false;
          }
          break;
        case 3:
          if (!require_texture_dimension(2) ||
              !require_mip_slice(descriptor.mip_slice) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 4:
          if (!require_texture_dimension(3) || !require_mip_slice(descriptor.mip_slice)) {
            return false;
          }
          break;
        case 5:
          if (!require_texture_dimension(3) ||
              !require_mip_slice(descriptor.mip_slice) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 6:
          if (!require_texture_dimension(3)) {
            return false;
          }
          break;
        case 7:
          if (!require_texture_dimension(3) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 8:
          if (!require_texture_dimension(4) ||
              !require_mip_slice(descriptor.mip_slice) ||
              !require_w_range(descriptor.first_w_slice, descriptor.w_size)) {
            return false;
          }
          break;
        default:
          break;
        }
      }
      if (function_name == "ID3D12Device::CreateDepthStencilView" && descriptor.has_view_desc) {
        switch (descriptor.view_dimension) {
        case 1:
          if (!require_texture_dimension(2) || !require_mip_slice(descriptor.mip_slice)) {
            return false;
          }
          break;
        case 2:
          if (!require_texture_dimension(2) ||
              !require_mip_slice(descriptor.mip_slice) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 3:
          if (!require_texture_dimension(3) || !require_mip_slice(descriptor.mip_slice)) {
            return false;
          }
          break;
        case 4:
          if (!require_texture_dimension(3) ||
              !require_mip_slice(descriptor.mip_slice) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        case 5:
          if (!require_texture_dimension(3)) {
            return false;
          }
          break;
        case 6:
          if (!require_texture_dimension(3) ||
              !require_array_range(descriptor.first_array_slice, descriptor.array_size)) {
            return false;
          }
          break;
        default:
          break;
        }
      }
    }
    descriptors_.push_back(std::move(descriptor));
    ++semantic_calls_seen_;
    return true;
  };

  const auto descriptor_function_from_kind = [&](std::uint32_t kind) -> std::string {
    switch (kind) {
    case 1:
      return "ID3D12Device::CreateConstantBufferView";
    case 2:
      return "ID3D12Device::CreateShaderResourceView";
    case 3:
      return "ID3D12Device::CreateUnorderedAccessView";
    case 4:
      return "ID3D12Device::CreateRenderTargetView";
    case 5:
      return "ID3D12Device::CreateDepthStencilView";
    default:
      return {};
    }
  };

  if (function_name == "D3D12CreateDevice") {
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      last_error_ = record_prefix(event) + ": D3D12CreateDevice missing device object id";
      return false;
    }
    if (!object_ref_kind_matches(objects_, event.object_refs.front(), trace::ObjectKind::Device)) {
      last_error_ = record_prefix(event) + ": D3D12CreateDevice output object is not a device";
      return false;
    }
    if (event.callsite.result_code < 0) {
      last_error_ = record_prefix(event) + ": captured D3D12CreateDevice failed";
      return false;
    }
    DeviceSemanticState device;
    device.device_object_id = event.object_refs.front();
    device.create_sequence = event.callsite.sequence;
    device.minimum_feature_level = payload_u32(payload, "minimum_feature_level", 0xb000u);
    devices_[device.device_object_id] = device;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
      function_name == "ID3D12Device::CreateComputePipelineState" ||
      function_name == "ID3D12Device2::CreatePipelineState") {
    const auto pipeline_state_object_id = event.object_refs.size() >= 2 ? event.object_refs.back() : trace::ObjectId{};
    if (pipeline_state_object_id == 0) {
      last_error_ = record_prefix(event) + ": pipeline create missing pipeline state object id";
      return false;
    }
    const auto pipeline_path = payload_string(payload, "pipeline_path");
    if (pipeline_path.empty()) {
      if (payload.value("pso_raw_version", 0) != 0) {
        last_error_ = record_prefix(event) +
            ": pipeline create has raw PSO evidence but no pipeline_path; run bundle-finalize or inspect incomplete D3D12 pipeline semantic rebuild";
      } else {
        last_error_ = record_prefix(event) + ": pipeline create missing pipeline_path";
      }
      return false;
    }
    json pipeline_asset;
    if (!read_pipeline_asset_json(bundle_root_, pipeline_path, &pipeline_asset, last_error_)) {
      return false;
    }
    std::vector<std::filesystem::path> required_pipeline_paths;
    required_pipeline_paths.emplace_back(pipeline_path);
    std::vector<std::filesystem::path> shader_paths;
    if (!collect_pipeline_shader_asset_paths(pipeline_asset, shader_paths, last_error_)) {
      last_error_ = record_prefix(event) + ": " + last_error_;
      return false;
    }
    required_pipeline_paths.insert(required_pipeline_paths.end(), shader_paths.begin(), shader_paths.end());
    if (!verify_event_blob_refs_cover_paths(event, required_pipeline_paths, blob_paths_, last_error_)) {
      return false;
    }
    PipelineSemanticState pipeline;
    pipeline.pipeline_state_object_id = pipeline_state_object_id;
    pipeline.create_sequence = event.callsite.sequence;
    const auto pipeline_type = payload_string(pipeline_asset, "type");
    pipeline.graphics = function_name == "ID3D12Device::CreateGraphicsPipelineState" ||
                        (function_name == "ID3D12Device2::CreatePipelineState" &&
                         pipeline_type != "compute");
    const bool pipeline_requires_dxmt_backend =
        payload.value("requires_dxmt_backend", false) ||
        pipeline_type == "mesh" ||
        pipeline_asset.contains("as") ||
        pipeline_asset.contains("ms");
    pipeline.relative_path = pipeline_path;
    pipeline.blob_refs = event.blob_refs;
    pipeline.root_signature_object_id = json_object_id(pipeline_asset.value("root_signature_object_id", json(nullptr)));
    pipeline.uses_embedded_root_signature =
        pipeline.root_signature_object_id == 0 &&
        payload_string(pipeline_asset, "root_signature_source") == "embedded";
    if (pipeline.root_signature_object_id != 0 && root_signatures_.find(pipeline.root_signature_object_id) == root_signatures_.end()) {
      last_error_ = record_prefix(event) + ": pipeline asset references an unknown root signature";
      return false;
    }
    pipeline.node_mask = payload_u32(pipeline_asset, "node_mask");
    pipeline.flags = payload_u32(pipeline_asset, "flags");
    pipeline.sample_mask = payload_u32(pipeline_asset, "sample_mask");
    pipeline.primitive_topology_type = payload_u32(pipeline_asset, "primitive_topology_type");
    pipeline.num_render_targets = payload_u32(pipeline_asset, "num_render_targets");
    pipeline.dsv_format = payload_u32(pipeline_asset, "dsv_format");
    if (const auto sample_desc = pipeline_asset.find("sample_desc"); sample_desc != pipeline_asset.end()) {
      if (!sample_desc->is_object()) {
        last_error_ = record_prefix(event) + ": pipeline sample_desc must be an object";
        return false;
      }
      pipeline.sample_count = payload_u32(*sample_desc, "count");
      pipeline.sample_quality = payload_u32(*sample_desc, "quality");
      if (pipeline.sample_count == 0) {
        last_error_ = record_prefix(event) + ": pipeline sample_desc count must be non-zero";
        return false;
      }
    }
    pipeline.ib_strip_cut_value = payload_u32(pipeline_asset, "ib_strip_cut_value");
    if (pipeline.graphics) {
      if (pipeline_asset.find("sample_desc") == pipeline_asset.end()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing sample_desc";
        return false;
      }
      const auto input_layout = pipeline_asset.find("input_layout");
      const auto blend_state = pipeline_asset.find("blend_state");
      const auto rasterizer_state = pipeline_asset.find("rasterizer_state");
      const auto depth_stencil_state = pipeline_asset.find("depth_stencil_state");
      if (input_layout == pipeline_asset.end() || !input_layout->is_object()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing input_layout";
        return false;
      }
      if (blend_state == pipeline_asset.end() || !blend_state->is_object()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing blend_state";
        return false;
      }
      if (rasterizer_state == pipeline_asset.end() || !rasterizer_state->is_object()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing rasterizer_state";
        return false;
      }
      if (depth_stencil_state == pipeline_asset.end() || !depth_stencil_state->is_object()) {
        last_error_ = record_prefix(event) + ": graphics pipeline asset missing depth_stencil_state";
        return false;
      }
      pipeline.has_input_layout = true;
      pipeline.has_blend_state = true;
      pipeline.has_rasterizer_state = true;
      pipeline.has_depth_stencil_state = true;
      std::string pipeline_metadata_error;
      if (!pipeline_requires_dxmt_backend &&
          !validate_graphics_pipeline_asset_metadata(pipeline_asset, pipeline_metadata_error)) {
        last_error_ = record_prefix(event) + ": " + pipeline_metadata_error;
        return false;
      }
      pipeline.input_element_count = payload_u32(*input_layout, "element_count");
      const auto elements = input_layout->find("elements");
      if (elements == input_layout->end() || !elements->is_array() || elements->size() != pipeline.input_element_count) {
        last_error_ = record_prefix(event) + ": graphics pipeline input layout element count mismatch";
        return false;
      }
    }
    const auto rtv_formats = pipeline_asset.find("rtv_formats");
    if (pipeline.graphics && (rtv_formats == pipeline_asset.end() || !rtv_formats->is_array())) {
      last_error_ = record_prefix(event) + ": graphics pipeline asset missing rtv_formats";
      return false;
    }
    if (rtv_formats != pipeline_asset.end()) {
      if (!rtv_formats->is_array()) {
        last_error_ = record_prefix(event) + ": pipeline rtv_formats must be an array";
        return false;
      }
      for (const auto &format : *rtv_formats) {
        pipeline.rtv_formats.push_back(static_cast<std::uint32_t>(json_u64(format)));
      }
      if (pipeline.graphics && pipeline.rtv_formats.size() < pipeline.num_render_targets) {
        last_error_ = record_prefix(event) + ": graphics pipeline rtv_formats shorter than num_render_targets";
        return false;
      }
    }
    pipeline.has_vertex_shader = pipeline_asset.contains("vs");
    pipeline.has_pixel_shader = pipeline_asset.contains("ps");
    pipeline.has_domain_shader = pipeline_asset.contains("ds");
    pipeline.has_hull_shader = pipeline_asset.contains("hs");
    pipeline.has_geometry_shader = pipeline_asset.contains("gs");
    pipeline.has_compute_shader = pipeline_asset.contains("cs");
    pipeline.requires_dxmt_backend = pipeline_requires_dxmt_backend;
    if (pipeline.graphics && !pipeline.requires_dxmt_backend && !pipeline.has_vertex_shader) {
      last_error_ = record_prefix(event) + ": graphics pipeline asset missing vertex shader entry";
      return false;
    }
    if (pipeline.graphics && pipeline.root_signature_object_id == 0 && !pipeline.uses_embedded_root_signature) {
      last_error_ = record_prefix(event) + ": graphics pipeline asset missing root signature object id";
      return false;
    }
    if (!pipeline.graphics && !pipeline.has_compute_shader) {
      last_error_ = record_prefix(event) + ": compute pipeline asset missing compute shader entry";
      return false;
    }
    if (!pipeline.graphics && pipeline.root_signature_object_id == 0 && !pipeline.uses_embedded_root_signature) {
      last_error_ = record_prefix(event) + ": compute pipeline asset missing root signature object id";
      return false;
    }
    pipelines_[pipeline.pipeline_state_object_id] = std::move(pipeline);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateRootSignature") {
    if (event.callsite.result_code < 0) {
      ++semantic_calls_seen_;
      return true;
    }
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateRootSignature missing root signature object id";
      return false;
    }
    if (!require_payload_key(event, payload, "node_mask", last_error_) ||
        !require_payload_key(event, payload, "bytecode_size", last_error_)) {
      return false;
    }
    RootSignatureSemanticState root_signature;
    root_signature.root_signature_object_id = event.object_refs[1];
    root_signature.create_sequence = event.callsite.sequence;
    root_signature.node_mask = payload_u32(payload, "node_mask");
    root_signature.bytecode_size = payload_u64(payload, "bytecode_size");
    root_signature.blob_refs = event.blob_refs;
    const auto root_signature_path = payload_string(payload, "root_signature_path");
    const auto descriptor_tables = payload.find("descriptor_tables");
    if (descriptor_tables != payload.end()) {
      if (!descriptor_tables->is_array()) {
        last_error_ = record_prefix(event) + ": CreateRootSignature descriptor_tables must be an array";
        return false;
      }
      root_signature.has_descriptor_table_metadata = true;
      for (const auto &table_payload : *descriptor_tables) {
        if (!table_payload.is_object()) {
          last_error_ = record_prefix(event) + ": CreateRootSignature descriptor table entries must be objects";
          return false;
        }
        RootDescriptorTableSemanticState table;
        table.root_parameter_index = payload_u32(table_payload, "root_parameter_index");
        table.shader_visibility = payload_u32(table_payload, "shader_visibility");
        const auto ranges = table_payload.find("ranges");
        if (ranges == table_payload.end() || !ranges->is_array()) {
          last_error_ = record_prefix(event) + ": CreateRootSignature descriptor table missing ranges";
          return false;
        }
        for (const auto &range_payload : *ranges) {
          if (!range_payload.is_object()) {
            last_error_ = record_prefix(event) + ": CreateRootSignature descriptor range entries must be objects";
            return false;
          }
          RootDescriptorRangeSemanticState range;
          range.type = payload_u32(range_payload, "type");
          range.descriptor_count = payload_u32(range_payload, "descriptor_count");
          range.base_shader_register = payload_u32(range_payload, "base_shader_register");
          range.register_space = payload_u32(range_payload, "register_space");
          range.offset_from_table_start = payload_u32(range_payload, "offset_from_table_start");
          range.flags = payload_u32(range_payload, "flags");
          if (range.descriptor_count == 0) {
            last_error_ = record_prefix(event) + ": CreateRootSignature descriptor range has zero descriptors";
            return false;
          }
          table.ranges.push_back(range);
        }
        root_signature.descriptor_tables.push_back(std::move(table));
      }
    }
    const auto root_parameters = payload.find("root_parameters");
    if (root_parameters != payload.end()) {
      if (!root_parameters->is_array()) {
        last_error_ = record_prefix(event) + ": CreateRootSignature root_parameters must be an array";
        return false;
      }
      root_signature.has_root_parameter_metadata = true;
      for (const auto &parameter_payload : *root_parameters) {
        if (!parameter_payload.is_object()) {
          last_error_ = record_prefix(event) + ": CreateRootSignature root parameter entries must be objects";
          return false;
        }
        RootParameterSemanticState parameter;
        parameter.root_parameter_index = payload_u32(parameter_payload, "root_parameter_index");
        parameter.parameter_type = payload_u32(parameter_payload, "parameter_type");
        parameter.shader_visibility = payload_u32(parameter_payload, "shader_visibility");
        parameter.shader_register = payload_u32(parameter_payload, "shader_register");
        parameter.register_space = payload_u32(parameter_payload, "register_space");
        parameter.num_32bit_values = payload_u32(parameter_payload, "num_32bit_values");
        parameter.range_count = payload_u32(parameter_payload, "range_count");
        parameter.flags = payload_u32(parameter_payload, "flags");
        if (parameter.parameter_type > 4) {
          last_error_ = record_prefix(event) + ": CreateRootSignature root parameter has unknown type";
          return false;
        }
        root_signature.root_parameters.push_back(parameter);
      }
    }
    if (!root_signature_path.empty()) {
      root_signature.relative_path = root_signature_path;
      const auto absolute_root_signature_path = bundle_root_ / root_signature.relative_path;
      if (!std::filesystem::is_regular_file(absolute_root_signature_path)) {
        last_error_ = record_prefix(event) + ": missing root signature asset " + root_signature_path;
        return false;
      }
      if (event.blob_refs.empty()) {
        last_error_ = record_prefix(event) + ": root signature asset is missing blob_refs";
        return false;
      }
      if (!verify_event_blob_refs_cover_paths(event, {root_signature.relative_path}, blob_paths_, last_error_)) {
        return false;
      }
      std::string asset_error;
      if (!read_exact_asset_bytes(
              absolute_root_signature_path,
              root_signature.bytecode_size,
              root_signature.bytes,
              asset_error)) {
        last_error_ = record_prefix(event) + ": " + asset_error;
        return false;
      }
    } else if (root_signature.bytecode_size != 0) {
      last_error_ = record_prefix(event) + ": CreateRootSignature missing root_signature_path";
      return false;
    }
    root_signatures_[root_signature.root_signature_object_id] = std::move(root_signature);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandQueue" ||
             function_name == "ID3D12Device9::CreateCommandQueue1") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandQueue missing queue object id";
      return false;
    }
    if (!object_ref_kind_matches(objects_, event.object_refs[1], trace::ObjectKind::CommandQueue)) {
      last_error_ = record_prefix(event) + ": CreateCommandQueue output object is not a command queue";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }
    CommandQueueSemanticState queue;
    queue.queue_object_id = event.object_refs[1];
    queue.create_sequence = event.callsite.sequence;
    queue.type = payload_u32(payload, "type");
    queue.priority = payload_i32(payload, "priority");
    queue.flags = payload_u32(payload, "flags");
    queue.node_mask = payload_u32(payload, "node_mask");
    command_queues_[queue.queue_object_id] = queue;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandAllocator") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandAllocator missing allocator object id";
      return false;
    }
    if (!object_ref_kind_matches(objects_, event.object_refs[1], trace::ObjectKind::CommandAllocator)) {
      last_error_ = record_prefix(event) + ": CreateCommandAllocator output object is not a command allocator";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_)) {
      return false;
    }
    CommandAllocatorSemanticState allocator;
    allocator.allocator_object_id = event.object_refs[1];
    allocator.create_sequence = event.callsite.sequence;
    allocator.type = payload_u32(payload, "type");
    command_allocators_[allocator.allocator_object_id] = allocator;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateFence") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateFence missing fence object id";
      return false;
    }
    if (!require_payload_key(event, payload, "initial_value", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }
    FenceSemanticState fence;
    fence.fence_object_id = event.object_refs[1];
    fence.create_sequence = event.callsite.sequence;
    fence.initial_value = payload_u64(payload, "initial_value");
    fence.current_value = fence.initial_value;
    fence.completed_value = fence.initial_value;
    fence.flags = payload_u32(payload, "flags");
    fences_[fence.fence_object_id] = fence;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device6::SetBackgroundProcessingMode" ||
             function_name == "ID3D12Device7::AddToStateObject" ||
             function_name == "ID3D12Device7::CreateProtectedResourceSession1" ||
             function_name == "ID3D12Device9::CreateShaderCacheSession" ||
             function_name == "ID3D12Device9::ShaderCacheControl") {
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommittedResource" ||
             function_name == "ID3D12Device8::CreateCommittedResource2") {
    if (!require_payload_key(event, payload, "resource_desc", last_error_) ||
        !require_payload_key(event, payload, "initial_state", last_error_)) {
      return false;
    }
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateCommittedResource missing resource object id";
      return false;
    }
    const auto &resource_desc = payload["resource_desc"];
    if (!resource_desc.is_object()) {
      last_error_ = record_prefix(event) + ": resource_desc must be an object";
      return false;
    }
    static constexpr const char *required_resource_desc_keys[] = {
        "dimension",
        "alignment",
        "width",
        "height",
        "depth_or_array_size",
        "mip_levels",
        "format",
        "sample_count",
        "sample_quality",
        "layout",
        "flags",
    };
    for (const char *key : required_resource_desc_keys) {
      if (!require_payload_key(event, resource_desc, key, last_error_)) {
        return false;
      }
    }
    ResourceSemanticState resource;
    resource.resource_object_id = event.object_refs[1];
    resource.create_sequence = event.callsite.sequence;
    resource.heap_type = payload_u32(payload, "heap_type");
    resource.heap_flags = payload_u32(payload, "heap_flags");
    resource.initial_state = payload_u32(payload, "initial_state");
    resource.current_state = resource.initial_state;
    resource.gpu_virtual_address = payload_u64(payload, "gpu_virtual_address");
    resource.swapchain_back_buffer = payload.value("swapchain_back_buffer", false);
    resource.swapchain_buffer_index = payload_u32(payload, "buffer_index");
    resource.width = payload_u64(resource_desc, "width");
    resource.alignment = payload_u64(resource_desc, "alignment");
    resource.dimension = payload_u32(resource_desc, "dimension");
    resource.height = payload_u32(resource_desc, "height");
    resource.depth_or_array_size = payload_u32(resource_desc, "depth_or_array_size");
    resource.mip_levels = payload_u32(resource_desc, "mip_levels");
    resource.format = payload_u32(resource_desc, "format");
    resource.sample_count = payload_u32(resource_desc, "sample_count");
    resource.sample_quality = payload_u32(resource_desc, "sample_quality");
    resource.layout = payload_u32(resource_desc, "layout");
    resource.flags = payload_u32(resource_desc, "flags");
    if (resource.width == 0 || resource.height == 0 || resource.depth_or_array_size == 0 ||
        resource.mip_levels == 0 || resource.sample_count == 0) {
      last_error_ = record_prefix(event) + ": resource_desc has invalid dimensions or sample count";
      return false;
    }
    const auto optimized_clear_value = payload.find("optimized_clear_value");
    if (optimized_clear_value != payload.end()) {
      if (!optimized_clear_value->is_null() && !optimized_clear_value->is_object()) {
        last_error_ = record_prefix(event) + ": optimized_clear_value must be null or an object";
        return false;
      }
      if (optimized_clear_value->is_object()) {
        if (!require_payload_key(event, *optimized_clear_value, "format", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "color", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "depth", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "stencil", last_error_)) {
          return false;
        }
        const auto color = optimized_clear_value->find("color");
        if (!color->is_array() || color->size() != 4) {
          last_error_ = record_prefix(event) + ": optimized_clear_value color must be an array of four numbers";
          return false;
        }
        resource.has_optimized_clear_value = true;
        resource.optimized_clear_format = payload_u32(*optimized_clear_value, "format");
        for (std::size_t index = 0; index < 4; ++index) {
          if (!(*color)[index].is_number()) {
            last_error_ = record_prefix(event) + ": optimized_clear_value color entries must be numeric";
            return false;
          }
          resource.optimized_clear_color[index] = (*color)[index].get<float>();
        }
        resource.optimized_clear_depth = payload_float(*optimized_clear_value, "depth");
        resource.optimized_clear_stencil = payload_u32(*optimized_clear_value, "stencil");
      }
    }
    resources_[resource.resource_object_id] = std::move(resource);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateReservedResource") {
    if (!require_payload_key(event, payload, "resource_desc", last_error_) ||
        !require_payload_key(event, payload, "initial_state", last_error_)) {
      return false;
    }
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateReservedResource missing resource object id";
      return false;
    }
    const auto &resource_desc = payload["resource_desc"];
    if (!resource_desc.is_object()) {
      last_error_ = record_prefix(event) + ": resource_desc must be an object";
      return false;
    }
    static constexpr const char *required_resource_desc_keys[] = {
        "dimension",
        "alignment",
        "width",
        "height",
        "depth_or_array_size",
        "mip_levels",
        "format",
        "sample_count",
        "sample_quality",
        "layout",
        "flags",
    };
    for (const char *key : required_resource_desc_keys) {
      if (!require_payload_key(event, resource_desc, key, last_error_)) {
        return false;
      }
    }
    ResourceSemanticState resource;
    resource.resource_object_id = event.object_refs[1];
    resource.create_sequence = event.callsite.sequence;
    resource.initial_state = payload_u32(payload, "initial_state");
    resource.current_state = resource.initial_state;
    resource.gpu_virtual_address = payload_u64(payload, "gpu_virtual_address");
    resource.reserved_resource = payload.value("reserved_resource", true);
    resource.width = payload_u64(resource_desc, "width");
    resource.alignment = payload_u64(resource_desc, "alignment");
    resource.dimension = payload_u32(resource_desc, "dimension");
    resource.height = payload_u32(resource_desc, "height");
    resource.depth_or_array_size = payload_u32(resource_desc, "depth_or_array_size");
    resource.mip_levels = payload_u32(resource_desc, "mip_levels");
    resource.format = payload_u32(resource_desc, "format");
    resource.sample_count = payload_u32(resource_desc, "sample_count");
    resource.sample_quality = payload_u32(resource_desc, "sample_quality");
    resource.layout = payload_u32(resource_desc, "layout");
    resource.flags = payload_u32(resource_desc, "flags");
    if (resource.width == 0 || resource.height == 0 || resource.depth_or_array_size == 0 ||
        resource.mip_levels == 0 || resource.sample_count == 0) {
      last_error_ = record_prefix(event) + ": resource_desc has invalid dimensions or sample count";
      return false;
    }
    const auto optimized_clear_value = payload.find("optimized_clear_value");
    if (optimized_clear_value != payload.end()) {
      if (!optimized_clear_value->is_null() && !optimized_clear_value->is_object()) {
        last_error_ = record_prefix(event) + ": optimized_clear_value must be null or an object";
        return false;
      }
      if (optimized_clear_value->is_object()) {
        if (!require_payload_key(event, *optimized_clear_value, "format", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "color", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "depth", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "stencil", last_error_)) {
          return false;
        }
        const auto color = optimized_clear_value->find("color");
        if (!color->is_array() || color->size() != 4) {
          last_error_ = record_prefix(event) + ": optimized_clear_value color must be an array of four numbers";
          return false;
        }
        resource.has_optimized_clear_value = true;
        resource.optimized_clear_format = payload_u32(*optimized_clear_value, "format");
        for (std::size_t index = 0; index < 4; ++index) {
          if (!(*color)[index].is_number()) {
            last_error_ = record_prefix(event) + ": optimized_clear_value color entries must be numeric";
            return false;
          }
          resource.optimized_clear_color[index] = (*color)[index].get<float>();
        }
        resource.optimized_clear_depth = payload_float(*optimized_clear_value, "depth");
        resource.optimized_clear_stencil = payload_u32(*optimized_clear_value, "stencil");
      }
    }
    resources_[resource.resource_object_id] = std::move(resource);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateHeap" ||
             function_name == "ID3D12Device3::OpenExistingHeapFromAddress" ||
             function_name == "ID3D12Device4::CreateHeap1") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateHeap missing heap object id";
      return false;
    }
    if (!require_payload_key(event, payload, "size_in_bytes", last_error_) ||
        !require_payload_key(event, payload, "heap_type", last_error_)) {
      return false;
    }
    HeapSemanticState heap;
    heap.heap_object_id = event.object_refs[1];
    heap.create_sequence = event.callsite.sequence;
    heap.size_in_bytes = payload_u64(payload, "size_in_bytes");
    heap.alignment = payload_u64(payload, "alignment");
    heap.heap_type = payload_u32(payload, "heap_type");
    heap.cpu_page_property = payload.contains("cpu_page_property") ? payload_u32(payload, "cpu_page_property") : 0;
    heap.memory_pool_preference = payload.contains("memory_pool_preference") ? payload_u32(payload, "memory_pool_preference") : 0;
    heap.creation_node_mask = payload.contains("creation_node_mask") ? payload_u32(payload, "creation_node_mask") : 1;
    heap.visible_node_mask = payload.contains("visible_node_mask") ? payload_u32(payload, "visible_node_mask") : 1;
    heap.flags = payload_u32(payload, "flags");
    if (heap.size_in_bytes == 0) {
      last_error_ = record_prefix(event) + ": CreateHeap size_in_bytes must be non-zero";
      return false;
    }
    heaps_[heap.heap_object_id] = heap;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreatePlacedResource" ||
             function_name == "ID3D12Device8::CreatePlacedResource1") {
    if (!require_payload_key(event, payload, "resource_desc", last_error_) ||
        !require_payload_key(event, payload, "heap_object_id", last_error_) ||
        !require_payload_key(event, payload, "initial_state", last_error_)) {
      return false;
    }
    if (event.object_refs.size() < 3 || event.object_refs[1] == 0 || event.object_refs[2] == 0) {
      last_error_ = record_prefix(event) + ": CreatePlacedResource missing heap or resource object id";
      return false;
    }
    const auto heap_object_id = event.object_refs[1];
    if (heaps_.find(heap_object_id) == heaps_.end()) {
      last_error_ = record_prefix(event) + ": CreatePlacedResource references an unknown heap";
      return false;
    }
    const auto &resource_desc = payload["resource_desc"];
    if (!resource_desc.is_object()) {
      last_error_ = record_prefix(event) + ": resource_desc must be an object";
      return false;
    }
    static constexpr const char *required_resource_desc_keys[] = {
        "dimension",
        "alignment",
        "width",
        "height",
        "depth_or_array_size",
        "mip_levels",
        "format",
        "sample_count",
        "sample_quality",
        "layout",
        "flags",
    };
    for (const char *key : required_resource_desc_keys) {
      if (!require_payload_key(event, resource_desc, key, last_error_)) {
        return false;
      }
    }
    ResourceSemanticState resource;
    resource.resource_object_id = event.object_refs[2];
    resource.heap_object_id = heap_object_id;
    resource.create_sequence = event.callsite.sequence;
    resource.heap_offset = payload_u64(payload, "heap_offset");
    resource.heap_type = heaps_[heap_object_id].heap_type;
    resource.heap_flags = heaps_[heap_object_id].flags;
    resource.initial_state = payload_u32(payload, "initial_state");
    resource.current_state = resource.initial_state;
    resource.gpu_virtual_address = payload_u64(payload, "gpu_virtual_address");
    resource.width = payload_u64(resource_desc, "width");
    resource.alignment = payload_u64(resource_desc, "alignment");
    resource.dimension = payload_u32(resource_desc, "dimension");
    resource.height = payload_u32(resource_desc, "height");
    resource.depth_or_array_size = payload_u32(resource_desc, "depth_or_array_size");
    resource.mip_levels = payload_u32(resource_desc, "mip_levels");
    resource.format = payload_u32(resource_desc, "format");
    resource.sample_count = payload_u32(resource_desc, "sample_count");
    resource.sample_quality = payload_u32(resource_desc, "sample_quality");
    resource.layout = payload_u32(resource_desc, "layout");
    resource.flags = payload_u32(resource_desc, "flags");
    if (resource.width == 0 || resource.height == 0 || resource.depth_or_array_size == 0 ||
        resource.mip_levels == 0 || resource.sample_count == 0) {
      last_error_ = record_prefix(event) + ": resource_desc has invalid dimensions or sample count";
      return false;
    }
    const auto optimized_clear_value = payload.find("optimized_clear_value");
    if (optimized_clear_value != payload.end()) {
      if (!optimized_clear_value->is_null() && !optimized_clear_value->is_object()) {
        last_error_ = record_prefix(event) + ": optimized_clear_value must be null or an object";
        return false;
      }
      if (optimized_clear_value->is_object()) {
        if (!require_payload_key(event, *optimized_clear_value, "format", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "color", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "depth", last_error_) ||
            !require_payload_key(event, *optimized_clear_value, "stencil", last_error_)) {
          return false;
        }
        const auto color = optimized_clear_value->find("color");
        if (!color->is_array() || color->size() != 4) {
          last_error_ = record_prefix(event) + ": optimized_clear_value color must be an array of four numbers";
          return false;
        }
        resource.has_optimized_clear_value = true;
        resource.optimized_clear_format = payload_u32(*optimized_clear_value, "format");
        for (std::size_t index = 0; index < 4; ++index) {
          if (!(*color)[index].is_number()) {
            last_error_ = record_prefix(event) + ": optimized_clear_value color entries must be numeric";
            return false;
          }
          resource.optimized_clear_color[index] = (*color)[index].get<float>();
        }
        resource.optimized_clear_depth = payload_float(*optimized_clear_value, "depth");
        resource.optimized_clear_stencil = payload_u32(*optimized_clear_value, "stencil");
      }
    }
    resources_[resource.resource_object_id] = std::move(resource);
    ++semantic_calls_seen_;

  } else if (function_name == "ID3D12Device::CreateConstantBufferView" ||
             function_name == "ID3D12Device::CreateShaderResourceView" ||
             function_name == "ID3D12Device::CreateUnorderedAccessView" ||
             function_name == "ID3D12Device::CreateRenderTargetView" ||
             function_name == "ID3D12Device::CreateDepthStencilView") {
    const auto resource_object_id = event.object_refs.size() >= 2 ? event.object_refs[1] : trace::ObjectId{};
    const auto counter_resource_object_id = event.object_refs.size() >= 3 ? event.object_refs[2] : trace::ObjectId{};
    if (!ingest_descriptor_payload(event, payload, function_name, resource_object_id,
                                   counter_resource_object_id, event.callsite.sequence)) {
      return false;
    }
  } else if (function_name == "ID3D12Device::CreateDescriptorViewBatch") {
    const auto ops = payload.find("ops");
    if (ops == payload.end() || !ops->is_array()) {
      last_error_ = record_prefix(event) + ": descriptor batch ops must be an array";
      return false;
    }
    const bool schema_v2 = payload_string(payload, "schema") == "descriptor-view-v2";
    if (schema_v2) {
      for (const auto &op : *ops) {
        if (!op.is_array() || op.size() < 16) {
          last_error_ = record_prefix(event) + ": descriptor batch v2 op must be a 16-column array";
          return false;
        }
        const auto create_sequence = json_u64(op[0], event.callsite.sequence);
        const auto op_function = descriptor_function_from_kind(static_cast<std::uint32_t>(json_u64(op[1])));
        json descriptor_payload = json::object();
        descriptor_payload["descriptor"] = json_u64(op[2]);
        descriptor_payload["format"] = json_u64(op[5]);
        descriptor_payload["view_dimension"] = json_u64(op[6]);
        descriptor_payload["shader_4_component_mapping"] = json_u64(op[7]);
        descriptor_payload["flags"] = json_u64(op[8]);
        descriptor_payload["buffer_location"] = json_u64(op[9]);
        descriptor_payload["size_in_bytes"] = json_u64(op[10]);
        if (op[11].is_string() && !op[11].get<std::string>().empty()) {
          descriptor_payload["gpuva_resolve_status"] = op[11];
        }
        descriptor_payload["resolved_resource_object_id"] = json_u64(op[12]);
        descriptor_payload["resolved_resource_offset"] = json_u64(op[13]);
        descriptor_payload["resolved_resource_width"] = json_u64(op[14]);
        if (!op[15].is_null()) {
          descriptor_payload["view"] = op[15];
        }
        const auto resource_object_id = static_cast<trace::ObjectId>(json_u64(op[3]));
        const auto counter_resource_object_id = static_cast<trace::ObjectId>(json_u64(op[4]));
        if (!ingest_descriptor_payload(event, descriptor_payload, op_function, resource_object_id,
                                       counter_resource_object_id, create_sequence)) {
          return false;
        }
      }
    } else {
      for (const auto &op : *ops) {
        if (!op.is_object()) {
          last_error_ = record_prefix(event) + ": descriptor batch op must be an object";
          return false;
        }
        std::string op_function = payload_string(op, "function");
        if (op_function.empty()) {
          const auto op_kind = payload_string(op, "kind");
          if (op_kind == "ConstantBufferView") {
            op_function = "ID3D12Device::CreateConstantBufferView";
          } else if (op_kind == "ShaderResourceView") {
            op_function = "ID3D12Device::CreateShaderResourceView";
          } else if (op_kind == "UnorderedAccessView") {
            op_function = "ID3D12Device::CreateUnorderedAccessView";
          } else if (op_kind == "RenderTargetView") {
            op_function = "ID3D12Device::CreateRenderTargetView";
          } else if (op_kind == "DepthStencilView") {
            op_function = "ID3D12Device::CreateDepthStencilView";
          }
        }
        const auto create_sequence = payload_u64(op, "sequence", event.callsite.sequence);
        const auto resource_object_id = static_cast<trace::ObjectId>(payload_u64(op, "resource_object_id"));
        const auto counter_resource_object_id = static_cast<trace::ObjectId>(payload_u64(op, "counter_resource_object_id"));
        if (!ingest_descriptor_payload(event, op, op_function, resource_object_id,
                                       counter_resource_object_id, create_sequence)) {
          return false;
        }
      }
    }
  } else if (function_name == "ID3D12Device::CreateSampler") {
    if (!require_payload_key(event, payload, "descriptor", last_error_)) {
      return false;
    }
    SamplerSemanticState sampler;
    sampler.create_sequence = event.callsite.sequence;
    sampler.descriptor = payload_u64(payload, "descriptor");
    if (!resolve_descriptor_binding_at(sampler.descriptor, false, event.callsite.sequence, sampler.binding, last_error_)) {
      last_error_ = record_prefix(event) + ": " + last_error_;
      return false;
    }
    sampler.has_desc = payload_has(payload, "filter");
    sampler.filter = payload_u32(payload, "filter");
    sampler.address_u = payload_u32(payload, "address_u");
    sampler.address_v = payload_u32(payload, "address_v");
    sampler.address_w = payload_u32(payload, "address_w");
    sampler.mip_lod_bias = payload_float(payload, "mip_lod_bias");
    sampler.max_anisotropy = payload_u32(payload, "max_anisotropy");
    sampler.comparison_func = payload_u32(payload, "comparison_func");
    const auto border_color = payload.find("border_color");
    if (border_color != payload.end()) {
      if (!border_color->is_array() || border_color->size() != 4) {
        last_error_ = record_prefix(event) + ": sampler border_color must be an array of four numbers";
        return false;
      }
      for (std::size_t index = 0; index < 4; ++index) {
        if (!(*border_color)[index].is_number()) {
          last_error_ = record_prefix(event) + ": sampler border_color entries must be numeric";
          return false;
        }
        sampler.border_color[index] = (*border_color)[index].get<float>();
      }
    }
    sampler.min_lod = payload_float(payload, "min_lod");
    sampler.max_lod = payload_float(payload, "max_lod");
    samplers_.push_back(std::move(sampler));
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateDescriptorHeap") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateDescriptorHeap missing heap object id";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_) ||
        !require_payload_key(event, payload, "num_descriptors", last_error_)) {
      return false;
    }
    DescriptorHeapSemanticState heap;
    heap.heap_object_id = event.object_refs[1];
    heap.create_sequence = event.callsite.sequence;
    heap.type = payload_u32(payload, "type");
    heap.num_descriptors = payload_u32(payload, "num_descriptors");
    heap.flags = payload_u32(payload, "flags");
    heap.node_mask = payload_u32(payload, "node_mask");
    heap.descriptor_size = payload_u32(payload, "descriptor_size");
    heap.cpu_start = payload_u64(payload, "cpu_start");
    heap.gpu_start = payload_u64(payload, "gpu_start");
    if (heap.num_descriptors == 0 || heap.descriptor_size == 0 || heap.cpu_start == 0) {
      last_error_ = record_prefix(event) + ": descriptor heap relocation metadata is incomplete";
      return false;
    }
    const bool shader_visible = descriptor_heap_type_is_shader_visible(heap.flags);
    if (shader_visible && !descriptor_heap_type_allows_shader_visibility(heap.type)) {
      last_error_ = record_prefix(event) + ": descriptor heap type cannot be shader-visible";
      return false;
    }
    if (shader_visible && heap.gpu_start == 0) {
      last_error_ = record_prefix(event) + ": shader-visible descriptor heap missing GPU start handle";
      return false;
    }
    if (!shader_visible && heap.gpu_start != 0) {
      last_error_ = record_prefix(event) + ": non shader-visible descriptor heap has GPU start handle";
      return false;
    }
    descriptor_heaps_[heap.heap_object_id] = std::move(heap);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateQueryHeap") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": CreateQueryHeap missing query heap object id";
      return false;
    }
    if (!object_ref_kind_matches(objects_, event.object_refs[1], trace::ObjectKind::QueryHeap)) {
      last_error_ = record_prefix(event) + ": CreateQueryHeap output object is not a query heap";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_) ||
        !require_payload_key(event, payload, "count", last_error_)) {
      return false;
    }
    QueryHeapSemanticState heap;
    heap.query_heap_object_id = event.object_refs[1];
    heap.create_sequence = event.callsite.sequence;
    heap.type = payload_u32(payload, "type");
    heap.count = payload_u32(payload, "count");
    heap.node_mask = payload_u32(payload, "node_mask");
    if (heap.count == 0) {
      last_error_ = record_prefix(event) + ": CreateQueryHeap count must be non-zero";
      return false;
    }
    query_heaps_[heap.query_heap_object_id] = std::move(heap);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CopyDescriptorsBatch") {
    const auto ops = payload.find("ops");
    if (ops == payload.end() || !ops->is_array()) {
      last_error_ = record_prefix(event) + ": CopyDescriptorsBatch ops must be an array";
      return false;
    }
    for (const auto &op : *ops) {
      if (!op.is_array() || op.size() < 6) {
        last_error_ = record_prefix(event) + ": CopyDescriptorsBatch op must be a 6-column array";
        return false;
      }
      CopyDescriptorSemanticState copy;
      copy.create_sequence = json_u64(op[0], event.callsite.sequence);
      copy.descriptor_heap_type = static_cast<std::uint32_t>(json_u64(op[1]));
      const auto descriptor_size = json_u64(op[2]);
      if (descriptor_size == 0) {
        last_error_ = record_prefix(event) + ": CopyDescriptorsBatch descriptor size must be non-zero";
        return false;
      }
      const auto &ranges = op[5];
      if (!ranges.is_array()) {
        last_error_ = record_prefix(event) + ": CopyDescriptorsBatch ranges must be an array";
        return false;
      }
      for (const auto &range : ranges) {
        if (!range.is_array() || range.size() < 3) {
          last_error_ = record_prefix(event) + ": CopyDescriptorsBatch range must be a 3-column array";
          return false;
        }
        const auto dst_base = json_u64(range[0]);
        const auto src_base = json_u64(range[1]);
        const auto count = json_u64(range[2]);
        if (dst_base == 0 || src_base == 0 || count == 0) {
          last_error_ = record_prefix(event) + ": CopyDescriptorsBatch range handles and count must be non-zero";
          return false;
        }
        for (std::uint64_t index = 0; index < count; ++index) {
          CopyDescriptorPair pair;
          pair.dst_descriptor = dst_base + index * descriptor_size;
          pair.src_descriptor = src_base + index * descriptor_size;
          DescriptorBinding ignored_binding;
          if (!resolve_descriptor_binding_at(pair.dst_descriptor, false, copy.create_sequence, ignored_binding, last_error_) ||
              !resolve_descriptor_binding_at(pair.src_descriptor, false, copy.create_sequence, ignored_binding, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          copy.descriptors.push_back(pair);
        }
      }
      descriptor_copies_.push_back(std::move(copy));
      ++semantic_calls_seen_;
    }
  } else if (function_name == "ID3D12Device::CopyDescriptors" ||
             function_name == "ID3D12Device::CopyDescriptorsSimple") {
    if (!require_payload_key(event, payload, "descriptor_heap_type", last_error_) ||
        !require_payload_key(event, payload, "descriptors", last_error_)) {
      return false;
    }
    const auto descriptors = payload.find("descriptors");
    if (descriptors == payload.end() || !descriptors->is_array()) {
      last_error_ = record_prefix(event) + ": CopyDescriptors descriptors must be an array";
      return false;
    }
    CopyDescriptorSemanticState copy;
    copy.create_sequence = event.callsite.sequence;
    copy.descriptor_heap_type = payload_u32(payload, "descriptor_heap_type");
    for (const auto &descriptor : *descriptors) {
      if (!descriptor.is_object()) {
        last_error_ = record_prefix(event) + ": CopyDescriptors descriptor entries must be objects";
        return false;
      }
      CopyDescriptorPair pair;
      pair.dst_descriptor = payload_u64(descriptor, "dst_descriptor");
      pair.src_descriptor = payload_u64(descriptor, "src_descriptor");
      if (pair.dst_descriptor == 0 || pair.src_descriptor == 0) {
        last_error_ = record_prefix(event) + ": CopyDescriptors descriptor handles must be non-zero";
        return false;
      }
      DescriptorBinding ignored_binding;
      if (!resolve_descriptor_binding_at(pair.dst_descriptor, false, event.callsite.sequence, ignored_binding, last_error_) ||
          !resolve_descriptor_binding_at(pair.src_descriptor, false, event.callsite.sequence, ignored_binding, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      copy.descriptors.push_back(pair);
    }
    descriptor_copies_.push_back(std::move(copy));
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandSignature") {
    if (event.object_refs.size() < 2 || event.object_refs.back() == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandSignature missing command signature object id";
      return false;
    }
    if (!require_payload_key(event, payload, "byte_stride", last_error_) ||
        !require_payload_key(event, payload, "argument_count", last_error_)) {
      return false;
    }
    CommandSignatureSemanticState signature;
    signature.command_signature_object_id = event.object_refs.back();
    signature.root_signature_object_id = event.object_refs.size() >= 3 ? event.object_refs[1] : trace::ObjectId{};
    signature.create_sequence = event.callsite.sequence;
    signature.byte_stride = payload_u32(payload, "byte_stride");
    signature.argument_count = payload_u32(payload, "argument_count");
    signature.node_mask = payload_u32(payload, "node_mask");
    const auto arguments = payload.find("arguments");
    if (arguments != payload.end()) {
      if (!arguments->is_array()) {
        last_error_ = record_prefix(event) + ": command signature arguments must be an array";
        return false;
      }
      if (arguments->size() != signature.argument_count) {
        last_error_ = record_prefix(event) + ": command signature argument_count does not match arguments payload";
        return false;
      }
      for (const auto &argument_payload : *arguments) {
        if (!argument_payload.is_object()) {
          last_error_ = record_prefix(event) + ": command signature argument must be an object";
          return false;
        }
        if (!require_payload_key(event, argument_payload, "type", last_error_)) {
          return false;
        }
        CommandSignatureArgument argument;
        argument.type = payload_u32(argument_payload, "type");
        argument.slot = payload_u32(argument_payload, "slot");
        argument.root_parameter_index = payload_u32(argument_payload, "root_parameter_index");
        argument.dest_offset_in32bit_values = payload_u32(argument_payload, "dest_offset_in32bit_values");
        argument.num32bit_values_to_set = payload_u32(argument_payload, "num32bit_values_to_set");
        if (argument.type == 3 && !payload_has(argument_payload, "slot")) {
          last_error_ = record_prefix(event) + ": vertex-buffer indirect argument missing slot";
          return false;
        }
        if (argument.type == 5 &&
            (!payload_has(argument_payload, "root_parameter_index") ||
             !payload_has(argument_payload, "dest_offset_in32bit_values") ||
             !payload_has(argument_payload, "num32bit_values_to_set"))) {
          last_error_ = record_prefix(event) + ": constant indirect argument is incomplete";
          return false;
        }
        if (command_signature_argument_uses_root_parameter(argument.type) &&
            !payload_has(argument_payload, "root_parameter_index")) {
          last_error_ = record_prefix(event) + ": root descriptor indirect argument missing root_parameter_index";
          return false;
        }
        signature.arguments.push_back(argument);
      }
    } else if (signature.argument_count != 0) {
      last_error_ = record_prefix(event) + ": CreateCommandSignature missing arguments payload";
      return false;
    }
    command_signatures_[signature.command_signature_object_id] = std::move(signature);
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandList") {
    if (event.object_refs.empty() || event.object_refs.back() == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandList missing command list object id";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_)) {
      return false;
    }
    const auto allocator_object_id = payload_u64(
        payload,
        "command_allocator_object_id",
        event.object_refs.size() >= 3 ? event.object_refs[1] : trace::ObjectId{});
    if (allocator_object_id == 0 || command_allocators_.find(allocator_object_id) == command_allocators_.end()) {
      last_error_ = record_prefix(event) + ": CreateCommandList references an unknown command allocator";
      return false;
    }
    const auto initial_pipeline_state_object_id = payload_u64(
        payload,
        "initial_pipeline_state_object_id",
        event.object_refs.size() >= 4 ? event.object_refs[2] : trace::ObjectId{});
    if (initial_pipeline_state_object_id != 0 &&
        pipelines_.find(initial_pipeline_state_object_id) == pipelines_.end()) {
      last_error_ = record_prefix(event) + ": CreateCommandList references an unknown initial pipeline state";
      return false;
    }
    const auto command_list_object_id = event.object_refs.back();
    if (!object_ref_kind_matches(objects_, command_list_object_id, trace::ObjectKind::CommandList)) {
      last_error_ = record_prefix(event) + ": CreateCommandList output object is not a command list";
      return false;
    }
    const auto type = payload_u32(payload, "type");
    if (command_allocators_[allocator_object_id].type != type) {
      last_error_ = record_prefix(event) + ": CreateCommandList type does not match allocator type";
      return false;
    }
    auto &state = command_lists_[command_list_object_id];
    state = CommandListSemanticState{};
    state.recording = true;
    state.first_sequence = event.callsite.sequence;
    state.type = type;
    state.node_mask = payload_u32(payload, "node_mask");
    state.allocator_object_id = allocator_object_id;
    state.pipeline_state_object_id = initial_pipeline_state_object_id;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Device::CreateCommandList1") {
    if (event.object_refs.size() < 2 || event.object_refs.back() == 0) {
      last_error_ = record_prefix(event) + ": CreateCommandList1 missing command list object id";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_)) {
      return false;
    }
    const auto command_list_object_id = event.object_refs.back();
    if (!object_ref_kind_matches(objects_, command_list_object_id, trace::ObjectKind::CommandList)) {
      last_error_ = record_prefix(event) + ": CreateCommandList1 output object is not a command list";
      return false;
    }
    auto &state = command_lists_[command_list_object_id];
    state = CommandListSemanticState{};
    state.recording = false;
    state.first_sequence = event.callsite.sequence;
    state.type = payload_u32(payload, "type");
    state.node_mask = payload_u32(payload, "node_mask");
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::Reset") {
    if (!command_list_state) {
      last_error_ = record_prefix(event) + ": Reset references an unknown command list";
      return false;
    }
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": Reset missing command allocator object id";
      return false;
    }
    const auto allocator_it = command_allocators_.find(event.object_refs[1]);
    if (allocator_it == command_allocators_.end()) {
      last_error_ = record_prefix(event) + ": Reset references an unknown command allocator";
      return false;
    }
    const auto previous_type = command_list_state->type;
    if (previous_type != 0 && allocator_it->second.type != previous_type) {
      last_error_ = record_prefix(event) + ": Reset allocator type does not match command list type";
      return false;
    }
    const auto initial_pipeline_state_object_id = event.object_refs.size() >= 3 ? event.object_refs[2] : trace::ObjectId{};
    if (initial_pipeline_state_object_id != 0 &&
        pipelines_.find(initial_pipeline_state_object_id) == pipelines_.end()) {
      last_error_ = record_prefix(event) + ": Reset references an unknown initial pipeline state";
      return false;
    }
    *command_list_state = CommandListSemanticState{};
    command_list_state->recording = true;
    command_list_state->first_sequence = event.callsite.sequence;
    command_list_state->type = allocator_it->second.type;
    command_list_state->allocator_object_id = event.object_refs[1];
    command_list_state->pipeline_state_object_id = initial_pipeline_state_object_id;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::Close") {
    if (command_list_state) {
      command_list_state->recording = false;
      command_list_state->close_sequence = event.callsite.sequence;
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetPipelineState") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": SetPipelineState missing pipeline state object id";
      return false;
    }
    if (pipelines_.find(event.object_refs[1]) == pipelines_.end()) {
      last_error_ = record_prefix(event) + ": SetPipelineState references an unknown pipeline state";
      return false;
    }
    if (command_list_state) {
      command_list_state->pipeline_state_object_id = event.object_refs[1];
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootSignature") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": SetGraphicsRootSignature missing root signature object id";
      return false;
    }
    if (root_signatures_.find(event.object_refs[1]) == root_signatures_.end()) {
      last_error_ = record_prefix(event) + ": SetGraphicsRootSignature references an unknown root signature";
      return false;
    }
    if (command_list_state) {
      command_list_state->graphics_root_signature_object_id = event.object_refs[1];
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetComputeRootSignature") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": SetComputeRootSignature missing root signature object id";
      return false;
    }
    if (root_signatures_.find(event.object_refs[1]) == root_signatures_.end()) {
      last_error_ = record_prefix(event) + ": SetComputeRootSignature references an unknown root signature";
      return false;
    }
    if (command_list_state) {
      command_list_state->compute_root_signature_object_id = event.object_refs[1];
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable") {
    if (!require_payload_key(event, payload, "root_parameter_index", last_error_) ||
        !require_payload_key(event, payload, "base_descriptor", last_error_)) {
      return false;
    }
    if (command_list_state) {
      DescriptorBinding binding;
      const auto base_descriptor = payload_u64(payload, "base_descriptor");
      if (!resolve_descriptor_binding_in_heaps_at(
              base_descriptor,
              true,
              command_list_state->descriptor_heap_object_ids,
              event.callsite.sequence,
              binding,
              last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      if (binding.heap_object_id == 0) {
        last_error_ = record_prefix(event) + ": root descriptor table references a null descriptor";
        return false;
      }
      const auto root_parameter_index = payload_u32(payload, "root_parameter_index");
      if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable") {
        command_list_state->graphics_root_tables[root_parameter_index] = binding;
      } else {
        command_list_state->compute_root_tables[root_parameter_index] = binding;
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant" ||
             function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants") {
    if (!require_payload_key(event, payload, "root_parameter_index", last_error_) ||
        !require_payload_key(event, payload, "constant_count", last_error_) ||
        !require_payload_key(event, payload, "dst_offset", last_error_) ||
        !require_payload_key(event, payload, "values", last_error_)) {
      return false;
    }
    const auto &values_payload = payload["values"];
    if (!values_payload.is_array()) {
      last_error_ = record_prefix(event) + ": root 32-bit constants values must be an array";
      return false;
    }
    const auto constant_count = payload_u32(payload, "constant_count");
    if (values_payload.size() != constant_count) {
      last_error_ = record_prefix(event) + ": root 32-bit constant_count does not match values payload";
      return false;
    }
    Root32BitConstantsBinding binding;
    binding.root_parameter_index = payload_u32(payload, "root_parameter_index");
    binding.dst_offset = payload_u32(payload, "dst_offset");
    binding.values.reserve(values_payload.size());
    for (const auto &value : values_payload) {
      if (!value.is_number_unsigned() && !value.is_number_integer()) {
        last_error_ = record_prefix(event) + ": root 32-bit constant value must be an integer";
        return false;
      }
      const auto constant = json_u64(value);
      if (constant > std::numeric_limits<std::uint32_t>::max()) {
        last_error_ = record_prefix(event) + ": root 32-bit constant value exceeds uint32";
        return false;
      }
      binding.values.push_back(static_cast<std::uint32_t>(constant));
    }
    if (command_list_state) {
      if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant" ||
          function_name == "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants") {
        command_list_state->graphics_root_constants[binding.root_parameter_index] = std::move(binding);
      } else {
        command_list_state->compute_root_constants[binding.root_parameter_index] = std::move(binding);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView" ||
             function_name == "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView" ||
             function_name == "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView" ||
             function_name == "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView") {
    if (!require_payload_key(event, payload, "root_parameter_index", last_error_) ||
        !require_payload_key(event, payload, "buffer_location", last_error_)) {
      return false;
    }
    if (command_list_state) {
      GpuVirtualAddressBinding binding;
      if (!resolve_gpu_virtual_address_at(payload_u64(payload, "buffer_location"), event.callsite.sequence, binding, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      if (!validate_gpu_virtual_address_resolve_payload(event, "root buffer_location", payload, binding, last_error_)) {
        return false;
      }
      const auto root_parameter_index = payload_u32(payload, "root_parameter_index");
      if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView") {
        command_list_state->graphics_root_constant_buffers[root_parameter_index] = binding;
      } else if (function_name == "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView") {
        command_list_state->compute_root_constant_buffers[root_parameter_index] = binding;
      } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView") {
        command_list_state->graphics_root_shader_resources[root_parameter_index] = binding;
      } else if (function_name == "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView") {
        command_list_state->compute_root_shader_resources[root_parameter_index] = binding;
      } else if (function_name == "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView") {
        command_list_state->graphics_root_unordered_accesses[root_parameter_index] = binding;
      } else {
        command_list_state->compute_root_unordered_accesses[root_parameter_index] = binding;
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetDescriptorHeaps") {
    if (!require_payload_key(event, payload, "heap_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->descriptor_heap_count = payload_u64(payload, "heap_count");
      command_list_state->descriptor_heap_object_ids.clear();
      command_list_state->graphics_root_tables.clear();
      command_list_state->compute_root_tables.clear();
    }
    if (payload_u64(payload, "heap_count") != event.object_refs.size() - 1) {
      last_error_ = record_prefix(event) + ": SetDescriptorHeaps object refs do not match heap_count";
      return false;
    }
    for (std::size_t index = 1; index < event.object_refs.size(); ++index) {
      const auto heap_it = descriptor_heaps_.find(event.object_refs[index]);
      if (heap_it == descriptor_heaps_.end()) {
        last_error_ = record_prefix(event) + ": SetDescriptorHeaps references an unknown descriptor heap";
        return false;
      }
      if (!descriptor_heap_type_is_shader_visible(heap_it->second.flags)) {
        last_error_ = record_prefix(event) + ": SetDescriptorHeaps references a non shader-visible descriptor heap";
        return false;
      }
      for (std::size_t prior_index = 1; prior_index < index; ++prior_index) {
        const auto prior_heap_it = descriptor_heaps_.find(event.object_refs[prior_index]);
        if (prior_heap_it != descriptor_heaps_.end() && prior_heap_it->second.type == heap_it->second.type) {
          last_error_ = record_prefix(event) + ": SetDescriptorHeaps binds duplicate heap types";
          return false;
        }
      }
      if (command_list_state) {
        command_list_state->descriptor_heap_object_ids.push_back(event.object_refs[index]);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::RSSetViewports") {
    if (!require_payload_key(event, payload, "viewport_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->viewports.clear();
      const auto viewport_count = payload_u32(payload, "viewport_count");
      const auto viewports = payload.find("viewports");
      if (viewports != payload.end()) {
        if (!viewports->is_array() || viewports->size() != viewport_count) {
          last_error_ = record_prefix(event) + ": RSSetViewports viewports count does not match viewport_count";
          return false;
        }
        for (const auto &viewport_payload : *viewports) {
          if (!viewport_payload.is_object()) {
            last_error_ = record_prefix(event) + ": RSSetViewports viewport entries must be objects";
            return false;
          }
          ViewportSemanticState viewport;
          viewport.x = payload_float(viewport_payload, "x");
          viewport.y = payload_float(viewport_payload, "y");
          viewport.width = payload_float(viewport_payload, "width");
          viewport.height = payload_float(viewport_payload, "height");
          viewport.min_depth = payload_float(viewport_payload, "min_depth");
          viewport.max_depth = payload_float(viewport_payload, "max_depth");
          command_list_state->viewports.push_back(viewport);
        }
      } else if (viewport_count > 0) {
        const auto first = payload.find("first");
        if (first == payload.end() || !first->is_object()) {
          last_error_ = record_prefix(event) + ": RSSetViewports missing viewport payload";
          return false;
        }
        if (viewport_count > 1) {
          last_error_ = record_prefix(event) + ": RSSetViewports first viewport cannot cover multiple viewports";
          return false;
        }
        ViewportSemanticState viewport;
        viewport.x = payload_float(*first, "x");
        viewport.y = payload_float(*first, "y");
        viewport.width = payload_float(*first, "width");
        viewport.height = payload_float(*first, "height");
        viewport.min_depth = payload_float(*first, "min_depth");
        viewport.max_depth = payload_float(*first, "max_depth");
        command_list_state->viewports.push_back(viewport);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::RSSetScissorRects") {
    if (!require_payload_key(event, payload, "rect_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->scissor_rects.clear();
      const auto rect_count = payload_u32(payload, "rect_count");
      const auto rects = payload.find("rects");
      if (rects != payload.end()) {
        if (!rects->is_array() || rects->size() != rect_count) {
          last_error_ = record_prefix(event) + ": RSSetScissorRects rects count does not match rect_count";
          return false;
        }
        for (const auto &rect_payload : *rects) {
          if (!rect_payload.is_object()) {
            last_error_ = record_prefix(event) + ": RSSetScissorRects rect entries must be objects";
            return false;
          }
          ScissorRectSemanticState rect;
          rect.left = payload_i32(rect_payload, "left");
          rect.top = payload_i32(rect_payload, "top");
          rect.right = payload_i32(rect_payload, "right");
          rect.bottom = payload_i32(rect_payload, "bottom");
          command_list_state->scissor_rects.push_back(rect);
        }
      } else if (rect_count > 0) {
        const auto first = payload.find("first");
        if (first == payload.end() || !first->is_object()) {
          last_error_ = record_prefix(event) + ": RSSetScissorRects missing rect payload";
          return false;
        }
        if (rect_count > 1) {
          last_error_ = record_prefix(event) + ": RSSetScissorRects first rect cannot cover multiple rects";
          return false;
        }
        ScissorRectSemanticState rect;
        rect.left = payload_i32(*first, "left");
        rect.top = payload_i32(*first, "top");
        rect.right = payload_i32(*first, "right");
        rect.bottom = payload_i32(*first, "bottom");
        command_list_state->scissor_rects.push_back(rect);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::OMSetRenderTargets") {
    if (!require_payload_key(event, payload, "render_target_count", last_error_) ||
        !require_payload_key(event, payload, "dsv", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->render_target_count = payload_u64(payload, "render_target_count");
      command_list_state->render_targets.clear();
      const auto render_targets = payload.find("render_targets");
      if (render_targets != payload.end()) {
        if (!render_targets->is_array()) {
          last_error_ = record_prefix(event) + ": render_targets must be an array";
          return false;
        }
        const auto single_range =
            (payload_bool(payload, "single_handle_to_descriptor_range") || payload_bool(payload, "single_descriptor_handle")) &&
            render_targets->size() == 1 &&
            command_list_state->render_target_count > 1;
        if (!single_range && render_targets->size() != command_list_state->render_target_count) {
          last_error_ = record_prefix(event) + ": render_targets count does not match render_target_count";
          return false;
        }
        DescriptorBinding first_binding;
        std::uint64_t descriptor_size = 0;
        if (single_range) {
          if (!resolve_descriptor_binding_at(json_u64((*render_targets)[0]), false, event.callsite.sequence, first_binding, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          const auto heap_it = descriptor_heaps_.find(first_binding.heap_object_id);
          if (heap_it == descriptor_heaps_.end() || heap_it->second.descriptor_size == 0) {
            last_error_ = record_prefix(event) + ": OMSetRenderTargets first RTV cannot be expanded";
            return false;
          }
          descriptor_size = heap_it->second.descriptor_size;
        }
        for (std::uint64_t index = 0; index < command_list_state->render_target_count; ++index) {
          const auto descriptor = single_range
                                      ? first_binding.descriptor + index * descriptor_size
                                      : json_u64((*render_targets)[static_cast<std::size_t>(index)]);
          DescriptorBinding binding;
          if (!resolve_descriptor_binding_at(descriptor, false, event.callsite.sequence, binding, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          command_list_state->render_targets.push_back(binding);
        }
      } else if (command_list_state->render_target_count == 1 && payload_u64(payload, "first_rtv") != 0) {
        DescriptorBinding binding;
        if (!resolve_descriptor_binding_at(payload_u64(payload, "first_rtv"), false, event.callsite.sequence, binding, last_error_)) {
          last_error_ = record_prefix(event) + ": " + last_error_;
          return false;
        }
        command_list_state->render_targets.push_back(binding);
      } else if (command_list_state->render_target_count > 1) {
        last_error_ = record_prefix(event) + ": OMSetRenderTargets first RTV cannot cover multiple render targets";
        return false;
      } else if (command_list_state->render_target_count == 1) {
        last_error_ = record_prefix(event) + ": OMSetRenderTargets missing render target payload";
        return false;
      }
      const auto dsv = payload_u64(payload, "dsv");
      if (dsv != 0 && !resolve_descriptor_binding_at(dsv, false, event.callsite.sequence, command_list_state->depth_stencil, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList4::BeginRenderPass") {
    if (!require_payload_key(event, payload, "render_targets_count", last_error_) ||
        !require_payload_key(event, payload, "render_targets", last_error_) ||
        !require_payload_key(event, payload, "has_depth_stencil", last_error_) ||
        !require_payload_key(event, payload, "depth_stencil", last_error_) ||
        !require_payload_key(event, payload, "flags", last_error_)) {
      return false;
    }
    const auto &render_targets = payload["render_targets"];
    if (!render_targets.is_array()) {
      last_error_ = record_prefix(event) + ": render pass render_targets must be an array";
      return false;
    }
    if (render_targets.size() != payload_u64(payload, "render_targets_count")) {
      last_error_ = record_prefix(event) + ": render pass render_targets_count does not match render_targets payload";
      return false;
    }
    for (const auto &render_target : render_targets) {
      if (!render_target.is_object()) {
        last_error_ = record_prefix(event) + ": render pass render target must be an object";
        return false;
      }
      std::string render_pass_error;
      if (!validate_render_pass_beginning_access(render_target["beginning_access"], render_pass_error) ||
          !validate_render_pass_ending_access(render_target["ending_access"], resources_, render_pass_error)) {
        last_error_ = record_prefix(event) + ": " + render_pass_error;
        return false;
      }
    }
    const auto has_depth_stencil = payload_bool(payload, "has_depth_stencil");
    const auto &depth_stencil = payload["depth_stencil"];
    if (has_depth_stencil) {
      if (!depth_stencil.is_object()) {
        last_error_ = record_prefix(event) + ": render pass depth_stencil must be an object";
        return false;
      }
      std::string render_pass_error;
      if (!validate_render_pass_beginning_access(depth_stencil["depth_beginning_access"], render_pass_error) ||
          !validate_render_pass_beginning_access(depth_stencil["stencil_beginning_access"], render_pass_error) ||
          !validate_render_pass_ending_access(depth_stencil["depth_ending_access"], resources_, render_pass_error) ||
          !validate_render_pass_ending_access(depth_stencil["stencil_ending_access"], resources_, render_pass_error)) {
        last_error_ = record_prefix(event) + ": " + render_pass_error;
        return false;
      }
    } else if (!depth_stencil.is_null()) {
      last_error_ = record_prefix(event) + ": render pass depth_stencil must be null when has_depth_stencil is false";
      return false;
    }
    if (command_list_state) {
      command_list_state->render_target_count = render_targets.size();
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList4::EndRenderPass") {
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ClearRenderTargetView" ||
             function_name == "ID3D12GraphicsCommandList::ClearDepthStencilView") {
    if (!require_payload_key(event, payload, "descriptor", last_error_)) {
      return false;
    }
    if (!require_payload_key(event, payload, "rect_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      ++command_list_state->clear_count;
      if (function_name == "ID3D12GraphicsCommandList::ClearRenderTargetView") {
        RenderTargetClearSemanticState clear;
        clear.sequence = event.callsite.sequence;
        clear.command_list_object_id = command_list_id;
        clear.rect_count = payload_u32(payload, "rect_count");
        if (!resolve_descriptor_binding_at(payload_u64(payload, "descriptor"), false, event.callsite.sequence, clear.render_target, last_error_)) {
          last_error_ = record_prefix(event) + ": " + last_error_;
          return false;
        }
        const auto color = payload.find("color");
        if (color != payload.end()) {
          if (!color->is_array() || color->size() != 4) {
            last_error_ = record_prefix(event) + ": ClearRenderTargetView color must be an array of four numbers";
            return false;
          }
          for (std::size_t index = 0; index < 4; ++index) {
            if (!(*color)[index].is_number()) {
              last_error_ = record_prefix(event) + ": ClearRenderTargetView color entries must be numeric";
              return false;
            }
            clear.color[index] = (*color)[index].get<float>();
          }
          clear.has_color = true;
        }
        if (!parse_rect_array_payload(
                event,
                payload,
                "ClearRenderTargetView",
                clear.rect_count,
                clear.rects,
                last_error_)) {
          return false;
        }
        command_list_state->render_target_clears.push_back(clear);
      } else {
        if (!require_payload_key(event, payload, "clear_flags", last_error_) ||
            !require_payload_key(event, payload, "depth", last_error_) ||
            !require_payload_key(event, payload, "stencil", last_error_)) {
          return false;
        }
        DepthStencilClearSemanticState clear;
        clear.sequence = event.callsite.sequence;
        clear.command_list_object_id = command_list_id;
        clear.clear_flags = payload_u32(payload, "clear_flags");
        clear.depth = payload_float(payload, "depth");
        clear.stencil = payload_u32(payload, "stencil");
        clear.rect_count = payload_u32(payload, "rect_count");
        if (!resolve_descriptor_binding_at(payload_u64(payload, "descriptor"), false, event.callsite.sequence, clear.depth_stencil, last_error_)) {
          last_error_ = record_prefix(event) + ": " + last_error_;
          return false;
        }
        if (!parse_rect_array_payload(
                event,
                payload,
                "ClearDepthStencilView",
                clear.rect_count,
                clear.rects,
                last_error_)) {
          return false;
        }
        command_list_state->depth_stencil_clears.push_back(clear);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint" ||
             function_name == "ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat") {
    if (!require_payload_key(event, payload, "gpu_descriptor", last_error_) ||
        !require_payload_key(event, payload, "cpu_descriptor", last_error_) ||
        !require_payload_key(event, payload, "values", last_error_) ||
        !require_payload_key(event, payload, "rect_count", last_error_)) {
      return false;
    }
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0 ||
        resources_.find(event.object_refs[1]) == resources_.end()) {
      last_error_ = record_prefix(event) + ": ClearUnorderedAccessView references an unknown resource";
      return false;
    }
    const auto values = payload.find("values");
    if (values == payload.end() || !values->is_array() || values->size() != 4) {
      last_error_ = record_prefix(event) + ": ClearUnorderedAccessView values must be an array of four numbers";
      return false;
    }
    if (command_list_state) {
      ++command_list_state->clear_count;
      UnorderedAccessClearSemanticState clear;
      clear.sequence = event.callsite.sequence;
      clear.command_list_object_id = command_list_id;
      clear.resource_object_id = event.object_refs[1];
      clear.integer = function_name == "ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint";
      clear.rect_count = payload_u32(payload, "rect_count");
      if (!resolve_descriptor_binding_at(payload_u64(payload, "gpu_descriptor"), true, event.callsite.sequence, clear.gpu_descriptor, last_error_) ||
          !resolve_descriptor_binding_at(payload_u64(payload, "cpu_descriptor"), false, event.callsite.sequence, clear.cpu_descriptor, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      for (std::size_t index = 0; index < 4; ++index) {
        if (clear.integer) {
          if (!(*values)[index].is_number_unsigned() && !(*values)[index].is_number_integer()) {
            last_error_ = record_prefix(event) + ": ClearUnorderedAccessViewUint values must be integers";
            return false;
          }
          clear.uint_values[index] = (*values)[index].get<std::uint32_t>();
        } else {
          if (!(*values)[index].is_number()) {
            last_error_ = record_prefix(event) + ": ClearUnorderedAccessViewFloat values must be numeric";
            return false;
          }
          clear.float_values[index] = (*values)[index].get<float>();
        }
      }
      if (!parse_rect_array_payload(
              event,
              payload,
              "ClearUnorderedAccessView",
              clear.rect_count,
              clear.rects,
              last_error_)) {
        return false;
      }
      command_list_state->unordered_access_clears.push_back(clear);
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::DiscardResource") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0 ||
        resources_.find(event.object_refs[1]) == resources_.end()) {
      last_error_ = record_prefix(event) + ": DiscardResource references an unknown resource";
      return false;
    }
    if (!require_payload_key(event, payload, "rect_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      DiscardResourceSemanticState discard;
      discard.sequence = event.callsite.sequence;
      discard.command_list_object_id = command_list_id;
      discard.resource_object_id = event.object_refs[1];
      discard.first_subresource = payload_u32(payload, "first_subresource");
      discard.subresource_count = payload_u32(payload, "subresource_count");
      discard.rect_count = payload_u32(payload, "rect_count");
      discard.has_region = discard.subresource_count != 0 || discard.rect_count != 0;
      if (!parse_rect_array_payload(
              event,
              payload,
              "DiscardResource",
              discard.rect_count,
              discard.rects,
              last_error_)) {
        return false;
      }
      command_list_state->discards.push_back(discard);
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::IASetPrimitiveTopology") {
    if (!require_payload_key(event, payload, "primitive_topology", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->primitive_topology = payload_u32(payload, "primitive_topology");
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::IASetVertexBuffers") {
    if (!require_payload_key(event, payload, "start_slot", last_error_) ||
        !require_payload_key(event, payload, "view_count", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->vertex_buffers.clear();
      const auto view_count = payload_u64(payload, "view_count");
      const auto views = payload.find("views");
      if (views != payload.end()) {
        if (!views->is_array()) {
          last_error_ = record_prefix(event) + ": vertex buffer views must be an array";
          return false;
        }
        if (views->size() != view_count) {
          last_error_ = record_prefix(event) + ": vertex buffer views count does not match view_count";
          return false;
        }
        const auto start_slot = payload_u32(payload, "start_slot");
        for (std::size_t index = 0; index < views->size(); ++index) {
          const auto &view = (*views)[index];
          if (!view.is_object()) {
            last_error_ = record_prefix(event) + ": vertex buffer view must be an object";
            return false;
          }
          if (payload_u64(view, "buffer_location") == 0) {
            continue;
          }
          VertexBufferBinding binding;
          binding.sequence = event.callsite.sequence;
          binding.slot = start_slot + static_cast<std::uint32_t>(index);
          if (!resolve_gpu_virtual_address_at(payload_u64(view, "buffer_location"), event.callsite.sequence, binding.address, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          if (!validate_gpu_virtual_address_resolve_payload(event, "vertex buffer_location", view, binding.address, last_error_)) {
            return false;
          }
          binding.size_in_bytes = payload_u32(view, "size_in_bytes");
          binding.stride_in_bytes = payload_u32(view, "stride_in_bytes");
          command_list_state->vertex_buffers.push_back(binding);
        }
      } else {
        const auto &first_view = payload.find("first") != payload.end() ? payload["first"] : json{};
        if (view_count != 0 && !first_view.is_object()) {
          last_error_ = record_prefix(event) + ": IASetVertexBuffers missing view payload";
          return false;
        }
        if (view_count > 1) {
          last_error_ = record_prefix(event) + ": IASetVertexBuffers first view cannot cover multiple vertex buffers";
          return false;
        }
        if (first_view.is_object() && payload_u64(first_view, "buffer_location") != 0) {
          VertexBufferBinding binding;
          binding.sequence = event.callsite.sequence;
          binding.slot = payload_u32(payload, "start_slot");
          if (!resolve_gpu_virtual_address_at(payload_u64(first_view, "buffer_location"), event.callsite.sequence, binding.address, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          if (!validate_gpu_virtual_address_resolve_payload(event, "vertex buffer_location", first_view, binding.address, last_error_)) {
            return false;
          }
          binding.size_in_bytes = payload_u32(first_view, "size_in_bytes");
          binding.stride_in_bytes = payload_u32(first_view, "stride_in_bytes");
          command_list_state->vertex_buffers.push_back(binding);
        }
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::IASetIndexBuffer") {
    if (!payload.empty() && !require_payload_key(event, payload, "buffer_location", last_error_)) {
      return false;
    }
    if (command_list_state && payload_u64(payload, "buffer_location") != 0) {
      IndexBufferBinding binding;
      binding.sequence = event.callsite.sequence;
      if (!resolve_gpu_virtual_address_at(payload_u64(payload, "buffer_location"), event.callsite.sequence, binding.address, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      if (!validate_gpu_virtual_address_resolve_payload(event, "index buffer_location", payload, binding.address, last_error_)) {
        return false;
      }
      binding.size_in_bytes = payload_u32(payload, "size_in_bytes");
      binding.format = payload_u32(payload, "format");
      command_list_state->index_buffer = binding;
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ResourceBarrier" ||
             function_name == "ID3D12GraphicsCommandList::ResourceBarrierBatch") {
    if (!require_payload_key(event, payload, "barrier_count", last_error_) ||
        !require_payload_key(event, payload, "barriers", last_error_)) {
      return false;
    }
    if (command_list_state) {
      command_list_state->barrier_count += payload_u64(payload, "barrier_count");
    }
    const auto &barriers = payload["barriers"];
    if (!barriers.is_array()) {
      last_error_ = record_prefix(event) + ": barriers must be an array";
      return false;
    }
    const auto barrier_count = payload_u64(payload, "barrier_count");
    if (barriers.size() != barrier_count) {
      last_error_ = record_prefix(event) + ": barrier_count does not match barriers payload";
      return false;
    }
    for (const auto &barrier_entry : barriers) {
      json compact_barrier_storage;
      const json *barrier = &barrier_entry;
      if (barrier_entry.is_array()) {
        if (barrier_entry.size() < 9) {
          last_error_ = record_prefix(event) + ": ResourceBarrierBatch op must be a 9-column array";
          return false;
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
        barrier = &compact_barrier_storage;
      }
      if (!barrier->is_object()) {
        last_error_ = record_prefix(event) + ": barrier entry must be an object or compact array";
        return false;
      }
      const auto barrier_sequence = barrier_entry.is_array()
                                        ? json_u64(barrier_entry[0], event.callsite.sequence)
                                        : event.callsite.sequence;
      if (!require_payload_key(event, *barrier, "type", last_error_) ||
          !require_payload_key(event, *barrier, "flags", last_error_)) {
        return false;
      }
      const auto barrier_type = payload_u32(*barrier, "type");
      const auto flags = payload_u32(*barrier, "flags");
      ResourceBarrierSemanticState barrier_state;
      barrier_state.sequence = barrier_sequence;
      barrier_state.command_list_object_id = command_list_id;
      barrier_state.type = barrier_type;
      barrier_state.flags = flags;
      if (barrier_type == 1) {
        if (!require_payload_key(event, *barrier, "resource_before_object_id", last_error_) ||
            !require_payload_key(event, *barrier, "resource_after_object_id", last_error_)) {
          return false;
        }
        const auto before_id = json_object_id((*barrier)["resource_before_object_id"]);
        const auto after_id = json_object_id((*barrier)["resource_after_object_id"]);
        if (before_id != 0 && resources_.find(before_id) == resources_.end()) {
          last_error_ = record_prefix(event) + ": ResourceBarrier references an unknown aliasing before resource";
          return false;
        }
        if (after_id != 0 && resources_.find(after_id) == resources_.end()) {
          last_error_ = record_prefix(event) + ": ResourceBarrier references an unknown aliasing after resource";
          return false;
        }
        barrier_state.resource_before_object_id = before_id;
        barrier_state.resource_after_object_id = after_id;
        if (command_list_state) {
          command_list_state->barriers.push_back(barrier_state);
        }
        continue;
      }
      if (barrier_type == 2) {
        if (!require_payload_key(event, *barrier, "resource_object_id", last_error_)) {
          return false;
        }
        const auto resource_object_id = json_object_id((*barrier)["resource_object_id"]);
        if (resource_object_id != 0 && resources_.find(resource_object_id) == resources_.end()) {
          last_error_ = record_prefix(event) + ": ResourceBarrier references an unknown UAV resource";
          return false;
        }
        barrier_state.resource_object_id = resource_object_id;
        if (command_list_state) {
          command_list_state->barriers.push_back(barrier_state);
        }
        continue;
      }
      if (barrier_type != 0) {
        continue;
      }
      if (!require_payload_key(event, *barrier, "resource_object_id", last_error_) ||
          !require_payload_key(event, *barrier, "before", last_error_) ||
          !require_payload_key(event, *barrier, "after", last_error_) ||
          !require_payload_key(event, *barrier, "subresource", last_error_)) {
        return false;
      }
      const auto resource_object_id = json_object_id((*barrier)["resource_object_id"]);
      if (resource_object_id == 0) {
        continue;
      }
      auto resource_it = resources_.find(resource_object_id);
      if (resource_it == resources_.end()) {
        last_error_ = record_prefix(event) + ": ResourceBarrier references an unknown replay resource";
        return false;
      }

      constexpr std::uint32_t begin_only = 0x1;
      constexpr std::uint32_t end_only = 0x2;
      constexpr std::uint32_t all_subresources = 0xffffffffu;
      auto &resource = resource_it->second;
      const auto before = payload_u32(*barrier, "before");
      const auto after = payload_u32(*barrier, "after");
      const auto subresource = payload_u32(*barrier, "subresource");
      if (subresource != all_subresources && subresource >= resource_subresource_count(resource)) {
        last_error_ = record_prefix(event) + ": ResourceBarrier subresource index is out of bounds";
        return false;
      }
      barrier_state.resource_object_id = resource_object_id;
      barrier_state.before = before;
      barrier_state.after = after;
      barrier_state.subresource = subresource;
      if (command_list_state) {
        command_list_state->barriers.push_back(barrier_state);
      }
      if ((flags & end_only) == 0) {
        bool before_state_matches = resource.current_state == before;
        if (subresource == all_subresources) {
          for (const auto &[tracked_subresource, tracked_state] : resource.subresource_states) {
            (void)tracked_subresource;
            if (tracked_state != before) {
              before_state_matches = false;
              break;
            }
          }
        } else {
          const auto subresource_state = resource.subresource_states.find(subresource);
          before_state_matches = subresource_state != resource.subresource_states.end()
                                     ? subresource_state->second == before
                                     : resource.current_state == before;
        }
	      }

      ResourceTransition transition;
      transition.sequence = barrier_sequence;
      transition.command_list_object_id = command_list_id;
      transition.flags = flags;
      transition.before = before;
      transition.after = after;
      transition.subresource = subresource;
      resource.transitions.push_back(transition);
      resource.last_transition_sequence = barrier_sequence;
      if ((flags & begin_only) == 0) {
        if (subresource == all_subresources) {
          resource.current_state = after;
          resource.subresource_states.clear();
        } else {
          resource.subresource_states[subresource] = after;
        }
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::DrawInstanced" ||
             function_name == "ID3D12GraphicsCommandList::DrawIndexedInstanced") {
    if (function_name == "ID3D12GraphicsCommandList::DrawInstanced") {
      if (!require_payload_key(event, payload, "vertex_count_per_instance", last_error_) ||
          !require_payload_key(event, payload, "instance_count", last_error_) ||
          !require_payload_key(event, payload, "start_vertex_location", last_error_) ||
          !require_payload_key(event, payload, "start_instance_location", last_error_)) {
        return false;
      }
    } else if (!require_payload_key(event, payload, "index_count_per_instance", last_error_) ||
               !require_payload_key(event, payload, "instance_count", last_error_) ||
               !require_payload_key(event, payload, "start_index_location", last_error_) ||
               !require_payload_key(event, payload, "base_vertex_location", last_error_) ||
               !require_payload_key(event, payload, "start_instance_location", last_error_)) {
      return false;
    }
    if (command_list_state) {
      if (function_name == "ID3D12GraphicsCommandList::DrawInstanced") {
        DrawSemanticState draw;
        draw.sequence = event.callsite.sequence;
        draw.command_list_object_id = command_list_id;
        draw.pipeline_state_object_id = command_list_state->pipeline_state_object_id;
        draw.graphics_root_signature_object_id = command_list_state->graphics_root_signature_object_id;
        draw.graphics_root_tables = command_list_state->graphics_root_tables;
        draw.render_targets = command_list_state->render_targets;
        draw.depth_stencil = command_list_state->depth_stencil;
        draw.viewports = command_list_state->viewports;
        draw.scissor_rects = command_list_state->scissor_rects;
        draw.primitive_topology = command_list_state->primitive_topology;
        draw.vertex_count_per_instance = payload_u32(payload, "vertex_count_per_instance");
        draw.instance_count = payload_u32(payload, "instance_count");
        draw.start_vertex_location = payload_u32(payload, "start_vertex_location");
        draw.start_instance_location = payload_u32(payload, "start_instance_location");
        command_list_state->draws.push_back(draw);
      } else {
        DrawIndexedSemanticState draw;
        draw.sequence = event.callsite.sequence;
        draw.command_list_object_id = command_list_id;
        draw.pipeline_state_object_id = command_list_state->pipeline_state_object_id;
        draw.graphics_root_signature_object_id = command_list_state->graphics_root_signature_object_id;
        draw.graphics_root_tables = command_list_state->graphics_root_tables;
        draw.render_targets = command_list_state->render_targets;
        draw.depth_stencil = command_list_state->depth_stencil;
        draw.viewports = command_list_state->viewports;
        draw.scissor_rects = command_list_state->scissor_rects;
        draw.primitive_topology = command_list_state->primitive_topology;
        draw.index_count_per_instance = payload_u32(payload, "index_count_per_instance");
        draw.instance_count = payload_u32(payload, "instance_count");
        draw.start_index_location = payload_u32(payload, "start_index_location");
        draw.base_vertex_location = payload_i32(payload, "base_vertex_location");
        draw.start_instance_location = payload_u32(payload, "start_instance_location");
        command_list_state->indexed_draws.push_back(draw);
      }
      ++command_list_state->draw_count;
    }
    ++draw_calls_seen_;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::Dispatch" ||
             function_name == "ID3D12GraphicsCommandList4::DispatchRays" ||
             function_name == "ID3D12GraphicsCommandList6::DispatchMesh") {
    if (!require_payload_key(event, payload, function_name == "ID3D12GraphicsCommandList4::DispatchRays" ? "width" : "thread_group_count_x", last_error_) ||
        !require_payload_key(event, payload, function_name == "ID3D12GraphicsCommandList4::DispatchRays" ? "height" : "thread_group_count_y", last_error_) ||
        !require_payload_key(event, payload, function_name == "ID3D12GraphicsCommandList4::DispatchRays" ? "depth" : "thread_group_count_z", last_error_)) {
      return false;
    }
    if (command_list_state) {
      if (function_name == "ID3D12GraphicsCommandList4::DispatchRays") {
        DispatchRaysSemanticState dispatch;
        dispatch.sequence = event.callsite.sequence;
        dispatch.command_list_object_id = command_list_id;
        dispatch.width = payload_u32(payload, "width");
        dispatch.height = payload_u32(payload, "height");
        dispatch.depth = payload_u32(payload, "depth");
        const auto parse_address_range = [&](const char *key, GpuVirtualAddressBinding &binding, std::uint64_t &size) -> bool {
          const auto range = payload.find(key);
          if (range == payload.end()) {
            return true;
          }
          if (!range->is_object()) {
            last_error_ = record_prefix(event) + ": DispatchRays shader table range must be an object";
            return false;
          }
          if (!resolve_gpu_virtual_address_at(payload_u64(*range, "start_address"), event.callsite.sequence, binding, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          if (!validate_gpu_virtual_address_resolve_payload(event, key, *range, binding, last_error_)) {
            return false;
          }
          size = payload_u64(*range, "size_in_bytes");
          return true;
        };
        const auto parse_address_range_and_stride = [&](
                                                         const char *key,
                                                         GpuVirtualAddressBinding &binding,
                                                         std::uint64_t &size,
                                                         std::uint64_t &stride) -> bool {
          const auto range = payload.find(key);
          if (range == payload.end()) {
            return true;
          }
          if (!range->is_object()) {
            last_error_ = record_prefix(event) + ": DispatchRays shader table range must be an object";
            return false;
          }
          if (!resolve_gpu_virtual_address_at(payload_u64(*range, "start_address"), event.callsite.sequence, binding, last_error_)) {
            last_error_ = record_prefix(event) + ": " + last_error_;
            return false;
          }
          if (!validate_gpu_virtual_address_resolve_payload(event, key, *range, binding, last_error_)) {
            return false;
          }
          size = payload_u64(*range, "size_in_bytes");
          stride = payload_u64(*range, "stride_in_bytes");
          return true;
        };
        if (!parse_address_range(
                "ray_generation_shader_record",
                dispatch.ray_generation_shader_record,
                dispatch.ray_generation_shader_record_size) ||
            !parse_address_range_and_stride(
                "miss_shader_table",
                dispatch.miss_shader_table,
                dispatch.miss_shader_table_size,
                dispatch.miss_shader_table_stride) ||
            !parse_address_range_and_stride(
                "hit_group_table",
                dispatch.hit_group_table,
                dispatch.hit_group_table_size,
                dispatch.hit_group_table_stride) ||
            !parse_address_range_and_stride(
                "callable_shader_table",
                dispatch.callable_shader_table,
                dispatch.callable_shader_table_size,
                dispatch.callable_shader_table_stride)) {
          return false;
        }
        command_list_state->ray_dispatches.push_back(dispatch);
      } else {
        DispatchSemanticState dispatch;
        dispatch.sequence = event.callsite.sequence;
        dispatch.command_list_object_id = command_list_id;
        dispatch.pipeline_state_object_id = command_list_state->pipeline_state_object_id;
        dispatch.graphics_root_signature_object_id = command_list_state->graphics_root_signature_object_id;
        dispatch.compute_root_signature_object_id = command_list_state->compute_root_signature_object_id;
        dispatch.graphics_root_tables = command_list_state->graphics_root_tables;
        dispatch.compute_root_tables = command_list_state->compute_root_tables;
        dispatch.thread_group_count_x = payload_u32(payload, "thread_group_count_x");
        dispatch.thread_group_count_y = payload_u32(payload, "thread_group_count_y");
        dispatch.thread_group_count_z = payload_u32(payload, "thread_group_count_z");
        dispatch.mesh = function_name == "ID3D12GraphicsCommandList6::DispatchMesh";
        command_list_state->dispatches.push_back(dispatch);
      }
      ++command_list_state->dispatch_count;
    }
    ++dispatch_calls_seen_;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::BeginQuery" ||
             function_name == "ID3D12GraphicsCommandList::EndQuery" ||
             function_name == "ID3D12GraphicsCommandList::ResolveQueryData") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": query command missing query heap object id";
      return false;
    }
    if (query_heaps_.find(event.object_refs[1]) == query_heaps_.end()) {
      last_error_ = record_prefix(event) + ": query command references an unknown query heap";
      return false;
    }
    if (!require_payload_key(event, payload, "type", last_error_)) {
      return false;
    }
    if (function_name == "ID3D12GraphicsCommandList::ResolveQueryData") {
      if (!require_payload_key(event, payload, "start_index", last_error_) ||
          !require_payload_key(event, payload, "query_count", last_error_) ||
          !require_payload_key(event, payload, "aligned_dst_buffer_offset", last_error_)) {
        return false;
      }
      if (event.object_refs.size() < 3 || event.object_refs[2] == 0) {
        last_error_ = record_prefix(event) + ": ResolveQueryData missing destination buffer object id";
        return false;
      }
      if (resources_.find(event.object_refs[2]) == resources_.end()) {
        last_error_ = record_prefix(event) + ": ResolveQueryData references an unknown destination buffer";
        return false;
      }
    } else if (!require_payload_key(event, payload, "index", last_error_)) {
      return false;
    }
    if (command_list_state) {
      QueryCommandSemanticState query;
      query.sequence = event.callsite.sequence;
      query.command_list_object_id = command_list_id;
      query.query_heap_object_id = event.object_refs[1];
      query.type = payload_u32(payload, "type");
      query.index = payload_u32(payload, "index");
      query.start_index = payload_u32(payload, "start_index");
      query.query_count = payload_u32(payload, "query_count");
      query.dst_buffer_object_id =
          event.object_refs.size() >= 3 ? event.object_refs[2] : trace::ObjectId{};
      query.aligned_dst_buffer_offset = payload_u64(payload, "aligned_dst_buffer_offset");
      query.resolve = function_name == "ID3D12GraphicsCommandList::ResolveQueryData";
      query.end = function_name == "ID3D12GraphicsCommandList::EndQuery";
      command_list_state->queries.push_back(query);
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::SetPredication") {
    if (!require_payload_key(event, payload, "aligned_buffer_offset", last_error_) ||
        !require_payload_key(event, payload, "operation", last_error_)) {
      return false;
    }
    const auto buffer_object_id = event.object_refs.size() >= 2 ? event.object_refs[1] : trace::ObjectId{};
    if (buffer_object_id != 0 && resources_.find(buffer_object_id) == resources_.end()) {
      last_error_ = record_prefix(event) + ": SetPredication references an unknown buffer";
      return false;
    }
    if (command_list_state) {
      PredicationSemanticState predication;
      predication.sequence = event.callsite.sequence;
      predication.command_list_object_id = command_list_id;
      predication.buffer_object_id = buffer_object_id;
      predication.aligned_buffer_offset = payload_u64(payload, "aligned_buffer_offset");
      predication.operation = payload_u32(payload, "operation");
      command_list_state->predications.push_back(predication);
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList2::WriteBufferImmediate") {
    if (!require_payload_key(event, payload, "count", last_error_) ||
        !require_payload_key(event, payload, "writes", last_error_)) {
      return false;
    }
    const auto writes = payload.find("writes");
    if (writes == payload.end() || !writes->is_array()) {
      last_error_ = record_prefix(event) + ": WriteBufferImmediate writes must be an array";
      return false;
    }
    if (writes->size() != payload_u64(payload, "count")) {
      last_error_ = record_prefix(event) + ": WriteBufferImmediate count does not match writes payload";
      return false;
    }
    WriteBufferImmediateSemanticState state;
    state.sequence = event.callsite.sequence;
    state.command_list_object_id = command_list_id;
    for (const auto &write : *writes) {
      if (!write.is_object()) {
        last_error_ = record_prefix(event) + ": WriteBufferImmediate write must be an object";
        return false;
      }
      if (!require_payload_key(event, write, "dest", last_error_) ||
          !require_payload_key(event, write, "value", last_error_)) {
        return false;
      }
      WriteBufferImmediateEntry entry;
      if (!resolve_gpu_virtual_address_at(payload_u64(write, "dest"), event.callsite.sequence, entry.dest, last_error_)) {
        last_error_ = record_prefix(event) + ": " + last_error_;
        return false;
      }
      if (!validate_gpu_virtual_address_resolve_payload(event, "WriteBufferImmediate dest", write, entry.dest, last_error_)) {
        return false;
      }
      entry.value = payload_u32(write, "value");
      const auto mode_it = write.find("mode");
      entry.mode = (mode_it != write.end() && !mode_it->is_null())
                       ? static_cast<std::uint32_t>(json_u64(*mode_it))
                       : 0;
      state.writes.push_back(entry);
    }
    if (command_list_state) {
      command_list_state->write_buffer_immediates.push_back(std::move(state));
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ExecuteIndirect") {
    if (!require_payload_key(event, payload, "max_command_count", last_error_) ||
        !require_payload_key(event, payload, "arg_buffer_offset", last_error_) ||
        !require_payload_key(event, payload, "count_buffer_offset", last_error_)) {
      return false;
    }
    if (event.object_refs.size() < 3 || event.object_refs[1] == 0 || event.object_refs[2] == 0) {
      last_error_ = record_prefix(event) + ": ExecuteIndirect missing command signature or argument buffer object id";
      return false;
    }
    const auto signature_it = command_signatures_.find(event.object_refs[1]);
    if (signature_it == command_signatures_.end()) {
      last_error_ = record_prefix(event) + ": ExecuteIndirect references an unknown command signature";
      return false;
    }
    if (resources_.find(event.object_refs[2]) == resources_.end()) {
      last_error_ = record_prefix(event) + ": ExecuteIndirect references an unknown argument buffer";
      return false;
    }
    const auto count_buffer_object_id = event.object_refs.size() >= 4 ? event.object_refs[3] : trace::ObjectId{};
    if (count_buffer_object_id != 0 && resources_.find(count_buffer_object_id) == resources_.end()) {
      last_error_ = record_prefix(event) + ": ExecuteIndirect references an unknown count buffer";
      return false;
    }
    if (command_list_state) {
      ExecuteIndirectSemanticState execute;
      execute.sequence = event.callsite.sequence;
      execute.command_list_object_id = command_list_id;
      execute.command_signature_object_id = event.object_refs[1];
      execute.arg_buffer_object_id = event.object_refs[2];
      execute.count_buffer_object_id = count_buffer_object_id;
      execute.pipeline_state_object_id = command_list_state->pipeline_state_object_id;
      execute.graphics_root_signature_object_id = command_list_state->graphics_root_signature_object_id;
      execute.compute_root_signature_object_id = command_list_state->compute_root_signature_object_id;
      execute.graphics_root_tables = command_list_state->graphics_root_tables;
      execute.compute_root_tables = command_list_state->compute_root_tables;
      execute.render_targets = command_list_state->render_targets;
      execute.depth_stencil = command_list_state->depth_stencil;
      execute.viewports = command_list_state->viewports;
      execute.scissor_rects = command_list_state->scissor_rects;
      execute.primitive_topology = command_list_state->primitive_topology;
      execute.max_command_count = payload_u32(payload, "max_command_count");
      execute.arg_buffer_offset = payload_u64(payload, "arg_buffer_offset");
      execute.count_buffer_offset = payload_u64(payload, "count_buffer_offset");
      command_list_state->indirect_executes.push_back(execute);
      ++command_list_state->draw_count;
    }
    ++draw_calls_seen_;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ExecuteBundle") {
    if (event.object_refs.size() < 2 || event.object_refs[1] == 0) {
      last_error_ = record_prefix(event) + ": ExecuteBundle missing bundle command list object id";
      return false;
    }
    const auto bundle_it = command_lists_.find(event.object_refs[1]);
    if (bundle_it == command_lists_.end()) {
      last_error_ = record_prefix(event) + ": ExecuteBundle references an unknown bundle command list";
      return false;
    }
    if (bundle_it->second.type != 1) {
      last_error_ = record_prefix(event) + ": ExecuteBundle references a non-bundle command list";
      return false;
    }
    if (bundle_it->second.recording) {
      last_error_ = record_prefix(event) + ": ExecuteBundle references an open bundle command list";
      return false;
    }
    if (command_list_state) {
      ExecuteBundleSemanticState execute;
      execute.sequence = event.callsite.sequence;
      execute.command_list_object_id = command_list_id;
      execute.bundle_command_list_object_id = event.object_refs[1];
      command_list_state->bundle_executes.push_back(execute);
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::CopyBufferRegion" ||
             function_name == "ID3D12GraphicsCommandList::CopyBufferRegionBatch" ||
             function_name == "ID3D12GraphicsCommandList::CopyTextureRegion" ||
             function_name == "ID3D12GraphicsCommandList::CopyTextureRegionBatch" ||
             function_name == "ID3D12GraphicsCommandList::CopyResource" ||
             function_name == "ID3D12GraphicsCommandList::ResolveSubresource" ||
             function_name == "ID3D12GraphicsCommandList::ResolveSubresourceRegion") {
    if (command_list_state) {
      if (function_name == "ID3D12GraphicsCommandList::CopyBufferRegionBatch") {
        if (!require_payload_key(event, payload, "op_count", last_error_) ||
            !require_payload_key(event, payload, "ops", last_error_)) {
          return false;
        }
        const auto &ops = payload["ops"];
        if (!ops.is_array()) {
          last_error_ = record_prefix(event) + ": CopyBufferRegionBatch ops must be an array";
          return false;
        }
        if (ops.size() != payload_u64(payload, "op_count")) {
          last_error_ = record_prefix(event) + ": CopyBufferRegionBatch op_count does not match ops payload";
          return false;
        }
        command_list_state->copy_count += ops.size();
        for (const auto &op : ops) {
          if (!op.is_object()) {
            last_error_ = record_prefix(event) + ": CopyBufferRegionBatch op must be an object";
            return false;
          }
          if (!require_payload_key(event, op, "sequence", last_error_) ||
              !require_payload_key(event, op, "dst_buffer_object_id", last_error_) ||
              !require_payload_key(event, op, "dst_offset", last_error_) ||
              !require_payload_key(event, op, "src_buffer_object_id", last_error_) ||
              !require_payload_key(event, op, "src_offset", last_error_) ||
              !require_payload_key(event, op, "byte_count", last_error_)) {
            return false;
          }
          const auto dst_object_id = json_object_id(op["dst_buffer_object_id"]);
          const auto src_object_id = json_object_id(op["src_buffer_object_id"]);
          if (dst_object_id == 0 || src_object_id == 0) {
            last_error_ = record_prefix(event) + ": CopyBufferRegionBatch op missing source or destination buffer object id";
            return false;
          }
          if (resources_.find(dst_object_id) == resources_.end()) {
            last_error_ = record_prefix(event) + ": CopyBufferRegionBatch references an unknown destination buffer";
            return false;
          }
          if (resources_.find(src_object_id) == resources_.end()) {
            last_error_ = record_prefix(event) + ": CopyBufferRegionBatch references an unknown source buffer";
            return false;
          }
          BufferCopySemanticState copy;
          copy.sequence = payload_u64(op, "sequence", event.callsite.sequence);
          copy.command_list_object_id = command_list_id;
          copy.dst_buffer_object_id = dst_object_id;
          copy.src_buffer_object_id = src_object_id;
          copy.dst_offset = payload_u64(op, "dst_offset");
          copy.src_offset = payload_u64(op, "src_offset");
          copy.byte_count = payload_u64(op, "byte_count");
          command_list_state->buffer_copies.push_back(copy);
        }
      } else if (function_name != "ID3D12GraphicsCommandList::CopyTextureRegionBatch") {
        ++command_list_state->copy_count;
      }
      if (function_name == "ID3D12GraphicsCommandList::CopyBufferRegion") {
        if (!require_payload_key(event, payload, "dst_offset", last_error_) ||
            !require_payload_key(event, payload, "src_offset", last_error_) ||
            !require_payload_key(event, payload, "byte_count", last_error_)) {
          return false;
        }
        if (event.object_refs.size() < 3 || event.object_refs[1] == 0 || event.object_refs[2] == 0) {
          last_error_ = record_prefix(event) + ": CopyBufferRegion missing source or destination buffer object id";
          return false;
        }
        if (resources_.find(event.object_refs[1]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyBufferRegion references an unknown destination buffer";
          return false;
        }
        if (resources_.find(event.object_refs[2]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyBufferRegion references an unknown source buffer";
          return false;
        }
        BufferCopySemanticState copy;
        copy.sequence = event.callsite.sequence;
        copy.command_list_object_id = command_list_id;
        copy.dst_buffer_object_id = event.object_refs[1];
        copy.src_buffer_object_id = event.object_refs[2];
        copy.dst_offset = payload_u64(payload, "dst_offset");
        copy.src_offset = payload_u64(payload, "src_offset");
        copy.byte_count = payload_u64(payload, "byte_count");
        command_list_state->buffer_copies.push_back(copy);
      } else if (function_name == "ID3D12GraphicsCommandList::CopyTextureRegionBatch") {
        if (!require_payload_key(event, payload, "op_count", last_error_) ||
            !require_payload_key(event, payload, "ops", last_error_)) {
          return false;
        }
        const auto &ops = payload["ops"];
        if (!ops.is_array()) {
          last_error_ = record_prefix(event) + ": CopyTextureRegionBatch ops must be an array";
          return false;
        }
        if (ops.size() != payload_u64(payload, "op_count")) {
          last_error_ = record_prefix(event) + ": CopyTextureRegionBatch op_count does not match ops payload";
          return false;
        }
        command_list_state->copy_count += ops.size();
        for (const auto &op : ops) {
          if (!op.is_array() || op.size() < 7 ||
              !op[1].is_array() || op[1].size() < 9 ||
              !op[5].is_array() || op[5].size() < 9) {
            last_error_ = record_prefix(event) + ": CopyTextureRegionBatch op must use compact array payload";
            return false;
          }
          TextureCopySemanticState copy;
          copy.sequence = json_u64(op[0], event.callsite.sequence);
          copy.command_list_object_id = command_list_id;
          const auto parse_location = [&](const json &location, TextureCopyLocation &target) -> bool {
            target.resource_object_id = json_object_id(location[0]);
            target.type = static_cast<std::uint32_t>(json_u64(location[1]));
            target.subresource_index = static_cast<std::uint32_t>(json_u64(location[2]));
            target.footprint_offset = json_u64(location[3]);
            target.footprint_format = static_cast<std::uint32_t>(json_u64(location[4]));
            target.footprint_width = static_cast<std::uint32_t>(json_u64(location[5]));
            target.footprint_height = static_cast<std::uint32_t>(json_u64(location[6]));
            target.footprint_depth = static_cast<std::uint32_t>(json_u64(location[7]));
            target.footprint_row_pitch = static_cast<std::uint32_t>(json_u64(location[8]));
            if (target.type > 1) {
              last_error_ = record_prefix(event) + ": CopyTextureRegionBatch has unsupported texture copy location type";
              return false;
            }
            return true;
          };
          if (!parse_location(op[1], copy.dst) ||
              !parse_location(op[5], copy.src)) {
            return false;
          }
          copy.dst_x = static_cast<std::uint32_t>(json_u64(op[2]));
          copy.dst_y = static_cast<std::uint32_t>(json_u64(op[3]));
          copy.dst_z = static_cast<std::uint32_t>(json_u64(op[4]));
          if (!op[6].is_null()) {
            if (!op[6].is_array() || op[6].size() < 6) {
              last_error_ = record_prefix(event) + ": CopyTextureRegionBatch src_box must be null or a 6-column array";
              return false;
            }
            copy.has_src_box = true;
            copy.src_box_left = static_cast<std::uint32_t>(json_u64(op[6][0]));
            copy.src_box_top = static_cast<std::uint32_t>(json_u64(op[6][1]));
            copy.src_box_front = static_cast<std::uint32_t>(json_u64(op[6][2]));
            copy.src_box_right = static_cast<std::uint32_t>(json_u64(op[6][3]));
            copy.src_box_bottom = static_cast<std::uint32_t>(json_u64(op[6][4]));
            copy.src_box_back = static_cast<std::uint32_t>(json_u64(op[6][5]));
          }
          if (copy.dst.resource_object_id != 0 && resources_.find(copy.dst.resource_object_id) == resources_.end()) {
            last_error_ = record_prefix(event) + ": CopyTextureRegionBatch references an unknown destination resource";
            return false;
          }
          if (copy.src.resource_object_id != 0 && resources_.find(copy.src.resource_object_id) == resources_.end()) {
            last_error_ = record_prefix(event) + ": CopyTextureRegionBatch references an unknown source resource";
            return false;
          }
          command_list_state->texture_copies.push_back(copy);
        }
      } else if (function_name == "ID3D12GraphicsCommandList::CopyTextureRegion" &&
          payload_has(payload, "dst") && payload_has(payload, "src")) {
        if (!require_payload_key(event, payload, "dst_x", last_error_) ||
            !require_payload_key(event, payload, "dst_y", last_error_) ||
            !require_payload_key(event, payload, "dst_z", last_error_)) {
          return false;
        }
        TextureCopySemanticState copy;
        copy.sequence = event.callsite.sequence;
        copy.command_list_object_id = command_list_id;
        copy.dst_x = payload_u32(payload, "dst_x");
        copy.dst_y = payload_u32(payload, "dst_y");
        copy.dst_z = payload_u32(payload, "dst_z");
        const auto src_box = payload.find("src_box");
        copy.has_src_box = payload.value("has_src_box", false) ||
                           (src_box != payload.end() && src_box->is_object());
        if (copy.has_src_box && src_box != payload.end() && src_box->is_object()) {
          copy.src_box_left = payload_u32(*src_box, "left");
          copy.src_box_top = payload_u32(*src_box, "top");
          copy.src_box_front = payload_u32(*src_box, "front");
          copy.src_box_right = payload_u32(*src_box, "right");
          copy.src_box_bottom = payload_u32(*src_box, "bottom");
          copy.src_box_back = payload_u32(*src_box, "back");
        }
        if (!parse_texture_copy_location_payload(event, "dst", copy.dst, last_error_) ||
            !parse_texture_copy_location_payload(event, "src", copy.src, last_error_)) {
          return false;
        }
        if (copy.dst.resource_object_id != 0 && resources_.find(copy.dst.resource_object_id) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyTextureRegion references an unknown destination resource";
          return false;
        }
        if (copy.src.resource_object_id != 0 && resources_.find(copy.src.resource_object_id) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyTextureRegion references an unknown source resource";
          return false;
        }
        command_list_state->texture_copies.push_back(copy);
      } else if (function_name == "ID3D12GraphicsCommandList::CopyTextureRegion") {
        last_error_ = record_prefix(event) + ": CopyTextureRegion missing texture copy location payload";
        return false;
      } else if (function_name == "ID3D12GraphicsCommandList::CopyResource") {
        if (event.object_refs.size() < 3 || event.object_refs[1] == 0 || event.object_refs[2] == 0) {
          last_error_ = record_prefix(event) + ": CopyResource missing source or destination resource object id";
          return false;
        }
        if (resources_.find(event.object_refs[1]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyResource references an unknown destination resource";
          return false;
        }
        if (resources_.find(event.object_refs[2]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": CopyResource references an unknown source resource";
          return false;
        }
        ResourceCopySemanticState copy;
        copy.sequence = event.callsite.sequence;
        copy.command_list_object_id = command_list_id;
        copy.dst_resource_object_id = event.object_refs[1];
        copy.src_resource_object_id = event.object_refs[2];
        command_list_state->resource_copies.push_back(copy);
      } else if (function_name == "ID3D12GraphicsCommandList::ResolveSubresource" ||
                 function_name == "ID3D12GraphicsCommandList::ResolveSubresourceRegion") {
        if (!require_payload_key(event, payload, "dst_subresource", last_error_) ||
            !require_payload_key(event, payload, "src_subresource", last_error_) ||
            !require_payload_key(event, payload, "format", last_error_)) {
          return false;
        }
        if (event.object_refs.size() < 3 || event.object_refs[1] == 0 || event.object_refs[2] == 0) {
          last_error_ = record_prefix(event) + ": ResolveSubresource missing source or destination resource object id";
          return false;
        }
        if (resources_.find(event.object_refs[1]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": ResolveSubresource references an unknown destination resource";
          return false;
        }
        if (resources_.find(event.object_refs[2]) == resources_.end()) {
          last_error_ = record_prefix(event) + ": ResolveSubresource references an unknown source resource";
          return false;
        }
        ResolveSubresourceSemanticState resolve;
        resolve.sequence = event.callsite.sequence;
        resolve.command_list_object_id = command_list_id;
        resolve.dst_resource_object_id = event.object_refs[1];
        resolve.src_resource_object_id = event.object_refs[2];
        resolve.dst_subresource = payload_u32(payload, "dst_subresource");
        resolve.src_subresource = payload_u32(payload, "src_subresource");
        resolve.format = payload_u32(payload, "format");
        resolve.mode = 3;
        if (function_name == "ID3D12GraphicsCommandList::ResolveSubresourceRegion") {
          if (!require_payload_key(event, payload, "dst_x", last_error_) ||
              !require_payload_key(event, payload, "dst_y", last_error_) ||
              !require_payload_key(event, payload, "src_rect", last_error_) ||
              !require_payload_key(event, payload, "mode", last_error_)) {
            return false;
          }
          resolve.dst_x = payload_u32(payload, "dst_x");
          resolve.dst_y = payload_u32(payload, "dst_y");
          resolve.mode = payload_u32(payload, "mode");
          const auto src_rect = payload.find("src_rect");
          if (src_rect != payload.end() && !src_rect->is_null()) {
            if (!src_rect->is_object()) {
              last_error_ = record_prefix(event) + ": ResolveSubresourceRegion src_rect must be null or an object";
              return false;
            }
            resolve.has_src_rect = true;
            resolve.src_rect_left = payload_i32(*src_rect, "left");
            resolve.src_rect_top = payload_i32(*src_rect, "top");
            resolve.src_rect_right = payload_i32(*src_rect, "right");
            resolve.src_rect_bottom = payload_i32(*src_rect, "bottom");
          }
        }
        command_list_state->resolves.push_back(resolve);
      }
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandListExt::TemporalUpscale") {
    if (event.object_refs.size() < 5) {
      last_error_ = record_prefix(event) + ": TemporalUpscale missing resource object refs";
      return false;
    }
    const auto require_resource_ref = [&](std::size_t index, const char *name) -> bool {
      if (event.object_refs[index] == 0 || resources_.find(event.object_refs[index]) == resources_.end()) {
        last_error_ = record_prefix(event) + ": TemporalUpscale references an unknown " + std::string(name) + " resource";
        return false;
      }
      return true;
    };
    if (!require_resource_ref(1, "color") ||
        !require_resource_ref(2, "depth") ||
        !require_resource_ref(3, "motion vector") ||
        !require_resource_ref(4, "output")) {
      return false;
    }
    if (event.object_refs.size() >= 6 &&
        event.object_refs[5] != 0 &&
        resources_.find(event.object_refs[5]) == resources_.end()) {
      last_error_ = record_prefix(event) + ": TemporalUpscale references an unknown exposure texture resource";
      return false;
    }
    if (!require_payload_key(event, payload, "input_content_width", last_error_) ||
        !require_payload_key(event, payload, "input_content_height", last_error_) ||
        !require_payload_key(event, payload, "motion_vector_scale_x", last_error_) ||
        !require_payload_key(event, payload, "motion_vector_scale_y", last_error_) ||
        !require_payload_key(event, payload, "pre_exposure", last_error_) ||
        !require_payload_key(event, payload, "jitter_offset_x", last_error_) ||
        !require_payload_key(event, payload, "jitter_offset_y", last_error_)) {
      return false;
    }
    if (command_list_state) {
      TemporalUpscaleSemanticState upscale;
      upscale.sequence = event.callsite.sequence;
      upscale.command_list_object_id = command_list_id;
      upscale.input_content_width = payload_u32(payload, "input_content_width");
      upscale.input_content_height = payload_u32(payload, "input_content_height");
      upscale.auto_exposure = payload_bool(payload, "auto_exposure");
      upscale.in_reset = payload_bool(payload, "in_reset");
      upscale.depth_reversed = payload_bool(payload, "depth_reversed");
      upscale.motion_vector_in_display_res = payload_bool(payload, "motion_vector_in_display_res");
      upscale.color_object_id = event.object_refs[1];
      upscale.depth_object_id = event.object_refs[2];
      upscale.motion_vector_object_id = event.object_refs[3];
      upscale.output_object_id = event.object_refs[4];
      upscale.exposure_texture_object_id = event.object_refs.size() >= 6 ? event.object_refs[5] : 0;
      upscale.motion_vector_scale_x = payload_float(payload, "motion_vector_scale_x");
      upscale.motion_vector_scale_y = payload_float(payload, "motion_vector_scale_y");
      upscale.pre_exposure = payload_float(payload, "pre_exposure");
      upscale.jitter_offset_x = payload_float(payload, "jitter_offset_x");
      upscale.jitter_offset_y = payload_float(payload, "jitter_offset_y");
      command_list_state->temporal_upscales.push_back(upscale);
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Resource::Map") {
    if (!require_payload_key(event, payload, "subresource", last_error_)) {
      return false;
    }
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      last_error_ = record_prefix(event) + ": Map missing resource object id";
      return false;
    }
    auto resource_it = resources_.find(event.object_refs.front());
    if (resource_it == resources_.end()) {
      last_error_ = record_prefix(event) + ": Map references an unknown replay resource";
      return false;
    }
    resource_it->second.mapped = payload.value("mapped", false);
    resource_it->second.mapped_subresource = payload_u32(payload, "subresource");
    resource_it->second.map_sequence = event.callsite.sequence;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12Resource::Unmap") {
    if (!require_payload_key(event, payload, "subresource", last_error_)) {
      return false;
    }
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      last_error_ = record_prefix(event) + ": Unmap missing resource object id";
      return false;
    }
    auto resource_it = resources_.find(event.object_refs.front());
    if (resource_it == resources_.end()) {
      last_error_ = record_prefix(event) + ": Unmap references an unknown replay resource";
      return false;
    }
    const auto subresource = payload_u32(payload, "subresource");
    if (resource_it->second.mapped && resource_it->second.mapped_subresource != subresource) {
      last_error_ = record_prefix(event) + ": Unmap subresource does not match prior Map";
      return false;
    }
    const auto buffer_path = payload_string(payload, "buffer_path");
    if (!buffer_path.empty() && !std::filesystem::is_regular_file(bundle_root_ / buffer_path)) {
      last_error_ = record_prefix(event) + ": missing mapped buffer asset " + buffer_path;
      return false;
    }
    const auto written_begin = payload_u64(payload, "written_begin");
    const auto written_end = payload_u64(payload, "written_end", written_begin);
    if (written_end < written_begin) {
      last_error_ = record_prefix(event) + ": Unmap written range is invalid";
      return false;
    }
    if (!buffer_path.empty()) {
      if (event.blob_refs.empty()) {
        last_error_ = record_prefix(event) + ": Unmap buffer asset is missing blob_refs";
        return false;
      }
      if (!verify_event_blob_refs_cover_paths(event, {std::filesystem::path(buffer_path)}, blob_paths_, last_error_)) {
        return false;
      }
      const auto absolute_buffer_path = bundle_root_ / buffer_path;
      std::vector<std::uint8_t> bytes;
      std::string asset_error;
      if (!read_exact_asset_bytes(absolute_buffer_path, written_end - written_begin, bytes, asset_error)) {
        last_error_ = record_prefix(event) + ": " + asset_error;
        return false;
      }
      ResourceDataUpdate update;
      update.sequence = event.callsite.sequence;
      update.subresource = subresource;
      update.written_begin = written_begin;
      update.written_end = written_end;
      update.relative_path = buffer_path;
      update.blob_refs = event.blob_refs;
      update.bytes = std::move(bytes);
      resource_it->second.data_updates.push_back(std::move(update));
    }
    resource_it->second.mapped = false;
    resource_it->second.map_sequence = 0;
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12GraphicsCommandList::ResolveQueryDataResult") {
    if (!require_payload_key(event, payload, "dst_buffer_object_id", last_error_) ||
        !require_payload_key(event, payload, "aligned_dst_buffer_offset", last_error_)) {
      return false;
    }
    const auto dst_buffer_object_id = payload_u64(payload, "dst_buffer_object_id");
    auto resource_it = resources_.find(dst_buffer_object_id);
    if (resource_it == resources_.end()) {
      last_error_ = record_prefix(event) + ": ResolveQueryDataResult references an unknown destination buffer";
      return false;
    }
    const auto buffer_path = payload_string(payload, "buffer_path");
    if (buffer_path.empty()) {
      last_error_ = record_prefix(event) + ": ResolveQueryDataResult missing buffer asset path";
      return false;
    }
    const auto resolved_size = payload_u64(payload, "resolved_size");
    const auto dst_offset = payload_u64(payload, "aligned_dst_buffer_offset");
    std::vector<std::uint8_t> bytes;
    std::string asset_error;
    bool loaded_from_asset = false;
    if (std::filesystem::is_regular_file(bundle_root_ / buffer_path)) {
      if (event.blob_refs.empty()) {
        asset_error = "ResolveQueryDataResult buffer asset is missing blob_refs";
      } else {
        std::string blob_ref_error;
        if (verify_event_blob_refs_cover_paths(event, {std::filesystem::path(buffer_path)}, blob_paths_, blob_ref_error)) {
          loaded_from_asset = read_exact_asset_bytes(bundle_root_ / buffer_path, resolved_size, bytes, asset_error);
        } else {
          asset_error = blob_ref_error;
        }
      }
    } else {
      asset_error = "missing query resolve asset " + buffer_path;
    }
    if (!loaded_from_asset) {
      const auto inline_hex = payload_string(payload, "resolved_data_hex");
      if (inline_hex.empty() || !decode_hex_bytes(inline_hex, resolved_size, bytes, asset_error)) {
        last_error_ = record_prefix(event) + ": " + asset_error;
        return false;
      }
    }
    ResourceDataUpdate update;
    update.sequence = event.callsite.sequence;
    update.subresource = 0;
    update.written_begin = dst_offset;
    update.written_end = dst_offset + resolved_size;
    update.relative_path = buffer_path;
    update.blob_refs = event.blob_refs;
    update.bytes = std::move(bytes);
    resource_it->second.data_updates.push_back(std::move(update));
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12CommandQueue::ExecuteCommandLists") {
    if (!event.object_refs.empty()) {
      const auto queue_object_id = event.object_refs.front();
      auto queue_it = command_queues_.find(queue_object_id);
      if (queue_it == command_queues_.end()) {
        last_error_ = record_prefix(event) + ": ExecuteCommandLists references an unknown command queue";
        return false;
      }
      const auto command_list_count = payload_u64(payload, "command_list_count", event.object_refs.size() - 1);
      if (command_list_count != event.object_refs.size() - 1) {
        last_error_ = record_prefix(event) + ": ExecuteCommandLists object refs do not match command_list_count";
        return false;
      }
      const auto bridge_command_list_count = payload_u64(payload, "bridge_command_list_count", 0);
      if (command_list_count == 0 && bridge_command_list_count != 0) {
        ++queue_it->second.execute_count;
        queue_it->second.last_execute_sequence = event.callsite.sequence;
        ++semantic_calls_seen_;
        return true;
      }
      submissions_.begin_batch(queue_object_id, 0, event.callsite.sequence);
      for (std::size_t index = 1; index < event.object_refs.size(); ++index) {
        const auto submitted_list = command_lists_.find(event.object_refs[index]);
        if (submitted_list == command_lists_.end()) {
          last_error_ = record_prefix(event) + ": ExecuteCommandLists references an unknown command list";
          return false;
        }
        if (submitted_list->second.recording || submitted_list->second.close_sequence == 0) {
          last_error_ = record_prefix(event) + ": ExecuteCommandLists references an open command list";
          return false;
        }
        if (submitted_list->second.type != queue_it->second.type) {
          last_error_ = record_prefix(event) + ": ExecuteCommandLists command list type does not match queue type";
          return false;
        }
        if (index == 1) {
          submissions_.set_command_allocator(submitted_list->second.allocator_object_id);
        }
        submissions_.append_descriptor_heaps(submitted_list->second.descriptor_heap_object_ids);
        submissions_.append_command_list(event.object_refs[index]);
      }
      ++queue_it->second.execute_count;
      queue_it->second.last_execute_sequence = event.callsite.sequence;
      submissions_.end_batch();
    }
    ++semantic_calls_seen_;
  } else if (function_name == "ID3D12CommandQueue::Signal" ||
             function_name == "ID3D12CommandQueue::Wait" ||
             function_name == "ID3D12Fence::Signal" ||
             function_name == "ID3D12Fence::SetEventOnCompletion" ||
             function_name == "ID3D12Fence::GetCompletedValue") {
    if (function_name == "ID3D12Fence::GetCompletedValue") {
      if (!require_payload_key(event, payload, "completed_value", last_error_)) {
        return false;
      }
      const auto fence_object_id = event.object_refs.empty() ? trace::ObjectId{} : event.object_refs.front();
      if (fence_object_id == 0) {
        last_error_ = record_prefix(event) + ": GetCompletedValue missing fence object id";
        return false;
      }
      auto fence_it = fences_.find(fence_object_id);
      if (fence_it == fences_.end()) {
        last_error_ = record_prefix(event) + ": GetCompletedValue references an unknown fence";
        return false;
      }
      const auto completed_value = payload_u64(payload, "completed_value");
      if (completed_value > fence_it->second.completed_value) {
        fence_it->second.completed_value = completed_value;
      }
      ++semantic_calls_seen_;
    } else {
      if (!require_payload_key(event, payload, "fence_value", last_error_)) {
        return false;
      }
      const bool queue_operation = function_name == "ID3D12CommandQueue::Signal" ||
                                   function_name == "ID3D12CommandQueue::Wait";
      const auto fence_object_id = queue_operation
                                       ? (event.object_refs.size() >= 2 ? event.object_refs[1] : trace::ObjectId{})
                                       : (event.object_refs.empty() ? trace::ObjectId{} : event.object_refs.front());
      if (fence_object_id == 0) {
        last_error_ = record_prefix(event) + ": fence operation missing fence object id";
        return false;
      }
      const auto queue_object_id = queue_operation && !event.object_refs.empty() ? event.object_refs.front() : trace::ObjectId{};
      if (queue_operation) {
        const auto queue_it = command_queues_.find(queue_object_id);
        if (queue_it == command_queues_.end()) {
          last_error_ = record_prefix(event) + ": fence operation references an unknown command queue";
          return false;
        }
      }
      auto fence_it = fences_.find(fence_object_id);
      if (fence_it == fences_.end()) {
        last_error_ = record_prefix(event) + ": fence operation references an unknown fence";
        return false;
      }
      FenceOperationSemanticState operation;
      operation.sequence = event.callsite.sequence;
      operation.queue_object_id = queue_object_id;
      operation.fence_object_id = fence_object_id;
      operation.fence_value = payload_u64(payload, "fence_value");
      operation.queue_operation = queue_operation;
      operation.wait_operation = function_name == "ID3D12CommandQueue::Wait";
      operation.event_operation = function_name == "ID3D12Fence::SetEventOnCompletion";
      fence_operations_.push_back(operation);
      if (function_name == "ID3D12CommandQueue::Signal" ||
          function_name == "ID3D12Fence::Signal") {
        fence_it->second.current_value = operation.fence_value;
        fence_it->second.last_signal_sequence = event.callsite.sequence;
        if (function_name == "ID3D12CommandQueue::Signal") {
          submissions_.signal_fence_for_queue(
              operation.queue_object_id,
              fence_object_id,
              operation.fence_value,
              event.callsite.sequence);
          const auto &batches = submissions_.completed_batches();
          if (!batches.empty() && batches.back().queue_object_id == operation.queue_object_id) {
            for (const auto command_list_object_id : batches.back().command_list_ids) {
              const auto command_list_it = command_lists_.find(command_list_object_id);
              if (command_list_it == command_lists_.end() || command_list_it->second.allocator_object_id == 0) {
                continue;
              }
              auto allocator_it = command_allocators_.find(command_list_it->second.allocator_object_id);
              if (allocator_it == command_allocators_.end()) {
                continue;
              }
              allocator_it->second.last_submit_sequence = batches.back().execute_sequence;
              allocator_it->second.last_submit_fence_object_id = fence_object_id;
              allocator_it->second.last_submit_fence_value = operation.fence_value;
            }
          }
        }
      } else if (function_name == "ID3D12CommandQueue::Wait") {
        fence_it->second.last_wait_sequence = event.callsite.sequence;
        submissions_.wait_on_fence_for_queue(
            operation.queue_object_id,
            fence_object_id,
            operation.fence_value,
            event.callsite.sequence);
      } else {
        fence_it->second.last_event_sequence = event.callsite.sequence;
        if (operation.fence_value > fence_it->second.completed_value) {
          fence_it->second.completed_value = operation.fence_value;
        }
      }
      ++semantic_calls_seen_;
    }
  } else if (function_name == "ID3D12CommandAllocator::Reset") {
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      last_error_ = record_prefix(event) + ": CommandAllocator::Reset missing allocator object id";
      return false;
    }
    auto allocator_it = command_allocators_.find(event.object_refs.front());
    if (allocator_it == command_allocators_.end()) {
      last_error_ = record_prefix(event) + ": CommandAllocator::Reset references an unknown allocator";
      return false;
    }
    if (allocator_it->second.last_submit_fence_object_id != 0) {
      const auto fence_it = fences_.find(allocator_it->second.last_submit_fence_object_id);
      if (fence_it == fences_.end()) {
        last_error_ = record_prefix(event) + ": CommandAllocator::Reset references an unknown submit fence";
        return false;
      }
      if (fence_it->second.completed_value < allocator_it->second.last_submit_fence_value) {
        last_error_ = record_prefix(event) + ": CommandAllocator::Reset before captured fence completion for prior submission";
        return false;
      }
    }
    for (const auto &[command_list_object_id, command_list] : command_lists_) {
      (void)command_list_object_id;
      if (command_list.recording && command_list.allocator_object_id == allocator_it->first) {
        last_error_ = record_prefix(event) + ": CommandAllocator::Reset while a referencing command list is recording";
        return false;
      }
    }
    ++allocator_it->second.reset_count;
    allocator_it->second.last_reset_sequence = event.callsite.sequence;
    ++semantic_calls_seen_;
  } else if (function_name == "D3D12CreateDevice" ||
             function_name == "ID3D12Device::QueryInterface" ||
             function_name == "IDXGIFactory::CreateSwapChain" ||
             function_name == "ID3D12GraphicsCommandList::QueryInterface" ||
             function_name == "IDXGISwapChain::Present" ||
             function_name == "DXMT::FenceDependencyBatch") {
    if (function_name == "IDXGISwapChain::Present" &&
        (!require_payload_key(event, payload, "sync_interval", last_error_) ||
         !require_payload_key(event, payload, "flags", last_error_))) {
      return false;
    }
    ++semantic_calls_seen_;
  }

  ++commands_replayed_;
  return true;
}
