#pragma once

#include "apitrace/event_types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace apitrace::trace {

inline constexpr std::uint32_t kCompiledDispatchVersion = 5;

struct CompiledDispatchHeader {
  std::uint64_t source_callstream_bytes = 0;
  std::uint64_t record_count = 0;
  std::uint64_t encoded_record_bytes = 0;
};

// Encodes one already-normalized callstream event. The readable callstream remains the
// authoritative trace; this record is a derived, rebuildable retrace acceleration artifact.
bool encode_compiled_dispatch_event(
    const EventRecord &event,
    std::vector<std::uint8_t> &encoded,
    std::string &error);

// Decodes the bounded typed-node payload emitted by bundle-finalize directly into the replay
// payload object. This format deliberately has no generic MessagePack fallback: every scalar and
// container carries an explicit node tag and all lengths are checked before allocation.
bool decode_compiled_payload_nodes(
    std::string_view encoded,
    nlohmann::json &payload,
    std::string &error);

bool write_compiled_dispatch_header(
    std::ostream &output,
    const CompiledDispatchHeader &header,
    std::string &error);

bool inspect_compiled_dispatch(
    const std::filesystem::path &path,
    std::uint64_t expected_source_callstream_bytes,
    CompiledDispatchHeader &header,
    std::string &error);

using CompiledDispatchEventCallback = std::function<bool(const EventRecord &event)>;

// Decodes one record at a time and releases its storage immediately after callback returns. A
// callback returning false requests a successful early stop, which lets native retrace honor an
// exact stop sequence without materializing the unread suffix.
bool for_each_compiled_dispatch_event(
    const std::filesystem::path &path,
    std::uint64_t expected_source_callstream_bytes,
    const CompiledDispatchEventCallback &callback,
    CompiledDispatchHeader *header,
    std::string &error);

bool load_compiled_dispatch_events(
    const std::filesystem::path &path,
    std::uint64_t expected_source_callstream_bytes,
    std::vector<EventRecord> &events,
    CompiledDispatchHeader *header,
    std::string &error);

} // namespace apitrace::trace
