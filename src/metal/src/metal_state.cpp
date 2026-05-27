#include "apitrace/metal_state.hpp"

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

} // namespace apitrace::metal
