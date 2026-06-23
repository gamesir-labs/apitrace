  void materialize_create_resources(const trace::EventRecord &event)
  {
    if (!device_) {
      event_ordered_record_materialize_error(event, "CreateResource requires a bootstrapped native device");
      return;
    }

    trace::ObjectId object_id = 0;
    if (!event_ordered_created_resource_object_id(event, object_id)) {
      return;
    }

    const auto *resource = event_ordered_resource_semantic_for_create_event(event, object_id);
    if (resource == nullptr) {
      event_ordered_record_materialize_error(
          event,
          "CreateResource missing ResourceSemanticState object_id=" + std::to_string(object_id) +
              " create_seq=" + std::to_string(event.callsite.sequence));
      return;
    }

    materialize_native_resource(event, *resource);
  }

  bool event_ordered_created_resource_object_id(
      const trace::EventRecord &event,
      trace::ObjectId &object_id)
  {
    const std::string_view function_name(event.callsite.function_name);
    if (event_ordered_contains(function_name, "CreateCommittedResource")) {
      if (event.object_refs.size() >= 2 && event.object_refs[1] != 0) {
        object_id = event.object_refs[1];
        return true;
      }
      if (event.object_refs.size() == 1 && event.object_refs[0] != 0) {
        object_id = event.object_refs[0];
        return true;
      }
      event_ordered_record_materialize_error(event, "CreateCommittedResource missing resource object id");
      return false;
    }

    if (event_ordered_contains(function_name, "CreatePlacedResource")) {
      if (event.object_refs.size() >= 3 && event.object_refs[2] != 0) {
        object_id = event.object_refs[2];
        return true;
      }
      event_ordered_record_materialize_error(event, "CreatePlacedResource missing resource object id");
      return false;
    }

    if (event_ordered_contains(function_name, "CreateReservedResource")) {
      if (event.object_refs.size() >= 2 && event.object_refs[1] != 0) {
        object_id = event.object_refs[1];
        return true;
      }
      event_ordered_record_materialize_error(event, "CreateReservedResource missing resource object id");
      return false;
    }

    event_ordered_log_stub_once(event_ordered_category(event), event);
    return false;
  }

  const D3D12ReplayBackend::ResourceSemanticState *event_ordered_resource_semantic_for_create_event(
      const trace::EventRecord &event,
      trace::ObjectId object_id) const
  {
    const auto live_it = backend_.live_resource_version_indices_.find(object_id);
    if (live_it == backend_.live_resource_version_indices_.end() ||
        live_it->second >= backend_.resource_versions_.size()) {
      return nullptr;
    }

    const auto &resource = backend_.resource_versions_[live_it->second];
    if (resource.resource_object_id != object_id ||
        resource.create_sequence != event.callsite.sequence) {
      return nullptr;
    }
    return &resource;
  }

  const D3D12ReplayBackend::ResourceSemanticState *event_ordered_previous_resource_semantic(
      trace::ObjectId object_id,
      std::uint64_t create_sequence) const
  {
    const D3D12ReplayBackend::ResourceSemanticState *previous = nullptr;
    for (const auto &resource : backend_.resource_versions_) {
      if (resource.resource_object_id != object_id ||
          resource.create_sequence >= create_sequence) {
        continue;
      }
      if (previous == nullptr ||
          previous->create_sequence < resource.create_sequence) {
        previous = &resource;
      }
    }
    return previous;
  }

  bool event_ordered_resource_has_prior_version(
      trace::ObjectId object_id,
      std::uint64_t create_sequence) const
  {
    return event_ordered_previous_resource_semantic(object_id, create_sequence) != nullptr;
  }

  void materialize_native_resource(
      const trace::EventRecord &event,
      const D3D12ReplayBackend::ResourceSemanticState &resource)
  {
    NativeResource native;
    std::string error;
    if (!make_native_resource(resource, native, error)) {
      if (resource.reserved_resource) {
        error += "; TODO(event-ordered P3): tile mapping materialization is handled separately";
      }
      event_ordered_record_materialize_error(event, std::move(error));
      return;
    }

    const trace::ObjectId object_id = resource.resource_object_id;
    const bool has_prior_version =
        event_ordered_resource_has_prior_version(object_id, resource.create_sequence);
    const bool tracks_versions = resource_version_natives_.find(object_id) != resource_version_natives_.end();

    NativeResource version_native;
    if (has_prior_version || tracks_versions) {
      if (!make_native_resource(resource, version_native, error)) {
        if (resource.reserved_resource) {
          error += "; TODO(event-ordered P3): tile mapping materialization is handled separately";
        }
        event_ordered_record_materialize_error(event, std::move(error));
        return;
      }
    }

    if (has_prior_version || tracks_versions) {
      auto &versions = resource_version_natives_[object_id];
      if (versions.empty()) {
        const auto existing = resources_.find(object_id);
        const auto *previous =
            event_ordered_previous_resource_semantic(object_id, resource.create_sequence);
        if (existing != resources_.end() && previous != nullptr) {
          versions.emplace_back(previous->create_sequence, std::move(existing->second));
          resources_.erase(existing);
        }
      }
      versions.emplace_back(resource.create_sequence, std::move(version_native));
      std::stable_sort(versions.begin(), versions.end(),
                       [](const auto &a, const auto &b) { return a.first < b.first; });
    }

    resources_[object_id] = std::move(native);
  }
