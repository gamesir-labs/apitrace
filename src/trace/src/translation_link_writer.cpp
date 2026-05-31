#include "apitrace/translation_link_writer.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <utility>

namespace apitrace::trace {

namespace {

using json = nlohmann::json;

std::string translation_link_record_json(const TranslationLinkRecord &record)
{
  json encoded = {
      {"record_type", record.record_type},
      {"scope_kind", record.scope_kind},
      {"d3d_sequence", record.d3d_sequence},
      {"metal_sequence_begin", record.metal_sequence_begin},
      {"metal_sequence_end", record.metal_sequence_end},
      {"frame_id", record.frame_id},
      {"payload", record.payload.empty() ? json::object() : json::parse(record.payload, nullptr, false)},
  };
  if (encoded["payload"].is_discarded()) {
    encoded["payload"] = json::object();
  }
  return encoded.dump();
}

} // namespace

struct TranslationLinkWriter::Impl {
  TraceBundleWriter *bundle_writer = nullptr;
  TranslationLinkStreamOptions options;
  bool open = false;

  // TODO: support multiple concurrent link streams without requiring the caller to manage bundle file naming.
};

TranslationLinkWriter::TranslationLinkWriter() : impl_(std::make_unique<Impl>()) {}

TranslationLinkWriter::~TranslationLinkWriter() = default;

bool TranslationLinkWriter::open(TraceBundleWriter &bundle_writer, TranslationLinkStreamOptions options)
{
  if (!bundle_writer.is_open()) {
    return false;
  }

  impl_->bundle_writer = &bundle_writer;
  impl_->options = std::move(options);
  impl_->open = true;

  impl_->bundle_writer->declare_analysis_stream(impl_->options.stream_name);
  // TODO: surface producer metadata in a readable analysis stream header once optional sideband metadata files are defined.
  return true;
}

void TranslationLinkWriter::append_record(const TranslationLinkRecord &record)
{
  if (!impl_->open || !impl_->bundle_writer) {
    return;
  }

  impl_->bundle_writer->append_analysis_line(impl_->options.stream_name, translation_link_record_json(record));

  // TODO: let callers opt into explicit flush boundaries when translated-call linking volume becomes large.
}

void TranslationLinkWriter::close()
{
  impl_->bundle_writer = nullptr;
  impl_->open = false;
}

bool TranslationLinkWriter::is_open() const noexcept
{
  return impl_ && impl_->open;
}

const TranslationLinkStreamOptions &TranslationLinkWriter::options() const noexcept
{
  return impl_->options;
}

} // namespace apitrace::trace
