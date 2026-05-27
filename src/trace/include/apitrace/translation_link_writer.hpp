#pragma once

#include "apitrace/trace_bundle_io.hpp"

#include <string>

namespace apitrace::trace {

struct TranslationLinkStreamOptions {
  std::string stream_name = "translation-links";
  std::string producer_name;

  // TODO: allow producers to request dedicated sideband files once per-producer stream layout is settled.
};

struct TranslationLinkRecord {
  std::string record_type = "opaque";
  std::string scope_kind;
  std::uint64_t d3d_sequence = 0;
  std::uint64_t metal_sequence_begin = 0;
  std::uint64_t metal_sequence_end = 0;
  std::uint64_t frame_id = 0;
  std::string payload;

  // TODO: keep payload ownership with the translation layer and avoid normalizing its schema inside apitrace.
};

class TranslationLinkWriter {
public:
  TranslationLinkWriter();
  ~TranslationLinkWriter();

  bool open(TraceBundleWriter &bundle_writer, TranslationLinkStreamOptions options);
  void append_record(const TranslationLinkRecord &record);
  void close();

  bool is_open() const noexcept;
  const TranslationLinkStreamOptions &options() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace apitrace::trace
