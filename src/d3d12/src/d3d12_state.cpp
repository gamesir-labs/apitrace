#include "apitrace/d3d12_state.hpp"

namespace apitrace::d3d12 {

void D3D12ObjectRegistry::track(const D3D12TrackedObject &object)
{
  objects_[object.object_id] = object;

  // TODO: validate object kind transitions once concrete D3D12 creation paths are wired in.
}

void D3D12ObjectRegistry::forget(trace::ObjectId object_id)
{
  objects_.erase(object_id);

  // TODO: surface lifetime diagnostics when replay destroys D3D12 objects out of order.
}

bool D3D12ObjectRegistry::contains(trace::ObjectId object_id) const noexcept
{
  return objects_.find(object_id) != objects_.end();
}

} // namespace apitrace::d3d12
