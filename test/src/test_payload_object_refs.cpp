#include "trace/src/payload_object_refs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool has_ref(const std::vector<apitrace::trace::ObjectId> &refs, apitrace::trace::ObjectId object_id)
{
  return std::find(refs.begin(), refs.end(), object_id) != refs.end();
}

bool expect_refs(
    const std::vector<apitrace::trace::ObjectId> &refs,
    std::initializer_list<apitrace::trace::ObjectId> present,
    std::initializer_list<apitrace::trace::ObjectId> absent)
{
  for (const auto object_id : present) {
    if (!has_ref(refs, object_id)) {
      std::cerr << "missing object ref " << object_id << "\n";
      return false;
    }
  }
  for (const auto object_id : absent) {
    if (has_ref(refs, object_id)) {
      std::cerr << "unexpected object ref " << object_id << "\n";
      return false;
    }
  }
  return true;
}

bool test_text_payload_refs()
{
  const std::string payload =
      R"({"command_buffer_id":3,"buffer_ids":[8,9,0],"render_pass_info":"{\"colors\":[{\"texture\":5,\"resolve_texture\":6}],\"depth\":{\"texture\":7}}","ignored_texture":13})";
  std::vector<apitrace::trace::ObjectId> refs;
  apitrace::trace::append_payload_text_object_refs(payload, refs);
  return expect_refs(refs, {3, 5, 6, 7, 8, 9}, {0, 13});
}

bool test_dom_nested_string_refs()
{
  nlohmann::json payload;
  payload["texture_id"] = 12;
  payload["render_pass_info"] =
      R"({"colors":[{"texture":11,"resolve_texture":14}],"stencil":{"texture":15}})";
  payload["nested"] = R"({"command_buffer_id":16,"buffer_ids":[17]})";

  std::vector<apitrace::trace::ObjectId> refs;
  apitrace::trace::append_payload_object_refs(payload, refs);
  return expect_refs(refs, {11, 12, 14, 15, 16, 17}, {});
}

bool test_invalid_text_is_ignored()
{
  std::vector<apitrace::trace::ObjectId> refs;
  apitrace::trace::append_payload_text_object_refs("not json: \"buffer_id\": 99", refs);
  return refs.empty();
}

} // namespace

int main()
{
  if (!test_text_payload_refs())
    return 1;
  if (!test_dom_nested_string_refs())
    return 1;
  if (!test_invalid_text_is_ignored())
    return 1;
  return 0;
}
