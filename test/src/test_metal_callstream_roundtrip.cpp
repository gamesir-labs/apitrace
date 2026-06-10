#include "apitrace/metal_event_types.hpp"

#include "../../src/trace/src/metal_callstream_writer.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using apitrace::trace::MetalCallKind;
using apitrace::trace::MetalEventRecord;

bool write_text(const std::filesystem::path &path, const std::string &text)
{
  std::ofstream output(path);
  if (!output.is_open()) {
    return false;
  }
  output << text;
  return true;
}

bool expect(bool condition, const char *message)
{
  if (condition) {
    return true;
  }
  std::cerr << message << "\n";
  return false;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    std::cerr << "usage: test_metal_callstream_roundtrip <metal-callstream.jsonl>\n";
    return 2;
  }

  const std::filesystem::path path = argv[1];
  std::filesystem::remove(path);

  const std::vector<MetalCallKind> kinds = {
      MetalCallKind::SetVertexTexture,
      MetalCallKind::SetVertexSamplerState,
      MetalCallKind::SetFragmentSamplerState,
      MetalCallKind::SetComputeTexture,
      MetalCallKind::SetComputeSamplerState,
      MetalCallKind::DrawIndexedPrimitivesIndirect,
      MetalCallKind::CopyBufferToTexture,
      MetalCallKind::EncoderState,
      MetalCallKind::SetComputeBytes,
      MetalCallKind::DispatchThreads,
      MetalCallKind::UseResources,
      MetalCallKind::MemoryBarrier,
      MetalCallKind::FenceOps,
      MetalCallKind::UpdateFence,
      MetalCallKind::WaitForFence,
      MetalCallKind::PushDebugGroup,
      MetalCallKind::PopDebugGroup,
      MetalCallKind::ObjectMetadata,
      MetalCallKind::InsertDebugSignpost,
  };

  std::string lines;
  std::uint64_t sequence = 1;
  for (const auto kind : kinds) {
    MetalEventRecord event;
    event.call_kind = kind;
    event.metal_sequence = sequence++;
    event.time_ns = 1700000000000000000ull + event.metal_sequence;
    event.elapsed_ns = 1000000ull * event.metal_sequence;
    event.d3d_sequence = 1000 + event.metal_sequence;
    event.frame_id = 7;
    event.object_id = 42;
    event.object_refs = {11, 12};
    event.blob_refs = {21, 22};
    event.function_name = "roundtrip";
    event.payload = "{\"index\":3,\"resource_ids\":[11,12]}";
    lines += apitrace::trace::detail::metal_event_record_json(event);
    lines += "\n";
  }

  if (!write_text(path, lines)) {
    std::cerr << "failed to write metal callstream\n";
    return 1;
  }

  std::vector<MetalEventRecord> parsed;
  std::string error;
  if (!apitrace::trace::detail::parse_metal_callstream(path, parsed, error)) {
    std::cerr << "failed to parse metal callstream: " << error << "\n";
    return 1;
  }

  if (!expect(parsed.size() == kinds.size(), "parsed call count mismatch")) {
    return 1;
  }
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    if (!expect(parsed[index].call_kind == kinds[index], "call kind mismatch") ||
        !expect(parsed[index].metal_sequence == index + 1, "metal sequence mismatch") ||
        !expect(parsed[index].time_ns == 1700000000000000001ull + index, "time ns mismatch") ||
        !expect(parsed[index].elapsed_ns == 1000000ull * (index + 1), "elapsed ns mismatch") ||
        !expect(parsed[index].d3d_sequence == 1001 + index, "d3d sequence mismatch") ||
        !expect(parsed[index].object_id == 42, "object id mismatch") ||
        !expect(parsed[index].object_refs.size() == 2, "object refs mismatch") ||
        !expect(parsed[index].blob_refs.size() == 2, "blob refs mismatch") ||
        !expect(parsed[index].payload.find("resource_ids") != std::string::npos, "payload mismatch")) {
      return 1;
    }
  }

  return 0;
}
