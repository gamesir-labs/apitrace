#pragma once

#include "apitrace/object_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apitrace::trace {

enum class EventKind {
  Call,
  ObjectCreate,
  ObjectDestroy,
  ResourceBlob,
  Boundary,
};

enum class BoundaryKind {
  Frame,
  CommandList,
  Submit,
  Present,
  Fence,
  Barrier,
  DebugMarker,
};

struct Callsite {
  std::uint64_t sequence = 0;
  std::string function_name;
  std::int32_t result_code = 0;

  // TODO: separate function identity from human-readable naming once stable opcodes exist.
};

struct EventRecord {
  EventKind kind = EventKind::Call;
  Callsite callsite;
  BoundaryKind boundary = BoundaryKind::Frame;
  std::vector<ObjectId> object_refs;
  std::vector<BlobId> blob_refs;
  std::string payload;

  // TODO: replace payload string with typed readable event payloads when callstream schema settles.
};

} // namespace apitrace::trace
