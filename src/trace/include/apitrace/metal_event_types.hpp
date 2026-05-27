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
  CopyBuffer,
  CopyTexture,
  BlitFill,
  UseResource,
  UseResources,
  UseHeap,
  SetVertexBufferOffset,
  SetFragmentBufferOffset,
  SetComputeBufferOffset,
  SetArgumentBuffer,
  MemoryBarrier,
  UpdateFence,
  WaitForFence,
  PresentDrawable,
  PushDebugGroup,
  PopDebugGroup,
  InsertDebugSignpost,
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
  std::uint64_t d3d_sequence = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t object_id = 0;
  std::vector<std::uint64_t> object_refs;
  std::vector<BlobId> blob_refs;
  std::string function_name;
  std::string payload;
};

} // namespace apitrace::trace
