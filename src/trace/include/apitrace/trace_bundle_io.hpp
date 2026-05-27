#pragma once

#include "apitrace/api_types.hpp"
#include "apitrace/asset_index.hpp"
#include "apitrace/bundle_layout.hpp"
#include "apitrace/checksum_index.hpp"
#include "apitrace/event_types.hpp"
#include "apitrace/metal_event_types.hpp"
#include "apitrace/object_types.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace apitrace::trace {

struct AnalysisRecord {
  std::string stream_name;
  std::string record_type;
  std::string payload;

  // TODO: keep payload readable by default while allowing producers to own their own schema.
};

class TraceBundleWriter {
public:
  TraceBundleWriter();
  ~TraceBundleWriter();

  bool open(const std::filesystem::path &bundle_root);
  void write_metadata(const TraceMetadata &metadata);
  void append_call_event(const EventRecord &event);
  void append_metal_event(const MetalEventRecord &event);
  AssetRecord register_asset(const AssetRecord &asset);
  AssetRecord register_metal_asset(MetalAssetKind kind, const AssetRecord &asset);
  void write_object_index(const std::vector<ObjectRecord> &objects);
  void declare_analysis_stream(std::string_view stream_name);
  void append_analysis_record(const AnalysisRecord &record);
  void write_checksum_index(const ChecksumIndex &checksums);
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
  TraceBundleReader();
  ~TraceBundleReader();

  bool open(const std::filesystem::path &bundle_root);
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
  const std::string &last_error() const noexcept;

private:
  // TODO: split bundle validation from bundle parsing once reader phases become explicit.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace::trace
