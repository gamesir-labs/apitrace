#include "apitrace/trace_bundle_io.hpp"

#include <memory>

namespace apitrace::trace {

struct TraceBundleReader::Impl {
  BundleLayout layout;
  TraceMetadata metadata;
  std::vector<EventRecord> events;
  std::vector<AssetRecord> assets;
  ChecksumIndex checksums;
  bool open = false;

  // TODO: track reader phases explicitly so validation, parsing, and asset discovery can fail independently.
  // TODO: cache parsed readable indexes separately from raw asset lookup results.
};

TraceBundleReader::TraceBundleReader() : impl_(std::make_unique<Impl>()) {}

TraceBundleReader::~TraceBundleReader() = default;

bool TraceBundleReader::open(const std::filesystem::path &bundle_root)
{
  impl_->layout.root_path = bundle_root;
  impl_->layout.callstream_path = bundle_root / kCallstreamFileName;
  impl_->layout.checksums_path = bundle_root / kChecksumsFileName;
  impl_->open = true;

  // TODO: load checksums.json first so every later read can be tied to declared bundle contents.
  // TODO: parse callstream.jsonl separately from secondary readable indexes to keep replay bootstrapping narrow.
  // TODO: add explicit missing-file diagnostics for required root entries before any event parsing starts.
  return true;
}

void TraceBundleReader::close()
{
  // TODO: release parsed callstream and index caches once asset streaming exists.
  impl_->open = false;
}

bool TraceBundleReader::is_open() const noexcept
{
  return impl_ && impl_->open;
}

const BundleLayout &TraceBundleReader::layout() const noexcept
{
  return impl_->layout;
}

const TraceMetadata &TraceBundleReader::metadata() const noexcept
{
  return impl_->metadata;
}

const std::vector<EventRecord> &TraceBundleReader::events() const noexcept
{
  return impl_->events;
}

const std::vector<AssetRecord> &TraceBundleReader::assets() const noexcept
{
  return impl_->assets;
}

const ChecksumIndex &TraceBundleReader::checksums() const noexcept
{
  return impl_->checksums;
}

} // namespace apitrace::trace
