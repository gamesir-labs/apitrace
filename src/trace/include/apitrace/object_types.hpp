#pragma once

#include <cstdint>
#include <string>

namespace apitrace::trace {

using ObjectId = std::uint64_t;
using BlobId = std::uint64_t;

enum class ObjectKind {
  Unknown,
  Device,
  Context,
  CommandQueue,
  CommandAllocator,
  CommandList,
  CommandSignature,
  Fence,
  SwapChain,
  Heap,
  Resource,
  View,
  Shader,
  PipelineState,
  RootSignature,
  DescriptorHeap,
  QueryHeap,
};

struct ObjectRecord {
  ObjectId object_id = 0;
  ObjectKind kind = ObjectKind::Unknown;
  ObjectId parent_object_id = 0;
  std::string debug_name;

  // TODO: add stable object classification details once readable object indexes carry more semantics.
};

struct ResourceBlobDesc {
  BlobId blob_id = 0;
  std::string debug_name;
  std::uint64_t size_bytes = 0;

  // TODO: move this into a dedicated blob metadata header when asset persistence starts diverging by type.
};

} // namespace apitrace::trace
