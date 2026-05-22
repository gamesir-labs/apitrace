#include "apitrace/trace_bundle_io.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace apitrace::trace {

struct TraceBundleWriter::Impl {
  BundleLayout layout;
  TraceMetadata metadata;
  std::vector<EventRecord> events;
  std::vector<AssetRecord> assets;
  std::vector<ObjectRecord> objects;
  std::vector<std::string> analysis_streams;
  std::vector<AnalysisRecord> analysis_records;
  ChecksumIndex checksums;
  bool open = false;

  // TODO: split buffered readable indexes from buffered raw asset writes once persistence begins.
  // TODO: add explicit writer-phase state so open/write/close sequencing can be validated.
};

TraceBundleWriter::TraceBundleWriter() : impl_(std::make_unique<Impl>()) {}

TraceBundleWriter::~TraceBundleWriter() = default;

bool TraceBundleWriter::open(const std::filesystem::path &bundle_root)
{
  impl_->layout.root_path = bundle_root;
  impl_->layout.callstream_path = bundle_root / kCallstreamFileName;
  impl_->layout.checksums_path = bundle_root / kChecksumsFileName;
  impl_->layout.analysis_directory_path = bundle_root / kAnalysisDirectoryName;
  impl_->layout.translation_links_path = impl_->layout.analysis_directory_path / kTranslationLinksFileName;
  impl_->open = true;

  // TODO: create the bundle directory tree before any readable index is emitted.
  // TODO: write placeholder root files so partially captured bundles remain inspectable.
  return true;
}

void TraceBundleWriter::write_metadata(const TraceMetadata &metadata)
{
  impl_->metadata = metadata;
  // TODO: emit readable metadata once the bundle file format is fixed.
  // TODO: decide whether metadata belongs in its own root file or the first callstream record.
}

void TraceBundleWriter::append_call_event(const EventRecord &event)
{
  impl_->events.push_back(event);
  // TODO: append one JSONL line per event to callstream.jsonl.
  // TODO: map typed event payloads into readable callstream schema records.
}

void TraceBundleWriter::register_asset(const AssetRecord &asset)
{
  impl_->assets.push_back(asset);
  // TODO: route each asset into shaders/, textures/, buffers/, or pipelines/.
  // TODO: derive relative paths from asset kind instead of trusting callers to precompute them.
}

void TraceBundleWriter::write_object_index(const std::vector<ObjectRecord> &objects)
{
  impl_->objects = objects;
  // TODO: materialize objects/objects.json as a readable object graph index.
  // TODO: decide whether object index emission happens incrementally or only at close().
}

void TraceBundleWriter::declare_analysis_stream(std::string_view stream_name)
{
  if (stream_name.empty()) {
    return;
  }

  const auto already_declared = std::find_if(
      impl_->analysis_streams.begin(),
      impl_->analysis_streams.end(),
      [stream_name](const std::string &existing_stream_name) {
        return existing_stream_name == stream_name;
      }) != impl_->analysis_streams.end();
  if (!already_declared) {
    impl_->analysis_streams.emplace_back(stream_name);
  }

  // TODO: materialize per-stream analysis file selection once optional sideband layout is finalized.
}

void TraceBundleWriter::append_analysis_record(const AnalysisRecord &record)
{
  impl_->analysis_records.push_back(record);
  // TODO: append readable JSONL records under analysis/ without forcing sideband producers to touch bundle paths.
  // TODO: validate that record.stream_name was declared before writing when writer-phase checks exist.
}

void TraceBundleWriter::write_checksum_index(const ChecksumIndex &checksums)
{
  impl_->checksums = checksums;
  // TODO: write checksums.json after callstream and asset paths are known.
  // TODO: centralize hashing policy so bundle hash and per-file hashes cannot diverge by caller.
}

void TraceBundleWriter::close()
{
  // TODO: flush callstream.jsonl in sequence order and verify monotonic event numbering.
  // TODO: flush analysis sideband streams before sealing checksums.json.
  // TODO: flush readable secondary indexes before sealing checksums.json.
  // TODO: emit bundle-finalization diagnostics when required files were never written.
  impl_->open = false;
}

bool TraceBundleWriter::is_open() const noexcept
{
  return impl_ && impl_->open;
}

const BundleLayout &TraceBundleWriter::layout() const noexcept
{
  return impl_->layout;
}

} // namespace apitrace::trace
