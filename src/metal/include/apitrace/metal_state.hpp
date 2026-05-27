#pragma once

#include "apitrace/metal_event_types.hpp"
#include "apitrace/object_types.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace apitrace::metal {

struct MetalTrackedObject {
  trace::ObjectId object_id = 0;
  trace::ObjectKind kind = trace::ObjectKind::Unknown;
  trace::MetalAssetKind asset_kind = trace::MetalAssetKind::Buffer;
  std::vector<trace::BlobId> blob_refs;
  std::filesystem::path asset_relative_path;
  std::string payload;
  std::string debug_label;

  // TODO: add native resource, pipeline, and encoder metadata once replay-state reconstruction starts.
};

class MetalObjectRegistry {
public:
  void track(const MetalTrackedObject &object);
  void forget(trace::ObjectId object_id);

  bool contains(trace::ObjectId object_id) const noexcept;
  const MetalTrackedObject *find(trace::ObjectId object_id) const noexcept;
  std::vector<MetalTrackedObject> find_by_asset_kind(trace::MetalAssetKind asset_kind) const;
  std::vector<trace::ObjectRecord> snapshot_object_records() const;

private:
  std::unordered_map<trace::ObjectId, MetalTrackedObject> objects_;

  // TODO: split long-lived resource objects from per-frame encoder state once Metal trace state boundaries are clearer.
};

} // namespace apitrace::metal
