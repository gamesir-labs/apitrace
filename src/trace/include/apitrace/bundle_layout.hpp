#pragma once

#include <filesystem>

namespace apitrace::trace {

inline constexpr const char *kCallstreamFileName = "callstream.jsonl";
inline constexpr const char *kChecksumsFileName = "checksums.json";
inline constexpr const char *kAnalysisDirectoryName = "analysis";
inline constexpr const char *kTranslationLinksFileName = "translation-links.jsonl";

struct BundleLayout {
  std::filesystem::path root_path;
  std::filesystem::path callstream_path;
  std::filesystem::path checksums_path;
  std::filesystem::path analysis_directory_path;
  std::filesystem::path translation_links_path;
  std::filesystem::path objects_directory_path;
  std::filesystem::path object_index_path;
  std::filesystem::path shaders_directory_path;
  std::filesystem::path textures_directory_path;
  std::filesystem::path buffers_directory_path;
  std::filesystem::path pipelines_directory_path;

  // TODO: add typed paths for objects/, shaders/, textures/, buffers/, pipelines/, and optional analysis streams.
  // TODO: distinguish required root entries from optional readable indexes once bundle validation exists.
};

} // namespace apitrace::trace
