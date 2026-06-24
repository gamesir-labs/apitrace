#include "apitrace/asset_index.hpp"
#include "apitrace/event_types.hpp"
#include "apitrace/trace_bundle_io.hpp"
#include "apitrace/translation_link_writer.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

std::string read_text(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::size_t count_regular_files(const std::filesystem::path &path)
{
  std::size_t count = 0;
  if (!std::filesystem::exists(path))
    return count;
  for (const auto &entry : std::filesystem::directory_iterator(path)) {
    if (entry.is_regular_file())
      ++count;
  }
  return count;
}

std::size_t count_final_buffer_files(const std::filesystem::path &path)
{
  std::size_t count = 0;
  if (!std::filesystem::exists(path))
    return count;
  for (const auto &entry : std::filesystem::directory_iterator(path)) {
    if (!entry.is_regular_file())
      continue;
    const auto name = entry.path().filename().generic_string();
    if (name.rfind("asset-", 0) != 0)
      ++count;
  }
  return count;
}

std::size_t count_regular_files_with_extension(const std::filesystem::path &path, const char *extension)
{
  std::size_t count = 0;
  if (!std::filesystem::exists(path))
    return count;
  for (const auto &entry : std::filesystem::directory_iterator(path)) {
    if (entry.is_regular_file() && entry.path().extension() == extension)
      ++count;
  }
  return count;
}

std::uintmax_t allocated_bytes(const std::filesystem::path &path)
{
#ifndef _WIN32
  struct stat st {};
  if (stat(path.c_str(), &st) == 0)
    return static_cast<std::uintmax_t>(st.st_blocks) * 512u;
#endif
  return std::filesystem::file_size(path);
}

bool contains_json_string_field(
    const std::string &text,
    const std::string &key,
    const std::string &value)
{
  const auto compact = "\"" + key + "\":\"" + value + "\"";
  const auto pretty = "\"" + key + "\": \"" + value + "\"";
  return text.find(compact) != std::string::npos ||
         text.find(pretty) != std::string::npos;
}

bool contains_json_u64_field(
    const std::string &text,
    const std::string &key,
    std::uint64_t value)
{
  const auto decimal = std::to_string(value);
  const auto compact = "\"" + key + "\":" + decimal;
  const auto pretty = "\"" + key + "\": " + decimal;
  return text.find(compact) != std::string::npos ||
         text.find(pretty) != std::string::npos;
}

bool checksum_matches_file(
    const std::string &checksums_json,
    const std::filesystem::path &bundle,
    const std::string &relative_path)
{
  const auto marker = "\"" + relative_path + "\": \"sha256:";
  const auto marker_pos = checksums_json.find(marker);
  if (marker_pos == std::string::npos)
    return false;
  const auto digest_start = marker_pos + marker.size();
  const auto digest_end = checksums_json.find('"', digest_start);
  if (digest_end == std::string::npos)
    return false;
  const auto encoded_digest = checksums_json.substr(digest_start, digest_end - digest_start);
  const auto size_separator = encoded_digest.find(':');
  const auto expected_digest =
      size_separator == std::string::npos ? encoded_digest : encoded_digest.substr(0, size_separator);
  const auto contents = read_text(bundle / relative_path);
  return apitrace::trace::content_hash_bytes(contents.data(), contents.size()) == expected_digest;
}

std::string checksum_entry_value(const std::string &digest, std::uintmax_t size)
{
  return digest + ":" + std::to_string(static_cast<std::uint64_t>(size));
}

std::uint64_t json_u64_field(const std::string &json, const std::string &field)
{
  const auto marker = "\"" + field + "\":";
  const auto marker_pos = json.find(marker);
  if (marker_pos == std::string::npos)
    return 0;

  auto value_pos = marker_pos + marker.size();
  while (value_pos < json.size() && json[value_pos] == ' ')
    ++value_pos;

  std::uint64_t value = 0;
  while (value_pos < json.size() && json[value_pos] >= '0' && json[value_pos] <= '9') {
    value = (value * 10) + static_cast<std::uint64_t>(json[value_pos] - '0');
    ++value_pos;
  }
  return value;
}

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

int run_bundle_check(
    const std::filesystem::path &bundle_check,
    const std::filesystem::path &bundle,
    const std::string &options)
{
  const auto command = shell_quote_path(bundle_check) + " " + options + " " + shell_quote_path(bundle);
  return std::system(command.c_str());
}

int run_bundle_finalize(
    const std::filesystem::path &bundle_finalize,
    const std::filesystem::path &bundle)
{
  const auto command =
      shell_quote_path(bundle_finalize) + " --no-progress " + shell_quote_path(bundle);
  return std::system(command.c_str());
}

std::size_t count_substrings(const std::string &text, const std::string &needle)
{
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

bool find_asset_path_by_blob(
    const std::filesystem::path &bundle,
    apitrace::trace::BlobId blob_id,
    std::string &relative_path)
{
  apitrace::trace::TraceBundleReader reader;
  if (!reader.open(bundle)) {
    return false;
  }
  for (const auto &asset : reader.assets()) {
    if (asset.blob_id == blob_id) {
      relative_path = asset.relative_path.generic_string();
      return true;
    }
  }
  return false;
}

bool asset_record_matches_file(
    const std::filesystem::path &bundle,
    const apitrace::trace::AssetRecord &asset)
{
  if (!asset.payload_path.empty())
    return true;

  const auto path = bundle / asset.relative_path;
  if (!std::filesystem::is_regular_file(path))
    return false;
  return std::filesystem::file_size(path) == asset.byte_size;
}

bool write_single_event_bundle(
    const std::filesystem::path &bundle,
    std::uint64_t blob_id,
    std::uint8_t byte_value,
    const char *producer)
{
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle))
    return false;
  writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, producer, false});
  writer.write_object_index({{blob_id, apitrace::trace::ObjectKind::Resource, 0, producer}});

  apitrace::trace::AssetRecord asset;
  asset.blob_id = blob_id;
  asset.kind = apitrace::trace::AssetKind::Buffer;
  asset.debug_name = producer;
  asset.payload_bytes.assign(64, byte_value);
  asset = writer.register_asset(std::move(asset));

  apitrace::trace::EventRecord event;
  event.kind = apitrace::trace::EventKind::ResourceBlob;
  event.callsite.sequence = 1;
  event.object_debug_name = producer;
  event.blob_refs = {asset.blob_id};
  event.payload = std::string("{\"buffer_path\":\"") + asset.relative_path.generic_string() + "\"}";
  writer.append_call_event(event);
  writer.close();
  return true;
}

bool primary_open_restarts_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  if (!write_single_event_bundle(bundle, 701, 0x71, "primary-restart-first") ||
      !write_single_event_bundle(bundle, 702, 0x72, "primary-restart-second")) {
    return false;
  }

  const auto callstream = read_text(bundle / "callstream.jsonl");
  if (count_substrings(callstream, "\"record_kind\":\"bundle_header\"") != 1 ||
      callstream.find("primary-restart-first") != std::string::npos ||
      callstream.find("primary-restart-second") == std::string::npos) {
    return false;
  }

  apitrace::trace::TraceBundleReader reader;
  if (!reader.open(bundle) || reader.events().size() != 1)
    return false;
  return reader.events()[0].blob_refs.size() == 1 &&
         reader.events()[0].blob_refs[0] == 702;
}

bool slow_spool_backpressure_stays_bounded(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  set_env_var("APITRACE_WRITER_STATS", "1");
  set_env_var("APITRACE_ASYNC_ASSET_WORKERS", "1");
  set_env_var("APITRACE_ASYNC_ASSET_THRESHOLD", "1");
  set_env_var("APITRACE_CHECKPOINT_ASSET_BYTES", "4096");
  set_env_var("APITRACE_TEST_ASSET_WRITE_DELAY_MS", "20");
  set_env_var("DXMT_CAPTURE_MAX_INFLIGHT_BYTES", "16384");
  set_env_var("DXMT_CAPTURE_LOW_WATER_BYTES", "8192");
  unset_env_var("DXMT_CAPTURE_FAST_UNSAFE");

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "slow-spool-backpressure-test", false});
  writer.write_object_index({{900, apitrace::trace::ObjectKind::Resource, 0, "slow-spool-resource"}});

  constexpr std::size_t kPayloadBytes = 4096;
  constexpr std::size_t kAssetCount = 24;
  constexpr std::uint64_t kMaxInFlightBytes = 16384;
  std::vector<std::uint8_t> payload(kPayloadBytes, 0x6d);
  std::uint64_t max_unpublished = 0;
  std::uint64_t max_register_ms = 0;
  std::size_t blocked_registers = 0;

  for (std::size_t index = 0; index < kAssetCount; ++index) {
    payload[0] = static_cast<std::uint8_t>(index);
    apitrace::trace::AssetRecord asset;
    asset.blob_id = 900 + index;
    asset.kind = apitrace::trace::AssetKind::Buffer;
    asset.debug_name = "d3d12-resource-unmap";
    asset.payload_bytes = payload;

    const auto begin = std::chrono::steady_clock::now();
    asset = writer.register_asset(std::move(asset));
    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin)
            .count());
    max_register_ms = std::max(max_register_ms, elapsed_ms);
    if (elapsed_ms >= 10)
      ++blocked_registers;

    apitrace::trace::EventRecord event;
    event.kind = apitrace::trace::EventKind::Call;
    event.callsite.sequence = index + 1;
    event.callsite.function_name = "ID3D12Resource::Unmap";
    event.blob_refs = {asset.blob_id};
    event.payload = std::string("{\"buffer_path\":\"") + asset.relative_path.generic_string() + "\"}";
    writer.append_call_event(event);

    const auto reserved =
        apitrace::trace::TraceBundleWriter::TestHooks::spool_reserved_offset_for_test(writer);
    const auto published =
        apitrace::trace::TraceBundleWriter::TestHooks::spool_published_offset_for_test(writer);
    max_unpublished = std::max(max_unpublished, reserved > published ? reserved - published : 0);
    if (max_unpublished > kMaxInFlightBytes + kPayloadBytes) {
      std::cerr << "slow spool backlog exceeded bound: " << max_unpublished << "\n";
      writer.close();
      return false;
    }
  }

  writer.checkpoint();
  const auto reserved =
      apitrace::trace::TraceBundleWriter::TestHooks::spool_reserved_offset_for_test(writer);
  const auto published =
      apitrace::trace::TraceBundleWriter::TestHooks::spool_published_offset_for_test(writer);
  const auto final_unpublished = reserved > published ? reserved - published : 0;
  const auto assets_json = read_text(bundle / "assets.json");
  const auto indexed_spooled_assets = count_substrings(assets_json, "\"payload_path\":\"spool/asset-payloads.bin\"");
  writer.close();

  unset_env_var("APITRACE_TEST_ASSET_WRITE_DELAY_MS");
  unset_env_var("APITRACE_ASYNC_ASSET_THRESHOLD");
  unset_env_var("APITRACE_CHECKPOINT_ASSET_BYTES");
  unset_env_var("DXMT_CAPTURE_MAX_INFLIGHT_BYTES");
  unset_env_var("DXMT_CAPTURE_LOW_WATER_BYTES");

  std::cerr << "slow-spool-backpressure max_unpublished_bytes=" << max_unpublished
            << " final_unpublished_bytes=" << final_unpublished
            << " blocked_registers=" << blocked_registers
            << " max_register_ms=" << max_register_ms
            << " indexed_spooled_assets=" << indexed_spooled_assets << "\n";

  return blocked_registers != 0 &&
         max_unpublished <= kMaxInFlightBytes + kPayloadBytes &&
         final_unpublished <= kPayloadBytes &&
         indexed_spooled_assets >= kAssetCount - 1;
}

bool replace_text_in_file(
    const std::filesystem::path &path,
    const std::string &needle,
    const std::string &replacement)
{
  auto text = read_text(path);
  const auto position = text.find(needle);
  if (position == std::string::npos) {
    return false;
  }
  text.replace(position, needle.size(), replacement);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  return output.good();
}

bool replace_any_text_in_file(
    const std::filesystem::path &path,
    const std::vector<std::pair<std::string, std::string>> &replacements)
{
  auto text = read_text(path);
  for (const auto &[needle, replacement] : replacements) {
    const auto position = text.find(needle);
    if (position == std::string::npos) {
      continue;
    }
    text.replace(position, needle.size(), replacement);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    return output.good();
  }
  return false;
}

bool refresh_checksum_entry(
    const std::filesystem::path &bundle,
    const std::string &relative_path)
{
  const auto checksums_path = bundle / "checksums.json";
  auto checksums = read_text(checksums_path);
  const auto marker = "\"" + relative_path + "\": \"sha256:";
  const auto marker_position = checksums.find(marker);
  if (marker_position == std::string::npos) {
    return false;
  }
  const auto digest_begin = marker_position + marker.size();
  const auto digest_end = checksums.find('"', digest_begin);
  if (digest_end == std::string::npos) {
    return false;
  }
  const auto contents = read_text(bundle / relative_path);
  const auto digest = apitrace::trace::content_hash_bytes(contents.data(), contents.size());
  checksums.replace(
      digest_begin,
      digest_end - digest_begin,
      checksum_entry_value(digest, contents.size()));
  std::ofstream output(checksums_path, std::ios::binary | std::ios::trunc);
  output << checksums;
  return output.good();
}

std::size_t checksum_files_insertion_position(const std::string &checksums)
{
  auto insertion_position = checksums.rfind("\n  }\n");
  if (insertion_position != std::string::npos) {
    return insertion_position;
  }
  insertion_position = checksums.rfind("\n  },\n");
  if (insertion_position != std::string::npos) {
    return insertion_position;
  }
  return std::string::npos;
}

bool upsert_checksum_entry(
    const std::filesystem::path &bundle,
    const std::string &relative_path)
{
  const auto checksums_path = bundle / "checksums.json";
  auto checksums = read_text(checksums_path);
  const auto marker = "\"" + relative_path + "\": \"sha256:";
  const auto digest = apitrace::trace::content_hash_file(bundle / relative_path);
  const auto file_size = std::filesystem::file_size(bundle / relative_path);
  const auto marker_position = checksums.find(marker);
  if (marker_position != std::string::npos) {
    const auto digest_begin = marker_position + marker.size();
    const auto digest_end = checksums.find('"', digest_begin);
    if (digest_end == std::string::npos) {
      return false;
    }
    checksums.replace(digest_begin, digest_end - digest_begin, checksum_entry_value(digest, file_size));
  } else {
    const auto insertion_position = checksum_files_insertion_position(checksums);
    if (insertion_position == std::string::npos) {
      return false;
    }
    checksums.insert(
        insertion_position,
        ",\n    \"" + relative_path + "\": \"sha256:" + checksum_entry_value(digest, file_size) + "\"");
  }
  std::ofstream output(checksums_path, std::ios::binary | std::ios::trunc);
  output << checksums;
  return output.good();
}

bool refresh_bundle_hash(const std::filesystem::path &bundle)
{
  const auto checksums_path = bundle / "checksums.json";
  auto checksums = read_text(checksums_path);
  std::map<std::string, std::string> records;
  std::size_t search_position = 0;
  while ((search_position = checksums.find("\": \"sha256:", search_position)) != std::string::npos) {
    std::size_t path_begin = search_position;
    while (path_begin > 0 && checksums[path_begin - 1] != '"')
      --path_begin;
    if (path_begin == 0) {
      return false;
    }
    const auto path = checksums.substr(path_begin, search_position - path_begin);
    const auto digest_begin = search_position + std::string("\": \"sha256:").size();
    const auto digest_end = checksums.find('"', digest_begin);
    if (digest_end == std::string::npos) {
      return false;
    }
    if (path != "bundle_hash") {
      const auto encoded = checksums.substr(digest_begin, digest_end - digest_begin);
      const auto size_separator = encoded.find(':');
      const auto digest = size_separator == std::string::npos ? encoded : encoded.substr(0, size_separator);
      const auto size_suffix = size_separator == std::string::npos ? std::string() : ("#" + encoded.substr(size_separator + 1));
      records[path] = digest + size_suffix;
    }
    search_position = digest_end + 1;
  }
  std::string fingerprint_source;
  for (const auto &entry : records) {
    fingerprint_source += entry.first;
    fingerprint_source += "=";
    fingerprint_source += entry.second;
    fingerprint_source += "\n";
  }
  const auto bundle_hash = std::string("sha256:") +
                           apitrace::trace::content_hash_bytes(fingerprint_source.data(), fingerprint_source.size());
  const auto marker = "\"bundle_hash\": \"";
  const auto hash_begin = checksums.find(marker);
  if (hash_begin == std::string::npos) {
    return false;
  }
  const auto value_begin = hash_begin + std::string(marker).size();
  const auto value_end = checksums.find('"', value_begin);
  if (value_end == std::string::npos) {
    return false;
  }
  checksums.replace(value_begin, value_end - value_begin, bundle_hash);
  std::ofstream output(checksums_path, std::ios::binary | std::ios::trunc);
  output << checksums;
  return output.good();
}

bool write_split_texture_resource_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "split-texture-resource-test";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({
      {201, apitrace::trace::ObjectKind::Resource, 0, "d3d-buffer"},
      {202, apitrace::trace::ObjectKind::Resource, 0, "d3d-texture"},
      {203, apitrace::trace::ObjectKind::Resource, 0, "metal-buffer"},
      {204, apitrace::trace::ObjectKind::Resource, 0, "metal-texture"},
  });

  std::vector<std::uint8_t> buffer_bytes(4096, 0x2a);
  apitrace::trace::AssetRecord shared_buffer;
  shared_buffer.blob_id = 201;
  shared_buffer.kind = apitrace::trace::AssetKind::Buffer;
  shared_buffer.debug_name = "shared-buffer";
  shared_buffer.payload_bytes = buffer_bytes;
  shared_buffer = writer.register_asset(std::move(shared_buffer));

  apitrace::trace::AssetRecord shared_buffer_alias;
  shared_buffer_alias.blob_id = 202;
  shared_buffer_alias.kind = apitrace::trace::AssetKind::Buffer;
  shared_buffer_alias.debug_name = "shared-buffer-alias";
  shared_buffer_alias.payload_bytes = buffer_bytes;
  shared_buffer_alias = writer.register_metal_asset(
      apitrace::trace::MetalAssetKind::Buffer,
      std::move(shared_buffer_alias));

  apitrace::trace::AssetRecord d3d_texture;
  d3d_texture.blob_id = 203;
  d3d_texture.kind = apitrace::trace::AssetKind::Texture;
  d3d_texture.debug_name = "d3d-only-texture";
  d3d_texture.payload_bytes.assign(16, 0x11);
  d3d_texture = writer.register_asset(std::move(d3d_texture));

  apitrace::trace::AssetRecord metal_texture;
  metal_texture.blob_id = 204;
  metal_texture.kind = apitrace::trace::AssetKind::Texture;
  metal_texture.debug_name = "metal-only-texture";
  metal_texture.payload_bytes.assign(16, 0x22);
  metal_texture = writer.register_metal_asset(
      apitrace::trace::MetalAssetKind::Texture,
      std::move(metal_texture));

  apitrace::trace::EventRecord d3d_buffer_event;
  d3d_buffer_event.kind = apitrace::trace::EventKind::ResourceBlob;
  d3d_buffer_event.callsite.sequence = 1;
  d3d_buffer_event.object_debug_name = "D3DSharedBuffer";
  d3d_buffer_event.blob_refs = {shared_buffer.blob_id};
  d3d_buffer_event.payload = std::string("{\"buffer_path\":\"") + shared_buffer.relative_path.generic_string() + "\"}";
  writer.append_call_event(d3d_buffer_event);

  apitrace::trace::EventRecord d3d_texture_event;
  d3d_texture_event.kind = apitrace::trace::EventKind::ResourceBlob;
  d3d_texture_event.callsite.sequence = 2;
  d3d_texture_event.object_debug_name = "D3DOnlyTexture";
  d3d_texture_event.blob_refs = {d3d_texture.blob_id};
  d3d_texture_event.payload = std::string("{\"texture_path\":\"") + d3d_texture.relative_path.generic_string() + "\"}";
  writer.append_call_event(d3d_texture_event);

  apitrace::trace::MetalEventRecord metal_buffer_event;
  metal_buffer_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  metal_buffer_event.metal_sequence = 1;
  metal_buffer_event.function_name = "MTLDevice.newBuffer";
  metal_buffer_event.blob_refs = {shared_buffer_alias.blob_id};
  metal_buffer_event.payload = std::string("{\"buffer_path\":\"") +
                               shared_buffer_alias.relative_path.generic_string() +
                               "\",\"length\":" + std::to_string(buffer_bytes.size()) + "}";
  writer.append_metal_event(metal_buffer_event);

  apitrace::trace::MetalEventRecord metal_texture_event;
  metal_texture_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  metal_texture_event.metal_sequence = 2;
  metal_texture_event.function_name = "MTLDevice.newTexture";
  metal_texture_event.blob_refs = {metal_texture.blob_id};
  metal_texture_event.payload = std::string("{\"texture_path\":\"") + metal_texture.relative_path.generic_string() + "\"}";
  writer.append_metal_event(metal_texture_event);

  writer.close();
  return true;
}

bool write_checkpoint_tail_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "checkpoint-tail-test";
  writer.write_metadata(metadata);
  writer.write_object_index({
      {300, apitrace::trace::ObjectKind::Device, 0, "device"},
      {301, apitrace::trace::ObjectKind::Resource, 300, "resource"},
  });

  std::vector<std::uint8_t> bytes(4096, 0x5a);
  apitrace::trace::AssetRecord asset;
  asset.blob_id = 301;
  asset.kind = apitrace::trace::AssetKind::Buffer;
  asset.debug_name = "checkpoint-tail-buffer";
  asset.payload_bytes = bytes;
  asset = writer.register_asset(std::move(asset));

  apitrace::trace::EventRecord checkpointed_event;
  checkpointed_event.kind = apitrace::trace::EventKind::ResourceBlob;
  checkpointed_event.callsite.sequence = 1;
  checkpointed_event.blob_refs = {asset.blob_id};
  checkpointed_event.payload = std::string("{\"buffer_path\":\"") + asset.relative_path.generic_string() + "\"}";
  writer.append_call_event(checkpointed_event);
  writer.checkpoint();
  const auto checkpoint_checksums = read_text(bundle / "checksums.json");

  apitrace::trace::EventRecord tail_event = checkpointed_event;
  tail_event.callsite.sequence = 2;
  writer.append_call_event(tail_event);
  writer.append_call_event(tail_event);
  writer.close();
  std::ofstream checksum_output(bundle / "checksums.json", std::ios::binary | std::ios::trunc);
  checksum_output << checkpoint_checksums;
  checksum_output.close();
  return true;
}

bool write_middle_missing_blob_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "middle-missing-blob-test", false});

  apitrace::trace::AssetRecord asset;
  asset.blob_id = 940;
  asset.kind = apitrace::trace::AssetKind::Buffer;
  asset.debug_name = "valid-unmap-buffer";
  asset.payload_bytes.assign(256, 0x44);
  asset = writer.register_asset(std::move(asset));

  apitrace::trace::EventRecord valid_unmap;
  valid_unmap.kind = apitrace::trace::EventKind::Call;
  valid_unmap.callsite.sequence = 1;
  valid_unmap.callsite.function_name = "ID3D12Resource::Unmap";
  valid_unmap.callsite.result_code = 0;
  valid_unmap.blob_refs = {asset.blob_id};
  valid_unmap.payload = std::string("{\"buffer_path\":\"") + asset.relative_path.generic_string() + "\"}";
  writer.append_call_event(valid_unmap);

  apitrace::trace::EventRecord missing_unmap;
  missing_unmap.kind = apitrace::trace::EventKind::Call;
  missing_unmap.callsite.sequence = 2;
  missing_unmap.callsite.function_name = "ID3D12Resource::Unmap";
  missing_unmap.callsite.result_code = 0;
  missing_unmap.blob_refs = {941};
  missing_unmap.payload = "{\"buffer_path\":\"buffers/asset-missing-middle.buffer\"}";
  writer.append_call_event(missing_unmap);

  apitrace::trace::EventRecord present_call;
  present_call.kind = apitrace::trace::EventKind::Call;
  present_call.callsite.sequence = 3;
  present_call.callsite.function_name = "IDXGISwapChain::Present";
  present_call.callsite.result_code = 0;
  present_call.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
  writer.append_call_event(present_call);

  apitrace::trace::EventRecord present_boundary;
  present_boundary.kind = apitrace::trace::EventKind::Boundary;
  present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
  present_boundary.callsite.sequence = 4;
  present_boundary.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
  writer.append_call_event(present_boundary);

  apitrace::trace::EventRecord next_present_call;
  next_present_call.kind = apitrace::trace::EventKind::Call;
  next_present_call.callsite.sequence = 5;
  next_present_call.callsite.function_name = "IDXGISwapChain::Present";
  next_present_call.callsite.result_code = 0;
  next_present_call.payload = "{\"frame_index\":1,\"sync_interval\":1,\"flags\":0}";
  writer.append_call_event(next_present_call);

  apitrace::trace::EventRecord next_present_boundary;
  next_present_boundary.kind = apitrace::trace::EventKind::Boundary;
  next_present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
  next_present_boundary.callsite.sequence = 6;
  next_present_boundary.payload = "{\"frame_index\":1,\"sync_interval\":1,\"flags\":0}";
  writer.append_call_event(next_present_boundary);
  writer.close();
  return true;
}

bool write_primary_blob_id_conflict_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "primary-blob-conflict-test", false});
  writer.write_object_index({
      {930, apitrace::trace::ObjectKind::Resource, 0, "first-buffer"},
      {931, apitrace::trace::ObjectKind::Resource, 0, "second-buffer"},
  });

  apitrace::trace::AssetRecord first;
  first.blob_id = 930;
  first.kind = apitrace::trace::AssetKind::Buffer;
  first.debug_name = "primary-conflict-first";
  first.payload_bytes.assign(64, 0x31);
  first = writer.register_asset(std::move(first));

  apitrace::trace::AssetRecord second;
  second.blob_id = 931;
  second.kind = apitrace::trace::AssetKind::Buffer;
  second.debug_name = "primary-conflict-second";
  second.payload_bytes.assign(64, 0x62);
  second = writer.register_asset(std::move(second));
  if (first.relative_path == second.relative_path) {
    return false;
  }

  apitrace::trace::EventRecord first_event;
  first_event.kind = apitrace::trace::EventKind::ResourceBlob;
  first_event.callsite.sequence = 1;
  first_event.object_debug_name = "primary-conflict-first";
  first_event.blob_refs = {first.blob_id};
  first_event.payload = std::string("{\"buffer_path\":\"") + first.relative_path.generic_string() + "\"}";
  writer.append_call_event(first_event);

  apitrace::trace::EventRecord second_event;
  second_event.kind = apitrace::trace::EventKind::ResourceBlob;
  second_event.callsite.sequence = 2;
  second_event.object_debug_name = "primary-conflict-second";
  second_event.blob_refs = {second.blob_id};
  second_event.payload = std::string("{\"buffer_path\":\"") + second.relative_path.generic_string() + "\"}";
  writer.append_call_event(second_event);
  writer.close();

  if (!replace_any_text_in_file(
          bundle / "assets.json",
          {
              {"\"blob_id\":931", "\"blob_id\":930"},
              {"\"blob_id\": 931", "\"blob_id\": 930"},
          }) ||
      !replace_any_text_in_file(
          bundle / "callstream.jsonl",
          {
              {"\"blob_refs\":[931]", "\"blob_refs\":[930]"},
              {"\"blob_refs\": [931]", "\"blob_refs\": [930]"},
          })) {
    return false;
  }
  return refresh_checksum_entry(bundle, "assets.json") &&
         refresh_checksum_entry(bundle, "callstream.jsonl") &&
         refresh_bundle_hash(bundle);
}

bool write_d3d_graphics_pipeline_bundle(const std::filesystem::path &bundle, bool shuffled_blob_refs = false)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "d3d-graphics-pipeline-test";
  writer.write_metadata(metadata);
  writer.write_object_index({
      {100, apitrace::trace::ObjectKind::Device, 0, "device"},
      {101, apitrace::trace::ObjectKind::RootSignature, 100, "root-signature"},
      {102, apitrace::trace::ObjectKind::PipelineState, 100, "pipeline-state"},
      {103, apitrace::trace::ObjectKind::CommandQueue, 100, "command-queue"},
      {104, apitrace::trace::ObjectKind::CommandAllocator, 100, "command-allocator"},
      {105, apitrace::trace::ObjectKind::CommandList, 100, "command-list"},
  });

  apitrace::trace::AssetRecord vs;
  vs.blob_id = 200;
  vs.kind = apitrace::trace::AssetKind::ShaderDxil;
  vs.debug_name = "vs";
  vs.payload_bytes.assign(32, 0x56);
  vs = writer.register_asset(std::move(vs));

  apitrace::trace::AssetRecord ps;
  ps.blob_id = 201;
  ps.kind = apitrace::trace::AssetKind::ShaderDxil;
  ps.debug_name = "ps";
  ps.payload_bytes.assign(32, 0x50);
  ps = writer.register_asset(std::move(ps));

  apitrace::trace::AssetRecord root_signature;
  root_signature.blob_id = 202;
  root_signature.kind = apitrace::trace::AssetKind::RootSignature;
  root_signature.debug_name = "root-signature";
  root_signature.payload_bytes.assign(16, 0x52);
  root_signature = writer.register_asset(std::move(root_signature));

  std::ostringstream pipeline_json;
  pipeline_json << "{"
                << "\"type\":\"graphics\","
                << "\"root_signature_object_id\":101,"
                << "\"node_mask\":0,"
                << "\"flags\":0,"
                << "\"input_layout\":{\"element_count\":0,\"elements\":[]},"
                << "\"blend_state\":{\"alpha_to_coverage_enable\":false,\"independent_blend_enable\":false,"
                << "\"render_targets\":[{\"blend_enable\":false,\"logic_op_enable\":false,"
                << "\"src_blend\":1,\"dest_blend\":0,\"blend_op\":1,"
                << "\"src_blend_alpha\":1,\"dest_blend_alpha\":0,\"blend_op_alpha\":1,"
                << "\"logic_op\":0,\"render_target_write_mask\":15}]},"
                << "\"sample_mask\":4294967295,"
                << "\"rasterizer_state\":{\"fill_mode\":3,\"cull_mode\":1,\"front_counter_clockwise\":false,"
                << "\"depth_bias\":0,\"depth_bias_clamp\":0.0,\"slope_scaled_depth_bias\":0.0,"
                << "\"depth_clip_enable\":true,\"multisample_enable\":false,\"antialiased_line_enable\":false,"
                << "\"forced_sample_count\":0,\"conservative_raster\":0},"
                << "\"depth_stencil_state\":{\"depth_enable\":false,\"depth_write_mask\":0,\"depth_func\":8,"
                << "\"stencil_enable\":false,\"stencil_read_mask\":255,\"stencil_write_mask\":255,"
                << "\"front_face\":{\"stencil_fail_op\":1,\"stencil_depth_fail_op\":1,\"stencil_pass_op\":1,\"stencil_func\":8},"
                << "\"back_face\":{\"stencil_fail_op\":1,\"stencil_depth_fail_op\":1,\"stencil_pass_op\":1,\"stencil_func\":8}},"
                << "\"stream_output\":{},"
                << "\"primitive_topology_type\":3,"
                << "\"num_render_targets\":1,"
                << "\"rtv_formats\":[28],"
                << "\"dsv_format\":0,"
                << "\"sample_desc\":{\"count\":1,\"quality\":0},"
                << "\"ib_strip_cut_value\":0,"
                << "\"vs\":{\"bytecode_size\":32,\"vs_path\":\"" << vs.relative_path.generic_string() << "\"},"
                << "\"ps\":{\"bytecode_size\":32,\"ps_path\":\"" << ps.relative_path.generic_string() << "\"},"
                << "\"ds\":null,"
                << "\"hs\":null,"
                << "\"gs\":null"
                << "}";

  apitrace::trace::AssetRecord pipeline;
  pipeline.blob_id = 203;
  pipeline.kind = apitrace::trace::AssetKind::Pipeline;
  pipeline.debug_name = "pipeline";
  const auto pipeline_text = pipeline_json.str();
  pipeline.payload_bytes.assign(pipeline_text.begin(), pipeline_text.end());
  pipeline = writer.register_asset(std::move(pipeline));

  apitrace::trace::EventRecord create_root_signature;
  create_root_signature.kind = apitrace::trace::EventKind::Call;
  create_root_signature.callsite.sequence = 1;
  create_root_signature.callsite.function_name = "ID3D12Device::CreateRootSignature";
  create_root_signature.callsite.result_code = 0;
  create_root_signature.object_refs = {100, 101};
  create_root_signature.blob_refs = {root_signature.blob_id};
  create_root_signature.payload =
      std::string("{\"node_mask\":0,\"bytecode_size\":16,\"root_signature_path\":\"") +
      root_signature.relative_path.generic_string() + "\"}";
  writer.append_call_event(create_root_signature);

  apitrace::trace::EventRecord create_pipeline;
  create_pipeline.kind = apitrace::trace::EventKind::Call;
  create_pipeline.callsite.sequence = 2;
  create_pipeline.callsite.function_name = "ID3D12Device::CreateGraphicsPipelineState";
  create_pipeline.callsite.result_code = 0;
  create_pipeline.object_refs = {100, 102};
  create_pipeline.blob_refs = shuffled_blob_refs
      ? std::vector<apitrace::trace::BlobId>{ps.blob_id, pipeline.blob_id, vs.blob_id}
      : std::vector<apitrace::trace::BlobId>{pipeline.blob_id, vs.blob_id, ps.blob_id};
  create_pipeline.payload = std::string("{\"pipeline_path\":\"") + pipeline.relative_path.generic_string() + "\"}";
  writer.append_call_event(create_pipeline);

  apitrace::trace::EventRecord create_queue;
  create_queue.kind = apitrace::trace::EventKind::Call;
  create_queue.callsite.sequence = 3;
  create_queue.callsite.function_name = "ID3D12Device::CreateCommandQueue";
  create_queue.callsite.result_code = 0;
  create_queue.object_refs = {100, 103};
  create_queue.payload = "{\"type\":0,\"priority\":0,\"flags\":0,\"node_mask\":0}";
  writer.append_call_event(create_queue);

  apitrace::trace::EventRecord create_allocator;
  create_allocator.kind = apitrace::trace::EventKind::Call;
  create_allocator.callsite.sequence = 4;
  create_allocator.callsite.function_name = "ID3D12Device::CreateCommandAllocator";
  create_allocator.callsite.result_code = 0;
  create_allocator.object_refs = {100, 104};
  create_allocator.payload = "{\"type\":0}";
  writer.append_call_event(create_allocator);

  apitrace::trace::EventRecord create_command_list;
  create_command_list.kind = apitrace::trace::EventKind::Call;
  create_command_list.callsite.sequence = 5;
  create_command_list.callsite.function_name = "ID3D12Device::CreateCommandList";
  create_command_list.callsite.result_code = 0;
  create_command_list.object_refs = {100, 104, 0, 105};
  create_command_list.payload =
      "{\"node_mask\":0,\"type\":0,\"command_allocator_object_id\":104,"
      "\"initial_pipeline_state_object_id\":0}";
  writer.append_call_event(create_command_list);

  apitrace::trace::EventRecord set_root_signature;
  set_root_signature.kind = apitrace::trace::EventKind::Call;
  set_root_signature.callsite.sequence = 6;
  set_root_signature.callsite.function_name = "ID3D12GraphicsCommandList::SetGraphicsRootSignature";
  set_root_signature.callsite.result_code = 0;
  set_root_signature.object_refs = {105, 101};
  set_root_signature.payload = "{\"compute\":false,\"root_signature_object_id\":101}";
  writer.append_call_event(set_root_signature);

  apitrace::trace::EventRecord set_pipeline;
  set_pipeline.kind = apitrace::trace::EventKind::Call;
  set_pipeline.callsite.sequence = 7;
  set_pipeline.callsite.function_name = "ID3D12GraphicsCommandList::SetPipelineState";
  set_pipeline.callsite.result_code = 0;
  set_pipeline.object_refs = {105, 102};
  set_pipeline.payload = "{\"pipeline_state_object_id\":102}";
  writer.append_call_event(set_pipeline);

  apitrace::trace::EventRecord close_command_list;
  close_command_list.kind = apitrace::trace::EventKind::Call;
  close_command_list.callsite.sequence = 8;
  close_command_list.callsite.function_name = "ID3D12GraphicsCommandList::Close";
  close_command_list.callsite.result_code = 0;
  close_command_list.object_refs = {105};
  close_command_list.payload = "{}";
  writer.append_call_event(close_command_list);

  apitrace::trace::EventRecord execute_command_lists;
  execute_command_lists.kind = apitrace::trace::EventKind::Call;
  execute_command_lists.callsite.sequence = 9;
  execute_command_lists.callsite.function_name = "ID3D12CommandQueue::ExecuteCommandLists";
  execute_command_lists.callsite.result_code = 0;
  execute_command_lists.object_refs = {103, 105};
  execute_command_lists.payload = "{\"command_list_count\":1}";
  writer.append_call_event(execute_command_lists);
  writer.close();
  return true;
}

bool write_duplicate_cross_api_resource_hash_bundle(const std::filesystem::path &bundle)
{
  std::filesystem::remove_all(bundle);
  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    return false;
  }
  apitrace::trace::TraceMetadata metadata;
  metadata.api = apitrace::trace::ApiKind::D3D12;
  metadata.producer = "duplicate-cross-api-resource-hash-test";
  metadata.has_metal_callstream = true;
  writer.write_metadata(metadata);
  writer.write_object_index({
      {301, apitrace::trace::ObjectKind::Resource, 0, "shared-texture"},
      {302, apitrace::trace::ObjectKind::Resource, 0, "d3d-duplicate-texture"},
      {303, apitrace::trace::ObjectKind::Resource, 0, "metal-duplicate-texture"},
  });

  std::vector<std::uint8_t> shared_texture_bytes(16, 0x7a);
  apitrace::trace::AssetRecord shared_texture;
  shared_texture.blob_id = 301;
  shared_texture.kind = apitrace::trace::AssetKind::Texture;
  shared_texture.debug_name = "shared-texture";
  shared_texture.payload_bytes = shared_texture_bytes;
  shared_texture = writer.register_asset(std::move(shared_texture));

  apitrace::trace::AssetRecord shared_texture_alias;
  shared_texture_alias.blob_id = 302;
  shared_texture_alias.kind = apitrace::trace::AssetKind::Texture;
  shared_texture_alias.debug_name = "shared-texture-alias";
  shared_texture_alias.payload_bytes = shared_texture_bytes;
  shared_texture_alias = writer.register_metal_asset(
      apitrace::trace::MetalAssetKind::Texture,
      std::move(shared_texture_alias));

  const std::vector<std::uint8_t> duplicate_texture_bytes(16, 0x44);
  const auto duplicate_texture_hash =
      apitrace::trace::content_hash_bytes(duplicate_texture_bytes.data(), duplicate_texture_bytes.size());
  apitrace::trace::AssetRecord d3d_duplicate_texture;
  d3d_duplicate_texture.blob_id = 303;
  d3d_duplicate_texture.kind = apitrace::trace::AssetKind::Texture;
  d3d_duplicate_texture.debug_name = "d3d-duplicate-texture";
  d3d_duplicate_texture.payload_bytes = duplicate_texture_bytes;
  d3d_duplicate_texture = writer.register_asset(std::move(d3d_duplicate_texture));

  apitrace::trace::AssetRecord metal_duplicate_texture;
  metal_duplicate_texture.blob_id = 304;
  metal_duplicate_texture.kind = apitrace::trace::AssetKind::Texture;
  metal_duplicate_texture.debug_name = "metal-duplicate-texture";
  metal_duplicate_texture.content_hash = duplicate_texture_hash;
  metal_duplicate_texture.byte_size = duplicate_texture_bytes.size();
  metal_duplicate_texture.payload_bytes = duplicate_texture_bytes;
  metal_duplicate_texture.relative_path =
      std::filesystem::path("textures") / ("forced-duplicate-" + duplicate_texture_hash + ".texture");
  metal_duplicate_texture = writer.register_asset(std::move(metal_duplicate_texture));

  apitrace::trace::EventRecord shared_d3d_texture_event;
  shared_d3d_texture_event.kind = apitrace::trace::EventKind::ResourceBlob;
  shared_d3d_texture_event.callsite.sequence = 1;
  shared_d3d_texture_event.object_debug_name = "D3DSharedTexture";
  shared_d3d_texture_event.blob_refs = {shared_texture.blob_id};
  shared_d3d_texture_event.payload =
      std::string("{\"texture_path\":\"") + shared_texture.relative_path.generic_string() + "\"}";
  writer.append_call_event(shared_d3d_texture_event);

  apitrace::trace::EventRecord duplicate_d3d_texture_event;
  duplicate_d3d_texture_event.kind = apitrace::trace::EventKind::ResourceBlob;
  duplicate_d3d_texture_event.callsite.sequence = 2;
  duplicate_d3d_texture_event.object_debug_name = "D3DDuplicateTexture";
  duplicate_d3d_texture_event.blob_refs = {d3d_duplicate_texture.blob_id};
  duplicate_d3d_texture_event.payload =
      std::string("{\"texture_path\":\"") + d3d_duplicate_texture.relative_path.generic_string() + "\"}";
  writer.append_call_event(duplicate_d3d_texture_event);

  apitrace::trace::MetalEventRecord shared_metal_texture_event;
  shared_metal_texture_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  shared_metal_texture_event.metal_sequence = 1;
  shared_metal_texture_event.function_name = "MTLDevice.newTexture";
  shared_metal_texture_event.blob_refs = {shared_texture_alias.blob_id};
  shared_metal_texture_event.payload =
      std::string("{\"texture_path\":\"") + shared_texture_alias.relative_path.generic_string() + "\"}";
  writer.append_metal_event(shared_metal_texture_event);

  apitrace::trace::MetalEventRecord duplicate_metal_texture_event;
  duplicate_metal_texture_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  duplicate_metal_texture_event.metal_sequence = 2;
  duplicate_metal_texture_event.function_name = "MTLDevice.newTexture";
  duplicate_metal_texture_event.blob_refs = {metal_duplicate_texture.blob_id};
  duplicate_metal_texture_event.payload =
      std::string("{\"texture_path\":\"") + metal_duplicate_texture.relative_path.generic_string() + "\"}";
  writer.append_metal_event(duplicate_metal_texture_event);

  writer.close();
  std::string original_duplicate_path;
  std::string current_metal_duplicate_path;
  if (!find_asset_path_by_blob(bundle, 303, original_duplicate_path) ||
      !find_asset_path_by_blob(bundle, 304, current_metal_duplicate_path)) {
    return false;
  }
  const auto forced_duplicate_path =
      (std::filesystem::path("textures") / ("forced-duplicate-" + duplicate_texture_hash + ".texture")).generic_string();
  if (original_duplicate_path == forced_duplicate_path) {
    return false;
  }
  std::filesystem::create_directories((bundle / forced_duplicate_path).parent_path());
  if (!std::filesystem::is_regular_file(bundle / forced_duplicate_path)) {
    std::filesystem::copy_file(
        bundle / original_duplicate_path,
        bundle / forced_duplicate_path,
        std::filesystem::copy_options::overwrite_existing);
  }
  if (current_metal_duplicate_path != forced_duplicate_path &&
      !replace_text_in_file(
          bundle / "assets.json",
          "\"blob_id\":304,\"path\":\"" + current_metal_duplicate_path + "\"",
          "\"blob_id\":304,\"path\":\"" + forced_duplicate_path + "\"")) {
    return false;
  }
  if (current_metal_duplicate_path != forced_duplicate_path &&
      !replace_text_in_file(
          bundle / "metal-callstream.jsonl",
          "\"blob_refs\":[304],\"function\":\"MTLDevice.newTexture\",\"payload\":{\"texture_path\":\"" +
              current_metal_duplicate_path + "\"",
          "\"blob_refs\":[304],\"function\":\"MTLDevice.newTexture\",\"payload\":{\"texture_path\":\"" +
              forced_duplicate_path + "\"")) {
    return false;
  }
  if (!refresh_checksum_entry(bundle, "assets.json") ||
      !refresh_checksum_entry(bundle, "metal-callstream.jsonl") ||
      !upsert_checksum_entry(bundle, forced_duplicate_path) ||
      !refresh_bundle_hash(bundle)) {
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2 && argc != 4) {
    std::cerr << "usage: test_trace_writer_assets <bundle> [bundle-check bundle-finalize]\n";
    return 2;
  }

  const std::filesystem::path bundle = argv[1];
  const std::filesystem::path bundle_check = argc == 4 ? std::filesystem::path(argv[2]) : std::filesystem::path();
  const std::filesystem::path bundle_finalize = argc == 4 ? std::filesystem::path(argv[3]) : std::filesystem::path();
  std::filesystem::remove_all(bundle);

  set_env_var("APITRACE_WRITER_STATS", "1");
  set_env_var("APITRACE_ASYNC_LINE_MAX_PENDING", "4096");
  set_env_var("APITRACE_ASYNC_ASSET_MAX_PENDING", "1048576");
  set_env_var("APITRACE_ASYNC_ASSET_WORKERS", "1");
  set_env_var("APITRACE_SYNC_CHECKPOINTS", "1");
  const auto primary_restart_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-primary-restart");
  if (!primary_open_restarts_bundle(primary_restart_bundle)) {
    std::cerr << "primary writer should restart an existing bundle instead of appending a new session\n";
    return 1;
  }
  const auto slow_spool_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-slow-spool-backpressure");
  if (!slow_spool_backpressure_stays_bounded(slow_spool_bundle)) {
    std::cerr << "slow spool capture should block and keep published asset prefix bounded\n";
    return 1;
  }

  apitrace::trace::TraceBundleWriter writer;
  if (!writer.open(bundle)) {
    std::cerr << "failed to open bundle\n";
    return 1;
  }
  writer.write_object_index({
      {100, apitrace::trace::ObjectKind::Device, 0, "device"},
      {101, apitrace::trace::ObjectKind::Resource, 100, "resource"},
      {102, apitrace::trace::ObjectKind::Resource, 100, "texture"},
      {103, apitrace::trace::ObjectKind::CommandQueue, 100, "metal-command-buffer"},
      {104, apitrace::trace::ObjectKind::CommandList, 103, "metal-blit-encoder"},
  });

  std::vector<std::uint8_t> bytes(2 * 1024 * 1024);
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = static_cast<std::uint8_t>(index & 0xff);

  std::vector<std::uint8_t> quarter_probe_a(2 * 1024 * 1024, 0);
  std::vector<std::uint8_t> quarter_probe_b = quarter_probe_a;
  quarter_probe_a[quarter_probe_a.size() / 4] = 0x31;
  quarter_probe_b[quarter_probe_b.size() / 4] = 0x62;
  if (apitrace::trace::fast_fingerprint_bytes(quarter_probe_a.data(), quarter_probe_a.size()) ==
      apitrace::trace::fast_fingerprint_bytes(quarter_probe_b.data(), quarter_probe_b.size())) {
    std::cerr << "large fast fingerprint should sample quarter offsets, not only first/middle/tail windows\n";
    return 1;
  }

  std::vector<std::uint8_t> small_bytes(256 * 1024);
  for (std::size_t index = 0; index < small_bytes.size(); ++index)
    small_bytes[index] = static_cast<std::uint8_t>((index * 3u) & 0xff);

  apitrace::trace::AssetRecord small_first;
  small_first.blob_id = 19;
  small_first.kind = apitrace::trace::AssetKind::Buffer;
  small_first.debug_name = "small-buffer";
  small_first.fast_fingerprint = "same-small-buffer";
  small_first.payload_bytes = small_bytes;
  small_first = writer.register_asset(std::move(small_first));

  apitrace::trace::AssetRecord small_second;
  small_second.blob_id = 20;
  small_second.kind = apitrace::trace::AssetKind::Buffer;
  small_second.debug_name = "small-buffer";
  small_second.fast_fingerprint = "same-small-buffer";
  small_second.payload_bytes = small_bytes;
  small_second = writer.register_asset(std::move(small_second));

  if (small_first.blob_id == small_second.blob_id || small_first.relative_path != small_second.relative_path) {
    std::cerr << "small duplicate asset should still reuse the pending exact-match path before async hash completion\n";
    return 1;
  }

  apitrace::trace::AssetRecord first;
  first.blob_id = 1;
  first.kind = apitrace::trace::AssetKind::Buffer;
  first.debug_name = "large-buffer";
  first.fast_fingerprint = "same-large-buffer";
  first.payload_bytes = bytes;
  first = writer.register_asset(std::move(first));

  apitrace::trace::AssetRecord second;
  second.blob_id = 2;
  second.kind = apitrace::trace::AssetKind::Buffer;
  second.debug_name = "large-buffer";
  second.fast_fingerprint = "same-large-buffer";
  second.payload_bytes = bytes;
  second = writer.register_asset(std::move(second));

  if (first.blob_id == second.blob_id || first.relative_path == second.relative_path) {
    std::cerr << "medium duplicate asset should avoid foreground exact-match reuse and defer aliasing to async hash\n";
    return 1;
  }

  apitrace::trace::AssetRecord metal_buffer;
  metal_buffer.blob_id = 3;
  metal_buffer.kind = apitrace::trace::AssetKind::Buffer;
  metal_buffer.debug_name = "metal-large-buffer";
  metal_buffer.fast_fingerprint = "same-large-buffer";
  metal_buffer.payload_bytes = bytes;
  metal_buffer = writer.register_metal_asset(apitrace::trace::MetalAssetKind::Buffer, std::move(metal_buffer));

  if (metal_buffer.blob_id == first.blob_id || metal_buffer.relative_path == first.relative_path) {
    std::cerr << "API-independent medium Metal buffer should avoid foreground exact-match reuse\n";
    return 1;
  }

  writer.checkpoint();
  if (!std::filesystem::is_regular_file(bundle / "assets.json") ||
      !std::filesystem::is_regular_file(bundle / "checksums.json")) {
    std::cerr << "checkpoint should publish asset and checksum indexes before close\n";
    return 1;
  }
  const auto checkpoint_assets = read_text(bundle / "assets.json");
  const auto checkpoint_checksums = read_text(bundle / "checksums.json");
  const auto first_relative_path = first.relative_path.generic_string();
  if (checkpoint_assets.find(first_relative_path) == std::string::npos ||
      !checksum_matches_file(checkpoint_checksums, bundle, "assets.json") ||
      !checksum_matches_file(checkpoint_checksums, bundle, first_relative_path)) {
    std::cerr << "checkpoint should describe and checksum completed async asset files\n";
    return 1;
  }

  std::vector<std::uint8_t> texture_bytes(128 * 128 * 4);
  for (std::size_t index = 0; index < texture_bytes.size(); ++index)
    texture_bytes[index] = static_cast<std::uint8_t>((index * 5u) & 0xff);

  apitrace::trace::AssetRecord generic_texture;
  generic_texture.blob_id = 4;
  generic_texture.kind = apitrace::trace::AssetKind::Texture;
  generic_texture.debug_name = "generic-texture";
  generic_texture.fast_fingerprint = "same-texture";
  generic_texture.payload_bytes = texture_bytes;
  generic_texture = writer.register_asset(std::move(generic_texture));

  apitrace::trace::AssetRecord metal_texture;
  metal_texture.blob_id = 5;
  metal_texture.kind = apitrace::trace::AssetKind::Texture;
  metal_texture.debug_name = "metal-texture";
  metal_texture.fast_fingerprint = "same-texture";
  metal_texture.payload_bytes = texture_bytes;
  metal_texture = writer.register_metal_asset(apitrace::trace::MetalAssetKind::Texture, std::move(metal_texture));

  if (metal_texture.blob_id == generic_texture.blob_id || metal_texture.relative_path != generic_texture.relative_path) {
    std::cerr << "Metal texture did not reuse API-independent texture storage while preserving blob identity\n";
    return 1;
  }

  std::vector<std::uint8_t> different_bytes = bytes;
  different_bytes.back() ^= 0xff;

  apitrace::trace::AssetRecord collision;
  collision.blob_id = 6;
  collision.kind = apitrace::trace::AssetKind::Buffer;
  collision.debug_name = "fingerprint-collision-buffer";
  collision.fast_fingerprint = "same-large-buffer";
  collision.payload_bytes = different_bytes;
  collision = writer.register_asset(std::move(collision));

  if (collision.blob_id == first.blob_id || collision.relative_path == first.relative_path ||
      collision.relative_path == second.relative_path || collision.relative_path == metal_buffer.relative_path) {
    std::cerr << "different large payloads with the same fast fingerprint should keep distinct temporary paths\n";
    return 1;
  }

  std::vector<std::uint8_t> cached_bytes(2 * 1024 * 1024);
  for (std::size_t index = 0; index < cached_bytes.size(); ++index)
    cached_bytes[index] = static_cast<std::uint8_t>((index * 13u) & 0xff);

  apitrace::trace::AssetRecord cached_first;
  cached_first.blob_id = 7;
  cached_first.kind = apitrace::trace::AssetKind::Buffer;
  cached_first.debug_name = "completed-cache-buffer";
  cached_first.fast_fingerprint = "completed-cache-buffer";
  cached_first.payload_bytes = cached_bytes;
  cached_first = writer.register_asset(std::move(cached_first));

  apitrace::trace::AssetRecord cached_second;
  cached_second.blob_id = 8;
  cached_second.kind = apitrace::trace::AssetKind::Buffer;
  cached_second.debug_name = "completed-cache-buffer";
  cached_second.fast_fingerprint = "completed-cache-buffer";
  cached_second.payload_bytes = cached_bytes;

  for (unsigned attempt = 0; attempt < 200 && count_final_buffer_files(bundle / "buffers") < 3; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  cached_second = writer.register_asset(std::move(cached_second));
  if (cached_second.blob_id == cached_first.blob_id ||
      cached_second.relative_path == cached_first.relative_path ||
      cached_second.relative_path.extension() != ".buffer") {
    std::cerr << "large completed-cache asset should avoid exact main-thread dedup before async hash completion\n";
    return 1;
  }

  std::vector<std::uint8_t> hash_only_bytes(5 * 1024 * 1024);
  for (std::size_t index = 0; index < hash_only_bytes.size(); ++index)
    hash_only_bytes[index] = static_cast<std::uint8_t>((index * 7u) & 0xff);

  apitrace::trace::AssetRecord hash_only_first;
  hash_only_first.blob_id = 9;
  hash_only_first.kind = apitrace::trace::AssetKind::Buffer;
  hash_only_first.debug_name = "hash-only-buffer";
  hash_only_first.payload_bytes = hash_only_bytes;
  hash_only_first = writer.register_asset(std::move(hash_only_first));

  apitrace::trace::AssetRecord hash_only_second;
  hash_only_second.blob_id = 10;
  hash_only_second.kind = apitrace::trace::AssetKind::Buffer;
  hash_only_second.debug_name = "hash-only-buffer";
  hash_only_second.payload_bytes = hash_only_bytes;
  hash_only_second = writer.register_asset(std::move(hash_only_second));
  if (hash_only_second.blob_id == hash_only_first.blob_id ||
      hash_only_second.relative_path == hash_only_first.relative_path) {
    std::cerr << "medium writer-generated duplicate should avoid foreground pending reuse before async hash completion\n";
    return 1;
  }

  std::vector<std::uint8_t> hash_only_large_bytes(20 * 1024 * 1024);
  for (std::size_t index = 0; index < hash_only_large_bytes.size(); ++index)
    hash_only_large_bytes[index] = static_cast<std::uint8_t>((index * 11u) & 0xff);

  apitrace::trace::AssetRecord hash_only_large_first;
  hash_only_large_first.blob_id = 17;
  hash_only_large_first.kind = apitrace::trace::AssetKind::Buffer;
  hash_only_large_first.debug_name = "hash-only-large-buffer";
  hash_only_large_first.payload_bytes = hash_only_large_bytes;
  hash_only_large_first = writer.register_asset(std::move(hash_only_large_first));

  apitrace::trace::AssetRecord hash_only_large_second;
  hash_only_large_second.blob_id = 18;
  hash_only_large_second.kind = apitrace::trace::AssetKind::Buffer;
  hash_only_large_second.debug_name = "hash-only-large-buffer";
  hash_only_large_second.payload_bytes = hash_only_large_bytes;
  hash_only_large_second = writer.register_asset(std::move(hash_only_large_second));
  if (hash_only_large_second.blob_id == hash_only_large_first.blob_id ||
      hash_only_large_second.relative_path == hash_only_large_first.relative_path) {
    std::cerr << "very large duplicate should stay on the async hash-only path before hash completion\n";
    return 1;
  }

  std::vector<std::uint8_t> known_hash_bytes(96 * 1024, 0x6b);
  const auto known_hash_digest =
      apitrace::trace::content_hash_bytes(known_hash_bytes.data(), known_hash_bytes.size());
  apitrace::trace::AssetRecord known_hash_first;
  known_hash_first.blob_id = 12;
  known_hash_first.kind = apitrace::trace::AssetKind::Buffer;
  known_hash_first.debug_name = "known-hash-buffer";
  known_hash_first.content_hash = known_hash_digest;
  known_hash_first.byte_size = known_hash_bytes.size();
  known_hash_first.payload_bytes = known_hash_bytes;
  known_hash_first = writer.register_asset(std::move(known_hash_first));

  apitrace::trace::AssetRecord known_hash_second;
  known_hash_second.blob_id = 13;
  known_hash_second.kind = apitrace::trace::AssetKind::Buffer;
  known_hash_second.debug_name = "known-hash-buffer-alias";
  known_hash_second.content_hash = known_hash_digest;
  known_hash_second.byte_size = known_hash_bytes.size();
  known_hash_second.payload_bytes = known_hash_bytes;
  known_hash_second = writer.register_asset(std::move(known_hash_second));
  if (known_hash_second.blob_id == known_hash_first.blob_id ||
      known_hash_second.relative_path != known_hash_first.relative_path) {
    std::cerr << "known content_hash duplicate should reuse the stored asset before hashing or writing\n";
    return 1;
  }

  std::vector<std::uint8_t> metal_known_hash_bytes(64 * 1024, 0x4d);
  const auto metal_known_hash_digest =
      apitrace::trace::content_hash_bytes(metal_known_hash_bytes.data(), metal_known_hash_bytes.size());
  apitrace::trace::AssetRecord metal_known_hash_first;
  metal_known_hash_first.blob_id = 14;
  metal_known_hash_first.kind = apitrace::trace::AssetKind::Pipeline;
  metal_known_hash_first.debug_name = "known-hash-metal-pipeline";
  metal_known_hash_first.content_hash = metal_known_hash_digest;
  metal_known_hash_first.byte_size = metal_known_hash_bytes.size();
  metal_known_hash_first.payload_bytes = metal_known_hash_bytes;
  metal_known_hash_first = writer.register_metal_asset(
      apitrace::trace::MetalAssetKind::RenderPipeline,
      std::move(metal_known_hash_first));

  apitrace::trace::AssetRecord metal_known_hash_second;
  metal_known_hash_second.blob_id = 15;
  metal_known_hash_second.kind = apitrace::trace::AssetKind::Pipeline;
  metal_known_hash_second.debug_name = "known-hash-metal-pipeline-alias";
  metal_known_hash_second.content_hash = metal_known_hash_digest;
  metal_known_hash_second.byte_size = metal_known_hash_bytes.size();
  metal_known_hash_second.payload_bytes = metal_known_hash_bytes;
  metal_known_hash_second = writer.register_metal_asset(
      apitrace::trace::MetalAssetKind::RenderPipeline,
      std::move(metal_known_hash_second));
  if (metal_known_hash_second.blob_id == metal_known_hash_first.blob_id ||
      metal_known_hash_second.relative_path != metal_known_hash_first.relative_path) {
    std::cerr << "known content_hash Metal duplicate should reuse the stored asset before hashing or writing\n";
    return 1;
  }
  const auto expected_metal_pipeline_path =
      std::filesystem::path("metal/pipelines") /
      (metal_known_hash_digest + ".pipeline.json");
  if (metal_known_hash_first.relative_path != expected_metal_pipeline_path) {
    std::cerr << "known content_hash Metal pipeline should use canonical Metal pipeline storage\n";
    return 1;
  }

  std::vector<std::uint8_t> metal_known_hash_library_bytes(48 * 1024, 0x7e);
  const auto metal_known_hash_library_digest =
      apitrace::trace::content_hash_bytes(
          metal_known_hash_library_bytes.data(),
          metal_known_hash_library_bytes.size());
  apitrace::trace::AssetRecord metal_known_hash_library;
  metal_known_hash_library.blob_id = 16;
  metal_known_hash_library.kind = apitrace::trace::AssetKind::Unknown;
  metal_known_hash_library.debug_name = "known-hash-metal-library";
  metal_known_hash_library.relative_path = metal_known_hash_library_digest + ".bin";
  metal_known_hash_library.content_hash = metal_known_hash_library_digest;
  metal_known_hash_library.byte_size = metal_known_hash_library_bytes.size();
  metal_known_hash_library.payload_bytes = metal_known_hash_library_bytes;
  metal_known_hash_library = writer.register_metal_asset(
      apitrace::trace::MetalAssetKind::Library,
      std::move(metal_known_hash_library));
  const auto expected_metal_library_path =
      std::filesystem::path("metal/libraries") /
      (metal_known_hash_library_digest + ".metallib");
  if (metal_known_hash_library.relative_path != expected_metal_library_path) {
    std::cerr << "known content_hash Metal library should not keep caller-provided generic storage\n";
    return 1;
  }

  apitrace::trace::EventRecord event;
  event.kind = apitrace::trace::EventKind::ResourceBlob;
  event.callsite.sequence = 1;
  event.object_debug_name = "duplicate-large-buffer";
  event.blob_refs = {first.blob_id};
  event.payload = std::string("{\"buffer_path\":\"") + first.relative_path.generic_string() + "\"}";
  writer.append_call_event(event);

  apitrace::trace::MetalEventRecord metal_event;
  metal_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  metal_event.metal_sequence = 1;
  metal_event.object_id = 101;
  metal_event.function_name = "MTLDevice.newBuffer";
  metal_event.blob_refs = {metal_buffer.blob_id};
  metal_event.payload = std::string("{\"buffer_path\":\"") +
                        metal_buffer.relative_path.generic_string() +
                        "\",\"length\":" + std::to_string(bytes.size()) + "}";
  writer.append_metal_event(metal_event);

  apitrace::trace::EventRecord second_event;
  second_event.kind = apitrace::trace::EventKind::ResourceBlob;
  second_event.callsite.sequence = 2;
  second_event.object_debug_name = "duplicate-large-buffer-second-blob";
  second_event.blob_refs = {second.blob_id};
  second_event.payload = std::string("{\"buffer_path\":\"") + second.relative_path.generic_string() + "\"}";
  writer.append_call_event(second_event);

  apitrace::trace::MetalEventRecord metal_collision_event;
  metal_collision_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  metal_collision_event.metal_sequence = 2;
  metal_collision_event.object_id = 101;
  metal_collision_event.function_name = "MTLDevice.newBuffer";
  metal_collision_event.blob_refs = {collision.blob_id};
  metal_collision_event.payload = std::string("{\"buffer_path\":\"") +
                                  collision.relative_path.generic_string() +
                                  "\",\"length\":" + std::to_string(different_bytes.size()) + "}";
  writer.append_metal_event(metal_collision_event);

  apitrace::trace::MetalEventRecord metal_texture_event;
  metal_texture_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  metal_texture_event.metal_sequence = 3;
  metal_texture_event.object_id = 102;
  metal_texture_event.function_name = "MTLDevice.newTexture";
  metal_texture_event.blob_refs = {metal_texture.blob_id};
  metal_texture_event.payload = std::string("{\"texture_path\":\"") + metal_texture.relative_path.generic_string() + "\"}";
  writer.append_metal_event(metal_texture_event);

  apitrace::trace::MetalEventRecord metal_command_buffer_begin;
  metal_command_buffer_begin.call_kind = apitrace::trace::MetalCallKind::CommandBufferBegin;
  metal_command_buffer_begin.metal_sequence = 4;
  metal_command_buffer_begin.object_id = 103;
  metal_command_buffer_begin.function_name = "MTLCommandQueue.commandBuffer";
  metal_command_buffer_begin.payload = "{}";
  writer.append_metal_event(metal_command_buffer_begin);

  apitrace::trace::MetalEventRecord metal_blit_encoder_begin;
  metal_blit_encoder_begin.call_kind = apitrace::trace::MetalCallKind::BlitEncoderBegin;
  metal_blit_encoder_begin.metal_sequence = 5;
  metal_blit_encoder_begin.object_id = 104;
  metal_blit_encoder_begin.function_name = "MTLCommandBuffer.blitCommandEncoder";
  metal_blit_encoder_begin.payload = "{\"command_buffer_id\":103}";
  metal_blit_encoder_begin.object_refs = {103};
  writer.append_metal_event(metal_blit_encoder_begin);

  apitrace::trace::MetalEventRecord metal_copy_event;
  metal_copy_event.call_kind = apitrace::trace::MetalCallKind::CopyBufferToTexture;
  metal_copy_event.metal_sequence = 6;
  metal_copy_event.object_id = 104;
  metal_copy_event.function_name = "MTLBlitCommandEncoder.copyFromBufferToTexture";
  metal_copy_event.object_refs = {101, 102};
  metal_copy_event.blob_refs = {cached_first.blob_id};
  metal_copy_event.payload = std::string("{\"source_buffer\":101,\"destination_texture\":102,"
                                         "\"source_offset\":0,\"source_bytes_per_row\":256,"
                                         "\"source_bytes_per_image\":2097152,"
                                         "\"source_size\":[256,8192,1],"
                                         "\"destination_slice\":0,\"destination_level\":0,"
                                         "\"destination_origin\":[0,0,0],\"source_asset_size\":") +
                             std::to_string(cached_bytes.size()) +
                             ",\"source_asset_path\":\"" +
                             cached_first.relative_path.generic_string() + "\"}";
  writer.append_metal_event(metal_copy_event);

  apitrace::trace::MetalEventRecord metal_blit_encoder_end;
  metal_blit_encoder_end.call_kind = apitrace::trace::MetalCallKind::BlitEncoderEnd;
  metal_blit_encoder_end.metal_sequence = 7;
  metal_blit_encoder_end.object_id = 104;
  metal_blit_encoder_end.function_name = "MTLBlitCommandEncoder.endEncoding";
  metal_blit_encoder_end.payload = "{}";
  writer.append_metal_event(metal_blit_encoder_end);

  apitrace::trace::MetalEventRecord metal_command_buffer_commit;
  metal_command_buffer_commit.call_kind = apitrace::trace::MetalCallKind::CommandBufferCommit;
  metal_command_buffer_commit.metal_sequence = 8;
  metal_command_buffer_commit.object_id = 103;
  metal_command_buffer_commit.function_name = "MTLCommandBuffer.commit";
  metal_command_buffer_commit.payload = "{}";
  writer.append_metal_event(metal_command_buffer_commit);

  apitrace::trace::EventRecord generic_texture_event;
  generic_texture_event.kind = apitrace::trace::EventKind::ResourceBlob;
  generic_texture_event.callsite.sequence = 3;
  generic_texture_event.object_debug_name = "generic-texture-blob";
  generic_texture_event.blob_refs = {generic_texture.blob_id};
  generic_texture_event.payload = std::string("{\"texture_path\":\"") + generic_texture.relative_path.generic_string() + "\"}";
  writer.append_call_event(generic_texture_event);

  apitrace::trace::MetalEventRecord hash_only_first_event;
  hash_only_first_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  hash_only_first_event.metal_sequence = 9;
  hash_only_first_event.object_id = 101;
  hash_only_first_event.function_name = "MTLDevice.newBuffer";
  hash_only_first_event.blob_refs = {hash_only_first.blob_id};
  hash_only_first_event.payload = std::string("{\"buffer_path\":\"") +
                                  hash_only_first.relative_path.generic_string() +
                                  "\",\"length\":" + std::to_string(hash_only_bytes.size()) + "}";
  writer.append_metal_event(hash_only_first_event);

  apitrace::trace::MetalEventRecord hash_only_second_event;
  hash_only_second_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  hash_only_second_event.metal_sequence = 10;
  hash_only_second_event.object_id = 101;
  hash_only_second_event.function_name = "MTLDevice.newBuffer";
  hash_only_second_event.blob_refs = {hash_only_second.blob_id};
  hash_only_second_event.payload = std::string("{\"buffer_path\":\"") +
                                   hash_only_second.relative_path.generic_string() +
                                   "\",\"length\":" + std::to_string(hash_only_bytes.size()) + "}";
  writer.append_metal_event(hash_only_second_event);

  apitrace::trace::MetalEventRecord hash_only_large_first_event;
  hash_only_large_first_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  hash_only_large_first_event.metal_sequence = 12;
  hash_only_large_first_event.object_id = 101;
  hash_only_large_first_event.function_name = "MTLDevice.newBuffer";
  hash_only_large_first_event.blob_refs = {hash_only_large_first.blob_id};
  hash_only_large_first_event.payload = std::string("{\"buffer_path\":\"") +
                                        hash_only_large_first.relative_path.generic_string() +
                                        "\",\"length\":" + std::to_string(hash_only_large_bytes.size()) + "}";
  writer.append_metal_event(hash_only_large_first_event);

  apitrace::trace::MetalEventRecord hash_only_large_second_event;
  hash_only_large_second_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  hash_only_large_second_event.metal_sequence = 13;
  hash_only_large_second_event.object_id = 101;
  hash_only_large_second_event.function_name = "MTLDevice.newBuffer";
  hash_only_large_second_event.blob_refs = {hash_only_large_second.blob_id};
  hash_only_large_second_event.payload = std::string("{\"buffer_path\":\"") +
                                         hash_only_large_second.relative_path.generic_string() +
                                         "\",\"length\":" + std::to_string(hash_only_large_bytes.size()) + "}";
  writer.append_metal_event(hash_only_large_second_event);

  apitrace::trace::MetalEventRecord direct_path_event;
  direct_path_event.call_kind = apitrace::trace::MetalCallKind::DeviceCreate;
  direct_path_event.metal_sequence = 14;
  direct_path_event.object_id = 101;
  direct_path_event.function_name = "MTLDevice.newBuffer";
  direct_path_event.blob_refs = {cached_first.blob_id};
  direct_path_event.payload = std::string("{\"path\":\"") +
                              cached_first.relative_path.generic_string() +
                              "\",\"length\":" + std::to_string(cached_bytes.size()) + "}";
  writer.append_metal_event(direct_path_event);

  std::vector<std::uint8_t> sparse_bytes(8 * 1024 * 1024, 0);
  sparse_bytes.front() = 0x41;
  sparse_bytes.back() = 0x42;
  apitrace::trace::AssetRecord sparse_asset;
  sparse_asset.blob_id = 11;
  sparse_asset.kind = apitrace::trace::AssetKind::Buffer;
  sparse_asset.debug_name = "sparse-zero-buffer";
  sparse_asset.payload_bytes = sparse_bytes;
  sparse_asset = writer.register_asset(std::move(sparse_asset));

  apitrace::trace::EventRecord sparse_event;
  sparse_event.kind = apitrace::trace::EventKind::ResourceBlob;
  sparse_event.callsite.sequence = 4;
  sparse_event.object_debug_name = "sparse-zero-buffer";
  sparse_event.blob_refs = {sparse_asset.blob_id};
  sparse_event.payload = std::string("{\"buffer_path\":\"") + sparse_asset.relative_path.generic_string() + "\"}";
  writer.append_call_event(sparse_event);

  apitrace::trace::AnalysisRecord analysis;
  analysis.stream_name = "resource-summary";
  analysis.record_type = "summary";
  analysis.payload = "{\"buffers\":3}";
  writer.append_analysis_record(analysis);

  apitrace::trace::TranslationLinkWriter link_writer;
  apitrace::trace::TranslationLinkStreamOptions link_options;
  link_options.stream_name = "translation-links";
  link_options.producer_name = "trace-writer-assets-test";
  if (!link_writer.open(writer, link_options)) {
    std::cerr << "failed to open translation link writer\n";
    return 1;
  }
  apitrace::trace::TranslationLinkRecord link_record;
  link_record.record_type = "scope";
  link_record.scope_kind = "draw_to_metal_calls";
  link_record.d3d_sequence = 1;
  link_record.metal_sequence_begin = 1;
  link_record.metal_sequence_end = 7;
  link_record.frame_id = 7;
  link_record.payload = "{\"encoder\":\"render\"}";
  link_writer.append_record(link_record);
  link_writer.close();

  apitrace::trace::AssetRecord pipeline_asset;
  pipeline_asset.blob_id = 16;
  pipeline_asset.kind = apitrace::trace::AssetKind::Pipeline;
  pipeline_asset.debug_name = "rewrite-candidate-pipeline";
  const auto pipeline_payload =
      std::string("{\"source\":\"") + first.relative_path.generic_string() +
      "\",\"collision\":\"" + collision.relative_path.generic_string() +
      "\",\"unrelated\":\"prefix-asset-not-a-trace-path\"}\n";
  pipeline_asset.payload_bytes.assign(pipeline_payload.begin(), pipeline_payload.end());
  pipeline_asset = writer.register_asset(std::move(pipeline_asset));
  const auto pipeline_json_path = bundle / pipeline_asset.relative_path;
  const auto resource_json_path = bundle / "textures" / "do-not-scan-resource-dir.json";
  {
    std::ofstream resource_json(resource_json_path, std::ios::binary | std::ios::trunc);
    resource_json << "{\"source\":\"" << first.relative_path.generic_string() << "\"}\n";
  }
  writer.close();
  unset_env_var("APITRACE_WRITER_STATS");
  unset_env_var("APITRACE_ASYNC_LINE_MAX_PENDING");
  unset_env_var("APITRACE_ASYNC_ASSET_MAX_PENDING");
  unset_env_var("APITRACE_ASYNC_ASSET_WORKERS");
  unset_env_var("APITRACE_SYNC_CHECKPOINTS");
  unset_env_var("APITRACE_CHECKPOINT_INTERVAL_MS");
  unset_env_var("APITRACE_CHECKPOINT_EVENT_INTERVAL");
  unset_env_var("APITRACE_CHECKPOINT_ASSET_BYTES");
  if (!bundle_finalize.empty() && run_bundle_finalize(bundle_finalize, bundle) != 0) {
    std::cerr << "bundle-finalize rejected the raw writer bundle\n";
    return 1;
  }

  const auto spooled_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-spooled-unmap");
  std::filesystem::remove_all(spooled_bundle);
  apitrace::trace::AssetRecord spooled_asset;
  std::string spooled_path;
  constexpr std::uint64_t spooled_byte_size = 512ull * 1024ull;
  {
    apitrace::trace::TraceBundleWriter spooled_writer;
    if (!spooled_writer.open(spooled_bundle)) {
      std::cerr << "failed to open spooled-unmap bundle\n";
      return 1;
    }
    spooled_writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "spooled-unmap-test", false});
    std::vector<std::uint8_t> unmap_bytes(spooled_byte_size);
    for (std::size_t index = 0; index < unmap_bytes.size(); ++index)
      unmap_bytes[index] = static_cast<std::uint8_t>((index * 17u) & 0xff);
    spooled_asset.blob_id = 910;
    spooled_asset.kind = apitrace::trace::AssetKind::Buffer;
    spooled_asset.debug_name = "d3d12-resource-unmap";
    spooled_asset.payload_bytes = unmap_bytes;
    spooled_asset = spooled_writer.register_asset(std::move(spooled_asset));
    spooled_path = spooled_asset.relative_path.generic_string();
    apitrace::trace::EventRecord spooled_event;
    spooled_event.kind = apitrace::trace::EventKind::Call;
    spooled_event.callsite.sequence = 1;
    spooled_event.callsite.function_name = "ID3D12Resource::Unmap";
    spooled_event.callsite.result_code = 0;
    spooled_event.blob_refs = {spooled_asset.blob_id};
    spooled_event.payload = std::string("{\"buffer_path\":\"") + spooled_path + "\"}";
    spooled_writer.append_call_event(spooled_event);
    spooled_writer.close();
  }
  const auto raw_spooled_assets_json = read_text(spooled_bundle / "assets.json");
  if (raw_spooled_assets_json.find("\"payload_path\"") == std::string::npos ||
      !contains_json_u64_field(raw_spooled_assets_json, "byte_size", spooled_byte_size) ||
      !std::filesystem::is_regular_file(spooled_bundle / "spool" / "asset-payloads.bin") ||
      std::filesystem::is_regular_file(spooled_bundle / spooled_path)) {
    std::cerr << "d3d12-resource-unmap should use live spool storage before finalize\n";
    return 1;
  }
  if (!bundle_finalize.empty() && run_bundle_finalize(bundle_finalize, spooled_bundle) != 0) {
    std::cerr << "bundle-finalize rejected the spooled-unmap bundle\n";
    return 1;
  }
  const auto finalized_spooled_assets_json = read_text(spooled_bundle / "assets.json");
  std::string finalized_spooled_path;
  if (finalized_spooled_assets_json.find("\"payload_path\"") != std::string::npos ||
      std::filesystem::exists(spooled_bundle / "spool") ||
      !find_asset_path_by_blob(spooled_bundle, spooled_asset.blob_id, finalized_spooled_path) ||
      !std::filesystem::is_regular_file(spooled_bundle / finalized_spooled_path)) {
    std::cerr << "bundle-finalize should materialize spooled assets into normal bundle storage\n";
    return 1;
  }
  const auto finalized_spooled_callstream = read_text(spooled_bundle / "callstream.jsonl");
  if (finalized_spooled_callstream.find("ID3D12Resource::Unmap") == std::string::npos ||
      finalized_spooled_callstream.find("\"blob_refs\":[910]") == std::string::npos ||
      finalized_spooled_callstream.find(finalized_spooled_path) == std::string::npos) {
    std::cerr << "bundle-finalize should preserve and rewrite spooled Unmap blob refs\n";
    return 1;
  }
  if (!bundle_check.empty() && run_bundle_check(bundle_check, spooled_bundle, "") != 0) {
    std::cerr << "bundle-check rejected checksum-valid finalized spooled Unmap payload refs\n";
    return 1;
  }

  const auto destructor_close_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-destructor-close-spool");
  std::filesystem::remove_all(destructor_close_bundle);
  {
    apitrace::trace::TraceBundleWriter destructor_writer;
    if (!destructor_writer.open(destructor_close_bundle)) {
      std::cerr << "failed to open destructor-close-spool bundle\n";
      return 1;
    }
    destructor_writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "destructor-close-spool-test", false});
    std::vector<std::uint8_t> destructor_bytes(96 * 1024, 0x5a);
    apitrace::trace::AssetRecord destructor_asset;
    destructor_asset.blob_id = 911;
    destructor_asset.kind = apitrace::trace::AssetKind::Buffer;
    destructor_asset.debug_name = "d3d12-resource-unmap";
    destructor_asset.payload_bytes = destructor_bytes;
    destructor_asset = destructor_writer.register_asset(std::move(destructor_asset));

    apitrace::trace::EventRecord destructor_event;
    destructor_event.kind = apitrace::trace::EventKind::Call;
    destructor_event.callsite.sequence = 1;
    destructor_event.callsite.function_name = "ID3D12Resource::Unmap";
    destructor_event.callsite.result_code = 0;
    destructor_event.blob_refs = {destructor_asset.blob_id};
    destructor_event.payload = std::string("{\"buffer_path\":\"") +
                               destructor_asset.relative_path.generic_string() + "\"}";
    destructor_writer.append_call_event(destructor_event);
  }
  const auto destructor_assets_json = read_text(destructor_close_bundle / "assets.json");
  if (destructor_assets_json.find("\"payload_path\"") == std::string::npos ||
      !contains_json_u64_field(destructor_assets_json, "blob_id", 911) ||
      !contains_json_u64_field(destructor_assets_json, "byte_size", 96 * 1024) ||
      !std::filesystem::is_regular_file(destructor_close_bundle / "checksums.json")) {
    std::cerr << "TraceBundleWriter destructor should close and publish spooled asset indexes\n";
    return 1;
  }

  const auto lightweight_checkpoint_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-lightweight-asset-checkpoint");
  std::filesystem::remove_all(lightweight_checkpoint_bundle);
  unset_env_var("APITRACE_SYNC_CHECKPOINTS");
  set_env_var("APITRACE_CHECKPOINT_ASSET_BYTES", "1");
  {
    apitrace::trace::TraceBundleWriter checkpoint_writer;
    if (!checkpoint_writer.open(lightweight_checkpoint_bundle)) {
      std::cerr << "failed to open lightweight-asset-checkpoint bundle\n";
      return 1;
    }
    checkpoint_writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "lightweight-asset-checkpoint-test", false});
    std::vector<std::uint8_t> checkpoint_bytes(80 * 1024, 0x31);
    apitrace::trace::AssetRecord checkpoint_asset;
    checkpoint_asset.blob_id = 912;
    checkpoint_asset.kind = apitrace::trace::AssetKind::Buffer;
    checkpoint_asset.debug_name = "d3d12-resource-unmap";
    checkpoint_asset.payload_bytes = checkpoint_bytes;
    checkpoint_asset = checkpoint_writer.register_asset(std::move(checkpoint_asset));

    apitrace::trace::EventRecord checkpoint_event;
    checkpoint_event.kind = apitrace::trace::EventKind::Call;
    checkpoint_event.callsite.sequence = 1;
    checkpoint_event.callsite.function_name = "ID3D12Resource::Unmap";
    checkpoint_event.callsite.result_code = 0;
    checkpoint_event.blob_refs = {checkpoint_asset.blob_id};
    checkpoint_event.payload = std::string("{\"buffer_path\":\"") +
                               checkpoint_asset.relative_path.generic_string() + "\"}";
    checkpoint_writer.append_call_event(checkpoint_event);

    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      const auto assets_json_path = lightweight_checkpoint_bundle / "assets.json";
      if (std::filesystem::is_regular_file(assets_json_path)) {
        const auto assets_json = read_text(assets_json_path);
        if (assets_json.find("\"payload_path\"") != std::string::npos &&
            contains_json_u64_field(assets_json, "blob_id", 912) &&
            contains_json_u64_field(assets_json, "byte_size", 80 * 1024)) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto lightweight_assets_json = read_text(lightweight_checkpoint_bundle / "assets.json");
    if (lightweight_assets_json.find("\"payload_path\"") == std::string::npos ||
        !contains_json_u64_field(lightweight_assets_json, "blob_id", 912) ||
        !contains_json_u64_field(lightweight_assets_json, "byte_size", 80 * 1024) ||
        std::filesystem::is_regular_file(lightweight_checkpoint_bundle / "checksums.json")) {
      std::cerr << "non-sync checkpoint should publish only a lightweight spooled asset index\n";
      return 1;
    }
    checkpoint_writer.close();
  }
  unset_env_var("APITRACE_CHECKPOINT_ASSET_BYTES");

  {
    std::ofstream unopened_output;
    const auto unopened_sparse_result =
        apitrace::trace::TraceBundleWriter::TestHooks::write_payload_sparse_for_test(unopened_output, small_bytes);
    const auto unopened_direct_result =
        apitrace::trace::TraceBundleWriter::TestHooks::write_payload_direct_for_test(unopened_output, small_bytes);
    if (unopened_sparse_result || unopened_direct_result) {
      std::cerr << "unopened asset output streams must report write failure\n";
      return 1;
    }
  }

  const auto retry_spool_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-asset-spool-retry");
  std::filesystem::remove_all(retry_spool_bundle);
  set_env_var("APITRACE_TEST_ASSET_SPOOL_FAIL_BEFORE_SUCCESS", "1");
  {
    apitrace::trace::TraceBundleWriter retry_writer;
    if (!retry_writer.open(retry_spool_bundle)) {
      std::cerr << "failed to open asset-spool-retry bundle\n";
      return 1;
    }
    retry_writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "asset-spool-retry-test", false});
    std::vector<std::uint8_t> retry_bytes(96 * 1024, 0x44);
    apitrace::trace::AssetRecord retry_asset;
    retry_asset.blob_id = 913;
    retry_asset.kind = apitrace::trace::AssetKind::Buffer;
    retry_asset.debug_name = "d3d12-resource-unmap";
    retry_asset.payload_bytes = retry_bytes;
    retry_asset = retry_writer.register_asset(std::move(retry_asset));
    retry_writer.seal_checkpoint();
    retry_writer.close();
  }
  unset_env_var("APITRACE_TEST_ASSET_SPOOL_FAIL_BEFORE_SUCCESS");
  const auto retry_assets_json = read_text(retry_spool_bundle / "assets.json");
  if (!contains_json_u64_field(retry_assets_json, "blob_id", 913) ||
      retry_assets_json.find("\"payload_path\"") == std::string::npos ||
      !std::filesystem::is_regular_file(retry_spool_bundle / "spool" / "asset-payloads.bin") ||
      std::filesystem::is_regular_file(retry_spool_bundle / "capture-incomplete.json") ||
      !std::filesystem::is_regular_file(retry_spool_bundle / "seal-checkpoint-primary.ready")) {
    std::cerr << "retryable spooled asset write should retry and publish durable completion\n";
    return 1;
  }

  const auto terminal_spool_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-asset-spool-terminal-failure");
  std::filesystem::remove_all(terminal_spool_bundle);
  set_env_var("APITRACE_TEST_ASSET_SPOOL_TERMINAL_FAILURE", "1");
  {
    apitrace::trace::TraceBundleWriter terminal_writer;
    if (!terminal_writer.open(terminal_spool_bundle)) {
      std::cerr << "failed to open asset-spool-terminal-failure bundle\n";
      return 1;
    }
    terminal_writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "asset-spool-terminal-test", false});
    std::vector<std::uint8_t> terminal_bytes(96 * 1024, 0x55);
    apitrace::trace::AssetRecord terminal_asset;
    terminal_asset.blob_id = 914;
    terminal_asset.kind = apitrace::trace::AssetKind::Buffer;
    terminal_asset.debug_name = "d3d12-resource-unmap";
    terminal_asset.payload_bytes = terminal_bytes;
    terminal_asset = terminal_writer.register_asset(std::move(terminal_asset));
    terminal_writer.seal_checkpoint();
    terminal_writer.close();
  }
  unset_env_var("APITRACE_TEST_ASSET_SPOOL_TERMINAL_FAILURE");
  const auto terminal_assets_json = read_text(terminal_spool_bundle / "assets.json");
  const auto terminal_incomplete_json = read_text(terminal_spool_bundle / "capture-incomplete.json");
  if (contains_json_u64_field(terminal_assets_json, "blob_id", 914) ||
      terminal_incomplete_json.find("\"status\": \"incomplete\"") == std::string::npos ||
      !contains_json_u64_field(terminal_incomplete_json, "blob_id", 914) ||
      terminal_incomplete_json.find("injected terminal spool write failure") == std::string::npos ||
      std::filesystem::is_regular_file(terminal_spool_bundle / "seal-checkpoint-primary.ready")) {
    std::cerr << "terminal spooled asset write should make the capture loudly incomplete without ready marker\n";
    return 1;
  }

  if (!bundle_finalize.empty()) {
    const auto middle_missing_blob_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-middle-missing-blob");
    if (!write_middle_missing_blob_bundle(middle_missing_blob_bundle)) {
      std::cerr << "failed to write middle-missing-blob bundle\n";
      return 1;
    }
    if (run_bundle_finalize(bundle_finalize, middle_missing_blob_bundle) == 0) {
      std::cerr << "bundle-finalize silently truncated a middle missing blob instead of failing\n";
      return 1;
    }
  }

  const auto triggered_checkpoint_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-triggered-checkpoint");
  std::filesystem::remove_all(triggered_checkpoint_bundle);
  set_env_var("APITRACE_SYNC_CHECKPOINTS", "1");
  set_env_var("APITRACE_CHECKPOINT_EVENT_INTERVAL", "1");
  {
    apitrace::trace::TraceBundleWriter checkpoint_writer;
    if (!checkpoint_writer.open(triggered_checkpoint_bundle)) {
      std::cerr << "failed to open triggered-checkpoint bundle\n";
      return 1;
    }
    checkpoint_writer.write_metadata({apitrace::trace::ApiKind::D3D12, 1, "checkpoint-test", false});
    apitrace::trace::AssetRecord checkpoint_asset;
    checkpoint_asset.blob_id = 901;
    checkpoint_asset.kind = apitrace::trace::AssetKind::Buffer;
    checkpoint_asset.content_hash =
        apitrace::trace::content_hash_bytes(bytes.data(), bytes.size());
    checkpoint_asset.byte_size = bytes.size();
    checkpoint_asset.payload_bytes = bytes;
    checkpoint_asset = checkpoint_writer.register_asset(std::move(checkpoint_asset));

    apitrace::trace::EventRecord checkpoint_event;
    checkpoint_event.kind = apitrace::trace::EventKind::ResourceBlob;
    checkpoint_event.callsite.sequence = 1;
    checkpoint_event.blob_refs = {checkpoint_asset.blob_id};
    checkpoint_event.payload = std::string("{\"buffer_path\":\"") +
                               checkpoint_asset.relative_path.generic_string() + "\"}";
    checkpoint_writer.append_call_event(checkpoint_event);

    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      if (std::filesystem::is_regular_file(triggered_checkpoint_bundle / "assets.json") &&
          std::filesystem::is_regular_file(triggered_checkpoint_bundle / "checksums.json")) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto checkpoint_json = read_text(triggered_checkpoint_bundle / "checksums.json");
    if (!checksum_matches_file(checkpoint_json, triggered_checkpoint_bundle, "callstream.jsonl") ||
        !checksum_matches_file(checkpoint_json, triggered_checkpoint_bundle, "assets.json") ||
        !checksum_matches_file(checkpoint_json, triggered_checkpoint_bundle, checkpoint_asset.relative_path.generic_string())) {
      std::cerr << "event-triggered checkpoint should make an unclosed bundle recoverable after forced termination\n";
      return 1;
    }
  }
  unset_env_var("APITRACE_CHECKPOINT_EVENT_INTERVAL");
  unset_env_var("APITRACE_SYNC_CHECKPOINTS");

  const auto preexisting_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-preexisting-known-hash");
  std::filesystem::remove_all(preexisting_bundle);
  std::vector<std::uint8_t> preexisting_bytes(80 * 1024, 0x71);
  const auto preexisting_hash =
      apitrace::trace::content_hash_bytes(preexisting_bytes.data(), preexisting_bytes.size());
  const auto preexisting_relative_path =
      std::filesystem::path("buffers") / (preexisting_hash + ".buffer");
  std::filesystem::create_directories(preexisting_bundle / "buffers");
  {
    std::ofstream preexisting_output(preexisting_bundle / preexisting_relative_path, std::ios::binary | std::ios::trunc);
    preexisting_output.write(
        reinterpret_cast<const char *>(preexisting_bytes.data()),
        static_cast<std::streamsize>(preexisting_bytes.size()));
  }
  apitrace::trace::TraceBundleWriter preexisting_writer;
  if (!preexisting_writer.open(preexisting_bundle)) {
    std::cerr << "failed to open preexisting known-hash bundle\n";
    return 1;
  }
  apitrace::trace::AssetRecord preexisting_asset;
  preexisting_asset.blob_id = 101;
  preexisting_asset.kind = apitrace::trace::AssetKind::Buffer;
  preexisting_asset.debug_name = "preexisting-known-hash-buffer";
  preexisting_asset.content_hash = preexisting_hash;
  preexisting_asset.byte_size = preexisting_bytes.size();
  preexisting_asset.payload_bytes = preexisting_bytes;
  preexisting_asset = preexisting_writer.register_asset(std::move(preexisting_asset));
  preexisting_writer.close();
  if (preexisting_asset.relative_path != preexisting_relative_path ||
      std::filesystem::file_size(preexisting_bundle / preexisting_relative_path) != preexisting_bytes.size() ||
      !checksum_matches_file(read_text(preexisting_bundle / "checksums.json"), preexisting_bundle, preexisting_relative_path.generic_string())) {
    std::cerr << "preexisting content_hash file was not reused safely\n";
    return 1;
  }

  const auto stale_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-stale-known-hash");
  std::filesystem::remove_all(stale_bundle);
  std::vector<std::uint8_t> stale_bytes(72 * 1024, 0x39);
  const auto stale_hash =
      apitrace::trace::content_hash_bytes(stale_bytes.data(), stale_bytes.size());
  const auto stale_relative_path = std::filesystem::path("buffers") / (stale_hash + ".buffer");
  std::filesystem::create_directories(stale_bundle / "buffers");
  {
    std::ofstream stale_output(stale_bundle / stale_relative_path, std::ios::binary | std::ios::trunc);
    stale_output << "stale";
  }
  apitrace::trace::TraceBundleWriter stale_writer;
  if (!stale_writer.open(stale_bundle)) {
    std::cerr << "failed to open stale known-hash bundle\n";
    return 1;
  }
  apitrace::trace::AssetRecord stale_asset;
  stale_asset.blob_id = 102;
  stale_asset.kind = apitrace::trace::AssetKind::Buffer;
  stale_asset.debug_name = "stale-known-hash-buffer";
  stale_asset.content_hash = stale_hash;
  stale_asset.byte_size = stale_bytes.size();
  stale_asset.payload_bytes = stale_bytes;
  stale_asset = stale_writer.register_asset(std::move(stale_asset));
  stale_writer.close();
  if (stale_asset.relative_path != stale_relative_path ||
      !checksum_matches_file(read_text(stale_bundle / "checksums.json"), stale_bundle, stale_relative_path.generic_string())) {
    std::cerr << "stale content_hash file was not corrected before checksum indexing\n";
    return 1;
  }

  const auto async_stale_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-async-stale-known-hash");
  std::filesystem::remove_all(async_stale_bundle);
  std::vector<std::uint8_t> async_stale_bytes(2 * 1024 * 1024, 0x5a);
  const auto async_stale_hash =
      apitrace::trace::content_hash_bytes(async_stale_bytes.data(), async_stale_bytes.size());
  const auto async_stale_relative_path = std::filesystem::path("buffers") / (async_stale_hash + ".buffer");
  std::filesystem::create_directories(async_stale_bundle / "buffers");
  {
    std::ofstream stale_output(async_stale_bundle / async_stale_relative_path, std::ios::binary | std::ios::trunc);
    stale_output << "stale";
  }
  apitrace::trace::TraceBundleWriter async_stale_writer;
  if (!async_stale_writer.open(async_stale_bundle)) {
    std::cerr << "failed to open async stale known-hash bundle\n";
    return 1;
  }
  apitrace::trace::AssetRecord async_stale_asset;
  async_stale_asset.blob_id = 103;
  async_stale_asset.kind = apitrace::trace::AssetKind::Buffer;
  async_stale_asset.debug_name = "async-stale-known-hash-buffer";
  async_stale_asset.content_hash = async_stale_hash;
  async_stale_asset.byte_size = async_stale_bytes.size();
  async_stale_asset.payload_bytes = async_stale_bytes;
  async_stale_asset = async_stale_writer.register_asset(std::move(async_stale_asset));
  async_stale_writer.close();
  if (async_stale_asset.relative_path != async_stale_relative_path ||
      !checksum_matches_file(read_text(async_stale_bundle / "checksums.json"), async_stale_bundle, async_stale_relative_path.generic_string())) {
    std::cerr << "async stale content_hash file was not corrected before checksum indexing\n";
    return 1;
  }

  apitrace::trace::TraceBundleReader reader;
  if (!reader.open(bundle)) {
    std::cerr << "reader failed to reopen bundle: " << reader.last_error() << "\n";
    return 1;
  }
  if (!reader.has_asset_index()) {
    std::cerr << "reader did not detect assets.json\n";
    return 1;
  }
  const auto checkpoint_tail_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-checkpoint-tail");
  set_env_var("APITRACE_SYNC_CHECKPOINTS", "1");
  if (!write_checkpoint_tail_bundle(checkpoint_tail_bundle)) {
    unset_env_var("APITRACE_SYNC_CHECKPOINTS");
    std::cerr << "failed to write checkpoint-tail bundle\n";
    return 1;
  }
  unset_env_var("APITRACE_SYNC_CHECKPOINTS");
  apitrace::trace::TraceBundleReader checkpoint_tail_reader;
  if (!checkpoint_tail_reader.open(checkpoint_tail_bundle) ||
      checkpoint_tail_reader.events().size() != 1 ||
      checkpoint_tail_reader.events()[0].callsite.sequence != 1) {
    std::cerr << "checkpoint reader should ignore uncheckpointed callstream tail after forced termination";
    if (!checkpoint_tail_reader.last_error().empty()) {
      std::cerr << ": " << checkpoint_tail_reader.last_error();
    }
    std::cerr << "\n";
    return 1;
  }
  if (reader.assets().size() < 21) {
    std::cerr << "expected at least twenty-one reader-visible blob aliases, got " << reader.assets().size() << "\n";
    return 1;
  }
  bool found_first = false;
  bool found_texture = false;
  bool found_collision = false;
  bool found_cached = false;
  bool found_hash_only_first = false;
  bool found_second = false;
  bool found_metal_buffer = false;
  bool found_metal_texture = false;
  bool found_hash_only_second = false;
  bool found_hash_only_large_first = false;
  bool found_hash_only_large_second = false;
  bool found_sparse = false;
  bool found_small_second = false;
  bool found_known_hash_second = false;
  bool found_second_final_record = false;
  std::filesystem::path sparse_reader_path;
  std::filesystem::path pipeline_reader_path;
  for (const auto &asset : reader.assets()) {
    if (!asset_record_matches_file(bundle, asset)) {
      std::cerr << "reader-visible asset metadata did not match the final asset file for blob "
                << asset.blob_id << "\n";
      return 1;
    }
    found_first = found_first || asset.blob_id == first.blob_id;
    found_texture = found_texture || asset.blob_id == generic_texture.blob_id;
    found_collision = found_collision || asset.blob_id == collision.blob_id;
    found_cached = found_cached || asset.blob_id == cached_first.blob_id;
    found_hash_only_first = found_hash_only_first || asset.blob_id == hash_only_first.blob_id;
    found_second = found_second || asset.blob_id == second.blob_id;
    found_metal_buffer = found_metal_buffer || asset.blob_id == metal_buffer.blob_id;
    found_metal_texture = found_metal_texture || asset.blob_id == metal_texture.blob_id;
    found_hash_only_second = found_hash_only_second || asset.blob_id == hash_only_second.blob_id;
    found_hash_only_large_first = found_hash_only_large_first || asset.blob_id == hash_only_large_first.blob_id;
    found_hash_only_large_second = found_hash_only_large_second || asset.blob_id == hash_only_large_second.blob_id;
    found_small_second = found_small_second || asset.blob_id == small_second.blob_id;
    found_known_hash_second = found_known_hash_second || asset.blob_id == known_hash_second.blob_id;
    if (asset.blob_id == second.blob_id &&
        asset.relative_path.generic_string().find("asset-") == std::string::npos &&
        asset.byte_size == bytes.size()) {
      found_second_final_record = true;
    }
	    if (asset.blob_id == sparse_asset.blob_id) {
	      found_sparse = true;
	      sparse_reader_path = asset.relative_path;
	    }
	    if (asset.blob_id == pipeline_asset.blob_id) {
	      pipeline_reader_path = asset.relative_path;
	    }
	  }
	  if (!found_first || !found_texture || !found_collision || !found_cached || !found_hash_only_first ||
	      !found_second || !found_metal_buffer || !found_metal_texture || !found_hash_only_second ||
	      !found_hash_only_large_first || !found_hash_only_large_second ||
	      !found_small_second || !found_known_hash_second || !found_sparse) {
	    std::cerr << "reader-visible blob ids did not match registered assets\n";
	    return 1;
	  }
  if (!found_second_final_record) {
    std::cerr << "async alias completion did not publish the final path and size for the second blob id\n";
    return 1;
  }
	  if (pipeline_reader_path.empty()) {
	    std::cerr << "reader-visible blob ids did not include pipeline asset\n";
	    return 1;
	  }
  if (reader.metal_assets().size() != 3) {
    std::cerr << "Metal-specific known-hash aliases were not indexed as Metal assets\n";
    return 1;
  }
  bool found_metal_known_hash_second = false;
  bool found_metal_known_hash_library = false;
  for (const auto &asset : reader.metal_assets()) {
    found_metal_known_hash_second = found_metal_known_hash_second || asset.blob_id == metal_known_hash_second.blob_id;
    found_metal_known_hash_library = found_metal_known_hash_library || asset.blob_id == metal_known_hash_library.blob_id;
  }
  if (!found_metal_known_hash_second || !found_metal_known_hash_library) {
    std::cerr << "reader-visible Metal blob ids did not include known-hash assets\n";
    return 1;
  }

  const auto corrupt_metal_sideband_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-corrupt-metal-sideband");
  std::filesystem::remove_all(corrupt_metal_sideband_bundle);
  std::filesystem::copy(bundle, corrupt_metal_sideband_bundle, std::filesystem::copy_options::recursive);
  {
    std::ofstream output(
        corrupt_metal_sideband_bundle / "metal-callstream.jsonl",
        std::ios::binary | std::ios::trunc);
    output << "{not valid json";
  }
  if (!refresh_checksum_entry(corrupt_metal_sideband_bundle, "metal-callstream.jsonl") ||
      !refresh_bundle_hash(corrupt_metal_sideband_bundle)) {
    std::cerr << "failed to refresh corrupt-metal-sideband checksums\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader strict_sideband_reader;
  if (strict_sideband_reader.open(corrupt_metal_sideband_bundle)) {
    std::cerr << "strict reader should reject malformed Metal sideband\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader light_sideband_reader;
  apitrace::trace::TraceBundleReader::OpenOptions light_sideband_options;
  light_sideband_options.load_metal_sideband = false;
  if (!light_sideband_reader.open(corrupt_metal_sideband_bundle, light_sideband_options)) {
    std::cerr << "light reader should ignore unused malformed Metal sideband: "
              << light_sideband_reader.last_error() << "\n";
    return 1;
  }
  if (!light_sideband_reader.metadata().has_metal_callstream ||
      !light_sideband_reader.metal_events().empty()) {
    std::cerr << "light reader should preserve Metal sideband presence without parsing events\n";
    return 1;
  }
  apitrace::trace::TraceBundleReader prefix_reader;
  auto prefix_options = light_sideband_options;
  prefix_options.stop_after_sequence = 1;
  if (!prefix_reader.open(corrupt_metal_sideband_bundle, prefix_options)) {
    std::cerr << "prefix reader should open with stop-after-sequence: "
              << prefix_reader.last_error() << "\n";
    return 1;
  }
  if (prefix_reader.events().empty() ||
      prefix_reader.events().back().callsite.sequence != 1) {
    std::cerr << "prefix reader should stop at the requested sequence\n";
    return 1;
  }

  const auto callstream = read_text(bundle / "callstream.jsonl");
  const auto metal_callstream = read_text(bundle / "metal-callstream.jsonl");
  const auto pipeline_json = read_text(bundle / pipeline_reader_path);
  const auto resource_json = read_text(resource_json_path);
  const auto checksums_json = read_text(bundle / "checksums.json");
  const auto assets_json = read_text(bundle / "assets.json");
  const auto writer_stats = read_text(bundle / "analysis" / "writer-stats.jsonl");
  if (assets_json.find("\"content_hash\"") == std::string::npos ||
      assets_json.find("\"fast_fingerprint\"") != std::string::npos ||
      assets_json.find("\"byte_size\"") == std::string::npos ||
      !contains_json_string_field(assets_json, "debug_name", "large-buffer") ||
      !contains_json_string_field(assets_json, "debug_name", "sparse-zero-buffer") ||
      !contains_json_u64_field(assets_json, "byte_size", 8388608) ||
      assets_json.find(expected_metal_library_path.generic_string()) == std::string::npos) {
    std::cerr << "assets.json did not preserve final replay metadata without capture-only fingerprints\n";
    return 1;
  }
  if (callstream.find("asset-") != std::string::npos) {
    std::cerr << "temporary asset path was not rewritten\n";
    return 1;
  }
  if (metal_callstream.find("asset-") != std::string::npos) {
    std::cerr << "temporary Metal asset path was not rewritten\n";
    return 1;
  }
  if (pipeline_json.find("buffers/asset-") != std::string::npos) {
    std::cerr << "temporary asset path was not rewritten in pipeline JSON\n";
    return 1;
  }
  if (resource_json.find("asset-") == std::string::npos) {
    std::cerr << "resource directory JSON should not be scanned by async alias rewrite\n";
    return 1;
  }
  if (checksums_json.find("textures/do-not-scan-resource-dir.json") != std::string::npos) {
    std::cerr << "resource directory JSON should not be discovered by checksum close scanning\n";
    return 1;
  }
  if (pipeline_json.find("buffers/") == std::string::npos || pipeline_json.find(".buffer") == std::string::npos) {
    std::cerr << "final buffer path missing from pipeline JSON\n";
    return 1;
  }
  if (pipeline_json.find("prefix-asset-not-a-trace-path") == std::string::npos) {
    std::cerr << "alias rewrite changed an unrelated asset-like token\n";
    return 1;
  }
  if (callstream.find("buffers/") == std::string::npos || callstream.find(".buffer") == std::string::npos) {
    std::cerr << "final buffer path missing from callstream\n";
    return 1;
  }
  if (metal_callstream.find("buffers/") == std::string::npos || metal_callstream.find("metal/buffers") != std::string::npos) {
    std::cerr << "Metal buffer did not reference the shared generic buffer path\n";
    return 1;
  }
  if (metal_callstream.find("textures/") == std::string::npos || metal_callstream.find("metal/textures") != std::string::npos) {
    std::cerr << "Metal texture did not reference the shared generic texture path in the callstream\n";
    return 1;
  }
  if (metal_texture.relative_path.generic_string().find("textures/") != 0 ||
      metal_texture.relative_path.generic_string().find(".texture") == std::string::npos ||
      metal_texture.relative_path.generic_string().find("metal/textures") != std::string::npos) {
    std::cerr << "Metal texture did not reference the shared generic texture path\n";
    return 1;
  }
  if (count_regular_files(bundle / "buffers") > 8) {
    std::cerr << "expected large duplicate buffers to be deduplicated after async hash/rewrite\n";
    return 1;
  }
  if (count_final_buffer_files(bundle / "buffers") != count_regular_files(bundle / "buffers")) {
    std::cerr << "large async dedup left temporary buffer files on disk\n";
    return 1;
  }
  if (count_regular_files_with_extension(bundle / "textures", ".texture") != 1) {
    std::cerr << "expected one shared generic texture asset\n";
    return 1;
  }
  if (count_regular_files(bundle / "metal" / "buffers") != 0) {
    std::cerr << "expected no Metal-specific duplicate buffer asset\n";
    return 1;
  }
  if (count_regular_files(bundle / "metal" / "textures") != 0) {
    std::cerr << "expected no Metal-specific duplicate texture asset\n";
    return 1;
  }
  if (count_regular_files(bundle / "metal" / "pipelines") != 1) {
    std::cerr << "expected one known-hash Metal pipeline asset\n";
    return 1;
  }
  if (!checksum_matches_file(checksums_json, bundle, "callstream.jsonl") ||
      !checksum_matches_file(checksums_json, bundle, "metal-callstream.jsonl") ||
      !checksum_matches_file(checksums_json, bundle, pipeline_reader_path.generic_string()) ||
      !checksum_matches_file(checksums_json, bundle, "objects/objects.json") ||
      !checksum_matches_file(checksums_json, bundle, "analysis/resource-summary.jsonl") ||
      !checksum_matches_file(checksums_json, bundle, "analysis/translation-links.jsonl") ||
      !checksum_matches_file(checksums_json, bundle, "analysis/writer-stats.jsonl")) {
    std::cerr << "checksum digest did not match rewritten stream or pipeline files\n";
    return 1;
  }
  if (!checksum_matches_file(checksums_json, bundle, sparse_reader_path.generic_string())) {
    std::cerr << "checksum digest did not match sparse buffer asset\n";
    return 1;
  }
  const auto sparse_path = bundle / sparse_reader_path;
  if (std::filesystem::file_size(sparse_path) != sparse_bytes.size()) {
    std::cerr << "sparse buffer logical file size changed\n";
    return 1;
  }
  const auto sparse_allocated = allocated_bytes(sparse_path);
  if (sparse_allocated < sparse_bytes.size()) {
    std::cerr << "sparse asset allocated_bytes=" << sparse_allocated
              << " logical_bytes=" << sparse_bytes.size() << "\n";
  }
  if (writer_stats.find("\"record_type\":\"writer_stats\"") == std::string::npos ||
      json_u64_field(writer_stats, "async_enqueued") < 1 ||
      json_u64_field(writer_stats, "async_queue_rejected") != 0 ||
      json_u64_field(writer_stats, "known_hash_hits") < 2 ||
      json_u64_field(writer_stats, "known_hash_bytes_avoided") < known_hash_bytes.size() + metal_known_hash_bytes.size() ||
      json_u64_field(writer_stats, "asset_writer_hash_and_write_count") != 0 ||
      json_u64_field(writer_stats, "asset_writer_write_without_hash_count") < 1 ||
      json_u64_field(writer_stats, "async_path_aliases") < 1 ||
      json_u64_field(writer_stats, "asset_rewrite_candidates_scanned") < 4 ||
      json_u64_field(writer_stats, "asset_rewrite_candidates_skipped_clean") < 1 ||
      json_u64_field(writer_stats, "asset_rewrite_replacements") < 4 ||
      json_u64_field(writer_stats, "asset_rewrite_digest_reuses") < 1 ||
      json_u64_field(writer_stats, "rewritten_asset_reference_files") < 1 ||
      json_u64_field(writer_stats, "checksum_files") < 12 ||
      json_u64_field(writer_stats, "genericized_metal_resources") != 2 ||
      json_u64_field(writer_stats, "sparse_zero_run_count") != 0 ||
      json_u64_field(writer_stats, "sparse_zero_bytes_skipped") != 0 ||
      json_u64_field(writer_stats, "exact_dedup_skipped_large") < 1 ||
      json_u64_field(writer_stats, "payload_cache_skipped_large") < 1 ||
      json_u64_field(writer_stats, "callstream_peak_pending_bytes") == 0 ||
      json_u64_field(writer_stats, "metal_callstream_peak_pending_bytes") == 0 ||
      json_u64_field(writer_stats, "analysis_peak_pending_bytes") == 0 ||
      json_u64_field(writer_stats, "sync_hash_bytes") != 0 ||
      json_u64_field(writer_stats, "payload_move_registrations") < 15 ||
      json_u64_field(writer_stats, "payload_move_bytes") != json_u64_field(writer_stats, "payload_bytes_seen")) {
    std::cerr << "writer stats did not cover the expected async dedup and rewrite paths\n";
    return 1;
  }
  const auto translation_links = read_text(bundle / "analysis" / "translation-links.jsonl");
  if (translation_links.find("\"scope_kind\":\"draw_to_metal_calls\"") == std::string::npos ||
      translation_links.find("\"payload\":{\"encoder\":\"render\"}") == std::string::npos ||
      translation_links.find("\"payload\":{\"record_type\"") != std::string::npos) {
    std::cerr << "translation link stream lost its raw record schema\n";
    return 1;
  }

  if (!bundle_check.empty()) {
    if (run_bundle_check(bundle_check, bundle, "") != 0) {
      std::cerr << "bundle-check rejected a checksum-valid shared generic resource bundle\n";
      return 1;
    }

    const auto stale_unreferenced_checksum_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-stale-unreferenced-checksum");
    std::filesystem::remove_all(stale_unreferenced_checksum_bundle);
    std::filesystem::copy(
        bundle,
        stale_unreferenced_checksum_bundle,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    std::filesystem::create_directories(stale_unreferenced_checksum_bundle / "analysis");
    {
      std::ofstream output(
          stale_unreferenced_checksum_bundle / "analysis" / "unreferenced-checksum.jsonl",
          std::ios::binary | std::ios::trunc);
      output << "{\"record_type\":\"first\"}\n";
    }
    if (!upsert_checksum_entry(stale_unreferenced_checksum_bundle, "analysis/unreferenced-checksum.jsonl") ||
        !refresh_bundle_hash(stale_unreferenced_checksum_bundle)) {
      std::cerr << "failed to add unreferenced checksum fixture\n";
      return 1;
    }
    {
      std::ofstream output(
          stale_unreferenced_checksum_bundle / "analysis" / "unreferenced-checksum.jsonl",
          std::ios::binary | std::ios::trunc);
      output << "{\"record_type\":\"second\"}\n";
    }
    if (run_bundle_check(bundle_check, stale_unreferenced_checksum_bundle, "") == 0) {
      std::cerr << "bundle-check accepted stale unreferenced checksum content\n";
      return 1;
    }

    const auto missing_asset_index_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-missing-asset-index");
    std::filesystem::remove_all(missing_asset_index_bundle);
    std::filesystem::copy(
        bundle,
        missing_asset_index_bundle,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(missing_asset_index_bundle / "assets.json");
    apitrace::trace::TraceBundleReader legacy_reader;
    if (!legacy_reader.open(missing_asset_index_bundle)) {
      std::cerr << "reader rejected legacy bundle without assets.json: "
                << legacy_reader.last_error() << "\n";
      return 1;
    }
    if (legacy_reader.has_asset_index()) {
      std::cerr << "reader reported an asset index after assets.json was removed\n";
      return 1;
    }
    if (run_bundle_check(bundle_check, missing_asset_index_bundle, "") == 0) {
      std::cerr << "bundle-check accepted a missing file listed in checksums.json\n";
      return 1;
    }

    const auto stale_hash_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-stale-asset-content-hash");
    std::filesystem::remove_all(stale_hash_bundle);
    std::filesystem::copy(
        bundle,
        stale_hash_bundle,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    if (!replace_any_text_in_file(
            stale_hash_bundle / "assets.json",
            {
                {"\"content_hash\":\"", "\"content_hash\":\"0"},
                {"\"content_hash\": \"", "\"content_hash\": \"0"},
            })) {
      std::cerr << "failed to corrupt asset content_hash fixture\n";
      return 1;
    }
    if (!refresh_checksum_entry(stale_hash_bundle, "assets.json") ||
        !refresh_bundle_hash(stale_hash_bundle)) {
      std::cerr << "failed to refresh stale content_hash fixture checksum\n";
      return 1;
    }
    if (run_bundle_check(bundle_check, stale_hash_bundle, "") != 0) {
      std::cerr << "bundle-check rejected checksum-valid stale asset content_hash metadata\n";
      return 1;
    }
    apitrace::trace::TraceBundleReader stale_hash_reader;
    if (stale_hash_reader.open(stale_hash_bundle)) {
      std::cerr << "reader accepted stale asset content_hash metadata\n";
      return 1;
    }
    const auto stale_size_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-stale-asset-byte-size");
    std::filesystem::remove_all(stale_size_bundle);
    std::filesystem::copy(
        bundle,
        stale_size_bundle,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    if (!replace_any_text_in_file(
            stale_size_bundle / "assets.json",
            {
                {"\"byte_size\":2097152", "\"byte_size\":2097153"},
                {"\"byte_size\": 2097152", "\"byte_size\": 2097153"},
            })) {
      std::cerr << "failed to corrupt asset byte_size fixture\n";
      return 1;
    }
    if (!refresh_checksum_entry(stale_size_bundle, "assets.json") ||
        !refresh_bundle_hash(stale_size_bundle)) {
      std::cerr << "failed to refresh stale byte_size fixture checksum\n";
      return 1;
    }
    if (run_bundle_check(bundle_check, stale_size_bundle, "") != 0) {
      std::cerr << "bundle-check rejected checksum-valid stale asset byte_size metadata\n";
      return 1;
    }
    apitrace::trace::TraceBundleReader stale_size_reader;
    if (stale_size_reader.open(stale_size_bundle)) {
      std::cerr << "reader accepted stale asset byte_size metadata\n";
      return 1;
    }

    const auto primary_blob_conflict_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-primary-blob-conflict");
    if (!write_primary_blob_id_conflict_bundle(primary_blob_conflict_bundle)) {
      std::cerr << "failed to write primary-blob-conflict bundle\n";
      return 1;
    }
    if (bundle_finalize.empty() || run_bundle_finalize(bundle_finalize, primary_blob_conflict_bundle) != 0) {
      std::cerr << "bundle-finalize failed to repair a path-proven primary blob_id conflict\n";
      return 1;
    }
    if (run_bundle_check(bundle_check, primary_blob_conflict_bundle, "") != 0) {
      std::cerr << "bundle-check rejected checksum-valid finalized path-proven primary blob_id conflict\n";
      return 1;
    }
    const auto repaired_primary_callstream = read_text(primary_blob_conflict_bundle / "callstream.jsonl");
    if (repaired_primary_callstream.find("\"blob_refs\":[930]") == std::string::npos ||
        repaired_primary_callstream.find("\"blob_refs\":[931]") == std::string::npos) {
      std::cerr << "bundle-finalize did not rewrite conflicting primary blob_refs away from the original duplicate id\n";
      return 1;
    }
    const auto d3d_pipeline_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-d3d-graphics-pipeline");
    if (!write_d3d_graphics_pipeline_bundle(d3d_pipeline_bundle)) {
      std::cerr << "failed to write d3d-graphics-pipeline bundle\n";
      return 1;
    }
    if (run_bundle_check(bundle_check, d3d_pipeline_bundle, "") != 0) {
      std::cerr << "bundle-check rejected checksum-valid D3D graphics pipeline nested shader refs\n";
      return 1;
    }
    const auto shuffled_d3d_pipeline_bundle =
        bundle.parent_path() / (bundle.filename().generic_string() + "-d3d-graphics-pipeline-shuffled-blob-refs");
    if (!write_d3d_graphics_pipeline_bundle(shuffled_d3d_pipeline_bundle, true)) {
      std::cerr << "failed to write shuffled d3d-graphics-pipeline bundle\n";
      return 1;
    }
    if (run_bundle_check(bundle_check, shuffled_d3d_pipeline_bundle, "") != 0) {
      std::cerr << "bundle-check rejected checksum-valid D3D graphics pipeline shuffled blob refs\n";
      return 1;
    }
  }

  apitrace::trace::TraceBundleWriter sideband_writer;
  if (!sideband_writer.open(bundle, apitrace::trace::TraceBundleOpenMode::SidebandOnly)) {
    std::cerr << "failed to open sideband writer\n";
    return 1;
  }
  apitrace::trace::AnalysisRecord appended_analysis;
  appended_analysis.stream_name = "resource-summary";
  appended_analysis.record_type = "sideband";
  appended_analysis.payload = "{\"buffers\":4}";
  sideband_writer.append_analysis_record(appended_analysis);
  apitrace::trace::TranslationLinkWriter sideband_link_writer;
  if (!sideband_link_writer.open(sideband_writer, link_options)) {
    std::cerr << "failed to open sideband translation link writer\n";
    return 1;
  }
  link_record.record_type = "sideband";
  link_record.frame_id = 8;
  sideband_link_writer.append_record(link_record);
  sideband_link_writer.close();
  sideband_writer.close();

  const auto appended_analysis_text = read_text(bundle / "analysis" / "resource-summary.jsonl");
  if (appended_analysis_text.find("\"record_type\":\"summary\"") == std::string::npos ||
      appended_analysis_text.find("\"record_type\":\"sideband\"") == std::string::npos) {
    std::cerr << "sideband analysis append overwrote existing analysis records\n";
    return 1;
  }
  const auto appended_translation_links = read_text(bundle / "analysis" / "translation-links.jsonl");
  if (appended_translation_links.find("\"record_type\":\"scope\"") == std::string::npos ||
      appended_translation_links.find("\"record_type\":\"sideband\"") == std::string::npos) {
    std::cerr << "sideband translation link append overwrote existing link records\n";
    return 1;
  }
  const auto sideband_callstream = read_text(bundle / "callstream.jsonl");
  if (sideband_callstream != callstream) {
    std::cerr << "sideband writer modified the primary D3D callstream\n";
    return 1;
  }

  const auto unsafe_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-unsafe-path");
  std::filesystem::remove_all(unsafe_bundle);
  {
    apitrace::trace::TraceBundleWriter unsafe_writer;
    if (!unsafe_writer.open(unsafe_bundle)) {
      std::cerr << "failed to open unsafe-path bundle\n";
      return 1;
    }
    unsafe_writer.write_object_index({});
    apitrace::trace::EventRecord unsafe_event;
    unsafe_event.kind = apitrace::trace::EventKind::ResourceBlob;
    unsafe_event.callsite.sequence = 1;
    unsafe_event.object_debug_name = "unsafe-path";
    unsafe_event.payload = "{\"buffer_path\":\"../escape.buffer\"}";
    unsafe_writer.append_call_event(unsafe_event);
    unsafe_writer.close();
  }
  apitrace::trace::TraceBundleReader unsafe_reader;
  if (unsafe_reader.open(unsafe_bundle) ||
      unsafe_reader.last_error().find("unsafe asset path reference") == std::string::npos) {
    std::cerr << "reader accepted an unsafe bundle-relative asset path\n";
    return 1;
  }

  const auto legal_frame_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-legal-frame-path");
  std::filesystem::remove_all(legal_frame_bundle);
  {
    apitrace::trace::TraceBundleWriter frame_writer;
    if (!frame_writer.open(legal_frame_bundle)) {
      std::cerr << "failed to open legal-frame-path bundle\n";
      return 1;
    }
    frame_writer.write_object_index({});
    apitrace::trace::AssetRecord frame_asset;
    frame_asset.blob_id = 9;
    frame_asset.kind = apitrace::trace::AssetKind::Texture;
    frame_asset.debug_name = "legal-present-frame";
    frame_asset.payload_bytes.assign(4 * 4, 0x42);
    frame_asset = frame_writer.register_asset(std::move(frame_asset));
    apitrace::trace::EventRecord frame_event;
    frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    frame_event.callsite.sequence = 1;
    frame_event.object_debug_name = "D3D12PresentFrame";
    frame_event.blob_refs = {frame_asset.blob_id};
    frame_event.payload = std::string("{\"frame_index\":0,\"width\":2,\"height\":2,"
                                      "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                      "\"format\":\"rgba8\",\"frame_path\":\"") +
                          frame_asset.relative_path.generic_string() + "\"}";
    frame_writer.append_call_event(frame_event);
    apitrace::trace::EventRecord present_call;
    present_call.kind = apitrace::trace::EventKind::Call;
    present_call.callsite.sequence = 2;
    present_call.callsite.function_name = "IDXGISwapChain::Present";
    present_call.callsite.result_code = 0;
    present_call.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    frame_writer.append_call_event(present_call);
    apitrace::trace::EventRecord present_boundary;
    present_boundary.kind = apitrace::trace::EventKind::Boundary;
    present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
    present_boundary.callsite.sequence = 3;
    present_boundary.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    frame_writer.append_call_event(present_boundary);
    frame_writer.close();
  }
  if (!bundle_check.empty() && run_bundle_check(bundle_check, legal_frame_bundle, "") != 0) {
    std::cerr << "bundle-check rejected checksum-valid legal D3D present-frame bundle\n";
    return 1;
  }

  const auto legal_test_present_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-legal-test-present");
  std::filesystem::remove_all(legal_test_present_bundle);
  {
    apitrace::trace::TraceBundleWriter test_present_writer;
    if (!test_present_writer.open(legal_test_present_bundle)) {
      std::cerr << "failed to open legal-test-present bundle\n";
      return 1;
    }
    test_present_writer.write_object_index({});
    apitrace::trace::EventRecord test_present_call;
    test_present_call.kind = apitrace::trace::EventKind::Call;
    test_present_call.callsite.sequence = 1;
    test_present_call.callsite.function_name = "IDXGISwapChain::Present";
    test_present_call.callsite.result_code = 0;
    test_present_call.payload = "{\"sync_interval\":1,\"flags\":1,\"present_test\":true}";
    test_present_writer.append_call_event(test_present_call);
    apitrace::trace::EventRecord real_present_call;
    real_present_call.kind = apitrace::trace::EventKind::Call;
    real_present_call.callsite.sequence = 2;
    real_present_call.callsite.function_name = "IDXGISwapChain::Present";
    real_present_call.callsite.result_code = 0;
    real_present_call.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    test_present_writer.append_call_event(real_present_call);
    apitrace::trace::EventRecord real_present_boundary;
    real_present_boundary.kind = apitrace::trace::EventKind::Boundary;
    real_present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
    real_present_boundary.callsite.sequence = 3;
    real_present_boundary.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    test_present_writer.append_call_event(real_present_boundary);
    test_present_writer.close();
  }
  if (!bundle_check.empty() && run_bundle_check(bundle_check, legal_test_present_bundle, "") != 0) {
    std::cerr << "bundle-check rejected checksum-valid legal test Present without frame_index\n";
    return 1;
  }

  const auto illegal_frame_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-illegal-frame-path");
  std::filesystem::remove_all(illegal_frame_bundle);
  {
    apitrace::trace::TraceBundleWriter illegal_writer;
    if (!illegal_writer.open(illegal_frame_bundle)) {
      std::cerr << "failed to open illegal-frame-path bundle\n";
      return 1;
    }
    illegal_writer.write_object_index({});
    apitrace::trace::AssetRecord frame_asset;
    frame_asset.blob_id = 9;
    frame_asset.kind = apitrace::trace::AssetKind::Texture;
    frame_asset.debug_name = "illegal-frame";
    frame_asset.payload_bytes.assign(4 * 4, 0x7f);
    frame_asset = illegal_writer.register_asset(std::move(frame_asset));
    apitrace::trace::EventRecord illegal_event;
    illegal_event.kind = apitrace::trace::EventKind::ResourceBlob;
    illegal_event.callsite.sequence = 1;
    illegal_event.object_debug_name = "NotPresentFrame";
    illegal_event.blob_refs = {frame_asset.blob_id};
    illegal_event.payload = std::string("{\"width\":2,\"height\":2,\"row_pitch\":8,\"frame_path\":\"") +
                            frame_asset.relative_path.generic_string() + "\"}";
    illegal_writer.append_call_event(illegal_event);
    illegal_writer.close();
  }

  const auto nonmonotonic_frame_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-nonmonotonic-present-frame");
  std::filesystem::remove_all(nonmonotonic_frame_bundle);
  {
    apitrace::trace::TraceBundleWriter nonmonotonic_writer;
    if (!nonmonotonic_writer.open(nonmonotonic_frame_bundle)) {
      std::cerr << "failed to open nonmonotonic-present-frame bundle\n";
      return 1;
    }
    nonmonotonic_writer.write_object_index({});
    apitrace::trace::AssetRecord first_frame_asset;
    first_frame_asset.blob_id = 90;
    first_frame_asset.kind = apitrace::trace::AssetKind::Texture;
    first_frame_asset.debug_name = "first-present-frame";
    first_frame_asset.payload_bytes.assign(4 * 4, 0x55);
    first_frame_asset = nonmonotonic_writer.register_asset(first_frame_asset);
    apitrace::trace::AssetRecord second_frame_asset;
    second_frame_asset.blob_id = 91;
    second_frame_asset.kind = apitrace::trace::AssetKind::Texture;
    second_frame_asset.debug_name = "second-present-frame";
    second_frame_asset.payload_bytes.assign(4 * 4, 0x66);
    second_frame_asset = nonmonotonic_writer.register_asset(second_frame_asset);
    apitrace::trace::EventRecord first_frame_event;
    first_frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    first_frame_event.callsite.sequence = 1;
    first_frame_event.object_debug_name = "D3D12PresentFrame";
    first_frame_event.blob_refs = {first_frame_asset.blob_id};
    first_frame_event.payload = std::string("{\"frame_index\":3,\"width\":2,\"height\":2,"
                                            "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                            "\"format\":\"rgba8\",\"frame_path\":\"") +
                                first_frame_asset.relative_path.generic_string() + "\"}";
    nonmonotonic_writer.append_call_event(first_frame_event);
    apitrace::trace::EventRecord second_frame_event;
    second_frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    second_frame_event.callsite.sequence = 2;
    second_frame_event.object_debug_name = "D3D12PresentFrame";
    second_frame_event.blob_refs = {second_frame_asset.blob_id};
    second_frame_event.payload = std::string("{\"frame_index\":3,\"width\":2,\"height\":2,"
                                             "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                             "\"format\":\"rgba8\",\"frame_path\":\"") +
                                 second_frame_asset.relative_path.generic_string() + "\"}";
    nonmonotonic_writer.append_call_event(second_frame_event);
    nonmonotonic_writer.close();
  }

  const auto mismatched_d3d_frame_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-mismatched-d3d-present-frame");
  std::filesystem::remove_all(mismatched_d3d_frame_bundle);
  {
    apitrace::trace::TraceBundleWriter mismatched_d3d_writer;
    if (!mismatched_d3d_writer.open(mismatched_d3d_frame_bundle)) {
      std::cerr << "failed to open mismatched-d3d-present-frame bundle\n";
      return 1;
    }
    mismatched_d3d_writer.write_object_index({});
    apitrace::trace::AssetRecord frame_asset;
    frame_asset.blob_id = 92;
    frame_asset.kind = apitrace::trace::AssetKind::Texture;
    frame_asset.debug_name = "mismatched-d3d-present-frame";
    frame_asset.payload_bytes.assign(4 * 4, 0x77);
    frame_asset = mismatched_d3d_writer.register_asset(std::move(frame_asset));
    apitrace::trace::EventRecord frame_event;
    frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    frame_event.callsite.sequence = 1;
    frame_event.object_debug_name = "D3D12PresentFrame";
    frame_event.blob_refs = {frame_asset.blob_id};
    frame_event.payload = std::string("{\"frame_index\":0,\"width\":2,\"height\":2,"
                                      "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                      "\"format\":\"rgba8\",\"frame_path\":\"") +
                          frame_asset.relative_path.generic_string() + "\"}";
    mismatched_d3d_writer.append_call_event(frame_event);
    apitrace::trace::EventRecord present_call;
    present_call.kind = apitrace::trace::EventKind::Call;
    present_call.callsite.sequence = 2;
    present_call.callsite.function_name = "IDXGISwapChain::Present";
    present_call.callsite.result_code = 0;
    present_call.payload = "{\"frame_index\":0,\"sync_interval\":2,\"flags\":0}";
    mismatched_d3d_writer.append_call_event(present_call);
    apitrace::trace::EventRecord present_boundary;
    present_boundary.kind = apitrace::trace::EventKind::Boundary;
    present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
    present_boundary.callsite.sequence = 3;
    present_boundary.payload = "{\"frame_index\":0,\"sync_interval\":2,\"flags\":0}";
    mismatched_d3d_writer.append_call_event(present_boundary);
    mismatched_d3d_writer.close();
  }

  const auto mismatched_d3d_boundary_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-mismatched-d3d-present-boundary");
  std::filesystem::remove_all(mismatched_d3d_boundary_bundle);
  {
    apitrace::trace::TraceBundleWriter mismatched_boundary_writer;
    if (!mismatched_boundary_writer.open(mismatched_d3d_boundary_bundle)) {
      std::cerr << "failed to open mismatched-d3d-present-boundary bundle\n";
      return 1;
    }
    mismatched_boundary_writer.write_object_index({});
    apitrace::trace::EventRecord present_call;
    present_call.kind = apitrace::trace::EventKind::Call;
    present_call.callsite.sequence = 1;
    present_call.callsite.function_name = "IDXGISwapChain::Present";
    present_call.callsite.result_code = 0;
    present_call.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    mismatched_boundary_writer.append_call_event(present_call);
    apitrace::trace::EventRecord present_boundary;
    present_boundary.kind = apitrace::trace::EventKind::Boundary;
    present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
    present_boundary.callsite.sequence = 2;
    present_boundary.payload = "{\"frame_index\":0,\"sync_interval\":2,\"flags\":0}";
    mismatched_boundary_writer.append_call_event(present_boundary);
    mismatched_boundary_writer.close();
  }

  const auto unsupported_present_frame_format_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-unsupported-present-frame-format");
  std::filesystem::remove_all(unsupported_present_frame_format_bundle);
  {
    apitrace::trace::TraceBundleWriter unsupported_format_writer;
    if (!unsupported_format_writer.open(unsupported_present_frame_format_bundle)) {
      std::cerr << "failed to open unsupported-present-frame-format bundle\n";
      return 1;
    }
    unsupported_format_writer.write_object_index({});
    apitrace::trace::AssetRecord frame_asset;
    frame_asset.blob_id = 93;
    frame_asset.kind = apitrace::trace::AssetKind::Texture;
    frame_asset.debug_name = "unsupported-present-frame-format";
    frame_asset.payload_bytes.assign(4 * 4, 0x88);
    frame_asset = unsupported_format_writer.register_asset(std::move(frame_asset));
    apitrace::trace::EventRecord frame_event;
    frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    frame_event.callsite.sequence = 1;
    frame_event.object_debug_name = "D3D12PresentFrame";
    frame_event.blob_refs = {frame_asset.blob_id};
    frame_event.payload = std::string("{\"frame_index\":0,\"width\":2,\"height\":2,"
                                      "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                      "\"format\":\"bgra8\",\"frame_path\":\"") +
                          frame_asset.relative_path.generic_string() + "\"}";
    unsupported_format_writer.append_call_event(frame_event);
    apitrace::trace::EventRecord present_call;
    present_call.kind = apitrace::trace::EventKind::Call;
    present_call.callsite.sequence = 2;
    present_call.callsite.function_name = "IDXGISwapChain::Present";
    present_call.callsite.result_code = 0;
    present_call.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    unsupported_format_writer.append_call_event(present_call);
    apitrace::trace::EventRecord present_boundary;
    present_boundary.kind = apitrace::trace::EventKind::Boundary;
    present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
    present_boundary.callsite.sequence = 3;
    present_boundary.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    unsupported_format_writer.append_call_event(present_boundary);
    unsupported_format_writer.close();
  }

  const auto mismatched_present_frame_blob_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-mismatched-present-frame-blob");
  std::filesystem::remove_all(mismatched_present_frame_blob_bundle);
  {
    apitrace::trace::TraceBundleWriter mismatched_blob_writer;
    if (!mismatched_blob_writer.open(mismatched_present_frame_blob_bundle)) {
      std::cerr << "failed to open mismatched-present-frame-blob bundle\n";
      return 1;
    }
    mismatched_blob_writer.write_object_index({});
    apitrace::trace::AssetRecord frame_asset;
    frame_asset.blob_id = 94;
    frame_asset.kind = apitrace::trace::AssetKind::Texture;
    frame_asset.debug_name = "present-frame-a";
    frame_asset.payload_bytes.assign(4 * 4, 0x91);
    frame_asset = mismatched_blob_writer.register_asset(std::move(frame_asset));
    apitrace::trace::AssetRecord other_asset;
    other_asset.blob_id = 95;
    other_asset.kind = apitrace::trace::AssetKind::Texture;
    other_asset.debug_name = "present-frame-b";
    other_asset.payload_bytes.assign(4 * 4, 0x92);
    other_asset = mismatched_blob_writer.register_asset(std::move(other_asset));
    apitrace::trace::EventRecord first_blob_event;
    first_blob_event.kind = apitrace::trace::EventKind::ResourceBlob;
    first_blob_event.callsite.sequence = 1;
    first_blob_event.object_debug_name = "first-conflicting-blob";
    first_blob_event.blob_refs = {other_asset.blob_id};
    first_blob_event.payload = std::string("{\"texture_path\":\"") + other_asset.relative_path.generic_string() + "\"}";
    mismatched_blob_writer.append_call_event(first_blob_event);
    apitrace::trace::EventRecord frame_event;
    frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    frame_event.callsite.sequence = 2;
    frame_event.object_debug_name = "D3D12PresentFrame";
    frame_event.blob_refs = {other_asset.blob_id};
    frame_event.payload = std::string("{\"frame_index\":0,\"width\":2,\"height\":2,"
                                      "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                      "\"format\":\"rgba8\",\"frame_path\":\"") +
                          frame_asset.relative_path.generic_string() + "\"}";
    mismatched_blob_writer.append_call_event(frame_event);
    apitrace::trace::EventRecord present_call;
    present_call.kind = apitrace::trace::EventKind::Call;
    present_call.callsite.sequence = 3;
    present_call.callsite.function_name = "IDXGISwapChain::Present";
    present_call.callsite.result_code = 0;
    present_call.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    mismatched_blob_writer.append_call_event(present_call);
    apitrace::trace::EventRecord present_boundary;
    present_boundary.kind = apitrace::trace::EventKind::Boundary;
    present_boundary.boundary = apitrace::trace::BoundaryKind::Present;
    present_boundary.callsite.sequence = 4;
    present_boundary.payload = "{\"frame_index\":0,\"sync_interval\":1,\"flags\":0}";
    mismatched_blob_writer.append_call_event(present_boundary);
    mismatched_blob_writer.close();
  }

  const auto present_frame_outside_textures_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-present-frame-outside-textures");
  std::filesystem::remove_all(present_frame_outside_textures_bundle);
  {
    apitrace::trace::TraceBundleWriter outside_textures_writer;
    if (!outside_textures_writer.open(present_frame_outside_textures_bundle)) {
      std::cerr << "failed to open present-frame-outside-textures bundle\n";
      return 1;
    }
    outside_textures_writer.write_object_index({});
    apitrace::trace::AssetRecord frame_asset;
    frame_asset.blob_id = 96;
    frame_asset.kind = apitrace::trace::AssetKind::Buffer;
    frame_asset.debug_name = "present-frame-buffer";
    frame_asset.payload_bytes.assign(4 * 4, 0x93);
    frame_asset = outside_textures_writer.register_asset(std::move(frame_asset));
    apitrace::trace::EventRecord frame_event;
    frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    frame_event.callsite.sequence = 1;
    frame_event.object_debug_name = "D3D12PresentFrame";
    frame_event.blob_refs = {frame_asset.blob_id};
    frame_event.payload = std::string("{\"frame_index\":0,\"width\":2,\"height\":2,"
                                      "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                      "\"format\":\"rgba8\",\"frame_path\":\"") +
                          frame_asset.relative_path.generic_string() + "\"}";
    outside_textures_writer.append_call_event(frame_event);
    outside_textures_writer.close();
  }

  const auto api_specific_resource_bundle =
      bundle.parent_path() / (bundle.filename().generic_string() + "-api-specific-resource-path");
  std::filesystem::remove_all(api_specific_resource_bundle);
  {
    std::filesystem::create_directories(api_specific_resource_bundle / "metal" / "textures");
    std::filesystem::create_directories(api_specific_resource_bundle / "objects");
    {
      std::ofstream metadata(api_specific_resource_bundle / "callstream.jsonl", std::ios::binary | std::ios::trunc);
      metadata << "{\"record_kind\":\"bundle_header\",\"format_version\":1,\"api\":\"Unknown\","
                  "\"producer\":\"api-specific-resource-path-test\","
                  "\"has_metal_callstream\":true,\"entry_file\":\"callstream.jsonl\"}\n";
      metadata << "{\"record_kind\":\"resource_blob\",\"sequence\":1,\"object_id\":0,"
                  "\"object_kind\":\"Unknown\",\"parent_object_id\":0,"
                  "\"debug_name\":\"MetalPresentFrame\",\"object_refs\":[],\"blob_refs\":[1],"
                  "\"payload\":{\"frame_index\":1,\"width\":2,\"height\":2,\"row_pitch\":8,"
                  "\"sync_interval\":1,\"flags\":0,\"format\":\"rgba8\","
                  "\"frame_path\":\"metal/textures/bad.texture\"}}\n";
    }
    {
      std::ofstream objects(api_specific_resource_bundle / "objects" / "objects.json", std::ios::binary | std::ios::trunc);
      objects << "{\n  \"objects\": []\n}\n";
    }
    {
      std::ofstream texture(api_specific_resource_bundle / "metal" / "textures" / "bad.texture", std::ios::binary | std::ios::trunc);
      std::vector<char> bytes(16, '\x22');
      texture.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto callstream_text = read_text(api_specific_resource_bundle / "callstream.jsonl");
    const auto objects_text = read_text(api_specific_resource_bundle / "objects" / "objects.json");
    const auto texture_text = read_text(api_specific_resource_bundle / "metal" / "textures" / "bad.texture");
    std::ofstream checksums(api_specific_resource_bundle / "checksums.json", std::ios::binary | std::ios::trunc);
    checksums << "{\n"
              << "  \"format_version\": 1,\n"
              << "  \"bundle_hash\": \"sha256:manual-test\",\n"
              << "  \"files\": {\n"
              << "    \"callstream.jsonl\": \"sha256:"
              << apitrace::trace::content_hash_bytes(callstream_text.data(), callstream_text.size()) << "\",\n"
              << "    \"objects/objects.json\": \"sha256:"
              << apitrace::trace::content_hash_bytes(objects_text.data(), objects_text.size()) << "\",\n"
              << "    \"metal/textures/bad.texture\": \"sha256:"
              << apitrace::trace::content_hash_bytes(texture_text.data(), texture_text.size()) << "\"\n"
              << "  }\n"
              << "}\n";
  }

  const auto legal_metal_frame_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-legal-metal-present-frame");
  std::filesystem::remove_all(legal_metal_frame_bundle);
  {
    apitrace::trace::TraceBundleWriter legal_metal_writer;
    if (!legal_metal_writer.open(legal_metal_frame_bundle)) {
      std::cerr << "failed to open legal-metal-present-frame bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata legal_metal_metadata;
    legal_metal_metadata.api = apitrace::trace::ApiKind::Unknown;
    legal_metal_metadata.producer = "legal-metal-present-frame-test";
    legal_metal_metadata.has_metal_callstream = true;
    legal_metal_writer.write_metadata(legal_metal_metadata);
    legal_metal_writer.write_object_index({
        {51, apitrace::trace::ObjectKind::CommandList, 0, "command-buffer"},
        {52, apitrace::trace::ObjectKind::Resource, 0, "drawable-texture"},
    });
    apitrace::trace::AssetRecord frame_asset;
    frame_asset.blob_id = 10;
    frame_asset.kind = apitrace::trace::AssetKind::Texture;
    frame_asset.debug_name = "legal-metal-present-frame";
    frame_asset.payload_bytes.assign(4 * 4, 0x33);
    frame_asset = legal_metal_writer.register_asset(std::move(frame_asset));
    apitrace::trace::EventRecord frame_event;
    frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    frame_event.callsite.sequence = 1;
    frame_event.object_debug_name = "MetalPresentFrame";
    frame_event.blob_refs = {frame_asset.blob_id};
    frame_event.payload = std::string("{\"frame_index\":7,\"width\":2,\"height\":2,"
                                      "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                      "\"format\":\"rgba8\",\"frame_path\":\"") +
                          frame_asset.relative_path.generic_string() + "\"}";
    legal_metal_writer.append_call_event(frame_event);
    apitrace::trace::MetalEventRecord present_event;
    present_event.call_kind = apitrace::trace::MetalCallKind::PresentDrawable;
    present_event.metal_sequence = 1;
    present_event.function_name = "MTLCommandBuffer.presentDrawable";
    present_event.object_id = 51;
    present_event.object_refs = {52};
    present_event.payload = "{\"drawable_id\":52,\"frame_index\":7,\"width\":2,\"height\":2,"
                            "\"sync_interval\":1,\"flags\":0}";
    legal_metal_writer.append_metal_event(present_event);
    legal_metal_writer.close();
  }

  const auto mismatched_metal_frame_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-mismatched-metal-present-frame");
  std::filesystem::remove_all(mismatched_metal_frame_bundle);
  {
    apitrace::trace::TraceBundleWriter mismatched_metal_writer;
    if (!mismatched_metal_writer.open(mismatched_metal_frame_bundle)) {
      std::cerr << "failed to open mismatched-metal-present-frame bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata mismatched_metal_metadata;
    mismatched_metal_metadata.api = apitrace::trace::ApiKind::Unknown;
    mismatched_metal_metadata.producer = "mismatched-metal-present-frame-test";
    mismatched_metal_metadata.has_metal_callstream = true;
    mismatched_metal_writer.write_metadata(mismatched_metal_metadata);
    mismatched_metal_writer.write_object_index({
        {61, apitrace::trace::ObjectKind::CommandList, 0, "command-buffer"},
        {62, apitrace::trace::ObjectKind::Resource, 0, "drawable-texture"},
    });
    apitrace::trace::AssetRecord frame_asset;
    frame_asset.blob_id = 11;
    frame_asset.kind = apitrace::trace::AssetKind::Texture;
    frame_asset.debug_name = "mismatched-metal-present-frame";
    frame_asset.payload_bytes.assign(4 * 4, 0x44);
    frame_asset = mismatched_metal_writer.register_asset(std::move(frame_asset));
    apitrace::trace::EventRecord frame_event;
    frame_event.kind = apitrace::trace::EventKind::ResourceBlob;
    frame_event.callsite.sequence = 1;
    frame_event.object_debug_name = "MetalPresentFrame";
    frame_event.blob_refs = {frame_asset.blob_id};
    frame_event.payload = std::string("{\"frame_index\":8,\"width\":2,\"height\":2,"
                                      "\"row_pitch\":8,\"sync_interval\":1,\"flags\":0,"
                                      "\"format\":\"rgba8\",\"frame_path\":\"") +
                          frame_asset.relative_path.generic_string() + "\"}";
    mismatched_metal_writer.append_call_event(frame_event);
    apitrace::trace::MetalEventRecord present_event;
    present_event.call_kind = apitrace::trace::MetalCallKind::PresentDrawable;
    present_event.metal_sequence = 1;
    present_event.function_name = "MTLCommandBuffer.presentDrawable";
    present_event.object_id = 61;
    present_event.object_refs = {62};
    present_event.payload = "{\"drawable_id\":62,\"frame_index\":9,\"width\":2,\"height\":2,"
                            "\"sync_interval\":1,\"flags\":0}";
    mismatched_metal_writer.append_metal_event(present_event);
    mismatched_metal_writer.close();
  }

  const auto missing_object_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-missing-object-ref");
  std::filesystem::remove_all(missing_object_bundle);
  {
    apitrace::trace::TraceBundleWriter missing_object_writer;
    if (!missing_object_writer.open(missing_object_bundle)) {
      std::cerr << "failed to open missing-object-ref bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata missing_object_metadata;
    missing_object_metadata.api = apitrace::trace::ApiKind::Unknown;
    missing_object_metadata.producer = "missing-object-ref-test";
    missing_object_metadata.has_metal_callstream = true;
    missing_object_writer.write_metadata(missing_object_metadata);
    missing_object_writer.write_object_index({});
    apitrace::trace::MetalEventRecord object_metadata;
    object_metadata.call_kind = apitrace::trace::MetalCallKind::ObjectMetadata;
    object_metadata.metal_sequence = 1;
    object_metadata.function_name = "MTLObject.metadata";
    object_metadata.object_id = 30;
    object_metadata.payload =
        "{\"kind\":\"dxmt_texture_view\",\"texture_id\":30,\"source_texture_id\":777,"
        "\"gpu_resource_id\":88,\"pixel_format\":80,\"texture_type\":2,"
        "\"level_start\":0,\"level_count\":1,\"slice_start\":0,\"slice_count\":1,"
        "\"swizzle\":[0,1,2,3]}";
    missing_object_writer.append_metal_event(object_metadata);
    missing_object_writer.close();
  }

  const auto missing_metal_object_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-missing-metal-object");
  std::filesystem::remove_all(missing_metal_object_bundle);
  {
    apitrace::trace::TraceBundleWriter missing_metal_object_writer;
    if (!missing_metal_object_writer.open(missing_metal_object_bundle)) {
      std::cerr << "failed to open missing-metal-object bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata missing_metal_object_metadata;
    missing_metal_object_metadata.api = apitrace::trace::ApiKind::Unknown;
    missing_metal_object_metadata.producer = "missing-metal-object-test";
    missing_metal_object_metadata.has_metal_callstream = true;
    missing_metal_object_writer.write_metadata(missing_metal_object_metadata);
    missing_metal_object_writer.write_object_index({});
    apitrace::trace::MetalEventRecord unknown_encoder_event;
    unknown_encoder_event.call_kind = apitrace::trace::MetalCallKind::DrawPrimitives;
    unknown_encoder_event.metal_sequence = 1;
    unknown_encoder_event.function_name = "MTLRenderCommandEncoder.drawPrimitives";
    unknown_encoder_event.object_id = 444;
    unknown_encoder_event.payload = "{\"primitive_type\":3,\"vertex_start\":0,\"vertex_count\":3}";
    missing_metal_object_writer.append_metal_event(unknown_encoder_event);
    missing_metal_object_writer.close();
  }

  const auto missing_array_object_bundle = bundle.parent_path() / (bundle.filename().generic_string() + "-missing-array-object-ref");
  std::filesystem::remove_all(missing_array_object_bundle);
  {
    apitrace::trace::TraceBundleWriter missing_array_object_writer;
    if (!missing_array_object_writer.open(missing_array_object_bundle)) {
      std::cerr << "failed to open missing-array-object-ref bundle\n";
      return 1;
    }
    apitrace::trace::TraceMetadata missing_array_object_metadata;
    missing_array_object_metadata.api = apitrace::trace::ApiKind::Unknown;
    missing_array_object_metadata.producer = "missing-array-object-ref-test";
    missing_array_object_metadata.has_metal_callstream = true;
    missing_array_object_writer.write_metadata(missing_array_object_metadata);
    missing_array_object_writer.write_object_index({
        {42, apitrace::trace::ObjectKind::CommandList, 0, "encoder"},
        {101, apitrace::trace::ObjectKind::Resource, 0, "known-resource"},
    });
    apitrace::trace::MetalEventRecord use_resources_event;
    use_resources_event.call_kind = apitrace::trace::MetalCallKind::UseResources;
    use_resources_event.metal_sequence = 1;
    use_resources_event.function_name = "MTLRenderCommandEncoder.useResources";
    use_resources_event.object_id = 42;
    use_resources_event.payload = "{\"resource_ids\":[101,777],\"usage\":1,\"stages\":1}";
    missing_array_object_writer.append_metal_event(use_resources_event);
    missing_array_object_writer.close();
  }

  return 0;
}
