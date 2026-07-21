  static std::uint64_t event_ordered_effective_resource_update_sequence(
      const D3D12ReplayBackend::ResourceDataUpdate &update)
  {
    return update.apply_sequence != 0 ? update.apply_sequence : update.sequence;
  }

  static bool event_ordered_resource_update_less(
      const ResourceDataUpdateRef &lhs,
      const ResourceDataUpdateRef &rhs)
  {
    if (lhs.sequence != rhs.sequence) {
      return lhs.sequence < rhs.sequence;
    }
    if (lhs.resource_object_id != rhs.resource_object_id) {
      return lhs.resource_object_id < rhs.resource_object_id;
    }
    if (lhs.update == nullptr || rhs.update == nullptr) {
      return lhs.update < rhs.update;
    }
    if (lhs.update->subresource != rhs.update->subresource) {
      return lhs.update->subresource < rhs.update->subresource;
    }
    if (lhs.update->written_begin != rhs.update->written_begin) {
      return lhs.update->written_begin < rhs.update->written_begin;
    }
    if (lhs.update->written_end != rhs.update->written_end) {
      return lhs.update->written_end < rhs.update->written_end;
    }
    const auto lhs_path = lhs.update->relative_path.generic_string();
    const auto rhs_path = rhs.update->relative_path.generic_string();
    if (lhs_path != rhs_path) {
      return lhs_path < rhs_path;
    }
    if (lhs.update->bytes.size() != rhs.update->bytes.size()) {
      return lhs.update->bytes.size() < rhs.update->bytes.size();
    }
    return lhs.update->blob_refs < rhs.update->blob_refs;
  }

  void event_ordered_note_resource_update(
      trace::ObjectId resource_object_id,
      const D3D12ReplayBackend::ResourceDataUpdate &update)
  {
    ResourceDataUpdateRef entry;
    entry.sequence = event_ordered_effective_resource_update_sequence(update);
    entry.resource_object_id = resource_object_id;
    entry.update = &update;
    std::string update_key = event_ordered_resource_update_key(entry);
    if (event_ordered_ingested_update_keys_.find(update_key) != event_ordered_ingested_update_keys_.end()) {
      return;
    }
    auto insert_at = std::upper_bound(
        resource_update_timeline_.begin(),
        resource_update_timeline_.end(),
        entry,
        [](const ResourceDataUpdateRef &value, const ResourceDataUpdateRef &existing) {
          return event_ordered_resource_update_less(value, existing);
        });
    const auto inserted_index = static_cast<std::size_t>(
        insert_at - resource_update_timeline_.begin());
    resource_update_timeline_.insert(insert_at, entry);
    if (inserted_index < next_resource_update_index_) {
      next_resource_update_index_ = inserted_index;
    }
    event_ordered_ingested_update_keys_.insert(std::move(update_key));
  }

  void event_ordered_ingest_resource_updates()
  {
    const auto &notifications = backend_.resource_data_update_notifications_;
    while (event_ordered_resource_update_notification_cursor_ < notifications.size()) {
      const auto &notification =
          notifications[event_ordered_resource_update_notification_cursor_++];
      if (notification.update_index >= backend_.event_ordered_resource_data_updates_.size()) {
        continue;
      }
      event_ordered_note_resource_update(
          notification.resource_object_id,
          backend_.event_ordered_resource_data_updates_[notification.update_index]);
    }
  }

  std::string event_ordered_resource_update_key(const ResourceDataUpdateRef &entry) const
  {
    std::ostringstream key;
    key << entry.resource_object_id << ":"
        << entry.sequence << ":";
    if (entry.update == nullptr) {
      key << "<null>";
      return key.str();
    }
    key << entry.update->subresource << ":"
        << entry.update->written_begin << ":"
        << entry.update->written_end << ":"
        << entry.update->relative_path.generic_string() << ":"
        << entry.update->bytes.size();
    for (const auto blob_ref : entry.update->blob_refs) {
      key << ":" << blob_ref;
    }
    return key.str();
  }

  void event_ordered_report_content_error(
      const trace::EventRecord &event,
      const char *phase,
      std::string error)
  {
    const std::string line = "[event-ordered] content:error seq=" +
                             std::to_string(event.callsite.sequence) +
                             " fn=" + event_ordered_event_name(event) +
                             " phase=" + phase + " " + error;
    backend_.last_error_ = std::move(error);
    event_ordered_failed_ = true;
    event_ordered_log(line);
  }

  EventOrderedMappedResource *event_ordered_mapped_resource(
      trace::ObjectId object_id,
      UINT subresource)
  {
    const auto resource_it = event_ordered_mapped_resources_.find(object_id);
    if (resource_it == event_ordered_mapped_resources_.end()) {
      return nullptr;
    }
    const auto subresource_it = resource_it->second.find(subresource);
    return subresource_it == resource_it->second.end() ? nullptr : &subresource_it->second;
  }

  void event_ordered_handle_resource_map(const trace::EventRecord &event)
  {
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      event_ordered_report_content_error(event, "map", "Map missing resource object id");
      return;
    }
    std::string error;
    const json *payload_ptr = payload_to_json_cached_ref(event, error);
    if (!payload_ptr) {
      event_ordered_report_content_error(event, "map", std::move(error));
      return;
    }
    const auto &payload = *payload_ptr;
    const UINT subresource = static_cast<UINT>(payload_u32(payload, "subresource"));
    D3D12_RANGE read_range{};
    const D3D12_RANGE *read_range_ptr = nullptr;
    if (!payload.contains("read_range") || !payload["read_range"].is_null()) {
      read_range.Begin = static_cast<SIZE_T>(payload_u64(payload, "read_begin"));
      read_range.End = static_cast<SIZE_T>(payload_u64(payload, "read_end", read_range.Begin));
      read_range_ptr = &read_range;
    }

    const ReplaySequenceScope sequence_scope(this, event.callsite.sequence);
    ID3D12Resource *resource = resource_for(event.object_refs.front(), error);
    if (!resource) {
      event_ordered_report_content_error(event, "map", std::move(error));
      return;
    }
    void *mapped = nullptr;
    const HRESULT hr = resource->Map(subresource, read_range_ptr, &mapped);
    const bool replay_mapped = SUCCEEDED(hr) && mapped != nullptr;
    const bool capture_mapped = event.callsite.result_code >= 0 && payload.value("mapped", false);
    if (replay_mapped != capture_mapped) {
      if (replay_mapped) {
        resource->Unmap(subresource, nullptr);
      }
      event_ordered_report_content_error(
          event,
          "map",
          "Map result differs from capture replay_hr=" +
              std::to_string(static_cast<std::int32_t>(hr)) +
              " captured_result=" + std::to_string(event.callsite.result_code));
      return;
    }
    if (!replay_mapped) {
      return;
    }

    auto &mapping = event_ordered_mapped_resources_[event.object_refs.front()][subresource];
    mapping.resource = resource;
    mapping.data = mapped;
    ++mapping.depth;
  }

  void event_ordered_handle_resource_data_update(const trace::EventRecord &event)
  {
    event_ordered_ingest_resource_updates();
    std::string error;
    if (!sync_resource_data_until(event.callsite.sequence, error)) {
      event_ordered_report_content_error(event, "resource-update", std::move(error));
    }
  }

  void event_ordered_handle_resource_unmap(const trace::EventRecord &event)
  {
    event_ordered_handle_resource_data_update(event);

    std::string error;
    const json *payload_ptr = payload_to_json_cached_ref(event, error);
    if (!payload_ptr) {
      event_ordered_report_content_error(event, "unmap", std::move(error));
      return;
    }
    const auto &payload = *payload_ptr;
    // Old bundles used Unmap as a generic mapped-memory snapshot record. Only the explicit bit in
    // the new schema proves that the game called the API; otherwise applying the bytes above is the
    // most faithful compatibility behavior available.
    if (!payload.value("api_call", false)) {
      return;
    }
    if (event.object_refs.empty() || event.object_refs.front() == 0) {
      event_ordered_report_content_error(event, "unmap", "Unmap missing resource object id");
      return;
    }
    const trace::ObjectId object_id = event.object_refs.front();
    const UINT subresource = static_cast<UINT>(payload_u32(payload, "subresource"));
    auto resource_it = event_ordered_mapped_resources_.find(object_id);
    if (resource_it == event_ordered_mapped_resources_.end()) {
      event_ordered_report_content_error(event, "unmap", "Unmap has no matching replay Map");
      return;
    }
    auto mapping_it = resource_it->second.find(subresource);
    if (mapping_it == resource_it->second.end() ||
        mapping_it->second.resource == nullptr ||
        mapping_it->second.depth == 0) {
      event_ordered_report_content_error(event, "unmap", "Unmap subresource has no matching replay Map");
      return;
    }

    D3D12_RANGE written_range{};
    const D3D12_RANGE *written_range_ptr = nullptr;
    if (payload.contains("written_begin") || payload.contains("written_end")) {
      written_range.Begin = static_cast<SIZE_T>(payload_u64(payload, "written_begin"));
      written_range.End = static_cast<SIZE_T>(payload_u64(payload, "written_end", written_range.Begin));
      written_range_ptr = &written_range;
    }
    mapping_it->second.resource->Unmap(subresource, written_range_ptr);
    if (--mapping_it->second.depth == 0) {
      resource_it->second.erase(mapping_it);
      if (resource_it->second.empty()) {
        event_ordered_mapped_resources_.erase(resource_it);
      }
    }
  }

  bool event_ordered_is_content_consumer_event(const trace::EventRecord &event) const
  {
    const std::string_view function_name(event.callsite.function_name);
    return event_ordered_is_record_call(event) ||
           function_name == "ID3D12CommandQueue::ExecuteCommandLists" ||
           event_ordered_is_present_event(event);
  }

  void event_ordered_sync_resource_content_before_consumer(const trace::EventRecord &event)
  {
    if (!event_ordered_is_content_consumer_event(event)) {
      return;
    }

    event_ordered_ingest_resource_updates();
    std::string error;
    if (!sync_resource_data_until(event.callsite.sequence, error)) {
      event_ordered_report_content_error(event, "resource-sync", std::move(error));
      return;
    }
  }

  static bool event_ordered_is_copy_descriptors_function(std::string_view function_name)
  {
    return function_name == "ID3D12Device::CopyDescriptors" ||
           function_name == "ID3D12Device::CopyDescriptorsSimple" ||
           function_name == "ID3D12Device::CopyDescriptorsBatch";
  }

  bool event_ordered_is_copy_descriptors_event(const trace::EventRecord &event) const
  {
    return event_ordered_is_copy_descriptors_function(event.callsite.function_name);
  }

  const D3D12ReplayBackend::SamplerSemanticState *event_ordered_sampler_semantic_at(
      std::uint64_t descriptor,
      std::uint64_t sequence) const
  {
    const D3D12ReplayBackend::SamplerSemanticState *found = nullptr;
    for (const auto &sampler : backend_.samplers_) {
      if (sampler.descriptor != descriptor || sampler.create_sequence > sequence) {
        continue;
      }
      if (found == nullptr || found->create_sequence < sampler.create_sequence) {
        found = &sampler;
      }
    }
    return found;
  }

  void event_ordered_remember_descriptor_copy_history(
      const D3D12ReplayBackend::CopyDescriptorSemanticState &copy)
  {
    for (const auto &pair : copy.descriptors) {
      const auto *source = descriptor_semantic_at(pair.src_descriptor, copy.create_sequence);
      D3D12ReplayBackend::DescriptorSemanticState copied;
      if (source != nullptr) {
        copied = *source;
      } else if (const auto *sampler =
                     event_ordered_sampler_semantic_at(pair.src_descriptor, copy.create_sequence)) {
        copied.kind = "Sampler";
        copied.create_sequence = sampler->create_sequence;
        copied.descriptor = sampler->descriptor;
        copied.binding = sampler->binding;
      } else {
        continue;
      }
      copied.create_sequence = copy.create_sequence;
      copied.descriptor = pair.dst_descriptor;
      D3D12ReplayBackend::DescriptorBinding dst_binding;
      std::string binding_error;
      if (backend_.resolve_descriptor_binding_at(
              pair.dst_descriptor,
              false,
              copy.create_sequence,
              dst_binding,
              binding_error)) {
        copied.binding = dst_binding;
      }
      event_ordered_copied_descriptor_storage_.push_back(std::move(copied));
      const auto &stored = event_ordered_copied_descriptor_storage_.back();
      descriptor_slot_history_[pair.dst_descriptor].push_back(
          DescriptorSlotHistoryEntry{copy.create_sequence, &stored});
    }
  }

  void event_ordered_refresh_descriptor_copy_state()
  {
    descriptor_copy_timeline_.clear();
    descriptor_copy_timeline_.reserve(backend_.descriptor_copies_.size());
    for (const auto &copy : backend_.descriptor_copies_) {
      descriptor_copy_timeline_.push_back(&copy);
    }
    std::stable_sort(
        descriptor_copy_timeline_.begin(),
        descriptor_copy_timeline_.end(),
        [](const auto *lhs, const auto *rhs) {
          return lhs->create_sequence < rhs->create_sequence;
        });
    next_descriptor_copy_index_ = descriptor_copy_timeline_.size();
  }

  void event_ordered_index_descriptor_copies()
  {
    while (event_ordered_descriptor_copy_index_cursor_ < backend_.descriptor_copies_.size()) {
      const auto &copy = backend_.descriptor_copies_[event_ordered_descriptor_copy_index_cursor_];
      event_ordered_descriptor_copy_indices_by_sequence_[copy.create_sequence].push_back(
          event_ordered_descriptor_copy_index_cursor_);
      ++event_ordered_descriptor_copy_index_cursor_;
    }
  }

  bool event_ordered_descriptor_copy_sequences_for_event(
      const trace::EventRecord &event,
      std::vector<std::uint64_t> &sequences) const
  {
    if (event.callsite.function_name != "ID3D12Device::CopyDescriptorsBatch") {
      sequences.push_back(event.callsite.sequence);
      return true;
    }

    std::string error;
    const json *payload_ptr = payload_to_json_cached_ref(event, error);
    if (!payload_ptr) {
      return false;
    }
    const auto &payload = *payload_ptr;
    const auto ops = payload.find("ops");
    if (ops == payload.end() || !ops->is_array()) {
      return false;
    }
    for (const auto &op : *ops) {
      if (!op.is_array() || op.empty()) {
        return false;
      }
      sequences.push_back(json_u64(op[0], event.callsite.sequence));
    }
    return true;
  }

  static std::string event_ordered_descriptor_copy_key(
      const D3D12ReplayBackend::CopyDescriptorSemanticState &copy)
  {
    std::ostringstream key;
    key << copy.create_sequence << ":" << copy.descriptor_heap_type;
    for (const auto &pair : copy.descriptors) {
      key << ":" << pair.dst_descriptor << "<-" << pair.src_descriptor;
    }
    return key.str();
  }

  void event_ordered_apply_copy_descriptors(const trace::EventRecord &event)
  {
    event_ordered_index_descriptor_copies();
    std::vector<std::uint64_t> copy_sequences;
    if (!event_ordered_descriptor_copy_sequences_for_event(event, copy_sequences)) {
      event_ordered_report_content_error(
          event,
          "copy-descriptors",
          "CopyDescriptors event payload could not be matched to semantic copies");
      return;
    }

    bool applied_any = false;
    for (const auto sequence : copy_sequences) {
      const auto index_it = event_ordered_descriptor_copy_indices_by_sequence_.find(sequence);
      if (index_it == event_ordered_descriptor_copy_indices_by_sequence_.end()) {
        continue;
      }
      for (const auto copy_index : index_it->second) {
        if (copy_index >= backend_.descriptor_copies_.size()) {
          continue;
        }
        const auto &copy = backend_.descriptor_copies_[copy_index];
        const auto key = event_ordered_descriptor_copy_key(copy);
        if (!event_ordered_copy_descriptors_applied_keys_.insert(key).second) {
          continue;
        }
        std::string error;
        if (!apply_descriptor_copy(copy, error)) {
          event_ordered_copy_descriptors_applied_keys_.erase(key);
          event_ordered_report_content_error(event, "copy-descriptors", std::move(error));
          continue;
        }
        event_ordered_remember_descriptor_copy_history(copy);
        ++event_ordered_copy_descriptors_applied_;
        applied_any = true;
      }
    }
    if (applied_any) {
      event_ordered_refresh_descriptor_copy_state();
    }
  }
