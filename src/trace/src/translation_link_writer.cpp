#include "apitrace/translation_link_writer.hpp"

#include <memory>
#include <utility>

namespace apitrace::trace {

struct TranslationLinkWriter::Impl {
  TraceBundleWriter *bundle_writer = nullptr;
  TranslationLinkStreamOptions options;
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
  // TODO: surface producer metadata in a readable analysis stream header once optional sideband metadata files are defined.
  return true;
}

void TranslationLinkWriter::append_record(const TranslationLinkRecord &record)
{
  if (!impl_->open || !impl_->bundle_writer) {
    return;
  }

  AnalysisRecord analysis_record;
  analysis_record.stream_name = impl_->options.stream_name;
  analysis_record.record_type = record.record_type;
  analysis_record.payload = record.payload;

  impl_->bundle_writer->append_analysis_record(analysis_record);

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
