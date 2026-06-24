#pragma once

#include "apitrace/asset_index.hpp"
#include "apitrace/event_types.hpp"
#include "apitrace/raw_capture_io.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apitrace::trace::raw {

constexpr std::uint32_t kRawEventContractVersion = 1;

enum class RawEventOpcode : std::uint32_t {
  PassthroughFinalJson = 0x0001,
  ResourceCreate = 0x0101,
  ResourceUnmap = 0x0102,
  GraphicsPipelineCreate = 0x0201,
  DrawInstanced = 0x0301,
  Dispatch = 0x0302,
  PresentCall = 0x0401,
  FrameBegin = 0x0402,
  FrameEnd = 0x0403,
  PresentBoundary = 0x0404,
};

enum class RawBlobKind : std::uint32_t {
  Unknown = 0,
  Buffer = 1,
  ShaderDxbc = 2,
  ShaderDxil = 3,
  RootSignature = 4,
};

struct DecodedRawEvent {
  EventRecord event;
  std::vector<AssetRecord> assets;
  std::string passthrough_jsonl_record;
};

struct RawDecodeResult {
  std::vector<DecodedRawEvent> events;
  std::string error;
};

std::string raw_event_contract_markdown();

std::vector<std::uint8_t> encode_resource_create_payload(
    ObjectId device_object_id,
    ObjectId resource_object_id,
    std::uint64_t dimension,
    std::uint64_t width,
    std::uint32_t height,
    std::uint16_t depth_or_array_size,
    std::uint16_t mip_levels,
    std::uint32_t format,
    std::uint32_t flags,
    std::uint32_t initial_state,
    std::string debug_name = {});

std::vector<std::uint8_t> encode_resource_unmap_payload(
    ObjectId resource_object_id,
    std::uint64_t raw_blob_id,
    std::uint64_t written_begin,
    std::uint64_t written_end);

std::vector<std::uint8_t> encode_graphics_pipeline_create_payload(
    ObjectId device_object_id,
    ObjectId root_signature_object_id,
    ObjectId pipeline_state_object_id,
    std::uint64_t vs_raw_blob_id,
    std::uint64_t vs_bytecode_size,
    std::uint64_t ps_raw_blob_id,
    std::uint64_t ps_bytecode_size,
    std::uint32_t node_mask,
    std::uint32_t flags,
    bool requires_dxmt_backend = false);

std::vector<std::uint8_t> encode_draw_instanced_payload(
    ObjectId command_list_object_id,
    std::uint32_t vertex_count_per_instance,
    std::uint32_t instance_count,
    std::uint32_t start_vertex_location,
    std::uint32_t start_instance_location);

std::vector<std::uint8_t> encode_dispatch_payload(
    ObjectId command_list_object_id,
    std::uint32_t thread_group_count_x,
    std::uint32_t thread_group_count_y,
    std::uint32_t thread_group_count_z);

std::vector<std::uint8_t> encode_present_payload(
    ObjectId swap_chain_object_id,
    std::uint64_t frame_index,
    std::uint32_t sync_interval,
    std::uint32_t flags);

std::vector<std::uint8_t> encode_frame_boundary_payload(std::uint64_t frame_index);

std::vector<std::uint8_t> encode_passthrough_final_json_payload(std::string_view final_jsonl_record);

RawDecodeResult decode_raw_events(
    const RawCaptureReader &reader,
    const std::vector<RawEventRecord> &records);

} // namespace apitrace::trace::raw
