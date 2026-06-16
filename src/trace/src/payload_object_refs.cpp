#include "payload_object_refs.hpp"

#include <algorithm>
#include <charconv>
#include <string>

namespace apitrace::trace {

namespace {

struct JsonStringToken {
  std::string storage;
  std::string_view value;
};

bool is_digit(char ch)
{
  return ch >= '0' && ch <= '9';
}

bool is_space(char ch)
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

void skip_ws(std::string_view text, std::size_t &pos)
{
  while (pos < text.size() && is_space(text[pos])) {
    ++pos;
  }
}

bool ends_with(std::string_view text, std::string_view suffix)
{
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool starts_like_json(std::string_view text)
{
  std::size_t pos = 0;
  skip_ws(text, pos);
  return pos < text.size() && (text[pos] == '{' || text[pos] == '[');
}

bool parse_json_string_token(std::string_view text, std::size_t &pos, JsonStringToken &token)
{
  token.storage.clear();
  token.value = {};
  if (pos >= text.size() || text[pos] != '"') {
    return false;
  }

  ++pos;
  const auto start = pos;
  auto chunk_start = pos;
  bool escaped = false;
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == '"') {
      if (!escaped) {
        token.value = text.substr(start, pos - start - 1);
      } else {
        token.storage.append(text.substr(chunk_start, pos - chunk_start - 1));
        token.value = token.storage;
      }
      return true;
    }
    if (ch != '\\' || pos >= text.size()) {
      continue;
    }

    escaped = true;
    token.storage.append(text.substr(chunk_start, pos - chunk_start - 1));
    const char escape = text[pos++];
    switch (escape) {
    case '"':
    case '\\':
    case '/':
      token.storage.push_back(escape);
      break;
    case 'b':
      token.storage.push_back('\b');
      break;
    case 'f':
      token.storage.push_back('\f');
      break;
    case 'n':
      token.storage.push_back('\n');
      break;
    case 'r':
      token.storage.push_back('\r');
      break;
    case 't':
      token.storage.push_back('\t');
      break;
    case 'u':
      if (pos + 4 <= text.size()) {
        pos += 4;
      }
      token.storage.push_back('?');
      break;
    default:
      token.storage.push_back(escape);
      break;
    }
    chunk_start = pos;
  }
  return false;
}

void append_object_ref(std::vector<ObjectId> &refs, ObjectId object_id)
{
  if (object_id == 0) {
    return;
  }
  if (std::find(refs.begin(), refs.end(), object_id) == refs.end()) {
    refs.push_back(object_id);
  }
}

bool is_object_reference_key(std::string_view key)
{
  static constexpr std::string_view kExactKeys[] = {
      "buffer_id",
      "buffer_object_id",
      "color_texture_id",
      "command_buffer_id",
      "depth_stencil_state_id",
      "depth_texture_id",
      "destination_buffer_id",
      "destination_texture",
      "destination_texture_id",
      "drawable_id",
      "encoder_id",
      "fence_id",
      "heap_id",
      "indirect_buffer_id",
      "index_buffer_id",
      "pipeline_state_id",
      "pipeline_state_object_id",
      "resource_id",
      "resolved_resource_object_id",
      "sampler_id",
      "sampler_state_id",
      "sampler_state_object_id",
      "source_buffer",
      "source_buffer_id",
      "source_texture",
      "source_texture_id",
      "texture_id",
      "texture_object_id",
      "resolve_texture_id",
      "stencil_texture_id",
  };
  for (const auto exact_key : kExactKeys) {
    if (key == exact_key) {
      return true;
    }
  }
  return ends_with(key, "_object_id");
}

bool is_object_reference_array_key(std::string_view key)
{
  static constexpr std::string_view kExactKeys[] = {
      "blend_state_ids",
      "buffer_ids",
      "buffer_object_ids",
      "command_allocator_ids",
      "command_buffer_ids",
      "command_list_ids",
      "command_queue_ids",
      "constant_buffer_view_ids",
      "depth_stencil_state_ids",
      "depth_stencil_view_ids",
      "descriptor_heap_ids",
      "encoder_ids",
      "fence_ids",
      "heap_ids",
      "input_layout_ids",
      "object_ids",
      "pipeline_state_ids",
      "pipeline_state_object_ids",
      "rasterizer_state_ids",
      "render_target_view_ids",
      "resource_ids",
      "resource_object_ids",
      "root_signature_ids",
      "sampler_ids",
      "sampler_state_ids",
      "sampler_state_object_ids",
      "shader_ids",
      "shader_resource_view_ids",
      "texture_ids",
      "texture_object_ids",
      "unordered_access_view_ids",
      "view_ids",
  };
  for (const auto exact_key : kExactKeys) {
    if (key == exact_key) {
      return true;
    }
  }
  return ends_with(key, "_object_ids");
}

bool is_render_pass_texture_key(std::string_view key)
{
  return key == "texture" || key == "resolve_texture";
}

bool parse_integer_value(std::string_view text, std::size_t pos, ObjectId &value)
{
  skip_ws(text, pos);
  if (pos >= text.size() || text[pos] == '-') {
    return false;
  }
  const auto begin = pos;
  while (pos < text.size() && is_digit(text[pos])) {
    ++pos;
  }
  if (begin == pos) {
    return false;
  }
  if (pos < text.size() && (text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E')) {
    return false;
  }
  ObjectId parsed = 0;
  const auto *first = text.data() + begin;
  const auto *last = text.data() + pos;
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc()) {
    return false;
  }
  value = parsed;
  return true;
}

std::size_t find_json_container_end(std::string_view text, std::size_t pos)
{
  if (pos >= text.size() || (text[pos] != '{' && text[pos] != '[')) {
    return pos;
  }

  unsigned object_depth = 0;
  unsigned array_depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (; pos < text.size(); ++pos) {
    const char ch = text[pos];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '{') {
      ++object_depth;
      continue;
    }
    if (ch == '[') {
      ++array_depth;
      continue;
    }
    if (ch == '}') {
      if (object_depth == 0) {
        return pos;
      }
      --object_depth;
    } else if (ch == ']') {
      if (array_depth == 0) {
        return pos;
      }
      --array_depth;
    }
    if (object_depth == 0 && array_depth == 0) {
      return pos + 1;
    }
  }
  return text.size();
}

void scan_numeric_array_values(std::string_view text, std::size_t pos, std::vector<ObjectId> &refs)
{
  if (pos >= text.size() || text[pos] != '[') {
    return;
  }
  ++pos;
  while (pos < text.size()) {
    skip_ws(text, pos);
    if (pos >= text.size() || text[pos] == ']') {
      return;
    }
    if (text[pos] == '"') {
      JsonStringToken ignored;
      parse_json_string_token(text, pos, ignored);
      continue;
    }
    if (text[pos] == '{' || text[pos] == '[') {
      pos = find_json_container_end(text, pos);
      continue;
    }
    ObjectId value = 0;
    if (parse_integer_value(text, pos, value)) {
      append_object_ref(refs, value);
      while (pos < text.size() && is_digit(text[pos])) {
        ++pos;
      }
      continue;
    }
    ++pos;
  }
}

void append_payload_text_object_refs_impl(
    std::string_view payload_text,
    std::vector<ObjectId> &object_refs,
    bool render_pass_context)
{
  if (!starts_like_json(payload_text)) {
    return;
  }

  std::size_t pos = 0;
  while (pos < payload_text.size()) {
    if (payload_text[pos] != '"') {
      ++pos;
      continue;
    }

    JsonStringToken key;
    if (!parse_json_string_token(payload_text, pos, key)) {
      ++pos;
      continue;
    }

    skip_ws(payload_text, pos);
    if (pos >= payload_text.size() || payload_text[pos] != ':') {
      continue;
    }
    ++pos;
    skip_ws(payload_text, pos);
    const auto value_pos = pos;

    if (is_object_reference_key(key.value) ||
        (render_pass_context && is_render_pass_texture_key(key.value))) {
      ObjectId object_id = 0;
      if (parse_integer_value(payload_text, value_pos, object_id)) {
        append_object_ref(object_refs, object_id);
      }
    }

    if (is_object_reference_array_key(key.value)) {
      scan_numeric_array_values(payload_text, value_pos, object_refs);
    }

    const bool nested_render_pass_context =
        render_pass_context || key.value == "render_pass_info";
    if (value_pos < payload_text.size() && payload_text[value_pos] == '"') {
      JsonStringToken value;
      if (parse_json_string_token(payload_text, pos, value)) {
        if (starts_like_json(value.value)) {
          append_payload_text_object_refs_impl(value.value, object_refs, nested_render_pass_context);
        }
        continue;
      }
    }

    if (nested_render_pass_context &&
        value_pos < payload_text.size() &&
        (payload_text[value_pos] == '{' || payload_text[value_pos] == '[')) {
      const auto end = find_json_container_end(payload_text, value_pos);
      if (end > value_pos) {
        append_payload_text_object_refs_impl(
            payload_text.substr(value_pos, end - value_pos),
            object_refs,
            true);
        pos = end;
        continue;
      }
    }

    pos = value_pos;
  }
}

void collect_render_pass_texture_refs(const nlohmann::json &pass, std::vector<ObjectId> &refs)
{
  if (!pass.is_object()) {
    return;
  }

  if (const auto it = pass.find("render_pass_info"); it != pass.end()) {
    collect_render_pass_texture_refs(*it, refs);
  }
  for (const auto *key : {"color_texture_id", "drawable_id", "depth_texture_id", "stencil_texture_id"}) {
    const auto it = pass.find(key);
    if (it != pass.end() && (it->is_number_unsigned() || it->is_number_integer())) {
      append_object_ref(refs, it->get<ObjectId>());
    }
  }

  const auto colors = pass.find("colors");
  if (colors != pass.end() && colors->is_array()) {
    for (const auto &color : *colors) {
      if (!color.is_object()) {
        continue;
      }
      for (const auto *key : {"texture", "resolve_texture"}) {
        const auto it = color.find(key);
        if (it != color.end() && (it->is_number_unsigned() || it->is_number_integer())) {
          append_object_ref(refs, it->get<ObjectId>());
        }
      }
    }
  }

  for (const auto *attachment_key : {"depth", "stencil"}) {
    const auto attachment = pass.find(attachment_key);
    if (attachment == pass.end() || !attachment->is_object()) {
      continue;
    }
    const auto texture = attachment->find("texture");
    if (texture != attachment->end() && (texture->is_number_unsigned() || texture->is_number_integer())) {
      append_object_ref(refs, texture->get<ObjectId>());
    }
  }
}

void collect_numeric_array_refs(const nlohmann::json &payload, std::vector<ObjectId> &refs)
{
  if (!payload.is_array()) {
    return;
  }
  for (const auto &child : payload) {
    if (child.is_number_unsigned() || child.is_number_integer()) {
      append_object_ref(refs, child.get<ObjectId>());
    }
  }
}

} // namespace

void append_payload_object_refs(const nlohmann::json &payload, std::vector<ObjectId> &object_refs)
{
  if (payload.is_object()) {
    for (const auto &[key, child] : payload.items()) {
      if (is_object_reference_key(key) && (child.is_number_unsigned() || child.is_number_integer())) {
        append_object_ref(object_refs, child.get<ObjectId>());
      }
      if (is_object_reference_array_key(key)) {
        collect_numeric_array_refs(child, object_refs);
      }
      if (key == "render_pass_info") {
        collect_render_pass_texture_refs(child, object_refs);
      }
      if (child.is_string()) {
        append_payload_text_object_refs_impl(
            child.get_ref<const std::string &>(),
            object_refs,
            key == "render_pass_info");
      }
      append_payload_object_refs(child, object_refs);
    }
    return;
  }

  if (payload.is_array()) {
    for (const auto &child : payload) {
      append_payload_object_refs(child, object_refs);
    }
  }
}

void append_payload_text_object_refs(std::string_view payload_text, std::vector<ObjectId> &object_refs)
{
  append_payload_text_object_refs_impl(payload_text.empty() ? std::string_view("{}") : payload_text, object_refs, false);
}

} // namespace apitrace::trace
