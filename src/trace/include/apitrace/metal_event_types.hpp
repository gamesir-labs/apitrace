#pragma once

#include "apitrace/object_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apitrace::trace {

enum class MetalCallKind {
  Unknown,
  DeviceCreate,
  CommandQueueCreate,
  CommandBufferBegin,
  CommandBufferCommit,
  RenderEncoderBegin,
  RenderEncoderEnd,
  ComputeEncoderBegin,
  ComputeEncoderEnd,
  BlitEncoderBegin,
  BlitEncoderEnd,
  BlitEncoderBatch,
  SetRenderPipelineState,
  SetVertexBuffer,
  SetVertexTexture,
  SetVertexSamplerState,
  SetFragmentBuffer,
  SetFragmentTexture,
  SetFragmentSamplerState,
  SetVertexBytes,
  SetFragmentBytes,
  SetCullMode,
  SetFrontFacingWinding,
  SetTriangleFillMode,
  SetDepthStencilState,
  SetViewport,
  SetScissorRect,
  DrawPrimitives,
  DrawIndexedPrimitives,
  DrawPrimitivesIndirect,
  DrawIndexedPrimitivesIndirect,
  SetComputePipelineState,
  SetComputeBuffer,
  SetComputeTexture,
  SetComputeSamplerState,
  DispatchThreadgroups,
  DispatchThreadgroupsIndirect,
  DispatchThreadsPerTile,
  BlitBatch,
  CopyBuffer,
  CopyBufferToTexture,
  CopyTexture,
  BlitFill,
  UseResource,
  UseResources,
  UseHeap,
  SetVertexBufferOffset,
  SetFragmentBufferOffset,
  SetComputeBufferOffset,
  SetArgumentBuffer,
  EncoderState,
  SetComputeBytes,
  DispatchThreads,
  MemoryBarrier,
  FenceOps,
  UpdateFence,
  WaitForFence,
  PresentDrawable,
  PushDebugGroup,
  PopDebugGroup,
  ObjectMetadata,
  InsertDebugSignpost,
  BufferUpdate,
  TextureUpdate,
};

enum class MetalBoundaryKind {
  Frame,
  CommandBuffer,
  RenderEncoder,
  ComputeEncoder,
  BlitEncoder,
  Present,
  Drawable,
};

enum class MetalAssetKind {
  Library,
  RenderPipeline,
  ComputePipeline,
  DepthStencilState,
  SamplerState,
  Buffer,
  Texture,
  ArgumentEncoder,
  Heap,
};

struct MetalEventRecord {
  MetalCallKind call_kind = MetalCallKind::Unknown;
  std::uint64_t metal_sequence = 0;
  std::uint64_t time_ns = 0;
  std::uint64_t elapsed_ns = 0;
  std::uint64_t d3d_sequence = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t object_id = 0;
  std::vector<std::uint64_t> object_refs;
  std::vector<BlobId> blob_refs;
  std::string function_name;
  std::string payload;
  bool payload_refs_scanned = false;
};

} // namespace apitrace::trace
