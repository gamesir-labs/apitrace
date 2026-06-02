#include "metal_callstream_writer.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string_view>

namespace apitrace::trace::detail {

namespace {

using json = nlohmann::json;

std::string_view metal_call_kind_name(MetalCallKind kind)
{
  switch (kind) {
  case MetalCallKind::Unknown:
    return "unknown";
  case MetalCallKind::DeviceCreate:
    return "device_create";
  case MetalCallKind::CommandQueueCreate:
    return "command_queue_create";
  case MetalCallKind::CommandBufferBegin:
    return "command_buffer_begin";
  case MetalCallKind::CommandBufferCommit:
    return "command_buffer_commit";
  case MetalCallKind::RenderEncoderBegin:
    return "render_encoder_begin";
  case MetalCallKind::RenderEncoderEnd:
    return "render_encoder_end";
  case MetalCallKind::ComputeEncoderBegin:
    return "compute_encoder_begin";
  case MetalCallKind::ComputeEncoderEnd:
    return "compute_encoder_end";
  case MetalCallKind::BlitEncoderBegin:
    return "blit_encoder_begin";
  case MetalCallKind::BlitEncoderEnd:
    return "blit_encoder_end";
  case MetalCallKind::BlitEncoderBatch:
    return "blit_encoder_batch";
  case MetalCallKind::SetRenderPipelineState:
    return "set_render_pipeline_state";
  case MetalCallKind::SetVertexBuffer:
    return "set_vertex_buffer";
  case MetalCallKind::SetVertexTexture:
    return "set_vertex_texture";
  case MetalCallKind::SetVertexSamplerState:
    return "set_vertex_sampler_state";
  case MetalCallKind::SetFragmentBuffer:
    return "set_fragment_buffer";
  case MetalCallKind::SetFragmentTexture:
    return "set_fragment_texture";
  case MetalCallKind::SetFragmentSamplerState:
    return "set_fragment_sampler_state";
  case MetalCallKind::SetVertexBytes:
    return "set_vertex_bytes";
  case MetalCallKind::SetFragmentBytes:
    return "set_fragment_bytes";
  case MetalCallKind::SetCullMode:
    return "set_cull_mode";
  case MetalCallKind::SetFrontFacingWinding:
    return "set_front_facing_winding";
  case MetalCallKind::SetTriangleFillMode:
    return "set_triangle_fill_mode";
  case MetalCallKind::SetDepthStencilState:
    return "set_depth_stencil_state";
  case MetalCallKind::SetViewport:
    return "set_viewport";
  case MetalCallKind::SetScissorRect:
    return "set_scissor_rect";
  case MetalCallKind::DrawPrimitives:
    return "draw_primitives";
  case MetalCallKind::DrawIndexedPrimitives:
    return "draw_indexed_primitives";
  case MetalCallKind::DrawPrimitivesIndirect:
    return "draw_primitives_indirect";
  case MetalCallKind::DrawIndexedPrimitivesIndirect:
    return "draw_indexed_primitives_indirect";
  case MetalCallKind::SetComputePipelineState:
    return "set_compute_pipeline_state";
  case MetalCallKind::SetComputeBuffer:
    return "set_compute_buffer";
  case MetalCallKind::SetComputeTexture:
    return "set_compute_texture";
  case MetalCallKind::SetComputeSamplerState:
    return "set_compute_sampler_state";
  case MetalCallKind::DispatchThreadgroups:
    return "dispatch_threadgroups";
  case MetalCallKind::DispatchThreadgroupsIndirect:
    return "dispatch_threadgroups_indirect";
  case MetalCallKind::DispatchThreadsPerTile:
    return "dispatch_threads_per_tile";
  case MetalCallKind::BlitBatch:
    return "blit_batch";
  case MetalCallKind::CopyBuffer:
    return "copy_buffer";
  case MetalCallKind::CopyBufferToTexture:
    return "copy_buffer_to_texture";
  case MetalCallKind::CopyTexture:
    return "copy_texture";
  case MetalCallKind::BlitFill:
    return "blit_fill";
  case MetalCallKind::UseResource:
    return "use_resource";
  case MetalCallKind::UseResources:
    return "use_resources";
  case MetalCallKind::UseHeap:
    return "use_heap";
  case MetalCallKind::SetVertexBufferOffset:
    return "set_vertex_buffer_offset";
  case MetalCallKind::SetFragmentBufferOffset:
    return "set_fragment_buffer_offset";
  case MetalCallKind::SetComputeBufferOffset:
    return "set_compute_buffer_offset";
  case MetalCallKind::SetArgumentBuffer:
    return "set_argument_buffer";
  case MetalCallKind::EncoderState:
    return "encoder_state";
  case MetalCallKind::SetComputeBytes:
    return "set_compute_bytes";
  case MetalCallKind::DispatchThreads:
    return "dispatch_threads";
  case MetalCallKind::MemoryBarrier:
    return "memory_barrier";
  case MetalCallKind::FenceOps:
    return "fence_ops";
  case MetalCallKind::UpdateFence:
    return "update_fence";
  case MetalCallKind::WaitForFence:
    return "wait_for_fence";
  case MetalCallKind::PresentDrawable:
    return "present_drawable";
  case MetalCallKind::PushDebugGroup:
    return "push_debug_group";
  case MetalCallKind::PopDebugGroup:
    return "pop_debug_group";
  case MetalCallKind::ObjectMetadata:
    return "object_metadata";
  case MetalCallKind::InsertDebugSignpost:
    return "insert_debug_signpost";
  case MetalCallKind::BufferUpdate:
    return "buffer_update";
  case MetalCallKind::TextureUpdate:
    return "texture_update";
  }
  return "unknown";
}

bool metal_call_kind_from_name(std::string_view name, MetalCallKind &kind)
{
  static constexpr MetalCallKind kKinds[] = {
      MetalCallKind::Unknown,
      MetalCallKind::DeviceCreate,
      MetalCallKind::CommandQueueCreate,
      MetalCallKind::CommandBufferBegin,
      MetalCallKind::CommandBufferCommit,
      MetalCallKind::RenderEncoderBegin,
      MetalCallKind::RenderEncoderEnd,
      MetalCallKind::ComputeEncoderBegin,
      MetalCallKind::ComputeEncoderEnd,
      MetalCallKind::BlitEncoderBegin,
      MetalCallKind::BlitEncoderEnd,
      MetalCallKind::BlitEncoderBatch,
      MetalCallKind::SetRenderPipelineState,
      MetalCallKind::SetVertexBuffer,
      MetalCallKind::SetVertexTexture,
      MetalCallKind::SetVertexSamplerState,
      MetalCallKind::SetFragmentBuffer,
      MetalCallKind::SetFragmentTexture,
      MetalCallKind::SetFragmentSamplerState,
      MetalCallKind::SetVertexBytes,
      MetalCallKind::SetFragmentBytes,
      MetalCallKind::SetCullMode,
      MetalCallKind::SetFrontFacingWinding,
      MetalCallKind::SetTriangleFillMode,
      MetalCallKind::SetDepthStencilState,
      MetalCallKind::SetViewport,
      MetalCallKind::SetScissorRect,
      MetalCallKind::DrawPrimitives,
      MetalCallKind::DrawIndexedPrimitives,
      MetalCallKind::DrawPrimitivesIndirect,
      MetalCallKind::DrawIndexedPrimitivesIndirect,
      MetalCallKind::SetComputePipelineState,
      MetalCallKind::SetComputeBuffer,
      MetalCallKind::SetComputeTexture,
      MetalCallKind::SetComputeSamplerState,
      MetalCallKind::DispatchThreadgroups,
      MetalCallKind::DispatchThreadgroupsIndirect,
      MetalCallKind::DispatchThreadsPerTile,
      MetalCallKind::BlitBatch,
      MetalCallKind::CopyBuffer,
      MetalCallKind::CopyBufferToTexture,
      MetalCallKind::CopyTexture,
      MetalCallKind::BlitFill,
      MetalCallKind::UseResource,
      MetalCallKind::UseResources,
      MetalCallKind::UseHeap,
      MetalCallKind::SetVertexBufferOffset,
      MetalCallKind::SetFragmentBufferOffset,
      MetalCallKind::SetComputeBufferOffset,
      MetalCallKind::SetArgumentBuffer,
      MetalCallKind::EncoderState,
      MetalCallKind::SetComputeBytes,
      MetalCallKind::DispatchThreads,
      MetalCallKind::MemoryBarrier,
      MetalCallKind::FenceOps,
      MetalCallKind::UpdateFence,
      MetalCallKind::WaitForFence,
      MetalCallKind::PresentDrawable,
      MetalCallKind::PushDebugGroup,
      MetalCallKind::PopDebugGroup,
      MetalCallKind::ObjectMetadata,
      MetalCallKind::InsertDebugSignpost,
      MetalCallKind::BufferUpdate,
      MetalCallKind::TextureUpdate,
  };

  for (const auto candidate : kKinds) {
    if (name == metal_call_kind_name(candidate)) {
      kind = candidate;
      return true;
    }
  }
  return false;
}

} // namespace

std::string metal_event_record_json(const MetalEventRecord &event)
{
  std::ostringstream record;
  record << "{\"call_kind\":\"" << metal_call_kind_name(event.call_kind)
         << "\",\"metal_sequence\":" << event.metal_sequence
         << ",\"d3d_sequence\":" << event.d3d_sequence
         << ",\"frame_id\":" << event.frame_id
         << ",\"object_id\":" << event.object_id
         << ",\"object_refs\":[";
  for (std::size_t i = 0; i < event.object_refs.size(); i++) {
    if (i)
      record << ',';
    record << event.object_refs[i];
  }
  record << "],\"blob_refs\":[";
  for (std::size_t i = 0; i < event.blob_refs.size(); i++) {
    if (i)
      record << ',';
    record << event.blob_refs[i];
  }
  record << "]";
  if (!event.function_name.empty()) {
    record << ",\"function\":\"" << event.function_name << "\"";
  }
  if (event.payload_refs_scanned) {
    record << ",\"payload_refs_scanned\":true";
  }
  record << ",\"payload\":"
         << (event.payload.empty() ? "{}" : event.payload)
         << "}";
  return record.str();
}

bool parse_metal_callstream(
    const std::filesystem::path &callstream_path,
    std::vector<MetalEventRecord> &events,
    std::string &error,
    std::uint64_t byte_limit)
{
  std::ifstream input(callstream_path);
  if (!input.is_open()) {
    error = "missing metal callstream: " + callstream_path.filename().generic_string();
    return false;
  }

  events.clear();
  std::string line;
  std::size_t line_number = 0;
  std::uint64_t consumed_bytes = 0;
  while (std::getline(input, line)) {
    const auto line_bytes = static_cast<std::uint64_t>(line.size() + 1);
    if (consumed_bytes + line_bytes > byte_limit) {
      break;
    }
    consumed_bytes += line_bytes;
    ++line_number;
    if (line.empty()) {
      continue;
    }

    json record = json::parse(line, nullptr, false);
    if (record.is_discarded() || !record.is_object()) {
      std::ostringstream message;
      message << callstream_path.filename().generic_string() << ": invalid JSON at line " << line_number;
      error = message.str();
      return false;
    }

    MetalEventRecord event;
    if (!record.contains("call_kind") || !record["call_kind"].is_string()) {
      std::ostringstream message;
      message << callstream_path.filename().generic_string() << ": line " << line_number << " missing call_kind";
      error = message.str();
      return false;
    }
    if (!metal_call_kind_from_name(record["call_kind"].get<std::string>(), event.call_kind)) {
      std::ostringstream message;
      message << callstream_path.filename().generic_string() << ": line " << line_number << " has unknown call_kind";
      error = message.str();
      return false;
    }

    event.metal_sequence = record.value("metal_sequence", 0ull);
    event.d3d_sequence = record.value("d3d_sequence", 0ull);
    event.frame_id = record.value("frame_id", 0ull);
    event.object_id = record.value("object_id", 0ull);
    event.function_name = record.value("function", std::string());
    event.payload_refs_scanned = record.value("payload_refs_scanned", false);
    if (event.metal_sequence == 0) {
      std::ostringstream message;
      message << callstream_path.filename().generic_string() << ": line " << line_number
              << " missing metal_sequence";
      error = message.str();
      return false;
    }

    if (record.contains("object_refs") && record["object_refs"].is_array()) {
      event.object_refs = record["object_refs"].get<std::vector<std::uint64_t>>();
    }
    if (record.contains("blob_refs") && record["blob_refs"].is_array()) {
      event.blob_refs = record["blob_refs"].get<std::vector<BlobId>>();
    }

    const auto payload_it = record.find("payload");
    event.payload = payload_it == record.end() ? std::string("{}") : payload_it->dump();
    events.push_back(std::move(event));
  }

  error.clear();
  return true;
}

std::string metal_asset_directory_name(MetalAssetKind kind)
{
  switch (kind) {
  case MetalAssetKind::Library:
    return std::string(kMetalDirectoryName) + "/" + kMetalLibrariesDirectoryName;
  case MetalAssetKind::RenderPipeline:
  case MetalAssetKind::ComputePipeline:
  case MetalAssetKind::DepthStencilState:
  case MetalAssetKind::SamplerState:
  case MetalAssetKind::ArgumentEncoder:
  case MetalAssetKind::Heap:
    return std::string(kMetalDirectoryName) + "/" + kMetalPipelinesDirectoryName;
  case MetalAssetKind::Buffer:
    return std::string(kMetalDirectoryName) + "/" + kMetalBuffersDirectoryName;
  case MetalAssetKind::Texture:
    return std::string(kMetalDirectoryName) + "/" + kMetalTexturesDirectoryName;
  }
  return std::string(kMetalDirectoryName);
}

std::string metal_asset_extension(MetalAssetKind kind)
{
  switch (kind) {
  case MetalAssetKind::Library:
    return ".metallib";
  case MetalAssetKind::RenderPipeline:
  case MetalAssetKind::ComputePipeline:
  case MetalAssetKind::DepthStencilState:
  case MetalAssetKind::SamplerState:
  case MetalAssetKind::ArgumentEncoder:
  case MetalAssetKind::Heap:
    return ".pipeline.json";
  case MetalAssetKind::Buffer:
    return ".buffer";
  case MetalAssetKind::Texture:
    return ".texture";
  }
  return ".bin";
}

bool is_metal_asset_path(const std::filesystem::path &relative_path, MetalAssetKind *kind)
{
  const auto generic = relative_path.generic_string();
  const auto prefix = std::string(kMetalDirectoryName) + "/";
  if (generic.rfind(prefix, 0) != 0) {
    return false;
  }

  if (kind == nullptr) {
    return true;
  }

  const auto libraries_prefix = prefix + std::string(kMetalLibrariesDirectoryName) + "/";
  const auto pipelines_prefix = prefix + std::string(kMetalPipelinesDirectoryName) + "/";
  const auto buffers_prefix = prefix + std::string(kMetalBuffersDirectoryName) + "/";
  const auto textures_prefix = prefix + std::string(kMetalTexturesDirectoryName) + "/";
  if (generic.rfind(libraries_prefix, 0) == 0) {
    *kind = MetalAssetKind::Library;
    return true;
  }
  if (generic.rfind(pipelines_prefix, 0) == 0) {
    *kind = MetalAssetKind::RenderPipeline;
    return true;
  }
  if (generic.rfind(buffers_prefix, 0) == 0) {
    *kind = MetalAssetKind::Buffer;
    return true;
  }
  if (generic.rfind(textures_prefix, 0) == 0) {
    *kind = MetalAssetKind::Texture;
    return true;
  }
  return false;
}

} // namespace apitrace::trace::detail
