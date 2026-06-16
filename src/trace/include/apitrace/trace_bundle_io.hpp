#pragma once

#include "apitrace/api_types.hpp"
#include "apitrace/asset_index.hpp"
#include "apitrace/bundle_layout.hpp"
#include "apitrace/checksum_index.hpp"
#include "apitrace/event_types.hpp"
#include "apitrace/metal_event_types.hpp"
#include "apitrace/object_types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace apitrace::trace {

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
  void append_call_event(const EventRecord &event);
  void append_call_event(EventRecord &&event);
  void append_metal_event(const MetalEventRecord &event);
  AssetRecord register_asset(const AssetRecord &asset);
  AssetRecord register_asset(AssetRecord &&asset);
  AssetRecord register_metal_asset(MetalAssetKind kind, const AssetRecord &asset);
  AssetRecord register_metal_asset(MetalAssetKind kind, AssetRecord &&asset);
  void write_object_index(const std::vector<ObjectRecord> &objects);
  void declare_analysis_stream(std::string_view stream_name);
  void append_analysis_line(std::string_view stream_name, std::string_view json_line);
  void append_analysis_record(const AnalysisRecord &record);
  void write_checksum_index(const ChecksumIndex &checksums);
  void flush();
  void checkpoint();
  void seal_checkpoint();
  void close();

  bool is_open() const noexcept;
  const BundleLayout &layout() const noexcept;

private:
  // TODO: split readable index emission from raw asset emission once bundle writing is implemented.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TraceBundleReader {
public:
  struct OpenOptions {
    bool load_metal_sideband = true;
    bool validate_checksum_contents = true;
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
  const std::string &last_error() const noexcept;

private:
  // TODO: split bundle validation from bundle parsing once reader phases become explicit.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace::trace
