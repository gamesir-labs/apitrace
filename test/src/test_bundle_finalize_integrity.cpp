#include "apitrace/event_types.hpp"
#include "apitrace/trace_bundle_io.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace apitrace::tools {
bool test_hook_bundle_finalize_reference_collection_matches_two_pass(
    const std::filesystem::path &bundle_root,
    std::size_t *path_ref_count,
    std::size_t *blob_id_ref_count);
} // namespace apitrace::tools

namespace {

void set_env_var(const char *name, const char *value)
{
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void unset_env_var(const char *name)
{
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

std::string read_text(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string shell_quote_path(const std::filesystem::path &path)
{
  std::string quoted = "'";
  for (const char ch : path.string()) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

int run_tool(const std::filesystem::path &tool, const std::filesystem::path &bundle)
{
  return std::system((shell_quote_path(tool) + " --no-progress " + shell_quote_path(bundle)).c_str());
}

int run_bundle_check(const std::filesystem::path &bundle_check, const std::filesystem::path &bundle)
{
  return std::system((shell_quote_path(bundle_check) + " " + shell_quote_path(bundle)).c_str());
}

int run_bundle_finalize_with_threshold(
    const std::filesystem::path &bundle_finalize,
    const std::filesystem::path &bundle,
    const char *threshold,
    const std::filesystem::path &stderr_path = {})
{
  set_env_var("DXMT_FINALIZE_MAX_TRUNCATE_FRAMES", threshold);
  auto command = shell_quote_path(bundle_finalize) + " --no-progress " + shell_quote_path(bundle);
  if (!stderr_path.empty()) {
    command += " 2> " + shell_quote_path(stderr_path);
  }
  const auto status = std::system(command.c_str());
  unset_env_var("DXMT_FINALIZE_MAX_TRUNCATE_FRAMES");
  return status;
}

void append_present_frame(
    apitrace::trace::TraceBundleWriter &writer,
    std::uint64_t frame_index,
    std::uint64_t &sequence)
{
  apitrace::trace::EventRecord frame_begin;
  frame_begin.kind = apitrace::trace::EventKind::Boundary;
  frame_begin.boundary = apitrace::trace::BoundaryKind::Frame;
  frame_begin.callsite.sequence = sequence++;
  frame_begin.payload = "{\"frame_index\":" + std::to_string(frame_index) + ",\"label\":\"FrameBegin\"}";
  writer.append_call_event(frame_begin);

  apitrace::trace::EventRecord present_call;
  present_call.kind = apitrace::trace::EventKind::Call;
  present_call.callsite.sequence = sequence++;
  present_call.callsite.function_name = "IDXGISwapChain::Present";
  present_call.callsite.result_code = 0;
  present_call.payload = "{\"frame_index\":" + std::to_string(frame_index) + ",\"sync_interval\":1,\"flags\":0}";
  writer.append_call_event(present_call);

  apitrace::trace::EventRecord present_boundary;
  present_boundary.kind = apitrace::trace::EventKind::Boundary;
  present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
  present_boundary.callsite.sequence = sequence++;
  present_boundary.payload = "{\"frame_index\":" + std::to_string(frame_index) + ",\"sync_interval\":1,\"flags\":0}";
  writer.append_call_event(present_boundary);

  apitrace::trace::EventRecord frame_end;
  frame_end.kind = apitrace::trace::EventKind::Boundary;
  frame_end.boundary = apitrace::trace::BoundaryKind::Frame;
  frame_end.callsite.sequence = sequence++;
  frame_end.payload = "{\"frame_index\":" + std::to_string(frame_index) + ",\"label\":\"FrameEnd\"}";
  writer.append_call_event(frame_end);
}

bool write_missing_blob_bundle(
    const std::filesystem::path &bundle,
    std::uint64_t frames_before_missing,
    std::uint64_t frames_after_missing)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "bundle-finalize-integrity-test", false});

  apitrace::trace::AssetRecord asset;
  asset.blob_id = 940;
  asset.kind = apitrace::trace::AssetKind::Buffer;
  asset.debug_name = "valid-unmap-buffer";
  asset.payload_bytes.assign(256, 0x44);
  asset = writer.register_asset(std::move(asset));

  std::uint64_t sequence = 1;
  for (std::uint64_t frame = 0; frame < frames_before_missing; ++frame) {
    append_present_frame(writer, frame, sequence);
  }

  apitrace::trace::EventRecord valid_unmap;
  valid_unmap.kind = apitrace::trace::EventKind::Call;
  valid_unmap.callsite.sequence = sequence++;
  valid_unmap.callsite.function_name = "ID3D12Resource::Unmap";
  valid_unmap.callsite.result_code = 0;
  valid_unmap.blob_refs = {asset.blob_id};
  valid_unmap.payload = std::string("{\"buffer_path\":\"") + asset.relative_path.generic_string() + "\"}";
  writer.append_call_event(valid_unmap);

  apitrace::trace::EventRecord missing_unmap;
  missing_unmap.kind = apitrace::trace::EventKind::Call;
  missing_unmap.callsite.sequence = sequence++;
  missing_unmap.callsite.function_name = "ID3D12Resource::Unmap";
  missing_unmap.callsite.result_code = 0;
  missing_unmap.blob_refs = {941};
  missing_unmap.payload = "{\"buffer_path\":\"buffers/asset-missing-middle.buffer\"}";
  writer.append_call_event(missing_unmap);

  for (std::uint64_t frame = frames_before_missing;
       frame < frames_before_missing + frames_after_missing;
       ++frame) {
    append_present_frame(writer, frame, sequence);
  }
  writer.close();
  return true;
}

std::size_t count_present_frames(const std::filesystem::path &bundle)
{
  std::ifstream input(bundle / "callstream.jsonl", std::ios::binary);
  std::unordered_set<std::uint64_t> frames;
  std::string line;
  while (std::getline(input, line)) {
    if (line.find("IDXGISwapChain::Present") == std::string::npos ||
        line.find("\"frame_index\"") == std::string::npos) {
      continue;
    }
    const auto marker = line.find("\"frame_index\"");
    const auto colon = line.find(':', marker + 13);
    if (colon == std::string::npos) {
      continue;
    }
    std::size_t cursor = colon + 1;
    while (cursor < line.size() && line[cursor] == ' ') {
      ++cursor;
    }
    std::uint64_t frame = 0;
    bool saw_digit = false;
    while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9') {
      saw_digit = true;
      frame = (frame * 10) + static_cast<std::uint64_t>(line[cursor] - '0');
      ++cursor;
    }
    if (saw_digit) {
      frames.insert(frame);
    }
  }
  return frames.size();
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 4) {
    std::cerr << "usage: " << argv[0] << " <tmp-bundle-root> <bundle-check> <bundle-finalize>\n";
    return 2;
  }

  const std::filesystem::path root = argv[1];
  const std::filesystem::path bundle_check = argv[2];
  const std::filesystem::path bundle_finalize = argv[3];
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto tail_bundle = root / "tail-missing.apitrace";
  if (!write_missing_blob_bundle(tail_bundle, 3, 2)) {
    std::cerr << "failed to write tail fixture\n";
    return 1;
  }
  std::size_t fused_path_refs = 0;
  std::size_t fused_blob_id_refs = 0;
  if (!apitrace::tools::test_hook_bundle_finalize_reference_collection_matches_two_pass(
          tail_bundle,
          &fused_path_refs,
          &fused_blob_id_refs) ||
      fused_path_refs == 0 ||
      fused_blob_id_refs == 0) {
    std::cerr << "fused reference collection diverged from the two-pass baseline\n";
    return 1;
  }
  if (run_bundle_finalize_with_threshold(bundle_finalize, tail_bundle, "2") != 0) {
    std::cerr << "finalize rejected bounded tail truncation\n";
    return 1;
  }
  if (count_present_frames(tail_bundle) != 3 ||
      read_text(tail_bundle / "callstream.jsonl").find("asset-missing-middle.buffer") != std::string::npos ||
      !std::filesystem::is_regular_file(tail_bundle / "checksums.json")) {
    std::cerr << "finalize did not preserve the expected consistent prefix\n";
    return 1;
  }
  if (run_bundle_check(bundle_check, tail_bundle) != 0) {
    std::cerr << "bundle-check rejected the truncated prefix bundle\n";
    return 1;
  }

  const auto mid_bundle = root / "midstream-missing.apitrace";
  if (!write_missing_blob_bundle(mid_bundle, 2, 3)) {
    std::cerr << "failed to write midstream fixture\n";
    return 1;
  }
  const auto checksums_before = read_text(mid_bundle / "checksums.json");
  const auto stderr_path = root / "midstream.stderr";
  if (run_bundle_finalize_with_threshold(bundle_finalize, mid_bundle, "2", stderr_path) == 0) {
    std::cerr << "finalize accepted over-threshold midstream loss\n";
    return 1;
  }
  const auto stderr_text = read_text(stderr_path);
  if (stderr_text.find("mid-stream integrity failure: would truncate 3 frames from frame 2 to end") ==
          std::string::npos ||
      stderr_text.find("capture lost data mid-stream") == std::string::npos ||
      read_text(mid_bundle / "checksums.json") != checksums_before ||
      read_text(mid_bundle / "callstream.jsonl").find("asset-missing-middle.buffer") == std::string::npos) {
    std::cerr << "finalize did not fail loudly without mutating midstream loss\n";
    return 1;
  }

  return 0;
}
