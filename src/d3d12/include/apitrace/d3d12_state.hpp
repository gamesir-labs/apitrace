#pragma once

#include "apitrace/object_types.hpp"

#include <string>
#include <unordered_map>

namespace apitrace::d3d12 {

struct D3D12TrackedObject {
  trace::ObjectId object_id = 0;
  trace::ObjectKind kind = trace::ObjectKind::Unknown;
  std::string debug_name;

  // TODO: add queue/list/descriptor/root-signature payloads once D3D12 state reconstruction begins.
};

class D3D12ObjectRegistry {
public:
  void track(const D3D12TrackedObject &object);
  void forget(trace::ObjectId object_id);

  bool contains(trace::ObjectId object_id) const noexcept;

private:
  std::unordered_map<trace::ObjectId, D3D12TrackedObject> objects_;

  // TODO: split descriptor-related state from command-submission state when D3D12 replay becomes concrete.
};

} // namespace apitrace::d3d12
