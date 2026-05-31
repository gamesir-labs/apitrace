#pragma once

#include "apitrace/object_types.hpp"

#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

namespace apitrace::trace {

void append_payload_object_refs(const nlohmann::json &payload, std::vector<ObjectId> &object_refs);
void append_payload_text_object_refs(std::string_view payload_text, std::vector<ObjectId> &object_refs);

} // namespace apitrace::trace
