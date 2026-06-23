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

  bool event_ordered_resource_update_seen(
      trace::ObjectId resource_object_id,
      const D3D12ReplayBackend::ResourceDataUpdate &update) const
  {
    const std::uint64_t effective_sequence =
        event_ordered_effective_resource_update_sequence(update);
    for (const auto &entry : resource_update_timeline_) {
      if (entry.sequence == effective_sequence &&
          entry.resource_object_id == resource_object_id &&
          entry.update != nullptr &&
          entry.update->subresource == update.subresource &&
          entry.update->written_begin == update.written_begin &&
          entry.update->written_end == update.written_end &&
          entry.update->relative_path.generic_string() == update.relative_path.generic_string() &&
          entry.update->bytes.size() == update.bytes.size() &&
          entry.update->blob_refs == update.blob_refs) {
        return true;
      }
    }
    return false;
  }

  void event_ordered_note_resource_update(
      trace::ObjectId resource_object_id,
      const D3D12ReplayBackend::ResourceDataUpdate &update)
  {
    if (event_ordered_resource_update_seen(resource_object_id, update)) {
      return;
    }
    event_ordered_resource_update_storage_.push_back(update);
    const auto &stable_update = event_ordered_resource_update_storage_.back();
    ResourceDataUpdateRef entry;
    entry.sequence = event_ordered_effective_resource_update_sequence(stable_update);
    entry.resource_object_id = resource_object_id;
    entry.update = &stable_update;
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
  }

  void event_ordered_ingest_resource_updates()
  {
    for (const auto &resource : backend_.resource_versions_) {
      for (const auto &update : resource.data_updates) {
        event_ordered_note_resource_update(resource.resource_object_id, update);
      }
    }
    for (const auto &[resource_object_id, resource] : backend_.resources_) {
      for (const auto &update : resource.data_updates) {
        event_ordered_note_resource_update(resource_object_id, update);
      }
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
    event_ordered_log(line);
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

  bool event_ordered_copy_belongs_to_event(
      const D3D12ReplayBackend::CopyDescriptorSemanticState &copy,
      const trace::EventRecord &event) const
  {
    if (event.callsite.function_name != "ID3D12Device::CopyDescriptorsBatch") {
      return copy.create_sequence == event.callsite.sequence;
    }

    json payload;
    std::string error;
    if (!payload_to_json(event, payload, error)) {
      return false;
    }
    const auto ops = payload.find("ops");
    if (ops == payload.end() || !ops->is_array()) {
      return false;
    }
    for (const auto &op : *ops) {
      if (op.is_array() && !op.empty() && json_u64(op[0]) == copy.create_sequence) {
        return true;
      }
    }
    return false;
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
    bool applied_any = false;
    for (const auto &copy : backend_.descriptor_copies_) {
      if (!event_ordered_copy_belongs_to_event(copy, event)) {
        continue;
      }
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
    if (applied_any) {
      event_ordered_refresh_descriptor_copy_state();
    }
  }
