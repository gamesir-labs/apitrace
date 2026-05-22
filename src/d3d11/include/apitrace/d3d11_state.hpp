#pragma once

#include "apitrace/object_types.hpp"

#include <string>
#include <unordered_map>

namespace apitrace::d3d11 {

struct D3D11TrackedObject {
  trace::ObjectId object_id = 0;
  trace::ObjectKind kind = trace::ObjectKind::Unknown;
  std::string debug_name;

  // TODO: add view/resource/shader-specific payloads once D3D11 object reconstruction starts.
};

class D3D11ObjectRegistry {
public:
  void track(const D3D11TrackedObject &object);
  void forget(trace::ObjectId object_id);

  bool contains(trace::ObjectId object_id) const noexcept;

private:
  std::unordered_map<trace::ObjectId, D3D11TrackedObject> objects_;

  // TODO: split device-wide objects from context-bound state once replay needs separate lookup paths.
};

} // namespace apitrace::d3d11
