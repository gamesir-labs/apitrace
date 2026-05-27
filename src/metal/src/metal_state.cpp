#include "apitrace/metal_state.hpp"

#include <algorithm>

namespace apitrace::metal {

void MetalObjectRegistry::track(const MetalTrackedObject &object)
{
  objects_[object.object_id] = object;
}

void MetalObjectRegistry::forget(trace::ObjectId object_id)
{
  objects_.erase(object_id);
}

bool MetalObjectRegistry::contains(trace::ObjectId object_id) const noexcept
{
  return objects_.find(object_id) != objects_.end();
}

const MetalTrackedObject *MetalObjectRegistry::find(trace::ObjectId object_id) const noexcept
{
  const auto it = objects_.find(object_id);
  return it == objects_.end() ? nullptr : &it->second;
}

std::vector<MetalTrackedObject> MetalObjectRegistry::find_by_asset_kind(trace::MetalAssetKind asset_kind) const
{
  std::vector<MetalTrackedObject> matches;
  for (const auto &[object_id, object] : objects_) {
    (void)object_id;
    if (object.asset_kind == asset_kind) {
      matches.push_back(object);
    }
  }
  return matches;
}

std::vector<trace::ObjectRecord> MetalObjectRegistry::snapshot_object_records() const
{
  std::vector<trace::ObjectRecord> records;
  records.reserve(objects_.size());
  for (const auto &[object_id, object] : objects_) {
    trace::ObjectRecord record;
    record.object_id = object_id;
    record.kind = object.kind;
    record.debug_name = object.debug_label;
    records.push_back(std::move(record));
  }

  std::sort(records.begin(), records.end(), [](const trace::ObjectRecord &lhs, const trace::ObjectRecord &rhs) {
    return lhs.object_id < rhs.object_id;
  });
  return records;
}

} // namespace apitrace::metal
