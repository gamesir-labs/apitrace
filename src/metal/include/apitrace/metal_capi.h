#pragma once

#include <stdint.h>

#if defined(_WIN32) && defined(APITRACE_METAL_SHARED)
#define APITRACE_METAL_IMPORT __declspec(dllimport)
#else
#define APITRACE_METAL_IMPORT
#endif

#ifndef APITRACE_METAL_API
#ifdef __cplusplus
#define APITRACE_METAL_API extern "C" APITRACE_METAL_IMPORT
#else
#define APITRACE_METAL_API APITRACE_METAL_IMPORT
#endif
#endif

typedef struct apitrace_metal_session apitrace_metal_session_t;

typedef struct apitrace_metal_copy_texture_op {
  uint64_t source_texture_id;
  uint64_t destination_texture_id;
  uint64_t source_origin_x;
  uint64_t source_origin_y;
  uint64_t source_origin_z;
  uint64_t source_size_width;
  uint64_t source_size_height;
  uint64_t source_size_depth;
  uint32_t source_slice;
  uint32_t source_level;
  uint64_t destination_origin_x;
  uint64_t destination_origin_y;
  uint64_t destination_origin_z;
  uint32_t destination_slice;
  uint32_t destination_level;
} apitrace_metal_copy_texture_op_t;

typedef enum {
  APITRACE_METAL_SCOPE_COMMAND_BUFFER = 1,
  APITRACE_METAL_SCOPE_ENCODER = 2,
  APITRACE_METAL_SCOPE_DRAW_TO_METAL = 3
} apitrace_metal_scope_kind;

APITRACE_METAL_API apitrace_metal_session_t *apitrace_metal_session_open(const char *bundle_root);
APITRACE_METAL_API void apitrace_metal_session_close(apitrace_metal_session_t *session);
APITRACE_METAL_API void apitrace_metal_session_flush(apitrace_metal_session_t *session);
APITRACE_METAL_API void apitrace_metal_session_seal_checkpoint(apitrace_metal_session_t *session);
APITRACE_METAL_API void apitrace_metal_set_current_d3d_sequence(apitrace_metal_session_t *session, uint64_t d3d_seq);
APITRACE_METAL_API uint64_t apitrace_metal_current_metal_sequence(apitrace_metal_session_t *session);

APITRACE_METAL_API uint64_t apitrace_metal_command_buffer_begin(
    apitrace_metal_session_t *session,
    uint64_t command_buffer_object_id,
    uint64_t frame_id,
    const char *label_utf8);
APITRACE_METAL_API void apitrace_metal_command_buffer_commit(
    apitrace_metal_session_t *session,
    uint64_t command_buffer_object_id);

APITRACE_METAL_API uint64_t apitrace_metal_render_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *render_pass_info_json);
APITRACE_METAL_API void apitrace_metal_render_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id);

APITRACE_METAL_API uint64_t apitrace_metal_compute_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_compute_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id);

APITRACE_METAL_API uint64_t apitrace_metal_blit_encoder_begin(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_blit_encoder_end(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id);
APITRACE_METAL_API void apitrace_metal_blit_encoder_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_blit_encoder_batch_with_command_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_blit_encoder_fence_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const uint64_t *wait_fences,
    uint32_t wait_fence_count,
    const uint64_t *update_fences,
    uint32_t update_fence_count);
APITRACE_METAL_API void apitrace_metal_blit_encoder_copy_texture_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    uint64_t source_texture_id,
    uint64_t destination_texture_id,
    uint64_t source_origin_x,
    uint64_t source_origin_y,
    uint64_t source_origin_z,
    uint64_t source_size_width,
    uint64_t source_size_height,
    uint64_t source_size_depth,
    uint32_t source_slice,
    uint32_t source_level,
    uint64_t destination_origin_x,
    uint64_t destination_origin_y,
    uint64_t destination_origin_z,
    uint32_t destination_slice,
    uint32_t destination_level,
    const uint64_t *wait_fences,
    uint32_t wait_fence_count,
    const uint64_t *update_fences,
    uint32_t update_fence_count);
APITRACE_METAL_API void apitrace_metal_blit_encoder_copy_texture_ops_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_object_id,
    uint64_t command_buffer_object_id,
    const apitrace_metal_copy_texture_op_t *copy_ops,
    uint32_t copy_op_count,
    const uint64_t *wait_fences,
    uint32_t wait_fence_count,
    const uint64_t *update_fences,
    uint32_t update_fence_count);

APITRACE_METAL_API void apitrace_metal_set_render_pipeline_state(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t pipeline_state_object_id);
APITRACE_METAL_API void apitrace_metal_set_vertex_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_vertex_buffer_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index,
    const void *bytes,
    uint64_t bytes_size);
APITRACE_METAL_API void apitrace_metal_set_vertex_texture(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t texture_object_id,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_vertex_sampler_state(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t sampler_state_object_id,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_fragment_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_fragment_buffer_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index,
    const void *bytes,
    uint64_t bytes_size);
APITRACE_METAL_API void apitrace_metal_set_fragment_texture(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t texture_object_id,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_fragment_sampler_state(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t sampler_state_object_id,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_vertex_bytes(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t index,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_set_fragment_bytes(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t index,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_set_vertex_buffer_offset(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t offset,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_fragment_buffer_offset(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t offset,
    uint32_t index);

APITRACE_METAL_API void apitrace_metal_set_compute_pipeline_state(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t pipeline_state_object_id);
APITRACE_METAL_API void apitrace_metal_set_compute_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_compute_buffer_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t offset,
    uint32_t index,
    const void *bytes,
    uint64_t bytes_size);
APITRACE_METAL_API void apitrace_metal_set_compute_texture(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t texture_object_id,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_compute_sampler_state(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t sampler_state_object_id,
    uint32_t index);
APITRACE_METAL_API void apitrace_metal_set_compute_buffer_offset(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t offset,
    uint32_t index);

APITRACE_METAL_API void apitrace_metal_draw_primitives(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t primitive_type,
    uint32_t vertex_start,
    uint32_t vertex_count,
    uint32_t instance_count,
    uint32_t base_instance);
APITRACE_METAL_API void apitrace_metal_draw_indexed_primitives(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t primitive_type,
    uint32_t index_count,
    uint32_t index_type,
    uint64_t index_buffer_id,
    uint64_t index_buffer_offset,
    uint32_t instance_count,
    int32_t base_vertex,
    uint32_t base_instance);
APITRACE_METAL_API void apitrace_metal_draw_primitives_indirect(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t primitive_type,
    uint64_t indirect_buffer_id,
    uint64_t indirect_buffer_offset);
APITRACE_METAL_API void apitrace_metal_draw_indexed_primitives_indirect(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t primitive_type,
    uint32_t index_type,
    uint64_t index_buffer_id,
    uint64_t index_buffer_offset,
    uint64_t indirect_buffer_id,
    uint64_t indirect_buffer_offset);
APITRACE_METAL_API void apitrace_metal_dispatch_threadgroups(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t tgx,
    uint32_t tgy,
    uint32_t tgz,
    uint32_t tx,
    uint32_t ty,
    uint32_t tz);
APITRACE_METAL_API void apitrace_metal_dispatch_threadgroups_indirect(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t indirect_buffer_id,
    uint64_t indirect_buffer_offset,
    uint32_t tx,
    uint32_t ty,
    uint32_t tz);

APITRACE_METAL_API void apitrace_metal_blit_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_blit_fence_batch(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const uint64_t *wait_fences,
    uint32_t wait_fence_count,
    const uint64_t *update_fences,
    uint32_t update_fence_count);
APITRACE_METAL_API void apitrace_metal_copy_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t source_buffer_id,
    uint64_t source_offset,
    uint64_t destination_buffer_id,
    uint64_t destination_offset,
    uint64_t size);
APITRACE_METAL_API void apitrace_metal_copy_buffer_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t source_buffer_id,
    uint64_t source_offset,
    uint64_t destination_buffer_id,
    uint64_t destination_offset,
    uint64_t size,
    const void *source_bytes,
    uint64_t source_bytes_size);
APITRACE_METAL_API void apitrace_metal_copy_buffer_to_texture_with_contents(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *payload_json,
    const void *source_bytes,
    uint64_t source_bytes_size);
APITRACE_METAL_API void apitrace_metal_copy_buffer_to_texture(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t source_buffer_id,
    uint64_t destination_texture_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_copy_texture(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t source_texture_id,
    uint64_t destination_texture_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_blit_fill(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t buffer_object_id,
    uint64_t range_start,
    uint64_t range_length,
    uint32_t value);

APITRACE_METAL_API void apitrace_metal_use_resource(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t resource_id,
    uint32_t usage,
    uint32_t stages);
APITRACE_METAL_API void apitrace_metal_use_resources(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const uint64_t *resource_ids,
    uint32_t count,
    uint32_t usage,
    uint32_t stages);
APITRACE_METAL_API void apitrace_metal_use_heap(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t heap_id);
APITRACE_METAL_API void apitrace_metal_set_argument_buffer(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t index,
    uint64_t buffer_id,
    uint64_t offset);
APITRACE_METAL_API void apitrace_metal_encoder_state(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_set_compute_bytes(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_dispatch_threads(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_dispatch_threads_per_tile(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint32_t width,
    uint32_t height);
APITRACE_METAL_API void apitrace_metal_memory_barrier(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_record_fence_ops(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *payload_json);

APITRACE_METAL_API void apitrace_metal_update_fence(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t fence_id,
    uint32_t stages);
APITRACE_METAL_API void apitrace_metal_wait_for_fence(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    uint64_t fence_id,
    uint32_t stages);

APITRACE_METAL_API void apitrace_metal_present_drawable(
    apitrace_metal_session_t *session,
    uint64_t command_buffer_id,
    uint64_t drawable_id,
    uint64_t texture_id,
    uint64_t frame_index,
    uint32_t width,
    uint32_t height,
    uint32_t sync_interval,
    uint32_t flags);
APITRACE_METAL_API void apitrace_metal_push_debug_group(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *label_utf8);
APITRACE_METAL_API void apitrace_metal_pop_debug_group(
    apitrace_metal_session_t *session,
    uint64_t encoder_id);
APITRACE_METAL_API void apitrace_metal_object_metadata(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    const char *payload_json);
APITRACE_METAL_API void apitrace_metal_insert_debug_signpost(
    apitrace_metal_session_t *session,
    uint64_t encoder_id,
    const char *label_utf8);

APITRACE_METAL_API uint64_t apitrace_metal_register_buffer(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    uint64_t length,
    uint32_t storage_mode,
    const void *initial_bytes,
    uint64_t initial_bytes_size);
APITRACE_METAL_API void apitrace_metal_update_buffer_contents(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    uint64_t offset,
    uint64_t length,
    uint32_t storage_mode,
    const void *bytes,
    uint64_t bytes_size);
APITRACE_METAL_API void apitrace_metal_replace_texture_region(
    apitrace_metal_session_t *session,
    uint64_t texture_id,
    uint64_t origin_x,
    uint64_t origin_y,
    uint64_t origin_z,
    uint64_t size_width,
    uint64_t size_height,
    uint64_t size_depth,
    uint32_t level,
    uint32_t slice,
    uint64_t bytes_per_row,
    uint64_t bytes_per_image,
    const void *bytes,
    uint64_t bytes_size);
APITRACE_METAL_API uint64_t apitrace_metal_register_texture(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    const char *descriptor_json);
APITRACE_METAL_API uint64_t apitrace_metal_register_drawable_texture(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    uint64_t drawable_id,
    const char *descriptor_json);
APITRACE_METAL_API uint64_t apitrace_metal_register_library(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    const void *metallib_bytes,
    uint64_t size);
APITRACE_METAL_API uint64_t apitrace_metal_register_render_pipeline(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    const char *descriptor_json,
    uint64_t vs_function_id,
    uint64_t fs_function_id);
APITRACE_METAL_API uint64_t apitrace_metal_register_mesh_render_pipeline(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    const char *descriptor_json,
    uint64_t object_function_id,
    uint64_t mesh_function_id,
    uint64_t fragment_function_id);
APITRACE_METAL_API uint64_t apitrace_metal_register_tile_render_pipeline(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    const char *descriptor_json,
    uint64_t tile_function_id);
APITRACE_METAL_API uint64_t apitrace_metal_register_compute_pipeline(
    apitrace_metal_session_t *session,
    uint64_t object_id,
    const char *descriptor_json,
    uint64_t cs_function_id);

APITRACE_METAL_API void apitrace_metal_emit_link(
    apitrace_metal_session_t *session,
    apitrace_metal_scope_kind scope,
    uint64_t d3d_sequence,
    uint64_t metal_sequence_begin,
    uint64_t metal_sequence_end,
    const char *payload_utf8);
