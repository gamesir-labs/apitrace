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

enum class EventPayloadEncoding : std::uint8_t {
  Json = 0,
  CompiledNodes = 1,
  CompiledTileMappings = 2,
  CompiledResourceDataUpdate = 3,
};

struct CompiledTileCoordinate {
  std::uint32_t subresource = 0;
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t z = 0;
};

struct CompiledTileRegionSize {
  std::uint32_t num_tiles = 0;
  bool use_box = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t depth = 0;
};

// Route-specific payload compiled by bundle-finalize. Native retrace only decodes these scalar
// arrays; it never reparses the original readable JSON object at submission time.
struct CompiledTileMappingPayload {
  std::uint32_t flags = 0;
  std::vector<CompiledTileCoordinate> regions;
  std::vector<CompiledTileRegionSize> region_sizes;
  std::vector<std::uint32_t> range_flags;
  std::vector<std::uint32_t> heap_range_offsets;
  std::vector<std::uint32_t> range_tile_counts;
};

struct CompiledResourceDataUpdatePayload {
  std::uint64_t apply_sequence = 0;
  std::uint32_t subresource = 0;
  std::uint64_t written_begin = 0;
  std::uint64_t written_end = 0;
  std::string buffer_path;
};

// Native D3D12 dispatch routing is compiled by bundle-finalize. Retrace consumes this value as a
// direct switch key; it must not rediscover the route by scanning the human-readable function
// name on every replayed call.
enum class CompiledDispatchRoute : std::uint32_t {
  Unspecified = 0,
  NoOp = 1,
  ObjectDestroy = 2,
  CreateDevice = 3,
  PresentBoundary = 4,
  PresentCall = 5,
  CreateMemory = 6,
  CreateResource = 7,
  CreateDescriptor = 8,
  CreatePipeline = 9,
  CreateCommandObject = 10,
  RecordCommand = 11,
  CopyDescriptors = 12,
  ExecuteCommandLists = 13,
  UpdateTileMappings = 14,
  QueueSignal = 15,
  QueueWait = 16,
  FenceSignal = 17,
  FenceSetEventOnCompletion = 18,
  ResourceMap = 19,
  ResourceUnmap = 20,
  ResourceDataUpdate = 21,
  DiagnosticStub = 22,
};

enum class CompiledCommandKind : std::uint16_t {
  Unknown,
  BeginCommandList,
  EndCommandList,
  SetPipelineState,
  SetRootSignature,
  SetDescriptorHeaps,
  SetRootDescriptorTable,
  SetRootConstants,
  SetRootConstantBufferView,
  SetViewports,
  SetScissorRects,
  SetRenderTargets,
  ClearRenderTarget,
  ClearDepthStencil,
  ClearUnorderedAccess,
  DiscardResource,
  SetPrimitiveTopology,
  SetVertexBuffers,
  SetIndexBuffer,
  ResourceBarrier,
  SetDynamicState,
  RenderPass,
  Query,
  Predication,
  WriteBufferImmediate,
  TemporalUpscale,
  UnsupportedNative,
  Draw,
  Dispatch,
  ExecuteIndirect,
  ExecuteBundle,
  Copy,
  Resolve,
  MapResource,
  UnmapResource,
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
  std::uint64_t time_ns = 0;
  std::uint64_t elapsed_ns = 0;
  BoundaryKind boundary = BoundaryKind::Frame;
  std::vector<ObjectId> object_refs;
  std::vector<BlobId> blob_refs;
  ObjectId object_id = 0;
  ObjectKind object_kind = ObjectKind::Unknown;
  ObjectId parent_object_id = 0;
  std::string object_debug_name;
  EventPayloadEncoding payload_encoding = EventPayloadEncoding::Json;
  CompiledDispatchRoute dispatch_route = CompiledDispatchRoute::Unspecified;
  CompiledCommandKind command_kind = CompiledCommandKind::Unknown;
  std::string payload;
  CompiledTileMappingPayload compiled_tile_mapping;
  CompiledResourceDataUpdatePayload compiled_resource_data_update;

  // TODO: replace payload string with typed readable event payloads when callstream schema settles.
};

} // namespace apitrace::trace
