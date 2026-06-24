#pragma once

#include "apitrace/api_types.hpp"
#include "apitrace/asset_index.hpp"
#include "apitrace/bundle_layout.hpp"
#include "apitrace/checksum_index.hpp"
#include "apitrace/event_types.hpp"
#include "apitrace/metal_event_types.hpp"
#include "apitrace/object_types.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace apitrace::trace {

struct TranslationLinkRecord;

std::string event_record_json(const EventRecord &event);

struct AnalysisRecord {
  std::string stream_name;
  std::string record_type;
  std::string payload;

  // TODO: keep payload readable by default while allowing producers to own their own schema.
};

enum class TraceBundleOpenMode {
  Primary,
  SidebandOnly,
};

class TraceBundleWriter {
public:
  struct RegisteredAssetPayload {
    AssetRecord asset;
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
  };

  TraceBundleWriter();
  ~TraceBundleWriter();

  bool open(
      const std::filesystem::path &bundle_root,
      TraceBundleOpenMode mode = TraceBundleOpenMode::Primary);
  void write_metadata(const TraceMetadata &metadata);
  // Runtime capture producers should enqueue raw evidence records only. Derived
  // semantic assets, expensive hashing, deduplication, and crash-tail repair are
  // finalized offline by the bundle tools so API threads do not wait on publish
  // work.
  EventRecord prepare_call_event(EventRecord event) const;
  void append_prepared_call_event(EventRecord &&event);
  void append_prepared_call_event(EventRecord &&event, std::string_view serialized_line);
  void append_call_event(const EventRecord &event);
  void append_call_event(EventRecord &&event);
  void append_existing_header_json_line(std::string_view json_line);
  void append_callstream_json_line(std::string_view json_line);
  void append_metal_event(const MetalEventRecord &event);
  AssetRecord register_asset(const AssetRecord &asset);
  AssetRecord register_asset(AssetRecord &&asset);
  std::vector<RegisteredAssetPayload> registered_asset_payloads_for_blob_refs(
      const std::vector<BlobId> &blob_refs);
  AssetRecord register_metal_asset(MetalAssetKind kind, const AssetRecord &asset);
  AssetRecord register_metal_asset(MetalAssetKind kind, AssetRecord &&asset);
  void write_object_index(const std::vector<ObjectRecord> &objects);
  void declare_analysis_stream(std::string_view stream_name);
  void append_analysis_line(std::string_view stream_name, std::string_view json_line);
  void append_analysis_record(const AnalysisRecord &record);
  void append_translation_link_record(std::string_view stream_name, const TranslationLinkRecord &record);
  void write_checksum_index(const ChecksumIndex &checksums);
  void flush();
  void checkpoint();
  void seal_checkpoint();
  void close();

  bool is_open() const noexcept;
  std::uint64_t initial_call_sequence() const noexcept;
  const BundleLayout &layout() const noexcept;

#if defined(APITRACE_ENABLE_TEST_HOOKS)
  struct TestHooks {
    static bool write_payload_sparse_for_test(
        std::ofstream &output,
        const std::vector<std::uint8_t> &payload);
    static bool write_payload_direct_for_test(
        std::ofstream &output,
        const std::vector<std::uint8_t> &payload);
    static bool hash_and_write_payload_sparse_for_test(
        std::ofstream &output,
        const std::vector<std::uint8_t> &payload);
    static std::uint64_t spool_reserved_offset_for_test(const TraceBundleWriter &writer);
    static std::uint64_t spool_published_offset_for_test(const TraceBundleWriter &writer);
    static AssetRecord reserve_blob_id_for_test(TraceBundleWriter &writer, AssetRecord asset);
    static std::uint64_t blob_id_scan_count_for_test(const TraceBundleWriter &writer);
    static void set_seal_checkpoint_heavy_phase_hook_for_test(void (*hook)());
  };
#endif

private:
  // TODO: split readable index emission from raw asset emission once bundle writing is implemented.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TraceBundleReader {
public:
  struct OpenTiming {
    std::uint64_t parse_assets_index_ms = 0;
    std::uint64_t parse_callstream_ms = 0;
    std::uint64_t register_blob_refs_ms = 0;
  };

  struct OpenOptions {
    bool load_metal_sideband = true;
    bool validate_checksum_contents = true;
    // When false, open() still parses bundle metadata, indexes, and callstream events, but skips
    // validating checksum entries and asset file references. This is for bundle-finalize's
    // in-process reconstruction after assets.json has been rewritten but before checksums.json is
    // refreshed; retrace should keep the default validation path.
    bool validate_file_references = true;
    bool wait_for_present_frame_blob = false;
    // When false, open() parses the bundle header (metadata) and validates assets/checksums but
    // skips parsing the callstream into events(). Used by the retrace replay-model fast path, where
    // the persisted object model replaces reconstruction and the event stream is never consumed —
    // skipping the multi-GB callstream parse. The caller must re-open with this true if it then
    // needs events() (e.g. model load failed and it must fall back to reconstruction).
    bool parse_callstream_events = true;
    // When true, open() discovers extra readable asset records from event payload paths and
    // validates blob_refs against assets.json while parsing the streams. Tools that perform their
    // own full reference validation can disable this to avoid a duplicate serial stream pass.
    bool discover_referenced_assets = true;
    bool collect_open_timing = false;
    // For D3D12 diagnostic replay, a stop sequence can target an event recorded inside a command
    // list. The reader still has to include the later ExecuteCommandLists event that submits that
    // list, while the native replayer performs the actual in-list truncation.
    bool extend_stop_after_sequence_to_command_list_submit = false;
    std::uint64_t stop_after_sequence = 0;
    std::uint64_t stop_after_present_frame = 0;
  };

  TraceBundleReader();
  ~TraceBundleReader();

  bool open(const std::filesystem::path &bundle_root);
  bool open(const std::filesystem::path &bundle_root, const OpenOptions &options);
  void close();

  bool is_open() const noexcept;
  const BundleLayout &layout() const noexcept;
  const TraceMetadata &metadata() const noexcept;
  const std::vector<EventRecord> &events() const noexcept;
  const std::vector<MetalEventRecord> &metal_events() const noexcept;
  const std::vector<AssetRecord> &assets() const noexcept;
  const std::vector<AssetRecord> &metal_assets() const noexcept;
  const std::vector<ObjectRecord> &objects() const noexcept;
  const ChecksumIndex &checksums() const noexcept;
  const std::unordered_set<std::string> &validated_checksum_paths() const noexcept;
  bool has_asset_index() const noexcept;
  bool prefix_limited() const noexcept;
  const OpenTiming &open_timing() const noexcept;
  const std::string &last_error() const noexcept;

private:
  // TODO: split bundle validation from bundle parsing once reader phases become explicit.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace::trace
