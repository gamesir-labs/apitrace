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

} // namespace apitrace::metal
