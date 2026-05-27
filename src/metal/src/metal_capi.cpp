#include "apitrace/metal_capi.hpp"

#include "apitrace/api_types.hpp"
#include "apitrace/metal_event_types.hpp"
#include "apitrace/translation_trace_recorder.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace apitrace::metal {

namespace detail {

thread_local std::uint64_t g_current_d3d_sequence = 0;

std::string copy_c_string(const char *text)
{
  return text == nullptr ? std::string() : std::string(text);
}

std::string dump_json(const json &payload)
{
  return payload.empty() ? std::string("{}") : payload.dump();
}

std::string scope_kind_name(apitrace_metal_scope_kind scope)
{
  switch (scope) {
  case APITRACE_METAL_SCOPE_COMMAND_BUFFER:
    return "command_buffer";
  case APITRACE_METAL_SCOPE_ENCODER:
    return "encoder";
  case APITRACE_METAL_SCOPE_DRAW_TO_METAL:
    return "draw_to_metal_calls";
  }
  return "opaque";
}

struct SessionState {
  std::mutex mutex;
  trace::TraceBundleWriter writer;
  MetalBridge bridge{MetalBridgeOptions{}};
  TranslationTraceRecorder recorder;
  MetalObjectRegistry object_registry;
  bool open = false;
};

std::vector<trace::BlobId> blob_refs_for_asset(const trace::AssetRecord &asset)
{
  if (asset.blob_id == 0) {
    return {};
  }

  return {asset.blob_id};
}

trace::ObjectKind object_kind_for_asset(trace::MetalAssetKind kind)
{
  switch (kind) {
  case trace::MetalAssetKind::Buffer:
  case trace::MetalAssetKind::Texture:
  case trace::MetalAssetKind::Heap:
    return trace::ObjectKind::Resource;
  case trace::MetalAssetKind::RenderPipeline:
  case trace::MetalAssetKind::ComputePipeline:
  case trace::MetalAssetKind::DepthStencilState:
  case trace::MetalAssetKind::SamplerState:
  case trace::MetalAssetKind::ArgumentEncoder:
    return trace::ObjectKind::PipelineState;
  case trace::MetalAssetKind::Library:
  default:
    return trace::ObjectKind::Shader;
  }
}

void track_object(
    SessionState &state,
    std::uint64_t object_id,
    trace::MetalAssetKind asset_kind,
    const trace::AssetRecord &asset,
    std::string payload)
{
  if (object_id == 0) {
    return;
  }

  MetalTrackedObject tracked;
  tracked.object_id = object_id;
  tracked.kind = object_kind_for_asset(asset_kind);
  tracked.asset_kind = asset_kind;
  tracked.blob_refs = blob_refs_for_asset(asset);
  tracked.asset_relative_path = asset.relative_path;
  tracked.payload = std::move(payload);
  tracked.debug_label = asset.debug_name.empty() ? asset.relative_path.filename().generic_string() : asset.debug_name;
  state.object_registry.track(tracked);
}

std::uint64_t record_metal_call(
    SessionState &state,
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    std::vector<std::uint64_t> object_refs,
    std::vector<trace::BlobId> blob_refs,
    const json &payload,
    std::uint64_t frame_id = 0)
{
  MetalTraceRecord trace_record;
  trace_record.call_kind = call_kind;
  trace_record.d3d_sequence = g_current_d3d_sequence;
  trace_record.frame_id = frame_id;
  trace_record.object_id = object_id;
  trace_record.object_refs = std::move(object_refs);
  trace_record.blob_refs = std::move(blob_refs);
  trace_record.translated_call_name = std::move(function_name);
  trace_record.translation_link_payload = dump_json(payload);
  state.recorder.record_metal_call(trace_record);
  return state.recorder.current_metal_sequence();
}

std::uint64_t record_metal_call(
    SessionState &state,
    trace::MetalCallKind call_kind,
    std::string function_name,
    std::uint64_t object_id,
    const json &payload)
{
  return record_metal_call(
      state,
      call_kind,
      std::move(function_name),
      object_id,
      {},
      {},
      payload);
}

std::vector<std::uint8_t> copy_bytes(const void *bytes, std::uint64_t size)
{
  std::vector<std::uint8_t> copied;
  if (bytes == nullptr || size == 0) {
    return copied;
  }
  copied.resize(static_cast<std::size_t>(size));
  std::memcpy(copied.data(), bytes, static_cast<std::size_t>(size));
  return copied;
}

trace::AssetRecord make_asset_record(std::uint64_t object_id, std::string debug_name, const void *bytes, std::uint64_t size)
{
  trace::AssetRecord asset;
  asset.blob_id = object_id;
  asset.debug_name = std::move(debug_name);
  asset.payload_bytes = copy_bytes(bytes, size);
  return asset;
}

void append_link(
    SessionState &state,
    apitrace_metal_scope_kind scope,
    std::uint64_t d3d_sequence,
    std::uint64_t metal_sequence_begin,
    std::uint64_t metal_sequence_end,
    std::uint64_t frame_id,
    const char *payload_utf8)
{
  trace::TranslationLinkRecord record;
  record.scope_kind = scope_kind_name(scope);
  record.d3d_sequence = d3d_sequence;
  record.metal_sequence_begin = metal_sequence_begin;
  record.metal_sequence_end = metal_sequence_end;
  record.frame_id = frame_id;
  record.payload = copy_c_string(payload_utf8);
  state.recorder.append_link_record(record);
}

} // namespace detail

} // namespace apitrace::metal

struct apitrace_metal_session {
  apitrace::metal::detail::SessionState state;
};

APITRACE_METAL_API apitrace_metal_session_t *apitrace_metal_session_open(const char *bundle_root)
{
  if (bundle_root == nullptr || *bundle_root == '\0') {
    return nullptr;
  }

  auto session = std::make_unique<apitrace_metal_session>();
  auto &state = session->state;
  if (!state.writer.open(bundle_root, apitrace::trace::TraceBundleOpenMode::SidebandOnly)) {
    return nullptr;
  }

  if (!state.bridge.initialize()) {
    state.writer.close();
    return nullptr;
  }

  apitrace::metal::TranslationTraceRecorderOptions recorder_options;
  recorder_options.enable_metal_trace = true;
  recorder_options.trace_label = "metal_capi";
  apitrace::trace::TranslationLinkStreamOptions link_options;
  link_options.producer_name = "apitrace_metal_capi";
  if (!state.recorder.open(state.writer, state.bridge, std::move(recorder_options), std::move(link_options))) {
    state.bridge.shutdown();
    state.writer.close();
    return nullptr;
  }

  state.open = true;
  return session.release();
}

APITRACE_METAL_API void apitrace_metal_session_close(apitrace_metal_session_t *session)
{
  if (session == nullptr) {
    return;
  }

  auto &state = session->state;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.open) {
      state.recorder.close();
      state.bridge.shutdown();
      state.writer.close();
      state.open = false;
    }
  }
  delete session;
}

APITRACE_METAL_API void apitrace_metal_set_current_d3d_sequence(
    apitrace_metal_session_t *session,
    uint64_t d3d_seq)
{
  (void)session;
  apitrace::metal::detail::g_current_d3d_sequence = d3d_seq;
}

APITRACE_METAL_API uint64_t apitrace_metal_current_metal_sequence(apitrace_metal_session_t *session)
{
  if (session == nullptr) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  return session->state.recorder.current_metal_sequence();
}

#define APITRACE_METAL_RECORD_JSON(function_name_literal, call_kind_value, object_id_value, ...)                     \
  do {                                                                                                                \
    if (session == nullptr) {                                                                                         \
      return;                                                                                                         \
    }                                                                                                                 \
    std::lock_guard<std::mutex> lock(session->state.mutex);                                                           \
    const auto payload = (__VA_ARGS__);                                                                               \
    apitrace::metal::detail::record_metal_call(session->state, call_kind_value, function_name_literal, object_id_value, payload); \
  } while (false)

#define APITRACE_METAL_RECORD_JSON_RET(function_name_literal, call_kind_value, object_id_value, ...)                 \
  do {                                                                                                                \
    if (session == nullptr) {                                                                                         \
      return 0;                                                                                                       \
    }                                                                                                                 \
    std::lock_guard<std::mutex> lock(session->state.mutex);                                                           \
    const auto payload = (__VA_ARGS__);                                                                               \
    return apitrace::metal::detail::record_metal_call(session->state, call_kind_value, function_name_literal, object_id_value, payload); \
  } while (false)

APITRACE_METAL_API uint64_t apitrace_metal_command_buffer_begin(
    apitrace_metal_session_t *session,
    uint64_t command_buffer_object_id,
    uint64_t frame_id,
    const char *label_utf8)
{
  if (session == nullptr) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  apitrace::metal::TranslationCommandBufferInfo info;
  info.frame_id = frame_id;
  info.command_buffer_id = command_buffer_object_id;
  info.d3d_sequence = apitrace::metal::detail::g_current_d3d_sequence;
  info.label = apitrace::metal::detail::copy_c_string(label_utf8);
  info.payload = json{{"frame_id", frame_id}, {"label", info.label}}.dump();
  session->state.recorder.begin_command_buffer(info);
  return session->state.recorder.current_metal_sequence();
}

APITRACE_METAL_API void apitrace_metal_command_buffer_commit(
    apitrace_metal_session_t *session,
    uint64_t command_buffer_object_id)
{
  (void)command_buffer_object_id;
  if (session == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  session->state.recorder.end_command_buffer();
}

APITRACE_METAL_API uint64_t apitrace_metal_render_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *render_pass_info_json)
{
  if (session == nullptr) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  apitrace::metal::TranslationEncoderInfo info;
  info.encoder_id = encoder_object_id;
  info.command_buffer_id = command_buffer_object_id;
  info.d3d_sequence = apitrace::metal::detail::g_current_d3d_sequence;
  info.pass_kind = apitrace::metal::TranslationPassKind::Render;
  info.payload = json{{"command_buffer_id", command_buffer_object_id},
                      {"render_pass_info", apitrace::metal::detail::copy_c_string(render_pass_info_json)}}
                     .dump();
  session->state.recorder.begin_encoder(info);
  return session->state.recorder.current_metal_sequence();
}

APITRACE_METAL_API void apitrace_metal_render_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id)
{
  (void)encoder_object_id;
  if (session == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  session->state.recorder.end_encoder();
}

APITRACE_METAL_API uint64_t apitrace_metal_compute_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json)
{
  if (session == nullptr) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  apitrace::metal::TranslationEncoderInfo info;
  info.encoder_id = encoder_object_id;
  info.command_buffer_id = command_buffer_object_id;
  info.d3d_sequence = apitrace::metal::detail::g_current_d3d_sequence;
  info.pass_kind = apitrace::metal::TranslationPassKind::Compute;
  info.payload = json{{"command_buffer_id", command_buffer_object_id},
                      {"payload", apitrace::metal::detail::copy_c_string(payload_json)}}
                     .dump();
  session->state.recorder.begin_encoder(info);
  return session->state.recorder.current_metal_sequence();
}

APITRACE_METAL_API void apitrace_metal_compute_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id)
{
  (void)encoder_object_id;
  if (session == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  session->state.recorder.end_encoder();
}

APITRACE_METAL_API uint64_t apitrace_metal_blit_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json)
{
  if (session == nullptr) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  apitrace::metal::TranslationEncoderInfo info;
  info.encoder_id = encoder_object_id;
  info.command_buffer_id = command_buffer_object_id;
  info.d3d_sequence = apitrace::metal::detail::g_current_d3d_sequence;
  info.pass_kind = apitrace::metal::TranslationPassKind::Blit;
  info.payload = json{{"command_buffer_id", command_buffer_object_id},
                      {"payload", apitrace::metal::detail::copy_c_string(payload_json)}}
                     .dump();
  session->state.recorder.begin_encoder(info);
  return session->state.recorder.current_metal_sequence();
}

APITRACE_METAL_API void apitrace_metal_blit_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id)
{
  (void)encoder_object_id;
  if (session == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  session->state.recorder.end_encoder();
}

APITRACE_METAL_API void apitrace_metal_set_render_pipeline_state(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t pipeline_state_object_id)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setRenderPipelineState",
      apitrace::trace::MetalCallKind::SetRenderPipelineState,
      encoder_id,
      json{{"pipeline_state_id", pipeline_state_object_id}});
}

APITRACE_METAL_API void apitrace_metal_set_vertex_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setVertexBuffer",
      apitrace::trace::MetalCallKind::SetVertexBuffer,
      encoder_id,
      json{{"buffer_id", buffer_object_id}, {"offset", offset}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_vertex_texture(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t texture_object_id, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setVertexTexture",
      apitrace::trace::MetalCallKind::SetVertexTexture,
      encoder_id,
      json{{"texture_id", texture_object_id}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_vertex_sampler_state(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t sampler_state_object_id, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setVertexSamplerState",
      apitrace::trace::MetalCallKind::SetVertexSamplerState,
      encoder_id,
      json{{"sampler_state_id", sampler_state_object_id}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_fragment_buffer(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t buffer_object_id, uint64_t offset, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setFragmentBuffer",
      apitrace::trace::MetalCallKind::SetFragmentBuffer,
      encoder_id,
      json{{"buffer_id", buffer_object_id}, {"offset", offset}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_fragment_texture(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t texture_object_id, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setFragmentTexture",
      apitrace::trace::MetalCallKind::SetFragmentTexture,
      encoder_id,
      json{{"texture_id", texture_object_id}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_fragment_sampler_state(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t sampler_state_object_id, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setFragmentSamplerState",
      apitrace::trace::MetalCallKind::SetFragmentSamplerState,
      encoder_id,
      json{{"sampler_state_id", sampler_state_object_id}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_vertex_bytes(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t index, const char *payload_json)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setVertexBytes",
      apitrace::trace::MetalCallKind::SetVertexBytes,
      encoder_id,
      json{{"index", index}, {"payload", apitrace::metal::detail::copy_c_string(payload_json)}});
}

APITRACE_METAL_API void apitrace_metal_set_fragment_bytes(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t index, const char *payload_json)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setFragmentBytes",
      apitrace::trace::MetalCallKind::SetFragmentBytes,
      encoder_id,
      json{{"index", index}, {"payload", apitrace::metal::detail::copy_c_string(payload_json)}});
}

APITRACE_METAL_API void apitrace_metal_set_vertex_buffer_offset(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t offset, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setVertexBufferOffset",
      apitrace::trace::MetalCallKind::SetVertexBufferOffset,
      encoder_id,
      json{{"offset", offset}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_fragment_buffer_offset(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t offset, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.setFragmentBufferOffset",
      apitrace::trace::MetalCallKind::SetFragmentBufferOffset,
      encoder_id,
      json{{"offset", offset}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_compute_pipeline_state(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t pipeline_state_object_id)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLComputeCommandEncoder.setComputePipelineState",
      apitrace::trace::MetalCallKind::SetComputePipelineState,
      encoder_id,
      json{{"pipeline_state_id", pipeline_state_object_id}});
}

APITRACE_METAL_API void apitrace_metal_set_compute_buffer(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t buffer_object_id, uint64_t offset, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLComputeCommandEncoder.setComputeBuffer",
      apitrace::trace::MetalCallKind::SetComputeBuffer,
      encoder_id,
      json{{"buffer_id", buffer_object_id}, {"offset", offset}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_compute_texture(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t texture_object_id, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLComputeCommandEncoder.setComputeTexture",
      apitrace::trace::MetalCallKind::SetComputeTexture,
      encoder_id,
      json{{"texture_id", texture_object_id}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_compute_sampler_state(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t sampler_state_object_id, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLComputeCommandEncoder.setComputeSamplerState",
      apitrace::trace::MetalCallKind::SetComputeSamplerState,
      encoder_id,
      json{{"sampler_state_id", sampler_state_object_id}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_set_compute_buffer_offset(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t offset, uint32_t index)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLComputeCommandEncoder.setComputeBufferOffset",
      apitrace::trace::MetalCallKind::SetComputeBufferOffset,
      encoder_id,
      json{{"offset", offset}, {"index", index}});
}

APITRACE_METAL_API void apitrace_metal_draw_primitives(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t primitive_type, uint32_t vertex_start, uint32_t vertex_count, uint32_t instance_count, uint32_t base_instance)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.drawPrimitives",
      apitrace::trace::MetalCallKind::DrawPrimitives,
      encoder_id,
      json{{"primitive_type", primitive_type},
           {"vertex_start", vertex_start},
           {"vertex_count", vertex_count},
           {"instance_count", instance_count},
           {"base_instance", base_instance}});
}

APITRACE_METAL_API void apitrace_metal_draw_indexed_primitives(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t primitive_type, uint32_t index_count, uint32_t index_type, uint64_t index_buffer_id, uint64_t index_buffer_offset, uint32_t instance_count, int32_t base_vertex, uint32_t base_instance)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.drawIndexedPrimitives",
      apitrace::trace::MetalCallKind::DrawIndexedPrimitives,
      encoder_id,
      json{{"primitive_type", primitive_type},
           {"index_count", index_count},
           {"index_type", index_type},
           {"index_buffer_id", index_buffer_id},
           {"index_buffer_offset", index_buffer_offset},
           {"instance_count", instance_count},
           {"base_vertex", base_vertex},
           {"base_instance", base_instance}});
}

APITRACE_METAL_API void apitrace_metal_draw_primitives_indirect(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t primitive_type, uint64_t indirect_buffer_id, uint64_t indirect_buffer_offset)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.drawPrimitivesIndirect",
      apitrace::trace::MetalCallKind::DrawPrimitivesIndirect,
      encoder_id,
      json{{"primitive_type", primitive_type},
           {"indirect_buffer_id", indirect_buffer_id},
           {"indirect_buffer_offset", indirect_buffer_offset}});
}

APITRACE_METAL_API void apitrace_metal_draw_indexed_primitives_indirect(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t primitive_type, uint32_t index_type, uint64_t index_buffer_id, uint64_t index_buffer_offset, uint64_t indirect_buffer_id, uint64_t indirect_buffer_offset)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.drawIndexedPrimitivesIndirect",
      apitrace::trace::MetalCallKind::DrawIndexedPrimitivesIndirect,
      encoder_id,
      json{{"primitive_type", primitive_type},
           {"index_type", index_type},
           {"index_buffer_id", index_buffer_id},
           {"index_buffer_offset", index_buffer_offset},
           {"indirect_buffer_id", indirect_buffer_id},
           {"indirect_buffer_offset", indirect_buffer_offset}});
}

APITRACE_METAL_API void apitrace_metal_dispatch_threadgroups(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t tgx, uint32_t tgy, uint32_t tgz, uint32_t tx, uint32_t ty, uint32_t tz)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLComputeCommandEncoder.dispatchThreadgroups",
      apitrace::trace::MetalCallKind::DispatchThreadgroups,
      encoder_id,
      json{{"tgx", tgx}, {"tgy", tgy}, {"tgz", tgz}, {"tx", tx}, {"ty", ty}, {"tz", tz}});
}

APITRACE_METAL_API void apitrace_metal_dispatch_threadgroups_indirect(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t indirect_buffer_id, uint64_t indirect_buffer_offset, uint32_t tx, uint32_t ty, uint32_t tz)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLComputeCommandEncoder.dispatchThreadgroupsIndirect",
      apitrace::trace::MetalCallKind::DispatchThreadgroupsIndirect,
      encoder_id,
      json{{"indirect_buffer_id", indirect_buffer_id},
           {"indirect_buffer_offset", indirect_buffer_offset},
           {"tx", tx},
           {"ty", ty},
           {"tz", tz}});
}

APITRACE_METAL_API void apitrace_metal_copy_buffer(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t source_buffer_id, uint64_t source_offset, uint64_t destination_buffer_id, uint64_t destination_offset, uint64_t size)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLBlitCommandEncoder.copyFromBuffer",
      apitrace::trace::MetalCallKind::CopyBuffer,
      encoder_id,
      json{{"source_buffer_id", source_buffer_id},
           {"source_offset", source_offset},
           {"destination_buffer_id", destination_buffer_id},
           {"destination_offset", destination_offset},
           {"size", size}});
}

APITRACE_METAL_API void apitrace_metal_copy_texture(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t source_texture_id, uint64_t destination_texture_id, const char *payload_json)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLBlitCommandEncoder.copyFromTexture",
      apitrace::trace::MetalCallKind::CopyTexture,
      encoder_id,
      json{{"source_texture_id", source_texture_id},
           {"destination_texture_id", destination_texture_id},
           {"payload", apitrace::metal::detail::copy_c_string(payload_json)}});
}

APITRACE_METAL_API void apitrace_metal_blit_fill(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t buffer_object_id, uint64_t range_start, uint64_t range_length, uint32_t value)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLBlitCommandEncoder.fillBuffer",
      apitrace::trace::MetalCallKind::BlitFill,
      encoder_id,
      json{{"buffer_id", buffer_object_id}, {"range_start", range_start}, {"range_length", range_length}, {"value", value}});
}

APITRACE_METAL_API void apitrace_metal_use_resource(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t resource_id, uint32_t usage, uint32_t stages)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.useResource",
      apitrace::trace::MetalCallKind::UseResource,
      encoder_id,
      json{{"resource_id", resource_id}, {"usage", usage}, {"stages", stages}});
}

APITRACE_METAL_API void apitrace_metal_use_resources(apitrace_metal_session_t *session, uint64_t encoder_id, const uint64_t *resource_ids, uint32_t count, uint32_t usage, uint32_t stages)
{
  json resources = json::array();
  for (uint32_t index = 0; index < count; ++index) {
    resources.push_back(resource_ids[index]);
  }
  APITRACE_METAL_RECORD_JSON(
      "MTLRenderCommandEncoder.useResources",
      apitrace::trace::MetalCallKind::UseResources,
      encoder_id,
      json{{"resource_ids", std::move(resources)}, {"usage", usage}, {"stages", stages}});
}

APITRACE_METAL_API void apitrace_metal_use_heap(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t heap_id)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.useHeap",
      apitrace::trace::MetalCallKind::UseHeap,
      encoder_id,
      json{{"heap_id", heap_id}});
}

APITRACE_METAL_API void apitrace_metal_set_argument_buffer(apitrace_metal_session_t *session, uint64_t encoder_id, uint32_t index, uint64_t buffer_id, uint64_t offset)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.setArgumentBuffer",
      apitrace::trace::MetalCallKind::SetArgumentBuffer,
      encoder_id,
      json{{"index", index}, {"buffer_id", buffer_id}, {"offset", offset}});
}

APITRACE_METAL_API void apitrace_metal_memory_barrier(apitrace_metal_session_t *session, uint64_t encoder_id, const char *payload_json)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.memoryBarrier",
      apitrace::trace::MetalCallKind::MemoryBarrier,
      encoder_id,
      json{{"payload", apitrace::metal::detail::copy_c_string(payload_json)}});
}

APITRACE_METAL_API void apitrace_metal_update_fence(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t fence_id, uint32_t stages)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.updateFence",
      apitrace::trace::MetalCallKind::UpdateFence,
      encoder_id,
      json{{"fence_id", fence_id}, {"stages", stages}});
}

APITRACE_METAL_API void apitrace_metal_wait_for_fence(apitrace_metal_session_t *session, uint64_t encoder_id, uint64_t fence_id, uint32_t stages)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.waitForFence",
      apitrace::trace::MetalCallKind::WaitForFence,
      encoder_id,
      json{{"fence_id", fence_id}, {"stages", stages}});
}

APITRACE_METAL_API void apitrace_metal_present_drawable(apitrace_metal_session_t *session, uint64_t command_buffer_id, uint64_t drawable_id, uint64_t frame_index, uint32_t width, uint32_t height, uint32_t sync_interval, uint32_t flags)
{
  if (session == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::PresentDrawable,
      "MTLCommandBuffer.presentDrawable",
      command_buffer_id,
      {drawable_id},
      {},
      json{{"drawable_id", drawable_id},
           {"frame_index", frame_index},
           {"width", width},
           {"height", height},
           {"sync_interval", sync_interval},
           {"flags", flags}},
      frame_index);
}

APITRACE_METAL_API void apitrace_metal_push_debug_group(apitrace_metal_session_t *session, uint64_t encoder_id, const char *label_utf8)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.pushDebugGroup",
      apitrace::trace::MetalCallKind::PushDebugGroup,
      encoder_id,
      json{{"label", apitrace::metal::detail::copy_c_string(label_utf8)}});
}

APITRACE_METAL_API void apitrace_metal_pop_debug_group(apitrace_metal_session_t *session, uint64_t encoder_id)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.popDebugGroup",
      apitrace::trace::MetalCallKind::PopDebugGroup,
      encoder_id,
      json::object());
}

APITRACE_METAL_API void apitrace_metal_insert_debug_signpost(apitrace_metal_session_t *session, uint64_t encoder_id, const char *label_utf8)
{
  APITRACE_METAL_RECORD_JSON(
      "MTLCommandEncoder.insertDebugSignpost",
      apitrace::trace::MetalCallKind::InsertDebugSignpost,
      encoder_id,
      json{{"label", apitrace::metal::detail::copy_c_string(label_utf8)}});
}

APITRACE_METAL_API uint64_t apitrace_metal_register_buffer(apitrace_metal_session_t *session, uint64_t object_id, uint64_t length, uint32_t storage_mode, const void *initial_bytes, uint64_t initial_bytes_size)
{
  if (session == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(session->state.mutex);
  auto asset = apitrace::metal::detail::make_asset_record(object_id, "buffer", initial_bytes, initial_bytes_size);
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::Buffer, asset);
  const auto payload = json{{"length", length},
                            {"storage_mode", storage_mode},
                            {"initial_bytes_size", initial_bytes_size},
                            {"buffer_path", asset.relative_path.generic_string()}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::Buffer,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newBuffer",
      object_id,
      {},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_texture(apitrace_metal_session_t *session, uint64_t object_id, const char *descriptor_json)
{
  if (session == nullptr) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  const auto payload = json{{"descriptor", apitrace::metal::detail::copy_c_string(descriptor_json)}};
  apitrace::trace::AssetRecord asset;
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::Texture,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newTexture",
      object_id,
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_library(apitrace_metal_session_t *session, uint64_t object_id, const void *metallib_bytes, uint64_t size)
{
  if (session == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(session->state.mutex);
  auto asset = apitrace::metal::detail::make_asset_record(object_id, "library", metallib_bytes, size);
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::Library, asset);
  const auto payload = json{{"library_path", asset.relative_path.generic_string()}, {"size", size}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::Library,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newLibrary",
      object_id,
      {},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_render_pipeline(apitrace_metal_session_t *session, uint64_t object_id, const char *descriptor_json, uint64_t vs_function_id, uint64_t fs_function_id)
{
  if (session == nullptr) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  const auto descriptor = apitrace::metal::detail::copy_c_string(descriptor_json);
  auto asset = apitrace::metal::detail::make_asset_record(
      object_id,
      "render_pipeline",
      descriptor.data(),
      static_cast<std::uint64_t>(descriptor.size()));
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::RenderPipeline, asset);
  const auto payload = json{{"descriptor_path", asset.relative_path.generic_string()},
                            {"vs_function_id", vs_function_id},
                            {"fs_function_id", fs_function_id}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::RenderPipeline,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newRenderPipelineState",
      object_id,
      {vs_function_id, fs_function_id},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API uint64_t apitrace_metal_register_compute_pipeline(apitrace_metal_session_t *session, uint64_t object_id, const char *descriptor_json, uint64_t cs_function_id)
{
  if (session == nullptr) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(session->state.mutex);
  const auto descriptor = apitrace::metal::detail::copy_c_string(descriptor_json);
  auto asset = apitrace::metal::detail::make_asset_record(
      object_id,
      "compute_pipeline",
      descriptor.data(),
      static_cast<std::uint64_t>(descriptor.size()));
  asset = session->state.writer.register_metal_asset(apitrace::trace::MetalAssetKind::ComputePipeline, asset);
  const auto payload = json{{"descriptor_path", asset.relative_path.generic_string()}, {"cs_function_id", cs_function_id}};
  apitrace::metal::detail::track_object(
      session->state,
      object_id,
      apitrace::trace::MetalAssetKind::ComputePipeline,
      asset,
      payload.dump());
  return apitrace::metal::detail::record_metal_call(
      session->state,
      apitrace::trace::MetalCallKind::DeviceCreate,
      "MTLDevice.newComputePipelineState",
      object_id,
      {cs_function_id},
      apitrace::metal::detail::blob_refs_for_asset(asset),
      payload);
}

APITRACE_METAL_API void apitrace_metal_emit_link(
    apitrace_metal_session_t *session,
    apitrace_metal_scope_kind scope,
    uint64_t d3d_sequence,
    uint64_t metal_sequence_begin,
    uint64_t metal_sequence_end,
    const char *payload_utf8)
{
  if (session == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(session->state.mutex);
  apitrace::metal::detail::append_link(
      session->state,
      scope,
      d3d_sequence,
      metal_sequence_begin,
      metal_sequence_end,
      0,
      payload_utf8);
}
