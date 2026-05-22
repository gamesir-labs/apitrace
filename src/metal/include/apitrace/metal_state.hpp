#pragma once

#include "apitrace/object_types.hpp"

#include <string>
#include <unordered_map>

namespace apitrace::metal {

struct MetalTrackedObject {
  trace::ObjectId object_id = 0;
  trace::ObjectKind kind = trace::ObjectKind::Unknown;
  std::string debug_label;

  // TODO: add native resource, pipeline, and encoder metadata once replay-state reconstruction starts.
};

class MetalObjectRegistry {
public:
  void track(const MetalTrackedObject &object);
  void forget(trace::ObjectId object_id);

  bool contains(trace::ObjectId object_id) const noexcept;

private:
  std::unordered_map<trace::ObjectId, MetalTrackedObject> objects_;

  // TODO: split long-lived resource objects from per-frame encoder state once Metal trace state boundaries are clearer.
};

} // namespace apitrace::metal
