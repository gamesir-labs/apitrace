#include "apitrace/translation_link_writer.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

namespace apitrace::trace {

namespace {

using json = nlohmann::json;

std::filesystem::path analysis_path_for_stream(const BundleLayout &layout, std::string_view stream_name)
{
  std::filesystem::path name(stream_name);
  if (!name.has_extension()) {
    name += ".jsonl";
  }
  return layout.analysis_directory_path / name;
}

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
  std::ofstream stream;
  bool open = false;

  // TODO: buffer sideband records only when translation-layer callers need transactional flush semantics.
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
  const auto stream_path = impl_->options.stream_name == "translation-links"
                               ? impl_->bundle_writer->layout().translation_links_path
                               : analysis_path_for_stream(impl_->bundle_writer->layout(), impl_->options.stream_name);
  impl_->stream.open(stream_path, std::ios::binary | std::ios::app);
  // TODO: surface producer metadata in a readable analysis stream header once optional sideband metadata files are defined.
  return impl_->stream.is_open();
}

void TranslationLinkWriter::append_record(const TranslationLinkRecord &record)
{
  if (!impl_->open || !impl_->bundle_writer) {
    return;
  }

  impl_->stream << translation_link_record_json(record) << "\n";

  // TODO: let callers opt into explicit flush boundaries when translated-call linking volume becomes large.
}

void TranslationLinkWriter::close()
{
  if (impl_->stream.is_open()) {
    impl_->stream.flush();
    impl_->stream.close();
  }
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
