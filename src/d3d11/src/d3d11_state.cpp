#include "apitrace/d3d11_state.hpp"

namespace apitrace::d3d11 {

void D3D11ObjectRegistry::track(const D3D11TrackedObject &object)
{
  objects_[object.object_id] = object;

  // TODO: validate object kind transitions when real D3D11 object creation paths are wired in.
}

void D3D11ObjectRegistry::forget(trace::ObjectId object_id)
{
  objects_.erase(object_id);

  // TODO: surface lifetime diagnostics when replay destroys objects out of order.
}

bool D3D11ObjectRegistry::contains(trace::ObjectId object_id) const noexcept
{
  return objects_.find(object_id) != objects_.end();
}

} // namespace apitrace::d3d11
