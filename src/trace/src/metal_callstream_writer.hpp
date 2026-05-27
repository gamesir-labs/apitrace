#pragma once

#include "apitrace/bundle_layout.hpp"
#include "apitrace/metal_event_types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace apitrace::trace::detail {

std::string metal_event_record_json(const MetalEventRecord &event);
bool parse_metal_callstream(
    const std::filesystem::path &callstream_path,
    std::vector<MetalEventRecord> &events,
    std::string &error);
std::string metal_asset_directory_name(MetalAssetKind kind);
std::string metal_asset_extension(MetalAssetKind kind);
bool is_metal_asset_path(const std::filesystem::path &relative_path, MetalAssetKind *kind = nullptr);

} // namespace apitrace::trace::detail
